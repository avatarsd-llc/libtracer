/**
 * @file
 * @brief The QSBR domain (#1376): the cross-thread grace period behind
 *        @ref tr::graph::reclaim_qsbr_t, ADR-0080's third reclamation policy.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Quiescent-state-based reclamation in the URCU sense, applied to two things: the `{fn, ctx}`
 * leg of a retired subscription under `reclaim_qsbr`, and, in every build, what the transport
 * plane parks on the graph (a removed link, a retired seam). The read-side announcement it
 * needs is ALREADY COMPUTED by the seam PR #1377 shipped: `graph.cpp`'s dispatch bracket keeps
 * a per-thread depth whose transition to 0 is the proof that this thread holds no
 * `edge_view_t` snapshot. That is `reclaim_local`'s entire grace-point argument, and this
 * domain reuses it verbatim, adding one announcement at the OUTERMOST bracket and nothing at
 * all per edge.
 *
 * The shape is modelled on `%edge_pin.hpp`, not on `%lkv_slot.hpp`: a bounded `constinit`
 * registry in `.bss`, a claimable per-thread cell, and no exit sweep to register.
 *
 * **Two users, one domain.** `%graph.cpp`'s dispatch bracket reaches it only through an
 * `if constexpr` branch a non-QSBR build discards, so under the other two policies `fan_out`
 * emits nothing from here. The transport plane uses it in EVERY build: `fwd_router_t` brackets
 * each inbound frame and each origination with `%enter` / `%leave`, and the graph frees a
 * parked link or value seam only once every bracket open when it was parked has closed
 * (`%advance`, `%horizon`). Both share one per-thread nesting depth, so a
 * dispatch nested in a frame neither re-announces nor goes quiescent early.
 *
 * @section qsbr_why_global Why the retired list is GLOBAL and the participant table is not
 *
 * ADR-0080 §"#897 maps onto the same seam" says each thread self-drains *its own* retired
 * list. That is the right shape for #897's displaced LKV nodes, which are pushed by a WRITER
 * thread that goes on writing — and it is discharged verbatim by calling
 * `%detail_hp::retire_and_flush(nullptr)` at the quiescent point, with no change to
 * `%lkv_slot.hpp` at all.
 *
 * It is the WRONG shape for a retired subscription pair, and the difference is the case this
 * policy exists for. Thread B unsubscribes while thread A is mid-dispatch; B must defer,
 * because A's live snapshot still names the pair. If B parked that pair in B's OWN
 * thread-local list, nothing would ever drain it: B is not dispatching, so B has no quiescent
 * point coming, and "unsubscribe once, then never touch the graph again" is the DOMINANT
 * teardown shape. The hook would never run. So the retired pairs live in one bounded
 * `.bss`-resident table that ANY participant drains at its own quiescent point — and in the
 * scenario above it is A, the thread whose quiescence completed the grace period, that runs
 * the hook.
 *
 * That has a consequence the API must state rather than hide: under this policy the release
 * hook may run **on a thread other than the caller of `unsubscribe()`**. Both shipped
 * policies promise the caller's own thread; a grace period that spans threads structurally
 * cannot, because the only alternatives are to block the caller (ADR-0080 §Decision 4 rejects
 * waiting outright) or to leak. See @ref tr::graph::reclaim_qsbr_t.
 *
 * @section qsbr_protocol The protocol
 *
 * Classic QSBR. One global epoch (`%control_t::epoch`), one per-thread state word
 * (`%cell_t::state`):
 *
 * - **online** (dispatch depth 0 → 1): `state = epoch | kOnlineBit`. One relaxed load of a
 *   read-mostly shared line plus one store to this thread's own cache-line-isolated cell. No
 *   atomic RMW, and once per fan-out rather than once per edge.
 * - **offline** (depth 1 → 0): `state = 0`. `0` means "holds nothing", which is what makes an
 *   idle thread parked in `epoll` non-blocking — the classic QSBR liveness hole, closed
 *   structurally instead of by a `thread_offline()` verb the embedder must remember.
 * - **retire**: bump the epoch (one RMW, control plane only) and scan. A pair retired at
 *   epoch `E` is free once every participant is either offline or online at an epoch `> E`.
 *
 * @section qsbr_wrap The 31-bit epoch, and why a wrap only delays a free
 *
 * The epoch and the online flag share one 32-bit word, so the announce and the exit are plain
 * 32-bit stores on every target, rv32 included: a 64-bit atomic there is a `libatomic` call
 * that masks interrupts, three of them per bracket. The epoch therefore wraps, after 2^31
 * advances, and "after" is decided by serial-number arithmetic (`%epoch_after`): `a` is after
 * `b` when `(a - b) mod 2^31` is in `[1, 2^30)`. That reads correctly as long as the epochs
 * compared are less than 2^30 apart.
 *
 * The only epochs that can drift that far apart are an open bracket's and the counter's: the
 * counter advances once per retirement (`graph_t::retire`, `park_release`, an unsubscribe under
 * `reclaim_qsbr`), never per frame, so the bound is a bracket held open across 2^30 removals.
 * The writer keeps it from being reached: `%advance` does not bump the epoch while any open
 * bracket is already `%kStallLag` (2^29) behind it, so the counter stops at most 2^29 plus
 * the concurrent writers ahead of the oldest open bracket. A stalled counter only DELAYS frees
 * — a pair retired at the stalled epoch waits for every bracket, the old one included — and
 * the first advance after that bracket closes moves on. So a bracket held open however long
 * never makes a pair look free while it is open. A thread counted in `overflow_online` has no
 * epoch at all, and already blocks every free while it is online.
 *
 * The scan is `O(kQsbrParticipants)` and lives on the RECLAIM path only. That is the precise
 * difference from the shape #635 rejected, which put a hazard scan on the READ path.
 *
 * @section qsbr_ordering Why the announce is followed by a `seq_cst` fence
 *
 * The hazard is Dekker-shaped and it is the only ordering argument this file makes. The
 * reader R announces itself and then reads what the writer may free (the edge array, a child
 * link); the writer W unpublishes it (`clear_edge`, an unrouted link, already done before it
 * gets here) and then reads the announcements. Neither side may miss the other.
 * Store-buffering permits exactly that outcome unless BOTH sides fence, and a `seq_cst` STORE
 * is not a fence: C++ does not order it before a later load of another location that is not
 * itself `seq_cst` (an ACQUIRE load may compile to `ldapr` on Armv8.3 and pass it). So the
 * argument is fence to fence ([atomics.fences], with the `seq_cst` total order S of
 * [atomics.order]):
 *
 * - R: `L_e`, a relaxed load of the epoch; `S_R`, a release store of its cell (online at
 *   `L_e`'s value), or a relaxed `overflow_online` increment; `F_R`, a `seq_cst` fence; then
 *   `L_R`, its reads.
 * - W: `S_W`, the unpublish; the epoch bump `B` (`seq_cst` RMW, `%advance`) that closes epoch
 *   `e`; `L_c`, an acquire load of the counter; `F_W`, the `seq_cst` fence `%horizon` pays;
 *   then `L_W`, its acquire loads of `overflow_online` and the cells. It frees only when every
 *   cell `L_W` reads is offline or online at an epoch after `e`, and `overflow_online` reads 0.
 *
 * `F_R` and `F_W` are both in S, so one precedes the other:
 *
 * 1. `F_W` before `F_R`. `S_W` is sequenced before `F_W` and `L_R` after `F_R`, so `L_R` reads
 *    `S_W` or later: R does not see what W unpublished.
 * 2. `F_R` before `F_W`. `S_R` is sequenced before `F_R` and `L_W` after `F_W`, so `L_W` reads
 *    `S_R` or later. Every later store to the cell is a release made after R's reads: R's
 *    offline store, R's own next announce, or, once R's thread has exited, the first announce
 *    of the thread that claimed the cell next (R's thread-exit release stores the cell,
 *    releases `claimed` and leaves R's record naming no cell, so R never stores to it again
 *    and a bracket R opens later counts into `overflow_online`; the new owner's `acq_rel`
 *    claim reads the release, so R's reads happen before its announce). `L_W` synchronizes with
 * whichever it reads, so R's reads happen before the free. The counter has only RMWs, so a later
 * value `L_W` reads is in the release sequence of R's `seq_cst` decrement, and a 0 read after `S_R`
 * means that decrement came first. If `L_W` reads `S_R` itself, R is online at `L_e`'s value. At
 * `e` or earlier, W does not free. After `e`, `L_e` read `B` or a later bump, which continues `B`'s
 * release sequence; `F_R` is an acquire fence sequenced after `L_e`, so `B` synchronizes with
 * `F_R`, `S_W` happens before `L_R`, and `L_R` reads `S_W` or later.
 *
 * Neither way is a reader inside its bracket while what it reads is freed. `%retire`, the
 * drainer and `graph_t::collect` reach `F_W` through `%horizon`, below every early-out on the
 * dispatch path; a stalled `%advance` is covered there (@ref qsbr_horizon).
 *
 * On x86-64 (GCC 13) the entry is a plain `mov` and one locked instruction (`lock or` to the
 * stack) where the `seq_cst` store was one `xchg`, so it still pays one full barrier. On rv32
 * it is `fence rw,w`, the store and `fence rw,rw`, as the `seq_cst` store was.
 *
 * Going OFFLINE needs only `release`: it must publish that this thread's reads are done, and
 * a release store is exactly "nothing above me moves below me".
 *
 * @section qsbr_flavours Three fence flavours, one chosen per build
 *
 * The fence follows the build, never the policy: who may run a hook, and when, is the same in
 * all three (ADR-0080 Amendment 1 §2, #2126). `%fence_flavour` picks one at compile time:
 *
 * - **COMPILER** where @ref tr::graph::kSingleCore holds. Entry: a relaxed store, then a compiler
 *   barrier. Exit: a compiler barrier, then a relaxed store. Reclaim side: a compiler barrier
 *   and relaxed loads. No hardware fence anywhere on the bracket (@ref qsbr_single_hart).
 * - **FENCED**, the default everywhere else: the release store and `seq_cst` fence above.
 * - **MEMBARRIER** where `kQsbrAsymmetricFence` meets a Linux target (@ref qsbr_asym). A refused
 *   registration falls back to FENCED at run time, the one fact no build can know.
 *
 * @section qsbr_single_hart The single-hart flavour
 *
 * One hart executes one instruction stream, and a context switch is a precise trap, so every
 * access a descheduled thread made is performed before the next thread's first instruction. The
 * hart's own accesses are performed in its program order as that hart observes them, so the only
 * reordering left is the compiler's, and a compiler barrier (`atomic_signal_fence`) removes it:
 * it keeps the announce above the bracket's reads and those reads above the offline store, and on
 * the reclaim side it keeps the unpublish and the epoch bump above the scan. The Dekker case
 * split of @ref qsbr_ordering then holds in hart order, with "precedes in S" read as "runs
 * earlier on the hart". The loads and stores are `relaxed` (`%kStoreOrder`, `%kLoadOrder`),
 * because the hart already gives the order a release or acquire would ask the hardware for, and
 * on rv32 each would cost a `fence`.
 *
 * This is a hardware and compiler argument, not one inside the C++ abstract machine, the same
 * argument as Linux's `smp_mb()` under `CONFIG_SMP=n`. It holds only while nothing but that hart
 * writes these words: the ESP32-C6's LP core does not run libtracer. On a second core it is a
 * use-after-free, which is why @ref tr::graph::kSingleCore defaults to false.
 *
 * @section qsbr_asym The asymmetric fence: a plain entry store, `membarrier` on the reclaim side
 *
 * Where `%kMembarrier` holds (a Linux host, `kQsbrAsymmetricFence`, and a registration the
 * kernel accepted), the reader's half of the barrier moves to the reclaim side, as liburcu's
 * "memb" flavour does. The reader R and the reclaimer W, with every access each side makes:
 *
 * - R: `L_e`, an ACQUIRE load of the epoch; `S_R`, a RELEASE store of its cell (online at
 *   `L_e`'s value), a plain `mov` on x86; a compiler barrier; then `L_R`, its reads of what W
 *   may free (the link, the edge array).
 * - W: `S_W`, the unpublish; the epoch bump (`seq_cst` RMW, `advance`) that closes epoch
 *   `e`; later `L_c` and `F1`, a `seq_cst` fence, `membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED)`,
 * `F2`, another `seq_cst` fence; then `L_W`, its loads of the cells. It frees only when every cell
 * `L_W` reads is offline or online at an epoch after `e`.
 *
 * The kernel's guarantee is that every thread of the process runs a full barrier `F_R` at some
 * point of its own program order between `F1` and `F2` (a thread not running then does so
 * when it next runs, at the context switch). So `F1 < F_R < F2` in the `seq_cst` order, and:
 *
 * 1. `F_R` falls before `L_R` in R's program order. `S_W` is sequenced before `F1`, and
 *    `F1 < F_R`, so `L_R` reads `S_W` or later: R does not see what W unpublished.
 * 2. `F_R` falls after `L_R`. Then it falls after `S_R` too, because the compiler barrier keeps
 *    `S_R` before `L_R` in program order, and `F_R < F2` with `F2` before `L_W`, so `L_W` reads
 *    `S_R` or later. Every later store to the cell is a release made after `L_R`, as in
 *    @ref qsbr_ordering case 2 (R's offline store, R's next announce, or the first announce of
 *    the cell's next owner), and `L_W`'s acquire loads synchronize with it, so `L_R` happens
 *    before the free. A relaxed `S_R` would not do: a later announce read by `L_W` would then
 *    order nothing. If it reads `S_R` itself, R is online at `L_e`'s value. If that is `e` or
 * earlier, W does not free. If it is after `e`, `L_e` read the bump that closed `e` or a later one;
 * the bump is a release, so `S_W` happens before `L_R`, and `L_R` cannot see what W unpublished.
 *
 * Neither way is a reader inside its bracket while the thing it reads is freed. `L_e` being an
 * acquire is what the second case needs: a relaxed load would let `L_R` be satisfied before it
 * on a weakly ordered CPU, so R could announce an epoch after `e` with a stale `L_R`. On x86 an
 * acquire load is a plain load; the full barrier of the fenced entry (@ref qsbr_ordering, a
 * `lock or` on x86-64) is what this saves.
 *
 * A scan's FALSE needs no `membarrier` (it only defers), so W scans once after `F1` alone,
 * and pays the `membarrier` only when that scan finds something to free, then scans again
 * after `F2` (`%horizon`). A `membarrier` call that fails makes the scan answer nothing past: frees
 * are deferred, never early. Readers use the plain store only after the registration succeeded
 * (`control_t::asym` is set inside `asym_ready`, which every fencing scan runs first), so a reader
 * that took the plain store always meets a reclaimer that calls it.
 *
 * @section qsbr_horizon One horizon per reclaim pass
 *
 * A reclaimer with several entries to free pays the fence once, not once per entry. `%horizon`
 * reads the counter (`L_c`), fences (`F_W`, or `F1`/`membarrier`/`F2`), scans the cells once and
 * returns a plain value, `%horizon_t`: the arc of epochs that the counter value `c` and every
 * online cell are after. Each entry is then one compare against it. Every reclaimer has one:
 * `%retire` asks about its own epoch, `%drain_ripe` about the shared table, and
 * `graph_t::collect` about both of its parks, outside the map lock.
 *
 * Why `L_c` makes the value good for an entry found AFTER the scan, however it got there: an
 * entry the horizon covers has an epoch `e` before `c`. Its bump `B` (the RMW that closed `e`)
 * then precedes in the counter's modification order the value `L_c` read, which continues
 * `B`'s release sequence, so `S_W` happens before `L_c` and so before `F_W`, which is all
 * either case above uses. Where `advance` stalled instead of bumping, its fence `F_X` follows
 * `S_W`, and its `seq_cst` load of `e` precedes in `S` the later bump that `L_c` read, which
 * precedes `F_W` ([atomics.order]: a `seq_cst` write coherence-ordered before a load that is
 * sequenced before a `seq_cst` fence precedes that fence). So `F_X` precedes `F_W` in `S`, and
 * both cases go through with `F_X` in `S_W`'s place. The loads `L_W` need only be acquire:
 * the fence-to-fence rule makes any atomic load after `F_W` read `S_R` or later, and acquire
 * is what lets the offline store it reads synchronize with it. On rv32 that is one `fence`
 * per load where `seq_cst` paid two. An entry parked
 * after `L_c` carries `c` or later, which the horizon does not cover: it waits for the next
 * pass. So no caller needs "nothing joins while I scan", and none holds a lock across the fence.
 *
 * @section qsbr_single_flight Single flight, under the asymmetric fence
 *
 * With `%kMembarrier`, `%drain_ripe`'s passes run single flight. A drainer whose scan (after a
 * fence, before any `membarrier`) found something ripe takes `%in_flight` with a `fetch_add`; one
 * that reads non-zero there has recorded itself and returns at once, without a `membarrier` of its
 * own. Skipping only defers a free, so safety is the arguments above unchanged.
 *
 * Liveness: the holder keeps `%in_flight` through its whole pass, hooks included, and gives it
 * back with an `exchange`. If that reads more than its own 1, a drainer skipped meanwhile, and
 * the holder runs one more pass, unflagged. The skipper's `fetch_add` precedes the `exchange` in
 * `%in_flight`'s modification order (a later one would have read 0 and held the flag itself),
 * and the `exchange` reads it, so everything sequenced before the skip (the skipper's offline
 * store, the entries it retired and the counter bump that stamped them) happens before the
 * second pass's counter read and scan. That pass then frees whatever the skipper's own pass
 * could have. A drainer that arrives during the second pass finds 0, or a new holder that owes
 * it the same, and so is never left unserved. So no skip strands an entry, even with no thread
 * running after it, and a holder runs at most two passes; the second pays a `membarrier` only if
 * its first scan still finds something ripe. `%retire`'s own grace check and
 * `graph_t::collect` are not gated. The fenced build has no gate.
 */
