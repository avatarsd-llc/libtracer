/**
 * @file
 * @brief ACL-cache coherence under a concurrent subtree invalidation (#880, ADR-0078).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The per-vertex effective-ACE merge is invalidated LOCK-FREE from an ancestor `:acl`
 * write (`graph_t::mark_subtree_acl_dirty` → `vertex_t::mark_acl_cache_dirty`, under only
 * `shared_lock(map_mutex_)`) while it is rebuilt under the vertex stripe lock. The
 * predecessor protocol expressed validity as a `{acl_gen, acl_cache_dirty}` PAIR whose
 * boolean the rebuilder cleared: an invalidation landing between the rebuilder's
 * generation recheck and that clear was overwritten, leaving the merge flagged CLEAN over
 * PRE-WRITE ancestor ACEs — a revoked policy still enforced, silently, until the next
 * `:acl` mutation anywhere in the chain. ADR-0078 folds validity into the counter itself
 * (odd ⇒ stale) and makes the publish a CAS, so recheck and publish are ONE atomic step.
 *
 * This is a race, so the instrument is a genuine racer, not a sequence of calls:
 *
 * - **Thread A (writer)** rewrites the TOP ancestor's `:acl` epoch by epoch, alternating
 *   which subject its one inheritable grant names, and brackets each write with `pending`
 *   (stored BEFORE) and `committed` (stored AFTER).
 * - **Thread B (prober, the main thread)** evaluates `graph_t::read` — i.e. `acl_allows` —
 *   on a DESCENDANT that carries its own ACL, so that descendant is the BEARING vertex and
 *   its merge is invalidated only by the lock-free subtree mark, never by its own
 *   `set_acl` (which takes the very stripe lock the rebuilder holds, and so cannot
 *   interleave into the window at all).
 *
 * The claim B makes is bracketed: when `committed == pending == e` around one evaluation,
 * epoch e's write happened-before it and epoch e+1's had not begun, so the verdict MUST be
 * epoch e's. Reading a stale merge there is the defect, directly observed. A quiescent
 * re-check after the writer joins then confirms the PERSISTENCE that makes it severe.
 *
 * Two shape choices decide whether the window is reachable at all, and both are asymmetric
 * on purpose (see @ref kChainDepth and @ref kSweepSteps): the merge is made EXPENSIVE while
 * the write that invalidates it is made CHEAP, and the writer's inter-epoch delay is SWEPT.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/security_acl.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::ace_t;
using tr::graph::ace_type_t;
using tr::graph::acl_right_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::subject_token_t;
using tr::graph::vertex_handle_t;

int g_failures = 0;

/** @brief Print one PASS/FAIL line for @p what and tally the failures. */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief The bytes of @p s, the opaque-token spelling this test uses for subjects. */
std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    if (!s.empty()) std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief The test resolver (ADR-0018): the caller context IS the subject token. */
std::expected<subject_token_t, tr::wire::err_t> caller_is_subject(std::string_view caller) {
    return as_bytes(caller);
}

/** @brief A single-segment view over a copy of @p bytes. */
tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

/** @brief The subject the prober presents; an EVEN epoch grants it READ, an ODD one does not. */
constexpr std::string_view kProbe = "probe";
/** @brief The subject an ODD epoch grants instead — never the prober. */
constexpr std::string_view kOther = "other";
/** @brief The bearer's own (never-rewritten) grant, which is what makes it BEARING. */
constexpr std::string_view kKeeper = "keeper";

/**
 * @brief Intermediate vertices between the rewritten ancestor and the bearer, each loaded
 *        with @ref kFillerAces inheritable ACEs.
 *
 * The asymmetry is the whole instrument. The lost update is a mark landing between the
 * rebuilder's generation recheck and its clear, an interval dominated by
 * `eff_aces = std::move(merged)` — destroying a vector of ACEs that each own a subject
 * allocation. If the invalidating WRITE costs as much as the rebuild it invalidates, the
 * next mark always arrives long after that interval has closed and the window is simply
 * never sampled. Loading the INTERMEDIATE ancestors (which the writer never touches) makes
 * the rebuild and its publish expensive while the top ancestor's rewrite stays a
 * single-ACE write, so consecutive marks straddle the window instead of overshooting it.
 */
