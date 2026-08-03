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
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source.hpp"

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
 * @brief The largest payload an RFC 6455 CONTROL frame may carry (§5.5).
 *
 * "All control frames MUST have a payload length of 125 bytes or less and MUST NOT be
 * fragmented." Bounding the reply to a peer's PING at this is what lets the PONG be built
 * entirely on the stack — no allocation, hence no failure mode to have a drop policy about
 * (#848). It also removes the reflection/amplification primitive an unbounded echo was.
 */
inline constexpr std::size_t kMaxControlPayload = 125;

/** @brief True when @p op is an RFC 6455 CONTROL opcode (`0x8`–`0xF`, §5.5). */
[[nodiscard]] constexpr bool is_control_opcode(opcode_t op) noexcept {
    return (static_cast<std::uint8_t>(op) & 0x08u) != 0;
}

/** @brief The three outcomes of decoding one frame off a peer's byte stream. */
enum class decode_status_t : std::uint8_t {
    OK,             /**< @brief A complete, well-formed frame was decoded. */
    NEED_MORE,      /**< @brief The buffer does not yet hold a whole frame. */
    PROTOCOL_ERROR, /**< @brief The peer violated RFC 6455 — FAIL the connection. */
};

/** @brief One @ref decode_frame_checked outcome: a status, and (on `OK`) the frame. */
struct decode_result_t {
    decode_status_t status = decode_status_t::NEED_MORE; /**< @brief Which outcome. */
    frame_t frame{};        /**< @brief The decoded frame — meaningful only on `OK`. */
    std::size_t consumed{}; /**< @brief Bytes taken off the front of the buffer, on `OK`. */
};

namespace detail {

/**
 * @brief The ONE RFC 6455 frame-decode implementation, shared by @ref ws::decode_frame and
 *        @ref ws::decode_frame_checked — they differ only in @p enforce_control_rules.
 *
 * @param buf                   The peer's byte stream.
 * @param enforce_control_rules Apply RFC 6455 §5.5 (see @ref ws::decode_frame_checked).
 */
[[nodiscard]] inline decode_result_t decode_one(std::span<const std::byte> buf,
                                                bool enforce_control_rules) {
    if (buf.size() < 2) return {};

    const std::uint8_t b0 = std::to_integer<std::uint8_t>(buf[0]);
    const std::uint8_t b1 = std::to_integer<std::uint8_t>(buf[1]);

    const bool fin = (b0 & 0x80u) != 0;
    const auto op = static_cast<opcode_t>(b0 & 0x0Fu);
    const bool masked = (b1 & 0x80u) != 0;
    std::uint64_t len = b1 & 0x7Fu;

    std::size_t pos = 2;
    if (len == 126) {
        if (buf.size() < pos + 2) return {};
        len = (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(buf[pos])) << 8) |
              static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(buf[pos + 1]));
        pos += 2;
    } else if (len == 127) {
        if (buf.size() < pos + 8) return {};
        len = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            len = (len << 8) | std::to_integer<std::uint8_t>(buf[pos + i]);
        }
        pos += 8;
    }

    // RFC 6455 §5.5: a control frame is at most 125 bytes and is never fragmented.
    // Checked off the HEADER, so an absurd declared length is rejected without ever
    // buffering (or echoing) the payload it claims.
    if (enforce_control_rules && is_control_opcode(op) && (len > kMaxControlPayload || !fin))
        return {decode_status_t::PROTOCOL_ERROR, {}, 0};

    std::array<std::uint8_t, 4> mask_key{};
    if (masked) {
        if (buf.size() < pos + 4) return {};
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
    if (len > buf.size() - pos) return {};

    decode_result_t out;
    out.status = decode_status_t::OK;
    out.frame.op = op;
    out.frame.fin = fin;
    out.frame.payload.resize(static_cast<std::size_t>(len));
    for (std::size_t i = 0; i < len; ++i) {
        std::uint8_t byte = std::to_integer<std::uint8_t>(buf[pos + i]);
        if (masked) byte ^= mask_key[i % 4];
        out.frame.payload[i] = static_cast<std::byte>(byte);
    }
    out.consumed = pos + static_cast<std::size_t>(len);
    return out;
}

}  // namespace detail