#ifndef LIBTRACER_QSBR_HPP
#define LIBTRACER_QSBR_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#if defined(__linux__) && defined(__has_include)
#if __has_include(<linux/membarrier.h>)
#include <linux/membarrier.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__NR_membarrier)
// The `MEMBARRIER_CMD_*` names are enumerators, not macros, so the header is detected here.
#define LIBTRACER_QSBR_MEMBARRIER 1
#endif
#endif
#endif

#include "libtracer/config.hpp"

namespace tr::graph::detail_qsbr {

#if defined(LIBTRACER_QSBR_MEMBARRIER)
/** @brief This target has `membarrier(2)` with the private expedited command. */
inline constexpr bool kHaveMembarrier = true;
#else
/** @brief This target has `membarrier(2)` with the private expedited command. */
inline constexpr bool kHaveMembarrier = false;
#endif

/** @brief How the bracket and the reclaim side order the announcement (@ref qsbr_flavours). */
enum class fence_flavour_t : std::uint8_t {
    COMPILER = 0,   /**< @brief One hart: compiler barriers only (@ref qsbr_single_hart). */
    FENCED = 1,     /**< @brief A release store and a `seq_cst` fence (@ref qsbr_ordering). */
    MEMBARRIER = 2, /**< @brief A plain entry, `membarrier` on the reclaim side (@ref qsbr_asym). */
};

/**
 * @brief The flavour a build with these facts compiles: @ref fence_flavour_t::COMPILER on one
 *        hart, @ref fence_flavour_t::MEMBARRIER where the opt-in meets a target that has
 *        `membarrier`, and @ref fence_flavour_t::FENCED otherwise.
 *
 * One hart wins over the opt-in: with no fence left on the entry, `membarrier` has nothing to
 * save. A MEMBARRIER build whose registration the kernel refuses still enters FENCED at run time.
 */
[[nodiscard]] constexpr fence_flavour_t fence_flavour(bool single_core, bool asym_opt_in,
                                                      bool have_membarrier) noexcept {
    if (single_core) return fence_flavour_t::COMPILER;
    return asym_opt_in && have_membarrier ? fence_flavour_t::MEMBARRIER : fence_flavour_t::FENCED;
}

static_assert(fence_flavour(false, false, true) == fence_flavour_t::FENCED &&
                  fence_flavour(false, true, false) == fence_flavour_t::FENCED,
              "a multi-core build is FENCED unless it opts in on a target with membarrier");
static_assert(fence_flavour(false, true, true) == fence_flavour_t::MEMBARRIER,
              "the opt-in on a target with membarrier is MEMBARRIER");
static_assert(fence_flavour(true, false, false) == fence_flavour_t::COMPILER &&
                  fence_flavour(true, true, true) == fence_flavour_t::COMPILER,
              "one hart is COMPILER, whatever the opt-in says");

/** @brief This build's flavour, from @ref tr::graph::kSingleCore and the asymmetric opt-in. */
inline constexpr fence_flavour_t kFlavour =
    fence_flavour(kSingleCore, kQsbrAsymmetricFence, kHaveMembarrier);

/**
 * @brief The asymmetric fence is compiled in (@ref qsbr_asym): the build asks for it, the target
 *        has `membarrier` and is not single-hart. False on every MCU target.
 */
inline constexpr bool kMembarrier = kFlavour == fence_flavour_t::MEMBARRIER;

/** @brief The single-hart flavour is compiled in (@ref qsbr_single_hart). */
inline constexpr bool kCompilerFence = kFlavour == fence_flavour_t::COMPILER;

/**
 * @brief The order of a cell's announce and offline stores and of a scan's loads: `release` /
 *        `acquire` on more than one core, `relaxed` on one hart, where a compiler barrier gives
 *        the same order and the hardware ordering would cost a `fence` per access on rv32.
 */
inline constexpr std::memory_order kStoreOrder =
    kCompilerFence ? std::memory_order_relaxed : std::memory_order_release;

/** @copydoc kStoreOrder */
inline constexpr std::memory_order kLoadOrder =
    kCompilerFence ? std::memory_order_relaxed : std::memory_order_acquire;

/**
 * @brief The grace period's full barrier: a `seq_cst` fence, or a compiler barrier on one hart
 *        (@ref qsbr_single_hart).
 */
inline void full_fence() noexcept {
    if constexpr (kCompilerFence)
        std::atomic_signal_fence(std::memory_order_seq_cst);
    else
        std::atomic_thread_fence(std::memory_order_seq_cst);
}

/**
 * @brief The domain's padding width: @ref tr::graph::kCacheLineBytes, floored at the
 *        announcement's natural alignment so every value of the knob stays well-formed.
 *
 * Identical in derivation to `%edge_pin.hpp`'s `kCellAlign`, and for the identical reason: a
 * single-core target sets the knob to 0 and the cell collapses onto its payload, because there
 * is no second core for a scan to false-share against.
 */
inline constexpr std::size_t kCellAlign = kCacheLineBytes > alignof(std::atomic<std::uint32_t>)
                                              ? kCacheLineBytes
                                              : alignof(std::atomic<std::uint32_t>);

/**
 * @brief The flag distinguishing "online at epoch 0" from "offline".
 *
 * The state word is `epoch | kOnlineBit` while dispatching and exactly `0` while quiescent, so
 * a scan tests one word per participant. The bit is the TOP one, leaving 31 bits of epoch,
 * which wraps; see @ref qsbr_wrap.
 */
inline constexpr std::uint32_t kOnlineBit = std::uint32_t{1} << 31;

/** @brief The epoch's 31 bits within the state word and the counter. */
inline constexpr std::uint32_t kEpochMask = kOnlineBit - 1;

/** @brief Half the epoch space: two epochs this far apart or more have no order. */
inline constexpr std::uint32_t kEpochWindow = std::uint32_t{1} << 30;

/** @brief How far the counter may run ahead of an open bracket before @ref advance stalls. */
inline constexpr std::uint32_t kStallLag = std::uint32_t{1} << 29;

/**
 * @brief Is epoch @p a strictly after epoch @p b, modulo 2^31 (see @ref qsbr_wrap)?
 *
 * Two epochs @ref kEpochWindow or more apart compare as NOT after in both directions, so a
 * reader the counter has lapped is never read as past a retirement: the free waits.
 */
[[nodiscard]] constexpr bool epoch_after(std::uint32_t a, std::uint32_t b) noexcept {
    const std::uint32_t d = (a - b) & kEpochMask;
    return d != 0 && d < kEpochWindow;
}

static_assert(epoch_after(1, 0) && !epoch_after(0, 0) && !epoch_after(0, 1),
              "epoch_after is a strict order on nearby epochs");
static_assert(epoch_after(0, kEpochMask) && !epoch_after(kEpochMask, 0),
              "across the wrap, 0 follows 2^31 - 1");
static_assert(epoch_after(kEpochWindow - 1, 0) && !epoch_after(kEpochWindow, 0) &&
                  !epoch_after(0, kEpochWindow),
              "epochs half the space apart have no order, so neither reads as past the other");
static_assert(epoch_after(kOnlineBit | 5, 4) == epoch_after(5, 4),
              "the online bit and the counter's 32nd bit are outside the compare");
static_assert(kStallLag < kEpochWindow, "the stall must engage before the compare window ends");

/**
 * @brief One participant's quiescent-state announcement, cache-line isolated.
 *
 * The isolation is the whole many-core argument: the announcing store lands on a line no other
 * participant reads except during a control-plane scan, so a dispatching thread never bounces
 * a line against a sibling that is also dispatching.
 */
struct alignas(kCellAlign) cell_t {
    /** @brief `epoch | kOnlineBit` (31 bits of epoch), or 0 when quiescent — a plain 32-bit
     *         store per bracket, never an RMW, on every target. */
    std::atomic<std::uint32_t> state{0};
    std::atomic<bool> claimed{false}; /**< @brief Whether a live thread owns this index. */
};

static_assert(alignof(cell_t) == kCellAlign,
              "the cell's alignas was silently dropped — see kCellAlign's derivation");

/** @brief The lifecycle of one `%retired_slot_t`, as a lock-free four-state handoff. */
enum class slot_state_t : std::uint32_t {
    EMPTY = 0,    /**< @brief Free for a retiring thread to claim. */
    FILLING = 1,  /**< @brief Claimed; its payload is not yet published. */
    READY = 2,    /**< @brief Payload published; awaiting its grace period. */
    CLAIMING = 3, /**< @brief One drainer has won this slot and will run its hook. */
};

/**
 * @brief One deferred release obligation and the epoch it was retired at.
 *
 * `ctx` / `release` are written only under @ref slot_state_t::FILLING and read only under
 * @ref slot_state_t::CLAIMING — both states are owned exclusively by one thread — so they are
 * plain members rather than atomics, published and consumed by @ref state's release/acquire
 * transitions. @ref epoch is atomic because a drainer reads it BEFORE it claims, to skip a slot
 * that is not yet past without writing to it; `%drain_ripe` checks it again once claimed.
 */
struct retired_slot_t {
    std::atomic<slot_state_t> state{slot_state_t::EMPTY}; /**< @brief The handoff. */
    void* ctx = nullptr;                                  /**< @brief The subscriber's context. */
    void (*release)(void*) = nullptr;                     /**< @brief Its release hook. */
    std::atomic<std::uint32_t> epoch{0};                  /**< @brief The epoch it retired at. */
};

/**
 * @brief The read-mostly control words, isolated from the participant cells.
 *
 * Both members are LOADED on the dispatch path (the epoch when going online, the live count
 * when going offline) and STORED only on the control plane, so they want to sit together on
 * one line that stays Shared in every core's cache — and want to be nowhere near the retired
 * table, which a retiring thread writes.
 */
struct alignas(kCellAlign) control_t {
    /**
     * @brief The global epoch; bumped once per retirement.
     *
     * Starts at **0**, and that is a footprint decision, not an arbitrary one. A non-zero
     * initializer anywhere in @ref registry_t moves the WHOLE object out of `.bss` and into
     * `.data` — measured on rv32 at 2,560 B of flash-backed initialized RAM instead of 2,560 B
     * of zero RAM, i.e. the cost paid twice. Nothing needs it to start at 1: "offline" is the
     * state word being exactly 0, and an online participant always sets @ref kOnlineBit, so
     * online-at-epoch-0 is already distinct from offline.
     *
     * 32-bit and wrapping; only its low 31 bits are compared, by @ref epoch_after. #1697 made
     * it 64-bit because an ORDER compare would read a wrapped epoch as old; the serial compare
     * and @ref advance's stall make a wrap delay a free instead (@ref qsbr_wrap). The advance is
     * a control-plane RMW (once per retirement), not a per-dispatch one.
     */
    std::atomic<std::uint32_t> epoch{0};
    /** @brief How many @ref retired_slot_t entries are occupied — the drain path's early-out. */
    std::atomic<std::uint32_t> live{0};
    /** @brief Dispatching threads that could not claim a cell; see @ref claim. */
    std::atomic<std::uint32_t> overflow_online{0};
    /**
     * @brief Non-zero once this process's `membarrier` registration succeeded: readers may
     *        then enter with a plain store (@ref qsbr_asym). Set once, inside @ref asym_ready;
     *        never cleared. Beside the epoch because the entry loads both.
     */
    std::atomic<std::uint32_t> asym{0};
};

/**
 * @brief The domain's storage.
 *
 * `constinit` and trivially destructible, exactly as `%edge_pin.hpp`'s registry is and for the
 * same reason: it lands in `.bss` with no guard variable and is never destroyed, so a
 * thread's exit release can always reach it.
 */
struct registry_t {
    std::array<cell_t, kQsbrParticipants> cells{}; /**< @brief The announcements. */
    control_t ctl{};                               /**< @brief The read-mostly words. */
    std::array<retired_slot_t, kDeferredReleaseSlots> retired{}; /**< @brief Deferred releases. */
};

/** @brief The one domain. */
[[nodiscard]] inline registry_t& registry() {
    static constinit registry_t reg{};
    return reg;
}

/**
 * @brief Drain passes in flight, for single flight (@ref qsbr_single_flight): 0 when none is,
 *        else 1 plus the passes that skipped. Off the registry's read-mostly line, and only
 *        drainers write it. Only a build with @ref kMembarrier refers to it, so only that build
 *        emits it.
 */
inline constinit std::atomic<std::uint32_t> in_flight{0};

/** @brief A record field meaning "no cell": every index was taken, or the thread is exiting. */
inline constexpr std::uint32_t kNoCell = ~std::uint32_t{0};

/**
 * @brief This thread's one record: how many brackets it has open and which cells it claimed.
 *
 * Trivially destructible and all-zero, so the access is a bare TLS address with no guard and
 * no `__cxa_thread_atexit` registration on the bracket path, and its storage stays valid until
 * the thread is gone, through every other `thread_local`'s destructor. A cell field is 0 before
 * the thread's first claim, @ref kNoCell when there was none to claim or it has been given
 * back, and the index plus one otherwise. The cells go back at thread exit through
 * @ref cell_release_t (and `%edge_pin.hpp`'s `pin_release_t`), a separate `thread_local` that
 * only the cold claim path constructs.
 */
struct thread_record_t {
    unsigned depth;     /**< @brief Brackets open on this thread; 0 ⇒ quiescent. */
    std::uint32_t cell; /**< @brief This thread's QSBR cell (see above). */
    std::uint32_t pin;  /**< @brief This thread's edge-pin cell (`%edge_pin.hpp`). */
};
static_assert(std::is_trivially_destructible_v<thread_record_t>);

/** @brief This thread's record. */
[[nodiscard]] inline thread_record_t& rec() noexcept {
    static thread_local constinit thread_record_t r{};
    return r;
}

/**
 * @brief Gives this thread's QSBR cell back when the thread ends. Constructed only by
 *        @ref claim, so the bracket path never touches a guarded object.
 *
 * It goes quiescent, releases `claimed`, and sets the record's cell to @ref kNoCell, so a
 * bracket a later `thread_local` destructor opens counts itself into
 * @ref control_t::overflow_online, which blocks every free, instead of announcing on a cell
 * another thread may own by then.
 */
struct cell_release_t {
    cell_release_t() = default;
    cell_release_t(const cell_release_t&) = delete;
    cell_release_t& operator=(const cell_release_t&) = delete;
    /** @brief Give the cell back; see the class. */
    ~cell_release_t() {
        thread_record_t& t = rec();
        if (t.cell != 0 && t.cell != kNoCell) {
            cell_t& c = registry().cells[t.cell - 1];
            c.state.store(0, std::memory_order_seq_cst);
            c.claimed.store(false, std::memory_order_release);
        }
        t.cell = kNoCell;
    }
};

/**
 * @brief Claim a QSBR cell for this thread, once: the table walk, out of line so the CAS sweep
 *        never lands in an inlined `fan_out`.
 *
 * @return The record's new cell field. @ref kNoCell, when every index is taken, is NOT a
 *         performance fallback, unlike `%edge_pin.hpp`'s: a thread that cannot announce
 *         itself is a thread a scan cannot see. It counts itself into
 *         @ref control_t::overflow_online instead, and any non-zero reading of that blocks
 *         every reclamation: safe, bounded, and visible as a stall rather than a crash. Raise
 *         @ref tr::graph::default_config_t::kQsbrParticipants if it happens. A full table is
 *         not re-walked on every dispatch: the record keeps @ref kNoCell.
 */
[[gnu::noinline]] inline std::uint32_t claim(thread_record_t& t) noexcept {
    registry_t& r = registry();
    for (std::size_t i = 0; i < kQsbrParticipants; ++i) {
        bool expected = false;
        if (r.cells[i].claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                       std::memory_order_relaxed)) {
            [[maybe_unused]] static thread_local cell_release_t release;
            t.cell = static_cast<std::uint32_t>(i + 1);
            return t.cell;
        }
    }
    t.cell = kNoCell;
    return kNoCell;
}

