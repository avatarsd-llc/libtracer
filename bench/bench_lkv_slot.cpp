/**
 * @file
 * @brief The LKV slot under concurrency: what ADR-0064 §2 would buy, and what each
 *        reclamation scheme costs to buy it (#604).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ADR-0064 §2 records that `std::atomic<std::shared_ptr<const rope_t>>` is **spin-locked**
 * in this libstdc++ — both the publish and every read take libstdc++'s hashed pointer-lock
 * bit — and that this is ~77 of the write path's remaining ~316 cycles, the largest single
 * term. It then declines to choose a replacement, because *reclamation is the whole
 * problem*: a reader that has loaded the pointer but not yet taken a reference must not
 * have the object freed under it. Three candidate schemes are named and none is picked.
 *
 * The ADR closes with the reason this file exists: *"The multi-writer win is unmeasured.
 * No bench in this repo exercises concurrent publishers on one stripe, so the contention
 * benefit is argued, not shown. A bench that does would also make §2's case measurable,
 * and is the natural prerequisite to attempting it."* Everything below is that bench.
 *
 * ## Part A — the slot in isolation, five arms
 *
 * The same workload against five implementations of "one pointer to an immutable value,
 * published by writers and read by readers":
 *
 *   sp-atomic   `std::atomic<std::shared_ptr<const T>>` — what libtracer ships today.
 *               Publish and read both take the libstdc++ lock bit; the read additionally
 *               pays a refcount RMW.
 *   raw-lag     `std::atomic<T*>` release/acquire with **no reclamation scheme at all**:
 *               the writer frees a payload only after a fixed lag of @ref kLagDepth further
 *               publishes. This is NOT a correct scheme and must never ship — a slow reader
 *               can still be holding a lagged pointer. It is the **ceiling**: the fastest a
 *               lock-free slot could possibly be if reclamation were free. ADR-0064 sized
 *               its own single-threaded terms the same way ("semantics-breaking, for sizing
 *               only").
 *   epoch       Epoch-based reclamation. The reader publishes the global epoch into its own
 *               padded registry slot (`seq_cst`, so the writer's scan cannot miss it), reads,
 *               then clears. The writer retires the displaced pointer with the epoch that
 *               follows it and frees a retired batch once no reader is still announcing an
 *               older one. Cheap read side; costs a per-thread registry.
 *   hazard      Hazard pointers. The reader publishes the pointer it is about to use
 *               (`seq_cst` store) and re-validates the slot; the writer scans every hazard
 *               slot before freeing. Bounded memory, but the read side pays exactly the kind
 *               of serializing store §2 is trying to delete. **Its reader does not keep the
 *               object** — it extracts one word inside the pinned window and unpins before
 *               returning, which is a cheaper contract than the real slot has.
 *   hazard-ref  The same scheme under the contract `vertex_t::read_stored()` actually has:
 *               the read returns an **owning** reference, so the pin must be promoted to a
 *               counted one before it is dropped. That promotion is a contended RMW on the
 *               shared payload — the same class of operation that makes `sp-atomic`'s read
 *               collapse. This arm exists because ADR-0069 §1 quoted `hazard`'s number as
 *               the host read cost, and `hazard`'s number was bought with a contract the
 *               library cannot honour (see @ref model_hazard_ref_t).
 *
 * Three shapes, because the arms do not rank the same way in all of them:
 *
 *   read        T readers, no writer. The pure read-side cost — ADR-0064's *"it is also
 *               paid by every read"*, isolated.
 *   pubsub      1 writer + (T-1) readers on ONE slot. The LKV shape: a sensor publishes and
 *               in-process consumers read. This is the shape the design centre cares about.
 *   write       T writers on T DISTINCT slots. No two threads share a value — so any
 *               scaling loss here is **false** contention, e.g. two unrelated vertices
 *               hashing to the same entry of libstdc++'s lock-bit table.
 *
 * ## Part B — the same question on the real path
 *
 * `run_graph` drives `graph_t::write` — and, since #604's read arm, `graph_t::read` — from T
 * threads, in three topologies, so Part A's finding lands on production code rather than a
 * model of it. The `-read` rows are the ones a slot change is gated on: Part A's cheap arms
 * measure a read that keeps nothing, while `graph_t::read` copies the rope out (cloning every
 * link's `segment_ptr_t`), so a shared LKV's refcount traffic is part of the real cost and no
 * model arm can be substituted for it. Topologies:
 *
 *   hot1        T threads driving ONE vertex — one lock bit, one `write_seq_`. For the read
 *               arm this is the shape that matters: T readers on ONE shared LKV.
 *   stripe1     T threads driving T DISTINCT vertices deliberately chosen to collide on a
 *               single `vertex_stripe_t`. After #555 a waiterless non-STREAM publish takes
 *               no stripe lock at all, so the expectation was that this arm now matches
 *               `spread`. **It does not** — `fan_out` reaches `snapshot_edges`, which takes
 *               the stripe mutex unconditionally, and that lock costs ×16.6 at T=24. See
 *               ADR-0064's corrected "Considered options" entry.
 *   spread      T threads writing T distinct vertices on T distinct stripes. Capped at
 *               tr::graph::kVertexLockStripes threads, since beyond that a collision is
 *               forced by the pigeonhole.
 *
 * Each topology runs at fan-0 (the store leg alone) and fan-1 (`inproc`, the reference point
 * the rest of the suite sweeps around), because the stripe lock is taken on both.
 *
 * DIAGNOSTIC, not a CI gate: thread-contention numbers are runner-dependent, so this is
 * deliberately not wired into perf.yml's regression gate — the same call
 * bench_rx_source_topology, bench_route_handle_contention and bench_fanout_clone_storm make.
 *
 * Output: the shared bench RESULT contract (bench_common.hpp) —
 *   mode=lkv_<shape>_<arm> / lkvgraph_<topology>-fan<N>[-read], fanout=T,
 *   pub_per_s=per-thread ops/s, deliv_per_s=aggregate ops/s, latency fields = ns/op.
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

using namespace bench;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::graph::vertex_t;
using tr::view::view_t;

namespace {

/** @brief Hard ceiling on threads, so both registries are fixed-size arrays with no
 *         allocation and no resize race inside the timed window. */
