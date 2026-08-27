/**
 * @file
 * @brief #1546 — the reference PLAYOUT helper (`playout.hpp`): what it derives, and what it
 *        refuses to do (RFC-0025 §4.7 / §4.3 / §4.2.1 Amendment 1).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * PLAYOUT is the one clock of the three-clock model that is never transmitted: the consumer
 * DERIVES it from the SAMPLE time in the batch's payload `TIME` child. This file pins that
 * derivation and — equally — the four things §4.7's ruled scope refuses.
 *
 * Seven claims:
 *
 * 1. **A uniform stream's times are ARITHMETIC over the descriptor's rate**, at 0 bytes per
 *    sample: `t(i) = base + i × dt_ns`, with the record carrying no offset run at all.
 * 2. **A non-uniform stream's times come out of the packed `i32` run**, `base + offsets[i]`,
 *    including negative offsets.
 * 3. **Late is flagged against the CALLER's "now" and the CALLER's budget** — a flag, never a
 *    disposition: a late sample is still visited, in order, like every other.
 * 4. **A gap surfaces and resets the expectation.** A reported `tr::flow::address_shift_gap`
 *    makes the report `GAP`, discards the sequence expectation, and — the half that would be
 *    easy to get wrong — does NOT cause a single sample of the batch it precedes to be
 *    skipped. Loss is reported, never masked and never "recovered from".
 * 5. **Nothing is reordered, dropped or PACED.** Every frame is visited exactly once in index
 *    order, and a batch whose samples are seconds in the future returns immediately: the
 *    helper never sleeps and never arms a timer (RFC-0005's unqualified ban). This test reads
 *    a clock to prove that; the helper never does.
 * 6. **An underivable time is reported, never wrapped**, and the expectation is dropped rather
 *    than projected past the representable epoch. Lateness saturates for the same reason.
 * 7. **A NEGATIVE derived time re-primes rather than overflowing** (#1580): the epoch headroom
 *    is computed on both arms, so a peer's negative offset cannot reach signed overflow.
 *
 * Plus the structural claim the "no library-internal buffers" doctrine turns on: **the
 * derivation allocates nothing.** The instrument is the global `operator new` counter of
 * `batch_compose_alloc_test` (itself on the `handler_write_alloc_test` precedent), armed only
 * around the `playout_batch` calls — with a positive control, because a zero-allocation
 * assertion passes loudest when the counter has silently died.
 */

#include "libtracer/playout.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <vector>

#include "libtracer/batch.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/tlv_emit.hpp"
#include "test_support.hpp"

namespace {

/** @brief Global-new call counter, live only while @ref g_arm is set. */
std::size_t g_allocs = 0;
/** @brief True while the counter is measuring — off during every test's setup. */
bool g_arm = false;

/** @brief The counted allocation itself — malloc-backed so `operator delete` can free it. */
void* counted(std::size_t n) {
    if (g_arm) ++g_allocs;
    return std::malloc(n == 0 ? 1 : n);
}

/** @brief The aligned counted allocation — `aligned_alloc` only for a genuinely OVER-aligned
 *         request (which `free` accepts), `malloc` for a fundamental one. */
void* counted_aligned(std::size_t n, std::size_t align) {
    if (align <= alignof(std::max_align_t)) return counted(n);
    if (g_arm) ++g_allocs;
    const std::size_t rounded = ((n == 0 ? 1 : n) + align - 1) / align * align;
    return std::aligned_alloc(align, rounded);
}

}  // namespace

