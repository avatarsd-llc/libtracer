/**
 * @file
 * @brief wt_h3 — the MODULE-PRIVATE minimal HTTP/3 + QPACK codec the WebTransport endpoint needs
 *        (ADR-0043 Phase B), and nothing more.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This header lives under
 * src/ (not include/): it is shared by transport_webtransport.cpp and the
 * webtransport tests, and is NOT public API.
 *
 * The exact subset implemented, and why it suffices:
 *
 *  - QUIC varints (RFC 9000 §16) — every H3 length/type field.
 *  - H3 frame encode/parse for SETTINGS (0x04) and HEADERS (0x01) only
 *    (RFC 9114 §7): the WebTransport session handshake is one SETTINGS
 *    exchange plus one extended-CONNECT HEADERS round-trip; DATA and the
 *    other frame types never appear on the streams we own (unknown frames on
 *    the control stream are skipped by length, per RFC 9114 §9).
 *  - QPACK (RFC 9204) with a ZERO-CAPACITY dynamic table. Neither endpoint
 *    ever sends SETTINGS_QPACK_MAX_TABLE_CAPACITY, so the peer's dynamic
 *    table capacity stays 0 (RFC 9204 §3.2.3 default) and every compliant
 *    encoder — Chrome included — MUST encode the CONNECT headers with static
 *    table references and literals only, with Required Insert Count = 0.
 *    Decoding therefore needs exactly three representations (RFC 9204 §4.5):
 *    Indexed Field Line (static), Literal Field Line With Name Reference
 *    (static), and Literal Field Line With Literal Name. The post-base and
 *    dynamic-indexed forms are impossible with RIC = 0 and are rejected.
 *  - The QPACK static table (RFC 9204 Appendix A) entries 0–28 verbatim —
 *    everything the handshake can reference by index (:authority, :path,
 *    :method, :scheme, :status). Entries 29–98 are accepted but surface as an
 *    anonymous header (empty name): the handshake validation only inspects
 *    :method/:protocol/:status, so a browser referencing, say, `user-agent`
 *    by static index is skipped correctly (its value string is still
 *    consumed) without carrying the whole 99-entry table.
 *  - Huffman DECODING (RFC 7541 Appendix B) — Chrome Huffman-encodes literal
 *    strings whenever that is shorter (`webtransport` is), so the server must
 *    decode it. Our own encoder never emits Huffman (a literal with H=0 is
 *    always legal), so no encoder is needed.
 *
 * This static-table-subset + zero-dynamic-table QPACK is fully conformant for
 * the peer roles we play; a general H3 endpoint would need ls-qpack — the
 * WebTransport handshake does not (ADR-0043 Phase B).
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tr::net::wt_h3 {

// ---- protocol constants (RFC 9114 / RFC 9204 / draft-ietf-webtrans-http3) ----

inline constexpr std::uint64_t kStreamTypeControl = 0x00;       // RFC 9114 §6.2.1
inline constexpr std::uint64_t kStreamTypePush = 0x01;          // RFC 9114 §4.6
inline constexpr std::uint64_t kStreamTypeQpackEncoder = 0x02;  // RFC 9204 §4.2
inline constexpr std::uint64_t kStreamTypeQpackDecoder = 0x03;  // RFC 9204 §4.2
inline constexpr std::uint64_t kStreamTypeWtUni = 0x54;         // WT unidirectional stream

inline constexpr std::uint64_t kFrameData = 0x00;
inline constexpr std::uint64_t kFrameHeaders = 0x01;
inline constexpr std::uint64_t kFrameSettings = 0x04;
/** The signal value opening a WebTransport bidirectional stream (draft §4.2). */
inline constexpr std::uint64_t kFrameWtStream = 0x41;

inline constexpr std::uint64_t kSettingEnableConnectProtocol = 0x08;     // RFC 9220
inline constexpr std::uint64_t kSettingH3Datagram = 0x33;                // RFC 9297
inline constexpr std::uint64_t kSettingEnableWebTransport = 0x2b603742;  // draft-02
inline constexpr std::uint64_t kSettingWtMaxSessions = 0xc671706a;       // draft-07+

// ---- QUIC varint (RFC 9000 §16) ----

/** A decoded varint: its value and the bytes it occupied. */
struct varint_t {
    std::uint64_t value;
    std::size_t consumed;
};

