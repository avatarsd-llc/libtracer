/**
 * @file
 * @brief #730 — every rope flatten `fwd_router_t` performs draws from its INJECTED
 *        `mem_backend_t`, and an exhausted one is answered by value, never by storing
 *        an empty value and reporting success.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The defect this file pins: the router's four `materialize()` call sites all took the
 * DEFAULT global-heap backend. The egress (per-delivery) one checked its result; the two
 * INGRESS ones did not, and nothing downstream catches an empty flatten —
 * `view::over_bytes` maps an empty span to an ENGAGED-empty optional by design, and
 * `graph_t::write` stores it and returns success. So a heap OOM during the ingress
 * `COMPACT` flatten REPLACED the subscriber's last-known value with nothing and called
 * that a delivery: silent corruption, not a dropped delivery.
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
 *   - the COMPACT case: the vertex still holds the PREVIOUS value, byte-exact — the
 *     assertion that fails loudly against the pre-guard code, which stored an empty rope;
 *   - the ADVERTISE case: the label stays UNBOUND, observable as the `HANDLE_NACK` a
 *     later COMPACT on it draws (RFC-0004 §E.1 self-heal);
 *   - the delivery case: NOTHING goes on the wire — no ADVERTISE, no COMPACT;
 *   - the bus-name rejection case: no reply is answered, and nothing is broadcast;
 *   - and each ends with the backend un-armed and the same flow succeeding, so a guard
 *     that over-rejects (or a seam that wedged the router) fails the control.
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
#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

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

/** @brief Append @p src to @p dst. */
void append(std::vector<std::byte>& dst, const std::vector<std::byte>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

/** @brief A `NAME` TLV. */
std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, s);
    return out;
}

/** @brief A `PATH` TLV over the given `/`-segments. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
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

/** @brief An opaque `VALUE` TLV holding one byte. */
std::vector<std::byte> b_value_u8(std::uint8_t v) {
    const std::byte b{v};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}

/** @brief `FIELD{ NAME "subscribers", VALUE u8 index_mode=ELEMENT }` — the `:subscribers[]`
 *         append. */
std::vector<std::byte> b_field_subscribers_append() {
    std::vector<std::byte> body;
    append(body, b_name("subscribers"));
    append(body, b_value_u8(1));
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}

/** @brief `SUBSCRIBER{ PATH target, SETTINGS qos{ NAME "delivery_compact" VALUE u8 } }`. */
std::vector<std::byte> b_subscriber(const std::vector<std::byte>& target, bool compact) {
    std::vector<std::byte> body;
    append(body, target);
    std::vector<std::byte> qos;
    append(qos, b_name("delivery_compact"));
    append(qos, b_value_u8(compact ? 1 : 0));
    std::vector<std::byte> settings;
    tr::wire::emit_tlv(settings, type_t::SETTINGS, opt_t{.pl = true}, qos);
    append(body, settings);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, body);
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

/** @brief True when @p frames contains a `HANDLE_NACK` for @p label. */
bool has_nack(const std::vector<std::vector<std::byte>>& frames, std::uint16_t label) {
    const std::vector<std::byte> want = tr::net::encode_handle_nack(label);
    return std::any_of(frames.begin(), frames.end(),
                       [&](const std::vector<std::byte>& f) { return f == want; });
}

// --- the defect: an ingress COMPACT flatten that OOMs must not overwrite the LKV -----

/**
 * @brief A refused `COMPACT` payload flatten leaves the previous value INTACT.
 *
 * This is the silent-corruption case. Pre-#730 the empty flatten flowed on into
 * `deliver_local` → `graph_t::write`, which stored it and returned success, so the
 * subscriber's last-known value became nothing and the delivery callback fired.
 */
