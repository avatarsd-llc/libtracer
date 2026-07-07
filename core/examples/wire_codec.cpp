/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief Wire codec deep-dive — build a structured frame, inspect its bytes, and
 *        measure encode / decode / round-trip throughput.
 *
 * Where `wire_roundtrip` proves byte-identity, this example is the performance and
 * anatomy companion (`docs/modules/frame-codec.md`, `docs/modules/wire-format-bits.md`).
 * It builds a POINT TLV carrying two VALUE children with a CRC trailer, prints the
 * encoded size and the raw header bytes, then times three things over many
 * iterations: `encode` (model → bytes), `decode` (bytes → borrowed tree), and the
 * full round-trip. It also confirms the decode is zero-copy (payload spans borrow
 * the encoded buffer) and that re-encoding is byte-identical.
 *
 * RESULT perf lines are informational (CI never flakes on timing); the self-checks
 * guard correctness. Runs under ctest as `example_wire_codec`.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "libtracer/tracer.hpp"

namespace {

using clock_t_ = std::chrono::steady_clock;
using tr::wire::opt_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

/** @brief A VALUE TLV borrowing @p bytes (little-endian payload; must outlive the TLV). */
tlv_t value_tlv(std::span<const std::byte> bytes) {
    tlv_t t;
    t.type = type_t::VALUE;
    t.payload = bytes;
    return t;
}

/** @brief Record a failed expectation on @p ok and report it. */
void check(bool& ok, bool cond, const char* what) {
    if (!cond) {
        std::printf("  [FAIL] %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    // Two 4-byte little-endian VALUE payloads; their bytes outlive every TLV below.
    const std::vector<std::byte> a = {std::byte{0x17}, std::byte{0x00}, std::byte{0x00},
                                      std::byte{0x00}};
    const std::vector<std::byte> b = {std::byte{0x2A}, std::byte{0x00}, std::byte{0x00},
                                      std::byte{0x00}};

    // A structured POINT (opt.pl = payload-is-children) with a CRC-32C trailer.
    tlv_t point;
    point.type = type_t::POINT;
    point.opt = opt_t{.pl = true, .cr = true};
    point.children = {value_tlv(a), value_tlv(b)};

    const std::vector<std::byte> wire = tr::wire::encode(point);
    std::printf("encoded POINT{VALUE,VALUE}+CRC: %zu bytes\n", wire.size());
    std::printf("  header bytes:");
    for (std::size_t i = 0; i < wire.size() && i < 6; ++i)
        std::printf(" %02X", static_cast<unsigned>(std::to_integer<std::uint8_t>(wire[i])));
    std::printf("  (type, opt, length, …)\n");

    bool ok = true;
    auto decoded = tr::wire::decode(std::span<const std::byte>(wire));
    check(ok, decoded.has_value(), "decode succeeds (CRC verifies)");
    if (decoded) {
        check(ok, decoded->type == type_t::POINT, "root is a POINT");
        check(ok, decoded->children.size() == 2, "POINT has two VALUE children");
        check(ok, decoded->trailer && decoded->trailer->crc.has_value(),
              "CRC trailer present and verified");
        if (decoded->children.size() == 2) {
            const auto c0 = decoded->children[0].payload;
            check(ok, c0.data() >= wire.data() && c0.data() < wire.data() + wire.size(),
                  "decoded payload borrows the encoded buffer (zero copy)");
        }
        check(ok, tr::wire::encode(*decoded) == wire, "encode(decode(bytes)) == bytes");
    }

    // --- perf: encode / decode / round-trip over the same frame ---
    constexpr int kIters = 50000;
    std::size_t sink = 0;

    auto t0 = clock_t_::now();
    for (int i = 0; i < kIters; ++i) sink += tr::wire::encode(point).size();
    auto t1 = clock_t_::now();
    for (int i = 0; i < kIters; ++i) {
        auto d = tr::wire::decode(std::span<const std::byte>(wire));
        sink += d ? d->children.size() : 0;
    }
    auto t2 = clock_t_::now();

    const double enc_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters;
    const double dec_ns = std::chrono::duration<double, std::nano>(t2 - t1).count() / kIters;
    const double rt_Mps = 1.0 / ((enc_ns + dec_ns) * 1e-9) / 1e6;
    std::printf("RESULT wire_codec bytes=%zu encode_ns=%.0f decode_ns=%.0f "
                "roundtrip_Mps=%.2f (sink=%zu)\n",
                wire.size(), enc_ns, dec_ns, rt_Mps, sink);

    std::printf("%s\n", ok ? "wire codec OK" : "wire codec FAILED");
    return ok ? 0 : 1;
}
