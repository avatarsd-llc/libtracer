/**
 * @file
 * @brief #477 — the store/delivery path's nothrow soft-fail discipline under OOM.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Injects allocation failure through `tr::detail::probe_fail_hook` (the global-heap twin
 * of the failing `mem_backend_t` the graph_value_backend_test precedent injects) and
 * probes the #453/#454 discipline on the writer-thread store/delivery legs:
 *   - STORE legs report status: an unallocatable LKV soft-fails as `BACKPRESSURE`,
 *     nothing published (the prior value survives);
 *   - the HANDLER null-shared_ptr "consumed" sentinel is NOT misread as that OOM;
 *   - DELIVERY legs drop (never abort, never corrupt): a wide fan-out degrades to the
 *     inline prefix, a spilled-rope target clone drops one leg, a stream ring append
 *     is shed (bounded-lossy history) — while the per-edge dispatch SNAPSHOT, which used
 *     to have a drop of its own, no longer allocates at all (#1448) and is pinned here as
 *     an inverted assertion: a remote fan-out survives a total heap refusal intact;
 *   - a stream drain under OOM DEFERS (cursor kept) and catches up once memory returns;
 *   - the two sheds that happen BEFORE the fan-out — a STREAM ring append and a
 *     `mark_pending` leg — are COUNTED at one per subscriber while the write still answers
 *     SUCCESS (#1003), which is what makes an abandoned fan-out visible at all.
 *
 * The STREAM cases are injected at a DIFFERENT seam since RFC-0025 §4.6.1 Amendment 2. The
 * ring is the RECEIVER's and is bounded in bytes by that vertex's own injected
 * `mem::block_source_t`, not by the global-heap probe, so exhaustion is injected there
 * (@ref refusing_source_t / a small `pool_source_t`) rather than through `probe_fail_hook`.
 * What they pin is the §4.4 pressure contract at that ring: best-effort sheds THE OLDEST and
 * accounts it, reliable answers `BACKPRESSURE` and sheds nothing, the charge/release pairing
 * is exact and symmetric, and one receiver running its source dry does not affect another.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "graph_sinks.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::op_resolver_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::view::rope_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

using tr::testing::check;
using tr::testing::make_value;

/** @brief Reject every probe — total heap exhaustion. */
bool fail_all(std::size_t) noexcept { return false; }

/** @brief Reject probes of >= 512 bytes — a fragmented heap with small blocks left. */
bool fail_big(std::size_t n) noexcept { return n < 512; }

/** @brief Reject exactly @ref g_reject_size-byte probes — pinpoint one growth site. */
std::size_t g_reject_size = 0;
bool fail_exact(std::size_t n) noexcept { return n != g_reject_size; }

/**
 * @brief A heap-backed `block_source_t` that can be switched to REFUSE, and that counts what
 *        it served — the receiver-ring seam's OOM injector (RFC-0025 §4.6.1 clause 3).
 *
 * The ring's bound is no longer the global heap probe `hook_guard_t` drives: it is the
 * RECEIVING vertex's own injected source, so exhaustion has to be injected THERE. Refusing by
 * a switch rather than by a byte cap keeps these tests about the pressure CONTRACT (shed,
 * account, gap / backpressure) rather than about arithmetic on the entry width.
 */
class refusing_source_t final : public tr::mem::block_source_t {
   public:
    /** @brief Named for the census, like every other source. */
    refusing_source_t() noexcept : block_source_t("test-refusing") {}
    /** @brief Serve from the heap unless refusing; count what was handed out. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (refuse) return nullptr;
        void* const p = ::operator new(bytes, std::align_val_t{align}, std::nothrow);
        if (p != nullptr) ++live;
        return p;
    }
    /** @brief Sized reclaim matching @ref try_alloc — the pairing these tests assert on. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        --live;
        ::operator delete(p, bytes, std::align_val_t{align});
    }
    bool refuse = false;   /**< @brief Flip to make every admission decline. */
    std::int64_t live = 0; /**< @brief Outstanding reservations — zero means no leak. */
};

/** @brief RAII: install an OOM-injection hook for one scope, always uninstalled on exit. */
struct hook_guard_t {
    explicit hook_guard_t(bool (*hook)(std::size_t) noexcept) {
        tr::detail::probe_fail_hook = hook;
    }
    ~hook_guard_t() { tr::detail::probe_fail_hook = nullptr; }
    hook_guard_t(const hook_guard_t&) = delete;
    hook_guard_t& operator=(const hook_guard_t&) = delete;
};

/** @brief The delivered-count callback: bumps the int behind @p ctx. */
void count_cb(void* ctx, const rope_t& /*value*/) { ++*static_cast<int*>(ctx); }

/**
 * @brief The delivered-SEQUENCE callback: appends each single-link value's first byte to
 *        the vector behind @p ctx — a count alone cannot tell a duplicate from a fresh
 *        entry, which is exactly what #925 is about.
 */
void record_cb(void* ctx, const rope_t& value) {
    static_cast<std::vector<std::uint8_t>*>(ctx)->push_back(
        std::to_integer<std::uint8_t>(value.only().bytes()[0]));
}

/**
 * @brief A 3-link (SPILLED) rope: its delivery clone must grow a heap chain, which is the
 *        one allocation `try_clone_rope` can be made to fail.
 *
 * An inline rope (≤ 2 links) reserves nothing, so a clone of one cannot fail and no
 * injection reaches the drop legs at all — the shape is load-bearing, not incidental.
 */
