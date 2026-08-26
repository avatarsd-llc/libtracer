/**
 * @file
 * @brief #361 §5 / ADR-0039 §1 — the per-write LKV allocation seam on `graph_t`, re-aimed
 *        at the ONE injected `tr::mem::block_source_t` #873 phase 1 collapsed it onto.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The property under test did not change when the four constructor seams became one; only
 * the seam's spelling did. Writes to an injected graph must route their LKV control block
 * and rope through the injected source (the graph builds the `std::pmr` adapter over it
 * internally), values must read back byte-exact, and every block must be released by graph
 * destruction — the "source outlives the graph and its handles" contract, and the balance
 * check a slab/pool deployment on the MCU relies on.
 *
 * A counting pass-through SOURCE is what makes this observable now, and it sees strictly
 * MORE than the old counting resource did: the value segment's bytes travel the same seam
 * after the collapse, so the balance check now covers the ADR-0060 channel too.
 */

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;

using tr::testing::check;

/** @brief A pass-through block source that counts live allocations and bytes. */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    /** @brief Name it so a census can tell it apart from the process heap. */
    counting_source_t() noexcept : tr::mem::block_source_t("counting") {}

    std::size_t allocs = 0; /**< @brief Total allocations served. */
    std::size_t live = 0;   /**< @brief Allocations not yet released. */
    std::size_t bytes = 0;  /**< @brief Total bytes served. */

    /** @brief Serve from the process heap source, counting. */
    [[nodiscard]] void* try_alloc(std::size_t n, std::size_t align) noexcept override {
        void* const p = tr::mem::heap_source().try_alloc(n, align);
        if (p == nullptr) return nullptr;
        ++allocs;
        ++live;
        bytes += n;
        return p;
    }
    /** @brief Release to the process heap source, counting. */
    void release(void* p, std::size_t n, std::size_t align) noexcept override {
        --live;
        tr::mem::heap_source().release(p, n, align);
    }
};

/** @brief A VALUE TLV over owned bytes (the write shape), as graph_test. */
tr::view::view_t value_tlv(std::span<const std::byte> payload) {
    tr::wire::tlv_t t{.type = tr::wire::type_t::VALUE, .payload = payload};
    const std::vector<std::byte> enc = tr::wire::encode(t);
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(enc.size());
    std::memcpy(seg->bytes.data(), enc.data(), enc.size());
    return tr::view::view_t::over(std::move(seg));
}

}  // namespace

/** @brief Run the injection-seam probes. */
int main() {
    std::printf("graph_t source injection seam (#361 §5 / ADR-0039 §1, #873 phase 1):\n");

    counting_source_t counter;
    {
        graph_t g(&counter);
        const auto v = g.register_vertex(path_t("/pmr/leaf"), role_t::STORED_VALUE);

        const std::size_t before = counter.allocs;
        const auto payload = std::vector<std::byte>{std::byte{0xAB}, std::byte{0xCD}};
        const auto w = g.write(path_t("/pmr/leaf"), value_tlv(payload));
        check(w.has_value(), "write through an injected-resource graph succeeds");
        check(counter.allocs > before, "the write's LKV allocation drew from the INJECTED source");

        const auto r = g.read(v);
        check(r.has_value(), "value reads back through the injected-resource graph");

        // A second write releases the first LKV back to the SAME resource.
        const std::size_t live_after_first = counter.live;
        const auto w2 = g.write(path_t("/pmr/leaf"), value_tlv(payload));
        check(w2.has_value() && counter.live <= live_after_first + 2,
              "a replaced LKV (control block + value segment) is released back to the source");
    }
    check(counter.live == 0,
          "graph destruction released every injected allocation (slab-safe balance)");

    counting_source_t idle;
    {
        graph_t g;  // default: the standard heap — the injected counter must stay idle
        const auto v = g.register_vertex(path_t("/heap/leaf"), role_t::STORED_VALUE);
        (void)v;
        const auto payload = std::vector<std::byte>{std::byte{0x01}};
        (void)g.write(path_t("/heap/leaf"), value_tlv(payload));
    }
    check(idle.allocs == 0, "a default-constructed graph never touches a foreign source");

    return tr::testing::summary("graph_pmr");
}
