/**
 * @file
 * @brief #1778 — every growth site of the graph core, driven to exhaustion of its table source.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The graph core's own state draws from the graph's table source (ADR-0083). Each operation
 * below is run against a source that serves `n` more blocks and then refuses, for `n = 0, 1,
 * 2, …` until the operation completes, on a fresh graph each time. At every refusal the
 * operation must answer by value (BACKPRESSURE, `false`, a counted drop), leave nothing
 * half-made, and succeed once the source has room again; and destroying the graph must give
 * every block back. A site that never refused (it completed at `n = 0`) would mean the
 * operation does not draw from the source at all, so each site also checks it was reached.
 *
 * The two abort paths — setup-time exhaustion in the constructor, `register_vertex` and
 * `register_child_type` (the ADR-0056 amendment) — run in a forked child, and the parent checks
 * the message names the call, the source and the bytes it was asked for.
 */

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "graph_sinks.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

#if defined(__unix__)
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#endif

namespace {

using tr::graph::app_access_t;
using tr::graph::app_field_t;
using tr::graph::delivery_mode_t;
using tr::graph::emission_mode_t;
using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::payload_right_t;
using tr::graph::result_t;
using tr::graph::retention_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::graph::vertex_policy_t;
using tr::testing::check;
using tr::testing::check_quiet;
using tr::testing::make_value;

/**
 * @brief A source that serves from the heap until armed, then serves `allow` more blocks and
 *        refuses every request after them. Counts live blocks so a leak is visible.
 */
class gate_source_t final : public tr::mem::block_source_t {
   public:
    gate_source_t() noexcept : tr::mem::block_source_t("gate") {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (armed_ && allow_ == 0) {
            ++refused_;
            if (bytes > largest_refused_) largest_refused_ = bytes;
            return nullptr;
        }
        if (armed_) --allow_;
        void* const p = ::operator new(bytes, std::align_val_t{align}, std::nothrow);
        if (p != nullptr) {
            ++live_;
            in_use_ += bytes;
        }
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        --live_;
        in_use_ -= bytes;
        ::operator delete(p, bytes, std::align_val_t{align});
    }
    [[nodiscard]] tr::mem::source_stats_t stats() const noexcept override {
        tr::mem::source_stats_t s;
        s.in_use = in_use_;
        s.refused = refused_;
        s.largest_refused = largest_refused_;
        return s;
    }

    /** @brief Serve @p allow more blocks, then refuse. */
    void arm(std::size_t allow) noexcept {
        armed_ = true;
        allow_ = allow;
        refused_ = 0;
    }
    /** @brief Serve everything again. */
    void disarm() noexcept { armed_ = false; }

    std::size_t refused_ = 0; /**< @brief Requests refused since the last @ref arm. */
    long live_ = 0;           /**< @brief Blocks handed out and not returned. */

