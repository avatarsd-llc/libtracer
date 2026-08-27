/**
 * @file
 * @brief Registry teardown — `child_registry_t::erase` / `remove_child` / `remove_connection`
 * (#494).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `child_registry_t` used to be add-only, so a retired connection left its
 * `name → transport_t*` entry resident and dangling — a use-after-free the moment
 * RFC-0014's remove-half (#492 S2b) drives create/remove churn from the wire. These
 * tests pin the teardown contract:
 *   - a removed connection's NAME stops resolving (the #494 regression: a forward to it
 *     must be a clean miss, not a route into freed memory);
 *   - removal retires the identity vertex — `/net/<name>` reads not_found;
 *   - the slot is TOMBSTONED, not erased: `size()` is stable while `live_size()` drops,
 *     which is what keeps a concurrent lock-free reader's iteration valid;
 *   - re-creating the same NAME REUSES its tombstone, so create/remove churn on a stable
 *     name set does not grow the table (the aggressive-churn HIL case);
 *   - removing an unknown NAME is a clean NotFound, and removal is idempotent;
 *   - and, since #873 phase 1, that the chunks the table grows by come from the INJECTED
 *     `tr::mem::block_source_t`, are returned to it at teardown, and answer a refusal by
 *     value rather than falling back to the global heap.
 */

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/mem_source.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::status_t;
using tr::net::child_registry_t;
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;

using tr::testing::check;

/** @brief A transport that only records what it was handed — no socket, no thread. */
class sink_link_t : public tr::net::transport_t {
   public:
    void send(std::span<const std::byte> frame) override {
        sent_.emplace_back(frame.begin(), frame.end());
    }

    /** @brief Number of frames this link was asked to carry. */
    [[nodiscard]] std::size_t sends() const noexcept { return sent_.size(); }

   private:
    std::vector<std::vector<std::byte>> sent_;
};

/** @brief SPEC{ name } with no config — the provide_link-staged connection form. */
tr::view::view_t conn_spec(std::string_view name) { return tr::net::conn_spec_t(name).view(); }

/** @brief The module staged connections in this test mount under. */
constexpr std::string_view kModule = "ws-client";

/** @brief The full mount path a connection of @p name occupies (RFC-0014 / ADR-0061). */
std::string mount_of(std::string_view name) {
    return "net/" + std::string(kModule) + "/" + std::string(name);
}

/**
 * @brief Create connection @p name over the staged @p link through the module's CREATOR
 *        ENDPOINT — `/net/<module>/conn`, the one creation door since RFC-0014 S7.
 *
 * The module is declared with an EMPTY kind: a staged link bypasses the transport factory, so
 * there is no kind to name, and the kind-less SPEC below resolves to that one declaration.
 * `register_module` deduplicates on (kind, role), so repeating it per create declares once.
 */
bool create_conn(graph_t& g, transport_vertex_t& net, sink_link_t& link, std::string_view name) {
    if (!net.register_module(std::string(kModule), "", tr::net::conn_role_t::DIAL)) return false;
    net.provide_link(std::string(kModule), std::string(name), link);
    const auto p = path_t::parse("/net/" + std::string(kModule) + "/conn");
    if (!p) return false;
    return g.write(*p, conn_spec(name)).has_value();
}

// --- the tests ---------------------------------------------------------------

/** @brief The #494 regression: a removed NAME stops resolving; its vertex is retired. */
void test_removed_name_stops_resolving() {
    std::printf("removed NAME stops resolving\n");
    graph_t g;
    fwd_router_t router(g);
    transport_vertex_t net(g, router);
    sink_link_t link;

    check(create_conn(g, net, link, "up"), "created /net/up over a staged link");
    check(router.registry().by_name(mount_of("up")) == &link,
          "the NAME resolves to its link while live");
    // A provided link is never auto-published (only a config-constructed socket self-reports),
    // so give the vertex a value to read back — that is what retirement must take away.
    check(net.set_link_state(mount_of("up"), tr::net::link_state_t::UP).has_value(),
          "published its liveness");
    check(g.read(*path_t::parse("/net/ws-client/up")).has_value(), "/net/up reads while live");

    const auto rm = net.remove_connection(mount_of("up"));
    check(rm.has_value(), "remove_connection succeeds");
    check(router.registry().by_name(mount_of("up")) == nullptr,
          "the NAME resolves to NOTHING after removal (#494: no dangling transport_t*)");
    const auto after = g.read(*path_t::parse("/net/ws-client/up"));
    check(!after && after.error() == status_t::NOT_FOUND, "/net/up is retired — reads not_found");
}

