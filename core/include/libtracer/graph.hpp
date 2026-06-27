/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The L4 in-process graph runtime. Holds the vertex map (keyed on canonical
 * PATH-TLV payload bytes, docs/reference/02 §dispatch) and exposes the entire
 * data API: read / write / await (ADR-0006). The hot path resolves a vertex_t*
 * once (at registration or via one guarded lookup), then read/write/await on
 * that handle are lock-free in the LKV slot. subscriber_t fan-out + field-write
 * land in M3b; M3a delivers values via the LKV and the blocking await.
 */
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

class graph_t {
   public:
    graph_t() = default;
    graph_t(const graph_t&) = delete;
    graph_t& operator=(const graph_t&) = delete;

    // Register a vertex at `path` (any field tail is ignored). Returns the pinned
    // handle, or PathInUse if the path is already registered.
    [[nodiscard]] result_t<vertex_t*> register_vertex(const path_t& path, role_t role,
                                                      handlers_t handlers = {},
                                                      settings_t settings = {});

    // Hot path — operate on a resolved handle; lock-free in the LKV slot.
    [[nodiscard]] result_t<view_t> read(vertex_t* v) const;
    [[nodiscard]] result_t<void> write(vertex_t* v, view_t value);
    // Field-write by handle: resolve the vertex_t* and field_path_t once (e.g. from a
    // path_t::parse("/x:settings.reliability") kept around), then reuse them on the
    // hot path — no string parse, no map lookup per call. An empty `field` is an
    // ordinary value write. Pass `path.field()` for the field selector.
    [[nodiscard]] result_t<void> write(vertex_t* v, const field_path_t& field, view_t value);
    [[nodiscard]] result_t<view_t> await(vertex_t* v, std::chrono::nanoseconds timeout);
    // Stream history, newest last (Stream role only).
    [[nodiscard]] result_t<std::vector<view_t>> history(vertex_t* v) const;

    // Subscribe `src` to a target vertex (spec-faithful: a write to src re-dispatches
    // the cloned value to `target`). NotFound if src is unknown.
    //
    // These subscribe(...) overloads are *host SDK sugar*, not new wire primitives:
    // the wire data API stays read/write/await (ADR-0006). On the wire, subscription
    // is a consumer-initiated SUBSCRIBER write into the producer's `:subscribers[]`
    // field (ADR-0026), exactly as connect() is sugar over that field-write. Routing
    // these helpers through the `:subscribers[]` field-write surface (rather than the
    // current direct path) is tracked in #59.
    [[nodiscard]] result_t<void> subscribe(const path_t& src, const path_t& target,
                                           delivery_mode_t mode = delivery_mode_t::EVERY);
    // Subscribe `src` to an in-process callback (sugar; the callback fires inline on
    // each write to src with a cloned view). `mode` gates delivery producer-side.
    [[nodiscard]] result_t<void> subscribe(const path_t& src,
                                           std::function<void(const view_t&)> callback,
                                           delivery_mode_t mode = delivery_mode_t::EVERY);

    // Convenience — resolve the path key once (guarded map lookup), then hot path.
    // A write/read whose path has a field tail (e.g. ":settings.deadline_ns",
    // ":subscribers[]", ":schema") is routed to the field surface.
    [[nodiscard]] result_t<view_t> read(const path_t& path) const;
    [[nodiscard]] result_t<void> write(const path_t& path, view_t value);
    [[nodiscard]] result_t<view_t> await(const path_t& path, std::chrono::nanoseconds timeout);

    // Resolve a canonical PATH-payload key to its vertex (nullptr if unknown).
    [[nodiscard]] vertex_t* find(std::span<const std::byte> key) const;

   private:
    // Update the vertex value (LKV/history/handler), then fan out to subscribers.
    // `depth` bounds in-process re-dispatch cycles (kMaxDispatchDepth).
    result_t<void> write_impl(vertex_t* v, view_t value, int depth);
    void fan_out(vertex_t* v, const view_t& value, int depth);
    // Field surface: ":settings.<f>", ":subscribers[]" / "[N]".
    result_t<void> field_write(vertex_t* v, const field_path_t& field, const view_t& value);
    // ":schema" read => a POINT descriptor (name + settings).
    [[nodiscard]] result_t<view_t> read_schema(vertex_t* v) const;

    mutable std::shared_mutex map_mutex_;
    std::unordered_map<path_key_t, std::unique_ptr<vertex_t>, path_key_hash_t> vertices_;
};

}  // namespace tr::graph
