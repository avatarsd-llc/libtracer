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
 *       expected.json's "reject" field (extracted by a tiny scan, no JSON parser).
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

#include "libtracer/path_ref.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

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

tlv_t path2(std::span<const std::byte> a, std::span<const std::byte> b) {
    tlv_t t{.type = type_t::PATH};
    t.opt.pl = true;
    t.children.push_back(name(a));
    t.children.push_back(name(b));
    return t;
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

    std::printf("Negative vectors (decode(reject.bin) fails with expected.json's error):\n");
    for (const auto& e : fs::recursive_directory_iterator(vroot)) {
        if (e.path().filename() != "reject.bin") continue;
        const std::string label = e.path().parent_path().filename().string();
        check(check_reject(e.path(), read_file(e.path())), label);
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
    golden("path/path-sensor-temp", path2(s_sensor, s_temp));
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
            {"structured PATH of two NAMEs", path2(s_sensor, s_temp), true},
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
        const auto dec = tr::wire::decode(read_file(vroot / "path/path-sensor-temp" / "input.bin"));
        check(dec.has_value() && dec->children.size() == 2, "PATH decodes to 2 NAME children");
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
