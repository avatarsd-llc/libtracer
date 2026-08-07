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
 *     is shed (bounded-lossy history);
 *   - a stream drain under OOM DEFERS (cursor kept) and catches up once memory returns.
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

#include "libtracer/byteorder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

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

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief A view_t over a fresh, owned heap segment holding @p bytes. */
view_t make_value(std::initializer_list<std::uint8_t> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    std::size_t i = 0;
    for (const std::uint8_t b : bytes) seg->bytes[i++] = std::byte{b};
    return view_t::over(std::move(seg));
}

/** @brief Reject every probe — total heap exhaustion. */
bool fail_all(std::size_t) noexcept { return false; }

/** @brief Reject probes of >= 512 bytes — a fragmented heap with small blocks left. */
bool fail_big(std::size_t n) noexcept { return n < 512; }

/** @brief Reject exactly @ref g_reject_size-byte probes — pinpoint one growth site. */
std::size_t g_reject_size = 0;
bool fail_exact(std::size_t n) noexcept { return n != g_reject_size; }

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
std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, s);
    return out;
}
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) {
        const std::vector<std::byte> n = b_name(s);
        body.insert(body.end(), n.begin(), n.end());
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}
std::vector<std::byte> b_fwd(fwd_op_t op, const std::vector<std::byte>& dst,
                             const std::vector<std::byte>& src) {
    std::vector<std::byte> body;
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::array{static_cast<std::byte>(op)});
    body.insert(body.end(), dst.begin(), dst.end());
    body.insert(body.end(), src.begin(), src.end());
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

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
    h.on_write = [&seen](const rope_t& in) -> tr::graph::result_t<void> {
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
    h.on_write = [&handled](const rope_t&) -> tr::graph::result_t<void> {
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
 * @brief A remote edge whose owning link copy OOMs is skipped by the snapshot — one counted
 *        drop per skipped edge (#896).
 *
 * The per-edge half of the snapshot's soft-fail: `try_copy_published` copies the cold half
 * (link NAME, stored caller) of a wire-made subscriber, and those are the only strings in a
 * dispatch view large enough to allocate. A local callback edge copies two pointers and a
 * refcount, which is why the small-fan-out hot path cannot reach this at all — and why the
 * drop needs a REMOTE subscriber to reproduce, not a mocked snapshot.
 */
void test_remote_edge_copy_drop() {
    std::printf("remote edge — a snapshot copy that cannot allocate drops that edge:\n");
    constexpr int kSubs = 3;               // < kInlineFanout: no overflow reserve in play
    constexpr std::size_t kLinkLen = 200;  // > SSO, so the copy is a real allocation
    graph_t g;
    auto v = g.register_vertex(path_t("/s/remote"), role_t::STORED_VALUE);
    int deliveries = 0;
    g.set_remote_delivery_sink(
        [&deliveries](const tr::graph::remote_delivery_t&, const rope_t&) { ++deliveries; });
    for (int i = 0; i < kSubs; ++i) {
        // Distinct links, one length: every copy probes the same byte count, so ONE
        // injected rejection covers the whole set.
        std::string link = "lnk" + std::to_string(i);
        link.append(kLinkLen - link.size(), 'x');
        check(g.subscribe_wire(v, make_value({0x04, 0x40, 0x00, 0x00}),
                               make_value({0x06, 0x40, 0x00, 0x00}), std::move(link))
                  .has_value(),
              "bind a remote subscriber over a long-named link");
    }
    const auto before = g.delivery_drops();
    {
        g_reject_size = kLinkLen + 1;  // the link copy's growth (+1 for the NUL)
        const hook_guard_t oom(fail_exact);
        check(g.write(v, make_value({0x30})).has_value(), "the write itself still succeeds");
    }
    check(g.read(v).has_value(), "the LKV landed — only the delivery legs were shed");
    check(deliveries == 0, "no remote delivery was made: every edge's view copy failed");
    const auto d = g.delivery_drops();
    check(d.out_of_memory == before.out_of_memory + kSubs,
          "each skipped edge is counted once, by the OOM cause");
    check(d.fan_out_truncated == before.fan_out_truncated,
          "a per-edge copy failure is not the capacity degrade");

    check(g.write(v, make_value({0x31})).has_value(), "post-OOM write succeeds");
    check(deliveries == kSubs, "every remote edge delivers again once memory returns");
    check(g.delivery_drops().out_of_memory == before.out_of_memory + kSubs,
          "and the full delivery counts no further drop");
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

/** @brief A stream's ring append is shed under OOM (bounded-lossy), the LKV still lands. */
void test_stream_ring_shed() {
    std::printf("stream ring — the deque append is shed under OOM, the LKV still lands:\n");
    graph_t g;
    auto v = g.register_vertex(path_t("/s/log"), role_t::STREAM);
    g.set_history_depth(v, 4);
    {
        const hook_guard_t frag(fail_big);  // the ring-append probe exceeds 512 B
        check(g.write(v, make_value({0x10})).has_value(), "the stream write succeeds");
        const auto r = g.read(v);
        check(r.has_value() && std::to_integer<int>((*r)->only().bytes()[0]) == 0x10,
              "the LKV published even though the ring entry was shed");
        const auto hist = g.history(v);
        check(hist.has_value() && hist->empty(), "the shed entry never entered the ring");
    }
    check(g.write(v, make_value({0x11})).has_value() && g.history(v)->size() == 1,
          "the ring accepts entries again once memory returns");
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
 * The write still answering SUCCESS with no drop counted is the OTHER half of this shed
 * and is deliberately not asserted here (#1003 owns making it visible).
 */
void test_stream_shed_append_no_redelivery() {
    std::printf("stream drain — a shed ring append re-delivers NOTHING (#925):\n");
    graph_t g;
    auto v = g.register_vertex(path_t("/s/shed"), role_t::STREAM);
    g.set_history_depth(v, 4);
    std::vector<std::uint8_t> seen;
    (void)g.subscribe(path_t("/s/shed"), record_cb, &seen);

    check(g.write(v, make_value({0x10})).has_value(), "the first stream write succeeds");
    check(seen.size() == 1 && seen[0] == 0x10, "the subscriber saw 0x10 exactly once");

    {
        const hook_guard_t frag(fail_big);  // the ring-append probe exceeds 512 B
        check(g.write(v, make_value({0x11})).has_value(), "the shed stream write still succeeds");
    }
    check(g.history(v).has_value() && g.history(v)->size() == 1,
          "the shed entry never entered the ring");
    check(seen.size() == 1, "the shed append delivered NOTHING — no phantom tail entry");
    check(seen.size() == 1 && seen.back() == 0x10, "0x10 was NOT re-delivered as 0x11's entry");

    // A queue, not a coalesce: the next real append still delivers, exactly once.
    check(g.write(v, make_value({0x12})).has_value(), "the ring accepts entries again");
    check(seen.size() == 2 && seen.back() == 0x12, "the next real append delivers once");
    g.propagate(v);
    check(seen.size() == 2, "a covering sweep after the shed re-delivers nothing");
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
    test_remote_edge_copy_drop();
    test_stream_ring_shed();
    test_stream_shed_append_no_redelivery();
    test_stream_drain_defer();
    test_composed_read_reply_backpressure();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
