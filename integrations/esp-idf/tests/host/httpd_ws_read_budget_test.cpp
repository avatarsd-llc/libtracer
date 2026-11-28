/**
 * @file
 * @brief #2959 — the per-session READ budget charges each READ to the session that delivered it,
 *        measured over the httpd WebSocket link with two sessions.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The router charges a READ to `transport_t::inbound_tally()` of the delivering link (#2774,
 * #2865). The httpd link answers that with the session slot's tally. If it fell back to the link's
 * one shared tally, two sessions on one link would be charged for each other's READs. This suite
 * drives the REAL chip translation unit (`httpd_ws_link.cpp`) against the host fake of
 * `esp_http_server`, with a real `graph_t` and `fwd_router_t` behind it, built with
 * `kReadBudget = 3`, and walks two sessions, A and B, through one read run:
 *
 *   0. A alone sends 5 READs, then the run is closed: all 5 are answered (a session alone in a
 *      run is never refused).
 *   1. B sends 1 READ: answered.
 *   2. A sends 5 READs: the 1st to 3rd are answered, the 4th and 5th get BACKPRESSURE.
 *   3. B sends 1 READ: answered, because it is B's 2nd in the run. This step catches B being
 *      charged to another session's slot (for example a stale peer left over from A).
 *   4. B sends 2 more: its 3rd is answered, its 4th gets BACKPRESSURE.
 *   5. After the run is closed, B's next READ is answered.
 *
 * What each step catches. If `inbound_tally()` answered null or one link-wide tally on this
 * peer-named link, A and B would share one tally and a run would never count two sessions, so
 * nothing would ever be refused: step 2 (A's 4th and 5th get BACKPRESSURE) and step 4's last
 * check (B's 4th) fail.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "fake_httpd.hpp"
#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/error.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_poly_ptr.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer_esp/httpd_ws_link.hpp"
#include "test_values.hpp"
#include "tlv_tree.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::httpd_ws_link_t;
using tr::testing::b_fwd;
using tr::testing::b_path;
using tr::view::rope_t;
using tr::wire::type_t;

static_assert(tr::graph::config_t::kReadBudget == 3,
              "this suite is built with the READ budget at 3");

constexpr int kFdA = 800;
constexpr int kFdB = 801;

int g_failures = 0;

/** @brief Record one assertion's verdict. */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief The fake server's handle, as the adopting constructor takes it. */
httpd_handle_t handle() { return static_cast<httpd_handle_t>(&fake_httpd::instance()); }

/** @brief Drain the control queue to quiescence, as the httpd task does. */
void drain() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/** @brief What a reply frame answered. */
enum class answer_t : std::uint8_t { RESULT, BACKPRESSURE, OTHER };

/** @brief Classify one `FWD{REPLY}` by its kind and, for an error, its registered code. */
answer_t classify(std::span<const std::byte> frame) {
    const auto d = tr::wire::decode(frame);
    if (!d || d->type != type_t::FWD || d->children.size() < 4) return answer_t::OTHER;
    const auto& c = d->children;
    if (c[3].type != type_t::VALUE || c[3].payload.size() != 1) return answer_t::OTHER;
    const auto kind = static_cast<reply_kind_t>(std::to_integer<std::uint8_t>(c[3].payload[0]));
    if (kind == reply_kind_t::RESULT) return answer_t::RESULT;
    if (kind != reply_kind_t::ERROR || c.size() != 5 || c[4].children.size() != 1)
        return answer_t::OTHER;
    const auto& err = c[4].children[0];
    if (err.children.size() != 1 || err.children[0].payload.size() != 2) return answer_t::OTHER;
    return tr::detail::load_le<std::uint16_t>(err.children[0].payload) ==
                   std::to_underlying(tr::wire::err_t::FLOW_BACKPRESSURE)
               ? answer_t::BACKPRESSURE
               : answer_t::OTHER;
}

/** @brief A VALUE TLV carrying one byte: what `/sensor/temp` holds, so a READ's reply decodes. */
std::vector<std::byte> b_value_u8(std::uint8_t v) {
    const std::byte b{v};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, tr::wire::opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}