/**
 * @brief A FIXED-CAPACITY byte sink — the encode target for a byte count that is a
 *        protocol constant rather than a peer's number (#934).
 *
 * The handshake preambles this codec emits (the control stream's SETTINGS, the two QPACK
 * stream types, the 200 response) have lengths fixed by the RFCs, so nothing about them
 * needs a growable container. Building them into one of these instead lets the whole
 * sequence be `constexpr`, which is what DELETES the allocation rather than making it
 * failable: the bytes end up in static storage and the LISTEN-side handshake — the
 * pre-auth path an unauthenticated peer reaches with one QUIC handshake — allocates
 * nothing at all to answer.
 *
 * It is a `push_back` sink on purpose, so the appenders below take it and a
 * `std::vector` through the SAME code (the DIAL-side request, whose length depends on a
 * configured `:authority`/`:path`, keeps its vector).
 *
 * @warning Writing past @p N is undefined; every builder here is checked by a
 *          `static_assert` on the resulting length.
 */
template <std::size_t N>
struct fixed_bytes_t {
    std::array<std::uint8_t, N> data{}; /**< @brief The storage. */
    std::size_t len = 0;                /**< @brief Bytes written, in [0, N]. */

    /** @brief Append one byte. */
    constexpr void push_back(std::uint8_t b) { data[len++] = b; }

    /** @brief The bytes written so far. */
    [[nodiscard]] constexpr std::span<const std::uint8_t> bytes() const {
        return std::span<const std::uint8_t>(data.data(), len);
    }
};

/** Decode one QUIC varint; nullopt = the buffer does not yet hold all its bytes. */
inline std::optional<varint_t> read_varint(std::span<const std::uint8_t> in) {
    if (in.empty()) return std::nullopt;
    const std::size_t len = static_cast<std::size_t>(1) << (in[0] >> 6);
    if (in.size() < len) return std::nullopt;
    std::uint64_t v = in[0] & 0x3f;
    for (std::size_t i = 1; i < len; ++i) v = (v << 8) | in[i];
    return varint_t{v, len};
}

/** Append @p v as a QUIC varint (shortest encoding) to any `push_back` byte sink —
 *  a `std::vector` (the DIAL request) or a @ref fixed_bytes_t (the constant preambles). */
template <class Out>
constexpr void append_varint(Out& out, std::uint64_t v) {
    if (v < 0x40) {
        out.push_back(static_cast<std::uint8_t>(v));
    } else if (v < 0x4000) {
        out.push_back(static_cast<std::uint8_t>(0x40 | (v >> 8)));
        out.push_back(static_cast<std::uint8_t>(v));
    } else if (v < 0x40000000) {
        out.push_back(static_cast<std::uint8_t>(0x80 | (v >> 24)));
        out.push_back(static_cast<std::uint8_t>(v >> 16));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
        out.push_back(static_cast<std::uint8_t>(v));
    } else {
        out.push_back(static_cast<std::uint8_t>(0xc0 | (v >> 56)));
        for (int s = 48; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>(v >> s));
    }
}

// ---- Huffman decoding (RFC 7541 Appendix B — the HPACK/QPACK code) ----