constexpr std::size_t kMaxThreads = 64;

/** @brief The swept thread counts. Matches bench_rx_source_topology so the two
 *         contention curves are read on the same axis. */
constexpr std::size_t kThreads[] = {1, 2, 4, 8, 16, 24};

/** @brief Per-thread operations in the throughput phase. */
constexpr std::size_t kOps = 400'000;

/** @brief Per-thread samples in the latency phase. */
constexpr std::size_t kLatOps = 100'000;

/** @brief How many publishes `raw-lag` waits before freeing a displaced payload. Large
 *         enough that the ceiling arm is not secretly measuring a use-after-free storm,
 *         small enough that its memory stays bounded (kLagDepth * sizeof(payload_t)). */
constexpr std::size_t kLagDepth = 1024;

/** @brief Retired pointers a writer accumulates before it attempts a reclamation scan.
 *         Standard EBR/hazard batching: the scan is O(threads), so amortizing it over a
 *         batch is part of the scheme, not a thumb on the scale. */
constexpr std::size_t kReclaimBatch = 64;

/**
 * @brief The published value. Sized to the real thing: ADR-0064 measured a leaf write's
 *        single allocation as the **104-byte** `allocate_shared` of the LKV rope, so the
 *        model allocates the same number of bytes per publish that production does.
 */
struct payload_t {
    std::uint64_t tag = 0;
    std::byte fill[96]{};
};
static_assert(sizeof(payload_t) == 104, "keep the model payload the size ADR-0064 measured");

/** @brief Cache-line-isolated registry slot: adjacent threads' announcements must never
 *         false-share, or the bench measures the padding bug instead of the scheme. */
struct alignas(64) slot_u64_t {
    std::atomic<std::uint64_t> v{0};
};

/** @brief Cache-line-isolated hazard slot (same reasoning as @ref slot_u64_t). */
struct alignas(64) slot_ptr_t {
    std::atomic<payload_t*> v{nullptr};
};

/** @brief Per-writer retire list, cache-line isolated so two writers' bookkeeping does not
 *         share a line in the `write` shape. */
struct alignas(64) retire_list_t {
    std::vector<std::pair<payload_t*, std::uint64_t>> items;
    std::size_t peak = 0;
};

// --- arm 1: what libtracer ships today ----------------------------------------------------

/**
 * @brief `std::atomic<std::shared_ptr<const payload_t>>` — the slot as it exists.
 *
 * Reclamation is the refcount, so there is no scheme to implement; the cost shows up as
 * the lock bit on both sides plus the RMW on the read side.
 */
class model_sp_atomic_t {
   public:
    void publish(std::size_t, std::uint64_t tag) {
        auto sp = std::make_shared<payload_t>();
        sp->tag = tag;
        slot_.store(std::move(sp));
    }
    [[nodiscard]] std::uint64_t read(std::size_t) {
        const std::shared_ptr<const payload_t> sp = slot_.load();
        return sp ? sp->tag : 0;
    }
    void seed() { publish(0, 1); }
    void drain() { slot_.store({}); }
    [[nodiscard]] std::size_t peak_parked() const { return 0; }
    [[nodiscard]] std::size_t registry_bytes(std::size_t) const { return 0; }

   private:
    std::atomic<std::shared_ptr<const payload_t>> slot_{};
};

// --- arm 2: the ceiling (NOT a correct scheme) --------------------------------------------

/**
 * @brief A plain `std::atomic<payload_t*>` with fixed-lag frees — the upper bound on what
 *        §2 can be worth.
 *
 * Deliberately unsound: a reader stalled for @ref kLagDepth publishes can dereference freed
 * memory. It exists so the two real schemes below can be quoted as a fraction of a known
 * ceiling rather than only against today's number.
 */
