/**
 * @file
 * @brief #2264 — the receive-source draws one terminus request and its reply cost, pinned as a
 *        counted metric at 64 B, 1 KiB, 4 KiB and 16 KiB.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Before #2264 a span-tier READ cost three draws on the receiving link's source: the decode
 * arena's node table, its open-node stack, and the reply's iov table. The open-node stack now
 * threads through the node table itself, and the reply gathers into the caller's inline table,
 * so the request costs exactly ONE draw (the node table) and a rope-tier request or forward of
 * a few links costs none. Only a reply wider than `kMaxInlineIov` links spills into the source,
 * which `try_probe_window_test` pins.
 *
 * The instrument counts every `try_alloc` the router's `rx` plane serves. The link's state
 * source stays the heap, so nothing else is counted.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/fwd_frame_view.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;

using tr::testing::b_fwd;
using tr::testing::b_path;
using tr::testing::check;

/** @brief A heap-backed `block_source_t` that counts the draws it serves. */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    counting_source_t() noexcept : block_source_t("test_rx") {}
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        ++draws_;
        return tr::mem::heap_source().try_alloc(bytes, align);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::heap_source().release(p, bytes, align);
    }
    /** @brief Draws since the last call; resets the count. */
    [[nodiscard]] int take() noexcept { return std::exchange(draws_, 0); }

   private:
    int draws_ = 0;
};

/** @brief A link that counts frames handed down and pushes frames up either tier. */
class rec_link_t final : public transport_t {
   public:
    explicit rec_link_t(bool ropes = false) : ropes_(ropes) {}
    void send(std::span<const std::byte> frame) override {
        ++sent;
        iov_entries = 1;
        bytes.assign(frame.begin(), frame.end());
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        ++sent;
        iov_entries = iov.size();
        bytes.clear();
        for (const auto s : iov) bytes.insert(bytes.end(), s.begin(), s.end());
    }
    [[nodiscard]] bool delivers_ropes() const override { return ropes_; }
    /** @brief Deliver @p frame: borrowed on a span link, as a @p links-link rope on a rope link. */
    void inject(std::span<const std::byte> frame, std::size_t links = 4) {
        if (!ropes_) {
            rx_.deliver_borrowed(frame);
            return;
        }
        tr::view::rope_t r;
        const std::size_t step = (frame.size() + links - 1) / links;
        for (std::size_t off = 0; off < frame.size(); off += step) {
            const std::size_t n = std::min(step, frame.size() - off);
            tr::view::segment_ptr_t seg = tr::view::heap_alloc(n);
            std::memcpy(seg->bytes.data(), frame.data() + off, n);
            r.append(tr::view::view_t::over(std::move(seg)));
        }
        rx_.deliver_rope(std::move(r));
    }
    int sent = 0;                 /**< @brief Frames the router handed down. */
    std::size_t iov_entries = 0;  /**< @brief Entries in the last frame's gather table. */
    std::vector<std::byte> bytes; /**< @brief The last frame's bytes, gathered in order. */

   private:
    bool ropes_ = false;
};

/** @brief A `VALUE` TLV carrying @p n payload bytes. */
std::vector<std::byte> value_tlv(std::size_t n) {
    const std::vector<std::byte> body(n, std::byte{0x5A});
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, tr::wire::type_t::VALUE, tr::wire::opt_t{}, body);
    return out;
}

constexpr std::array<std::size_t, 4> kSizes{64, 1024, 4096, 16384};

