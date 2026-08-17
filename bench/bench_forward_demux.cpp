/**
 * @file
 * @brief The forward-demux baseline: what one FWD forward hop costs, and how much of that
 *        is the registry scan (ADR-0061's one open acceptance condition).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ADR-0061 proposes replacing the flat bare-NAME demux with a per-module strip-K
 * descent. Its hot-path cost was UNCONFIRMED and unmeasurable: `bench_forward_heap`
 * times nothing (it counts allocations) and `bench_fanout_clone_storm` measures
 * refcount contention, so no bench relates forward-hop cost to registry size.
 *
 * This is the BASELINE, deliberately landed against TODAY's flat `by_name` so it needs
 * no strip-K code — it is what the ADR is accepted (or rejected) against, and the
 * yardstick the strip-K PR must not regress.
 *
 * **Two axes, because the two terms move in opposite directions.** Sweeping registry
 * size alone would measure only the term strip-K *shrinks* and miss the one it *adds*:
 *
 *  - `fixed` — the target child is registered FIRST, so `by_name` hits on its first
 *    compare and the scan is ~free. This isolates the constant per-hop cost: header
 *    peek, offset dispatch, head rebuild, egress. **This is the term strip-K grows**,
 *    by a `segment[0]=="net"` literal compare plus a module match — both independent of
 *    registry size. The acceptance question is whether this number has headroom for two
 *    more segment compares.
 *  - `scan` — the target child is registered LAST, so `by_name` walks the whole table.
 *    `scan(N) - fixed(N)` is the scan's marginal cost at N links. **This is the term
 *    strip-K shrinks**: today's `by_name` scans every link and then asks every bus child
 *    to `peer_link`, whereas the per-module key and the per-endpoint `resolve_peer`
 *    narrow both passes to one module's members.
 *
 * Emits one RESULT row per (mode, N) in bench_common's shared format, so collate.py and
 * the perf history pick it up unchanged. `fanout` carries N (registered children) and
 * `endpoints` carries the target's 1-based scan position.
 *
 * NOT a wall-clock throughput number: every hop is driven synchronously on this thread
 * with a capture transport, so the reported latency is pure router cost with no socket,
 * no thread handoff, and no allocation on the measured path.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/fwd_frame_view.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport.hpp"

namespace {

using tr::graph::graph_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Registry sizes swept — N = children registered on the node. */
constexpr std::size_t kLinkCounts[] = {1, 2, 4, 8, 16, 32, 64};

/**
 * @brief Per-point wall-clock budget, in seconds — the ONE tunable, and it is a policy
 *        input, not a bound: `LIBTRACER_BENCH_SECONDS` overrides it.
 *
 * How many samples that buys is derived, never declared. Longer means tighter
 * percentiles; it cannot change what is measured.
 */
constexpr double kDefaultBudgetSeconds = 1.0;

/**
 * @brief Seconds per measured point (env-overridable; non-positive/garbage ⇒ default).
 */
[[nodiscard]] double budget_seconds() {
    const char* const env = std::getenv("LIBTRACER_BENCH_SECONDS");
    if (env == nullptr) return kDefaultBudgetSeconds;
    const double v = std::strtod(env, nullptr);
    return v > 0.0 ? v : kDefaultBudgetSeconds;
}

/**
 * @brief The batch size a leg is timed in — DERIVED from the target window, not chosen.
 *
 * One hop costs tens of nanoseconds, the same order as `clock_gettime` itself, so timing each
 * hop individually measures the clock rather than the router. (An early draft of this bench
 * did exactly that and reported the whole-table scan *beating* the first-hit lookup — the
 * signature of a clock-dominated window.) Batching amortizes the two clock reads, but the
 * right batch is a property of the HOST's clock, not a constant to hardcode.
 *
 * This bench used to derive it with a local PLATEAU rule: double until the per-hop figure
 * stops improving by 5 %. That rule compares two *timed* quantities, so the machine gets a
 * vote in the answer — see `calibrate_batch_for_window`'s own note (#1358). It bites here.
 * Over 24 executions of the same three resolve legs on the quiet pinned host the plateau rule
 * latched batches of **8, 16 and 32** on one leg, and the batch-8 execution read `36 ns`
 * against `33–34 ns` for the others: ~3 ns (9 %) of pure calibrator, in discrete clusters,
 * with nothing but the lottery between the two arms. That is precisely the shape of a false
 * attribution, and this bench's whole third axis is an attribution (#1346).
 *
 * So ask the question directly instead: keep doubling until the measured WINDOW reaches
 * `bench::kMinBatchWindowNs`. The batch then follows the operation's own cost, repeats across
 * executions, and self-scales across a sweep whose arms differ by an order of magnitude. Every
 * leg still reports its batch, so the measurement states its own assumption.
 *
 * Directionally safe for the banked series: a longer window can only remove clock overhead, so
 * the smaller-is-better latency rows move down or stay put. The bench source changes in this
 * commit anyway, which marks every series it feeds.
 * @param hop Runs one forward hop.
 */
