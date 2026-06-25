// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// Conformance harness for the seed vectors under tests/conformance/vectors/v1/.
// No JSON parser: input.bin is self-describing, so the codec is validated by
//   (1) generic roundtrip  — encode(decode(input.bin)) == input.bin, for every vector;
//   (2) golden builders     — encode(built) == input.bin && decode(input.bin) == built;
//   (3) targeted asserts     — the CRC value, the PATH child count, reserved-bit rejection.
// expected.json stays as the human-readable / cross-language spec.

#include "libtracer/tracer.hpp"

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
#include <vector>

namespace {

namespace fs = std::filesystem;
using tracer::Error;
using tracer::Tlv;
using tracer::Type;

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
Tlv status_ok() { return Tlv{.type = Type::Status}; }
Tlv value(std::span<const std::byte> p) { return Tlv{.type = Type::Value, .payload = p}; }
Tlv name(std::span<const std::byte> p) { return Tlv{.type = Type::Name, .payload = p}; }

Tlv path2(std::span<const std::byte> a, std::span<const std::byte> b) {
    Tlv t{.type = Type::Path};
    t.opt.pl = true;
    t.children.push_back(name(a));
    t.children.push_back(name(b));
    return t;
}

Tlv value_crc(std::span<const std::byte> p) {
    Tlv t{.type = Type::Value, .payload = p};
    t.opt.cr = true;
    t.trailer = tracer::Trailer{
        .ts = std::nullopt,
        .crc = tracer::Crc{.width = tracer::Crc::Width::Crc32c, .value = tracer::crc::crc32c(p)}};
    return t;
}

}  // namespace

int main() {
    const fs::path vroot{LIBTRACER_VECTORS_DIR};

    std::printf("Generic roundtrip (decode -> encode == input.bin):\n");
    for (const auto& e : fs::recursive_directory_iterator(vroot)) {
        if (e.path().filename() != "input.bin") continue;
        const std::string label = e.path().parent_path().filename().string();
        const std::vector<std::byte> bytes = read_file(e.path());
        const auto dec = tracer::decode(bytes);
        if (!dec) {
            check(false, label + " (decode failed)");
            continue;
        }
        check(tracer::encode(*dec) == bytes, label);
    }

    std::printf("Golden builders (encode == input.bin && decode == built):\n");
    static constexpr std::array b_true{std::byte{0x01}};
    static constexpr std::array b_val{std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC},
                                      std::byte{0xDD}, std::byte{0xEE}};
    static constexpr std::array s_sensor{std::byte{'s'}, std::byte{'e'}, std::byte{'n'},
                                         std::byte{'s'}, std::byte{'o'}, std::byte{'r'}};
    static constexpr std::array s_temp{std::byte{'t'}, std::byte{'e'}, std::byte{'m'},
                                       std::byte{'p'}};

    const auto golden = [&](const std::string& sub, const Tlv& built) {
        const std::vector<std::byte> input = read_file(vroot / sub / "input.bin");
        check(tracer::encode(built) == input, sub + " encode");
        const auto dec = tracer::decode(input);
        check(dec.has_value() && tracer::equal(*dec, built), sub + " decode");
    };
    golden("framing/empty-status-ok", status_ok());
    golden("tlv-types/value-bool-true", value(b_true));
    golden("path/path-sensor-temp", path2(s_sensor, s_temp));
    golden("crc/value-crc32c", value_crc(b_val));

    std::printf("Targeted asserts:\n");
    check(tracer::crc::crc32c(b_val) == 0x2312C9B6u, "crc32c(AABBCCDDEE) == 0x2312C9B6");
    {
        static constexpr std::array want{std::byte{0x09}, std::byte{0}, std::byte{0}, std::byte{0}};
        check(std::ranges::equal(tracer::encode(status_ok()), want),
              "empty STATUS encodes to 09 00 00 00");
    }
    {
        const auto dec = tracer::decode(read_file(vroot / "path/path-sensor-temp" / "input.bin"));
        check(dec.has_value() && dec->children.size() == 2, "PATH decodes to 2 NAME children");
    }
    {
        const std::vector<std::byte> bad{std::byte{0x09}, std::byte{0x01}, std::byte{0}, std::byte{0}};
        const auto dec = tracer::decode(bad);
        check(!dec.has_value() && dec.error() == Error::FrameInvalid,
              "reserved-bit input rejected as frame::invalid");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
