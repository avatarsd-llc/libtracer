// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

#include "libtracer/graph.hpp"

#include <mutex>
#include <utility>

namespace tracer::graph {

Result<Vertex*> Graph::register_vertex(const Path& path, Role role, Handlers handlers,
                                       Settings settings) {
    PathKey key{std::vector<std::byte>(path.key().begin(), path.key().end())};
    const std::unique_lock lock(map_mutex_);
    if (vertices_.find(key) != vertices_.end()) return std::unexpected(Status::PathInUse);
    auto vertex = std::make_unique<Vertex>(role, key, settings, std::move(handlers));
    Vertex* ptr = vertex.get();
    vertices_.emplace(std::move(key), std::move(vertex));
    return ptr;
}

Vertex* Graph::find(std::span<const std::byte> key) const {
    PathKey k{std::vector<std::byte>(key.begin(), key.end())};
    const std::shared_lock lock(map_mutex_);
    const auto it = vertices_.find(k);
    return it == vertices_.end() ? nullptr : it->second.get();
}

Result<View> Graph::read(Vertex* v) const {
    if (v->role_ == Role::Handler) {
        if (v->handlers_.on_read) return v->handlers_.on_read();
        return std::unexpected(Status::NotFound);
    }
    const std::shared_ptr<const View> sp = v->lkv_.load();  // lock-free
    if (!sp) return std::unexpected(Status::NotFound);
    return *sp;  // copies the View => clones the SegmentPtr (refcount bump, no byte copy)
}

Result<void> Graph::write(Vertex* v, View value) {
    if (v->role_ == Role::Handler) {
        if (!v->handlers_.on_write) return std::unexpected(Status::NotFound);
        Result<void> r = v->handlers_.on_write(value);
        if (r) {
            const std::lock_guard lock(v->m_);
            ++v->write_seq_;
            v->cv_.notify_all();  // wake awaiters even though there is no LKV
        }
        return r;
    }

    auto sp = std::make_shared<const View>(std::move(value));
    v->lkv_.store(sp);  // lock-free publish of the new last-known-value

    const std::lock_guard lock(v->m_);
    if (v->role_ == Role::Stream) {
        v->history_.push_back(sp);
        const std::size_t keep =
            v->settings_.history_keep_last ? v->settings_.history_keep_last : 1;
        while (v->history_.size() > keep) v->history_.pop_front();
    }
    ++v->write_seq_;
    v->cv_.notify_all();
    return {};
}

Result<View> Graph::await(Vertex* v, std::chrono::nanoseconds timeout) {
    std::unique_lock lock(v->m_);
    const std::uint64_t seq0 = v->write_seq_;
    if (!v->cv_.wait_for(lock, timeout, [&] { return v->write_seq_ != seq0; })) {
        return std::unexpected(Status::Timeout);
    }
    lock.unlock();
    const std::shared_ptr<const View> sp = v->lkv_.load();
    if (!sp) return std::unexpected(Status::NotFound);  // e.g. a Handler-role write
    return *sp;
}

Result<std::vector<View>> Graph::history(Vertex* v) const {
    if (v->role_ != Role::Stream) return std::unexpected(Status::SchemaNotFound);
    const std::lock_guard lock(v->m_);
    std::vector<View> out;
    out.reserve(v->history_.size());
    for (const auto& sp : v->history_) out.push_back(*sp);  // clone each (refcount bump)
    return out;
}

Result<View> Graph::read(const Path& path) const {
    Vertex* v = find(path.key());
    if (!v) return std::unexpected(Status::NotFound);
    return read(v);
}

Result<void> Graph::write(const Path& path, View value) {
    Vertex* v = find(path.key());
    if (!v) return std::unexpected(Status::NotFound);
    return write(v, std::move(value));
}

Result<View> Graph::await(const Path& path, std::chrono::nanoseconds timeout) {
    Vertex* v = find(path.key());
    if (!v) return std::unexpected(Status::NotFound);
    return await(v, timeout);
}

}  // namespace tracer::graph
