/**
 * @file
 * @brief #1460 phases (a) and (c) — RFC-0025 conformance vector 12 `stream/receiver-ring-flood`,
 *        the RING-PRESSURE and RECOVERY arms: at exhaustion the receiver's ring must decline as
 *        a POLICY OUTCOME under §4.4, account every loss, and recover exactly when the pressure
 *        does.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The sibling file `plane_isolation_test.cpp` binds phase (b) of the same vector — one plane's
 * store running dry must not starve another's. That arm could land before this one because it
 * needs only the injected `block_source_t` seam; these two assert against the RECEIVING STREAM
 * ring's `try_alloc` refusal path, which did not exist in the tree until the fused #1461 + #1462
 * rewrite of `vertex_t::store()`'s STREAM arm moved the queue off the producer and onto the
 * consumer's own vertex. With that in, the two arms the R-A9 split parked are assertable.
 *
 * ### What §4.4 promises, and what is therefore asserted
 *
 * The pressure contract is selected by the receiver's own declared arm
 * (`graph_t::set_ring_source`'s `reliable` flag), and the two arms fail in DIFFERENT directions:
 *
 * - **best-effort** — the admission is funded by shedding **the oldest entry, whole**. The write
 *   still succeeds: losing a queued delivery is the policy, not a fault. Every shed must be
 *   accounted twice over — once in the operator-facing delivery census
 *   (`graph_t::delivery_drops().out_of_memory`), once as an in-order
 *   `tr::flow::address_shift_gap` the CONSUMER reads off its own drain — and the ring must never
 *   grow past the byte bound it was given.
 * - **reliable** — nothing is shed, the ring does not grow, and the LOCAL producer's store
 *   answers `status_t::BACKPRESSURE`. A gap here would be a contract violation: the reliable arm
 *   trades latency for completeness, so a lost entry is exactly what it must not do.
 *
 * Silence is the one forbidden behaviour on either arm, and a suite that only checked "nothing
 * crashed" would pass against a ring that quietly dropped everything. So every arm below asserts
 * the counter **MOVED**, by the amount the pressure actually cost, and reads the injected
 * source's own refusal tally as an independent witness that the `try_alloc → nullptr` path was
 * genuinely taken rather than the bound merely never being reached.
 *
 * ### Phase (c) — recovery, in three separable claims
 *
 * Recovery is not "it works again afterwards". It is (i) the receiver resumes admitting the
 * moment its source can fund an entry, (ii) the accounting reflects **exactly** what was dropped
 * and then **stops** — a gap counter that keeps climbing through a healthy stream is as useless
 * as one that never moved — and (iii) nothing leaked: every reservation the flood took is handed
 * back, so the source's live balance returns to its pre-flood value. Each is a separate check.
 *
 * ### The instrument
 *
 * @ref metered_source_t is a hard byte ceiling over the heap whose ceiling MOVES — which is what
 * makes recovery expressible at all: `bump_source_t` cannot un-exhaust, and a pool source would
 * fold the ceiling into a size-class geometry that the assertions would then be about. It counts
 * its refusals and its live balance, so "the store refused" and "no block leaked" are direct
 * reads rather than inferences.
 *
 * Entry width is MEASURED, never hardcoded: one admission into an unbounded ring reports its own
 * reserved width through `graph_t::ring_reserved_bytes`, and the ceilings below are multiples of
 * that. A test that guessed `sizeof(ring_entry_t)` would be asserting against the compiler's
 * layout rather than against the contract.
 *
 * Single-threaded, so it is meaningful under ASan+UBSan (`-fno-sanitize-recover=all`) and costs
 * nothing under TSan beyond the vertex stripe's own locking.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>
#include <string_view>
#include <vector>

#include "libtracer/mem_source.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::view::rope_t;

using tr::testing::check;
using tr::testing::make_value;

/** @brief The payload every admission below carries, in bytes — uniform, so the ring's byte
 *         bound is a whole number of entries and a shed is unambiguous. */
constexpr std::size_t kPayloadBytes = 32;

