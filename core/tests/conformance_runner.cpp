/**
 * @file
 * @brief Conformance harness for the seed vectors under tests/conformance/vectors/v1/.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * No JSON parser: input.bin is self-describing, so the codec is validated by
 *   (1) generic roundtrip  — encode(decode(input.bin)) == input.bin, for every vector;
 *   (2) golden builders     — encode(built) == input.bin && decode(input.bin) == built;
 *   (3) targeted asserts     — the CRC value, the PATH child count, reserved-bit rejection;
 *   (4) negative vectors    — decode(reject.bin) MUST fail with the error named by
 *       expected.json's "reject" field (extracted by a tiny scan, no JSON parser);
 *   (5) input legality       — every input.bin is a legal frame (packed PATH bodies, label
 *       elements, the FWD head) unless expected.json declares "malformed_input": true (#1587);
 *   (6) noise transcripts    — on a build with a Noise crypto backend, every
 *       noise/<case>/transcript.json is replayed and each recorded value compared (#2072).
 * expected.json stays as the human-readable / cross-language spec.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/op_resolve.hpp"
#include "libtracer/packed_path.hpp"
#include "libtracer/path_label.hpp"
#include "libtracer/path_pair.hpp"
#include "libtracer/path_ref.hpp"
#include "libtracer/tlv_emit.hpp"
#if defined(LIBTRACER_NOISE_CRYPTO_OPENSSL) || defined(LIBTRACER_NOISE_CRYPTO_SODIUM) || \
    defined(LIBTRACER_NOISE_CRYPTO_PSA)
#include "libtracer/security_noise.hpp"
#endif
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "tlv_tree.hpp"  // host-only owning tree (#1829)

namespace {

namespace fs = std::filesystem;
using tr::wire::tlv_t;
using tr::wire::type_t;

using tr::testing::check;

// --- hex + error helpers (used by the --roundtrip differential-fuzz mode) ----
/**
 * @brief The four decode outcomes, mapped to the stable conformance ERR:<name> strings.
 *
 * decode only ever yields these; any other err_t (schema/flow/…) is UNKNOWN here.
 */
const char* error_name(tr::wire::err_t e) noexcept {
    switch (e) {
        case tr::wire::err_t::FRAME_TRUNCATED:
            return "FRAME_TRUNCATED";
        case tr::wire::err_t::FRAME_INVALID:
            return "FRAME_INVALID";
        case tr::wire::err_t::FRAME_CRC_FAIL:
            return "FRAME_CRC_FAIL";
        case tr::wire::err_t::TLV_NESTING_TOO_DEEP:
            return "TLV_NESTING_TOO_DEEP";
        default:
            return "UNKNOWN";
    }
}

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

/**
 * @brief Read one hex frame per stdin line; for each, print decode->encode re-encoded as hex, or
 *        `ERR:<reason>` if it fails to decode.
 *
 * One output line per input line —
 * the differential-fuzz driver (tests/conformance/diff_fuzz.py) compares these
 * against the TS core and the canonical generator output, byte-for-byte.
 */
int run_roundtrip() {
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
        const auto dec = tr::wire::decode(*bytes);
        if (!dec) {
            std::printf("ERR:%s\n", error_name(dec.error()));
            continue;
        }
        const std::vector<std::byte> re = tr::wire::encode(*dec);
        std::printf("%s\n", to_hex(re).c_str());
    }
    return 0;
}

/**
 * @brief The expected decode-error name for a negative case: the value of expected.json's top-level
 *        "reject" field (tiny scan-to-quote, mirroring the Rust vector tests' json_str helper — the
 *        field is machine-written pure ASCII, no escapes).
 */
std::optional<std::string> reject_expectation(const fs::path& case_dir) {
    std::ifstream f(case_dir / "expected.json");
    if (!f) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::size_t key = text.find("\"reject\"");
    if (key == std::string::npos) return std::nullopt;
    const std::size_t colon = text.find(':', key + 8);
    if (colon == std::string::npos) return std::nullopt;
    const std::size_t q1 = text.find('"', colon + 1);
    if (q1 == std::string::npos) return std::nullopt;
    const std::size_t q2 = text.find('"', q1 + 1);
    if (q2 == std::string::npos) return std::nullopt;
    return text.substr(q1 + 1, q2 - q1 - 1);
}

/** @brief One negative case: decode(reject.bin) MUST fail with exactly the expected error. */
bool check_reject(const fs::path& reject_bin, std::span<const std::byte> bytes) {
    const auto want = reject_expectation(reject_bin.parent_path());
    if (!want) return false;  // a reject.bin without a "reject" expectation is malformed
    const auto dec = tr::wire::decode(bytes);
    return !dec.has_value() && *want == error_name(dec.error());
}

