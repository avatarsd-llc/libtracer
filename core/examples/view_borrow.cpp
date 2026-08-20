/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — `borrow`: route the application's OWN bytes, without owning them.
 *
 * The L0/L1 substrate is a binding layer, and the extreme of the spectrum is the
 * **transparent byte router** (ADR-0012, `CONTEXT.md` §Memory-binding spectrum): point a
 * segment at an MMIO register, a program variable, or a const ROM table and libtracer routes
 * those bytes — no copy, no CRC imposed. `tr::view::borrow` allocates only the ~32-byte
 * `segment_t` control block; the payload pointer is the caller's, and the lifetime is the
 * caller's promise.
 *
 * The contrast that makes the choice visible is `over_bytes`, which COPIES into a fresh
 * segment. Both give a `view_t`; only one keeps the pointer. The example asserts pointer
 * identity for the borrow and non-identity for the copy, then writes through the application
 * buffer and reads the new value back out of the borrowed view — live, not a snapshot.
 *
 * Runs under ctest as `example_view_borrow`; returns non-zero on any failed check.
 */

#include <array>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>

#include "libtracer/tracer.hpp"

namespace {

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/** @brief A const table standing in for ROM — the `borrow_const` case. */
constexpr std::array<std::byte, 4> kRomTable{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                             std::byte{0xEF}};

}  // namespace

int main() {
    bool ok = true;
    std::array<std::byte, 8> app{};  // the application's buffer — libtracer never owns it
    app[0] = std::byte{0x01};

    const tr::view::view_t borrowed = tr::view::view_t::over(tr::view::borrow(app));
    std::printf("borrowed view: %zu bytes at the app's own address\n", borrowed.length);
    check(ok, borrowed.bytes().data() == app.data(), "borrow keeps the caller's pointer");
    check(ok, borrowed.length == app.size(), "over the caller's whole buffer");

    // Live, not a snapshot: the application writes and the view already sees it.
    app[0] = std::byte{0x02};
    check(ok, borrowed.bytes()[0] == std::byte{0x02}, "a write through the app buffer is visible");

    const auto copied = tr::view::over_bytes(app);
    check(ok, copied.has_value(), "over_bytes produced a view");
    if (!copied) return 1;
    check(ok, copied->bytes().data() != app.data(), "over_bytes COPIES into a fresh segment");
    app[0] = std::byte{0x03};
    check(ok, copied->bytes()[0] == std::byte{0x02}, "so the copy froze the value it was given");

    const tr::view::view_t rom = tr::view::view_t::over(tr::view::borrow_const(kRomTable));
    check(ok, rom.bytes().data() == kRomTable.data(), "borrow_const borrows read-only bytes too");
    check(ok, rom.is_host(), "and reports HOST space — the CPU may dereference it");
    return ok ? 0 : 1;
}
