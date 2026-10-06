/**
 * @file
 * @brief transport_ws PROTOCOL-layer test (#54).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Asserts the hand-written RFC 6455
 * codec against the standard's known vectors:
 *   - the Sec-WebSocket-Accept derivation from RFC 6455 §1.3,
 *   - the masked client "Hello" data frame from RFC 6455 §5.7,
 *   - a tiny server BINARY frame (FIN=1, unmasked) round-trip,
 *   - the need-more (nullopt) signal on a truncated buffer, and
 *   - the 16-bit extended-length (126 marker) path, and
 *   - the word-wide unmask (#1922) against a byte-wise reference at every length mod 8 and
 *     every starting mask offset.
 */

#include "libtracer/ws.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "test_support.hpp"

namespace {

using tr::testing::check;

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

/**
 * @brief One UNMASKED frame carrying the RAW opcode nibble @p op, 7-bit length form.
 *
 * Takes a `std::uint8_t` rather than an `opcode_t` precisely because the values under test
 * are the ones `opcode_t` does NOT name: the RFC 6455 §5.2 reserved ranges. FIN and the
 * payload size are the caller's, so a reserved CONTROL opcode can be given the perfectly
 * legal §5.5 shape that is the sharp half of the case.
 */
/** @brief A whole server frame in a `std::vector` — the test-side reference form of
 *         `ws::try_encode_frame` (#1780 removed the library's vector-returning encoder). */
[[maybe_unused]] std::vector<std::byte> server_frame_ref(tr::net::ws::opcode_t op,
                                                         std::span<const std::byte> payload) {
    std::array<std::byte, tr::net::ws::kMaxServerFrameHeader> header{};
    const std::size_t hlen = tr::net::ws::encode_frame_header(header, op, payload.size());
    std::vector<std::byte> out(header.begin(), header.begin() + static_cast<std::ptrdiff_t>(hlen));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/** @brief A whole masked client frame in a `std::vector` — the test-side reference form of
 *         `ws::try_encode_client_frame`. */
[[maybe_unused]] std::vector<std::byte> client_frame_ref(tr::net::ws::opcode_t op,
                                                         std::span<const std::byte> payload,
                                                         std::uint32_t mask_key) {
    std::vector<std::byte> out(tr::net::ws::detail::client_frame_bytes(payload.size()));
    tr::net::ws::detail::put_client_frame(out, op, payload, mask_key, /*fin=*/true);
    return out;
}

std::vector<std::byte> raw_op_frame(std::uint8_t op, std::size_t len, bool fin = true) {
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>((fin ? 0x80u : 0x00u) | (op & 0x0Fu)));
    out.push_back(static_cast<std::byte>(len));  // MASK=0, 7-bit length
    for (std::size_t i = 0; i < len; ++i) out.push_back(static_cast<std::byte>(0x5Au));
    return out;
}

}  // namespace

