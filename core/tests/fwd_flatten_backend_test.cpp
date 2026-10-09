/**
 * @file
 * @brief #730 — every rope flatten `fwd_router_t` performs draws from its INJECTED
 *        `mem_backend_t`, and an exhausted one is answered by value, never by storing
 *        an empty value and reporting success.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The defect this file pinned: the router's `materialize()` call sites all took the DEFAULT
 * global-heap backend, and an ingress flatten that came back empty flowed on as a stored empty
 * value. The ingress `COMPACT` site that made it a silent corruption is retired with COMPACT
 * itself (#1951); the cold bus-name rejection site remains, and this file pins its seam.
 *
 * @section seam Why the seam had to come first
 *
 * The guard alone was rejected on this issue's own argument (#730, option A): with every
 * site on the global heap there is no way to make a flatten fail, so the guard could never
 * be exercised and nobody could prove it still worked. `fwd_router_t` now takes a
 * `mem_backend_t*` beside its `mr` / `rx` injections, and this file injects one that
 * refuses on command — `graph_value_backend_test` is the precedent for the pattern.
 *
 * @section instrument The instrument is checked before the guard is
 *
 * A flatten only happens on a MULTI-link rope: `materialize()` returns a single-link rope's
 * one link zero-copy and never touches the backend. A test whose rope arrived contiguous
 * would therefore pass with the backend unplugged and the guard deleted — the vacuous
 * shape. Every armed case here asserts `refusals() > 0` FIRST: the injected backend was
 * asked and said no. If that count is zero the case proves nothing, and says so.
 *
 * @section observables What each case asserts
 *
 * A drop is invisible by construction, so each case asserts something POSITIVE:
 *
 *   - the bus-name rejection case: no reply is answered, and nothing is broadcast;
 *   - and the backend un-armed, the same flow succeeds, so a guard that over-rejects (or a
 *     seam that wedged the router) fails the control.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "tlv_tree.hpp"  // host-only owning tree (#1829)

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;

/**
 * @brief A `mem_backend_t` that serves from the heap until it is armed, then refuses.
 *
 * Delegation, not a private allocator: a served segment is the heap backend's own, so it
 * reclaims through the heap exactly as an un-injected router's would — the ONLY difference
 * between armed and un-armed is the `nullptr`, which is the variable under test. `destroy`
 * forwards for completeness; no segment this object hands out ever names it (the ADR-0047
 * dispatch reads the allocating backend off the segment).
 */
class arming_backend_t final : public tr::mem::mem_backend_t {
   public:
    arming_backend_t() noexcept : mem_backend_t("test_arming") {}

    [[nodiscard]] tr::view::segment_t* alloc(
        std::size_t size, tr::mem::alloc_hint_t hint = tr::mem::alloc_hint_t::NONE) override {
        if (armed_) {
            ++refusals_;
            return nullptr;
        }
        ++served_;
        return tr::mem::heap_backend().alloc(size, hint);
    }
    void destroy(tr::view::segment_t* seg) noexcept override {
        tr::mem::heap_backend().destroy(seg);
    }

    /** @brief Refuse every subsequent allocation (the heap-exhaustion stand-in). */
    void arm() noexcept { armed_ = true; }
    /** @brief Serve again — the positive control's precondition. */
    void disarm() noexcept { armed_ = false; }
    /** @brief How many allocations were REFUSED — the instrument check. */
    [[nodiscard]] int refusals() const noexcept { return refusals_; }
    /** @brief How many were served — proves the seam is consulted while un-armed too. */
    [[nodiscard]] int served() const noexcept { return served_; }

   private:
    bool armed_ = false;
    int refusals_ = 0;
    int served_ = 0;
};

/** @brief A point-to-point endpoint that counts what it was handed (a bus peer's slot). */
struct p2p_link_t : transport_t {
    std::size_t received = 0; /**< @brief Frames this endpoint was handed. */
    void send(std::span<const std::byte>) override { ++received; }
};

/**
 * @brief A multi-peer (bus) transport with a fixed name→endpoint peer table.
 *
 * Modelled on `mount_routing_test.cpp`'s `bus_link_impl_t`, and here for one reason: a
 * `dst` naming this link's own mount with a residual that matches NO peer is the
 * ADR-0073 §3 / RFC-0020 rejection — the only path that reaches the cold bus-name
 * rejection flatten. `broadcasts` counts frames pushed at the bus endpoint itself, which
 * a real adapter fans out to every open peer; any count here is the forbidden shape.
 */
