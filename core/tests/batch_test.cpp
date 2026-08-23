/**
 * @file
 * @brief The BATCH record (`0x80`) — the fold, the derived sample clock, and what the
 *        reader declines (RFC-0025 §4.2.1 / §4.1.2, `batch.hpp`).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Four claims, and the middle two are the ones a vector cannot make on its own (HARNESS.md
 * §what a vector gates):
 *
 * 1. **The fold is a concatenation under one header.** N sample frames become ONE written
 *    value and no sample's own bytes change — which is what makes `emit_batch` usable on
 *    frames a STREAM vertex already holds, rather than a re-encoder.
 * 2. **A uniform stream spends 0 BYTES PER SAMPLE on time.** `t(i) = base + i × dt_ns` is
 *    ARITHMETIC over the descriptor's rate; nothing per-sample is transmitted. The test
 *    asserts the derivation *and* the byte count that makes it worth having — three 2-byte
 *    samples cost 6 bytes of payload, not 18 (the retired per-child-trailer spelling) and not
 *    18 either under a packed offset array.
 * 3. **`dt_ns` comes from the DESCRIPTOR, not from the record.** The same child layout reads
 *    as "three samples" against `dt_ns != 0` and as "an offset array plus two samples"
 *    against `dt_ns == 0`. The ablation below drives one buffer both ways.
 * 4. **A reader's refusal is not the codec's.** `read_batch` answers nothing for bytes that
 *    do not spell the convention, while the same bytes still decode and still round-trip —
 *    the user range is a range the protocol does not opine on.
 *
 * `conformance_vectors` pins `stream/batch-time-roundtrip` byte-exactly against the emitter,
 * so the vector records the spelling rather than merely accompanying it (ADR-0028: the C++
 * core is golden).
 */

#include "libtracer/batch.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/frame.hpp"
#include "libtracer/mem_borrowed.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/subscriber.hpp"
#include "libtracer/tlv_emit.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;
using tr::wire::batch_view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief The vector's base — 2023-11-14T22:13:20Z, in nanoseconds since the Unix epoch. */
constexpr std::int64_t kBase = 1'700'000'000'000'000'000;

/** @brief The descriptor's nominal sample period for the uniform arm: a 1 kHz acquisition. */
constexpr std::uint64_t kDt = 1000;

