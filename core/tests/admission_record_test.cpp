/**
 * @file
 * @brief #2814: the charges a subscriber edge's admission takes are one record — refused whole,
 *        and given back whole by the one release every edge that ends goes through.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Every vector reads the charges through one ledger (`ledger_t`): the producer's listener count
 * and a child's listeners-above (RFC-0005, #635), the hold on each link (#1816), and each
 * session's ADMIT/RELEASE balance (#1841). The index entry is read through
 * `graph_t::link_edge_candidates`.
 *
 * 1. A refusal at each step of the admission leaves nothing charged: the edge block, the
 *    session's ADMIT, the departure index entry, and the slot verb's three refusals (a slot the
 *    index names does not exist, the source could not carry the edge, the slot changed under
 *    the write). The table-source steps are reached by refusing every draw in turn; the ADMIT
 *    by an embedder that refuses it, which leaves no index entry either.
 * 2. A local edge has no session, even when the write that made it carried a link: it is
 *    never offered to the session seam and never indexed.
 * 3. A replace that displaces a delivering edge, and one that displaces a suspended edge, are
 *    each net zero on every charge: afterwards the ledger reads the new edge's record alone,
 *    and clearing it reads nothing.
 *
 * The vectors catch a release that leaves a charge out. Drop the count, the hold or the RELEASE
 * from `graph_t::release_admission`, or answer a refused index entry without releasing the
 * record: the refusal sweep of vector 1 fails for each, and vector 3 for the first three too.
 * In a build without `kSessionAdmission` the file is skipped, as `session_admission_test` is:
 * the categories below are told apart by the ADMIT.
 */

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "graph_sinks.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::field_path_t;
using tr::graph::field_step_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::session_event_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;
using tr::testing::make_value;
using tr::testing::test_link;

/** @brief A source that serves @ref arm's budget of blocks and then refuses, by value. */
class gate_source_t final : public tr::mem::block_source_t {
   public:
    gate_source_t() noexcept : tr::mem::block_source_t("gate") {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (armed_ && allow_ == 0) return nullptr;
        if (armed_) --allow_;
        return ::operator new(bytes, std::align_val_t{align}, std::nothrow);
    }
    void release(void* p, std::size_t, std::size_t align) noexcept override {
        ::operator delete(p, std::align_val_t{align});
    }
    /** @brief Serve @p allow more blocks, then refuse. */
    void arm(std::size_t allow) noexcept {
        armed_ = true;
        allow_ = allow;
    }
    /** @brief Serve everything again. */
    void disarm() noexcept { armed_ = false; }

   private:
    bool armed_ = false;    /**< @brief Refusing past the budget? */
    std::size_t allow_ = 0; /**< @brief Blocks left before refusing. */
};

/** @brief A SUBSCRIBER TLV whose PATH names @p marker. */
std::vector<std::byte> b_subscriber(std::string_view marker) {
    std::vector<std::byte> path_body;
    (void)tr::wire::emit_path_segment(path_body, marker);
    std::vector<std::byte> body;
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{}, path_body);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, body);
    return out;
}

/** @brief A PATH TLV naming @p link — the return route a wire subscribe arrives with. */
std::vector<std::byte> b_route(std::string_view link) {
    std::vector<std::byte> body;
    (void)tr::wire::emit_path_segment(body, link);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief An empty `STATUS` — the `:subscribers[N]` clear sentinel. */
std::vector<std::byte> b_clear() {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::STATUS, opt_t{}, std::span<const std::byte>{});
    return out;
}

/** @brief The `:subscribers[N]` field, or the `:subscribers[]` append for no @p slot. */
field_path_t subscribers(std::optional<std::uint16_t> slot) {
    field_path_t f;
    (void)f.steps.push_back(field_step_t{.name = "subscribers",
                                         .indexed = true,
                                         .append = !slot.has_value(),
                                         .index = slot.value_or(0)});
    return f;
}

