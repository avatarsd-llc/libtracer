// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

#include "libtracer/graph.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string_view>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/view.hpp"

namespace tr::graph {

using wire::encode;
using wire::opt_t;
using wire::tlv_t;
using wire::type_t;
using wire::view_as_tlv;
namespace {

// Emit a VALUE TLV holding a `width`-byte little-endian integer — the one bespoke
// emitter for building a :schema POINT; NAME/SETTINGS/POINT use detail::emit_*.
void emit_value(std::vector<std::byte>& out, std::uint64_t value, int width) {
    std::vector<std::byte> payload(static_cast<std::size_t>(width));
    detail::store_le(payload, value, static_cast<std::size_t>(width));
    detail::emit_tlv(out, type_t::VALUE, opt_t{}, payload);
}

// The last NAME segment within a canonical PATH-payload key (the vertex's name).
[[nodiscard]] std::span<const std::byte> last_segment(std::span<const std::byte> key) {
    std::span<const std::byte> last;
    std::size_t i = 0;
    while (i + 4 <= key.size()) {
        const std::size_t len = std::to_integer<std::uint8_t>(key[i + 2]) |
                                (std::to_integer<std::uint8_t>(key[i + 3]) << 8);
        if (i + 4 + len > key.size()) break;
        last = key.subspan(i + 4, len);
        i += 4 + len;
    }
    return last;
}

// Reconstruct a canonical PATH key from a decoded PATH TLV's NAME children.
[[nodiscard]] std::vector<std::byte> path_child_key(const tlv_t& path) {
    std::vector<std::byte> key;
    for (const auto& name : path.children) {
        const auto enc = encode(name);  // a NAME TLV: 02 00 <len> <bytes>
        key.insert(key.end(), enc.begin(), enc.end());
    }
    return key;
}

}  // namespace

result_t<vertex_t*> graph_t::register_vertex(const path_t& path, role_t role, handlers_t handlers,
                                             settings_t settings) {
    path_key_t key{std::vector<std::byte>(path.key().begin(), path.key().end())};
    const std::unique_lock lock(map_mutex_);
    if (vertices_.find(key) != vertices_.end()) return std::unexpected(status_t::PATH_IN_USE);
    auto vertex = std::make_unique<vertex_t>(role, key, settings, std::move(handlers));
    vertex_t* ptr = vertex.get();
    vertices_.emplace(std::move(key), std::move(vertex));
    return ptr;
}

vertex_t* graph_t::find(std::span<const std::byte> key) const {
    path_key_t k{std::vector<std::byte>(key.begin(), key.end())};
    const std::shared_lock lock(map_mutex_);
    const auto it = vertices_.find(k);
    return it == vertices_.end() ? nullptr : it->second.get();
}

result_t<view_t> graph_t::read(vertex_t* v) const {
    if (v->role_ == role_t::HANDLER) {
        if (v->handlers_.on_read) return v->handlers_.on_read();
        return std::unexpected(status_t::NOT_FOUND);
    }
    const std::shared_ptr<const view_t> sp = v->lkv_.load();  // lock-free
    if (!sp) return std::unexpected(status_t::NOT_FOUND);
    return *sp;  // copies the view_t => clones the segment_ptr_t (refcount bump, no byte copy)
}

void graph_t::fan_out(vertex_t* v, const view_t& value, int depth) {
    // Evaluate the per-subscriber delivery policy UNDER the lock (ON_CHANGE compares
    // and updates last_delivered, which lives on the real subscriber), then dispatch
    // the survivors OUTSIDE the lock (callbacks / re-dispatch may re-enter the graph).
    std::vector<subscriber_t> to_dispatch;
    {
        const std::lock_guard lock(v->m_);
        const std::span<const std::byte> bytes = value.bytes();
        for (subscriber_t& s : v->subs_) {
            if (!s.active) continue;
            if (s.mode == delivery_mode_t::ON_CHANGE) {
                if (std::equal(s.last_delivered.begin(), s.last_delivered.end(), bytes.begin(),
                               bytes.end())) {
                    continue;  // suppressed: value unchanged since last delivery
                }
                s.last_delivered.assign(bytes.begin(), bytes.end());
            }
            // Copy only the dispatch-relevant fields (not last_delivered).
            to_dispatch.push_back(subscriber_t{s.target_key, s.callback, s.mode, {}, true});
        }
    }
    for (const subscriber_t& s : to_dispatch) {
        if (s.callback) s.callback(value);  // cloned view
        if (!s.target_key.empty() && depth + 1 < kMaxDispatchDepth) {
            if (vertex_t* target = find(s.target_key)) {
                (void)write_impl(target, value, depth + 1);  // value cloned into the param
            }
        }
    }
}

result_t<void> graph_t::write_impl(vertex_t* v, view_t value, int depth) {
    if (v->role_ == role_t::HANDLER) {
        if (!v->handlers_.on_write) return std::unexpected(status_t::NOT_FOUND);
        result_t<void> r = v->handlers_.on_write(value);
        if (!r) return r;
        {
            const std::lock_guard lock(v->m_);
            ++v->write_seq_;
            v->cv_.notify_all();
        }
        fan_out(v, value, depth);
        return {};
    }

    auto sp = std::make_shared<const view_t>(std::move(value));
    v->lkv_.store(sp);  // lock-free publish of the new last-known-value

    {
        const std::lock_guard lock(v->m_);
        if (v->role_ == role_t::STREAM) {
            v->history_.push_back(sp);
            const std::size_t keep =
                v->settings_.history_keep_last ? v->settings_.history_keep_last : 1;
            while (v->history_.size() > keep) v->history_.pop_front();
        }
        ++v->write_seq_;
        v->cv_.notify_all();
    }
    fan_out(v, *sp, depth);
    return {};
}

result_t<void> graph_t::write(vertex_t* v, view_t value) {
    return write_impl(v, std::move(value), 0);
}

result_t<void> graph_t::write(vertex_t* v, const field_path_t& field, view_t value) {
    if (field.empty()) return write_impl(v, std::move(value), 0);
    return field_write(v, field, value);
}

result_t<view_t> graph_t::await(vertex_t* v, std::chrono::nanoseconds timeout) {
    std::unique_lock lock(v->m_);
    const std::uint64_t seq0 = v->write_seq_;
    if (!v->cv_.wait_for(lock, timeout, [&] { return v->write_seq_ != seq0; })) {
        return std::unexpected(status_t::TIMEOUT);
    }
    lock.unlock();
    const std::shared_ptr<const view_t> sp = v->lkv_.load();
    if (!sp) return std::unexpected(status_t::NOT_FOUND);  // e.g. a Handler-role write
    return *sp;
}

result_t<std::vector<view_t>> graph_t::history(vertex_t* v) const {
    if (v->role_ != role_t::STREAM) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    const std::lock_guard lock(v->m_);
    std::vector<view_t> out;
    out.reserve(v->history_.size());
    for (const auto& sp : v->history_) out.push_back(*sp);  // clone each (refcount bump)
    return out;
}

result_t<void> graph_t::subscribe(const path_t& src, const path_t& target, delivery_mode_t mode) {
    vertex_t* v = find(src.key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    subscriber_t s;
    s.target_key.assign(target.key().begin(), target.key().end());
    s.mode = mode;
    const std::lock_guard lock(v->m_);
    v->subs_.push_back(std::move(s));
    return {};
}

result_t<void> graph_t::subscribe(const path_t& src, std::function<void(const view_t&)> callback,
                                  delivery_mode_t mode) {
    vertex_t* v = find(src.key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    subscriber_t s;
    s.callback = std::move(callback);
    s.mode = mode;
    const std::lock_guard lock(v->m_);
    v->subs_.push_back(std::move(s));
    return {};
}

result_t<void> graph_t::field_write(vertex_t* v, const field_path_t& field, const view_t& value) {
    const field_step_t& step0 = field.steps[0];

    if (step0.name == "subscribers") {
        if (step0.append) {
            const auto sub = view_as_tlv(value);
            if (!sub || sub->type != type_t::SUBSCRIBER)
                return std::unexpected(status_t::TYPE_MISMATCH);
            subscriber_t s;
            for (const auto& child : sub->children) {
                if (child.type == type_t::PATH) {
                    s.target_key = path_child_key(child);
                    break;
                }
            }
            if (s.target_key.empty()) return std::unexpected(status_t::TYPE_MISMATCH);
            const std::lock_guard lock(v->m_);
            v->subs_.push_back(std::move(s));
            return {};
        }
        if (step0.indexed) {  // clear a subscriber slot (unsubscribe)
            const std::lock_guard lock(v->m_);
            if (step0.index < v->subs_.size()) v->subs_[step0.index].active = false;
            return {};
        }
        return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    }

    if (step0.name == "settings" && field.steps.size() >= 2) {
        const auto tlv = view_as_tlv(value);
        if (!tlv || tlv->type != type_t::VALUE) return std::unexpected(status_t::TYPE_MISMATCH);
        const std::uint64_t n = detail::load_le(tlv->payload);
        const std::string& f = field.steps[1].name;
        const std::lock_guard lock(v->m_);
        if (f == "reliability") {
            v->settings_.reliability = static_cast<std::uint8_t>(n);
        } else if (f == "durability") {
            v->settings_.durability = static_cast<std::uint8_t>(n);
        } else if (f == "priority") {
            v->settings_.priority = static_cast<std::uint8_t>(n);
        } else if (f == "history_keep_last") {
            v->settings_.history_keep_last = static_cast<std::uint32_t>(n);
        } else if (f == "queue_max_bytes") {
            v->settings_.queue_max_bytes = static_cast<std::uint32_t>(n);
        } else if (f == "deadline_ns") {
            v->settings_.deadline_ns = n;
        } else {
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        }
        return {};
    }

    return std::unexpected(status_t::SCHEMA_NOT_FOUND);
}

result_t<view_t> graph_t::read_schema(vertex_t* v) const {
    settings_t s;
    {
        const std::lock_guard lock(v->m_);
        s = v->settings_;
    }
    // POINT { NAME <vertex name>, SETTINGS { NAME "deadline_ns" VALUE u64,
    //                                        NAME "history_keep_last" VALUE u32 } }
    std::vector<std::byte> settings_children;
    detail::emit_name(settings_children, "deadline_ns");
    emit_value(settings_children, s.deadline_ns, 8);
    detail::emit_name(settings_children, "history_keep_last");
    emit_value(settings_children, s.history_keep_last, 4);

    std::vector<std::byte> point_body;
    detail::emit_name(point_body, last_segment(v->key_.bytes));
    detail::emit_tlv(point_body, type_t::SETTINGS, opt_t{.pl = true},
                     settings_children);  // SETTINGS

    std::vector<std::byte> point;
    detail::emit_tlv(point, type_t::POINT, opt_t{.pl = true}, point_body);  // POINT

    segment_ptr_t seg = view::heap_alloc(point.size());
    if (!seg) return std::unexpected(status_t::BACKPRESSURE);
    std::memcpy(seg->bytes.data(), point.data(), point.size());
    return view_t::over(std::move(seg));
}

result_t<view_t> graph_t::read(const path_t& path) const {
    vertex_t* v = find(path.key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    if (!path.field().empty()) {
        const field_path_t& f = path.field();
        if (f.steps.size() == 1 && f.steps[0].name == "schema") return read_schema(v);
        return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    }
    return read(v);
}

result_t<void> graph_t::write(const path_t& path, view_t value) {
    vertex_t* v = find(path.key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    return write(v, path.field(), std::move(value));  // handle-based; see the vertex_t* overload
}

result_t<view_t> graph_t::await(const path_t& path, std::chrono::nanoseconds timeout) {
    vertex_t* v = find(path.key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    return await(v, timeout);
}

}  // namespace tr::graph
