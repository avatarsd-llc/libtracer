/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — one BATCH record, two choices on it: framed or packed samples, carried
 *        standalone or folded.
 *
 * The application composes the batch. It holds three samples it already acquired, chooses
 * their base time and their offsets from its own clock, and decides when the batch is full;
 * the graph has no clock, no window and no counter, and never learns that a value is a batch
 * (RFC-0025 §4.1.3, Amendment 4). This example therefore reads no clock at all: the base time
 * is a constant standing in for the application's own number.
 *
 * The same batch (base, offsets `[0, 1500, -250]` ns, samples `0x64 0x65 0x66` as u16) is
 * spelled four ways, and each spelling's bytes are printed and checked:
 *
 * | samples | carriage   | header type | checked against                         |
 * | ------- | ---------- | ----------- | --------------------------------------- |
 * | framed  | standalone | `0x80`      | vector `stream/batch-composed-standalone` |
 * | framed  | folded     | `0x01`      | vector `stream/batch-composed-folded`     |
 * | packed  | standalone | `0x80`      | the pinned bytes below                    |
 * | packed  | folded     | `0x01`      | the pinned bytes below                    |
 *
 * Framed samples are one `VALUE` child each, roped on behind one owned head segment by
 * `compose_batch` and never copied. Packed samples are one `VALUE` block of `n × width` bytes
 * (Amendment 5), written by `encode_packed_batch` into the caller's span. The carriage changes
 * only the header type byte: `0x80` for a standalone record, `VALUE` (`0x01`) for the seat a
 * `propagate(v, FOLD)` branch-write node admits.
 *
 * Then the batch is pushed both ways, through the ordinary write path: compose, swap the value
 * in with `assign`, call `propagate`. The default propagate delivers the standalone record as
 * is; `propagate(v, FOLD)` delivers one branch-write frame for the subtree, checked against
 * vector `stream/fold-carries-composed-batch`.
 *
 * Runs under ctest as `example_graph_batch`; returns non-zero on any failed check.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/batch.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::emission_mode_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::view::rope_t;
using tr::wire::batch_carriage_t;

/** @brief The batch base: the application's own sample time of frame 0, in ns. */
constexpr std::int64_t kBase = 1'700'000'000'000'000'000;
/** @brief Per-sample offsets from the base: a non-uniform stream, so they ride along. */
constexpr std::array<std::int32_t, 3> kOffsets{0, 1500, -250};
/** @brief The three samples the application acquired, as u16 readings. */
constexpr std::array<std::uint16_t, 3> kSamples{0x0064, 0x0065, 0x0066};
/** @brief The descriptor's `width` for the packed shape: one u16 per sample. */
constexpr std::uint16_t kWidth = 2;

/**
 * @brief The packed standalone record, written out by hand from the layout in
 *        `docs/reference/05-protocol-tlvs.md` (an independent source, not the encoder's output).
 */
constexpr std::array<std::uint8_t, 42> kPackedStandalone{
    0x80, 0x40, 0x26, 0x00,                          // BATCH, opt.PL, body 38 bytes
    0x0C, 0x00, 0x08, 0x00,                          // TIME, body 8 bytes
    0x00, 0x00, 0x2A, 0x36, 0xFE, 0x9C, 0x97, 0x17,  //   u64 LE base
    0x01, 0x00, 0x0C, 0x00,                          // VALUE, body 12 bytes: the offsets
    0x00, 0x00, 0x00, 0x00,                          //   i32 LE 0
    0xDC, 0x05, 0x00, 0x00,                          //   i32 LE 1500
    0x06, 0xFF, 0xFF, 0xFF,                          //   i32 LE -250
    0x01, 0x00, 0x06, 0x00,                          // VALUE, body 6 bytes: the block
    0x64, 0x00, 0x65, 0x00, 0x66, 0x00,              //   three u16 LE samples, no headers
};

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/** @brief Print @p bytes as hex, sixteen to a line, under @p label. */
void dump(const char* label, std::span<const std::byte> bytes) {
    std::printf("%s (%zu bytes):\n", label, bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i)
        std::printf("%s%02x", i % 16 == 0 ? (i == 0 ? "    " : "\n    ") : " ",
                    static_cast<unsigned>(bytes[i]));
    std::printf("\n");
}

