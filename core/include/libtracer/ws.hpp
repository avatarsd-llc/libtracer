/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * transport_ws (#54) — RFC 6455 WebSocket PROTOCOL layer: the pure,
 * socket-free, unit-testable part. Hand-written, no external WS library.
 *
 * This header covers exactly two concerns:
 *   1. the opening-handshake key derivation (SHA-1 + base64 → Sec-WebSocket-Accept), and
 *   2. the data-frame codec (decode one masked/unmasked frame; encode one
 *      server frame, FIN=1, unmasked).
 *
 * It deliberately knows nothing about sockets, HTTP parsing, or fragmentation
 * reassembly — those live in a later increment. Header-only, inline; mirrors
 * the style of crc.hpp.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tr::net::ws {

namespace detail {

/** @brief Left-rotate a 32-bit word by @p n bits (SHA-1 primitive). */
[[nodiscard]] constexpr std::uint32_t rotl32(std::uint32_t v, unsigned n) noexcept {
    return (v << n) | (v >> (32 - n));
}

}  // namespace detail

/**
 * @brief Standard SHA-1 over an arbitrary byte span (RFC 3174 / FIPS 180-1).
 *
 * Returns the 20-byte digest. Used by accept_key() for the RFC 6455 opening
 * handshake; SHA-1 is required there for protocol compatibility, not security.
 *
 * @param data Bytes to hash.
 * @return The 20-byte big-endian SHA-1 digest.
 */
[[nodiscard]] inline std::array<std::byte, 20> sha1(std::span<const std::byte> data) noexcept {
    std::uint32_t h0 = 0x67452301u;
    std::uint32_t h1 = 0xEFCDAB89u;
    std::uint32_t h2 = 0x98BADCFEu;
    std::uint32_t h3 = 0x10325476u;
    std::uint32_t h4 = 0xC3D2E1F0u;

    const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8u;

    // Build the padded message: original bytes, 0x80, zero pad to 56 mod 64,
    // then the 64-bit big-endian bit length.
    std::vector<std::uint8_t> msg(data.size());
    for (std::size_t i = 0; i < data.size(); ++i) {
        msg[i] = std::to_integer<std::uint8_t>(data[i]);
    }
    msg.push_back(0x80u);
    while (msg.size() % 64u != 56u) {
        msg.push_back(0x00u);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<std::uint8_t>((bit_len >> (i * 8)) & 0xFFu));
    }

    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        std::array<std::uint32_t, 80> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(msg[chunk + i * 4 + 0]) << 24) |
                   (static_cast<std::uint32_t>(msg[chunk + i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(msg[chunk + i * 4 + 2]) << 8) |
                   (static_cast<std::uint32_t>(msg[chunk + i * 4 + 3]));
        }
        for (std::size_t i = 16; i < 80; ++i) {
            w[i] = detail::rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        std::uint32_t a = h0;
        std::uint32_t b = h1;
        std::uint32_t c = h2;
        std::uint32_t d = h3;
        std::uint32_t e = h4;

        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t tmp = detail::rotl32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = detail::rotl32(b, 30);
            b = a;
            a = tmp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<std::byte, 20> out{};
    const std::uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (std::size_t i = 0; i < 5; ++i) {
        out[i * 4 + 0] = static_cast<std::byte>((hs[i] >> 24) & 0xFFu);
        out[i * 4 + 1] = static_cast<std::byte>((hs[i] >> 16) & 0xFFu);
        out[i * 4 + 2] = static_cast<std::byte>((hs[i] >> 8) & 0xFFu);
        out[i * 4 + 3] = static_cast<std::byte>(hs[i] & 0xFFu);
    }
    return out;
}

/**
 * @brief Standard base64 encoding (RFC 4648, with '=' padding).
 *
 * @param data Bytes to encode.
 * @return The base64 text.
 */
[[nodiscard]] inline std::string base64(std::span<const std::byte> data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const std::uint32_t n = (std::to_integer<std::uint32_t>(data[i]) << 16) |
                                (std::to_integer<std::uint32_t>(data[i + 1]) << 8) |
                                (std::to_integer<std::uint32_t>(data[i + 2]));
        out.push_back(kAlphabet[(n >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(n >> 12) & 0x3Fu]);
        out.push_back(kAlphabet[(n >> 6) & 0x3Fu]);
        out.push_back(kAlphabet[n & 0x3Fu]);
    }

    const std::size_t rem = data.size() - i;
    if (rem == 1) {
        const std::uint32_t n = std::to_integer<std::uint32_t>(data[i]) << 16;
        out.push_back(kAlphabet[(n >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(n >> 12) & 0x3Fu]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const std::uint32_t n = (std::to_integer<std::uint32_t>(data[i]) << 16) |
                                (std::to_integer<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(kAlphabet[(n >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(n >> 12) & 0x3Fu]);
        out.push_back(kAlphabet[(n >> 6) & 0x3Fu]);
        out.push_back('=');
    }
    return out;
}

/**
 * @brief Compute the RFC 6455 Sec-WebSocket-Accept value for a client key.
 *
 * accept = base64(sha1(client_key + GUID)) where GUID is the fixed magic
 * "258EAFA5-E914-47DA-95CA-C5AB0DC85B11". The server returns this in the
 * 101 Switching Protocols response to prove it spoke RFC 6455.
 *
 * @param client_key The raw Sec-WebSocket-Key header value sent by the client.
 * @return The Sec-WebSocket-Accept text.
 */
[[nodiscard]] inline std::string accept_key(std::string_view client_key) {
    static constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::vector<std::byte> buf;
    buf.reserve(client_key.size() + kGuid.size());
    for (char ch : client_key) buf.push_back(static_cast<std::byte>(ch));
    for (char ch : kGuid) buf.push_back(static_cast<std::byte>(ch));
    const std::array<std::byte, 20> digest = sha1(buf);
    return base64(digest);
}

/** @brief RFC 6455 frame opcodes (the subset libtracer cares about). */
enum class opcode_t : std::uint8_t {
    CONT = 0x0,   /**< Continuation frame. */
    TEXT = 0x1,   /**< Text (UTF-8) data frame. */
    BINARY = 0x2, /**< Binary data frame. */
    CLOSE = 0x8,  /**< Connection close control frame. */
    PING = 0x9,   /**< Ping control frame. */
    PONG = 0xA,   /**< Pong control frame. */
};

/** @brief One decoded RFC 6455 data/control frame (payload already unmasked). */
struct frame_t {
    opcode_t op;                    /**< Frame opcode. */
    bool fin;                       /**< FIN bit (true = final fragment). */
    std::vector<std::byte> payload; /**< Unmasked application payload. */
};

/**
 * @brief Decode exactly one RFC 6455 frame from the front of @p buf.
 *
 * Handles the FIN bit, opcode, the MASK bit with its 4-byte masking key
 * (client→server frames are masked; the payload is unmasked in place), and the
 * 7 / 16 / 64-bit extended length encodings. Both masked and unmasked frames
 * decode.
 *
 * @param buf A byte stream that may contain a partial or whole frame, possibly
 *            followed by more frames.
 * @return nullopt if @p buf does not yet hold a complete frame (need more
 *         bytes); otherwise the decoded frame paired with the number of bytes
 *         consumed from the front of @p buf.
 */
[[nodiscard]] inline std::optional<std::pair<frame_t, std::size_t>> decode_frame(
    std::span<const std::byte> buf) {
    if (buf.size() < 2) return std::nullopt;

    const std::uint8_t b0 = std::to_integer<std::uint8_t>(buf[0]);
    const std::uint8_t b1 = std::to_integer<std::uint8_t>(buf[1]);

    const bool fin = (b0 & 0x80u) != 0;
    const auto op = static_cast<opcode_t>(b0 & 0x0Fu);
    const bool masked = (b1 & 0x80u) != 0;
    std::uint64_t len = b1 & 0x7Fu;

    std::size_t pos = 2;
    if (len == 126) {
        if (buf.size() < pos + 2) return std::nullopt;
        len = (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(buf[pos])) << 8) |
              static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(buf[pos + 1]));
        pos += 2;
    } else if (len == 127) {
        if (buf.size() < pos + 8) return std::nullopt;
        len = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            len = (len << 8) | std::to_integer<std::uint8_t>(buf[pos + i]);
        }
        pos += 8;
    }

    std::array<std::uint8_t, 4> mask_key{};
    if (masked) {
        if (buf.size() < pos + 4) return std::nullopt;
        for (std::size_t i = 0; i < 4; ++i) {
            mask_key[i] = std::to_integer<std::uint8_t>(buf[pos + i]);
        }
        pos += 4;
    }

    // Overflow-safe length check: `pos + len` can wrap (a 64-bit-length frame of
    // 0xFFFF...FF), bypassing `buf.size() < pos + len` and causing an OOB read in the
    // unmask loop. `pos <= buf.size()` is guaranteed by the header/mask checks above, so
    // `buf.size() - pos` is non-negative and `len > buf.size() - pos` cannot overflow.
    // This also bounds `len` by the bytes available, so the resize() below cannot
    // over-allocate.
    if (len > buf.size() - pos) return std::nullopt;

    frame_t frame;
    frame.op = op;
    frame.fin = fin;
    frame.payload.resize(static_cast<std::size_t>(len));
    for (std::size_t i = 0; i < len; ++i) {
        std::uint8_t byte = std::to_integer<std::uint8_t>(buf[pos + i]);
        if (masked) byte ^= mask_key[i % 4];
        frame.payload[i] = static_cast<std::byte>(byte);
    }

    return std::make_pair(std::move(frame), pos + static_cast<std::size_t>(len));
}

/**
 * @brief Encode one server→client RFC 6455 frame: FIN=1, given opcode, UNMASKED.
 *
 * Server frames MUST NOT be masked (RFC 6455 §5.1), so the MASK bit is always
 * 0 and no masking key is emitted. The length uses the smallest legal encoding
 * (7-bit, then the 126 + 2-byte marker, then the 127 + 8-byte marker).
 *
 * @param op      The frame opcode.
 * @param payload The application payload to send.
 * @return The fully serialized frame bytes, ready to write to the socket.
 */
[[nodiscard]] inline std::vector<std::byte> encode_frame(opcode_t op,
                                                         std::span<const std::byte> payload) {
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>(0x80u | static_cast<std::uint8_t>(op)));  // FIN=1

    const std::size_t len = payload.size();
    if (len < 126) {
        out.push_back(static_cast<std::byte>(len));  // MASK=0
    } else if (len <= 0xFFFF) {
        out.push_back(static_cast<std::byte>(126));
        out.push_back(static_cast<std::byte>((len >> 8) & 0xFFu));
        out.push_back(static_cast<std::byte>(len & 0xFFu));
    } else {
        out.push_back(static_cast<std::byte>(127));
        for (int i = 7; i >= 0; --i) {
            out.push_back(
                static_cast<std::byte>((static_cast<std::uint64_t>(len) >> (i * 8)) & 0xFFu));
        }
    }

    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/**
 * @brief Encode one client→server RFC 6455 frame: FIN=1, given opcode, MASKED.
 *
 * Client frames MUST be masked (RFC 6455 §5.1): the MASK bit is set, a 4-byte
 * masking key is emitted big-endian after the length, and every payload byte is
 * XOR'd with `mask_key[i % 4]`. The caller supplies @p mask_key per frame; it
 * need not be cryptographically strong (libtracer is not defending against a
 * same-process attacker), only varied — a counter-derived value is fine. The
 * length uses the smallest legal encoding (7-bit, then 126 + 2 bytes, then
 * 127 + 8 bytes). The server-side encode_frame() above is unaffected.
 *
 * @param op       The frame opcode.
 * @param payload  The application payload to send.
 * @param mask_key The 32-bit masking key (its 4 bytes form the RFC 6455 key).
 * @return The fully serialized masked frame bytes, ready to write to the socket.
 */
[[nodiscard]] inline std::vector<std::byte> encode_client_frame(opcode_t op,
                                                                std::span<const std::byte> payload,
                                                                std::uint32_t mask_key) {
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>(0x80u | static_cast<std::uint8_t>(op)));  // FIN=1

    const std::size_t len = payload.size();
    if (len < 126) {
        out.push_back(static_cast<std::byte>(0x80u | static_cast<std::uint8_t>(len)));  // MASK=1
    } else if (len <= 0xFFFF) {
        out.push_back(static_cast<std::byte>(0x80u | 126u));
        out.push_back(static_cast<std::byte>((len >> 8) & 0xFFu));
        out.push_back(static_cast<std::byte>(len & 0xFFu));
    } else {
        out.push_back(static_cast<std::byte>(0x80u | 127u));
        for (int i = 7; i >= 0; --i) {
            out.push_back(
                static_cast<std::byte>((static_cast<std::uint64_t>(len) >> (i * 8)) & 0xFFu));
        }
    }

    const std::array<std::uint8_t, 4> mk{static_cast<std::uint8_t>((mask_key >> 24) & 0xFFu),
                                         static_cast<std::uint8_t>((mask_key >> 16) & 0xFFu),
                                         static_cast<std::uint8_t>((mask_key >> 8) & 0xFFu),
                                         static_cast<std::uint8_t>(mask_key & 0xFFu)};
    for (std::uint8_t m : mk) out.push_back(static_cast<std::byte>(m));
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(
            static_cast<std::byte>(std::to_integer<std::uint8_t>(payload[i]) ^ mk[i % 4]));
    }
    return out;
}

}  // namespace tr::net::ws
