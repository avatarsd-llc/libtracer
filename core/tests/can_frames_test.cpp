/**
 * @file
 * @brief can_transport_t PURE framing-layer test (#55).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Host-testable, no SocketCAN, no
 * real socket. Asserts:
 *   - the 29-bit structured CAN-ID codec against an explicit bit-layout vector,
 *   - header-elided can_framing split + reassembly (classic 8B + CAN-FD 64B),
 *   - can_reassembly out-of-order + missing-interior + totality handling,
 *   - the in-band advertise frame codec (explicit bytes, round-trip, need-more).
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "libtracer/can.hpp"
#include "libtracer/can_framing.hpp"
#include "libtracer/can_reassembly.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/view.hpp"
#include "test_support.hpp"

namespace {

/** @brief The endpoint run a test group may span: the whole CAN data window. */
constexpr std::uint32_t kRun = 4095;

/** @brief A pass-through backend that counts its draws and releases. */
class counting_backend_t final : public tr::mem::mem_backend_t {
   public:
    counting_backend_t() noexcept : mem_backend_t("counting") {}
    tr::view::segment_t* alloc(std::size_t size,
                               tr::mem::alloc_hint_t hint = tr::mem::alloc_hint_t::NONE) override {
        ++allocs;
        tr::view::segment_t* const seg = tr::mem::heap_backend().alloc(size, hint);
        // Re-pointed at this backend, untagged, so the segment's release comes back HERE: the
        // test can see which backend a segment was returned to.
        if (seg != nullptr) {
            seg->backend = this;
            seg->btag = tr::mem::backend_tag::UNKNOWN;
        }
        return seg;
    }
    void destroy(tr::view::segment_t* seg) noexcept override {
        ++frees;
        tr::mem::heap_backend().destroy(seg);
    }
    std::size_t alignment() const noexcept override { return tr::mem::heap_backend().alignment(); }
    std::size_t max_segment_size() const noexcept override {
        return tr::mem::heap_backend().max_segment_size();
    }
    tr::mem::mem_space_t space() const noexcept override { return tr::mem::heap_backend().space(); }

    int allocs = 0; /**< @brief Draws served. */
    int frees = 0;  /**< @brief Segments released. */
};

/** @brief A whole advertise frame in a `std::vector` — the test-side form of
 *         `can::encode_advertise` (#1780 moved it into a caller's `bytes_t`); empty when
 *         the advertise is unencodable. */
[[maybe_unused]] std::vector<std::byte> advertise_vec(const tr::net::can::advertise_t& a) {
    tr::mem::bytes_t b(tr::mem::heap_source());
    if (!tr::net::can::encode_advertise(b, a)) return {};
    return std::vector<std::byte>(b.begin(), b.end());
}

using tr::testing::check;

std::vector<std::byte> bytes_of(std::initializer_list<std::uint8_t> vals) {
    std::vector<std::byte> v;
    v.reserve(vals.size());
    for (std::uint8_t b : vals) v.push_back(static_cast<std::byte>(b));
    return v;
}

/** @brief A heap segment holding a copy of `src` (so views over it can be reassembled). */
tr::view::view_t view_over(const std::vector<std::byte>& src) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) seg->bytes[i] = src[i];
    return tr::view::view_t::over(std::move(seg));
}

/** @brief Flatten a rope's logical bytes into a contiguous vector for comparison. */
std::vector<std::byte> rope_bytes(const tr::view::rope_t& r) {
    std::vector<std::byte> out;
    r.walk([&](std::span<const std::byte> chunk) {
        out.insert(out.end(), chunk.begin(), chunk.end());
    });
    return out;
}

/**
 * @brief Chain a payload's CAN data-field windows back into one rope, in order.
 *
 * The local inverse of the split — which is all the round-trip checks below ever wanted
 * from the `view_can_frames_t::to_rope()` that #932 removed. It was never on a production
 * path (the real far side is `tr::net::can_reassembly_t`, exercised in section 3), so it
 * lives here as the two-line test convenience it always was rather than as a shipped API.
 */
tr::view::rope_t can_rope(const tr::view::view_t& payload, tr::net::can::can_frame_mode_t mode) {
    tr::view::rope_t r;
    const std::size_t n = tr::net::can::can_frame_count(payload, mode);
    for (std::size_t i = 0; i < n; ++i) r.append(tr::net::can::can_frame_at(payload, mode, i));
    return r;
}

