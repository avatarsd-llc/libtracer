/**
 * @file
 * @brief The ONE node composition the ADR-0079 store sweep measures, shared by the timed
 *        harness (`bench_store_sweep.cpp`) and the process-heap escape census
 *        (`bench_store_escape.cpp`).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ADR-0079 §Verification commissions "a three-configuration sweep (WIDE / MID / NARROW) over
 * the same workload". *The same workload* is the load-bearing word, so the node builder lives
 * here rather than being written twice: the escape census and the latency/throughput harness
 * must be composing the same object, or their columns describe two different nodes and the
 * memory figure cannot be read beside the latency figure.
 *
 * @section arms The four arms, and why the baseline is one of them
 *
 * | arm | graph `mr` | graph `ctl` | router `label_src`/`rx` | transport egress |
 * | --- | --- | --- | --- | --- |
 * | `H-baseline` | `std::pmr::get_default_resource()` | `heap_source()` | `heap_source()` |
 * `heap_source()` | | `WIDE`   | one slab (via the pmr adapter) | the one slab | the one slab | the
 * one slab | | `MID`    | graph store | graph store | net store | net store | | `NARROW` | graph
 * store | graph store | **per-child** store | **per-lane** store |
 *
 * `H-baseline` is what ships today. Without it the sweep can rank three injected
 * compositions against each other but cannot say whether any of them beats the default, which
 * is the only question a deployer actually asks.
 *
 * Total slab BYTES are held equal across WIDE / MID / NARROW — the rule
 * `bench_rx_source_topology` already follows, for the same reason: a sweep whose arms hold
 * different budgets measures budget, not topology. @ref bench_store::budget_t derives all
 * three splits from one total.
 *
 * @section limits WHAT THIS SWEEP CANNOT VARY — read this before quoting any figure
 *
 * `vertex_t` placement is STILL `std::make_unique<vertex_t>` on the global heap
 * (`core/src/graph.cpp`, the `ensure_vertex` / `register_vertex_key_span` bodies). That is
 * ADR-0079 **Stage 2** (#843, gated on #1285) and it has not landed. So the graph half of the
 * composition this file varies is **descent stacks and the `std::pmr` control-block channel
 * only**, not vertex placement:
 *
 *   * `ctl` is drawn by the composed-subtree READ's `mem::block_array_t<work_t>` collect stack
 *     and by the branch-decode overflow leg's `bump_source_t` fallback.
 *   * `mr` is drawn by the WRITE's `vertex_t::store` — the LKV control block plus the rope
 *     wrapper `allocate_shared` mints.
 *
 * A consequence that must be stated wherever the memory column is: the process-heap escape is
 * NOT zero in any arm, and cannot be until #843 lands. That number is the point, not an
 * embarrassment — it is what "bounded node" currently delivers.
 *
 * @section held Held constant in every arm, deliberately
 *
 * `mem::mem_backend_t` / the graph's `value_backend` stays `heap_backend()`. ADR-0079 keeps
 * need C (payload segments — refcounts, DMA hooks) separate from the block source on purpose;
 * varying it here would make the sweep measure two substrates at once. The composed read's
 * per-level POINT headers draw from THAT backend, so they are a constant term in every arm.
 *
 * @see bench_rx_source_topology.cpp — the single-seam ancestor of this file. It sweeps the
 *      router's `rx` seam alone, with no rounds, no carried null and no memory column; this
 *      harness composes the whole node and adds all three. The lane/rope machinery below is
 *      that bench's, reused rather than reinvented.
 */
#ifndef BENCH_STORE_SWEEP_NODE_HPP
#define BENCH_STORE_SWEEP_NODE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/mem_source.hpp"
#include "libtracer/mem_source_pmr.hpp"
#include "libtracer/mem_source_sync.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport.hpp"