template <typename Hop>
[[nodiscard]] std::size_t calibrate_batch(Hop&& hop) {
    return bench::calibrate_batch_for_window(hop);
}

/**
 * @brief A transport that only counts what it was handed — no I/O, no allocation.
 *
 * The scatter-gather `send` is overridden for the same reason `bench_forward_heap` does
 * it: the base class flattens an iov into a temporary vector, which would put a
 * measurement artifact (an allocation) inside the timed window. A real zero-copy
 * transport (sendmsg/writev/RDMA) overrides it exactly this way.
 */
struct capture_transport_t : transport_t {
    std::size_t sends = 0;
    /** @brief Push an inbound frame up this link's installed receiver, as a real one does. */
    void deliver(std::span<const std::byte> f) { rx_.deliver_borrowed(f); }
    void send(std::span<const std::byte> f) override {
        ++sends;
        sink += f.size();
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        ++sends;
        for (const auto& s : iov) sink += s.size();
    }
    /** @brief Accumulates lengths so the compiler cannot elide the egress. */
    std::size_t sink = 0;
};

/** @brief Append a PACKED PATH TLV over @p segs (RFC-0018 — `opt.PL = 0`). */
void emit_path(std::vector<std::byte>& out, std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
}

/**
 * @brief Append the PRE-RFC-0018 `PATH` spelling over @p segs — a structured body of NAME
 *        children — so falsifier 1's two arms can time the same address two ways.
 *
 * This is the ONLY place the retired encoding survives, and it is here rather than in the
 * core because the core no longer emits it and must not learn to again. It is what makes
 * the comparison honest: the packed arm is timed against the encoding it replaces, in one
 * binary, on the same address, rather than against a number recorded on another host.
 */
void emit_path_literal(std::vector<std::byte>& out, std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) tr::wire::emit_name(body, s);
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
}

/** @brief Which `dst`/`src` spelling a bench frame carries — falsifier 1's two arms. */
enum class path_form_t : std::uint8_t {
    PACKED,  /**< @brief RFC-0018 `[u8 len][bytes]` records, the shipped encoding. */
    LITERAL, /**< @brief The retired `NAME`-child body, for the comparison only. */
};

/** @brief FWD{ op=WRITE, dst, src, VALUE } — the frame a forward hop shrinks and grows. */
std::vector<std::byte> make_fwd(std::initializer_list<std::string_view> dst,
                                std::initializer_list<std::string_view> src,
                                std::span<const std::byte> payload,
                                path_form_t form = path_form_t::PACKED) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    if (form == path_form_t::PACKED) {
        emit_path(body, dst);
        emit_path(body, src);
    } else {
        emit_path_literal(body, dst);
        emit_path_literal(body, src);
    }
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, payload);
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