namespace detail {

/** One HPACK Huffman code: the canonical code bits and their count. */
struct huff_code_t {
    std::uint32_t code;
    std::uint8_t bits;
};

/** RFC 7541 Appendix B, symbols 0..255 (EOS handled as padding, not a symbol). */
inline constexpr huff_code_t kHuff[256] = {
    {0x1ff8, 13},     {0x7fffd8, 23},  {0xfffffe2, 28},  {0xfffffe3, 28},  {0xfffffe4, 28},
    {0xfffffe5, 28},  {0xfffffe6, 28}, {0xfffffe7, 28},  {0xfffffe8, 28},  {0xffffea, 24},
    {0x3ffffffc, 30}, {0xfffffe9, 28}, {0xfffffea, 28},  {0x3ffffffd, 30}, {0xfffffeb, 28},
    {0xfffffec, 28},  {0xfffffed, 28}, {0xfffffee, 28},  {0xfffffef, 28},  {0xffffff0, 28},
    {0xffffff1, 28},  {0xffffff2, 28}, {0x3ffffffe, 30}, {0xffffff3, 28},  {0xffffff4, 28},
    {0xffffff5, 28},  {0xffffff6, 28}, {0xffffff7, 28},  {0xffffff8, 28},  {0xffffff9, 28},
    {0xffffffa, 28},  {0xffffffb, 28}, {0x14, 6},        {0x3f8, 10},      {0x3f9, 10},
    {0xffa, 12},      {0x1ff9, 13},    {0x15, 6},        {0xf8, 8},        {0x7fa, 11},
    {0x3fa, 10},      {0x3fb, 10},     {0xf9, 8},        {0x7fb, 11},      {0xfa, 8},
    {0x16, 6},        {0x17, 6},       {0x18, 6},        {0x0, 5},         {0x1, 5},
    {0x2, 5},         {0x19, 6},       {0x1a, 6},        {0x1b, 6},        {0x1c, 6},
    {0x1d, 6},        {0x1e, 6},       {0x1f, 6},        {0x5c, 7},        {0xfb, 8},
    {0x7ffc, 15},     {0x20, 6},       {0xffb, 12},      {0x3fc, 10},      {0x1ffa, 13},
    {0x21, 6},        {0x5d, 7},       {0x5e, 7},        {0x5f, 7},        {0x60, 7},
    {0x61, 7},        {0x62, 7},       {0x63, 7},        {0x64, 7},        {0x65, 7},
    {0x66, 7},        {0x67, 7},       {0x68, 7},        {0x69, 7},        {0x6a, 7},
    {0x6b, 7},        {0x6c, 7},       {0x6d, 7},        {0x6e, 7},        {0x6f, 7},
    {0x70, 7},        {0x71, 7},       {0x72, 7},        {0xfc, 8},        {0x73, 7},
    {0xfd, 8},        {0x1ffb, 13},    {0x7fff0, 19},    {0x1ffc, 13},     {0x3ffc, 14},
    {0x22, 6},        {0x7ffd, 15},    {0x3, 5},         {0x23, 6},        {0x4, 5},
    {0x24, 6},        {0x5, 5},        {0x25, 6},        {0x26, 6},        {0x27, 6},
    {0x6, 5},         {0x74, 7},       {0x75, 7},        {0x28, 6},        {0x29, 6},
    {0x2a, 6},        {0x7, 5},        {0x2b, 6},        {0x76, 7},        {0x2c, 6},
    {0x8, 5},         {0x9, 5},        {0x2d, 6},        {0x77, 7},        {0x78, 7},
    {0x79, 7},        {0x7a, 7},       {0x7b, 7},        {0x7ffe, 15},     {0x7fc, 11},
    {0x3ffd, 14},     {0x1ffd, 13},    {0xffffffc, 28},  {0xfffe6, 20},    {0x3fffd2, 22},
    {0xfffe7, 20},    {0xfffe8, 20},   {0x3fffd3, 22},   {0x3fffd4, 22},   {0x3fffd5, 22},
    {0x7fffd9, 23},   {0x3fffd6, 22},  {0x7fffda, 23},   {0x7fffdb, 23},   {0x7fffdc, 23},
    {0x7fffdd, 23},   {0x7fffde, 23},  {0xffffeb, 24},   {0x7fffdf, 23},   {0xffffec, 24},
    {0xffffed, 24},   {0x3fffd7, 22},  {0x7fffe0, 23},   {0xffffee, 24},   {0x7fffe1, 23},
    {0x7fffe2, 23},   {0x7fffe3, 23},  {0x7fffe4, 23},   {0x1fffdc, 21},   {0x3fffd8, 22},
    {0x7fffe5, 23},   {0x3fffd9, 22},  {0x7fffe6, 23},   {0x7fffe7, 23},   {0xffffef, 24},
    {0x3fffda, 22},   {0x1fffdd, 21},  {0xfffe9, 20},    {0x3fffdb, 22},   {0x3fffdc, 22},
    {0x7fffe8, 23},   {0x7fffe9, 23},  {0x1fffde, 21},   {0x7fffea, 23},   {0x3fffdd, 22},
    {0x3fffde, 22},   {0xfffff0, 24},  {0x1fffdf, 21},   {0x3fffdf, 22},   {0x7fffeb, 23},
    {0x7fffec, 23},   {0x1fffe0, 21},  {0x1fffe1, 21},   {0x3fffe0, 22},   {0x1fffe2, 21},
    {0x7fffed, 23},   {0x3fffe1, 22},  {0x7fffee, 23},   {0x7fffef, 23},   {0xfffea, 20},
    {0x3fffe2, 22},   {0x3fffe3, 22},  {0x3fffe4, 22},   {0x7ffff0, 23},   {0x3fffe5, 22},
    {0x3fffe6, 22},   {0x7ffff1, 23},  {0x3ffffe0, 26},  {0x3ffffe1, 26},  {0xfffeb, 20},
    {0x7fff1, 19},    {0x3fffe7, 22},  {0x7ffff2, 23},   {0x3fffe8, 22},   {0x1ffffec, 25},
    {0x3ffffe2, 26},  {0x3ffffe3, 26}, {0x3ffffe4, 26},  {0x7ffffde, 27},  {0x7ffffdf, 27},
    {0x3ffffe5, 26},  {0xfffff1, 24},  {0x1ffffed, 25},  {0x7fff2, 19},    {0x1fffe3, 21},
    {0x3ffffe6, 26},  {0x7ffffe0, 27}, {0x7ffffe1, 27},  {0x3ffffe7, 26},  {0x7ffffe2, 27},
    {0xfffff2, 24},   {0x1fffe4, 21},  {0x1fffe5, 21},   {0x3ffffe8, 26},  {0x3ffffe9, 26},
    {0xffffffd, 28},  {0x7ffffe3, 27}, {0x7ffffe4, 27},  {0x7ffffe5, 27},  {0xfffec, 20},
    {0xfffff3, 24},   {0xfffed, 20},   {0x1fffe6, 21},   {0x3fffe9, 22},   {0x1fffe7, 21},
    {0x1fffe8, 21},   {0x7ffff3, 23},  {0x3fffea, 22},   {0x3fffeb, 22},   {0x1ffffee, 25},
    {0x1ffffef, 25},  {0xfffff4, 24},  {0xfffff5, 24},   {0x3ffffea, 26},  {0x7ffff4, 23},
    {0x3ffffeb, 26},  {0x7ffffe6, 27}, {0x3ffffec, 26},  {0x3ffffed, 26},  {0x7ffffe7, 27},
    {0x7ffffe8, 27},  {0x7ffffe9, 27}, {0x7ffffea, 27},  {0x7ffffeb, 27},  {0xffffffe, 28},
    {0x7ffffec, 27},  {0x7ffffed, 27}, {0x7ffffee, 27},  {0x7ffffef, 27},  {0x7fffff0, 27},
    {0x3ffffee, 26},
};

}  // namespace detail