class raw_lag_slot_t {
   public:
    void publish(std::size_t, std::uint64_t tag) {
        auto* p = new payload_t;
        p->tag = tag;
        slot_.store(p, std::memory_order_release);
        const std::size_t i = cursor_++ % kLagDepth;
        delete lag_[i];  // freed only after kLagDepth further publishes
        lag_[i] = p;
    }
    [[nodiscard]] std::uint64_t read(std::size_t) {
        const payload_t* p = slot_.load(std::memory_order_acquire);
        return p ? p->tag : 0;
    }
    void seed() { publish(0, 1); }
    void drain() {
        slot_.store(nullptr, std::memory_order_release);
        for (auto*& p : lag_) {
            delete p;
            p = nullptr;
        }
    }
    [[nodiscard]] std::size_t peak_parked() const { return kLagDepth; }
    [[nodiscard]] std::size_t registry_bytes(std::size_t) const { return 0; }

   private:
    std::atomic<payload_t*> slot_{nullptr};
    std::array<payload_t*, kLagDepth> lag_{};
    std::size_t cursor_ = 0;  // writer-local in every shape (one writer per slot)
};

// --- arm 3: epoch-based reclamation --------------------------------------------------------

/**
 * @brief Epoch-based reclamation (ADR-0064 §2 candidate 1).
 *
 * A reader announces the current global epoch in its own registry slot before loading the
 * pointer, and clears it afterwards. A writer retires the displaced pointer stamped with
 * the epoch it advanced to, and frees a retired entry once every announcing reader is at a
 * strictly later epoch. The announce is `seq_cst` and the writer's bump precedes its scan,
 * so a reader that announced an old epoch is always visible to the scan that would free
 * what it is about to read.
 */
class epoch_slot_t2 {
   public:
    void publish(std::size_t tid, std::uint64_t tag) {
        auto* p = new payload_t;
        p->tag = tag;
        payload_t* old = slot_.exchange(p, std::memory_order_acq_rel);
        const std::uint64_t e = global_.fetch_add(1, std::memory_order_seq_cst) + 1;
        if (old != nullptr) {
            retire_list_t& rl = retired_[tid];
            rl.items.emplace_back(old, e);
            rl.peak = std::max(rl.peak, rl.items.size());
            if (rl.items.size() >= kReclaimBatch) reclaim(rl);
        }
    }
    [[nodiscard]] std::uint64_t read(std::size_t tid) {
        // Announce BEFORE loading, seq_cst, so a concurrent scan cannot conclude this
        // reader is quiescent while it is about to dereference.
        reg_[tid].v.store(global_.load(std::memory_order_relaxed), std::memory_order_seq_cst);
        const payload_t* p = slot_.load(std::memory_order_acquire);
        const std::uint64_t t = p ? p->tag : 0;
        reg_[tid].v.store(0, std::memory_order_release);  // quiescent
        return t;
    }
    void seed() { publish(0, 1); }
    void drain() {
        payload_t* p = slot_.exchange(nullptr, std::memory_order_acq_rel);
        delete p;
        for (auto& rl : retired_) {
            for (auto& [q, e] : rl.items) delete q;
            rl.items.clear();
        }
    }
    [[nodiscard]] std::size_t peak_parked() const {
        std::size_t m = 0;
        for (const auto& rl : retired_) m = std::max(m, rl.peak);
        return m;
    }
    [[nodiscard]] std::size_t registry_bytes(std::size_t threads) const {
        return threads * sizeof(slot_u64_t);
    }

   private:
    void reclaim(retire_list_t& rl) {
        std::uint64_t floor = UINT64_MAX;
        for (const auto& s : reg_) {
            const std::uint64_t e = s.v.load(std::memory_order_seq_cst);
            if (e != 0) floor = std::min(floor, e);
        }
        std::size_t keep = 0;
        for (std::size_t i = 0; i < rl.items.size(); ++i) {
            if (rl.items[i].second < floor) {
                delete rl.items[i].first;
            } else {
                rl.items[keep++] = rl.items[i];
            }
        }
        rl.items.resize(keep);
    }

    std::atomic<payload_t*> slot_{nullptr};
    std::atomic<std::uint64_t> global_{1};
    std::array<slot_u64_t, kMaxThreads> reg_{};
    std::array<retire_list_t, kMaxThreads> retired_{};
};

// --- arm 4: hazard pointers ---------------------------------------------------------------

/**
 * @brief Hazard pointers (ADR-0064 §2 candidate 2).
 *
 * The reader publishes the pointer it intends to dereference and re-reads the slot to
 * confirm it did not change in between; the writer scans every hazard slot before freeing.
 * Memory is bounded by construction (at most one retired object per reader can be pinned),
 * but the read side pays a `seq_cst` store plus a second load — which is precisely the class
 * of serializing operation ADR-0064's own finding says is what actually costs.
 */