/**
 * @brief The PRE-RFC-0018 `dst` gate, transcribed BYTE-FAITHFULLY from `peek_fwd_dst_any` as
 *        it stood at `5e7659e3` — the commit before RFC-0018 S1+S2 landed (#1341).
 *
 * RFC-0018 falsifier 1 is stated as an IN-BINARY comparison for a reason: *"if the packed arm
 * does not beat the literal arm on the resolve leg, this RFC is void"*, and a comparison
 * against a remembered number cannot falsify anything. The control arm is therefore only
 * worth what its faithfulness to the retired code is worth — and the first version of it was
 * not faithful, which is what #1346 was opened to settle. It hand-rolled a cleaned-up walk,
 * and each cleanup was worth nanoseconds. Ablated one rung at a time in ONE binary on the
 * quiet pinned host (best-of-12 rounds, `calibrate_batch_for_window`, A/A null 0.0 %):
 *
 *  - **+13 ns** — the gate. The hand-rolled one carried the canonical arm only; the retired
 *    `peek_fwd_dst_any` also tests `PATH_REF`, returns a `fwd_dst_kind_t`, writes a
 *    `ref_count` out-param, and is reached through the `peek_fwd_dst` wrapper.
 *  - **+4 ns** — one segment. `prefill` fills `kDstSegCacheSlots` (4) slots and this bench's
 *    `dst` has five segments, so the retired leg parses a fourth header the hand-rolled arm,
 *    walking exactly the three it is asked for, never read.
 *  - **+14 ns** — the walker. `dst_seg_walk_t`'s inline cache stores, its
 *    `last_`/`have_`/`cached_`/`pos_` bookkeeping and three `at()` calls each returning an
 *    `std::optional<std::pair<std::size_t, std::size_t>>`, against `pos += h->total; ++n;`
 *    in registers.
 *
 * That is **31 ns of the 56 ns the arm should have read** — more than the whole effect the
 * falsifier is trying to detect, and enough to invert its verdict: at face value the packed
 * arm LOST, 32 ns against a 25 ns control.
 *
 * So this and @ref legacy_dst_seg_walk_t are TRANSCRIPTIONS, not reimplementations: the
 * retired code, line for line, kept here and nowhere else because the core no longer emits
 * the `NAME`-child body and must not learn to again. Do not "simplify" either of them; every
 * store, branch and `std::optional` below is load-bearing to what this arm claims to be.
 *
 * @param cur       Cursor over the frame.
 * @param pre       Filled exactly as the retired peek filled it.
 * @param ref_count The retired out-param — element count on the `PATH_REF` answer, else 0.
 */
template <class Cursor>
[[gnu::flatten]] [[nodiscard]] tr::net::fwd_dst_kind_t legacy_peek_fwd_dst_any(
    const Cursor& cur, tr::net::fwd_pre_t& pre, std::size_t& ref_count) {
    pre = tr::net::fwd_pre_t{};
    ref_count = 0;
    const auto fwd_h = tr::net::read_fwd_header(cur, 0);
    if (!fwd_h || fwd_h->type != type_t::FWD || !fwd_h->opt.pl)
        return tr::net::fwd_dst_kind_t::NONE;
    const std::size_t body_end = fwd_h->body_off + fwd_h->body_len;
    const auto op_h = tr::net::read_fwd_header(cur, fwd_h->body_off);
    if (!op_h || op_h->type != type_t::VALUE) return tr::net::fwd_dst_kind_t::NONE;
    const std::size_t dst_pos = fwd_h->body_off + op_h->total;
    if (dst_pos >= body_end) return tr::net::fwd_dst_kind_t::NONE;
    const auto dst_h = tr::net::read_fwd_header(cur, dst_pos);
    if (!dst_h) return tr::net::fwd_dst_kind_t::NONE;
    const bool is_ref = dst_h->type == type_t::PATH_REF;
    // The gate's own read of segment 0, handed over below rather than discarded. Nothing is
    // filled until BOTH arms have accepted, so a rejected frame leaves `pre` cleared.
    std::size_t seg0_off = 0;
    std::size_t seg0_len = 0;
    if (is_ref) {
        if (!tr::wire::path_ref_body_valid(dst_h->opt.pl, dst_h->opt.ll, dst_h->body_len))
            return tr::net::fwd_dst_kind_t::NONE;
    } else {
        if (dst_h->type != type_t::PATH || dst_h->body_len == 0)
            return tr::net::fwd_dst_kind_t::NONE;
        const auto seg_h = tr::net::read_fwd_header(cur, dst_h->body_off);
        if (!seg_h || seg_h->type != type_t::NAME) return tr::net::fwd_dst_kind_t::NONE;
        seg0_off = seg_h->body_off;
        seg0_len = seg_h->body_len;
    }
    pre.valid = true;
    pre.fwd_opt = fwd_h->opt;
    pre.body_end = body_end;
    pre.op_pos = fwd_h->body_off;
    pre.op_total = op_h->total;
    pre.op_body_off = op_h->body_off;
    pre.op_body_len = op_h->body_len;
    pre.dst_body_off = dst_h->body_off;
    pre.dst_end = dst_h->body_off + dst_h->body_len;
    pre.after_dst = dst_pos + dst_h->total;
    if (!is_ref) {
        pre.seg0_off = seg0_off;
        pre.seg0_len = seg0_len;
        pre.strip_at = dst_h->body_off;  // caller overwrites once strip_k is known
        return tr::net::fwd_dst_kind_t::PATH;
    }
    pre.dst_ref = true;
    pre.strip_at = dst_h->body_off + tr::wire::kPathRefElementBytes;
    if (pre.strip_at > pre.dst_end) pre.strip_at = pre.dst_end;  // the H = 0 body
    ref_count = tr::wire::path_ref_element_count(dst_h->body_len);
    return tr::net::fwd_dst_kind_t::PATH_REF;
}

