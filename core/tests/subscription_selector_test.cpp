/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief `subscription_selector_t` (#2024), through its public interface only: the host API, the
 *        `:settings.app.active` / `:settings.app.options` fields and the creator door.
 *
 *   1. A switch from A to B suspends what only A lists, resumes what only B lists, leaves what
 *      both list delivering, and never delivers twice to a target A and B reach by different
 *      subscriptions. Selecting none silences every ref.
 *   2. Selecting is an ordinary field write: `NAME <option>` selects, an empty `STATUS` selects
 *      none, an unknown name is NOT_FOUND, anything else TYPE_MISMATCH; `options` has no write
 *      surface. A read of either field answers the selector's state (RFC-0034 shapes).
 *   3. A refused switch under memory pressure answers BACKPRESSURE, leaves no target served
 *      twice and nothing double-delivered, and a retry completes it once the source has room.
 *   4. A dangling ref is listed INERT, a switch skips it, and nothing removes it on its own.
 *   5. Teardown: destroying the selector retires its vertex and leaves every subscription in the
 *      state it held; retiring a producer makes its refs inert.
 *   6. The creator door: a creation hook makes an instance from a write to a missing child.
 *   7. `add` refuses a bad name, a gone subscription and a full table by value; adding a listed
 *      ref again re-reads it.
 *   8. A target-form subscription is a ref through `subscription_at`.
 *   9. A subscribe that reuses an inert ref's slot comes back under the selector once the owner
 *      lists it again, and switches like any ref (the review's slot-reuse case).
 *  10. `remove` drops a ref from every option, leaves its subscription as it is, and frees its
 *      place.
 *  11. One operation at a time: a peer's switches and `options` reads that meet answer
 *      BACKPRESSURE and change nothing, a retry completes them, and an `active` read never
 *      refuses. The same again on the busy flag's guarded binding (no atomic RMW).
 *  12. A temporary option name does not compile; a literal and an lvalue do.
 */

#include "libtracer/subscription_selector.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "libtracer/frame.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::result_t;
using tr::graph::role_t;
using tr::graph::selector_ref_state_t;
using tr::graph::status_t;
using tr::graph::subscription_t;
using tr::graph::value_t;
using tr::graph::vertex_handle_t;
using tr::testing::check;
using tr::testing::make_value;
using tr::wire::tlv_node_t;
using tr::wire::type_t;

/** @brief The selector under test: room for 8 refs and 4 options. */
using selector_t = tr::graph::subscription_selector_t<8, 4>;
/** @brief The same, with the busy flag on its guarded binding: a load and a store inside the
 *         build's guard, as a core with no atomic read-modify-write takes it. */
using guarded_selector_t = tr::graph::subscription_selector_t<8, 4, tr::graph::guard_t, false>;
static_assert(sizeof(guarded_selector_t) == sizeof(selector_t), "the binding costs no bytes");

// (12) A temporary name would dangle once add() returns, and a later `options` read would put
// freed bytes on the wire: it does not compile. A literal and an lvalue do.
template <class Sel>
concept takes_temporary_name = requires(Sel& s, subscription_t r) { s.add(std::string("a"), r); };
template <class Sel>
concept takes_temporary_const_name =
    requires(Sel& s, subscription_t r, const std::string&& n) { s.add(std::move(n), r); };
template <class Sel>
concept takes_temporary_pmr_name =
    requires(Sel& s, subscription_t r) { s.add(std::pmr::string("a"), r); };
template <class Sel>
concept takes_lasting_name = requires(Sel& s, subscription_t r, std::string& n) {
    s.add("a", r);
    s.add(n, r);
    s.add(std::string_view("a"), r);
};
static_assert(!takes_temporary_name<selector_t> && !takes_temporary_const_name<selector_t> &&
              !takes_temporary_pmr_name<selector_t> && takes_lasting_name<selector_t>);

/** @brief A one-byte write payload. */
tr::view::view_t byte_value(std::uint8_t b) {
    const std::byte one[1] = {std::byte{b}};
    return make_value(one);
}

/** @brief A counting sink: deliveries seen. */
struct counter_t {
    int seen = 0; /**< @brief Deliveries received. */
};

/** @brief The callback for @ref counter_t. */
void count(void* ctx, const value_t& /*v*/) { ++static_cast<counter_t*>(ctx)->seen; }

/** @brief A source that serves @ref arm's budget of blocks and then refuses, by value. */
class gate_source_t final : public tr::mem::block_source_t {
   public:
    gate_source_t() noexcept : tr::mem::block_source_t("gate") {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (armed_) return nullptr;
        return ::operator new(bytes, std::align_val_t{align}, std::nothrow);
    }
    void release(void* p, std::size_t, std::size_t align) noexcept override {
        ::operator delete(p, std::align_val_t{align});
    }
    /** @brief Refuse every draw from now on. */
    void arm() noexcept { armed_ = true; }
    /** @brief Serve everything again. */
    void disarm() noexcept { armed_ = false; }

   private:
    bool armed_ = false; /**< @brief Refusing? */
};

/** @brief A TLV of @p type over @p body, as a written value. */
tr::view::view_t tlv_value(type_t type, std::string_view body) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(
        out, type, {},
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(body.data()), body.size()));
    return make_value(out);
}