/** @brief Announce that this thread is entering a dispatch — the read-side half. */
inline void go_online(thread_record_t& t) noexcept {
    registry_t& r = registry();
    std::uint32_t c = t.cell;
    if (c == 0) [[unlikely]]
        c = claim(t);
    if (c == kNoCell) {
        r.ctl.overflow_online.fetch_add(1, std::memory_order_relaxed);
        full_fence();
        return;
    }
    // A STALE (smaller) epoch is always the conservative direction — it can only make a scan
    // decide this thread is not yet past a retirement it is in fact past, which defers a free
    // rather than permitting one. Relaxed where the fence follows; acquire for the plain
    // store, whose proof needs it (@ref qsbr_asym case 2). Both stores are releases, so a scan
    // that reads one also sees this thread's earlier brackets done (@ref qsbr_ordering).
    const std::uint32_t e =
        r.ctl.epoch.load(kMembarrier ? std::memory_order_acquire : std::memory_order_relaxed);
    const std::uint32_t word = (e & kEpochMask) | kOnlineBit;
    if constexpr (kMembarrier) {
        if (r.ctl.asym.load(std::memory_order_relaxed) != 0) {
            r.cells[c - 1].state.store(word, std::memory_order_release);
            std::atomic_signal_fence(std::memory_order_seq_cst);  // keeps S_R before L_R
            return;
        }
    }
    // The fence after the store orders it before this bracket's reads (@ref qsbr_ordering).
    r.cells[c - 1].state.store(word, kStoreOrder);
    full_fence();
}