/**
 * @brief A hard byte ceiling over the heap whose ceiling can MOVE, counting refusals and the
 *        live balance.
 *
 * Three things this has that no shipped source has together, each load-bearing here:
 *
 *  - a ceiling that can be RAISED, which is how "the pressure clears" is expressed. A
 *    `bump_source_t` over a slab is a one-way street — once drained it stays drained even after
 *    its blocks are released — so recovery could not be observed through one at all.
 *  - a LIVE BALANCE, so "no block was leaked" is a read of the source rather than an inference
 *    from the ring's own bookkeeping (which is the thing under test and may not be its own
 *    witness).
 *  - a REFUSAL count, so an arm that asserts "the ring shed" also proves the shed was provoked
 *    by a real `try_alloc → nullptr` and not by a bound that was never reached.
 *
 * Sized reclaim is the `block_source_t` contract, so the balance is exact and needs no header.
 */
class metered_source_t final : public tr::mem::block_source_t {
   public:
    /** @brief Construct with an initial ceiling of @p budget bytes. */
    explicit metered_source_t(std::size_t budget)
        : tr::mem::block_source_t("metered"), budget_(budget) {}

    /** @brief Serve from the heap while the ceiling allows; refuse (and count it) when it does
     *         not. Never falls through to the heap on refusal — the ceiling IS the bound. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (live_ + bytes > budget_) {
            ++refusals_;
            return nullptr;
        }
        void* const p = ::operator new(bytes, std::align_val_t{align}, std::nothrow);
        if (p == nullptr) {
            ++refusals_;
            return p;
        }
        live_ += bytes;
        ++served_;
        return p;
    }

    /** @brief Sized, aligned reclaim — the balance is decremented by exactly what was charged. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        if (p == nullptr) return;
        live_ -= bytes;
        ++returned_;
        ::operator delete(p, std::align_val_t{align});
    }

    /** @brief Move the ceiling — lowering it applies PRESSURE, raising it CLEARS it. */
    void set_budget(std::size_t budget) noexcept { budget_ = budget; }
    /** @brief Bytes currently outstanding against this source. */
    [[nodiscard]] std::size_t live_bytes() const noexcept { return live_; }
    /** @brief How many requests were refused for want of ceiling. */
    [[nodiscard]] std::uint64_t refusals() const noexcept { return refusals_; }
    /** @brief How many requests were served. */
    [[nodiscard]] std::uint64_t served() const noexcept { return served_; }
    /** @brief How many blocks were handed back. */
    [[nodiscard]] std::uint64_t returned() const noexcept { return returned_; }

   private:
    std::size_t budget_;         /**< @brief The movable ceiling, in bytes. */
    std::size_t live_ = 0;       /**< @brief Outstanding bytes. */
    std::uint64_t refusals_ = 0; /**< @brief `try_alloc` refusals. */
    std::uint64_t served_ = 0;   /**< @brief Successful `try_alloc`s. */
    std::uint64_t returned_ = 0; /**< @brief `release`s. */
};

/** @brief Records that a delivery reached the subscriber — present so the receiving vertex has
 *         an edge of its own, which is what makes a shed COUNTABLE in the delivery census
 *         (`count_store_drops` is own-subs-wide, and a vertex nobody subscribes to counts
 *         nothing by design). */
struct sink_t {
    std::size_t deliveries = 0; /**< @brief How many values arrived. */
    /** @brief The subscriber callback. */
    void operator()(const rope_t&) { ++deliveries; }
};

/** @brief One admission's worth of value — @p tag repeated, so a read-back names its writer. */
[[nodiscard]] rope_t payload(std::uint8_t tag) {
    const std::vector<std::byte> bytes(kPayloadBytes, static_cast<std::byte>(tag));
    return rope_t{make_value(bytes)};
}

/**
 * @brief A receiving STREAM vertex wired to its own metered source, with an edge of its own.
 *
 * `set_history_depth` is deliberately GENEROUS: the depth intent must not be what trims the
 * ring, or the arms below would be measuring RFC-0025 §4.6's depth bound instead of §4.6.1
 * clause 3's BYTE bound. The two compose, and this suite is about the byte one.
 */
