/**
 * @file
 * @brief L0/L1 substrate tests: refcount lifetime (the canonical intrusive_ptr orderings), zero-
 *        copy subview/concat, rope serialization equivalence (the docs/reference/02 proof
 *        obligation), the view->TLV cast claim with the lifetime gap M1 left open now closed, and
 *        the bounded pool backend.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Reuses the
 * seed vectors as real TLV bytes; no JSON parser. Builds twice — once with
 * atomic refcounts, once with -DLIBTRACER_NO_ATOMIC (single-threaded mode).
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

namespace fs = std::filesystem;

using tr::testing::check;

std::vector<std::byte> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::ranges::transform(raw, out.begin(), [](char c) {
        return static_cast<std::byte>(static_cast<unsigned char>(c));
    });
    return out;
}

/**
 * @brief A backend that counts how many times destroy fires and frees the control block it is
 *        handed.
 *
 * Used to assert exact refcount lifetime.
 */
class CountingBackend final : public tr::mem::mem_backend_t {
   public:
    CountingBackend() noexcept : tr::mem::mem_backend_t("counting") {}
    void destroy(tr::view::segment_t* seg) noexcept override {
        ++destroys;
        delete seg;
    }
    int destroys = 0;
};

/** @brief Make an owned segment over `bytes` backed by `be` (refcount = 1, adopted). */
tr::view::segment_ptr_t make_segment(CountingBackend& be, std::span<std::byte> bytes) {
    return tr::view::segment_ptr_t::adopt(new tr::view::segment_t(&be, bytes));
}

void test_refcount_lifetime() {
    std::printf("Refcount lifetime (intrusive_ptr orderings):\n");
    std::array<std::byte, 8> store{};
    CountingBackend be;
    {
        tr::view::segment_ptr_t a = make_segment(be, store);
        check(a.use_count() == 1, "fresh segment use_count == 1");
        tr::view::segment_ptr_t b = a;  // clone (relaxed inc)
        check(a.use_count() == 2, "clone bumps to 2 (fan-out to subscriber 1)");
        {
            tr::view::segment_ptr_t c = b;  // clone
            check(a.use_count() == 3, "second clone -> 3 (fan-out to subscriber 2)");
            check(be.destroys == 0, "no destroy while referenced");
        }
        check(a.use_count() == 2, "release of subscriber 2 -> 2");
    }
    check(be.destroys == 1, "destroy fires exactly once when the last handle drops");
}

void test_transfer_vs_clone() {
    std::printf("Ownership transfer vs clone (docs/reference/02 §delivery):\n");
    std::array<std::byte, 4> store{};
    CountingBackend be;
    tr::view::segment_ptr_t a = make_segment(be, store);
    tr::view::segment_ptr_t moved = std::move(a);  // transfer: take the existing ref
    check(moved.use_count() == 1 && !a, "move transfers ownership, no net refcount change");
    tr::view::segment_ptr_t cloned = moved;  // clone: a new ref
    check(moved.use_count() == 2, "clone adds exactly one reference");
    check(be.destroys == 0, "still alive while a handle remains");
}

void test_zero_copy_subview_concat() {
    std::printf("Zero-copy subview / concat / iovec:\n");
    std::array<std::byte, 16> store{};
    for (std::size_t i = 0; i < store.size(); ++i) store[i] = static_cast<std::byte>(i);
    std::array<std::byte, 4> tail{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                  std::byte{0xEF}};

    tr::view::view_t whole = tr::view::view_t::over(tr::view::borrow(store));
    tr::view::view_t sub = whole.subview(4, 8);
    check(sub.bytes().data() == store.data() + 4, "subview points into the same buffer (no copy)");
    check(sub.length == 8, "subview length is exact");

    tr::view::rope_t rope(whole);
    rope.append(tr::view::view_t::over(tr::view::borrow(tail)));
    check(rope.link_count() == 2, "rope has 2 links");
    check(rope.total_length() == store.size() + tail.size(), "rope total length sums links");

    const auto iov = rope.to_iovec();
    check(iov.size() == 2 && iov[0].data() == store.data() && iov[1].data() == tail.data(),
          "to_iovec spans point into the original buffers (scatter-gather, no copy)");
}

