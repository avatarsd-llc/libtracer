/**
 * @file
 * @brief Unit tests for the `std::pmr` adapter over the block seam (mem_source_pmr.hpp, #873).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The adapter runs ONE direction — `block_source_t` -> `std::pmr::memory_resource` — and
 * the reverse is structurally forbidden (`memory_resource::allocate` is `returns_nonnull`
 * and signals only by throwing, which is the defect ADR-0065 created the block seam to
 * escape). So what these tests pin is not "an allocator works": it is the three properties
 * that make the adapter worth shipping at all —
 *
 *  - **pass-through**: every byte a `std::pmr` container takes is traced to the injected
 *    source, so the deployer's slab is where the container actually lives;
 *  - **sized-release fidelity**: `std::pmr`'s sized+aligned `deallocate` round-trips the
 *    seam's header-free `release` contract, so a `pool_source_t` under the adapter
 *    RECYCLES rather than growing monotonically;
 *  - **the family-5 consumer**: `tr::net::can_reassembly_t`'s two `std::pmr::map`s hold a
 *    refcounted `tr::view::view_t`, which fails `block_array_t`'s trivially-copyable /
 *    trivially-destructible assertions and therefore CANNOT be retyped — this adapter is
 *    what puts that store on a bounded slab.
 *
 * Plus the boundary itself, stated as a test rather than only as a doc comment: the
 * adapter throws where the seam would have returned `nullptr`.
 *
 * THE ABLATION (run by hand; see core/tests/CMakeLists.txt for the exact command):
 * `-DLIBTRACER_ABLATE_PMR_ADAPTER` hands the containers `std::pmr::new_delete_resource()`
 * instead of the adapter, and sections 2/3/4/5 go RED — a guard that cannot redden is not
 * a guard.
 */

#include "libtracer/mem_source_pmr.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory_resource>
#include <new>
#include <span>
#include <type_traits>
#include <vector>

#include "libtracer/can_reassembly.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/view.hpp"
#include "libtracer/view_can.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;

/**
 * @brief A `block_source_t` that counts what passes through it and forwards to a delegate.
 *
 * The census shape `bench/bench_failable_census.cpp`'s `counting_source_t` already uses:
 * the counting lives in the SOURCE, never in the adapter — route-handle-pattern rule 5
 * forbids a second refusal vocabulary, and an atomic counter on the adapter would put a
 * shared RMW on an allocation path.
 */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    explicit counting_source_t(tr::mem::block_source_t& down) noexcept
        : tr::mem::block_source_t("counting"), down_(&down) {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        void* const p = down_->try_alloc(bytes, align);
        if (p == nullptr) return nullptr;
        ++allocs_;
        bytes_ += bytes;
        live_ += bytes;
        last_align_ = align;
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        ++releases_;
        live_ -= bytes;
        down_->release(p, bytes, align);
    }

    std::size_t allocs_ = 0;     /**< @brief Blocks handed out. */
    std::size_t releases_ = 0;   /**< @brief Blocks returned. */
    std::size_t bytes_ = 0;      /**< @brief Cumulative bytes served. */
    std::size_t live_ = 0;       /**< @brief Bytes served but not yet returned. */
    std::size_t last_align_ = 0; /**< @brief Alignment of the most recent request. */

   private:
    tr::mem::block_source_t* down_;
};

/**
 * @brief The resource a container under test is handed — the ABLATION seam.
 *
 * Unablated it is the adapter; ablated it is the global heap, which is exactly the state
 * the adapter exists to leave behind. Every check that asserts "the bytes came from the
 * injected source" fails in the ablated build, which is what makes those checks guards.
 */
[[nodiscard]] std::pmr::memory_resource* under_test(tr::mem::source_resource_t& a) noexcept {
#if defined(LIBTRACER_ABLATE_PMR_ADAPTER)
    static_cast<void>(a);
    return std::pmr::new_delete_resource();
#else
    return &a;
#endif
}

/** @brief An over-aligned element, so a container's request carries an alignment worth checking.
 */
struct alignas(64) wide_t {
    std::uint64_t v = 0; /**< @brief Payload; the alignment is the point, not the value. */
};

/** @brief A heap segment holding a copy of @p src, so views over it can be reassembled. */
tr::view::view_t view_over(const std::vector<std::byte>& src) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) seg->bytes[i] = src[i];
    return tr::view::view_t::over(std::move(seg));
}

/** @brief Flatten a rope's logical bytes into a contiguous vector for comparison. */
std::vector<std::byte> rope_bytes(const tr::view::rope_t& r) {
    std::vector<std::byte> out;
    r.walk([&](std::span<const std::byte> chunk) {
        out.insert(out.end(), chunk.begin(), chunk.end());
    });
    return out;
}

/** @brief A 0,1,2,… byte ramp of length @p n. */
std::vector<std::byte> ramp(std::size_t n) {
    std::vector<std::byte> v;
    v.reserve(n);
    for (std::size_t i = 0; i < n; ++i) v.push_back(static_cast<std::byte>(i & 0xFF));
    return v;
}

