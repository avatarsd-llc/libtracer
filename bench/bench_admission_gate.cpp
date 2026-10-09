/**
 * @file
 * @brief The declaration-gated write rows (#2032): a write through an `on_admit` filter and a
 *        write through a payload-right table, at the 64 B .. 16 KiB payload ladder, with and
 *        without re-registration churn at a sibling address.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The gate reads a vertex's declaration off the graph's declaration list. Before #2032 every
 * registration that declared anything PREPENDED a node to that list, so re-registration churn
 * at one address pushed every OLDER declarer's node further down the walk; after it, each
 * address keeps one node. The rows therefore time a write to `/a`, declared first, while a
 * sibling `/b` has been registered and retired `churn` times.
 *
 * Rows, one per (kind, payload bytes, churn):
 *
 * - `admit`  — `/a` is a `STORED_VALUE` with an accept-as-written `on_admit`;
 * - `rights` — `/a` is a `STORED_VALUE` declaring one payload-right row for `SPEC`, written a
 *   `SPEC`-led payload, so the gate reads its declaration on every write.
 *
 * Each row reports the best-of-rounds batch mean (`ns/op`, and `Mops/s` from it), the p50 and
 * p99 of single timed writes, and the table-source bytes the churn left behind (`churn_B`).
 * Diagnostic, not a perf gate. Run it pinned, one arm at a time, A/B against the base tree.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <optional>
#include <span>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::acl_right_t;
using tr::graph::admission_t;
using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::payload_right_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::graph::write_ctx_t;

/** @brief A heap source that counts live bytes, injected as the graph's root. */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    counting_source_t() noexcept : tr::mem::block_source_t("bench") {}
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        void* const p = ::operator new(bytes, std::align_val_t{align}, std::nothrow);
        if (p != nullptr) in_use_ += bytes;
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        in_use_ -= bytes;
        ::operator delete(p, bytes, std::align_val_t{align});
    }
    [[nodiscard]] tr::mem::source_stats_t stats() const noexcept override {
        tr::mem::source_stats_t s;
        s.in_use = in_use_;
        return s;
    }

   private:
    std::size_t in_use_ = 0; /**< @brief Outstanding bytes. */
};

/** @brief The accept-as-written filter both kinds' sibling churn installs. */
admission_t accept(void*, const tr::graph::value_t&, const write_ctx_t&) { return std::nullopt; }

/** @brief A one-link rope over an owned heap copy of @p bytes. */
tr::view::rope_t rope_over(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::rope_t{tr::view::view_t::over(std::move(seg))};
}

/** @brief A payload of @p bytes: a `SPEC` TLV for the rights kind, raw bytes otherwise. */
tr::view::rope_t payload(std::size_t bytes, bool spec) {
    const std::vector<std::byte> body(bytes, std::byte{0x5a});
    if (!spec) return rope_over(body);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, tr::wire::type_t::SPEC, tr::wire::opt_t{}, body);
    return rope_over(out);
}

/** @brief One row's figures. */
struct row_t {
    double ns_per_op = 0;        /**< @brief Best-of-rounds batch mean. */
    std::uint64_t p50 = 0;       /**< @brief Single-write median, ns. */
    std::uint64_t p99 = 0;       /**< @brief Single-write p99, ns. */
    std::size_t churn_bytes = 0; /**< @brief Table bytes the churn left live. */
};

/** @brief Build the topology for one row and time writes to `/a`. */
row_t run_row(bool rights, std::size_t bytes, int churn) {
    counting_source_t src;
    row_t out;
    {
        graph_t g(src);
        const payload_right_t rows[] = {
            payload_right_t{tr::wire::type_t::SPEC, acl_right_t::WRITE}};
        handlers_t ha;
        if (!rights) ha.on_admit = {&accept, nullptr};
        const path_t a_path("/a");
        const auto a = g.register_vertex_key(
            a_path.key(), role_t::STORED_VALUE, ha, {},
            rights ? std::span<const payload_right_t>(rows) : std::span<const payload_right_t>{});
        if (!a) return out;
        // The sibling churn: the same declaration kind, registered and retired `churn` times.
        const path_t b_path("/b");
        const std::size_t before = src.stats().in_use;
        for (int i = 0; i < churn; ++i) {
            handlers_t hb;
            if (!rights) hb.on_admit = {&accept, nullptr};
            const auto b = g.register_vertex_key(b_path.key(), role_t::STORED_VALUE, hb, {},
                                                 rights ? std::span<const payload_right_t>(rows)
                                                        : std::span<const payload_right_t>{});
            if (b) (void)g.retire(*b);
        }
        out.churn_bytes = src.stats().in_use - before;

        const tr::view::rope_t value = payload(bytes, rights);
        auto op = [&] { bench::do_not_optimize(g.write(*a, value)); };
        const std::size_t batch = bench::calibrate_batch_for_window(op);
        double best = 1e30;
        for (int round = 0; round < 7; ++round) {
            const std::uint64_t t0 = bench::now_ns();
            for (std::size_t i = 0; i < batch; ++i) op();
            best = std::min(best,
                            static_cast<double>(bench::now_ns() - t0) / static_cast<double>(batch));
        }
        out.ns_per_op = best;
        std::vector<std::uint64_t> samples(20000);
        for (std::uint64_t& s : samples) {
            const std::uint64_t t0 = bench::now_ns();
            op();
            s = bench::now_ns() - t0;
        }
        std::sort(samples.begin(), samples.end());
        out.p50 = samples[samples.size() / 2];
        out.p99 = samples[samples.size() * 99 / 100];
    }
    return out;
}

}  // namespace

int main() {
    constexpr std::size_t kBytes[] = {64, 1024, 4096, 16384};
    constexpr int kChurn[] = {0, 256};
    std::printf("%-8s %7s %6s %10s %8s %8s %8s %9s\n", "kind", "bytes", "churn", "ns/op", "Mops/s",
                "p50_ns", "p99_ns", "churn_B");
    for (const bool rights : {false, true})
        for (const std::size_t bytes : kBytes)
            for (const int churn : kChurn) {
                const row_t r = run_row(rights, bytes, churn);
                std::printf("%-8s %7zu %6d %10.1f %8.2f %8llu %8llu %9zu\n",
                            rights ? "rights" : "admit", bytes, churn, r.ns_per_op,
                            1e3 / r.ns_per_op, static_cast<unsigned long long>(r.p50),
                            static_cast<unsigned long long>(r.p99), r.churn_bytes);
            }
    return 0;
}