struct receiver_t {
    /** @brief Wire the vertex at @p where, charging its ring to @p src under @p reliable. */
    receiver_t(graph_t& g, const char* where, metered_source_t& src, bool reliable)
        : v(g.register_vertex(*path_t::parse(where), role_t::STREAM)) {
        g.set_history_depth(v, kGenerousDepth);
        g.set_ring_source(v, &src, reliable);
        const tr::graph::result_t<tr::graph::subscription_t> s =
            g.subscribe(*path_t::parse(where), sink);
        check(s.has_value(), "setup: the receiver has an edge of its own");
    }

    /** @brief Deep enough that the ENTRY count never trims — the byte bound must be the only
     *         thing that bites. */
    static constexpr std::uint32_t kGenerousDepth = 64;

    vertex_handle_t v; /**< @brief The receiving STREAM vertex. */
    sink_t sink;       /**< @brief Its subscriber. */
};

/** @brief `ring_reserved_bytes`, unwrapped — a non-STREAM role cannot reach here. */
[[nodiscard]] std::size_t reserved(const graph_t& g, vertex_handle_t v) {
    const tr::graph::result_t<std::size_t> r = g.ring_reserved_bytes(v);
    check(r.has_value(), "probe: the receiver reports its reserved width");
    return r.has_value() ? *r : 0;
}

/** @brief `stream_gaps`, unwrapped — the cumulative `tr::flow::address_shift_gap` census. */
[[nodiscard]] std::uint64_t gaps(const graph_t& g, vertex_handle_t v) {
    const tr::graph::result_t<std::uint64_t> r = g.stream_gaps(v);
    check(r.has_value(), "probe: the receiver reports its gap census");
    return r.has_value() ? *r : 0;
}

/**
 * @brief Measure ONE entry's reserved width by admitting one into an unbounded ring.
 *
 * Hardcoding this would make every ceiling below a claim about `sizeof(ring_entry_t)` on this
 * compiler. Measuring it makes them claims about entries.
 */
[[nodiscard]] std::size_t measure_entry_width() {
    // The sources outlive the graph: a vertex hands its reservations back in its destructor,
    // so a source that died first would be a pure-virtual call on a corpse. Declaration order
    // IS that lifetime order here and in every arm below, and must not be shuffled.
    metered_source_t unbounded(1U << 20U);
    graph_t g;
    receiver_t rx(g, "/probe/rx", unbounded, /*reliable=*/false);
    check(g.assign(rx.v, payload(0x01)).has_value(), "probe: one admission into an unbounded ring");
    const std::size_t w = reserved(g, rx.v);
    check(w > kPayloadBytes, "probe: an entry costs its payload plus the ring's own overhead");
    return w;
}

/**
 * @brief (a) BEST-EFFORT — at exhaustion the ring sheds the oldest, the write still succeeds,
 *        and every loss is accounted on both faces.
 */