   private:
    bool armed_ = false;
    std::size_t allow_ = 0;
    std::size_t in_use_ = 0;
    std::size_t largest_refused_ = 0;
};

/** @brief What one exhaustion sweep of a site found. */
struct sweep_t {
    std::size_t refusals = 0; /**< @brief Budgets at which the operation was refused. */
    bool clean = true;        /**< @brief Every refusal answered by value and left no trace. */
    bool retried = true;      /**< @brief Every refused operation succeeded with room. */
    bool no_leak = true;      /**< @brief Every graph gave back all it drew. */
    bool completed = false;   /**< @brief Some budget sufficed. */
};

/**
 * @brief Drive one operation to exhaustion: for each budget, build a graph on a fresh gate,
 *        prepare it with room (`setup`), arm, run `op`, and judge the outcome.
 *
 * `op(g, ctx)` answers 1 when it completed, 0 when it was refused by value, and -1 when it
 * answered something else (a failure). `untouched(g, ctx)` checks a refused op left nothing.
 */
template <class Ctx, class Setup, class Op, class Untouched>
sweep_t drive(Setup setup, Op op, Untouched untouched) {
    sweep_t s;
    for (std::size_t allow = 0; allow < 4096 && !s.completed; ++allow) {
        gate_source_t src;
        {
            graph_t g{src};
            Ctx ctx = setup(g);
            src.arm(allow);
            const int r = op(g, ctx);
            src.disarm();
            if (r > 0) {
                s.completed = true;
            } else {
                ++s.refusals;
                s.clean = s.clean && r == 0 && src.refused_ > 0 && untouched(g, ctx);
                s.retried = s.retried && op(g, ctx) > 0;
            }
        }
        s.no_leak = s.no_leak && src.live_ == 0;
    }
    return s;
}

/** @brief Report one site's sweep. */
void report(const sweep_t& s, std::string_view site) {
    std::printf("%.*s: refused at %zu budget(s)\n", static_cast<int>(site.size()), site.data(),
                s.refusals);
    check(s.completed && s.refusals > 0, "the site draws from the table source and completes");
    check(s.clean, "every refusal answers by value and leaves nothing half-made");
    check(s.retried, "every refused operation succeeds once the source has room");
    check(s.no_leak, "the graph gives back every block it drew");
}

/** @brief The status a result answered, or SUCCESS. */
template <class T>
int verdict(const result_t<T>& r) {
    if (r) return 1;
    return r.error() == status_t::BACKPRESSURE ? 0 : -1;
}

/** @brief An empty context for sites that need none. */
struct none_t {};

/** @brief Whether @p p resolves to a registered vertex. */
bool found(const graph_t& g, std::string_view p) {
    return g.find(path_t::parse(p)->key()).has_value();
}

/** @brief A canonical key for @p p, owned. */
std::vector<std::byte> key_of(std::string_view p) {
    const auto parsed = path_t::parse(p);
    const auto k = parsed->key();
    return {k.begin(), k.end()};
}

/**
 * @brief A policy-less registration at @p p (a plain `register_vertex_key`, which applies none)
 *        inherits nothing a refused one left on the placeholder: no app field (`gain` is
 *        undeclared), `LAST` retention and the default share threshold. It retires the vertex
 *        again, so the sweep's retry finds a placeholder.
 */
bool inherits_nothing(graph_t& g, std::string_view p) {
    if (found(g, p)) return false;
    const std::vector<std::byte> key = key_of(p);
    const auto h = g.register_vertex_key(key, role_t::STORED_VALUE);  // no policy of its own
    if (!h) return false;
    const std::string field = std::string(p) + ":settings.app.gain";
    const bool clean = !g.write(*path_t::parse(field), make_value({0x01})) &&
                       g.retention(*h) == retention_t::LAST &&
                       g.share_threshold_bytes(*h) == tr::graph::config_t::kShareThresholdBytes;
    return g.retire(*h).has_value() && clean;
}

/** @brief Registration: a deep STREAM with every per-vertex declaration that allocates. */
void test_register() {
    std::printf("register_vertex — descent, index, extension, policy, rights, admission:\n");
    const payload_right_t rows[] = {
        payload_right_t{.type = tr::wire::type_t::VALUE, .right = tr::graph::acl_right_t::WRITE}};
    const std::byte catalog[] = {std::byte{0x01}, std::byte{0x02}};
    const auto op = [&rows, &catalog](graph_t& g, none_t&) {
        handlers_t h;
        auto admit = [](const tr::graph::value_t&,
                        const tr::graph::write_ctx_t&) -> tr::graph::admission_t {
            return std::nullopt;
        };
        static auto keep = admit;
        h.on_admit = tr::graph::thunk(keep);
        vertex_policy_t p;
        p.retention = retention_t::N;
        p.depth = 4;
        p.app_fields = {app_field_t{.name = "gain", .access = app_access_t::RW}};
        return verdict(g.register_vertex_key(key_of("/a/b/c"), role_t::STREAM, h, std::move(p),
                                             rows, catalog));
    };
    const auto untouched = [](graph_t& g, none_t&) { return inherits_nothing(g, "/a/b/c"); };
    report(drive<none_t>([](graph_t&) { return none_t{}; }, op, untouched), "register");
}

/** @brief Registration whose policy drops retention and moves the threshold: a refusal after
 *         the policy landed leaves the placeholder with none of it (#1778 review). */
void test_register_policy_reset() {
    std::printf("register_vertex — a refused registration leaves no policy on the placeholder:\n");
    const payload_right_t rows[] = {
        payload_right_t{.type = tr::wire::type_t::VALUE, .right = tr::graph::acl_right_t::WRITE}};
    const auto op = [&rows](graph_t& g, none_t&) {
        vertex_policy_t p;
        p.retention = retention_t::NONE;
        p.share_threshold_bytes = 0;
        p.ring_reliable = true;
        p.app_fields = {app_field_t{.name = "gain", .access = app_access_t::RW}};
        return verdict(g.register_vertex_key(key_of("/n/v"), role_t::STORED_VALUE, {}, std::move(p),
                                             rows, {}));
    };
    const auto untouched = [](graph_t& g, none_t&) { return inherits_nothing(g, "/n/v"); };
    report(drive<none_t>([](graph_t&) { return none_t{}; }, op, untouched), "register policy");
}

/**
 * @brief Registration that lands the vertex in the UNCONDITIONAL sweep set (#1920): the mode
 *        and its set entry land inside the registration, so a refusal at the entry, or at the
 *        value seam drawn after it, registers nothing. A policy-less registration at the same
 *        address afterwards is IF_NEWER, and a covering sweep does not deliver it.
 */
void test_register_unconditional() {
    std::printf("register_vertex — UNCONDITIONAL mode and its sweep-set entry:\n");
    static auto on_write = [](const tr::graph::value_t&,
                              const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
        return {};
    };
    const auto op = [](graph_t& g, none_t&) {
        handlers_t h;
        h.on_write = tr::graph::thunk(on_write);  // a value seam, drawn after the set entry
        vertex_policy_t p;
        p.delivery_mode = delivery_mode_t::UNCONDITIONAL;
        return verdict(
            g.try_register_vertex(*path_t::parse("/u/v"), role_t::STORED_VALUE, h, std::move(p)));
    };
    const auto untouched = [](graph_t& g, none_t&) {
        if (found(g, "/u/v")) return false;
        const auto h = g.register_vertex_key(key_of("/u/v"), role_t::STORED_VALUE);  // no policy
        if (!h) return false;
        std::size_t delivered = 0;
        const auto count = [](void* ctx, const tr::graph::value_t&) {
            ++*static_cast<std::size_t*>(ctx);
        };
        const bool if_newer =
            std::bit_cast<tr::graph::vertex_t*>(*h)->delivery_mode() == delivery_mode_t::IF_NEWER;
        const auto sub = g.subscribe(*path_t::parse("/u"), +count, &delivered);
        const bool stored = g.write(*h, make_value({0x01})).has_value();
        delivered = 0;  // the write's own bubbled delivery
        const bool swept = g.propagate(*g.find(path_t::parse("/u")->key())).has_value();
        const bool clean = if_newer && sub && stored && swept && delivered == 0;
        if (sub) (void)g.unsubscribe(*sub);
        return g.retire(*h).has_value() && clean;
    };
    report(drive<none_t>(
               [](graph_t& g) {
                   (void)g.register_vertex(*path_t::parse("/u"), role_t::STORED_VALUE);
                   return none_t{};
               },
               op, untouched),
           "register UNCONDITIONAL");
}

/**
 * @brief The creation hook (RFC-0030 §7.2), both of its draws: installing one takes a
 *        declaration node from the table source, and a write the hook creates for registers
 *        the child from it. A refused install leaves no hook; a refused creation leaves no
 *        child (or, refused after the hook registered it, an empty one), and its BACKPRESSURE
 *        reaches the writer.
 */
void test_creation_hook() {
    std::printf("set_creation_hook, and a write the hook creates for:\n");
    if (!tr::graph::kCreationHooks) {
        std::printf("  (skipped: this build binds kCreationHooks = false)\n");
        return;
    }
    const auto parent = [](graph_t& g) {
        return g.register_vertex(*path_t::parse("/p"), role_t::STORED_VALUE);
    };
    const auto install = [](graph_t& g, vertex_handle_t& p) {
        return verdict(g.set_creation_hook(p, {&tr::testing::create_stored_value, &g}));
    };
    const auto no_hook = [](graph_t& g, vertex_handle_t&) {
        const auto w = g.write(*path_t::parse("/p/x"), make_value({0x01}));
        return !w && w.error() == status_t::NOT_FOUND && !found(g, "/p/x");
    };
    report(drive<vertex_handle_t>(parent, install, no_hook), "set_creation_hook");

    const auto hooked = [](graph_t& g) {
        const vertex_handle_t p = g.register_vertex(*path_t::parse("/p"), role_t::STORED_VALUE);
        (void)tr::testing::allow_creation(g, p);
        return p;
    };
    const auto create = [](graph_t& g, vertex_handle_t&) {
        return verdict(g.write(*path_t::parse("/p/x"), make_value({0x01})));
    };
    // A refusal before the hook registered leaves no child. One after it (the value's own
    // block) leaves the child the hook made, holding nothing: the registration is the app's
    // and stands, as any registration does when a later write to it is refused.
    const auto no_child = [](graph_t& g, vertex_handle_t&) {
        return !found(g, "/p/x") || !g.read(*path_t::parse("/p/x")).has_value();
    };
    report(drive<vertex_handle_t>(hooked, create, no_child), "hook-created write");
}

/** @brief Registration over a placeholder: a refusal leaves no extension block on it. */
void test_register_placeholder() {
    std::printf("register_vertex — a value seam over an existing placeholder:\n");
    static auto on_read = []() -> tr::graph::result_t<tr::graph::value_ref_t> {
        return std::unexpected(status_t::NOT_FOUND);
    };
    handlers_t h;
    h.on_read = tr::graph::thunk(on_read);
    bool clean = true;
    bool completed = false;
    for (std::size_t allow = 0; allow < 64 && !completed; ++allow) {
        gate_source_t src;
        graph_t g{src};
        (void)g.register_vertex(*path_t::parse("/p/v/c"),
                                role_t::STORED_VALUE);  // "/p/v" is a placeholder
        const long before = src.live_;
        src.arm(allow);
        const auto r = g.try_register_vertex(*path_t::parse("/p/v"), role_t::STORED_VALUE, h);
        src.disarm();
        completed = r.has_value();
        clean = clean && (completed || (r.error() == status_t::BACKPRESSURE &&
                                        src.live_ == before && !found(g, "/p/v")));
    }
    check(completed, "the registration completes once the source has room");
    check(clean, "a refused one leaves the placeholder as it was: no block drawn stays behind");
}

/** @brief A session anchor: its record, the slot and the vertex. */
void test_anchor() {
    std::printf("register_session_anchor:\n");
    const auto op = [](graph_t& g, none_t&) {
        return verdict(g.register_session_anchor(":mount/peer"));
    };
    const auto untouched = [](graph_t& g, none_t&) {
        return !g.find_session_anchor(":mount/peer");
    };
    report(drive<none_t>([](graph_t&) { return none_t{}; }, op, untouched), "anchor");
}

/** @brief The delivery sink the subscribe sites install. */
void sink(void*, const tr::graph::value_t&) {}

/** @brief A subscriber edge: the edge block, its slot table and the published array. */
void test_subscribe() {
    std::printf("subscribe — edge block, slot table, published edge array:\n");
    const auto setup = [](graph_t& g) {
        return g.register_vertex(*path_t::parse("/s"), role_t::STORED_VALUE);
    };
    const auto op = [](graph_t& g, vertex_handle_t&) {
        return verdict(g.subscribe(*path_t::parse("/s"), &sink, nullptr));
    };
    const auto untouched = [](graph_t& g, vertex_handle_t& v) { return g.own_subs(v) == 0; };
    report(drive<vertex_handle_t>(setup, op, untouched), "subscribe");
}

/** @brief Retirement: the cleanup key, the moved-out slot tables and the seam park. */
void test_retire() {
    std::printf("retire — cleanup key, slot-table list, seam park:\n");
    const auto setup = [](graph_t& g) {
        handlers_t h;
        auto on_write = [](const tr::graph::value_t&,
                           const tr::graph::write_ctx_t&) -> result_t<void> { return {}; };
        static auto keep = on_write;
        h.on_write = tr::graph::thunk(keep);
        const vertex_handle_t v = g.register_vertex(*path_t::parse("/r"), role_t::HANDLER, h);
        (void)g.register_vertex(*path_t::parse("/r/x"), role_t::STORED_VALUE);
        (void)g.subscribe(*path_t::parse("/r/x"), &sink, nullptr);
        return v;
    };
    const auto op = [](graph_t& g, vertex_handle_t& v) { return verdict(g.retire(v)); };
    const auto untouched = [](graph_t& g, vertex_handle_t&) {
        return found(g, "/r") && found(g, "/r/x");
    };
    report(drive<vertex_handle_t>(setup, op, untouched), "retire");
}

/** @brief Every policy member a vertex reports — what a refused `set_policy` must leave. */
struct policy_state_t {
    tr::mem::block_source_t* ring_source = nullptr;   /**< @brief The bound ring source. */
    bool ring_reliable = false;                       /**< @brief The §4.4 arm. */
    retention_t retention = retention_t::NONE;        /**< @brief What the vertex retains. */
    std::uint32_t depth = 0;                          /**< @brief The ring depth. */
    std::size_t threshold = 0;                        /**< @brief The share threshold. */
    std::size_t fields = 0;                           /**< @brief Installed app-field slots. */
    delivery_mode_t mode = delivery_mode_t::IF_NEWER; /**< @brief The delivery mode. */

