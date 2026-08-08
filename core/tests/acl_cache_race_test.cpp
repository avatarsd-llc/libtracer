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
 *
 * A third decides whether the instrument is LIVE, and it belongs to B, not to A: the run
 * ends when B has banked @ref kClaimsPerParity brackets of each parity (and A has raced at
 * least @ref kRaceEpochsMin epochs), never when a clock runs out — @ref kQuietEpochStride
 * has A hold still until B can bank one. The wall clock survives only as
 * @ref kRunCeilingMs, whose expiry is a FAILURE.
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
 * long end of the sweep also yields quiescent brackets (`committed == pending`) in which the
 * prober's verdict becomes a claim — plentifully on an unloaded build, and not at all on a
 * loaded one, which is why brackets are not left to it (see @ref kQuietEpochStride).
 */
constexpr std::uint32_t kSweepSteps = 96;
/** @brief Cycles the sweep spans — see @ref kSweepSteps. */
constexpr std::uint32_t kSweepCycles = 2;
/** @brief Sequential invalidate-then-evaluate cycles timed to size the sweep. */
constexpr int kCalibrationCycles = 32;
/** @brief Spin iterations timed to convert the measured cycle into spin units. */
constexpr std::uint32_t kSpinCalibration = 200000;
/**
 * @brief Bracketed claims of EACH parity the prober must bank before the writer stops.
 *
 * This is the run's TERMINATION condition, not a threshold checked afterwards, and that is
 * the whole point (#1036). A wall-clock writer budget adapts the WRITER to the host and
 * leaves the PROBER to luck: on the loaded runner that produced #1036 the writer got its
 * full 4000 epochs while the prober ran 35 probes and bracketed NONE of them, because the
 * writer's longest idle (the sweep's long end, ~2 cycles) was orders of magnitude shorter
 * than one contended walk. Counting writer epochs cannot fix that — the thing being counted
 * is prober brackets. So the prober counts them, and the writer runs until it has enough:
 * a fast host finishes early, a slow one takes longer, and neither can finish vacuous.
 */
constexpr std::uint64_t kClaimsPerParity = 8;
/**
 * @brief Epochs the writer races through between two RENDEZVOUS windows.
 *
 * A bracket cannot be waited for from the prober's side alone: it exists only when the
 * writer happens to be idle across a whole walk. So the writer manufactures one every
 * @ref kQuietEpochStride epochs — it stops writing, holds the graph at that epoch, and does
 * not resume until the prober has completed one evaluation entirely inside the window. What
 * the claim asserts is unchanged (a verdict observed while `committed == pending == e` MUST
 * be epoch e's); only its EXISTENCE stops being an accident of the host. Natural brackets
 * off the sweep's long end still count and, on an unloaded build, still dominate.
 *
 * ODD on purpose: the parked epoch's parity then alternates window to window, so the two
 * halves of @ref kClaimsPerParity both fill even when not one natural bracket occurs.
 */
constexpr std::uint32_t kQuietEpochStride = 127;
/**
 * @brief Racing epochs the writer must complete regardless of how fast the claims arrive.
 *
 * The claims quota proves the instrument is LIVE; it does not buy exposure. On an unloaded
 * release build the prober banks its quota within a few dozen epochs, which would leave the
 * detector a fraction of the invalidations that catch #880 (ADR-0078 measured ~4–5 k stale
 * verdicts per run against the predecessor protocol). Racing epochs, not seconds, are what
 * the exposure scales with, so the floor is denominated in epochs — and set at the epoch
 * CEILING the wall-clock budget it replaces used to reach, so no run races less than before.
 */
constexpr std::uint32_t kRaceEpochsMin = 4096;
/**
 * @brief Wall-clock ceiling on the whole race, in milliseconds.
 *
 * A CEILING, never a budget: reaching it means the prober could not bank its quota (or the
 * writer could not reach @ref kRaceEpochsMin) and the run is reported as a FAILURE. It is
 * not a stop condition the assertions then accommodate — that is precisely how a flaky case
 * becomes a vacuous one, green on exactly the loaded host where the interleaving never
 * happened. So the margin has to be wide enough that only a wedge reaches it: a TSan build
 * needs ~0.5 s unloaded, ~1.2 s pinned to two cores, and ~13.5 s pinned to two cores against
 * six competing spinners — four times the oversubscription of the runner that filed #1036.
 */