/**
 * @brief The adapter IS a `std::pmr::memory_resource` — that is the whole point of it.
 */
static_assert(std::is_base_of_v<std::pmr::memory_resource, tr::mem::source_resource_t>,
              "source_resource_t must be usable wherever a std::pmr::memory_resource is");

/**
 * @brief …and it must NEVER pass where a FAILABLE source is wanted.
 *
 * The adapter's boundary is a throw/abort, so a peer-provoked path that asked for a
 * `block_source_t` and silently got this would be back at the ADR-0065 defect. Making the
 * slip a compile error is the same argument `mem_source_test.cpp` makes in the other
 * direction for `block_source_t` against `memory_resource`.
 */
static_assert(!std::is_convertible_v<tr::mem::source_resource_t*, tr::mem::block_source_t*>,
              "a source_resource_t* must not pass where a block_source_t* is wanted");
static_assert(std::is_final_v<tr::mem::source_resource_t>,
              "the adapter is a leaf: no subclass may reinterpret its boundary");

}  // namespace

int main() {
    std::printf("mem_source_pmr — the std::pmr adapter over the block seam (#873):\n");
#if defined(LIBTRACER_ABLATE_PMR_ADAPTER)
    std::printf("  *** ABLATED: containers are on std::pmr::new_delete_resource() ***\n");
#endif

    // --- 1. The seam's own contract: identity, and the source it reports. ---
    {
        counting_source_t counting(tr::mem::heap_source());
        tr::mem::source_resource_t adapter{counting};
        tr::mem::source_resource_t other{counting};

        check(&adapter.source() == &counting, "the adapter reports the source it was given");
        check(adapter.is_equal(adapter), "a resource equals itself");
        check(!adapter.is_equal(other),
              "two adapters over ONE source compare unequal (address identity, not RTTI)");
        check(!adapter.is_equal(*std::pmr::new_delete_resource()),
              "the adapter is not equal to the global heap resource");
    }

    // --- 2. Pass-through: every byte a std::pmr container takes is the source's. ---
    {
        counting_source_t counting(tr::mem::heap_source());
        tr::mem::source_resource_t adapter{counting};
        {
            std::pmr::vector<int> v{under_test(adapter)};
            for (int i = 0; i < 512; ++i) v.push_back(i);
            check(v.size() == 512, "the container holds what was put in it");
            check(v[0] == 0 && v[511] == 511, "…and its elements are intact");
            check(counting.allocs_ > 0, "the container's growth drew from the injected source");
            check(counting.live_ >= 512 * sizeof(int),
                  "the live bytes cover the container's storage");
        }
        check(counting.releases_ == counting.allocs_,
              "every block the container took was returned to the source");
        check(counting.live_ == 0, "nothing is left live at scope exit");
    }

    // --- 3. Alignment: an over-aligned element's request reaches the source intact. ---
    {
        counting_source_t counting(tr::mem::heap_source());
        tr::mem::source_resource_t adapter{counting};
        std::pmr::vector<wide_t> v{under_test(adapter)};
        v.push_back(wide_t{7});
        check(counting.last_align_ >= alignof(wide_t),
              "the source sees the element type's alignment, not a default");
        check(reinterpret_cast<std::uintptr_t>(v.data()) % alignof(wide_t) == 0,
              "the block handed back honours that alignment");
        check(v[0].v == 7, "the over-aligned element round-trips");
    }

    // --- 4. SIZED-RELEASE FIDELITY: a pool_source_t under the adapter RECYCLES.
    // This is the load-bearing one. std::pmr's deallocate carries the original size and
    // alignment; block_source_t::release requires exactly that pair, which is what lets a
    // pool block carry no header. If the round-trip were lossy the pool would grow
    // monotonically and `used()` would climb with every churn cycle. ---
    {
        std::array<std::byte, 32 * 1024> slab{};
        std::array<tr::mem::size_class_t, 8> classes{};
        tr::mem::pool_source_t<> pool{slab, classes};
        tr::mem::source_resource_t adapter{pool};

        std::size_t after_first = 0;
        bool contents_ok = true;
        bool emptied_ok = true;
        for (int cycle = 0; cycle < 32; ++cycle) {
            std::pmr::map<int, int> m{under_test(adapter)};
            for (int i = 0; i < 48; ++i) m.emplace(i, i * 3);
            if (m.size() != 48 || m.at(47) != 141) contents_ok = false;
            for (int i = 0; i < 48; ++i) m.erase(i);
            if (!m.empty()) emptied_ok = false;
            if (cycle == 0) after_first = pool.used();
        }
        check(contents_ok, "every churn cycle's map holds its 48 entries");
        check(emptied_ok, "…and every cycle empties again");
        check(after_first > 0, "the map's nodes came out of the injected slab");
        check(after_first > 0 && pool.used() == after_first,
              "32 churn cycles carve no more slab than the first — the blocks RECYCLE");
        check(pool.overflowed() == 0, "no block was lost: the class span was big enough");
        check(pool.classes_used() <= classes.size(), "the class table stayed within its span");
    }

    // --- 5. THE FAMILY-5 CONSUMER, unmodified: can_reassembly_t over the adapter.
    // Its two std::pmr::map's hold a refcounted view_t, so block_array_t's static asserts
    // reject them and the store cannot be retyped — that is precisely why an adapter is
    // owed. pool_source_t has NO upstream, so if the drive below completes at all, every
    // byte of the reassembly structure came from this slab and none escaped to the process
    // heap. (The slice PAYLOADS are heap segments built by the fixture; they are the
    // transport's bytes, not the store's, and mem_backend_t is need C — deliberately out
    // of ADR-0079's scope.) ---
    {
        std::array<std::byte, 64 * 1024> slab{};
        std::array<tr::mem::size_class_t, 8> classes{};
        tr::mem::pool_source_t<> pool{slab, classes};
        tr::mem::source_resource_t adapter{pool};

        const std::vector<std::byte> payload = ramp(20);
        const tr::view::view_t pv = view_over(payload);
        const auto slice = [&](std::size_t i) {
            return tr::view::can_frame_at(pv, tr::view::can_frame_mode_t::CLASSIC, i);
        };
        const auto key_ts = [](std::uint64_t ts) {
            tr::net::reassembly_key_t k;
            k.ts = ts;
            return k;
        };

        tr::net::can_reassembly_t reasm{under_test(adapter), /*max_groups=*/2};
        const std::size_t n = tr::view::can_frame_count(pv, tr::view::can_frame_mode_t::CLASSIC);
        check(n == 3, "a 20-byte payload splits into 3 classic CAN data fields");

        // Out-of-order arrival, then the advertise's slice count, then assembly.
        reasm.set_now(1000);
        reasm.add_slice(key_ts(1), 2, slice(2));
        reasm.add_slice(key_ts(1), 0, slice(0));
        check(reasm.has_interior_gap(key_ts(1)), "the missing interior slice is visible");
        reasm.add_slice(key_ts(1), 1, slice(1));
        reasm.set_expected_count(key_ts(1), static_cast<std::uint32_t>(n));
        check(reasm.is_complete(key_ts(1)), "the group completes once every index is present");
        const auto assembled = reasm.assemble(key_ts(1));
        check(assembled.has_value(), "a complete group assembles");
        check(assembled && rope_bytes(*assembled) == payload,
              "the rope reproduces the payload byte-for-byte, zero copies");
        check(pool.used() > 0, "the reassembly structure lives in the injected slab");
        check(pool.overflowed() == 0, "every freed node went back onto its class list");

        // The bound still bounds: a third live group evicts the oldest and ticks the
        // counter. The store's policy is untouched by where its bytes come from.
        reasm.add_slice(key_ts(2), 0, slice(0));
        check(reasm.dropped_groups() == 0, "no eviction while within the group bound");
        reasm.add_slice(key_ts(3), 0, slice(0));
        check(reasm.dropped_groups() == 1, "a 3rd group evicts one — a bounded drop, not an OOM");
        check(!reasm.contains(key_ts(1)), "the oldest group was the one evicted");

        // The stale sweep frees a group that stopped making progress, and its blocks go
        // back to the pool rather than being pinned for the process's life (#912).
        reasm.set_now(9000);
        check(reasm.sweep_stale(100) == 2, "both idle groups age out");
        const std::size_t plateau = pool.used();
        for (int i = 0; i < 16; ++i) {
            reasm.add_slice(key_ts(100 + static_cast<std::uint64_t>(i)), 0, slice(0));
        }
        check(pool.used() == plateau, "the swept groups' blocks were recycled, not re-carved");
    }

    // --- 6. THE BOUNDARY, stated as a test. A bump source with null_source() upstream is a
    // HARD bound; past it the adapter has nothing to answer with but a throw. This is what
    // "placement and bounding, NOT failability" means at runtime, and why a peer-provoked
    // store migrates onto block_array_t instead of onto this. ---
#if defined(__cpp_exceptions) && __cpp_exceptions
    {
        std::array<std::byte, 128> buf{};
        tr::mem::bump_source_t bump{buf, tr::mem::null_source()};
        tr::mem::source_resource_t adapter{bump};

        std::pmr::vector<int> v{&adapter};
        v.push_back(1);
        v.push_back(2);
        const std::size_t size_before = v.size();
        const std::size_t cap_before = v.capacity();

        bool threw = false;
        try {
            v.reserve(100000);  // far past the 128-byte hard bound
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        check(threw, "past the hard bound the adapter throws bad_alloc — its boundary");
        check(v.size() == size_before && v.capacity() == cap_before,
              "the container is unchanged by the refusal");
        check(v[0] == 1 && v[1] == 2, "…and its elements survive it");
    }
#else
    std::printf("  (boundary check skipped: -fno-exceptions, where the boundary is abort())\n");
#endif

    return tr::testing::summary("mem_source_pmr");
}