/** @brief The slot is tombstoned, not erased — stable addresses for lock-free readers. */
void test_tombstone_not_erase() {
    std::printf("erase tombstones rather than shrinking the table\n");
    child_registry_t reg;
    sink_link_t a;
    sink_link_t b;
    (void)reg.add("a", a);
    (void)reg.add("b", b);
    check(reg.size() == 2 && reg.live_size() == 2, "two live children");

    check(reg.erase("a"), "erase reports it removed a live child");
    check(reg.size() == 2, "the SLOT stays (no shift, no realloc — readers keep iterating)");
    check(reg.live_size() == 1, "but only one child still resolves");
    check(reg.by_name("a") == nullptr, "the tombstoned NAME misses");
    check(reg.by_name("b") == &b, "its neighbour is untouched");
    check(!reg.erase("a"), "erasing a tombstone reports nothing removed (idempotent)");
    check(!reg.erase("nope"), "erasing an unknown NAME reports nothing removed");
}

/** @brief Churn on a stable name set reuses tombstones — the table does not grow. */
void test_churn_reuses_tombstones() {
    std::printf("create/remove churn reuses tombstones\n");
    child_registry_t reg;
    sink_link_t a;
    sink_link_t b;
    (void)reg.add("conn", a);
    for (int i = 0; i < 50; ++i) {
        reg.erase("conn");
        (void)reg.add("conn", (i % 2 == 0) ? b : a);
    }
    reg.erase("conn");
    (void)reg.add("conn", b);
    check(reg.size() == 1, "51 create/remove rounds on one NAME still occupy ONE slot");
    check(reg.by_name("conn") == &b, "and the NAME resolves to the CURRENT link");

    sink_link_t c;
    (void)reg.add("other", c);
    check(reg.size() == 2, "a genuinely new NAME appends");
}

/** @brief Removal is idempotent at the vertex layer, and unknown names are NotFound. */
void test_remove_unknown_and_idempotent() {
    std::printf("removing an absent connection\n");
    graph_t g;
    fwd_router_t router(g);
    transport_vertex_t net(g, router);
    sink_link_t link;

    const auto miss = net.remove_connection(mount_of("never-made"));
    check(!miss && miss.error() == status_t::NOT_FOUND, "an unknown NAME is a clean NotFound");

    check(create_conn(g, net, link, "up"), "created /net/up");
    check(net.remove_connection(mount_of("up")).has_value(), "first removal succeeds");
    const auto again = net.remove_connection(mount_of("up"));
    check(!again && again.error() == status_t::NOT_FOUND,
          "a second removal is NotFound, not a crash");
}

/** @brief A removed NAME is free for re-use — the vertex re-virginizes (RFC-0009 §B.6). */
void test_name_is_reusable_after_removal() {
    std::printf("a removed NAME can be created again\n");
    graph_t g;
    fwd_router_t router(g);
    transport_vertex_t net(g, router);
    sink_link_t first;
    sink_link_t second;

    check(create_conn(g, net, first, "up"), "created /net/up over link #1");
    check(net.remove_connection(mount_of("up")).has_value(), "removed it");
    check(create_conn(g, net, second, "up"), "created /net/up again over link #2");
    check(router.registry().by_name(mount_of("up")) == &second,
          "the NAME now resolves to the NEW link");
    check(router.registry().size() == 1, "and it reused the tombstoned slot");
}

/**
 * @brief Re-adding a LIVE name rebinds its slot — it never grows a second, shadow one.
 *
 * The regression. `add` only ever reused a TOMBSTONE, so re-registering a live name appended
 * a duplicate. `erase` then nulled the first match and returned `true` — the caller destroys
 * its transport on that `true` — while `by_name` went on resolving the freed link through the
 * shadow slot. That is exactly the dangling-`transport_t*` hole #494 was written to close,
 * reopened from a direction it did not consider.
 */
