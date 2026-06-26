// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

#include "libtracer/loopback.hpp"

#include <utility>

namespace tr {

void loopback_endpoint_t::set_receiver(receiver_t receiver) {
    const std::lock_guard lock(m_);
    receiver_ = std::move(receiver);
}

void loopback_endpoint_t::send(std::span<const std::byte> frame) {
    if (peer_) peer_->enqueue(std::vector<std::byte>(frame.begin(), frame.end()));
}

void loopback_endpoint_t::enqueue(std::vector<std::byte> frame) {
    {
        const std::lock_guard lock(m_);
        inbox_.push_back(std::move(frame));
    }
    cv_.notify_one();
}

void loopback_endpoint_t::start() {
    thread_ = std::thread([this] { run(); });
}

void loopback_endpoint_t::stop() {
    {
        const std::lock_guard lock(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void loopback_endpoint_t::run() {
    for (;;) {
        std::vector<std::byte> frame;
        receiver_t receiver;
        {
            std::unique_lock lock(m_);
            cv_.wait(lock, [this] { return stop_ || !inbox_.empty(); });
            if (inbox_.empty()) return;  // woken to stop with nothing left to drain
            frame = std::move(inbox_.front());
            inbox_.pop_front();
            receiver = receiver_;  // copy under the lock (TSan-safe vs set_receiver)
        }
        if (receiver) receiver(frame);
    }
}

loopback_channel_t::loopback_channel_t() {
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

}  // namespace tr
