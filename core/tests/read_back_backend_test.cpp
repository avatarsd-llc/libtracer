/**
 * @file
 * @brief #1583 — every #873 phase-3 READ-BACK ENCODER draws its owned segment from `graph_t`'s
 *        injected source, and a refused draw degrades BY VALUE.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Phase 3 of #873 moved ten `view::over_bytes` sites in `core/src/graph.cpp` off the
 * one-argument (global-heap) spelling and onto the graph's injected `value_backend_`. The
 * `docs/reference/09-memory-substrate.md` ledger row named `folded_read_backend_test` and
 * `graph_value_backend_test` as the instruments, but those two pin only the `:children` FOLD
 * and the write-path `value.materialize` flatten. The other encoders were unpinned: reverting
 * one to the one-argument `over_bytes` would have stayed green. This file is that pin.
 *
 * @section families The families, one per migrated site
 *
 * Eight are reached as READS, whose answer IS the encoded record, so the width the seam sees
 * is measured rather than asserted:
 *
 *   - `:stats.graph.delivery` — the census block (`read_stats`);
 *   - `:schema` — the synthesized POINT (`read_schema`);
 *   - `:settings` and `:settings.app` — the RFC-0010 §A.4 containers;
 *   - `:settings.app.<name>` — one declared app field's stored bytes;
 *   - `:acl` — the re-encoded ACE list (`read_acl`);
 *   - `:identity` — the pre-serialized node record (`read_identity`);
 *   - `read_children_materialized` — the MATERIALIZED listing (`read_children`), which is a
 *     different encoder from the `:children` fold `folded_read_backend_test` already pins.
 *
 * Two are reached as SUBSCRIBES, whose encoded record never crosses back to the caller, so the
 * width is stated analytically from the encoder's own arithmetic:
 *
 *   - the owned `SUBSCRIBER{PATH}` record the `subscribe` sugar enters the field-write door
 *     with;
 *   - the owned mount-route `PATH` TLV `subscribe_wire` binds on a mount-routed target.
 *
 * @section instrument Three arms per family, in this order
 *
 * 1. **WIDTH** — run the family once on a pass-through probe and learn how many bytes the
 *    encoder produces (or take the stated arithmetic for the two write-shaped families). What
 *    reaches the source is ONE draw of `source_backend_t::block_bytes(width)`, because phase 3
 *    packs the `segment_t` control block and the payload into a single `try_alloc`.
 * 2. **ROUTING** — re-run it with the probe watching exactly that block size, armed only AFTER
 *    the fixture is built so no registration or pmr draw is counted. Exactly one watched draw.
 *    Put the encoder back on the one-argument `over_bytes` and this is 0.
 * 3. **REFUSAL** — re-run it with the probe REFUSING that block size, again armed only after
 *    setup. The family must answer `BACKPRESSURE` by value. This is the arm that pins "and not
 *    the global heap": a silent fallback to `malloc` would make the operation SUCCEED, and a
 *    throw or an abort would not be a value at all.
 *
 * The probe passes every unwatched size straight through to the process heap source, untouched
 * and uncounted — the `folded_read_backend_test` convention, and the reason `served() == 1`
 * still means "one segment for this encoder" now that ONE source serves the whole graph.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/mem_source.hpp"
#include "libtracer/mem_source_backend.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::ace_t;
using tr::graph::acl_right_t;
using tr::graph::app_access_t;
using tr::graph::app_field_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::result_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::graph::wire_target_split_t;
using tr::view::rope_t;
using tr::view::view_t;

using tr::testing::check;

/**
 * @brief A pass-through `block_source_t` that can COUNT or REFUSE draws of one exact size.
 *
 * Both instruments are armed after the fixture is built, so the registration blocks, the pmr
 * control blocks and the failable channel — which since the #873 phase-1 constructor collapse
 * all come from this same source — are served silently and never counted.
 */
class probe_source_t final : public tr::mem::block_source_t {
   public:
    probe_source_t() noexcept : block_source_t("read_back_probe") {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (bytes == watch_ && watch_ != 0) {
            if (refuse_) {
                ++refused_;
                return nullptr;
            }
            ++served_;
        }
        return tr::mem::heap_source().try_alloc(bytes, align);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::heap_source().release(p, bytes, align);
    }

