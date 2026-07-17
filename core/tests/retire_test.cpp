/**
 * @file
 * @brief graph_t::retire() — RFC-0009 §B/§C/§E.6 vertex retirement (#407).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Asserts the properties retirement exists for, and the ones a NAIVE retire (just
 * flip registered_) would get wrong:
 *   - §B.2 the vertex becomes invisible to find / read / :children[];
 *   - §C.2 a revived-but-unwritten path reads not_found, NOT the retired owner's value;
 *   - §B.6 re-virginize: a revived vertex inherits NONE of the retired owner's state —
 *     the confused-deputy (stale ACEs) and the stale-handler (a lock-free UAF) are the
 *     two the panel proved a flag-flip alone would ship;
 *   - §B.3 retiring a vertex retires its whole subtree;
 *   - §B.4 idempotent; §B.5 delivers nothing.
 *
 * The final test races a lock-free :children reader against a retire on the same vertex
 * — the case the atomic-swap-and-park handler machinery exists for. It carries its own
 * weight under ThreadSanitizer (core-ci.yml `tsan` gate).
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::ace_t;
using tr::graph::ace_type_t;
using tr::graph::acl_right_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::subject_token_t;
using tr::graph::vertex_handle_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    if (!s.empty()) std::memcpy(out.data(), s.data(), s.size());
    return out;
}

tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

tr::view::view_t val_u8(std::uint8_t b) {
    const std::byte one[1] = {std::byte{b}};
    return make_value(one);
}

/** @brief The ADR-0018 test resolver: a non-empty caller is its own subject; empty = trusted. */
std::optional<subject_token_t> caller_is_subject(std::string_view caller) {
    if (caller.empty()) return std::nullopt;
    return as_bytes(caller);
}

std::vector<std::byte> acl_granting(std::string_view subject, std::uint32_t mask,
                                    std::uint8_t flags = 0) {
    std::vector<ace_t> a;
    a.push_back(ace_t{.type = ace_type_t::ALLOW,
                      .flags = flags,
                      .subject = as_bytes(subject),
                      .access_mask = mask});
    return tr::graph::encode_acl(a);
}

constexpr std::uint32_t bit(acl_right_t r) { return static_cast<std::uint32_t>(r); }

// ---------------------------------------------------------------------------
// §B.2 — a retired vertex is invisible to find / read / :children[].
void test_retire_hides_vertex() {
    std::printf("§B.2: a retired vertex is invisible (find / read / :children):\n");
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    vertex_handle_t b = g.register_vertex(path_t("/dev/b"), role_t::STORED_VALUE);
    (void)g.write(b, val_u8(0x55));

    check(g.read(path_t("/dev/b")).has_value(), "before retire: /dev/b reads");
    check(g.find(path_t("/dev/b").key()).has_value(), "before retire: /dev/b resolves via find");

    check(g.retire(b).has_value(), "retire(/dev/b) succeeds");

    const auto r = g.read(path_t("/dev/b"));
    check(!r.has_value() && r.error() == status_t::NOT_FOUND,
          "after retire: /dev/b reads NOT_FOUND (0x0020, as never-built)");
    check(!g.find(path_t("/dev/b").key()).has_value(), "after retire: /dev/b no longer resolves");
    // The parent's :children[] no longer lists it (the enumerated children exclude
    // unregistered nodes).
    const auto kids = g.read(path_t("/dev:children"));
    check(kids.has_value(), "/dev:children still reads");
    if (kids.has_value()) {
        const tr::view::view_t flat = kids->flatten();
        const auto dec = tr::wire::decode(flat.bytes());
        const bool lists_b = dec && [&] {
            for (const auto& m : dec->children)
                for (const auto& gc : m.children)
                    if (gc.type == type_t::NAME && tr::detail::as_string_view(gc.payload) == "b")
                        return true;
            return false;
        }();
        check(!lists_b, "/dev:children no longer lists the retired child 'b'");
    }
}