void test_rope_equivalence(const fs::path& vroot) {
    std::printf("rope_t serialization equivalence (the docs/reference/02 proof obligation):\n");
    const std::vector<std::byte> flat = read_file(vroot / "path/path-sensor-temp" / "input.bin");
    check(!flat.empty(), "seed vector loaded");

    // Split the one TLV's bytes across two borrowed segments — a 2-link rope.
    const std::size_t cut = flat.size() / 2;
    std::vector<std::byte> part_a(flat.begin(), flat.begin() + cut);
    std::vector<std::byte> part_b(flat.begin() + cut, flat.end());
    tr::view::rope_t rope(tr::view::view_t::over(tr::view::borrow(part_a)));
    rope.append(tr::view::view_t::over(tr::view::borrow(part_b)));

    const tr::view::view_t materialized = rope.flatten();  // one copy, into a heap segment
    const auto mb = materialized.bytes();
    check(std::ranges::equal(mb, flat), "flatten(rope) reproduces the flat bytes exactly");

    const auto t_flat = tr::wire::decode(flat);
    const auto t_rope = tr::wire::decode(materialized);
    check(t_flat.has_value() && t_rope.has_value() && tr::wire::equal(*t_flat, *t_rope),
          "decode(flatten(rope)) == decode(flat)");
    // RFC-0018: a PATH body is packed records with `opt.PL = 0`, so it decodes as ONE
    // opaque node — the rope tier must see the same 12-byte body the flat tier does.
    check(t_rope.has_value() && t_rope->children.empty() && t_rope->payload.size() == 12,
          "the rope-decoded PATH carries its 12-byte packed body");
}

void test_cast_claim_outlives_source(const fs::path& vroot) {
    std::printf("Cast claim — tlv_t outlives its source buffer via the segment refcount:\n");
    tr::view::view_t v;
    {
        // Copy a seed vector into an OWNED heap segment, build a view, then let
        // the original buffer go out of scope. M1 alone would dangle here.
        const std::vector<std::byte> src = read_file(vroot / "crc/value-crc32c" / "input.bin");
        tr::view::segment_ptr_t seg = tr::view::heap_alloc(src.size());
        check(static_cast<bool>(seg), "heap segment allocated");
        std::memcpy(seg->bytes.data(), src.data(), src.size());
        v = tr::view::view_t::over(std::move(seg));
    }  // `src` freed here; `v` (and its segment copy) survives

    const auto tlv = tr::wire::decode(v);
    check(tlv.has_value(), "decode(view_t) decodes after the source buffer was freed");
    check(tlv.has_value() && tlv->type == tr::wire::type_t::VALUE, "decoded type is VALUE");
    check(tlv.has_value() && tlv->trailer && tlv->trailer->crc.has_value(),
          "CRC trailer present and verified (decode would have failed otherwise)");
}

void test_bounded_pool() {
    std::printf("Bounded pool backend (custom allocator over a fixed slab):\n");
    std::array<std::byte, 256> slab{};
    tr::mem::pool_t pool(slab, 16);
    const std::size_t cap = pool.capacity();
    check(cap > 0, "pool carved at least one slot from the slab");
    check(pool.available() == cap, "all slots free initially");

    std::vector<tr::view::segment_ptr_t> held;
    while (tr::view::segment_t* s = pool.alloc(16))
        held.push_back(tr::view::segment_ptr_t::adopt(s));
    check(held.size() == cap, "pool hands out exactly capacity slots");
    check(pool.alloc(16) == nullptr, "an exhausted pool returns nullptr (BACKPRESSURE)");
    check(pool.available() == 0, "no slots available when full");

    held.pop_back();  // releases one segment -> destroy returns its slot
    check(pool.available() == 1, "freeing a segment returns its slot to the pool");
    tr::view::segment_t* again = pool.alloc(16);
    check(again != nullptr, "alloc succeeds again after a free");
    const tr::view::segment_ptr_t reclaim = tr::view::segment_ptr_t::adopt(again);
    check(pool.alloc(17) == nullptr, "a request larger than the slot payload is refused");
}

/**
 * @brief The module-set traits are compile-time backend contracts (ADR-0047 §2); pin the load-
 *        bearing distinctions so a backend change that violates them fails to build.
 */
static_assert(!tr::mem::pool_t::needs_cache_ops, "the pool is plain RAM — no DMA cache ops");
static_assert(!tr::mem::pool_t::is_isr_safe,
              "the bare pool's free-list RMW is unsynchronized — NOT ISR-safe (#928); ISR safety "
              "is synchronized_pool_t with an ISR-safe policy");
static_assert(tr::mem::pool_t::is_nonblocking,
              "the pool's free-list alloc/destroy is O(1) — no heap, no syscall (#928)");
