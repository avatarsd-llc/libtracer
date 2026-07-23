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
using tr::graph::settings_t;
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
    check(r.has_value() && std::to_integer<int>(r->only().bytes()[0]) == 0x11,
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
    graph_t g;
    auto v = g.register_vertex(path_t("/s/c"), role_t::STORED_VALUE);
    std::array<int, kSubs> counts{};
    for (int& c : counts) (void)g.subscribe(path_t("/s/c"), count_cb, &c);
    {
        const hook_guard_t frag(fail_big);  // the 12-view overflow reserve exceeds 512 B
        check(g.write(v, make_value({0x02})).has_value(), "the write itself still succeeds");
    }
    int delivered = 0;
    for (const int c : counts) delivered += c;
    check(delivered == static_cast<int>(tr::graph::vertex_t::kInlineFanout),
          "exactly the inline-prefix subscribers were delivered, the rest dropped");
    check(g.write(v, make_value({0x03})).has_value(), "post-OOM write succeeds");
    delivered = 0;
    for (const int c : counts) delivered += c;
    check(delivered == static_cast<int>(tr::graph::vertex_t::kInlineFanout + kSubs),
          "all subscribers deliver again once memory returns");
}

/** @brief A spilled (>2-link) value's target-edge clone drops that ONE leg on OOM. */
void test_target_clone_drop() {
    std::printf("target edge — the spilled-rope delivery clone drops on OOM:\n");
    graph_t g;
    auto a = g.register_vertex(path_t("/s/src"), role_t::STORED_VALUE);
    auto b = g.register_vertex(path_t("/s/dst"), role_t::STORED_VALUE);
    (void)g.subscribe(path_t("/s/src"), path_t("/s/dst"));
    const auto three_link = [] {  // 3 links => the clone must grow a heap chain
        rope_t r{make_value({0x0A})};
        r.append(make_value({0x0B}));
        r.append(make_value({0x0C}));
        return r;
    };
    {
        g_reject_size = 3 * sizeof(view_t);  // exactly the clone's chain reserve
        const hook_guard_t oom(fail_exact);
        check(g.write(a, three_link()).has_value(), "the source write itself succeeds");
    }
    check(!g.read(b).has_value(), "the target delivery leg was dropped (no partial write)");
    check(g.write(a, three_link()).has_value() && g.read(b).has_value(),
          "the target edge delivers again once memory returns");
}

/** @brief A stream's ring append is shed under OOM (bounded-lossy), the LKV still lands. */
void test_stream_ring_shed() {
    std::printf("stream ring — the deque append is shed under OOM, the LKV still lands:\n");
    graph_t g;
    settings_t s;
    s.history_keep_last = 4;
    auto v = g.register_vertex(path_t("/s/log"), role_t::STREAM, {}, s);
    {
        const hook_guard_t frag(fail_big);  // the ring-append probe exceeds 512 B
        check(g.write(v, make_value({0x10})).has_value(), "the stream write succeeds");
        const auto r = g.read(v);
        check(r.has_value() && std::to_integer<int>(r->only().bytes()[0]) == 0x10,
              "the LKV published even though the ring entry was shed");
        const auto hist = g.history(v);
        check(hist.has_value() && hist->empty(), "the shed entry never entered the ring");
    }
    check(g.write(v, make_value({0x11})).has_value() && g.history(v)->size() == 1,
          "the ring accepts entries again once memory returns");
}

/** @brief A stream drain under OOM DEFERS (cursor kept) and catches up afterwards. */
void test_stream_drain_defer() {
    std::printf("stream drain — an OOM propagate defers the batch, never loses it:\n");
    graph_t g;
    settings_t s;
    s.history_keep_last = 8;
    auto v = g.register_vertex(path_t("/s/tail"), role_t::STREAM, {}, s);
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
        const auto arena = tr::wire::decode_into(fwd, *std::pmr::get_default_resource());
        auto reply = resolver.resolve(*arena, "ws0");
        check(reply.has_value() && reply->link_count() == reply_links,
              "baseline composed reply = FWD head + folded snapshot links");
    }

    // Fragmented heap: fail EXACTLY the reply's link-table reserve.
    {
        const auto arena = tr::wire::decode_into(fwd, *std::pmr::get_default_resource());
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
    test_stream_ring_shed();
    test_stream_drain_defer();
    test_composed_read_reply_backpressure();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