/** @brief A terminus READ, answered with a @p n-byte value: draws per request, per tier. */
void test_read_draws(bool ropes, int expected) {
    for (const std::size_t n : kSizes) {
        counting_source_t rx;
        graph_t g{tr::mem::heap_source()};
        const auto v = g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
        (void)g.write(v, tr::view::rope_t(tr::testing::make_value(value_tlv(n))));
        rec_link_t in(ropes);
        fwd_router_t router(g, {.rx = &rx});
        (void)router.attach_link("in", in);
        const auto req = b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"origin"}));

        in.inject(req);  // warm: one-time link state lands here, not in the count
        (void)rx.take();
        in.sent = 0;
        constexpr int kRequests = 8;
        for (int i = 0; i < kRequests; ++i) in.inject(req);
        const int draws = rx.take();
        std::printf("  %s READ, %5zu B value: %d rx draws / %d requests\n", ropes ? "rope" : "span",
                    n, draws, kRequests);
        check(in.sent == kRequests, "every request was answered");
        check(draws == expected * kRequests, ropes ? "rope-tier READ: rx draws per request"
                                                   : "span-tier READ: rx draws per request");
    }
}

/**
 * @brief A rope-tier WRITE forwarded to another link: no gather draw over a few links, ONE
 *        draw over many, and the same bytes out whatever the inbound shape (#2471).
 *
 * A rope over at most `kFwdStackRopeLinks` links gathers into the stack table. A wider one
 * gathers once, straight into one block from the receiving link's source sized twice the stack
 * table, so a dozen links cost one draw: no thrown-away stack gather, and no block that grows
 * (three draws before #2471). The forwarded bytes are compared against the span tier's for
 * every shape, including one link per byte.
 */
void test_forward_draws() {
    for (const std::size_t n : kSizes) {
        counting_source_t rx;
        graph_t g{tr::mem::heap_source()};
        rec_link_t in(/*ropes=*/true);
        rec_link_t out;
        fwd_router_t router(g, {.rx = &rx});
        (void)router.attach_link("in", in);
        (void)router.attach_link("out", out);
        const auto frame =
            b_fwd(fwd_op_t::WRITE, b_path({"out", "x"}), b_path({"origin"}), {}, value_tlv(n));

        // A one-link rope takes the span tier: its bytes are the reference every shape matches.
        in.inject(frame, 1);
        check(out.sent == 1 && out.iov_entries <= tr::net::kFwdMaxIov,
              "the span tier forwards from the stack table");
        const std::vector<std::byte> want = out.bytes;
        constexpr int kFrames = 8;
        const auto draws_for = [&](std::size_t links) {
            in.inject(frame, links);  // warm: one-time link state lands here, not in the count
            (void)rx.take();
            out.sent = 0;
            for (int i = 0; i < kFrames; ++i) in.inject(frame, links);
            check(out.sent == kFrames, "every frame was forwarded");
            check(out.bytes == want, "a rope forward's bytes match the contiguous frame's");
            return rx.take();
        };

        const int few = draws_for(4);
        std::printf("  rope forward, %5zu B value, 4 links: %d rx draws / %d frames\n", n, few,
                    kFrames);
        check(few == 0, "a 4-link rope forward gathers on the stack: no rx draw");

        // Past kFwdStackRopeLinks: a dozen links put one cut in most regions.
        const int wide = draws_for(12);
        std::printf(
            "  rope forward, %5zu B value, 12 links (%zu entries): %d rx draws / %d frames\n", n,
            out.iov_entries, wide, kFrames);
        check(out.iov_entries > tr::net::kFwdMaxIov, "the 12-link frame overflows the stack table");
        check(out.iov_entries <= 2 * tr::net::kFwdMaxIov, "and fits the first spill block");
        check(wide == kFrames, "an overflowing rope forward draws ONE block per frame");

        // One link per byte: the widest split; only the bytes are pinned.
        (void)draws_for(frame.size());
    }
}

}  // namespace

int main() {
    std::printf("#2264: receive-source draws per terminus request\n");
    test_read_draws(/*ropes=*/false, /*expected=*/1);  // the arena node table, nothing else
    test_read_draws(/*ropes=*/true, /*expected=*/0);   // no arena, inline reply table
    test_forward_draws();
    return tr::testing::summary("terminus_rx_draws");
}
