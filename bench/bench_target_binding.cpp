/**
 * @file
 * @brief What `graph::target_binding_t` (#830, shipped in #1174) actually buys on the local
 *        target-edge delivery leg — the depth A/B, its A/A null, and the counter ablation.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * #1174 landed the per-edge binding and #830 stayed OPEN because nothing measured it: the
 * `+21.3 ns/segment` and `flat 11 ns` figures its changelog quotes are #830's own PRIOR
 * measurement of `find_ptr` and `deref_vertex_slot` in isolation, carried over. They are not
 * a measurement of the shipped delivery leg, and they were not taken on this host.
 *
 * @section why_one_binary Why both arms live in ONE binary, and why neither is hand-written
 *
 * The measurement rule this bench is built to satisfy is that a quoted delta must be
 * IN-BINARY: on `bench_forward_demux` byte-identical source read 44 ns in one binary and 56
 * in another, and adding one unrelated function to a translation unit moved an untouched leg
 * +9.8 %. A "with #1174" build against a "without #1174" build could not be quoted here.
 *
 * `dispatch_edge_target` makes the in-binary form free, because it already contains BOTH
 * spellings and picks between them per edge:
 *
 * @code
 *   if (e.binding.bound())
 *       if (const auto bound = deref_vertex_slot(...)) target = bound->get();
 *   if (target == nullptr) { target_canonical_resolves_.fetch_add(...); target = find_ptr(key); }
 * @endcode
 *
 * So the two arms are the SAME function in the SAME binary, differing only in whether the
 * edge carries a minted binding — and what decides that is not a bench switch but the shipped
 * mint rule in `admit_subscriber`: *a target that does not exist yet stays unbound and keeps
 * the canonical spelling*. Subscribing before the target is registered therefore produces a
 * genuinely canonical edge, and subscribing after produces a bound one. Everything downstream
 * of the resolve — the fan-in ACL gate, the nothrow rope clone, `store_value` — is byte-
 * identical between the arms.
 *
 * This is what removes the #1346 unfaithful-control hazard entirely: there is no transcribed
 * control arm to prove faithful, because the control arm IS the shipped fallback leg, reached
 * through the shipped mint rule. Faithfulness is not argued, it is COUNTED —
 * `graph_t::target_canonical_resolves()` increments once per delivery on the canonical arm and
 * never on the bound arm, and every point below asserts both.
 *
 * @section the_one_asymmetry The one asymmetry, and how it is bounded
 *
 * The canonical arm is not free of #1174: it pays the relaxed `fetch_add` on
 * `target_canonical_resolves_`, which the pre-#1174 code did not have. That INFLATES the
 * canonical arm, so the measured win is an over-estimate — by exactly one uncontended relaxed
 * `fetch_add`. Rather than hand-wave the size of that term, the `tgt-bind-ctr` /
 * `tgt-bind-noctr` legs price it in this same binary, and the summary prints the corrected
 * delta beside the raw one. Both are reported; only the corrected one is a claim.
 *
 * @section arms What is emitted
 *
 * | mode                | what it is                                                        |
 * |---------------------|-------------------------------------------------------------------|
 * | `tgt-bind-bound`    | target registered BEFORE the subscribe — `deref_vertex_slot` leg   |
 * | `tgt-bind-bound-aa` | byte-identical construction to the above — the A/A null arm        |
 * | `tgt-bind-canon`    | target registered AFTER the subscribe — the `find_ptr` fallback    |
 * | `tgt-bind-ctr`      | one uncontended relaxed `fetch_add`, the canonical arm's own tax   |
 * | `tgt-bind-noctr`    | the same loop without it — subtract to price the tax              |
 *
 * `endpoints` on every RESULT row is the target key's DEPTH in segments, which is the axis:
 * `find_ptr` walks it and the deref does not.
 *
 * @section reading How to read it
 *
 * Best of N executions per mode, never the median of them — contamination is one-sided, and
 * on this host median-of-rounds once put one binary against itself at −33 %…+54 % where
 * best-of-rounds on the same samples stayed inside 1.44 %. Run `python3 bench/host_guard.py
 * wait` first and record `/proc/loadavg` either side. A `[min..max]` spread near 2× with the
 * ends clustered means the window was contaminated: repeat it, do not report it.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::view::view_t;

/** @brief Target-key depths swept: a SHALLOW key and a DEEP one, plus a midpoint. */
constexpr std::size_t kDepths[] = {1, 4, 12};

