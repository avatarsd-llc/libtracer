/**
 * @file
 * @brief The mount WIDTH lift: a mount of any width registers and resolves (#523).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Two defects, one shape.
 *
 * **#523** — a mount key of more than three segments registered happily and resolved for
 * nothing. `size()` and `live_size()` reported a healthy child while every forward to it
 * missed and fell through to the terminus with no error anywhere. The bound was a debug
 * `assert`, so under `NDEBUG` — every release build — there was no bound at all, while the
 * descent could still only reach the widths a compile-time constant enumerated.
 *
 * @section ablation What goes RED if a guard is ablated
 *
 * Every case here is tied to one mechanism, so the suite is a set of ablation probes rather
 * than a wall of assertions:
 *   - delete the `seg_count` prefix match (restore the fixed-width descent) → `W=4/8/33` and
 *     both identity cases fail;
 *   - delete the `k <= best_k` longest-match filter → `longest_match_wins` fails;
 *   - delete the walker's backwards-restart → `narrow_after_wide` fails;
 *   - delete the `!next` exact-mount check → `exact_mount_terminates` fails;
 *   - delete `routable_mount_name` → `unaddressable_names_refused` fails.
 *
 * The OOM half of `add_child`'s new
 * `bool` lives in `mount_add_oom_test.cpp`, which needs its own binary to replace the global
 * nothrow `operator new`.
 *
 * One thing deliberately NOT claimed: `matches_prefix`'s trailing `pos == key.size()` and its
 * `/` check are NOT ablation-provable, and this was checked rather than assumed — ablating
 * either leaves the whole suite green, because the digest and joined-length pre-filters
 * already reject every case that could reach them. They are kept because they are what makes
 * `matches_prefix` correct read on its own, not because a test needs them; `segment_boundaries`
 * below pins the BEHAVIOUR (a byte prefix is not a segment prefix), which is what matters, and
 * it goes red if the prefix match itself is broken.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "route_frame_builder.hpp"  // host-only frame builders (#1779)
#include "test_support.hpp"
#include "tlv_tree.hpp"  // host-only owning tree (#1829)

namespace {

using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;

/** @brief A link that records every frame it is asked to send. */
struct recording_link_t : tr::net::transport_t {
    std::vector<std::vector<std::byte>> sent; /**< @brief Frames handed to this endpoint. */
    void send(std::span<const std::byte> f) override { sent.emplace_back(f.begin(), f.end()); }
};

/** @brief A `FWD{WRITE, dst, src, payload}` frame over the given segment lists. */
std::vector<std::byte> make_fwd(std::span<const std::string> dst,
                                std::span<const std::string> src) {
    std::vector<std::byte> payload;
    const std::byte pv[2] = {std::byte{0x01}, std::byte{0x02}};
    tr::wire::emit_tlv(payload, type_t::VALUE, opt_t{}, std::span<const std::byte>(pv, 2));
    return tr::testing::b_fwd(tr::graph::fwd_op_t::WRITE, tr::testing::b_path(dst),
                              tr::testing::b_path(src), {}, payload);
}

/** @brief The NAME segments of the PATHs inside a FWD frame — `[dst, src]`. */
std::vector<std::vector<std::string>> paths_of(std::span<const std::byte> frame) {
    std::vector<std::vector<std::string>> out;
    const auto dec = tr::wire::decode(frame);
    if (!dec || dec->type != type_t::FWD) return out;
    for (const auto& child : dec->children) {
        if (child.type != type_t::PATH) continue;
        // Packed records (RFC-0018): `[u8 len][bytes]` in the PATH's own payload.
        std::vector<std::string> segs;
        for (std::size_t at = 0; at < child.payload.size();) {
            const auto len = static_cast<std::size_t>(static_cast<std::uint8_t>(child.payload[at]));
            if (len == 0 || at + 1 + len > child.payload.size()) break;
            segs.emplace_back(tr::detail::as_string_view(child.payload.subspan(at + 1, len)));
            at += 1 + len;
        }
        out.push_back(std::move(segs));
    }
    return out;
}

/** @brief A mount of @p w segments: `m0/m1/…`. */
std::vector<std::string> mount_segments(std::size_t w) {
    std::vector<std::string> segs;
    for (std::size_t i = 0; i < w; ++i) segs.push_back("m" + std::to_string(i));
    return segs;
}