/**
 * @brief The canonical wrapper, transcribed from `peek_fwd_dst` at `5e7659e3` — the entry the
 *        retired resolve leg actually called.
 */
template <class Cursor>
[[nodiscard]] bool legacy_peek_fwd_dst(const Cursor& cur, tr::net::fwd_pre_t& pre) {
    std::size_t ref_count = 0;
    return legacy_peek_fwd_dst_any(cur, pre, ref_count) == tr::net::fwd_dst_kind_t::PATH;
}

/**
 * @brief The PRE-RFC-0018 forward-only walker over a `dst`'s leading `NAME` segments,
 *        transcribed BYTE-FAITHFULLY from `dst_seg_walk_t` at `5e7659e3`.
 *
 * Its cache-line-sized inline cache, its split `at` and its `std::optional<std::pair>` returns
 * are the retired instrument, not an accident of this bench — see @ref legacy_peek_fwd_dst_any
 * for why they are reproduced rather than tidied. The cache is sized off the SHIPPED
 * `tr::net::kDstSegCacheSlots` so the retired arm keeps tracking the same config quantity the
 * live walker does; that is the one thing here that is deliberately not frozen at `5e7659e3`.
 *
 * @tparam Cursor A grammar byte-source cursor (span or rope).
 */
template <class Cursor>
class legacy_dst_seg_walk_t {
   public:
    /** @brief Walk the `dst` window @p pre describes, over @p cur. */
    legacy_dst_seg_walk_t(const Cursor& cur, const tr::net::fwd_pre_t& pre) noexcept
        : cur_(&cur), body_off_(pre.dst_body_off), end_(pre.dst_end), pos_(pre.dst_body_off) {}

    /** @brief Fill the inline cache NOW, in ONE tight loop. */
    void prefill() {
        while (cached_ < tr::net::kDstSegCacheSlots && pos_ < end_) {
            const auto h = tr::net::read_fwd_header(*cur_, pos_);
            if (!h || h->type != type_t::NAME) return;
            last_ = {h->body_off, h->body_len};
            cache_[cached_++] = last_;
            pos_ += h->total;
            ++have_;
        }
    }

    /**
     * @brief Segment @p i's `[body_off, body_len)`.
     * @retval std::nullopt The `dst` has no segment @p i.
     */
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> at(std::size_t i) {
        if (i < cached_) return cache_[i];
        return walk_to(i);
    }

   private:
    /** @brief The uncached half of `at`: walk forward until segment @p i is in hand. */
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> walk_to(std::size_t i) {
        if (i < have_) {
            have_ = cached_;
            pos_ =
                cached_ == 0 ? body_off_ : cache_[cached_ - 1].first + cache_[cached_ - 1].second;
        }
        while (have_ <= i) {
            if (pos_ >= end_) return std::nullopt;
            const auto h = tr::net::read_fwd_header(*cur_, pos_);
            if (!h || h->type != type_t::NAME) return std::nullopt;
            last_ = {h->body_off, h->body_len};
            if (have_ < tr::net::kDstSegCacheSlots) cache_[cached_++] = last_;
            pos_ += h->total;
            ++have_;
        }
        return last_;
    }