rope_t three_link() {
    rope_t r{make_value({0x0A})};
    r.append(make_value({0x0B}));
    r.append(make_value({0x0C}));
    return r;
}

// --- FWD wire builders + reply decoders (the op_resolve_test idiom) -----------
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) {
        (void)tr::wire::emit_path_segment(body, s);
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}
using tr::testing::b_fwd;

/** @brief A decoded reply: the flattened backing view kept alongside the borrowing tlv. */
struct decoded_reply_t {
    view_t flat;
    tlv_t tlv;
};
/** @brief Flatten then decode a reply rope (the one allowed copy, at the consumer). */
decoded_reply_t decode_reply(const rope_t& reply) {
    view_t flat = reply.flatten();
    const auto dec = tr::wire::decode(flat.bytes());
    return decoded_reply_t{std::move(flat), dec ? *dec : tlv_t{}};
}
std::uint8_t value_u8(const tlv_t& v) { return tr::detail::load_le<std::uint8_t>(v.payload); }
/** @brief The registered u16 code of a STATUS{ ERROR{ VALUE u16 LE } } payload — 0 on mismatch. */
std::uint16_t status_error_code(const tlv_t& status) {
    if (status.type != type_t::STATUS || status.children.size() != 1) return 0;
    const tlv_t& err = status.children[0];
    if (err.type != type_t::ERROR || !err.opt.pl || err.children.empty()) return 0;
    const tlv_t& id = err.children[0];
    if (id.type != type_t::VALUE || id.payload.size() != 2) return 0;
    return tr::detail::load_le<std::uint16_t>(id.payload);
}

/** @brief STORE leg: an unallocatable LKV soft-fails as BACKPRESSURE, prior value kept. */
void test_store_backpressure() {
    std::printf("store leg — LKV allocation OOM => BACKPRESSURE, nothing published:\n");
    graph_t g;
    auto v = g.register_vertex(path_t("/s/a"), role_t::STORED_VALUE);
    check(g.write(v, make_value({0x11})).has_value(), "baseline write lands");
    {
        const hook_guard_t oom(fail_all);
        const auto w = g.write(v, make_value({0x22}));
        check(!w.has_value() && w.error() == status_t::BACKPRESSURE,
              "OOM write soft-fails as BACKPRESSURE (never abort)");
    }
    const auto r = g.read(v);
    check(r.has_value() && std::to_integer<int>((*r)->only().bytes()[0]) == 0x11,
          "the prior LKV survives the rejected write (no corruption)");
    check(g.write(v, make_value({0x33})).has_value(), "the vertex recovers once memory returns");
}

/** @brief The HANDLER "consumed" null shared_ptr is a SUCCESS, not misread as OOM. */
void test_handler_sentinel() {
    std::printf("handler leg — the consumed-value sentinel stays a success under OOM:\n");
    graph_t g;
    int seen = -1;
    tr::graph::handlers_t h;
    h.on_write = [&seen](const rope_t& in,
                         const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
        seen = std::to_integer<int>(in.only().bytes()[0]);
        return {};
    };
    auto v = g.register_vertex(path_t("/h/sink"), role_t::HANDLER, std::move(h));
    const hook_guard_t oom(fail_all);
    const auto w = g.write(v, make_value({0x5A}));
    check(w.has_value(), "a handler write succeeds under OOM (stores nothing to allocate)");
    check(seen == 0x5A, "the handler consumed the value");
}

/** @brief Small local fan-out stays allocation-free: it delivers even under pressure. */
void test_small_fanout_allocation_free() {
    std::printf("small fan-out — the inline snapshot path reaches no probe:\n");
    graph_t g;
    auto v = g.register_vertex(path_t("/s/b"), role_t::STORED_VALUE);
    int count = 0;
    (void)g.subscribe(path_t("/s/b"), count_cb, &count);
    const hook_guard_t frag(fail_big);  // the small LKV block still fits
    check(g.write(v, make_value({0x01})).has_value(), "write succeeds on a fragmented heap");
    check(count == 1, "the local callback still delivers (zero-alloc snapshot)");
}

