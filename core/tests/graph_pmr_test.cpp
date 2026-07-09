/**
 * @file
 * @brief #361 §5 — the ADR-0039 §1 injection seam on `graph_t`: per-write LKV
 *        allocations (control block + rope) draw from the constructor-injected
 *        `std::pmr::memory_resource`, and a default-constructed graph keeps the
 *        standard heap (zero churn).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Uses a counting pass-through resource: writes to an injected graph must route
 * allocations through it, values must read back byte-exact, and every
 * allocation must be released by graph destruction (the "resource outlives the
 * graph and its handles" contract) — the balance check is what a slab/pool
 * deployment on the MCU relies on.
 */

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief A pass-through resource that counts live allocations and bytes. */
class counting_resource_t final : public std::pmr::memory_resource {
   public:
    std::size_t allocs = 0; /**< @brief Total allocations served. */
    std::size_t live = 0;   /**< @brief Allocations not yet deallocated. */
    std::size_t bytes = 0;  /**< @brief Total bytes served. */

   private:
    /** @brief Serve from the default resource, counting. */
    void* do_allocate(std::size_t n, std::size_t align) override {
        ++allocs;
        ++live;
        bytes += n;
        return std::pmr::get_default_resource()->allocate(n, align);
    }
    /** @brief Release to the default resource, counting. */
    void do_deallocate(void* p, std::size_t n, std::size_t align) override {
        --live;
        std::pmr::get_default_resource()->deallocate(p, n, align);
    }
    /** @brief Identity equality (a stateful counter is only equal to itself). */
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override {
        return this == &o;
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
    std::printf("graph_t pmr injection seam (#361 §5 / ADR-0039 §1):\n");

    counting_resource_t counter;
    {
        graph_t g(&counter);
        const auto v = g.register_vertex(path_t("/pmr/leaf"), role_t::STORED_VALUE);

        const std::size_t before = counter.allocs;
        const auto payload = std::vector<std::byte>{std::byte{0xAB}, std::byte{0xCD}};
        const auto w = g.write(path_t("/pmr/leaf"), value_tlv(payload));
        check(w.has_value(), "write through an injected-resource graph succeeds");
        check(counter.allocs > before,
              "the write's LKV allocation drew from the INJECTED resource");

        const auto r = g.read(v);
        check(r.has_value(), "value reads back through the injected-resource graph");

        // A second write releases the first LKV back to the SAME resource.
        const std::size_t live_after_first = counter.live;
        const auto w2 = g.write(path_t("/pmr/leaf"), value_tlv(payload));
        check(w2.has_value() && counter.live <= live_after_first + 1,
              "a replaced LKV is released back to the injected resource");
    }
    check(counter.live == 0,
          "graph destruction released every injected allocation (slab-safe balance)");

    counting_resource_t idle;
    {
        graph_t g;  // default: the standard heap — the injected counter must stay idle
        const auto v = g.register_vertex(path_t("/heap/leaf"), role_t::STORED_VALUE);
        (void)v;
        const auto payload = std::vector<std::byte>{std::byte{0x01}};
        (void)g.write(path_t("/heap/leaf"), value_tlv(payload));
    }
    check(idle.allocs == 0, "a default-constructed graph never touches a foreign resource");

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