void test_best_effort_sheds_and_accounts(std::size_t entry) {
    std::printf(" (a) best-effort — the ring sheds the oldest and ACCOUNTS it:\n");
    // Three entries' worth, exactly. The fourth admission cannot be funded without a shed.
    constexpr std::uint64_t kFit = 3;
    metered_source_t src(kFit * entry);  // outlives `g` — see measure_entry_width
    graph_t g;
    receiver_t rx(g, "/rx/best-effort", src, /*reliable=*/false);

    // POSITIVE CONTROL — while the ceiling has room, nothing is refused and nothing is shed.
    for (std::uint64_t i = 0; i < kFit; ++i)
        check(g.assign(rx.v, payload(static_cast<std::uint8_t>(0x10 + i))).has_value(),
              "best-effort: an admission the ceiling can fund succeeds");
    check(gaps(g, rx.v) == 0, "best-effort: and sheds nothing while it has room");
    check(src.refusals() == 0, "best-effort: the source refused nothing — the bound was not hit");
    check(reserved(g, rx.v) == kFit * entry,
          "best-effort: the ring holds exactly what it admitted");

    // THE FLOOD. Every one of these must be funded by shedding the oldest.
    constexpr std::uint64_t kOver = 5;
    const std::uint64_t drops0 = g.delivery_drops().out_of_memory;
    for (std::uint64_t i = 0; i < kOver; ++i)
        check(g.assign(rx.v, payload(static_cast<std::uint8_t>(0x20 + i))).has_value(),
              "best-effort: an admission past the bound STILL SUCCEEDS — shedding is the policy");

    check(src.refusals() == kOver,
          "best-effort: each of those took the source's try_alloc REFUSAL path");
    check(gaps(g, rx.v) == kOver,
          "best-effort: the gap census MOVED by exactly what the pressure cost");
    check(g.delivery_drops().out_of_memory - drops0 == kOver,
          "best-effort: and the operator-facing delivery census moved with it, one per subscriber");
    check(reserved(g, rx.v) == kFit * entry,
          "best-effort: the ring never grew past the byte bound it was given");

    // The CONSUMER's face of the same loss: an in-order gap, delivered once, at the shed point.
    std::vector<std::shared_ptr<const rope_t>> batch;
    std::uint64_t gap = 0;
    const tr::graph::result_t<std::size_t> drained = g.drain_unflushed(rx.v, batch, &gap);
    check(drained.has_value(), "best-effort: the consumer drains its own ring");
    check(gap == kOver, "best-effort: the drain reports the gap — silence is the forbidden answer");
    check(drained.has_value() && *drained == kFit,
          "best-effort: and hands over exactly what survived, oldest-first");

    std::uint64_t again = 0;
    batch.clear();
    const tr::graph::result_t<std::size_t> second = g.drain_unflushed(rx.v, batch, &again);
    check(second.has_value() && again == 0,
          "best-effort: the gap is reported ONCE — a re-drain does not re-report it");

    // The shed cost QUEUED deliveries, never the state plane: the last-known-value is intact.
    const tr::graph::result_t<tr::graph::value_ref_t> lkv = g.read(rx.v);
    check(lkv.has_value() && static_cast<bool>(*lkv), "best-effort: the LKV survived the flood");
    check(lkv.has_value() && static_cast<bool>(*lkv) &&
              std::to_integer<std::uint8_t>((**lkv).only().bytes()[0]) ==
                  static_cast<std::uint8_t>(0x20 + kOver - 1),
          "best-effort: and it is the LAST value written, byte-exact — a shed is a QUEUE loss");
}

/**
 * @brief (a) RELIABLE — at exhaustion the admission is refused, NOTHING is shed, and the local
 *        producer is told so by value.
 */
void test_reliable_refuses_and_sheds_nothing(std::size_t entry) {
    std::printf("\n (a) reliable — the admission is REFUSED and nothing is lost:\n");
    constexpr std::uint64_t kFit = 3;
    metered_source_t src(kFit * entry);  // outlives `g` — see measure_entry_width
    graph_t g;
    receiver_t rx(g, "/rx/reliable", src, /*reliable=*/true);

    // POSITIVE CONTROL — the same verb, the same vertex, while the ceiling has room.
    for (std::uint64_t i = 0; i < kFit; ++i)
        check(g.assign(rx.v, payload(static_cast<std::uint8_t>(0x30 + i))).has_value(),
              "reliable: an admission the ceiling can fund succeeds");

    const std::uint64_t drops0 = g.delivery_drops().out_of_memory;
    const tr::graph::result_t<void> refused = g.assign(rx.v, payload(0x3F));
    check(!refused.has_value(), "reliable: the admission past the bound is REFUSED");
    check(!refused.has_value() && refused.error() == status_t::BACKPRESSURE,
          "reliable: and says so as BACKPRESSURE, to a producer that can slow down");
    check(src.refusals() == 1, "reliable: the refusal came from the source, not from a guess");
    check(gaps(g, rx.v) == 0, "reliable: NOTHING was shed — completeness is what this arm buys");
    check(g.delivery_drops().out_of_memory == drops0,
          "reliable: and nothing was counted lost, because nothing was");
    check(reserved(g, rx.v) == kFit * entry, "reliable: the ring did not grow past its bound");
}

/**
 * @brief (c) RECOVERY — the receiver resumes the moment the ceiling does, the accounting stops
 *        exactly where the losses did, and no reservation is leaked.
 */