/** @brief Wide fan-out under OOM degrades to the inline prefix — drops, never aborts. */
void test_wide_fanout_degrade() {
    std::printf("wide fan-out — overflow snapshot OOM degrades to the inline prefix:\n");
    constexpr std::size_t kSubs = 12;  // > kInlineFanout (8)
    constexpr std::uint64_t kShed = kSubs - tr::graph::vertex_t::kInlineFanout;
    graph_t g;
    auto v = g.register_vertex(path_t("/s/c"), role_t::STORED_VALUE);
    std::array<int, kSubs> counts{};
    for (int& c : counts) (void)g.subscribe(path_t("/s/c"), count_cb, &c);
    const auto before = g.delivery_drops();
    {
        const hook_guard_t frag(fail_big);  // the 12-view overflow reserve exceeds 512 B
        check(g.write(v, make_value({0x02})).has_value(), "the write itself still succeeds");
    }
    int delivered = 0;
    for (const int c : counts) delivered += c;
    check(delivered == static_cast<int>(tr::graph::vertex_t::kInlineFanout),
          "exactly the inline-prefix subscribers were delivered, the rest dropped");

    // The observable, at the WIDTH it sheds (#896). The degrade used to be invisible: an
    // operator watching delivery_drops() saw zero while a third of the fan-out evaporated.
    // Its own cause, too — this is a buffer that could not be widened, not a delivery whose
    // clone failed, and an alarm that cannot tell them apart cannot act on either.
    const auto d = g.delivery_drops();
    check(d.fan_out_truncated == before.fan_out_truncated + kShed,
          "every edge past the inline prefix is counted, one per shed delivery");
    check(d.out_of_memory == before.out_of_memory,
          "and the capacity degrade is NOT attributed to the OOM cause");

    check(g.write(v, make_value({0x03})).has_value(), "post-OOM write succeeds");
    delivered = 0;
    for (const int c : counts) delivered += c;
    check(delivered == static_cast<int>(tr::graph::vertex_t::kInlineFanout + kSubs),
          "all subscribers deliver again once memory returns");
    check(g.delivery_drops().fan_out_truncated == before.fan_out_truncated + kShed,
          "and the full delivery counts no further drop");
}

/**
 * @brief A HANDLER write whose notify clone OOMs sheds the vertex's WHOLE fan-out — and
 *        counts every shed delivery, not one event (#896).
 *
 * The sharpest of the three uncounted sites: `write_impl`'s handler branch clones the value
 * for delivery because the handler consumes the original, and on a failed clone it skips
 * `deliver_vertex` entirely — no edge is dispatched at all — while still returning success.
 * The behaviour is specified (the handler ran; un-running it is not on the table), so the
 * only thing that can tell an operator it happened is the counter. It moved by nothing, and
 * a counter that moved by ONE would have been almost as misleading: the unit of this drop is
 * a delivery, and this one drops all of them.
 */
void test_handler_notify_clone_sheds_fan_out() {
    std::printf("handler notify — a failed delivery clone sheds the WHOLE fan-out:\n");
    constexpr int kSubs = 3;  // < kInlineFanout: the snapshot itself never allocates
    graph_t g;
    int handled = 0;
    tr::graph::handlers_t h;
    h.on_write = [&handled](const rope_t&,
                            const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
        ++handled;
        return {};
    };
    auto v = g.register_vertex(path_t("/h/fan"), role_t::HANDLER, std::move(h));
    std::array<int, kSubs> counts{};
    for (int& c : counts) (void)g.subscribe(path_t("/h/fan"), count_cb, &c);
    const auto before = g.delivery_drops();
    {
        g_reject_size = 3 * sizeof(view_t);  // exactly the notify clone's chain reserve
        const hook_guard_t oom(fail_exact);
        check(g.write(v, three_link()).has_value(), "the handler write still reports success");
    }
    check(handled == 1, "the handler ran — the value was consumed, not lost");
    int delivered = 0;
    for (const int c : counts) delivered += c;
    check(delivered == 0, "NOT ONE subscriber was delivered — the clone sheds the fan-out");
    const auto d = g.delivery_drops();
    check(d.out_of_memory == before.out_of_memory + kSubs,
          "all 3 shed deliveries are counted (a single +1 for a fan-out of N is the defect)");
    check(d.no_target == before.no_target && d.denied == before.denied &&
              d.fan_out_truncated == before.fan_out_truncated,
          "and by its own cause — not a missing target, a denial, or a capacity degrade");

    check(g.write(v, three_link()).has_value(), "the handler write succeeds once memory returns");
    delivered = 0;
    for (const int c : counts) delivered += c;
    check(delivered == kSubs, "and the full fan-out delivers again");
    check(g.delivery_drops().out_of_memory == before.out_of_memory + kSubs,
          "with no further drop counted");
}

/**
 * @brief A remote edge's dispatch snapshot reaches NO allocator, so a total-heap refusal
 *        cannot shed a single remote delivery (#1448 — inverted from #896's drop).
 *
 * **This assertion used to be its own opposite, and the inversion is the finding.** Until
 * #1448 the snapshot deep-copied the cold half (link NAME, stored caller) into every
 * `edge_view_t`, and those two `std::string`s were the only allocation on the per-edge
 * delivery path. This test pinned the resulting soft-fail: reject exactly that growth and
 * every remote edge's delivery is skipped and counted. The cold half is now a refcount
 * SHARE, so nothing on the path allocates and there is nothing left to fail — the
 * deliveries land instead of dropping, and `vertex_t::snapshot_drops_t` no longer even
 * carries an `out_of_memory` cause.
 *
 * The assertion is therefore the exact inverse of the one it replaces, taken through the
 * SAME injection: refuse precisely the byte count the link copy used to ask for and every
 * REMOTE edge still delivers, in full, with no drop counted by any cause. The old code
 * read `deliveries == 0` and three counted OOM drops through this identical hook, so this
 * is an ablation the tree can be put back to rather than a claim. (`fail_all` cannot be
 * used here: it also refuses the LKV store, so the write soft-fails as BACKPRESSURE and
 * the fan-out is never reached — the pinpoint hook is what keeps the delivery path in
 * scope.)
 *
 * **Two canaries keep it from passing vacuously.** An injection hook that is not consulted
 * proves nothing, and "nothing allocated" is exactly what a blinded hook also reports. So
 * the window first asserts the hook IS live and IS refusing this very growth (a direct
 * `tr::detail::try_assign` of a link-sized string must fail inside it), and the edges are
 * built over links well past the SSO boundary so that a copy, if one happened, would have
 * to reach the allocator.
 */
