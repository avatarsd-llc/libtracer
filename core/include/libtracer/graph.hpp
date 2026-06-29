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
#include <string>
#include <string_view>
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

/**
 * @brief What the producer fan-out hands a remote subscriber's delivery sink (#136).
 *
 * A pure description of one remote subscription edge: the consumer's accumulated
 * return route and this node's NAME for the link it arrived on, both opaque to L4,
 * plus the @ref vertex_t::subscriber_t delivery_compact opt-in. The injected sink
 * (a `tr::net` concern — @ref graph_t::set_remote_delivery_sink) interprets these:
 * it maps @ref link to a transport child and emits a full-route `FWD{WRITE}` or,
 * when @ref delivery_compact, an auto-promoted label `COMPACT` (RFC-0004 §D/§E.1).
 * Borrowed for the sink call only — the sink must not retain the spans.
 */
struct remote_delivery_t {
    std::string_view link;                   /**< @brief This node's NAME for the consumer link. */
    std::span<const std::byte> return_route; /**< @brief Consumer return route (PATH TLV bytes). */
    bool delivery_compact = false;           /**< @brief Opt-in to label-compacted delivery. */
};

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
    // Field-read by handle (the read dual of the field-write overload): an empty `field`
    // is an ordinary value read; otherwise serve ":schema", ":acl", or a single
    // ":subscribers[N]" slot (the slot's stored SUBSCRIBER view, zero-copy). For the
    // whole-array ":subscribers[]" read use read_subscribers(). Used by the FWD resolver.
    [[nodiscard]] result_t<view_t> read(vertex_t* v, const field_path_t& field) const;
    // Read the ":subscribers[]" array: the populated slot SUBSCRIBER views in slot order
    // (each a zero-copy refcount clone of the stored source view). The FWD resolver ropes
    // these under a fresh PL=1 wrapper into the REPLY (RFC-0004 §D, no byte copy).
    [[nodiscard]] result_t<std::vector<view_t>> read_subscribers(vertex_t* v) const;
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

    // Install the sink the producer fan-out hands each REMOTE subscriber's delivery to
    // (#136, RFC-0004 §D/§E.1). Set once at wiring time by the transport plane
    // (tr::net::fwd_router_t) before frames flow; the sink fires on whatever thread
    // calls write() (outside the vertex lock), and on subscribe for a transient-local
    // latch. L4 keeps it as an opaque std::function, so the graph never depends on a
    // transport. A null sink (the default) ⇒ remote slots are stored but never deliver.
    void set_remote_delivery_sink(
        std::function<void(const remote_delivery_t&, const view_t&)> sink);

    // Store a REMOTE subscriber on `v`: a SUBSCRIBER slot carrying the consumer's
    // `return_route` + this node's NAME for its `link` + the `delivery_compact` opt-in,
    // so a later write fans out a FWD{WRITE}/COMPACT delivery via the remote sink. The
    // wire dual of the local subscribe(...) sugar, driven by the FWD resolver on an
    // inbound `:subscribers[]` WRITE (#59/#136). If `v` is transient-local
    // (settings.durability == 1) and already holds a value, the current LKV is latched
    // to this subscriber immediately (one synchronous sink call, RFC-0004 §D). The
    // `source_view` (the SUBSCRIBER TLV) is retained zero-copy so a `:subscribers[]`
    // read serves it back. NotFound is impossible (the caller holds `v`).
    [[nodiscard]] result_t<void> add_remote_subscriber(
        vertex_t* v, view_t source_view, std::vector<std::byte> return_route, std::string link,
        bool delivery_compact, delivery_mode_t mode = delivery_mode_t::EVERY);

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
    // ":acl" read => the raw stored ACL TLV bytes (structural; #81-A, ADR-0018/0020). Storage
    // only — enforcement is the deferred security_acl module, not this layer.
    [[nodiscard]] result_t<view_t> read_acl(vertex_t* v) const;

    mutable std::shared_mutex map_mutex_;
    std::unordered_map<path_key_t, std::unique_ptr<vertex_t>, path_key_hash_t> vertices_;
    // The remote-delivery sink (#136). Set once before frames flow, then read-only on
    // the write hot path — no lock needed (a benign data race with a late setup write
    // is excluded by the "configure before frames flow" contract, mirroring fwd_router).
    std::function<void(const remote_delivery_t&, const view_t&)> remote_sink_;
};

}  // namespace tr::graph