/** @brief The ADR-0079 per-configuration store sweep: its node, its arms and its budget. */
namespace bench_store {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief The four allocation-store compositions this sweep rotates between. */
enum class arm_t {
    H_BASELINE, /**< @brief What ships: the process heap at every channel. The CONTROL. */
    WIDE,       /**< @brief One store behind every channel (ADR-0079's MCU fold of MID). */
    MID,        /**< @brief One store per PLANE — graph and net (ADR-0079's default). */
    NARROW,     /**< @brief Per-plane graph, per-child rx and per-link egress (the host fan). */
};

/** @brief Every arm, in report order — the control first, so a table reads against it. */
inline constexpr arm_t kArms[] = {arm_t::H_BASELINE, arm_t::WIDE, arm_t::MID, arm_t::NARROW};

/** @brief The arm's stable label — the key every RESULT row and every table column joins on. */
[[nodiscard]] inline const char* name_of(arm_t a) noexcept {
    switch (a) {
        case arm_t::H_BASELINE:
            return "H-baseline";
        case arm_t::WIDE:
            return "WIDE";
        case arm_t::MID:
            return "MID";
        case arm_t::NARROW:
            return "NARROW";
    }
    return "?";
}

/**
 * @brief Receive-thread counts the fan-out arm sweeps.
 *
 * Fixed rather than clamped to `hardware_concurrency`, exactly as `bench_rx_source_topology`
 * and `bench_route_handle_contention` do: ADR-0079 §Verification asks the sweep to "bracket
 * the thread counts that matter (single-thread MCU shape AND >= T where WIDE diverges)", and a
 * ladder that shrinks on a small runner brackets neither reproducibly.
 */
inline constexpr std::size_t kThreads[] = {1, 2, 4, 8, 16, 24};

/** @brief Rope link count per inbound frame — the iov width the forward hop must gather for. */
inline constexpr std::size_t kRopeLinks = 4;

/** @brief Registered children under each lane's subtree root — the composed read's node count. */
inline constexpr std::size_t kSubtreeChildren = 3;

/** @brief Payload bytes of the VALUE TLV each lane writes. */
inline constexpr std::size_t kValueBytes = 32;

/**
 * @brief The slab budget, split three ways so WIDE / MID / NARROW hold the SAME total.
 *
 * Sized generously on purpose: every mode faults on `overflowed() != 0` and the `hwm` mode
 * reports the peak `used()` each store actually needed, so an over-sized slab is reported as
 * slack rather than silently distorting a number, while an under-sized one would put every arm
 * on a degraded path. The figures a deployer sizes against are the reported ones, never these.
 */
struct budget_t {
    /** @brief Bytes for the graph plane's store (`ctl` + the `mr` adapter). */
    static constexpr std::size_t kGraph = 32U * 1024U;
    /** @brief Bytes for ONE lane's rx store, and separately for one lane's egress store. */
    static constexpr std::size_t kNetPerLane = 4U * 1024U;
    /** @brief Free-list class slots on a store several channels share. */
    static constexpr std::size_t kSharedClasses = 64;
    /** @brief Free-list class slots on a per-lane store. */
    static constexpr std::size_t kLaneClasses = 16;

    /** @brief The total every non-baseline arm is given, whatever its topology. */
    [[nodiscard]] static constexpr std::size_t total(std::size_t lanes) noexcept {
        return kGraph + lanes * 2U * kNetPerLane;
    }
    /** @brief MID's net-plane store: everything that is not the graph's. */
    [[nodiscard]] static constexpr std::size_t net_shared(std::size_t lanes) noexcept {
        return lanes * 2U * kNetPerLane;
    }
};

// --- the counting decorator ---------------------------------------------------------------

/**
 * @brief A `block_source_t` that COUNTS what it serves, optionally over an inner store.
 *
 * Two jobs, and it is deliberately kept off the timed path for both. With @p inner it is the
 * seam-reachability instrument: a channel wired to a store nothing draws from is an arm that
 * silently measures nothing, which is the single most likely way this harness ships vacuous.
 * With no inner it is a standalone counting store for the disjointness canary.
 *
 * @note SERVES FROM `std::aligned_alloc`, NEVER `::operator new`, when it has no inner store —
 *       exactly as `bench_failable_census.cpp`'s `counting_source_t` does, and for the same
 *       reason. `bench_store_escape.cpp` overrides the global `operator new` to count what
 *       ESCAPES to the process heap; a decorator that forwarded to `heap_source()` would land
 *       in BOTH columns and the two would stop being disjoint. That was a live defect, fixed
 *       in #1402 and canaried in #1421 — do not reintroduce it.
 *
 * @note The counters are atomic because the fan-out arm's lanes share a store in WIDE and MID.
 *       That is precisely why no timed mode wires one: a relaxed RMW per allocation is small,
 *       but it is not nothing, and it would land on the arms unequally.
 */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    /** @brief Count every request; serve from @p inner, or from `std::aligned_alloc` if null. */
    explicit counting_source_t(tr::mem::block_source_t* inner = nullptr) noexcept
        : tr::mem::block_source_t("count"), inner_(inner) {}