/** @brief Write `active` on @p sel's vertex as @p caller (empty = the owner). */
result_t<void> write_active(graph_t& g, vertex_handle_t sel, const tr::view::view_t& v,
                            std::string_view caller = {}) {
    return g.write(sel, path_t("/x:settings.app.active").field(), v, caller);
}

/** @brief Read field @p name of @p sel's vertex, as bytes; empty on an error. */
std::vector<std::byte> read_field(graph_t& g, vertex_handle_t sel, std::string_view name) {
    const auto r = g.read(sel, path_t("/x:settings.app." + std::string(name)).field());
    if (!r) return {};
    const auto b = (*r)->only().bytes();
    return {b.begin(), b.end()};
}

/** @brief The text of an opaque TLV node's body. */
std::string_view text_of(const tlv_node_t& n) {
    return {reinterpret_cast<const char*>(n.body().data()), n.body().size()};
}

/** @brief One ref as `options` lists it. */
struct listed_ref_t {
    std::string option;  /**< @brief The option it is listed under. */
    std::uint32_t slot;  /**< @brief Its `:subscribers[N]` slot. */
    std::string state;   /**< @brief live / suspended / inert. */
    std::size_t key_len; /**< @brief Its producer PATH key's length. */
};

/** @brief Parse an `options` read (RFC-0034) into a flat list; empty on a malformed value. */
std::vector<listed_ref_t> parse_options(std::span<const std::byte> bytes) {
    std::vector<listed_ref_t> out;
    const auto root = tlv_node_t::over(bytes);
    if (!root || root->type() != type_t::SETTINGS) return {};
    std::string option;
    for (const tlv_node_t& o : root->children()) {
        if (o.type() == type_t::NAME) {
            option = std::string(text_of(o));
            continue;
        }
        for (const tlv_node_t& ref : o.children()) {
            if (ref.type() != type_t::SETTINGS) continue;
            listed_ref_t row{option, 0, {}, 0};
            std::string_view key;
            for (const tlv_node_t& m : ref.children()) {
                if (m.type() == type_t::NAME && key.empty()) {
                    key = text_of(m);
                    continue;
                }
                if (key == "producer") row.key_len = m.body().size();
                if (key == "slot")
                    for (std::size_t i = 0; i < m.body().size(); ++i)
                        row.slot |= std::to_integer<std::uint32_t>(m.body()[i]) << (8 * i);
                if (key == "state") row.state = std::string(text_of(m));
                key = {};
            }
            out.push_back(row);
        }
    }
    return out;
}

