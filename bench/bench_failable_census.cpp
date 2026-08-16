/**
 * @file
 * @brief Costs the ADR-0065 `block_source_t` migration: (A) how many BLOCKS each peer-driven
 *        control-plane operation allocates and through which seam, and (B) what the two
 *        candidate guard shapes cost in ns.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Two modes, because the migration has two costs and they are measured differently.
 *
 * `blocks` — a DETERMINISTIC census (counts, not times). A counting `std::pmr::memory_resource`
 * plus this TU's global operator-new override separate the blocks a route-handle operation draws
 * from the injected resource (`mr_served`) from those that ESCAPE to the global heap. Every one
 * of the pmr-served blocks is a throwing allocation on a peer-driven path: on the shipping
 * `-fno-exceptions` profile that is an `abort()`, which is what ADR-0065 exists to remove.
 * The operations mirror `fwd_router_t::on_advertise` / `on_nack`'s calls one for one.
 *
 * `grow` — an INTERLEAVED A/B of the two guard shapes for a growable array of trivially-copyable
 * elements, which is what the 29 `detail::try_reserve` / `try_push_back` sites and the ADR-0065
 * destination respectively are:
 *   - `base`  `std::vector<T>` + `tr::detail::try_push_back` — today. Growth is the vector's own
 *             (throwing) reserve with its failure caught: ONE allocator round trip per growth on
 *             a hosted build. Under `-fno-exceptions` the helper cannot catch, so it still
 *             probes the GLOBAL heap with a throwaway `operator new` + `operator delete` first
 *             and that arm costs THREE round trips (#923).
 *   - `cand`  `tr::mem::block_array_t<T>` over an injected `block_source_t` — ADR-0065. ONE
 *             `try_alloc` (a virtual call) per growth, `memcpy` relocation, no probe.
 *   - `ctrl`  a raw `std::vector<T>::push_back` with no guard at all — the unguarded floor, a
 *             control arm that must not move between conditions.
 * Rep-interleaved (base, cand, ctrl, base, ...) so a thermal/frequency drift hits all three arms;
 * every rep's value is printed so a reader can run the overlap check, not just the median.
 *
 * `guard` — a GATE, not a measurement (#848). For each registered peer-reachable egress
 * operation it runs the operation once with the allocator counting and LOGGING every request
 * size, dedupes those sizes, and then re-runs the operation once per distinct observed size
 * `s` with the allocator refusing every request larger than `s - 1`. It counts how many of
 * those runs escaped as a `std::bad_alloc`. An escape is a mechanical proof that a request of
 * that size on that path is unguarded — no label, no hand-written note, no human judgement.
 * The injection threshold is derived from what the operation was OBSERVED to ask for, never
 * hand-chosen; see "Why a size threshold and not 'fail the k-th allocation'" on
 * `g_max_alloc` below for why the index-based injector is the wrong instrument HERE (the
 * behavioural harness in `core/tests/transport_alloc_softfail_test.cpp` does use one, over
 * paths that hold no `try_reserve` probe). `main` returns non-zero if any arm's expectation
 * is violated, so this mode is runnable as a CI gate rather than a bench somebody reads.
 *
 * The instrument is kept honest by a deliberately-UNGUARDED control arm (the retained
 * throwing `ws::encode_frame`), which the gate requires to escape: if the injector ever
 * breaks, stops arming, or the compiler elides the allocation, that arm reports `escaped=0`
 * and the gate fails ON THE GOOD BUILD. That is a permanently-live mutant, unlike a one-off
 * "revert a site and confirm it reddens" ritual nobody repeats.
 *
 * Single-threaded by construction: all three modes measure per-operation allocation shape,
 * and the global counter is process-wide.
 */

#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory_resource>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/can.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/iov_table.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/ws.hpp"

// --- global operator-new counter (this TU owns the override) -----------------