    /** @brief Serve and count; a refusal is counted and propagated, never papered over. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        void* p = nullptr;
        if (inner_ != nullptr) {
            p = inner_->try_alloc(bytes, align);
        } else {
            // `aligned_alloc` wants a size that is a multiple of the alignment.
            const std::size_t rounded = (bytes + align - 1U) & ~(align - 1U);
            p = std::aligned_alloc(align, rounded != 0 ? rounded : align);
        }
        if (p == nullptr) {
            refusals.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        blocks.fetch_add(1, std::memory_order_relaxed);
        bytes_served.fetch_add(bytes, std::memory_order_relaxed);
        const long long now =
            live.fetch_add(static_cast<long long>(bytes), std::memory_order_relaxed) +
            static_cast<long long>(bytes);
        long long seen = peak.load(std::memory_order_relaxed);
        while (now > seen && !peak.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
        }
        return p;
    }

    /** @brief Return a block to the inner store, or to `free`. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        if (p == nullptr) return;
        live.fetch_sub(static_cast<long long>(bytes), std::memory_order_relaxed);
        if (inner_ != nullptr) {
            inner_->release(p, bytes, align);
        } else {
            (void)align;
            std::free(p);
        }
    }

    std::atomic<std::size_t> blocks{0};       /**< @brief Blocks served. */
    std::atomic<std::size_t> bytes_served{0}; /**< @brief Their total requested size. */
    std::atomic<std::size_t> refusals{0};     /**< @brief Requests the store could not meet. */
    std::atomic<long long> live{0};           /**< @brief Bytes currently outstanding. */
    std::atomic<long long> peak{0};           /**< @brief High-water of @ref live. */

   private:
    tr::mem::block_source_t* inner_; /**< @brief Borrowed; null means serve from the C heap. */
};

// --- wire builders (canonical bytes via the production emit helpers) -----------------------

/** @brief A NAME record over @p s. */
[[nodiscard]] inline std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, s);
    return out;
}

