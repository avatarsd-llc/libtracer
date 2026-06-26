// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// An L4 graph vertex: a named, addressable position holding a value, a bounded
// history, or a user handler (docs/reference/11 §roles). Pinned in place (the
// atomic LKV slot + mutex + condvar are non-movable); always handled via a
// Vertex* returned by Graph::register_vertex. The read/write LKV hot path is
// lock-free (an atomic shared_ptr swap, the orderings M2 already pays for); the
// mutex guards only the history ring, the subscriber list (M3b), and the await
// waiter accounting.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "libtracer/path.hpp"
#include "libtracer/status.hpp"
#include "libtracer/view.hpp"

namespace tracer::graph {

enum class Role {
    StoredValue,  // role 1: last-writer-wins; holds the last-written View
    Stream,       // role 2: bounded history ring sized by settings.history_keep_last
    Handler,      // roles 3-7: user on_read / on_write supplies the behavior
};

// The mandatory core QoS fields (docs/reference/02 §core writable fields).
struct Settings {
    std::uint8_t reliability = 0;         // 0=best-effort, 1=reliable
    std::uint8_t durability = 0;          // 0=volatile, 1=transient-local
    std::uint32_t history_keep_last = 1;  // Stream ring depth (>=1)
    std::uint64_t deadline_ns = 0;        // 0=off; max ns between writes before a liveness fault
    std::uint8_t priority = 0;            // 0=low .. 255=critical (transport hint, not a wire bit)
    std::uint32_t queue_max_bytes = 0;    // 0=unbounded; per-subscriber back-pressure cap
};

// User behavior for a Handler-role vertex.
struct Handlers {
    std::function<Result<View>()> on_read;
    std::function<Result<void>(const View&)> on_write;
};

// One subscription edge (M3b). A write to this vertex fans out to a target vertex
// (target_key — spec-faithful re-dispatch) and/or an in-process callback (sugar).
// docs/reference/02 §dispatch + 04 §write fanout. Inactive slots model an
// unsubscribe (a cleared :subscribers[N]).
struct Subscriber {
    std::vector<std::byte> target_key;          // canonical PATH key (empty => callback-only)
    std::function<void(const View&)> callback;  // null => target-only
    bool active = true;
};

class Vertex {
   public:
    Vertex(Role role, PathKey key, Settings settings, Handlers handlers)
        : role_(role), key_(std::move(key)), settings_(settings), handlers_(std::move(handlers)) {}

    Vertex(const Vertex&) = delete;
    Vertex& operator=(const Vertex&) = delete;

    [[nodiscard]] Role role() const noexcept { return role_; }
    [[nodiscard]] const PathKey& key() const noexcept { return key_; }
    [[nodiscard]] const Settings& settings() const noexcept { return settings_; }

   private:
    friend class Graph;

    Role role_;
    PathKey key_;
    Settings settings_;
    Handlers handlers_;

    std::atomic<std::shared_ptr<const View>> lkv_{};   // lock-free read/write hot path
    std::deque<std::shared_ptr<const View>> history_;  // Stream ring; guarded by m_
    std::vector<Subscriber> subs_;                     // fan-out edges; guarded by m_
    std::mutex m_;
    std::condition_variable cv_;
    std::uint64_t write_seq_ = 0;  // bumped per write; await waits for an increment (guarded by m_)
};

}  // namespace tracer::graph
