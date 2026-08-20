/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a DEVICE link, and why refusing to flatten it is PERMANENT.
 *
 * A segment carries the address space of the backend that made it. `HOST` bytes are
 * CPU-addressable; `DEVICE` bytes (GPU/accelerator memory, ADR-0024) are not, and the codec
 * must never dereference them. A rope may be HETEROGENEOUS — a host header link chained to a
 * device payload link — and `all_host()` is the one question a host-side operation asks
 * before touching bytes.
 *
 * The pay-off is in the error channel. `flatten_err_t::NOT_HOST` is a property of the rope
 * itself: no retry ever fixes it, and the payload must go out via its device path. That is a
 * different verdict from `NO_MEMORY`, which is transient backpressure (see
 * `view_pool_backend`) — collapsing the two is exactly what let a local OOM be reported to a
 * peer as a malformed frame (#917).
 *
 * `borrow_device` tags ordinary host memory `DEVICE` and registers no byte-mover, so
 * `mem::transfer` refuses it — a real device backend lives in the `backends/` tier and
 * registers its own. Nothing here is conditional: every verdict below is deterministic in a
 * stock build with no accelerator present.
 *
 * Runs under ctest as `example_view_device_rope`; returns non-zero on any failed check.
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

}  // namespace

int main() {
    bool ok = true;
    std::array<std::byte, 8> header_bytes{};
    std::array<std::byte, 8> payload_bytes{};

    const tr::view::view_t host = tr::view::view_t::over(tr::view::borrow(header_bytes));
    tr::view::segment_ptr_t dev_seg = tr::view::borrow_device(payload_bytes);
    const tr::view::view_t device = tr::view::view_t::over(dev_seg);
    check(ok, host.is_host() && !host.is_device(), "a borrowed host link is CPU-addressable");
    check(ok, device.is_device(), "a borrow_device link reports DEVICE space");

    tr::view::rope_t frame;
    frame.append(host);
    frame.append(device);
    std::printf("heterogeneous rope: %zu links, %zu bytes, all_host=%d\n", frame.link_count(),
                frame.total_length(), static_cast<int>(frame.all_host()));
    check(ok, !frame.all_host(), "one DEVICE link makes the whole rope non-host");

    const auto refused = frame.try_flatten();
    check(ok, !refused && refused.error() == tr::view::flatten_err_t::NOT_HOST,
          "try_flatten refuses it as NOT_HOST — permanent, and never a retry");

    // The out-of-core arm: no backend registered a byte-mover for this segment, so the
    // transfer is declined by value rather than guessed at with a memcpy.
    std::array<std::byte, 8> staging{};
    check(ok, !tr::mem::transfer(dev_seg.get(), staging, tr::mem::io_dir_t::DEVICE_TO_CPU),
          "and mem::transfer declines a DEVICE segment no backends/ module claims");

    // The host half of the same rope still flattens — the refusal is about the link, not the
    // rope type.
    check(ok, frame.subrope(0, 8).all_host(), "the host sub-range is host, and stays flattenable");
    return ok ? 0 : 1;
}
