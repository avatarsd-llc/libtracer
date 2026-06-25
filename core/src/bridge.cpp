// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

#include "libtracer/bridge.hpp"

#include <chrono>
#include <cstring>
#include <utility>

#include "libtracer/mem_heap.hpp"
#include "libtracer/view.hpp"

namespace tracer {
namespace {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// A 24-byte recent-set key: origin (16) || ts (8 LE).
std::string recent_key(const RouterMeta& meta) {
    std::string k(meta.origin.size() + 8, '\0');
    std::memcpy(k.data(), meta.origin.data(), meta.origin.size());
    for (int i = 0; i < 8; ++i)
        k[meta.origin.size() + static_cast<std::size_t>(i)] =
            static_cast<char>((meta.ts >> (8 * i)) & 0xFF);
    return k;
}

}  // namespace

Bridge::Bridge(graph::Graph& graph, Transport& transport, PeerId peer)
    : graph_(graph), transport_(transport), peer_(peer) {
    transport_.set_receiver([this](std::span<const std::byte> frame) { on_frame(frame); });
}

graph::Result<void> Bridge::export_vertex(const graph::Path& src) {
    return graph_.subscribe(src, [this](const View& value) {
        const RouterMeta meta{.origin = peer_, .ts = now_ns(), .hop = 0};
        const auto frame = router_wrap(value.bytes(), meta);
        transport_.send(frame);
    });
}

void Bridge::set_mount(const graph::Path& mount) {
    mount_key_.assign(mount.key().begin(), mount.key().end());
    have_mount_ = true;
}

void Bridge::set_recent_set_capacity(std::size_t capacity) { recent_cap_ = capacity; }

void Bridge::set_reforward(bool on) { reforward_ = on; }

bool Bridge::seen(const RouterMeta& meta) {
    if (recent_cap_ == 0) return false;  // dedup disabled — hop_count alone terminates
    std::string key = recent_key(meta);
    const std::lock_guard lock(m_);
    if (recent_set_.contains(key)) return true;
    recent_set_.insert(key);
    recent_order_.push_back(std::move(key));
    while (recent_order_.size() > recent_cap_) {
        recent_set_.erase(recent_order_.front());
        recent_order_.pop_front();
    }
    return false;
}

void Bridge::on_frame(std::span<const std::byte> frame) {
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

    if (have_mount_) {
        // Materialize the data TLV into an owned heap segment — the frame buffer
        // dies when on_frame returns, but the graph stores the View past then.
        // (One copy at the bridge boundary; reference/08 §cross-substrate.)
        const auto data = unwrapped->data;
        SegmentPtr seg = mem::heap_alloc(data.size());
        if (seg) {
            std::memcpy(seg->bytes.data(), data.data(), data.size());
            if (graph::Vertex* mount = graph_.find(mount_key_)) {
                (void)graph_.write(mount, View::over(std::move(seg)));
                delivered_.fetch_add(1);
            }
        }
    }

    if (reforward_) {  // re-emit with hop+1 (cycle-termination test)
        RouterMeta meta = unwrapped->meta;
        meta.hop = static_cast<std::uint8_t>(meta.hop + 1);
        transport_.send(router_wrap(unwrapped->data, meta));
    }
}

}  // namespace tracer
