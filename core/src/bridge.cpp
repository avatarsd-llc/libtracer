// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

#include "libtracer/bridge.hpp"

#include <chrono>
#include <cstring>
#include <utility>

#include "libtracer/mem_heap.hpp"
#include "libtracer/view.hpp"

namespace tr {

// L1 types this layer consumes (upward dependency on tr::view, docs/adr/0016 §2).
using view::segment_ptr_t;
using view::view_t;
namespace {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// A 24-byte recent-set key: origin (16) || ts (8 LE).
std::string recent_key(const router_meta_t& meta) {
    std::string k(meta.origin.size() + 8, '\0');
    std::memcpy(k.data(), meta.origin.data(), meta.origin.size());
    for (int i = 0; i < 8; ++i)
        k[meta.origin.size() + static_cast<std::size_t>(i)] =
            static_cast<char>((meta.ts >> (8 * i)) & 0xFF);
    return k;
}

}  // namespace

bridge_t::bridge_t(graph::graph_t& graph, transport_t& transport, peer_id_t peer)
    : graph_(graph), transport_(transport), peer_(peer) {
    transport_.set_receiver([this](std::span<const std::byte> frame) { on_frame(frame); });
}

graph::result_t<void> bridge_t::export_vertex(const graph::path_t& src) {
    return graph_.subscribe(src, [this](const view_t& value) {
        const router_meta_t meta{.origin = peer_, .ts = now_ns(), .hop = 0};
        const auto frame = router_wrap(value.bytes(), meta);
        transport_.send(frame);
    });
}

void bridge_t::set_mount(const graph::path_t& mount) {
    // Resolve the vertex handle once (the mount vertex must already be registered);
    // the receive thread then writes through it with no string/lookup per frame.
    mount_vertex_.store(graph_.find(mount.key()));
}

void bridge_t::set_recent_set_capacity(std::size_t capacity) { recent_cap_.store(capacity); }

void bridge_t::set_reforward(bool on) { reforward_.store(on); }

bool bridge_t::seen(const router_meta_t& meta) {
    const std::size_t cap = recent_cap_.load(std::memory_order_relaxed);
    if (cap == 0) return false;  // dedup disabled — hop_count alone terminates
    std::string key = recent_key(meta);
    const std::lock_guard lock(m_);
    if (recent_set_.contains(key)) return true;
    recent_set_.insert(key);
    recent_order_.push_back(std::move(key));
    while (recent_order_.size() > cap) {
        recent_set_.erase(recent_order_.front());
        recent_order_.pop_front();
    }
    return false;
}

void bridge_t::on_frame(std::span<const std::byte> frame) {
    const auto unwrapped = router_unwrap(frame);
    if (!unwrapped) return;  // malformed — drop

    if (unwrapped->meta.hop >= kMaxHops) {  // the termination guarantee (ADR-0014)
        hop_dropped_.fetch_add(1);
        return;
    }
    if (seen(unwrapped->meta)) {
        deduped_.fetch_add(1);
        return;
    }

    if (graph::vertex_t* mount = mount_vertex_.load(std::memory_order_relaxed)) {
        // Materialize the data TLV into an owned heap segment — the frame buffer
        // dies when on_frame returns, but the graph stores the view_t past then.
        // (One copy at the bridge boundary; reference/08 §cross-substrate.)
        const auto data = unwrapped->data;
        segment_ptr_t seg = view::heap_alloc(data.size());
        if (seg) {
            std::memcpy(seg->bytes.data(), data.data(), data.size());
            (void)graph_.write(mount, view_t::over(std::move(seg)));
            delivered_.fetch_add(1);
        }
    }

    if (reforward_.load(std::memory_order_relaxed)) {  // re-emit with hop+1 (cycle test)
        router_meta_t meta = unwrapped->meta;
        meta.hop = static_cast<std::uint8_t>(meta.hop + 1);
        transport_.send(router_wrap(unwrapped->data, meta));
    }
}

}  // namespace tr