struct bus_link_impl_t : transport_t, tr::net::bus_link_t {
    std::vector<std::pair<std::string, p2p_link_t*>> peers; /**< @brief name → endpoint. */
    std::size_t broadcasts = 0; /**< @brief Frames sent at the bus endpoint itself. */
    void send(std::span<const std::byte>) override { ++broadcasts; }
    tr::net::bus_link_t* bus() override { return this; }
    transport_t* peer_link(std::string_view name) override {
        for (auto& [n, l] : peers) {
            if (n == name) return l;
        }
        return nullptr;
    }
    void enumerate_peers(const tr::net::bus_link_t::peer_visitor_t& visit) const override {
        for (const auto& [n, l] : peers) visit(n);
    }
    /** @brief The handle's index into @ref peers is its name (#1294). */
    [[nodiscard]] std::string_view peer_name(tr::net::peer_handle_t peer,
                                             std::span<char>) const override {
        if (!peer.valid() || peer.index >= peers.size()) return {};
        return peers[peer.index].first;
    }
};

/** @brief A link that records what the router sends back, and can push ropes upward. */
class rec_link_t : public transport_t {
   public:
    explicit rec_link_t(bool ropes = false) : ropes_(ropes) {}
    void send(std::span<const std::byte> frame) override {
        sent.emplace_back(frame.begin(), frame.end());
    }
    [[nodiscard]] bool delivers_ropes() const override { return ropes_; }
    void inject(tr::view::rope_t frame) { rx_.deliver_rope(std::move(frame)); }
    std::vector<std::vector<std::byte>> sent;

   private:
    bool ropes_ = false;
};

// --- wire builders -----------------------------------------------------------------

/** @brief A `PATH` TLV over the given `/`-segments. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief An opaque `VALUE` TLV holding a little-endian `u32`. */
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::array<std::byte, 4> raw{};
    tr::detail::store_le(std::span<std::byte>(raw), v, 4);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, raw);
    return out;
}

using tr::testing::b_fwd;

/**
 * @brief A rope over @p bytes split into @p links links — the multi-link shape is what
 *        makes `materialize()` FLATTEN instead of returning its one link zero-copy.
 */
tr::view::rope_t as_rope(std::span<const std::byte> bytes, std::size_t links) {
    tr::view::rope_t r;
    if (bytes.empty() || links == 0) return r;
    const std::size_t step = (bytes.size() + links - 1) / links;
    for (std::size_t off = 0; off < bytes.size(); off += step) {
        const std::size_t n = std::min(step, bytes.size() - off);
        tr::view::segment_ptr_t seg = tr::view::heap_alloc(n);
        std::memcpy(seg->bytes.data(), bytes.data() + off, n);
        r.append(tr::view::view_t::over(std::move(seg)));
    }
    return r;
}

/** @brief The `u32` a vertex currently holds, or `nullopt` if it holds nothing usable. */
std::optional<std::uint32_t> stored_u32(const graph_t& g, vertex_handle_t v) {
    const auto ref = g.read(v);
    if (!ref || !*ref) return std::nullopt;
    // An EMPTY stored rope is the corruption this file exists to catch — report it as
    // "nothing", never as a decode crash (`only()` asserts a single link).
    if ((*ref)->total_length() == 0 || (*ref)->link_count() != 1) return std::nullopt;
    const auto tlv = tr::wire::decode((*ref)->only());
    if (!tlv || tlv->payload.size() != 4) return std::nullopt;
    return tr::detail::load_le<std::uint32_t>(tlv->payload);
}

// --- the fourth site: the COLD bus-name rejection flatten ----------------------------