/** @brief Those segments joined by `/` — the qualified name `add_child` takes. */
std::string join(std::span<const std::string> segs) {
    std::string out;
    for (const std::string& s : segs) {
        if (!out.empty()) out.push_back('/');
        out += s;
    }
    return out;
}

// --- #523: a mount of ANY width registers and resolves ------------------------

/**
 * @brief Register a mount of width @p w and forward through it.
 *
 * `w = 1, 2, 3` are the widths that already worked; `4`, `8` and `33` are the ones the fixed
 * peek window could never reach. 33 in particular crosses the old window several times over,
 * so a residual off-by-a-window bug cannot pass it by luck.
 */
void test_width(std::size_t w) {
    const std::vector<std::string> mount = mount_segments(w);
    const std::string name = join(mount);
    std::printf("mount width W=%zu (%s)\n", w, name.c_str());

    tr::graph::graph_t graph;
    tr::net::fwd_router_t router{graph};
    recording_link_t down;
    recording_link_t up;
    check(router.add_child(name, down), "a mount of this width registers");
    check(router.add_child("in", up), "the inbound link registers");
    check(router.registry().live_size() == 2, "both are live children");

    std::vector<std::string> dst = mount;
    dst.emplace_back("leaf");
    const std::vector<std::string> src = {"origin"};
    router.on_frame("in", make_fwd(dst, src));

    check(down.sent.size() == 1, "a FWD addressed through the mount is FORWARDED, not absorbed");
    if (down.sent.size() != 1) return;
    const auto paths = paths_of(down.sent[0]);
    if (paths.size() != 2) {
        check(false, "the forwarded frame still carries dst and src");
        return;
    }
    const std::vector<std::string> want_dst = {"leaf"};
    check(paths[0] == want_dst, "dst lost EXACTLY the mount's own segments");
    const std::vector<std::string> want_src = {"in", "origin"};
    check(paths[1] == want_src, "src grew by the inbound mount");
}

/** @brief A name no address could ever name is refused, always — not asserted in debug. */
void test_unaddressable_names_refused() {
    std::printf("unaddressable mount names are refused (#523)\n");
    tr::graph::graph_t graph;
    tr::net::fwd_router_t router{graph};
    recording_link_t link;

    check(!router.add_child("", link), "an empty name is refused");
    check(!router.add_child("a//b", link), "a name with an EMPTY segment is refused");
    check(!router.add_child("a/", link), "a trailing separator is refused");
    check(!router.add_child("/a", link), "a leading separator is refused");
    check(router.registry().live_size() == 0,
          "and NOTHING was registered — no healthy-looking ghost");

    // The one real bound: a `dst` carries at most `kMaxSegments` segments, so a wider mount
    // cannot be the prefix of any address that exists.
    const std::string too_wide = join(mount_segments(tr::graph::kMaxSegments + 1));
    check(!router.add_child(too_wide, link), "a name wider than the path budget is refused");
    const std::string at_budget = join(mount_segments(tr::graph::kMaxSegments));
    check(router.add_child(at_budget, link), "a name exactly AT the budget is accepted");
}

/** @brief Two mounts share a prefix — the wider one wins, whatever the table order. */
void test_longest_match_wins() {
    std::printf("longest match wins across widths\n");
    tr::graph::graph_t graph;
    tr::net::fwd_router_t router{graph};
    recording_link_t shallow;
    recording_link_t deep;
    recording_link_t in;
    // Registered SHALLOW FIRST so a pass that returned the first hit rather than the widest
    // would answer wrongly — the old descent got this right only by starting at the widest
    // width, which is exactly the loop the single pass replaces.
    (void)router.add_child("net/a/b", shallow);
    (void)router.add_child("net/a/b/c/d", deep);
    (void)router.add_child("in", in);

    router.on_frame("in", make_fwd(std::vector<std::string>{"net", "a", "b", "c", "d", "x"},
                                   std::vector<std::string>{"o"}));
    check(deep.sent.size() == 1 && shallow.sent.empty(), "the 5-segment mount beats the 3");

    router.on_frame("in", make_fwd(std::vector<std::string>{"net", "a", "b", "z"},
                                   std::vector<std::string>{"o"}));
    check(shallow.sent.size() == 1, "an address below only the shallow mount still resolves");
}