/**
 * @brief Decode an RFC 7541 Huffman-coded string into CALLER-OWNED storage.
 *
 * Allocates nothing: the decoded bytes land in @p out, which the caller sizes.
 * Returns the number of bytes written, or nullopt on an invalid code, invalid
 * padding (padding must be a strictly-shorter-than-8-bit prefix of EOS, i.e.
 * all ones), or an @p out too small to hold the result — the last is the
 * peer-provoked case, and refusing it is the point (#1305).
 */
inline std::optional<std::size_t> huffman_decode_into(std::span<const std::uint8_t> in,
                                                      std::span<char> out) {
    std::size_t written = 0;
    std::uint32_t cur = 0;
    std::uint8_t nbits = 0;
    for (const std::uint8_t byte : in) {
        for (int bit = 7; bit >= 0; --bit) {
            cur = (cur << 1) | ((byte >> bit) & 1u);
            ++nbits;
            if (nbits > 30) return std::nullopt;  // longer than any code
            // Linear scan is fine: handshake header strings are tiny.
            for (int sym = 0; sym < 256; ++sym) {
                if (detail::kHuff[sym].bits == nbits && detail::kHuff[sym].code == cur) {
                    if (written == out.size()) return std::nullopt;  // storage exhausted
                    out[written++] = static_cast<char>(sym);
                    cur = 0;
                    nbits = 0;
                    break;
                }
            }
        }
    }
    // Trailing padding: fewer than 8 bits, all ones (a prefix of EOS).
    if (nbits >= 8) return std::nullopt;
    if (cur != (1u << nbits) - 1u) return std::nullopt;
    return written;
}

/**
 * @brief The owning convenience wrapper over huffman_decode_into.
 *
 * Sized for the widest possible expansion (the shortest code is 5 bits, so at
 * most 8/5 bytes out per byte in) and therefore never refuses for storage. Used
 * where an owned string is wanted and the input is not peer-provoked — the
 * decoder proper goes through huffman_decode_into into fixed scratch.
 */
