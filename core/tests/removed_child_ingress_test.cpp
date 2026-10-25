// SPDX-License-Identifier: Apache-2.0
//
/**
 * @file
 * @brief #2302 — a frame from a REMOVED child is dropped at the ingress: never forwarded,
 *        never applied, whichever door it arrives through.
 *
 * ## What is being defended
 *
 * `remove_child` tombstones the child's registry slot and its receive context (#884), but the
 * transport may still hold the receiver `add_child` installed, and an SDK host may still push a
 * frame through the public by-name door. Neither arrival was checked: through the stale context
 * a request still went out over the descent with the dead child's mount run in its `src`, and by
 * name the tombstoned slot looked like a name never registered, so `src` grew by the whole
 * mount name packed as ONE segment (`"net/downlink/cli"`), a route no reply can resolve.
 *
 * ## What the tests assert
 *
 * - control: a LIVE child's request is forwarded with `src` grown by its three-segment run;
 * - after `remove_child`, a request through the still-installed receiver is not forwarded, on
 *   both receive tiers (borrowed span and a two-link rope);
 * - after `remove_child`, the same request through the public by-name `on_frame` is not
 *   forwarded either;
 * - a WRITE addressed to this node from the removed child is not applied and not answered;
 * - a name the registry NEVER held keeps the documented by-name fallback;
 * - a re-add of the removed name routes again, through both doors.
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::testing::b_fwd;
using tr::testing::b_path;
using tr::testing::bytes_t;
using tr::testing::check;

/**
 * @brief A point-to-point link that records what the router sends down it and can push a frame
 *        up whatever receiver is installed on it — the ingress a real transport drives.
 */
class record_link_t : public transport_t {
   public:
    explicit record_link_t(bool ropes = false) : ropes_(ropes) {}
    void send(std::span<const std::byte> frame) override {
        sent.emplace_back(frame.begin(), frame.end());
    }
    [[nodiscard]] bool delivers_ropes() const override { return ropes_; }
    /** @brief Push @p frame up the span-tier receiver, borrowed. */
    void inject(std::span<const std::byte> frame) { rx_.deliver_borrowed(frame); }
    /** @brief Push @p frame up the rope-tier receiver as a two-link rope. */
    void inject_rope(std::span<const std::byte> frame) {
        tr::view::rope_t r;
        const std::size_t half = frame.size() / 2;
        for (const std::span<const std::byte> part : {frame.first(half), frame.subspan(half)}) {
            tr::view::segment_ptr_t seg = tr::view::heap_alloc(part.size());
            std::memcpy(seg->bytes.data(), part.data(), part.size());
            r.append(tr::view::view_t::over(std::move(seg)));
        }
        rx_.deliver_rope(std::move(r));
    }
    std::vector<bytes_t> sent; /**< @brief Every frame handed down, in order. */

   private:
    bool ropes_ = false;
};

constexpr std::string_view kIn = "net/downlink/cli"; /**< @brief The inbound child. */
constexpr std::string_view kOut = "net/uplink/b";    /**< @brief The egress child. */

