/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/transport_can.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <optional>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/config_reader.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/segment.hpp"
#include "libtracer/view.hpp"

namespace tr::net {

namespace {

/**
 * @brief The canonical bus-peer name (ADR-0044): 'n' + the node id in decimal, no leading zeros —
 *        deterministic, collision-safe within the bus, and small enough for a stack buffer (13-bit
 *        node => at most 4 digits).
 */
struct peer_name_buf_t {
    std::array<char, 8> buf{};
    std::size_t len = 0;
    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view(buf.data(), len);
    }
};

[[nodiscard]] peer_name_buf_t format_peer_name(std::uint16_t node) {
    peer_name_buf_t out;
    out.buf[0] = 'n';
    const auto [end, ec] = std::to_chars(out.buf.data() + 1, out.buf.data() + out.buf.size(), node);
    out.len = ec == std::errc{} ? static_cast<std::size_t>(end - out.buf.data()) : 0;
    return out;
}

/**
 * @brief The exact inverse of format_peer_name: nullopt for anything non-canonical (wrong prefix,
 *        empty digits, leading zero, non-digit, out of the node range).
 */
[[nodiscard]] std::optional<std::uint16_t> parse_peer_name(std::string_view name) {
    if (name.size() < 2 || name.front() != 'n') return std::nullopt;
    const std::string_view digits = name.substr(1);
    if (digits.size() > 1 && digits.front() == '0') return std::nullopt;  // non-canonical
    std::uint32_t v = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10u + static_cast<std::uint32_t>(c - '0');
        if (v > can::kNodeMax) return std::nullopt;
    }
    return static_cast<std::uint16_t>(v);
}

/**
 * @brief Reassembly identity for a group is derived purely from the CAN ID: the `node` sub-field
 *        becomes the 16-byte origin and the group's base endpoint becomes the `ts`.
 *
 * Both peers compute it the same way from the same id, so no per-frame
 * origin/ts ever rides the constrained bus (header-elided).
 */
tr::net::can_origin_id_t origin_of(std::uint16_t node) {
    tr::net::can_origin_id_t o{};
    o[0] = static_cast<std::uint8_t>(node & 0xFFu);
    o[1] = static_cast<std::uint8_t>((node >> 8) & 0xFFu);
    return o;
}

tr::net::reassembly_key_t key_of(std::uint16_t node, std::uint16_t base_endpoint) {
    return tr::net::reassembly_key_t{origin_of(node), static_cast<std::uint64_t>(base_endpoint)};
}

/**
 * @brief The monotonic stamp the reassembly buffer ages groups against: steady-clock
 *        milliseconds, the same unit `rx_ttl` is expressed in.
 *
 * The buffer holds no clock of its own (it is a pure framing primitive), so the
 * transport — which already reads `steady_clock` once per inbound frame for the
 * ADR-0044 last-heard table — feeds it this.
 */
[[nodiscard]] std::uint64_t stamp_ms(std::chrono::steady_clock::time_point t) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch());
    return static_cast<std::uint64_t>(ms.count());
}

// (removed) own_copy — the alloc/copy/over triplet now lives in one audited
// locus, tr::view::over_bytes (mem_heap.hpp). Call sites use it directly.

}  // namespace

// socketcan_link_t lives in its OWN translation unit (src/socketcan_link.cpp,
// Linux-only; src/socketcan_link_stub.cpp elsewhere) — platform selection is a
// build-system concern, never an in-source #ifdef. This TU stays 100% portable:
// the transport talks only to the can_link_t seam.