void test_recovery_resumes_stops_counting_and_leaks_nothing(std::size_t entry) {
    std::printf("\n (c) recovery — resume, stop counting, leak nothing:\n");
    constexpr std::uint64_t kFit = 3;
    constexpr std::uint64_t kOver = 4;
    metered_source_t src(kFit * entry);  // both sources outlive `g` — see measure_entry_width
    metered_source_t healthy(1U << 20U);
    graph_t g;
    receiver_t rx(g, "/rx/recover", src, /*reliable=*/false);

    for (std::uint64_t i = 0; i < kFit + kOver; ++i)
        check(g.assign(rx.v, payload(static_cast<std::uint8_t>(0x40 + i))).has_value(),
              "recovery: the flood runs to completion under best-effort");
    const std::uint64_t shed = gaps(g, rx.v);
    check(shed == kOver, "recovery: the flood cost exactly the sheds the pressure explains");

    // A separate receiver on the SAME graph, charged to a source of its own, proves the resume
    // below is this receiver's own and not the graph shrugging off pressure everywhere.
    receiver_t other(g, "/rx/unpressured", healthy, /*reliable=*/false);
    check(g.assign(other.v, payload(0x50)).has_value(),
          "recovery: a receiver on its own source was never under pressure");
    check(gaps(g, other.v) == 0, "recovery: and shed nothing while its neighbour was flooding");

    // THE PRESSURE CLEARS. Nothing about the vertex changes — only its source's ceiling.
    src.set_budget(1U << 20U);
    const std::uint64_t refusals_at_clear = src.refusals();
    constexpr std::uint64_t kAfter = 6;
    for (std::uint64_t i = 0; i < kAfter; ++i)
        check(g.assign(rx.v, payload(static_cast<std::uint8_t>(0x60 + i))).has_value(),
              "recovery: the receiver admits again the moment its source can fund an entry");

    check(src.refusals() == refusals_at_clear,
          "recovery: and refuses nothing more — the try_alloc path is quiet again");
    check(gaps(g, rx.v) == shed,
          "recovery: the gap census STOPPED at what was actually dropped, and does not keep "
          "climbing through a healthy stream");
    check(reserved(g, rx.v) == (kFit + kAfter) * entry,
          "recovery: the ring grows again, now that its budget allows it");

    // The CONSUMER learns of the whole loss exactly once, then hears only clean batches.
    std::vector<std::shared_ptr<const rope_t>> batch;
    std::uint64_t gap = 0;
    check(g.drain_unflushed(rx.v, batch, &gap).has_value() && gap == shed,
          "recovery: the drain accounts exactly what was dropped, once");
    check(g.assign(rx.v, payload(0x6F)).has_value(), "recovery: one more clean admission");
    batch.clear();
    gap = 1;
    check(g.drain_unflushed(rx.v, batch, &gap).has_value() && gap == 0,
          "recovery: and the batch after recovery carries NO gap");

    // NO LEAK. Retiring the vertex releases every reservation to the source that served it
    // (`~ring_state_t::release_all`), so the balance returns to its pre-flood value: zero.
    check(src.live_bytes() > 0, "recovery: the receiver is holding reservations before it retires");
    check(g.retire(rx.v).has_value(), "recovery: the receiver retires");
    check(src.live_bytes() == 0,
          "recovery: every reservation the flood took came back — the live balance is zero again");
    check(src.served() == src.returned(),
          "recovery: served and returned agree — no block was leaked or double-counted");
}

}  // namespace

int main() {
    std::printf(
        "#1460 phases (a)+(c) — RFC-0025 vector 12 `stream/receiver-ring-flood`, ring "
        "pressure\n\n");
    const std::size_t entry = measure_entry_width();
    std::printf(" one ring entry reserves %zu bytes (measured)\n\n", entry);
    test_best_effort_sheds_and_accounts(entry);
    test_reliable_refuses_and_sheds_nothing(entry);
    test_recovery_resumes_stops_counting_and_leaks_nothing(entry);
    return tr::testing::summary("ring_pressure");
}
