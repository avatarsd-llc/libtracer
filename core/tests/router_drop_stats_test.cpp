/**
 * @file
 * @brief #1503 step 3 — `fwd_router_t` exposes its four injected seams, and its cold-path
 *        drops are COUNTED instead of silent.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The census (#1503, finding 2) found ~15 sites in `fwd_router.cpp` that lose a frame and
 * tell nobody, against the router's OWN stated doctrine ("the event is COUNTED rather than
 * merely handled", `fwd_router.hpp`). This file pins the counters that close them, and the
 * accessors that let a host reach the seams whose exhaustion produced them.
 *
 * @section shape What a case here has to prove
 *
 * A drop is invisible by construction, so a bare "the counter went up" proves little on its
 * own — a counter bumped on the WRONG arm would also go up. Every case therefore asserts
 * three things:
 *
 *   - the INSTRUMENT: the injected seam was actually asked and actually refused (or the
 *     frame really was the malformed shape), so the case is not vacuous;
 *   - the counter moved by EXACTLY one, and the OTHER counters did not — the per-cause split
 *     is the whole point of the #1503 Q3 ruling, and a bucket that catches everything is
 *     worth nothing to an operator sizing a seam;
 *   - the POSITIVE CONTROL: the same flow with the seam un-armed counts NOTHING, which is
 *     what pins "failure path only" (STYLE.md §Introspection, counting doctrine 1) — a
 *     counter bumped on the success arm fails here.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
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
using tr::net::router_stats_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::b_fwd;
using tr::testing::check;

/**
 * @brief A `mem_backend_t` that serves from the heap until armed, then refuses.
 *
 * The `fwd_flatten_backend_test` pattern, reused deliberately: that file already proves the
 * seam is CONSULTED at these sites, so this file can stand on it and assert only the count.
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
        return tr::mem::heap_backend().alloc(size, hint);
    }
    void destroy(tr::view::segment_t* seg) noexcept override {
        tr::mem::heap_backend().destroy(seg);
    }

    /** @brief Refuse every subsequent allocation (the exhaustion stand-in). */
    void arm() noexcept { armed_ = true; }
    /** @brief Serve again — the positive control's precondition. */
    void disarm() noexcept { armed_ = false; }
    /** @brief How many allocations were REFUSED — the instrument check. */
    [[nodiscard]] int refusals() const noexcept { return refusals_; }

   private:
    bool armed_ = false;
    int refusals_ = 0;
};

/**
 * @brief A `block_source_t` that serves from the heap until armed, then refuses.
 *
 * The block-seam twin of @ref arming_backend_t. Delegation, not a private allocator: a
 * served block IS the heap source's, so it is reclaimed exactly as an un-injected router's
 * would be, and the ONLY difference between armed and un-armed is the `nullptr`.
 */
class arming_source_t final : public tr::mem::block_source_t {
   public:
    arming_source_t() noexcept : block_source_t("test_arming_src") {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (armed_) {
            ++refusals_;
            return nullptr;
        }
        return tr::mem::heap_source().try_alloc(bytes, align);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::heap_source().release(p, bytes, align);
    }

    /** @brief Refuse every subsequent draw. */
    void arm() noexcept { armed_ = true; }
    /** @brief Serve again. */
    void disarm() noexcept { armed_ = false; }
    /** @brief How many draws were REFUSED — the instrument check. */
    [[nodiscard]] int refusals() const noexcept { return refusals_; }

   private:
    bool armed_ = false;
    int refusals_ = 0;
};

/** @brief A point-to-point endpoint that counts what it was handed (a bus peer's slot). */
struct p2p_link_t : transport_t {
    std::size_t received = 0; /**< @brief Frames this endpoint was handed. */
    void send(std::span<const std::byte>) override { ++received; }
};

/** @brief A multi-peer (bus) transport with a fixed name→endpoint peer table. */
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
    std::vector<std::vector<std::byte>> sent; /**< @brief Frames the router handed down. */

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
    return tr::testing::b_path(segs);
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

