/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/loopback.hpp"

#include <algorithm>
#include <new>

namespace tr::net {

namespace {

constexpr std::size_t kFrameAlign = alignof(std::max_align_t);

}  // namespace

void loopback_endpoint_t::release(frame_t* f) noexcept {
    src_.release(f, sizeof(frame_t) + f->len, kFrameAlign);
}

void loopback_endpoint_t::send(std::span<const std::byte> frame) {
    const std::span<const std::byte> one[1] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void loopback_endpoint_t::send(std::span<const std::span<const std::byte>> iov) {
    if (peer_ == nullptr) return;
    std::size_t total = 0;
    for (const std::span<const std::byte>& s : iov) total += s.size();
    // The block comes from the RECEIVING endpoint's source, which releases it after delivery.
    // The spans are gathered straight into it (#2679): the base gather's block from this
    // link's egress store, and the second copy out of it, are gone.
    mem::block_source_t& src = peer_->src_;
    void* const p = src.try_alloc(sizeof(frame_t) + total, kFrameAlign);
    if (p == nullptr) {
        drops_.count_dropped_tx();
        return;
    }
    auto* const f = ::new (p) frame_t{nullptr, total};
    std::byte* out = reinterpret_cast<std::byte*>(f + 1);
    for (const std::span<const std::byte>& s : iov) out = std::copy(s.begin(), s.end(), out);
    if (!peer_->enqueue(f)) {
        peer_->release(f);
        drops_.count_dropped_tx();
    }
}

bool loopback_endpoint_t::enqueue(frame_t* f) {
    {
        const std::lock_guard lock(m_);
        if (!running_) return false;
        if (tail_ != nullptr) {
            tail_->next = f;
        } else {
            head_ = f;
        }
        tail_ = f;
    }
    cv_.notify_one();
    return true;
}

void loopback_endpoint_t::entry(void* self) { static_cast<loopback_endpoint_t*>(self)->run(); }

void loopback_endpoint_t::start() {
    const std::lock_guard lock(m_);
    running_ = thread_.start(&loopback_endpoint_t::entry, this, 0, "tr_loop", src_);
}

void loopback_endpoint_t::stop() {
    {
        const std::lock_guard lock(m_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
    // The thread drained the inbox before it returned, and once stop_ is set nothing new is
    // queued: running_ goes false with it.
}

void loopback_endpoint_t::run() {
    for (;;) {
        frame_t* f = nullptr;
        {
            std::unique_lock lock(m_);
            cv_.wait(lock, [this] { return stop_ || head_ != nullptr; });
            if (head_ == nullptr) {  // woken to stop with nothing left to drain
                running_ = false;
                return;
            }
            f = head_;
            head_ = f->next;
            if (head_ == nullptr) tail_ = nullptr;
        }
        // The slot snapshots its {fn, ctx} pair under its own lock and dispatches
        // outside it (TSan-safe vs a concurrent set_receiver).
        rx_.deliver_borrowed(
            std::span<const std::byte>(reinterpret_cast<std::byte*>(f + 1), f->len));
        release(f);
    }
}

loopback_channel_t::loopback_channel_t(mem::block_source_t& src) : a_(src), b_(src) {
    a_.peer_ = &b_;
    b_.peer_ = &a_;
    a_.start();
    b_.start();
}

loopback_channel_t::~loopback_channel_t() { shutdown(); }

void loopback_channel_t::shutdown() {
    a_.stop();
    b_.stop();
}

}  // namespace tr::net
