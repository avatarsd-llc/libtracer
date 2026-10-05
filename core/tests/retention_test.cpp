/**
 * @file
 * @brief RFC-0028 slice 6 (D4) — retention is ONE per-vertex, per-field policy:
 *        `retention_t { NONE, LAST, N }`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The §6.6 gate, and the pieces around it that make it mean something:
 *
 *  - **A `wo` app field stores nothing.** Its write reaches `on_app_field_write` with the bytes,
 *    and the field table's lazily-allocated `values` store is never allocated on its account —
 *    asserted by counting global-heap allocations across the store itself, against an `rw`
 *    field on the same table as the positive control (whose first store DOES allocate it).
 *  - **A `NONE` value vertex answers `read` with `NOT_FOUND` after a delivered write.** The
 *    subscriber is proven to have been delivered to first, so the `NOT_FOUND` is retention and
 *    not a write that never happened.
 *  - **The relay draws no block.** A `NONE` vertex whose only subscribers are callbacks delivers
 *    from the stack: zero global-heap allocations per write, against the `LAST` arm's one.
 *  - **The ring rides the injected source.** A STREAM's queued entries live in their own
 *    reservations, so steady-state appends cost the global heap nothing the same writes to a
 *    ring-less vertex do not also cost (the retired `std::deque` put a chunk there every 16).
 *
 * The allocation instrument is a global `operator new` counter on the
 * `handler_write_alloc_test` precedent — every allocating and deallocating form replaced, since
 * a hole would make the allocation under test invisible. Assertions are slot-aware: under the
 * `hazard_slot_t` binding a retaining store also draws reclamation nodes, so a retaining arm is
 * only ever bounded BELOW, and the zero claims are made about arms that retain nothing.
 */

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <string>
#include <vector>

#include "libtracer/mem_source.hpp"
#include "libtracer/tracer.hpp"
#include "test_history.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

/** @brief Global-new call counter, live only while @ref g_arm is set. */
std::atomic<std::size_t> g_allocs{0};
std::atomic<bool> g_arm{false};

/** @brief The counted allocation itself — malloc-backed so `operator delete` can free it. */
void* counted(std::size_t n) {
    if (g_arm.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n == 0 ? 1 : n);
}

/** @brief The aligned counted allocation — `aligned_alloc` only for a genuinely OVER-aligned
 *         request (which `free` accepts), `malloc` for a fundamental one. */
