/**
 * @file
 * @brief ws differential-fuzz harness (#60 / hardening) — the C++ side of the RFC 6455 WebSocket
 *        frame-decoder differential fuzzer.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The ws frame layer is
 * network-facing attack surface: it parses untrusted bytes (FIN/opcode, the
 * 7/16/64-bit length encodings, the client mask, and the overflow-safe
 * 64-bit-over-long path) BEFORE the TLV layer ever sees them, so it must never
 * crash / read out of bounds on ANY input — that invariant is exactly what this
 * harness exists to exercise.
 *
 * Contract (one deterministic line of stdout per stdin line):
 *   - input is one hex-encoded frame byte sequence per line;
 *   - on a full decode it prints  OK\t<opcode>\t<fin>\t<consumed>\t<payload-hex>
 *     where <opcode> is the raw 4-bit opcode (decimal), <fin> is 0/1, <consumed>
 *     is the number of header+payload bytes consumed from the front of the buffer,
 *     and <payload-hex> is the unmasked payload as lowercase hex (empty for a
 *     zero-length payload);
 *   - on need-more (decode_frame -> nullopt: a truncated header/length/mask/payload
 *     OR a 64-bit over-long length on a short buffer) it prints  NEED_MORE;
 *   - on a harness-level problem (non-hex / empty line) it prints  ERR:<reason>.
 *
 * The TypeScript twin lives at
 * bindings/typescript/packages/transport-ws/fuzz/decode_harness.mjs and emits the
 * byte-identical contract; tests/conformance/ws_diff_fuzz.py feeds both and
 * asserts they agree. This is NOT an add_test() unit test — it is a helper binary
 * the Python driver spawns (mirrors ws_interop_server).
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/ws.hpp"

namespace {

/** @brief Decode one hex string to bytes; nullopt on odd length or a non-hex nibble. */
std::optional<std::vector<std::byte>> from_hex(std::string_view s) {
    if (s.size() % 2 != 0) return std::nullopt;
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::byte> out;
    out.reserve(s.size() / 2);
    for (std::size_t i = 0; i < s.size(); i += 2) {
        const int hi = nibble(s[i]);
        const int lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<std::byte>((hi << 4) | lo));
    }
    return out;
}

std::string to_hex(std::span<const std::byte> b) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (const std::byte by : b) {
        const auto v = std::to_integer<std::uint8_t>(by);
        out.push_back(kHex[v >> 4]);
        out.push_back(kHex[v & 0x0F]);
    }
    return out;
}

}  // namespace

int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) {
            std::printf("ERR:EMPTY_LINE\n");
            continue;
        }
        const auto bytes = from_hex(line);
        if (!bytes) {
            std::printf("ERR:BAD_HEX\n");
            continue;
        }
        const auto dec = tr::net::ws::decode_frame(*bytes);
        if (!dec) {
            std::printf("NEED_MORE\n");
            continue;
        }
        const tr::net::ws::frame_t& f = dec->first;
        const std::size_t consumed = dec->second;
        std::printf("OK\t%u\t%d\t%zu\t%s\n", static_cast<unsigned>(static_cast<std::uint8_t>(f.op)),
                    f.fin ? 1 : 0, consumed, to_hex(f.payload).c_str());
    }
    return 0;
}