void test_compact_flatten_oom_preserves_last_known_value() {
    std::printf("an ingress COMPACT whose payload flatten is refused keeps the stored value:\n");
    graph_t g;
    const vertex_handle_t sink = g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    arming_backend_t fb;
    fwd_router_t router(g, std::pmr::get_default_resource(), &tr::mem::heap_source(), &fb);
    rec_link_t up(/*ropes=*/true);
    (void)router.add_child("up", up);

    constexpr std::uint16_t kLabel = 0x0444u;
    constexpr std::uint32_t kFirst = 0x11223344u;
    constexpr std::uint32_t kSecond = 0x55667788u;

    up.inject(as_rope(tr::net::encode_advertise(kLabel, b_path({"sink"})), 3));
    up.inject(as_rope(tr::net::encode_compact(kLabel, b_value_u32(kFirst)), 3));
    check(stored_u32(g, sink) == kFirst, "the flow delivers its first value");
    check(fb.served() > 0, "and the INJECTED backend served those flattens (the seam is live)");

    // The exhaustion.
    fb.arm();
    up.sent.clear();
    up.inject(as_rope(tr::net::encode_compact(kLabel, b_value_u32(kSecond)), 3));
    check(fb.refusals() > 0,
          "instrument: the injected backend was ASKED and refused (a flatten really happened)");
    check(stored_u32(g, sink) == kFirst,
          "the vertex still holds the PREVIOUS value — the refused flatten was not stored");

    // The positive control: the guard drops one delivery, it does not wedge the flow.
    fb.disarm();
    up.inject(as_rope(tr::net::encode_compact(kLabel, b_value_u32(kSecond)), 3));
    check(stored_u32(g, sink) == kSecond, "and the next delivery lands once memory returns");
}

// --- the ADVERTISE arm: same seam, and the binding must not happen -------------------

/**
 * @brief A refused `ADVERTISE` route flatten binds NOTHING.
 *
 * The guard here is redundant with the decode below it (an empty span does not decode) —
 * which is exactly why this case pins the BEHAVIOUR and, more importantly, the SEAM: with
 * the site back on the global heap the flatten would SUCCEED under this injection, the
 * label WOULD bind, and the probe below would deliver instead of drawing a NACK.
 */
void test_advertise_flatten_oom_binds_nothing() {
    std::printf("an ingress ADVERTISE whose route flatten is refused binds no label:\n");
    graph_t g;
    const vertex_handle_t sink = g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    arming_backend_t fb;
    fwd_router_t router(g, std::pmr::get_default_resource(), &tr::mem::heap_source(), &fb);
    rec_link_t up(/*ropes=*/true);
    (void)router.add_child("up", up);

    constexpr std::uint16_t kLabel = 0x0555u;
    constexpr std::uint32_t kVal = 0x9ABCDEF0u;

    fb.arm();
    up.inject(as_rope(tr::net::encode_advertise(kLabel, b_path({"sink"})), 3));
    check(fb.refusals() > 0,
          "instrument: the injected backend was ASKED and refused the route flatten");

    // Probe with memory back, so the probe's OWN payload flatten succeeds and the answer
    // reports the label's state rather than a second exhaustion.
    fb.disarm();
    up.sent.clear();
    up.inject(as_rope(tr::net::encode_compact(kLabel, b_value_u32(kVal)), 3));
    check(has_nack(up.sent, kLabel), "the label is UNBOUND — a COMPACT on it draws a HANDLE_NACK");
    check(!stored_u32(g, sink).has_value(), "and nothing was delivered into /sink");

    // The positive control: the identical ADVERTISE binds once memory returns.
    up.sent.clear();
    up.inject(as_rope(tr::net::encode_advertise(kLabel, b_path({"sink"})), 3));
    up.inject(as_rope(tr::net::encode_compact(kLabel, b_value_u32(kVal)), 3));
    check(stored_u32(g, sink) == kVal, "the same flow binds and delivers with memory available");
    check(!has_nack(up.sent, kLabel), "and no NACK was sent for a label that IS bound");
}

