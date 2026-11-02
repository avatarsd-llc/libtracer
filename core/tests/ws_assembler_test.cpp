/**
 * @file
 * @brief #2483 — `ws_assembler_t`, the one RFC 6455 reassembly, pinned at its header by one
 *        table: every row is a run of fragments and the outcome each one must produce.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The rows cover what every link that feeds the assembler relies on: a stray CONT is dropped,
 * a BINARY mid-assembly drops the stale message and keeps the new one, an empty first
 * fragment opens a message, fragments whose charged cost passes the cap are OVERSIZE, a
 * refused chain is REFUSED and the link re-syncs at the next BINARY, and the charge is the
 * payload plus @ref tr::net::ws_assembler_t::kFragmentCost per fragment, to the byte.
 */

#include "libtracer/ws_assembler.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source.hpp"

namespace {

using tr::net::ws_assembler_t;
using status_t = ws_assembler_t::status_t;
using op_t = tr::net::ws::opcode_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief A heap-backed source that refuses every draw while @ref refusing is set. */
class refusing_source_t final : public tr::mem::block_source_t {
   public:
    refusing_source_t() noexcept : block_source_t("refusing") {}
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        return refusing ? nullptr : tr::mem::heap_source().try_alloc(bytes, align);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::heap_source().release(p, bytes, align);
    }
    bool refusing = false; /**< @brief Refuse every draw while set. */
};

/** @brief One fragment fed to the assembler, and the outcome it must produce. */
struct step_t {
    op_t op;           /**< @brief BINARY or CONT. */
    bool fin;          /**< @brief Its FIN bit. */
    std::size_t len;   /**< @brief Its payload bytes (each byte is its fragment index). */
    status_t want;     /**< @brief The status @ref ws_assembler_t::on_piece must answer. */
    std::size_t bytes; /**< @brief On COMPLETE: the message's total length. */
    std::size_t first; /**< @brief On COMPLETE: the fragment index its first byte carries. */
};

/** @brief One row: a name, the cap, whether the source refuses, and the run. */
struct row_t {
    const char* name;
    std::size_t cap;
    bool refuse;
    std::vector<step_t> steps;
};

constexpr std::size_t kCost = ws_assembler_t::kFragmentCost;
constexpr std::size_t kBig = 1u << 20;

/** @brief A @p len-byte owning fragment, every byte @p fill (an empty view for 0 bytes). */
tr::view::view_t piece(std::size_t len, std::size_t fill) {
    if (len == 0) return {};
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(len);
    std::memset(seg->bytes.data(), static_cast<int>(fill & 0xFF), len);
    return tr::view::view_t{std::move(seg), 0, len};
}

void run(const row_t& row) {
    std::printf("%s:\n", row.name);
    refusing_source_t src;
    src.refusing = row.refuse;
    ws_assembler_t a(src);
    std::size_t i = 0;
    for (const step_t& s : row.steps) {
        ws_assembler_t::result_t r = a.on_piece(s.op, s.fin, piece(s.len, i), row.cap);
        bool ok = r.status == s.want;
        if (ok && s.want == status_t::COMPLETE) {
            const tr::view::view_t flat = r.message.materialize();
            ok = flat.length == s.bytes &&
                 (s.bytes == 0 || flat.bytes()[0] == static_cast<std::byte>(s.first));
        }
        // Idle after every outcome but PENDING: a link that delivers touches nothing after.
        if (s.want != status_t::PENDING) ok = ok && !a.assembling() && a.charged == 0;
        char what[64];
        std::snprintf(what, sizeof what, "fragment %zu answers as the table says", i);
        check(ok, what);
        ++i;
    }
}

}  // namespace

int main() {
    std::printf("ws_assembler_t table (#2483), kFragmentCost = %zu:\n", kCost);
    const std::vector<row_t> rows = {
        {"a stray CONT is dropped, and the next BINARY opens a message",
         kBig,
         false,
         {{op_t::CONT, true, 4, status_t::PENDING, 0, 0},
          {op_t::BINARY, false, 2, status_t::PENDING, 0, 0},
          {op_t::CONT, true, 3, status_t::COMPLETE, 5, 1}}},
        {"a BINARY mid-assembly drops the stale message and keeps the new one",
         kBig,
         false,
         {{op_t::BINARY, false, 7, status_t::PENDING, 0, 0},
          {op_t::BINARY, false, 2, status_t::PENDING, 0, 0},
          {op_t::CONT, true, 2, status_t::COMPLETE, 4, 1}}},
        {"an empty first fragment opens the message",
         kBig,
         false,
         {{op_t::BINARY, false, 0, status_t::PENDING, 0, 0},
          {op_t::CONT, false, 0, status_t::PENDING, 0, 0},
          {op_t::CONT, true, 3, status_t::COMPLETE, 3, 2}}},
        {"fragments whose charged cost passes the cap are OVERSIZE, then re-sync",
         3 * kCost,
         false,
         {{op_t::BINARY, false, 0, status_t::PENDING, 0, 0},
          {op_t::CONT, false, 0, status_t::PENDING, 0, 0},
          {op_t::CONT, false, 0, status_t::PENDING, 0, 0},
          {op_t::CONT, false, 0, status_t::OVERSIZE, 0, 0},
          {op_t::CONT, true, 1, status_t::PENDING, 0, 0},
          {op_t::BINARY, false, 1, status_t::PENDING, 0, 0},
          {op_t::CONT, true, 1, status_t::COMPLETE, 2, 5}}},
        {"a refused chain is REFUSED, and its CONT is stray after it",
         kBig,
         true,
         {{op_t::BINARY, false, 2, status_t::REFUSED, 0, 0},
          {op_t::CONT, true, 2, status_t::PENDING, 0, 0}}},
        {"the charge is payload + kFragmentCost per fragment: exactly at the cap completes",
         (5 + kCost) + (6 + kCost),
         false,
         {{op_t::BINARY, false, 5, status_t::PENDING, 0, 0},
          {op_t::CONT, true, 6, status_t::COMPLETE, 11, 0}}},
        {"the charge is payload + kFragmentCost per fragment: one byte past it is OVERSIZE",
         (5 + kCost) + (6 + kCost) - 1,
         false,
         {{op_t::BINARY, false, 5, status_t::PENDING, 0, 0},
          {op_t::CONT, true, 6, status_t::OVERSIZE, 0, 0}}},
    };
    for (const row_t& row : rows) run(row);

    std::printf("charged tracks the arithmetic between fragments:\n");
    refusing_source_t src;
    ws_assembler_t a(src);
    (void)a.on_piece(op_t::BINARY, false, piece(5, 0), kBig);
    check(a.assembling() && a.charged == 5 + kCost, "one fragment charges its bytes + kCost");
    (void)a.on_piece(op_t::CONT, false, piece(0, 0), kBig);
    check(a.charged == 5 + 2 * kCost, "an empty fragment charges kCost alone");

    std::printf("%s\n", g_failures == 0 ? "OK" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