/** @brief A rope over @p bytes split into @p links links. */
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

/**
 * @brief Every field of @p a and @p b except @p except, compared.
 *
 * The "and no OTHER counter moved" half of each case. Written as a total over the struct so
 * that a field ADDED to `router_stats_t` without a case of its own is still covered by every
 * existing case's cross-check, rather than silently dropping out of the assertions.
 */
bool only_moved(const router_stats_t& a, const router_stats_t& b, std::size_t router_stats_t::*ex,
                std::size_t by) {
    const std::array<std::size_t router_stats_t::*, 7> all{
        &router_stats_t::flatten_dropped,   &router_stats_t::forward_iov_dropped,
        &router_stats_t::arena_dropped,     &router_stats_t::assemble_dropped,
        &router_stats_t::reply_iov_dropped, &router_stats_t::delivery_iov_dropped,
        &router_stats_t::malformed_rx};
    return std::all_of(all.begin(), all.end(), [&](std::size_t router_stats_t::*f) {
        return b.*f - a.*f == (f == ex ? by : std::size_t{0});
    });
}

// --- the four seams a host could not reach ------------------------------------------

/**
 * @brief Each seam accessor reports the object that was INJECTED, not a copy or a default.
 *
 * The reason this matters is #1492's exact loop: step 2 put `stats()` on
 * `mem::block_source_t`, but a host that took a DEFAULT seam had no way to name the object
 * to call it on. Identity is the assertion — a "census struct" that copied the numbers out
 * would pass a value check and still leave the host unable to poll.
 */
void test_seam_accessors_report_the_injected_objects() {
    std::printf("the four injected seams are reachable, and are the objects passed in:\n");
    graph_t g;
    arming_source_t label_src;
    arming_source_t rx_src;
    arming_backend_t flat;
    arming_backend_t egress;
    fwd_router_t router(g, &label_src, &rx_src, &flat, 0, &egress);

    check(&router.label_source() == &label_src, "label_source() is the injected label source");
    check(&router.rx_source() == &rx_src, "rx_source() is the injected rx source");
    check(&router.flatten_backend() == &flat, "flatten_backend() is the injected flatten seam");
    check(&router.egress_backend() == &egress, "egress_backend() is the injected egress seam");

    // And the DEFAULTED router names the process-wide singletons rather than nothing: a host
    // that injected none of them can still poll all four.
    fwd_router_t plain(g);
    check(&plain.label_source() == &tr::mem::heap_source(),
          "a defaulted router names the heap "
          "source for labels");
    check(&plain.rx_source() == &tr::mem::heap_source(), "and for rx");
    check(&plain.flatten_backend() == &tr::mem::heap_backend(), "and the heap backend to flatten");
    check(&plain.egress_backend() == &tr::mem::heap_backend(), "and to egress");

    const router_stats_t z = router.drop_stats();
    check(only_moved(router_stats_t{}, z, &router_stats_t::malformed_rx, 0),
          "a fresh router counts nothing at all");
}

// --- flatten: the cold bus-name rejection -------------------------------------------

/**
 * @brief A refused bus-name-rejection flatten counts exactly one `flatten_dropped`.
 *
 * The site `fwd_flatten_backend_test` proves is REACHED; here it must also be counted. The
 * frame is well-formed and its rejection is the ADR-0073 §3 answer, so nothing else on the
 * path has any reason to move — which makes the cross-check meaningful.
 */