/** @brief A key that is a BYTE prefix but not a SEGMENT prefix must not match. */
void test_segment_boundaries() {
    std::printf("prefix matching respects segment boundaries\n");
    tr::graph::graph_t graph;
    tr::net::fwd_router_t router{graph};
    recording_link_t abc;
    recording_link_t in;
    (void)router.add_child("net/abc", abc);
    (void)router.add_child("in", in);

    // "net/ab" is a byte prefix of "net/abc"; "net"+"ab" as segments must NOT match it.
    router.on_frame(
        "in", make_fwd(std::vector<std::string>{"net", "ab", "x"}, std::vector<std::string>{"o"}));
    check(abc.sent.empty(), "net/ab does not match the mount net/abc");
    router.on_frame(
        "in", make_fwd(std::vector<std::string>{"net", "abc", "x"}, std::vector<std::string>{"o"}));
    check(abc.sent.size() == 1, "net/abc does");
}

/** @brief A dst naming the mount EXACTLY addresses the connection vertex — it terminates. */
void test_exact_mount_terminates() {
    std::printf("a dst naming the mount exactly terminates here\n");
    tr::graph::graph_t graph;
    tr::net::fwd_router_t router{graph};
    recording_link_t down;
    recording_link_t in;
    (void)router.add_child("net/a/b/c/d/e", down);
    (void)router.add_child("in", in);

    router.on_frame("in", make_fwd(std::vector<std::string>{"net", "a", "b", "c", "d", "e"},
                                   std::vector<std::string>{"o"}));
    check(down.sent.empty(), "nothing is forwarded — the address IS the mount");
}

/** @brief A narrower slot visited AFTER a wider one still resolves (the walker restart). */
void test_narrow_after_wide() {
    std::printf("a narrow mount registered after a wide one still resolves\n");
    tr::graph::graph_t graph;
    tr::net::fwd_router_t router{graph};
    recording_link_t wide;
    recording_link_t narrow;
    recording_link_t in;
    // Order matters: the pass reaches `wide` first and walks the dst out to width 6, then
    // must walk BACK to width 1 for `narrow`. A walker that only ever moved forward would
    // answer the second slot from a stale cursor.
    (void)router.add_child("q0/q1/q2/q3/q4/q5", wide);
    (void)router.add_child("solo", narrow);
    (void)router.add_child("in", in);

    router.on_frame("in",
                    make_fwd(std::vector<std::string>{"solo", "x"}, std::vector<std::string>{"o"}));
    check(narrow.sent.size() == 1 && wide.sent.empty(), "the 1-segment mount resolves");
}

/**
 * @brief A >4-segment mount forwards, and splits the address at the mount (#523).
 *
 * FAILS on the pre-lift code shape, and not by the assert: under `NDEBUG` the 5-segment mount
 * registers and the `FWD` falls through to the terminus, so nothing is forwarded.
 */
void test_deep_mount_forwards() {
    std::printf("deep mount: a 5-segment mount forwards the residual\n");
    tr::graph::graph_t graph;
    tr::net::fwd_router_t router{graph};
    recording_link_t down;
    recording_link_t in;
    (void)router.add_child("net/ws/s/rack/slot", down);  // 5 segments — past the old window
    (void)router.add_child("in", in);

    router.on_frame("in", make_fwd(std::vector<std::string>{"net", "ws", "s", "rack", "slot", "v"},
                                   std::vector<std::string>{"o"}));
    check(down.sent.size() == 1, "FWD: the deep mount forwards");
    std::vector<std::string> fwd_residual;
    if (down.sent.size() == 1) {
        const auto paths = paths_of(down.sent[0]);
        if (paths.size() == 2) fwd_residual = paths[0];
    }
    check(fwd_residual == std::vector<std::string>{"v"},
          "the address splits at the mount: only the residual travels");
    check(in.sent.empty(), "nothing goes back to the ingress link");
}

}  // namespace

int main() {
    for (const std::size_t w : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4},
                                std::size_t{8}, std::size_t{33}})
        test_width(w);
    test_unaddressable_names_refused();
    test_longest_match_wins();
    test_segment_boundaries();
    test_exact_mount_terminates();
    test_narrow_after_wide();
    test_deep_mount_forwards();
    return tr::testing::summary("mount_width");
}
