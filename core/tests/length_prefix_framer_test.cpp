/**
 * @file
 * @brief length_prefix_framer unit test — drives the u32-length-prefix reassembly state machine
 *        directly (no QUIC connection), the whole point of extracting it from transport_quic /
 *        transport_webtransport (finding #4): prefix/body split across chunks, multiple frames per
 *        chunk, empty records, over the protocol cap => malformed, over local capacity =>
 *        backpressure drain + resync (#932), and reset.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
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
#include "test_support.hpp"

namespace {

using tr::testing::check;

using bytes_t = std::vector<std::byte>;

bytes_t ramp(std::size_t n, std::uint8_t base = 0) {
    bytes_t v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::byte>(base + i);
    return v;
}

/** @brief A u32-LE length-prefixed record: <len><payload>. */
bytes_t record(std::span<const std::byte> payload) {
    bytes_t r;
    tr::detail::append_le(r, static_cast<std::uint32_t>(payload.size()), 4);
    r.insert(r.end(), payload.begin(), payload.end());
    return r;
}

/**
 * @brief A backend that fails `alloc` on demand (to exercise backpressure), else yields a real heap
 *        segment (whose backend is the heap singleton, so it self-reclaims).
 */
class toggle_backend_t final : public tr::mem::mem_backend_t {
   public:
    toggle_backend_t() noexcept : mem_backend_t("toggle") {}
    bool fail = false;
    std::size_t max_seg = ~std::size_t{0}; /**< largest allocatable segment (default: unbounded) */
    tr::view::segment_t* alloc(std::size_t size,
                               tr::mem::alloc_hint_t = tr::mem::alloc_hint_t::NONE) override {
        // A bounded backend refuses what it could never hold, exactly as a real
        // pool does — the framer's DROP leg depends on that honesty.
        if (fail || size > max_seg) return nullptr;
        return tr::mem::heap_backend().alloc(size);
    }
    void destroy(tr::view::segment_t*) noexcept override {}  // never fires (heap-owned segments)
    [[nodiscard]] std::size_t max_segment_size() const noexcept override { return max_seg; }
};

/** @brief Collect each delivered frame's bytes so the test can compare against the input. */
struct collector_t {
    std::vector<bytes_t> frames;
    void operator()(tr::view::segment_ptr_t seg, std::size_t len) {
        frames.emplace_back(seg->bytes.data(), seg->bytes.data() + len);
    }
};

/** @brief Count backpressure drops as the framer decides them (the `on_drop` seam). */
struct drop_counter_t {
    std::size_t drops = 0;
    void operator()() { ++drops; }
};