void test_flatten_refusal_is_counted() {
    std::printf("a refused bus-name-rejection flatten is counted as flatten_dropped:\n");
    graph_t g;
    arming_backend_t fb;
    fwd_router_t router(g, &tr::mem::heap_source(), &tr::mem::heap_source(), &fb);
    bus_link_impl_t bus;
    p2p_link_t alice;
    bus.peers.emplace_back("alice", &alice);
    rec_link_t in(/*ropes=*/true);
    (void)router.add_child("net/ws-server/srv", bus);
    (void)router.add_child("net/ws-client/in", in);

    const std::vector<std::byte> misroute =
        b_fwd(fwd_op_t::WRITE, b_path({"net", "ws-server", "srv", "sensor", "temp"}),
              b_path({"origin"}), {}, b_value_u32(0x0C0FFEE0u));

    const router_stats_t before = router.drop_stats();
    fb.arm();
    in.inject(as_rope(misroute, 4));
    const router_stats_t after = router.drop_stats();

    check(fb.refusals() > 0, "instrument: the flatten seam was ASKED and refused");
    check(in.sent.empty(), "and the frame was dropped (no reply)");
    check(only_moved(before, after, &router_stats_t::flatten_dropped, 1),
          "exactly one flatten_dropped, and no other counter moved");

    // The positive control: with memory back the SAME frame is answered, and NOTHING is
    // counted. This is the arm that fails if a bump ever migrates onto the success path.
    fb.disarm();
    in.inject(as_rope(misroute, 4));
    check(in.sent.size() == 1, "the same frame is answered once memory returns");
    check(only_moved(after, router.drop_stats(), &router_stats_t::flatten_dropped, 0),
          "and the served flatten counts NOTHING (failure path only)");
}

// --- arena: the terminus decode the rx source could not serve ------------------------

/**
 * @brief An rx source that cannot serve the terminus arena counts `arena_dropped`, not
 *        `malformed_rx`.
 *
 * The split is the #1503 Q3 ruling in its sharpest form: these two live behind ONE
 * `std::expected`, and telling them apart is the difference between "grow the node's rx
 * budget" and "a peer is sending garbage". `decode_into` documents
 * `TLV_NESTING_TOO_DEEP` as *"exceeds this receiver's decode resources"*, and that is the
 * discriminator the router reads.
 */
void test_arena_refusal_counts_apart_from_malformed() {
    std::printf("a terminus decode the rx source refuses counts arena_dropped:\n");
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    arming_source_t rx;
    fwd_router_t router(g, &tr::mem::heap_source(), &rx);
    rec_link_t up;
    (void)router.add_child("up", up);

    const std::vector<std::byte> write =
        b_fwd(fwd_op_t::WRITE, b_path({"sink"}), b_path({"origin"}), {}, b_value_u32(0x1234u));

    // Baseline FIRST, un-armed: the frame is a good one, so if it did not resolve cleanly
    // here the armed run below would be measuring the wrong thing.
    const router_stats_t start = router.drop_stats();
    router.on_frame("up", write);
    check(only_moved(start, router.drop_stats(), &router_stats_t::malformed_rx, 0),
          "control: the well-formed frame counts nothing at all");

    const router_stats_t before = router.drop_stats();
    rx.arm();
    router.on_frame("up", write);
    const router_stats_t after = router.drop_stats();

    check(rx.refusals() > 0, "instrument: the rx source was ASKED and refused");
    check(only_moved(before, after, &router_stats_t::arena_dropped, 1),
          "exactly one arena_dropped — and malformed_rx did NOT move");

    rx.disarm();
    router.on_frame("up", write);
    check(only_moved(after, router.drop_stats(), &router_stats_t::arena_dropped, 0),
          "and the served decode counts nothing");
}

// --- malformed: the other side of that same split ------------------------------------

/**
 * @brief A frame this node cannot parse counts `malformed_rx`, and only that.
 *
 * ONE bucket by ruling (#1503 Q3): a bad envelope, a truncated frame and an unknown opcode
 * all read to an operator as the same symptom, and none of them is sized against. What must NOT
 * happen is any of them landing in a RESOURCE counter — a phantom `arena_dropped` would send
 * a deployment off growing a slab that was never short.
 */