void test_duplicate_add_rebinds() {
    std::printf("duplicate add rebinds rather than shadowing\n");
    child_registry_t reg;
    sink_link_t a;
    sink_link_t b;
    (void)reg.add("net/ws-client/x", a);
    (void)reg.add("net/ws-client/x", a);
    check(reg.size() == 1, "a repeated add does not grow the table");
    check(reg.live_size() == 1, "and leaves exactly one live child");

    check(reg.erase("net/ws-client/x"), "erase reports it removed a live child");
    check(reg.by_name("net/ws-client/x") == nullptr,
          "and NOTHING resolves afterwards — no shadow slot keeps the freed link reachable");

    // Rebinding to a different link must take effect, not resolve the stale one.
    (void)reg.add("net/ws-client/x", a);
    (void)reg.add("net/ws-client/x", b);
    check(reg.by_name("net/ws-client/x") == &b, "a re-add rebinds the name to the NEW link");
    check(reg.live_size() == 1, "still one slot for the name");
}

/**
 * @brief A slot's address survives every later append (ADR-0063 / #521).
 *
 * The property the chunked list exists for, and the one `std::vector` could not give:
 * `push_back` reallocated and invalidated every slot reference in the table. That was sound
 * only while the registry was "immutable after setup" — RFC-0014 ended that by making
 * connection create/remove a RUNTIME operation, so a lock-free forward read can be walking
 * the table while a CREATE appends on another thread.
 *
 * Pinned by ADDRESS rather than by value: a stale pointer into a reallocated vector usually
 * still reads plausible bytes, so only identity catches the regression.
 */
/**
 * @brief The chunks come from the INJECTED source, are RETURNED at teardown, and a refusal
 *        is a clean `false` rather than a fallback to the global heap (#873 phase 1).
 *
 * Three properties in one fixture, because they are the same claim from three sides. The
 * counting source proves the bytes travel the seam at all (on the pre-#873 code it serves
 * nothing and the first row reddens); its balance at scope exit proves teardown releases
 * every chunk in the size it took it, which is what a pool source needs to recycle them; and
 * a source that refuses proves the registry answers by value on the growth path it always
 * answered by value on — the seam `conn_add_oom_test` drives from the other end.
 */
void test_chunks_draw_from_the_injected_source() {
    std::printf("registry chunks draw from the injected block_source_t (#873 phase 1)\n");

    /** @brief A pass-through source counting live blocks and the sizes it served. */
    class counting_source_t final : public tr::mem::block_source_t {
       public:
        counting_source_t() noexcept : block_source_t("counting") {}
        std::size_t served = 0; /**< @brief Blocks handed out. */
        std::size_t live = 0;   /**< @brief Blocks not yet returned. */
        bool refuse = false;    /**< @brief Refuse everything from now on. */

        [[nodiscard]] void* try_alloc(std::size_t n, std::size_t a) noexcept override {
            if (refuse) return nullptr;
            void* const p = tr::mem::heap_source().try_alloc(n, a);
            if (p == nullptr) return nullptr;
            ++served;
            ++live;
            return p;
        }
        void release(void* p, std::size_t n, std::size_t a) noexcept override {
            --live;
            tr::mem::heap_source().release(p, n, a);
        }
    };

    counting_source_t src;
    std::vector<std::unique_ptr<sink_link_t>> links;
    {
        child_registry_t reg(src);
        // Enough names to cross at least one chunk boundary, so more than one block is drawn.
        constexpr std::size_t kN = 9;
        bool added = true;
        for (std::size_t i = 0; i < kN; ++i) {
            links.push_back(std::make_unique<sink_link_t>());
            added = added && reg.add("net/ws-client/c" + std::to_string(i), *links.back());
        }
        check(added && reg.live_size() == kN, "every child registered through the seam");
        check(src.served >= 2, "the INJECTED source served the chunks (more than one of them)");
        check(src.live == src.served, "and nothing was returned while the table is alive");
    }
    check(src.live == 0, "teardown returned every chunk to the source (sized release)");

    // A refusing source: the growth answers by value, exactly as a full table does — never an
    // abort, and never a silent fallback to the global heap.
    {
        counting_source_t refusing;
        refusing.refuse = true;
        child_registry_t reg(refusing);
        sink_link_t link;
        check(!reg.add("net/ws-client/x", link), "a refused chunk makes add() report failure");
        check(reg.size() == 0, "and registers nothing at all");
        check(refusing.served == 0, "nothing was served, so nothing escaped to the heap either");
    }
}

