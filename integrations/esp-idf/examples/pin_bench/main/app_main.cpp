/**
 * @file
 * @brief pin_bench — RFC-0022 §6's MCU half, ON SILICON: what §3.D's pin predicate costs an
 *        ESP32-C6 in store latency, receive-pool occupancy and free heap.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * §6 names the MCU half as "the receive pool is small", and that is a **runtime** question:
 * a free-slot floor, a backpressure onset and a `min_free` trend are behaviours of a small
 * fixed pool under load. `-Os` `size` and `-fstack-usage` cannot observe a held buffer or an
 * exhaustion event, so the toolchain census (which this repo still requires, and which is
 * reported alongside) answers the static side only.
 *
 * The host's glibc numbers do not transfer either: ADR-0060's own gate note records that
 * pooled allocation is ~2.5x glibc on the host because tcache serves a hot same-size `malloc`
 * in ~15 ns, while ESP-IDF's `multi_heap` costs hundreds of nanoseconds and fragments. The
 * copy branch is the branch that allocates, so a host-only verdict is structurally biased
 * toward "pinning looks good". This app is what removes that bias.
 *
 * @section pinb_shape What it drives
 *
 * No network. Each iteration builds one FWD{WRITE} frame inside a segment drawn from a
 * bounded pool — the shape `udp_transport_t` hands up, and the shape whose slot a pin holds —
 * and resolves it against one of `V` STORED_VALUE vertices, round-robin. `V` is the RAM axis:
 * one vertex holds one value, so pinning holds ONE slot however large the segment; the held
 * quantity is `V x slot_bytes`, and the interesting boundary is `V` against the slot count.
 *
 * @section pinb_arms Arms, and why they interleave inside one boot
 *
 * K is a per-vertex `u32` here rather than `config_t::kPinPayloadRatio`, exactly as in the
 * host bench, so every arm rotates inside ONE image and ONE boot. Building one flash image
 * per K and running them back to back would reintroduce the sequential-run confound at the
 * worst possible place — a reflash changes heap layout, and heap layout is half of what this
 * app measures.
 *
 * @section pinb_reach Reachability
 *
 * Every row carries `pins`/`copies` decided by segment-pointer identity between the stored
 * value and the pool's own slots, and the app refuses to print a table at all if its
 * calibration line-break fails: a CRC-trailered payload and a K that cannot clear the ratio
 * must both report zero pins, and a pin-always arm must report all of them.
 */

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "libtracer/mem_pool.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

constexpr const char* kTag = "pin_bench";

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::op_resolver_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief The RX slot size the pool carves — the `segment_bytes` a pin would hold. */
constexpr std::size_t kSlotBytes = 1024;
/** @brief How many RX slots the node has. Deliberately small: that is §6's premise. */
constexpr std::size_t kSlots = 24;
/** @brief Stores timed per (arm, vertex-count) cell. */
constexpr std::size_t kIters = 2000;

/** @brief A NAME TLV. */
std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, s);
    return out;
}

/** @brief A PATH TLV over `segs`. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) {
        const std::vector<std::byte> n = b_name(s);
        body.insert(body.end(), n.begin(), n.end());
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

/** @brief A one-byte VALUE TLV. */
std::vector<std::byte> b_u8_value(std::uint8_t v) {
    const std::byte b{v};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}

/** @brief A VALUE TLV of `n` bytes; `crc` sets the trailer bit that blocks pinning. */
std::vector<std::byte> b_value(std::size_t n, bool crc) {
    std::vector<std::byte> p(n, std::byte{0xA5});
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{.cr = crc}, p);
    return out;
}