/** @brief Announce that this thread now holds no snapshot — the quiescent state itself. */
inline void go_offline(thread_record_t& t) noexcept {
    registry_t& r = registry();
    if (t.cell == kNoCell) {
        r.ctl.overflow_online.fetch_sub(1, std::memory_order_seq_cst);
        return;
    }
    // On one hart the barrier keeps this bracket's reads above the store (@ref qsbr_single_hart).
    if constexpr (kCompilerFence) std::atomic_signal_fence(std::memory_order_seq_cst);
    r.cells[t.cell - 1].state.store(0, kStoreOrder);
}

/**
 * @brief Enter a read-side section on this thread. Only the outermost one announces, so a
 *        nested section never re-announces at a newer epoch than the one it is covered by.
 */
inline void enter() noexcept {
    thread_record_t& t = rec();
    if (t.depth++ == 0) go_online(t);
}

/**
 * @brief Leave one read-side section.
 * @return True when it was the outermost: this thread is now quiescent.
 */
[[nodiscard]] inline bool leave() noexcept {
    thread_record_t& t = rec();
    if (--t.depth != 0) return false;
    go_offline(t);
    return true;
}

/**
 * @brief Close the epoch after a writer's unpublish: what the writer parks now is free once
 *        @ref horizon_t::past answers true for the value returned.
 *
 * `seq_cst`, and load-bearing for the same reason @ref retire's bump is: this RMW (or, when
 * stalled, the fence) is the full barrier between the caller's unpublish and any later scan.
 *
 * Does not bump while an open bracket is @ref kStallLag or more behind the counter, and
 * answers the current epoch instead, re-read `seq_cst` after the fence: what is parked then
 * waits for that bracket too (@ref qsbr_wrap). The check is one relaxed load per
 * participant, on the control plane.
 */