/** @brief The raw bytes of a conformance vector's `input.bin`. */
std::vector<std::byte> vector_bytes(std::string_view case_dir) {
    const std::filesystem::path p =
        std::filesystem::path{LIBTRACER_VECTORS_DIR} / case_dir / "input.bin";
    std::ifstream f(p, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    return out;
}

/** @brief Byte equality over two spans. */
bool same(std::span<const std::byte> a, std::span<const std::byte> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

/** @brief One sample frame: `VALUE{ u16 LE }` — the ADC-reading shape of the vector. */
std::vector<std::byte> sample(std::uint16_t reading) {
    std::vector<std::byte> out;
    tr::wire::emit_value_le<std::uint16_t>(out, reading);
    return out;
}

/** @brief The three sample frames of the vector, as the spans `emit_batch` folds. */
struct samples_t {
    std::vector<std::vector<std::byte>> frames;
    std::vector<std::span<const std::byte>> spans;
};

/** @brief Build the vector's three samples (0x0064, 0x0065, 0x0066). */
samples_t three_samples() {
    samples_t s;
    for (const std::uint16_t r : {0x0064, 0x0065, 0x0066}) s.frames.push_back(sample(r));
    for (const std::vector<std::byte>& f : s.frames) s.spans.emplace_back(f);
    return s;
}

/** @brief Decode a whole buffer as one TLV — the codec tier, unaware of any convention. */
std::optional<tr::wire::tlv_t> decode_all(std::span<const std::byte> bytes) {
    auto got = tr::wire::decode(bytes, tr::mem::heap_source());
    if (!got) return std::nullopt;
    return std::move(*got);
}

/**
 * @brief Claim 1 + the vector: the fold is a concatenation under one header, and these are
 *        the bytes.
 */
void fold_is_a_concatenation() {
    std::printf("§4.2.1 fold — N sample frames become ONE written value:\n");
    const samples_t s = three_samples();

    std::vector<std::byte> folded;
    tr::wire::emit_batch(folded, kBase, s.spans);

    check(same(folded, vector_bytes("stream/batch-time-roundtrip")),
          "stream/batch-time-roundtrip is byte-exact against the emitter");
    check(folded.size() == tr::wire::batch_wire_bytes(6 * 3),
          "... and batch_wire_bytes sizes the buffer the emitter fills");
    check(std::to_integer<std::uint8_t>(folded[0]) == 0x80,
          "the record's type byte is the ASSIGNED user-range code 0x80 (Amendment 3 clause 6)");
    check(opt_t::decode(std::to_integer<std::uint8_t>(folded[1])) == opt_t{.pl = true},
          "structured, and NO trailer: wire time is the outermost frame's, never a sample's");

    // The concatenation claim, made where it can be false: every sample's own bytes appear in
    // the fold unchanged, in order. A re-encoding fold would still round-trip and still derive
    // the right times — and would still fail this.
    std::size_t at = 4 + tr::wire::kBatchTimeChildBytes;
    for (const std::vector<std::byte>& f : s.frames) {
        check(same(std::span<const std::byte>(folded).subspan(at, f.size()), f),
              "the sample frame's own bytes ride the fold VERBATIM");
        at += f.size();
    }
    check(at == folded.size(), "... and the fold appends nothing after the last sample");
}

/** @brief Claim 2: a uniform stream derives every sample time and transmits none of them. */
void uniform_time_is_derived() {
    std::printf("§4.2.1 uniform — 0 bytes per sample on time:\n");
    const std::vector<std::byte> bytes = vector_bytes("stream/batch-time-roundtrip");
    const auto tlv = decode_all(bytes);
    check(tlv.has_value() && tr::wire::is_batch(*tlv), "the vector decodes as a BATCH record");
    if (!tlv) return;

    const auto b = tr::wire::read_batch(*tlv, kDt);
    check(b.has_value(), "and reads as the convention against the descriptor's dt_ns");
    if (!b) return;

    check(b->uniform() && b->offsets.empty(), "a declared rate ⇒ uniform ⇒ NO offset array");
    check(b->base_ns == kBase && b->size() == 3, "one base child, three sample frames");
    check(b->sample_time_ns(0) == kBase && b->sample_time_ns(1) == kBase + 1000 &&
              b->sample_time_ns(2) == kBase + 2000,
          "t(i) = base + i × dt_ns — derived, never read off the wire");
    check(!b->sample_time_ns(3).has_value(), "and an index past the fold answers nothing");

    // The byte claim the derivation exists for. Three 2-byte samples: 6 bytes of sample
    // payload out of a 34-byte record, and the ONLY time on the wire is the 12-byte base.
    check(bytes.size() == 34 && bytes.size() - 4 - tr::wire::kBatchTimeChildBytes == 18,
          "three 6-byte sample frames — no per-sample time bytes at all");
    std::vector<std::byte> with_offsets;
    tr::wire::emit_batch(with_offsets, kBase, three_samples().spans,
                         std::array<std::int32_t, 3>{0, 1000, 2000});
    check(with_offsets.size() == bytes.size() + 4 + 3 * 4,
          "... where spelling the same three times explicitly would cost 16 bytes more");
}

/** @brief Claim 3: which shape the children are is the DESCRIPTOR's fact, not the record's. */
void the_descriptor_decides_the_shape() {
    std::printf("§4.2.1 non-uniform — the packed i32 offset array:\n");
    const samples_t s = three_samples();
    constexpr std::array<std::int32_t, 3> kOffsets{0, 1500, -250};

    std::vector<std::byte> bytes;
    tr::wire::emit_batch(bytes, kBase, s.spans, kOffsets);
    check(bytes.size() == tr::wire::batch_wire_bytes(6 * 3, 3),
          "the non-uniform record is the uniform one plus ONE 16-byte offset child");

    const auto tlv = decode_all(bytes);
    check(tlv.has_value(), "the non-uniform record decodes");
    if (!tlv) return;
    check(tlv->children.size() == 5, "TIME + one packed offset child + three sample frames");

    const auto b = tr::wire::read_batch(*tlv, /*dt_ns=*/0);
    check(b.has_value(), "and reads as the convention against dt_ns = 0");
    if (!b) return;
    check(!b->uniform() && b->size() == 3, "dt_ns = 0 ⇒ non-uniform ⇒ three samples remain");
    check(b->sample_time_ns(0) == kBase && b->sample_time_ns(1) == kBase + 1500 &&
              b->sample_time_ns(2) == kBase - 250,
          "t(i) = base + offsets[i], and the offset is SIGNED — a sample may precede the base");

    // THE ABLATION. The same bytes, read against a declared rate, are four samples of which
    // the first is the offset run — a silently different reading, which is why `dt_ns` is a
    // parameter of the read and not something the record is asked for.
    const auto as_uniform = tr::wire::read_batch(*tlv, kDt);
    check(as_uniform.has_value() && as_uniform->size() == 4 && as_uniform->offsets.empty(),
          "the SAME bytes against dt_ns != 0 read as four samples — the descriptor decides");
    check(as_uniform->sample_time_ns(1) == kBase + 1000,
          "... and derive from the rate, since a uniform batch carries no offsets");
}

/** @brief Claim 4: the reader declines shapes the codec still carries. */
void a_readers_refusal_is_not_the_codecs() {
    std::printf("§User range — the graph never interprets a BATCH (claim 5):\n");

    // A user-range record that is NOT the convention: no TIME base at all.
    std::vector<std::byte> stray;
    {
        std::vector<std::byte> body;
        tr::wire::emit_value_le<std::uint16_t>(body, 0x0064);
        tr::wire::emit_tlv(stray, type_t::BATCH, opt_t{.pl = true}, body);
    }
    const auto stray_tlv = decode_all(stray);
    check(stray_tlv.has_value(), "a 0x80 record with no TIME child still DECODES");
    check(stray_tlv && !tr::wire::read_batch(*stray_tlv, kDt).has_value(),
          "... and the reader declines it — a refusal by the reader, never by the codec");
    if (stray_tlv) {
        check(same(tr::wire::encode(*stray_tlv), stray),
              "... while the same bytes round-trip byte-for-byte (the user range stands)");
    }

    // A non-uniform record whose offset run does not match its sample count: readable bytes,
    // an unreadable convention.
    std::vector<std::byte> mismatched;
    tr::wire::emit_batch(mismatched, kBase, three_samples().spans,
                         std::array<std::int32_t, 2>{0, 10});
    const auto bad = decode_all(mismatched);
    check(bad.has_value() && !tr::wire::read_batch(*bad, 0).has_value(),
          "an offset run that does not cover every sample is declined, not guessed at");

    // A foreign type code is not a BATCH however batch-shaped its children are.
    std::vector<std::byte> foreign;
    {
        std::vector<std::byte> body;
        tr::wire::emit_batch_time(body, kBase);
        tr::wire::emit_tlv(foreign, static_cast<type_t>(0x81), opt_t{.pl = true}, body);
    }
    const auto foreign_tlv = decode_all(foreign);
    check(foreign_tlv.has_value() && !tr::wire::is_batch(*foreign_tlv),
          "0x81 is not BATCH — one code is assigned, and it is 0x80");
}

/** @brief RFC-0025 §4.1: the class field, and what an absent word still means. */
void delivery_class_bits() {
    std::printf("§4.1 delivery_class — bits 6-7 of the packed word:\n");
    using tr::graph::delivery_class_t;
    using tr::graph::delivery_policy_t;
    static_assert(delivery_policy_t{}.delivery_class() == delivery_class_t::CONFLATE,
                  "absent ⇒ all-zero ⇒ conflate: today's behaviour, byte-identically");
    static_assert(delivery_policy_t{0x0040}.delivery_class() == delivery_class_t::IMMEDIATE);
    static_assert(delivery_policy_t{0x0080}.delivery_class() == delivery_class_t::BATCH);
    static_assert(delivery_policy_t{0x00C0}.delivery_class() == delivery_class_t::STREAM);
    static_assert(delivery_policy_t{0xFF3F}.delivery_class() == delivery_class_t::CONFLATE,
                  "every OTHER bit set leaks nothing into the class");
    static_assert(delivery_policy_t{0x00C0}.reliability() == 0 &&
                      delivery_policy_t{0x00C0}.priority() == 0 &&
                      !delivery_policy_t{0x00C0}.durability_request(),
                  "and the class leaks into no honoured field below it");
    check(true, "the §4.1 packing is pinned by static_assert (see the source)");
}

/** @brief The three samples as VIEWS over stable buffers — what an app already holds. */
struct sample_views_t {
    samples_t frames = three_samples();
    std::vector<tr::view::view_t> views;

    sample_views_t() {
        for (const std::vector<std::byte>& f : frames.frames)
            views.push_back(
                tr::view::view_t::over(tr::view::borrow_const(std::span<const std::byte>(f))));
    }
};

/** @brief The non-uniform offsets both composition vectors carry. */
constexpr std::array<std::int32_t, 3> kComposedOffsets{0, 1500, -250};

/**
 * @brief Amendment 4 §4.1.3 clause 4 — a composition REFERENCES the app's sample bytes, and one
 *        layout serves both carriages.
 */
void composition_references_rather_than_copies() {
    std::printf("§4.1.3 compose — one owned head link, the samples referenced:\n");
    const sample_views_t s;

    const tr::view::rope_t standalone =
        tr::wire::compose_batch(tr::mem::heap_backend(), tr::wire::batch_carriage_t::STANDALONE,
                                kBase, s.views, kComposedOffsets);
    check(standalone.link_count() == 1 + s.views.size(),
          "ONE owned head link plus one link per sample — nothing else is allocated");
    check(standalone.links()[0].bytes().size() == tr::wire::batch_head_bytes(6 * 3, 3),
          "the head link is exactly the record header + TIME base + offset child");

    // THE zero-copy claim, made structurally rather than by size: every sample link points at
    // the app's own buffer. A composer that copied would produce identical BYTES and fail here.
    for (std::size_t i = 0; i < s.views.size(); ++i) {
        check(standalone.links()[i + 1].bytes().data() == s.frames.frames[i].data(),
              "the sample link IS the app's buffer — a refcounted reference, not a copy");
    }

    std::vector<std::byte> flat;
    standalone.walk(
        [&flat](std::span<const std::byte> b) { flat.insert(flat.end(), b.begin(), b.end()); });
    check(same(flat, vector_bytes("stream/batch-composed-standalone")),
          "stream/batch-composed-standalone is byte-exact against the composer");

    const tr::view::rope_t folded =
        tr::wire::compose_batch(tr::mem::heap_backend(), tr::wire::batch_carriage_t::FOLDED, kBase,
                                s.views, kComposedOffsets);
    std::vector<std::byte> flat_folded;
    folded.walk([&flat_folded](std::span<const std::byte> b) {
        flat_folded.insert(flat_folded.end(), b.begin(), b.end());
    });
    check(same(flat_folded, vector_bytes("stream/batch-composed-folded")),
          "stream/batch-composed-folded is byte-exact against the composer");

    // The carriage rule, as a difference: ONE byte, and it is the header type.
    check(flat.size() == flat_folded.size(), "the two carriages are the same length");
    std::size_t differing = 0;
    for (std::size_t i = 0; i < flat.size(); ++i)
        if (flat[i] != flat_folded[i]) ++differing;
    check(differing == 1 && std::to_integer<std::uint8_t>(flat[0]) == 0x80 &&
              std::to_integer<std::uint8_t>(flat_folded[0]) == 0x01,
          "exactly ONE byte differs — the header type (0x80 BATCH vs 0x01 VALUE)");

    // One layout, not two: the contiguous emitter must reach the same bytes in both carriages.
    for (const auto carriage :
         {tr::wire::batch_carriage_t::STANDALONE, tr::wire::batch_carriage_t::FOLDED}) {
        std::vector<std::byte> emitted;
        tr::wire::emit_batch(emitted, kBase, s.frames.spans, kComposedOffsets, carriage);
        check(
            same(emitted, carriage == tr::wire::batch_carriage_t::STANDALONE ? flat : flat_folded),
            "emit_batch and compose_batch share ONE layout implementation, per carriage");
    }
}

}  // namespace

/** @brief Runs every BATCH-record check; non-zero exit ⇒ some check failed. */
int main() {
    fold_is_a_concatenation();
    uniform_time_is_derived();
    the_descriptor_decides_the_shape();
    a_readers_refusal_is_not_the_codecs();
    composition_references_rather_than_copies();
    delivery_class_bits();
    return tr::testing::summary("batch");
}