static_assert(!tr::mem::heap_backend_t::is_isr_safe, "operator new/delete is not ISR-safe");
static_assert(!tr::mem::heap_backend_t::is_nonblocking,
              "operator new/delete may lock or syscall — not nonblocking (#928)");
static_assert(tr::mem::heap_backend_t::owns_bytes, "the heap owns its bytes (durably storable)");
static_assert(!tr::mem::detail::borrowed_backend_t::owns_bytes,
              "a borrow must NOT be durably stored");

void test_mem_transfer() {
    std::printf("mem::transfer — host byte-move + cache-hook seam (ADR-0047 §2):\n");
    std::array<std::byte, 8> src{};
    for (std::size_t i = 0; i < src.size(); ++i) src[i] = static_cast<std::byte>(0xA0 + i);

    // Pool segment: write host bytes in (CPU_TO_DEVICE), read them back out (DEVICE_TO_CPU).
    std::array<std::byte, 256> slab{};
    tr::mem::pool_t pool(slab, 16);
    tr::view::segment_ptr_t pseg = tr::view::segment_ptr_t::adopt(pool.alloc(8));
    check(static_cast<bool>(pseg), "pool segment allocated for transfer");
    check(tr::mem::transfer(pseg.get(), src, tr::mem::io_dir_t::CPU_TO_DEVICE),
          "CPU_TO_DEVICE copies host bytes into the pool segment");
    check(std::ranges::equal(pseg->bytes, src), "the segment now holds the source bytes");
    std::array<std::byte, 8> out{};
    check(tr::mem::transfer(pseg.get(), out, tr::mem::io_dir_t::DEVICE_TO_CPU),
          "DEVICE_TO_CPU copies the segment bytes back out to host");
    check(std::ranges::equal(out, src), "the round-trip preserves the bytes");

    // Heap segment: the same round-trip through a different (host) backend.
    tr::view::segment_ptr_t hseg = tr::view::heap_alloc(8);
    check(tr::mem::transfer(hseg.get(), src, tr::mem::io_dir_t::CPU_TO_DEVICE) &&
              std::ranges::equal(hseg->bytes, src),
          "transfer works over the heap backend too");

    // A DEVICE-space segment is refused whatever its backend tag (#928): the SPACE tag decides
    // CPU-copyability, so the borrowed-DEVICE stand-in now gets the same clean false a
    // semantically identical UNKNOWN-tagged device segment always got through
    // `transfer_generic` — the backend tag stays a fast path, never an outcome-changing input.
    // (This deliberately flips the pre-#928 assertion that the fast path memcpy'd it.)
    std::array<std::byte, 8> dev_store{};
    tr::view::segment_ptr_t dseg = tr::view::borrow_device(dev_store);
    check(!tr::mem::transfer(dseg.get(), src, tr::mem::io_dir_t::CPU_TO_DEVICE),
          "transfer refuses a DEVICE-space segment (the borrowed-DEVICE stand-in included)");
    check(std::ranges::all_of(dev_store, [](std::byte b) { return b == std::byte{0}; }),
          "the refused device bytes are untouched");

    // Guards: an over-long host buffer and a null segment are rejected, not truncated.
    std::array<std::byte, 9> too_big{};
    check(!tr::mem::transfer(pseg.get(), too_big, tr::mem::io_dir_t::CPU_TO_DEVICE),
          "a host buffer larger than the segment is refused");
    check(!tr::mem::transfer(nullptr, src, tr::mem::io_dir_t::DEVICE_TO_CPU),
          "a null segment is refused");
}

/**
 * @brief What a registered device hook was last asked to move — the test's observation point.
 */
struct device_hook_log_t {
    const tr::mem::mem_backend_t* backend = nullptr; /**< @brief Whose segment reached the hook. */
    std::size_t bytes = 0;                           /**< @brief How many bytes were asked for. */
    int hook_id = 0;                                 /**< @brief WHICH hook ran (0 = none yet). */
};

/** @brief The one log both hooks below write; reset between properties. */
device_hook_log_t g_hook_log;

/**
 * @brief A `DEVICE`-space backend over caller-owned HOST bytes — the GPU-free stand-in that
 *        lets the `register_device_backend` seam be pinned on every PR (#1381).
 *
 * The real device backend is a `backends/` tier module needing a GPU, so CI can never build
 * it. What CI *can* build is a second member of the same seam: anything whose `space()` is
 * `DEVICE` takes `mem::transfer`'s registry arm, and the bytes behind it being ordinary RAM
 * is exactly what makes the routing observable without hardware.
 */