// --- input.bin legality pre-pass (#1587) ------------------------------------

/**
 * @brief The `expected.json` manifest key a deliberately-illegal `input.bin` declares.
 *
 * The round-trip contract is satisfied by ANY well-formed TLV, so a vector can bank a frame
 * no conformant origin emits and still score `ok` on every core (`fwd/fwd-label-mint-reply`
 * did, until a99d362f). The legality pre-pass closes that: every `input.bin` must also be a
 * frame the reference would accept as legal, unless its manifest says `"malformed_input":
 * true` — the vectors whose whole point is an address a hop MUST refuse but can still carry.
 */
constexpr std::string_view kMalformedInputKey = "malformed_input";

/**
 * @brief Whether @p case_dir's `expected.json` declares `"malformed_input": true` (tiny scan,
 *        the same no-JSON-parser shape as @ref reject_expectation).
 */
bool malformed_input_declared(const fs::path& case_dir) {
    std::ifstream f(case_dir / "expected.json");
    if (!f) return false;
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::string quoted = "\"" + std::string(kMalformedInputKey) + "\"";
    const std::size_t key = text.find(quoted);
    if (key == std::string::npos) return false;
    std::size_t at = text.find(':', key + quoted.size());
    if (at == std::string::npos) return false;
    at = text.find_first_not_of(" \t\r\n", at + 1);
    return at != std::string::npos && text.compare(at, 4, "true") == 0;
}

/**
 * @brief Why a packed `PATH` body is not a legal frame-path address, or `nullopt` if it is.
 *
 * Frame-path context (RFC-0018 §5.4 Amendment 1), the permissive one: escape records are
 * admissible and a foreign `kind` is stepped over by its declared length. What is NOT
 * admissible anywhere is a body that does not tile into records, and a `kind = 0x16` element
 * that is neither an RFC-0029 PAIR (`len = 8`) nor a label inside RFC-0027 §5.3.2's two
 * structural clauses with a non-reserved generation (§4.1).
 */
std::optional<std::string> packed_path_illegal(std::span<const std::byte> body) {
    std::size_t at = 0;
    while (at < body.size()) {
        const std::size_t span = tr::wire::packed_record_span(body, at);
        if (span == 0) return "PATH body does not tile into packed records (RFC-0018)";
        const auto kind = tr::wire::packed_escape_kind(body, at);
        if (kind == tr::wire::kPackedEscapeKindLabel && !tr::wire::path_label_at(body, at) &&
            !tr::wire::path_pair_at(body, at))
            return "PATH carries a malformed `0x16` element: neither a label (RFC-0027 §5.3.2 / "
                   "§4.1) nor a PAIR (RFC-0029 §5.1)";
        at += span;
    }
    return std::nullopt;
}

/**
 * @brief Why a `FWD` is not a legal frame, or `nullopt` if it is — RFC-0004 §B's child order
 *        `{ op, dst, [FIELD], src, [kind] … }`, with a defined opcode and `kind ∈ {RESULT,
 *        ERROR}` REQUIRED on a `REPLY`.
 *
 * Deliberately the HEAD only: what follows `src` (payload, `await_timeout`, the reverse list)
 * is op- and flag-dependent and already parsed by the terminus; the head is what every
 * conformant origin must spell, and the slot the a99d362f defect dropped.
 */
std::optional<std::string> fwd_illegal(const tlv_t& t) {
    if (!t.opt.pl) return "FWD is not structured (RFC-0004 §B: opt.PL = 1)";
    const auto& ch = t.children;
    const auto is_u8_value = [](const tlv_t& c) {
        return c.type == type_t::VALUE && !c.opt.pl && c.payload.size() == 1;
    };
    if (ch.empty() || !is_u8_value(ch[0])) return "FWD's first child is not a u8 VALUE op";
    const auto opcode = static_cast<std::uint8_t>(std::to_integer<std::uint8_t>(ch[0].payload[0]) &
                                                  tr::graph::kFwdOpcodeMask);
    if (opcode > static_cast<std::uint8_t>(tr::graph::fwd_op_t::REPLY))
        return "FWD op names no defined operation (RFC-0004 §B)";
    // `dst` and `src` MAY each be a `PATH_REF` (RFC-0024 §9.1's amendment of RFC-0004 §B).
    const auto is_address = [](const tlv_t& c) {
        return c.type == type_t::PATH || c.type == type_t::PATH_REF;
    };
    std::size_t i = 1;
    if (i >= ch.size() || !is_address(ch[i])) return "FWD dst is missing or not a PATH / PATH_REF";
    ++i;
    if (i < ch.size() && ch[i].type == type_t::FIELD) ++i;
    if (i >= ch.size() || !is_address(ch[i])) return "FWD src is missing or not a PATH / PATH_REF";
    ++i;
    if (opcode != static_cast<std::uint8_t>(tr::graph::fwd_op_t::REPLY)) return std::nullopt;
    if (i >= ch.size() || !is_u8_value(ch[i]))
        return "FWD{REPLY} has no u8 VALUE kind after src (RFC-0004 §B)";
    const auto kind = std::to_integer<std::uint8_t>(ch[i].payload[0]);
    if (kind != static_cast<std::uint8_t>(tr::graph::reply_kind_t::RESULT) &&
        kind != static_cast<std::uint8_t>(tr::graph::reply_kind_t::ERROR))
        return "FWD{REPLY} kind is neither RESULT nor ERROR (RFC-0004 §B)";
    return std::nullopt;
}

