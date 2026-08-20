/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — `tlv_view_t`: decoding a frame whose bytes are scattered across a rope.
 *
 * This is the L1↔L2 hinge. Memory composition and TLV composition are orthogonal
 * (`CONTEXT.md` §Two compositions), so a link boundary may fall ANYWHERE — including
 * mid-header — and the decoder must not care. `tlv_view_t::over` (ADR-0053) adopts the rope
 * as one lazy TLV: it parses the root header with the CRC walk DEFERRED, and nothing that is
 * not accessed is ever decoded.
 *
 * The frame here is split deliberately inside the first child's header, so the two links a
 * transport happened to receive it in are not a TLV boundary at all. `children().next()`
 * still materializes one child at a time, `verify()` is the deferred CRC walk run when a
 * consumer wants it, and `materialize()` is the SINGLE explicit copy point — everything the
 * lazy tier deferred, paid once, by whoever asked.
 *
 * Runs under ctest as `example_wire_lazy_view`; returns non-zero on any failed check.
 */

#include <cstddef>
#include <cstdio>
#include <span>
#include <vector>

#include "libtracer/tlv_view.hpp"
#include "libtracer/tracer.hpp"

namespace {

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
    point.opt.pl = true;
    point.opt.cr = true;  // a CRC trailer, so verify() has real work to do
    point.children.push_back(tlv_t{.type = type_t::VALUE, .payload = std::span(x)});
    point.children.push_back(tlv_t{.type = type_t::VALUE, .payload = std::span(y)});
    const std::vector<std::byte> frame = tr::wire::encode(point);

    // Split at byte 6: the root header is bytes 0-3, so this cuts the FIRST CHILD's header
    // in half. Two segments, two links, one logical frame — assembled by chaining.
    const std::size_t cut = 6;
    tr::view::rope_t rope;
    rope.append(*tr::view::over_bytes(std::span(frame).first(cut)));
    rope.append(*tr::view::over_bytes(std::span(frame).subspan(cut)));
    std::printf("frame of %zu bytes delivered as %zu links, split mid-header at %zu\n",
                frame.size(), rope.link_count(), cut);

    const auto lazy = tr::wire::tlv_view_t::over(rope);
    check(ok, lazy.has_value(), "tlv_view_t::over anchors the bounds without walking the body");
    if (!lazy) return 1;
    check(ok, lazy->type() == type_t::POINT, "the root type reads back");
    check(ok, lazy->structured() && lazy->body_size() == frame.size() - 4 - 4,
          "as does the body size — header and CRC trailer excluded");

    auto kids = lazy->children();
    const auto first = kids.next();
    const auto second = kids.next();
    check(ok, first && *first && (*first)->type() == type_t::VALUE, "one child header at a time");
    check(ok, second && *second, "then the next, straight across the link boundary");
    const auto past_end = kids.next();
    check(ok, past_end && !*past_end && kids.exhausted(), "and the region ends cleanly");

    check(ok, lazy->verify().has_value(), "verify() is the deferred CRC walk, run on demand");

    const auto flat = lazy->materialize();
    check(ok, flat.has_value(), "materialize() is the one explicit copy point");
    check(ok, flat && flat->root.children.size() == 2,
          "and it yields the same eager tree an ordinary decode would");
    return ok ? 0 : 1;
}