class model_hazard_t {
   public:
    void publish(std::size_t tid, std::uint64_t tag) {
        auto* p = new payload_t;
        p->tag = tag;
        payload_t* old = slot_.exchange(p, std::memory_order_acq_rel);
        if (old != nullptr) {
            retire_list_t& rl = retired_[tid];
            rl.items.emplace_back(old, 0);
            rl.peak = std::max(rl.peak, rl.items.size());
            if (rl.items.size() >= kReclaimBatch) reclaim(rl);
        }
    }
    [[nodiscard]] std::uint64_t read(std::size_t tid) {
        payload_t* p = nullptr;
        for (;;) {
            p = slot_.load(std::memory_order_acquire);
            haz_[tid].v.store(p, std::memory_order_seq_cst);
            if (slot_.load(std::memory_order_acquire) == p) break;  // still published: pinned
        }
        const std::uint64_t t = p ? p->tag : 0;
        haz_[tid].v.store(nullptr, std::memory_order_release);
        return t;
    }
    void seed() { publish(0, 1); }
    void drain() {
        payload_t* p = slot_.exchange(nullptr, std::memory_order_acq_rel);
        delete p;
        for (auto& rl : retired_) {
            for (auto& [q, e] : rl.items) delete q;
            rl.items.clear();
        }
    }
    [[nodiscard]] std::size_t peak_parked() const {
        std::size_t m = 0;
        for (const auto& rl : retired_) m = std::max(m, rl.peak);
        return m;
    }
    [[nodiscard]] std::size_t registry_bytes(std::size_t threads) const {
        return threads * sizeof(slot_ptr_t);
    }

   private:
    void reclaim(retire_list_t& rl) {
        std::array<payload_t*, kMaxThreads> pinned{};
        std::size_t n = 0;
        for (const auto& h : haz_) {
            payload_t* p = h.v.load(std::memory_order_seq_cst);
            if (p != nullptr) pinned[n++] = p;
        }
        std::size_t keep = 0;
        for (std::size_t i = 0; i < rl.items.size(); ++i) {
            payload_t* q = rl.items[i].first;
            const bool held =
                std::find(pinned.begin(), pinned.begin() + n, q) != pinned.begin() + n;
            if (held) {
                rl.items[keep++] = rl.items[i];
            } else {
                delete q;
            }
        }
        rl.items.resize(keep);
    }

    std::atomic<payload_t*> slot_{nullptr};
    std::array<slot_ptr_t, kMaxThreads> haz_{};
    std::array<retire_list_t, kMaxThreads> retired_{};
};

// --- arm 5: hazard pointers under the OWNING read contract --------------------------------

/**
 * @brief The published value with the intrusive refcount an owning read needs.
 *
 * Deliberately the same 104 bytes as @ref payload_t, so `hazard` and `hazard-ref` are read on
 * one axis and the delta is the contract, not the allocation size.
 */
struct rc_payload_t {
    std::atomic<std::uint32_t> rc{1}; /**< @brief Live references, the publisher's included. */
    std::atomic<bool> parked{false};  /**< @brief Already on a retire list; blocks a re-park. */
    std::uint64_t tag = 0;            /**< @brief The value a reader extracts. */
    std::byte fill[88]{};             /**< @brief Pad to the LKV rope size ADR-0064 measured. */
};
static_assert(sizeof(rc_payload_t) == 104, "keep both hazard arms on one payload size");

/** @brief Cache-line-isolated hazard slot over @ref rc_payload_t (see @ref slot_ptr_t). */
struct alignas(64) rc_slot_ptr_t {
    std::atomic<rc_payload_t*> v{nullptr};
};

/** @brief Per-thread retire list for @ref model_hazard_ref_t, cache-line isolated. */
struct alignas(64) rc_retire_list_t {
    std::vector<rc_payload_t*> items;
    std::size_t peak = 0;
};

/**
 * @brief Hazard pointers whose read returns an **owning** reference — the contract
 *        `vertex_t::read_stored()` actually has.
 *
 * The `hazard` arm pins, extracts one word, and unpins before returning: its reader never
 * keeps the object, so it never pays for ownership. The real slot cannot use that contract.
 * `graph_t::read_subtree_folded` stashes one LKV per node into a `std::vector<snap_node_t>`
 * that outlives the map lock and spans three passes (`core/src/graph.cpp`), so **N ropes are
 * held simultaneously** — a single per-thread hazard slot cannot express N pins, and the
 * handle must outlive the pinned window regardless.
 *
 * An owning read therefore has to promote: pin, `fetch_add` the payload's refcount, unpin,
 * hand back the counted reference. The promotion is a read-modify-write on a cache line
 * every reader shares, which is the same term that makes `sp-atomic`'s read collapse under
 * T readers. So the honest question is not "hazard vs epoch" but "how much of `sp-atomic`'s
 * read cost is the libstdc++ lock bit (which hazard deletes) and how much is the refcount
 * RMW (which it does not)" — and only this arm answers it.
 */
class model_hazard_ref_t {
   public:
    void publish(std::size_t tid, std::uint64_t tag) {
        auto* p = new rc_payload_t;
        p->tag = tag;
        rc_payload_t* old = slot_.exchange(p, std::memory_order_acq_rel);
        if (old != nullptr) release(tid, old);  // drop the publisher's reference
    }