std::vector<std::byte> ramp(std::size_t n) {
    std::vector<std::byte> v;
    v.reserve(n);
    for (std::size_t i = 0; i < n; ++i) v.push_back(static_cast<std::byte>(i & 0xFF));
    return v;
}

}  // namespace

int main() {
    std::printf("transport_can pure framing layer:\n");

    // --- 1. 29-bit structured CAN-ID codec, explicit bit-layout vector. ---
    {
        using namespace tr::net::can;
        // version=1 (4b) | node=0x15A (13b) | endpoint=0xABC (12b)
        //  = (1<<25) | (0x15A<<12) | 0xABC = 0x0215AABC
        const can_id_fields_t f{1, 0x15A, 0xABC};
        const std::uint32_t id = encode_can_id(f);
        check(id == 0x0215AABCu, "encode_can_id(v=1,node=0x15A,ep=0xABC) == 0x0215AABC");

        const auto back = decode_can_id(id);
        check(back.has_value() && *back == f, "decode_can_id round-trips the fields");

        // Lower numeric ID = higher bus priority: a lower node wins the bus.
        const std::uint32_t lo = encode_can_id({1, 0x000, 0xFFF});
        const std::uint32_t hi = encode_can_id({1, 0x001, 0x000});
        check(lo < hi, "lower node id => lower numeric ID (higher arbitration priority)");

        // A value beyond 29 bits is not a legal extended ID.
        check(!decode_can_id(kIdMax + 1).has_value(), "decode rejects an over-29-bit value");

        // Address-shift: slice index shifts the endpoint sub-field.
        const auto s2 = slice_can_id(f, 2);
        check(s2.has_value() && *s2 == encode_can_id({1, 0x15A, 0xABE}),
              "slice_can_id(base, 2) shifts endpoint by 2");
        check(!slice_can_id({0, 0, kEndpointMax}, 1).has_value(),
              "slice_can_id overflowing the endpoint field returns nullopt");
    }

    // --- 2. can_framing: header-elided split + zero-copy reassembly. ---
    {
        using tr::net::can::can_frame_at;
        using tr::net::can::can_frame_count;
        using tr::net::can::can_frame_mode_t;

        // Zero frames for an empty payload — the boundary the count's ceiling division owns.
        check(can_frame_count(tr::view::view_t{}, can_frame_mode_t::CLASSIC) == 0,
              "empty payload => 0 CAN frames");

        // Single classic frame: 6 bytes fit in one 8-byte data field.
        {
            const std::vector<std::byte> payload = ramp(6);
            const auto pv = view_over(payload);
            check(can_frame_count(pv, can_frame_mode_t::CLASSIC) == 1,
                  "6-byte payload => 1 classic CAN frame");
            check(rope_bytes(can_rope(pv, can_frame_mode_t::CLASSIC)) == payload,
                  "  single classic frame round-trips");
        }
        // Multi classic frames: 20 bytes => 8 + 8 + 4.
        {
            const std::vector<std::byte> payload = ramp(20);
            const auto pv = view_over(payload);
            check(can_frame_count(pv, can_frame_mode_t::CLASSIC) == 3,
                  "20-byte payload => 3 classic CAN frames (8+8+4)");
            check(can_frame_at(pv, can_frame_mode_t::CLASSIC, 0).length == 8 &&
                      can_frame_at(pv, can_frame_mode_t::CLASSIC, 2).length == 4,
                  "  classic frames are 8,8,4 bytes");
            check(rope_bytes(can_rope(pv, can_frame_mode_t::CLASSIC)) == payload,
                  "  multi classic frame round-trips");
        }
        // CAN-FD: 100 bytes => 64 + 36.
        {
            const std::vector<std::byte> payload = ramp(100);
            const auto pv = view_over(payload);
            check(can_frame_count(pv, can_frame_mode_t::FD) == 2,
                  "100-byte payload => 2 CAN-FD frames (64+36)");
            check(can_frame_at(pv, can_frame_mode_t::FD, 0).length == 64,
                  "  first FD frame is 64 bytes");
            check(rope_bytes(can_rope(pv, can_frame_mode_t::FD)) == payload,
                  "  multi CAN-FD frame round-trips");
        }
        // CAN-FD DLC lattice helper.
        check(tr::net::can::can_fd_dlc_round_up(36) == 48, "can_fd_dlc_round_up(36) == 48");
        check(tr::net::can::can_fd_dlc_round_up(8) == 8, "can_fd_dlc_round_up(8) == 8");
    }

    // --- 3. can_reassembly: out-of-order, missing-interior, totality. ---
    {
        using tr::net::can::can_frame_mode_t;
        tr::net::reassembly_key_t key;
        key.origin = {1, 2, 3};  // rest zero
        key.ts = 0xCAFE;

        const std::vector<std::byte> payload = ramp(20);  // 3 classic slices.
        const auto pv = view_over(payload);
        // The windows are derived per call now (#932); this binds the two fixed arguments.
        const auto slice = [&](std::size_t i) {
            return tr::net::can::can_frame_at(pv, can_frame_mode_t::CLASSIC, i);
        };

        // Feed slices OUT OF ORDER: 2, 0, 1, into the group's one segment (#2258).
        tr::net::can_reassembly_t reasm;
        check(reasm.open_group(key, 3, 20, 8, kRun, tr::mem::heap_backend()),
              "the advertise admits the group (3 slices of 8 carry 20 bytes)");
        check(reasm.add_slice(key, 2, slice(2).bytes()), "the 4-byte tail slice is taken");
        check(reasm.add_slice(key, 0, slice(0).bytes()), "slice 0 is taken");
        check(!reasm.is_complete(key), "group incomplete with slice 1 missing");
        check(reasm.has_interior_gap(key), "interior gap detected (have 0,2 not 1)");
        check(!reasm.take(key).has_value(), "take() returns nullopt while incomplete");
        check(reasm.contains(key), "... and leaves the group in place");

        check(reasm.add_slice(key, 1, slice(1).bytes()), "slice 1 is taken");
        check(!reasm.has_interior_gap(key), "no interior gap once slice 1 arrives");
        check(reasm.is_complete(key), "group complete: all 3 slices present");

        const auto taken = reasm.take(key);
        check(taken.has_value(), "take() yields the message when complete");
        check(taken && taken->length == payload.size() &&
                  std::vector<std::byte>(taken->bytes().begin(), taken->bytes().end()) == payload,
              "  reassembled (out-of-order) segment == original payload, trimmed to the total");
        check(!reasm.contains(key), "take() stops tracking the group");
        check(reasm.dropped_groups() == 0, "a delivered group is not counted as dropped");

        // A slice the group cannot take truthfully is refused, never fabricated.
        tr::net::can_reassembly_t strict;
        check(!strict.add_slice(key, 0, slice(0).bytes()),
              "a slice with no admitted group is refused");
        check(strict.open_group(key, 3, 20, 8, kRun, tr::mem::heap_backend()), "admitted");
        check(!strict.add_slice(key, 3, slice(0).bytes()), "an index past the count is refused");
        check(!strict.add_slice(key, 0, slice(2).bytes()), "a short interior slice is refused");
        check(!strict.add_slice(key, 2, {}), "an empty (DLC-0) slice is refused");
        check(strict.slice_count(key) == 0, "... and none of them was counted");
        const std::array<std::byte, 8> padded{};
        check(strict.add_slice(key, 2, padded), "an over-long tail (DLC padding) is cut to fit");
    }

    // --- 3 (oversize). The receiver pays for what its peer announced (#2258): an advertise whose
    //     total its slice geometry cannot carry, or whose segment the backend refuses, is
    //     refused at admission and counted. ---
    {
        tr::net::reassembly_key_t key;
        key.ts = 7;
        tr::net::can_reassembly_t reasm;
        check(!reasm.open_group(key, 2, 1000, 8, kRun, tr::mem::heap_backend()),
              "2 classic slices cannot carry 1000 bytes: refused");
        check(!reasm.open_group(key, 2, 0xFFFFFFFFu, 64, kRun, tr::mem::heap_backend()),
              "a 4 GiB claim on 2 FD slices is refused without a draw");
        check(!reasm.open_group(key, 3, 16, 8, kRun, tr::mem::heap_backend()),
              "16 bytes fit in 2 slices, so a 3-slice claim is refused");
        check(!reasm.open_group(key, 0, 0, 8, kRun, tr::mem::heap_backend()),
              "a 0-slice group is refused");
        check(!reasm.open_group(key, 2, 100, 8, kRun, tr::mem::heap_backend()),
              "an FD-sized claim (64 + 36) on a classic link is refused");
        check(reasm.dropped_groups() == 5 && !reasm.contains(key),
              "each refusal is counted, and nothing is tracked");
        check(reasm.open_group(key, 2, 100, 64, kRun, tr::mem::heap_backend()),
              "the same claim on an FD link is admitted at the FD width");
        std::array<std::byte, 64> fd_slice{};
        fd_slice.fill(std::byte{0x5A});
        check(reasm.add_slice(key, 1, fd_slice) && reasm.add_slice(key, 0, fd_slice),
              "a padded FD tail and a full FD slice are taken");
        const auto fd = reasm.take(key);
        check(fd && fd->length == 100, "the FD group is delivered trimmed to its 100 bytes");
    }

    // --- 3 (caps). Admission is bounded by the link's caps, refused rather than drawn, and a
    //     full budget refuses the newcomer without evicting an open group. ---
    {
        const auto key_ts = [](std::uint64_t ts) {
            tr::net::reassembly_key_t k;
            k.ts = ts;
            return k;
        };
        counting_backend_t counting;
        tr::net::can_reassembly_t capped(tr::mem::heap_source(), 0, /*max_message=*/1024,
                                         /*max_inflight=*/2 * (1024 + 16));
        // A geometry-valid claim of 65535 FD slices (about 4 MiB) is over the message cap.
        check(!capped.open_group(key_ts(1), 65535, 65535u * 64u - 10u, 64, 65535, counting),
              "a geometry-valid 65535-slice claim over max_message is refused");
        // A run longer than the endpoint slots left can never be filled.
        check(!capped.open_group(key_ts(2), 3, 20, 8, /*max_count=*/2, counting),
              "a slice count past the endpoint window is refused");
        // The u32 extremes: the segment size is computed wide and never wraps into a draw.
        check(!capped.open_group(key_ts(3), 0x20000000u, 0xFFFFFFFFu, 8, 0xFFFFFFFFu, counting),
              "a claim whose segment size would wrap a u32 is refused");
        check(counting.allocs == 0, "none of those refusals drew from the backend");
        check(capped.dropped_groups() == 3, "each refusal is counted on dropped_groups");

        // Two 1 KiB groups fill the in-flight budget; a third is refused, and both stay open.
        check(capped.open_group(key_ts(10), 128, 1024, 8, kRun, counting) &&
                  capped.open_group(key_ts(11), 128, 1024, 8, kRun, counting),
              "two maximum-size groups are admitted within the budget");
        check(capped.inflight_bytes() == 2 * (1024 + 16), "each is charged message + bits");
        check(!capped.open_group(key_ts(12), 1, 8, 8, kRun, counting),
              "a third group past the in-flight budget is refused");
        check(capped.contains(key_ts(10)) && capped.contains(key_ts(11)),
              "... and no open group was evicted to make room");
        check(capped.dropped_groups() == 4, "the budget refusal is counted");
        capped.erase(key_ts(10));
        check(capped.inflight_bytes() == 1024 + 16, "a dropped group returns its charge");
        check(capped.open_group(key_ts(12), 1, 8, 8, kRun, counting),
              "and the next group fits again");
        check(counting.allocs == 3 && counting.frees == 1,
              "three draws, and the dropped one went back to its own backend");
    }

    // --- 3a. The group's own count (#2049): a re-sent slice counts once, and dropping a group
    //         that sits between two others takes exactly its own slices. ---
    {
        using tr::net::can::can_frame_mode_t;
        const std::vector<std::byte> payload = ramp(20);  // 3 classic slices.
        const auto pv = view_over(payload);
        const auto slice = [&](std::size_t i) {
            return tr::net::can::can_frame_at(pv, can_frame_mode_t::CLASSIC, i);
        };
        const auto key_ts = [](std::uint64_t ts) {
            tr::net::reassembly_key_t k;
            k.ts = ts;
            return k;
        };
        tr::net::can_reassembly_t reasm;
        for (const std::uint64_t ts : {1u, 2u, 3u}) {
            (void)reasm.open_group(key_ts(ts), 3, 20, 8, kRun, tr::mem::heap_backend());
            (void)reasm.add_slice(key_ts(ts), 0, slice(0).bytes());
            (void)reasm.add_slice(key_ts(ts), 1, slice(1).bytes());
        }
        (void)reasm.add_slice(key_ts(2), 1, slice(1).bytes());  // a duplicate of slice 1
        check(reasm.slice_count(key_ts(2)) == 2, "a re-sent slice replaces, and counts once");
        check(!reasm.is_complete(key_ts(2)), "... so two distinct slices of three stay incomplete");
        check(!reasm.has_interior_gap(key_ts(2)), "... and slices 0,1 have no interior gap");

        reasm.erase(key_ts(2));  // the middle group of the three
        check(!reasm.contains(key_ts(2)) && reasm.slice_count(key_ts(2)) == 0,
              "erase drops the middle group");
        check(reasm.slice_count(key_ts(1)) == 2 && reasm.slice_count(key_ts(3)) == 2,
              "... and leaves both neighbours in place");
        for (const std::uint64_t ts : {1u, 3u}) {
            (void)reasm.add_slice(key_ts(ts), 2, slice(2).bytes());
        }
        const auto a = reasm.take(key_ts(1));
        const auto b = reasm.take(key_ts(3));
        check(a && rope_bytes(tr::view::rope_t(*a)) == payload && b &&
                  rope_bytes(tr::view::rope_t(*b)) == payload,
              "both neighbours still assemble byte-exact");
    }

    // --- 3b. can_reassembly bounding: evict-oldest + dropped_groups counter. ---
    {
        using tr::net::can::can_frame_mode_t;
        const std::vector<std::byte> payload = ramp(20);
        const auto pv = view_over(payload);
        // The windows are derived per call now (#932); this binds the two fixed arguments.
        const auto slice = [&](std::size_t i) {
            return tr::net::can::can_frame_at(pv, can_frame_mode_t::CLASSIC, i);
        };
        const auto key_ts = [](std::uint64_t ts) {
            tr::net::reassembly_key_t k;
            k.ts = ts;
            return k;
        };

        // Bound live groups at 2; a third distinct group evicts the oldest (ts=1),
        // never OOM (the no-synthetic-limits doctrine: bounded drop + a counter).
        tr::net::can_reassembly_t bounded(tr::mem::heap_source(), /*max_groups=*/2);
        const auto admit = [&](std::uint64_t ts) {
            (void)bounded.open_group(key_ts(ts), 3, 20, 8, kRun, tr::mem::heap_backend());
            (void)bounded.add_slice(key_ts(ts), 0, slice(0).bytes());
        };
        admit(1);
        admit(2);
        check(bounded.dropped_groups() == 0, "no eviction while within the group bound");
        admit(3);  // exceeds 2 => evict oldest (ts=1)
        check(bounded.dropped_groups() == 1, "a 3rd group evicts one (dropped_groups == 1)");
        check(!bounded.contains(key_ts(1)), "the oldest group (ts=1) was evicted");
        check(bounded.contains(key_ts(2)) && bounded.contains(key_ts(3)),
              "the two newest groups survive the bound");
    }

    // --- 3c. can_reassembly age-out: sweep_stale reclaims a group that stopped
    // making progress (#912). A count bound alone never frees a group a lost slice
    // left permanently incomplete: a group is taken only once it is complete, so
    // without this its segment is pinned for the process's life. The buffer
    // holds NO clock — the caller stamps it, so this is fully deterministic.
    {
        using tr::net::can::can_frame_mode_t;
        const std::vector<std::byte> payload = ramp(20);
        const auto pv = view_over(payload);
        // The windows are derived per call now (#932); this binds the two fixed arguments.
        const auto slice = [&](std::size_t i) {
            return tr::net::can::can_frame_at(pv, can_frame_mode_t::CLASSIC, i);
        };
        const auto key_ts = [](std::uint64_t ts) {
            tr::net::reassembly_key_t k;
            k.ts = ts;
            return k;
        };

        tr::net::can_reassembly_t aging;
        aging.set_now(1000);
        (void)aging.open_group(key_ts(1), 3, 20, 8, kRun, tr::mem::heap_backend());  // 3 promised
        (void)aging.add_slice(key_ts(1), 0, slice(0).bytes());  // ... only 1 of which lands
        check(!aging.is_complete(key_ts(1)), "the group is incomplete (a slice was lost)");

        aging.set_now(1100);
        check(aging.sweep_stale(500) == 0, "a group inside the age window is not swept");
        check(aging.contains(key_ts(1)) && aging.dropped_groups() == 0,
              "no counter tick while the group is young");

        // A live group keeps its place: a touch restamps it, so only groups that
        // stopped making progress age out.
        aging.set_now(1400);
        (void)aging.add_slice(key_ts(1), 1, slice(1).bytes());
        aging.set_now(1800);
        check(aging.sweep_stale(500) == 0,
              "a group still receiving slices is restamped, not swept");

        aging.set_now(2400);  // 1000 past the last touch, window is 500
        check(aging.sweep_stale(500) == 1, "the stalled group ages out");
        check(!aging.contains(key_ts(1)), "the swept group's segment is reclaimed");
        check(aging.dropped_groups() == 1, "an age-out ticks dropped_groups, like an eviction");
    }

    // --- 4. in-band advertise frame codec. ---
    {
        using namespace tr::net::can;
        advertise_t a;
        a.can_id = 0x0215AABC;
        a.group = false;
        a.group_total_len = 0;
        a.slice_count = 1;
        a.path = "/a/b";

        const std::vector<std::byte> enc = advertise_vec(a);
        const std::vector<std::byte> expect = bytes_of({
            0xAD, 0x02, 0x00, 0x00,  // magic, fmt v2 (ADR-0044), flags, reserved
            0xBC, 0xAA, 0x15, 0x02,  // can_id LE (0x0215AABC)
            0x00, 0x00, 0x00, 0x00,  // group_total_len LE
            0x01, 0x00,              // slice_count LE
            0xFF, 0xFF,              // target_node LE (broadcast — undirected)
            0x04, 0x00,              // path_len LE
            0x2F, 0x61, 0x2F, 0x62,  // "/a/b"
        });
        check(enc == expect, "encode_advertise matches the explicit byte vector");

        const auto dec = decode_advertise(enc);
        check(dec.has_value(), "advertise frame decodes");
        if (dec) {
            check(dec->first == a, "  advertise round-trips field-for-field");
            check(dec->second == enc.size(), "  consumes the whole frame");
        }

        // Group (manifest) form round-trips.
        advertise_t g;
        g.can_id = encode_can_id({1, 7, 0});
        g.group = true;
        g.group_total_len = 100;
        g.slice_count = 2;
        g.path = "/cam/0";
        const std::vector<std::byte> genc = advertise_vec(g);  // the decode views it
        const auto gdec = decode_advertise(genc);
        check(gdec.has_value() && gdec->first == g, "group/manifest advertise round-trips");

        // Directed form (ADR-0044): a target node id rides the manifest.
        advertise_t d = g;
        d.target = 5;
        const std::vector<std::byte> denc = advertise_vec(d);  // the decode views it
        const auto ddec = decode_advertise(denc);
        check(ddec.has_value() && ddec->first == d && ddec->first.target == 5,
              "directed advertise (target_node) round-trips");

        // Hello/presence form (ADR-0044): slice_count == 0 binds nothing.
        advertise_t h;
        h.can_id = encode_can_id({0, 3, 0});
        h.slice_count = 0;
        h.path = "/board";
        const std::vector<std::byte> henc = advertise_vec(h);  // the decode views it
        const auto hdec = decode_advertise(henc);
        check(hdec.has_value() && hdec->first == h && hdec->first.slice_count == 0,
              "hello (slice_count 0) advertise round-trips");

        // Need-more: a truncated header returns nullopt.
        check(!decode_advertise(std::span<const std::byte>(enc.data(), 10)).has_value(),
              "truncated advertise header returns nullopt (need-more)");
        // Bad magic is rejected.
        std::vector<std::byte> bad = enc;
        bad[0] = static_cast<std::byte>(0x00);
        check(!decode_advertise(bad).has_value(), "wrong magic is rejected");
    }

    return tr::testing::summary("can_frames");
}