[[nodiscard]] inline std::uint32_t advance() noexcept {
    registry_t& r = registry();
    const std::uint32_t now = r.ctl.epoch.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < kQsbrParticipants; ++i) {
        const std::uint32_t s = r.cells[i].state.load(std::memory_order_relaxed);
        // A cell AHEAD of `now` (a concurrent bump landed after the load) reads as a lag past
        // the window and is not a stall; a cell really behind is never more than kStallLag
        // plus the concurrent writers behind, far inside it.
        const std::uint32_t lag = (now - s) & kEpochMask;
        if (s != 0 && lag >= kStallLag && lag < kEpochWindow) {
            // Re-read after the fence: a concurrent bump since `now` is then in the answer,
            // and a later epoch only makes what the caller parks wait longer.
            full_fence();
            return r.ctl.epoch.load(std::memory_order_seq_cst);
        }
    }
    return r.ctl.epoch.fetch_add(1, std::memory_order_seq_cst);
}

/**
 * @brief Register this process for `membarrier`, once; true when readers may enter with a
 *        plain store (@ref qsbr_asym).
 *
 * The first call registers (`MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED`) and, on success, sets
 * `control_t::asym`; every later call returns the cached answer. `graph_t`'s constructor calls
 * it so the plain entry is in use from the start, and every fencing scan calls it before it
 * relies on the answer. A refused registration leaves the fenced entry in place for good.
 */