    [[nodiscard]] std::uint64_t read(std::size_t tid) {
        rc_payload_t* p = nullptr;
        for (;;) {
            p = slot_.load(std::memory_order_acquire);
            haz_[tid].v.store(p, std::memory_order_seq_cst);
            if (slot_.load(std::memory_order_acquire) == p) break;  // still published: pinned
        }
        if (p == nullptr) {
            haz_[tid].v.store(nullptr, std::memory_order_release);
            return 0;
        }
        // Promote the pin to a counted reference, THEN unpin — this is what makes the handle
        // outlive the pinned window, and it is the cost the `hazard` arm never pays.
        p->rc.fetch_add(1, std::memory_order_acq_rel);
        haz_[tid].v.store(nullptr, std::memory_order_release);
        const std::uint64_t t = p->tag;  // dereferenced off the owned reference, not the pin
        release(tid, p);                 // the caller dropping its handle
        return t;
    }

    void seed() { publish(0, 1); }

    void drain() {
        rc_payload_t* p = slot_.exchange(nullptr, std::memory_order_acq_rel);
        delete p;  // single-threaded at drain: the publisher's reference is the only one left
        for (auto& rl : retired_) {
            for (auto* q : rl.items) delete q;
            rl.items.clear();
        }
    }

    [[nodiscard]] std::size_t peak_parked() const {
        std::size_t m = 0;
        for (const auto& rl : retired_) m = std::max(m, rl.peak);
        return m;
    }

    [[nodiscard]] std::size_t registry_bytes(std::size_t threads) const {
        return threads * sizeof(rc_slot_ptr_t);
    }

   private:
    /**
     * @brief Drop one reference; the last one parks the object for the hazard scan.
     *
     * Parking is latched, not repeated: a reader can pin a pointer, see it retired, and then
     * promote it back to `rc == 1`. `parked` makes that resurrection re-use the existing
     * list entry instead of adding a second one, which is what would double-free it.
     */
    void release(std::size_t tid, rc_payload_t* p) {
        if (p->rc.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        if (p->parked.exchange(true, std::memory_order_acq_rel)) return;  // already listed
        rc_retire_list_t& rl = retired_[tid];
        rl.items.push_back(p);
        rl.peak = std::max(rl.peak, rl.items.size());
        if (rl.items.size() >= kReclaimBatch) reclaim(rl);
    }

    void reclaim(rc_retire_list_t& rl) {
        std::array<rc_payload_t*, kMaxThreads> pinned{};
        std::size_t n = 0;
        for (const auto& h : haz_) {
            rc_payload_t* p = h.v.load(std::memory_order_seq_cst);
            if (p != nullptr) pinned[n++] = p;
        }
        std::size_t keep = 0;
        for (std::size_t i = 0; i < rl.items.size(); ++i) {
            rc_payload_t* q = rl.items[i];
            const bool held =
                std::find(pinned.begin(), pinned.begin() + n, q) != pinned.begin() + n;
            // Announced, or resurrected by a promotion since it was parked: stays parked.
            if (held || q->rc.load(std::memory_order_acquire) != 0) {
                rl.items[keep++] = q;
            } else {
                delete q;
            }
        }
        rl.items.resize(keep);
    }

    std::atomic<rc_payload_t*> slot_{nullptr};
    std::array<rc_slot_ptr_t, kMaxThreads> haz_{};
    std::array<rc_retire_list_t, kMaxThreads> retired_{};
};

// --- the runner ---------------------------------------------------------------------------

/** @brief Which workload shape a point runs. */
enum class shape_t { READ, PUBSUB, WRITE };

[[nodiscard]] const char* shape_name(shape_t s) {
    switch (s) {
        case shape_t::READ:
            return "read";
        case shape_t::PUBSUB:
            return "pubsub";
        case shape_t::WRITE:
            return "write";
    }
    return "?";
}

/** @brief Consumed so the optimizer cannot delete a read whose value is otherwise unused. */
std::atomic<std::uint64_t> g_sink{0};

/**
 * @brief Run one (arm, shape, T) point and emit its RESULT line.
 *
 * One `slot_t` per writer in the WRITE shape (T independent slots — nothing is shared, so
 * any loss is false contention); one shared slot otherwise.
 */
template <typename slot_t>
void run_slot(const char* arm, shape_t shape, std::size_t T) {
    if (shape == shape_t::PUBSUB && T < 2) return;  // needs a writer AND a reader

    const std::size_t slots = (shape == shape_t::WRITE) ? T : 1;
    std::vector<std::unique_ptr<slot_t>> ss;
    ss.reserve(slots);
    for (std::size_t i = 0; i < slots; ++i) {
        ss.push_back(std::make_unique<slot_t>());
        ss.back()->seed();
    }

    // A thread's job in this shape. In PUBSUB thread 0 writes and the rest read; in WRITE
    // every thread writes its own slot; in READ every thread reads the one shared slot.
    const auto is_writer = [&](std::size_t t) {
        return shape == shape_t::WRITE || (shape == shape_t::PUBSUB && t == 0);
    };
    const auto slot_of = [&](std::size_t t) -> slot_t& {
        return *ss[shape == shape_t::WRITE ? t : 0];
    };

    const auto body = [&](std::size_t t, std::size_t n) {
        slot_t& s = slot_of(t);
        std::uint64_t acc = 0;
        if (is_writer(t)) {
            for (std::size_t i = 0; i < n; ++i) s.publish(t, i + 2);
        } else {
            for (std::size_t i = 0; i < n; ++i) acc += s.read(t);
        }
        g_sink.fetch_add(acc, std::memory_order_relaxed);
    };

    // --- throughput phase ---
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    ts.reserve(T);
    for (std::size_t t = 0; t < T; ++t) {
        ts.emplace_back([&, t]() {
            body(t, 2000);  // warmup, untimed
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) { /* spin until released */
            }
            body(t, kOps);
        });
    }
    while (ready.load(std::memory_order_acquire) < T) { /* wait for all warmed up */
    }
    const std::uint64_t t0 = now_ns();
    go.store(true, std::memory_order_release);
    for (auto& th : ts) th.join();
    const double secs = static_cast<double>(now_ns() - t0) / 1e9;

