/**
 * @file
 * @brief #1049 — `graph_t`'s five configuration seams under a setter storm: no reader is
 *        ever handed a destroyed target, a torn pair, or a half-mutated container.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The L4 analogue of `fwd_sink_race_test` (#914), and a strictly worse defect than the one
 * that test pins. The router's sinks were a torn `{fn, ctx}` PAIR; `graph_t`'s three
 * callback seams were `std::function`s, and assigning a `std::function` DESTROYS the old
 * target — freeing its captured state while a reader may be inside the call. That is a
 * use-after-free, not a torn read, and it reached three places at once: the remote-delivery
 * sink on the write hot path, the subject resolver the ACL gate consults on every gated read
 * and write, and the subscription observer. The router's `sink_slot_t` does not transfer to a
 * `std::function` (it statically requires a pointer-sized function pointer), so the fix was
 * to NARROW the three to the ADR-0047 `{fn, ctx}` shape and publish them through that slot.
 *
 * Two further members were declared under the same retired "configure before frames flow"
 * comment and are covered here too. Neither is a callback, so the slot cannot hold them:
 * the creatable-child-type catalog is a `std::map` a public verb inserts into while the
 * in-band creation path — driven by a PEER's bytes — walks it, and the node identity record
 * is a buffer that install and clear both free while `read_identity` tests its emptiness and
 * then memcpys it. Both are control-plane cold, so both took a lock.
 *
 * @par The detectors
 * For the three callback seams the detector is functional and needs no sanitizer: a
 * self-identifying context. Seam A is only ever installed with probe A, seam B only with
 * probe B, and the installed function checks the tag of the probe it was handed — so a torn
 * publish is a tag mismatch, and a call into a destroyed target is (at minimum) a tag
 * mismatch as well. For the catalog and the identity record the storm's own outcome is the
 * detector: every observed answer must be one of the finitely many LEGAL answers, which a
 * torn map walk or a freed buffer does not satisfy. Both are also live TSan/ASan vehicles,
 * and that is where their pre-fix failure is loudest — necessarily so for the catalog, whose
 * functional detector is PROBABILISTIC: on the parent commit it reddens on most runs (three
 * or fewer of twenty thousand creations miss an entry the tree is rebalancing under them),
 * not on every one. The three callback scenarios and the identity one redden deterministically
 * there, so the suite as a whole does; the catalog's own guarantee is the sanitizer's.
 *
 * Every flipper runs for as long as its reader storm does, never for a fixed count: a setter
 * is orders of magnitude cheaper than a write or a vertex creation, so a counted flipper
 * finishes in the first few percent of the storm and the rest runs unopposed — which is how
 * a race detector passes vacuously. Every scenario also carries a positive control asserting
 * the graph did its day job and the seam actually fired.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/graph.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::view::rope_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;
using tr::testing::make_value;

/** @brief A seam context that knows which seam it belongs to. */
struct probe_t {
    char tag;                  /**< @brief 'A' or 'B' — whose context this is. */
    std::atomic<long> hits{0}; /**< @brief Dispatches that reached the right seam. */
};

/** @brief Contexts handed to the wrong seam, or to a destroyed one — must stay zero. */
std::atomic<long> g_torn{0};

/**
 * @brief Score one dispatch: the seam was handed a context, and it must be ITS context.
 * @param ctx      The `void*` the graph passed back.
 * @param expected The tag of the probe this seam is only ever installed with.
 */
void score(void* ctx, char expected) {
    auto* const p = static_cast<probe_t*>(ctx);
    if (p == nullptr || p->tag != expected) {
        g_torn.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    p->hits.fetch_add(1, std::memory_order_relaxed);
}

void sink_a(void* ctx, const tr::graph::remote_delivery_t&, const rope_t&) { score(ctx, 'A'); }
void sink_b(void* ctx, const tr::graph::remote_delivery_t&, const rope_t&) { score(ctx, 'B'); }

std::expected<tr::graph::subject_token_t, tr::wire::err_t> resolver_a(void* ctx,
                                                                      std::string_view caller) {
    score(ctx, 'A');
    return tr::graph::subject_token_t(caller.size(), std::byte{'a'});
}
std::expected<tr::graph::subject_token_t, tr::wire::err_t> resolver_b(void* ctx,
                                                                      std::string_view caller) {
    score(ctx, 'B');
    return tr::graph::subject_token_t(caller.size(), std::byte{'a'});
}

void observer_a(void* ctx, const tr::graph::sub_event_t&) { score(ctx, 'A'); }
void observer_b(void* ctx, const tr::graph::sub_event_t&) { score(ctx, 'B'); }

/** @brief `PATH{ NAME segs… }` — the canonical key payload, via the production emitters. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

/** @brief `SUBSCRIBER{ PATH @p marker }` — one wire subscription record. */
std::vector<std::byte> b_subscriber(std::string_view marker) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, b_path({marker}));
    return out;
}

