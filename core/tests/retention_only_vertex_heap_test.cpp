/**
 * @file
 * @brief #2001 — a vertex whose only declaration is its retention (`NONE`, no fields) draws
 *        nothing beyond a plain leaf, at registration or through `set_policy`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A retention-`NONE` relay latches no value and keeps no ring, so neither the cold extension
 * block nor any field state has anything to hold. Measured at the public seam: a counting
 * pass-through `block_source_t` injected into the graph, `kN` registrations averaged so the
 * graph's own table growth amortises away, and each shape read as its SURCHARGE over a plain
 * `STORED_VALUE` leaf registered the same way. A field declared later must still land, and
 * draw its blocks then.
 */

#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::retention_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;

using tr::testing::check;
using tr::testing::make_value;

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

/** @brief A VALUE TLV carrying @p payload verbatim. */
std::vector<std::byte> value_tlv(std::span<const std::byte> payload) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, tr::wire::type_t::VALUE, tr::wire::opt_t{}, payload);
    return out;
}

/** @brief Per-vertex surcharge of @p c over @p base. */
cost_t per(const cost_t& c, const cost_t& base) {
    return cost_t{(c.bytes - base.bytes) / kN, (c.allocs - base.allocs) / kN};
}

/**
 * @brief A retention-only vertex that gains a field later: the field lands and reads back,
 *        the retention holds, and the declaration draws its blocks then, not before.
 */
void test_field_declared_later(role_t role, const char* label) {
    std::printf("  %s: a field declared later\n", label);
    counting_source_t src;
    {
        graph_t g(src);
        const vertex_handle_t v =
            g.register_vertex(path_t("/x"), role, {}, {.retention = retention_t::NONE});
        const long long a0 = src.live_allocs;
        std::vector<tr::graph::app_field_t> table;
        table.push_back(
            tr::graph::app_field_t{.name = "kp", .access = tr::graph::app_access_t::RW});
        check(g.set_policy(v, {.retention = retention_t::NONE, .app_fields = std::move(table)})
                  .has_value(),
              "a retention-only vertex accepts a field table later");
        check(src.live_allocs > a0, "... and the field table draws its blocks then");
        check(g.retention(v) == retention_t::NONE, "... and the retention still holds");
        const auto field = path_t::parse("/x:settings.app.kp");
        const std::vector<std::byte> four{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        check(g.write(v, field->field(), make_value(value_tlv(four)), {}).has_value(),
              "... and a write to the new field lands");
        const auto back = g.read(v, field->field(), {});
        check(back.has_value(), "... and the field reads back");
        // The vertex itself is still a relay: a direct write is delivered, nothing retained.
        check(g.write(path_t("/x"), make_value(value_tlv(four))).has_value(),
              "... and the vertex still takes a direct write");
        check(!g.read(path_t("/x")).has_value(), "... which it does not retain");
    }
    check(src.live_allocs == 0, "graph destruction released every block it drew");
}

/** @brief A STREAM declared `NONE` at registration answers its ring observables as empty. */
void test_stream_none_observables() {
    std::printf("  STREAM NONE: ring observables with no extension block\n");
    graph_t g;
    const vertex_handle_t v =
        g.register_vertex(path_t("/s"), role_t::STREAM, {}, {.retention = retention_t::NONE});
    check(g.retention(v) == retention_t::NONE, "a STREAM declared NONE reads back NONE");
    const std::vector<std::byte> one{std::byte{7}};
    check(g.write(path_t("/s"), make_value(value_tlv(one))).has_value(),
          "a write to a NONE STREAM is delivered");
    const auto reserved = g.ring_reserved_bytes(v);
    check(reserved.has_value() && *reserved == 0, "its ring reserves no bytes");
    const auto gaps = g.stream_gaps(v);
    check(gaps.has_value() && *gaps == 0, "its ring has shed nothing");
    // Switching it back to the role default draws the block and retains again.
    check(g.set_policy(v, {}).has_value(), "the role default applies over NONE");
    check(g.retention(v) == retention_t::N, "... and the STREAM retains N again");
    check(g.write(path_t("/s"), make_value(value_tlv(one))).has_value(), "... and takes a write");
    const auto held = g.ring_reserved_bytes(v);
    check(held.has_value() && *held > 0, "... which its ring now holds");
}

}  // namespace

/** @brief Run the retention-only vertex heap probes. */
int main() {
    std::printf("Retention-only vertex heap (#2001):\n");

    const cost_t leaf = measure("leaf", [](graph_t& g, const path_t& p) {
        (void)g.register_vertex(p, role_t::STORED_VALUE);
    });
    const cost_t relay_reg = measure("rr", [](graph_t& g, const path_t& p) {
        (void)g.register_vertex(p, role_t::STORED_VALUE, {}, {.retention = retention_t::NONE});
    });
    const cost_t relay_set = measure("rs", [](graph_t& g, const path_t& p) {
        const vertex_handle_t v = g.register_vertex(p, role_t::STORED_VALUE);
        (void)g.set_policy(v, {.retention = retention_t::NONE});
    });
    const cost_t stream_reg = measure("sr", [](graph_t& g, const path_t& p) {
        (void)g.register_vertex(p, role_t::STREAM, {}, {.retention = retention_t::NONE});
    });
    const cost_t stream_n = measure(
        "sn", [](graph_t& g, const path_t& p) { (void)g.register_vertex(p, role_t::STREAM); });

    const cost_t relay_reg_x = per(relay_reg, leaf);
    const cost_t relay_set_x = per(relay_set, leaf);
    const cost_t stream_reg_x = per(stream_reg, leaf);
    const cost_t stream_n_x = per(stream_n, leaf);
    std::printf("  plain leaf:                       %lld B / %lld blocks per vertex\n",
                leaf.bytes / kN, leaf.allocs / kN);
    std::printf("  STORED_VALUE NONE at registration: +%lld B / +%lld blocks over a leaf\n",
                relay_reg_x.bytes, relay_reg_x.allocs);
    std::printf("  STORED_VALUE NONE via set_policy:  +%lld B / +%lld blocks over a leaf\n",
                relay_set_x.bytes, relay_set_x.allocs);
    std::printf("  STREAM NONE at registration:       +%lld B / +%lld blocks over a leaf\n",
                stream_reg_x.bytes, stream_reg_x.allocs);
    std::printf("  STREAM default (N, depth 1):       +%lld B / +%lld blocks over a leaf\n",
                stream_n_x.bytes, stream_n_x.allocs);

    check(relay_reg_x.bytes == 0 && relay_reg_x.allocs == 0,
          "a STORED_VALUE declared NONE at registration costs what a leaf costs");
    check(relay_set_x.bytes == 0 && relay_set_x.allocs == 0,
          "a STORED_VALUE switched to NONE by set_policy costs what a leaf costs");
    check(stream_reg_x.bytes == 0 && stream_reg_x.allocs == 0,
          "a STREAM declared NONE at registration costs what a leaf costs (#2001)");
    // The default STREAM still draws its extension block: depth lives there.
    check(stream_n_x.allocs == 1 &&
              stream_n_x.bytes == static_cast<long long>(sizeof(tr::graph::vertex_ext_t)),
          "a default STREAM draws exactly its extension block over a leaf");

    test_field_declared_later(role_t::STORED_VALUE, "STORED_VALUE NONE");
    test_field_declared_later(role_t::STREAM, "STREAM NONE");
    test_stream_none_observables();

    return tr::testing::summary("retention_only_vertex_heap");
}