/**
 * @brief A refused bus-name-rejection flatten drops the frame, and never broadcasts it.
 *
 * The fourth `materialize()` site (`fwd_router.cpp:577`) was added by RFC-0020 / #741 and
 * pointed at `flat_` by #730 — and until this case NOTHING exercised it. The #730 verify
 * pass reverted BOTH halves of the site (the seam and the empty-check) and the whole suite
 * still reported `100% tests passed, 0 tests failed out of 72`. So a refactor could have
 * restored the global-heap draw with the suite green, which is precisely the state the
 * seam exists to make impossible.
 *
 * What this case pins is the SEAM. The `if (flat.empty()) return;` beside it is a redundant
 * early-out — `reject_bus_name_hop` opens with a `wire::decode` that an empty span fails —
 * and the code says so. With the site back on the DEFAULT heap the flatten SUCCEEDS under
 * this injection, the rejection reply goes out, and the `sent.empty()` assertion below
 * fails; the instrument check fails first and reports the case as vacuous rather than
 * green.
 *
 * The path: a `dst` naming a multi-peer mount with a residual segment that resolves no
 * current peer. It is COLD (an error answer) but peer-drivable, and it arrives here as a
 * MULTI-LINK rope, which is the only shape that flattens at all.
 */
void test_bus_name_reject_flatten_oom_drops_the_frame() {
    std::printf("a refused bus-name-rejection flatten drops the frame and answers nothing:\n");
    graph_t g;
    arming_backend_t fb;
    fwd_router_t router(g, {.flat = &fb});
    bus_link_impl_t bus;
    p2p_link_t alice;
    bus.peers.emplace_back("alice", &alice);
    rec_link_t in(/*ropes=*/true);
    (void)router.add_child("net/ws-server/srv", bus);
    (void)router.add_child("net/ws-client/in", in);

    // `srv` is the bus link's own NAME and `sensor` names no peer on it — the ADR-0073 §3
    // rejection. `src` is intact, so a well-formed frame is ANSWERED rather than dropped,
    // which is what makes the drop below attributable to the flatten.
    const std::vector<std::byte> misroute =
        b_fwd(fwd_op_t::WRITE, b_path({"net", "ws-server", "srv", "sensor", "temp"}),
              b_path({"origin"}), {}, b_value_u32(0x0C0FFEE0u));

    fb.arm();
    in.inject(as_rope(misroute, 4));
    check(fb.refusals() > 0,
          "instrument: the injected backend was ASKED and refused the rejection flatten");
    check(in.sent.empty(), "no reply went out — the refused flatten drops the frame by value");
    check(bus.broadcasts == 0, "and nothing was fanned out over the bus endpoint");
    check(alice.received == 0, "nor pushed at a peer endpoint");

    // The positive control: the SAME frame is answered once memory returns, which proves
    // the drop above was the exhaustion and not a misbuilt frame that never reached the
    // rejection arm at all.
    fb.disarm();
    in.inject(as_rope(misroute, 4));
    check(in.sent.size() == 1, "the same frame draws exactly one directed reply with memory");
    check(bus.broadcasts == 0, "still never broadcast (ADR-0073 S3)");
    if (in.sent.size() == 1) {
        const auto dec = tr::wire::decode(in.sent[0]);
        check(dec && dec->type == type_t::FWD, "the answer is an FWD");
        bool is_reply = false;
        if (dec) {
            for (const auto& c : dec->children) {
                if (c.type == type_t::VALUE && c.payload.size() == 1 &&
                    static_cast<fwd_op_t>(std::to_integer<std::uint8_t>(c.payload[0])) ==
                        fwd_op_t::REPLY) {
                    is_reply = true;
                    break;
                }
            }
        }
        check(is_reply, "and it is the REPLY the rejection assembles");
    }
}

// --- the default: an un-injected router behaves exactly as before --------------------

/** @brief The defaulted parameter keeps the global-heap behaviour byte for byte. */
void test_default_backend_unchanged() {
    std::printf("an un-injected router still routes a multi-link frame:\n");
    graph_t g;
    const vertex_handle_t sink = g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    fwd_router_t router(g);  // no backend argument at all
    rec_link_t up(/*ropes=*/true);
    (void)router.add_child("up", up);

    constexpr std::uint32_t kVal = 0x0BADF00Du;
    up.inject(
        as_rope(b_fwd(fwd_op_t::WRITE, b_path({"sink"}), b_path({}), {}, b_value_u32(kVal)), 3));
    check(stored_u32(g, sink) == kVal, "a multi-link FWD{WRITE} delivers with no injection");
}

}  // namespace

int main() {
    std::printf("fwd_router_t flatten backend seam (#730)\n\n");

    test_bus_name_reject_flatten_oom_drops_the_frame();
    std::printf("\n");
    test_default_backend_unchanged();

    return tr::testing::summary("fwd_flatten_backend");
}
