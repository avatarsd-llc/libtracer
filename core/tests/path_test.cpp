/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Path parsing / validation (docs/reference/03-addressing.md). Focus: reserved-
 * character rejection in NAME segments (§Reserved characters), the structural
 * limits, and that the field tail (dot-separated, with [N]/[]) still parses.
 */

#include <cstdio>
#include <string_view>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::path_t;
using tr::graph::status_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

void ok_parse(std::string_view text) {
    const auto r = path_t::parse(text);
    check(r.has_value(), text);
}

void rejected(std::string_view text) {
    const auto r = path_t::parse(text);
    check(!r.has_value() && r.error() == status_t::INVALID_PATH, text);
}

}  // namespace

int main() {
    std::printf("Valid paths parse:\n");
    ok_parse("/");
    ok_parse("/sensor/temp");
    ok_parse("/a/b/c");
    ok_parse("/i2c-bus/0x68/accel");
    ok_parse("/x:settings.reliability");  // dot is the field-chain separator (not a NAME char)
    ok_parse("/x:subscribers[]");         // append field
    ok_parse("/x:subscribers[3]");        // indexed field

    std::printf("Reserved characters in a NAME segment are rejected (reference/03 §Reserved):\n");
    rejected("/a.b");  // '.' is the field separator, illegal in a NAME
    rejected("/a*b");  // '*' wildcard
    rejected("/a?b");  // '?' reserved
    rejected("/sensor/te.mp");
    rejected("/a/b*/c");

    std::printf("Structural limits still hold:\n");
    rejected("relative/no/root");  // must be rooted at '/'
    rejected("/a//b");             // empty segment

    std::printf(g_failures == 0 ? "\nPATH: PASS\n" : "\nPATH: FAIL (%d)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