    const Cursor* cur_;
    std::size_t body_off_;
    std::size_t end_;
    std::size_t pos_;
    std::size_t have_ = 0;   /**< @brief Segments walked; `last_` is number `have_-1`. */
    std::size_t cached_ = 0; /**< @brief Entries of `cache_` filled (`<= have_`). */
    std::pair<std::size_t, std::size_t> last_{0, 0};
    /** @brief Uninitialised on purpose, as the retired walker's was: `cached_` gates reads. */
    std::array<std::pair<std::size_t, std::size_t>, tr::net::kDstSegCacheSlots> cache_;
};

/**
 * @brief Time one forward hop with @p links children, the target at @p target_pos.
 *
 * A frame arrives on "in" addressed to "out"; the hop strips "out" from `dst`, prepends
 * "in" to `src`, and sends on "out" — no terminus, no local vertex. Filler children
 * ("l0", "l1", …) pad the registry so the target sits at scan position @p target_pos.
 * @param links      Total forwardable children registered (N).
 * @param target_pos 1-based position of "out" among them (1 = first, `links` = last).
 * @param mode       RESULT mode tag ("fixed" or "scan").
 */
std::uint64_t run_point(std::size_t links, std::size_t target_pos, const char* mode) {
    graph_t graph;
    fwd_router_t router(graph);
    capture_transport_t in_link;
    capture_transport_t out_link;
    // Stable storage: add_child holds a reference for the transport's lifetime.
    std::vector<capture_transport_t> filler(links);

    std::size_t next_filler = 0;
    for (std::size_t i = 1; i <= links; ++i) {
        if (i == target_pos) {
            router.add_child("net/ws-client/out", out_link);
        } else {
            // Zero-padded so every filler is the SAME LENGTH AS THE TARGET ("out", giving a
            // 17-byte qualified name). Two separate things were wrong before. Unpadded,
            // "l0".."l9" were 16 bytes and "l10"+ were 17, so `by_segments`'s length
            // pre-filter rejected a varying fraction of the table before any string compare
            // and the reported "ns per link" measured that ratio rather than the scan. And a
            // filler that differs in length from the target is rejected on the length check
            // alone, so it never reaches the segment compare at all — which measures the
            // list WALK, not the scan. Matching the target's width is what makes this the
            // honest worst case the row claims to report.
            char fname[32];
            std::snprintf(fname, sizeof fname, "net/ws-client/l%02zu", next_filler);
            router.add_child(fname, filler[next_filler]);
            ++next_filler;
        }
    }

    // The INBOUND child is registered LAST, and ONLY here. A forward hop performs TWO scans
    // of the registry, not one: `by_segments` for the dst mount, and `entry_by_name` for the
    // inbound child's precomputed `src` prefix. Registering it last measures the worst case
    // for that second scan, so the two are never conflated.
    //
    // This used to `add_child` the inbound link a second time, at the TOP of the function as
    // well. The intent -- "registered last" -- therefore never took effect: `entry_by_name`
    // matched the first slot at position 1, and the second scan stayed exactly as invisible
    // as the revision this comment was written to fix. Worse, the duplicate registration was
    // itself a live registry bug (a shadow slot that survives `erase`), which is how it was
    // finally caught. Keep this to ONE call.
    router.add_child("net/ws-server/in", in_link);

    const std::byte payload[4] = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                  std::byte{0xEF}};
    const std::vector<std::byte> frame =
        make_fwd({"net", "ws-client", "out", "sensor", "temp"}, {"reply"},
                 std::span<const std::byte>(payload, 4));

    // Drive the hop through the INBOUND LINK's receiver, not `router.on_frame` directly.
    // That is how a real transport delivers: `add_child` installs a receiver bound to a
    // stable per-child ctx, and the hop reads its mount run off that ctx instead of scanning
    // the registry for it. Calling `on_frame` by name takes the ctx-less entry — a path only
    // tests and SDK hosts use — so the bench was timing a routing shape production never
    // executes, and was blind to the ctx optimization entirely.
    // Drive the hop through the INBOUND LINK's receiver, not `router.on_frame` directly —
    // that is how a real transport delivers, and `add_child` wires a per-child receiver ctx
    // for it. Calling `on_frame` by name takes a ctx-less entry only tests and SDK hosts use,
    // so the bench was timing a routing shape production never executes.
    const auto hop = [&] { in_link.deliver(frame); };

    // Calibration doubles as the warm-up: it primes lazy statics and caches, and its
    // own timings are discarded.
    const std::size_t batch = calibrate_batch(hop);

    // Sample until the budget is spent — the sample COUNT falls out of the host's speed
    // rather than being declared. Each sample is one amortized batch.
    bench::Latency lat;
    const std::uint64_t deadline_ns = static_cast<std::uint64_t>(budget_seconds() * 1e9);
    const std::uint64_t t0 = bench::now_ns();
    std::size_t batches = 0;
    std::uint64_t total = 0;
    while (total < deadline_ns) {
        const std::uint64_t a = bench::now_ns();
        for (std::size_t i = 0; i < batch; ++i) hop();
        lat.add((bench::now_ns() - a) / batch);  // per-hop ns, clock cost amortized
        ++batches;
        total = bench::now_ns() - t0;
    }

    const double hops = static_cast<double>(batches) * static_cast<double>(batch);
    const double hops_per_s = total == 0 ? 0.0 : hops * 1e9 / static_cast<double>(total);
    // size_bytes = the frame the hop carried; fanout = N; endpoints = scan position.
    const bench::Latency::Summary s = lat.summarize();
    bench::emit("libtracer", mode, frame.size(), links, target_pos, hops_per_s, hops_per_s, 0.0, s);

    // State the measurement's own parameters: a reader can tell whether the window was
    // amortized on THIS host, rather than trusting a constant baked in on another.
    std::printf("NOTE mode=%s links=%zu batch=%zu samples=%zu\n", mode, links, batch, batches);
    if (out_link.sends == 0) std::printf("WARN mode=%s links=%zu forwarded NOTHING\n", mode, links);
    return s.p50;
}

