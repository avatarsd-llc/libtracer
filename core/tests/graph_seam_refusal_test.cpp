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

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
 * @brief A policy-less registration at @p p (the write-create door, which applies no policy)
 *        inherits nothing a refused one left on the placeholder: no app field (`gain` is
 *        undeclared), `LAST` retention and the default share threshold. It retires the vertex
 *        again, so the sweep's retry finds a placeholder.
 */
bool inherits_nothing(graph_t& g, std::string_view p) {
    if (found(g, p)) return false;
    const std::vector<std::byte> key = key_of(p);
    const auto h = g.ensure_vertex(key);  // the write-create door: no policy of its own
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

/** @brief Registration that enrolls the vertex in the UNCONDITIONAL sweep set. */
void test_register_unconditional() {
    std::printf("register_vertex — UNCONDITIONAL enrollment:\n");
    const auto op = [](graph_t& g, none_t&) {
        vertex_policy_t p;
        p.delivery_mode = delivery_mode_t::UNCONDITIONAL;
        return verdict(
            g.try_register_vertex(*path_t::parse("/u/v"), role_t::STORED_VALUE, {}, std::move(p)));
    };
    const auto untouched = [](graph_t& g, none_t&) { return !found(g, "/u/v"); };
    report(drive<none_t>([](graph_t&) { return none_t{}; }, op, untouched),
           "register UNCONDITIONAL");
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

/** @brief A policy change on a live vertex: extension, field table, UNCONDITIONAL entry. */
void test_set_policy() {
    std::printf("set_policy — extension block, field table, sweep-set entry:\n");
    const auto setup = [](graph_t& g) {
        return g.register_vertex(*path_t::parse("/p"), role_t::STORED_VALUE);
    };
    const auto op = [](graph_t& g, vertex_handle_t& v) {
        vertex_policy_t p;
        p.app_fields = {app_field_t{.name = "k", .access = app_access_t::RW}};
        p.delivery_mode = delivery_mode_t::UNCONDITIONAL;
        return verdict(g.set_policy(v, std::move(p)));
    };
    // A refused policy may have applied the members before the refusal (documented); the
    // retry below is what pins that it then completes.
    const auto untouched = [](graph_t& g, vertex_handle_t&) { return found(g, "/p"); };
    report(drive<vertex_handle_t>(setup, op, untouched), "set_policy");
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
    test_anchor();
    test_subscribe();
    test_retire();
    test_set_policy();
    test_app_field_store();
    test_identity();
    test_for_each_vertex();
    test_sweep();
#if defined(__unix__)
    test_init_exhaustion_message();
#endif
    return tr::testing::summary("graph_seam_refusal");
}