    /** @brief Count draws of exactly @p n bytes from now on (the routing instrument). */
    void watch(std::size_t n) noexcept { watch_ = n; }
    /** @brief Refuse draws of exactly @p n bytes from now on (the exhaustion instrument). */
    void refuse(std::size_t n) noexcept {
        watch_ = n;
        refuse_ = true;
    }
    /** @brief How many watched draws were served. */
    [[nodiscard]] std::size_t served() const noexcept { return served_; }
    /** @brief How many watched draws were refused. */
    [[nodiscard]] std::size_t refused() const noexcept { return refused_; }

   private:
    std::size_t watch_ = 0;
    bool refuse_ = false;
    std::size_t served_ = 0;
    std::size_t refused_ = 0;
};

/** @brief The one BLOCK an owned record of @p width bytes costs at the injected source. */
constexpr std::size_t block_for(std::size_t width) noexcept {
    return tr::mem::source_backend_t::block_bytes(width);
}

/**
 * @brief The three arms, over a family whose ANSWER carries the encoded record.
 *
 * @param what  The family's name, for the check messages.
 * @param setup Builds the fixture on a fresh graph and returns the vertex the read addresses.
 * @param read  Performs the read; returns the record's byte count, or the failing status.
 */
template <class setup_t, class read_t>
void pin_read_family(std::string_view what, setup_t setup, read_t read) {
    std::size_t width = 0;
    {
        probe_source_t src;
        graph_t g(&src);
        const vertex_handle_t v = setup(g);
        const result_t<std::size_t> r = read(g, v);
        check(r.has_value(), std::string(what) + " — the read answers at all (the fixture holds)");
        if (!r) return;
        width = *r;
    }
    const std::size_t block = block_for(width);

    {
        probe_source_t src;
        graph_t g(&src);
        const vertex_handle_t v = setup(g);
        src.watch(block);
        const result_t<std::size_t> r = read(g, v);
        check(r.has_value() && src.served() == 1,
              std::string(what) + " — exactly ONE segment drawn from the INJECTED source");
    }
    {
        probe_source_t src;
        graph_t g(&src);
        const vertex_handle_t v = setup(g);
        src.refuse(block);
        const result_t<std::size_t> r = read(g, v);
        check(src.refused() > 0, std::string(what) + " — the refusing source WAS consulted");
        check(!r.has_value() && r.error() == status_t::BACKPRESSURE,
              std::string(what) + " — a refused segment is BACKPRESSURE, never a heap fallback");
    }
}

/**
 * @brief The routing and refusal arms, over a family whose encoded record never crosses back.
 *
 * @param what  The family's name, for the check messages.
 * @param width The record's byte count, stated from the encoder's own arithmetic.
 * @param setup Builds the fixture on a fresh graph.
 * @param op    Performs the operation that encodes the record.
 */
template <class setup_t, class op_t>
void pin_op_family(std::string_view what, std::size_t width, setup_t setup, op_t op) {
    const std::size_t block = block_for(width);
    {
        probe_source_t src;
        graph_t g(&src);
        setup(g);
        src.watch(block);
        const result_t<void> r = op(g);
        check(r.has_value() && src.served() == 1,
              std::string(what) + " — exactly ONE segment drawn from the INJECTED source");
    }
    {
        probe_source_t src;
        graph_t g(&src);
        setup(g);
        src.refuse(block);
        const result_t<void> r = op(g);
        check(src.refused() > 0, std::string(what) + " — the refusing source WAS consulted");
        check(!r.has_value() && r.error() == status_t::BACKPRESSURE,
              std::string(what) + " — a refused segment is BACKPRESSURE, never a heap fallback");
    }
}

/** @brief A string's bytes as an ACL subject token. */
std::vector<std::byte> subject_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief The record count a field read answers, or the status it failed with. */
result_t<std::size_t> record_width(result_t<rope_t> r) {
    if (!r) return std::unexpected(r.error());
    return r->flatten().bytes().size();
}

