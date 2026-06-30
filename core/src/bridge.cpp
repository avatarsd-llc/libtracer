/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/bridge.hpp"

#include <chrono>
#include <cstring>
#include <utility>

#include "libtracer/mem_heap.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/view.hpp"

namespace tr::net {

using wire::decode;
using wire::encode;
using wire::opt_t;
using wire::tlv_t;
using wire::type_t;

// L1 types this layer consumes (upward dependency on tr::view, docs/adr/0016 §2).
using view::segment_ptr_t;
using view::view_t;
namespace {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Per-origin monotonic origin_timestamp — a hybrid logical clock (ADR-0019).
// `origin_timestamp` is the in-flight *identity* (with origin_peer_id), so it must
// be strictly increasing and never collide, even when the wall clock is too coarse
// to separate two writes or steps backward (NTP). HLC rule: max(now, last+1). The
// counter is a process-wide function-local static, so every bridge on this node
// (which all share one origin peer-id) draws from one monotonic source. The CAS
// loop keeps it correct under concurrent exports from multiple transport threads.
std::uint64_t next_origin_ts() {
    static std::atomic<std::uint64_t> last{0};
    const std::uint64_t now = now_ns();
    std::uint64_t prev = last.load(std::memory_order_relaxed);
    std::uint64_t next;
    do {
        next = now > prev ? now : prev + 1;
    } while (!last.compare_exchange_weak(prev, next, std::memory_order_relaxed));
    return next;
}

// A 24-byte recent-set key: origin (16) || ts (8 LE).
std::string recent_key(const router_meta_t& meta) {
    std::string k(meta.origin.size() + 8, '\0');
    std::memcpy(k.data(), meta.origin.data(), meta.origin.size());
    for (int i = 0; i < 8; ++i)
        k[meta.origin.size() + static_cast<std::size_t>(i)] =
            static_cast<char>((meta.ts >> (8 * i)) & 0xFF);
    return k;
}

}  // namespace

bridge_t::bridge_t(graph::graph_t& graph, transport_t& transport, peer_id_t peer)
    : graph_(graph), transport_(transport), peer_(peer) {
    transport_.set_receiver([this](std::span<const std::byte> frame) { on_frame(frame); });
}

graph::result_t<void> bridge_t::export_vertex(const graph::path_t& src) {
    return graph_.subscribe(src, [this](const view_t& value) {
        const router_meta_t meta{.origin = peer_, .ts = next_origin_ts(), .hop = 0};
        const auto frame = router_wrap(value.bytes(), meta);
        transport_.send(frame);
    });
}

void bridge_t::set_mount(const graph::path_t& mount) {
    // Resolve the vertex handle once (the mount vertex must already be registered);
    // the receive thread then writes through it with no string/lookup per frame.
    mount_vertex_.store(graph_.find(mount.key()));
}

void bridge_t::set_status_path(const graph::path_t& status) {
    // Resolve the status vertex once (must already be registered), exactly as
    // set_mount resolves the mount. The receive thread emits through it on a hop-cap
    // drop with no per-frame lookup.
    status_vertex_.store(graph_.find(status.key()));
}

// Build STATUS{ERROR u8=0x0D (NESTING_TOO_DEEP)} and write it to the status path,
// fanned out to that vertex's subscribers — the ADR-0014 "MUST emit a local error"
// on a hop_count cap drop (docs/reference/05 §0x0D, 07 §cycle handling). One owned
// copy at emit time (view::over_bytes), then zero-copy to subscribers. The spec
// reuses NESTING_TOO_DEEP for hop exhaustion; a distinct HOP_LIMIT code is a spec
// change (RFC), not done here (issue #77).
void bridge_t::emit_hop_limit_status() {
    graph::vertex_t* const status = status_vertex_.load(std::memory_order_relaxed);
    if (status == nullptr) return;  // no status path wired — silent drop (counter only)
    std::vector<std::byte> err;
    const std::byte code{0x0D};  // NESTING_TOO_DEEP
    detail::emit_tlv(err, type_t::ERROR, opt_t{}, std::span<const std::byte>(&code, 1));
    std::vector<std::byte> status_tlv;
    detail::emit_tlv(status_tlv, type_t::STATUS, opt_t{.pl = true}, err);
    if (const view_t v = view::over_bytes(status_tlv); !v.empty()) {
        (void)graph_.write(status, v);  // fan-out to status subscribers (zero-copy)
    }
}

void bridge_t::set_recent_set_capacity(std::size_t capacity) { recent_cap_.store(capacity); }

void bridge_t::set_reforward(bool on) { reforward_.store(on); }

bool bridge_t::seen(const router_meta_t& meta) {
    const std::size_t cap = recent_cap_.load(std::memory_order_relaxed);
    if (cap == 0) return false;  // dedup disabled — hop_count alone terminates
    std::string key = recent_key(meta);
    const std::lock_guard lock(m_);
    if (recent_set_.contains(key)) return true;
    recent_set_.insert(key);
    recent_order_.push_back(std::move(key));
    while (recent_order_.size() > cap) {
        recent_set_.erase(recent_order_.front());
        recent_order_.pop_front();
    }
    return false;
}

void bridge_t::on_frame(std::span<const std::byte> frame) {
    const auto unwrapped = router_unwrap(frame);
    if (!unwrapped) return;  // malformed — drop

    if (unwrapped->meta.hop >= kMaxHops) {  // the termination guarantee (ADR-0014)
        hop_dropped_.fetch_add(1);
        emit_hop_limit_status();  // ADR-0014: MUST emit a local error to the status path
        return;
    }
    if (seen(unwrapped->meta)) {
        deduped_.fetch_add(1);
        return;
    }

    if (graph::vertex_t* mount = mount_vertex_.load(std::memory_order_relaxed)) {
        // Materialize the data TLV into an owned heap segment — the frame buffer
        // dies when on_frame returns, but the graph stores the view_t past then.
        // (One copy at the bridge boundary; reference/08 §cross-substrate.)
        const auto data = unwrapped->data;
        // One audited locus for the alloc/copy/over triplet (view::over_bytes). A
        // well-formed ROUTER carries a non-empty data TLV; an empty result is an
        // allocation failure (or a malformed empty-data frame) — skip, don't crash.
        if (view_t v = view::over_bytes(data); !v.empty()) {
            (void)graph_.write(mount, std::move(v));
            delivered_.fetch_add(1);
        }
    }

    if (reforward_.load(std::memory_order_relaxed)) {  // re-emit with hop+1 (cycle test)
        router_meta_t meta = unwrapped->meta;
        meta.hop = static_cast<std::uint8_t>(meta.hop + 1);
        transport_.send(router_wrap(unwrapped->data, meta));
    }
}

}  // namespace tr::net