void test_remote_edge_snapshot_is_allocation_free() {
    std::printf("remote edge — the dispatch snapshot allocates nothing, so nothing sheds:\n");
    constexpr int kSubs = 3;               // < kInlineFanout: no overflow reserve in play
    constexpr std::size_t kLinkLen = 200;  // > SSO, so any copy would be a real allocation
    graph_t g;
    auto v = g.register_vertex(path_t("/s/remote"), role_t::STORED_VALUE);
    int deliveries = 0;
    std::size_t seen_link_len = 0;
    const tr::testing::remote_sink_guard_t sink_guard(
        g, [&](const tr::graph::remote_delivery_t& d, const rope_t&) {
            ++deliveries;
            seen_link_len = d.link.size();
        });
    for (int i = 0; i < kSubs; ++i) {
        std::string link = "lnk" + std::to_string(i);
        link.append(kLinkLen - link.size(), 'x');
        check(g.subscribe_wire(v, make_value({0x04, 0x40, 0x00, 0x00}),
                               make_value({0x06, 0x00, 0x00, 0x00}), std::move(link))
                  .has_value(),
              "bind a remote subscriber over a long-named link");
    }
    const auto before = g.delivery_drops();
    {
        g_reject_size = kLinkLen + 1;  // the link copy's growth (+1 for the NUL)
        const hook_guard_t oom(fail_exact);
        // CANARY: the hook is installed AND it refuses a growth of exactly the size the
        // copy this path used to make asked for. Without this the whole test would also
        // pass with the hook blinded, which is the free pass it exists to deny (#1420's
        // canary discipline).
        std::string canary;
        check(!tr::detail::try_assign(canary, std::string(kLinkLen, 'x')),
              "canary: the injection is live and refuses a link-sized owning copy");
        check(g.write(v, make_value({0x30})).has_value(), "the write itself still succeeds");
    }
    check(g.read(v).has_value(), "the LKV landed");
    check(deliveries == kSubs,
          "EVERY remote edge delivered under the refusal — the snapshot copies no bytes");
    check(seen_link_len == kLinkLen,
          "and the sink saw the WHOLE link name, borrowed from the shared record");
    const auto d = g.delivery_drops();
    check(d.out_of_memory == before.out_of_memory,
          "no OOM drop was counted: the per-edge snapshot has no allocation left to fail");
    check(d.fan_out_truncated == before.fan_out_truncated, "and no capacity degrade either");
}

/** @brief A spilled (>2-link) value's target-edge clone drops that ONE leg on OOM. */
void test_target_clone_drop() {
    std::printf("target edge — the spilled-rope delivery clone drops on OOM:\n");
    graph_t g;
    auto a = g.register_vertex(path_t("/s/src"), role_t::STORED_VALUE);
    auto b = g.register_vertex(path_t("/s/dst"), role_t::STORED_VALUE);
    (void)g.subscribe(path_t("/s/src"), path_t("/s/dst"));
    const std::uint64_t oom_before = g.delivery_drops().out_of_memory;
    {
        g_reject_size = 3 * sizeof(view_t);  // exactly the clone's chain reserve
        const hook_guard_t oom(fail_exact);
        check(g.write(a, three_link()).has_value(), "the source write itself succeeds");
    }
    check(!g.read(b).has_value(), "the target delivery leg was dropped (no partial write)");

    // ASSERT THE OBSERVABLE, not only the behaviour. A dropped delivery is otherwise
    // indistinguishable from one that never had a target, and `delivery_drops()` is the only
    // thing that tells an operator which happened. This path was already exercised here and
    // the counter was never checked, so `out_of_memory` could have stopped counting without
    // any test noticing.
    const auto d = g.delivery_drops();
    check(d.out_of_memory == oom_before + 1, "the OOM drop is counted once, by its own cause");
    check(d.no_target == 0 && d.denied == 0,
          "and is not attributed to a missing target or a denied write");

    check(g.write(a, three_link()).has_value() && g.read(b).has_value(),
          "the target edge delivers again once memory returns");
    check(g.delivery_drops().out_of_memory == oom_before + 1,
          "and the successful redelivery counts no further drop");
}

/** @brief A receiver ring's admission is refused by ITS OWN source (bounded-lossy), the LKV
 *         still lands — the byte bound of RFC-0025 §4.6.1 clause 3, injected where it lives. */
void test_stream_ring_shed() {
    std::printf("receiver ring — the admission is refused by the vertex's own source:\n");
    refusing_source_t src;  // MUST outlive the graph: the ring releases against it
    graph_t g;
    auto v = g.register_vertex(path_t("/s/log"), role_t::STREAM);
    g.set_ring_source(v, &src);
    g.set_history_depth(v, 4);
    {
        src.refuse = true;  // the receiver's own budget declines the reservation
        check(g.write(v, make_value({0x10})).has_value(), "the stream write succeeds");
        const auto r = g.read(v);
        check(r.has_value() && std::to_integer<int>((*r)->only().bytes()[0]) == 0x10,
              "the LKV published even though the ring entry was refused admission");
        const auto hist = g.history(v);
        check(hist.has_value() && hist->empty(), "the refused entry never entered the ring");
    }
    src.refuse = false;
    check(g.write(v, make_value({0x11})).has_value() && g.history(v)->size() == 1,
          "the ring admits again once the source can fund it");
    check(src.live == 1, "exactly one reservation is held — charge on append, one entry kept");
    check(g.ring_reserved_bytes(v).has_value() && *g.ring_reserved_bytes(v) > 0,
          "and the byte bound's observable reports it");
}

