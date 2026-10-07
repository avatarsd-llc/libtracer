/**
 * @file
 * @brief #1640 — what one value-less HANDLER vertex draws from the graph's injected source at
 *        registration, with and without a describe table.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A HANDLER vertex holding only `on_read` / `on_write` latches no value and owns no ring, so
 * it should cost what a plain leaf costs plus its two `{fn, ctx}` seams, not the whole cold
 * extension block a stateful vertex carries. A describe table borrowed from static storage
 * should add no declaration bytes at all (ADR-0058).
 *
 * Measured at the public seam: a counting pass-through `block_source_t` injected into the
 * graph, `kN` registrations averaged so the graph's own table growth amortises away, and each
 * HANDLER shape read as its SURCHARGE over a plain `STORED_VALUE` leaf registered the same way.
 */

#include <array>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;

using tr::testing::check;

/** @brief A pass-through block source that counts the live balance it serves. */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    /** @brief Name it so a census can tell it apart from the process heap. */
    counting_source_t() noexcept : tr::mem::block_source_t("counting") {}

    long long live_allocs = 0; /**< @brief Allocations not yet released. */
    long long live_bytes = 0;  /**< @brief Bytes not yet released. */

    /** @brief Serve from the process heap source, counting. */
    [[nodiscard]] void* try_alloc(std::size_t n, std::size_t align) noexcept override {
        void* const p = tr::mem::heap_source().try_alloc(n, align);
        if (p == nullptr) return nullptr;
        ++live_allocs;
        live_bytes += static_cast<long long>(n);
        return p;
    }
    /** @brief Release to the process heap source, counting. */
    void release(void* p, std::size_t n, std::size_t align) noexcept override {
        --live_allocs;
        live_bytes -= static_cast<long long>(n);
        tr::mem::heap_source().release(p, n, align);
    }
};

/** @brief What @ref kN registrations of one shape leave live, in total. */
struct cost_t {
    long long bytes = 0;  /**< @brief Live bytes. */
    long long allocs = 0; /**< @brief Live blocks. */
};

/** @brief Registrations averaged per shape: enough that table growth amortises away. */
constexpr long long kN = 256;

/** @brief A static describe table, the flash-resident spelling an MCU owner writes. */
constexpr std::array<std::byte, 16> kDescriptor{};
constexpr std::array<tr::graph::app_field_static_t, 3> kDescribe{{
    {.name = "unit", .descriptor = kDescriptor},
    {.name = "label", .descriptor = kDescriptor},
    {.name = "range", .descriptor = kDescriptor},
}};

/** @brief The handlers a value-less HANDLER vertex carries: `on_read` and `on_write` only —
 *         a read-rate census seam and an op seam, both stateless. */
tr::graph::handlers_t op_handlers() {
    tr::graph::handlers_t h;
    h.on_read = tr::graph::thunk(
        []() -> tr::graph::result_t<tr::graph::value_ref_t> { return tr::graph::value_ref_t{}; });
    h.on_write = tr::graph::thunk(
        [](const tr::graph::value_t&, const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
            return {};
        });
    return h;
}

/**
 * @brief Register @ref kN vertices of one shape under @p dir on a fresh graph over a counting
 *        source and return the live balance they leave.
 */
template <typename Register>
cost_t measure(std::string_view dir, Register reg) {
    counting_source_t src;
    cost_t out;
    {
        graph_t g(src);
        // Warm the parent and the graph's tables outside the window.
        char pb[48];
        std::snprintf(pb, sizeof pb, "/%.*s/warm", static_cast<int>(dir.size()), dir.data());
        reg(g, path_t(pb));
        const long long b0 = src.live_bytes;
        const long long a0 = src.live_allocs;
        for (long long i = 0; i < kN; ++i) {
            std::snprintf(pb, sizeof pb, "/%.*s/v%04lld", static_cast<int>(dir.size()), dir.data(),
                          i);
            reg(g, path_t(pb));
        }
        out.bytes = src.live_bytes - b0;
        out.allocs = src.live_allocs - a0;
    }
    check(src.live_allocs == 0, "graph destruction released every block it drew");
    return out;
}

}  // namespace

/** @brief Run the HANDLER-vertex registration heap probes. */
int main() {
    std::printf("HANDLER vertex registration heap (#1640):\n");

    const cost_t leaf = measure("leaf", [](graph_t& g, const path_t& p) {
        (void)g.register_vertex(p, role_t::STORED_VALUE);
    });
    const cost_t op = measure("op", [](graph_t& g, const path_t& p) {
        (void)g.register_vertex(p, role_t::HANDLER, op_handlers());
    });
    const cost_t op_described = measure("opd", [](graph_t& g, const path_t& p) {
        (void)g.register_vertex(p, role_t::HANDLER, op_handlers(), {.app_fields = kDescribe});
    });

    const cost_t op_owned = measure("opo", [](graph_t& g, const path_t& p) {
        std::vector<tr::graph::app_field_t> owned;
        for (const auto& f : kDescribe)
            owned.push_back({.name = std::string(f.name),
                             .descriptor = {f.descriptor.begin(), f.descriptor.end()}});
        (void)g.register_vertex(p, role_t::HANDLER, op_handlers(), {.app_fields = owned});
    });

    // Per-vertex surcharges over a plain leaf registered the same way: the graph's own table
    // growth is identical across shapes, so it cancels exactly.
    const auto per = [](const cost_t& c, const cost_t& base) {
        return cost_t{(c.bytes - base.bytes) / kN, (c.allocs - base.allocs) / kN};
    };
    const cost_t op_x = per(op, leaf);
    const cost_t desc_x = per(op_described, op);
    const cost_t owned_x = per(op_owned, op);
    std::printf("  plain leaf:                  %lld B / %lld blocks per vertex\n", leaf.bytes / kN,
                leaf.allocs / kN);
    std::printf("  HANDLER on_read+on_write:    +%lld B / +%lld blocks over a leaf\n", op_x.bytes,
                op_x.allocs);
    std::printf("  + borrowed describe table:   +%lld B / +%lld blocks over that\n", desc_x.bytes,
                desc_x.allocs);
    std::printf("  + owning describe table:     +%lld B / +%lld blocks over that\n", owned_x.bytes,
                owned_x.allocs);

    // A value-less HANDLER draws its cold block and its two-seam block, and nothing else.
    const auto ext = static_cast<long long>(sizeof(tr::graph::vertex_ext_t));
    const auto seam = static_cast<long long>(sizeof(tr::graph::value_handlers_t));
    check(op_x.allocs == 2 && op_x.bytes == ext + seam,
          "a HANDLER draws exactly its extension block and its value seam over a leaf");
    // A borrowed describe table is viewed in place: one group block, no declaration bytes.
    check(desc_x.allocs == 1 &&
              desc_x.bytes == static_cast<long long>(sizeof(tr::graph::app_field_group_t)),
          "a borrowed describe table adds only the app-field group, never the declaration");
    if constexpr (sizeof(void*) == 8) {
        // The #1640 budget on a 64-bit host: the cold block holds only what every ext-bearing
        // vertex can need; ACL and STREAM state hang off it lazily.
        check(ext <= 48, "sizeof(vertex_ext_t) <= 48 B on a 64-bit host (#1640)");
        check(op_x.bytes <= 96, "a value-less HANDLER costs <= 96 B over a leaf (#1640)");
    }

    return tr::testing::summary("handler_vertex_heap");
}