// ---------------------------------------------------------------------------
// §B.4 / §C.2 / §E.1 — revive succeeds (not PATH_IN_USE), and is a fresh vertex.
void test_revive_is_fresh() {
    std::printf("§E.1: write-creates revives a retired path as a FRESH vertex:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    vertex_handle_t b = g.register_vertex(path_t("/dev/b"), role_t::STORED_VALUE);
    (void)g.write(b, val_u8(0x55));
    check(g.retire(b).has_value(), "retire(/dev/b)");

    // §B.4: re-registering the same path succeeds — it is not PATH_IN_USE anymore.
    const auto revived = g.try_register_vertex(path_t("/dev/b"), role_t::STORED_VALUE);
    check(revived.has_value(), "revive: re-register /dev/b succeeds (NOT PATH_IN_USE)");

    // §C.2: the revived path is valueless — the retired owner's 0x55 is GONE.
    const auto r = g.read(path_t("/dev/b"));
    check(!r.has_value() && r.error() == status_t::NOT_FOUND,
          "revived /dev/b reads NOT_FOUND — the retired LKV did not survive");

    // And it accepts a fresh write.
    if (revived.has_value()) {
        (void)g.write(*revived, val_u8(0x22));
        const auto r2 = g.read(path_t("/dev/b"));
        bool ok = false;
        if (r2.has_value()) {
            const tr::view::view_t f = r2->flatten();
            ok = f.bytes().size() == 1 && f.bytes()[0] == std::byte{0x22};
        }
        check(ok, "revived /dev/b takes a fresh write (reads 0x22)");
    }
}

// ---------------------------------------------------------------------------
// §B.6 — the confused deputy: a revived path inherits its PARENT's ACL, never the
// retired owner's. This is the security core of retirement.
void test_confused_deputy() {
    std::printf("§B.6: revived path inherits the PARENT ACL, not the retired owner's:\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    (void)g.register_vertex(path_t("/net"), role_t::STORED_VALUE);
    vertex_handle_t b = g.register_vertex(path_t("/net/b"), role_t::STORED_VALUE);

    // Parent grants READ to "alice", INHERIT-flagged so descendants pick it up. The child
    // /net/b is locked to "xavier" ALONE (its own ACE shadows the inherited one).
    check(g.write(path_t("/net:acl"),
                  make_value(acl_granting("alice", bit(acl_right_t::READ), tr::graph::kAceInherit)))
              .has_value(),
          "/net:acl grants alice READ (INHERIT)");
    check(g.write(path_t("/net/b:acl"), make_value(acl_granting("xavier", bit(acl_right_t::READ))))
              .has_value(),
          "/net/b:acl grants xavier READ (its own ACE, atop the inherited alice grant)");
    (void)g.write(b, val_u8(0x01), {});  // trusted local seed

    // Baseline: /net/b's effective grants are {xavier (own), alice (inherited)} — an
    // unrelated subject is denied. xavier is the discriminating one: its access comes
    // SOLELY from the child's own ACE, which retirement must not let survive.
    check(g.read(b, "xavier").has_value(), "before retire: xavier reads /net/b (its own ACE)");
    check(!g.read(b, "mallory").has_value(), "before retire: an unrelated subject is denied");

    // Retire, then a DIFFERENT owner revives /net/b.
    check(g.retire(b).has_value(), "retire(/net/b)");
    const auto y = g.try_register_vertex(path_t("/net/b"), role_t::STORED_VALUE);
    check(y.has_value(), "revive /net/b");
    if (!y.has_value()) return;
    (void)g.write(*y, val_u8(0x02), {});  // trusted local seed on the revived vertex

    // The revived /net/b has NO own ACE → it inherits /net's policy: alice YES, xavier NO.
    check(g.read(*y, "alice").has_value(),
          "revived /net/b: alice READS — it inherited the PARENT policy");
    check(!g.read(*y, "xavier").has_value(),
          "revived /net/b: xavier DENIED — the retired owner's ACE did NOT survive (no deputy)");
}

// ---------------------------------------------------------------------------
// §B.6 — no stale handler: a revived vertex runs none of the retired owner's value seam.
void test_no_stale_handler() {
    std::printf("§B.6: a revived vertex runs none of the retired owner's handlers:\n");
    graph_t g;
    auto ran = std::make_shared<std::atomic<int>>(0);

    tr::graph::handlers_t h;
    h.on_children = [ran]() -> tr::graph::result_t<tr::view::view_t> {
        ran->fetch_add(1, std::memory_order_relaxed);
        // A recognisable synthesized listing (an empty POINT is enough to tell it ran).
        std::vector<std::byte> pt;
        tr::wire::emit_tlv(pt, type_t::POINT, opt_t{.pl = true}, std::span<const std::byte>{});
        const auto v = tr::view::over_bytes(pt);
        if (!v) return std::unexpected(status_t::BACKPRESSURE);
        return *v;
    };
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    vertex_handle_t handler = g.register_vertex(path_t("/dev/h"), role_t::HANDLER, std::move(h));

    (void)g.read(path_t("/dev/h:children"));  // exercises the handler once
    check(ran->load() >= 1, "the HANDLER's on_children ran before retire");

    check(g.retire(handler).has_value(), "retire(/dev/h)");
    const int before = ran->load();
    // Revive as a PLAIN stored_value (supplies NO handlers).
    const auto revived = g.try_register_vertex(path_t("/dev/h"), role_t::STORED_VALUE);
    check(revived.has_value(), "revive /dev/h as a plain stored_value");
    (void)g.read(path_t("/dev/h:children"));  // must NOT reach the retired handler
    check(ran->load() == before,
          "the retired owner's on_children did NOT run on the revived vertex (no stale seam)");
}

// ---------------------------------------------------------------------------
// §B.3 — retiring a vertex retires its whole subtree.
void test_subtree() {
    std::printf("§B.3: retire takes the whole subtree:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/a"), role_t::STORED_VALUE);
    vertex_handle_t a = g.find(path_t("/a").key()).value();
    (void)g.register_vertex(path_t("/a/b"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/a/b/c"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/a/d"), role_t::STORED_VALUE);

    check(g.retire(a).has_value(), "retire(/a)");
    for (const char* p : {"/a", "/a/b", "/a/b/c", "/a/d"}) {
        const auto r = g.read(path_t(p));
        check(!r.has_value() && r.error() == status_t::NOT_FOUND,
              std::string("subtree member gone: ") + p);
    }
    // The subtree is revivable afterwards.
    check(g.try_register_vertex(path_t("/a/b/c"), role_t::STORED_VALUE).has_value(),
          "a retired subtree member re-registers (revive through revived intermediates)");
}

// ---------------------------------------------------------------------------
// §B.4 — idempotent; §B.5 — delivers nothing.
void test_idempotent_and_silent() {
    std::printf("§B.4/§B.5: idempotent, and retirement delivers nothing:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    vertex_handle_t leaf = g.register_vertex(path_t("/p/leaf"), role_t::STORED_VALUE);

    // A subtree subscriber above the retired vertex, to catch any spurious delivery.
    auto deliveries = std::make_shared<std::atomic<int>>(0);
    auto on_deliver = [deliveries](const tr::view::rope_t&) {
        deliveries->fetch_add(1, std::memory_order_relaxed);
    };
    (void)g.subscribe(path_t("/p"), on_deliver);
    const int before = deliveries->load();

    check(g.retire(leaf).has_value(), "retire(/p/leaf)");
    check(deliveries->load() == before, "retirement delivered nothing to the ancestor subscriber");

    // §B.4: a second retire on the same (now unregistered) vertex is a no-op success.
    check(g.retire(leaf).has_value(), "retiring an already-retired vertex succeeds (idempotent)");
    check(deliveries->load() == before, "the idempotent retire also delivered nothing");
}

// ---------------------------------------------------------------------------
// The concurrency case the atomic-swap-and-park handler machinery exists for: lock-free
// :children readers racing a retire on the same vertex. This must survive both the DATA
// race (TSAN) and the logical race a naive caller has — a check-then-call across two
// seam loads, where a swap between them throws std::bad_function_call. The window is
// widened deliberately (many concurrent readers hammering with NO gating sleep, retire
// mid-flight) so a regression aborts here rather than intermittently in CI.
void test_concurrent_read_vs_retire() {
    std::printf("TSAN: lock-free :children readers race retire on the same vertex:\n");
    auto make_handler = [] {
        tr::graph::handlers_t h;
        h.on_children = []() -> tr::graph::result_t<tr::view::view_t> {
            std::vector<std::byte> pt;
            tr::wire::emit_tlv(pt, type_t::POINT, opt_t{.pl = true}, std::span<const std::byte>{});
            const auto v = tr::view::over_bytes(pt);
            if (!v) return std::unexpected(status_t::BACKPRESSURE);
            return *v;
        };
        return h;
    };
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/dev/h"), role_t::HANDLER, make_handler());

    std::atomic<bool> stop{false};
    std::atomic<long> reads{0};
    std::atomic<long> churns{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                (void)g.read(path_t("/dev/h:children"));  // lock-free seam load inside
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    // A churner that retires AND revives /dev/h in a tight loop: each cycle swaps the
    // seam pointer to null (retire) then to a fresh block (revive), so thousands of swaps
    // race the readers' seam loads. A between-loads swap on a naive check-then-call caller
    // throws bad_function_call — this widens the window far past the single-retire form
    // that only surfaced intermittently in CI.
    std::thread churner([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            if (auto vh = g.find(path_t("/dev/h").key()); vh.has_value()) (void)g.retire(*vh);
            (void)g.try_register_vertex(path_t("/dev/h"), role_t::HANDLER, make_handler());
            churns.fetch_add(1, std::memory_order_relaxed);
        }
    });
    while (reads.load() < 20000 || churns.load() < 2000) { /* spin until well-mixed */
    }
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& r : readers) r.join();
    churner.join();
    check(true, "read-vs-retire/revive churn completed without a crash / bad_function_call / UAF");
}

}  // namespace

int main() {
    test_retire_hides_vertex();
    test_revive_is_fresh();
    test_confused_deputy();
    test_no_stale_handler();
    test_subtree();
    test_idempotent_and_silent();
    test_concurrent_read_vs_retire();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