namespace {

bool g_armed = false;
std::size_t g_allocs = 0;
std::size_t g_frees = 0;
std::size_t g_bytes = 0;
/**
 * @brief While armed, refuse any request LARGER than this — "the heap cannot serve a block
 *        this big". `SIZE_MAX` refuses nothing.
 *
 * The `guard` mode's injection point. It sits in the real `operator new` because that is
 * what an UNGUARDED `std::vector::reserve` actually calls — `tr::detail::probe_fail_hook`
 * is consulted only from inside the `try_*` seams, so a harness built on it would pass
 * vacuously at exactly the sites this gate exists to catch.
 *
 * @par Why a size threshold and not "fail the k-th allocation"
 * Under `-fno-exceptions` — the profile the gate speaks for — `tr::detail::try_reserve`
 * PROBES with a nothrow allocation of exactly n bytes, frees it, and then runs the throwing
 * `reserve(n)` that the just-freed block satisfies (a hosted build catches instead, #923).
 * An index-based injector refuses that second call even though a real heap that served the
 * probe would always serve it — and because the `try_*` seams are `noexcept`, the
 * fabricated throw `terminate`s the process instead of proving anything. A size threshold
 * cannot fabricate it: the probe and its `reserve` ask for the SAME number of bytes, so
 * they always agree. Thresholds are swept over the request sizes the operation was
 * OBSERVED to make, so nothing here is hand-chosen.
 */
std::size_t g_max_alloc = static_cast<std::size_t>(-1);
/** @brief The request sizes seen in the current armed window (for the threshold sweep). */
std::vector<std::size_t>* g_size_log = nullptr;

void* counted_alloc(std::size_t size) {
    if (g_armed) {
        ++g_allocs;
        g_bytes += size;
        if (g_size_log != nullptr) {
            std::vector<std::size_t>* log = g_size_log;
            g_size_log = nullptr;  // the log's own growth must not recurse into itself
            log->push_back(size);
            g_size_log = log;
        }
        if (size > g_max_alloc) return nullptr;
    }
    return std::malloc(size != 0 ? size : 1);
}

void counted_free(void* p) {
    if (p == nullptr) return;
    if (g_armed) ++g_frees;
    std::free(p);
}

}  // namespace

