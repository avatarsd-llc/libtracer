/**
 * @file
 * @brief Re-registering at one vertex address reuses that address's declaration nodes (#2032).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `graph_t` keeps two declaration lists off the vertex: the ADMISSION hooks
 * (`handlers_t::on_admit` and the app-field pair) and the RFC-0014 Amendment 2/3 payload-right
 * rows with their `:schema` catalog. The vertex map is insert-only (ADR-0057), so retiring a
 * path and registering it again leaves the vertex count flat. Before #2032 each such cycle
 * PREPENDED one more immortal node to each list, so memory and the gate's walk grew with churn
 * rather than with population.
 *
 * Pinned here, through a counting table source injected as the graph's root:
 *
 * 1. N register / write / retire cycles at ONE path, each with its own `on_admit`, its own rows
 *    and its own catalog, hold the source's live blocks and bytes FLAT after the first cycle;
 * 2. every cycle is served the CURRENT registration's declarations, never a previous occupant's:
 *    the filter that runs, the right the gate demands, and the `:schema` catalog served;
 * 3. `set_creation_hook`, installed again and again, rewrites the same node in place — flat —
 *    and carries the vertex's filter over unchanged;
 * 4. a writer and a `:schema` reader racing the churn from another thread never see a torn
 *    declaration: no filter is ever called with another registration's context, and every
 *    catalog served is one registration's, whole (or none, for a read that met a retirement). (Its
 * teeth are under ThreadSanitizer and on a weakly ordered core; on the host it is the smoke test of
 * the latch.)
 *
 * Vector 1 fails on the pre-#2032 tree: the live-block count rises by two every cycle.
 */

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::acl_right_t;
using tr::graph::admission_t;
using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::payload_right_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::subject_token_t;
using tr::graph::vertex_handle_t;
using tr::graph::write_ctx_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;
using tr::testing::make_value;

/** @brief A heap-backed source that counts its live blocks and bytes, so growth is visible. */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    counting_source_t() noexcept : tr::mem::block_source_t("churn") {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
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
        return s;
    }

    /** @brief Blocks drawn and not yet released. */
    [[nodiscard]] std::size_t live() const noexcept { return live_; }

   private:
    std::size_t live_ = 0;   /**< @brief Outstanding blocks. */
    std::size_t in_use_ = 0; /**< @brief Outstanding bytes. */
};

/** @brief `s` as bytes. */
std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief The test resolver: the caller context IS the subject (acl_test's shape). */
std::expected<subject_token_t, tr::wire::err_t> caller_is_subject(void*, std::string_view caller) {
    return as_bytes(caller);
}

/** @brief One ALLOW ACE granting @p subject exactly @p right, as a whole `:acl` payload. */
std::vector<std::byte> allow(std::string_view subject, acl_right_t right) {
    const std::vector<tr::graph::ace_t> aces{
        tr::graph::ace_t{.type = tr::graph::ace_type_t::ALLOW,
                         .flags = 0,
                         .subject = as_bytes(subject),
                         .access_mask = static_cast<std::uint32_t>(right),
                         .expires_ns = 0}};
    return tr::graph::encode_acl(aces);
}

/** @brief A `SPEC{}` payload — the type every cycle's rows declare a right for. */
tr::view::view_t spec_payload() {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, std::span<const std::byte>{});
    return make_value(out);
}

/** @brief Which registration's filter ran last, written by @ref record_admit. */
struct occupant_t {
    int id = -1;    /**< @brief The registration this context belongs to. */
    int* last_seen; /**< @brief Where the filter records whose context it was handed. */
};

/** @brief The filter every cycle installs: record whose context ran, accept as written. */
admission_t record_admit(void* ctx, const tr::graph::value_t&, const write_ctx_t&) {
    const auto* o = static_cast<const occupant_t*>(ctx);
    *o->last_seen = o->id;
    return std::nullopt;
}