/**
 * @brief Decode exactly one RFC 6455 frame from the front of @p buf, distinguishing
 *        "need more bytes" from "the peer broke the protocol" — the form a TRANSPORT uses.
 *
 * Handles the FIN bit, opcode, the MASK bit with its 4-byte masking key
 * (client→server frames are masked; the payload is unmasked in place), and the
 * 7 / 16 / 64-bit extended length encodings. Both masked and unmasked frames decode.
 *
 * **RFC 6455 §5.5 is enforced here** (#848): a CONTROL opcode (`0x8`-`0xF`) carrying more
 * than @ref kMaxControlPayload bytes, or arriving with `FIN` clear, is `PROTOCOL_ERROR` —
 * and it is diagnosed from the frame HEADER, before the payload has to be buffered, so an
 * absurd declared length costs nothing. Without this a peer could send a 1 MiB PING and have
 * the node echo it straight back: an unauthenticated reflection/amplification primitive,
 * and (because the echo was a `std::vector`) a peer-triggered `abort()` on the
 * `-fno-exceptions` profile.
 *
 * @param buf A byte stream that may contain a partial or whole frame, possibly
 *            followed by more frames.
 * @return `NEED_MORE` while @p buf is short of a whole frame, `PROTOCOL_ERROR` on an
 *         RFC 6455 violation (the caller must fail the connection, RFC 6455 §7.1.7),
 *         else `OK` with the frame and the bytes consumed.
 */
[[nodiscard]] inline decode_result_t decode_frame_checked(std::span<const std::byte> buf) {
    return detail::decode_one(buf, /*enforce_control_rules=*/true);
}

/**
 * @brief Decode exactly one RFC 6455 frame from the front of @p buf — the pure
 *        outcome-collapsing decoder, byte-for-byte the behaviour it has always had.
 *
 * Deliberately does NOT apply the §5.5 control-frame rules: this is the function
 * `tests/conformance/ws_diff_fuzz.py` holds against the TypeScript `decodeFrame`, and the
 * two cores must answer identically on every input. §5.5 is a CONNECTION-FAILURE policy,
 * not a decode outcome — it belongs to whoever owns the socket. Every transport in this
 * repository therefore uses @ref decode_frame_checked; only a caller that has no connection
 * to fail should use this one.
 *
 * @param buf A byte stream that may contain a partial or whole frame, possibly
 *            followed by more frames.
 * @return nullopt if @p buf does not yet hold a complete frame (need more bytes);
 *         otherwise the decoded frame paired with the bytes consumed from the front.
 */
[[nodiscard]] inline std::optional<std::pair<frame_t, std::size_t>> decode_frame(
    std::span<const std::byte> buf) {
    decode_result_t r = detail::decode_one(buf, /*enforce_control_rules=*/false);
    if (r.status != decode_status_t::OK) return std::nullopt;
    return std::make_pair(std::move(r.frame), r.consumed);
}

/**
 * @brief The largest a server→client frame HEADER can be: 1 FIN/opcode byte +
 *        1 length byte + 8 extended-length bytes (the 64-bit form).
 *
 * Server frames carry NO masking key (RFC 6455 §5.1), so 10 bytes bounds every
 * header. A stack buffer of this size is the fixed prefix a scatter-gather
 * egress rides ahead of the payload spans.
 */
inline constexpr std::size_t kMaxServerFrameHeader = 10;

/**
 * @brief Encode ONLY a server→client RFC 6455 frame header into @p out, UNMASKED.
 *
 * Writes byte0 = `(fin?0x80:0)|op`, then the payload length in the smallest legal
 * encoding (7-bit, then the 126 + 2-byte u16-BE marker, then the 127 + 8-byte
 * u64-BE marker); the MASK bit is always 0 (server frames MUST NOT be masked,
 * RFC 6455 §5.1). This is the ONE length-encoding implementation —
 * `encode_frame()` appends the payload after it, and the transport's zero-copy
 * scatter-gather egress rides the payload spans behind it with no copy.
 *
 * @param out The header buffer to fill (`kMaxServerFrameHeader` bytes suffice).
 * @param op  The frame opcode.
 * @param len The payload length in bytes.
 * @param fin The FIN bit (default true — a complete, unfragmented message;
 *            pass false for a non-final fragment, RFC 6455 §5.4).
 * @return The number of header bytes written into @p out (2, 4, or 10).
 */