/**
 * @brief The charge/release pairing is EXACT and SYMMETRIC: a steady-state ring at depth N
 *        holds a bounded reservation total, and a retired ring returns every byte.
 *
 * The failure this pins is the expensive one — a reservation held past the entry that earned
 * it. It would not show as a crash or a wrong value; it would show as a receiver that
 * gradually stops admitting, months later, on a source nobody was watching.
 */
void test_ring_reservations_are_symmetric() {
    std::printf("receiver ring — charge on append, release on retire, no leak:\n");
    refusing_source_t src;
    {
        graph_t g;
        auto v = g.register_vertex(path_t("/s/sym"), role_t::STREAM);
        g.set_ring_source(v, &src);
        g.set_history_depth(v, 3);
        for (int i = 0; i < 10; ++i) check(g.write(v, make_value({0x20})).has_value(), "write");
        check(src.live == 3, "the trim released every entry the depth intent dropped");
        const auto held = g.ring_reserved_bytes(v);
        check(held.has_value() && *held > 0, "the ring reports the bytes it holds reserved");
        check(g.history(v)->size() == 3, "and the depth intent is what bounds the entry count");
    }
    check(src.live == 0, "tearing the graph down returned every reservation — no leak");
}

/**
 * @brief Per-vertex isolation (ruling R-A2): one receiver running its source dry MUST NOT
 *        affect another. This is what "per-injection-point, never a shared pool" buys, and
 *        ADR-0079's amendment measured what a folded source costs instead (0.01x at T=24).
 */
void test_ring_sources_are_isolated_per_vertex() {
    std::printf("receiver ring — one vertex running dry does not affect another:\n");
    refusing_source_t dry;  // both MUST outlive the graph — the rings release against them
    refusing_source_t healthy;
    graph_t g;
    auto a = g.register_vertex(path_t("/iso/a"), role_t::STREAM);
    auto b = g.register_vertex(path_t("/iso/b"), role_t::STREAM);
    g.set_ring_source(a, &dry);
    g.set_ring_source(b, &healthy);
    g.set_history_depth(a, 4);
    g.set_history_depth(b, 4);

    dry.refuse = true;
    check(g.write(a, make_value({0x30})).has_value(), "the exhausted receiver's write succeeds");
    check(g.history(a)->empty(), "…and queues nothing — its own budget declined");
    check(g.write(b, make_value({0x31})).has_value(), "the other receiver's write succeeds");
    check(g.history(b)->size() == 1, "…and queues normally — the flood is a blast radius");
    check(healthy.live == 1 && dry.live == 0, "each ring charged its OWN seam, never a pool");
}

/**
 * @brief The best-effort arm of RFC-0025 §4.4, at the receiver ring: shed the OLDEST whole,
 *        account the loss, and surface `tr::flow::address_shift_gap` IN ORDER at the shed
 *        point. A shed with no accounting is non-conforming; silence is the one behaviour
 *        the pressure contract forbids.
 */
void test_ring_best_effort_sheds_oldest_with_a_gap() {
    std::printf("receiver ring — best-effort sheds the OLDEST, counts it, and raises a gap:\n");
    // A pool sized so the ring funds a couple of entries and then must evict to admit.
    // Declared BEFORE the graph: blocks are host-owned and the source must outlive them.
    std::array<std::byte, 512> slab{};
    std::array<tr::mem::size_class_t, 4> classes{};
    tr::mem::pool_source_t pool(slab, classes);
    graph_t g;
    auto v = g.register_vertex(path_t("/s/gap"), role_t::STREAM);
    g.set_ring_source(v, &pool, /*reliable=*/false);
    g.set_history_depth(v, 64);  // depth intent far past what the byte bound can fund

    int seen = 0;
    (void)g.subscribe(path_t("/s/gap"), count_cb, &seen);
    const auto before = g.delivery_drops();
    for (int i = 0; i < 64; ++i)
        check(g.write(v, make_value({static_cast<std::uint8_t>(i)})).has_value(),
              "every best-effort write SUCCEEDs — completeness is what is sacrificed");
    const auto gaps = g.stream_gaps(v);
    check(gaps.has_value() && *gaps > 0, "the byte bound bit: shed points were recorded");
    check(g.delivery_drops().out_of_memory > before.out_of_memory,
          "and every shed is ACCOUNTED — a silent shed is non-conforming");
    const auto hist = g.history(v);
    check(hist.has_value() && hist->size() < 64,
          "the ring holds what the SOURCE could fund, never the declared depth");
    check(!hist->empty(), "…and the newest entries survived: oldest-first is the shed order");

    std::vector<std::shared_ptr<const tr::view::rope_t>> batch;
    std::uint64_t gap_before = 0;
    (void)g.drain_unflushed(v, batch, &gap_before);
    check(gap_before > 0, "the consumer is TOLD about the discontinuity, in order, at the drain");
    std::uint64_t again = 1;
    (void)g.drain_unflushed(v, batch, &again);
    check(again == 0, "…exactly once — a gap already surfaced is not re-reported");
}