/** @brief True iff @p hay contains @p needle as a contiguous run. */
bool contains(std::span<const std::byte> hay, std::span<const std::byte> needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

/** @brief The bytes a `:schema` read serves (empty when the read failed). */
std::vector<std::byte> schema_bytes(graph_t& g) {
    const auto r = g.read(path_t("/ctl:schema"));
    if (!r) return {};
    const tr::view::view_t flat = (**r).flatten();
    const std::span<const std::byte> b = flat.bytes();
    return std::vector<std::byte>(b.begin(), b.end());
}

/**
 * @brief Free what this thread's hazard-pointer retired list parked, when the build binds
 *        `hazard_slot_t` — a displaced LKV value or declaration record waits there for a scan
 *        rather than freeing at once, so a sample taken without this reads the parking, not the
 *        graph. A no-op under every other slot.
 */
void drain_parked() {
    if constexpr (std::is_same_v<tr::graph::lkv_slot_t, tr::graph::hazard_slot_t>)
        tr::graph::detail_hp::retire_and_flush(nullptr);
}

/** @brief A source sample: live blocks and bytes. */
struct sample_t {
    std::size_t live = 0;   /**< @brief Live blocks. */
    std::size_t in_use = 0; /**< @brief Live bytes. */
};

// --- 1 + 2. the churn, and who answers each cycle -------------------------------------------

/**
 * @brief Vectors 1 and 2 — N cycles at one path stay flat, and each cycle is its own.
 *
 * The cycles alternate the filter's context, the right `SPEC` demands (CREATE, then WRITE) and
 * the catalog's content (same length, so a flat source is the expected reading, not a lucky
 * one). The `CREATE`-only peer's write is therefore admitted on even cycles and refused on odd
 * ones — a stale row from the previous occupant would flip that.
 */
void test_reregistration_is_flat_and_current() {
    std::printf("vectors 1-2 — re-registration churn at one path:\n");
    constexpr int kCycles = 32;
    counting_source_t src;
    {
        graph_t g(src);
        {
            auto hooks = g.hooks();
            hooks.subject_resolver = {caller_is_subject, nullptr};
            g.set_hooks(hooks);
        }
        const path_t path("/ctl");
        const std::span<const std::byte> key = path.key();
        int last_seen = -1;
        std::vector<occupant_t> occupants;
        occupants.reserve(kCycles);
        std::vector<sample_t> after;
        bool hooks_current = true;
        bool rows_current = true;
        bool catalog_current = true;
        bool cycles_ok = true;
        for (int i = 0; i < kCycles; ++i) {
            occupants.push_back(occupant_t{i, &last_seen});
            handlers_t h;
            h.on_admit = {&record_admit, &occupants.back()};
            const bool even = (i % 2) == 0;
            const payload_right_t rows[] = {
                payload_right_t{type_t::SPEC, even ? acl_right_t::CREATE : acl_right_t::WRITE}};
            std::vector<std::byte> catalog;
            tr::wire::emit_name(catalog, even ? "even" : "odd!");

            const auto v = g.register_vertex_key(key, role_t::STORED_VALUE, h, {}, rows, catalog);
            if (!v) {
                check(false, "the path registers again");
                return;
            }
            cycles_ok = cycles_ok && g.write(path_t("/ctl:acl"),
                                             make_value(allow("peer-c", acl_right_t::CREATE)))
                                         .has_value();

            last_seen = -1;
            const auto w = g.write(*v, spec_payload(), "peer-c");
            rows_current = rows_current && (w.has_value() == even);
            hooks_current = hooks_current && (even ? last_seen == i : last_seen == -1);
            catalog_current = catalog_current && contains(schema_bytes(g), catalog);

            cycles_ok = cycles_ok && g.retire(*v).has_value();
            drain_parked();
            after.push_back(sample_t{src.live(), src.stats().in_use});
        }
        check(cycles_ok, "every cycle registers, authorizes peer-c and retires");
        check(hooks_current, "every cycle's write ran THAT cycle's filter, never an earlier one");
        check(rows_current, "every cycle's gate demanded THAT cycle's right for SPEC");
        check(catalog_current, "every cycle's :schema served THAT cycle's catalog");
        // The first cycle draws the nodes; the second is the first true re-registration.
        const sample_t base = after[1];
        bool flat = true;
        for (std::size_t i = 2; i < after.size(); ++i)
            flat = flat && after[i].live == base.live && after[i].in_use == base.in_use;
        if (!flat)
            std::printf("    live blocks after cycle 2: %zu, after cycle %d: %zu\n", base.live,
                        kCycles, after.back().live);
        check(flat, "live blocks and bytes are FLAT after the first cycle");
    }
    check(src.live() == 0, "and the graph returns every block it drew");
}

// --- 3. the creation hook -------------------------------------------------------------------

/** @brief A creation hook that registers nothing — only its installation is under test. */
tr::graph::result_t<void> create_nothing(void*, vertex_handle_t, std::span<const std::byte>,
                                         std::string_view, const tr::view::rope_t&) {
    return std::unexpected(status_t::NOT_FOUND);
}

/**
 * @brief Vector 3 — installing the creation hook again and again rewrites one node, and the
 *        vertex's filter rides along unchanged.
 */
void test_creation_hook_reinstall_is_flat() {
    std::printf("vector 3 — creation-hook re-installation:\n");
    if constexpr (!tr::graph::config_t::kCreationHooks) {
        std::printf("    (skipped: this build closes out creation hooks)\n");
        return;
    }
    constexpr int kInstalls = 16;
    counting_source_t src;
    {
        graph_t g(src);
        int last_seen = -1;
        occupant_t me{7, &last_seen};
        handlers_t h;
        h.on_admit = {&record_admit, &me};
        const vertex_handle_t v = g.register_vertex(path_t("/dev"), role_t::STORED_VALUE, h);
        std::vector<std::size_t> live;
        bool installed = true;
        for (int i = 0; i < kInstalls; ++i) {
            installed = installed && g.set_creation_hook(v, {&create_nothing, nullptr}).has_value();
            live.push_back(src.live());
        }
        check(installed, "every installation succeeds");
        check(std::all_of(live.begin(), live.end(), [&](std::size_t n) { return n == live[0]; }),
              "every re-installation reuses the vertex's node: live blocks FLAT");
        check(g.write(v, tr::view::rope_t{make_value({0x01})}).has_value(), "a write lands");
        check(last_seen == 7, "and still meets the vertex's own filter");
    }
    check(src.live() == 0, "and the graph returns every block it drew");
}

// --- 4. the race ---------------------------------------------------------------------------

/** @brief A context that names the filter it belongs to — a torn hook pairs it with another. */
struct tagged_t {
    int tag;                /**< @brief 1 for @ref admit_one's, 2 for @ref admit_two's. */
    std::atomic<int>* torn; /**< @brief Bumped when a filter is handed a foreign tag. */
};

/** @brief The odd registrations' filter; counts a context that is not its own. */
admission_t admit_one(void* ctx, const tr::graph::value_t&, const write_ctx_t&) {
    const auto* t = static_cast<const tagged_t*>(ctx);
    if (t->tag != 1) t->torn->fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
}

/** @brief The even registrations' filter; counts a context that is not its own. */
admission_t admit_two(void* ctx, const tr::graph::value_t&, const write_ctx_t&) {
    const auto* t = static_cast<const tagged_t*>(ctx);
    if (t->tag != 2) t->torn->fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
}

/**
 * @brief Vector 4 — re-registration on one thread, writes and `:schema` reads on another.
 *
 * The registrations alternate both hook halves (filter AND context) and both catalogs, so a
 * reader that copied `fn` from one and `ctx` from the next calls a filter with a foreign tag,
 * and a reader that saw half a record serves neither catalog.
 */
void test_racing_reader_sees_whole_declarations() {
    std::printf("vector 4 — a reader racing the churn:\n");
    constexpr int kCycles = 2000;
    graph_t g;
    const path_t path("/ctl");
    const std::span<const std::byte> key = path.key();
    std::atomic<int> torn{0};
    tagged_t one{1, &torn};
    tagged_t two{2, &torn};
    std::vector<std::byte> cat_one;
    std::vector<std::byte> cat_two;
    tr::wire::emit_name(cat_one, "catalog-one");
    tr::wire::emit_name(cat_two, "catalog-two-is-longer");
    const payload_right_t rows[] = {payload_right_t{type_t::SPEC, acl_right_t::WRITE}};

    // What `:schema` serves a vertex at this path that declared no catalog: the answer a
    // reader racing a RETIREMENT may get, since retirement lowers the flag the read tests.
    std::vector<std::byte> no_catalog;
    if (const auto v = g.register_vertex_key(key, role_t::STORED_VALUE)) {
        no_catalog = schema_bytes(g);
        (void)g.retire(*v);
    }
    check(!no_catalog.empty(), "the catalog-less answer is known");

    std::atomic<bool> done{false};
    std::atomic<int> foreign_catalogs{0};
    std::atomic<int> reads{0};
    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            if (const auto h = g.find(key)) (void)g.write(*h, tr::view::rope_t{make_value({1})});
            const std::vector<std::byte> b = schema_bytes(g);
            if (!b.empty() && b != no_catalog && !contains(b, cat_one) && !contains(b, cat_two))
                foreign_catalogs.fetch_add(1, std::memory_order_relaxed);
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });
    bool cycles_ok = true;
    for (int i = 0; i < kCycles; ++i) {
        const bool odd = (i % 2) != 0;
        handlers_t h;
        h.on_admit = odd ? tr::graph::admit_hook_t{&admit_one, &one}
                         : tr::graph::admit_hook_t{&admit_two, &two};
        const std::vector<std::byte>& catalog = odd ? cat_one : cat_two;
        const auto v = g.register_vertex_key(key, role_t::STORED_VALUE, h, {}, rows, catalog);
        cycles_ok = cycles_ok && v.has_value() && g.retire(*v).has_value();
    }
    done.store(true, std::memory_order_release);
    reader.join();
    check(cycles_ok, "every cycle registers and retires");
    check(torn.load() == 0, "no filter was ever handed another registration's context");
    check(foreign_catalogs.load() == 0,
          "every catalog served was one registration's, whole, or none (a racing retirement)");
    std::printf("    (%d racing reads)\n", reads.load());
}

}  // namespace

int main() {
    test_reregistration_is_flat_and_current();
    test_creation_hook_reinstall_is_flat();
    test_racing_reader_sees_whole_declarations();
    return tr::testing::summary("declaration_churn");
}