void* counted_aligned(std::size_t n, std::size_t align) {
    if (align <= alignof(std::max_align_t)) return counted(n);
    if (g_arm.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
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

using tr::graph::app_access_t;
using tr::graph::app_field_t;
using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::retention_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::graph::vertex_t;
using tr::testing::check;
using tr::testing::make_value;
using tr::view::rope_t;
using tr::view::view_t;

/** @brief Writes per counted window; a settle pass takes the one-offs out of it first. */
constexpr std::size_t kWrites = 200;

/** @brief Run @p op once to settle, then @ref kWrites times under the counter; return the
 *         global-heap allocations the window saw. */
template <class Op>
[[nodiscard]] std::size_t count_allocs(Op&& op) {
    for (std::size_t i = 0; i < 16; ++i) op();
    g_allocs.store(0, std::memory_order_relaxed);
    g_arm.store(true, std::memory_order_seq_cst);
    for (std::size_t i = 0; i < kWrites; ++i) op();
    g_arm.store(false, std::memory_order_seq_cst);
    return g_allocs.load(std::memory_order_relaxed);
}

/** @brief The vertex behind a handle — the tests below reach the per-vertex verbs the graph
 *         wraps, on the `qos_policy_test` precedent. */
[[nodiscard]] vertex_t* vx(const vertex_handle_t& v) { return std::bit_cast<vertex_t*>(v); }

/** @brief A TLV-shaped value, built from a byte string (what an app field stores verbatim). */
[[nodiscard]] std::vector<std::byte> tlv(std::string_view s) {
    std::vector<std::byte> out{std::byte{0x01}, std::byte{0x00}, static_cast<std::byte>(s.size()),
                               std::byte{0x00}};
    for (const char c : s) out.push_back(static_cast<std::byte>(c));
    return out;
}

/** @brief Counts callback deliveries. */
void count_sink(void* ctx, const tr::graph::value_t& /*value*/) {
    static_cast<std::atomic<std::uint64_t>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------

/** @brief §6.6 gate 1 — a `wo` write reaches the seam and leaves `values` unallocated. */
void test_wo_field_stores_nothing() {
    std::printf("app field `wo` — delivered to the seam, stored nowhere:\n");
    graph_t g;
    int fired = 0;
    std::vector<std::byte> seen;
    handlers_t h;
    auto h_on_app_field_write = [&](std::string_view name, const view_t& value) {
        if (name == "secret") {
            ++fired;
            seen.assign(value.bytes().begin(), value.bytes().end());
        }
    };
    h.on_app_field_write = tr::graph::thunk(h_on_app_field_write);
    const vertex_handle_t v =
        g.register_vertex(path_t("/dev/a"), role_t::STORED_VALUE, std::move(h));
    std::vector<app_field_t> table;
    table.push_back(app_field_t{.name = "secret", .access = app_access_t::WO});
    table.push_back(app_field_t{.name = "mode", .access = app_access_t::RW});
    table.push_back(
        app_field_t{.name = "pulse", .access = app_access_t::RW, .retention = retention_t::NONE});
    (void)g.set_policy(v, {.app_fields = std::move(table)});

    check(tr::graph::app_field_slot_t{.name = "w", .access = app_access_t::WO}.retention ==
                  retention_t::NONE &&
              tr::graph::app_field_slot_t{.name = "r", .access = app_access_t::RW}.retention ==
                  retention_t::LAST,
          "the retention default follows the access: `wo` is NONE, `rw` is LAST");

    const std::vector<std::byte> val = tlv("hunter2");
    const path_t secret("/dev/a:settings.app.secret");
    check(g.write(v, secret.field(), make_value(val), "peer").has_value(),
          "a remote write to the `wo` field lands");
    check(fired == 1 && seen == val, "... and reaches on_app_field_write with its bytes");
    check(!g.read(path_t("/dev/a:settings.app.secret")).has_value(),
          "... and has no read surface, as before");

    // The store itself, counted: a `wo` field must not allocate the lazy `values` store (or
    // anything else), while the first store to an `rw` field on the SAME table must — which is
    // what proves the counter could see the allocation this asserts is absent.
    const std::size_t wo_allocs =
        count_allocs([&] { (void)vx(v)->app_field_store("secret", val); });
    check(wo_allocs == 0, "a `wo` store allocates NOTHING — `values` stays unallocated");
    g_allocs.store(0, std::memory_order_relaxed);
    g_arm.store(true, std::memory_order_seq_cst);
    (void)vx(v)->app_field_store("mode", val);
    g_arm.store(false, std::memory_order_seq_cst);
    check(g_allocs.load() > 0,
          "positive control: the first `rw` store DOES allocate `values` (the counter is live)");

    for (const app_field_t& f : vx(v)->app_fields_snapshot())
        if (f.name == "secret")
            check(f.value.empty(), "the table snapshot holds no bytes for the `wo` field");

    // An `rw` field declared NONE: the write is applied and nothing is kept to read back.
    check(g.write(path_t("/dev/a:settings.app.pulse"), make_value(val)).has_value(),
          "a write to an `rw` field declared NONE lands");
    check(!g.read(path_t("/dev/a:settings.app.pulse")).has_value(),
          "... and reads back NOT_FOUND — nothing was retained");

    // An initial value carried by a `wo` declaration is dropped like any write to it.
    std::vector<app_field_t> t2;
    t2.push_back(app_field_t{.name = "key", .access = app_access_t::WO, .value = val});
    const vertex_handle_t w = g.register_vertex(path_t("/dev/b"), role_t::STORED_VALUE);
    (void)g.set_policy(w, {.app_fields = std::move(t2)});
    const std::vector<app_field_t> snap = vx(w)->app_fields_snapshot();
    check(snap.size() == 1 && snap[0].value.empty(),
          "an install-time value on a `wo` field is not stored either");
}

/** @brief §6.6 gate 2 — a `NONE` value vertex answers `read` with `NOT_FOUND` after a delivered
 *         write, and the verbs that need a retained value refuse. */
void test_none_vertex_relays() {
    std::printf("\nvalue vertex `NONE` — the pure relay:\n");
    graph_t g;
    const path_t p("/relay/x");
    const vertex_handle_t v = g.register_vertex(p, role_t::STORED_VALUE);
    std::atomic<std::uint64_t> recv{0};
    check(g.subscribe(p, &count_sink, &recv).has_value(), "a callback subscriber");
    check(g.retention(v) == retention_t::LAST, "a STORED_VALUE retains LAST by default");

    // Held first, so the switch is seen to DROP what it held.
    check(g.write(v, make_value({0x01})).has_value() && g.read(v).has_value(),
          "under LAST a write is readable");
    check(g.set_policy(v, {.retention = retention_t::NONE}).has_value(),
          "NONE is legal on a STORED_VALUE");
    check(g.retention(v) == retention_t::NONE, "... and reads back as NONE");
    check(!g.read(v).has_value() && g.read(v).error() == status_t::NOT_FOUND,
          "switching to NONE drops the held value — read is NOT_FOUND at once");

    const std::uint64_t seq0 = recv.load();
    check(g.write(v, make_value({0x02})).has_value(), "a write to the NONE vertex succeeds");
    check(recv.load() == seq0 + 1, "... and IS delivered to the subscriber");
    const auto r = g.read(v);
    check(!r.has_value() && r.error() == status_t::NOT_FOUND,
          "GATE: after a delivered write, read answers NOT_FOUND");
    check(!g.assign(v, make_value({0x03})).has_value() &&
              g.assign(v, make_value({0x03})).error() == status_t::SCHEMA_NOT_FOUND,
          "assign refuses — there is no state plane to land in (RFC-0008 Amendment 2)");
    check(!g.propagate(v).has_value(), "propagate refuses for the same reason");

    // Back to LAST: retention resumes with the next write.
    check(g.set_policy(v, {.retention = retention_t::LAST}).has_value() &&
              g.write(v, make_value({0x04})).has_value() && g.read(v).has_value(),
          "back to LAST, the next write is readable again");

    // A NONE TARGET: a delivery into it lands (the target's sequence moves) and keeps nothing.
    const path_t src("/relay/src");
    const path_t dst("/relay/dst");
    const vertex_handle_t s = g.register_vertex(src, role_t::STORED_VALUE);
    const vertex_handle_t d = g.register_vertex(dst, role_t::STORED_VALUE);
    check(g.set_policy(d, {.retention = retention_t::NONE}).has_value(), "a NONE target");
    check(g.subscribe(src, dst).has_value(), "wired src -> dst");
    const tr::graph::write_seq_t dseq = vx(d)->current_seq();
    check(g.write(s, make_value({0x05})).has_value(), "the source write lands");
    check(vx(d)->current_seq() == dseq + 1, "the delivery reached the NONE target");
    check(!g.read(d).has_value(), "... which retains nothing — read is NOT_FOUND");
    check(g.read(s).has_value(), "the source (LAST) still holds its own");

    // A STREAM under NONE: no ring, nothing to read back, but writes still reach subscribers.
    const path_t sp("/relay/stream");
    const vertex_handle_t st = g.register_vertex(sp, role_t::STREAM);
    std::atomic<std::uint64_t> srecv{0};
    check(g.subscribe(sp, &count_sink, &srecv).has_value(), "a STREAM subscriber");
    check(g.retention(st) == retention_t::N, "a STREAM retains N by default");
    check(!g.set_policy(st, {.retention = retention_t::LAST}).has_value(),
          "LAST is refused on a STREAM");
    check(g.set_policy(st, {.retention = retention_t::NONE}).has_value(),
          "NONE is legal on a STREAM");
    check(g.write(st, make_value({0x06})).has_value() && srecv.load() == 1,
          "a NONE STREAM still delivers its write");
    const auto hist = tr::testing::history_of(g, st);
    check(hist.has_value() && hist->empty() && !g.read(st).has_value(),
          "... and keeps no ring entry and no value");

    // A HANDLER is NONE and nothing else.
    handlers_t h;
    auto h_on_write = [](const tr::graph::value_t&,
                         const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> { return {}; };
    h.on_write = tr::graph::thunk(h_on_write);
    const vertex_handle_t hv = g.register_vertex(path_t("/relay/h"), role_t::HANDLER, std::move(h));
    check(g.retention(hv) == retention_t::NONE, "a HANDLER retains NONE");
    check(g.set_policy(hv, {.retention = retention_t::NONE}).has_value() &&
              !g.set_policy(hv, {.retention = retention_t::LAST}).has_value() &&
              !g.set_policy(hv, {.retention = retention_t::N, .depth = 4}).has_value(),
          "a HANDLER accepts NONE and refuses LAST and N");

    // Retirement resets the declaration: the next occupant starts from its role's default.
    check(g.retire(v).has_value(), "retire the relay");
    const vertex_handle_t again = g.register_vertex(p, role_t::STORED_VALUE);
    check(g.retention(again) == retention_t::LAST, "the next occupant retains LAST again");
}

/** @brief The relay draws no block: a `NONE` write to callback-only subscribers allocates
 *         nothing, where the same write under `LAST` publishes one block. */
void test_none_relay_draws_no_block() {
    std::printf("\nNONE relay — zero blocks per write to callback subscribers:\n");
    // A persistent segment: building the written rope is a refcount, never an allocation.
    const view_t seg = make_value({0xAB, 0xCD, 0xEF, 0x01});
    std::atomic<std::uint64_t> recv{0};
    graph_t g;
    const path_t pn("/z/none");
    const path_t pl("/z/last");
    const vertex_handle_t none = g.register_vertex(pn, role_t::STORED_VALUE);
    const vertex_handle_t last = g.register_vertex(pl, role_t::STORED_VALUE);
    check(g.set_policy(none, {.retention = retention_t::NONE}).has_value(),
          "the relay arm is NONE");
    for (int i = 0; i < 8; ++i) {
        (void)g.subscribe(pn, &count_sink, &recv);
        (void)g.subscribe(pl, &count_sink, &recv);
    }
    const std::uint64_t before = recv.load();
    const std::size_t none_allocs = count_allocs([&] { (void)g.write(none, rope_t{seg}); });
    check(recv.load() - before == 8 * (kWrites + 16), "the NONE arm delivered every write");
    const std::size_t last_allocs = count_allocs([&] { (void)g.write(last, rope_t{seg}); });
    std::printf("   allocations per write: NONE %.3f, LAST %.3f\n",
                static_cast<double>(none_allocs) / kWrites,
                static_cast<double>(last_allocs) / kWrites);
    check(none_allocs == 0, "NONE with K=8 callback subscribers draws ZERO blocks per write");
    check(last_allocs >= kWrites,
          "positive control: LAST publishes at least one block per write (the counter is live)");
}

/**
 * @brief A counting ring source: malloc-backed, so what the ring draws from it never shows on
 *        the global-heap counter, and the live balance proves nothing leaks.
 */
class ring_source_t final : public tr::mem::block_source_t {
   public:
    ring_source_t() : tr::mem::block_source_t("ring") {}
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        const std::size_t a = align < alignof(std::max_align_t) ? alignof(std::max_align_t) : align;
        void* const p = std::aligned_alloc(a, (bytes + a - 1) / a * a);
        if (p != nullptr) live += bytes;
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t /*align*/) noexcept override {
        live -= bytes;
        std::free(p);
    }
    std::size_t live = 0; /**< @brief Outstanding bytes. */
};

/** @brief The ring rides the injected source: its entries cost the global heap nothing. */
void test_ring_rides_the_source() {
    std::printf("\nSTREAM ring — the entries live in their own reservations:\n");
    const view_t seg = make_value({0x11, 0x22, 0x33, 0x44});
    ring_source_t src;  // outlives the graph: the ring releases into it on teardown
    {
        graph_t g;
        const vertex_handle_t st = g.register_vertex(path_t("/r/stream"), role_t::STREAM);
        const vertex_handle_t sv = g.register_vertex(path_t("/r/stored"), role_t::STORED_VALUE);
        check(g.set_policy(st, {.retention = retention_t::N, .depth = 64, .ring_source = &src})
                  .has_value(),
              "a ring 64 deep over its own source");
        // `assign` stores without delivering, so the only difference between the two arms is
        // the ring admission itself. The retired deque allocated a chunk every 16 appends.
        // Both arms are WARMED first, uncounted: under `hazard_slot_t` each LKV publish
        // displaces a reclamation node, and until this thread's scans stock its free list a
        // publish draws that node from the global heap ON PURPOSE (the #873 carve-out in
        // `lkv_slot.hpp`). Whichever arm ran first paid that one-time priming — measured under
        // the qsbr TSan leg as STREAM 249 against STORED_VALUE 201 — which is not the ring.
        for (std::size_t i = 0; i < kWrites; ++i) {
            (void)g.assign(st, rope_t{seg});
            (void)g.assign(sv, rope_t{seg});
        }
        const std::size_t stream_allocs = count_allocs([&] { (void)g.assign(st, rope_t{seg}); });
        const std::size_t stored_allocs = count_allocs([&] { (void)g.assign(sv, rope_t{seg}); });
        std::printf("   allocations over %zu appends: STREAM %zu, STORED_VALUE %zu\n", kWrites,
                    stream_allocs, stored_allocs);
        check(stream_allocs <= stored_allocs,
              "a ring append costs the global heap nothing beyond the value itself");
        const auto held = g.ring_reserved_bytes(st);
        check(held.has_value() && *held == src.live && src.live > 0,
              "every byte the ring holds is a reservation on ITS source");
        const auto hist = tr::testing::history_of(g, st);
        check(hist.has_value() && hist->size() == 64, "the ring holds exactly its depth");
    }
    check(src.live == 0, "teardown returned every reservation — the entries went with them");
}

}  // namespace

int main() {
    std::printf("RFC-0028 slice 6 — retention (D4)\n\n");
    test_wo_field_stores_nothing();
    test_none_vertex_relays();
    test_none_relay_draws_no_block();
    test_ring_rides_the_source();
    return tr::testing::summary("retention");
}