/**
 * @brief Split the fixed hop into the part a resolution cache could remove and the part it
 *        cannot — the ceiling on any resolve-once scheme (ADR-0062 / RFC-0004 §E.1).
 *
 * A cache that turns a stable `dst` into a token can only remove work that depends on the
 * DESTINATION. It cannot remove work that depends on the FRAME, because the frame is new on
 * every hop. So the fixed per-hop cost divides in two:
 *
 *  - `resolve` — `peek_fwd_dst` opens the `dst` window and the walk hands back the mount
 *    run. Its answer is the same for every frame to the same destination, so a token that
 *    names the resolved link makes it dead work. **This is the ceiling on caching**, and the
 *    registry scan (axis 2) sits on top of it.
 *  - `rebuild` — `rebuild_fwd_forward` strips the local mount run and grows `src` by it,
 *    emitting the new head. Its output embeds THIS frame's residual `dst`, `src` and payload
 *    offsets, so it must run per frame no matter how the link was found. **This is the floor.**
 *
 * Reporting them separately is the point: a resolve-once scheme is worth building only if
 * `resolve` is a large share of the hop, and no amount of caching touches `rebuild`. Both are
 * timed on the SAME frame the hop axes use, through the same public offset-dispatch entry
 * points production takes, so this is a decomposition of the shipped path — not a model of a
 * hypothetical one.
 */
