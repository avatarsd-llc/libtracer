/**
 * @file
 * @brief PATH_REF (0x14) codec test — the bound-path wire form of RFC-0024 §4.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Locks the structural half of RFC-0024's new address form, which is all a codec
 * can hold: the 4-byte envelope and its `PL = 0` / `LL = 0` MUSTs (§4.2), the
 * fixed 8-byte `(u32 index, u32 generation)` little-endian element (§4.4), the
 * count-is-`length / 8` rule and the 255-element bound (§4.3), and the reject a
 * violation of any of them earns — `tr::frame::invalid`, the same answer a set
 * reserved bit gets, never UB and never a partial parse.
 *
 * What an element MEANS is deliberately absent: an element is node-scoped, so
 * the bounds check into a host's vertex map, the generation compare and the ACL
 * re-check at the deref'd vertex (RFC-0024 §5-§6) belong to the router and are
 * not testable here. This file also pins the shared conformance vectors under
 * tests/conformance/vectors/v1/path-ref/ against the programmatic builder, so a
 * divergence between `emit_path_ref` and the published bytes fails here rather
 * than only in a binding's byte pin.
 */

#include "libtracer/path_ref.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/frame.hpp"
#include "libtracer/grammar.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"

namespace {

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief Encode a `PATH_REF` over @p elements, or an empty buffer past the §4.3 bound. */
std::vector<std::byte> emit(std::span<const tr::wire::path_ref_element_t> elements) {
    std::vector<std::byte> out;
    if (!tr::wire::emit_path_ref(out, elements)) out.clear();
    return out;
}

/** @brief Lowercase hex of @p bytes — the spelling the vectors' `hex` field uses. */
std::string hex(std::span<const std::byte> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::byte b : bytes) {
        const auto v = std::to_integer<std::uint8_t>(b);
        out.push_back(kDigits[v >> 4]);
        out.push_back(kDigits[v & 0x0F]);
    }
    return out;
}

/** @brief A hand-built `PATH_REF` frame: raw @p opt and @p length over a raw @p body. */
std::vector<std::byte> raw_frame(std::uint8_t opt, std::uint16_t length,
                                 std::span<const std::byte> body) {
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>(0x14));
    out.push_back(static_cast<std::byte>(opt));
    out.push_back(static_cast<std::byte>(length & 0xFF));
    out.push_back(static_cast<std::byte>(length >> 8));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/** @brief True iff decoding @p bytes fails with exactly `FRAME_INVALID`. */
bool rejected_invalid(const std::vector<std::byte>& bytes) {
    const auto dec = tr::wire::decode(bytes);
    return !dec.has_value() && dec.error() == tr::wire::err_t::FRAME_INVALID;
}

}  // namespace