[[nodiscard]] inline bool asym_ready() noexcept {
#if defined(LIBTRACER_QSBR_MEMBARRIER)
    if constexpr (kMembarrier) {
        static const bool ok = [] {
            const bool reg =
                syscall(__NR_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) == 0;
            if (reg) registry().ctl.asym.store(1, std::memory_order_relaxed);
            return reg;
        }();
        return ok;
    }
#endif
    return false;
}

/**
 * @brief The epochs one scan saw as past: an arc of the 2^31 epoch circle that ends at
 *        @ref end and is @ref len long (@ref qsbr_horizon). The default value covers nothing.
 */
struct horizon_t {
    std::uint32_t end = 0; /**< @brief The newest epoch that is past. */
    std::uint32_t len = 0; /**< @brief How many epochs, counting back from @ref end, are past. */

    /** @brief Is epoch @p e past for every bracket this horizon saw? */
    [[nodiscard]] constexpr bool past(std::uint32_t e) const noexcept {
        return ((end - e) & kEpochMask) < len;
    }
};

/**
 * @brief The scan alone, with no fence: the epochs every participant is past, given that
 *        @p c was read from the counter before the scan. @p MO orders its loads: `acquire`
 *        after the pass's fence, so a reader's offline store it reads synchronizes with it,
 *        and `relaxed` for the look ahead (after `F1`, before any `membarrier`) whose answer
 *        only decides whether to pay the `membarrier`.
 *
 * Each online cell `s` is past exactly the arc of epochs @ref epoch_after places before it, and
 * so is the counter value @p c: arcs of one length, `kEpochWindow - 1`, ending at `s - 1` and
 * `c - 1`. Arcs of one length meet in the arc that ends at the oldest end and is shortened by
 * the spread of the ends, or in nothing once that spread reaches the length. So the scan keeps
 * only each cell's lag behind the counter, a signed 31-bit distance (a cell read after `c` may
 * be ahead of it), and its extremes. A thread counted in `overflow_online` has no epoch, so the
 * answer is then the empty arc.
 */