constexpr std::size_t kChainDepth = 4;
/** @brief Inheritable ACEs each intermediate carries — see @ref kChainDepth. */
constexpr std::size_t kFillerAces = 48;
/**
 * @brief Distinct inter-epoch delays the writer cycles through, and how far the sweep runs
 *        (in units of one measured invalidate-plus-rebuild cycle).
 *
 * The writer does NOT delay by a constant. The mark that gets lost has to land inside a
 * publish sitting at a roughly fixed offset after the PREVIOUS mark, so a constant period
 * either always misses that offset or always hits it — and which one is an accident of the
 * host, not a property of the protocol. Sweeping walks the next mark's phase across the
 * whole rebuild, so one run covers the window instead of sampling a single point of it. The
 * long end of the sweep doubles as the quiescent bracket (`committed == pending`) in which
 * the prober's verdict becomes a claim.
 */
constexpr std::uint32_t kSweepSteps = 96;
/** @brief Cycles the sweep spans — see @ref kSweepSteps. */
constexpr std::uint32_t kSweepCycles = 2;
/** @brief Sequential invalidate-then-evaluate cycles timed to size the sweep. */
constexpr int kCalibrationCycles = 32;
/** @brief Spin iterations timed to convert the measured cycle into spin units. */
constexpr std::uint32_t kSpinCalibration = 200000;
/** @brief Ceiling on the writer's epochs — @ref kWriterBudgetMs is the real limit. */
constexpr std::uint32_t kEpochsMax = 4000;
/** @brief Floor, so even a heavily instrumented build still yields a live instrument. */
constexpr std::uint32_t kEpochsMin = 64;
/**
 * @brief Wall-clock milliseconds the writer is allowed.
 *
 * The epoch count is DERIVED from this and the measured cycle rather than fixed, for the
 * same reason the delay is: a TSan build evaluates two orders of magnitude slower than a
 * release one, so a fixed count is either a blink or a wedge. Under TSan the first draft of
 * this test completed FIVE probes — every guard vacuous — because the prober could not
 * finish a walk between two marks. Deriving both knobs from one timed cycle keeps the same
 * source honest on both builds.
 */
constexpr std::int64_t kWriterBudgetMs = 2000;

/** @brief True iff epoch @p e's ancestor ACL grants @ref kProbe the READ right. */
constexpr bool grants_probe(std::uint32_t e) { return (e % 2) == 0; }

/** @brief Busy-spin for @p n units — a sub-microsecond delay a sleep cannot express. */
void spin(std::uint32_t n) {
    volatile std::uint32_t sink = 0;
    for (std::uint32_t i = 0; i < n; ++i) sink = sink + i;
}

/** @brief One ALLOW ACE granting @p subject the @p right, inheritable per @p inherit. */
ace_t grant(std::string_view subject, acl_right_t right, bool inherit) {
    return ace_t{.type = ace_type_t::ALLOW,
                 .flags = inherit ? tr::graph::kAceInherit : std::uint8_t{0},
                 .subject = as_bytes(subject),
                 .access_mask = static_cast<std::uint32_t>(right),
                 .expires_ns = 0};
}

/** @brief `/anc` followed by @p depth intermediate segments (`/anc/m0/m1/…`). */
std::string chain_path(std::size_t depth) {
    std::string p = "/anc";
    for (std::size_t i = 0; i < depth; ++i) {
        p += "/m";
        p += static_cast<char>('0' + static_cast<char>(i));
    }
    return p;
}

/** @brief @ref kFillerAces inheritable ACEs naming subjects no caller ever presents. */
std::vector<std::byte> filler_acl(std::size_t level) {
    std::vector<ace_t> aces;
    aces.reserve(kFillerAces);
    for (std::size_t i = 0; i < kFillerAces; ++i) {
        char subject[24] = {};
        std::snprintf(subject, sizeof(subject), "filler-%zu-%02zu", level, i);
        aces.push_back(grant(subject, acl_right_t::READ, true));
    }
    return tr::graph::encode_acl(aces);
}

/**
 * @brief The top ancestor's `:acl` for @p epoch: ONE inheritable READ grant, naming
 *        @ref kProbe on an EVEN epoch and @ref kOther on an ODD one.
 */
