/**
 * @file
 * @brief Test-only: append a PAIR element to a `std::vector` body.
 *
 * The installed encoder writes into a core byte array (#1781: no owning std type crosses the
 * public API); the suites build their frame bodies as vectors, so they spell PAIRs here, over
 * the same allocation-free @ref tr::wire::path_pair_store. A retired `0x14 PATH_REF` is spelled
 * here too: the library no longer emits one, and the suites build it only to see it refused.
 */
#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "libtracer/path_pair.hpp"
#include "libtracer/path_ref.hpp"
#include "libtracer/tlv_emit.hpp"

namespace tr::testing {

/** @brief Append the 11-byte PAIR record for @p pair to @p out. */
inline void emit_path_pair(std::vector<std::byte>& out, const tr::wire::path_pair_t& pair) {
    std::array<std::byte, tr::wire::kPathPairRecordBytes> rec{};
    tr::wire::path_pair_store(rec, pair);
    out.insert(out.end(), rec.begin(), rec.end());
}

/**
 * @brief Append a retired `0x14 PATH_REF` TLV over @p elements to @p out: the 4-byte envelope
 *        (`opt = 0`) and the bare 8-byte element array, RFC-0024 §4.
 */
inline void emit_retired_path_ref(std::vector<std::byte>& out,
                                  std::span<const tr::wire::path_ref_element_t> elements) {
    std::vector<std::byte> body(elements.size() * tr::wire::kPathRefElementBytes);
    for (std::size_t i = 0; i < elements.size(); ++i) {
        tr::wire::pair_store_le(
            std::span<std::byte>(body).subspan(i * tr::wire::kPathRefElementBytes,
                                               tr::wire::kPathRefElementBytes),
            elements[i]);
    }
    tr::wire::emit_tlv(out, tr::wire::type_t::PATH_REF, tr::wire::opt_t{}, body);
}

}  // namespace tr::testing