/** @brief Read one field spelling off @p v — the whole `path:field` form, parsed once. */
result_t<std::size_t> read_field(const graph_t& g, vertex_handle_t v, std::string_view spelling) {
    const result_t<path_t> p = path_t::parse(spelling);
    if (!p) return std::unexpected(status_t::INVALID_PATH);
    return record_width(g.read(v, p->field()));
}

/** @brief Register `/s` as a plain stored value — the fixture most families need. */
vertex_handle_t plain_vertex(graph_t& g) {
    return g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
}

/** @brief `/s` with one declared `rw` app field `kp`, holding a written VALUE TLV. */
vertex_handle_t vertex_with_app_field(graph_t& g) {
    const vertex_handle_t v = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    std::vector<app_field_t> table;
    table.push_back(app_field_t{.name = "kp", .access = app_access_t::RW});
    g.set_app_fields(v, std::move(table));
    const std::array<std::byte, 2> le{std::byte{0x88}, std::byte{0x13}};  // 5000
    const tr::wire::tlv_t value{.type = tr::wire::type_t::VALUE, .payload = le};
    const std::vector<std::byte> bytes = tr::wire::encode(value);
    const result_t<path_t> fp = path_t::parse("/s:settings.app.kp");
    (void)g.write(v, fp->field(), tr::testing::make_value(bytes));
    return v;
}

/** @brief The `SUBSCRIBER{PATH}` bytes a wire subscribe carries toward @p target. */
std::vector<std::byte> wire_subscriber(const path_t& target) {
    const std::span<const std::byte> key = target.key();
    std::vector<std::byte> body;
    tr::wire::emit_tlv(body, tr::wire::type_t::PATH, tr::wire::opt_t{}, key);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, tr::wire::type_t::SUBSCRIBER, tr::wire::opt_t{.pl = true}, body);
    return out;
}

/** @brief A `view_t` over an owned copy of @p bytes (global heap — never the graph's source). */
view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) seg->bytes[i] = bytes[i];
    return view_t::over(std::move(seg));
}

/** @brief The residual the stub resolver reports below the mount — one packed `leaf` record. */
constexpr std::array<std::byte, 5> kResidual{std::byte{0x04}, std::byte{'l'}, std::byte{'e'},
                                             std::byte{'a'}, std::byte{'f'}};

/**
 * @brief The stub mount resolver — every target routes through the mount `mnt`.
 *
 * It stands in for the transport plane's ADR-0061 strip-K descent, which is what
 * `configure_wire_target_resolver` exists to accept: the encoder under test is `graph_t`'s, so
 * pinning it needs no net plane and no live link.
 */
wire_target_split_t stub_split(void* ctx, std::span<const std::byte> key) {
    (void)ctx;
    (void)key;
    return wire_target_split_t{.link = "mnt", .residual = kResidual};
}

}  // namespace