/** @brief (1) A switch suspends, resumes and leaves the shared ref alone; no double delivery. */
void test_switching() {
    std::printf("a switch moves only the refs the two options do not share:\n");
    graph_t g;
    const vertex_handle_t in = g.register_vertex(path_t("/in"), role_t::STORED_VALUE);
    counter_t t1, t2, both, t1_via_b;
    const auto s1 = g.subscribe(path_t("/in"), count, &t1);
    const auto s2 = g.subscribe(path_t("/in"), count, &t2);
    const auto sb = g.subscribe(path_t("/in"), count, &both);
    // B reaches target t1 by a second subscription of its own (t1_via_b counts it apart).
    const auto s1b = g.subscribe(path_t("/in"), count, &t1_via_b);
    check(s1 && s2 && sb && s1b, "four subscriptions on /in");
    for (const auto& s : {s1, s2, s1b}) (void)g.set_suspended(*s, true);

    selector_t sel(g);
    check(sel.attach(path_t("/sel")).has_value(), "the selector registers its vertex");
    check(sel.add("a", *s1) && sel.add("a", *sb) && sel.add("b", *s2) && sel.add("b", *sb) &&
              sel.add("b", *s1b),
          "option a = {s1, shared}, option b = {s2, shared, s1 by another edge}");
    check(sel.select("a").has_value() && sel.active() == "a" && sel.settled(), "select a");
    (void)g.write(in, byte_value(1));
    check(t1.seen == 1 && both.seen == 1 && t2.seen == 0 && t1_via_b.seen == 0,
          "a delivers to s1 and the shared ref only");

    check(sel.select("b").has_value() && sel.active() == "b", "switch to b");
    (void)g.write(in, byte_value(2));
    check(t1.seen == 1 && t2.seen == 1 && both.seen == 2 && t1_via_b.seen == 1,
          "b delivers to s2, the shared ref and t1's other edge; s1 is silent");
    check(t1.seen + t1_via_b.seen == 2, "target t1 took one delivery per write, never two");
    check(sel.state(*sb).value_or(selector_ref_state_t::INERT) == selector_ref_state_t::LIVE,
          "the shared ref stayed live through the switch");

    check(sel.select("").has_value() && sel.active().empty(), "select none");
    (void)g.write(in, byte_value(3));
    check(t1.seen == 1 && t2.seen == 1 && both.seen == 2 && t1_via_b.seen == 1,
          "none silences every ref");
    check(g.own_subs(in) == 0, "and the producer counts no delivering edge");

    const auto r = sel.select("c");
    check(!r && r.error() == status_t::NOT_FOUND && sel.active().empty(),
          "an unknown option is NOT_FOUND and changes nothing");
}