/** @brief Payload bytes each delivery carries — fixed; this bench's axis is depth, not size. */
constexpr std::size_t kPayload = 64;

/** @brief Seconds each arm is sampled for, overridable so a smoke run is cheap. */
[[nodiscard]] double budget_seconds() {
    if (const char* s = std::getenv("BENCH_BUDGET_S")) {
        const double v = std::atof(s);
        if (v > 0.0) return v;
    }
    return 1.0;
}

/** @brief A VALUE TLV of @ref kPayload bytes — the value every arm writes. */
[[nodiscard]] std::vector<std::byte> value_tlv() {
    std::vector<std::byte> p(kPayload, std::byte{0xAB});
    tr::wire::tlv_t t{};
    t.type = tr::wire::type_t::VALUE;
    t.payload = p;
    return tr::wire::encode(t);
}

/** @brief Per-write owned heap view — the allocating path every fan-out row here uses. */
[[nodiscard]] view_t owned_view(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/**
 * @brief The absolute target path at depth @p depth — `/b0/b1/.../t`, @p depth segments total.
 *
 * The intermediates are never registered: `register_vertex` on the leaf creates them as
 * structural placeholders, which is the ordinary shape of a deep address and the one
 * `find_ptr` walks. Registering them would change what the canonical arm is timing.
 */
[[nodiscard]] std::string target_path(std::size_t depth) {
    std::string s;
    for (std::size_t i = 0; i + 1 < depth; ++i) s += "/b" + std::to_string(i);
    s += "/t";
    return s;
}

/**
 * @brief A SUBSCRIBER TLV whose PATH child is @p key — the WIRE subscribe form.
 *
 * `g.subscribe(path, callback)` cannot produce this: its edges carry a NULL `target_key` and
 * so never reach `dispatch_edge_target` at all. A packed PATH (`opt.PL = 0`, RFC-0018 records)
 * is what the shipped door parses, and its segments are what `find_ptr` re-walks per delivery
 * on the canonical arm.
 */
[[nodiscard]] view_t subscriber_tlv(const std::string& key) {
    std::vector<std::byte> body;
    std::size_t pos = 1;  // skip the leading '/'
    while (pos <= key.size()) {
        const std::size_t next = key.find('/', pos);
        const std::size_t end = next == std::string::npos ? key.size() : next;
        (void)tr::wire::emit_path_segment(body, std::string_view(key).substr(pos, end - pos));
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    tr::wire::tlv_t path{.type = tr::wire::type_t::PATH, .payload = body};  // packed, PL = 0
    tr::wire::tlv_t sub{.type = tr::wire::type_t::SUBSCRIBER};
    sub.opt.pl = true;
    sub.children.push_back(path);
    return owned_view(tr::wire::encode(sub));
}

/** @brief Which of `dispatch_edge_target`'s two resolve spellings the edge will take. */
enum class arm_t : std::uint8_t {
    BOUND,     /**< @brief Target exists at admission — `admit_subscriber` mints, deref leg. */
    CANONICAL, /**< @brief Target absent at admission — stays unbound, `find_ptr` leg. */
};

/** @brief One arm's measured result, carried back to the summary. */
struct point_t {
    std::uint64_t p50 = 0; /**< @brief Median per-delivery nanoseconds. */
    std::uint64_t lo = 0;  /**< @brief Fastest sample — the contamination diagnostic's floor. */
    std::uint64_t hi = 0;  /**< @brief Slowest sample. */
    bool ok = false;       /**< @brief Did the counter ablation agree with the arm's claim? */
};

/**
 * @brief Time one delivery arm at target-key depth @p depth, and ABLATE it while doing so.
 *
 * The ablation is not a separate test that could drift from the measurement — it is read off
 * the very run being timed. `target_canonical_resolves()` counts ONLY the fallback, so on the
 * bound arm it must not move across the whole timed window, and on the canonical arm it must
 * move exactly once per delivery. Either mismatch means the arm did not take the leg its name
 * claims, and the point is emitted with `ok = false` so no summary line can quote it.
 *
 * @param depth Segments in the target key.
 * @param arm   Which leg to construct.
 * @param mode  RESULT row label.
 */
[[nodiscard]] point_t run_arm(std::size_t depth, arm_t arm, const char* mode) {
    graph_t g;
    const std::string key = target_path(depth);
    const path_t tpath = *path_t::parse(key);
    const path_t src_path = *path_t::parse("/src");
    const path_t sub_path = *path_t::parse("/src:subscribers[]");

    // ORDER IS THE WHOLE EXPERIMENT. `admit_subscriber` mints only if `find_ptr(target_key)`
    // already answers, so registering the target before or after the subscribe is what
    // decides which leg every subsequent delivery takes. Nothing else differs between arms.
    if (arm == arm_t::BOUND) (void)g.register_vertex(tpath, role_t::STORED_VALUE);
    const vertex_handle_t src = g.register_vertex(src_path, role_t::STORED_VALUE);
    if (!g.write(sub_path, subscriber_tlv(key)).has_value()) {
        std::printf("SKIP mode=%s depth=%zu: the target edge did not admit\n", mode, depth);
        return {};
    }
    if (arm == arm_t::CANONICAL) (void)g.register_vertex(tpath, role_t::STORED_VALUE);

    const std::vector<std::byte> tlv = value_tlv();
    const auto write_once = [&] { (void)g.write(src, owned_view(tlv)); };

    // Calibration doubles as the warm-up, and its own timings are discarded. The WINDOW rule,
    // not the retired plateau rule: the plateau rule compares two timed quantities, so the
    // machine picks the batch and the batch moves the number by up to ~8 % in discrete
    // clusters (#1358) — which is the exact shape of the effect this bench is looking for.
    const std::size_t batch = bench::calibrate_batch_for_window(write_once);

    const std::uint64_t before = g.target_canonical_resolves();
    bench::Latency lat;
    std::vector<std::uint64_t> per_batch;
    const auto deadline_ns = static_cast<std::uint64_t>(budget_seconds() * 1e9);
    const std::uint64_t t0 = bench::now_ns();
    std::size_t batches = 0;
    std::uint64_t total = 0;
    while (total < deadline_ns) {
        const std::uint64_t a = bench::now_ns();
        for (std::size_t i = 0; i < batch; ++i) write_once();
        const std::uint64_t per_op = (bench::now_ns() - a) / batch;
        lat.add(per_op);
        per_batch.push_back(per_op);
        ++batches;
        total = bench::now_ns() - t0;
    }
    const std::uint64_t resolves = g.target_canonical_resolves() - before;
    const std::uint64_t deliveries = static_cast<std::uint64_t>(batches) * batch;

    // THE ABLATION, on the timed run itself. `expect` is 0 for a bound edge and one per
    // delivery for an unbound one; anything else and this arm is not the leg it claims.
    const std::uint64_t expect = arm == arm_t::BOUND ? 0 : deliveries;
    const bool ok = resolves == expect;

    const bench::Latency::Summary s = lat.summarize();
    const double per_s =
        total == 0 ? 0.0 : static_cast<double>(deliveries) * 1e9 / static_cast<double>(total);
    bench::emit("libtracer", mode, kPayload, 1, depth, per_s, per_s, 0.0, s);
    std::uint64_t lo = per_batch.empty() ? 0 : per_batch.front();
    std::uint64_t hi = lo;
    for (const std::uint64_t v : per_batch) {
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    std::printf(
        "NOTE mode=%s depth=%zu batch=%zu batches=%zu deliveries=%llu canonical_resolves=%llu "
        "expected=%llu ablation=%s p50=%lluns lo=%lluns hi=%lluns\n",
        mode, depth, batch, batches, static_cast<unsigned long long>(deliveries),
        static_cast<unsigned long long>(resolves), static_cast<unsigned long long>(expect),
        ok ? "PASS" : "FAIL", static_cast<unsigned long long>(s.p50),
        static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi));
    if (!ok)
        std::printf("WARN mode=%s depth=%zu ABLATION FAILED — this point measures the WRONG leg\n",
                    mode, depth);
    return {s.p50, lo, hi, ok};
}

/**
 * @brief Price the canonical arm's own #1174 tax: one uncontended relaxed `fetch_add`.
 *
 * `dispatch_edge_target`'s fallback increments `target_canonical_resolves_` before calling
 * `find_ptr`, so the canonical arm above is NOT the pre-#1174 baseline — it is the pre-#1174
 * baseline plus this. Measuring it here, in this binary, turns "the win is slightly
 * overstated" from a caveat into a number the summary can subtract.
 *
 * Both legs run the same loop shape over the same accumulator width; only the atomicity of the
 * increment differs, and `sink` is printed so neither can be elided.
 *
 * @param atomic Use the relaxed `fetch_add` rather than a plain increment.
 * @param mode   RESULT row label.
 */
[[nodiscard]] double run_counter_leg(bool atomic, const char* mode) {
    std::atomic<std::uint64_t> counter{0};
    std::uint64_t plain = 0;
    volatile std::uint64_t sink = 0;
    const auto op = [&] {
        if (atomic)
            counter.fetch_add(1, std::memory_order_relaxed);
        else
            ++plain;
        sink = sink + 1;
    };
    const std::size_t batch = bench::calibrate_batch_for_window(op);
    bench::Latency lat;
    const auto deadline_ns = static_cast<std::uint64_t>(budget_seconds() * 1e9);
    const std::uint64_t t0 = bench::now_ns();
    std::size_t batches = 0;
    std::uint64_t total = 0;
    while (total < deadline_ns) {
        const std::uint64_t a = bench::now_ns();
        for (std::size_t i = 0; i < batch; ++i) op();
        lat.add((bench::now_ns() - a) / batch);
        ++batches;
        total = bench::now_ns() - t0;
    }
    const bench::Latency::Summary s = lat.summarize();
    bench::emit("libtracer", mode, kPayload, 1, 0, 0.0, 0.0, 0.0, s);
    // Reported as a DOUBLE over the whole window, not as the p50: these legs cost single-digit
    // nanoseconds, where `Latency`'s integer per-sample division truncates by up to 1 ns —
    // a quarter of the quantity being measured, and the term this figure is subtracted from
    // is worth tens of nanoseconds. The delta of two truncated integers is not good enough.
    const double per_op =
        static_cast<double>(total) / (static_cast<double>(batches) * static_cast<double>(batch));
    std::printf("NOTE mode=%s batch=%zu batches=%zu sink=%llu p50=%lluns per_op=%.3fns\n", mode,
                batch, batches, static_cast<unsigned long long>(sink),
                static_cast<unsigned long long>(s.p50), per_op);
    return per_op;
}

/** @brief Percent difference of @p a from @p b, guarding a zero denominator. */
[[nodiscard]] double pct(double a, double b) { return b == 0.0 ? 0.0 : 100.0 * (a - b) / b; }

}  // namespace

int main() {
    std::printf("# bench_target_binding — what target_binding_t buys the local target edge\n");
    std::printf("# arms differ ONLY in whether the target existed at admit_subscriber time\n");

    // A DISCARDED first arm. `calibrate_batch_for_window` warms each arm's own caches, but it
    // cannot warm the PROCESS: the first `graph_t` in the executable pays lazy statics, the
    // first heap growth and the first touch of every code page the delivery leg spans. Left in
    // the sweep that cost showed up as a 24 ns bracket spread at the first depth and nowhere
    // else — an A/A null an order of magnitude worse than the 1 ns the later depths read,
    // which would have swamped the shallow arm's whole effect.
    std::printf("# discarded warm-up arm — the process's first graph_t, not a data point\n");
    (void)run_arm(kDepths[0], arm_t::BOUND, "tgt-bind-warmup");

    point_t bound_a[std::size(kDepths)];
    point_t bound_aa[std::size(kDepths)];
    point_t canon[std::size(kDepths)];
    point_t bound_b[std::size(kDepths)];

    for (std::size_t i = 0; i < std::size(kDepths); ++i) {
        const std::size_t d = kDepths[i];
        // ORDER-ALTERNATED and BRACKETED: two bound readings straddle the canonical one, so a
        // warm-up or a frequency ramp cannot be mistaken for the effect, and their own spread
        // is the honest error bar the verdict is read against. `bound-aa` is a THIRD reading
        // of the identical construction, immediately adjacent — the tightest A/A null
        // available, since nothing at all separates its arm from `bound`'s.
        bound_a[i] = run_arm(d, arm_t::BOUND, "tgt-bind-bound");
        bound_aa[i] = run_arm(d, arm_t::BOUND, "tgt-bind-bound-aa");
        canon[i] = run_arm(d, arm_t::CANONICAL, "tgt-bind-canon");
        bound_b[i] = run_arm(d, arm_t::BOUND, "tgt-bind-bound");
    }

    const double ctr_ns = run_counter_leg(true, "tgt-bind-ctr");
    const double noctr_ns = run_counter_leg(false, "tgt-bind-noctr");
    const double tax = ctr_ns - noctr_ns;

    std::printf("\n%-7s %-11s %-11s %-11s %-11s %s\n", "depth", "bound_ns", "bound_aa", "canon_ns",
                "aa_null_%", "delta_%");
    for (std::size_t i = 0; i < std::size(kDepths); ++i) {
        const double b =
            0.5 * (static_cast<double>(bound_a[i].p50) + static_cast<double>(bound_b[i].p50));
        std::printf("%-7zu %-11.1f %-11llu %-11llu %-11.2f %.2f\n", kDepths[i], b,
                    static_cast<unsigned long long>(bound_aa[i].p50),
                    static_cast<unsigned long long>(canon[i].p50),
                    pct(static_cast<double>(bound_aa[i].p50), b),
                    pct(static_cast<double>(canon[i].p50), b));
    }

    std::printf(
        "\nCOUNTER-TAX one relaxed fetch_add = %.3f ns (%.3f - %.3f). The canonical arm\n"
        "            pays it and the pre-#1174 baseline did not, so the RAW delta below\n"
        "            OVERSTATES the binding's win by exactly this much.\n",
        tax, ctr_ns, noctr_ns);

    std::printf("\n%-7s %-13s %-13s %-13s %s\n", "depth", "raw_delta_ns", "corrected_ns",
                "bracket_ns", "verdict");
    for (std::size_t i = 0; i < std::size(kDepths); ++i) {
        const double b =
            0.5 * (static_cast<double>(bound_a[i].p50) + static_cast<double>(bound_b[i].p50));
        const double raw = static_cast<double>(canon[i].p50) - b;
        // The bracket spread and the adjacent A/A reading are the two in-binary nulls. A delta
        // must clear the LARGER of them before it is a result at all.
        const double bracket =
            static_cast<double>(bound_a[i].p50 > bound_b[i].p50 ? bound_a[i].p50 - bound_b[i].p50
                                                                : bound_b[i].p50 - bound_a[i].p50);
        const double aa = static_cast<double>(bound_aa[i].p50) > b
                              ? static_cast<double>(bound_aa[i].p50) - b
                              : b - static_cast<double>(bound_aa[i].p50);
        const double null = bracket > aa ? bracket : aa;
        const bool sound = bound_a[i].ok && bound_aa[i].ok && canon[i].ok && bound_b[i].ok;
        std::printf("%-7zu %-13.1f %-13.1f %-13.1f %s\n", kDepths[i], raw, raw - tax, bracket,
                    !sound               ? "UNSOUND-ABLATION-FAILED"
                    : (raw - tax) > null ? "BINDING-WINS"
                                         : "INSIDE-THE-NULL");
    }
    std::printf(
        "\nSUMMARY every delta above is IN-BINARY: both arms are the same `dispatch_edge_target`\n"
        "        in this executable, selected by the shipped mint rule in `admit_subscriber`,\n"
        "        and each point's leg is COUNTED by target_canonical_resolves(), not assumed.\n");
    return 0;
}