/** @brief For the cases whose subject is not backpressure. */
constexpr auto ignore_drops = [] {};

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
        drop_counter_t d;
        const auto res = f.feed(heap, kMax, rec.data(), rec.size(), c, d);
        check(!res.malformed && d.drops == 0, "whole record: no malformed/drop");
        check(c.frames.size() == 1 && c.frames[0] == payload, "one frame == the payload");
    }

    // 2. The same record fed one byte at a time (prefix + body split maximally).
    {
        tr::net::length_prefix_framer f;
        collector_t c;
        const bytes_t payload = ramp(37, 5);
        const bytes_t rec = record(payload);
        for (std::byte b : rec) f.feed(heap, kMax, &b, 1, c, ignore_drops);
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
        f.feed(heap, kMax, stream.data(), stream.size(), c, ignore_drops);
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
        f.feed(heap, kMax, stream.data(), stream.size(), c, ignore_drops);
        check(c.frames.size() == 1 && c.frames[0] == real,
              "empty record skipped; the next record delivers");
    }

    // 5. An oversize length prefix is malformed and stops the feed.
    {
        tr::net::length_prefix_framer f;
        collector_t c;
        const bytes_t rec = record(ramp(100));  // claims 100 bytes...
        const auto res = f.feed(heap, /*max_frame=*/8, rec.data(), rec.size(), c,
                                ignore_drops);  // ...cap is 8
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
        drop_counter_t d;
        const auto res1 = f.feed(be, kMax, r1.data(), r1.size(), c, d);
        check(!res1.malformed && d.drops == 1 && c.frames.empty(),
              "alloc failure drops the frame (dropped == 1), none delivered");

        be.fail = false;
        const bytes_t r2 = record(kept_payload);
        const auto res2 = f.feed(be, kMax, r2.data(), r2.size(), c, d);
        check(
            !res2.malformed && d.drops == 1 && c.frames.size() == 1 && c.frames[0] == kept_payload,
            "framing resyncs: the next record delivers cleanly after a drop");
    }

    // 6b. (#932) A prefix beyond the backend's capacity but INSIDE the protocol cap
    //     is backpressure, not malformed: the peer obeyed the agreed limit, our local
    //     segment size is our problem — drain, count, resync, keep the connection.
    {
        tr::net::length_prefix_framer f;
        collector_t c;
        toggle_backend_t be;
        be.max_seg = 8;                        // this backend never allocates more than 8 bytes
        const bytes_t big = record(ramp(20));  // claims 20 > 8, though 20 < kMax
        drop_counter_t d;
        const auto res = f.feed(be, kMax, big.data(), big.size(), c, d);
        check(!res.malformed, "a frame beyond backend.max_segment_size() is NOT malformed (#932)");
        check(d.drops == 1, "it is counted as a backpressure drop instead");
        check(c.frames.empty(), "no frame delivered when the backend could never hold it");

        const bytes_t fits = ramp(6, 0x70);
        const bytes_t r2 = record(fits);
        const auto res2 = f.feed(be, kMax, r2.data(), r2.size(), c, d);
        check(!res2.malformed && d.drops == 1 && c.frames.size() == 1 && c.frames[0] == fits,
              "the over-capacity frame was DRAINED: framing resyncs and the next record delivers");
    }

    // 6c. (#932) Only a length beyond the PROTOCOL cap still tears the stream down.
    {
        tr::net::length_prefix_framer f;
        collector_t c;
        toggle_backend_t be;
        be.max_seg = 8;
        const bytes_t rec = record(ramp(20));
        const auto res = f.feed(be, /*max_frame=*/10, rec.data(), rec.size(), c, ignore_drops);
        check(res.malformed, "a length above max_frame is still malformed");
    }

    // 7. reset() discards partial state (a half-read prefix does not corrupt the next).
    {
        tr::net::length_prefix_framer f;
        collector_t c;
        const bytes_t rec = record(ramp(5, 0x60));
        f.feed(heap, kMax, rec.data(), 2, c, ignore_drops);  // feed only 2 of the 4 prefix bytes
        check(c.frames.empty(), "no frame from a partial prefix");
        f.reset();
        f.feed(heap, kMax, rec.data(), rec.size(), c, ignore_drops);  // a fresh, complete record
        check(c.frames.size() == 1 && c.frames[0] == ramp(5, 0x60),
              "after reset, a complete record parses (no stale prefix bytes)");
    }

    // 8. The shared rule kernel directly (on_prefix / effective_cap) — the same
    //    rules tcp_transport_t applies in pull mode without the chunk machine.
    {
        using framer_t = tr::net::length_prefix_framer;
        using kind_t = framer_t::prefix_decision_t::kind_t;
        toggle_backend_t be;
        be.max_seg = 64;
        check(framer_t::effective_cap(be, 1000) == 64, "effective_cap: backend capacity wins");
        check(framer_t::effective_cap(be, 16) == 16, "effective_cap: caller ceiling wins");

        check(framer_t::on_prefix(be, 1000, 0).kind == kind_t::EMPTY, "on_prefix: len 0 => EMPTY");
        check(framer_t::on_prefix(be, 1000, 1001).kind == kind_t::MALFORMED,
              "on_prefix: len > the protocol cap => MALFORMED");
        check(framer_t::on_prefix(be, 1000, 65).kind == kind_t::DROP,
              "on_prefix: len inside the protocol cap but past the backend's slot => DROP (#932)");
        be.fail = true;
        check(framer_t::on_prefix(be, 1000, 10).kind == kind_t::DROP,
              "on_prefix: alloc failure => DROP");
        be.fail = false;
        auto dec = framer_t::on_prefix(be, 1000, 10);
        check(dec.kind == kind_t::ACCEPT && dec.seg && dec.seg->bytes.size() >= 10,
              "on_prefix: ACCEPT carries a segment holding the frame");
    }

    // 9. configured_cap — the :settings max_frame resolution every framed transport
    //    assigns through — is TIGHTEN-ONLY against kDefaultMaxFrame (#1035): a
    //    config-writable key must not raise the ingress cap.
    {
        using framer_t = tr::net::length_prefix_framer;
        constexpr std::size_t kDefault = framer_t::kDefaultMaxFrame;
        check(framer_t::configured_cap(0) == kDefault, "configured_cap: 0 (unset) => the default");
        check(framer_t::configured_cap(4096) == 4096,
              "configured_cap: a value below the default tightens the cap");
        check(framer_t::configured_cap(kDefault) == kDefault,
              "configured_cap: the default itself passes through");
        check(framer_t::configured_cap(kDefault * 2) == kDefault,
              "configured_cap: a value ABOVE the default is clamped — tighten-only, never raise");
    }

    // 10. (#1255) A drop is reported BEFORE any frame that follows it in the SAME
    //     chunk is delivered. This is the ordering the transports' `dropped_rx`
    //     counters rest on: a receiver woken by the delivered frame must already be
    //     able to see the drop that preceded it. One chunk carrying [dropped][kept]
    //     is the exact shape the WebTransport backpressure test hits, where small
    //     frames routinely coalesce into one msquic RECEIVE.
    {
        tr::net::length_prefix_framer f;
        toggle_backend_t be;
        be.max_seg = 8;  // 20 bytes can never be held; 6 can
        const bytes_t kept_payload = ramp(6, 0x70);
        bytes_t stream = record(ramp(20));  // dropped
        const bytes_t rk = record(kept_payload);
        stream.insert(stream.end(), rk.begin(), rk.end());  // ...then delivered, same chunk

        drop_counter_t d;
        std::size_t drops_visible_at_delivery = 0;
        collector_t c;
        auto observe = [&](tr::view::segment_ptr_t seg, std::size_t len) {
            drops_visible_at_delivery = d.drops;  // what a receiver could see right now
            c(std::move(seg), len);
        };
        const auto res = f.feed(be, kMax, stream.data(), stream.size(), observe, d);
        check(!res.malformed && d.drops == 1, "one drop, one delivery, no malformed");
        check(c.frames.size() == 1 && c.frames[0] == kept_payload, "the second record delivered");
        check(drops_visible_at_delivery == 1,
              "the drop is already counted when the following frame is delivered (#1255)");

        // Ablation — the pre-fix shape, reconstructed: tally the drop locally and
        // publish it only after the feed returns. The delivery then observes a STALE
        // count, which is precisely the defect. Kept as an executable statement of
        // WHY the callback exists, so a future revert to per-chunk batching fails here
        // instead of intermittently in a live transport suite.
        tr::net::length_prefix_framer f2;
        std::size_t local_tally = 0;
        std::size_t published = 0;
        std::size_t published_visible_at_delivery = 0;
        collector_t c2;
        auto observe2 = [&](tr::view::segment_ptr_t seg, std::size_t len) {
            published_visible_at_delivery = published;
            c2(std::move(seg), len);
        };
        f2.feed(be, kMax, stream.data(), stream.size(), observe2, [&] { ++local_tally; });
        published = local_tally;  // the old post-feed publication point
        check(published == 1 && published_visible_at_delivery == 0,
              "ablation: batching the count until after the feed hides it from the delivery");
    }

    return tr::testing::summary("length_prefix_framer");
}
