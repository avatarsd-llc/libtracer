/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * socketcan_link_t stub — the non-Linux implementation of the SocketCAN link:
 * every operation is a no-op and ok() stays false, so a program referencing
 * socketcan_link_t still links everywhere while a platform port (e.g. the
 * ESP-IDF component's twai_link_t) supplies the real can_link_t. Selected by
 * the build system on non-Linux platforms (no in-source #ifdefs — platform
 * selection is a build-system concern); Linux compiles src/socketcan_link.cpp.
 */

#include <utility>

#include "libtracer/transport_can.hpp"

namespace tr::net {

socketcan_link_t::socketcan_link_t(const std::string&) {}

socketcan_link_t::~socketcan_link_t() = default;

void socketcan_link_t::on_receive(rx_fn_t rx) {
    const std::lock_guard lock(m_);
    rx_ = std::move(rx);
}

void socketcan_link_t::write_raw(const can_frame_data_t&) {}

void socketcan_link_t::run() {}

}  // namespace tr::net
