/**
 * @file
 * @brief Test-only: append a PAIR element to a `std::vector` body.
 *
 * The installed encoder writes into a core byte array (#1781: no owning std type crosses the
 * public API); the suites build their frame bodies as vectors, so they spell PAIRs here, over
 * the same allocation-free @ref tr::wire::path_pair_store.
 */
#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "libtracer/path_pair.hpp"

namespace tr::testing {

/** @brief Append the 11-byte PAIR record for @p pair to @p out. */
inline void emit_path_pair(std::vector<std::byte>& out, const tr::wire::path_pair_t& pair) {
    std::array<std::byte, tr::wire::kPathPairRecordBytes> rec{};
    tr::wire::path_pair_store(rec, pair);
    out.insert(out.end(), rec.begin(), rec.end());
}

}  // namespace tr::testing