/** @brief Why a decoded tree is not a legal frame, or `nullopt` if every node in it is. */
std::optional<std::string> frame_illegal(const tlv_t& t) {
    if (t.type == type_t::PATH) {
        if (t.opt.pl) return "PATH is structured (RFC-0018: a packed body is opt.PL = 0)";
        if (auto why = packed_path_illegal(t.payload)) return why;
    }
    if (t.type == type_t::FWD) {
        if (auto why = fwd_illegal(t)) return why;
    }
    for (const tlv_t& c : t.children) {
        if (auto why = frame_illegal(c)) return why;
    }
    return std::nullopt;
}

/**
 * @brief The legality verdict for one `input.bin`: `nullopt` when it passes, else the failure
 *        message.
 *
 * Both directions fail: an illegal frame without the flag (the bug class), and a flag on a
 * legal frame (a stale declaration, which would otherwise excuse the next real defect).
 * @param bytes    The vector's `input.bin`.
 * @param declared Whether its manifest declares `"malformed_input": true`.
 */
std::optional<std::string> legality_failure(std::span<const std::byte> bytes, bool declared) {
    const auto dec = tr::wire::decode(bytes);
    if (!dec) return std::string("input.bin does not decode");
    const auto why = frame_illegal(*dec);
    if (why && !declared)
        return "input.bin is not a legal frame (" + *why + ") — a deliberately-illegal vector " +
               "must declare \"" + std::string(kMalformedInputKey) + "\": true in expected.json";
    if (!why && declared)
        return "expected.json declares \"" + std::string(kMalformedInputKey) +
               "\": true but input.bin is a legal frame — drop the flag";
    return std::nullopt;
}

/**
 * @brief True when the in-place node @p n reads exactly as the owning tree @p t: type, opt,
 *        payload bytes, trailer values and every child, recursively (#1648).
 */
bool same_tree(const tr::wire::tlv_node_t& n, const tlv_t& t) {
    if (n.type() != t.type || n.opt() != t.opt || n.trailer() != t.trailer) return false;
    if (!std::ranges::equal(n.payload(), t.payload)) return false;
    std::size_t i = 0;
    for (const tr::wire::tlv_node_t c : n.children()) {
        if (i >= t.children.size() || !same_tree(c, t.children[i])) return false;
        ++i;
    }
    return i == t.children.size();
}

std::vector<std::byte> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::ranges::transform(raw, out.begin(), [](char c) {
        return static_cast<std::byte>(static_cast<unsigned char>(c));
    });
    return out;
}

// --- golden builders (construct each seed TLV programmatically) -------------
tlv_t status_ok() { return tlv_t{.type = type_t::STATUS}; }
tlv_t value(std::span<const std::byte> p) { return tlv_t{.type = type_t::VALUE, .payload = p}; }
tlv_t name(std::span<const std::byte> p) { return tlv_t{.type = type_t::NAME, .payload = p}; }

/**
 * @brief A packed two-segment `PATH` (RFC-0018): `opt.PL = 0`, body = `[u8 len][bytes]`
 *        records. The body bytes are OWNED by @p storage, which must outlive the TLV.
 */
tlv_t path2(std::span<const std::byte> a, std::span<const std::byte> b,
            std::vector<std::byte>& storage) {
    storage.clear();
    (void)tr::wire::emit_path_segment(storage, a);
    (void)tr::wire::emit_path_segment(storage, b);
    return tlv_t{.type = type_t::PATH, .payload = std::span<const std::byte>(storage)};
}

tlv_t value_crc(std::span<const std::byte> p) {
    tlv_t t{.type = type_t::VALUE, .payload = p};
    t.opt.cr = true;
    t.trailer =
        tr::wire::trailer_t{.ts = std::nullopt,
                            .crc = tr::wire::crc_t{.width = tr::wire::crc_t::width_t::CRC32C,
                                                   .value = tr::crc::crc32c(p)}};
    return t;
}

