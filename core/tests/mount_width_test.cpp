/**
 * @file
 * @brief The mount WIDTH lift: a mount of any width registers, resolves, and resolves the
 *        SAME way on the FWD plane and the COMPACT plane (#523, #765).
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
 * **#765** — a label binding records where an address SPLIT into "local mount" and "remote
 * residual". Registering a deeper mount moves that split, and nothing noticed: a full `FWD`
 * resolved against the new mount while a `COMPACT` riding the old label still used the old
 * one. The two planes disagreed about the same address. Until the width bound was lifted that
 * was unreachable — but only because NEITHER plane could reach a deeper mount. Agreement by
 * mutual failure, not by construction.
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
 *   - delete `routable_mount_name` → `unaddressable_names_refused` fails;
 *   - delete the `mount_gen` stamp or its comparison → `shape_change_is_seen` fails.
 *
 * The suite prints **63** assertions across those cases; the OOM half of `add_child`'s new
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

#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;

/** @brief A link that records every frame it is asked to send. */
struct recording_link_t : tr::net::transport_t {
    std::vector<std::vector<std::byte>> sent; /**< @brief Frames handed to this endpoint. */
    void send(std::span<const std::byte> f) override { sent.emplace_back(f.begin(), f.end()); }
};

/** @brief `["a","b",…]` as a PATH TLV appended to @p out. */
void emit_path(std::vector<std::byte>& out, std::span<const std::string> segs) {
    std::vector<std::byte> body;
    for (const std::string& s : segs) tr::wire::emit_name(body, s);
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
}

/** @brief A `FWD{WRITE, dst, src, payload}` frame over the given segment lists. */
std::vector<std::byte> make_fwd(std::span<const std::string> dst,
                                std::span<const std::string> src) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    emit_path(body, dst);
    emit_path(body, src);
    const std::byte payload[2] = {std::byte{0x01}, std::byte{0x02}};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(payload, 2));
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

