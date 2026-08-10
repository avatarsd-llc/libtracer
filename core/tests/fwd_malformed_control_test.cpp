/**
 * @file
 * @brief The malformed-control-frame rejection surface of `fwd_router_t` — the guards a
 *        peer's bytes reach first, and which nothing asserted.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A mutation sweep over `core/src/fwd_router.cpp` disabled each single-line guard on the
 * peer-reachable RX path in turn, rebuilt, and ran the whole suite under
 * `-fsanitize=address,undefined`. **Eighteen of twenty survived** — nothing noticed the
 * guard was gone. The two caught were the root-CRC check and the `all_host` door on the
 * rope arm.
 *
 * That is a coverage hole, not dead code: these are the first checks a hostile or merely
 * buggy peer's bytes meet. This file sends the frames that reach them. `encode_advertise`
 * and `encode_compact` can only build WELL-FORMED frames, which is exactly why the
 * rejection paths had never been exercised — every case here assembles its frame by hand.
 *
 * @section observables What each case asserts
 *
 * A drop is invisible by construction, so "nothing happened" is not an assertion — it
 * would pass just as well if the frame never arrived, or if the router had been broken
 * into rejecting everything. Every case therefore asserts something POSITIVE:
 *
 *   - a malformed `ADVERTISE` must leave its label **unbound**, which is observable: a
 *     later `COMPACT` on that label comes back as a `HANDLE_NACK` (the RFC-0004 §E.1
 *     self-heal), not as a delivery;
 *   - a **positive control** runs on the same router afterwards — a well-formed
 *     advertise/compact pair that must still bind and still deliver. A guard that
 *     over-rejects breaks the control.
 *
 * @section redundancy On redundant guards
 *
 * Some guards shadow each other: a bare-label `ADVERTISE` is stopped by the
 * `child1_off == 0` check and, one line later, by the empty-subspan decode failing. A
 * mutation disabling only one of a redundant pair is unobservable to any test. These
 * cases therefore pin the **behaviour** — the frame is rejected and the label stays
 * unbound — not any single line. Which of the two rejects it is an implementation detail;
 * that it is rejected is not.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;

/** @brief A link that records what the router sends back, and can push ropes upward. */
class rec_link_t : public transport_t {
   public:
    explicit rec_link_t(bool ropes = false) : ropes_(ropes) {}
    void send(std::span<const std::byte> frame) override {
        sent.emplace_back(frame.begin(), frame.end());
    }
    [[nodiscard]] bool delivers_ropes() const override { return ropes_; }
    void inject(tr::view::rope_t frame) { rx_.deliver_rope(std::move(frame)); }
    std::vector<std::vector<std::byte>> sent;

   private:
    bool ropes_ = false;
};

// --- wire builders (hand-assembled, so the children can be wrong on purpose) ------

/** @brief A `PATH` TLV over the given `/`-segments. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

/** @brief An opaque `VALUE` TLV holding a little-endian `u32`. */
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::array<std::byte, 4> raw{};
    tr::detail::store_le(std::span<std::byte>(raw), v, 4);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, raw);
    return out;
}

/** @brief The 2-byte little-endian label `VALUE` that leads every control frame. */
std::vector<std::byte> b_label(std::uint16_t label) {
    std::array<std::byte, 2> raw{};
    tr::detail::store_le(std::span<std::byte>(raw), label, 2);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, raw);
    return out;
}

/** @brief Concatenate TLV byte blobs into one child run. */
std::vector<std::byte> cat(std::initializer_list<std::vector<std::byte>> parts) {
    std::vector<std::byte> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

/** @brief A structured control frame with an arbitrary child run. */
std::vector<std::byte> b_control(type_t outer, const std::vector<std::byte>& children) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, outer, opt_t{.pl = true}, children);
    return out;
}

/** @brief A rope over `bytes` split into @p links links — ≥2 selects the rope control arm. */
tr::view::rope_t as_rope(std::span<const std::byte> bytes, std::size_t links) {
    tr::view::rope_t r;
    if (bytes.empty() || links == 0) return r;
    const std::size_t step = (bytes.size() + links - 1) / links;
    for (std::size_t off = 0; off < bytes.size(); off += step) {
        const std::size_t n = std::min(step, bytes.size() - off);
        tr::view::segment_ptr_t seg = tr::view::heap_alloc(n);
        std::memcpy(seg->bytes.data(), bytes.data() + off, n);
        r.append(tr::view::view_t::over(std::move(seg)));
    }
    return r;
}