/** @brief One wire subscribe at @p v over session @p link as @p subject: a routed edge. */
tr::graph::result_t<void> wire_sub(graph_t& g, vertex_handle_t v, std::string_view link,
                                   std::string_view subject) {
    return g.subscribe_wire(v, make_value(b_subscriber("t")), make_value(b_route(link)), link, {},
                            subject, {}, test_link(link));
}

/**
 * @brief A `:subscribers[N]` write of @p payload by @p caller, carried over session @p link
 *        (none when empty), as the router's walker hands it in.
 */
tr::graph::result_t<void> slot_write(graph_t& g, vertex_handle_t v, std::optional<std::uint16_t> n,
                                     const std::vector<std::byte>& payload, std::string_view caller,
                                     std::string_view link) {
    return g.write(v, subscribers(n), tr::view::rope_t{make_value(payload)}, caller, nullptr, link,
                   link.empty() ? tr::graph::link_pair_t{} : test_link(link));
}

/**
 * @brief Every charge an admission takes, as the embedder and the net plane see them.
 *
 * Installs the link-hold and the session-admission seams, and the link-name answer a router
 * would give for the sessions below (#1941), so a RELEASE names the session its ADMIT did.
 */
struct ledger_t {
    std::map<std::uint32_t, int> holds; /**< @brief Holds per link (its `test_link` slot). */
    std::map<std::string, int> charged; /**< @brief Charges per session. */
    int admits = 0;                     /**< @brief ADMITs granted, ever. */
    bool refuse = false;                /**< @brief Refuse every ADMIT? */
    bool negative = false;              /**< @brief Did a hold or a charge go below zero? */
    std::function<void()> race;         /**< @brief Run on another thread inside ADMIT #race_at. */
    int race_at = 0; /**< @brief Which granted ADMIT runs @ref race; 0 for none. */

    /** @brief The link names a transport plane would answer for this file's pairs. */
    static const std::vector<std::string>& names() {
        static const std::vector<std::string> kNames{"s1", "s2"};
        return kNames;
    }

    /** @brief Install every seam on @p g, pointing at this ledger. */
    void install(graph_t& g) {
        tr::testing::name_test_links(g, names());
        g.set_hook(&tr::graph::graph_hooks_t::link_hold,
                   {[](void* ctx, tr::graph::link_pair_t link, bool held) {
                        auto& self = *static_cast<ledger_t*>(ctx);
                        int& n = self.holds[link.conn];
                        n += held ? 1 : -1;
                        self.negative |= n < 0;
                    },
                    this});
        [&g, this](const auto& hook) {
            using hook_t = std::remove_cvref_t<decltype(hook)>;
            if constexpr (requires { hook.fn; })
                g.set_hook(&tr::graph::graph_hooks_t::session_admission,
                           hook_t{&ledger_t::on_session, this});
        }(tr::graph::graph_hooks_t{}.session_admission);
    }

    /** @brief The installed session-admission hook. */
    static bool on_session(void* ctx, const session_event_t& e) {
        auto& self = *static_cast<ledger_t*>(ctx);
        int& n = self.charged[std::string(e.session)];
        switch (e.kind) {
            case session_event_t::kind_t::ADMIT:
                if (self.refuse) return false;
                ++n;
                if (++self.admits == self.race_at && self.race) std::thread(self.race).join();
                return true;
            case session_event_t::kind_t::RELEASE:
                --n;
                self.negative |= n < 0;
                return true;
            case session_event_t::kind_t::END:
                n = 0;
                return true;
        }
        return true;
    }

    /**
     * @brief Does the ledger read exactly @p subs counted edges at @p p (and so at its child
     *        @p c), @p held holds on link `s1` and @p owed charges to @p session, and nothing
     *        on any other link or session?
     */
    [[nodiscard]] bool reads(const graph_t& g, vertex_handle_t p, vertex_handle_t c,
                             std::uint32_t subs, int held, std::string_view session = "s1",
                             int owed = 0) const {
        bool ok = !negative && g.own_subs(p) == subs && g.has_subscribers(c) == (subs != 0);
        for (const auto& [link, n] : holds) ok &= n == (link == test_link("s1").conn ? held : 0);
        for (const auto& [name, n] : charged) ok &= n == (name == session ? owed : 0);
        return ok;
    }