[[nodiscard]] inline std::size_t encode_frame_header(
    std::array<std::byte, kMaxServerFrameHeader>& out, opcode_t op, std::size_t len,
    bool fin = true) {
    out[0] = static_cast<std::byte>((fin ? 0x80u : 0x00u) | static_cast<std::uint8_t>(op));
    if (len < 126) {
        out[1] = static_cast<std::byte>(len);  // MASK=0
        return 2;
    }
    if (len <= 0xFFFF) {
        out[1] = static_cast<std::byte>(126);
        out[2] = static_cast<std::byte>((len >> 8) & 0xFFu);
        out[3] = static_cast<std::byte>(len & 0xFFu);
        return 4;
    }
    out[1] = static_cast<std::byte>(127);
    for (int i = 0; i < 8; ++i) {
        out[2 + i] =
            static_cast<std::byte>((static_cast<std::uint64_t>(len) >> ((7 - i) * 8)) & 0xFFu);
    }
    return 10;
}

/**
 * @brief Encode one server→client RFC 6455 frame: given opcode, UNMASKED.
 *
 * Server frames MUST NOT be masked (RFC 6455 §5.1), so the MASK bit is always
 * 0 and no masking key is emitted. The length uses the smallest legal encoding
 * (7-bit, then the 126 + 2-byte marker, then the 127 + 8-byte marker) — encoded
 * by the shared `encode_frame_header()` helper, then the payload appended.
 *
 * @param op      The frame opcode.
 * @param payload The application payload to send.
 * @param fin     The FIN bit (default true — a complete, unfragmented message;
 *                pass false for a non-final fragment, RFC 6455 §5.4).
 * @return The fully serialized frame bytes, ready to write to the socket.
 */