    const double agg = secs > 0 ? static_cast<double>(T) * kOps / secs : 0.0;
    const double per_thread = agg / static_cast<double>(T);

    // --- latency phase: per-op timing under the same parallel load ---
    std::vector<Latency> lats(T);
    std::atomic<std::size_t> ready2{0};
    std::atomic<bool> go2{false};
    std::vector<std::thread> ls;
    ls.reserve(T);
    for (std::size_t t = 0; t < T; ++t) {
        ls.emplace_back([&, t]() {
            slot_t& s = slot_of(t);
            Latency mine;
            std::uint64_t acc = 0;
            ready2.fetch_add(1, std::memory_order_acq_rel);
            while (!go2.load(std::memory_order_acquire)) { /* spin */
            }
            for (std::size_t i = 0; i < kLatOps; ++i) {
                const std::uint64_t a = now_ns();
                if (is_writer(t)) {
                    s.publish(t, i + 2);
                } else {
                    acc += s.read(t);
                }
                mine.add(now_ns() - a);
            }
            g_sink.fetch_add(acc, std::memory_order_relaxed);
            lats[t] = std::move(mine);
        });
    }
    while (ready2.load(std::memory_order_acquire) < T) { /* wait */
    }
    go2.store(true, std::memory_order_release);
    for (auto& th : ls) th.join();

    Latency pooled;
    for (auto& l : lats) pooled.merge(l);

    // The scheme's RAM cost, on stderr so it never enters the RESULT stream: peak retired
    // objects still parked, and the per-thread registry. This is the other half of the
    // ruling #604 needs — a read-side win bought with unbounded parking is not a win under
    // the project's second priority.
    std::size_t peak = 0;
    for (const auto& s : ss) peak = std::max(peak, s->peak_parked());
    std::fprintf(stderr, "  [scheme] %s/%s T=%zu peak_parked=%zu (%zu B) registry=%zu B\n", arm,
                 shape_name(shape), T, peak, peak * sizeof(payload_t),
                 ss.front()->registry_bytes(T));

    for (auto& s : ss) s->drain();

    const std::string mode = std::string("lkv_") + shape_name(shape) + "_" + arm;
    emit("libtracer", mode.c_str(), sizeof(payload_t), T, /*endpoints=*/1, per_thread, agg,
         /*mb_per_s=*/0.0, pooled.summarize());
}

/** @brief Every arm at one (shape, T), so the five are always read as a set. */
void run_all_arms(shape_t shape, std::size_t T) {
    run_slot<model_sp_atomic_t>("sp-atomic", shape, T);
    run_slot<raw_lag_slot_t>("raw-lag", shape, T);
    run_slot<epoch_slot_t2>("epoch", shape, T);
    run_slot<model_hazard_t>("hazard", shape, T);
    run_slot<model_hazard_ref_t>("hazard-ref", shape, T);
}

// --- Part B: the real write path ----------------------------------------------------------

/** @brief Which vertex topology the T threads drive. */
enum class topo_t { HOT1, STRIPE1, SPREAD };

/**
 * @brief Which graph verb the timed window drives.
 *
 * Part A ranks model slots; only this dimension puts the ranking on the real member. `READ`
 * exists because ADR-0069 quoted Part A's `hazard` read as the host read cost, and Part A's
 * read contract is not the one `graph_t::read` has — it copies the rope out (cloning each
 * link's `segment_ptr_t`) before returning, so the shared refcount traffic Part A's cheap arm
 * never pays is unavoidable here. This arm is the baseline any slot change must beat.
 */
enum class op_t { WRITE, READ };

[[nodiscard]] const char* op_name(op_t o) {
    switch (o) {
        case op_t::WRITE:
            return "";  // the pre-existing rows keep their exact names
        case op_t::READ:
            return "-read";
    }
    return "?";
}

[[nodiscard]] const char* topo_name(topo_t t) {
    switch (t) {
        case topo_t::HOT1:
            return "hot1";
        case topo_t::STRIPE1:
            return "stripe1";
        case topo_t::SPREAD:
            return "spread";
    }
    return "?";
}

/** @brief A 64-byte VALUE TLV — the reference payload the rest of the suite sweeps around. */
[[nodiscard]] std::vector<std::byte> value_tlv(std::size_t n) {
    std::vector<std::byte> body(n, std::byte{0xA5});
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, tr::wire::type_t::VALUE, tr::wire::opt_t{}, body);
    return out;
}