/** @brief How the two RX arms are reached — the same bytes, two entry points. */
enum class arm_t { SPAN, ROPE };

const char* arm_name(arm_t a) { return a == arm_t::SPAN ? "span" : "rope"; }

/** @brief Feed @p frame to the router through the arm under test. */
void feed(fwd_router_t& r, rec_link_t& link, std::string_view name,
          const std::vector<std::byte>& frame, arm_t arm) {
    if (arm == arm_t::SPAN) {
        r.on_frame(name, frame);
    } else {
        link.inject(as_rope(frame, 3));  // 3 links ⇒ never the contiguous fast path
    }
}

/** @brief True when @p frames contains a `HANDLE_NACK` for @p label. */
bool has_nack(const std::vector<std::vector<std::byte>>& frames, std::uint16_t label) {
    const std::vector<std::byte> want = tr::net::encode_handle_nack(label);
    return std::any_of(frames.begin(), frames.end(),
                       [&](const std::vector<std::byte>& f) { return f == want; });
}

/**
 * @brief The shared body: send @p bad, require the label unbound, then require a
 *        well-formed flow on the SAME router to still bind and deliver.
 */
void reject_and_still_work(const char* what, const std::vector<std::byte>& bad, arm_t arm) {
    std::printf("%s (%s arm):\n", what, arm_name(arm));
    graph_t g;
    const auto sink = path_t::parse("/sink");
    const tr::graph::vertex_handle_t sink_v = g.register_vertex(*sink, role_t::STORED_VALUE);
    fwd_router_t router(g);
    rec_link_t up(arm == arm_t::ROPE);
    (void)router.add_child("up", up);

    constexpr std::uint16_t kBad = 0x0111u;
    constexpr std::uint16_t kGood = 0x0222u;
    constexpr std::uint32_t kVal = 0xC0FFEE01u;

    feed(router, up, "up", bad, arm);

    // The label must be UNBOUND. Probing it with a COMPACT is what makes that observable:
    // an unbound label answers with a HANDLE_NACK (RFC-0004 E.1 self-heal), a bound one
    // would deliver into /sink.
    up.sent.clear();
    feed(router, up, "up", tr::net::encode_compact(kBad, b_value_u32(0xDEADBEEFu)), arm);
    check(has_nack(up.sent, kBad), "the malformed frame left its label UNBOUND (NACK on probe)");
    check(!g.read(sink_v).has_value(), "and nothing was delivered into /sink");

    // The positive control: the router must still be usable. A guard that over-rejects,
    // or a rejection that corrupted the label table, fails HERE and not above.
    feed(router, up, "up", tr::net::encode_advertise(kGood, b_path({"sink"})), arm);
    feed(router, up, "up", tr::net::encode_compact(kGood, b_value_u32(kVal)), arm);
    const auto stored = g.read(sink_v);
    check(stored.has_value(), "a well-formed flow on the same router still delivers");
    if (stored) {
        const auto inner = tr::wire::decode((*stored)->only());
        check(inner && inner->payload.size() == 4 &&
                  tr::detail::load_le<std::uint32_t>(inner->payload) == kVal,
              "and delivers the right bytes (the reject did not corrupt the label table)");
    }
}

/**
 * @brief A frame shorter than a TLV header must be dropped before anything reads it.
 *
 * `on_frame_impl`'s `frame.size() < 4` is the only thing standing between a runt frame and
 * `read_fwd_header` reading a 4-byte header out of it. With the guard disabled this is a
 * heap-buffer-overflow, which is why the case is worth its own test rather than a comment.
 */
void test_runt_frames() {
    std::printf("runt frames (shorter than a TLV header) are dropped before any read:\n");
    graph_t g;
    const tr::graph::vertex_handle_t sink_v =
        g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    fwd_router_t router(g);
    rec_link_t up;
    (void)router.add_child("up", up);

    for (std::size_t n = 0; n < 4; ++n) {
        std::vector<std::byte> runt(n, std::byte{0x11});
        router.on_frame("up", runt);
    }
    check(up.sent.empty(), "no reply to any 0..3-byte frame");

    // Positive control on the same router.
    router.on_frame("up", tr::net::encode_advertise(7, b_path({"sink"})));
    router.on_frame("up", tr::net::encode_compact(7, b_value_u32(0x5A5A5A5Au)));
    const auto stored = g.read(sink_v);
    check(stored.has_value(), "and the router still works afterwards");
}

}  // namespace