[[nodiscard]] inline std::vector<std::byte> encode_frame(opcode_t op,
                                                         std::span<const std::byte> payload,
                                                         bool fin = true) {
    std::array<std::byte, kMaxServerFrameHeader> header{};
    const std::size_t hlen = encode_frame_header(header, op, payload.size(), fin);

    std::vector<std::byte> out;
    out.reserve(hlen + payload.size());
    out.insert(out.end(), header.begin(), header.begin() + static_cast<std::ptrdiff_t>(hlen));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/**
 * @brief The largest a complete server→client CONTROL frame can be: a 2-byte header
 *        (a control length is always < 126) plus @ref kMaxControlPayload payload bytes.
 */
inline constexpr std::size_t kMaxServerControlFrame = 2 + kMaxControlPayload;

/**
 * @brief The largest a complete client→server CONTROL frame can be: a 2-byte header,
 *        the 4-byte masking key, and @ref kMaxControlPayload payload bytes.
 */
inline constexpr std::size_t kMaxClientControlFrame = 2 + 4 + kMaxControlPayload;

/**
 * @brief Encode one whole server→client CONTROL frame (PONG/CLOSE) into @p out — on the
 *        STACK, nothrow, UNMASKED.
 *
 * The reply to a peer's PING must not be able to fail: dropping a PONG costs the link
 * (RFC 6455 §5.5.2/§5.5.3 make it the required response, and a peer whose PINGs go
 * unanswered may fail the connection), while closing the session lets whoever caused the
 * heap pressure decide the topology. Since §5.5 bounds a control payload at
 * @ref kMaxControlPayload — enforced by @ref decode_frame_checked — the whole frame fits a
 * fixed stack buffer and there is no failure mode left to have a policy about (#848).
 *
 * Length encoding is delegated to @ref encode_frame_header, which stays the ONE
 * length-encoding implementation.
 *
 * @param out     The frame buffer to fill.
 * @param op      The control opcode (PONG / CLOSE).
 * @param payload The control payload to echo (at most @ref kMaxControlPayload bytes).
 * @retval 0 @p payload exceeds @ref kMaxControlPayload — nothing was written.
 * @return The number of frame bytes written into @p out.
 */
[[nodiscard]] inline std::size_t encode_server_control(
    std::array<std::byte, kMaxServerControlFrame>& out, opcode_t op,
    std::span<const std::byte> payload) noexcept {
    if (payload.size() > kMaxControlPayload) return 0;
    std::array<std::byte, kMaxServerFrameHeader> header{};
    const std::size_t hlen = encode_frame_header(header, op, payload.size());  // always 2 here
    std::memcpy(out.data(), header.data(), hlen);
    if (!payload.empty()) std::memcpy(out.data() + hlen, payload.data(), payload.size());
    return hlen + payload.size();
}

/**
 * @brief Encode one whole client→server CONTROL frame into @p out — on the STACK, nothrow,
 *        MASKED (RFC 6455 §5.1).
 *
 * The client twin of @ref encode_server_control: same reasoning, plus the 4-byte masking
 * key and the payload XOR. A control length is always < 126, so the header is the 2-byte
 * form with `MASK=1`.
 *
 * @param out      The frame buffer to fill.
 * @param op       The control opcode (PONG / CLOSE).
 * @param payload  The control payload to echo (at most @ref kMaxControlPayload bytes).
 * @param mask_key The 32-bit masking key (its 4 bytes form the RFC 6455 key).
 * @retval 0 @p payload exceeds @ref kMaxControlPayload — nothing was written.
 * @return The number of frame bytes written into @p out.
 */
[[nodiscard]] inline std::size_t encode_client_control(
    std::array<std::byte, kMaxClientControlFrame>& out, opcode_t op,
    std::span<const std::byte> payload, std::uint32_t mask_key) noexcept {
    const std::size_t len = payload.size();
    if (len > kMaxControlPayload) return 0;
    out[0] = static_cast<std::byte>(0x80u | static_cast<std::uint8_t>(op));   // FIN=1
    out[1] = static_cast<std::byte>(0x80u | static_cast<std::uint8_t>(len));  // MASK=1
    const std::array<std::uint8_t, 4> mk{static_cast<std::uint8_t>((mask_key >> 24) & 0xFFu),
                                         static_cast<std::uint8_t>((mask_key >> 16) & 0xFFu),
                                         static_cast<std::uint8_t>((mask_key >> 8) & 0xFFu),
                                         static_cast<std::uint8_t>(mask_key & 0xFFu)};
    for (std::size_t i = 0; i < 4; ++i) out[2 + i] = static_cast<std::byte>(mk[i]);
    for (std::size_t i = 0; i < len; ++i)
        out[6 + i] = static_cast<std::byte>(std::to_integer<std::uint8_t>(payload[i]) ^ mk[i % 4]);
    return 6 + len;
}

namespace detail {

/** @brief The exact byte count a client→server frame of @p payload_len occupies:
 *         the smallest legal length encoding, the 4-byte masking key, then the payload. */
[[nodiscard]] constexpr std::size_t client_frame_bytes(std::size_t payload_len) noexcept {
    const std::size_t hlen = payload_len < 126 ? 2u : (payload_len <= 0xFFFF ? 4u : 10u);
    return hlen + 4u + payload_len;
}

/**
 * @brief Write one whole MASKED client frame into @p out — the ONE client-side
 *        length-encoding + masking implementation, shared by the throwing
 *        @ref ws::encode_client_frame and the nothrow @ref ws::try_encode_client_frame.
 *
 * Writes through an already-sized buffer by index rather than appending, so the store it
 * fills is the caller's choice (a `std::vector` for the throwing form, a
 * `tr::mem::block_array_t` for the nothrow one) and this function allocates nothing at all.
 *
 * @pre @p out.size() >= `client_frame_bytes(payload.size())`.
 * @return The number of bytes written (exactly `client_frame_bytes(payload.size())`).
 */
inline std::size_t put_client_frame(std::span<std::byte> out, opcode_t op,
                                    std::span<const std::byte> payload, std::uint32_t mask_key,
                                    bool fin) noexcept {
    std::size_t n = 0;
    out[n++] = static_cast<std::byte>((fin ? 0x80u : 0x00u) | static_cast<std::uint8_t>(op));

    const std::size_t len = payload.size();
    if (len < 126) {
        out[n++] = static_cast<std::byte>(0x80u | static_cast<std::uint8_t>(len));  // MASK=1
    } else if (len <= 0xFFFF) {
        out[n++] = static_cast<std::byte>(0x80u | 126u);
        out[n++] = static_cast<std::byte>((len >> 8) & 0xFFu);
        out[n++] = static_cast<std::byte>(len & 0xFFu);
    } else {
        out[n++] = static_cast<std::byte>(0x80u | 127u);
        for (int i = 7; i >= 0; --i) {
            out[n++] = static_cast<std::byte>((static_cast<std::uint64_t>(len) >> (i * 8)) & 0xFFu);
        }
    }

    const std::array<std::uint8_t, 4> mk{static_cast<std::uint8_t>((mask_key >> 24) & 0xFFu),
                                         static_cast<std::uint8_t>((mask_key >> 16) & 0xFFu),
                                         static_cast<std::uint8_t>((mask_key >> 8) & 0xFFu),
                                         static_cast<std::uint8_t>(mask_key & 0xFFu)};
    for (std::uint8_t m : mk) out[n++] = static_cast<std::byte>(m);
    for (std::size_t i = 0; i < len; ++i) {
        out[n++] = static_cast<std::byte>(std::to_integer<std::uint8_t>(payload[i]) ^ mk[i % 4]);
    }
    return n;
}

}  // namespace detail

/**
 * @brief NOTHROW @ref encode_client_frame — build the masked frame into @p out, soft-failing
 *        on OOM instead of a `bad_alloc` `abort()` under `-fno-exceptions` (#848).
 *
 * The one WS egress encoder that survives as a nothrow twin rather than being deleted: a
 * client frame MUST be masked (RFC 6455 §5.1), so the bytes on the wire are not the caller's
 * bytes and cannot be gathered by reference the way the UNMASKED server frames are. Reuse
 * one @p out buffer across calls and the steady state allocates nothing. On `0` the caller
 * DROPS the frame — the same answer the delivery path already gives under exhaustion.
 *
 * @par Why @ref tr::mem::block_array_t and not a `std::vector` + `tr::detail::try_reserve`
 * `try_reserve` is `noexcept`, but only its PROBE is nothrow: it frees the probe block and
 * then runs the throwing `std::vector::reserve`, on the argument that the just-freed block
 * satisfies it. Refuse that second allocation and the `bad_alloc` crosses a `noexcept`
 * boundary — `std::terminate`, not a soft failure, and a bare `abort()` under
 * `-fno-exceptions`. That is the very outcome #848 exists to remove, so this path draws from
 * the failable seam instead (ADR-0065): growth is ONE `block_source_t::try_alloc` that
 * answers `nullptr`, with no unguardable second step.
 *
 * @param out      The reusable frame buffer; its capacity is retained across calls.
 * @param op       The frame opcode.
 * @param payload  The application payload to send.
 * @param mask_key The 32-bit masking key (its 4 bytes form the RFC 6455 key).
 * @param fin      The FIN bit (default true — a complete, unfragmented message).
 * @retval 0 The frame buffer could not be grown — nothing was written, drop the frame.
 * @return The number of frame bytes written to `out.data()` (never 0 on success: a client
 *         frame is at least a 2-byte header plus the 4-byte masking key).
 */
[[nodiscard]] inline std::size_t try_encode_client_frame(mem::block_array_t<std::byte>& out,
                                                         opcode_t op,
                                                         std::span<const std::byte> payload,
                                                         std::uint32_t mask_key,
                                                         bool fin = true) noexcept {
    const std::size_t want = detail::client_frame_bytes(payload.size());
    if (!out.reserve(want)) return 0;
    return detail::put_client_frame(std::span<std::byte>(out.data(), want), op, payload, mask_key,
                                    fin);
}

/**
 * @brief Encode one client→server RFC 6455 frame: given opcode, MASKED.
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
 * @param fin      The FIN bit (default true — a complete, unfragmented message;
 *                 pass false for a non-final fragment, RFC 6455 §5.4).
 * @return The fully serialized masked frame bytes, ready to write to the socket.
 */
[[nodiscard]] inline std::vector<std::byte> encode_client_frame(opcode_t op,
                                                                std::span<const std::byte> payload,
                                                                std::uint32_t mask_key,
                                                                bool fin = true) {
    std::vector<std::byte> out(detail::client_frame_bytes(payload.size()));  // the throwing grow
    detail::put_client_frame(std::span<std::byte>(out), op, payload, mask_key, fin);
    return out;
}

}  // namespace tr::net::ws
