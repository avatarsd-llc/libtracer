// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

#include "libtracer/graph.hpp"

#include <cstring>
#include <mutex>
#include <string_view>
#include <utility>

#include "libtracer/frame.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/view.hpp"

namespace tracer::graph {
namespace {

// --- small TLV byte emitters for building a :schema POINT (no Tlv-model needed) -
void emit_tlv(std::vector<std::byte>& out, std::uint8_t type, bool pl,
              std::span<const std::byte> body) {
    const auto len = static_cast<std::uint16_t>(body.size());
    out.push_back(static_cast<std::byte>(type));
    out.push_back(static_cast<std::byte>(pl ? 0x40 : 0x00));
    out.push_back(static_cast<std::byte>(len & 0xFF));
    out.push_back(static_cast<std::byte>((len >> 8) & 0xFF));
    out.insert(out.end(), body.begin(), body.end());
}

void emit_name(std::vector<std::byte>& out, std::span<const std::byte> name) {
    emit_tlv(out, 0x02, false, name);  // NAME
}

void emit_name(std::vector<std::byte>& out, std::string_view name) {
    std::vector<std::byte> b(name.size());
    for (std::size_t i = 0; i < name.size(); ++i)
        b[i] = static_cast<std::byte>(static_cast<unsigned char>(name[i]));
    emit_name(out, b);
}

void emit_value(std::vector<std::byte>& out, std::uint64_t value, int width) {
    std::vector<std::byte> payload(static_cast<std::size_t>(width));
    for (int i = 0; i < width; ++i)
        payload[static_cast<std::size_t>(i)] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
    emit_tlv(out, 0x01, false, payload);  // VALUE
}

// Read a little-endian unsigned of up to 8 bytes from a payload span.
[[nodiscard]] std::uint64_t read_le(std::span<const std::byte> p) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < p.size() && i < 8; ++i)
        v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i])) << (8 * i);
    return v;
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
[[nodiscard]] std::vector<std::byte> path_child_key(const Tlv& path) {
    std::vector<std::byte> key;
    for (const auto& name : path.children) {
        const auto enc = encode(name);  // a NAME TLV: 02 00 <len> <bytes>
        key.insert(key.end(), enc.begin(), enc.end());
    }
    return key;
}

}  // namespace

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

void Graph::fan_out(Vertex* v, const View& value, int depth) {
    std::vector<Subscriber> snapshot;
    {
        const std::lock_guard lock(v->m_);
        snapshot = v->subs_;  // copy, then dispatch outside the lock (callbacks may re-enter)
    }
    for (const auto& s : snapshot) {
        if (!s.active) continue;
        if (s.callback) s.callback(value);  // callbacks always fire (cloned view)
        if (!s.target_key.empty() && depth + 1 < kMaxDispatchDepth) {
            if (Vertex* target = find(s.target_key)) {
                (void)write_impl(target, value, depth + 1);  // value copied (clone) into the param
            }
        }
    }
}

