/**
 * @file
 * @brief Test-only access to `graph_t`'s flat `:children` serialize, the oracle the folded
 *        listing is diffed against (#2437).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Production serves `:children` folded (`graph_t::read_children_folded`). The flat form is
 * kept only so a differential test has an independent answer to compare against; without it
 * the comparison would be tautological. It is not public API, so the tests reach it here.
 */
#ifndef LIBTRACER_TESTS_GRAPH_CHILDREN_TEST_DOOR_HPP
#define LIBTRACER_TESTS_GRAPH_CHILDREN_TEST_DOOR_HPP

#include "libtracer/graph.hpp"

namespace tr::graph {

/** @brief The test-only door `graph.hpp` declares (#2437): the flat `:children` oracle. */
struct graph_children_test_door_t {
    /**
     * @brief MATERIALIZED `:children` listing of @p v: the flat single-link serialize of the
     *        same `POINT{ POINT{NAME}… }` the fold gathers.
     */
    static result_t<value_ref_t> read_children_materialized(const graph_t& g,
                                                            graph::vertex_handle_t v) {
        return g.read_children_materialized(v);
    }
};

}  // namespace tr::graph

namespace tr::testing {

/** @brief The flat `:children` listing of @p v, the folded listing's oracle: for tests only. */
inline graph::result_t<graph::value_ref_t> read_children_materialized(const graph::graph_t& g,
                                                                      graph::vertex_handle_t v) {
    return graph::graph_children_test_door_t::read_children_materialized(g, v);
}

}  // namespace tr::testing

#endif  // LIBTRACER_TESTS_GRAPH_CHILDREN_TEST_DOOR_HPP
