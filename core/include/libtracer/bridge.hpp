// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// The bridge: connects a local Graph to a Transport, attaching/shedding the
// ROUTER envelope and terminating cycles (docs/reference/10 §bridge, 07 §cycle
// handling). Egress: export_vertex subscribes a local vertex and forwards each
// write as a ROUTER-wrapped frame. Ingress: the transport receiver unwraps,
// dedups (recent-set on (origin, ts)), enforces hop_count/MAX_HOPS (the
// termination guarantee, ADR-0014), then writes the bare data TLV to the mount
// vertex so local subscribers receive it (one copy at the bridge boundary).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#include "libtracer/graph.hpp"
#include "libtracer/path.hpp"
#include "libtracer/router.hpp"
#include "libtracer/transport.hpp"

namespace tr {

// The hop_count termination cap (docs/reference/05 §0x0D, ADR-0014). 32 mirrors
// the in-process kMaxDispatchDepth.
inline constexpr std::uint8_t kMaxHops = 32;

class Bridge {
   public:
    Bridge(graph::Graph& graph, Transport& transport, PeerId peer);

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    // Forward every write to `src` (a local vertex) onto the transport, ROUTER-
    // wrapped (origin = this peer, ts = now, hop = 0). NotFound if src is unknown.
    [[nodiscard]] graph::Result<void> export_vertex(const graph::Path& src);

    // Where ingested data lands (the proxy vertex; must be registered before frames
    // arrive). set_recent_set_capacity(0) disables dedup (pure hop_count
    // termination). set_reforward(true) re-emits ingested frames with hop+1 (used to
    // build a cycle for the termination test).
    void set_mount(const graph::Path& mount);
    void set_recent_set_capacity(std::size_t capacity);
    void set_reforward(bool on);

    [[nodiscard]] std::uint64_t delivered() const noexcept { return delivered_.load(); }
    [[nodiscard]] std::uint64_t deduped() const noexcept { return deduped_.load(); }
    [[nodiscard]] std::uint64_t hop_dropped() const noexcept { return hop_dropped_.load(); }

   private:
    void on_frame(std::span<const std::byte> frame);  // the transport receiver
    [[nodiscard]] bool seen(const RouterMeta& meta);  // recent-set check + insert

    graph::Graph& graph_;
    Transport& transport_;
    PeerId peer_;

    // Resolved once at set_mount (no per-frame string/lookup); atomic because the
    // transport's receive thread reads these while setup writes them.
    std::atomic<graph::Vertex*> mount_vertex_{nullptr};
    std::atomic<bool> reforward_{false};
    std::atomic<std::size_t> recent_cap_{64};

    std::mutex m_;  // guards the recent-set
    std::unordered_set<std::string> recent_set_;
    std::deque<std::string> recent_order_;

    std::atomic<std::uint64_t> delivered_{0};
    std::atomic<std::uint64_t> deduped_{0};
    std::atomic<std::uint64_t> hop_dropped_{0};
};

}  // namespace tr