/**
 * @brief The reliable arm of RFC-0025 §4.4, at the receiver ring: refuse the admission,
 *        shed NOTHING, and answer the LOCAL producer `BACKPRESSURE`.
 *
 * v1 reach, stated so it is not read as an omission: there is no wire carrier for
 * backpressure — the per-edge credit window is PARKED as the v2 escalation (§4.6.1 clause 7)
 * — so this status reaches a local producer and a remote one sees the receiver's drop tally.
 */
void test_ring_reliable_answers_backpressure() {
    std::printf("receiver ring — the reliable arm refuses and answers BACKPRESSURE:\n");
    refusing_source_t src;  // MUST outlive the graph: the ring releases against it
    graph_t g;
    auto v = g.register_vertex(path_t("/s/rel"), role_t::STREAM);
    g.set_ring_source(v, &src, /*reliable=*/true);
    g.set_history_depth(v, 4);
    check(g.write(v, make_value({0x40})).has_value(), "a funded write lands normally");
    check(g.history(v)->size() == 1, "…and queues its entry");

    src.refuse = true;
    const auto r = g.write(v, make_value({0x41}));
    check(!r && r.error() == status_t::BACKPRESSURE,
          "the refused admission answers BACKPRESSURE to the rate-aware producer");
    check(g.history(v)->size() == 1, "NOTHING was shed — the reliable arm never drops");
    check(g.stream_gaps(v).has_value() && *g.stream_gaps(v) == 0,
          "…and raises no gap: a gap means a loss, and there was none");
}

/**
 * @brief A SHED ring append must not make the next drain re-deliver the previous entry
 *        (#925) — the drain counts APPENDS, not write-sequence bumps.
 *
 * `store` bumps `write_seq_` unconditionally (it is the await/readiness cursor, and the
 * LKV publish above it DID land), so a drain that derives "how many entries are new" from
 * a sequence delta claims a tail entry the shed never appended. Nothing is ever removed
 * from the ring on a drain, so it re-takes the newest ALREADY-FLUSHED entry and the
 * subscriber observes the same stream element twice — on machinery whose whole point is
 * an in-order queue rather than a coalesce (RFC-0008 §E).
 *
 * The write still answering SUCCESS is the OTHER half of this shed, and that half is now
 * VISIBLE rather than silent — `test_stream_shed_is_counted` below owns the counter
 * assertions (#1003). This one stays about WHAT was delivered, not what was counted.
 */
void test_stream_shed_append_no_redelivery() {
    std::printf("stream drain — a shed ring append re-delivers NOTHING (#925):\n");
    refusing_source_t src;  // MUST outlive the graph: the ring releases against it
    graph_t g;
    auto v = g.register_vertex(path_t("/s/shed"), role_t::STREAM);
    g.set_ring_source(v, &src);
    g.set_history_depth(v, 4);
    std::vector<std::uint8_t> seen;
    (void)g.subscribe(path_t("/s/shed"), record_cb, &seen);

    check(g.write(v, make_value({0x10})).has_value(), "the first stream write succeeds");
    check(seen.size() == 1 && seen[0] == 0x10, "the subscriber saw 0x10 exactly once");

    {
        src.refuse = true;  // the RECEIVER's own source declines the reservation
        check(g.write(v, make_value({0x11})).has_value(), "the shed stream write still succeeds");
        src.refuse = false;
    }
    // §4.4's best-effort arm at the receiver ring: the refused admission shed the OLDEST
    // queued entry (whole, one per admission) to try to fund itself, and still could not.
    // What is asserted here is what #925 is actually about — the DRAIN must not fabricate a
    // tail out of the un-appended entry — so the ring being empty is fine; a re-delivery is not.
    check(g.history(v).has_value() && g.history(v)->empty(),
          "the refused entry never entered the ring, and the oldest was shed to try");
    check(seen.size() == 1, "the shed append delivered NOTHING — no phantom tail entry");
    check(seen.size() == 1 && seen.back() == 0x10, "0x10 was NOT re-delivered as 0x11's entry");

    // A queue, not a coalesce: the next real append still delivers, exactly once.
    check(g.write(v, make_value({0x12})).has_value(), "the ring accepts entries again");
    check(seen.size() == 2 && seen.back() == 0x12, "the next real append delivers once");
    g.propagate(v);
    check(seen.size() == 2, "a covering sweep after the shed re-delivers nothing");
}

/**
 * @brief A shed STREAM ring append is COUNTED, one per subscriber, and the write still
 *        answers SUCCESS (#1003).
 *
 * The shed happens BEFORE the fan-out is ever reached: the store verb skips the append, the
 * STREAM arm of the eager delivery drains zero entries and returns before a single edge is
 * snapshotted, so the loss never passes any dispatch-plane counting site. Every counter read
 * a zero delta while an entire fan-out was abandoned — the node concluding nothing was shed
 * while N deliveries were.
 *
 * Both halves of the ruling are asserted: SUCCESS is the specified answer (the LKV publish
 * landed and the ring is bounded-lossy by contract, RFC-0008 §E), so the counter is the ONLY
 * thing that can carry the loss; and the width is per SUBSCRIBER, matching the eager
 * handler-clone leg that sheds a whole fan-out.
 */