transport_can::transport_can(std::unique_ptr<can_link_t> link, transport_can_config_t config)
    : link_(std::move(link)),
      cfg_(std::move(config)),
      // The injected-bound seam the reassembly buffer was built around, finally
      // reached (#912): before this it was default-constructed, so max_groups was
      // 0 and its evict-oldest never fired. Both RX buffers draw from the same
      // injected resource — the RX thread never reaches the global heap.
      reasm_(cfg_.reasm_mr, cfg_.max_groups),
      pending_(cfg_.reasm_mr) {
    // The RX staleness window is derived, not invented: left at zero it tracks the
    // configured peer liveness window, since RX state a peer would have completed
    // is dead once that peer is itself considered gone.
    if (cfg_.rx_ttl <= kCanRxTtlFromPeerTtl) cfg_.rx_ttl = cfg_.peer_ttl;
    // ...and a window that is still non-positive after the derivation means the
    // peer window is itself zero, i.e. NOTHING is live. Normalize it to zero here
    // so all three consumers read one value and read it the same way: the peer
    // enumeration (`now - last_heard > peer_ttl`), the group sweep
    // (`sweep_stale(0)`), and `expire_pending` each then retain only what arrived
    // this instant. Before this, zero meant "instantly expired" to the enumeration
    // and "never expire" to the pending age-out, so a configured `peer_ttl_ms=0`
    // silently disabled the one bound that holds under the default `max_pending=0`
    // — the exact unbounded growth #912 exists to close. A negative window was
    // worse than inconsistent: `static_cast<std::uint64_t>` of it made
    // `sweep_stale` compare against ~1.8e19 and reclaim nothing at all.
    if (cfg_.rx_ttl < std::chrono::milliseconds::zero()) {
        cfg_.rx_ttl = std::chrono::milliseconds::zero();
    }
    // Resolve the slice-byte seam ONCE (#911). `nullptr` means the process heap, which
    // is what this path used unconditionally before; resolving here rather than
    // branching per slice keeps the RX path at one indirect call either way.
    rx_backend_ = cfg_.rx_backend != nullptr ? cfg_.rx_backend : &tr::mem::heap_backend();
    link_->on_receive([this](const can_frame_data_t& f) { on_rx(f); });
    // Announce presence at join (ADR-0044): a hello advertise (slice_count == 0)
    // seeds every listener's last-heard table before any data flows, so a fresh
    // node is enumerable immediately. Liveness thereafter refreshes with traffic.
    emit_hello();
}

transport_can::~transport_can() {
    // Drop the receivers first (both the peer-named slot and the flat fallback);
    // then releasing the link stops its receive thread, which can no longer
    // re-enter a half-destroyed transport.
    peer_rx_.set(nullptr, nullptr);
    peer_rx_.set_rope(nullptr, nullptr);
    rx_.set(nullptr, nullptr);
    rx_.set_rope(nullptr, nullptr);
    link_.reset();
}

std::optional<can::advertise_t> transport_can::learned_binding(std::uint32_t base_can_id) const {
    const std::lock_guard lock(const_cast<std::mutex&>(rx_m_));
    const auto it = learned_.find(base_can_id);
    if (it == learned_.end()) return std::nullopt;
    return it->second.adv;
}

std::uint64_t transport_can::dropped_groups() const {
    // reasm_ is plain (non-atomic) RX-thread state, so its counter is read under
    // the ingress lock — introspection, never a hot path.
    const std::lock_guard lock(const_cast<std::mutex&>(rx_m_));
    return reasm_.dropped_groups();
}

std::size_t transport_can::pending_slices() const {
    const std::lock_guard lock(const_cast<std::mutex&>(rx_m_));
    return pending_.size();
}

// --- the bus capability (ADR-0044) -------------------------------------------

void transport_can::touch_peer(std::uint16_t node, std::chrono::steady_clock::time_point now) {
    const std::lock_guard lock(peers_m_);
    // Insert-only, one entry per DISTINCT node id ever heard (the learned_ map's
    // policy): growth tracks the bus population — structurally bounded by the
    // 13-bit id space, never per-frame — and an existing entry only refreshes.
    const auto [it, fresh] = peers_.try_emplace(node);
    it->second.last_heard = now;
    if (fresh) {
        it->second.endpoint.owner_ = this;
        it->second.endpoint.node_.store(node, std::memory_order_relaxed);
    }
}