/** @brief A FWD{WRITE} frame addressed at `/s/b<idx>`. */
std::vector<std::byte> b_fwd_write(std::size_t payload, std::size_t idx, bool crc) {
    const std::string leaf = "b" + std::to_string(idx);
    std::vector<std::byte> body;
    const auto app = [&body](const std::vector<std::byte>& s) {
        body.insert(body.end(), s.begin(), s.end());
    };
    app(b_u8_value(static_cast<std::uint8_t>(fwd_op_t::WRITE)));
    app(b_path({"s", leaf}));
    app(b_path({"c"}));
    app(b_value(payload, crc));
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

/**
 * @brief A bounded RX pool that also remembers which segments it issued.
 *
 * Membership is the pin/copy verdict without a compile-time instrument: a pinned store holds
 * a POOL slot, a copied store holds a fresh `multi_heap` block. @ref available is the
 * free-slot floor, and a `nullptr` from @ref alloc is backpressure — the datagram a real
 * transport would have to drop.
 */
class rx_pool_t final : public tr::mem::mem_backend_t {
   public:
    rx_pool_t(std::span<std::byte> slab, std::size_t slot)
        : mem_backend_t("pin_bench_rx"),
          inner_(slab, slot),
          lo_(slab.data()),
          hi_(slab.data() + slab.size()) {}

    tr::view::segment_t* alloc(std::size_t n, tr::mem::alloc_hint_t hint) override {
        tr::view::segment_t* seg = inner_.alloc(n, hint);
        if (seg != nullptr) {
            seg->backend = this;
            seg->btag = tr::mem::backend_tag::UNKNOWN;
        } else {
            ++refused_;
        }
        return seg;
    }
    void destroy(tr::view::segment_t* seg) noexcept override { inner_.destroy(seg); }
    [[nodiscard]] std::size_t alignment() const noexcept override { return inner_.alignment(); }
    [[nodiscard]] std::size_t max_segment_size() const noexcept override {
        return inner_.max_segment_size();
    }
    [[nodiscard]] tr::mem::backend_tag tag() const noexcept override {
        return tr::mem::backend_tag::UNKNOWN;
    }

    /**
     * @brief Is @p p one of this pool's slots? Decided by SLAB ADDRESS RANGE, not by a list.
     *
     * The first version of this remembered the segments `alloc` handed out, capped at 64
     * entries. That instrument was measurably wrong and the bench's own output is what
     * exposed it: at 512 B / 1 vertex the p50 was 126 us in every arm and every round — the
     * pinned figure — while the pin counter flipped between 2,000 and 0 round to round. A
     * pool free list is LIFO and a one-vertex cell only ever cycles two slots, so the capped
     * list froze holding two pointers; every later store landing on a THIRD slot was then
     * reported as a copy. The `--calibrate` line-break did not catch it, because calibration
     * runs on a fresh pool where the recorded slots are the ones in use — a reachability
     * instrument can be right at t=0 and stale by t=1.
     *
     * A slab-range test cannot go stale: `pool_t` carves both the bytes and the `segment_t`
     * control block out of the slab, so "inside the slab" IS "is a pool slot", exactly, for
     * every slot, forever.
     */
    [[nodiscard]] bool issued(const tr::view::segment_t* p) const noexcept {
        const std::byte* q = reinterpret_cast<const std::byte*>(p);
        return q >= lo_ && q < hi_;
    }
    [[nodiscard]] std::size_t available() const noexcept { return inner_.available(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return inner_.capacity(); }
    [[nodiscard]] std::uint32_t refused() const noexcept { return refused_; }
    void clear_refused() noexcept { refused_ = 0; }

   private:
    tr::mem::pool_t inner_;
    const std::byte* lo_;
    const std::byte* hi_;
    std::uint32_t refused_ = 0;
};

/** @brief One cell's outcome. */
struct cell_t {
    std::uint32_t p50_ns = 0, p99_ns = 0, mean_ns = 0;
    std::uint32_t pins = 0, copies = 0, stores = 0;
    std::size_t free_floor = 0; /**< fewest pool slots free at any sample during the load */
    std::uint32_t refused = 0;  /**< pool allocs that returned nullptr = backpressure */
    std::size_t min_free = 0;   /**< `heap_caps_get_minimum_free_size` after the cell */
};

/** @brief Exact p50/p99 by insertion into a small sorted vector — no histogram, no reservoir. */
std::uint32_t quantile(std::vector<std::uint32_t>& v, double q) {
    if (v.empty()) return 0;
    std::size_t i = static_cast<std::size_t>(q * static_cast<double>(v.size()));
    if (i >= v.size()) i = v.size() - 1;
    return v[i];
}

/**
 * @brief Run one (K, vertices) cell.
 *
 * The frame is copied into a POOL segment each iteration — the transport's own cost, paid in
 * every arm — and the copy is untimed. Only `resolve` is timed. If the pool refuses (every
 * slot pinned), the iteration is counted as backpressure and skipped, which is exactly what a
 * transport does with the datagram.
 */
cell_t run_cell(rx_pool_t& pool, std::uint32_t k, std::size_t vertices, std::size_t payload,
                bool crc = false) {
    graph_t g;
    op_resolver_t resolver(g);
    tr::graph::settings_t s;
    s.store_ref_min_bytes = k;
    std::vector<tr::graph::vertex_handle_t> handles;
    for (std::size_t i = 0; i < vertices; ++i)
        handles.push_back(
            g.register_vertex(path_t("/s/b" + std::to_string(i)), role_t::STORED_VALUE, {}, s));

    std::vector<std::vector<std::byte>> frames;
    for (std::size_t i = 0; i < vertices; ++i) frames.push_back(b_fwd_write(payload, i, crc));

    cell_t out;
    out.free_floor = pool.capacity();
    pool.clear_refused();
    std::vector<std::uint32_t> lat;
    lat.reserve(kIters);
    std::uint64_t sum = 0;

    for (std::size_t i = 0; i < kIters; ++i) {
        const std::vector<std::byte>& fr = frames[i % vertices];
        tr::view::segment_t* raw = pool.alloc(kSlotBytes, tr::mem::alloc_hint_t::NONE);
        if (raw == nullptr) continue;  // pool exhausted: the drop a transport would take
        tr::view::segment_ptr_t seg = tr::view::segment_ptr_t::adopt(raw);
        std::memcpy(seg->bytes.data(), fr.data(), fr.size());
        tr::view::view_t fv = tr::view::view_t::over(std::move(seg)).subview(0, fr.size());

        const auto arena = tr::wire::decode_into(fv.bytes(), tr::mem::heap_source());
        if (!arena) continue;

        const std::int64_t t0 = esp_timer_get_time();
        auto reply = resolver.resolve(*arena, {}, &fv);
        const std::int64_t t1 = esp_timer_get_time();
        const std::uint32_t ns = static_cast<std::uint32_t>((t1 - t0) * 1000);
        lat.push_back(ns);
        sum += ns;
        if (!reply) continue;

        ++out.stores;
        const auto rd = g.read(handles[i % vertices]);
        if (rd && (*rd)->link_count() == 1 && (*rd)->only().owner &&
            pool.issued((*rd)->only().owner.get()))
            ++out.pins;
        else
            ++out.copies;

        const std::size_t avail = pool.available();
        if (avail < out.free_floor) out.free_floor = avail;
    }
    out.refused = pool.refused();
    // Sort in place for the exact order statistics (kIters is small; no histogram).
    for (std::size_t i = 1; i < lat.size(); ++i) {
        const std::uint32_t x = lat[i];
        std::size_t j = i;
        while (j > 0 && lat[j - 1] > x) {
            lat[j] = lat[j - 1];
            --j;
        }
        lat[j] = x;
    }
    out.p50_ns = quantile(lat, 0.50);
    out.p99_ns = quantile(lat, 0.99);
    out.mean_ns = lat.empty() ? 0 : static_cast<std::uint32_t>(sum / lat.size());
    out.min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    return out;
}

/** @brief The arms, rotated per round inside this one boot. */
struct arm_t {
    const char* label;
    std::uint32_t k;
};
constexpr arm_t kArms[] = {
    {"A-sentinel", 0}, {"D2", 2}, {"D8", 8}, {"D64", 64}, {"C-pin-always", 0xFFFFFFFFu},
};
constexpr std::size_t kVertexSet[] = {1, 8, 32};
constexpr std::size_t kPayloads[] = {64, 512};
constexpr int kRounds = 6;

/** @brief Break the instrument's line; a failure refuses the whole table. */
bool calibrate(rx_pool_t& pool) {
    bool ok = true;
    const auto expect = [&ok](const char* what, std::uint32_t got, std::uint32_t want) {
        const bool good = got == want;
        ESP_LOGI(kTag, "CALIBRATE %s %s got=%" PRIu32 " want=%" PRIu32, good ? "PASS" : "FAIL",
                 what, got, want);
        if (!good) ok = false;
    };
    const cell_t pos = run_cell(pool, 0xFFFFFFFFu, 1, 512);
    expect("pin-always pins every store", pos.copies, 0);
    expect("pin-always pin count is the store count", pos.pins, pos.stores);
    const cell_t sent = run_cell(pool, 0, 1, 512);
    expect("sentinel K=0 never pins", sent.pins, 0);
    const cell_t crc = run_cell(pool, 0xFFFFFFFFu, 1, 512, /*crc=*/true);
    expect("CRC-trailered payload never pins", crc.pins, 0);
    // 64 B payload cannot clear a 1024 B slot at K = 2 (64 * 2 = 128).
    const cell_t ratio = run_cell(pool, 2, 1, 64);
    expect("K=2 against a 1024 B slot at 64 B payload never pins", ratio.pins, 0);
    return ok;
}

}  // namespace

extern "C" void app_main() {
    static std::vector<std::byte> slab((kSlotBytes + 96) * kSlots + 512);
    static rx_pool_t pool(std::span<std::byte>(slab), kSlotBytes);

    ESP_LOGI(kTag, "RFC-0022 §6 MCU half — slots=%u slot_bytes=%u boot_free=%u",
             static_cast<unsigned>(pool.capacity()), static_cast<unsigned>(kSlotBytes),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT)));

    if (!calibrate(pool)) {
        ESP_LOGE(kTag, "instrument calibration FAILED — refusing to report any cell");
        return;
    }
    ESP_LOGI(kTag,
             "# PINROW round arm K payload vertices p50ns p99ns meanns pins copies stores "
             "free_floor refused min_free");

    for (int r = 0; r < kRounds; ++r) {
        constexpr std::size_t nA = sizeof(kArms) / sizeof(kArms[0]);
        for (std::size_t j = 0; j < nA; ++j) {
            // Rotate: arm j leads round j, so no arm always runs on a cold or a warm heap.
            const arm_t& arm = kArms[(static_cast<std::size_t>(r) + j) % nA];
            for (std::size_t payload : kPayloads) {
                for (std::size_t V : kVertexSet) {
                    const cell_t c = run_cell(pool, arm.k, V, payload);
                    ESP_LOGI(kTag,
                             "PINROW %d %s %" PRIu32 " %u %u %" PRIu32 " %" PRIu32 " %" PRIu32
                             " %" PRIu32 " %" PRIu32 " %" PRIu32 " %u %" PRIu32 " %u",
                             r, arm.label, arm.k, static_cast<unsigned>(payload),
                             static_cast<unsigned>(V), c.p50_ns, c.p99_ns, c.mean_ns, c.pins,
                             c.copies, c.stores, static_cast<unsigned>(c.free_floor), c.refused,
                             static_cast<unsigned>(c.min_free));
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
        }
    }
    ESP_LOGI(kTag, "PINDONE min_free=%u free=%u",
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT)));
}