/** @brief The NAME segments of the PATHs inside a FWD frame — `[dst, src]`. */
std::vector<std::vector<std::string>> paths_of(std::span<const std::byte> frame) {
    std::vector<std::vector<std::string>> out;
    const auto dec = tr::wire::decode(frame);
    if (!dec || dec->type != type_t::FWD) return out;
    for (const auto& child : dec->children) {
        if (child.type != type_t::PATH) continue;
        std::vector<std::string> segs;
        for (const auto& seg : child.children) {
            if (seg.type == type_t::NAME)
                segs.emplace_back(tr::detail::as_string_view(seg.payload));
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
    router.add_child("net/a/b", shallow);
    router.add_child("net/a/b/c/d", deep);
    router.add_child("in", in);

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
    router.add_child("net/abc", abc);
    router.add_child("in", in);

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
    router.add_child("net/a/b/c/d/e", down);
    router.add_child("in", in);

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
    router.add_child("q0/q1/q2/q3/q4/q5", wide);
    router.add_child("solo", narrow);
    router.add_child("in", in);

    router.on_frame("in",
                    make_fwd(std::vector<std::string>{"solo", "x"}, std::vector<std::string>{"o"}));
    check(narrow.sent.size() == 1 && wide.sent.empty(), "the 1-segment mount resolves");
}

// --- #765: the FWD and COMPACT planes agree BY CONSTRUCTION --------------------

/** @brief The label a stale-label observer last saw, and how many times it fired. */
struct stale_log_t {
    std::size_t hits = 0;    /**< @brief Observer invocations. */
    std::uint16_t label = 0; /**< @brief The last stale label. */
    std::string inbound;     /**< @brief The link it arrived on. */
};

/** @brief The stale-label observer, as a plain function pointer (ADR-0047). */
void on_stale(void* ctx, std::string_view inbound, std::uint16_t label) {
    auto* const log = static_cast<stale_log_t*>(ctx);
    ++log->hits;
    log->label = label;
    log->inbound = std::string(inbound);
}

/**
 * @brief A >4-segment mount resolves IDENTICALLY through FWD and through COMPACT (#765).
 *
 * FAILS on the pre-lift code shape, and not by the assert: under `NDEBUG` the 5-segment mount
 * registers, the `FWD` falls through to the terminus (nothing is forwarded) and the ADVERTISE
 * is absorbed as a local binding. The two planes "agree" only in that both are wrong — which
 * is precisely what this issue called agreement by mutual failure.
 */
void test_deep_mount_planes_agree() {
    std::printf("deep mount: FWD and COMPACT resolve identically (#765)\n");
    tr::graph::graph_t graph;
    tr::net::fwd_router_t router{graph};
    recording_link_t down;
    recording_link_t in;
    router.add_child("net/ws/s/rack/slot", down);  // 5 segments — past the old window
    router.add_child("in", in);

    // Plane 1 — a full FWD.
    router.on_frame("in", make_fwd(std::vector<std::string>{"net", "ws", "s", "rack", "slot", "v"},
                                   std::vector<std::string>{"o"}));
    check(down.sent.size() == 1, "FWD: the deep mount forwards");
    std::vector<std::string> fwd_residual;
    if (down.sent.size() == 1) {
        const auto paths = paths_of(down.sent[0]);
        if (paths.size() == 2) fwd_residual = paths[0];
    }

    // Plane 2 — an ADVERTISE over the same address, then a COMPACT on the bound label.
    std::vector<std::byte> route;
    emit_path(route, std::vector<std::string>{"net", "ws", "s", "rack", "slot", "v"});
    router.on_frame("in", tr::net::encode_advertise(11, route));
    check(down.sent.size() == 2, "COMPACT plane: the advertise is RELAYED, not absorbed");
    std::vector<std::string> adv_residual;
    if (down.sent.size() == 2) {
        const auto dec = tr::wire::decode(down.sent[1]);
        if (dec && dec->children.size() >= 2) {
            for (const auto& seg : dec->children[1].children) {
                if (seg.type == type_t::NAME)
                    adv_residual.emplace_back(tr::detail::as_string_view(seg.payload));
            }
        }
    }
    check(!fwd_residual.empty() && fwd_residual == adv_residual,
          "the two planes split the SAME address at the SAME point");

    const std::byte pl[2] = {std::byte{0xAA}, std::byte{0xBB}};
    std::vector<std::byte> value;
    tr::wire::emit_tlv(value, type_t::VALUE, opt_t{}, std::span<const std::byte>(pl, 2));
    router.on_frame("in", tr::net::encode_compact(11, value));
    check(down.sent.size() == 3, "and a COMPACT on that label relays downstream");
    check(in.sent.empty(), "no NACK — the binding is valid against the current mount shape");
}

/**
 * @brief Registering a DEEPER mount restamps the label plane, so it cannot silently diverge.
 *
 * The exact scenario #765 describes: bind through `net/ws/s`, then register `net/ws/s/rack`.
 * The FWD plane now resolves the deeper mount. The COMPACT plane must NOT keep delivering
 * through the old split — it takes the RFC-0004 §E.1 self-heal (drop, observe, NACK).
 */
void test_shape_change_is_seen() {
    std::printf("a deeper registration restamps live label bindings (#765)\n");
    tr::graph::graph_t graph;
    tr::net::fwd_router_t router{graph};
    recording_link_t shallow;
    recording_link_t deeper;
    recording_link_t in;
    stale_log_t log;
    router.on_stale_label(&on_stale, &log);
    router.add_child("net/ws/s", shallow);
    router.add_child("in", in);

    std::vector<std::byte> route;
    emit_path(route, std::vector<std::string>{"net", "ws", "s", "rack", "v"});
    router.on_frame("in", tr::net::encode_advertise(21, route));
    check(shallow.sent.size() == 1, "the label binds through the shallow mount");

    const std::byte pl[2] = {std::byte{0x01}, std::byte{0x02}};
    std::vector<std::byte> value;
    tr::wire::emit_tlv(value, type_t::VALUE, opt_t{}, std::span<const std::byte>(pl, 2));
    router.on_frame("in", tr::net::encode_compact(21, value));
    check(shallow.sent.size() == 2 && log.hits == 0, "and relays while the shape holds");

    // The move: a deeper mount under the same prefix. Both links are alive, both targets are
    // what they always were — only the SPLIT moved.
    router.add_child("net/ws/s/rack", deeper);

    // FWD plane: resolves the new, deeper mount.
    router.on_frame("in", make_fwd(std::vector<std::string>{"net", "ws", "s", "rack", "v"},
                                   std::vector<std::string>{"o"}));
    check(deeper.sent.size() == 1, "FWD now takes the deeper mount");

    // COMPACT plane: must NOT keep using the old split.
    const std::size_t before = shallow.sent.size();
    router.on_frame("in", tr::net::encode_compact(21, value));
    check(shallow.sent.size() == before,
          "the stale binding delivers NOTHING through the old split");
    check(log.hits == 1 && log.label == 21, "the stale-label observer fires with that label");
    check(log.inbound == "in", "and names the link the stale COMPACT arrived on");
    check(!in.sent.empty(), "and a HANDLE_NACK goes back to prompt a re-advertise");

    // Self-heal: the peer re-advertises, and the flow resumes against the NEW shape.
    router.on_frame("in", tr::net::encode_advertise(21, route));
    check(deeper.sent.size() == 2, "the re-advertise binds through the deeper mount");
    const std::size_t deep_before = deeper.sent.size();
    router.on_frame("in", tr::net::encode_compact(21, value));
    check(deeper.sent.size() == deep_before + 1, "and COMPACT now agrees with FWD");
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
    test_deep_mount_planes_agree();
    test_shape_change_is_seen();
    return tr::testing::summary("mount_width");
}