inline std::optional<std::string> huffman_decode(std::span<const std::uint8_t> in) {
    std::string out((in.size() * 8) / 5 + 1, '\0');
    const auto written = huffman_decode_into(in, std::span<char>(out.data(), out.size()));
    if (!written) return std::nullopt;
    out.resize(*written);
    return out;
}

// ---- QPACK static-table subset + prefixed integers (RFC 9204 / RFC 7541 §5.1) ----

/** One decoded header field, as NON-OWNING views (#1305): into the constexpr
 *  static table, into the caller's input buffer, or into the decode store's
 *  Huffman scratch — never onto the heap. An EMPTY name marks a static-table
 *  entry beyond index 28 — a header the handshake validation ignores (see the
 *  file header). */
struct header_t {
    std::string_view name;
    std::string_view value;
};

namespace detail {

/** RFC 9204 Appendix A entries 0..28 — the handshake-relevant static table. */
struct static_entry_t {
    std::string_view name;
    std::string_view value;
};
inline constexpr static_entry_t kStatic[29] = {
    {":authority", ""},
    {":path", "/"},
    {"age", "0"},
    {"content-disposition", ""},
    {"content-length", "0"},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"referer", ""},
    {"set-cookie", ""},
    {":method", "CONNECT"},
    {":method", "DELETE"},
    {":method", "GET"},
    {":method", "HEAD"},
    {":method", "OPTIONS"},
    {":method", "POST"},
    {":method", "PUT"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "103"},
    {":status", "200"},
    {":status", "304"},
    {":status", "404"},
    {":status", "503"},
};
inline constexpr std::size_t kStaticTableSize = 99;  // RFC 9204 Appendix A total

/** Decode an RFC 7541 §5.1 prefixed integer whose first byte is in[0] with
 *  @p prefix_bits of value. nullopt = truncated or absurdly large. */
inline std::optional<varint_t> read_prefixed_int(std::span<const std::uint8_t> in,
                                                 unsigned prefix_bits) {
    if (in.empty()) return std::nullopt;
    const std::uint64_t mask = (1u << prefix_bits) - 1u;
    std::uint64_t v = in[0] & mask;
    if (v < mask) return varint_t{v, 1};
    std::size_t i = 1;
    unsigned shift = 0;
    while (true) {
        if (i >= in.size() || shift > 56) return std::nullopt;
        const std::uint8_t b = in[i++];
        v += static_cast<std::uint64_t>(b & 0x7f) << shift;
        shift += 7;
        if ((b & 0x80) == 0) return varint_t{v, i};
    }
}

/** Append an RFC 7541 §5.1 prefixed integer, OR-ing @p flags into the first byte. */
inline void append_prefixed_int(std::vector<std::uint8_t>& out, std::uint64_t v,
                                unsigned prefix_bits, std::uint8_t flags) {
    const std::uint64_t mask = (1u << prefix_bits) - 1u;
    if (v < mask) {
        out.push_back(static_cast<std::uint8_t>(flags | v));
        return;
    }
    out.push_back(static_cast<std::uint8_t>(flags | mask));
    v -= mask;
    while (v >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(0x80 | (v & 0x7f)));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

/** Read one QPACK string literal as a VIEW: H flag @p huffman, then the
 *  already-decoded length @p len of raw bytes at @p in. A raw literal views @p
 *  in directly; a Huffman one decodes into @p scratch from @p used onward (and
 *  advances @p used). nullopt = truncated, bad Huffman, or scratch exhausted. */
inline std::optional<std::string_view> read_string_body(std::span<const std::uint8_t> in,
                                                        std::uint64_t len, bool huffman,
                                                        std::span<char> scratch,
                                                        std::size_t& used) {
    if (in.size() < len) return std::nullopt;
    const auto body = in.first(static_cast<std::size_t>(len));
    if (!huffman) return std::string_view(reinterpret_cast<const char*>(body.data()), body.size());
    const auto written = huffman_decode_into(body, scratch.subspan(used));
    if (!written) return std::nullopt;
    const std::string_view out(scratch.data() + used, *written);
    used += *written;
    return out;
}

}  // namespace detail