// --- the egress arm: the per-delivery flatten draws from the same injection ----------

/**
 * @brief A refused per-delivery `COMPACT` flatten sends NOTHING and fails no write.
 *
 * The guard at this site predates #730; what is new is that the flatten draws from the
 * router's injection, so a bounded node's bound covers it. The assertion is what makes
 * that observable: with the site on the global heap this delivery would go out.
 */
void test_delivery_flatten_oom_sends_nothing() {
    std::printf("a per-delivery COMPACT whose flatten is refused puts nothing on the wire:\n");
    graph_t g;
    arming_backend_t fb;
    fwd_router_t router(g, std::pmr::get_default_resource(), &tr::mem::heap_source(), &fb);
    rec_link_t client;
    (void)router.add_child("client", client);

    const vertex_handle_t feed =
        g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    router.on_frame("client",
                    b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"client"}),
                          b_field_subscribers_append(), b_subscriber(b_path({"client"}), true)));
    client.sent.clear();  // discard the subscribe REPLY

    // A MULTI-link stored value: a single-link one materializes zero-copy and would never
    // reach the seam (the vacuous shape this check exists to exclude).
    fb.arm();
    const std::vector<std::byte> v1 = b_value_u32(0xA1A1A1A1u);
    check(g.write(feed, as_rope(v1, 2)).has_value(),
          "the write itself SUCCEEDS — a fan-out leg is a separate obligation");
    check(fb.refusals() > 0,
          "instrument: the injected backend was ASKED and refused the delivery flatten");
    check(client.sent.empty(), "and NOTHING went on the wire — no ADVERTISE, no COMPACT");

    // The positive control: the same delivery goes out once memory returns. ONE frame, not
    // two — `ensure_egress` runs BEFORE the flatten, so the dropped attempt above already
    // spent the label's "fresh" edge. That is the documented shape (RFC-0004 §E.1): a
    // dropped fresh ADVERTISE self-heals when the peer NACKs the unknown label, and the
    // guard must not invent a re-advertise the pre-#730 code never made either.
    fb.disarm();
    const std::vector<std::byte> v2 = b_value_u32(0xB2B2B2B2u);
    check(g.write(feed, as_rope(v2, 2)).has_value(), "the next write succeeds");
    check(client.sent.size() == 1, "and delivers with memory available");
    if (client.sent.size() == 1) {
        const auto cmp = tr::wire::decode(client.sent[0]);
        check(cmp && cmp->type == type_t::COMPACT, "the frame is the auto-promoted COMPACT");
    }
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
    fwd_router_t router(g, std::pmr::get_default_resource(), &tr::mem::heap_source(), &fb);
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
    std::printf("an un-injected router still flattens on the global heap:\n");
    graph_t g;
    const vertex_handle_t sink = g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    fwd_router_t router(g);  // no backend argument at all
    rec_link_t up(/*ropes=*/true);
    (void)router.add_child("up", up);

    constexpr std::uint16_t kLabel = 0x0666u;
    constexpr std::uint32_t kVal = 0x0BADF00Du;
    up.inject(as_rope(tr::net::encode_advertise(kLabel, b_path({"sink"})), 3));
    up.inject(as_rope(tr::net::encode_compact(kLabel, b_value_u32(kVal)), 3));
    check(stored_u32(g, sink) == kVal, "a multi-link COMPACT delivers with no injection");
}

}  // namespace

int main() {
    std::printf("fwd_router_t flatten backend seam (#730)\n\n");

    test_compact_flatten_oom_preserves_last_known_value();
    std::printf("\n");
    test_advertise_flatten_oom_binds_nothing();
    std::printf("\n");
    test_delivery_flatten_oom_sends_nothing();
    std::printf("\n");
    test_bus_name_reject_flatten_oom_drops_the_frame();
    std::printf("\n");
    test_default_backend_unchanged();

    return tr::testing::summary("fwd_flatten_backend");
}
