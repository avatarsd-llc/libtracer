/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The transport seam: one wire technology behind a uniform interface. A bridge
 * sends framed bytes (a complete TLV, ROUTER-wrapped) via send(), and receives
 * inbound frames through a registered receiver callback (which may fire on an
 * internal transport thread). This C++ seam is callback + recv-thread
 * (docs/reference/10 §"Transport ↔ L4: tr::Transport"), an implementation choice
 * (ADR-0013) that matches how a real socket transport's receive loop would feed a
 * bridge. A transport never sees TLV semantics — only framed bytes.
 */
#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace tr::net {

// A 16-byte node/peer identity — the ROUTER `origin_peer_id` (docs/reference/05
// §0x0D ROUTER).
using peer_id_t = std::array<std::byte, 16>;

class transport_t {
   public:
    using receiver_t = std::function<void(std::span<const std::byte>)>;

    virtual ~transport_t() = default;

    // Emit one frame (a complete TLV's bytes) onto the wire.
    virtual void send(std::span<const std::byte> frame) = 0;

    // Scatter-gather send: emit the gathered spans as ONE frame, without a flatten
    // copy — hand a rope's `to_iovec()` straight to the wire (the "rope we put into
    // tx"). The default gathers into a temporary and calls send(); transports with
    // native scatter-gather (sendmsg/writev/RDMA SGE) override this to avoid the copy.
    virtual void send(std::span<const std::span<const std::byte>> iov) {
        std::size_t total = 0;
        for (const auto& s : iov) total += s.size();
        std::vector<std::byte> tmp;
        tmp.reserve(total);
        for (const auto& s : iov) tmp.insert(tmp.end(), s.begin(), s.end());
        send(std::span<const std::byte>(tmp));
    }

    // Register the sink for inbound frames (the bridge's ingest). Must be set
    // before frames flow; delivery may occur on an internal transport thread.
    virtual void set_receiver(receiver_t receiver) = 0;
};

}  // namespace tr::net