/**
 * @brief The most field lines one encoded field section may carry (#1305).
 *
 * The input cap on a field section is in BYTES (kMaxHandshakeBytes, 16 KiB) but
 * the decoder's cost is per ELEMENT: the cheapest representation is a one-byte
 * Indexed Field Line, so a byte cap alone lets an UNAUTHENTICATED peer turn
 * 16 KiB into ~16 400 elements. Only an element-count bound closes that gap.
 * The extended-CONNECT handshake needs five field lines (`:method`,
 * `:protocol`, `:scheme`, `:authority`, `:path`) and the 200 response one, so
 * 32 is generous for every section this codec is ever asked to decode.
 */
inline constexpr std::size_t kMaxFieldSectionHeaders = 32;

/**
 * @brief The Huffman-decoded literal bytes one field section may carry (#1305).
 *
 * Only Huffman-coded literals need storage of their own — raw literals are
 * viewed in place. 4 KiB is far past what an extended CONNECT's `:authority`,
 * `:path` and `:protocol` can legitimately need, and a section wanting more is
 * refused like any other malformed one.
 */
inline constexpr std::size_t kMaxFieldSectionScratch = 4096;

/**
 * @brief The caller-owned storage one field-section decode borrows (#1305).
 *
 * decode_field_section allocates NOTHING: the field lines land in this fixed
 * array and the Huffman-decoded literals in this fixed scratch, so a peer's
 * 16 KiB field section can no longer amplify into heap. The decoded headers are
 * views — into the constexpr static table, into the caller's INPUT buffer, or
 * into `scratch` — so both this store and the input buffer must outlive every
 * read of the result, and the input must not be mutated meanwhile.
 */
struct field_section_t {
    std::array<header_t, kMaxFieldSectionHeaders> headers; /**< Decoded lines, [0, count). */
    std::size_t count = 0;                                 /**< Field lines decoded. */
    std::array<char, kMaxFieldSectionScratch> scratch;     /**< Huffman-decoded literal bytes. */
    std::size_t scratch_used = 0;                          /**< Bytes of `scratch` taken. */
};

/**
 * Decode a complete QPACK encoded field section into @p store under the
 * zero-dynamic-table contract (see the file header): Required Insert Count MUST
 * be 0 and every representation must be static/literal. The result views @p in
 * and @p store, and owns nothing. nullopt = malformed, out of subset, more than
 * kMaxFieldSectionHeaders field lines, or more than kMaxFieldSectionScratch
 * bytes of Huffman literal — all of them refusals the caller scopes to the
 * stream.
 */
[[nodiscard]] inline std::optional<std::span<const header_t>> decode_field_section(
    std::span<const std::uint8_t> in, field_section_t& store) {
    store.count = 0;
    store.scratch_used = 0;
    const std::span<char> scratch(store.scratch);
    const auto emit = [&store](std::string_view name, std::string_view value) {
        store.headers[store.count++] = header_t{name, value};
    };

    // Encoded Field Section Prefix: Required Insert Count (8-bit prefix) +
    // S bit / Delta Base (7-bit prefix). With RIC = 0 the base is 0 too.
    const auto ric = detail::read_prefixed_int(in, 8);
    if (!ric || ric->value != 0) return std::nullopt;  // dynamic table use — out of subset
    in = in.subspan(ric->consumed);
    const auto base = detail::read_prefixed_int(in, 7);
    if (!base) return std::nullopt;
    in = in.subspan(base->consumed);

    while (!in.empty()) {
        // Every arm below emits exactly one field line, so the ceiling is
        // enforced once, here, before any of them can consume storage.
        if (store.count >= kMaxFieldSectionHeaders) return std::nullopt;
        const std::uint8_t b = in[0];
        if ((b & 0x80) != 0) {
            // Indexed Field Line: 1 T IIIIII — static only (T=1).
            if ((b & 0x40) == 0) return std::nullopt;
            const auto idx = detail::read_prefixed_int(in, 6);
            if (!idx || idx->value >= detail::kStaticTableSize) return std::nullopt;
            in = in.subspan(idx->consumed);
            if (idx->value < 29) {
                const auto& e = detail::kStatic[idx->value];
                emit(e.name, e.value);
            } else {
                emit({}, {});  // ignored header (name intentionally empty)
            }
        } else if ((b & 0xc0) == 0x40) {
            // Literal Field Line With Name Reference: 01 N T IIII — static only.
            if ((b & 0x10) == 0) return std::nullopt;
            const auto idx = detail::read_prefixed_int(in, 4);
            if (!idx || idx->value >= detail::kStaticTableSize) return std::nullopt;
            in = in.subspan(idx->consumed);
            if (in.empty()) return std::nullopt;
            const bool h = (in[0] & 0x80) != 0;
            const auto vlen = detail::read_prefixed_int(in, 7);
            if (!vlen) return std::nullopt;
            in = in.subspan(vlen->consumed);
            const auto value =
                detail::read_string_body(in, vlen->value, h, scratch, store.scratch_used);
            if (!value) return std::nullopt;
            in = in.subspan(static_cast<std::size_t>(vlen->value));
            std::string_view name;
            if (idx->value < 29) name = detail::kStatic[idx->value].name;
            emit(name, *value);
        } else if ((b & 0xe0) == 0x20) {
            // Literal Field Line With Literal Name: 001 N H NNN.
            const bool name_h = (b & 0x08) != 0;
            const auto nlen = detail::read_prefixed_int(in, 3);
            if (!nlen) return std::nullopt;
            in = in.subspan(nlen->consumed);
            const auto name =
                detail::read_string_body(in, nlen->value, name_h, scratch, store.scratch_used);
            if (!name) return std::nullopt;
            in = in.subspan(static_cast<std::size_t>(nlen->value));
            if (in.empty()) return std::nullopt;
            const bool value_h = (in[0] & 0x80) != 0;
            const auto vlen = detail::read_prefixed_int(in, 7);
            if (!vlen) return std::nullopt;
            in = in.subspan(vlen->consumed);
            const auto value =
                detail::read_string_body(in, vlen->value, value_h, scratch, store.scratch_used);
            if (!value) return std::nullopt;
            in = in.subspan(static_cast<std::size_t>(vlen->value));
            emit(*name, *value);
        } else {
            // 0000/0001 = post-base forms: impossible with RIC = 0.
            return std::nullopt;
        }
    }
    return std::span<const header_t>(store.headers.data(), store.count);
}

