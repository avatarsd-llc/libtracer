/**
 * @file
 * @brief Test-only access to `transport_vertex_t`'s unbracketed owned-link lookup.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The app reaches an owned link through `transport_vertex_t::with_link`, inside a frame
 * bracket. Tests that only need the pointer, on links nothing removes under them, use this.
 */
#ifndef LIBTRACER_TESTS_TRANSPORT_VERTEX_TEST_ACCESS_HPP
#define LIBTRACER_TESTS_TRANSPORT_VERTEX_TEST_ACCESS_HPP

#include <string_view>

#include "libtracer/transport_vertex.hpp"

namespace tr::net {

/** @brief The friend `transport_vertex_t` names for its test-only accessor. */
struct transport_vertex_test_access {
    /** @brief `transport_vertex_t::link_of`: the owned link of @p name, or nullptr. */
    static transport_t* link_of(const transport_vertex_t& net, std::string_view name) {
        return net.link_of(name);
    }
};

}  // namespace tr::net

namespace tr::testing {

/** @brief The owned link of connection @p name, unbracketed: for tests only. */
inline net::transport_t* link_of(const net::transport_vertex_t& net, std::string_view name) {
    return net::transport_vertex_test_access::link_of(net, name);
}

}  // namespace tr::testing

#endif  // LIBTRACER_TESTS_TRANSPORT_VERTEX_TEST_ACCESS_HPP
