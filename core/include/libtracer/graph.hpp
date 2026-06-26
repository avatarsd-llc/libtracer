// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// The L4 in-process graph runtime. Holds the vertex map (keyed on canonical
// PATH-TLV payload bytes, docs/reference/02 §dispatch) and exposes the entire
// data API: read / write / await (ADR-0006). The hot path resolves a Vertex*
// once (at registration or via one guarded lookup), then read/write/await on
// that handle are lock-free in the LKV slot. Subscriber fan-out + field-write
// land in M3b; M3a delivers values via the LKV and the blocking await.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "libtracer/path.hpp"
#include "libtracer/status.hpp"
#include "libtracer/vertex.hpp"
#include "libtracer/view.hpp"

namespace tr::graph {

// In-process fan-out cycle bound: a re-dispatch chain deeper than this is dropped
// (Backpressure). This is the in-process analogue of the wire hop_count/MAX_HOPS
// (ADR-0014); an A->B->A subscriber loop terminates here instead of recursing
// forever. See ADR-0015.
inline constexpr int kMaxDispatchDepth = 32;

class Graph {
   public:
    Graph() = default;
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;

    // Register a vertex at `path` (any field tail is ignored). Returns the pinned
    // handle, or PathInUse if the path is already registered.
    [[nodiscard]] Result<Vertex*> register_vertex(const Path& path, Role role,
                                                  Handlers handlers = {}, Settings settings = {});

    // Hot path — operate on a resolved handle; lock-free in the LKV slot.
    [[nodiscard]] Result<view_t> read(Vertex* v) const;
    [[nodiscard]] Result<void> write(Vertex* v, view_t value);
    // Field-write by handle: resolve the Vertex* and FieldPath once (e.g. from a
    // Path::parse("/x:settings.reliability") kept around), then reuse them on the
    // hot path — no string parse, no map lookup per call. An empty `field` is an
    // ordinary value write. Pass `path.field()` for the field selector.
    [[nodiscard]] Result<void> write(Vertex* v, const FieldPath& field, view_t value);
    [[nodiscard]] Result<view_t> await(Vertex* v, std::chrono::nanoseconds timeout);
    // Stream history, newest last (Stream role only).
    [[nodiscard]] Result<std::vector<view_t>> history(Vertex* v) const;

    // Subscribe `src` to a target vertex (spec-faithful: a write to src re-dispatches
    // the cloned value to `target`). NotFound if src is unknown.
    [[nodiscard]] Result<void> subscribe(const Path& src, const Path& target);
    // Subscribe `src` to an in-process callback (sugar; the callback fires inline on
    // each write to src with a cloned view).
    [[nodiscard]] Result<void> subscribe(const Path& src,
                                         std::function<void(const view_t&)> callback);

    // Convenience — resolve the path key once (guarded map lookup), then hot path.
    // A write/read whose path has a field tail (e.g. ":settings.deadline_ns",
    // ":subscribers[]", ":schema") is routed to the field surface.
    [[nodiscard]] Result<view_t> read(const Path& path) const;
    [[nodiscard]] Result<void> write(const Path& path, view_t value);
    [[nodiscard]] Result<view_t> await(const Path& path, std::chrono::nanoseconds timeout);

    // Resolve a canonical PATH-payload key to its vertex (nullptr if unknown).
    [[nodiscard]] Vertex* find(std::span<const std::byte> key) const;

   private:
    // Update the vertex value (LKV/history/handler), then fan out to subscribers.
    // `depth` bounds in-process re-dispatch cycles (kMaxDispatchDepth).
    Result<void> write_impl(Vertex* v, view_t value, int depth);
    void fan_out(Vertex* v, const view_t& value, int depth);
    // Field surface: ":settings.<f>", ":subscribers[]" / "[N]".
    Result<void> field_write(Vertex* v, const FieldPath& field, const view_t& value);
    // ":schema" read => a POINT descriptor (name + settings).
    [[nodiscard]] Result<view_t> read_schema(Vertex* v) const;

    mutable std::shared_mutex map_mutex_;
    std::unordered_map<PathKey, std::unique_ptr<Vertex>, PathKeyHash> vertices_;
};

}  // namespace tr::graph
