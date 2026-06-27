/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Conformance harness for the seed vectors under tests/conformance/vectors/v1/.
 * No JSON parser: input.bin is self-describing, so the codec is validated by
 *   (1) generic roundtrip  — encode(decode(input.bin)) == input.bin, for every vector;
 *   (2) golden builders     — encode(built) == input.bin && decode(input.bin) == built;
 *   (3) targeted asserts     — the CRC value, the PATH child count, reserved-bit rejection.
 * expected.json stays as the human-readable / cross-language spec.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/tracer.hpp"

namespace {

namespace fs = std::filesystem;
using tr::wire::tlv_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
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

}  // namespace

int main(int argc, char** argv) {
    const fs::path vroot{LIBTRACER_VECTORS_DIR};

    // `--tap`: emit the portable cross-core contract (encode(decode(input)) == input,
    // per vector) as TAP for the polyglot driver (tests/conformance/HARNESS.md).
    if (argc > 1 && std::string_view(argv[1]) == "--tap") {
        std::vector<std::pair<std::string, bool>> tap;
        for (const auto& e : fs::recursive_directory_iterator(vroot)) {
            if (e.path().filename() != "input.bin") continue;
            const std::string rel = fs::relative(e.path().parent_path(), vroot).generic_string();
            const std::vector<std::byte> bytes = read_file(e.path());
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
        check(!dec.has_value() && dec.error() == tr::wire::error_t::FRAME_INVALID,
              "reserved-bit input rejected as frame::invalid");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