/** @brief (2) The field surface: `active` writes and reads, `options` reads. */
void test_fields() {
    std::printf("selecting is a field write; both fields read back the selector's state:\n");
    graph_t g;
    const vertex_handle_t in = g.register_vertex(path_t("/in"), role_t::STORED_VALUE);
    counter_t a, b;
    const auto sa = g.subscribe(path_t("/in"), count, &a);
    const auto sb = g.subscribe(path_t("/in"), count, &b);
    (void)g.set_suspended(*sa, true);
    (void)g.set_suspended(*sb, true);
    selector_t sel(g);
    const auto vh = sel.attach(path_t("/sel"));
    check(vh && sel.add("a", *sa) && sel.add("b", *sb), "a selector with options a and b");

    check(write_active(g, *vh, tlv_value(type_t::NAME, "a")).has_value() && sel.active() == "a",
          "the owner's NAME write selects a");
    check(write_active(g, *vh, tlv_value(type_t::NAME, "b"), "peer").has_value() &&
              sel.active() == "b",
          "a caller-attributed NAME write selects b (`active` is rw)");
    (void)g.write(in, byte_value(1));
    check(a.seen == 0 && b.seen == 1, "and b is what delivers");

    std::vector<std::byte> name_b;
    tr::wire::emit_name(name_b, std::string_view("b"));
    check(read_field(g, *vh, "active") == name_b, "a read of active answers NAME \"b\"");

    const auto opts = parse_options(read_field(g, *vh, "options"));
    check(opts.size() == 2 && opts[0].option == "a" && opts[0].state == "suspended" &&
              opts[1].option == "b" && opts[1].state == "live",
          "options lists each option's refs with their state");
    check(opts.size() == 2 && opts[0].slot == 0 && opts[1].slot == 1 &&
              opts[0].key_len == path_t("/in").key().size(),
          "... each ref by its producer's PATH and its :subscribers[N] slot");

    const auto unknown = write_active(g, *vh, tlv_value(type_t::NAME, "zz"));
    check(!unknown && unknown.error() == status_t::NOT_FOUND && sel.active() == "b",
          "an unknown option name is NOT_FOUND, and b stays active");
    const auto value = write_active(g, *vh, tlv_value(type_t::VALUE, "b"));
    check(!value && value.error() == status_t::TYPE_MISMATCH, "a VALUE is TYPE_MISMATCH");
    const auto empty = write_active(g, *vh, tlv_value(type_t::NAME, ""));
    check(!empty && empty.error() == status_t::TYPE_MISMATCH, "an empty NAME is TYPE_MISMATCH");
    const auto opt_write =
        g.write(*vh, path_t("/x:settings.app.options").field(), tlv_value(type_t::NAME, "a"));
    check(!opt_write && opt_write.error() == status_t::SCHEMA_NOT_FOUND,
          "options has no write surface, not even the owner's");

    check(write_active(g, *vh, tlv_value(type_t::STATUS, "")).has_value() && sel.active().empty(),
          "an empty STATUS selects none");
    std::vector<std::byte> ok_status;
    tr::wire::emit_tlv(ok_status, type_t::STATUS, {}, {});
    check(read_field(g, *vh, "active") == ok_status, "and active then reads as an empty STATUS");
}

/** @brief (3) A refused switch under memory pressure, and its retry. */
void test_refused_switch() {
    std::printf("a refused switch never double-delivers, and a retry completes it:\n");
    gate_source_t gate;
    graph_t g{gate};
    const vertex_handle_t in = g.register_vertex(path_t("/in"), role_t::STORED_VALUE);
    counter_t a, b, other;
    const auto sa = g.subscribe(path_t("/in"), count, &a);
    const auto sb = g.subscribe(path_t("/in"), count, &b);
    const auto so = g.subscribe(path_t("/in"), count, &other);
    (void)g.set_suspended(*sb, true);
    selector_t sel(g);
    check(sel.add("a", *sa) && sel.add("b", *sb) && sel.select("a"), "a selector on a");

    gate.arm();
    check(sel.select("a").has_value(), "re-selecting the active option draws nothing");
    // An unrelated unsubscribe whose republish is refused leaves /in's array stale, so the next
    // resume on /in has to rebuild it: the one draw a toggle can make.
    check(g.unsubscribe(*so).has_value(), "an unsubscribe with no room still takes effect");
    const auto r = sel.select("b");
    check(!r && r.error() == status_t::BACKPRESSURE, "the switch to b answers BACKPRESSURE");
    check(sel.active() == "b" && !sel.settled(), "b is active and the switch is not settled");
    gate.disarm();
    (void)g.write(in, byte_value(1));
    check(a.seen == 0 && b.seen == 0 && other.seen == 0,
          "in between, nothing delivers: a gap, never a double delivery");
    check(sel.state(*sb).value_or(selector_ref_state_t::INERT) == selector_ref_state_t::SUSPENDED,
          "the refused ref is still suspended");

    check(sel.select("b").has_value() && sel.settled(), "the retry, with room, completes");
    (void)g.write(in, byte_value(2));
    check(a.seen == 0 && b.seen == 1, "and b delivers");

    gate.arm();
    check(sel.select("a").has_value() && sel.select("b").has_value(),
          "with the array current again, a switch draws nothing");
    gate.disarm();
}

