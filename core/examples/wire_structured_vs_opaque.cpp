/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — `opt.PL`: an opaque TLV holds bytes, a structured TLV holds children.
 *
 * TLV composition (L3) composes MEANING: the leaf is an opaque TLV (`opt.PL = 0`, its body
 * is payload bytes), the composite is a structured TLV (`opt.PL = 1`, its body is a packed
 * run of sub-TLVs) whose type code says what the children mean (`CONTEXT.md` §Two
 * compositions). One bit decides which, and it decides it for the SAME body bytes: this
 * example encodes `POINT{VALUE, VALUE}`, clears `PL` on the copy, and decodes the very same
 * bytes as one opaque payload of exactly the children's length.
 *
 * That is the decoupling from memory composition (L1): meaning is `opt.PL`, storage is the
 * view/rope chain, and neither constrains the other.
 *
 * Runs under ctest as `example_wire_structured_vs_opaque`; returns non-zero on any failure.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "libtracer/tracer.hpp"

namespace {

using tr::wire::opt_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

}  // namespace

int main() {
    bool ok = true;
    const std::vector<std::byte> x{std::byte{0x11}, std::byte{0x22}};
    const std::vector<std::byte> y{std::byte{0x33}, std::byte{0x44}};

    tlv_t point;
    point.type = type_t::POINT;
    point.opt.pl = true;  // structured: the body is children, not bytes
    point.children.push_back(tlv_t{.type = type_t::VALUE, .payload = std::span(x)});
    point.children.push_back(tlv_t{.type = type_t::VALUE, .payload = std::span(y)});

    std::vector<std::byte> frame = tr::wire::encode(point);
    std::printf("POINT{VALUE,VALUE} is %zu bytes: 4 header + 2 x (4 header + 2 body)\n",
                frame.size());

    const auto structured = tr::wire::decode(frame);
    check(ok, structured.has_value(), "the structured frame decodes");
    check(ok, structured && structured->children.size() == 2, "PL = 1 gives two children");
    check(ok, structured && structured->payload.empty(), "and no opaque payload at all");

    // The SAME body bytes, read with PL cleared. Byte 1 is the root's opt (see
    // wire_tlv_header): clearing bit 6 tells the decoder "this body is payload".
    std::vector<std::byte> as_opaque = frame;
    as_opaque[1] = static_cast<std::byte>(static_cast<std::uint8_t>(as_opaque[1]) & 0xBFu);

    const auto opaque = tr::wire::decode(as_opaque);
    check(ok, opaque.has_value(), "the same bytes with PL = 0 also decode");
    check(ok, opaque && opaque->children.empty(), "PL = 0 gives no children");
    check(ok, opaque && opaque->payload.size() == frame.size() - 4,
          "the payload IS the children region, byte for byte");
    return ok ? 0 : 1;
}