// ---- handshake byte builders ----

/** Append one H3 frame: varint type, varint length, payload — to any `push_back`
 *  byte sink (see @ref append_varint). */
template <class Out>
constexpr void append_h3_frame(Out& out, std::uint64_t type,
                               std::span<const std::uint8_t> payload) {
    append_varint(out, type);
    append_varint(out, payload.size());
    for (const std::uint8_t b : payload) out.push_back(b);
}

namespace detail {

/**
 * @brief Build the control-stream preamble at COMPILE time (see @ref control_stream_bytes).
 *
 * The two scratch capacities are generous upper bounds on encodings whose real lengths the
 * `static_assert`s below pin exactly; nothing here is peer-influenced.
 */
[[nodiscard]] constexpr fixed_bytes_t<32> build_control_stream() {
    fixed_bytes_t<24> settings;
    append_varint(settings, kSettingEnableConnectProtocol);
    append_varint(settings, 1);
    append_varint(settings, kSettingH3Datagram);
    append_varint(settings, 1);
    append_varint(settings, kSettingEnableWebTransport);
    append_varint(settings, 1);
    append_varint(settings, kSettingWtMaxSessions);
    append_varint(settings, 1);
    fixed_bytes_t<32> out;
    append_varint(out, kStreamTypeControl);
    append_h3_frame(out, kFrameSettings, settings.bytes());
    return out;
}

/** @brief The control-stream preamble, in static storage (#934). */
inline constexpr fixed_bytes_t<32> kControlStream = build_control_stream();
static_assert(kControlStream.len == 21, "the SETTINGS preamble is a 21-byte protocol constant");

}  // namespace detail

/**
 * The complete bytes an endpoint writes on its control stream: the stream type
 * (0x00) followed by ONE SETTINGS frame advertising the WebTransport-enabling
 * settings (extended CONNECT per RFC 9220, H3 datagrams per RFC 9297, and both
 * the draft-02 ENABLE_WEBTRANSPORT and draft-07+ WT_MAX_SESSIONS ids — an
 * endpoint of either vintage finds the one it knows; unknown ids are ignored).
 *
 * A view of static storage, computed at compile time: the LISTEN side opens its H3 face
 * from an msquic connection callback the moment an UNAUTHENTICATED peer completes the
 * QUIC handshake, and that path must not be able to allocate (#934).
 */
[[nodiscard]] inline constexpr std::span<const std::uint8_t> control_stream_bytes() {
    return detail::kControlStream.bytes();
}

