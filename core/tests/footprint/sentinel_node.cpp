/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The Cortex-M0 footprint sentinel's fixed workload — the "required modules" a
 * minimum-feature (P0) libtracer node links: the L0/L1 substrate (bounded pool
 * backend + segment/view/rope), the L2/L3 wire codec (frame encode/decode plus
 * the ADR-0041 terminus arena decode), and L4 addressing (canonical PATH
 * validation). No graph runtime, no transports, no threads — the surface
 * docs/spec/v1.md §3.1 guarantees an MCU can carry.
 *
 * This is a *fixture*, not a demo: it is cross-compiled bare-metal
 * (arm-none-eabi, -Os -fno-exceptions -fno-rtti, LIBTRACER_NO_ATOMIC), linked,
 * stripped, and its flash footprint gated at <= 16 KiB by
 * tools/cortexm0_footprint.py (ADR-0047 §5). Keep it stable: because the same
 * workload re-measures on every change, a size delta reflects a change in the
 * required modules, not a change here. It exercises each required entry point
 * so --gc-sections keeps the genuinely-reachable code and nothing else.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/frame.hpp"
#include "libtracer/mem_pool.hpp"
#include "libtracer/path.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/tlv_arena.hpp"
#include "libtracer/view.hpp"

namespace {

// The MCU backend of record (ADR-0016): a caller-owned static slab, no heap.
alignas(std::max_align_t) std::array<std::byte, 4096> g_slab{};

// Fold bytes into an accumulator so the optimizer cannot elide the work above.
std::uint32_t fold(std::uint32_t acc, std::span<const std::byte> bytes) noexcept {
    for (const std::byte b : bytes) acc = acc * 131u + static_cast<std::uint8_t>(b);
    return acc;
}

std::vector<std::byte> to_bytes(std::string_view s) {
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    return std::vector<std::byte>(p, p + s.size());
}

}  // namespace

int main() {
    using namespace tr;
    std::uint32_t acc = 0;

    // Substrate: a bounded pool over the static slab (alloc-or-nullptr, no heap).
    mem::pool_t pool(g_slab, 256);

    // Wire: build "/sensor/temp" as a PATH TLV with NAME children (the addressing
    // shape §3.1.2 places in .rodata on a real node), encode it, key it.
    wire::tlv_t root;
    root.type = wire::type_t::PATH;
    root.opt.pl = true;
    for (const std::string_view seg : {std::string_view{"sensor"}, std::string_view{"temp"}}) {
        wire::tlv_t name;
        name.type = wire::type_t::NAME;
        name.payload = to_bytes(seg);
        root.children.push_back(std::move(name));
    }
    const std::vector<std::byte> bytes = wire::encode(root);
    acc = fold(acc, bytes);
    acc = fold(acc, wire::path_key(root));

    // Wire: owning decode (the vector tree) and the terminus arena decode (a flat
    // pre-order array over borrowed spans, drawn from a fixed monotonic buffer —
    // no per-node heap; ADR-0041 / ADR-0039 §3).
    if (const auto dec = wire::decode(bytes); dec) {
        acc += static_cast<std::uint32_t>(dec->children.size());
    }
    alignas(std::max_align_t) std::array<std::byte, 512> arena_buf{};
    std::pmr::monotonic_buffer_resource mr(arena_buf.data(), arena_buf.size());
    if (const auto arena = wire::decode_into(bytes, mr); arena) {
        acc += static_cast<std::uint32_t>(arena->size());
    }

    // Substrate + L1: copy the frame into a pool segment, window it with a view,
    // scatter it as a two-link rope, then flatten back through the pool (never
    // the default heap backend — MCU nodes have none).
    if (view::segment_t* seg = pool.alloc(bytes.size())) {
        view::segment_ptr_t owner = view::segment_ptr_t::adopt(seg);
        std::memcpy(owner->bytes.data(), bytes.data(), bytes.size());
        const view::view_t whole = view::view_t::over(owner);
        const std::size_t mid = bytes.size() / 2;
        view::rope_t rope(whole.subview(0, mid));
        rope.append(whole.subview(mid, bytes.size() - mid));
        const view::view_t flat = rope.flatten(pool);
        acc = fold(acc, flat.bytes());
        acc += static_cast<std::uint32_t>(rope.link_count());
    }

    // Addressing: validate a canonical path (the init-time registration path,
    // §3.1.3) — the size/depth limits enforced once, off the hot path.
    if (const auto p = graph::path_t::parse("/sensor/temp"); p) acc += 1u;

    return static_cast<int>(acc & 0x7f);
}