std::vector<std::byte> ancestor_acl(std::uint32_t epoch) {
    const std::vector<ace_t> one = {
        grant(grants_probe(epoch) ? kProbe : kOther, acl_right_t::READ, true)};
    return tr::graph::encode_acl(one);
}

/** @brief Nanoseconds since @p start on the steady clock. */
std::int64_t ns_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                start)
        .count();
}

/** @brief What one invalidate-plus-rebuild-plus-evaluate cycle costs on THIS build. */
struct calibration_t {
    std::int64_t cycle_ns = 1;     /**< @brief Wall-clock of one cycle. */
    std::uint32_t cycle_spins = 1; /**< @brief @ref spin units that take about as long. */
};

/**
 * @brief Time one cycle single-threaded, before the racer starts.
 *
 * Times the cycle, times @ref spin, and returns both, so every delay and count this test
 * asks for is expressed in CYCLES instead of in numbers that only mean something on one
 * host and one instrumentation level.
 */
calibration_t calibrate(graph_t& g, vertex_handle_t bearer, std::span<const std::byte> acl_a,
                        std::span<const std::byte> acl_b) {
    const auto cycle_start = std::chrono::steady_clock::now();
    for (int i = 0; i < kCalibrationCycles; ++i) {
        (void)g.write(path_t("/anc:acl"), make_value((i % 2) == 0 ? acl_a : acl_b));
        (void)g.read(bearer, kProbe);
    }
    const std::int64_t cycle_ns =
        std::max<std::int64_t>(1, ns_since(cycle_start) / kCalibrationCycles);
    const auto spin_start = std::chrono::steady_clock::now();
    spin(kSpinCalibration);
    const std::int64_t spin_ns = std::max<std::int64_t>(1, ns_since(spin_start));
    return calibration_t{.cycle_ns = cycle_ns,
                         .cycle_spins = static_cast<std::uint32_t>(
                             std::max<std::int64_t>(1, (cycle_ns * kSpinCalibration) / spin_ns))};
}

/**
 * @brief One thread rewrites an ancestor's `:acl`; another evaluates a descendant's gate.
 *
 * Fails on either signature of #880: a stale verdict observed inside a quiescent bracket,
 * or — the severe form — a stale verdict that SURVIVES the writer, which is what a lost
 * invalidation produces (nothing later re-raises the flag the rebuilder cleared).
 */