/** @brief (4) A dangling ref is listed INERT, skipped, and kept. */
void test_dangling_ref() {
    std::printf("a dangling ref is listed inert and never blocks a switch:\n");
    graph_t g;
    const vertex_handle_t in = g.register_vertex(path_t("/in"), role_t::STORED_VALUE);
    counter_t a, b;
    const auto sa = g.subscribe(path_t("/in"), count, &a);
    const auto sb = g.subscribe(path_t("/in"), count, &b);
    (void)g.set_suspended(*sb, true);
    selector_t sel(g);
    const auto vh = sel.attach(path_t("/sel"));
    check(vh && sel.add("a", *sa) && sel.add("b", *sb) && sel.add("b", *sa), "two options");
    check(sel.select("a").has_value(), "select a");

    check(g.unsubscribe(*sb).has_value(), "b's own subscription is removed behind its back");
    check(sel.select("b").has_value() && sel.settled(), "the switch to b still succeeds");
    check(sel.state(*sb).value_or(selector_ref_state_t::LIVE) == selector_ref_state_t::INERT,
          "the gone ref reads INERT");
    (void)g.write(in, byte_value(1));
    check(a.seen == 1 && b.seen == 0, "the live ref b shares with a still delivers");

    const auto opts = parse_options(read_field(g, *vh, "options"));
    check(opts.size() == 3 && opts[2].option == "b" && opts[2].state == "inert" &&
              opts[2].slot == 1 && opts[1].state == "live",
          "options still lists it, inert, at its slot");
    check(sel.select("a").has_value() && sel.select("b").has_value(),
          "switching back and forth keeps skipping it");
}

/** @brief (5) Teardown: the selector's own, and its producer's. */
void test_teardown() {
    std::printf("teardown leaves the subscriptions alone; a retired producer's refs go inert:\n");
    graph_t g;
    const vertex_handle_t in = g.register_vertex(path_t("/in"), role_t::STORED_VALUE);
    counter_t a;
    const auto sa = g.subscribe(path_t("/in"), count, &a);
    {
        selector_t sel(g);
        check(sel.attach(path_t("/sel")) && sel.add("a", *sa) && sel.select("a"),
              "a selector on a");
        check(g.find(path_t("/sel").key()).has_value(), "its vertex is registered");
    }
    check(!g.find(path_t("/sel").key()), "destroying the selector retired its vertex");
    check(g.is_suspended(*sa).has_value() && !*g.is_suspended(*sa),
          "and left the subscription as it was: live");
    (void)g.write(in, byte_value(1));
    check(a.seen == 1, "it still delivers");

    selector_t again(g);
    check(again.attach(path_t("/sel")).has_value(), "the address can host a new selector");
    check(again.add("a", *sa) && again.select("").has_value(), "which can silence the same ref");
    check(g.retire(in).has_value(), "retire the producer");
    check(again.state(*sa).value_or(selector_ref_state_t::LIVE) == selector_ref_state_t::INERT,
          "its ref goes inert");
    check(again.select("a").has_value() && again.settled(), "and a switch skips it");
}

/** @brief The embedder's creation hook: each missing child of /sel becomes a selector. */
struct factory_t {
    graph_t* g = nullptr;                          /**< @brief The graph. */
    std::vector<std::unique_ptr<selector_t>> made; /**< @brief The embedder-owned instances. */

    /** @brief The hook: make one selector at the child key. */
    static result_t<void> create(void* ctx, vertex_handle_t /*parent*/,
                                 std::span<const std::byte> child_key, std::string_view /*subject*/,
                                 const tr::view::rope_t& /*payload*/) {
        auto& f = *static_cast<factory_t*>(ctx);
        auto sel = std::make_unique<selector_t>(*f.g);
        if (const auto r = sel->attach(child_key); !r) return std::unexpected(r.error());
        f.made.push_back(std::move(sel));
        return {};
    }
};