class fake_device_backend_t final : public tr::mem::mem_backend_t {
   public:
    fake_device_backend_t() noexcept : mem_backend_t("fake_device") {}
    void destroy(tr::view::segment_t* seg) noexcept override { delete seg; }
    [[nodiscard]] tr::mem::mem_space_t space() const noexcept override {
        return tr::mem::mem_space_t::DEVICE;
    }
};

/** @brief The shared body of both hooks: log the call, then move the bytes for real. */
bool log_and_move(tr::view::segment_t* seg, std::span<std::byte> host, tr::mem::io_dir_t dir,
                  int hook_id) noexcept {
    if (seg == nullptr || host.size() > seg->bytes.size()) return false;
    g_hook_log = {seg->backend, host.size(), hook_id};
    if (dir == tr::mem::io_dir_t::CPU_TO_DEVICE) {
        std::memcpy(seg->bytes.data(), host.data(), host.size());
    } else {
        std::memcpy(host.data(), seg->bytes.data(), host.size());
    }
    return true;
}

/** @brief Device hook #1 — the first registration. */
bool device_hook_a(tr::view::segment_t* seg, std::span<std::byte> host,
                   tr::mem::io_dir_t dir) noexcept {
    return log_and_move(seg, host, dir, 1);
}

/** @brief Device hook #2 — the replacement, to prove re-registration rebinds the same slot. */
bool device_hook_b(tr::view::segment_t* seg, std::span<std::byte> host,
                   tr::mem::io_dir_t dir) noexcept {
    return log_and_move(seg, host, dir, 2);
}

/** @brief A segment of @p bytes owned by @p backend (the fake device's `alloc` stand-in). */
tr::view::segment_ptr_t device_segment(tr::mem::mem_backend_t& backend,
                                       std::span<std::byte> bytes) {
    return tr::view::segment_ptr_t::adopt(new (std::nothrow) tr::view::segment_t(&backend, bytes));
}

/**
 * @brief The device-backend registration seam (#1381) — `mem::transfer`'s out-of-core arm,
 *        proved without a GPU.
 *
 * This is the property the `backends/` tier depends on and the one the vendor `#ifdef` used
 * to provide: a `DEVICE` segment reaches its OWN backend's byte-move, and only that one.
 * Registrations are process-global and this test fills the table, so it runs last.
 */
