// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// The path object (RFC-0030 §5) and the host-API doors that take one.

#include "libtracer/path_object.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "libtracer/graph.hpp"
#include "libtracer/packed_path.hpp"
#include "libtracer/path.hpp"

namespace tr::graph {

namespace {

// How many times a chain read retries a racing learn before it reads no chain. Learns come
// from matched replies, so a reader that loses this many times in a row is losing to a storm,
// and the canonical form is the answer anyway.
constexpr int kChainReadTries = 4;

}  // namespace

std::optional<vertex_slot_t> path_object_t::element0() const noexcept {
    if (b_ == nullptr) return std::nullopt;
    const std::uint64_t e = b_->element0.load(std::memory_order_acquire);
    if (e == kNoElement0) return std::nullopt;
    return vertex_slot_t{.index = static_cast<std::uint32_t>(e),
                         .generation = static_cast<std::uint32_t>(e >> 32)};
}

void path_object_t::set_element0(std::optional<vertex_slot_t> e) const noexcept {
    // A saturated generation is never honoured (deref_vertex_slot), and it is also the high half
    // of kNoElement0, so the two cannot be confused: such an element is simply not kept.
    const std::uint64_t w = e && e->generation != kGenerationSaturated
                                ? (std::uint64_t{e->generation} << 32) | e->index
                                : kNoElement0;
    b_->element0.store(w, std::memory_order_release);
}

std::size_t path_object_t::chain(std::span<std::byte> out) const noexcept {
    if (b_ == nullptr) return 0;
    // The reader half of a seqlock: an even, unchanged counter around the copy proves no learn
    // overlapped it. The words are atomics so the race a retry discards is not a data race.
    for (int t = 0; t < kChainReadTries; ++t) {
        const std::uint32_t s = b_->seq.load(std::memory_order_acquire);
        if ((s & 1U) != 0) continue;
        const std::uint32_t len = b_->chain_len.load(std::memory_order_relaxed);
        if (len == 0 || len > out.size()) return 0;
        for (std::size_t w = 0; w * 8 < len; ++w) {
            const std::uint64_t word = b_->chain[w].load(std::memory_order_relaxed);
            std::memcpy(out.data() + w * 8, &word, std::min<std::size_t>(8, len - w * 8));
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (b_->seq.load(std::memory_order_relaxed) == s) return len;
    }
    return 0;
}

bool path_object_t::learn(std::span<const std::byte> chain) noexcept {
    if (b_ == nullptr || chain.empty()) return false;
    if (chain.size() > kPathChainCapacity) {
        // The reply taught a chain this build cannot hold, so the one held is no longer the
        // route: drop it and stay canonical (§5.4).
        clear_chain();
        return false;
    }
    // The writer half: take the counter odd, or give up when another learn holds it.
    std::uint32_t s = b_->seq.load(std::memory_order_relaxed);
    if ((s & 1U) != 0 || !b_->seq.compare_exchange_strong(s, s + 1, std::memory_order_relaxed))
        return false;
    std::atomic_thread_fence(std::memory_order_release);
    for (std::size_t w = 0; w * 8 < chain.size(); ++w) {
        std::uint64_t word = 0;
        std::memcpy(&word, chain.data() + w * 8, std::min<std::size_t>(8, chain.size() - w * 8));
        b_->chain[w].store(word, std::memory_order_relaxed);
    }
    b_->chain_len.store(static_cast<std::uint32_t>(chain.size()), std::memory_order_relaxed);
    b_->seq.store(s + 2, std::memory_order_release);
    return true;
}

void path_object_t::clear_chain() noexcept {
    // Length 0 is "no chain", and a single store needs no counter: a reader that loaded the old
    // length before it either finishes its copy and is confirmed by the counter, or reads 0.
    if (b_ != nullptr) b_->chain_len.store(0, std::memory_order_relaxed);
}

void path_object_t::release() noexcept {
    if (b_ == nullptr) return;
    mem::block_source_t* const src = b_->src;
    const std::size_t bytes = b_->block_bytes;
    b_->~body_t();
    src->release(b_, bytes, alignof(body_t));
    b_ = nullptr;
}

result_t<path_object_t> graph_t::make_path(std::string_view text) {
    const result_t<path_t> parsed = path_t::parse(text, *tables_);
    if (!parsed) return std::unexpected(parsed.error());
    if (!parsed->field().empty()) return std::unexpected(status_t::INVALID_PATH);
    const std::span<const std::byte> key = parsed->key();
    const std::size_t bytes = sizeof(path_object_t::body_t) + key.size();
    void* const mem = tables_->try_alloc(bytes, alignof(path_object_t::body_t));
    if (mem == nullptr) return std::unexpected(status_t::BACKPRESSURE);
    auto* const b = new (mem) path_object_t::body_t{};
    auto* const tail = reinterpret_cast<std::byte*>(b + 1);
    if (!key.empty()) std::memcpy(tail, key.data(), key.size());
    b->src = tables_;
    b->block_bytes = bytes;
    b->key = tail;
    b->key_len = static_cast<std::uint32_t>(key.size());
    b->element0.store(path_object_t::kNoElement0, std::memory_order_relaxed);
    return path_object_t{b};
}

result_t<path_object_t> graph_t::make_path(std::span<const std::byte> key) {
    if (key.size() > kMaxPathBytes || !wire::packed_path_valid_key(key))
        return std::unexpected(status_t::INVALID_PATH);
    void* const mem =
        tables_->try_alloc(sizeof(path_object_t::body_t), alignof(path_object_t::body_t));
    if (mem == nullptr) return std::unexpected(status_t::BACKPRESSURE);
    auto* const b = new (mem) path_object_t::body_t{};
    b->src = tables_;
    b->block_bytes = sizeof(path_object_t::body_t);
    b->key = key.data();
    b->key_len = static_cast<std::uint32_t>(key.size());
    b->element0.store(path_object_t::kNoElement0, std::memory_order_relaxed);
    return path_object_t{b};
}

std::optional<vertex_handle_t> graph_t::resolve(const path_object_t& p) const {
    if (p.empty()) return std::nullopt;
    if (const std::optional<vertex_slot_t> e = p.element0())
        if (const std::optional<vertex_handle_t> vh = deref_vertex_slot(e->index, e->generation))
            return vh;
    // No element 0, or a stale one (§5.2 Fallback): the canonical bytes, which always answer.
    vertex_t* const v = find_ptr(p.key());
    if (v == nullptr) {
        p.set_element0(std::nullopt);
        return std::nullopt;
    }
    const vertex_handle_t vh{v};
    p.set_element0(vertex_slot(vh));
    return vh;
}

result_t<value_ref_t> graph_t::read(const path_object_t& p, std::string_view caller) const {
    const std::optional<vertex_handle_t> vh = resolve(p);
    if (!vh) return std::unexpected(status_t::NOT_FOUND);
    return read(*vh, caller);
}

result_t<value_ref_t> graph_t::read(const path_object_t& p, const field_path_t& field,
                                    std::string_view caller) const {
    const std::optional<vertex_handle_t> vh = resolve(p);
    if (!vh) return std::unexpected(status_t::NOT_FOUND);
    return read(*vh, field, caller);
}

result_t<void> graph_t::write(const path_object_t& p, view::rope_t value, std::string_view caller) {
    if (const std::optional<vertex_handle_t> vh = resolve(p))
        return write_impl(vh->get(), std::move(value), caller, nullptr);
    if (p.empty()) return std::unexpected(status_t::NOT_FOUND);
    // A miss creates only where a parent's creation hook admits it (RFC-0030 §7), as the string
    // form's does; the vertex it makes is the one the object names from now on.
    const result_t<vertex_t*> made =
        find_or_create_ptr(p.key(), caller, [&]() -> const view::rope_t& { return value; });
    if (!made) return std::unexpected(made.error());
    p.set_element0(vertex_slot(vertex_handle_t{*made}));
    return write_impl(*made, std::move(value), caller, nullptr);
}

result_t<void> graph_t::write(const path_object_t& p, const field_path_t& field, view::rope_t value,
                              std::string_view caller) {
    const std::optional<vertex_handle_t> vh = resolve(p);
    if (!vh) return std::unexpected(status_t::NOT_FOUND);
    return write(*vh, field, std::move(value), caller);
}

result_t<subscription_t> graph_t::subscribe(const path_object_t& src, subscriber_fn_t fn, void* ctx,
                                            delivery_policy_t policy) {
    const std::optional<vertex_handle_t> vh = resolve(src);
    if (!vh) return std::unexpected(status_t::NOT_FOUND);
    subscriber_t s;
    s.callback = fn;
    s.callback_ctx = ctx;
    s.policy = policy;
    return admit_subscriber(vh->get(), std::move(s), {});
}

result_t<void> graph_t::retire(const path_object_t& p) {
    const std::optional<vertex_handle_t> vh = resolve(p);
    if (!vh) return std::unexpected(status_t::NOT_FOUND);
    const result_t<void> r = retire(*vh);
    if (r) p.set_element0(std::nullopt);
    return r;
}

}  // namespace tr::graph