void* operator new(std::size_t n) {
    void* const p = counted(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) {
    void* const p = counted(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new(std::size_t n, std::align_val_t a) {
    void* const p = counted_aligned(n, static_cast<std::size_t>(a));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned(n, static_cast<std::size_t>(a));
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

namespace {

using tr::testing::check;
using tr::wire::batch_view_t;
using tr::wire::playout_batch;
using tr::wire::playout_clock_t;
using tr::wire::playout_continuity_t;
using tr::wire::playout_cursor_t;
using tr::wire::playout_report_t;
using tr::wire::playout_sample_t;

/** @brief The batch base — the app's number, 2023-11-14T22:13:20Z in ns. */
constexpr std::int64_t kBase = 1'700'000'000'000'000'000;

/** @brief The descriptor's nominal sample period for the uniform arm: a 1 kHz acquisition. */
constexpr std::uint64_t kDt = 1000;

/** @brief How many sample frames every batch below folds. */
constexpr std::size_t kSamples = 3;

/**
 * @brief One decoded batch and the bytes and `tlv_t` it borrows from — the lifetime a
 *        @ref tr::wire::batch_view_t requires, kept in one object.
 */
struct decoded_batch_t {
    std::vector<std::byte> bytes;
    std::optional<tr::wire::tlv_t> tlv;
    batch_view_t view;
};

/** @brief One sample frame: `VALUE{ u16 LE }` — the ADC-reading shape. */
std::vector<std::byte> sample_frame(std::uint16_t reading) {
    std::vector<std::byte> out;
    tr::wire::emit_value_le<std::uint16_t>(out, reading);
    return out;
}

/**
 * @brief Fold @ref kSamples frames at @p base_ns and decode them back, against a descriptor
 *        declaring @p dt_ns.
 *
 * @param offsets_ns The non-uniform stream's packed run, or empty for the uniform arm.
 */
decoded_batch_t build(std::int64_t base_ns, std::uint64_t dt_ns,
                      std::span<const std::int32_t> offsets_ns = {}) {
    decoded_batch_t d;
    std::vector<std::vector<std::byte>> frames;
    std::vector<std::span<const std::byte>> spans;
    for (std::uint16_t r = 0x0064; r < 0x0064 + kSamples; ++r) frames.push_back(sample_frame(r));
    for (const std::vector<std::byte>& f : frames) spans.emplace_back(f);
    tr::wire::emit_batch(d.bytes, base_ns, spans, offsets_ns);

    auto got = tr::wire::decode(d.bytes, tr::mem::heap_source());
    check(got.has_value(), "the folded batch decodes as an ordinary structured TLV");
    d.tlv = std::move(*got);
    const std::optional<batch_view_t> v = tr::wire::read_batch(*d.tlv, dt_ns);
    check(v.has_value(), "... and read_batch spells the convention out of it");
    d.view = *v;
    return d;
}

/** @brief A visitor's record of what it saw, in visitation order — fixed storage, so the
 *         armed allocation counter measures the derivation and not the recorder. */
struct trace_t {
    std::array<std::size_t, 8> index{};
    std::array<std::int64_t, 8> time_ns{};
    std::array<std::int64_t, 8> lateness_ns{};
    std::array<bool, 8> known{};
    std::array<bool, 8> late{};
    std::size_t seen = 0;

    /** @brief Record one visited sample. */
    void operator()(const playout_sample_t& s) {
        if (seen >= index.size()) return;
        index[seen] = s.index;
        time_ns[seen] = s.sample_time_ns;
        lateness_ns[seen] = s.lateness_ns;
        known[seen] = s.time_known;
        late[seen] = s.late;
        ++seen;
    }
};

/** @brief Claim 1: a uniform stream's times are derived from the descriptor's rate, and the
 *         record spends NOTHING per sample to say so. */
void uniform_times_are_derived_at_zero_bytes_per_sample() {
    std::printf("§4.2.1 uniform — t(i) = base + i x dt_ns, at 0 B/sample:\n");
    const decoded_batch_t d = build(kBase, kDt);
    check(d.view.uniform() && d.view.offsets.empty(),
          "a uniform batch carries no offset run at all — the rate is the descriptor's");

    playout_cursor_t cursor{};
    trace_t t;
    const playout_report_t r =
        playout_batch(cursor, d.view, playout_clock_t{.now_ns = kBase}, 0, t);

    check(r.samples == kSamples && t.seen == kSamples, "every sample frame is visited, once");
    check(r.continuity == playout_continuity_t::FIRST && !r.expected_valid,
          "the first batch on a fresh cursor is FIRST, with no expectation to report");
    bool derived = true;
    for (std::size_t i = 0; i < kSamples; ++i)
        derived =
            derived && t.known[i] && t.time_ns[i] == kBase + static_cast<std::int64_t>(i * kDt);
    check(derived, "t(0)=base, t(1)=base+1000, t(2)=base+2000 — arithmetic, not bytes");
    check(cursor.primed && cursor.next_sample_time_ns == kBase + 3000,
          "the cursor is primed at the last derived time plus the declared period");
}

/** @brief Claim 2: a non-uniform stream's times come out of the packed `i32` run. */
void non_uniform_times_come_from_the_packed_run() {
    std::printf("§4.2.1 non-uniform — t(i) = base + offsets[i], out of ONE packed child:\n");
    constexpr std::array<std::int32_t, kSamples> kOffsets{0, 250, -125};
    const decoded_batch_t d = build(kBase, 0, kOffsets);
    check(!d.view.uniform() && d.view.offsets.size() == kSamples * tr::wire::kBatchOffsetBytes,
          "a non-uniform batch carries exactly one i32 per frame, contiguously");

    playout_cursor_t cursor{};
    trace_t t;
    const playout_report_t r =
        playout_batch(cursor, d.view, playout_clock_t{.now_ns = kBase}, 0, t);

    check(r.samples == kSamples && t.seen == kSamples, "every sample frame is visited, once");
    check(t.time_ns[0] == kBase && t.time_ns[1] == kBase + 250 && t.time_ns[2] == kBase - 125,
          "each offset is applied to the base, signed — a NEGATIVE offset moves time back");
    check(cursor.primed && cursor.next_sample_time_ns == kBase - 125,
          "with no declared rate there is no forward projection: the expectation is the last "
          "sample's own time, a lower bound");
}

/** @brief Claim 3: late is the caller's "now" against the caller's budget, and it is a flag
 *         and not a disposition. */
void late_is_flagged_against_the_callers_now() {
    std::printf("§4.7 lateness — the caller's clock, the caller's budget, no disposition:\n");
    const decoded_batch_t d = build(kBase, kDt);

    playout_cursor_t cursor{};
    trace_t at_last;
    const playout_report_t r =
        playout_batch(cursor, d.view, playout_clock_t{.now_ns = kBase + 2000}, 0, at_last);
    check(at_last.lateness_ns[0] == 2000 && at_last.lateness_ns[2] == 0,
          "lateness is now - t(i), signed, per sample");
    check(at_last.late[0] && at_last.late[1] && !at_last.late[2],
          "late is STRICTLY past the budget — a sample due exactly now is not late");
    check(r.late == 2 && r.samples == kSamples,
          "the census counts the late ones and visits ALL of them: a flag, never a drop");

    cursor.forget();
    trace_t budgeted;
    const playout_report_t b =
        playout_batch(cursor, d.view,
                      playout_clock_t{.now_ns = kBase + 2000, .late_after_ns = 1000}, 0, budgeted);
    check(b.late == 1 && budgeted.late[0] && !budgeted.late[1],
          "a de-jitter depth of 1 ms is the caller's to declare, and it moves the line");

    cursor.forget();
    trace_t ahead;
    const playout_report_t a =
        playout_batch(cursor, d.view, playout_clock_t{.now_ns = kBase - 5000}, 0, ahead);
    check(a.late == 0 && ahead.lateness_ns[0] == -5000,
          "a sample still ahead of now has negative lateness and is not late");
}

/** @brief Claim 4: a reported gap surfaces, resets the expectation, and skips nothing. */
void a_gap_surfaces_and_resets_the_expectation() {
    std::printf("§4.4/§4.5 loss — address_shift_gap surfaces and resets, never masks:\n");
    const decoded_batch_t first = build(kBase, kDt);
    const decoded_batch_t next = build(kBase + 3000, kDt);
    const decoded_batch_t after = build(kBase + 90'000, kDt);
    const playout_clock_t clock{.now_ns = kBase};

    playout_cursor_t cursor{};
    trace_t t0;
    const playout_report_t r0 = playout_batch(cursor, first.view, clock, 0, t0);
    check(r0.continuity == playout_continuity_t::FIRST, "batch 1 primes the cursor");

    trace_t t1;
    const playout_report_t r1 = playout_batch(cursor, next.view, clock, 0, t1);
    check(r1.continuity == playout_continuity_t::CONTINUOUS && r1.expected_valid &&
              r1.expected_sample_time_ns == kBase + 3000,
          "batch 2 is CONTINUOUS, and the expectation it met travels in the report");

    trace_t t2;
    const playout_report_t r2 = playout_batch(cursor, after.view, clock, 2, t2);
    check(r2.continuity == playout_continuity_t::GAP && r2.gaps == 2,
          "a reported shed makes the batch GAP and passes the census through verbatim");
    check(!r2.expected_valid,
          "the sequence expectation is DISCARDED — no drift is claimed across a hole");
    check(r2.samples == kSamples && t2.seen == kSamples,
          "and not one sample of the batch behind the gap is skipped");

    trace_t t3;
    const decoded_batch_t resumed = build(kBase + 93'000, kDt);
    const playout_report_t r3 = playout_batch(cursor, resumed.view, clock, 0, t3);
    check(r3.continuity == playout_continuity_t::CONTINUOUS && r3.expected_valid,
          "the batch AFTER the gap re-establishes the expectation from what it saw");

    cursor.forget();
    check(!cursor.primed, "forget() is the same reset, exposed for the caller's own reasons");
}

/** @brief Claim 5: order is preserved, nothing is dropped, and nothing is PACED. */
void nothing_is_reordered_dropped_or_paced() {
    std::printf("§4.7 refusals — no reorder, no drop, no de-jitter, NO PACING:\n");
    // Samples one second apart, all far in the future: a pacing helper would sleep for them.
    const decoded_batch_t d = build(kBase, 1'000'000'000u);
    playout_cursor_t cursor{};
    trace_t t;

    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    const playout_report_t r =
        playout_batch(cursor, d.view, playout_clock_t{.now_ns = kBase}, 0, t);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    check(elapsed < 200,
          "a batch spanning two seconds of sample time returns immediately — the helper never "
          "sleeps and never arms a timer");
    check(r.samples == kSamples && t.seen == kSamples && t.index[0] == 0 && t.index[1] == 1 &&
              t.index[2] == 2,
          "every frame visited exactly once, in index order — no reordering, no dropping");
}

/** @brief Claim 6: an underivable time is reported rather than wrapped, and so is an
 *         unrepresentable lateness. */
void an_underivable_time_is_reported_not_wrapped() {
    std::printf("§4.2.1 edges — unrepresentable is REPORTED, never wrapped into plausible:\n");
    constexpr std::int64_t kNearMax = std::numeric_limits<std::int64_t>::max() - 1500;
    const decoded_batch_t d = build(kNearMax, kDt);

    playout_cursor_t cursor{};
    trace_t t;
    const playout_report_t r =
        playout_batch(cursor, d.view, playout_clock_t{.now_ns = kBase}, 0, t);
    check(r.samples == kSamples && r.undated == 1 && t.known[0] && t.known[1] && !t.known[2],
          "the sample whose derivation leaves the ns epoch is visited and marked UNDATED");
    check(t.time_ns[2] == 0 && !t.late[2],
          "an undated sample carries no fabricated time and is not flagged late");
    check(!cursor.primed,
          "the expectation is dropped rather than projected past the representable epoch");

    // Lateness of a "now" an epoch away from the sample: saturated, not wrapped into early.
    playout_cursor_t c2{};
    trace_t far;
    const playout_report_t f =
        playout_batch(c2, build(kBase, kDt).view,
                      playout_clock_t{.now_ns = std::numeric_limits<std::int64_t>::min()}, 0, far);
    check(far.lateness_ns[0] == std::numeric_limits<std::int64_t>::min() && f.late == 0,
          "an underflowing difference SATURATES — a hopelessly early sample never reads late");
}

/**
 * @brief Claim 7 (#1580): a NEGATIVE derived sample time re-primes the cursor instead of
 *        overflowing the headroom subtraction.
 *
 * The re-prime's headroom used to be spelled `int64max - last_ns`, which is signed overflow —
 * UB — the moment the last derived time is negative. That is peer-reachable: a non-uniform
 * batch is `base + offsets[i]` and §4.2.1 permits a negative `i32` offset, so `read_batch`'s
 * `base_ns >= 0` guarantee does not survive the addition.
 *
 * Only the `dt_ns == 0` arm is pinned, because it is the only one a batch can actually be in
 * with a negative derived time: `read_batch` pins `base_ns >= 0`, and the RATE arm adds a
 * non-negative `i * dt_ns` to that base, so a rated batch's times are never negative.
 */
void a_negative_derived_time_re_primes_without_overflow() {
    std::printf("§4.2.1 edges — a NEGATIVE derived time re-primes, it does not overflow:\n");
    constexpr std::array<std::int32_t, kSamples> kBackwards{-3000, -2000, -1000};
    const decoded_batch_t d = build(0, 0, kBackwards);

    playout_cursor_t cursor{};
    trace_t t;
    const playout_report_t r = playout_batch(cursor, d.view, playout_clock_t{.now_ns = 0}, 0, t);
    check(r.samples == kSamples && t.time_ns[2] == -1000,
          "a base of 0 with negative offsets derives times BEFORE the epoch origin");
    check(cursor.primed && cursor.next_sample_time_ns == -1000,
          "the cursor re-primes at the last derived time plus dt (0 here) — no UB, no forget()");
}

/** @brief The structural claim: the derivation allocates NOTHING. */
void the_derivation_allocates_nothing() {
    std::printf("no library-internal buffers — the derivation allocates nothing:\n");
    const decoded_batch_t uniform = build(kBase, kDt);
    constexpr std::array<std::int32_t, kSamples> kOffsets{0, 250, -125};
    const decoded_batch_t packed = build(kBase, 0, kOffsets);
    playout_cursor_t cursor{};
    trace_t t;
    const playout_clock_t clock{.now_ns = kBase + 2000};

    g_allocs = 0;
    g_arm = true;
    const playout_report_t a = playout_batch(cursor, uniform.view, clock, 0, t);
    const playout_report_t b = playout_batch(cursor, packed.view, clock, 3, t);
    const playout_report_t c = playout_batch(cursor, uniform.view, clock, 0);
    const std::size_t derived = g_allocs;
    // Positive control: the counter must be able to see an allocation on this very line.
    std::vector<std::byte> control;
    control.resize(64);
    const std::size_t after_control = g_allocs;
    g_arm = false;

    check(derived == 0, "three playout_batch calls, visiting and counts-only: ZERO allocations");
    check(after_control > derived, "positive control — the counter was live throughout");
    check(a.samples == kSamples && b.gaps == 3 && c.samples == kSamples,
          "and the measured calls did the work: a live counter over dead calls proves nothing");
}

}  // namespace

/** @brief Runs every playout-helper check; non-zero exit ⇒ some check failed. */
int main() {
    uniform_times_are_derived_at_zero_bytes_per_sample();
    non_uniform_times_come_from_the_packed_run();
    late_is_flagged_against_the_callers_now();
    a_gap_surfaces_and_resets_the_expectation();
    nothing_is_reordered_dropped_or_paced();
    an_underivable_time_is_reported_not_wrapped();
    a_negative_derived_time_re_primes_without_overflow();
    the_derivation_allocates_nothing();
    return tr::testing::summary("playout");
}