// --- encode/decode symmetry (#886) -----------------------------------------

/** @brief A `PATH_REF` over @p body, with the raw structural bits a caller could set. */
tlv_t path_ref(std::span<const std::byte> body, bool pl = false, bool ll = false) {
    tlv_t t{.type = type_t::PATH_REF};
    t.opt.pl = pl;
    t.opt.ll = ll;
    // PL routes the body through `children`, exactly as a caller who mistook a PATH_REF for a
    // structured type would build it; otherwise the body is the opaque element array.
    if (pl) {
        t.children.push_back(name(body));
    } else {
        t.payload = body;
    }
    return t;
}

/** @brief A structured `FWD` wrapping one child — the nesting case for the symmetry property. */
tlv_t fwd_wrapping(const tlv_t& child) {
    tlv_t t{.type = type_t::FWD};
    t.opt.pl = true;
    t.children.push_back(child);
    return t;
}

/**
 * @brief One symmetry case: a tree, and whether `encode` is required to accept it.
 *
 * `accepted == false` is not "this is untestable" — it is the assertion that `encode` REFUSES,
 * which is the half of the property that was missing before #886.
 */
struct symmetry_case_t {
    std::string what; /**< @brief The shape, as it appears on the PASS/FAIL line. */
    tlv_t tlv;        /**< @brief The tree to serialize. */
    bool accepted;    /**< @brief Whether `encode` must emit bytes rather than refuse. */
};

/**
 * @brief The standing codec property: every `encode()` SUCCESS must `decode()` (#886).
 *
 * This is the durable instrument, not a regression test for one shape. `encode` and
 * `grammar::parse_header` are two spellings of one grammar, and nothing but this property
 * keeps them honest: before #886 an ill-formed `PATH_REF` `tlv_t` serialized happily and
 * `decode` answered `tr::frame::invalid`, so the library round-tripped into a frame it
 * would not accept. Any future per-type rule added to the decoder and forgotten in the
 * encoder fails here.
 *
 * Refusal is spelled "emits nothing" — an accepted TLV always carries at least its 4-byte
 * header, so an empty result cannot be confused with a legal encoding.
 */
void check_symmetry(const symmetry_case_t& c) {
    const std::vector<std::byte> bytes = tr::wire::encode(c.tlv);
    check(bytes.empty() != c.accepted,
          c.what + (c.accepted ? " — encode emits" : " — encode refuses (emits nothing)"));
    // The property proper. Guarded on non-empty so a refusal does not score a vacuous pass:
    // decode() of an empty buffer fails, which would read as "the property holds" for every
    // rejected case. The acceptance check above is what makes this non-vacuous.
    if (!bytes.empty()) {
        const auto dec = tr::wire::decode(bytes);
        check(dec.has_value(), c.what + " — and the emitted bytes decode");
    }
}

// --- noise/ transcripts (RFC-0033 Appendix A, #2072) --------------------------
#if defined(LIBTRACER_NOISE_CRYPTO_OPENSSL) || defined(LIBTRACER_NOISE_CRYPTO_SODIUM) || \
    defined(LIBTRACER_NOISE_CRYPTO_PSA)
/** @brief Whether this build has a Noise crypto backend to replay the noise/ transcripts on. */
constexpr bool kHasNoise = true;

/**
 * @brief The string value of @p key in a flat-keyed transcript.json (tiny scan, no JSON
 *        parser: every key in a transcript is unique, wherever it nests).
 */
std::string transcript_str(std::string_view text, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t at = text.find(needle);
    if (at == std::string_view::npos) return {};
    const std::size_t open = text.find('"', text.find(':', at + needle.size()));
    const std::size_t close = text.find('"', open + 1);
    return std::string(text.substr(open + 1, close - open - 1));
}