int main() {
    std::printf("fwd_router malformed-control-frame rejection surface\n\n");

    test_runt_frames();

    for (const arm_t arm : {arm_t::SPAN, arm_t::ROPE}) {
        // No child at all: `ADVERTISE{ VALUE label }`. `child1_off` stays 0.
        reject_and_still_work("bare-label ADVERTISE (no route child)",
                              b_control(type_t::ADVERTISE, b_label(0x0111u)), arm);

        // A route child that is not a PATH — `on_advertise`'s type check is the only
        // thing that stops a VALUE being walked as a route.
        reject_and_still_work(
            "ADVERTISE whose route child is a VALUE, not a PATH",
            b_control(type_t::ADVERTISE, cat({b_label(0x0111u), b_value_u32(0xABADCAFEu)})), arm);

        // Structured route child whose own body is garbage — the sub-decode must fail.
        {
            std::vector<std::byte> bad_path;
            const std::array<std::byte, 3> junk{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
            tr::wire::emit_tlv(bad_path, type_t::PATH, opt_t{.pl = true}, junk);
            reject_and_still_work("ADVERTISE whose PATH child has an undecodable body",
                                  b_control(type_t::ADVERTISE, cat({b_label(0x0111u), bad_path})),
                                  arm);
        }

        // Not structured: an ADVERTISE with PL=0 is not a control frame at all, so
        // `peek_control` refuses it before the label is ever read.
        {
            std::vector<std::byte> opaque;
            const std::array<std::byte, 4> raw{std::byte{0x11}, std::byte{0x01}, std::byte{0x00},
                                               std::byte{0x00}};
            tr::wire::emit_tlv(opaque, type_t::ADVERTISE, opt_t{}, raw);
            reject_and_still_work("ADVERTISE with PL=0 (not a structured control frame)", opaque,
                                  arm);
        }

        // A leading child that is a NAME, not the opaque u16 VALUE the label must be.
        {
            std::vector<std::byte> named;
            tr::wire::emit_name(named, "nope");
            reject_and_still_work("ADVERTISE whose leading child is a NAME, not a label VALUE",
                                  b_control(type_t::ADVERTISE, cat({named, b_path({"sink"})})),
                                  arm);
        }

        // Trailing bytes past a well-formed root: `peek_control` requires
        // `outer->total == cur.size()`, so a peer cannot append and have it ignored.
        {
            std::vector<std::byte> trailing = tr::net::encode_advertise(0x0111u, b_path({"sink"}));
            trailing.push_back(std::byte{0x00});
            reject_and_still_work("ADVERTISE with trailing bytes past the root", trailing, arm);
        }

        // A bare-label COMPACT carries no payload — it must not deliver an empty value.
        {
            std::printf("bare-label COMPACT (no payload child) (%s arm):\n", arm_name(arm));
            graph_t g;
            const auto sink = path_t::parse("/sink");
            const tr::graph::vertex_handle_t sink_v =
                g.register_vertex(*sink, role_t::STORED_VALUE);
            fwd_router_t router(g);
            rec_link_t up(arm == arm_t::ROPE);
            (void)router.add_child("up", up);
            constexpr std::uint16_t kLabel = 0x0333u;
            feed(router, up, "up", tr::net::encode_advertise(kLabel, b_path({"sink"})), arm);
            feed(router, up, "up", b_control(type_t::COMPACT, b_label(kLabel)), arm);
            check(!g.read(sink_v).has_value(),
                  "a payload-less COMPACT on a BOUND label delivers nothing");

            const std::uint32_t kVal = 0x11223344u;
            feed(router, up, "up", tr::net::encode_compact(kLabel, b_value_u32(kVal)), arm);
            const auto stored = g.read(sink_v);
            check(stored.has_value(), "and the binding still delivers a well-formed COMPACT");
        }
    }

    return tr::testing::summary("fwd_malformed_control");
}