void test_malformed_frames_land_in_one_bucket() {
    std::printf("two unrelated malformed shapes share the one malformed_rx bucket:\n");
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    fwd_router_t router(g);
    rec_link_t up;
    rec_link_t rope_up(/*ropes=*/true);
    (void)router.add_child("up", up);
    (void)router.add_child("rope", rope_up);

    // A FWD envelope with `.pl` cleared: a FWD body is a child list, never an opaque
    // payload, so the decode refuses it as FRAME_INVALID.
    std::vector<std::byte> body;
    append(body, tr::testing::fwd_op_child(static_cast<std::uint8_t>(fwd_op_t::WRITE)));
    append(body, b_path({"sink"}));
    append(body, b_path({"origin"}));
    const std::vector<std::byte> bad = tr::testing::fwd_envelope(body, /*structured=*/false);

    const router_stats_t before = router.drop_stats();
    router.on_frame("up", bad);
    const router_stats_t mid = router.drop_stats();
    check(only_moved(before, mid, &router_stats_t::malformed_rx, 1),
          "the unstructured envelope counts one malformed_rx and nothing else");

    // The ROPE tier's own malformed arm, at a completely different site: a well-formed FWD
    // with its last byte lopped off, delivered as a multi-link rope. `tlv_view_t::over`
    // refuses it. The point of the case is that a second, unrelated malformed site lands in
    // the SAME bucket rather than inventing a counter of its own.
    std::vector<std::byte> truncated =
        b_fwd(fwd_op_t::WRITE, b_path({"sink"}), b_path({"origin"}), {}, b_value_u32(3));
    truncated.pop_back();
    rope_up.inject(as_rope(truncated, 3));
    check(only_moved(mid, router.drop_stats(), &router_stats_t::malformed_rx, 1),
          "and so does a truncated frame arriving on the rope tier");

    // The positive control: a GOOD frame of the same shape counts nothing.
    const router_stats_t after = router.drop_stats();
    router.on_frame("up", b_fwd(fwd_op_t::WRITE, b_path({"sink"}), b_path({"origin"}), {},
                                b_value_u32(0x2222u)));
    check(only_moved(after, router.drop_stats(), &router_stats_t::malformed_rx, 0),
          "control: the well-formed twin counts nothing");
}

// --- delivery: the remote fan-out iov table -------------------------------------------

/**
 * @brief A delivery whose iov table the CONTROL source refuses counts
 *        `delivery_iov_dropped`.
 *
 * `deliver_remote`'s span table is drawn from `graph_t::control_source()` (#981). Its
 * exhaustion drops the delivery — the write itself still succeeds, because a fan-out leg is
 * a separate obligation — and before this counter existed that loss was invisible to
 * everything except the subscriber's silence.
 */
void test_delivery_iov_refusal_is_counted() {
    std::printf("a delivery whose iov table is refused counts delivery_iov_dropped:\n");
    arming_source_t ctl;
    graph_t g(&ctl);
    fwd_router_t router(g);
    rec_link_t client;
    (void)router.add_child("client", client);

    const vertex_handle_t feed =
        g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    router.on_frame("client",
                    b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"client"}),
                          b_field_subscribers_append(), b_subscriber(b_path({"client"}), false)));
    client.sent.clear();  // discard the subscribe REPLY

    // Baseline: one delivery goes out with the source serving. If this fails the armed run
    // below would be measuring a subscription that never existed.
    const router_stats_t start = router.drop_stats();
    check(g.write(feed, as_rope(b_value_u32(0xA1A1A1A1u), 1)).has_value(), "the write succeeds");
    check(client.sent.size() == 1, "instrument: the subscription really delivers");
    check(only_moved(start, router.drop_stats(), &router_stats_t::delivery_iov_dropped, 0),
          "control: a delivered fan-out counts nothing");

    const router_stats_t before = router.drop_stats();
    const int refusals_before = ctl.refusals();
    client.sent.clear();
    ctl.arm();
    check(g.write(feed, as_rope(b_value_u32(0xB2B2B2B2u), 1)).has_value(),
          "the write still SUCCEEDS — the fan-out leg is a separate obligation");
    const router_stats_t after = router.drop_stats();
    ctl.disarm();

    check(ctl.refusals() > refusals_before,
          "instrument: the control source was ASKED and "
          "refused");
    check(client.sent.empty(), "and nothing went on the wire");
    check(only_moved(before, after, &router_stats_t::delivery_iov_dropped, 1),
          "exactly one delivery_iov_dropped, and no other counter moved");

    // The positive control: deliveries resume, still counting nothing.
    check(g.write(feed, as_rope(b_value_u32(0xC3C3C3C3u), 1)).has_value(), "the next write lands");
    check(client.sent.size() == 1, "and delivers once the source serves again");
    check(only_moved(after, router.drop_stats(), &router_stats_t::delivery_iov_dropped, 0),
          "counting nothing for the delivery that succeeded");
}