/** @brief One transcript, replayed on the build's backend, every value compared. */
void check_noise_transcript(const fs::path& dir, const std::string& label) {
    namespace noise = tr::net::noise;
    using B = noise::default_crypto_t;
    std::ifstream f(dir / "transcript.json");
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto bytes = [&](std::string_view key) {
        return from_hex(transcript_str(text, key)).value_or(std::vector<std::byte>{});
    };
    const auto key32 = [&](std::string_view key) {
        noise::key32_t k{};
        const auto b = bytes(key);
        if (b.size() == k.size()) std::ranges::copy(b, k.begin());
        return k;
    };
    const auto number = [&](std::string_view key) {
        return std::stoull("0" + transcript_str(text, key));
    };
    const auto same = [&](std::span<const std::byte> got, std::string_view key) {
        return to_hex(got) == transcript_str(text, key);
    };
    const auto is = [&](const noise::key32_t& got, std::string_view key) { return same(got, key); };
    const std::string at = label + ": ";

    check(B::init() && transcript_str(text, "protocol_name") == noise::kProtocolName &&
              same(std::as_bytes(std::span(noise::kPrologue)), "prologue"),
          at + "the protocol name and the prologue are RFC-0033 §5.2's");
    const auto byte0 = [&](std::string_view key) {
        const auto b = bytes(key);
        return b.size() == 1 ? std::uint8_t(b[0]) : std::uint8_t{0xFF};
    };
    const noise::first_payload_t p1{.flags = byte0("msg1_flags"),
                                    .counter = number("msg1_counter_decimal")};
    check(same(noise::encode_first_payload(p1), "msg1_payload"),
          at + "msg1's payload is the flags byte then the counter, u64 LE");

    // The symmetric state token by token (Appendix A.3), on one chain both sides share.
    noise::symmetric_state_t<B> s;
    typename B::aead_t hc;  // the link's handshake cipher
    B::dh_key_t ei;
    B::dh_key_t er;
    noise::key32_t pub_i{};
    noise::key32_t pub_r{};
    noise::key32_t dh{};
    const auto ck_h = [&](std::string_view step) {
        return is(s.chaining_key(), std::string(step) + "_ck") &&
               is(s.handshake_hash(), std::string(step) + "_h");
    };
    const auto ck_h_k = [&](std::string_view step) {
        return ck_h(step) && is(s.cipher_key(), std::string(step) + "_k");
    };
    check(s.initialize(std::as_bytes(std::span(noise::kProtocolName))) && ck_h("initialize"),
          at + "InitializeSymmetric");
    check(s.mix_hash(bytes("prologue")) && ck_h("prologue"), at + "MixHash(prologue)");
    check(s.mix_key_and_hash(key32("psk")) && ck_h_k("psk"), at + "-> psk: MixKeyAndHash(psk)");
    check(ei.set(key32("initiator_ephemeral_private"), pub_i) &&
              is(pub_i, "initiator_ephemeral_public") && s.mix_hash(pub_i) &&
              is(s.handshake_hash(), "initiator_e_h") && s.mix_key(pub_i) && ck_h_k("initiator_e"),
          at + "-> e: e.pub, MixHash(e.pub), MixKey(e.pub)");
    std::array<std::byte, 9 + noise::kTagLen> ct1{};
    check(s.encrypt_and_hash(hc, bytes("msg1_payload"), ct1.data()) &&
              is(s.handshake_hash(), "msg1_payload_h"),
          at + "-> payload: EncryptAndHash");
    check(er.set(key32("responder_ephemeral_private"), pub_r) &&
              is(pub_r, "responder_ephemeral_public") && s.mix_hash(pub_r) &&
              is(s.handshake_hash(), "responder_e_h") && s.mix_key(pub_r) && ck_h_k("responder_e"),
          at + "<- e: e.pub, MixHash(e.pub), MixKey(e.pub)");
    noise::key32_t dh_r{};
    check(ei.agree(pub_r, dh) && er.agree(pub_i, dh_r) && dh == dh_r && is(dh, "dh_ee") &&
              s.mix_key(dh) && is(s.chaining_key(), "ee_ck") && is(s.cipher_key(), "ee_k"),
          at + "<- ee: DH(e, re) from both sides, MixKey");
    std::array<std::byte, 1 + noise::kTagLen> ct2{};
    check(s.encrypt_and_hash(hc, bytes("msg2_payload"), ct2.data()) &&
              is(s.handshake_hash(), "msg2_payload_h") && is(s.handshake_hash(), "handshake_hash"),
          at + "<- payload: EncryptAndHash, and h is the handshake hash");
    noise::key32_t k1{};
    noise::key32_t k2{};
    check(s.split(k1, k2) && is(k1, "k_i2r") && is(k2, "k_r2i"), at + "Split()");

    // The same transcript through the two handshake_t sides, datagram by datagram.
    noise::symmetric_state_t<B> base;
    check(noise::psk_state(key32("psk"), base) && is(base.chaining_key(), "psk_ck") &&
              is(base.handshake_hash(), "psk_h"),
          at + "psk_state is the state after the psk token");
    noise::handshake_t<B> i(noise::role_t::INITIATOR, base, hc);
    noise::handshake_t<B> r(noise::role_t::RESPONDER, base, hc);
    std::array<std::byte, noise::kFirstMessageBytes> m1{};
    std::array<std::byte, noise::kSecondMessageBytes> m2{};
    check(
        i.write_first(key32("initiator_ephemeral_private"), p1, m1).has_value() && same(m1, "msg1"),
        at + "msg1 (58 B), written by the initiator");
    const auto got1 = r.read_first(m1);
    check(got1 && got1->flags == p1.flags && got1->counter == p1.counter,
          at + "msg1 read by the responder: flags and counter");
    const auto f2 = byte0("msg2_payload");
    check(r.write_second(key32("responder_ephemeral_private"), f2, m2).has_value() &&
              same(m2, "msg2"),
          at + "msg2 (50 B), written by the responder");
    const auto got2 = i.read_second(m2);
    check(got2 && *got2 == f2, at + "msg2 read by the initiator: flags");
    check(is(i.symmetric_state().handshake_hash(), "handshake_hash") &&
              is(r.symmetric_state().handshake_hash(), "handshake_hash"),
          at + "both sides' handshake hash");
    noise::transport_cipher_t<B> ti;
    noise::transport_cipher_t<B> tr_;
    check(i.split(ti).has_value() && r.split(tr_).has_value(), at + "both sides split");

    const auto transport = [&](noise::transport_cipher_t<B>& from, noise::transport_cipher_t<B>& to,
                               std::string_view nonce, std::string_view frame,
                               std::string_view want) {
        const std::vector<std::byte> pt = bytes(frame);
        std::vector<std::byte> d(pt.size() + noise::kTransportOverhead);
        const auto n = from.seal(0, number(nonce), pt, d);
        const auto h = noise::parse_transport_header(d);
        const bool wrote = n && same(d, want);
        const auto opened = to.open(h.value_or(noise::transport_header_t{}), d);
        return wrote && h && opened && std::ranges::equal(*opened, pt);
    };
    check(transport(ti, tr_, "confirmation_nonce_decimal", "", "confirmation"),
          at + "the empty confirmation, i->r (25 B), sealed and opened");
    check(transport(ti, tr_, "frame_i2r_nonce_decimal", "frame_i2r", "datagram_i2r"),
          at + "frame i->r (33 B), sealed and opened");
    check(transport(tr_, ti, "frame_r2i_nonce_decimal", "frame_r2i", "datagram_r2i"),
          at + "frame r->i (33 B), sealed and opened");
}
#else
/** @brief Whether this build has a Noise crypto backend to replay the noise/ transcripts on. */
constexpr bool kHasNoise = false;

