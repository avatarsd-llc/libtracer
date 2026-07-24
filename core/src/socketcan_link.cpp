/**
 * @file
 * @brief socketcan_link_t — the Linux `PF_CAN`/`SOCK_RAW` implementation of the `can_link_t` seam.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This TU is Linux-ONLY and is selected by the build system
 * (core/CMakeLists.txt compiles it when CMAKE_SYSTEM_NAME is Linux; every other
 * platform compiles src/socketcan_link_stub.cpp instead). No in-source #ifdefs:
 * platform selection is a build-system concern, per the no-feature-macro ruling.
 * Platform ports (e.g. the ESP-IDF component's twai_link_t) implement the same
 * can_link_t seam in their own TU, next to their platform headers.
 */

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>
#include <utility>

#include "libtracer/transport_can.hpp"

namespace tr::net {

void* socketcan_link_t::thread_entry(void* self) {
    static_cast<socketcan_link_t*>(self)->run();
    return nullptr;
}

socketcan_link_t::socketcan_link_t(const std::string& ifname, std::size_t recv_stack) {
    fd_ = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd_ < 0) return;

    // Best-effort CAN-FD: a classic-only controller leaves this off and still works.
    const int enable_fd = 1;
    ::setsockopt(fd_, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable_fd, sizeof(enable_fd));

    ifreq ifr{};
    std::strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
    if (::ioctl(fd_, SIOCGIFINDEX, &ifr) < 0) {
        ::close(fd_);
        fd_ = -1;
        return;
    }

    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd_);
        fd_ = -1;
        return;
    }

    // A receive timeout lets the recv loop poll stop_ for a clean shutdown.
    timeval tv{.tv_sec = 0, .tv_usec = 100000};  // 100 ms
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    pthread_attr_t attr;
    ::pthread_attr_init(&attr);
    // A hint below the platform floor makes setstacksize return EINVAL and leaves
    // the default stacksize in place — fall back rather than fail the spawn.
    if (recv_stack != 0) (void)::pthread_attr_setstacksize(&attr, recv_stack);
    started_ = (::pthread_create(&thread_, &attr, &socketcan_link_t::thread_entry, this) == 0);
    ::pthread_attr_destroy(&attr);
}

socketcan_link_t::~socketcan_link_t() {
    stop_.store(true, std::memory_order_relaxed);
    if (started_) {
        ::pthread_join(thread_, nullptr);
        started_ = false;
    }
    // Reset the fd under the write lock before closing so an in-flight write_raw
    // never touches a closed/reused descriptor.
    int doomed;
    {
        const std::lock_guard lock(write_m_);
        doomed = fd_;
        fd_ = -1;
    }
    if (doomed >= 0) ::close(doomed);
}

void socketcan_link_t::on_receive(rx_fn_t rx) {
    const std::lock_guard lock(m_);
    rx_ = std::move(rx);
}

void socketcan_link_t::write_raw(const can_frame_data_t& frame) {
    const std::lock_guard lock(write_m_);
    if (fd_ < 0) return;
    // A CAN_RAW write is all-or-nothing per frame: on error (or a short write,
    // which CAN_RAW never splits) the frame is dropped, best-effort — mirroring
    // the RX side's skip-and-continue policy.
    if (frame.fd) {
        canfd_frame f{};
        f.can_id = frame.id | CAN_EFF_FLAG;  // 29-bit extended id
        f.len = frame.len;
        std::memcpy(f.data, frame.data.data(), frame.len);
        if (::write(fd_, &f, sizeof(f)) != static_cast<ssize_t>(sizeof(f))) return;
    } else {
        can_frame f{};
        f.can_id = frame.id | CAN_EFF_FLAG;
        f.can_dlc = frame.len;
        std::memcpy(f.data, frame.data.data(), frame.len);
        if (::write(fd_, &f, sizeof(f)) != static_cast<ssize_t>(sizeof(f))) return;
    }
}

void socketcan_link_t::run() {
    while (!stop_.load(std::memory_order_relaxed)) {
        canfd_frame f{};
        const ssize_t n = ::read(fd_, &f, sizeof(f));
        if (n < 0) continue;  // timeout / EAGAIN → re-check stop_
        if (n != static_cast<ssize_t>(sizeof(can_frame)) &&
            n != static_cast<ssize_t>(sizeof(canfd_frame)))
            continue;

        can_frame_data_t out;
        out.id = f.can_id & CAN_EFF_MASK;  // strip EFF/RTR/ERR flags → 29-bit id
        out.fd = (n == static_cast<ssize_t>(sizeof(canfd_frame)));
        out.len = f.len;
        if (out.len > 64) out.len = 64;
        std::memcpy(out.data.data(), f.data, out.len);

        rx_fn_t rx;
        {
            const std::lock_guard lock(m_);
            rx = rx_;
        }
        if (rx) rx(out);
    }
}

}  // namespace tr::net