/** @brief A whole rope's bytes, gathered. */
std::vector<std::byte> bytes_of(const rope_t& r) {
    std::vector<std::byte> out;
    r.walk([&out](std::span<const std::byte> s) { out.insert(out.end(), s.begin(), s.end()); });
    return out;
}

/** @brief A delivered value's bytes, gathered. */
std::vector<std::byte> bytes_of(const tr::graph::value_t& v) {
    std::vector<std::byte> out;
    v.walk([&out](std::span<const std::byte> s) { out.insert(out.end(), s.begin(), s.end()); });
    return out;
}

/** @brief The `input.bin` of conformance vector @p name (`<category>/<case>`). */
std::vector<std::byte> vector_bytes(std::string_view name) {
    std::ifstream f(std::filesystem::path{LIBTRACER_VECTORS_DIR} / name / "input.bin",
                    std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out;
    for (const char c : raw) out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return out;
}

/** @brief @p pinned as bytes, with its header type byte replaced by @p type. */
std::vector<std::byte> pinned(std::span<const std::uint8_t> bytes, std::uint8_t type) {
    std::vector<std::byte> out;
    for (const std::uint8_t b : bytes) out.push_back(std::byte{b});
    out[0] = std::byte{type};
    return out;
}

}  // namespace

int main() {
    bool ok = true;

    // The application's own sample frames, one encoded VALUE each, held as views over its own
    // buffers: exactly what a composition references instead of copying.
    std::vector<std::vector<std::byte>> frames;
    for (const std::uint16_t s : kSamples) {
        std::vector<std::byte> f;
        tr::wire::emit_value_le<std::uint16_t>(f, s);
        frames.push_back(std::move(f));
    }
    std::vector<tr::view::view_t> views;
    for (const std::vector<std::byte>& f : frames)
        views.push_back(
            tr::view::view_t::over(tr::view::borrow_const(std::span<const std::byte>(f))));

    // Choice 1, framed samples: one owned head segment, the samples roped on behind it.
    std::printf("framed samples, composed by compose_batch:\n");
    rope_t standalone = tr::wire::compose_batch(
        tr::mem::heap_backend(), batch_carriage_t::STANDALONE, kBase, views, kOffsets);
    rope_t folded = tr::wire::compose_batch(tr::mem::heap_backend(), batch_carriage_t::FOLDED,
                                            kBase, views, kOffsets);
    const std::vector<std::byte> framed_standalone = bytes_of(standalone);
    const std::vector<std::byte> framed_folded = bytes_of(folded);
    dump("  framed, standalone", framed_standalone);
    dump("  framed, folded", framed_folded);
    check(ok, standalone.link_count() == 1 + views.size(),
          "the composition is one head link plus one link per sample: nothing was copied");
    check(ok, framed_standalone == vector_bytes("stream/batch-composed-standalone"),
          "framed standalone matches stream/batch-composed-standalone byte for byte");
    check(ok, framed_folded == vector_bytes("stream/batch-composed-folded"),
          "framed folded matches stream/batch-composed-folded byte for byte");

    // Choice 2, packed samples: the descriptor declares width 2, so the three samples are one
    // block of 6 bytes, with no per-sample header.
    std::printf("packed samples, encoded by encode_packed_batch:\n");
    std::vector<std::byte> block;
    for (const std::uint16_t s : kSamples) {
        block.push_back(std::byte{static_cast<std::uint8_t>(s & 0xFFu)});
        block.push_back(std::byte{static_cast<std::uint8_t>(s >> 8)});
    }
    std::array<std::byte, 64> out{};
    std::vector<std::byte> packed_standalone(
        out.begin(),
        out.begin() + static_cast<std::ptrdiff_t>(tr::wire::encode_packed_batch(
                          out, batch_carriage_t::STANDALONE, kBase, kWidth, block, kOffsets)));
    std::vector<std::byte> packed_folded(
        out.begin(),
        out.begin() + static_cast<std::ptrdiff_t>(tr::wire::encode_packed_batch(
                          out, batch_carriage_t::FOLDED, kBase, kWidth, block, kOffsets)));
    dump("  packed, standalone", packed_standalone);
    dump("  packed, folded", packed_folded);
    check(ok, packed_standalone == pinned(kPackedStandalone, 0x80),
          "packed standalone matches the pinned bytes");
    check(ok, packed_folded == pinned(kPackedStandalone, 0x01),
          "packed folded is the same bytes headed VALUE (0x01): the carriage is one byte");
    check(ok, framed_standalone.size() - packed_standalone.size() == 4 * (kSamples.size() - 1),
          "the block saves the 4-byte header of every sample but the first");

    // Reading a record back: the descriptor (dt_ns = 0, width) says how; the bytes do not.
    const auto framed_node = tr::wire::tlv_node_t::over(framed_standalone);
    const auto packed_node = tr::wire::tlv_node_t::over(packed_standalone);
    // value_or, not a dereference: an empty view reads as zero samples and fails the check.
    const tr::wire::batch_view_t framed_view =
        (framed_node ? tr::wire::read_batch(*framed_node, 0) : std::nullopt)
            .value_or(tr::wire::batch_view_t{});
    const tr::wire::batch_view_t packed_view =
        (packed_node ? tr::wire::read_packed_batch(*packed_node, 0, kWidth) : std::nullopt)
            .value_or(tr::wire::batch_view_t{});
    check(ok, framed_view.size() == 3 && packed_view.size() == 3,
          "both shapes read back as three samples, the packed count being n = 6 / width");
    check(ok, packed_view.sample_time_ns(2) == kBase - 250,
          "sample 2's time is derived from the base and its offset");

    // Pushing it. Logs and callbacks are declared before the graph that points at them.
    std::vector<std::vector<std::byte>> at_b;
    std::vector<std::vector<std::byte>> at_s;
    auto on_b = [&at_b](const tr::graph::value_t& v) { at_b.push_back(bytes_of(v)); };
    auto on_s = [&at_s](const tr::graph::value_t& v) { at_s.push_back(bytes_of(v)); };
    graph_t g;
    const vertex_handle_t b = g.register_vertex(path_t("/b"), role_t::STORED_VALUE);
    const vertex_handle_t s = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    const vertex_handle_t adc = g.register_vertex(path_t("/s/adc0"), role_t::STORED_VALUE);
    check(ok, g.subscribe(path_t("/b"), on_b).has_value(), "a subscriber at /b");
    check(ok, g.subscribe(path_t("/s"), on_s).has_value(), "a subtree subscriber at /s");

    std::printf("push standalone: compose, assign, propagate:\n");
    check(ok, g.assign(b, std::move(standalone)).has_value(), "the batch is swapped in at /b");
    check(ok, g.propagate(b).has_value(), "propagate delivers it");
    check(ok, at_b.size() == 1 && at_b[0] == framed_standalone,
          "the subscriber receives the standalone record unchanged");

    std::printf("push folded: compose, assign, propagate FOLD:\n");
    check(ok, g.assign(adc, std::move(folded)).has_value(), "the batch is swapped in at /s/adc0");
    check(ok, g.propagate(s, emission_mode_t::FOLD).has_value(),
          "propagate FOLD sweeps /s into one branch-write frame");
    if (at_s.size() == 1) dump("  the branch-write frame", at_s[0]);
    check(ok, at_s.size() == 1 && at_s[0] == vector_bytes("stream/fold-carries-composed-batch"),
          "the frame matches stream/fold-carries-composed-batch byte for byte");

    std::printf("%s\n", ok ? "graph_batch: all checks passed" : "graph_batch: FAILED");
    return ok ? 0 : 1;
}
