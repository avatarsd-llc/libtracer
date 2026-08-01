/**
 * @file
 * @brief The ADR-0072 reclamation domain, first tenant: retired value seams (#576).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Three properties, each of which the pre-ADR-0072 park-forever vector got wrong or
 * could not state:
 *
 *   1. **Bounded under churn.** Repeated retire of a handler-bearing vertex keeps the
 *      count of LIVE seam blocks bounded by the domain's scan batch, where the old
 *      `retired_seams_` vector grew monotonically — one block per retire, forever, at a
 *      rate a remote peer chooses (RFC-0014 connection churn). Counted through an
 *      instrumented seam (a probe `shared_ptr` captured by the handler, whose use_count
 *      falls when the block is actually destroyed) AND the domain's own accounting.
 *   2. **Freed only after the callback returns.** A reader parked inside a SLOW user
 *      callback has ANNOUNCED the seam block; a concurrent retire plus enough churn to
 *      force scans must not free it while the callback is in flight (ASan/TSan would
 *      flag the use-after-free), and a scan after the reader returns must free it.
 *      This is the concurrency case the announce/scan protocol exists for; it carries
 *      its own weight under ThreadSanitizer (core-ci.yml `tsan` gate).
 *   3. **The placeholder / no-ext path pays nothing.** The announce is gated on the
 *      existing has-extension check (ADR-0072 §4), asserted structurally through the
 *      `seam_announces()` counter (the RFC-0005 `ancestor_walks()` mold), never by
 *      timing: plain-leaf writes/reads and `:children` enumerations leave it at 0.
 */

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/hazard_domain.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::mem::hazard_domain_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

tr::view::view_t val_u8(std::uint8_t b) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(1);
    seg->bytes[0] = std::byte{b};
    return tr::view::view_t::over(std::move(seg));
}

/** @brief A handler whose on_read captures @p probe — the block-lifetime instrument:
 *         the probe's use_count drops by one exactly when the seam block is destroyed. */
tr::graph::handlers_t probed_handlers(std::shared_ptr<int> probe) {
    tr::graph::handlers_t h;
    h.on_read = [probe = std::move(probe)]() -> tr::graph::result_t<tr::view::rope_t> {
        return tr::view::rope_t{val_u8(0x42)};
    };
    return h;
}

// ---------------------------------------------------------------------------
// (1) Churn: live seam blocks stay BOUNDED where the parked vector grew forever.
void test_churn_is_bounded() {
    std::printf("#576: retire churn keeps live seam blocks bounded:\n");
    auto probe = std::make_shared<int>(0);
    constexpr std::size_t kCycles = 5 * hazard_domain_t::kRetireBatch;
    long peak_live = 0;
    {
        graph_t g;
        bool all_cycles_ok = true;
        for (std::size_t i = 0; i < kCycles; ++i) {
            const auto vh =
                g.try_register_vertex(path_t("/churn"), role_t::HANDLER, probed_handlers(probe));
            if (!vh.has_value() || !g.retire(*vh).has_value()) {
                all_cycles_ok = false;
                break;
            }
            const long live = probe.use_count() - 1;  // blocks still holding a capture
            if (live > peak_live) peak_live = live;
        }
        check(all_cycles_ok, "every register/retire cycle succeeded (revive per RFC-0009 §B.4)");
        // No reader is ever mid-callback here, so every scan frees its whole batch: the
        // live count can never exceed one scan batch. The old vector reached kCycles.
        check(peak_live <= static_cast<long>(hazard_domain_t::kRetireBatch),
              "peak live seam blocks <= the domain scan batch (was: one per retire, forever)");
        check(g.seam_domain().retired_count() <= hazard_domain_t::kRetireBatch,
              "the domain's own accounting agrees (parked records <= one batch)");
    }
    // (and the NEW ~graph_t runs the final sweep — nothing retired outlives the graph)
    check(probe.use_count() == 1, "~graph_t final sweep freed every remaining block");
}