/** @brief The QPACK encoder stream's whole preamble — its type varint and nothing else
 *         (RFC 9204 §4.2: the dynamic table stays at capacity 0). */
inline constexpr std::array<std::uint8_t, 1> kQpackEncoderStreamBytes{
    static_cast<std::uint8_t>(kStreamTypeQpackEncoder)};
/** @brief The QPACK decoder stream's whole preamble (see @ref kQpackEncoderStreamBytes). */
inline constexpr std::array<std::uint8_t, 1> kQpackDecoderStreamBytes{
    static_cast<std::uint8_t>(kStreamTypeQpackDecoder)};
static_assert(kStreamTypeQpackEncoder < 0x40 && kStreamTypeQpackDecoder < 0x40,
              "both QPACK stream types encode as a ONE-byte QUIC varint");

namespace detail {

/** Append a QPACK string literal with H=0 (never Huffman on our encode side). */
inline void append_string(std::vector<std::uint8_t>& out, std::string_view s) {
    append_prefixed_int(out, s.size(), 7, 0x00);
    out.insert(out.end(), s.begin(), s.end());
}

}  // namespace detail

/**
 * The QPACK encoded field section of the extended CONNECT request
 * (`:method=CONNECT, :protocol=webtransport, :scheme=https, :authority, :path`)
 * — static references where the table has the pair, literals (H=0) elsewhere.
 */
inline std::vector<std::uint8_t> encode_connect_field_section(std::string_view authority,
                                                              std::string_view path) {
    std::vector<std::uint8_t> out{0x00, 0x00};  // RIC=0, base=0 (no dynamic table)
    out.push_back(0xc0 | 15);                   // :method: CONNECT   (static 15)
    out.push_back(0xc0 | 23);                   // :scheme: https     (static 23)
    // :authority — literal with static name reference 0.
    detail::append_prefixed_int(out, 0, 4, 0x50);
    detail::append_string(out, authority);
    if (path == "/") {
        out.push_back(0xc0 | 1);  // :path: /  (static 1)
    } else {
        detail::append_prefixed_int(out, 1, 4, 0x50);  // :path name ref (static 1)
        detail::append_string(out, path);
    }
    // :protocol: webtransport — literal name (not in the static table).
    detail::append_prefixed_int(out, sizeof(":protocol") - 1, 3, 0x20);
    constexpr std::string_view proto_name = ":protocol";
    out.insert(out.end(), proto_name.begin(), proto_name.end());
    detail::append_string(out, "webtransport");
    return out;
}

/** The QPACK encoded field section of the `200` CONNECT response — a 3-byte protocol
 *  constant, so it owns fixed storage rather than a vector (#934). */
[[nodiscard]] inline constexpr std::array<std::uint8_t, 3> encode_status_200_field_section() {
    return {0x00, 0x00, static_cast<std::uint8_t>(0xc0 | 25)};  // :status: 200 (static 25)
}

namespace detail {

/** @brief Build the whole 200-response HEADERS frame at COMPILE time (see
 *         @ref status_200_headers_frame). */
[[nodiscard]] constexpr fixed_bytes_t<8> build_status_200_headers_frame() {
    fixed_bytes_t<8> out;
    const std::array<std::uint8_t, 3> section = encode_status_200_field_section();
    append_h3_frame(out, kFrameHeaders, std::span<const std::uint8_t>(section));
    return out;
}

/** @brief The 200-response HEADERS frame, in static storage (#934). */
inline constexpr fixed_bytes_t<8> kStatus200Frame = build_status_200_headers_frame();
static_assert(kStatus200Frame.len == 5, "the 200 response is a 5-byte protocol constant");

}  // namespace detail

/**
 * The COMPLETE HEADERS frame answering an accepted extended CONNECT: `0x01` (HEADERS),
 * its length, and the 3-byte `:status: 200` field section.
 *
 * A view of static storage, computed at compile time. The LISTEN side writes it from a
 * stream callback on the strength of ONE unauthenticated peer's HEADERS frame, so the
 * answer itself must cost no allocation (#934); the only heap left on that path is the
 * one copy msquic owns until SEND_COMPLETE, and that one is failable
 * (`send_ctx_t::make_raw`).
 */
[[nodiscard]] inline constexpr std::span<const std::uint8_t> status_200_headers_frame() {
    return detail::kStatus200Frame.bytes();
}

}  // namespace tr::net::wt_h3
