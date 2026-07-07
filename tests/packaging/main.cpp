/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Minimal downstream consumer of an INSTALLED libtracer (see the sibling
 * CMakeLists.txt and the core-ci `install-consume` job). Includes a public
 * header and calls a symbol defined in the compiled library (path.cpp), so the
 * build fails if headers aren't installed and the link fails if the exported
 * archive isn't found. The C++ standard comes from libtracer's exported
 * `cxx_std_23` usage requirement — this fixture sets none itself.
 */
#include <cstdio>
#include <libtracer/path.hpp>

int main() {
    const tr::graph::path_t p{"/sensor/temp"};  // compiled symbol: path.cpp
    if (p.segment_count() != 2) {
        std::fprintf(stderr, "unexpected segment_count %zu\n", p.segment_count());
        return 1;
    }
    std::printf("find_package(libtracer) consume OK\n");
    return 0;
}
