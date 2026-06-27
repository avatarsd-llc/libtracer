/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * transport_ws PROTOCOL-layer test (#54). Asserts the hand-written RFC 6455
 * codec against the standard's known vectors:
 *   - the Sec-WebSocket-Accept derivation from RFC 6455 §1.3,
 *   - the masked client "Hello" data frame from RFC 6455 §5.7,
 *   - a tiny server BINARY frame (FIN=1, unmasked) round-trip,
 *   - the need-more (nullopt) signal on a truncated buffer, and
 *   - the 16-bit extended-length (126 marker) path.
 */

#include "libtracer/ws.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

std::vector<std::byte> bytes_of(std::initializer_list<std::uint8_t> vals) {
    std::vector<std::byte> v;
    v.reserve(vals.size());
    for (std::uint8_t b : vals) v.push_back(static_cast<std::byte>(b));
    return v;
}

std::vector<std::byte> bytes_of(std::string_view s) {
    std::vector<std::byte> v;
    v.reserve(s.size());
    for (char c : s) v.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
    return v;
}

std::string str_of(std::span<const std::byte> b) {
    std::string s;
    s.reserve(b.size());
    for (std::byte c : b) s.push_back(static_cast<char>(std::to_integer<std::uint8_t>(c)));
    return s;
}

}  // namespace

int main() {
    using namespace tr::net::ws;
    std::printf("transport_ws RFC 6455 protocol layer:\n");

    // RFC 6455 §1.3 — the canonical Sec-WebSocket-Accept worked example.
    {
        const std::string acc = accept_key("dGhlIHNhbXBsZSBub25jZQ==");
        check(acc == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
              "accept_key matches RFC 6455 §1.3 vector (s3pPLMBiTxaQ9kYGzzhZRbK+xOo=)");
    }

    // RFC 6455 §5.7 — single-frame masked "Hello" from a client.
    {
        const std::vector<std::byte> masked_hello =
            bytes_of({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58});
        const auto dec = decode_frame(masked_hello);
        check(dec.has_value(), "masked client \"Hello\" frame decodes");
        if (dec) {
            const frame_t& f = dec->first;
            check(f.op == opcode_t::TEXT, "  opcode == TEXT");
            check(f.fin, "  FIN set");
            check(str_of(f.payload) == "Hello", "  payload unmasks to \"Hello\"");
            check(dec->second == 11, "  consumed == 11 bytes");
        }
    }

    // Server BINARY frame: FIN=1, unmasked, 7-bit length. encode -> exact bytes.
    {
        const std::vector<std::byte> payload = bytes_of("Hi");
        const std::vector<std::byte> enc = encode_frame(opcode_t::BINARY, payload);
        const std::vector<std::byte> expect = bytes_of({0x82, 0x02, 'H', 'i'});
        check(enc == expect, "encode_frame(BINARY, \"Hi\") == 82 02 'H' 'i'");

        const auto dec = decode_frame(enc);
        check(dec.has_value(), "encoded BINARY frame decodes");
        if (dec) {
            check(dec->first.op == opcode_t::BINARY, "  round-trip opcode == BINARY");
            check(dec->first.fin, "  round-trip FIN set");
            check(str_of(dec->first.payload) == "Hi", "  round-trip payload == \"Hi\"");
            check(dec->second == enc.size(), "  round-trip consumes whole buffer");
        }
    }

    // Need-more: a 1-byte buffer is an incomplete frame -> nullopt.
    {
        const std::vector<std::byte> partial = bytes_of({0x81});
        check(!decode_frame(partial).has_value(), "1-byte buffer returns nullopt (need-more)");
    }

    // 16-bit extended length: 200-byte payload uses the 126 marker + 2-byte len.
    {
        std::vector<std::byte> payload(200, static_cast<std::byte>(0xAB));
        const std::vector<std::byte> enc = encode_frame(opcode_t::BINARY, payload);
        check(enc.size() == 2 + 2 + 200, "200-byte frame is 2 + 2-byte-len + 200 payload");
        check(std::to_integer<std::uint8_t>(enc[1]) == 126,
              "  uses the 126 extended-length marker");
        check(std::to_integer<std::uint8_t>(enc[2]) == 0x00 &&
                  std::to_integer<std::uint8_t>(enc[3]) == 0xC8,
              "  2-byte big-endian length == 200 (00 C8)");

        const auto dec = decode_frame(enc);
        check(dec.has_value(), "200-byte frame decodes");
        if (dec) {
            check(dec->first.payload.size() == 200, "  decoded payload is 200 bytes");
            check(dec->first.payload == payload, "  decoded payload round-trips byte-exactly");
            check(dec->second == enc.size(), "  consumes whole buffer");
        }
    }

    // Malformed: a 64-bit-length frame (len marker 127) claiming ~2^64 bytes but with a
    // short buffer. The bounds check must be overflow-safe — `pos + len` would wrap and
    // bypass a naive `buf.size() < pos + len`, causing an OOB read. Must return nullopt,
    // never crash / read past the buffer.
    {
        const std::vector<std::byte> evil =
            bytes_of({0x82, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xAA, 0xBB});
        check(!decode_frame(evil).has_value(),
              "64-bit over-long length is rejected (nullopt), no overflow/OOB read");
    }

    std::printf(g_failures == 0 ? "\nWS: PASS\n" : "\nWS: FAIL (%d)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