void test_slot_addresses_are_stable() {
    std::printf("slot addresses survive appends (#521)\n");
    child_registry_t reg;
    std::vector<std::unique_ptr<sink_link_t>> links;
    std::vector<const child_registry_t::child_t*> slots;

    // Span several chunks so the walk crosses chunk boundaries, not just one block.
    constexpr std::size_t kN = 17;
    for (std::size_t i = 0; i < kN; ++i) {
        links.push_back(std::make_unique<sink_link_t>());
        const std::string name = "net/ws-client/l" + std::to_string(i);
        (void)reg.add(name, *links.back());
        slots.push_back(reg.entry_by_name(name));
    }
    check(slots.size() == kN && slots[0] != nullptr, "every child registered");

    bool stable = true;
    bool resolves = true;
    for (std::size_t i = 0; i < kN; ++i) {
        const std::string name = "net/ws-client/l" + std::to_string(i);
        if (reg.entry_by_name(name) != slots[i]) stable = false;
        if (slots[i]->link() != links[i].get()) resolves = false;
    }
    check(stable, "the address captured at add time is STILL the slot's address");
    check(resolves, "and each slot still points at its own link");
    check(reg.live_size() == kN, "all children live across the chunk boundaries");

    // A tombstone must not disturb its neighbours' addresses either.
    check(reg.erase("net/ws-client/l0"), "erase a child in the first chunk");
    bool stable_after = true;
    for (std::size_t i = 1; i < kN; ++i) {
        if (reg.entry_by_name("net/ws-client/l" + std::to_string(i)) != slots[i])
            stable_after = false;
    }
    check(stable_after, "a tombstone leaves every other slot address untouched");
    check(slots[0]->link() == nullptr, "and the tombstoned slot reads null in place");
}

/**
 * @brief The scan's two digest paths must agree — pinned DIRECTLY, not only through a lookup.
 *
 * `by_segments` pre-filters candidates on `digest_segments(segs)` and every slot carries
 * `digest_name(name)` computed at `add` time. If those two ever disagreed, nothing would throw
 * and no test would obviously break: the registry would simply stop resolving the affected
 * name, which reads as a routing bug arbitrarily far away. So compare the two functions on
 * their own, across the shapes that actually stress the split — one segment, three segments,
 * an empty segment, a name whose bytes contain no separator, and names differing only in the
 * last byte (the pair the digest most needs to tell apart).
 */
void test_digest_paths_agree() {
    using reg_t = tr::net::child_registry_t;
    const std::vector<std::vector<std::string>> cases = {
        {"net"},
        {"net", "ws-client"},
        {"net", "ws-client", "peer-a"},
        {"net", "ws-client", "peer-b"},
        {"net", "can", "0"},
        {"net", "can", "1"},
        {"", "x"},
        {"a", "", "b"},
        {"averyverylongmodulename", "andaverylongchildnametoo"},
    };
    bool agree = true;
    for (const auto& segs : cases) {
        std::string joined;
        std::vector<std::string_view> views;
        views.reserve(segs.size());
        for (std::size_t i = 0; i < segs.size(); ++i) {
            if (i != 0) joined += '/';
            joined += segs[i];
        }
        // The views must point INTO the finished string's segments, not into `segs`, so this
        // exercises the same borrowed-span shape the forward path hands in.
        std::size_t at = 0;
        for (std::size_t i = 0; i < segs.size(); ++i) {
            views.emplace_back(joined.data() + at, segs[i].size());
            at += segs[i].size() + 1;
        }
        if (reg_t::digest_name(joined) != reg_t::digest_segments(views)) {
            std::printf("  [FAIL] digest disagreement on \"%s\"\n", joined.c_str());
            agree = false;
        }
    }
    check(agree, "digest_name and digest_segments agree on every shape");

    // Discrimination, not just agreement: the pairs above that differ only in their last byte
    // must land on different digests, or the filter degenerates to the length check alone.
    const std::vector<std::string_view> a = {"net", "can", "0"};
    const std::vector<std::string_view> b = {"net", "can", "1"};
    check(reg_t::digest_segments(a) != reg_t::digest_segments(b),
          "names differing only in the last byte get different digests");
}

}  // namespace

int main() {
    test_removed_name_stops_resolving();
    test_tombstone_not_erase();
    test_churn_reuses_tombstones();
    test_remove_unknown_and_idempotent();
    test_name_is_reusable_after_removal();

    test_duplicate_add_rebinds();
    test_slot_addresses_are_stable();
    test_chunks_draw_from_the_injected_source();
    test_digest_paths_agree();
    return tr::testing::summary("registry_teardown");
}