    /** @brief Read every member off @p v. */
    static policy_state_t of(vertex_handle_t v) {
        const tr::graph::vertex_t* const x = std::bit_cast<tr::graph::vertex_t*>(v);
        return {x->ring_source(),     x->ring_reliable(),         x->retention(),
                x->retention_depth(), x->share_threshold_bytes(), x->app_field_slots().size(),
                x->delivery_mode()};
    }
    /** @brief Member-wise equality. */
    bool operator==(const policy_state_t&) const = default;
};

/** @brief A vertex and the policy state it had before the operation. */
struct policy_ctx_t {
    vertex_handle_t v;     /**< @brief The vertex the policy lands on. */
    policy_state_t before; /**< @brief Its members before the operation. */
};

/**
 * @brief A policy change on a live vertex is all-or-nothing (#1883): a refusal at any of its
 *        allocations (the extension block, the ring state, the owned field table and its
 *        group, the UNCONDITIONAL sweep-set entry) leaves every member as it was.
 */
void test_set_policy() {
    std::printf("set_policy — every member, all-or-nothing:\n");
    // A STORED_VALUE has no extension block yet, so the first member that moves draws one.
    const auto leaf = [](graph_t& g) {
        const vertex_handle_t v = g.register_vertex(*path_t::parse("/p"), role_t::STORED_VALUE);
        return policy_ctx_t{v, policy_state_t::of(v)};
    };
    const auto leaf_op = [](graph_t& g, policy_ctx_t& c) {
        vertex_policy_t p;
        p.share_threshold_bytes = 7;
        p.app_fields = {app_field_t{.name = "k", .access = app_access_t::RW}};
        p.delivery_mode = delivery_mode_t::UNCONDITIONAL;
        return verdict(g.set_policy(c.v, std::move(p)));
    };
    const auto untouched = [](graph_t&, policy_ctx_t& c) {
        return policy_state_t::of(c.v) == c.before;
    };
    report(drive<policy_ctx_t>(leaf, leaf_op, untouched), "set_policy on a leaf");
    // A STREAM has its extension block but no ring state yet; the policy moves every member.
    const auto stream = [](graph_t& g) {
        const vertex_handle_t v = g.register_vertex(*path_t::parse("/s"), role_t::STREAM);
        return policy_ctx_t{v, policy_state_t::of(v)};
    };
    const auto stream_op = [](graph_t& g, policy_ctx_t& c) {
        vertex_policy_t p;
        p.ring_source = &tr::mem::heap_source();
        p.ring_reliable = true;
        p.retention = retention_t::N;
        p.depth = 4;
        p.share_threshold_bytes = 7;
        p.app_fields = {
            app_field_t{.name = "k", .access = app_access_t::RW, .value = {std::byte{1}}}};
        p.delivery_mode = delivery_mode_t::UNCONDITIONAL;
        return verdict(g.set_policy(c.v, std::move(p)));
    };
    report(drive<policy_ctx_t>(stream, stream_op, untouched), "set_policy on a stream");
}

/** @brief An app-field write: the table's value slots and the stored bytes. */
void test_app_field_store() {
    std::printf("app field write — value slots, stored bytes:\n");
    const auto setup = [](graph_t& g) {
        vertex_policy_t p;
        p.app_fields = {app_field_t{.name = "k", .access = app_access_t::RW}};
        return g.register_vertex(*path_t::parse("/f"), role_t::STORED_VALUE, {}, std::move(p));
    };
    const auto op = [](graph_t& g, vertex_handle_t&) {
        return verdict(g.write(*path_t::parse("/f:settings.app.k"), make_value({0x01, 0x02})));
    };
    // Declared, never written: a refused write leaves it unset, which reads as an error.
    const auto untouched = [](graph_t& g, vertex_handle_t&) {
        return !g.read(*path_t::parse("/f:settings.app.k"));
    };
    report(drive<vertex_handle_t>(setup, op, untouched), "app field write");
}

/** @brief The node identity record. */
void test_identity() {
    std::printf("set_identity — the record copy:\n");
    const auto op = [](graph_t& g, none_t&) {
        const std::vector<std::byte> key(32, std::byte{0x5a});
        return verdict(g.set_identity(1, key));
    };
    const auto untouched = [](graph_t& g, none_t&) {
        return !g.read(*path_t::parse("/i:identity"));
    };
    const auto setup = [](graph_t& g) {
        (void)g.register_vertex(*path_t::parse("/i"), role_t::STORED_VALUE);
        return none_t{};
    };
    report(drive<none_t>(setup, op, untouched), "identity");
}

/** @brief The sorted vertex walk's snapshot. */
void test_for_each_vertex() {
    std::printf("for_each_vertex — the key snapshot:\n");
    const auto setup = [](graph_t& g) {
        (void)g.register_vertex(*path_t::parse("/w/a"), role_t::STORED_VALUE);
        (void)g.register_vertex(*path_t::parse("/w/b"), role_t::STORED_VALUE);
        return none_t{};
    };
    std::size_t visited = 0;
    const auto op = [&visited](graph_t& g, none_t&) {
        visited = 0;
        return g.for_each_vertex([&visited](tr::wire::key_view_t, vertex_handle_t) { ++visited; })
                   ? 1
                   : 0;
    };
    const auto untouched = [&visited](graph_t&, none_t&) { return visited == 0; };
    report(drive<none_t>(setup, op, untouched), "for_each_vertex");
}

/** @brief An assign's pending mark and the covering sweep's key snapshot. */
void test_sweep() {
    std::printf("assign + propagate — the pending mark and the sweep snapshot:\n");
    std::size_t delivered = 0;
    const auto count = [](void* ctx, const tr::graph::value_t&) {
        ++*static_cast<std::size_t*>(ctx);
    };
    // The mark: an observed IF_NEWER assign whose value fits but whose mark does not lands,
    // and sheds the mark as a counted drop. The injected root also serves the value, so the
    // budget is swept until the store fits and the mark is what runs dry.
    bool shed_counted = false;
    bool no_leak = true;
    for (std::size_t allow = 0; allow < 64 && !shed_counted; ++allow) {
        gate_source_t src;
        {
            graph_t g{src};
            const vertex_handle_t v =
                g.register_vertex(*path_t::parse("/q/x"), role_t::STORED_VALUE);
            (void)g.subscribe(*path_t::parse("/q/x"), +count, &delivered);
            const auto before = g.delivery_drops().out_of_memory;
            src.arm(allow);
            const bool assigned = g.assign(v, tr::view::rope_t{make_value({0x07})}).has_value();
            src.disarm();
            shed_counted = assigned && g.delivery_drops().out_of_memory > before;
        }
        no_leak = no_leak && src.live_ == 0;
    }
    check(shed_counted, "a refused pending mark is a counted OUT_OF_MEMORY drop, the assign lands");
    check(no_leak, "and the graph gives back every block it drew");
    // The sweep: its scratch is a stack frame first, so only a snapshot that outgrows the
    // frame reaches the source. Refused there, the sweep delivers what it collected and keeps
    // the rest of the marks, and the next sweep delivers them.
    {
        constexpr std::size_t kMarks = 64;  // 64 keys of ~26 B: past the 512 B frame
        gate_source_t src;
        {
            graph_t g{src};
            const vertex_handle_t q = g.register_vertex(*path_t::parse("/q"), role_t::STORED_VALUE);
            bool marked = true;
            for (std::size_t i = 0; i < kMarks; ++i) {
                char p[48];
                std::snprintf(p, sizeof p, "/q/a-long-child-name-%04zu", i);
                const vertex_handle_t x =
                    g.register_vertex(*path_t::parse(p), role_t::STORED_VALUE);
                (void)g.subscribe(*path_t::parse(p), +count, &delivered);
                marked = g.assign(x, tr::view::rope_t{make_value({0x07})}).has_value() && marked;
            }
            check(marked, "assign marks");
            delivered = 0;
            src.arm(0);
            (void)g.propagate(q);
            const bool deferred = delivered < kMarks;
            const bool folded_refused = !g.propagate(q, emission_mode_t::FOLD);
            src.disarm();
            check(deferred, "a sweep refused past its frame defers what it could not collect");
            check(folded_refused, "a refused FOLD sweep answers BACKPRESSURE");
            (void)g.propagate(q);
            check(delivered == kMarks, "every retained mark is delivered by the next sweep");
        }
        check(src.live_ == 0, "and the graph gives back every block it drew");
    }
}

/** @brief A SUBSCRIBER{PATH <target>, SETTINGS{delivery_compact 1}} record's bytes. */
std::vector<std::byte> subscriber_record(std::string_view target) {
    const std::vector<std::byte> key = key_of(target);
    std::vector<std::byte> settings;
    tr::wire::emit_name(settings, "delivery_compact");
    tr::wire::emit_value_le(settings, std::uint8_t{1}, 1);
    std::vector<std::byte> body;
    tr::wire::emit_tlv(body, tr::wire::type_t::PATH, tr::wire::opt_t{}, key);
    tr::wire::emit_tlv(body, tr::wire::type_t::SETTINGS, tr::wire::opt_t{.pl = true}, settings);
    std::vector<std::byte> rec;
    tr::wire::emit_tlv(rec, tr::wire::type_t::SUBSCRIBER, tr::wire::opt_t{.pl = true}, body);
    return rec;
}

/** @brief The local target sugar (#1885): the record's staging and the edge's target key. */
void test_subscribe_target() {
    std::printf("subscribe to a local target — record staging, target key:\n");
    const auto setup = [](graph_t& g) {
        (void)g.register_vertex(*path_t::parse("/t/dst"), role_t::STORED_VALUE);
        return g.register_vertex(*path_t::parse("/t"), role_t::STORED_VALUE);
    };
    const auto op = [](graph_t& g, vertex_handle_t&) {
        return verdict(g.subscribe(*path_t::parse("/t"), *path_t::parse("/t/dst")));
    };
    const auto untouched = [](graph_t& g, vertex_handle_t& v) { return g.own_subs(v) == 0; };
    report(drive<vertex_handle_t>(setup, op, untouched), "subscribe target");
}

/** @brief A wire subscriber (#1885): its target key, cold half and both names. */
void test_subscribe_wire() {
    std::printf("subscribe_wire — target key, cold half, link and caller names:\n");
    const auto setup = [](graph_t& g) {
        return g.register_vertex(*path_t::parse("/w"), role_t::STORED_VALUE);
    };
    const auto op = [](graph_t& g, vertex_handle_t& v) {
        const std::vector<std::byte> rec = subscriber_record("/consumer/in");
        const std::string link(64, 'l');  // names longer than any inline buffer
        const std::string caller(64, 'c');
        return verdict(g.subscribe_wire(v, make_value(rec), make_value({0x06, 0x00, 0x00, 0x00}),
                                        link, {}, caller));
    };
    const auto untouched = [](graph_t& g, vertex_handle_t& v) { return g.own_subs(v) == 0; };
    report(drive<vertex_handle_t>(setup, op, untouched), "subscribe_wire");
}

/** @brief The colon-field reads that stage a TLV (#1885): children, schema, settings. */
void test_field_reads() {
    std::printf("field reads — :children, :schema, :settings staging:\n");
    const auto setup = [](graph_t& g) {
        vertex_policy_t p;
        p.app_fields = {app_field_t{.name = "gain", .access = app_access_t::RW}};
        const vertex_handle_t v =
            g.register_vertex(*path_t::parse("/r"), role_t::STORED_VALUE, {}, std::move(p));
        (void)g.register_vertex(*path_t::parse("/r/a"), role_t::STORED_VALUE);
        (void)g.register_vertex(*path_t::parse("/r/b"), role_t::STORED_VALUE);
        (void)g.write(*path_t::parse("/r:settings.app.gain"), make_value({0x01}));
        return v;
    };
    const auto nothing = [](graph_t&, vertex_handle_t&) { return true; };
    for (const char* field : {"/r:children", "/r:schema", "/r:settings", "/r:settings.app"}) {
        const auto op = [field](graph_t& g, vertex_handle_t&) {
            return verdict(g.read(*path_t::parse(field)));
        };
        report(drive<vertex_handle_t>(setup, op, nothing), field);
    }
}

/**
 * @brief A wide fan-out's snapshot (#1885): the writing call's stack frame first, the table
 *        source past it, and a counted truncation when that is dry. No thread keeps a buffer.
 */
void test_fan_out() {
    std::printf("fan_out — the wide snapshot past its stack frame:\n");
    // Past the frame (8 x kInlineFanout views), so the snapshot reaches the table source.
    constexpr std::size_t kSubs = 9 * tr::graph::kInlineFanout + 1;
    std::size_t delivered = 0;
    const auto count = [](void* ctx, const tr::graph::value_t&) {
        ++*static_cast<std::size_t*>(ctx);
    };
    gate_source_t src;
    {
        graph_t g{src};
        const vertex_handle_t v = g.register_vertex(*path_t::parse("/fan"), role_t::STORED_VALUE);
        for (std::size_t i = 0; i < kSubs; ++i)
            (void)g.subscribe(*path_t::parse("/fan"), +count, &delivered);
        check(g.own_subs(v) == kSubs, "the wide fan-out is subscribed");
        // The value is stored before the snapshot, so the budget is swept until the store
        // fits and the snapshot is what runs dry.
        bool truncated = false;
        for (std::size_t allow = 0; allow < 16 && !truncated; ++allow) {
            delivered = 0;
            const auto before = g.delivery_drops().fan_out_truncated;
            src.arm(allow);
            const bool wrote = g.write(*path_t::parse("/fan"), make_value({0x01})).has_value();
            src.disarm();
            const auto shed = g.delivery_drops().fan_out_truncated - before;
            truncated = wrote && shed != 0;
            if (truncated) {
                check(delivered == tr::graph::kInlineFanout && delivered + shed == kSubs,
                      "a refused snapshot delivers the inline prefix and counts the rest");
            }
        }
        check(truncated, "a dry table source truncates the wide fan-out by value");
        delivered = 0;
        check(g.write(*path_t::parse("/fan"), make_value({0x02})).has_value() && delivered == kSubs,
              "with room again every subscriber is delivered");
        // Inside the frame, the snapshot draws nothing at all.
        delivered = 0;
        (void)g.register_vertex(*path_t::parse("/mid"), role_t::STORED_VALUE);
        constexpr std::size_t kMid = 4 * tr::graph::kInlineFanout;
        for (std::size_t i = 0; i < kMid; ++i)
            (void)g.subscribe(*path_t::parse("/mid"), +count, &delivered);
        const auto before = g.delivery_drops().fan_out_truncated;
        bool framed = false;
        for (std::size_t allow = 0; allow < 16 && !framed; ++allow) {
            delivered = 0;
            src.arm(allow);
            framed = g.write(*path_t::parse("/mid"), make_value({0x03})).has_value();
            src.disarm();
        }
        check(framed && delivered == kMid && g.delivery_drops().fan_out_truncated == before,
              "a fan-out inside the stack frame is never truncated: it draws nothing");
    }
    check(src.live_ == 0, "and the graph gives back every block it drew");
}

#if defined(__unix__)
/** @brief Run @p body in a forked child with stderr captured; answer what it printed and
 *         whether it died of SIGABRT. */
template <class Body>
bool aborts_with(Body body, std::string& err) {
    int fds[2];
    if (pipe(fds) != 0) return false;
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 2);
        close(fds[0]);
        body();
        _exit(0);  // not reached when the body aborts
    }
    close(fds[1]);
    char buf[512];
    for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;)
        err.append(buf, static_cast<std::size_t>(n));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