int main() {
    using namespace tr::wire;

    std::printf("PATH_REF (0x14) codec — RFC-0024 §4:\n");

    // ===== the envelope and the element, byte for byte (§4.2, §4.4) =====================
    {
        const path_ref_element_t elems[] = {{.index = 7, .generation = 3},
                                            {.index = 42, .generation = 1}};
        const std::vector<std::byte> bytes = emit(elems);
        check(bytes.size() == 4 + 8 * 2, "a 2-host bound path is 4 + 8H = 20 bytes");
        check(hex(bytes) == "1400100007000000030000002a00000001000000",
              "the envelope is 14 00 <u16 LE length> and each element is index++generation LE");
        check(std::to_integer<std::uint8_t>(bytes[1]) == 0x00,
              "opt is 0x00 — PL=0 and LL=0 are both MUSTs, so no bit is set");
    }

    // ===== round-trip through the generic codec ========================================
    {
        const path_ref_element_t elems[] = {{.index = 7, .generation = 3},
                                            {.index = 19, .generation = 12},
                                            {.index = 42, .generation = 1}};
        const std::vector<std::byte> bytes = emit(elems);
        const auto dec = decode(bytes);
        check(dec.has_value(), "a 3-host bound path decodes");
        if (dec) {
            check(dec->type == type_t::PATH_REF, "the decoded type is PATH_REF (0x14)");
            check(!dec->opt.pl && dec->children.empty(),
                  "the body is opaque to the generic codec — a record array, not children");
            check(encode(*dec) == bytes, "encode(decode(bytes)) == bytes");
            check(path_ref_element_count(dec->payload.size()) == 3,
                  "the element count is length / 8 — there is no count field on the wire");
            for (std::size_t i = 0; i < 3; ++i) {
                check(path_ref_element_at(dec->payload, i) == elems[i],
                      "element " + std::to_string(i) + " reads back index and generation");
            }
        }
    }

    // ===== the widths are u32 each, and the top of the range survives (§4.4) ============
    {
        const path_ref_element_t saturated[] = {
            {.index = 0xFFFF'FFFFu, .generation = 0xFFFF'FFFFu}};
        const std::vector<std::byte> bytes = emit(saturated);
        const auto dec = decode(bytes);
        check(dec.has_value() && path_ref_element_at(dec->payload, 0) == saturated[0],
              "a saturated u32 generation round-trips — the width is not truncated anywhere");
    }

    // ===== the count bound, from both sides (§4.3) ======================================
    {
        std::vector<path_ref_element_t> at_bound(kMaxPathRefElements);
        for (std::size_t i = 0; i < at_bound.size(); ++i) {
            at_bound[i] = {.index = static_cast<std::uint32_t>(i),
                           .generation = static_cast<std::uint32_t>((i * 7) % 65536)};
        }
        const std::vector<std::byte> bytes = emit(at_bound);
        check(bytes.size() == 4 + kMaxPathRefBodyBytes,
              "255 elements is 2040 body bytes — 2044 total, still a u16 length");
        check(decode(bytes).has_value(), "the bound itself (255 elements) decodes");

        std::vector<path_ref_element_t> over(kMaxPathRefElements + 1);
        check(emit(over).empty(), "emitting 256 elements refuses rather than truncating");

        // Hand-built, because the emitter refuses to produce it: one element over the bound.
        const std::vector<std::byte> body(kMaxPathRefBodyBytes + kPathRefElementBytes,
                                          std::byte{0});
        check(rejected_invalid(raw_frame(0x00, static_cast<std::uint16_t>(body.size()), body)),
              "256 elements on the wire is FRAME_INVALID");
    }

    // ===== the structural rejects — every one is FRAME_INVALID, never UB ================
    {
        const std::vector<std::byte> one(kPathRefElementBytes, std::byte{0});
        std::vector<std::byte> ragged = one;
        ragged.insert(ragged.end(), 4, std::byte{0xAA});
        check(rejected_invalid(raw_frame(0x00, static_cast<std::uint16_t>(ragged.size()), ragged)),
              "a length that is not a multiple of 8 is FRAME_INVALID");
        check(rejected_invalid(raw_frame(0x40, static_cast<std::uint16_t>(one.size()), one)),
              "opt.PL = 1 on a PATH_REF is FRAME_INVALID (the body is not child TLVs)");

        // LL widens the header to 6 bytes and the length field to u32; the body still has to
        // be one whole element, so this frame is well-formed under every rule EXCEPT LL=0.
        std::vector<std::byte> ll_frame{std::byte{0x14}, std::byte{0x08}, std::byte{0x08},
                                        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
        ll_frame.insert(ll_frame.end(), one.begin(), one.end());
        check(rejected_invalid(ll_frame),
              "opt.LL = 1 on a PATH_REF is FRAME_INVALID (u32 length is unreachable under 2040 B)");

        check(decode(raw_frame(0x00, 0, {})).has_value(),
              "an empty body is structurally well-formed — a route with no hops is the "
              "router's to refuse, not the codec's");
    }

    // ===== the reject survives nesting: a PATH_REF child inside a structured parent ======
    {
        const std::vector<std::byte> one(kPathRefElementBytes, std::byte{0});
        std::vector<std::byte> child = raw_frame(0x40, static_cast<std::uint16_t>(one.size()), one);
        std::vector<std::byte> parent;
        emit_header(parent, type_t::FWD, opt_t{.pl = true}, child.size());
        parent.insert(parent.end(), child.begin(), child.end());
        check(rejected_invalid(parent),
              "a malformed PATH_REF nested in a FWD rejects the whole frame — the rule is in "
              "the shared header grammar, so it fires at every depth");
    }

    // ===== the published conformance vectors, pinned against the builder =================
    {
        const path_ref_element_t one[] = {{.index = 7, .generation = 3}};
        const path_ref_element_t two[] = {{.index = 7, .generation = 3},
                                          {.index = 42, .generation = 1}};
        const path_ref_element_t three[] = {{.index = 7, .generation = 3},
                                            {.index = 19, .generation = 12},
                                            {.index = 42, .generation = 1}};
        // The published input.bin of path-ref/ref-1host, ref-2host and ref-3host, hex-pinned
        // here so a change to emit_path_ref that no longer produces the published bytes fails
        // in this suite rather than only in a binding's byte pin.
        check(hex(emit(one)) == "140008000700000003000000",
              "emit_path_ref reproduces path-ref/ref-1host's input.bin (12 B)");
        check(hex(emit(two)) == "1400100007000000030000002a00000001000000",
              "emit_path_ref reproduces path-ref/ref-2host's input.bin (20 B)");
        check(hex(emit(three)) == "140018000700000003000000130000000c0000002a00000001000000",
              "emit_path_ref reproduces path-ref/ref-3host's input.bin (28 B)");
        // The two structurally-invalid vectors, pinned by the same rule that rejects them.
        const std::vector<std::byte> ragged_body = {
            std::byte{0x07}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
            std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD}};
        const std::vector<std::byte> ragged = raw_frame(0x00, 12, ragged_body);
        check(hex(ragged) == "14000c000700000003000000aabbccdd" && rejected_invalid(ragged),
              "path-ref/ref-len-not-multiple-of-8's reject.bin decodes to FRAME_INVALID");
        std::vector<std::byte> pl_set = emit(two);
        pl_set[1] = std::byte{0x40};
        check(hex(pl_set) == "1440100007000000030000002a00000001000000" && rejected_invalid(pl_set),
              "path-ref/ref-pl-set's reject.bin decodes to FRAME_INVALID");
    }

    std::printf(g_failures == 0 ? "\nPATH_REF: PASS\n" : "\nPATH_REF: FAIL (%d)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