void* operator new(std::size_t size) {
    void* p = counted_alloc(size);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size) {
    void* p = counted_alloc(size);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return counted_alloc(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return counted_alloc(size);
}
void* operator new(std::size_t size, std::align_val_t) { return operator new(size); }
void* operator new(std::size_t size, std::align_val_t, const std::nothrow_t&) noexcept {
    return counted_alloc(size);
}
void* operator new[](std::size_t size, std::align_val_t) { return operator new(size); }
void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, std::size_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::size_t) noexcept { counted_free(p); }
void operator delete(void* p, std::align_val_t) noexcept { counted_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { counted_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { counted_free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { counted_free(p); }

namespace {

namespace ws = tr::net::ws;
namespace can = tr::net::can;

/** @brief A `std::pmr::memory_resource` that counts what it serves (blocks and bytes). */
class counting_resource_t final : public std::pmr::memory_resource {
   public:
    std::size_t allocs = 0; /**< @brief Blocks served since construction. */
    std::size_t bytes = 0;  /**< @brief Bytes served since construction. */

   private:
    void* do_allocate(std::size_t n, std::size_t align) override {
        ++allocs;
        bytes += n;
        return std::pmr::get_default_resource()->allocate(n, align);
    }
    void do_deallocate(void* p, std::size_t n, std::size_t align) override {
        std::pmr::get_default_resource()->deallocate(p, n, align);
    }
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override {
        return this == &o;
    }
};

/** @brief One census row: blocks through the injected seam and blocks that escaped to the heap. */
struct census_t {
    std::size_t mr_blocks = 0;
    std::size_t mr_bytes = 0;
    std::size_t heap_blocks = 0;
    std::size_t heap_bytes = 0;
};

void print_census(const char* op, const census_t& c, std::size_t n, const char* note) {
    std::printf(
        "RESULT failable blocks op=%s mr_blocks=%.2f heap_blocks=%.2f mr_bytes=%.1f "
        "heap_bytes=%.1f n=%zu note=%s\n",
        op, static_cast<double>(c.mr_blocks) / static_cast<double>(n),
        static_cast<double>(c.heap_blocks) / static_cast<double>(n),
        static_cast<double>(c.mr_bytes) / static_cast<double>(n),
        static_cast<double>(c.heap_bytes) / static_cast<double>(n), n, note);
}

/** @brief A NAME-only PATH TLV of @p segs segments — a plausible learned route. */
std::vector<std::byte> make_route(std::size_t segs) {
    std::vector<std::byte> body;
    for (std::size_t i = 0; i < segs; ++i) {
        char nb[24];
        std::snprintf(nb, sizeof nb, "seg%04zu", i);
        (void)tr::wire::emit_path_segment(body, std::string_view(nb));
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, tr::wire::type_t::PATH, tr::wire::opt_t{}, body);
    return out;
}

/**
 * @brief A link that DISCARDS every send, overriding BOTH forms so it allocates nothing.
 *
 * Overriding the gather form is the load-bearing half: `transport_t::send(iov)`'s base
 * implementation concatenates into a temporary, so a link that inherited it would charge the
 * router for a buffer the router did not ask for and make a NOALLOC arm unreadable. Sizes are
 * kept so an arm can prove the frame was really emitted rather than silently dropped.
 */
class null_link_t final : public tr::net::transport_t {
   public:
    void send(std::span<const std::byte> frame) override {
        last_ = frame.size();
        ++sends_;
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        last_ = 0;
        for (const std::span<const std::byte> s : iov) last_ += s.size();
        ++sends_;
    }
    /** @brief Total bytes of the last send. */
    [[nodiscard]] std::size_t last() const noexcept { return last_; }
    /** @brief Sends seen so far. */
    [[nodiscard]] std::size_t sends() const noexcept { return sends_; }

   private:
    std::size_t last_ = 0;  /**< @brief Bytes of the last send. */
    std::size_t sends_ = 0; /**< @brief Sends seen. */
};

/**
 * @brief The block census of the route-handle control path.
 *
 * Each armed window brackets exactly one peer-driven operation repeated @p n times, on a
 * fresh table set, so the per-op figure is an average over a clean state machine rather
 * than a steady-state one.
 */
int run_blocks() {
    constexpr std::size_t kN = 256;
    const std::vector<std::byte> route = make_route(4);

    // (1) FIRST bind on a NEW link — `fwd_router_t::on_advertise`'s terminus leg reaches this
    //     through `bind_ingress`, which calls `tables()`: the #603-defect-1 `allocate_shared`.
    {
        counting_resource_t mr;
        tr::net::route_handle_t rh{&mr};
        census_t c;
        const std::size_t mr0 = mr.allocs;
        const std::size_t mrb0 = mr.bytes;
        g_allocs = g_frees = g_bytes = 0;
        g_armed = true;
        for (std::size_t i = 0; i < kN; ++i) {
            char lb[32];
            std::snprintf(lb, sizeof lb, "link-%05zu", i);
            tr::net::handle_binding_t b;
            b.terminus = true;
            b.local_route.assign(route.begin(), route.end());
            (void)rh.bind_ingress(std::string_view(lb), 1, std::move(b));
        }
        g_armed = false;
        c.mr_blocks = mr.allocs - mr0;
        c.mr_bytes = mr.bytes - mrb0;
        c.heap_blocks = g_allocs;
        c.heap_bytes = g_bytes;
        print_census("bind_ingress_new_link", c, kN,
                     "first-touch:allocate_shared+pmr_string+map_node+entry_vector");
    }

    // (2) A further bind on an EXISTING link — the steady-state learn.
    {
        counting_resource_t mr;
        tr::net::route_handle_t rh{&mr};
        {
            tr::net::handle_binding_t b;
            b.terminus = true;
            (void)rh.bind_ingress("link-0", 0, std::move(b));
        }
        census_t c;
        const std::size_t mr0 = mr.allocs;
        const std::size_t mrb0 = mr.bytes;
        g_allocs = g_frees = g_bytes = 0;
        g_armed = true;
        for (std::size_t i = 0; i < kN; ++i) {
            tr::net::handle_binding_t b;
            b.terminus = true;
            b.local_route.assign(route.begin(), route.end());
            (void)rh.bind_ingress("link-0", static_cast<std::uint16_t>(i + 1), std::move(b));
        }
        g_armed = false;
        c.mr_blocks = mr.allocs - mr0;
        c.mr_bytes = mr.bytes - mrb0;
        c.heap_blocks = g_allocs;
        c.heap_bytes = g_bytes;
        print_census("bind_ingress_same_link", c, kN, "steady:entry_vector_growth+local_route");
    }

    // (3) `record_egress` — `on_advertise`'s forwarding leg, once per learned label.
    {
        counting_resource_t mr;
        tr::net::route_handle_t rh{&mr};
        (void)rh.record_egress("link-0", 1, route);
        census_t c;
        const std::size_t mr0 = mr.allocs;
        const std::size_t mrb0 = mr.bytes;
        g_allocs = g_frees = g_bytes = 0;
        g_armed = true;
        for (std::size_t i = 0; i < kN; ++i)
            (void)rh.record_egress("link-0", static_cast<std::uint16_t>(i + 2), route);
        g_armed = false;
        c.mr_blocks = mr.allocs - mr0;
        c.mr_bytes = mr.bytes - mrb0;
        c.heap_blocks = g_allocs;
        c.heap_bytes = g_bytes;
        print_census("record_egress", c, kN, "route_bytes_copy+egress_vector_growth");
    }

    // (4) `ensure_egress` — the DELIVERY path's label allocation (`deliver_remote`).
    {
        counting_resource_t mr;
        tr::net::route_handle_t rh{&mr};
        (void)rh.ensure_egress("link-0", route);
        std::vector<std::vector<std::byte>> routes;
        routes.reserve(kN);
        for (std::size_t i = 0; i < kN; ++i) routes.push_back(make_route(4 + (i % 3)));
        census_t c;
        const std::size_t mr0 = mr.allocs;
        const std::size_t mrb0 = mr.bytes;
        g_allocs = g_frees = g_bytes = 0;
        g_armed = true;
        for (const std::vector<std::byte>& r : routes)
            (void)rh.ensure_egress("link-0", std::span<const std::byte>(r));
        g_armed = false;
        c.mr_blocks = mr.allocs - mr0;
        c.mr_bytes = mr.bytes - mrb0;
        c.heap_blocks = g_allocs;
        c.heap_bytes = g_bytes;
        print_census("ensure_egress_new_route", c, kN, "linear_scan_miss+route_copy");
    }

    // (5) `egress_route` — `on_nack`'s owning read: a std::vector COPY out of the table,
    //     on the GLOBAL heap (the return type is a plain vector, not a pmr one).
    {
        counting_resource_t mr;
        tr::net::route_handle_t rh{&mr};
        for (std::size_t i = 0; i < 64; ++i)
            (void)rh.record_egress("link-0", static_cast<std::uint16_t>(i + 1), route);
        census_t c;
        const std::size_t mr0 = mr.allocs;
        const std::size_t mrb0 = mr.bytes;
        g_allocs = g_frees = g_bytes = 0;
        g_armed = true;
        for (std::size_t i = 0; i < kN; ++i)
            (void)rh.egress_route("link-0", static_cast<std::uint16_t>((i % 64) + 1));
        g_armed = false;
        c.mr_blocks = mr.allocs - mr0;
        c.mr_bytes = mr.bytes - mrb0;
        c.heap_blocks = g_allocs;
        c.heap_bytes = g_bytes;
        print_census("egress_route_lookup", c, kN, "on_nack:owning_copy_to_GLOBAL_heap");
    }

    // (6) What an ADVERTISE emission costs, before and after #885. The BUILDER is retained
    //     for tests and tooling and measured here as the reference; the router's own door,
    //     which is what a peer provokes, is measured next to it.
    {
        census_t c;
        g_allocs = g_frees = g_bytes = 0;
        g_armed = true;
        for (std::size_t i = 0; i < kN; ++i) {
            const std::vector<std::byte> adv = tr::net::encode_advertise(1, route);
            asm volatile("" : : "r"(adv.data()) : "memory");
        }
        g_armed = false;
        c.heap_blocks = g_allocs;
        c.heap_bytes = g_bytes;
        print_census("encode_advertise_builder", c, kN, "body+out:retained_for_tests_only");
    }
    {
        // The production door since #885: a WARM `advertise` (the label is already bound, so
        // `ensure_egress` reuses it) writes a 12-byte head on the stack, references the
        // caller's route, and hands the link two spans. Nothing is built and nothing escapes
        // to the heap — the link below overrides the gather form, so the count is the
        // ROUTER's, not a transport's concatenation.
        census_t c;
        tr::graph::graph_t g;
        tr::net::fwd_router_t router(g);
        null_link_t link;
        router.add_child("down", link);
        (void)router.advertise("down", route);  // mint + record, outside the window
        g_allocs = g_frees = g_bytes = 0;
        g_armed = true;
        for (std::size_t i = 0; i < kN; ++i) {
            const std::uint16_t l = router.advertise("down", route);
            asm volatile("" : : "r"(l) : "memory");
        }
        g_armed = false;
        c.heap_blocks = g_allocs;
        c.heap_bytes = g_bytes;
        print_census("fwd_router_warm_advertise", c, kN, "gathered_off_a_stack_head:NO_alloc");
    }
    {
        census_t c;
        static constexpr char kSeg[] = "segment";
        const std::span<const std::byte> seg(reinterpret_cast<const std::byte*>(kSeg), 7);
        static std::vector<std::byte> packed;
        packed.clear();
        for (std::size_t i = 0; i < 4; ++i) (void)tr::wire::emit_path_segment(packed, seg);
        const tr::wire::tlv_t tlv = [&] {
            tr::wire::tlv_t t;
            t.type = tr::wire::type_t::PATH;  // packed body, PL = 0 (RFC-0018)
            t.payload = std::span<const std::byte>(packed);
            return t;
        }();
        g_allocs = g_frees = g_bytes = 0;
        g_armed = true;
        for (std::size_t i = 0; i < kN; ++i) {
            const std::vector<std::byte> b = tr::wire::encode(tlv);
            asm volatile("" : : "r"(b.data()) : "memory");
        }
        g_armed = false;
        c.heap_blocks = g_allocs;
        c.heap_bytes = g_bytes;
        print_census("wire_encode_4seg_path", c, kN,
                     "on_advertise:strip+re-encode:UNGUARDED_recursive");

        // `on_advertise` also DEEP-COPIES the decoded route TLV before stripping
        // (`tlv_t stripped = route;`) — one `std::vector<tlv_t>` per node, also unguarded.
        census_t d;
        g_allocs = g_frees = g_bytes = 0;
        g_armed = true;
        for (std::size_t i = 0; i < kN; ++i) {
            tr::wire::tlv_t copy = tlv;
            asm volatile("" : : "r"(copy.children.data()) : "memory");
        }
        g_armed = false;
        d.heap_blocks = g_allocs;
        d.heap_bytes = g_bytes;
        print_census("tlv_deep_copy_4seg", d, kN, "on_advertise:stripped=route:UNGUARDED");
    }
    return 0;
}

// --- mode C: the GUARD gate (#848) -------------------------------------------

/** @brief What an arm's escape count is REQUIRED to be for the gate to pass. */
enum class expect_t : std::uint8_t {
    GUARDED,   /**< @brief Every injection point must soft-fail: `escaped == 0`. */
    NOALLOC,   /**< @brief The operation must allocate nothing at all: `allocs == 0`. */
    UNGUARDED, /**< @brief The control arm: at least one injection point MUST escape. */
};

/** @brief One registered peer-reachable egress operation the gate drives. */
struct arm_t {
    const char* name;          /**< @brief Stable, greppable arm name. */
    expect_t expect;           /**< @brief The pass condition for this arm. */
    std::function<void()> run; /**< @brief Perform the operation once, fixed inputs. */
};

/**
 * @brief Drive one arm: learn its allocation count, then refuse each allocation in turn.
 * @return true when the arm met its @ref expect_t.
 */
bool run_arm(const arm_t& arm) {
    /** @brief Clears the arming state on EVERY exit, including a stack unwind. */
    struct armed_scope_t {
        explicit armed_scope_t(std::size_t max_alloc,
                               std::vector<std::size_t>* log = nullptr) noexcept {
            g_allocs = g_frees = g_bytes = 0;
            g_max_alloc = max_alloc;
            g_size_log = log;
            g_armed = true;
        }
        ~armed_scope_t() {
            g_armed = false;
            g_max_alloc = static_cast<std::size_t>(-1);
            g_size_log = nullptr;
        }
        armed_scope_t(const armed_scope_t&) = delete;
        armed_scope_t& operator=(const armed_scope_t&) = delete;
    };

    // (1) Learn the operation's allocation count AND its request sizes, refusing nothing.
    std::vector<std::size_t> sizes;
    sizes.reserve(64);  // outside the armed window: this growth must not be counted
    std::size_t n = 0;
    {
        const armed_scope_t scope(static_cast<std::size_t>(-1), &sizes);
        arm.run();
        n = g_allocs;
    }
    std::sort(sizes.begin(), sizes.end());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());

    // (2) For each observed request size, deny a block that big and see if the failure escapes.
    std::size_t escaped = 0;
    std::size_t first_escape = 0;
    for (const std::size_t s : sizes) {
        if (s == 0) continue;
        bool threw = false;
        try {
            const armed_scope_t scope(s - 1);
            arm.run();
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        if (threw) {
            ++escaped;
            if (first_escape == 0) first_escape = s;
        }
    }

    const bool ok = arm.expect == expect_t::UNGUARDED ? escaped > 0
                    : arm.expect == expect_t::NOALLOC ? n == 0 && escaped == 0
                                                      : escaped == 0;
    const char* expect_s = arm.expect == expect_t::UNGUARDED ? "UNGUARDED"
                           : arm.expect == expect_t::NOALLOC ? "NOALLOC"
                                                             : "GUARDED";
    std::printf(
        "RESULT failable guarded op=%s expect=%s allocs=%zu sizes=%zu escaped=%zu "
        "first_escape_bytes=%zu "
        "verdict=%s\n",
        arm.name, expect_s, n, sizes.size(), escaped, first_escape, ok ? "PASS" : "FAIL");
    return ok;
}

/**
 * @brief The gate over every peer-reachable egress operation this bench can link.
 *
 * Coverage is bounded by linkage, and that is why #848 gave the overflow gather a NAMED
 * type in a header. `tr::net::iov_table_t` is new — what `transport_ws.cpp`'s anonymous
 * namespace held was the free function `build_server_iov` over a bare
 * `std::vector<::iovec>&`; the PATTERN was extracted, not the class. The five throwing
 * growths it replaced were `build_server_iov`'s own table (shared by the WS broadcast and
 * the directed per-peer send), the WS broadcast's per-peer scratch table, tcp
 * `prefixed_iov_t`'s record table, tcp's broadcast scratch table, and the udp datagram
 * gather. All five now reduce to `iov_table_t::acquire`, which is why one arm here covers
 * them. The socket-driving legs themselves are gated behaviourally by
 * `core/tests/transport_alloc_softfail_test.cpp`.
 */
int run_guard() {
    // Fixed inputs, built OUTSIDE the armed windows so their own allocations never count.
    static const std::vector<std::byte> payload(512, std::byte{0x5A});
    static const std::vector<std::byte> control(ws::kMaxControlPayload, std::byte{0x11});
    static std::array<::iovec, tr::net::kMaxInlineIov + 1> inline_vec;
    static const std::vector<std::byte> route = make_route(4);
    static can::advertise_t adv = [] {
        can::advertise_t a;
        a.can_id = 0x1234;
        a.slice_count = 3;
        a.path = "node/sensor/temperature";
        return a;
    }();
    // The label plane's fixture, PRIMED here: the router, its (non-allocating) link, the
    // egress binding the warm-advertise arm reuses, and a COMPACT for a label nothing binds.
    // Construction and the first advertise are outside every armed window on purpose — the
    // arms measure the steady state, which is what a peer drives.
    static tr::graph::graph_t label_graph;
    static tr::net::fwd_router_t label_router_obj(label_graph);
    static null_link_t label_link_obj;
    static tr::net::fwd_router_t* const label_router = [] {
        label_router_obj.add_child("down", label_link_obj);
        (void)label_router_obj.advertise("down", route);
        return &label_router_obj;
    }();
    static null_link_t* const label_link = &label_link_obj;
    static const std::vector<std::byte> stale_compact = tr::net::encode_compact(0x4242, route);

    const arm_t arms[] = {
        // The CONTROL arm: the retained THROWING server encoder, which nothing on a
        // peer-driven path calls any more. It MUST escape — if it stops escaping, the
        // injector is broken and every other verdict on this page is worthless.
        {"ws_encode_frame_throwing_CONTROL", expect_t::UNGUARDED,
         [] {
             const std::vector<std::byte> f = ws::encode_frame(ws::opcode_t::BINARY, payload);
             asm volatile("" : : "r"(f.data()) : "memory");
         }},
        // A5 — the one WS egress encoder that survives as a nothrow twin.
        {"ws_try_encode_client_frame", expect_t::GUARDED,
         [] {
             // A FRESH buffer per run: a warmed capacity would make the sweep vacuous
             // (the reserve returns true without allocating at all).
             tr::mem::block_array_t<std::byte> out(tr::mem::heap_source());
             (void)ws::try_encode_client_frame(out, ws::opcode_t::BINARY, payload, 7u);
             asm volatile("" : : "r"(out.data()) : "memory");
         }},
        // A4/A6 — the PONG reply, now built entirely on the stack.
        {"ws_encode_server_control_pong", expect_t::NOALLOC,
         [] {
             std::array<std::byte, ws::kMaxServerControlFrame> out{};
             const std::size_t n = ws::encode_server_control(out, ws::opcode_t::PONG, control);
             asm volatile("" : : "r"(n) : "memory");
         }},
        {"ws_encode_client_control_pong", expect_t::NOALLOC,
         [] {
             std::array<std::byte, ws::kMaxClientControlFrame> out{};
             const std::size_t n = ws::encode_client_control(out, ws::opcode_t::PONG, control, 9u);
             asm volatile("" : : "r"(n) : "memory");
         }},
        // A1 — the CAN advertise header, on the stack; `emit_advertise` slices it in place.
        // Measure `encode_advertise_header` itself: the contiguous `encode_advertise` twin
        // returns a std::vector and so allocates by construction, which is why the CAN
        // transport does not use it. (An ablation that swapped this body to that twin was
        // left in place on the branch, making the arm's own NOALLOC expectation fail.)
        {"can_encode_advertise_header", expect_t::NOALLOC,
         [] {
             std::array<std::byte, can::kAdvertiseHeaderSize> header{};
             const bool ok = can::encode_advertise_header(header, adv, adv.path);
             asm volatile("" : : "r"(ok), "r"(header.data()) : "memory");
         }},
        // B1/B2 — the ONE overflow gather behind all four `acquire` call sites:
        // transport_ws.cpp:175 (build_server_iov, for both WS send overrides),
        // transport_tcp.cpp:78, transport_udp.cpp:109, and posix_endpoint.cpp:367 — the
        // broadcast's per-peer scratch, ONE store for both servers since #871 folded their
        // fan-out into slot_server_t::broadcast_iov (it was two sites before that).
        {"iov_table_overflow_gather", expect_t::GUARDED,
         [] {
             tr::net::iov_table_t<::iovec> table(inline_vec);
             ::iovec* v = table.acquire(inline_vec.size() + 8);  // past the inline bound
             asm volatile("" : : "r"(v) : "memory");
         }},
        // #885 — the label plane's three egress arms, driven through the ROUTER's own doors
        // rather than through a re-spelled copy of the emitter, so a call site that reverts
        // to the retained builder reddens here even though the emitter itself is fine.
        //
        // The producer door, warm: the label is already bound (primed above), so
        // `ensure_egress` reuses it and the only work left is putting the frame on the link.
        {"fwd_router_warm_advertise", expect_t::NOALLOC,
         [] {
             const std::uint16_t l = label_router->advertise("down", route);
             asm volatile("" : : "r"(l) : "memory");
         }},
        // The peer-provoked half: a COMPACT naming a label this node never bound. The answer
        // is ten fixed bytes off the stack.
        {"fwd_router_stale_label_nack", expect_t::NOALLOC,
         [] {
             label_router->on_frame("down", stale_compact);
             asm volatile("" : : "r"(label_link->last()) : "memory");
         }},
    };

    int failures = 0;
    for (const arm_t& a : arms)
        if (!run_arm(a)) ++failures;
    std::printf("RESULT failable guard_gate arms=%zu failed=%d verdict=%s\n",
                sizeof(arms) / sizeof(arms[0]), failures, failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}

// --- mode B: the guard-shape A/B --------------------------------------------

/** @brief A 48-byte trivially-copyable element — the arena/plan node shape these arrays hold. */
struct elem_t {
    std::uint64_t a, b, c, d, e, f;
};

/** @brief `std::vector` + the global-heap probe guard: today's shape at the 29 call sites. */
[[gnu::noinline]] std::uint64_t arm_try_push_back(std::size_t n) {
    std::vector<elem_t> v;
    for (std::size_t i = 0; i < n; ++i) {
        elem_t x{i, i, i, i, i, i};
        if (!tr::detail::try_push_back(v, std::move(x))) return 0;
    }
    return v.back().a + v.size();
}

/** @brief `block_array_t` over an injected source: the ADR-0065 destination shape. */
[[gnu::noinline]] std::uint64_t arm_block_array(std::size_t n, tr::mem::block_source_t& src) {
    tr::mem::block_array_t<elem_t> v{src};
    for (std::size_t i = 0; i < n; ++i) {
        elem_t* s = v.push_slot();
        if (s == nullptr) return 0;
        *s = elem_t{i, i, i, i, i, i};
    }
    return v.back().a + v.size();
}

/** @brief Unguarded `std::vector::push_back` — the control arm; must not move. */
[[gnu::noinline]] std::uint64_t arm_raw_vector(std::size_t n) {
    std::vector<elem_t> v;
    for (std::size_t i = 0; i < n; ++i) v.push_back(elem_t{i, i, i, i, i, i});
    return v.back().a + v.size();
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

/** @brief Time one arm: @p iters arrays of @p n elements, returns ns per ELEMENT. */
template <class F>
double time_arm(F&& f, std::size_t n, std::size_t iters) {
    std::uint64_t sink = 0;
    const std::uint64_t t0 = bench::now_ns();
    for (std::size_t i = 0; i < iters; ++i) sink += f();
    const std::uint64_t t1 = bench::now_ns();
    asm volatile("" : : "r"(sink) : "memory");
    return static_cast<double>(t1 - t0) / static_cast<double>(iters * n);
}

int run_grow(std::size_t n, std::size_t reps) {
    // Size the batch so ONE rep of ONE arm runs ~100 ms: short reps are dominated by
    // scheduler noise, and the overlap check then reports noise instead of the effect.
    const std::size_t kIters = std::max<std::size_t>(20000, 30'000'000 / n);
    tr::mem::block_source_t& src = tr::mem::heap_source();
    std::vector<double> a, b, c;
    // Warm the allocator so the first rep does not pay page faults the others do not.
    (void)arm_try_push_back(n);
    (void)arm_block_array(n, src);
    (void)arm_raw_vector(n);
    for (std::size_t r = 0; r < reps; ++r) {
        // INTERLEAVED: one rep of each arm, in turn — never all-of-one-then-the-other.
        a.push_back(time_arm([&] { return arm_try_push_back(n); }, n, kIters));
        b.push_back(time_arm([&] { return arm_block_array(n, src); }, n, kIters));
        c.push_back(time_arm([&] { return arm_raw_vector(n); }, n, kIters));
    }
    for (std::size_t r = 0; r < reps; ++r)
        std::printf(
            "SAMPLE grow n=%zu rep=%zu try_push_back=%.3f block_array=%.3f raw_vector=%.3f\n", n, r,
            a[r], b[r], c[r]);
    const auto mn = [](const std::vector<double>& v) {
        return *std::min_element(v.begin(), v.end());
    };
    const auto mx = [](const std::vector<double>& v) {
        return *std::max_element(v.begin(), v.end());
    };
    std::printf(
        "RESULT failable grow n=%zu reps=%zu try_push_back_med=%.3f block_array_med=%.3f "
        "raw_vector_med=%.3f try_min=%.3f try_max=%.3f blk_min=%.3f blk_max=%.3f "
        "raw_min=%.3f raw_max=%.3f unit=ns_per_element\n",
        n, reps, median(a), median(b), median(c), mn(a), mx(a), mn(b), mx(b), mn(c), mx(c));
    return 0;
}

/** @brief Time one `try_reserve`-style growth in isolation: probe+reserve vs a bare try_alloc. */
int run_probe(std::size_t reps) {
    constexpr std::size_t kIters = 200000;
    constexpr std::size_t kBytes = 1024;
    tr::mem::block_source_t& src = tr::mem::heap_source();
    std::vector<double> a, b;
    for (std::size_t r = 0; r < reps; ++r) {
        {
            const std::uint64_t t0 = bench::now_ns();
            std::size_t ok = 0;
            for (std::size_t i = 0; i < kIters; ++i) {
                std::vector<std::byte> v;
                ok += tr::detail::try_reserve(v, kBytes) ? 1u : 0u;
                asm volatile("" : : "r"(v.data()) : "memory");
            }
            const std::uint64_t t1 = bench::now_ns();
            asm volatile("" : : "r"(ok) : "memory");
            a.push_back(static_cast<double>(t1 - t0) / static_cast<double>(kIters));
        }
        {
            const std::uint64_t t0 = bench::now_ns();
            std::size_t ok = 0;
            for (std::size_t i = 0; i < kIters; ++i) {
                void* p = src.try_alloc(kBytes, alignof(std::max_align_t));
                ok += p != nullptr ? 1u : 0u;
                asm volatile("" : : "r"(p) : "memory");
                src.release(p, kBytes, alignof(std::max_align_t));
            }
            const std::uint64_t t1 = bench::now_ns();
            asm volatile("" : : "r"(ok) : "memory");
            b.push_back(static_cast<double>(t1 - t0) / static_cast<double>(kIters));
        }
    }
    for (std::size_t r = 0; r < reps; ++r)
        std::printf("SAMPLE probe rep=%zu try_reserve=%.2f try_alloc=%.2f\n", r, a[r], b[r]);
    const auto mn = [](const std::vector<double>& v) {
        return *std::min_element(v.begin(), v.end());
    };
    const auto mx = [](const std::vector<double>& v) {
        return *std::max_element(v.begin(), v.end());
    };
    std::printf(
        "RESULT failable probe reps=%zu try_reserve_med=%.2f try_alloc_med=%.2f "
        "res_min=%.2f res_max=%.2f alloc_min=%.2f alloc_max=%.2f unit=ns_per_growth\n",
        reps, median(a), median(b), mn(a), mx(a), mn(b), mx(b));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? std::string_view(argv[1]) : std::string_view("blocks");
    if (mode == "blocks") return run_blocks();
    if (mode == "grow") {
        const std::size_t n = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 64;
        const std::size_t reps = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 9;
        return run_grow(n, reps);
    }
    if (mode == "probe") {
        const std::size_t reps = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 9;
        return run_probe(reps);
    }
    if (mode == "guard") return run_guard();
    std::printf("usage: bench_failable_census [blocks|grow N REPS|probe REPS|guard]\n");
    return 2;
}