/** @brief A PATH TLV over @p segs. */
[[nodiscard]] inline std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) {
        (void)tr::wire::emit_path_segment(body, s);
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief A FWD TLV carrying @p op with @p dst and @p src. */
[[nodiscard]] inline std::vector<std::byte> b_fwd(fwd_op_t op, const std::vector<std::byte>& dst,
                                                  const std::vector<std::byte>& src) {
    const std::byte ob{static_cast<std::uint8_t>(op)};
    std::vector<std::byte> ov;
    tr::wire::emit_tlv(ov, type_t::VALUE, opt_t{}, std::span<const std::byte>(&ob, 1));

    std::vector<std::byte> body;
    body.insert(body.end(), ov.begin(), ov.end());
    body.insert(body.end(), dst.begin(), dst.end());
    body.insert(body.end(), src.begin(), src.end());

    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

/** @brief A VALUE TLV of @p payload bytes — the value a lane writes into the graph. */
[[nodiscard]] inline std::vector<std::byte> b_value(std::size_t payload) {
    const std::vector<std::byte> p(payload, std::byte{0xAB});
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}

/** @brief One heap-backed rope link over @p bytes (a genuine scatter-gather piece). */
[[nodiscard]] inline tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

/** @brief Split @p bytes into @p links roughly-equal rope links, each its own segment. */
[[nodiscard]] inline tr::view::rope_t rope_of(std::span<const std::byte> bytes, std::size_t links) {
    tr::view::rope_t r;
    const std::size_t step = bytes.size() / links;
    std::size_t at = 0;
    for (std::size_t i = 0; i + 1 < links && step > 0; ++i) {
        r.append(make_value(bytes.subspan(at, step)));
        at += step;
    }
    r.append(make_value(bytes.subspan(at)));
    return r;
}

// --- links ---------------------------------------------------------------------------------

/** @brief The inbound link: hands the frame up as the rope it already is (ADR-0053 §5). */
class rope_in_t : public transport_t {
   public:
    /** @brief Inbound-only; nothing is ever sent on this link. */
    void send(std::span<const std::byte>) override {}
    /** @brief Rope delivery, so the forward hop gathers rather than flattening. */
    [[nodiscard]] bool delivers_ropes() const override { return true; }
    /** @brief Push one frame up as if the wire had just produced it. */
    void inject(tr::view::rope_t frame) { rx_.deliver_rope(std::move(frame)); }
};

/**
 * @brief The egress sink: counts frames and drops them.
 *
 * @note IT DELIBERATELY DOES NOT OVERRIDE THE GATHER FORM. `bench_rx_source_topology`'s sink
 *       overrides `send(iov)` and therefore never touches the egress store at all — which is
 *       fine there, because that bench sweeps the rx seam. Here the egress store is one of the
 *       four channels under test, so the gather must fall through to
 *       `transport_t::send(std::span<const std::span<const std::byte>>)`'s base body, whose
 *       `mem::block_array_t<std::byte> tmp(egress_source())` is the draw. Adding a gather
 *       override to this class would silently zero one column of the sweep; the `calibrate`
 *       mode's per-channel canary is what stops that happening unnoticed.
 *
 * Like its ancestor it does not RETAIN the bytes: a per-thread vector growing for a whole
 * timed window would put the general allocator back into the measurement.
 */
class sink_out_t : public transport_t {
   public:
    /** @brief Absorb one gathered frame. */
    void send(std::span<const std::byte> frame) override {
        bytes += frame.size();
        ++frames;
    }
    std::size_t frames = 0; /**< @brief Frames this link was handed. */
    std::size_t bytes = 0;  /**< @brief Their total size, kept so the gather is not elided. */
};

// --- the stores -----------------------------------------------------------------------------

/**
 * @brief Every slab, pool and adapter one arm needs, wired for one lane count.
 *
 * Built in FULL before the escape census arms its `operator new` counter, and deliberately so:
 * the injected slab is the DEPLOYER's budget, declared up front, and counting it as node heap
 * would report a bounded node as the most expensive one on the page. What the escape column
 * counts is what the node reaches for BEYOND its slab.
 */
struct stores_t {
    std::vector<std::byte> wide_slab;                 /**< @brief WIDE's single store. */
    std::vector<std::byte> graph_slab;                /**< @brief MID/NARROW's graph-plane store. */
    std::vector<std::byte> net_slab;                  /**< @brief MID's net-plane store. */
    std::vector<std::vector<std::byte>> lane_rx_slab; /**< @brief NARROW's per-child rx stores. */
    std::vector<std::vector<std::byte>> lane_tx_slab; /**< @brief NARROW's per-link egress. */

    std::vector<tr::mem::size_class_t> wide_cls;  /**< @brief Free-list slots for @ref wide. */
    std::vector<tr::mem::size_class_t> graph_cls; /**< @brief Free-list slots for @ref graph. */
    std::vector<tr::mem::size_class_t> net_cls;   /**< @brief Free-list slots for @ref net. */
    std::vector<std::vector<tr::mem::size_class_t>> lane_rx_cls; /**< @brief Per-lane rx slots. */
    std::vector<std::vector<tr::mem::size_class_t>> lane_tx_cls; /**< @brief Per-lane tx slots. */

    /** @brief WIDE's store. Locking: every lane's receive thread touches it. */
    std::unique_ptr<tr::mem::pool_source_t<tr::mem::sync_mutex_t>> wide;
    /** @brief The graph-plane store, shared by `ctl` and the `mr` adapter in MID and NARROW. */
    std::unique_ptr<tr::mem::pool_source_t<tr::mem::sync_mutex_t>> graph;
    /** @brief MID's net-plane store, shared by every child's rx and every link's egress. */
    std::unique_ptr<tr::mem::pool_source_t<tr::mem::sync_mutex_t>> net;
    /** @brief NARROW's per-child rx stores — one thread each, so `sync_none_t`. */
    std::vector<std::unique_ptr<tr::mem::pool_source_t<tr::mem::sync_none_t>>> lane_rx;
    /** @brief NARROW's per-link egress stores — one sending thread each, so `sync_none_t`. */
    std::vector<std::unique_ptr<tr::mem::pool_source_t<tr::mem::sync_none_t>>> lane_tx;

    /** @brief The `std::pmr` face of whichever store the graph's `mr` channel draws from. */
    std::unique_ptr<tr::mem::source_resource_t> mr_adapter;

    /** @brief Optional counting decorators, one per channel — `hwm` and `calibrate` only. */
    std::unique_ptr<counting_source_t> count_ctl;
    std::unique_ptr<counting_source_t> count_mr;  /**< @brief Wraps the `mr` channel's store. */
    std::unique_ptr<counting_source_t> count_rx;  /**< @brief Wraps the router's DEFAULT rx. */
    std::unique_ptr<counting_source_t> count_egr; /**< @brief Wraps the shared egress store. */
    std::vector<std::unique_ptr<counting_source_t>> count_lane_rx; /**< @brief NARROW per-child. */
    std::vector<std::unique_ptr<counting_source_t>> count_lane_tx; /**< @brief NARROW per-link. */

    /** @brief Every pool this arm owns, for the fault sweep and the HWM report. */
    struct pool_view_t {
        const char* label = "";   /**< @brief Store name as the tables print it. */
        std::size_t used = 0;     /**< @brief Bytes carved (`pool_source_t::used`). */
        std::size_t capacity = 0; /**< @brief Slab bytes it was given. */
        std::size_t classes = 0;  /**< @brief `pool_source_t::classes_used`. */
        std::size_t overflow = 0; /**< @brief `pool_source_t::overflowed` — must be zero. */
    };

    /** @brief Snapshot every live pool's occupancy — the deterministic memory column. */
    [[nodiscard]] std::vector<pool_view_t> occupancy() const {
        std::vector<pool_view_t> out;
        const auto add = [&out](const char* label, const auto& p, std::size_t cap) {
            if (p) out.push_back({label, p->used(), cap, p->classes_used(), p->overflowed()});
        };
        add("wide", wide, wide_slab.size());
        add("graph", graph, graph_slab.size());
        add("net", net, net_slab.size());
        for (std::size_t i = 0; i < lane_rx.size(); ++i) {
            add("lane-rx", lane_rx[i], lane_rx_slab[i].size());
        }
        for (std::size_t i = 0; i < lane_tx.size(); ++i) {
            add("lane-tx", lane_tx[i], lane_tx_slab[i].size());
        }
        return out;
    }
};

/** @brief One receive thread's private wiring: its links, its frame and its two vertices. */
struct lane_t {
    rope_in_t in;                 /**< @brief The inbound link the rope arrives on. */
    sink_out_t out;               /**< @brief The outbound link the hop forwards to. */
    tr::view::rope_t frame;       /**< @brief The pre-built inbound frame, re-injected per op. */
    std::vector<std::byte> value; /**< @brief Stable bytes the graph write borrows. */
    std::optional<vertex_handle_t> root; /**< @brief The composed-read subtree root. */
    std::optional<vertex_handle_t> leaf; /**< @brief The vertex the write lands on. */
};

/**
 * @brief The whole node under one arm: stores, graph, router and @p lanes lanes.
 *
 * Declaration order IS destruction order reversed, and it matters: the graph holds `std::pmr`
 * containers built over @ref stores_t::mr_adapter, so the graph must die before the stores.
 */
class node_t {
   public:
    /**
     * @brief Compose the node for @p arm with @p lanes lanes.
     *
     * @param count_channels Wrap each channel in a @ref counting_source_t. Off in every timed
     *        mode — the decorator's atomics are cheap but not free, and they would land on the
     *        arms unequally. On in `hwm` and `calibrate`, which are untimed by construction.
     * @param after_stores Called with @p ctx once every slab and pool exists but before the
     *        graph, the router or any lane does. It is the seam `bench_store_escape.cpp` arms
     *        its process-heap counter on, so that an arm's own DECLARED SLAB — the deployer's
     *        budget, stated up front — is not counted as node heap. Without it the most
     *        bounded arm would report the largest escape on the page, which is exactly
     *        backwards. Null in every other caller.
     * @param ctx Opaque context handed back to @p after_stores.
     */
    node_t(arm_t arm, std::size_t lanes, bool count_channels, void (*after_stores)(void*) = nullptr,
           void* ctx = nullptr)
        : arm_(arm), lanes_n_(lanes) {
        build_stores(count_channels);
        if (after_stores != nullptr) after_stores(ctx);
        build_graph();
        build_lanes();
    }

    node_t(const node_t&) = delete;
    node_t& operator=(const node_t&) = delete;

    /** @brief The arm this node was composed for. */
    [[nodiscard]] arm_t arm() const noexcept { return arm_; }
    /** @brief The stores, for the occupancy and canary reports. */
    [[nodiscard]] const stores_t& stores() const noexcept { return st_; }
    /** @brief The stores, mutably — the canaries read their counters. */
    [[nodiscard]] stores_t& stores() noexcept { return st_; }
    /** @brief Lane @p i's private wiring. */
    [[nodiscard]] lane_t& lane(std::size_t i) noexcept { return *lanes_[i]; }
    /** @brief Lane count. */
    [[nodiscard]] std::size_t lanes() const noexcept { return lanes_n_; }

    /**
     * @brief LEG 1 — the rope FWD forward hop. Draws the router's `rx` AND the link's egress.
     *
     * A `kRopeLinks`-link rope arrives on child `c<i>` naming child `u<i>`; the hop gathers the
     * untouched links onward. The rx draw is the terminus arena; the egress draw is the base
     * `send(iov)` gather temporary, reachable only because @ref sink_out_t declines to override
     * the gather form.
     */
    void step_net(lane_t& l) { l.in.inject(l.frame); }

    /**
     * @brief LEG 2 — one graph WRITE. Draws the `std::pmr` `mr` channel.
     *
     * `vertex_t::store` mints the LKV control block and the rope wrapper through `mr_`
     * (`core/src/graph.cpp`'s `store_value`). The value is a BORROWED view over the lane's own
     * stable buffer, so the `value_backend` — held constant at `heap_backend()` in every arm —
     * contributes nothing here and the `mr` draw is isolated.
     */
    void step_write(lane_t& l) {
        (void)g_->write(*l.leaf, tr::view::view_t::over(tr::view::borrow_const(l.value)));
    }

    /**
     * @brief LEG 3 — one composed subtree READ. Draws the graph's `ctl` channel.
     *
     * `read_subtree_folded` walks the subtree on a `mem::block_array_t<work_t>` collect stack
     * over `ctl_` (the #981 migration), so a root with @ref kSubtreeChildren registered children
     * draws at least one block per call. This is the ONLY leg that reaches `ctl` — see the file
     * header on what the graph arm therefore does and does not sweep.
     */
    void step_read(lane_t& l) { (void)g_->read_subtree_folded(*l.root); }

    /** @brief The whole per-lane workload: all three legs, in the order a node runs them. */
    void step_full(lane_t& l) {
        step_net(l);
        step_write(l);
        step_read(l);
    }

    /**
     * @brief Report any lane that forwarded nothing, or any pool that overflowed.
     *
     * FAULT, never report: a pool that overflowed stopped recycling and started counting, and a
     * lane that forwarded nothing timed a miss. Either way the point describes something other
     * than the workload, so it must not be published — the rule
     * `bench_rx_source_topology::run_point` already enforces at its seam.
     *
     * @return true when the point is trustworthy.
     */
    [[nodiscard]] bool faults(const char* what, std::size_t T) const {
        bool ok = true;
        for (std::size_t i = 0; i < lanes_n_; ++i) {
            if (lanes_[i]->out.frames == 0) {
                std::fprintf(stderr, "FAULT %s %s T=%zu: lane %zu forwarded nothing\n",
                             name_of(arm_), what, T, i);
                ok = false;
            }
        }
        for (const stores_t::pool_view_t& p : st_.occupancy()) {
            if (p.overflow != 0) {
                std::fprintf(stderr, "FAULT %s %s T=%zu: store %s overflowed %zu\n", name_of(arm_),
                             what, T, p.label, p.overflow);
                ok = false;
            }
            if (p.used > p.capacity - p.capacity / 8U) {
                std::fprintf(stderr,
                             "FAULT %s %s T=%zu: store %s carved %zu/%zu — the slab is not a "
                             "ceiling any more\n",
                             name_of(arm_), what, T, p.label, p.used, p.capacity);
                ok = false;
            }
        }
        for (const counting_source_t* c : counted()) {
            if (c->refusals.load() != 0) {
                std::fprintf(stderr, "FAULT %s %s T=%zu: a store refused %zu request(s)\n",
                             name_of(arm_), what, T, c->refusals.load());
                ok = false;
            }
        }
        return ok;
    }

    /** @brief Every counting decorator in play, in a stable order (empty when uncounted). */
    [[nodiscard]] std::vector<const counting_source_t*> counted() const {
        std::vector<const counting_source_t*> out;
        const auto add = [&out](const auto& p) {
            if (p) out.push_back(p.get());
        };
        add(st_.count_ctl);
        add(st_.count_mr);
        add(st_.count_rx);
        add(st_.count_egr);
        for (const auto& p : st_.count_lane_rx) add(p);
        for (const auto& p : st_.count_lane_tx) add(p);
        return out;
    }

   private:
    /** @brief Carve every slab and stand up every pool, plus the pmr adapter over `mr`'s store. */
    void build_stores(bool count_channels) {
        const std::size_t T = lanes_n_;
        if (arm_ == arm_t::WIDE) {
            st_.wide_slab.resize(budget_t::total(T));
            st_.wide_cls.resize(budget_t::kSharedClasses);
            st_.wide = std::make_unique<tr::mem::pool_source_t<tr::mem::sync_mutex_t>>(
                st_.wide_slab, st_.wide_cls);
        } else if (arm_ == arm_t::MID || arm_ == arm_t::NARROW) {
            st_.graph_slab.resize(budget_t::kGraph);
            st_.graph_cls.resize(budget_t::kSharedClasses);
            st_.graph = std::make_unique<tr::mem::pool_source_t<tr::mem::sync_mutex_t>>(
                st_.graph_slab, st_.graph_cls);
        }
        if (arm_ == arm_t::MID) {
            st_.net_slab.resize(budget_t::net_shared(T));
            st_.net_cls.resize(budget_t::kSharedClasses);
            st_.net = std::make_unique<tr::mem::pool_source_t<tr::mem::sync_mutex_t>>(st_.net_slab,
                                                                                      st_.net_cls);
        }
        if (arm_ == arm_t::NARROW) {
            st_.lane_rx_slab.resize(T);
            st_.lane_tx_slab.resize(T);
            st_.lane_rx_cls.resize(T);
            st_.lane_tx_cls.resize(T);
            st_.lane_rx.reserve(T);
            st_.lane_tx.reserve(T);
            for (std::size_t i = 0; i < T; ++i) {
                st_.lane_rx_slab[i].resize(budget_t::kNetPerLane);
                st_.lane_tx_slab[i].resize(budget_t::kNetPerLane);
                st_.lane_rx_cls[i].resize(budget_t::kLaneClasses);
                st_.lane_tx_cls[i].resize(budget_t::kLaneClasses);
                st_.lane_rx.push_back(
                    std::make_unique<tr::mem::pool_source_t<tr::mem::sync_none_t>>(
                        st_.lane_rx_slab[i], st_.lane_rx_cls[i]));
                st_.lane_tx.push_back(
                    std::make_unique<tr::mem::pool_source_t<tr::mem::sync_none_t>>(
                        st_.lane_tx_slab[i], st_.lane_tx_cls[i]));
            }
        }

        // Resolve each of the four channels to the store its arm points it at.
        tr::mem::block_source_t* const heap = &tr::mem::heap_source();
        switch (arm_) {
            case arm_t::H_BASELINE:
                ctl_src_ = heap;
                mr_src_ = heap;
                rx_src_ = heap;
                egr_src_ = heap;
                break;
            case arm_t::WIDE:
                ctl_src_ = mr_src_ = rx_src_ = egr_src_ = st_.wide.get();
                break;
            case arm_t::MID:
                ctl_src_ = mr_src_ = st_.graph.get();
                rx_src_ = egr_src_ = st_.net.get();
                break;
            case arm_t::NARROW:
                ctl_src_ = mr_src_ = st_.graph.get();
                rx_src_ = egr_src_ = nullptr;  // per-lane; see build_lanes
                break;
        }
        // The undecorated store a NARROW node's node-wide wiring (the router's label table)
        // falls back to, so no arm leaves an allocation on the process heap by accident.
        plane_fallback_ = arm_ == arm_t::H_BASELINE ? heap : mr_src_;

        if (count_channels) {
            st_.count_ctl = std::make_unique<counting_source_t>(ctl_src_);
            st_.count_mr = std::make_unique<counting_source_t>(mr_src_);
            ctl_src_ = st_.count_ctl.get();
            mr_src_ = st_.count_mr.get();
            if (rx_src_ != nullptr) {
                st_.count_rx = std::make_unique<counting_source_t>(rx_src_);
                st_.count_egr = std::make_unique<counting_source_t>(egr_src_);
                rx_src_ = st_.count_rx.get();
                egr_src_ = st_.count_egr.get();
            }
        }

        // The `mr` channel is a `std::pmr` seam, so it reaches its store through #1401's
        // adapter. The baseline is the ONE arm whose `mr` normally stays on the process-default
        // resource, which is exactly what makes it the control. It is routed through the
        // adapter ONLY when counting, so the census can say how many blocks the channel draws
        // at all — a reference the injected arms' counts are read against. The consequence is
        // disclosed rather than hidden: the baseline's `hwm` / `chan` rows describe a very
        // slightly different composition from its TIMED rows, which never see the adapter.
        if (arm_ != arm_t::H_BASELINE || count_channels) {
            st_.mr_adapter = std::make_unique<tr::mem::source_resource_t>(*mr_src_);
        }
    }

    /** @brief Stand up the graph on the resolved `mr` / `ctl` channels, then the router. */
    void build_graph() {
        std::pmr::memory_resource* const mr =
            st_.mr_adapter ? static_cast<std::pmr::memory_resource*>(st_.mr_adapter.get())
                           : std::pmr::get_default_resource();
        g_ = std::make_unique<graph_t>(mr, &tr::mem::heap_backend(), ctl_src_);
        // `label_src` and the DEFAULT `rx` both take the net-plane store, per ADR-0079's
        // composition table. NARROW has no node-wide net store — every child overrides `rx`
        // through `add_child` — so its label table takes the GRAPH store rather than the heap:
        // a NARROW node that fell back to `heap_source()` for its label bindings would not be
        // the bounded composition the arm is supposed to represent. Deliberately the
        // UNDECORATED store, so wiring-frequency label allocations do not land in the `ctl`
        // channel's per-frame draw count.
        tr::mem::block_source_t* const label_src = rx_src_ != nullptr ? rx_src_ : plane_fallback_;
        tr::mem::block_source_t* const rx_default = label_src;
        router_ = std::make_unique<fwd_router_t>(*g_, label_src, rx_default);
    }

    /** @brief Build each lane's links, frame, vertices and — in NARROW — its own two stores. */
    void build_lanes() {
        lanes_.reserve(lanes_n_);
        for (std::size_t i = 0; i < lanes_n_; ++i) {
            auto l = std::make_unique<lane_t>();
            const std::string in_name = "c" + std::to_string(i);
            const std::string out_name = "u" + std::to_string(i);

            const std::vector<std::byte> bytes =
                b_fwd(fwd_op_t::READ, b_path({out_name, "sensor"}), b_path({"reply-ep"}));
            l->frame = rope_of(bytes, kRopeLinks);
            l->value = b_value(kValueBytes);

            tr::mem::block_source_t* rx = rx_src_;  // null in NARROW => this lane's own
            tr::mem::block_source_t* egr = egr_src_;
            if (arm_ == arm_t::NARROW) {
                rx = st_.lane_rx[i].get();
                egr = st_.lane_tx[i].get();
                if (st_.count_ctl) {  // counting is on: decorate the per-lane stores too
                    st_.count_lane_rx.push_back(std::make_unique<counting_source_t>(rx));
                    st_.count_lane_tx.push_back(std::make_unique<counting_source_t>(egr));
                    rx = st_.count_lane_rx.back().get();
                    egr = st_.count_lane_tx.back().get();
                }
            }
            if (egr != nullptr) l->out.set_egress_source(*egr);

            if (!router_->add_child(in_name, l->in, rx) ||
                !router_->add_child(out_name, l->out, rx)) {
                std::fprintf(stderr, "FAULT %s: add_child failed for lane %zu\n", name_of(arm_), i);
                std::abort();
            }

            // The lane's subtree: a root with `kSubtreeChildren` registered children, so the
            // composed read has a stack to grow and the write has a leaf to land on.
            const std::string base = "/bench/w" + std::to_string(i);
            l->root = g_->register_vertex(*path_t::parse(base), role_t::STORED_VALUE);
            for (std::size_t c = 0; c < kSubtreeChildren; ++c) {
                const vertex_handle_t vh = g_->register_vertex(
                    *path_t::parse(base + "/s" + std::to_string(c)), role_t::STORED_VALUE);
                if (c == 0) l->leaf = vh;
            }
            lanes_.push_back(std::move(l));
        }

        // One full pass per lane BEFORE any timed window: the first forward through a child
        // carves its slab and warms every lazily-built table, which would otherwise land
        // entirely in T=1's first microseconds and nowhere else.
        for (auto& l : lanes_) step_full(*l);
    }

    arm_t arm_;           /**< @brief The composition under test. */
    std::size_t lanes_n_; /**< @brief Lane (and receive-thread) count. */
    stores_t st_;         /**< @brief Declared FIRST so it outlives the graph. */
    tr::mem::block_source_t* ctl_src_ = nullptr; /**< @brief Resolved graph `ctl` channel. */
    tr::mem::block_source_t* mr_src_ = nullptr;  /**< @brief Resolved graph `mr` channel. */
    tr::mem::block_source_t* rx_src_ = nullptr;  /**< @brief Resolved router `rx`; null=per-lane. */
    tr::mem::block_source_t* egr_src_ = nullptr; /**< @brief Resolved egress; null=per-lane. */
    tr::mem::block_source_t* plane_fallback_ = nullptr; /**< @brief Node-wide wiring fallback. */
    std::unique_ptr<graph_t> g_;                        /**< @brief The graph under test. */
    std::unique_ptr<fwd_router_t> router_;              /**< @brief The FWD router under test. */
    std::vector<std::unique_ptr<lane_t>> lanes_; /**< @brief Stable addresses: the router holds
                                                      pointers into these. */
};

}  // namespace bench_store

#endif  // BENCH_STORE_SWEEP_NODE_HPP