void test_device_backend_registry() {
    std::printf("register_device_backend — the DEVICE-space transfer seam (#1381):\n");
    std::array<fake_device_backend_t, tr::mem::kDeviceBackendSlots> slotted{};
    fake_device_backend_t spare;  // one more backend than the table can hold

    std::array<std::byte, 8> src{};
    for (std::size_t i = 0; i < src.size(); ++i) src[i] = static_cast<std::byte>(0x50 + i);
    std::array<std::byte, 8> store{};
    tr::view::segment_ptr_t seg = device_segment(slotted[0], store);
    check(static_cast<bool>(seg) && seg->space == tr::mem::mem_space_t::DEVICE,
          "the fake device backend's segment reports DEVICE space");

    // (b) UNREGISTERED — the pre-registry refusal, now proved against the registry rather
    // than against a vendor tag. Nothing has registered yet, so there is nothing to route to.
    check(!tr::mem::transfer(seg.get(), src, tr::mem::io_dir_t::CPU_TO_DEVICE),
          "an UNREGISTERED device backend's segment is refused");
    check(std::ranges::all_of(store, [](std::byte b) { return b == std::byte{0}; }),
          "the refused bytes are untouched");
    check(!tr::mem::register_device_backend(spare, nullptr), "a null hook registers nothing");

    // (a) REGISTERED — transfer routes to that backend's hook, in both directions.
    check(tr::mem::register_device_backend(slotted[0], &device_hook_a),
          "register_device_backend accepts the first backend");
    g_hook_log = {};
    check(tr::mem::transfer(seg.get(), src, tr::mem::io_dir_t::CPU_TO_DEVICE),
          "CPU_TO_DEVICE reaches the registered hook");
    check(g_hook_log.hook_id == 1 && g_hook_log.backend == &slotted[0] && g_hook_log.bytes == 8,
          "the hook saw its OWN backend's segment and the full span");
    check(std::ranges::equal(store, src), "the hook moved the bytes host -> device");
    std::array<std::byte, 8> out{};
    check(tr::mem::transfer(seg.get(), out, tr::mem::io_dir_t::DEVICE_TO_CPU) &&
              std::ranges::equal(out, src),
          "DEVICE_TO_CPU reaches the same hook and round-trips");

    // Keyed by BACKEND IDENTITY, not by space: a second, unregistered device backend is still
    // refused while the first one routes. This is what lets a second vendor plug in.
    std::array<std::byte, 8> other_store{};
    tr::view::segment_ptr_t other = device_segment(spare, other_store);
    check(!tr::mem::transfer(other.get(), src, tr::mem::io_dir_t::CPU_TO_DEVICE),
          "a DIFFERENT unregistered device backend is still refused");

    // (c) RE-REGISTERING REPLACES the hook (insert_or_assign) — and must not eat a second slot.
    check(tr::mem::register_device_backend(slotted[0], &device_hook_b),
          "re-registering the same backend succeeds");
    g_hook_log = {};
    check(tr::mem::transfer(seg.get(), src, tr::mem::io_dir_t::CPU_TO_DEVICE),
          "transfer still routes after the replacement");
    check(g_hook_log.hook_id == 2, "the REPLACEMENT hook ran, not the original");

    // (d) OVERFLOW refuses and registers nothing. Filling the remaining slots must succeed,
    // which is also the proof that (c) rebound slot 0 rather than consuming a new one.
    for (std::size_t i = 1; i < tr::mem::kDeviceBackendSlots; ++i) {
        check(tr::mem::register_device_backend(slotted[i], &device_hook_a),
              "the remaining slots accept a registration");
    }
    check(!tr::mem::register_device_backend(spare, &device_hook_a),
          "a full table REFUSES the next backend (kDeviceBackendSlots)");
    g_hook_log = {};
    check(!tr::mem::transfer(other.get(), src, tr::mem::io_dir_t::CPU_TO_DEVICE),
          "the refused backend registered nothing — its segments still get false");
    check(g_hook_log.hook_id == 0, "no hook ran for the refused backend");
}

}  // namespace

void test_memory_space() {
    std::printf("memory-space tag — host vs device (docs/adr/0024):\n");
    std::array<std::byte, 8> host_bytes{};
    std::array<std::byte, 8> dev_bytes{};

    const tr::view::view_t hv = tr::view::view_t::over(tr::view::borrow(host_bytes));
    check(hv.is_host() && !hv.is_device(), "borrow() yields a HOST view");
    check(hv.owner->space == tr::mem::mem_space_t::HOST, "host segment.space == HOST");

    const tr::view::view_t dv = tr::view::view_t::over(tr::view::borrow_device(dev_bytes));
    check(dv.is_device() && !dv.is_host(), "borrow_device() yields a DEVICE view");
    check(dv.owner->space == tr::mem::mem_space_t::DEVICE, "device segment.space == DEVICE");

    check(tr::mem::heap_backend().space() == tr::mem::mem_space_t::HOST,
          "heap backend defaults to HOST");

    tr::view::rope_t host_rope(hv);
    host_rope.append(tr::view::view_t::over(tr::view::borrow(host_bytes)));
    check(host_rope.all_host(), "host+host rope is all_host");

    tr::view::rope_t hetero(hv);
    hetero.append(dv);  // a heterogeneous host(header)+device(payload) rope (the mem_cuda shape)
    check(!hetero.all_host(), "host+device rope is NOT all_host");

    // flatten must refuse the heterogeneous rope WITHOUT touching the device link.
    check(hetero.flatten().empty(), "flatten() refuses a heterogeneous rope (no device deref)");

    const tr::view::view_t hflat = host_rope.flatten();
    check(!hflat.empty() && hflat.length == 16, "flatten() of an all-host rope still works");
}

int main() {
    const fs::path vroot{LIBTRACER_VECTORS_DIR};

    test_refcount_lifetime();
    test_transfer_vs_clone();
    test_zero_copy_subview_concat();
    test_rope_equivalence(vroot);
    test_cast_claim_outlives_source(vroot);
    test_bounded_pool();
    test_mem_transfer();
    test_memory_space();
    // Last: it fills the process-global device-backend table (#1381).
    test_device_backend_registry();

#ifdef LIBTRACER_NO_ATOMIC
    std::printf("\n(built with LIBTRACER_NO_ATOMIC — single-threaded refcount)\n");
#endif
    return tr::testing::summary("substrate");
}