[[nodiscard]] std::uint64_t run_leg(const char* mode, bool rebuild_leg,
                                    path_form_t form = path_form_t::PACKED) {
    const std::byte payload[4] = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                  std::byte{0xEF}};
    const std::vector<std::byte> frame =
        make_fwd({"net", "ws-client", "out", "sensor", "temp"}, {"reply"},
                 std::span<const std::byte>(payload, 4), form);
    const tr::wire::grammar::span_cursor cur{frame};

    // Accumulated so the optimizer cannot delete the call it is here to time.
    std::size_t sink = 0;
    tr::net::fwd_pre_t pre{};
    // The mount TLV a real child carries precomputed (#508) — content is irrelevant to the
    // timing, only that the rebuild emits it as one span.
    const std::array<std::string_view, 2> mount_segs{"net", "ws-server"};
    const std::vector<std::byte> mount_tlv =
        tr::net::encode_mount_tlv(mount_segs).value_or(std::vector<std::byte>{});
    // Production peeks ONCE and hands the parse to the rebuild via `pre`, so the rebuild leg
    // is timed the same way — otherwise it would re-parse and double-count the very work
    // this axis is trying to attribute to the peek.
    (void)tr::net::peek_fwd_dst(cur, pre);

    const auto leg = [&] {
        if (form == path_form_t::LITERAL) {
            // Falsifier 1's control arm — the RETIRED code, line for line (#1346), driven
            // exactly as the packed arm below is: one peek, one walker, prefill, three asks.
            // Every divergence from that shape was worth nanoseconds; see
            // `legacy_peek_fwd_dst_any`.
            tr::net::fwd_pre_t local{};
            if (!legacy_peek_fwd_dst(cur, local)) return;
            legacy_dst_seg_walk_t<tr::wire::grammar::span_cursor> w(cur, local);
            w.prefill();
            for (std::size_t i = 0; i < 3; ++i) sink += w.at(i).has_value();
            return;
        }
        if (rebuild_leg) {
            // Strip the two-segment local mount run (`net` / `ws-client`) — the same K the hop
            // strips, so the emitted head matches the one the hop builds.
            const auto rb = tr::net::rebuild_fwd_forward(cur, mount_tlv, "in", 2, &pre);
            sink += rb ? rb->head1.span().size() : 0;
        } else {
            tr::net::fwd_pre_t local{};
            if (!tr::net::peek_fwd_dst(cur, local)) return;
            // The peek no longer materializes the segments — it opens the window and the walk
            // reads them (#523). Timing the peek alone would therefore no longer be timing the
            // work this axis attributes to it, so the walk of the mount run rides with it.
            tr::net::dst_seg_walk_t<tr::wire::grammar::span_cursor> w(cur, local);
            w.prefill();
            for (std::size_t i = 0; i < 3; ++i) sink += w.at(i).has_value();
        }
    };

    const std::size_t batch = calibrate_batch(leg);
    bench::Latency lat;
    const std::uint64_t deadline_ns = static_cast<std::uint64_t>(budget_seconds() * 1e9);
    const std::uint64_t t0 = bench::now_ns();
    std::size_t batches = 0;
    std::uint64_t total = 0;
    while (total < deadline_ns) {
        const std::uint64_t a = bench::now_ns();
        for (std::size_t i = 0; i < batch; ++i) leg();
        lat.add((bench::now_ns() - a) / batch);
        ++batches;
        total = bench::now_ns() - t0;
    }
    const bench::Latency::Summary s = lat.summarize();
    bench::emit("libtracer", mode, frame.size(), 1, 1, 0.0, 0.0, 0.0, s);
    std::printf("NOTE mode=%s batch=%zu samples=%zu sink=%zu\n", mode, batch, batches, sink);
    return s.p50;
}

}  // namespace