/** @brief (6) An instance created through the creator door. */
void test_creator_door() {
    std::printf("an instance is created through the creator door:\n");
    graph_t g;
    factory_t f{&g, {}};
    const vertex_handle_t parent = g.register_vertex(path_t("/sel"), role_t::STORED_VALUE);
    const auto installed = g.set_creation_hook(parent, {&factory_t::create, &f});
    if (!tr::graph::kCreationHooks) {
        check(!installed && installed.error() == status_t::SCHEMA_NOT_FOUND,
              "(this build has no creation hooks; the install is refused)");
        return;
    }
    check(installed.has_value(), "the embedder installs its hook on /sel");
    check(g.write(path_t("/sel/one"), byte_value(0)).has_value() && f.made.size() == 1,
          "a write to the missing /sel/one creates a selector there");
    const auto vh = g.find(path_t("/sel/one").key());
    check(vh && f.made[0]->vertex() == vh, "the new vertex is the selector's");
    const auto opts = read_field(g, *vh, "options");
    check(parse_options(opts).empty() && !opts.empty(), "it starts with no options");
}

/** @brief (7) `add` refuses by value. */
void test_add_refusals() {
    std::printf("add refuses a bad name, a gone subscription and a full table:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/in"), role_t::STORED_VALUE);
    counter_t c;
    std::vector<subscription_t> subs;
    for (int i = 0; i < 9; ++i) subs.push_back(*g.subscribe(path_t("/in"), count, &c));
    selector_t sel(g);
    const auto bad = sel.add("a/b", subs[0]);
    check(!bad && bad.error() == status_t::INVALID_PATH, "a name with a reserved character");
    const auto none = sel.add("a", subscription_t{});
    check(!none && none.error() == status_t::NOT_FOUND, "a handle naming no subscription");
    check(sel.add("a", subs[0]).has_value(), "a first ref");
    check(sel.add("a", subs[0]).has_value(), "the same ref again is a re-read, not a refusal");
    for (std::size_t i = 1; i < 8; ++i) (void)sel.add("a", subs[i]);
    const auto full = sel.add("a", subs[8]);
    check(!full && full.error() == status_t::BACKPRESSURE, "a ninth distinct ref");
    for (const char* o : {"b", "c", "d"}) (void)sel.add(o, subs[0]);
    const auto opts = sel.add("e", subs[0]);
    check(!opts && opts.error() == status_t::BACKPRESSURE, "a fifth option");
    check(sel.select("").has_value(), "a full selector still selects none");
}

/** @brief (8) A target-form subscription is reached by its slot, and a selector switches it. */
void test_target_form() {
    std::printf("a target-path subscription is a ref through subscription_at:\n");
    graph_t g;
    const vertex_handle_t in = g.register_vertex(path_t("/in"), role_t::STORED_VALUE);
    const vertex_handle_t ta = g.register_vertex(path_t("/out/a"), role_t::STORED_VALUE);
    const vertex_handle_t tb = g.register_vertex(path_t("/out/b"), role_t::STORED_VALUE);
    check(g.subscribe(path_t("/in"), path_t("/out/a")) &&
              g.subscribe(path_t("/in"), path_t("/out/b")),
          "two target-form subscriptions, in slots 0 and 1");
    const subscription_t sa = g.subscription_at(in, 0);
    const subscription_t sb = g.subscription_at(in, 1);
    check(!(sa == sb) && g.is_suspended(sa) && g.is_suspended(sb),
          "subscription_at names each slot, and each names a live edge");
    const auto none = g.is_suspended(g.subscription_at(in, 2));
    check(!none && none.error() == status_t::NOT_FOUND, "an empty slot's handle is NOT_FOUND");
    const auto at = g.subscription_address(sb);
    check(at && at->producer == in && at->slot == 1, "subscription_address reads it back");
    selector_t sel(g);
    check(sel.add("a", sa) && sel.add("b", sb) && sel.select("b"), "select b");
    (void)g.write(in, byte_value(7));
    check(!g.read(ta) && g.read(tb).has_value(), "only /out/b received the write");
    check(g.unsubscribe(sa).has_value(), "the handle unsubscribes the target-form edge");
    const auto gone = g.is_suspended(g.subscription_at(in, 0));
    check(!gone && gone.error() == status_t::NOT_FOUND, "after which its slot is not found");
}

