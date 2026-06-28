/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/graph.hpp"

#include <algorithm>
#include <array>
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
    // and updates last_delivered, which lives on the real subscriber), snapshotting
    // the survivors, then dispatch them OUTSIDE the lock (callbacks / re-dispatch may
    // re-enter the graph). The snapshot carries only the dispatch-relevant fields;
    // small fan-out (the common case) uses a stack buffer — no per-publish heap
    // allocation — and large fan-out reserves the heap vector exactly once.
    struct disp_t {
        std::function<void(const view_t&)> callback;
        std::vector<std::byte> target_key;
    };
    constexpr std::size_t kInlineFanout = 8;
    std::array<disp_t, kInlineFanout> inline_buf;
    std::vector<disp_t> heap_buf;
    std::size_t inline_n = 0;
    bool use_heap = false;
    {
        const std::lock_guard lock(v->m_);
        const std::span<const std::byte> bytes = value.bytes();
        if (v->subs_.size() > kInlineFanout) {
            use_heap = true;
            heap_buf.reserve(v->subs_.size());
        }
        for (subscriber_t& s : v->subs_) {
            if (!s.active) continue;
            if (s.mode == delivery_mode_t::ON_CHANGE) {
                if (std::equal(s.last_delivered.begin(), s.last_delivered.end(), bytes.begin(),
                               bytes.end())) {
                    continue;  // suppressed: value unchanged since last delivery
                }
                s.last_delivered.assign(bytes.begin(), bytes.end());
            }
            if (use_heap)
                heap_buf.push_back(disp_t{s.callback, s.target_key});
            else
                inline_buf[inline_n++] = disp_t{s.callback, s.target_key};
        }
    }
    const auto dispatch = [&](const disp_t& d) {
        if (d.callback) d.callback(value);  // cloned view
        if (!d.target_key.empty() && depth + 1 < kMaxDispatchDepth) {
            if (vertex_t* target = find(d.target_key)) {
                (void)write_impl(target, value, depth + 1);  // value cloned into the param
            }
        }
    };
    if (use_heap)
        for (const disp_t& d : heap_buf) dispatch(d);
    else
        for (std::size_t i = 0; i < inline_n; ++i) dispatch(inline_buf[i]);
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
            // Parse the optional qos_settings SETTINGS for the route-handle opt-in
            // (NAME "delivery_compact" VALUE u8, RFC-0004 §E.1 / docs/reference/05).
            // Back-compat: a SUBSCRIBER without it (or an older parser) just keeps
            // the full-route delivery path — existing conformance vectors unaffected.
            for (const auto& child : sub->children) {
                if (child.type != type_t::SETTINGS) continue;
                const std::vector<tlv_t>& q = child.children;
                for (std::size_t i = 0; i + 1 < q.size(); ++i) {
                    if (q[i].type != type_t::NAME || q[i + 1].type != type_t::VALUE) continue;
                    const std::span<const std::byte> nm = q[i].payload;
                    const std::string_view name(reinterpret_cast<const char*>(nm.data()),
                                                nm.size());
                    if (name == "delivery_compact")
                        s.delivery_compact = detail::load_le<std::uint8_t>(q[i + 1].payload) != 0;
                }
            }
            s.source_view = value;  // retain the SUBSCRIBER TLV zero-copy (refcount clone) so a
                                    // later :subscribers[] read ropes it into the REPLY (ADR-0035).
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

    if (step0.name == "acl") {
        // STRUCTURAL STORAGE ONLY (#81-A, ADR-0018/0020): validate the value is an ACL TLV and
        // stash its raw bytes opaquely — we do NOT parse the NFSv4 ACE children or enforce them.
        // Enforcement (read/write/subscribe gating) is the deferred security_acl module.
        const auto acl = view_as_tlv(value);
        if (!acl || acl->type != type_t::ACL) return std::unexpected(status_t::TYPE_MISMATCH);
        const std::span<const std::byte> bytes = value.bytes();
        const std::lock_guard lock(v->m_);
        v->acl_.assign(bytes.begin(), bytes.end());  // storing replaces; empty => no restrictions
        return {};
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

result_t<view_t> graph_t::read_acl(vertex_t* v) const {
    // Serve back the raw :acl TLV bytes stored by field_write (heap-alloc + copy, like
    // read_schema), or NOT_FOUND when none was set. Opaque: no ACE parsing, no enforcement.
    std::vector<std::byte> acl;
    {
        const std::lock_guard lock(v->m_);
        if (v->acl_.empty()) return std::unexpected(status_t::NOT_FOUND);
        acl = v->acl_;
    }
    segment_ptr_t seg = view::heap_alloc(acl.size());
    if (!seg) return std::unexpected(status_t::BACKPRESSURE);
    std::memcpy(seg->bytes.data(), acl.data(), acl.size());
    return view_t::over(std::move(seg));
}

result_t<view_t> graph_t::read(vertex_t* v, const field_path_t& field) const {
    if (field.empty()) return read(v);
    if (field.steps.size() == 1 && field.steps[0].name == "schema") return read_schema(v);
    if (field.steps.size() == 1 && field.steps[0].name == "acl") return read_acl(v);
    // A single subscriber slot ":subscribers[N]" — serve the stored SUBSCRIBER view (clone).
    if (field.steps.size() == 1 && field.steps[0].name == "subscribers" && field.steps[0].indexed &&
        !field.steps[0].append && !field.steps[0].wildcard) {
        const std::lock_guard lock(v->m_);
        const std::size_t idx = field.steps[0].index;
        if (idx < v->subs_.size() && v->subs_[idx].active && v->subs_[idx].source_view.owner)
            return v->subs_[idx].source_view;  // clone (refcount bump, no byte copy)
        return std::unexpected(status_t::NOT_FOUND);
    }
    return std::unexpected(status_t::SCHEMA_NOT_FOUND);
}

result_t<std::vector<view_t>> graph_t::read_subscribers(vertex_t* v) const {
    const std::lock_guard lock(v->m_);
    std::vector<view_t> out;
    out.reserve(v->subs_.size());
    for (const subscriber_t& s : v->subs_)
        if (s.active && s.source_view.owner) out.push_back(s.source_view);  // clone each (refcount)
    return out;
}

result_t<view_t> graph_t::read(const path_t& path) const {
    vertex_t* v = find(path.key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    if (!path.field().empty()) {
        const field_path_t& f = path.field();
        if (f.steps.size() == 1 && f.steps[0].name == "schema") return read_schema(v);
        if (f.steps.size() == 1 && f.steps[0].name == "acl") return read_acl(v);
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