/** @brief Built without a Noise backend: nothing to replay a transcript on. */
void check_noise_transcript(const fs::path&, const std::string&) {}
#endif
}  // namespace

int main(int argc, char** argv) {
    const fs::path vroot{LIBTRACER_VECTORS_DIR};

    // `--roundtrip`: differential-fuzz batch mode (no vectors dir). See run_roundtrip.
    if (argc > 1 && std::string_view(argv[1]) == "--roundtrip") {
        return run_roundtrip();
    }

    // `--tap`: emit the portable cross-core contract (encode(decode(input)) == input,
    // per vector) as TAP for the polyglot driver (tests/conformance/HARNESS.md).
    if (argc > 1 && std::string_view(argv[1]) == "--tap") {
        std::vector<std::pair<std::string, bool>> tap;
        for (const auto& e : fs::recursive_directory_iterator(vroot)) {
            const auto fname = e.path().filename();
            if (fname != "input.bin" && fname != "reject.bin") continue;
            const std::string rel = fs::relative(e.path().parent_path(), vroot).generic_string();
            const std::vector<std::byte> bytes = read_file(e.path());
            if (fname == "reject.bin") {
                tap.emplace_back(rel, check_reject(e.path(), bytes));
                continue;
            }
            const auto dec = tr::wire::decode(bytes);
            tap.emplace_back(rel, dec.has_value() && tr::wire::encode(*dec) == bytes);
        }
        std::sort(tap.begin(), tap.end());
        std::printf("TAP version 13\n1..%zu\n", tap.size());
        int n = 0, fails = 0;
        for (const auto& [rel, ok] : tap) {
            std::printf("%s %d - %s\n", ok ? "ok" : "not ok", ++n, rel.c_str());
            if (!ok) ++fails;
        }
        return fails == 0 ? 0 : 1;
    }

    std::printf("Generic roundtrip (decode -> encode == input.bin):\n");
    for (const auto& e : fs::recursive_directory_iterator(vroot)) {
        if (e.path().filename() != "input.bin") continue;
        const std::string label = e.path().parent_path().filename().string();
        const std::vector<std::byte> bytes = read_file(e.path());
        const auto dec = tr::wire::decode(bytes);
        if (!dec) {
            check(false, label + " (decode failed)");
            continue;
        }
        check(tr::wire::encode(*dec) == bytes, label);
    }

    std::printf("Input legality (input.bin is a legal frame, or declares %s):\n",
                std::string(kMalformedInputKey).c_str());
    for (const auto& e : fs::recursive_directory_iterator(vroot)) {
        if (e.path().filename() != "input.bin") continue;
        const fs::path dir = e.path().parent_path();
        const std::string label = fs::relative(dir, vroot).generic_string();
        const auto fail = legality_failure(read_file(e.path()), malformed_input_declared(dir));
        check(!fail, label + (fail ? ": " + *fail : std::string()));
    }
    {
        // The gate itself, against a known-bad frame: a FWD{REPLY} with no `kind` — the exact
        // shape fwd/fwd-label-mint-reply banked before a99d362f. Undeclared it must fail and
        // the message must name the flag; declared it must pass.
        static constexpr std::array no_kind_reply{
            std::byte{0x0F}, std::byte{0x40}, std::byte{0x0D}, std::byte{0x00},  // FWD, PL=1
            std::byte{0x01}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
            std::byte{0x03},                                                     // op = REPLY
            std::byte{0x06}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},  // dst = /
            std::byte{0x06}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},  // src = /
        };
        const auto undeclared = legality_failure(no_kind_reply, /*declared=*/false);
        check(undeclared && undeclared->find(kMalformedInputKey) != std::string::npos,
              "an undeclared illegal input.bin fails, naming the malformed_input flag");
        check(!legality_failure(no_kind_reply, /*declared=*/true),
              "the same frame passes once it declares malformed_input");
    }

    std::printf("Negative vectors (decode(reject.bin) fails with expected.json's error):\n");
    for (const auto& e : fs::recursive_directory_iterator(vroot)) {
        if (e.path().filename() != "reject.bin") continue;
        const std::string label = e.path().parent_path().filename().string();
        check(check_reject(e.path(), read_file(e.path())), label);
    }

    // The in-place walker must accept, refuse and read the corpus exactly as `decode` does
    // (#1648): same verdict, same error, same tree.
    std::printf("Walker equivalence (tlv_node_t::over + children() == decode, every vector):\n");
    for (const auto& e : fs::recursive_directory_iterator(vroot)) {
        const auto fname = e.path().filename();
        if (fname != "input.bin" && fname != "reject.bin") continue;
        const std::string label = e.path().parent_path().filename().string();
        const std::vector<std::byte> bytes = read_file(e.path());
        const auto dec = tr::wire::decode(bytes);
        const auto node = tr::wire::tlv_node_t::over(bytes);
        if (!dec) {
            check(!node && node.error() == dec.error(), label + " (refused alike)");
            continue;
        }
        check(node && same_tree(*node, *dec) && std::ranges::equal(node->bytes(), bytes),
              label + " (read alike)");
    }

    // noise/ cases are transcripts (RFC-0033 §12.2), not frames: no input.bin, so the codec
    // walks above and the --tap matrix never see them. A build with a Noise crypto backend
    // replays each one on that backend (#2072).
    std::printf("Noise transcripts (noise/*/transcript.json, backend %s):\n",
                kHasNoise ? "of this build" : "none: skipped");
    for (const auto& e : fs::recursive_directory_iterator(vroot)) {
        if (e.path().filename() != "transcript.json") continue;
        const fs::path dir = e.path().parent_path();
        const std::string label = fs::relative(dir, vroot).generic_string();
        if (!kHasNoise) {
            std::printf("  [SKIP] %s (configure with -DLIBTRACER_NOISE_CRYPTO=...)\n",
                        label.c_str());
            continue;
        }
        check_noise_transcript(dir, label);
    }

    std::printf("Golden builders (encode == input.bin && decode == built):\n");
    static constexpr std::array b_true{std::byte{0x01}};
    static constexpr std::array b_val{std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC},
                                      std::byte{0xDD}, std::byte{0xEE}};
    static constexpr std::array s_sensor{std::byte{'s'}, std::byte{'e'}, std::byte{'n'},
                                         std::byte{'s'}, std::byte{'o'}, std::byte{'r'}};
    static constexpr std::array s_temp{std::byte{'t'}, std::byte{'e'}, std::byte{'m'},
                                       std::byte{'p'}};

    const auto golden = [&](const std::string& sub, const tlv_t& built) {
        const std::vector<std::byte> input = read_file(vroot / sub / "input.bin");
        check(tr::wire::encode(built) == input, sub + " encode");
        const auto dec = tr::wire::decode(input);
        check(dec.has_value() && tr::wire::equal(*dec, built), sub + " decode");
    };
    golden("framing/empty-status-ok", status_ok());
    golden("tlv-types/value-bool-true", value(b_true));
    std::vector<std::byte> sensor_temp_body;
    golden("path/path-sensor-temp", path2(s_sensor, s_temp, sensor_temp_body));
    golden("crc/value-crc32c", value_crc(b_val));

    std::printf("Encode/decode symmetry (every encode() success must decode()):\n");
    {
        // Two well-formed PATH_REF elements, little-endian: (index 7, gen 3), (index 9, gen 1).
        static constexpr std::array<tr::wire::path_ref_element_t, 2> els{
            tr::wire::path_ref_element_t{.index = 7, .generation = 3},
            tr::wire::path_ref_element_t{.index = 9, .generation = 1}};
        std::vector<std::byte> ref_body;
        (void)tr::wire::emit_path_ref(ref_body, els);
        // Drop the emitter's envelope, keep the bare element array — sized from the emitter's
        // own size function so the split cannot drift from the format.
        ref_body.erase(ref_body.begin(), ref_body.begin() + static_cast<std::ptrdiff_t>(
                                                                tr::wire::path_ref_wire_bytes(0)));

        // Bodies are owned here because tlv_t::payload BORROWS. Sizes, not magic numbers: the
        // §4.3 bound and the element stride both come from path_ref.hpp.
        const std::vector<std::byte> odd_body(tr::wire::kPathRefElementBytes + 1, std::byte{0});
        const std::vector<std::byte> over_body(
            tr::wire::kMaxPathRefBodyBytes + tr::wire::kPathRefElementBytes, std::byte{0});
        const std::vector<std::byte> at_bound_body(tr::wire::kMaxPathRefBodyBytes, std::byte{0});
        const std::vector<std::byte> empty_body;

        const symmetry_case_t cases[] = {
            {"empty STATUS", status_ok(), true},
            {"VALUE over opaque bytes", value(b_val), true},
            {"packed PATH of two segment records", path2(s_sensor, s_temp, sensor_temp_body), true},
            {"VALUE with a CRC32C trailer", value_crc(b_val), true},
            {"PATH_REF, two elements", path_ref(ref_body), true},
            {"PATH_REF, zero elements", path_ref(empty_body), true},
            {"PATH_REF at the RFC-0024 §4.3 bound", path_ref(at_bound_body), true},
            {"PATH_REF with opt.PL set", path_ref(ref_body, /*pl=*/true), false},
            {"PATH_REF with opt.LL set", path_ref(ref_body, /*pl=*/false, /*ll=*/true), false},
            {"PATH_REF whose length % 8 != 0", path_ref(odd_body), false},
            {"PATH_REF past the RFC-0024 §4.3 bound", path_ref(over_body), false},
            {"FWD carrying a well-formed PATH_REF", fwd_wrapping(path_ref(ref_body)), true},
            {"FWD carrying an ill-formed PATH_REF", fwd_wrapping(path_ref(ref_body, /*pl=*/true)),
             false},
        };
        for (const symmetry_case_t& c : cases) check_symmetry(c);

        // The accepted spelling must not have drifted either: the generic encode() of a
        // well-formed PATH_REF is byte-identical to the guarded emitter's output, so applying
        // the rule on this door changed nothing a caller already had.
        std::vector<std::byte> via_emitter;
        (void)tr::wire::emit_path_ref(via_emitter, els);
        check(tr::wire::encode(path_ref(ref_body)) == via_emitter,
              "a well-formed PATH_REF encodes byte-identically to emit_path_ref");
    }

    std::printf("Targeted asserts:\n");
    check(tr::crc::crc32c(b_val) == 0x2312C9B6u, "crc32c(AABBCCDDEE) == 0x2312C9B6");
    {
        static constexpr std::array want{std::byte{0x09}, std::byte{0}, std::byte{0}, std::byte{0}};
        check(std::ranges::equal(tr::wire::encode(status_ok()), want),
              "empty STATUS encodes to 09 00 00 00");
    }
    {
        // The bytes are BOUND: `decode` borrows, and the packed body is read through the
        // decoded node's payload span, so a temporary here would dangle.
        const std::vector<std::byte> path_bytes =
            read_file(vroot / "path/path-sensor-temp" / "input.bin");
        const auto dec = tr::wire::decode(path_bytes);
        // RFC-0018: the body is packed records, so a PATH decodes as ONE opaque node whose
        // payload tiles into `06 'sensor' 04 'temp'` — 12 bytes, no child TLVs at all.
        check(dec.has_value() && !dec->opt.pl && dec->children.empty() &&
                  dec->payload.size() == 12 && tr::wire::packed_path_valid_key(dec->payload),
              "PATH decodes to one packed 12-byte body of two literal records");
    }
    {
        const std::vector<std::byte> bad{std::byte{0x09}, std::byte{0x01}, std::byte{0},
                                         std::byte{0}};
        const auto dec = tr::wire::decode(bad);
        check(!dec.has_value() && dec.error() == tr::wire::err_t::FRAME_INVALID,
              "reserved-bit input rejected as frame::invalid");
    }

    return tr::testing::summary("conformance_runner");
}