constexpr std::int64_t kRunCeilingMs = 60000;

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
 * Times the cycle, times @ref spin, and returns both, so the writer's swept delay is
 * expressed in CYCLES instead of in a spin count that only means something on one host and
 * one instrumentation level. A TSan build evaluates two orders of magnitude slower than a
 * release one; the first draft of this test hard-coded the delay and completed FIVE probes
 * under TSan, every guard vacuous.
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

    // The sweep's step is derived from one timed cycle; the RUN LENGTH is derived from the
    // prober (kClaimsPerParity) and the exposure floor (kRaceEpochsMin), never from a clock.
    const calibration_t cal = calibrate(g, bearer, even_acl, odd_acl);
    const std::uint32_t step_spins = (cal.cycle_spins * kSweepCycles) / kSweepSteps + 1;

    std::atomic<std::uint32_t> pending{0};      // epoch whose write may be IN FLIGHT
    std::atomic<std::uint32_t> committed{0};    // epoch whose write has fully landed
    std::atomic<std::uint32_t> quiet_epoch{0};  // epoch the writer is HELD at (0 ⇒ racing)
    std::atomic<std::uint32_t> quiet_ack{0};    // newest held epoch the prober bracketed
    std::atomic<bool> prober_done{false};       // quota banked, or the ceiling hit
    std::thread writer([&] {
        for (std::uint32_t e = 1; !prober_done.load(std::memory_order_acquire); ++e) {
            pending.store(e, std::memory_order_release);
            (void)g.write(path_t("/anc:acl"), make_value(grants_probe(e) ? even_acl : odd_acl));
            committed.store(e, std::memory_order_release);
            if ((e % kQuietEpochStride) == 0) {
                // Rendezvous (see kQuietEpochStride): hold epoch e until the prober has run a
                // whole evaluation inside the window. Every evaluation that STARTS here is
                // bracketed by construction, since pending cannot advance while we wait.
                quiet_epoch.store(e, std::memory_order_release);
                while (quiet_ack.load(std::memory_order_acquire) < e &&
                       !prober_done.load(std::memory_order_acquire))
                    std::this_thread::yield();
                quiet_epoch.store(0, std::memory_order_release);
            }
            spin((e % kSweepSteps) * step_spins);
        }
    });

    std::uint64_t probes = 0;  // evaluations run
    std::uint64_t claims = 0;  // evaluations inside a quiescent bracket
    std::uint64_t granted_claims = 0;
    std::uint64_t denied_claims = 0;
    std::uint64_t stale_hits = 0;
    std::uint32_t first_stale_epoch = 0;
    bool ceiling_hit = false;
    const auto race_start = std::chrono::steady_clock::now();
    while (granted_claims < kClaimsPerParity || denied_claims < kClaimsPerParity ||
           committed.load(std::memory_order_acquire) < kRaceEpochsMin) {
        if (ns_since(race_start) > kRunCeilingMs * 1000000) {
            ceiling_hit = true;
            break;
        }
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
        // This evaluation ran entirely inside a held window, so release its writer.
        if (quiet_epoch.load(std::memory_order_acquire) == before)
            quiet_ack.store(before, std::memory_order_release);
    }
    const std::int64_t race_ms = ns_since(race_start) / 1000000;
    prober_done.store(true, std::memory_order_release);
    writer.join();

    // Quiescent: nothing is in flight, so the merge must be the last epoch's, full stop.
    const std::uint32_t final_epoch = committed.load(std::memory_order_acquire);
    const bool final_allowed = g.read(bearer, kProbe).has_value();

    std::printf(
        "  %u epochs in %lld ms @ %lld ns/cycle: %llu probes, %llu bracketed claims "
        "(%llu grant / %llu deny), %llu stale\n",
        final_epoch, static_cast<long long>(race_ms), static_cast<long long>(cal.cycle_ns),
        static_cast<unsigned long long>(probes), static_cast<unsigned long long>(claims),
        static_cast<unsigned long long>(granted_claims),
        static_cast<unsigned long long>(denied_claims),
        static_cast<unsigned long long>(stale_hits));
    if (stale_hits != 0)
        std::printf("  first stale verdict at epoch %u (expected %s, got %s)\n", first_stale_epoch,
                    grants_probe(first_stale_epoch) ? "ALLOW" : "DENY",
                    grants_probe(first_stale_epoch) ? "DENY" : "ALLOW");

    // Instrument liveness first: a run that stopped on the clock instead of on the prober's
    // quota measured whatever the host let it — the vacuous-guard failure mode, reported as
    // such rather than accommodated.
    char quota_msg[128];
    std::snprintf(quota_msg, sizeof(quota_msg),
                  "the prober banked %llu bracketed claims of EACH parity (instrument is live)",
                  static_cast<unsigned long long>(kClaimsPerParity));
    check(!ceiling_hit, "the race ended on the prober's quota, not on the wall-clock ceiling");
    check(granted_claims >= kClaimsPerParity && denied_claims >= kClaimsPerParity, quota_msg);
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