/** @brief A `VALUE` TLV carrying the little-endian @p v. */
bytes_t b_value_u32(std::uint32_t v) {
    std::array<std::byte, 4> b{};
    for (std::size_t i = 0; i < 4; ++i) b[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
    bytes_t out;
    tr::wire::emit_tlv(out, tr::wire::type_t::VALUE, tr::wire::opt_t{}, b);
    return out;
}

/** @brief The bytes a local `read` of @p text answers, flattened (empty on a miss). */
bytes_t held(const graph_t& g, std::string_view text) {
    const auto r = g.read(path_t(std::string(text)));
    bytes_t out;
    std::vector<std::span<const std::byte>> iov;
    if (r.has_value() && (**r).try_to_iovec(iov))
        for (const std::span<const std::byte> s : iov) out.insert(out.end(), s.begin(), s.end());
    return out;
}

/** @brief One router, the inbound child @ref kIn and the egress child @ref kOut. */
struct node_t {
    graph_t g;
    fwd_router_t r{g};
    record_link_t cli;
    record_link_t b;

    explicit node_t(bool ropes = false) : cli(ropes) {
        (void)g.register_vertex(path_t("/sensor"), role_t::STORED_VALUE);
        check(r.attach_link(std::string(kIn), cli), "the inbound child registers");
        check(r.attach_link(std::string(kOut), b), "the egress child registers");
    }
};

/** @brief `FWD{READ}` for `b`'s `/sensor`, with `src = /origin` — the issue's frame. */
bytes_t forwarded_read() {
    return b_fwd(fwd_op_t::READ, b_path({"net", "uplink", "b", "sensor"}), b_path({"origin"}));
}

/** @brief @ref forwarded_read as `b` must receive it from @p child — `src` grown by its run. */
bytes_t as_forwarded(std::initializer_list<std::string_view> child) {
    std::vector<std::string_view> src(child);
    src.emplace_back("origin");
    return b_fwd(fwd_op_t::READ, b_path({"sensor"}), b_path(src));
}

void test_live_child_is_forwarded() {
    std::printf("control: a LIVE child's request is forwarded, src grown by its mount run\n");
    node_t n;
    n.cli.inject(forwarded_read());
    check(n.b.sent.size() == 1 && n.b.sent[0] == as_forwarded({"net", "downlink", "cli"}),
          "through the receiver: src = [net][downlink][cli][origin]");
    n.r.on_frame(kIn, forwarded_read());
    check(n.b.sent.size() == 2 && n.b.sent[1] == as_forwarded({"net", "downlink", "cli"}),
          "by name: the same three-segment run");
}

void test_stale_receiver_is_dropped(bool ropes) {
    std::printf("after remove_child, a frame through the still-installed %s receiver is dropped\n",
                ropes ? "rope" : "span");
    node_t n(ropes);
    check(n.r.remove_child(kIn), "the inbound child is removed");
    if (ropes)
        n.cli.inject_rope(forwarded_read());
    else
        n.cli.inject(forwarded_read());
    check(n.b.sent.empty(), "nothing is forwarded over the descent");
    check(n.cli.sent.empty(), "and nothing is answered");
}

void test_tombstoned_name_is_dropped() {
    std::printf("after remove_child, a frame by the removed NAME is dropped\n");
    node_t n;
    check(n.r.remove_child(kIn), "the inbound child is removed");
    n.r.on_frame(kIn, forwarded_read());
    check(n.b.sent.empty(), "on_frame: nothing is forwarded (no one-segment `net/downlink/cli`)");
}

void test_removed_child_write_is_not_applied() {
    std::printf("a WRITE from a removed child is neither applied nor answered\n");
    node_t n;
    const bytes_t src = b_path({"origin"});
    n.cli.inject(b_fwd(fwd_op_t::WRITE, b_path({"sensor"}), src, {}, b_value_u32(1)));
    const bytes_t first = held(n.g, "/sensor");
    check(!first.empty() && n.cli.sent.size() == 1, "control: a live child's WRITE lands");
    check(n.r.remove_child(kIn), "the inbound child is removed");
    n.cli.inject(b_fwd(fwd_op_t::WRITE, b_path({"sensor"}), src, {}, b_value_u32(2)));
    n.r.on_frame(kIn, b_fwd(fwd_op_t::WRITE, b_path({"sensor"}), src, {}, b_value_u32(3)));
    check(held(n.g, "/sensor") == first, "neither the stale-receiver nor the by-name WRITE lands");
    check(n.cli.sent.size() == 1, "and neither is answered");
}

void test_unregistered_name_keeps_the_fallback() {
    std::printf("a name the registry never held keeps the by-name fallback\n");
    node_t n;
    n.r.on_frame("host", forwarded_read());
    check(n.b.sent.size() == 1 && n.b.sent[0] == as_forwarded({"host"}),
          "src grows by the bare inbound name, as documented");
}

void test_readd_routes_again() {
    std::printf("a re-added name routes again, through both doors\n");
    node_t n;
    check(n.r.remove_child(kIn), "the inbound child is removed");
    check(n.r.attach_link(std::string(kIn), n.cli), "and re-added over the same link");
    n.cli.inject(forwarded_read());
    n.r.on_frame(kIn, forwarded_read());
    check(n.b.sent.size() == 2 && n.b.sent[0] == as_forwarded({"net", "downlink", "cli"}) &&
              n.b.sent[1] == n.b.sent[0],
          "both forwarded with the three-segment run");
}

}  // namespace

int main() {
    test_live_child_is_forwarded();
    test_stale_receiver_is_dropped(false);
    test_stale_receiver_is_dropped(true);
    test_tombstoned_name_is_dropped();
    test_removed_child_write_is_not_applied();
    test_unregistered_name_keeps_the_fallback();
    test_readd_routes_again();
    return tr::testing::summary("removed_child_ingress");
}
