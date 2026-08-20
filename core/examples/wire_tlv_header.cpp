/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — the TLV header: `<type> <opt> <length>`, and the `opt` byte.
 *
 * Every TLV on the wire starts with four bytes: a type code, the one-byte `opt` bitfield,
 * and a little-endian `u16` length — widening to a `u32` (six bytes in all) when the body
 * exceeds `0xFFFF` (`docs/reference/01-data-format.md` §header + opt). There is no varint
 * and no CRC in the header. Two facts a first-contact reader needs are checked here: the
 * `opt` byte round-trips through `opt_t::decode`/`encode` exactly, with bits 7 and 0
 * reserved-MUST-be-zero; and the length width is `emit_tlv`'s decision, not the caller's —
 * an oversize body widens the header whatever `opt.ll` was passed as (#924).
 *
 * Runs under ctest as `example_wire_tlv_header`; returns non-zero on any failed check.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/** @brief The raw `opt` byte of the TLV that starts at the front of @p frame. */
std::uint8_t opt_byte(const std::vector<std::byte>& frame) {
    return static_cast<std::uint8_t>(frame[1]);
}

}  // namespace

int main() {
    bool ok = true;

    // The opt byte is a bitfield, not an enum: pack it, unpack it, get the same bits back.
    const opt_t set{.pl = true, .ts = false, .cr = true, .ll = false, .cw = false, .tf = false};
    std::printf("opt{pl,cr} encodes to 0x%02X\n", set.encode());
    check(ok, opt_t::decode(set.encode()) == set, "opt_t encode/decode round-trips exactly");
    check(ok, !opt_t::reserved_set(set.encode()), "a well-formed opt leaves bits 7 and 0 clear");
    check(ok, opt_t::reserved_set(0x81), "a set reserved bit is detectable before any parse");

    // A small body: the 4-byte header, length as u16 LE, LL clear.
    const std::vector<std::byte> small(4, std::byte{0xAB});
    std::vector<std::byte> narrow;
    tr::wire::emit_tlv(narrow, type_t::VALUE, opt_t{}, small);
    std::printf("4-byte body -> %zu bytes on the wire, opt = 0x%02X\n", narrow.size(),
                opt_byte(narrow));
    check(ok, narrow.size() == 4 + small.size(), "a small TLV carries a 4-byte header");
    check(ok, (opt_byte(narrow) & 0x08) == 0, "the LL bit stays clear for a u16 length");

    // An oversize body widens the header to six bytes and sets LL — even though the opt
    // passed in said otherwise. emit_tlv owns the width decision; the caller does not.
    const std::vector<std::byte> big(0x1'0000, std::byte{0xCD});
    std::vector<std::byte> wide;
    tr::wire::emit_tlv(wide, type_t::VALUE, opt_t{.ll = false}, big);
    std::printf("65536-byte body -> %zu bytes on the wire, opt = 0x%02X\n", wide.size(),
                opt_byte(wide));
    check(ok, wide.size() == 6 + big.size(), "an oversize body widens the header to 6 bytes");
    check(ok, (opt_byte(wide) & 0x08) != 0, "and emit_tlv sets LL itself, ignoring the opt given");
    return ok ? 0 : 1;
}
