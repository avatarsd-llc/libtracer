/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief The hazard domain's node class outlives static destruction, slab source included
 *        (#1782).
 *
 * The exit sweep and late `thread_local` participants return nodes after static destructors
 * have run, and a return can release a slab: once the class already keeps `kSlabClassCap` free
 * slabs, an emptied one goes straight back to the class's heap source. So that source must
 * be as immortal as the pool. This test holds four slabs' worth of nodes in an object whose
 * destructor runs AFTER the node class's translation unit has been torn down, and gives them
 * back there. With the source outside the never-destroyed storage, that release reaches a
 * destroyed `heap_source_t` (a pure virtual call, or a sanitizer report); with it inside, the
 * process exits 0.
 *
 * The ordering rests on this TU's static initialization running before the node-class TU's: a
 * static whose destructor is registered earlier runs later. The build links this TU first and
 * compiles its own copy of the node-class TU at -O0 (see `core/tests/CMakeLists.txt`), where
 * the old layout fails as `pure virtual method called`.
 */

#include <cstdio>
#include <cstdlib>

#include "libtracer/lkv_slot.hpp"

namespace {

namespace hp = tr::graph::detail_hp;

/** @brief Nodes to hold: four 4 KiB slabs of 252, so at least two slabs are released. */
constexpr std::size_t kHold = 4 * 252;

/** @brief Holds nodes from `main` until after static destruction of the library. */
struct late_holder_t {
    hp::lists_t list{}; /**< @brief The held nodes, as a participant's free list. */
    /** @brief Return every held node to the node class, after the library's statics died. */
    ~late_holder_t() {
        hp::spill_nodes(list, list.freelist_n);
        std::printf("returned the held nodes after static destruction: ok\n");
        std::fflush(stdout);
    }
};

/** @brief Its destructor is registered before the node class's, so it runs after it. */
late_holder_t g_late;

}  // namespace

int main() {
    while (g_late.list.freelist_n < kHold) {
        if (hp::refill_nodes(g_late.list) == 0) {
            std::printf("FAIL: the node class refused a slab\n");
            return 1;
        }
    }
    std::printf("holding %zu nodes until exit\n", g_late.list.freelist_n);
    return 0;
}
