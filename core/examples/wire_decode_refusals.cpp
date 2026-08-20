/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — the four verdicts `decode` returns, and who each one accuses.
 *
 * `decode` answers `std::expected<tlv_t, err_t>`, and the error side is the RFC-0002 registry
 * (`libtracer/error.hpp`) rather than a decode-private vocabulary. The three a reader hits
 * first are permanent accusations against the bytes: `FRAME_TRUNCATED` (the frame stops
 * mid-TLV), `FRAME_INVALID` (well-formed prefix, trailing bytes after it — or a reserved
 * `opt` bit set) and `FRAME_CRC_FAIL` (see `wire_trailer`).
 *
 * The fourth is different in kind. `TLV_NESTING_TOO_DEEP` means "exceeds **this receiver's**
 * decode resources" (RFC-0006, `CONTEXT.md` §Resource bound): the walk stack starts in inline
 * slots and spills into a caller-injected `block_source_t`, so the SAME bytes that a
 * heap-spilled decode accepts are refused by `decode(bytes, mem::null_source())` — the
 * spelling of "no spill at all". Nothing here asserts a depth number, because there is no
 * constant to assert: the bound is the source the caller passed.
 *
 * Runs under ctest as `example_wire_decode_refusals`; returns non-zero on any failed check.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <utility>
#include <vector>

#include "libtracer/tracer.hpp"

namespace {

using tr::wire::err_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/** @brief `depth` nested structured `POINT`s wrapping one `VALUE` over @p body. */
tlv_t nest(std::span<const std::byte> body, int depth) {
    tlv_t cur{.type = type_t::VALUE, .payload = body};
    for (int i = 0; i < depth; ++i) {
        tlv_t parent;
        parent.type = type_t::POINT;
        parent.opt.pl = true;
        parent.children.push_back(std::move(cur));
        cur = std::move(parent);
    }
    return cur;
}

}  // namespace

int main() {
    bool ok = true;
    const std::vector<std::byte> body(4, std::byte{0x5A});

    std::vector<std::byte> frame = tr::wire::encode(tlv_t{.type = type_t::VALUE, .payload = body});
    check(ok, tr::wire::decode(frame).has_value(), "the reference frame decodes cleanly");

    std::vector<std::byte> short_frame(frame.begin(), frame.end() - 1);
    const auto truncated = tr::wire::decode(short_frame);
    check(ok, !truncated && truncated.error() == err_t::FRAME_TRUNCATED,
          "one byte short is FRAME_TRUNCATED");

    std::vector<std::byte> extra = frame;
    extra.push_back(std::byte{0x00});
    const auto trailing = tr::wire::decode(extra);
    check(ok, !trailing && trailing.error() == err_t::FRAME_INVALID,
          "a byte after the one TLV is FRAME_INVALID — decode consumes the whole input");

    std::vector<std::byte> reserved = frame;
    reserved[1] |= std::byte{0x01};  // bit 0 of opt is reserved-MUST-be-zero
    const auto bad_opt = tr::wire::decode(reserved);
    check(ok, !bad_opt && bad_opt.error() == err_t::FRAME_INVALID,
          "a set reserved opt bit is FRAME_INVALID");

    // The receiver-resource verdict. Same bytes, two injected spill sources, two answers.
    const std::vector<std::byte> deep = tr::wire::encode(nest(body, 16));
    std::printf("a 16-deep frame is %zu bytes\n", deep.size());
    const auto no_spill = tr::wire::decode(deep, tr::mem::null_source());
    check(ok, !no_spill && no_spill.error() == err_t::TLV_NESTING_TOO_DEEP,
          "with a source that serves nothing, the walk stack cannot spill");
    check(ok, tr::wire::decode(deep, tr::mem::heap_source()).has_value(),
          "and the very same bytes decode once the caller injects one that can");
    return ok ? 0 : 1;
}