template <std::memory_order MO>
[[nodiscard]] inline horizon_t scan(std::uint32_t c) noexcept {
    registry_t& r = registry();
    if (r.ctl.overflow_online.load(MO) != 0) return {};
    std::int32_t behind = 0;  // the counter's own lag is 0
    std::int32_t ahead = 0;
    for (std::size_t i = 0; i < kQsbrParticipants; ++i) {
        const std::uint32_t s = r.cells[i].state.load(MO);
        if (s == 0) continue;
        // Shifted up and back down, the 31-bit distance `c - s` becomes a signed lag.
        const auto lag = static_cast<std::int32_t>(((c - s) & kEpochMask) << 1) >> 1;
        behind = std::max(behind, lag);
        ahead = std::min(ahead, lag);
    }
    const std::int32_t len = static_cast<std::int32_t>(kEpochWindow - 1) - (behind - ahead);
    if (len <= 0) return {};
    return {c - 1 - static_cast<std::uint32_t>(behind), static_cast<std::uint32_t>(len)};
}

/**
 * @brief One reclaim pass's horizon: the counter, then the fence, then the scan
 *        (@ref qsbr_horizon). Its @ref horizon_t::past is a grace period for any entry the
 *        caller then finds, whenever that entry was parked.
 *
 * The fence `F_W` comes first, before any scan. It is the grace period's fence, and it is
 * also what keeps a pass live: a reader that leaves and drains has made its offline store
 * before it, so of two readers leaving at once, at least one sees the other gone (a scan
 * BEFORE the fence could see each other still online, and both would return without freeing
 * a pair nothing then frees). With the asymmetric fence (@ref qsbr_asym) the pass then asks
 * @p wants with a scan that pays no `membarrier`, and pays `membarrier` and `F2` only when it
 * answers true; a false only defers. A `membarrier` call that fails yields the empty horizon,
 * so frees are deferred, never early. The fenced build has no such question: its one fence is
 * the whole cost.
 */