/** @brief A minimal frame body: the first data frame claims the session. */
const std::byte kClaim[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

/** @brief Answers one session received. */
struct tally_t {
    std::size_t result = 0;
    std::size_t backpressure = 0;
    std::size_t other = 0;
};

/** @brief Send @p n READs from @p fd, one at a time, and tally what came back on that socket. */
tally_t reads(int fd, std::size_t n) {
    const std::vector<std::byte> read =
        b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"back"}));
    tally_t t;
    for (std::size_t i = 0; i < n; ++i) {
        fake_httpd::instance().clear_sent_frames();
        (void)fake_httpd::instance().deliver_frame(fd, read);
        drain();
        std::size_t replies = 0;
        for (const auto& f : fake_httpd::instance().sent_frames()) {
            if (f.fd != fd) {
                ++t.other;
                continue;
            }
            ++replies;
            switch (classify(f.payload)) {
                case answer_t::RESULT:
                    ++t.result;
                    break;
                case answer_t::BACKPRESSURE:
                    ++t.backpressure;
                    break;
                case answer_t::OTHER:
                    ++t.other;
                    break;
            }
        }
        if (replies != 1) ++t.other;
    }
    return t;
}

/** @brief True when @p t holds exactly @p r results, @p b BACKPRESSURE replies and nothing else. */
bool is(const tally_t& t, std::size_t r, std::size_t b) {
    return t.result == r && t.backpressure == b && t.other == 0;
}

void test_the_budget_is_charged_per_session() {
    std::printf("#2959 — the READ budget (3) over one httpd link, two sessions:\n");
    graph_t g;
    fwd_router_t router(g);
    (void)g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    check(g.write(path_t("/sensor/temp"), rope_t{tr::testing::make_value(b_value_u8(42))})
              .has_value(),
          "/sensor/temp holds a value");
    auto link = tr::mem::make_poly<httpd_ws_link_t>(tr::mem::net_source(), handle(), "/ws",
                                                    tr::net::httpd_ws_config_t{.peer_named = true});
    check(link->ok(), "the adopting link registered its URI");
    check(router.attach_link("ws", *link), "the link is attached to the router");

    fake_httpd::instance().open_session(kFdA);
    (void)fake_httpd::instance().deliver_frame(kFdA, kClaim);
    fake_httpd::instance().open_session(kFdB);
    (void)fake_httpd::instance().deliver_frame(kFdB, kClaim);
    drain();

    // 0 — a session alone in a run is never refused.
    check(is(reads(kFdA, 5), 5, 0), "0: A alone, 5 READs: all answered");
    router.close_read_run();
    // 1
    check(is(reads(kFdB, 1), 1, 0), "1: B's 1st READ is answered");
    // 2
    check(is(reads(kFdA, 3), 3, 0), "2: A's 1st to 3rd READ are answered");
    check(is(reads(kFdA, 2), 0, 2), "2: A's 4th and 5th get BACKPRESSURE");
    // 3
    check(is(reads(kFdB, 1), 1, 0),
          "3: B's 2nd READ is answered (not charged to another session's slot)");
    // 4
    check(is(reads(kFdB, 1), 1, 0), "4: B's 3rd READ is answered");
    check(is(reads(kFdB, 1), 0, 1), "4: B's 4th READ gets BACKPRESSURE");
    // 5
    router.close_read_run();
    check(is(reads(kFdB, 1), 1, 0), "5: after close_read_run, B's next READ is answered");

    fake_httpd::instance().close_all();
    drain();
    link.reset();
}

}  // namespace

int main() {
    std::printf("== httpd_ws_read_budget_test ==\n");
    test_the_budget_is_charged_per_session();
    std::printf("%s\n", g_failures == 0 ? "httpd_ws_read_budget: ALL PASS (0 failures)"
                                        : "httpd_ws_read_budget: FAILURES");
    return g_failures == 0 ? 0 : 1;
}