int main() {
    using namespace tr::net::ws;
    std::printf("transport_ws RFC 6455 protocol layer:\n");

    // RFC 6455 §1.3 — the canonical Sec-WebSocket-Accept worked example.
    {
        const std::string acc(accept_key("dGhlIHNhbXBsZSBub25jZQ==").view());
        check(acc == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
              "accept_key matches RFC 6455 §1.3 vector (s3pPLMBiTxaQ9kYGzzhZRbK+xOo=)");
    }

    // RFC 6455 §5.7 — single-frame masked "Hello" from a client.
    {
        std::vector<std::byte> masked_hello =
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
        std::vector<std::byte> enc = server_frame_ref(opcode_t::BINARY, payload);
        const std::vector<std::byte> expect = bytes_of({0x82, 0x02, 'H', 'i'});
        check(enc == expect, "server frame (BINARY, \"Hi\") == 82 02 'H' 'i'");

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
        std::vector<std::byte> partial = bytes_of({0x81});
        check(!decode_frame(partial).has_value(), "1-byte buffer returns nullopt (need-more)");
    }

    // 16-bit extended length: 200-byte payload uses the 126 marker + 2-byte len.
    {
        std::vector<std::byte> payload(200, static_cast<std::byte>(0xAB));
        std::vector<std::byte> enc = server_frame_ref(opcode_t::BINARY, payload);
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
            check(std::ranges::equal(dec->first.payload, payload),
                  "  decoded payload round-trips byte-exactly");
            check(dec->second == enc.size(), "  consumes whole buffer");
        }
    }

    // Malformed: a 64-bit-length frame (len marker 127) claiming ~2^64 bytes but with a
    // short buffer. The bounds check must be overflow-safe — `pos + len` would wrap and
    // bypass a naive `buf.size() < pos + len`, causing an OOB read. Must return nullopt,
    // never crash / read past the buffer.
    {
        std::vector<std::byte> evil =
            bytes_of({0x82, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xAA, 0xBB});
        check(!decode_frame(evil).has_value(),
              "64-bit over-long length is rejected (nullopt), no overflow/OOB read");
    }

    // RFC 6455 §5.2 (#1060) — a RESERVED opcode FAILS the connection, on the CHECKED
    // decoder only. Both reserved ranges are driven, because they reach the rule from
    // opposite sides: `0x3`-`0x7` are sorted as DATA by is_control_opcode and so met no
    // decoder rule at all, while `0xB`-`0xF` are control frames a peer can shape perfectly
    // legally (FIN set, <= 125 bytes) and slide straight past §5.5.
    {
        char what[128];
        for (std::uint8_t op = 0x3; op <= 0xF; ++op) {
            if (is_defined_opcode(static_cast<opcode_t>(op))) continue;  // 0x8-0xA
            std::vector<std::byte> f = raw_op_frame(op, 2);

            const decode_result_t r = decode_frame_checked(f, kNoPayloadCap);
            std::snprintf(what, sizeof what,
                          "checked decode of a legal-shaped reserved 0x%X frame is "
                          "PROTOCOL_ERROR",
                          op);
            check(r.status == decode_status_t::PROTOCOL_ERROR, what);

            // The UNCHECKED twin's contract is unchanged: it owns no connection to fail, and
            // `ws_diff_fuzz.py` holds it byte-for-byte against the TypeScript decoder.
            const auto dec = decode_frame(f);
            std::snprintf(what, sizeof what,
                          "  ...and the UNCHECKED decoder still decodes 0x%X unchanged", op);
            check(dec.has_value() && static_cast<std::uint8_t>(dec->first.op) == op &&
                      dec->second == f.size(),
                  what);
        }
    }

    // Off the HEADER: two bytes of a reserved frame announcing an extended length, with not
    // one payload byte present. The verdict is PROTOCOL_ERROR, not NEED_MORE — nothing was
    // buffered and no further byte could have made the opcode legal.
    {
        // FIN | opcode 0x3, then MASK=1 with the 126 extended-length marker — so a decoder
        // that reached the length ladder would need two more bytes and answer NEED_MORE.
        std::vector<std::byte> stub = bytes_of({0x83, 0xFE});
        const decode_result_t r = decode_frame_checked(stub, kNoPayloadCap);
        check(r.status == decode_status_t::PROTOCOL_ERROR,
              "a reserved opcode is diagnosed off the header alone (not NEED_MORE)");
        check(r.consumed == 0 && r.frame.payload.empty(), "  nothing consumed, nothing buffered");
    }

    // The six DEFINED opcodes are untouched by the §5.2 rule: each still decodes, TEXT and
    // PONG included (what a transport then does with those is transport policy, not a decode
    // outcome). This is the guard that stops the new clause being written too wide.
    {
        char what[128];
        static constexpr std::uint8_t kDefined[] = {0x0, 0x1, 0x2, 0x8, 0x9, 0xA};
        for (std::uint8_t op : kDefined) {
            std::vector<std::byte> f = raw_op_frame(op, 2);
            const decode_result_t r = decode_frame_checked(f, kNoPayloadCap);
            std::snprintf(what, sizeof what, "checked decode of DEFINED opcode 0x%X still OK", op);
            check(r.status == decode_status_t::OK && static_cast<std::uint8_t>(r.frame.op) == op &&
                      r.consumed == f.size(),
                  what);
        }
    }

    // ...and the two §5.5 verdicts the new clause sits next to still fire, on a DEFINED
    // control opcode. Ablating the §5.2 clause must leave these green (and ablating §5.5
    // must leave the reserved-opcode cases above green): they are separate rules.
    {
        std::vector<std::byte> oversize = bytes_of({0x89, 0x7E, 0x10, 0x00});  // PING, len 4096
        oversize.resize(4 + 4096, std::byte{0x77});
        check(
            decode_frame_checked(oversize, kNoPayloadCap).status == decode_status_t::PROTOCOL_ERROR,
            "§5.5 still rejects an over-125-byte control frame");
        std::vector<std::byte> nonfinal = raw_op_frame(0x9, 8, /*fin=*/false);
        check(
            decode_frame_checked(nonfinal, kNoPayloadCap).status == decode_status_t::PROTOCOL_ERROR,
            "§5.5 still rejects a non-final control frame");
    }

    // #1922: the word-wide unmask equals the byte-wise RFC 6455 §5.3 reference. The buffer
    // starts at each of the eight offsets from an 8-byte boundary, so the head runs 0..7 bytes
    // and the word loop starts at every one of the four key rotations; the lengths 0..40 cover
    // every length mod 8 with zero, one and several whole words behind each head.
    {
        const std::array<std::uint8_t, 4> key{0x37, 0xFA, 0x21, 0x3D};
        alignas(8) std::array<std::byte, 64> buf{};
        bool all_equal = true;
        bool all_restored = true;
        for (std::size_t offset = 0; offset < 8; ++offset) {
            for (std::size_t len = 0; len <= 40; ++len) {
                const std::span<std::byte> data(buf.data() + offset, len);
                for (std::size_t i = 0; i < len; ++i)
                    data[i] = static_cast<std::byte>((i * 29 + offset * 7 + len) & 0xFFu);
                std::vector<std::byte> want(data.begin(), data.end());
                const std::vector<std::byte> original = want;
                for (std::size_t i = 0; i < len; ++i) want[i] ^= static_cast<std::byte>(key[i % 4]);
                const std::byte before = offset > 0 ? buf[offset - 1] : std::byte{0};
                const std::byte after = buf[offset + len];
                detail::xor_mask(data, key);
                all_equal = all_equal && std::ranges::equal(data, want) &&
                            (offset == 0 || buf[offset - 1] == before) &&
                            buf[offset + len] == after;
                detail::xor_mask(data, key);
                all_restored = all_restored && std::ranges::equal(data, original);
            }
        }
        check(all_equal,
              "word-wide unmask == byte-wise reference, every length mod 8 x every mask offset, "
              "no byte outside the payload touched");
        check(all_restored, "  unmasking twice restores the input");
    }

    // ...and through the decoder: a masked frame whose payload starts at every offset from an
    // 8-byte boundary still decodes to its plaintext.
    {
        std::vector<std::byte> plain(1000);
        for (std::size_t i = 0; i < plain.size(); ++i)
            plain[i] = static_cast<std::byte>((i * 13) & 0xFFu);
        const std::vector<std::byte> frame = client_frame_ref(opcode_t::BINARY, plain, 0xA1B2C3D4u);
        bool all_ok = true;
        for (std::size_t offset = 0; offset < 8; ++offset) {
            std::vector<std::byte> store(frame.size() + 16);
            const std::size_t lead =
                (8u - reinterpret_cast<std::uintptr_t>(store.data()) % 8u) % 8u + offset;
            const std::span<std::byte> buf(store.data() + lead, frame.size());
            std::ranges::copy(frame, buf.begin());
            const decode_result_t r = decode_frame_checked(buf, kNoPayloadCap);
            all_ok = all_ok && r.status == decode_status_t::OK &&
                     std::ranges::equal(r.frame.payload, plain);
        }
        check(all_ok, "masked 1000-byte frame decodes at every buffer offset mod 8");
    }

    return tr::testing::summary("ws");
}