template <class Wants>
[[nodiscard]] inline horizon_t horizon([[maybe_unused]] Wants&& wants) noexcept {
    registry_t& r = registry();
    const std::uint32_t c = r.ctl.epoch.load(kLoadOrder);  // L_c, before F_W
    full_fence();
#if defined(LIBTRACER_QSBR_MEMBARRIER)
    if constexpr (kMembarrier) {
        if (!wants(scan<std::memory_order_relaxed>(c))) return {};
        if (asym_ready() && syscall(__NR_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0) != 0)
            return {};
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
#endif
    return scan<kLoadOrder>(c);
}

/**
 * @brief Run every retired pair whose grace period has elapsed, under one horizon.
 *
 * Called at a quiescent point (and once more by a retiring thread that had to defer). A slot is
 * CLAIMED before its epoch is trusted: between a load of the epoch and the claim, another
 * drainer may run that pair and a retiring thread refill the slot, and the claim alone would
 * then win a pair this pass never checked. Claimed, the slot cannot change, so its epoch is
 * checked again. A refilled slot is judged by its own epoch, as an entry appended after the
 * scan is (see @ref qsbr_horizon): it passes only if stamped before the counter read, and
 * otherwise goes back.
 *
 * A slot is EMPTIED before its hook runs, for the reason `%graph.cpp`'s park drain gives: a
 * hook is arbitrary user code that may publish, and a nested publish re-enters here.
 *
 * With @ref kMembarrier the pass runs single flight (@ref qsbr_single_flight): it holds
 * @ref in_flight from before its horizon until after its last hook, and runs ONE more pass,
 * unflagged, when its release shows that a drainer skipped meanwhile.
 */
[[gnu::noinline]] inline void drain_ripe() noexcept {
    registry_t& r = registry();
    const auto ripe = [&r](const horizon_t& h0) {
        for (const retired_slot_t& s : r.retired)
            if (s.state.load(std::memory_order_acquire) == slot_state_t::READY &&
                h0.past(s.epoch.load(std::memory_order_relaxed)))
                return true;
        return false;
    };
    if constexpr (kMembarrier) {
        // The fence first, for the reason @ref horizon gives: this scan decides whether to run.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!ripe(scan<std::memory_order_relaxed>(r.ctl.epoch.load(std::memory_order_relaxed))) ||
            in_flight.fetch_add(1, std::memory_order_acq_rel) != 0)
            return;
    }
    for (bool held = kMembarrier;; held = false) {
        const horizon_t h = horizon(ripe);
        for (retired_slot_t& s : r.retired) {
            slot_state_t expected = slot_state_t::READY;
            if (!h.past(s.epoch.load(std::memory_order_relaxed)) ||
                !s.state.compare_exchange_strong(expected, slot_state_t::CLAIMING,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_relaxed))
                continue;
            if (!h.past(s.epoch.load(std::memory_order_relaxed))) {
                s.state.store(slot_state_t::READY, std::memory_order_release);
                continue;
            }
            void* const ctx = s.ctx;
            void (*const fn)(void*) = s.release;
            s.ctx = nullptr;
            s.release = nullptr;
            r.ctl.live.fetch_sub(1, std::memory_order_relaxed);
            s.state.store(slot_state_t::EMPTY, std::memory_order_release);
            fn(ctx);
        }
        if constexpr (kMembarrier)
            if (held && in_flight.exchange(0, std::memory_order_acq_rel) != 1) continue;
        return;
    }
}

/**
 * @brief The quiescent-point entry point: reclaim anything ripe, cheaply deciding there is not.
 *
 * THE EARLY-OUT IS THE WHOLE POINT and it is why @ref drain_ripe is `noinline`. This is called
 * from every outermost dispatch exit, so all a publish may pay is one relaxed load of a
 * read-mostly line and one branch nobody takes; the scan behind it must never be inlined into
 * `fan_out`, which is a property `objdump` is asked to confirm rather than a hope. (Measured:
 * letting it inline added 162 instructions to `fan_out`.)
 */
inline void drain() noexcept {
    if (registry().ctl.live.load(std::memory_order_relaxed) == 0) return;
    drain_ripe();
}

/**
 * @brief Retire one `{ctx, release}` pair at this policy's grace point.
 *
 * Releases INLINE when no participant can still be holding a snapshot that names it — which
 * is every unsubscribe on a quiet node, and is what keeps property (a) ("quiescent on return")
 * true under this policy exactly as it is under the other two. Otherwise the pair is deferred
 * into the shared table and freed by whichever participant next completes the grace period.
 *
 * @return False when the pair was REFUSED a slot; the caller counts it and says so. Refusing
 *         is the same deliberate leak the per-thread park makes, for the same reason: running
 *         the hook here would free a context a live fan-out still names.
 */
inline bool retire(void* ctx, void (*release)(void*)) noexcept {
    registry_t& r = registry();
    // seq_cst, and load-bearing: this RMW is the full barrier standing between the caller's
    // `clear_edge` and the scan below, which is the writer half of the Dekker pairing.
    const std::uint32_t e = advance();
    if (horizon([e](const horizon_t& h) { return h.past(e); }).past(e)) {
        release(ctx);
        return true;
    }
    for (retired_slot_t& s : r.retired) {
        slot_state_t expected = slot_state_t::EMPTY;
        if (!s.state.compare_exchange_strong(expected, slot_state_t::FILLING,
                                             std::memory_order_acq_rel, std::memory_order_relaxed))
            continue;
        s.ctx = ctx;
        s.release = release;
        s.epoch.store(e, std::memory_order_relaxed);
        r.ctl.live.fetch_add(1, std::memory_order_relaxed);
        s.state.store(slot_state_t::READY, std::memory_order_release);
        // One retry, on THIS thread, now that the pair is visible to every drainer: the
        // participants that were online a moment ago may have quiesced during the push, and a
        // node whose only other thread has since gone idle would otherwise wait for a dispatch
        // that never comes. After this, a surviving entry means a participant genuinely has
        // not quiesced yet. Under single flight this retry may skip; the pass in flight then
        // runs once more for it (@ref qsbr_single_flight).
        drain();
        return true;
    }
    return false;
}

}  // namespace tr::graph::detail_qsbr

#endif  // LIBTRACER_QSBR_HPP