// ---------------------------------------------------------------------------
// (2) A reader in a SLOW callback vs a concurrent retire: the announced block survives
//     the callback and is freed after it returns. TSan/ASan carry the race half.
void test_slow_reader_vs_retire() {
    std::printf("ADR-0072: an announced seam block outlives a concurrent retire + scans:\n");
    graph_t g;
    auto probe = std::make_shared<int>(0);
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};

    tr::graph::handlers_t h;
    h.on_read = [probe, &entered, &release]() -> tr::graph::result_t<tr::view::rope_t> {
        entered.store(true, std::memory_order_release);
        entered.notify_one();
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        return tr::view::rope_t{val_u8(0x77)};
    };
    const vertex_handle_t slow = g.register_vertex(path_t("/slow"), role_t::HANDLER, std::move(h));

    std::thread reader([&g] {
        const auto r = g.read(path_t("/slow"));
        // The read began before the retire, so the pinned seam must have served it.
        if (!r.has_value()) ++g_failures;
    });
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();

    // Retire the seam the reader is INSIDE, then churn far past the scan threshold so
    // several scans run while the callback is still in flight.
    check(g.retire(slow).has_value(), "retire(/slow) while its on_read is in flight");
    auto churn_probe = std::make_shared<int>(0);
    for (std::size_t i = 0; i < 3 * hazard_domain_t::kRetireBatch; ++i) {
        const auto vh =
            g.try_register_vertex(path_t("/other"), role_t::HANDLER, probed_handlers(churn_probe));
        if (vh.has_value()) (void)g.retire(*vh);
    }
    check(probe.use_count() == 2,
          "the announced block survived retire + scans while the callback ran");

    release.store(true, std::memory_order_release);
    reader.join();

    // With the announcement cleared, the next threshold crossing reclaims it.
    for (std::size_t i = 0; i < 2 * hazard_domain_t::kRetireBatch; ++i) {
        const auto vh =
            g.try_register_vertex(path_t("/other"), role_t::HANDLER, probed_handlers(churn_probe));
        if (vh.has_value()) (void)g.retire(*vh);
    }
    check(probe.use_count() == 1, "the block was freed by a scan AFTER the callback returned");
}

// ---------------------------------------------------------------------------
// (2b) Many lock-free readers race retire/revive churn — the retire_test race, kept
//      here too so THIS suite fails alone if the announce loop regresses. TSan-load.
void test_concurrent_read_vs_retire_churn() {
    std::printf("TSAN: readers race retire/revive churn through the domain:\n");
    graph_t g;
    auto probe = std::make_shared<int>(0);
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/dev/h"), role_t::HANDLER, probed_handlers(probe));

    std::atomic<bool> stop{false};
    std::atomic<long> reads{0};
    std::atomic<long> churns{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                // The :children form, like retire_test's race: it reaches the seam
                // WITHOUT the read(vh) role() fork, whose unsynchronized role_ byte is a
                // separate, pre-existing finding this suite does not adjudicate. The
                // announce → protect → clear window under test is identical.
                (void)g.read(path_t("/dev/h:children"));
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    std::thread churner([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            if (auto vh = g.find(path_t("/dev/h").key()); vh.has_value()) (void)g.retire(*vh);
            (void)g.try_register_vertex(path_t("/dev/h"), role_t::HANDLER, probed_handlers(probe));
            churns.fetch_add(1, std::memory_order_relaxed);
        }
    });
    while (reads.load() < 20000 || churns.load() < 2000) { /* spin until well-mixed */
    }
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& r : readers) r.join();
    churner.join();
    // Announced survivors of a scan re-park, so the bound is a batch plus one batch of
    // survivors (plus the currently-registered block) — still a CONSTANT, against the
    // old vector's one-block-per-retire growth (churns here is in the thousands).
    const long live = probe.use_count() - 1;
    check(live <= 2 * static_cast<long>(hazard_domain_t::kRetireBatch) + 1,
          "after the storm, live blocks are bounded by the domain (not by churn count)");
    check(true, "read-vs-retire churn through the domain: no crash / UAF / race");
}

// ---------------------------------------------------------------------------
// (3) The gate: no ext block ⇒ no announce — structural, via seam_announces().
void test_no_announce_without_ext() {
    std::printf("ADR-0072 §4: the placeholder / no-ext path executes no announce:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/plain"), role_t::STORED_VALUE);
    const vertex_handle_t leaf = g.register_vertex(path_t("/plain/leaf"), role_t::STORED_VALUE);

    check(g.write(leaf, val_u8(0x01)).has_value(), "plain-leaf write succeeds");
    check(g.read(path_t("/plain/leaf")).has_value(), "plain-leaf read succeeds");
    check(g.read(path_t("/plain:children")).has_value(), ":children enumerates (no seam)");
    check(g.seam_announces() == 0,
          "no-ext writes/reads/:children opened ZERO announce windows (the gate held)");

    // And the counter is live — a handler-bearing vertex's ops do announce.
    auto probe = std::make_shared<int>(0);
    (void)g.register_vertex(path_t("/h"), role_t::HANDLER, probed_handlers(probe));
    (void)g.read(path_t("/h"));
    check(g.seam_announces() > 0, "a handler read DID announce (the instrument is live)");
}

}  // namespace

int main() {
    test_churn_is_bounded();
    test_slow_reader_vs_retire();
    test_no_announce_without_ext();
    test_concurrent_read_vs_retire_churn();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
