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

#include "libtracer/view.hpp"

namespace tr::net {

// A 16-byte node/peer identity — the ROUTER `origin_peer_id` (docs/reference/05
// §0x0D ROUTER).
using peer_id_t = std::array<std::byte, 16>;

class transport_t {
   public:
    using receiver_t = std::function<void(std::span<const std::byte>)>;
    /** @brief The OWNING inbound sink (ADR-0042): each frame is a `view_t` over a
     *         refcounted segment the receiver may keep, subview, or rope onward. */
    using view_receiver_t = std::function<void(view::view_t)>;

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

    /**
     * @brief Register the optional OWNING inbound sink (the ADR-0042 receiver seam).
     *
     * A transport that can hand up owning frames (its @ref delivers_views returns
     * true) delivers each inbound frame to @p receiver as a `view::view_t` over a
     * refcounted segment drawn from a host-injected `mem_backend_t` — the receiver
     * may pin, subview, or rope the frame beyond the callback (unlike the borrowed
     * span of @ref set_receiver, which dies when the callback returns). Must be set
     * before frames flow; delivery may occur on an internal transport thread.
     *
     * The base implementation is a documented no-op: a span-only transport ignores
     * an installed view receiver, honestly — there is NO adapter that wraps a
     * borrowed span into a `view_t` whose refcount would lie about lifetime
     * (ADR-0042 §1). A transport that honors this seam MUST also override
     * @ref delivers_views to return true, so `fwd_router_t::add_child` installs
     * the receiver matching the link's capability.
     */
    virtual void set_view_receiver(view_receiver_t receiver) { (void)receiver; }

    /**
     * @brief The owning-delivery capability (ADR-0042 §1): true iff this transport
     *        honors @ref set_view_receiver by delivering refcounted `view_t` frames.
     */
    [[nodiscard]] virtual bool delivers_views() const { return false; }
};

}  // namespace tr::net