/** @brief `SPEC{ NAME "type" @p type, NAME "name" @p name }` — one in-band creation. */
view_t b_spec(std::string_view type, std::string_view name) {
    std::vector<std::byte> body;
    tr::wire::emit_name(body, "type");
    tr::wire::emit_name(body, type);
    tr::wire::emit_name(body, "name");
    tr::wire::emit_name(body, name);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);
    return make_value(out);
}

/** @brief Bind one wire subscriber at @p v arriving over @p link. */
bool wire_sub(graph_t& g, vertex_handle_t v, std::string_view link, std::string_view marker) {
    return g
        .subscribe_wire(v, make_value(b_subscriber(marker)), make_value(b_path({link})),
                        std::string(link))
        .has_value();
}

constexpr int kWrites = 120000;

/** @brief The settled positive-control batch each callback scenario runs after its storm. */
constexpr int kSettled = 100;

/**
 * @brief The remote-delivery sink — the WRITE HOT PATH — flipped under a write storm.
 */
void remote_sink_flip_race() {
    std::printf("remote-delivery sink, flipped under a write storm:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE);
    check(wire_sub(g, v, "cli", "c0"), "one REMOTE subscriber edge, so the sink is reached");

    probe_t a{'A'};
    probe_t b{'B'};
    const long torn_before = g_torn.load();
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<long> ok_writes{0};

    std::thread flipper([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        while (!stop.load(std::memory_order_relaxed)) {
            g.configure_remote_delivery_sink(&sink_a, &a);
            g.configure_remote_delivery_sink(&sink_b, &b);
        }
    });
    std::thread writer([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < kWrites; ++i)
            if (g.write(v, make_value({0x41})).has_value()) ok_writes.fetch_add(1);
        stop.store(true, std::memory_order_relaxed);
    });
    go.store(true, std::memory_order_release);
    flipper.join();
    writer.join();

    const long hits = a.hits.load() + b.hits.load();
    std::printf("    (%ld of %d writes reached a sink during the storm)\n", hits, kWrites);
    check(ok_writes.load() == kWrites, "every write still succeeded under the storm");
    check(hits > 0, "the sink was reachable DURING the storm (the torn check is not vacuous)");
    check(g_torn.load() == torn_before, "no sink was handed the other sink's context");
    // The positive control is SETTLED, not a share of the storm. A slot mid-publish reads as
    // EMPTY by design, and a flipper publishing back-to-back swallows a large — and
    // build-dependent — share: under TSan the setter is barely slowed while the write path is,
    // so a percentage floor that holds on a release build fails on a sanitizer leg for a
    // reason that has nothing to do with the defect. Assert instead that with the slot
    // SETTLED every single write reaches the sink, exactly.
    g.configure_remote_delivery_sink(&sink_a, &a);
    const long before = a.hits.load();
    for (int i = 0; i < kSettled; ++i) (void)g.write(v, make_value({0x41}));
    check(a.hits.load() - before == kSettled, "settled: every write reaches the sink, exactly");
}

/**
 * @brief The subject resolver — the ACL gate, on every gated read and write.
 */