/** @brief The setup-time exhaustion message (ADR-0056 amendment, ADR-0083). */
void test_init_exhaustion_message() {
    std::printf("setup-time exhaustion aborts with the source and the bytes:\n");
    std::string err;
    const bool ctor = aborts_with(
        [] {
            gate_source_t src;
            src.arm(0);
            graph_t g{src};
        },
        err);
    check(ctor && err.find("graph_t: the \"gate\" memory source refused") != std::string::npos &&
              err.find("bytes needed") != std::string::npos,
          "a graph whose source cannot hold its roots aborts naming the source and the bytes");

    err.clear();
    const bool reg = aborts_with(
        [] {
            gate_source_t src;
            graph_t g{src};
            src.arm(0);
            (void)g.register_vertex(*path_t::parse("/x/y"), role_t::STORED_VALUE);
        },
        err);
    check(
        reg && err.find("register_vertex: the \"gate\" memory source refused") != std::string::npos,
        "register_vertex on a dry source aborts naming the call and the source");
    unsigned long bytes = 0;
    const std::size_t at = err.find('(');
    check(at != std::string::npos &&
              std::sscanf(err.c_str() + at, "(%lu bytes needed", &bytes) == 1 && bytes > 0,
          "and the message carries the size it was refused");

    err.clear();
    const bool ceiling = aborts_with(
        [] {
            graph_t g;
            g.set_vertex_ceiling(g.vertex_slot_count());
            (void)g.register_vertex(*path_t::parse("/c"), role_t::STORED_VALUE);
        },
        err);
    check(ceiling && err.find("register_vertex: the vertex ceiling") != std::string::npos &&
              err.find("memory source") == std::string::npos,
          "register_vertex past the vertex ceiling aborts naming the ceiling, not a source");

    err.clear();
    const bool cat = aborts_with(
        [] {
            gate_source_t src;
            graph_t g{src};
            src.arm(0);
            g.register_child_type("thermostat", {});
        },
        err);
    check(cat && err.find("register_child_type: the \"gate\"") != std::string::npos,
          "register_child_type on a dry source aborts naming the call");

    // The runtime door answers instead of aborting.
    gate_source_t src;
    {
        graph_t g{src};
        src.arm(0);
        const auto r = g.try_register_vertex(*path_t::parse("/x/y"), role_t::STORED_VALUE);
        src.disarm();
        check(!r && r.error() == status_t::BACKPRESSURE,
              "try_register_vertex on a dry source answers BACKPRESSURE");
    }
}
#endif

}  // namespace

int main() {
    test_register();
    test_register_policy_reset();
    test_register_unconditional();
    test_register_placeholder();
    test_creation_hook();
    test_anchor();
    test_subscribe();
    test_retire();
    test_set_policy();
    test_app_field_store();
    test_identity();
    test_for_each_vertex();
    test_sweep();
    test_fan_out();
    test_subscribe_target();
    test_subscribe_wire();
    test_field_reads();
#if defined(__unix__)
    test_init_exhaustion_message();
#endif
    return tr::testing::summary("graph_seam_refusal");
}