/** @brief Run the #1583 read-back-encoder seam pins. */
int main() {
    std::printf("read-back encoders draw from the injected source (#873 phase 3, #1583):\n");

    // `graph.cpp:read_stats` — the `:stats` census block (RFC-0010 Amendment 1).
    pin_read_family("read_stats / :stats.graph.delivery", plain_vertex,
                    [](const graph_t& g, vertex_handle_t v) {
                        return read_field(g, v, "/s:stats.graph.delivery");
                    });

    // `graph.cpp:read_schema` — the synthesized `:schema` POINT.
    pin_read_family(
        "read_schema / :schema", vertex_with_app_field,
        [](const graph_t& g, vertex_handle_t v) { return read_field(g, v, "/s:schema"); });

    // `graph.cpp:read_settings` — the RFC-0010 §A.4 `:settings` container.
    pin_read_family(
        "read_settings / :settings", vertex_with_app_field,
        [](const graph_t& g, vertex_handle_t v) { return read_field(g, v, "/s:settings"); });

    // `graph.cpp:read_settings_app` — the app container alone.
    pin_read_family(
        "read_settings_app / :settings.app", vertex_with_app_field,
        [](const graph_t& g, vertex_handle_t v) { return read_field(g, v, "/s:settings.app"); });

    // `graph.cpp`'s NAMED app-field arm — one declared field's stored TLV, served verbatim.
    pin_read_family(
        "app field bytes / :settings.app.kp", vertex_with_app_field,
        [](const graph_t& g, vertex_handle_t v) { return read_field(g, v, "/s:settings.app.kp"); });

    // `graph.cpp:read_acl` — the RE-ENCODED ACE list (#907), not a copy of the stored bytes.
    pin_read_family(
        "read_acl / :acl",
        [](graph_t& g) {
            const vertex_handle_t v = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
            std::vector<ace_t> aces;
            ace_t grant;
            grant.subject = subject_bytes("EVERYONE@");
            grant.access_mask = static_cast<std::uint32_t>(acl_right_t::READ) |
                                static_cast<std::uint32_t>(acl_right_t::READ_ACL);
            aces.push_back(std::move(grant));
            (void)g.write(path_t("/s:acl"), tr::testing::make_value(tr::graph::encode_acl(aces)));
            return v;
        },
        [](const graph_t& g, vertex_handle_t v) { return read_field(g, v, "/s:acl"); });

    // `graph.cpp:read_identity` — the pre-serialized node record (RFC-0011 §C).
    pin_read_family(
        "read_identity / :identity",
        [](graph_t& g) {
            const vertex_handle_t v = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
            // The RFC-0011 §B registry: kind 0x01 is ed25519 and fixes the key at 32 bytes.
            std::array<std::byte, 32> key{};
            for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::byte>(i + 1);
            (void)g.set_identity(0x01, key);
            return v;
        },
        [](const graph_t& g, vertex_handle_t v) { return read_field(g, v, "/s:identity"); });

    // `graph.cpp:read_children` — the MATERIALIZED listing, a different encoder from the
    // `:children` FOLD that `folded_read_backend_test` already pins.
    pin_read_family(
        "read_children (materialized)",
        [](graph_t& g) {
            const vertex_handle_t v = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
            (void)g.register_vertex(path_t("/s/a"), role_t::STORED_VALUE);
            (void)g.register_vertex(path_t("/s/b"), role_t::STORED_VALUE);
            return v;
        },
        [](const graph_t& g, vertex_handle_t v) {
            return record_width(g.read_children_materialized(v));
        });

    // `graph.cpp:subscribe` — the owned `SUBSCRIBER{PATH}` the sugar enters the field-write
    // door with. Width from the encoder's own arithmetic: the SUBSCRIBER header, the PATH
    // header and the target's key, with no `SETTINGS` child for an all-zero policy.
    {
        const path_t target("/t");
        const std::size_t width = 4u + 4u + target.key().size();
        pin_op_family(
            "subscribe / the owned SUBSCRIBER record", width,
            [](graph_t& g) {
                (void)g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
                (void)g.register_vertex(path_t("/t"), role_t::STORED_VALUE);
            },
            [](graph_t& g) { return g.subscribe(path_t("/s"), path_t("/t")); });
    }

    // `graph.cpp:subscribe_wire` — the owned mount-route `PATH` TLV a mount-routed target
    // binds. The stub resolver stands in for the transport plane's strip-K descent, so this
    // pins the encoder without the net plane. Width: the PATH header plus the residual.
    {
        const std::vector<std::byte> sub = wire_subscriber(path_t("/anywhere"));
        const std::vector<std::byte> route = wire_subscriber(path_t("/back"));
        const std::size_t width = 4u + kResidual.size();
        pin_op_family(
            "subscribe_wire / the owned mount-route PATH", width,
            [](graph_t& g) {
                (void)g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
                g.configure_wire_target_resolver(stub_split, nullptr);
            },
            [&](graph_t& g) {
                const std::optional<vertex_handle_t> v = g.find(path_t("/s").key());
                if (!v) return result_t<void>(std::unexpect, status_t::NOT_FOUND);
                return g.subscribe_wire(*v, owned(sub), owned(route), "arrival", view_t{}, "",
                                        tr::graph::link_id_t{});
            });
    }

    return tr::testing::summary("read_back_backend");
}