void transport_can::enumerate_peers(const peer_visitor_t& visit) const {
    const auto now = std::chrono::steady_clock::now();
    const std::lock_guard lock(peers_m_);
    for (const auto& [node, e] : peers_) {
        if (now - e.last_heard > cfg_.peer_ttl) continue;  // expired = inaudible
        const peer_name_buf_t name = format_peer_name(node);
        visit(name.view());
    }
}

transport_t* transport_can::peer_link(std::string_view peer) {
    const std::optional<std::uint16_t> node = parse_peer_name(peer);
    if (!node) return nullptr;
    const auto now = std::chrono::steady_clock::now();
    const std::lock_guard lock(peers_m_);
    const auto it = peers_.find(*node);
    if (it == peers_.end() || now - it->second.last_heard > cfg_.peer_ttl) return nullptr;
    return &it->second.endpoint;
}

void transport_can::peer_endpoint_t::send(std::span<const std::byte> frame) {
    owner_->send_impl(frame, node_.load(std::memory_order_relaxed));
}

std::optional<std::uint16_t> transport_can::alloc_base(std::size_t slice_count) {
    // Everything here stays in std::size_t. The u16 this used to narrow to was the
    // silent half of #910: a >65535-slice group wrapped `span` (65536 became 0), the
    // reservation then trivially "fit", and the manifest advertised the same wrapped
    // count — a group of 65536 slices announced as a hello. The width the endpoint
    // field can actually hold is asserted below, not assumed by a cast.
    const std::size_t span = slice_count == 0 ? 1 : slice_count;
    // A group occupies CONSECUTIVE slots [base, base+span-1], so one wider than the
    // whole data-endpoint window fits at no base and no wrap rescues it. Refuse it
    // here so the caller never advertises a count it cannot deliver.
    if (span > kCanMaxGroupSlices) return std::nullopt;
    if (static_cast<std::size_t>(next_base_) + span - 1u > can::kEndpointMax) {
        next_base_ = kCanFirstDataEndpoint;  // wrap, leaving the control slot free
    }
    const std::uint16_t base = next_base_;
    next_base_ = static_cast<std::uint16_t>(static_cast<std::size_t>(base) + span);
    return base;
}