    /** @brief Nothing charged anywhere. */
    [[nodiscard]] bool clear(const graph_t& g, vertex_handle_t p, vertex_handle_t c) const {
        return reads(g, p, c, 0, 0);
    }
};

/** @brief Does the vertex behind @p h have an edge block — was it ever subscribed to? */
bool has_block(vertex_handle_t h) { return std::bit_cast<tr::graph::vertex_t*>(h)->has_edges(); }

/**
 * @brief Vector 1, the table-source steps: refuse every draw of a wire subscribe in turn.
 *
 * Draw k is refused by serving k - 1 and arming. The admission's own draws are the edge block,
 * the index entry, and the edge array; the ADMIT and the index entry tell the three apart, and
 * the block is the refused draw after which the next budget finds one.
 */
void test_refused_at_every_table_draw() {
    std::printf("a wire subscribe refused at each table draw leaves nothing charged:\n");
    bool saw_block = false;
    bool saw_index = false;
    bool saw_slot = false;
    bool saw_success = false;
    bool all_clear = true;
    bool prev_blockless_refusal = false;
    for (std::size_t allow = 0; allow < 64 && !saw_success; ++allow) {
        gate_source_t gate;
        graph_t g{gate};
        ledger_t ledger;
        ledger.install(g);
        const vertex_handle_t p = g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
        const vertex_handle_t c = g.register_vertex(path_t("/p/c"), role_t::STORED_VALUE);
        gate.arm(allow);
        const auto r = wire_sub(g, p, "s1", "alice");
        gate.disarm();
        if (r.has_value()) {
            saw_success = true;
            saw_block |= prev_blockless_refusal;
            check(ledger.reads(g, p, c, 1, 1, "s1", 1),
                  "the budget that admits: one count, one hold, one charge");
            check(tr::testing::evict_test_link(g, "s1") == 1 && ledger.clear(g, p, c),
                  "and its departure leaves nothing");
            continue;
        }
        const bool admitted = ledger.admits > 0;
        const bool indexed = g.link_edge_candidates("s1") > 0;
        saw_block |= prev_blockless_refusal && has_block(p);
        prev_blockless_refusal = !has_block(p);
        saw_index |= admitted && !indexed;
        saw_slot |= admitted && indexed;
        all_clear &= r.error() == status_t::BACKPRESSURE && ledger.clear(g, p, c);
    }
    check(saw_success, "the sweep ends on the budget that admits");
    check(saw_block, "it refused the edge block");
    check(saw_index, "it refused the index entry after the ADMIT was granted");
    check(saw_slot, "it refused the edge array after the index entry landed");
    check(all_clear, "every refusal answered BACKPRESSURE and left nothing charged");
}

/** @brief Vector 1, the ADMIT: a refused session is charged nothing and grows no index. */
void test_refused_admit() {
    std::printf("a refused ADMIT leaves nothing charged and no index entry:\n");
    graph_t g;
    ledger_t ledger;
    ledger.install(g);
    const vertex_handle_t p = g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    const vertex_handle_t c = g.register_vertex(path_t("/p/c"), role_t::STORED_VALUE);
    ledger.refuse = true;
    const auto r = wire_sub(g, p, "s1", "alice");
    check(!r && r.error() == status_t::BACKPRESSURE, "the subscribe is refused BACKPRESSURE");
    check(ledger.clear(g, p, c), "nothing is charged");
    check(g.link_edge_candidates("s1") == 0, "and the index holds no entry for the session");
}

/**
 * @brief Vector 1, the slot verb's refusals, and vector 2.
 *
 * A slot the index names does not exist: only the owner passes the `[N]` gate for such a slot,
 * and the owner's edge is local, so this one is a count alone. Its write carries a link, which
 * is what vector 2 reads: no ADMIT and no index entry. The block refusal of a replace is the
 * same write at a vertex with no edges. The edge-changed refusal is a race: inside the
 * replace's ADMIT, after the holder check, the owner clears the slot and another peer's append
 * reuses it.
 */
