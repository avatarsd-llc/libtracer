/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * transport_can PURE framing-layer test (#55). Host-testable, no SocketCAN, no
 * real socket. Asserts:
 *   - the 29-bit structured CAN-ID codec against an explicit bit-layout vector,
 *   - header-elided view_can_frames split + reassembly (classic 8B + CAN-FD 64B),
 *   - can_reassembly out-of-order + missing-interior + totality handling,
 *   - the in-band advertise frame codec (explicit bytes, round-trip, need-more).
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/can.hpp"
#include "libtracer/can_reassembly.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/view.hpp"
#include "libtracer/view_can.hpp"

namespace {

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

std::vector<std::byte> bytes_of(std::initializer_list<std::uint8_t> vals) {
    std::vector<std::byte> v;
    v.reserve(vals.size());
    for (std::uint8_t b : vals) v.push_back(static_cast<std::byte>(b));
    return v;
}

// A heap segment holding a copy of `src` (so views over it can be reassembled).
tr::view::view_t view_over(const std::vector<std::byte>& src) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) seg->bytes[i] = src[i];
    return tr::view::view_t::over(std::move(seg));
}

// Flatten a rope's logical bytes into a contiguous vector for comparison.
std::vector<std::byte> rope_bytes(const tr::view::rope_t& r) {
    std::vector<std::byte> out;
    r.walk([&](std::span<const std::byte> chunk) {
        out.insert(out.end(), chunk.begin(), chunk.end());
    });
    return out;
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

    // --- 2. view_can_frames: header-elided split + zero-copy reassembly. ---
    {
        using tr::view::can_frame_mode_t;
        using tr::view::view_can_frames_t;

        // Single classic frame: 6 bytes fit in one 8-byte data field.
        {
            const std::vector<std::byte> payload = ramp(6);
            const auto framed =
                view_can_frames_t::split(view_over(payload), can_frame_mode_t::CLASSIC);
            check(framed.frame_count() == 1, "6-byte payload => 1 classic CAN frame");
            check(rope_bytes(framed.to_rope()) == payload, "  single classic frame round-trips");
        }
        // Multi classic frames: 20 bytes => 8 + 8 + 4.
        {
            const std::vector<std::byte> payload = ramp(20);
            const auto framed =
                view_can_frames_t::split(view_over(payload), can_frame_mode_t::CLASSIC);
            check(framed.frame_count() == 3, "20-byte payload => 3 classic CAN frames (8+8+4)");
            check(framed.frames()[0].length == 8 && framed.frames()[2].length == 4,
                  "  classic frames are 8,8,4 bytes");
            check(rope_bytes(framed.to_rope()) == payload, "  multi classic frame round-trips");
        }
        // CAN-FD: 100 bytes => 64 + 36.
        {
            const std::vector<std::byte> payload = ramp(100);
            const auto framed = view_can_frames_t::split(view_over(payload), can_frame_mode_t::FD);
            check(framed.frame_count() == 2, "100-byte payload => 2 CAN-FD frames (64+36)");
            check(framed.frames()[0].length == 64, "  first FD frame is 64 bytes");
            check(rope_bytes(framed.to_rope()) == payload, "  multi CAN-FD frame round-trips");
        }
        // CAN-FD DLC lattice helper.
        check(tr::view::can_fd_dlc_round_up(36) == 48, "can_fd_dlc_round_up(36) == 48");
        check(tr::view::can_fd_dlc_round_up(8) == 8, "can_fd_dlc_round_up(8) == 8");
    }

    // --- 3. can_reassembly: out-of-order, missing-interior, totality. ---
    {
        using tr::view::can_frame_mode_t;
        using tr::view::view_can_frames_t;
        tr::net::reassembly_key_t key;
        key.origin = {1, 2, 3};  // rest zero
        key.ts = 0xCAFE;

        const std::vector<std::byte> payload = ramp(20);  // 3 classic slices.
        const auto framed = view_can_frames_t::split(view_over(payload), can_frame_mode_t::CLASSIC);

        // Feed slices OUT OF ORDER: 2, 0, 1.
        tr::net::can_reassembly_t reasm;
        reasm.set_expected_count(key, 3);
        reasm.add_slice(key, 2, framed.frames()[2]);
        reasm.add_slice(key, 0, framed.frames()[0]);
        check(!reasm.is_complete(key), "group incomplete with slice 1 missing");
        check(reasm.has_interior_gap(key), "interior gap detected (have 0,2 not 1)");
        check(!reasm.assemble(key).has_value(), "assemble() returns nullopt while incomplete");

        reasm.add_slice(key, 1, framed.frames()[1]);
        check(!reasm.has_interior_gap(key), "no interior gap once slice 1 arrives");
        check(reasm.is_complete(key), "group complete: all 3 slices present + expected set");

        const auto assembled = reasm.assemble(key);
        check(assembled.has_value(), "assemble() yields a rope when complete");
        check(assembled && rope_bytes(*assembled) == payload,
              "  reassembled (out-of-order) rope == original payload");

        // Totality opt-in: without expected_count, completeness is undecidable.
        tr::net::can_reassembly_t no_total;
        no_total.add_slice(key, 0, framed.frames()[0]);
        no_total.add_slice(key, 1, framed.frames()[1]);
        no_total.add_slice(key, 2, framed.frames()[2]);
        check(!no_total.is_complete(key),
              "no expected_count => not complete (trailing-drop blind)");
        no_total.erase(key);
        check(!no_total.contains(key), "erase() drops the group");
    }

    // --- 3b. can_reassembly bounding: evict-oldest + dropped_groups counter. ---
    {
        using tr::view::can_frame_mode_t;
        using tr::view::view_can_frames_t;
        const std::vector<std::byte> payload = ramp(20);
        const auto framed = view_can_frames_t::split(view_over(payload), can_frame_mode_t::CLASSIC);
        const auto key_ts = [](std::uint64_t ts) {
            tr::net::reassembly_key_t k;
            k.ts = ts;
            return k;
        };

        // Bound live groups at 2; a third distinct group evicts the oldest (ts=1),
        // never OOM (the no-synthetic-limits doctrine: bounded drop + a counter).
        tr::net::can_reassembly_t bounded(std::pmr::new_delete_resource(), /*max_groups=*/2);
        bounded.add_slice(key_ts(1), 0, framed.frames()[0]);
        bounded.add_slice(key_ts(2), 0, framed.frames()[0]);
        check(bounded.dropped_groups() == 0, "no eviction while within the group bound");
        bounded.add_slice(key_ts(3), 0, framed.frames()[0]);  // exceeds 2 => evict oldest (ts=1)
        check(bounded.dropped_groups() == 1, "a 3rd group evicts one (dropped_groups == 1)");
        check(!bounded.contains(key_ts(1)), "the oldest group (ts=1) was evicted");
        check(bounded.contains(key_ts(2)) && bounded.contains(key_ts(3)),
              "the two newest groups survive the bound");
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

        const std::vector<std::byte> enc = encode_advertise(a);
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
        const auto gdec = decode_advertise(encode_advertise(g));
        check(gdec.has_value() && gdec->first == g, "group/manifest advertise round-trips");

        // Directed form (ADR-0044): a target node id rides the manifest.
        advertise_t d = g;
        d.target = 5;
        const auto ddec = decode_advertise(encode_advertise(d));
        check(ddec.has_value() && ddec->first == d && ddec->first.target == 5,
              "directed advertise (target_node) round-trips");

        // Hello/presence form (ADR-0044): slice_count == 0 binds nothing.
        advertise_t h;
        h.can_id = encode_can_id({0, 3, 0});
        h.slice_count = 0;
        h.path = "/board";
        const auto hdec = decode_advertise(encode_advertise(h));
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

    std::printf(g_failures == 0 ? "\nCAN: PASS\n" : "\nCAN: FAIL (%d)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
