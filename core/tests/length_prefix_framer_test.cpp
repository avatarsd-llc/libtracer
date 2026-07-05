/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * length_prefix_framer unit test — drives the u32-length-prefix reassembly state
 * machine directly (no QUIC connection), the whole point of extracting it from
 * transport_quic / transport_webtransport (finding #4): prefix/body split across
 * chunks, multiple frames per chunk, empty records, oversize => malformed,
 * backpressure drain + resync, and reset.
 */
#include "libtracer/length_prefix_framer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/view.hpp"

namespace {

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

using bytes_t = std::vector<std::byte>;

bytes_t ramp(std::size_t n, std::uint8_t base = 0) {
    bytes_t v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::byte>(base + i);
    return v;
}

// A u32-LE length-prefixed record: <len><payload>.
bytes_t record(std::span<const std::byte> payload) {
    bytes_t r;
    tr::detail::append_le(r, static_cast<std::uint32_t>(payload.size()), 4);
    r.insert(r.end(), payload.begin(), payload.end());
    return r;
}

// A backend that fails `alloc` on demand (to exercise backpressure), else yields a
// real heap segment (whose backend is the heap singleton, so it self-reclaims).
class toggle_backend_t final : public tr::mem::mem_backend_t {
   public:
    toggle_backend_t() noexcept : mem_backend_t("toggle") {}
    bool fail = false;
    tr::view::segment_t* alloc(std::size_t size,
                               tr::mem::alloc_hint_t = tr::mem::alloc_hint_t::NONE) override {
        return fail ? nullptr : tr::mem::heap_backend().alloc(size);
    }
    void destroy(tr::view::segment_t*) noexcept override {}  // never fires (heap-owned segments)
};

// Collect each delivered frame's bytes so the test can compare against the input.
struct collector_t {
    std::vector<bytes_t> frames;
    void operator()(tr::view::segment_ptr_t seg, std::size_t len) {
        frames.emplace_back(seg->bytes.data(), seg->bytes.data() + len);
    }
};

constexpr std::size_t kMax = 1u << 20;  // a generous frame cap for the normal cases

}  // namespace

int main() {
    std::printf("length_prefix_framer — u32-length-prefix stream reassembly:\n");
    tr::mem::mem_backend_t& heap = tr::mem::heap_backend();

    // 1. One record delivered whole.
    {
        tr::net::length_prefix_framer f;
        collector_t c;
        const bytes_t payload = ramp(10, 1);
        const bytes_t rec = record(payload);
        const auto res = f.feed(heap, kMax, rec.data(), rec.size(), c);
        check(!res.malformed && res.dropped == 0, "whole record: no malformed/drop");
        check(c.frames.size() == 1 && c.frames[0] == payload, "one frame == the payload");
    }

    // 2. The same record fed one byte at a time (prefix + body split maximally).
    {
        tr::net::length_prefix_framer f;
        collector_t c;
        const bytes_t payload = ramp(37, 5);
        const bytes_t rec = record(payload);
        for (std::byte b : rec) f.feed(heap, kMax, &b, 1, c);
        check(c.frames.size() == 1 && c.frames[0] == payload,
              "byte-by-byte feed reassembles the frame");
    }

    // 3. Two records concatenated, fed in one chunk => two frames in order.
    {
        tr::net::length_prefix_framer f;
        collector_t c;
        const bytes_t a = ramp(4, 0x10);
        const bytes_t b = ramp(6, 0x20);
        bytes_t stream = record(a);
        const bytes_t rb = record(b);
        stream.insert(stream.end(), rb.begin(), rb.end());
        f.feed(heap, kMax, stream.data(), stream.size(), c);
        check(c.frames.size() == 2 && c.frames[0] == a && c.frames[1] == b,
              "two records in one chunk => two ordered frames");
    }

    // 4. An empty record (len == 0) is a no-op; a following record still parses.
    {
        tr::net::length_prefix_framer f;
        collector_t c;
        const bytes_t empty = record({});
        const bytes_t real = ramp(8, 0x30);
        bytes_t stream = empty;
        const bytes_t rr = record(real);
        stream.insert(stream.end(), rr.begin(), rr.end());
        f.feed(heap, kMax, stream.data(), stream.size(), c);
        check(c.frames.size() == 1 && c.frames[0] == real,
              "empty record skipped; the next record delivers");
    }

    // 5. An oversize length prefix is malformed and stops the feed.
    {
        tr::net::length_prefix_framer f;
        collector_t c;
        const bytes_t rec = record(ramp(100));  // claims 100 bytes...
        const auto res = f.feed(heap, /*max_frame=*/8, rec.data(), rec.size(), c);  // ...cap is 8
        check(res.malformed, "oversize prefix => result.malformed");
        check(c.frames.empty(), "no frame delivered from a malformed stream");
    }

    // 6. Backpressure: a failing alloc drops one frame (drained), then resyncs.
    {
        tr::net::length_prefix_framer f;
        collector_t c;
        toggle_backend_t be;
        const bytes_t dropped_payload = ramp(12, 0x40);
        const bytes_t kept_payload = ramp(9, 0x50);

        be.fail = true;
        const bytes_t r1 = record(dropped_payload);
        const auto res1 = f.feed(be, kMax, r1.data(), r1.size(), c);
        check(res1.dropped == 1 && c.frames.empty(),
              "alloc failure drops the frame (dropped == 1), none delivered");

        be.fail = false;
        const bytes_t r2 = record(kept_payload);
        const auto res2 = f.feed(be, kMax, r2.data(), r2.size(), c);
        check(res2.dropped == 0 && c.frames.size() == 1 && c.frames[0] == kept_payload,
              "framing resyncs: the next record delivers cleanly after a drop");
    }

    // 7. reset() discards partial state (a half-read prefix does not corrupt the next).
    {
        tr::net::length_prefix_framer f;
        collector_t c;
        const bytes_t rec = record(ramp(5, 0x60));
        f.feed(heap, kMax, rec.data(), 2, c);  // feed only 2 of the 4 prefix bytes
        check(c.frames.empty(), "no frame from a partial prefix");
        f.reset();
        f.feed(heap, kMax, rec.data(), rec.size(), c);  // a fresh, complete record
        check(c.frames.size() == 1 && c.frames[0] == ramp(5, 0x60),
              "after reset, a complete record parses (no stale prefix bytes)");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
