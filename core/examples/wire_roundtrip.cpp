/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief L2/L3 wire codec round-trip — build a TLV, encode to bytes, decode back.
 *
 * The wire codec is the one place bytes become a `tlv_t` tree and back
 * (`docs/modules/frame-codec.md`). This example builds a structured PATH TLV
 * (`/sensor/temp` — two NAME children) with a CRC trailer, `encode`s it to wire
 * bytes, `decode`s those bytes into a fresh tree, and checks that re-encoding
 * reproduces the exact wire bytes.
 * It also shows the zero-copy nature of decode: the decoded payloads are
 * `std::span`s that BORROW the encoded buffer, so no payload bytes are copied.
 *
 * Runs under ctest as `example_wire_roundtrip`: it checks structure, byte-identity,
 * and the verified CRC trailer, returning non-zero on any mismatch.
 */

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "libtracer/tracer.hpp"

namespace {

using tr::wire::opt_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

/** @brief A byte span over the characters of @p s (no copy; @p s must outlive the span). */
std::span<const std::byte> bytes_of(const std::string& s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

/** @brief A NAME TLV borrowing @p name's bytes. */
tlv_t name_tlv(const std::string& name) {
    tlv_t t;
    t.type = type_t::NAME;
    t.payload = bytes_of(name);
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
    // The NAME segment bytes must outlive every TLV that borrows them.
    const std::string seg0 = "sensor";
    const std::string seg1 = "temp";

    // Build a PACKED PATH TLV (RFC-0018 — `opt.PL = 0`, the body is a run of
    // `[u8 len][bytes]` segment records) with a CRC trailer (opt.cr — encode recomputes
    // the CRC-32C over the body).
    std::vector<std::byte> packed;
    (void)tr::wire::emit_path_segment(packed, seg0);
    (void)tr::wire::emit_path_segment(packed, seg1);
    tlv_t path;
    path.type = type_t::PATH;
    path.opt = opt_t{.cr = true};
    path.payload = std::span<const std::byte>(packed);

    // Encode the model to wire bytes, then decode those bytes back into a tree.
    const std::vector<std::byte> wire = tr::wire::encode(path);
    std::printf("encoded /sensor/temp PATH TLV: %zu bytes\n", wire.size());

    const std::expected<tlv_t, tr::wire::err_t> decoded =
        tr::wire::decode(std::span<const std::byte>(wire));

    bool ok = true;
    check(ok, decoded.has_value(), "decode succeeds (CRC trailer verifies)");
    if (decoded) {
        std::printf("decoded: type=0x%02X, %zu children, trailer.crc=%s\n",
                    static_cast<unsigned>(decoded->type), decoded->children.size(),
                    (decoded->trailer && decoded->trailer->crc) ? "present" : "absent");
        check(ok, decoded->type == type_t::PATH, "decoded root is a PATH");
        check(ok, !decoded->opt.pl, "a packed PATH is NOT structured (opt.PL = 0, RFC-0018)");
        check(ok, decoded->children.empty(), "a packed PATH has no child TLVs");
        check(ok, decoded->payload.size() == 1 + seg0.size() + 1 + seg1.size(),
              "the body is one length byte per segment plus the segment text");
        check(ok, decoded->trailer && decoded->trailer->crc.has_value(),
              "decoded PATH carries the verified CRC trailer");
        // The decoded payload borrows the encoded buffer — zero copy.
        {
            const auto body = decoded->payload;
            check(ok, body.data() >= wire.data() && body.data() < wire.data() + wire.size(),
                  "the packed body is a span INTO the encoded buffer (zero copy)");
            const auto want = bytes_of(seg0);
            const bool same =
                body.size() > want.size() &&
                static_cast<std::size_t>(static_cast<std::uint8_t>(body[0])) == want.size() &&
                std::equal(want.begin(), want.end(), body.begin() + 1);
            check(ok, same, "the first packed record round-trips to \"sensor\"");
        }
        // The strongest round-trip invariant: re-encoding the decoded tree
        // reproduces the exact wire bytes (byte-identical, CRC and all).
        check(ok, tr::wire::encode(*decoded) == wire, "encode(decode(bytes)) == bytes");
    }

    std::printf("%s\n", ok ? "round-trip OK" : "round-trip FAILED");
    return ok ? 0 : 1;
}
