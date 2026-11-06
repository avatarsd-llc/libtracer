// SPDX-License-Identifier: Apache-2.0
//
/**
 * @file
 * @brief #2580 — the router's compile-time frame observer, `config_t::frame_observer_t`, is
 *        called once per inbound frame, on every link, with that link's name, before dispatch.
 *
 * ## What is being defended
 *
 * An application keeps per-link liveness and traffic accounting by naming a frame observer in
 * its configuration. The router calls `frame_observer_t::on_frame(inbound_link_name)` at the
 * top of both receive tiers and holds nothing for it. A frame that dispatch then refuses (too
 * short, a type the router does not serve, a READ of a vertex that does not exist) is still a
 * frame that arrived, so it is observed too.
 *
 * ## What the tests assert
 *
 * - The default is the no-op `tr::net::no_frame_observer_t`, so a build that names no
 *   observer pays nothing (a compile-time check here; the symbol ratchet and RAM ledger gate
 *   the rest).
 * - Under the test preset's counting observer, every frame below moves the count by exactly
 *   one and leaves its link's name as the last one seen: two span-tier links, a rope-tier link
 *   with both a one-link and a two-link rope, the public by-name door, and a bus peer (whose
 *   name is the peer's, run only where the build binds `tr::net::kBusLinks`).
 * - BEFORE dispatch: each link records the count at the moment the router sends a reply down
 *   it, and that count already includes the frame being answered.
 * - Refused frames count: a 2-byte runt, a non-`FWD` TLV, and a READ of a missing vertex.
 *
 * A CI leg that binds its own fragment has no counting observer; there the behavioural cases
 * print SKIP and the test exits 77.
 */
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::b_fwd;
using tr::testing::b_path;
using tr::testing::check;

static_assert(
    std::is_same_v<tr::graph::default_config_t::frame_observer_t, tr::net::no_frame_observer_t>,
    "the default frame observer is the no-op");
static_assert(std::is_empty_v<tr::net::no_frame_observer_t>, "the no-op holds nothing");
static_assert((tr::net::no_frame_observer_t::on_frame("x"), true),
              "the no-op is a constant expression: nothing to run");

/** @brief The counting observer's shape, as the test preset binds it. */
template <class O>
concept counting_observer = requires {
    { O::calls } -> std::convertible_to<std::size_t>;
    { O::last_name() } -> std::same_as<std::string_view>;
};

/**
 * @brief A point-to-point link that pushes frames up its own receivers and records, for every
 *        frame the router sends down it, the observer's count at that moment.
 */
template <class O>
class record_link_t : public transport_t {
   public:
    explicit record_link_t(bool ropes = false) : ropes_(ropes) {}
    void send(std::span<const std::byte>) override { calls_at_send.push_back(O::calls); }
    [[nodiscard]] bool delivers_ropes() const override { return ropes_; }
    /** @brief Push @p frame up the span-tier receiver, borrowed. */
    void inject(std::span<const std::byte> frame) { rx_.deliver_borrowed(frame); }
    /** @brief Push @p frame up the rope-tier receiver as a rope of @p links links (1 or 2). */
    void inject_rope(std::span<const std::byte> frame, std::size_t links) {
        tr::view::rope_t r;
        const std::size_t cut = links == 1 ? frame.size() : frame.size() / 2;
        for (const std::span<const std::byte> part : {frame.first(cut), frame.subspan(cut)}) {
            if (part.empty()) continue;
            tr::view::segment_ptr_t seg = tr::view::heap_alloc(part.size());
            std::memcpy(seg->bytes.data(), part.data(), part.size());
            r.append(tr::view::view_t::over(std::move(seg)));
        }
        (void)rx_.deliver_rope(std::move(r));
    }
    /** @brief `O::calls` at each send, in order. */
    std::vector<std::size_t> calls_at_send;

   private:
    bool ropes_ = false;
};