Result<void> Graph::write_impl(Vertex* v, View value, int depth) {
    if (v->role_ == Role::Handler) {
        if (!v->handlers_.on_write) return std::unexpected(Status::NotFound);
        Result<void> r = v->handlers_.on_write(value);
        if (!r) return r;
        {
            const std::lock_guard lock(v->m_);
            ++v->write_seq_;
            v->cv_.notify_all();
        }
        fan_out(v, value, depth);
        return {};
    }

    auto sp = std::make_shared<const View>(std::move(value));
    v->lkv_.store(sp);  // lock-free publish of the new last-known-value

    {
        const std::lock_guard lock(v->m_);
        if (v->role_ == Role::Stream) {
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

Result<void> Graph::write(Vertex* v, View value) { return write_impl(v, std::move(value), 0); }

Result<void> Graph::write(Vertex* v, const FieldPath& field, View value) {
    if (field.empty()) return write_impl(v, std::move(value), 0);
    return field_write(v, field, value);
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

Result<void> Graph::subscribe(const Path& src, const Path& target) {
    Vertex* v = find(src.key());
    if (!v) return std::unexpected(Status::NotFound);
    Subscriber s;
    s.target_key.assign(target.key().begin(), target.key().end());
    const std::lock_guard lock(v->m_);
    v->subs_.push_back(std::move(s));
    return {};
}

Result<void> Graph::subscribe(const Path& src, std::function<void(const View&)> callback) {
    Vertex* v = find(src.key());
    if (!v) return std::unexpected(Status::NotFound);
    Subscriber s;
    s.callback = std::move(callback);
    const std::lock_guard lock(v->m_);
    v->subs_.push_back(std::move(s));
    return {};
}

Result<void> Graph::field_write(Vertex* v, const FieldPath& field, const View& value) {
    const FieldStep& step0 = field.steps[0];

    if (step0.name == "subscribers") {
        if (step0.append) {
            const auto sub = view_as_tlv(value);
            if (!sub || sub->type != Type::Subscriber) return std::unexpected(Status::TypeMismatch);
            Subscriber s;
            for (const auto& child : sub->children) {
                if (child.type == Type::Path) {
                    s.target_key = path_child_key(child);
                    break;
                }
            }
            if (s.target_key.empty()) return std::unexpected(Status::TypeMismatch);
            const std::lock_guard lock(v->m_);
            v->subs_.push_back(std::move(s));
            return {};
        }
        if (step0.indexed) {  // clear a subscriber slot (unsubscribe)
            const std::lock_guard lock(v->m_);
            if (step0.index < v->subs_.size()) v->subs_[step0.index].active = false;
            return {};
        }
        return std::unexpected(Status::SchemaNotFound);
    }

    if (step0.name == "settings" && field.steps.size() >= 2) {
        const auto tlv = view_as_tlv(value);
        if (!tlv || tlv->type != Type::Value) return std::unexpected(Status::TypeMismatch);
        const std::uint64_t n = read_le(tlv->payload);
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
            return std::unexpected(Status::SchemaNotFound);
        }
        return {};
    }

    return std::unexpected(Status::SchemaNotFound);
}

Result<View> Graph::read_schema(Vertex* v) const {
    Settings s;
    {
        const std::lock_guard lock(v->m_);
        s = v->settings_;
    }
    // POINT { NAME <vertex name>, SETTINGS { NAME "deadline_ns" VALUE u64,
    //                                        NAME "history_keep_last" VALUE u32 } }
    std::vector<std::byte> settings_children;
    emit_name(settings_children, "deadline_ns");
    emit_value(settings_children, s.deadline_ns, 8);
    emit_name(settings_children, "history_keep_last");
    emit_value(settings_children, s.history_keep_last, 4);

    std::vector<std::byte> point_body;
    emit_name(point_body, last_segment(v->key_.bytes));
    emit_tlv(point_body, 0x0B, true, settings_children);  // SETTINGS

    std::vector<std::byte> point;
    emit_tlv(point, 0x07, true, point_body);  // POINT

    SegmentPtr seg = mem::heap_alloc(point.size());
    if (!seg) return std::unexpected(Status::Backpressure);
    std::memcpy(seg->bytes.data(), point.data(), point.size());
    return View::over(std::move(seg));
}

Result<View> Graph::read(const Path& path) const {
    Vertex* v = find(path.key());
    if (!v) return std::unexpected(Status::NotFound);
    if (!path.field().empty()) {
        const FieldPath& f = path.field();
        if (f.steps.size() == 1 && f.steps[0].name == "schema") return read_schema(v);
        return std::unexpected(Status::SchemaNotFound);
    }
    return read(v);
}

Result<void> Graph::write(const Path& path, View value) {
    Vertex* v = find(path.key());
    if (!v) return std::unexpected(Status::NotFound);
    return write(v, path.field(), std::move(value));  // handle-based; see the Vertex* overload
}

Result<View> Graph::await(const Path& path, std::chrono::nanoseconds timeout) {
    Vertex* v = find(path.key());
    if (!v) return std::unexpected(Status::NotFound);
    return await(v, timeout);
}

}  // namespace tracer::graph