void test_stream_shed_is_counted() {
    std::printf("stream ring — a shed append is COUNTED per subscriber, write still SUCCEEDs:\n");
    refusing_source_t src;  // MUST outlive the graph: the ring releases against it
    graph_t g;
    auto v = g.register_vertex(path_t("/s/counted"), role_t::STREAM);
    g.set_ring_source(v, &src);
    g.set_history_depth(v, 4);
    constexpr int kSubs = 3;
    int seen = 0;
    for (int i = 0; i < kSubs; ++i) (void)g.subscribe(path_t("/s/counted"), count_cb, &seen);

    const auto before = g.delivery_drops();
    {
        src.refuse = true;  // the RECEIVER's own source declines the reservation
        check(g.write(v, make_value({0x10})).has_value(),
              "the write SUCCEEDs — the LKV published and the ring is lossy by contract");
        src.refuse = false;
    }
    check(seen == 0, "not one of the three subscribers was delivered");

    const auto after = g.delivery_drops();
    check(after.out_of_memory - before.out_of_memory == static_cast<std::uint64_t>(kSubs),
          "the shed fan-out is counted ONCE PER SUBSCRIBER, not once per event");
    check(after.no_target == before.no_target && after.denied == before.denied &&
              after.fan_out_truncated == before.fan_out_truncated,
          "and is not confused with another cause");

    // The healthy write behind it must deliver AND count nothing further — an increment that
    // drifted onto the delivering path would be invisible in behaviour and make the counters lie.
    check(g.write(v, make_value({0x11})).has_value(), "the next healthy write succeeds");
    check(seen == kSubs, "and delivers to all three subscribers");
    check(g.delivery_drops().out_of_memory == after.out_of_memory,
          "the delivering path counts no drop at all");
}

/**
 * @brief A shed deferred mark is counted too: `assign` under a failed pending-mark probe,
 *        then a FULLY HEALTHY covering propagate that delivers nothing (#1003).
 *
 * Stronger than a deferral. An unmarked vertex rides no ancestor sweep, so with no later
 * write to re-mark it the assigned value is never delivered at all — a lost delivery whose
 * own code comment claimed equivalence to "an eager delivery leg under the same pressure"
 * while the eager legs counted and this one did not.
 *
 * The control (same sequence, no injection) is asserted alongside it, because "delivered
 * nothing" only means something if the identical unindexed sequence delivers.
 */
void test_shed_pending_mark_is_counted() {
    std::printf("deferred mark — a shed pending mark is counted, and the sweep delivers none:\n");
    graph_t g;
    auto parent = g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    auto child = g.register_vertex(path_t("/p/c"), role_t::STORED_VALUE);
    int seen = 0;
    (void)g.subscribe(path_t("/p/c"), count_cb, &seen);  // one own-sub on the assigned vertex

    // Control: the same assign+propagate with no injection delivers exactly once.
    check(g.assign(child, make_value({0x01})).has_value(), "the control assign succeeds");
    g.propagate(parent);
    check(seen == 1, "control — the covering sweep delivers the assigned value once");

    const auto before = g.delivery_drops();
    {
        // Pinpoint the mark: reject EXACTLY the pending-set node probe, spelled as graph.cpp
        // spells it, so the store above it still allocates and the assign still succeeds. A
        // blanket fail_big would not shed this at all — the node probe is well under its
        // 512 B threshold — and a fail_all would soft-fail the store instead, which is a
        // different defect on a different plane.
        g_reject_size = 8 * sizeof(void*) + sizeof(std::vector<std::byte>);
        const hook_guard_t frag(fail_exact);
        check(g.assign(child, make_value({0x02})).has_value(),
              "the assign SUCCEEDs — the value was stored, only the MARK was shed");
    }
    // A fully healthy sweep now: nothing is injected, so anything undelivered is lost, not
    // deferred.
    g.propagate(parent);
    check(seen == 1, "the shed mark means the covering sweep delivers NOTHING — a lost delivery");

    // Exactly one: the vertex has one own-sub, and the ruled width is one per subscriber.
    // Pinpointing the probe is what makes this exact — only the set-node leg was declined,
    // so a second increment here would mean some other leg started counting too.
    const auto after = g.delivery_drops();
    check(after.out_of_memory - before.out_of_memory == 1,
          "the shed deferred delivery is counted once per subscriber, not silent");
    check(after.no_target == before.no_target && after.denied == before.denied &&
              after.fan_out_truncated == before.fan_out_truncated,
          "and is not confused with another cause");
}

/** @brief A stream drain under OOM DEFERS (cursor kept) and catches up afterwards. */
void test_stream_drain_defer() {
    std::printf("stream drain — an OOM propagate defers the batch, never loses it:\n");
    graph_t g;
    auto v = g.register_vertex(path_t("/s/tail"), role_t::STREAM);
    g.set_history_depth(v, 8);
    int count = 0;
    (void)g.subscribe(path_t("/s/tail"), count_cb, &count);
    (void)g.assign(v, make_value({0x01}));
    (void)g.assign(v, make_value({0x02}));
    (void)g.assign(v, make_value({0x03}));
    {
        const hook_guard_t oom(fail_all);
        g.propagate(v);
        check(count == 0, "the OOM propagate delivered nothing (dropped, not aborted)");
    }
    g.propagate(v);
    check(count == 3, "the deferred ring entries all deliver on the next sweep");
}