/** @brief (9) A slot reused by a later subscribe comes back once the owner lists it again. */
void test_slot_reuse() {
    std::printf("a resubscribe that reuses an inert ref's slot is listed again and switches:\n");
    graph_t g;
    const vertex_handle_t in = g.register_vertex(path_t("/in"), role_t::STORED_VALUE);
    counter_t a, b, b_again;
    const auto sa = g.subscribe(path_t("/in"), count, &a);
    const auto sb = g.subscribe(path_t("/in"), count, &b);
    (void)g.set_suspended(*sb, true);
    selector_t sel(g);
    check(sel.add("a", *sa) && sel.add("b", *sb) && sel.select("a"), "options a and b, a active");
    check(g.unsubscribe(*sb).has_value() && sel.select("b") && sel.select("a"),
          "b's edge goes; a switch through b marks it inert");
    check(sel.state(*sb).value_or(selector_ref_state_t::LIVE) == selector_ref_state_t::INERT,
          "the ref reads INERT");

    // The peer comes back: its subscribe takes the first free slot, b's old one.
    const auto again = g.subscribe(path_t("/in"), count, &b_again);
    check(again && *again == *sb, "the resubscribe reuses the slot, so its handle equals the ref");
    (void)g.write(in, byte_value(1));
    check(a.seen == 1 && b_again.seen == 1,
          "admitted live, it delivers until the owner lists it (the selector owns only its bits)");

    check(sel.add("b", *again).has_value(), "the owner lists it again");
    check(sel.state(*again).value_or(selector_ref_state_t::INERT) == selector_ref_state_t::LIVE,
          "which re-reads it: no longer inert, and live");
    check(sel.select("a").has_value(), "re-selecting a brings it in line");
    (void)g.write(in, byte_value(2));
    check(a.seen == 2 && b_again.seen == 1, "a delivers and the reused ref is silent");
    check(sel.select("b").has_value() && sel.settled(), "and a switch to b");
    (void)g.write(in, byte_value(3));
    check(a.seen == 2 && b_again.seen == 2, "moves delivery to it");
}

/** @brief (10) `remove` drops a ref everywhere and frees its place. */
void test_remove() {
    std::printf("remove drops a ref from every option and leaves its subscription alone:\n");
    graph_t g;
    const vertex_handle_t in = g.register_vertex(path_t("/in"), role_t::STORED_VALUE);
    counter_t x, y, z;
    const auto sx = g.subscribe(path_t("/in"), count, &x);
    const auto sy = g.subscribe(path_t("/in"), count, &y);
    const auto sz = g.subscribe(path_t("/in"), count, &z);
    (void)g.set_suspended(*sy, true);
    (void)g.set_suspended(*sz, true);
    tr::graph::subscription_selector_t<3, 2> sel(g);
    const auto vh = sel.attach(path_t("/sel"));
    check(vh && sel.add("a", *sx) && sel.add("b", *sy) && sel.add("b", *sz) && sel.add("a", *sz),
          "a = {x, z}, b = {y, z}");
    check(sel.select("b").has_value(), "select b: y and z deliver");

    check(sel.remove(*sy).has_value(), "remove y");
    const auto twice = sel.remove(*sy);
    check(!twice && twice.error() == status_t::NOT_FOUND, "a second remove is NOT_FOUND");
    const auto st = sel.state(*sy);
    check(!st && st.error() == status_t::NOT_FOUND, "y is listed nowhere");
    check(g.is_suspended(*sy).has_value() && !*g.is_suspended(*sy),
          "and its subscription keeps the state it held: live");

    const auto opts = parse_options(read_field(g, *vh, "options"));
    check(opts.size() == 3 && opts[0].option == "a" && opts[0].slot == 0 && opts[1].slot == 2 &&
              opts[2].option == "b" && opts[2].slot == 2 && opts[2].state == "live",
          "options lists a = {x, z} and b = {z}: the refs after y moved up");
    check(sel.select("a").has_value(), "select a");
    (void)g.write(in, byte_value(1));
    check(x.seen == 1 && z.seen == 1 && y.seen == 1,
          "x and z deliver; y, no longer listed, is the owner's and still delivers");

    const auto extra = g.subscribe(path_t("/in"), count, &y);
    check(extra && sel.add("b", *extra).has_value(), "the freed place takes a new ref");
    const auto full = sel.add("b", *sy);
    check(!full && full.error() == status_t::BACKPRESSURE, "and the table is full again");
}

