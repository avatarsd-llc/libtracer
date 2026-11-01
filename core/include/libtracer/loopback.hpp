/**
 * @file
 * @brief An in-process loopback transport for development and tests.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * An in-process loopback transport (dev/test only — docs/reference/10's
 * transports are all real wire tech). A loopback_channel_t owns two endpoints; a
 * frame sent on one endpoint is delivered to the OTHER endpoint's receiver, on
 * that endpoint's receive thread (modeling async cross-"wire" delivery, so no
 * forwarding recurses on the sender's stack). No sockets; deterministic; the
 * vehicle for exercising FWD forward/reply routing (RFC-0004, ADR-0040) end to
 * end.
 *
 * Lifetime: call shutdown() (or destroy the channel) before the registered
 * receivers are destroyed — it joins the receive threads so no frame is
 * delivered to a dead receiver.
 *
 * Memory (#2288): each queued frame is ONE block drawn from the channel's source (an
 * intrusive FIFO; the link lives in the block), and each receive thread is a
 * @ref tr::static_thread_t spawned from the same source. Nothing comes from the platform
 * heap. A frame whose block the source refuses is dropped and counted in
 * `drop_stats().dropped_tx` of the sending endpoint.
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/static_thread.hpp"
#include "libtracer/transport.hpp"

namespace tr::net {

class loopback_channel_t;

/**
 * @brief One end of an in-process loopback link (dev/test only).
 *
 * A @ref transport_t whose @ref send hands the frame to the PEER endpoint's receiver, on
 * that peer's receive thread. Constructed and owned by a @ref loopback_channel_t.
 */
class loopback_endpoint_t final : public transport_t {
   public:
    /** @brief Send one frame — delivered to the peer endpoint's receiver on its recv thread.
     *         A frame the source refuses, or sent with no running peer, is dropped and
     *         counted in @ref drop_stats (`dropped_tx`). */
    void send(std::span<const std::byte> frame) override;

    /** @brief The frames this endpoint's @ref send dropped (`dropped_tx`); the other fields
     *         stay zero. */
    [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override {
        return {.dropped_tx = dropped_tx_.load(std::memory_order_relaxed)};
    }

   private:
    friend class loopback_channel_t;
    /** @brief One queued frame: this header, then its bytes, in one block from `src_`. */
    struct frame_t {
        frame_t* next;   /**< @brief The inbox FIFO's link. */
        std::size_t len; /**< @brief The frame's byte count. */
    };

    explicit loopback_endpoint_t(mem::block_source_t& src) noexcept : src_(src) {}

    void start();
    void stop();                    // idempotent: join the recv thread
    bool enqueue(frame_t* f);       // called by the peer's send(); false = not running
    void run();                     // receive thread
    static void entry(void* self);  // the static_thread_t trampoline into run()
    void release(frame_t* f) noexcept;

    mem::block_source_t& src_;
    loopback_endpoint_t* peer_ = nullptr;
    frame_t* head_ = nullptr;  // the inbox FIFO, guarded by m_
    frame_t* tail_ = nullptr;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool running_ = false;  // the receive thread started and is not yet stopped (guarded by m_)
    std::atomic<std::uint64_t> dropped_tx_{0};
    static_thread_t thread_;
};

/**
 * @brief An in-process loopback channel: two endpoints, each delivering to the other.
 *
 * A frame sent on @ref a is delivered to @ref b's receiver (and vice-versa), on that
 * endpoint's receive thread — modeling async cross-"wire" delivery so forwarding never
 * recurses on the sender's stack. No sockets; deterministic. The vehicle for exercising FWD
 * forward/reply routing end to end (RFC-0004, ADR-0040). Non-copyable. Call @ref shutdown
 * (or destroy the channel) before the registered receivers are destroyed.
 */
class loopback_channel_t {
   public:
    /**
     * @brief Construct a channel and start both endpoints' receive threads.
     *
     * @param src Where every queued frame and both receive threads come from; it MUST
     *            outlive the channel. The default is the build's net sub-pool.
     */
    explicit loopback_channel_t(mem::block_source_t& src = mem::net_source());
    /** @brief Destroy the channel, joining both receive threads first. */
    ~loopback_channel_t();
    loopback_channel_t(const loopback_channel_t&) = delete;
    loopback_channel_t& operator=(const loopback_channel_t&) = delete;

    /** @brief The first endpoint; a frame sent here is delivered to @ref b. */
    [[nodiscard]] loopback_endpoint_t& a() noexcept { return a_; }
    /** @brief The second endpoint; a frame sent here is delivered to @ref a. */
    [[nodiscard]] loopback_endpoint_t& b() noexcept { return b_; }

    /** @brief Join both receive threads so no frame reaches a dead receiver (idempotent). */
    void shutdown();

   private:
    loopback_endpoint_t a_;
    loopback_endpoint_t b_;
};

}  // namespace tr::net