void transport_can::emit_advertise(const can::advertise_t& adv) {
    // The advertise rides the control ID as an in-order byte stream, sliced into
    // CLASSIC (≤8B, exact-length) windows so no CAN-FD DLC padding can perturb the
    // stream decoder on the far side.
    //
    // It NEVER needed a contiguous buffer (#848): the slicing below walks the stream 8
    // bytes at a time, so it walks TWO sources in order — the 18-byte STACK header, then
    // this node's own `cfg_.path` IN PLACE, with a window free to straddle the join.
    // `emit_advertise` is unconditional in `send_impl`, so the `std::vector` this replaces
    // was a per-send abort() risk under `-fno-exceptions`. Zero allocation, zero drop:
    // 18 bytes of stack and a view of a string this transport already owns for its life.
    std::array<std::byte, can::kAdvertiseHeaderSize> header{};
    if (!can::encode_advertise_header(header, adv, cfg_.path)) {
        // Over-long: emit nothing. Without the manifest no peer can bind the group,
        // so the whole send is lost — counted, not silent (#912).
        dropped_tx_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::span<const std::byte> parts[2] = {
        std::span<const std::byte>(header),
        std::as_bytes(std::span<const char>(cfg_.path.data(), cfg_.path.size()))};

    const std::uint32_t control_id =
        can::encode_can_id({cfg_.version, cfg_.node, kCanControlEndpoint});
    std::size_t part = 0;  // which source the next byte comes from
    std::size_t off = 0;   // read cursor within that source
    while (part < 2) {
        can_frame_data_t frame;
        frame.id = control_id;
        frame.fd = false;
        std::size_t n = 0;
        // Fill one window from as many sources as it takes — the header/path join is not
        // a frame boundary, so the wire is byte-for-byte what a contiguous encode gives.
        while (n < tr::view::kCanClassicMaxData && part < 2) {
            if (off == parts[part].size()) {
                ++part;
                off = 0;
                continue;
            }
            const std::size_t take =
                std::min<std::size_t>(tr::view::kCanClassicMaxData - n, parts[part].size() - off);
            std::memcpy(frame.data.data() + n, parts[part].data() + off, take);
            n += take;
            off += take;
        }
        if (n == 0) break;  // the stream ended exactly on a window boundary
        frame.len = static_cast<std::uint8_t>(n);
        link_->write_raw(frame);
    }
}

void transport_can::emit_hello() {
    // The presence form (ADR-0044): slice_count == 0 binds nothing and precedes no
    // data — it only says "this node is on the bus" and carries its identity path.
    can::advertise_t hello;
    hello.can_id = can::encode_can_id({cfg_.version, cfg_.node, kCanControlEndpoint});
    hello.group = false;
    hello.group_total_len = 0;
    hello.slice_count = 0;
    // `hello.path` stays empty on purpose: emit_advertise announces cfg_.path in place, so
    // filling it here would buy nothing but a per-hello std::string allocation.
    const std::lock_guard lock(tx_m_);
    emit_advertise(hello);
}

void transport_can::send(std::span<const std::byte> frame) {
    send_impl(frame, can::kCanBroadcastNode);
}

void transport_can::send_impl(std::span<const std::byte> frame, std::uint16_t target) {
    if (frame.empty()) return;

    const std::lock_guard lock(tx_m_);

    // Own the bytes so view_can_frames_t can carve zero-copy subviews out of them.
    const auto payload = tr::view::over_bytes(frame);
    if (!payload) {
        dropped_tx_.fetch_add(1, std::memory_order_relaxed);  // alloc failure => backpressure drop
        return;
    }
    const tr::view::view_can_frames_t frames =
        tr::view::view_can_frames_t::split(*payload, cfg_.mode);
    const std::size_t count = frames.frame_count();
    if (count == 0) {
        dropped_tx_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // RESERVE FIRST, then advertise (#910). The manifest is a promise of `slice_count`
    // slices, and it used to be emitted before the loop that discovers the endpoint
    // window is too small — so a group over the window advertised N and sent N-1, and
    // every receiver on the bus pinned a reassembly group that could never complete.
    // Reserving here makes the promise checkable before it is made. The alternative
    // shape — advertise, then retract — was declined: a retraction is a SECOND wire
    // concern (a new control-frame semantic every peer must implement, and one that
    // is itself lossy on the medium that lost the tail slices), where the capacity is
    // a purely local fact the sender already holds.
    const std::optional<std::uint16_t> base_ep = alloc_base(count);
    if (!base_ep) {
        dropped_tx_.fetch_add(1, std::memory_order_relaxed);  // whole group refused, nothing said
        return;
    }
    const can::can_id_fields_t base_fields{cfg_.version, cfg_.node, *base_ep};
    const std::uint32_t base_id = can::encode_can_id(base_fields);

    // The manifest carries the exact total length (so the peer trims FD padding),
    // the slice count (totality opt-in → trailing-drop detection), and — for a
    // per-peer directed send (ADR-0044) — the target node id.
    can::advertise_t adv;
    adv.can_id = base_id;
    adv.group = count > 1;
    adv.group_total_len = static_cast<std::uint32_t>(frame.size());
    adv.slice_count = static_cast<std::uint16_t>(count);
    adv.target = target;
    // `adv.path` stays empty: emit_advertise walks cfg_.path in place. Copying it here
    // would put a std::string allocation back on EVERY send — the very thing #848 removes.
    emit_advertise(adv);

    const bool fd = cfg_.mode == tr::view::can_frame_mode_t::FD;
    for (std::size_t i = 0; i < count; ++i) {
        const tr::view::view_t& window = frames.frames()[i];
        const std::span<const std::byte> wb = window.bytes();
        const auto slice_id = can::slice_can_id(base_fields, i);
        if (!slice_id) {
            // Unreachable by construction: alloc_base reserved [base, base+count) inside
            // the endpoint window before the manifest went out, so every index here is in
            // range. Kept as a hard, COUNTED stop rather than the silent `break` it was —
            // if the reservation invariant is ever broken the group is reported lost, not
            // half-delivered against a manifest that promised all of it.
            dropped_tx_.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        can_frame_data_t out;
        out.id = *slice_id;
        out.fd = fd;
        const std::size_t logical = wb.size();
        const std::size_t on_wire =
            fd ? tr::view::can_fd_dlc_round_up(logical) : logical;  // DLC pad (FD only)
        out.len = static_cast<std::uint8_t>(on_wire);
        std::memcpy(out.data.data(), wb.data(), logical);
        // Pad bytes already zero (data{} is value-initialized); the peer trims to
        // group_total_len so the padding never reaches the delivered frame.
        link_->write_raw(out);
    }
}

void transport_can::on_rx(const can_frame_data_t& frame) {
    const auto fields = can::decode_can_id(frame.id);
    if (!fields) return;
    // Discovery-layer versioning (ADR-0030): a distinct version prefix is a
    // disjoint protocol band — frames outside ours are not ours to interpret.
    if (fields->version != cfg_.version) return;
    // Self-echo guard (e.g. CAN_RAW_RECV_OWN_MSGS, or a second local socket):
    // our own frames neither feed the map nor make us our own peer.
    if (fields->node == cfg_.node) return;
    // One clock read per inbound frame, shared by the ADR-0044 last-heard table and
    // the RX age-out below — the RX path's only notion of "now".
    const auto now = std::chrono::steady_clock::now();
    // ANY valid same-version frame from another node is a liveness signal —
    // the last-heard table (ADR-0044) is refreshed before the payload is parsed.
    touch_peer(fields->node, now);

    const std::lock_guard lock(rx_m_);
    rx_now_ = now;
    reasm_.set_now(stamp_ms(now));
    // Age out parked slices on EVERY inbound frame, not just on a park: a bus that
    // stops emitting unmatched data must still shed what it already parked.
    expire_pending();
    if (fields->endpoint == kCanControlEndpoint) {
        // Accumulate the per-node advertise byte stream and pop every complete frame.
        std::vector<std::byte>& buf = control_[fields->node];
        const std::span<const std::byte> in = frame.bytes();
        buf.insert(buf.end(), in.begin(), in.end());
        while (!buf.empty()) {
            const auto decoded = can::decode_advertise(buf);
            if (decoded) {
                learn_advertise(decoded->first);
                buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(decoded->second));
                continue;
            }
            // Not decodable from the front. A plausible prefix just needs more
            // bytes; anything else is a fragment (a mid-stream join saw the tail
            // of an in-flight advertise, or a lost control frame tore one) —
            // resynchronize by dropping bytes up to the next plausible boundary,
            // or the stream wedges permanently on the garbage prefix.
            if (can::advertise_prefix_plausible(buf)) break;
            std::size_t skip = 1;
            while (skip < buf.size() &&
                   std::to_integer<std::uint8_t>(buf[skip]) != can::kAdvertiseMagic) {
                ++skip;
            }
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(skip));
        }
        return;
    }
    process_data(frame);
}

void transport_can::learn_advertise(const can::advertise_t& adv) {
    // The on-advertise stale-group sweep (#912). Every group is created by an
    // advertise, so this is the cadence that sees every group. `erase` is reached
    // only after `is_complete`, so a group a lost advertise or a lost data slice
    // left permanently incomplete is pinned until something ages it out — this.
    reasm_.sweep_stale(static_cast<std::uint64_t>(cfg_.rx_ttl.count()));

    // The hello/presence form (slice_count == 0) binds nothing — its liveness
    // effect already landed in touch_peer on the frame that carried it.
    if (adv.slice_count == 0) return;
    // A directed group addressed to another node (ADR-0044) is learned so its
    // data slices are recognized and CONSUMED, but never reassembled/delivered.
    const bool deliver = adv.target == can::kCanBroadcastNode || adv.target == cfg_.node;
    learned_[adv.can_id] = binding_t{adv, deliver};
    const auto base = can::decode_can_id(adv.can_id);
    if (!base) return;
    if (deliver) reasm_.set_expected_count(key_of(base->node, base->endpoint), adv.slice_count);

    // A data frame may have arrived ahead of its manifest (cross-ID arbitration);
    // re-drive any now-matchable pending slices. Compact in place and lift only the
    // matches into one batch drawn from the SAME injected resource — the old
    // two-vector rebuild reallocated the whole queue from the global heap on every
    // advertise, and a pmr container cannot be move-assigned across resources.
    std::pmr::vector<pending_slice_t> ready(pending_.get_allocator());
    auto keep = pending_.begin();
    for (auto& p : pending_) {
        const auto ff = can::decode_can_id(p.frame.id);
        if (ff && ff->node == base->node && ff->endpoint >= base->endpoint &&
            ff->endpoint < base->endpoint + adv.slice_count) {
            ready.push_back(std::move(p));
        } else {
            *keep++ = std::move(p);
        }
    }
    pending_.erase(keep, pending_.end());
    for (const auto& p : ready) process_data(p.frame);
}

void transport_can::process_data(const can_frame_data_t& frame) {
    const auto fields = can::decode_can_id(frame.id);
    if (!fields) return;

    // Find the binding whose [base, base+slice_count) endpoint range owns this id.
    const binding_t* binding = nullptr;
    std::uint16_t base_ep = 0;
    for (const auto& [base_id, b] : learned_) {
        const auto base = can::decode_can_id(base_id);
        if (!base || base->node != fields->node) continue;
        if (fields->endpoint >= base->endpoint &&
            fields->endpoint < base->endpoint + b.adv.slice_count) {
            binding = &b;
            base_ep = base->endpoint;
            break;
        }
    }
    if (!binding) {
        park_pending(frame);  // hold until its advertise lands — bounded and aged
        return;
    }
    // A directed group addressed to another node (ADR-0044): recognized, consumed,
    // dropped — no reassembly buffer is spent on a neighbour's traffic.
    if (!binding->deliver) return;

    const tr::net::reassembly_key_t key = key_of(fields->node, base_ep);
    const std::uint32_t index = static_cast<std::uint32_t>(fields->endpoint - base_ep);
    // `over_bytes` has exactly two outcomes and nullopt means ONE thing: the backend
    // refused (empty input still returns an engaged empty view). The `value_or(view_t{})`
    // this replaces converted that refusal into a fabricated engaged-EMPTY slice (#911):
    // the reassembly buffer counts entries without inspecting their length, so the
    // placeholder satisfied `is_complete`, `assemble` chained it, and the trim below
    // shortened the rope to whatever did arrive — a byte-wrong, short frame delivered
    // upstream as valid. A refusal is backpressure, so it drops like backpressure: the
    // whole group goes (the surviving slices can never be completed into anything true),
    // the slice ticks dropped_rx, and `discard` ticks dropped_groups. Never fabricate.
    //
    // A zero-length data field takes the SAME disposition, and for the same reason.
    // `over_bytes` returns an ENGAGED empty view for empty input, so a DLC-0 data frame
    // walks the success path and inserts exactly the placeholder the paragraph above
    // exists to prevent — the buffer counts it, `is_complete` fires early, and the trim
    // delivers a short frame. A conforming sender never emits one (every slice of a
    // group is a fragment of a non-empty frame, and the last is full or partial, never
    // absent), so this is the non-conforming-peer door onto the identical corruption.
    // Guarding on the frame rather than on the view's length keeps the check at the
    // seam where the wire fact lives.
    if (frame.len == 0) {
        reasm_.discard(key);
        dropped_rx_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::optional<tr::view::view_t> slice = tr::view::over_bytes(frame.bytes(), *rx_backend_);
    if (!slice) {
        reasm_.discard(key);
        dropped_rx_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    reasm_.add_slice(key, index, *std::move(slice));

    if (!reasm_.is_complete(key)) return;
    const auto rope = reasm_.assemble(key);
    const std::uint32_t total = binding->adv.group_total_len;
    reasm_.erase(key);
    // The learned binding is kept (the identity↔path map persists and self-heals by
    // overwrite when the node re-advertises); only the per-group slice buffer is freed.
    if (!rope) return;

    // Trim back to the advertised total by SHORTENING the tail link (subrope) —
    // CAN-FD DLC padding removal is never a flatten (CONTEXT.md: "trimming
    // transport padding is shortening the last link"). The trimmed rope IS the
    // delivered frame (ADR-0053 §5); only a span-tier receiver pays a flatten.
    const std::size_t n = std::min<std::size_t>(total, rope->total_length());
    deliver(fields->node, rope->subrope(0, n));
}

/**
 * @brief Park a data slice whose advertise has not landed — bounded in count, counted on drop.
 *
 * The unbounded park this replaces was reachable by any bus peer: a data frame on a
 * never-advertised endpoint was appended and, since the only drain is the
 * covered-range re-drive in @ref learn_advertise, never reclaimed (#912). The count
 * ceiling is the injected `max_pending`, never a magic number; the age ceiling is
 * @ref expire_pending.
 */
void transport_can::park_pending(const can_frame_data_t& frame) {
    // Evict the OLDEST parked slices to make room — append order is arrival order,
    // so the front is oldest. A newly arrived slice is the one most likely to have
    // its advertise still in flight, so it is the one worth keeping.
    if (cfg_.max_pending != 0 && pending_.size() >= cfg_.max_pending) {
        const std::size_t over = pending_.size() + 1 - cfg_.max_pending;
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(over));
        dropped_rx_.fetch_add(over, std::memory_order_relaxed);
    }
    pending_.push_back(pending_slice_t{frame, rx_now_});
}

/**
 * @brief Drop parked slices older than `rx_ttl` — the age-out that holds under the
 *        shipped default config, where the count caps are opt-in.
 *
 * A parked slice waits for an advertise that, on a bus that drops control frames,
 * may never come. Arrival order is insertion order, so every stale entry is a
 * contiguous prefix and the scan stops at the first live one.
 */
void transport_can::expire_pending() {
    // No `rx_ttl <= 0` early-out. The constructor normalized the window to
    // non-negative, and zero here means the same thing it means to the peer
    // enumeration and to `sweep_stale(0)`: nothing but this instant is retained.
    // Disabling the sweep on zero was how a configured `peer_ttl_ms=0` re-opened
    // unbounded RX growth under the default `max_pending=0`.
    if (pending_.empty()) return;
    const auto cutoff = rx_now_ - cfg_.rx_ttl;
    auto first_live = pending_.begin();
    while (first_live != pending_.end() && first_live->arrived < cutoff) ++first_live;
    const auto stale = static_cast<std::size_t>(first_live - pending_.begin());
    if (stale == 0) return;
    pending_.erase(pending_.begin(), first_live);
    dropped_rx_.fetch_add(stale, std::memory_order_relaxed);
}

void transport_can::deliver(std::uint16_t src_node, tr::view::rope_t frame) {
    // The sender's bus name becomes the FWD hop's inbound NAME (ADR-0044), so a
    // reply routes back to exactly that peer, directed. The slot performs the
    // tier select (ADR-0053 §5): a rope sink takes the reassembled rope as-is,
    // zero-copy; a span-only sink pays the single materialize into the heap
    // backend (its borrowed span must be contiguous — the legitimate
    // bridge-boundary copy).
    //
    // Precedence per the bus_link_t contract: the peer-named slot when any peer
    // sink is installed; otherwise the flat transport_t slot (the frame without
    // its peer name) — a single-peer consumer needs no bus facet.
    if (peer_rx_.has_any()) {
        const peer_name_buf_t name = format_peer_name(src_node);
        peer_rx_.deliver_rope(name.view(), std::move(frame));
        return;
    }
    rx_.deliver_rope(std::move(frame));
}

// --- the `can` catalog factory (ADR-0027 / ADR-0043 §5 / ADR-0044) -----------

transport_vertex_t::transport_factory_t can_transport_factory(std::pmr::memory_resource* reasm_mr,
                                                              mem::mem_backend_t* rx_backend) {
    return [reasm_mr, rx_backend](
               const conn_settings_t& /*settings*/,
               const wire::tlv_t* raw_config) -> graph::result_t<std::unique_ptr<transport_t>> {
        // Every CAN-private key is parsed HERE from the raw config TLV (the
        // ADR-0043 §5 leanness ruling): nothing CAN-shaped lands in the shared
        // conn_settings_t. The shared config_reader_t walk, CAN's own keys.
        std::string ifname;
        transport_can_config_t cfg;
        bool have_node = false;
        const config_reader_t reader(raw_config);
        if (const auto v = reader.name("ifname")) ifname = std::string(*v);
        if (const auto v = reader.name("path")) cfg.path = std::string(*v);
        if (const auto v = reader.u16("node")) {
            cfg.node = *v;
            have_node = true;
        }
        if (const auto v = reader.u8("version")) cfg.version = *v;
        if (const auto v = reader.flag("fd"))
            cfg.mode = *v ? tr::view::can_frame_mode_t::FD : tr::view::can_frame_mode_t::CLASSIC;
        if (const auto v = reader.u32("peer_ttl_ms")) cfg.peer_ttl = std::chrono::milliseconds(*v);
        // The ingress bounds (#912). Without these keys the reassembly buffer's
        // evict-oldest seam was unreachable from production config at all — the
        // buffer was default-constructed with max_groups == 0. The pmr resource
        // cannot ride a config TLV (it is a pointer, not a wire value), so it is
        // injected at factory-registration time instead.
        cfg.reasm_mr = reasm_mr != nullptr ? reasm_mr : std::pmr::new_delete_resource();
        // Same reasoning one seam over (#911): the slice-byte backend is a pointer, so
        // it rides the factory registration, not the config TLV. nullptr = process heap.
        cfg.rx_backend = rx_backend;
        if (const auto v = reader.u32("max_groups")) cfg.max_groups = static_cast<std::size_t>(*v);
        if (const auto v = reader.u32("max_pending"))
            cfg.max_pending = static_cast<std::size_t>(*v);
        if (const auto v = reader.u32("rx_ttl_ms")) cfg.rx_ttl = std::chrono::milliseconds(*v);
        if (ifname.empty() || !have_node || cfg.node > can::kNodeMax ||
            cfg.version > can::kVersionMax) {
            return std::unexpected(graph::status_t::TYPE_MISMATCH);
        }
        auto link = std::make_unique<socketcan_link_t>(ifname);
        if (!link->ok()) return std::unexpected(graph::status_t::NOT_FOUND);  // no kernel CAN
        return std::make_unique<transport_can>(std::move(link), std::move(cfg));
    };
}

}  // namespace tr::net