/** @brief (11) Operations that meet answer BACKPRESSURE and change nothing; a retry completes.
 *         @p Sel picks the busy flag's binding. */
template <class Sel>
void test_one_at_a_time(const char* binding) {
    std::printf("a peer's switches and reads that meet never wait and never half-run (%s):\n",
                binding);
    graph_t g;
    const vertex_handle_t in = g.register_vertex(path_t("/in"), role_t::STORED_VALUE);
    counter_t a, b;
    const auto sa = g.subscribe(path_t("/in"), count, &a);
    const auto sb = g.subscribe(path_t("/in"), count, &b);
    (void)g.set_suspended(*sb, true);
    Sel sel(g);
    const auto vh = sel.attach(path_t("/sel"));
    check(vh && sel.add("a", *sa) && sel.add("b", *sb) && sel.select("a"), "options a and b");

    std::atomic<bool> stop{false};
    std::atomic<int> odd{0};
    std::atomic<int> active_refused{0};
    std::thread reader([&] {
        while (!stop.load()) {
            const auto r = g.read(*vh, path_t("/x:settings.app.options").field());
            if (!r && r.error() != status_t::BACKPRESSURE) ++odd;
            if (!g.read(*vh, path_t("/x:settings.app.active").field())) ++active_refused;
        }
    });
    int refused = 0;
    for (int i = 0; i < 2000; ++i) {
        const auto r = write_active(g, *vh, tlv_value(type_t::NAME, (i & 1) ? "b" : "a"), "peer");
        if (!r) (r.error() == status_t::BACKPRESSURE ? ++refused : ++odd);
    }
    stop = true;
    reader.join();
    check(odd.load() == 0, "every answer was success or BACKPRESSURE");
    check(active_refused.load() == 0, "and an active read never refused: it takes no flag");
    std::printf("    (%d of 2000 switches met a read and were refused)\n", refused);

    // Capped, so a flag left set with no holder fails here instead of hanging the test.
    result_t<void> done = sel.select("b");
    for (int tries = 1; !done && tries < 1000; ++tries) done = sel.select("b");
    check(done.has_value(), "a retried switch completes within 1,000 tries (no stuck flag)");
    (void)g.write(in, byte_value(1));
    const int a_seen = a.seen;
    check(sel.settled() && b.seen >= 1 && g.is_suspended(*sa).value_or(false),
          "a retried switch to b completes: a is suspended, b delivers");
    (void)g.write(in, byte_value(2));
    check(a.seen == a_seen, "and a stays silent");
}

}  // namespace

int main() {
    test_switching();
    test_fields();
    test_refused_switch();
    test_dangling_ref();
    test_teardown();
    test_creator_door();
    test_add_refusals();
    test_target_form();
    test_slot_reuse();
    test_remove();
    test_one_at_a_time<selector_t>("one atomic exchange");
    test_one_at_a_time<guarded_selector_t>("guarded load and store");
    return tr::testing::summary("subscription_selector");
}