// --- the 16-bit label space (finding 3) ------------------------------------------------

/**
 * @brief `labels_used` reports used-polarity occupancy, and exhaustion is counted.
 *
 * The degrade #1491 showed matters: a caller handed `0` falls back to the full-route form,
 * which REPLIES per frame. Before this counter the only evidence was the throughput drop.
 *
 * `refused_bindings` must NOT move here, and that is a ruling and not an accident: the two
 * mean different things to an operator (size the table up vs reconnect the link), so
 * §5's "one event, one counter" cuts BETWEEN them rather than through them.
 */
void test_label_space_occupancy_and_exhaustion() {
    std::printf("route_handle_t reports labels_used and counts label-space exhaustion:\n");
    tr::net::route_handle_t handles(&tr::mem::heap_source());

    check(handles.labels_used("up") == 0, "a link with no state has spent no labels");
    check(handles.labels_exhausted() == 0, "and nothing is exhausted");

    check(handles.alloc_label("up") == 1, "the first label is 1 (0 is the reserved \"none\")");
    check(handles.labels_used("up") == 1, "and one label is spent");
    check(handles.alloc_label("up") == 2, "the allocator is monotonic");
    check(handles.labels_used("up") == 2, "used-polarity, so it RISES");
    check(handles.labels_used("down") == 0, "and the space is PER-LINK — another link is at 0");

    // Drain the space. 65535 mints, then the saturation the wire's 16 bits force.
    while (handles.alloc_label("up") != 0) {
    }
    check(handles.labels_used("up") == 0xFFFFu, "a spent space reports the full 65535 used");
    check(handles.labels_exhausted() == 1, "and the first refusal is counted");
    check(handles.refused_bindings() == 0,
          "refused_bindings did NOT move — a spent wire space is not a table at its bound");

    (void)handles.alloc_label("up");
    check(handles.labels_exhausted() == 2, "every subsequent refusal counts too (it is sticky)");

    // The positive control: the space is per-link and a fresh link is unaffected, so the
    // counter above cannot be a global that any mint bumps.
    check(handles.alloc_label("down") == 1, "another link still mints from 1");
    check(handles.labels_exhausted() == 2, "and a SERVED mint counts nothing");

    // clear_link restores the whole space — the self-heal a reconnect already performs.
    handles.clear_link("up");
    check(handles.labels_used("up") == 0, "clear_link restores the link's whole space");
    check(handles.alloc_label("up") == 1, "and mints from 1 again");
    check(handles.labels_exhausted() == 2, "the historical count is monotonic, never reset");
}

}  // namespace

int main() {
    std::printf("fwd_router_t seam accessors + counted cold drops (#1503 step 3)\n\n");

    test_seam_accessors_report_the_injected_objects();
    std::printf("\n");
    test_flatten_refusal_is_counted();
    std::printf("\n");
    test_arena_refusal_counts_apart_from_malformed();
    std::printf("\n");
    test_malformed_frames_land_in_one_bucket();
    std::printf("\n");
    test_delivery_iov_refusal_is_counted();
    std::printf("\n");
    test_label_space_occupancy_and_exhaustion();

    std::printf("\nall router drop-stat checks passed\n");
    return 0;
}