/**
 * @brief A composed-root READ whose reply link-table reserve OOMs replies an ADDRESSED
 *        BACKPRESSURE, never a silently-dropped (`link_count() == 0`) rope.
 *
 * The dead-web-ui regression: a READ of a vertex with children serves a folded
 * ~hundreds-of-links snapshot. `read_subtree_folded` itself is nothrow-guarded (it soft-fails
 * BACKPRESSURE), but it can SUCCEED and then the reply builder's own link-table reserve (one
 * link larger, a second large contiguous block) fail on the fragmented heap — the pre-fix
 * `assemble` returned an empty rope that `fwd_router`'s `link_count() == 0` guard dropped with
 * NO reply. A WS client read that as a dead session and churned teardown+redial. The fix wraps
 * every success reply in `or_backpressure`, so the empty rope becomes an addressed
 * BACKPRESSURE the client falls back on over the same link.
 *
 * Injection targets EXACTLY the reply reserve: `heap_alloc` (the per-node POINT headers, the
 * reply head) bypasses `probe_fail_hook`, so the only injectable reply allocation is
 * `assemble`'s `try_reserve(1 + fold_links)` — `(1 + fold_links) * sizeof(view_t)` bytes. The
 * fold's own reserve is one link smaller and its node/stack growths are a different element
 * size, so `fail_exact` on that byte count fails ONLY the reply reserve.
 */
void test_composed_read_reply_backpressure() {
    std::printf(
        "composed READ — reply-assembly OOM => addressed BACKPRESSURE, not a silent drop:\n");
    graph_t g;
    op_resolver_t resolver(g);
    constexpr int kChildren = 40;
    (void)g.register_vertex(path_t("/r"), role_t::STORED_VALUE);
    check(g.write(path_t("/r"), make_value({0x00})).has_value(), "seed the root value");
    for (int i = 0; i < kChildren; ++i) {
        const std::string name = "/r/c" + std::to_string(i);
        (void)g.register_vertex(path_t(name), role_t::STORED_VALUE);
        (void)g.write(path_t(name), make_value({static_cast<std::uint8_t>(i)}));
    }
    // The fold: root (POINT header + own LKV = 2 links) + each child (POINT header + borrowed
    // NAME + LKV = 3 links); the reply prepends ONE FWD head link.
    const std::size_t fold_links = 2u + 3u * kChildren;
    const std::size_t reply_links = 1u + fold_links;
    const auto fwd = b_fwd(fwd_op_t::READ, b_path({"r"}), b_path({"reply-ep"}));

    // Baseline (no injection): the composed reply builds — head + every folded snapshot link.
    {
        const auto arena = tr::wire::decode_into(fwd, tr::mem::heap_source());
        auto reply = resolver.resolve(*arena, "ws0");
        check(reply.has_value() && reply->link_count() == reply_links,
              "baseline composed reply = FWD head + folded snapshot links");
    }

    // Fragmented heap: fail EXACTLY the reply's link-table reserve.
    {
        const auto arena = tr::wire::decode_into(fwd, tr::mem::heap_source());
        g_reject_size = reply_links * sizeof(view_t);
        auto reply = [&] {
            const hook_guard_t frag(fail_exact);
            return resolver.resolve(*arena, "ws0");
        }();
        check(reply.has_value() && reply->link_count() != 0,
              "the OOM reply is NOT a silently-dropped (link_count()==0) rope");
        const decoded_reply_t dr = decode_reply(*reply);
        check(dr.tlv.type == type_t::FWD && dr.tlv.children.size() == 5,
              "the reply is a well-formed 5-child FWD{REPLY}, never a headerless frame");
        check(dr.tlv.children.size() == 5 &&
                  value_u8(dr.tlv.children[3]) == static_cast<std::uint8_t>(reply_kind_t::ERROR),
              "kind == ERROR (an addressed reply, not a drop)");
        check(dr.tlv.children.size() == 5 &&
                  status_error_code(dr.tlv.children[4]) == 0x0040 /*tr::flow::backpressure*/,
              "ERROR payload == STATUS{ ERROR{ VALUE u16=0x0040 tr::flow::backpressure } }");
    }
}

}  // namespace

/** @brief Run the #477 nothrow soft-fail probes. */
int main() {
    std::printf("graph store/delivery nothrow soft-fail (#477):\n");
    test_store_backpressure();
    test_handler_sentinel();
    test_small_fanout_allocation_free();
    test_wide_fanout_degrade();
    test_target_clone_drop();
    test_handler_notify_clone_sheds_fan_out();
    test_remote_edge_snapshot_is_allocation_free();
    test_stream_ring_shed();
    test_ring_reservations_are_symmetric();
    test_ring_sources_are_isolated_per_vertex();
    test_ring_best_effort_sheds_oldest_with_a_gap();
    test_ring_reliable_answers_backpressure();
    test_stream_shed_append_no_redelivery();
    test_stream_shed_is_counted();
    test_shed_pending_mark_is_counted();
    test_stream_drain_defer();
    test_composed_read_reply_backpressure();
    return tr::testing::summary("graph_oom_softfail");
}