void test_refused_slot_verb() {
    std::printf("a replace the slot verb refuses leaves nothing charged:\n");
    graph_t g;
    ledger_t ledger;
    ledger.install(g);
    const vertex_handle_t p = g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    const vertex_handle_t c = g.register_vertex(path_t("/p/c"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/t"), role_t::STORED_VALUE);
    const std::vector<std::byte> sub = b_subscriber("t");

    const auto blockless = slot_write(g, p, 0, sub, {}, "s1");
    check(!blockless && blockless.error() == status_t::INVALID_PATH,
          "a replace at a vertex with no edges is refused at the block");
    check(ledger.clear(g, p, c), "...and nothing is charged");

    check(slot_write(g, p, std::nullopt, sub, "peer", {}).has_value(),
          "a peer's field-write edge lands in slot 0");
    check(ledger.reads(g, p, c, 1, 0, "peer", 1), "it holds a count and its session's charge");

    const auto missing = slot_write(g, p, 5, sub, {}, "s1");
    check(!missing && missing.error() == status_t::INVALID_PATH,
          "the owner's replace of a slot that does not exist is refused");
    check(ledger.reads(g, p, c, 1, 0, "peer", 1), "...and gives its count back");
    check(ledger.charged.count("s1") == 0 && g.link_edge_candidates("s1") == 0,
          "a local edge whose write carried a link is never offered nor indexed");

    ledger.race_at = ledger.admits + 1;
    ledger.race = [&] {
        (void)slot_write(g, p, 0, b_clear(), {}, {});
        (void)slot_write(g, p, std::nullopt, sub, "other", {});
    };
    const auto changed = slot_write(g, p, 0, sub, "peer", {});
    ledger.race = nullptr;
    check(!changed && changed.error() == status_t::PERMISSION_DENIED,
          "a replace whose slot was reused after the holder check is refused");
    check(ledger.reads(g, p, c, 1, 0, "other", 1),
          "...and only the edge that reused the slot is charged");
    check(slot_write(g, p, 0, b_clear(), {}, {}).has_value() && ledger.clear(g, p, c),
          "clearing that edge leaves nothing");
}

/**
 * @brief Vector 3: a holder's replace of its own routed edge, delivering or suspended.
 *
 * The replacement is a field-write edge carried over the same session: it delivers locally, so
 * it holds no link, and it is charged to that session. The displaced edge's whole record goes
 * back, so the ledger reads the new edge alone.
 */
void test_replace_is_net_zero(bool suspended) {
    std::printf("a replace displacing a %s edge is net zero on every charge:\n",
                suspended ? "suspended" : "delivering");
    graph_t g;
    ledger_t ledger;
    ledger.install(g);
    const vertex_handle_t p = g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    const vertex_handle_t c = g.register_vertex(path_t("/p/c"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/t"), role_t::STORED_VALUE);
    check(wire_sub(g, p, "s1", "alice").has_value() && ledger.reads(g, p, c, 1, 1, "s1", 1),
          "a routed edge: one count, one hold, one charge");
    if (suspended) {
        check(g.set_suspended(g.subscription_at(p, 0), true).has_value() &&
                  ledger.reads(g, p, c, 0, 1, "s1", 1),
              "suspended, it keeps its hold and its charge and is no longer counted");
    }
    check(slot_write(g, p, 0, b_subscriber("t"), "alice", "s1").has_value(),
          "its holder replaces it over the same session");
    check(ledger.reads(g, p, c, 1, 0, "s1", 1), "the ledger reads the replacement's record alone");
    check(slot_write(g, p, 0, b_clear(), "alice", "s1").has_value() && ledger.clear(g, p, c),
          "clearing the replacement leaves nothing");
}

}  // namespace

int main() {
    if constexpr (!tr::graph::kSessionAdmission) {
        std::printf("  (skipped: this build binds kSessionAdmission = false)\n");
        return tr::testing::summary("admission_record");
    } else {
        test_refused_at_every_table_draw();
        test_refused_admit();
        test_refused_slot_verb();
        test_replace_is_net_zero(false);
        test_replace_is_net_zero(true);
        return tr::testing::summary("admission_record");
    }
}
