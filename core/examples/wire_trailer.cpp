/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — the optional trailer: a per-TLV CRC and a per-TLV timestamp.
 *
 * Integrity is OPT-IN and it rides at the END, not in the header (`CONTEXT.md` §Wire format;
 * `docs/reference/01-data-format.md` §trailer). `opt.cr` says a CRC-32C follows the body and
 * `encode` recomputes it, so a single flipped payload byte turns the frame into
 * `err_t::FRAME_CRC_FAIL` — a receiver's verdict about the bytes, not about the sender's
 * intent. `opt.ts` says a wire-time stamp follows; `stamp_ts` sets the bit and the value
 * together, which matters because a TLV claiming `opt.ts` with no value in hand is REFUSED
 * loudly by `encode` (an empty vector, #1109) rather than emitted with a silent zero.
 *
 * Runs under ctest as `example_wire_trailer`; returns non-zero on any failed check.
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
    const std::vector<std::byte> body(4, std::byte{0x5A});

    tlv_t v;
    v.type = type_t::VALUE;
    v.opt.cr = true;  // ask for the CRC trailer; encode computes it
    v.payload = std::span(body);

    std::vector<std::byte> frame = tr::wire::encode(v);
    std::printf("VALUE with a CRC trailer: %zu bytes (4 header + 4 body + 4 CRC-32C)\n",
                frame.size());
    const auto good = tr::wire::decode(frame);
    check(ok, good.has_value(), "the frame decodes");
    check(ok, good && good->trailer && good->trailer->crc.has_value(), "and carries a CRC");

    // One flipped payload byte. The CRC is what makes that a verdict rather than a guess.
    std::vector<std::byte> corrupt = frame;
    corrupt[4] ^= std::byte{0x01};
    const auto bad = tr::wire::decode(corrupt);
    check(ok, !bad && bad.error() == tr::wire::err_t::FRAME_CRC_FAIL,
          "a single flipped body byte is FRAME_CRC_FAIL");

    // The timestamp half. stamp_ts sets opt.ts, clears opt.tf and writes the value at once.
    tlv_t stamped;
    stamped.type = type_t::VALUE;
    stamped.payload = std::span(body);
    tr::wire::stamp_ts(stamped, 1'700'000'000'000'000'000);
    const auto rt = tr::wire::decode(tr::wire::encode(stamped));
    check(ok, rt && rt->trailer && rt->trailer->ts, "a stamped TLV round-trips its timestamp");
    check(ok, rt && rt->trailer->ts->value == 1'700'000'000'000'000'000, "with the value intact");

    // The loud refusal: the bit without the value is never emitted as a silent zero.
    tlv_t claims_ts;
    claims_ts.type = type_t::VALUE;
    claims_ts.opt.ts = true;  // set by hand, with no trailer value behind it
    claims_ts.payload = std::span(body);
    check(ok, tr::wire::encode(claims_ts).empty(),
          "opt.ts with no value refuses to encode at all (empty vector)");
    return ok ? 0 : 1;
}