/** @brief A bus with one fixed peer, `n7`, whose directed endpoint is @ref peer. */
template <class O>
struct bus_record_link_t : transport_t, tr::net::bus_link_t {
    record_link_t<O> peer; /**< @brief The directed endpoint `peer_link("n7")` hands out. */
    void send(std::span<const std::byte>) override {}
    void send(std::span<const std::span<const std::byte>>) override {}
    tr::net::bus_link_t* bus() override { return this; }
    transport_t* peer_link(std::string_view name) override {
        return name == "n7" ? &peer : nullptr;
    }
    void enumerate_peers(const tr::net::bus_link_t::peer_visitor_t& visit) const override {
        visit("n7");
    }
    [[nodiscard]] std::string_view peer_name(tr::net::peer_handle_t h,
                                             std::span<char>) const override {
        return h.valid() && h.index == 0 ? std::string_view{"n7"} : std::string_view{};
    }
    /** @brief Push @p frame up the peer-named slot, tagged as peer `n7`. */
    void inject(std::span<const std::byte> frame) {
        peer_rx_.deliver_borrowed(tr::net::peer_handle_t{0, 1}, frame);
    }
};

/** @brief One frame: the count moved by exactly one, and @p link was the name it saw. */
template <class O, class Push>
void expect_one(const char* what, std::string_view link, Push&& push) {
    const std::size_t before = O::calls;
    push();
    std::printf("%s:\n", what);
    check(O::calls == before + 1, "  observed exactly once");
    check(O::last_name() == link, "  with the inbound link's name");
}

/** @brief The last reply on @p l was sent after the frame it answers was observed. */
template <class O>
void expect_observed_before_dispatch(const record_link_t<O>& l) {
    check(!l.calls_at_send.empty() && l.calls_at_send.back() == O::calls,
          "  dispatch answered it, after the observer had counted it");
}

template <class O>
void run() {
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    fwd_router_t router(g);
    record_link_t<O> a;
    record_link_t<O> b;
    record_link_t<O> r(/*ropes=*/true);
    bus_record_link_t<O> bus;
    (void)router.attach_link("a", a);
    (void)router.attach_link("b", b);
    (void)router.attach_link("r", r);

    const auto read = b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"x", "back"}));
    const auto missing = b_fwd(fwd_op_t::READ, b_path({"nope"}), b_path({"x", "back"}));
    const std::array<std::byte, 2> runt{std::byte{0x01}, std::byte{0x02}};
    std::vector<std::byte> not_fwd;
    tr::wire::emit_tlv(not_fwd, type_t::VALUE, opt_t{}, read);

    expect_one<O>("a READ on span link a", "a", [&] { a.inject(read); });
    expect_observed_before_dispatch(a);
    expect_one<O>("a READ on span link b", "b", [&] { b.inject(read); });
    expect_observed_before_dispatch(b);
    expect_one<O>("a READ of a missing vertex on a (answered with an error)", "a",
                  [&] { a.inject(missing); });
    expect_observed_before_dispatch(a);
    expect_one<O>("a 2-byte runt on b (dropped)", "b", [&] { b.inject(runt); });
    expect_one<O>("a non-FWD TLV on a (refused as an unserved type)", "a",
                  [&] { a.inject(not_fwd); });
    expect_one<O>("a one-link rope READ on rope link r", "r", [&] { r.inject_rope(read, 1); });
    expect_observed_before_dispatch(r);
    expect_one<O>("a two-link rope READ on rope link r", "r", [&] { r.inject_rope(read, 2); });
    expect_observed_before_dispatch(r);
    expect_one<O>("a two-link rope runt on r (dropped)", "r", [&] { r.inject_rope(runt, 2); });
    expect_one<O>("the public by-name span door", "door", [&] { router.on_frame("door", read); });

    if constexpr (tr::net::kBusLinks) {
        (void)router.attach_link("bus", bus);
        expect_one<O>("a READ from bus peer n7 (named as the peer)", "n7", [&] {
            bus.inject(b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"n7", "back"})));
        });
        expect_observed_before_dispatch(bus.peer);
    } else {
        std::printf("bus peer case SKIPPED: this build closed the ADR-0044 bus module out\n");
    }
}

}  // namespace

int main() {
    std::printf("#2580 config_t::frame_observer_t\n\n");
    using observer_t = tr::graph::config_t::frame_observer_t;
    if constexpr (counting_observer<observer_t>) {
        run<observer_t>();
    } else {
        std::printf("SKIP: this build's fragment binds no counting frame observer\n");
        return 77;
    }
    return tr::testing::summary("fwd_frame_observer");
}