void subject_resolver_flip_race() {
    std::printf("subject resolver, flipped under a gated read/write storm:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE);

    probe_t a{'A'};
    probe_t b{'B'};
    const long torn_before = g_torn.load();
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<long> ok_ops{0};

    std::thread flipper([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        while (!stop.load(std::memory_order_relaxed)) {
            g.configure_subject_resolver(&resolver_a, &a);
            g.configure_subject_resolver(&resolver_b, &b);
        }
    });
    std::thread ops([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        // A NON-EMPTY caller is what reaches the resolver at all: the empty (local) context
        // is settled as trusted before it runs (#905). No ACE bears on /v, so every op is
        // allowed whichever resolver answered — the assertion is about WHOSE context each
        // was handed, not about the verdict.
        for (int i = 0; i < kWrites; ++i) {
            if (g.write(v, make_value({0x42}), "peer-a").has_value()) ok_ops.fetch_add(1);
            if (g.read(v, "peer-a").has_value()) ok_ops.fetch_add(1);
        }
        stop.store(true, std::memory_order_relaxed);
    });
    go.store(true, std::memory_order_release);
    flipper.join();
    ops.join();

    const long hits = a.hits.load() + b.hits.load();
    std::printf("    (%ld of %d gated ops consulted a resolver during the storm)\n", hits,
                2 * kWrites);
    check(ok_ops.load() == 2 * kWrites, "every gated op still succeeded under the storm");
    check(hits > 0, "the resolver was reachable DURING the storm (the torn check is not vacuous)");
    check(g_torn.load() == torn_before, "no resolver was handed the other resolver's context");
    // Settled positive control — see remote_sink_flip_race for why it is not a share.
    g.configure_subject_resolver(&resolver_a, &a);
    const long before = a.hits.load();
    for (int i = 0; i < kSettled; ++i) (void)g.read(v, "peer-a");
    check(a.hits.load() - before == kSettled, "settled: every gated read consults the resolver");
}

constexpr int kSubs = 5000;

/**
 * @brief The subscription observer — the subscribe / clear path.
 */
void subscription_observer_flip_race() {
    std::printf("subscription observer, flipped under a wire-subscribe storm:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE);

    probe_t a{'A'};
    probe_t b{'B'};
    const long torn_before = g_torn.load();
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<long> ok_subs{0};

    std::thread flipper([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        while (!stop.load(std::memory_order_relaxed)) {
            g.configure_subscription_observer(&observer_a, &a);
            g.configure_subscription_observer(&observer_b, &b);
        }
    });
    std::thread subscriber([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        // subscribe_wire carries a non-EMPTY caller (the link NAME), which is exactly the
        // EXTERNAL discrimination the observer fires on.
        for (int i = 0; i < kSubs; ++i)
            if (wire_sub(g, v, "cli", "c")) ok_subs.fetch_add(1);
        stop.store(true, std::memory_order_relaxed);
    });
    go.store(true, std::memory_order_release);
    flipper.join();
    subscriber.join();

    const long hits = a.hits.load() + b.hits.load();
    std::printf("    (%ld of %d subscribes reached an observer during the storm)\n", hits, kSubs);
    check(ok_subs.load() == kSubs, "every subscribe still landed under the storm");
    check(hits > 0, "the observer was reachable DURING the storm (the torn check is not vacuous)");
    check(g_torn.load() == torn_before, "no observer was handed the other observer's context");
    // Settled positive control — see remote_sink_flip_race for why it is not a share.
    g.configure_subscription_observer(&observer_a, &a);
    const long before = a.hits.load();
    for (int i = 0; i < kSettled; ++i) (void)wire_sub(g, v, "cli", "c");
    check(a.hits.load() - before == kSettled, "settled: every subscribe reaches the observer");
}

constexpr int kCreates = 20000;

/**
 * @brief The creatable-child-type catalog: a public insert against a tree a PEER's bytes walk.
 *
 * The map is the member the `{fn, ctx}` publication cannot reach, so this scenario's verdict
 * is the set of legal outcomes rather than a context tag. Every in-band creation must answer
 * either "created" or `SCHEMA_NOT_FOUND` / `PATH_IN_USE` — a walk of a tree mid-rebalance
 * answers neither reliably, and under TSan/ASan it is reported outright.
 */
void child_catalog_flip_race() {
    std::printf("child-type catalog, registered under an in-band creation storm:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<long> created{0};
    std::atomic<long> illegal{0};

    std::thread flipper([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        // Distinct keys, so the tree genuinely GROWS and rebalances rather than
        // insert_or_assign-ing one node in place. BOUNDED at kFillers, then cycled: the
        // catalog has no erase, and under a sanitizer the reader storm runs slowly enough
        // that an unbounded flipper would allocate a fresh `std::function` and map node per
        // iteration for minutes. kFillers is sized so the GROWTH phase outlasts the whole
        // creation storm on a release build (~25 MB of catalog) while still being bounded. The
        // growth phase is what rebalances; cycling afterwards still mutates the map the reader
        // walks.
        constexpr int kFillers = 200000;
        for (int i = 0; !stop.load(std::memory_order_relaxed); ++i) {
            g.register_child_type(
                "filler" + std::to_string(i % kFillers),
                [](graph_t& gg, std::vector<std::byte> key, const tr::wire::tlv_t*) {
                    return gg.register_vertex_key(std::move(key), role_t::STORED_VALUE);
                });
        }
    });
    std::thread creator([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < kCreates; ++i) {
            const auto w =
                g.write(path_t("/dev:children[]"), b_spec("stored_value", "c" + std::to_string(i)));
            if (w.has_value()) {
                created.fetch_add(1);
            } else if (w.error() != status_t::SCHEMA_NOT_FOUND &&
                       w.error() != status_t::PATH_IN_USE && w.error() != status_t::BACKPRESSURE) {
                illegal.fetch_add(1);
            }
        }
        stop.store(true, std::memory_order_relaxed);
    });
    go.store(true, std::memory_order_release);
    flipper.join();
    creator.join();

    std::printf("    (%ld of %d in-band creations landed)\n", created.load(), kCreates);
    // Positive control: the built-in `stored_value` entry is registered by the constructor
    // and never removed, so EVERY creation must have found it. A walk that missed it is the
    // failure this scenario exists to catch.
    check(created.load() == kCreates, "every in-band creation found the catalog entry");
    check(illegal.load() == 0, "no creation answered outside the legal status set");
}

constexpr int kIdentityReads = 120000;

/**
 * @brief The node identity record: install / clear against the read served ABOVE the gate.
 *
 * The one member on #1049's list backing an operation served to an UNAUTHENTICATED peer by
 * design (RFC-0011 §C.2 — a TOFU peer must be able to pin the key before it can prove
 * anything), which is what makes its use-after-free remotely reachable. `read_identity` tests
 * emptiness and then memcpys the buffer; install and clear each free it. The verdict is that
 * every answer is one of the two LEGAL ones — the 60-byte ed25519 record, or absent — never a
 * third thing, and never a read of freed memory (loudest under ASan/TSan).
 */
void identity_flip_race() {
    std::printf("node identity record, rotated under a pre-auth :identity read storm:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    const auto p = path_t::parse("/dev:identity");
    check(p.has_value(), "/dev:identity parses");
    const auto v = g.find(p->key());
    check(v.has_value(), "/dev resolves");

    const std::vector<std::byte> key_a(32, std::byte{0x11});
    const std::vector<std::byte> key_b(32, std::byte{0x22});

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<long> served{0};
    std::atomic<long> absent{0};
    std::atomic<long> illegal{0};

    std::thread flipper([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        while (!stop.load(std::memory_order_relaxed)) {
            (void)g.set_identity(0x01, key_a);
            (void)g.set_identity(0x01, key_b);
            g.clear_identity();
        }
    });
    std::thread reader([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < kIdentityReads; ++i) {
            // "mallory" — an uncredentialed caller, which is precisely who this facet is
            // reachable by, and the reason a UAF here is a remote primitive rather than a
            // host-side footgun.
            const auto r = g.read(*v, p->field(), "mallory");
            if (!r) {
                if (r.error() == status_t::SCHEMA_NOT_FOUND) {
                    absent.fetch_add(1);
                } else {
                    illegal.fetch_add(1);
                }
                continue;
            }
            const view_t flat = r->flatten();
            const auto bytes = flat.bytes();
            // 60 bytes is the RFC-0011 §B ed25519 record, pinned by identity_test. Any other
            // size is a record this graph never built.
            if (bytes.size() != 60) {
                illegal.fetch_add(1);
                continue;
            }
            served.fetch_add(1);
        }
        stop.store(true, std::memory_order_relaxed);
    });
    go.store(true, std::memory_order_release);
    flipper.join();
    reader.join();

    std::printf("    (%ld served, %ld absent, of %d reads)\n", served.load(), absent.load(),
                kIdentityReads);
    check(illegal.load() == 0, "every :identity answer was the 60-byte record or absent");
    // Positive control both ways: a run that only ever saw "absent" would prove nothing
    // about the read of a live buffer, and one that never saw "absent" never raced a clear.
    check(served.load() > 0, "the record WAS served during the rotation storm");
    check(absent.load() > 0, "the clear WAS observed during the rotation storm");
}

}  // namespace

int main() {
    std::printf("graph_t configuration-seam publish coherence (#1049):\n");
    remote_sink_flip_race();
    subject_resolver_flip_race();
    subscription_observer_flip_race();
    child_catalog_flip_race();
    identity_flip_race();
    return tr::testing::summary("graph_config_race");
}