/**
 * @brief Pick T vertices matching @p topo, by their stripe index.
 *
 * The stripe a vertex rides is `vertex_stripe_index(&v)` — a hash of its pinned address —
 * so the only way to *choose* a topology is to register a surplus of vertices and select
 * from them. `vertex_handle_t` keeps its `vertex_t*` private, but the class asserts
 * immediately below its definition that it is trivially copyable and exactly pointer-sized,
 * which is precisely the license a bench needs to recover the address for a read-only
 * classification like this one.
 *
 * @return T POOL INDICES (not handles: the caller needs each vertex's path too, since
 *         `subscribe`'s callback sugar is path-keyed), or empty if the surplus did not
 *         contain enough matches.
 */
[[nodiscard]] std::vector<std::size_t> select_by_stripe(const std::vector<vertex_handle_t>& pool,
                                                        topo_t topo, std::size_t T) {
    std::vector<std::size_t> pick;
    if (topo == topo_t::HOT1) {
        pick.assign(T, 0);  // every thread drives the SAME vertex
        return pick;
    }
    std::vector<std::vector<std::size_t>> by_stripe(tr::graph::kVertexLockStripes);
    for (std::size_t i = 0; i < pool.size(); ++i)
        by_stripe[tr::graph::vertex_stripe_index(std::bit_cast<vertex_t*>(pool[i]))].push_back(i);
    if (topo == topo_t::STRIPE1) {
        for (const auto& bucket : by_stripe) {
            if (bucket.size() >= T) return {bucket.begin(), bucket.begin() + T};
        }
        return {};
    }
    for (const auto& bucket : by_stripe) {  // SPREAD: one vertex from T distinct stripes
        if (!bucket.empty()) pick.push_back(bucket.front());
        if (pick.size() == T) return pick;
    }
    return {};
}

/**
 * @brief Run one (topology, T) point on the production `graph_t::write` verb.
 *
 * Every thread reuses one borrowed view over its own buffer, so the timed loop allocates
 * nothing beyond what `store` itself allocates — what is being compared is the publish, not
 * the allocator.
 *
 * @p subs selects the leg: 0 subscribers is the STORE leg alone (the 233-of-335 cycles
 * ADR-0064 attributes to it); 1 subscriber is `inproc`, the fan-1 reference point the rest
 * of the suite sweeps around. Both are run, because `graph_t::fan_out` takes the stripe
 * mutex in `snapshot_edges` **unconditionally** — a zero-subscriber write pays it too — so
 * measuring only the empty case would leave the finding open to "that is just the idle
 * path".
 */