void test_ancestor_rewrite_vs_descendant_eval() {
    std::printf("ACL cache vs lock-free subtree invalidation (#880, ADR-0078):\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    (void)g.register_vertex(path_t("/anc"), role_t::STORED_VALUE);
    for (std::size_t d = 1; d <= kChainDepth; ++d)
        (void)g.register_vertex(path_t(chain_path(d)), role_t::STORED_VALUE);
    const std::string bearer_path = chain_path(kChainDepth) + "/bearer";
    const vertex_handle_t bearer = g.register_vertex(path_t(bearer_path), role_t::STORED_VALUE);

    // A value so an ALLOWED read is distinguishable from a denied one by `has_value` alone.
    std::vector<std::byte> datum;
    const std::byte payload[1] = {std::byte{7}};
    tr::wire::emit_tlv(datum, tr::wire::type_t::VALUE, tr::wire::opt_t{}, payload);
    (void)g.write(bearer, make_value(datum));

    // The intermediates' static load — never rewritten, so it costs the WRITER nothing and
    // the rebuilder everything.
    for (std::size_t d = 1; d <= kChainDepth; ++d)
        (void)g.write(path_t(chain_path(d) + ":acl"), make_value(filler_acl(d)));

    // The bearer's OWN, never-rewritten ACL. It is load-bearing for the instrument: with it,
    // the bearing vertex IS the descendant, so the only thing that invalidates the merge
    // under test is the lock-free mark fanned out from the top ancestor's write.
    const std::vector<ace_t> own = {grant(kKeeper, acl_right_t::WRITE, false)};
    (void)g.write(path_t(bearer_path + ":acl"), make_value(tr::graph::encode_acl(own)));

    const std::vector<std::byte> even_acl = ancestor_acl(0);
    const std::vector<std::byte> odd_acl = ancestor_acl(1);

    // Both knobs derived from one timed cycle — see kWriterBudgetMs.
    const calibration_t cal = calibrate(g, bearer, even_acl, odd_acl);
    const std::uint32_t step_spins = (cal.cycle_spins * kSweepCycles) / kSweepSteps + 1;
    // An epoch costs its write plus the sweep's mean delay — about (1 + kSweepCycles/2) cycles.
    const std::int64_t epoch_ns = cal.cycle_ns * (2 + kSweepCycles) / 2;
    const std::uint32_t epochs = std::clamp(static_cast<std::uint32_t>(std::min<std::int64_t>(
                                                kEpochsMax, kWriterBudgetMs * 1000000 / epoch_ns)),
                                            kEpochsMin, kEpochsMax);

    std::atomic<std::uint32_t> pending{0};    // epoch whose write may be IN FLIGHT
    std::atomic<std::uint32_t> committed{0};  // epoch whose write has fully landed
    std::thread writer([&] {
        for (std::uint32_t e = 1; e <= epochs; ++e) {
            pending.store(e, std::memory_order_release);
            (void)g.write(path_t("/anc:acl"), make_value(grants_probe(e) ? even_acl : odd_acl));
            committed.store(e, std::memory_order_release);
            spin((e % kSweepSteps) * step_spins);
        }
    });

    std::uint64_t probes = 0;  // evaluations run
    std::uint64_t claims = 0;  // evaluations inside a quiescent bracket
    std::uint64_t granted_claims = 0;
    std::uint64_t denied_claims = 0;
    std::uint64_t stale_hits = 0;
    std::uint32_t first_stale_epoch = 0;
    while (committed.load(std::memory_order_acquire) < epochs) {
        const std::uint32_t before = committed.load(std::memory_order_acquire);
        if (before == 0) continue;  // no epoch has landed yet
        const bool allowed = g.read(bearer, kProbe).has_value();
        const std::uint32_t after = pending.load(std::memory_order_acquire);
        ++probes;
        // Unbracketed: a write was in flight across the evaluation, so BOTH verdicts are
        // legal and nothing is claimed.
        if (before != after) continue;
        ++claims;
        (allowed ? granted_claims : denied_claims)++;
        if (allowed != grants_probe(before)) {
            if (stale_hits == 0) first_stale_epoch = before;
            ++stale_hits;
        }
    }
    writer.join();

    // Quiescent: nothing is in flight, so the merge must be the last epoch's, full stop.
    const std::uint32_t final_epoch = committed.load(std::memory_order_acquire);
    const bool final_allowed = g.read(bearer, kProbe).has_value();

    std::printf(
        "  %u epochs @ %lld ns/cycle: %llu probes, %llu bracketed claims "
        "(%llu grant / %llu deny), %llu stale\n",
        epochs, static_cast<long long>(cal.cycle_ns), static_cast<unsigned long long>(probes),
        static_cast<unsigned long long>(claims), static_cast<unsigned long long>(granted_claims),
        static_cast<unsigned long long>(denied_claims),
        static_cast<unsigned long long>(stale_hits));
    if (stale_hits != 0)
        std::printf("  first stale verdict at epoch %u (expected %s, got %s)\n", first_stale_epoch,
                    grants_probe(first_stale_epoch) ? "ALLOW" : "DENY",
                    grants_probe(first_stale_epoch) ? "DENY" : "ALLOW");

    // Instrument liveness first: a run that never bracketed a claim, or never resolved BOTH
    // policies, measured nothing — the vacuous-guard failure mode, reported as such.
    check(claims > 0, "the prober bracketed at least one evaluation (instrument is live)");
    check(granted_claims > 0 && denied_claims > 0,
          "both epoch parities were observed (the probe resolves the ancestor's grant)");
    check(stale_hits == 0,
          "no evaluation saw a pre-write ancestor merge inside a quiescent bracket");
    check(final_allowed == grants_probe(final_epoch),
          "the merge is the final epoch's once the writer is gone (no cache pinned stale)");
}

}  // namespace

int main() {
    test_ancestor_rewrite_vs_descendant_eval();
    std::printf("%s\n", g_failures == 0 ? "OK" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