int main() {
    std::vector<std::uint64_t> fixed;
    std::vector<std::uint64_t> scan;

    // Axis 1 — fixed per-hop cost: target first, so the scan hits immediately. The term
    // strip-K ADDS (a literal + a module compare) is measured against this number.
    for (const std::size_t n : kLinkCounts) fixed.push_back(run_point(n, 1, "fwd-demux-fixed"));
    // Axis 2 — scan cost: target last, so by_name walks the whole table. The delta from
    // axis 1 at the same N is the scan's marginal cost — the term strip-K NARROWS.
    for (const std::size_t n : kLinkCounts) scan.push_back(run_point(n, n, "fwd-demux-scan"));

    // The derived answer to ADR-0061's acceptance question, so it need not be
    // reconstructed by hand from the RESULT rows.
    std::printf("\n%-8s %-14s %-14s %-14s %s\n", "links", "fixed_p50_ns", "scan_p50_ns",
                "scan_delta_ns", "ns_per_link");
    for (std::size_t i = 0; i < std::size(kLinkCounts); ++i) {
        const double delta = static_cast<double>(scan[i]) - static_cast<double>(fixed[i]);
        const std::size_t compares = kLinkCounts[i] > 1 ? kLinkCounts[i] - 1 : 1;
        std::printf("%-8zu %-14llu %-14llu %-14.1f %.2f\n", kLinkCounts[i],
                    static_cast<unsigned long long>(fixed[i]),
                    static_cast<unsigned long long>(scan[i]), delta,
                    delta / static_cast<double>(compares));
    }
    std::printf(
        "\nSUMMARY fixed_per_hop_ns=%llu (size-independent — the term strip-K ADDS,\n"
        "        a segment[0]==\"net\" literal + a module compare, is measured against this)\n",
        static_cast<unsigned long long>(fixed.empty() ? 0 : fixed[0]));

    // Axis 3 — what a resolution cache could and could not remove from that fixed hop.
    std::printf("\n");
    // RFC-0018 FALSIFIER 1, in this binary: the resolve leg, timed over the PACKED body and
    // over the retired NAME-child body, on the same address with the same rejections and the
    // same `fwd_pre_t` fill. Order-alternated (packed, literal, packed) so a warm-up or a
    // frequency ramp cannot be mistaken for the difference — the two packed readings bracket
    // the literal one, and the SUMMARY below prints their spread alongside the delta.
    // **If the packed arm does not beat the literal arm here, RFC-0018 is void** (§10.1).
    const std::uint64_t resolve_ns = run_leg("fwd-demux-resolve", false);
    // RENAMED from `fwd-demux-resolve-literal` (#1346). The old row measured a hand-rolled
    // walk, not the retired one, and read 31 ns light; keeping the name across a change in
    // WHAT the row measures is the one thing the methodology never allows.
    const std::uint64_t resolve_lit_ns =
        run_leg("fwd-demux-resolve-legacy", false, path_form_t::LITERAL);
    const std::uint64_t resolve_ns2 = run_leg("fwd-demux-resolve", false);
    const std::uint64_t rebuild_ns = run_leg("fwd-demux-rebuild", true);

    const double hop = static_cast<double>(fixed.empty() ? 0 : fixed[0]);
    const double scan_hi = scan.empty() ? 0.0 : static_cast<double>(scan.back()) - hop;
    std::printf("\n%-22s %-12s %s\n", "leg", "p50_ns", "share of the fixed hop");
    std::printf("%-22s %-12llu %.1f%%   <- CEILING on a resolve-once cache\n",
                "resolve (cacheable)", static_cast<unsigned long long>(resolve_ns),
                hop == 0.0 ? 0.0 : 100.0 * static_cast<double>(resolve_ns) / hop);
    std::printf("%-22s %-12llu %.1f%%   <- FLOOR: per-frame, no cache removes it\n",
                "rebuild (per-frame)", static_cast<unsigned long long>(rebuild_ns),
                hop == 0.0 ? 0.0 : 100.0 * static_cast<double>(rebuild_ns) / hop);
    std::printf(
        "\nSUMMARY a perfect resolve-once cache saves at most %llu ns of a %.0f ns hop"
        " (%.1f%%),\n        plus the registry scan, which is %.0f ns at %zu links and"
        " ~0 at <=16.\n",
        static_cast<unsigned long long>(resolve_ns), hop,
        hop == 0.0 ? 0.0 : 100.0 * static_cast<double>(resolve_ns) / hop, scan_hi,
        kLinkCounts[std::size(kLinkCounts) - 1]);

    // Falsifier 1's verdict line. `packed_spread` is the honest error bar: the two packed
    // readings were taken either side of the literal one, so a delta smaller than their own
    // spread is not a result.
    const double packed =
        0.5 * (static_cast<double>(resolve_ns) + static_cast<double>(resolve_ns2));
    const double spread = static_cast<double>(resolve_ns > resolve_ns2 ? resolve_ns - resolve_ns2
                                                                       : resolve_ns2 - resolve_ns);
    const double lit = static_cast<double>(resolve_lit_ns);
    std::printf(
        "\nFALSIFIER-1 resolve leg: packed_p50_ns=%.1f (spread %.1f over two readings)"
        " literal_p50_ns=%.1f\n"
        "            delta_ns=%.1f (%.1f%% of the literal leg) verdict=%s\n"
        "            RFC-0018 §10.1: the packed arm MUST beat the literal arm here or the"
        " RFC is void.\n",
        packed, spread, lit, lit - packed, lit == 0.0 ? 0.0 : 100.0 * (lit - packed) / lit,
        (lit - packed) > spread ? "PACKED-WINS" : "INCONCLUSIVE-OR-REFUTED");
    return 0;
}