void run_graph(topo_t topo, std::size_t T, std::size_t subs, op_t op) {
    if (topo == topo_t::SPREAD && T > tr::graph::kVertexLockStripes) return;  // pigeonhole

    graph_t g;
    std::vector<vertex_handle_t> pool;
    std::vector<path_t> paths;
    constexpr std::size_t kSurplus = 4096;  // enough that some stripe holds >= 24 vertices
    pool.reserve(kSurplus);
    paths.reserve(kSurplus);
    for (std::size_t i = 0; i < kSurplus; ++i) {
        auto p = path_t::parse("/bench/lkv/v" + std::to_string(i));
        if (!p) continue;
        pool.push_back(g.register_vertex(*p, role_t::STORED_VALUE));
        paths.push_back(*p);
    }
    const std::vector<std::size_t> idx = select_by_stripe(pool, topo, T);
    if (idx.size() != T) {
        std::fprintf(stderr, "SKIP lkvgraph_%s T=%zu: no %zu vertices in that topology\n",
                     topo_name(topo), T, T);
        return;
    }
    std::vector<vertex_handle_t> verts;
    verts.reserve(T);
    for (std::size_t i : idx) verts.push_back(pool[i]);

    // One in-process callback subscriber per DISTINCT vertex when asked. HOT1 names the same
    // vertex T times, so subscribing per thread there would silently turn a fan-1 comparison
    // into fan-T; it gets exactly one.
    std::vector<std::unique_ptr<slot_u64_t>> recv;
    const std::size_t distinct = (topo == topo_t::HOT1) ? 1 : T;
    for (std::size_t t = 0; t < distinct && subs > 0; ++t) {
        recv.push_back(std::make_unique<slot_u64_t>());
        (void)g.subscribe(
            paths[idx[t]],
            [](void* ctx, const tr::view::rope_t&) {
                static_cast<slot_u64_t*>(ctx)->v.fetch_add(1, std::memory_order_relaxed);
            },
            recv.back().get());
    }

    constexpr std::size_t S = 64;
    std::vector<std::vector<std::byte>> bufs(T);
    std::vector<view_t> views;
    views.reserve(T);
    const std::vector<std::byte> tlv = value_tlv(S);
    for (std::size_t t = 0; t < T; ++t) {
        bufs[t] = tlv;  // per-thread copy => per-thread segment, no shared refcount
        views.push_back(view_t::over(tr::view::borrow_const(bufs[t])));
    }

    // Prove the wiring writes AND delivers before timing it: a topology selection that
    // quietly produced the wrong handles, or a subscription that failed to attach, would
    // emit a perfectly plausible row measuring the cost of not delivering.
    if (!g.write(verts[0], views[0]).has_value() || !g.read(verts[0]).has_value()) {
        std::fprintf(stderr, "SKIP lkvgraph_%s T=%zu: write/read wiring check failed\n",
                     topo_name(topo), T);
        return;
    }
    if (subs > 0 && recv.front()->v.load(std::memory_order_relaxed) == 0) {
        std::fprintf(stderr, "SKIP lkvgraph_%s T=%zu: subscriber received nothing\n",
                     topo_name(topo), T);
        return;
    }

    // A READ arm must find a landed LKV on EVERY vertex it reads, not just verts[0]: an
    // unseeded vertex answers NOT_FOUND, which measures the miss path instead of the slot.
    if (op == op_t::READ)
        for (std::size_t t = 0; t < T; ++t) (void)g.write(verts[t], views[t]);

    // The timed operation. The READ result is folded into a thread-local accumulator and
    // published to the sink ONCE after the loop — an atomic per op would swamp the very
    // contention the arm exists to measure.
    const auto drive = [&](std::size_t t) -> std::size_t {
        if (op == op_t::WRITE) {
            (void)g.write(verts[t], views[t]);
            return 0;
        }
        const auto r = g.read(verts[t]);
        return r ? r->total_length() : 0;
    };

    std::atomic<std::size_t> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    ts.reserve(T);
    for (std::size_t t = 0; t < T; ++t) {
        ts.emplace_back([&, t]() {
            std::size_t acc = 0;
            for (std::size_t i = 0; i < 2000; ++i) acc += drive(t);  // warmup
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) { /* spin until released */
            }
            for (std::size_t i = 0; i < kOps; ++i) acc += drive(t);
            g_sink.fetch_add(acc, std::memory_order_relaxed);
        });
    }
    while (ready.load(std::memory_order_acquire) < T) { /* wait for all warmed up */
    }
    const std::uint64_t t0 = now_ns();
    go.store(true, std::memory_order_release);
    for (auto& th : ts) th.join();
    const double secs = static_cast<double>(now_ns() - t0) / 1e9;
    const double agg = secs > 0 ? static_cast<double>(T) * kOps / secs : 0.0;

    std::vector<Latency> lats(T);
    std::atomic<std::size_t> ready2{0};
    std::atomic<bool> go2{false};
    std::vector<std::thread> ls;
    ls.reserve(T);
    for (std::size_t t = 0; t < T; ++t) {
        ls.emplace_back([&, t]() {
            Latency mine;
            std::size_t acc = 0;
            ready2.fetch_add(1, std::memory_order_acq_rel);
            while (!go2.load(std::memory_order_acquire)) { /* spin */
            }
            for (std::size_t i = 0; i < kLatOps; ++i) {
                const std::uint64_t a = now_ns();
                acc += drive(t);
                mine.add(now_ns() - a);
            }
            g_sink.fetch_add(acc, std::memory_order_relaxed);
            lats[t] = std::move(mine);
        });
    }
    while (ready2.load(std::memory_order_acquire) < T) { /* wait */
    }
    go2.store(true, std::memory_order_release);
    for (auto& th : ls) th.join();

    Latency pooled;
    for (auto& l : lats) pooled.merge(l);

    const std::string mode =
        std::string("lkvgraph_") + topo_name(topo) + (subs > 0 ? "-fan1" : "-fan0") + op_name(op);
    emit("libtracer", mode.c_str(), S, T, /*endpoints=*/1, agg / static_cast<double>(T), agg,
         agg * static_cast<double>(S) / 1e6, pooled.summarize());
}

}  // namespace

int main(int argc, char** argv) {
    const std::string only = argc > 1 ? argv[1] : "";
    const std::size_t hw = std::max<std::size_t>(1, std::thread::hardware_concurrency());

    if (only.empty() || only == "slot") {
        for (shape_t shape : {shape_t::READ, shape_t::PUBSUB, shape_t::WRITE}) {
            for (std::size_t T : kThreads) {
                if (T > hw || T > kMaxThreads) continue;
                run_all_arms(shape, T);
            }
        }
    }
    if (only.empty() || only == "graph") {
        for (std::size_t subs : {std::size_t{0}, std::size_t{1}}) {
            for (topo_t topo : {topo_t::HOT1, topo_t::STRIPE1, topo_t::SPREAD}) {
                for (std::size_t T : kThreads) {
                    if (T > hw) continue;
                    run_graph(topo, T, subs, op_t::WRITE);
                }
            }
        }
        // The read arm takes no `subs` sweep: a read delivers to nobody, so fan-out is not a
        // dimension of it. HOT1 is the shape that matters — T readers on ONE shared LKV.
        for (topo_t topo : {topo_t::HOT1, topo_t::STRIPE1, topo_t::SPREAD}) {
            for (std::size_t T : kThreads) {
                if (T > hw) continue;
                run_graph(topo, T, /*subs=*/0, op_t::READ);
            }
        }
    }
    return 0;
}
