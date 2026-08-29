/**
 * @file
 * @brief The pre-store ADMISSION seam: `handlers_t::on_admit` on the retaining roles and
 *        `handlers_t::on_app_field_admit` on the app-field plane.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Before this seam a vertex could RETAIN a value or REFUSE a write, never both: refusal lived
 * on the HANDLER role's `on_write`, and a HANDLER retains nothing (`graph_t::store_value`'s
 * null-shared_ptr sentinel — see `nonretaining_contract_test`). The filter runs inside
 * `store_value`, above the tail every storing role shares, so what is pinned here is not merely
 * that a callback fires but WHERE it fires relative to state and delivery:
 *
 * 1. accept as written (`std::nullopt`) — vector 1;
 * 2. accept NORMALISED — the stored value and every delivery are the filter's, never the
 *    writer's — vectors 2 and 8 (STREAM);
 * 3. REFUSE — the status reaches the writer, the prior last-known-value stands and no
 *    subscriber sees anything — vectors 3, 6 (assign) and 9 (a fan-in delivery);
 * 4. the seam runs BEFORE the store and BEFORE delivery, proved from inside the filter — vector
 *    4;
 * 5. a vertex that installs no filter is untouched, and neither is the HANDLER role, whose
 *    `on_write` is its own seam — vectors 5 and 7;
 * 6. the app-field plane's equivalent, including that a refusal leaves the prior bytes and does
 *    not fire the apply seam — vectors 10-12.
 *
 * Every vector fails with the production hunks reverted: 1-9 by the filter never running (the
 * written value lands and is delivered), 10-12 by the field bytes storing verbatim.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::admission_t;
using tr::graph::app_access_t;
using tr::graph::app_field_t;
using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::graph::write_ctx_t;
using tr::view::rope_t;
using tr::view::view_t;

using tr::testing::check;
using tr::testing::make_value;

/** @brief The single byte a one-byte rope carries (0 when the rope is not that shape). */
[[nodiscard]] std::uint8_t only_byte(const rope_t& r) {
    const view_t flat = r.flatten();
    const std::span<const std::byte> b = flat.bytes();
    return b.size() == 1 ? std::to_integer<std::uint8_t>(b[0]) : 0;
}

/** @brief The single byte currently stored at @p p, or 0 when nothing is stored there. */
[[nodiscard]] std::uint8_t stored_byte(graph_t& g, const path_t& p) {
    const auto r = g.read(p);
    return r ? only_byte(**r) : 0;
}

/** @brief A one-byte value the graph never reads into anything (never a branch POINT). */
[[nodiscard]] rope_t byte_value(std::uint8_t b) { return rope_t{make_value({b})}; }

std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief A VALUE TLV over @p payload — the shape an app-field write carries. */
std::vector<std::byte> value_tlv(std::string_view payload) {
    std::vector<std::byte> out;
    const std::vector<std::byte> p = as_bytes(payload);
    tr::wire::emit_tlv(out, tr::wire::type_t::VALUE, tr::wire::opt_t{}, p);
    return out;
}

/** @brief The exact bytes a field read serves, flattened (empty when the read failed). */
[[nodiscard]] std::vector<std::byte> field_bytes(graph_t& g, const path_t& p) {
    const auto r = g.read(p);
    if (!r) return {};
    const view_t flat = (**r).flatten();
    const std::span<const std::byte> b = flat.bytes();
    return std::vector<std::byte>(b.begin(), b.end());
}

/** @brief One subscriber's delivery log — every byte it was handed, in order. */
struct log_t {
    std::vector<std::uint8_t> bytes;
};

// --- 1. accept as written -------------------------------------------------------------------

/**
 * @brief Vector 1 — a filter answering `std::nullopt` accepts the write UNCHANGED.
 *
 * The zero-work answer, and the one a pure validator gives. The vertex is a plain
 * `STORED_VALUE`: retention, delivery and read-back must be byte-identical to a vertex with no
 * filter at all, which is what vector 5 is the control for.
 */
void test_accept_as_written() {
    std::printf("vector 1 — accept as written:\n");
    graph_t g;
    int calls = 0;
    handlers_t h;
    h.on_admit = [&calls](const rope_t&, const write_ctx_t&) -> admission_t {
        ++calls;
        return std::nullopt;
    };
    const vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE, std::move(h));

    log_t seen;
    auto on_value = [&seen](const rope_t& r) { seen.bytes.push_back(only_byte(r)); };
    check(g.subscribe(path_t("/v"), on_value).has_value(), "subscribe to the filtered vertex");

    check(g.write(v, byte_value(0x11)).has_value(), "the accepted write succeeds");
    check(calls == 1, "the filter ran exactly once");
    check(stored_byte(g, path_t("/v")) == 0x11, "the written value is what is retained");
    check(seen.bytes.size() == 1 && seen.bytes[0] == 0x11, "and what the subscriber was delivered");
}

// --- 2. normalise ---------------------------------------------------------------------------

/**
 * @brief Vector 2 — a filter returning a rope replaces the written value EVERYWHERE.
 *
 * The writer's spelling must reach no reader: not the last-known-value, not the subscriber.
 * The filter here canonicalises "any non-zero" to `0x01`, the smallest normalisation with an
 * observable difference.
 */
void test_normalise_replaces_the_stored_value() {
    std::printf("vector 2 — normalise:\n");
    graph_t g;
    handlers_t h;
    h.on_admit = [](const rope_t& value, const write_ctx_t&) -> admission_t {
        if (only_byte(value) == 0) return std::nullopt;  // already canonical
        return rope_t{make_value({0x01})};
    };
    const vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE, std::move(h));

    log_t seen;
    auto on_value = [&seen](const rope_t& r) { seen.bytes.push_back(only_byte(r)); };
    check(g.subscribe(path_t("/v"), on_value).has_value(), "subscribe to the normalising vertex");

    check(g.write(v, byte_value(0x7f)).has_value(), "the normalised write succeeds");
    check(stored_byte(g, path_t("/v")) == 0x01, "the NORMALISED value is what is retained");
    check(seen.bytes.size() == 1 && seen.bytes[0] == 0x01,
          "the subscriber saw the normalised value and never the written one");

    // The pass-through arm of the same filter, so the vector cannot pass by normalising
    // unconditionally.
    check(g.write(v, byte_value(0x00)).has_value(), "control — the canonical write succeeds");
    check(seen.bytes.size() == 2 && seen.bytes[1] == 0x00,
          "control — a value the filter accepts as-is is delivered as-is");
}

// --- 3. refuse ------------------------------------------------------------------------------

/**
 * @brief Vector 3 — a refusal reaches the writer and leaves the vertex exactly as it was.
 *
 * The whole of the refusal contract in one vector: the status is the writer's (the same
 * propagation a HANDLER's `on_write` refusal takes), the PRIOR last-known-value stands, and the
 * subscriber is never delivered the refused value.
 */
void test_refusal_propagates_and_preserves_the_lkv() {
    std::printf("vector 3 — refuse:\n");
    graph_t g;
    handlers_t h;
    // Even bytes only — an invariant a stored value could not previously defend.
    h.on_admit = [](const rope_t& value, const write_ctx_t&) -> admission_t {
        if ((only_byte(value) & 1U) != 0) return std::unexpected(status_t::TYPE_MISMATCH);
        return std::nullopt;
    };
    const vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE, std::move(h));

    log_t seen;
    auto on_value = [&seen](const rope_t& r) { seen.bytes.push_back(only_byte(r)); };
    check(g.subscribe(path_t("/v"), on_value).has_value(), "subscribe to the refusing vertex");

    check(g.write(v, byte_value(0x20)).has_value(), "the admitted write lands");
    check(stored_byte(g, path_t("/v")) == 0x20, "and is retained");

    const auto refused = g.write(v, byte_value(0x21));
    check(!refused && refused.error() == status_t::TYPE_MISMATCH,
          "the refused write answers the filter's own status");
    check(stored_byte(g, path_t("/v")) == 0x20, "the PRIOR last-known-value is intact");
    check(seen.bytes.size() == 1 && seen.bytes[0] == 0x20,
          "and no subscriber ever saw the refused value");
}

// --- 4. ordering ----------------------------------------------------------------------------

/**
 * @brief Vector 4 — the filter runs BEFORE the store and BEFORE any delivery, proved from
 *        inside the filter itself.
 *
 * A callback that merely fires cannot tell "before the store" from "after it", so the filter
 * re-enters the graph and looks: the vertex must still hold the PRIOR value and the subscriber
 * must not have been called. Both observations are taken on the writer's thread, inside the one
 * call whose ordering is in question.
 */
void test_admission_precedes_store_and_delivery() {
    std::printf("vector 4 — admission runs before the store and before delivery:\n");
    graph_t g;
    log_t seen;
    std::uint8_t seen_during = 0xff;
    std::size_t deliveries_during = 0xff;

    handlers_t h;
    h.on_admit = [&](const rope_t&, const write_ctx_t&) -> admission_t {
        seen_during = stored_byte(g, path_t("/v"));
        deliveries_during = seen.bytes.size();
        return std::nullopt;
    };
    const vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE, std::move(h));
    auto on_value = [&seen](const rope_t& r) { seen.bytes.push_back(only_byte(r)); };
    check(g.subscribe(path_t("/v"), on_value).has_value(), "subscribe to the observed vertex");

    check(g.write(v, byte_value(0x40)).has_value(), "the first write lands");
    check(seen_during == 0 && deliveries_during == 0,
          "on the first write the vertex held nothing yet and nothing was delivered");
    check(g.write(v, byte_value(0x41)).has_value(), "the second write lands");
    check(seen_during == 0x40,
          "inside the filter the vertex still holds the PRIOR value — the store is downstream");
    check(deliveries_during == 1,
          "and only the FIRST write's delivery had happened — the fan-out is downstream too");
    check(stored_byte(g, path_t("/v")) == 0x41 && seen.bytes.size() == 2,
          "control — both writes did land and deliver once the call returned");
}

// --- 5. the no-filter fast path -------------------------------------------------------------

/**
 * @brief Vector 5 — a vertex that installs no filter is untouched, seam block or not.
 *
 * Two shapes, because the seam block is allocated on handler PRESENCE and not on role: a bare
 * leaf (no extension block at all) and a vertex that installed a DIFFERENT value seam
 * (`on_children`, so the block exists and only `on_admit` inside it is empty). Neither may
 * acquire an admission step.
 */
void test_no_filter_is_unchanged() {
    std::printf("vector 5 — the no-filter path:\n");
    graph_t g;
    const vertex_handle_t bare = g.register_vertex(path_t("/bare"), role_t::STORED_VALUE);

    handlers_t h;
    h.on_children = []() -> tr::graph::result_t<view_t> { return make_value({0x00}); };
    const vertex_handle_t seamed =
        g.register_vertex(path_t("/seamed"), role_t::STORED_VALUE, std::move(h));

    log_t seen;
    auto on_value = [&seen](const rope_t& r) { seen.bytes.push_back(only_byte(r)); };
    check(g.subscribe(path_t("/bare"), on_value).has_value(), "subscribe to the bare leaf");

    check(g.write(bare, byte_value(0x55)).has_value(), "the bare leaf accepts every write");
    check(stored_byte(g, path_t("/bare")) == 0x55, "and retains it verbatim");
    check(seen.bytes.size() == 1 && seen.bytes[0] == 0x55, "and delivers it verbatim");
    check(g.write(seamed, byte_value(0x56)).has_value(),
          "a vertex bearing a DIFFERENT value seam is not filtered either");
    check(stored_byte(g, path_t("/seamed")) == 0x56, "and retains it verbatim");
}

// --- 6. assign ------------------------------------------------------------------------------

/**
 * @brief Vector 6 — `assign` is filtered exactly as `write` is.
 *
 * Admission is a property of the VERTEX, not of a door: the state half of the
 * accumulate-then-flush pair stores through the same seam, so a filter's invariant holds against
 * the sweep plane too. Both arms are pinned — a refusal keeps the prior value out of the next
 * sweep, and a normalisation is what that sweep delivers.
 */
void test_assign_takes_the_same_seam() {
    std::printf("vector 6 — assign is filtered too:\n");
    graph_t g;
    handlers_t h;
    h.on_admit = [](const rope_t& value, const write_ctx_t&) -> admission_t {
        const std::uint8_t b = only_byte(value);
        if (b == 0xee) return std::unexpected(status_t::TYPE_MISMATCH);
        if (b == 0xaa) return rope_t{make_value({0xab})};
        return std::nullopt;
    };
    const vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE, std::move(h));

    log_t seen;
    auto on_value = [&seen](const rope_t& r) { seen.bytes.push_back(only_byte(r)); };
    check(g.subscribe(path_t("/v"), on_value).has_value(), "subscribe to the filtered vertex");

    const auto refused = g.assign(v, byte_value(0xee));
    check(!refused && refused.error() == status_t::TYPE_MISMATCH, "the refused assign says so");
    check(!g.read(path_t("/v")).has_value(), "and stored nothing at all");
    check(g.propagate(v).has_value(), "a sweep over the refused assign succeeds");
    check(seen.bytes.empty(), "and delivers nothing — there is no state to flush");

    check(g.assign(v, byte_value(0xaa)).has_value(), "the normalised assign lands");
    check(stored_byte(g, path_t("/v")) == 0xab, "as the NORMALISED value");
    check(g.propagate(v).has_value(), "the covering sweep succeeds");
    check(seen.bytes.size() == 1 && seen.bytes[0] == 0xab,
          "and flushes the normalised value, never the assigned one");
}

// --- 7. the HANDLER role --------------------------------------------------------------------

/**
 * @brief Vector 7 — a HANDLER vertex takes `on_write`, never `on_admit`.
 *
 * The two seams are the complementary halves of the write path's role fork, and a vertex is
 * never asked both: `on_write` already has the refusal power on the role that retains nothing,
 * and running an admission filter beside it would double-dispatch every handler write. Both
 * callbacks are installed here, so the vector fails if either the wrong one runs or both do.
 */
void test_handler_role_takes_on_write_only() {
    std::printf("vector 7 — the HANDLER role is unchanged:\n");
    graph_t g;
    int writes = 0;
    int admits = 0;
    handlers_t h;
    h.on_write = [&writes](const rope_t&, const write_ctx_t&) -> tr::graph::result_t<void> {
        ++writes;
        return {};
    };
    h.on_admit = [&admits](const rope_t&, const write_ctx_t&) -> admission_t {
        ++admits;
        return std::unexpected(status_t::TYPE_MISMATCH);  // would be visible if it ran
    };
    const vertex_handle_t v = g.register_vertex(path_t("/h"), role_t::HANDLER, std::move(h));

    check(g.write(v, byte_value(0x60)).has_value(), "the handler write succeeds");
    check(writes == 1, "on_write ran");
    check(admits == 0, "and the admission filter did not — the role fork is exclusive");
}

// --- 8. the STREAM role ---------------------------------------------------------------------

/**
 * @brief Vector 8 — a STREAM's ring queues the ADMITTED value.
 *
 * The receiving role's ring is charged and drained from what `store_value` published, so the
 * normalisation must be what the drain delivers — the retention half of the seam's promise on
 * the one role whose delivery does not read the last-known-value.
 */
void test_stream_ring_queues_the_admitted_value() {
    std::printf("vector 8 — a STREAM queues what was admitted:\n");
    graph_t g;
    handlers_t h;
    h.on_admit = [](const rope_t& value, const write_ctx_t&) -> admission_t {
        if (only_byte(value) == 0x71) return std::unexpected(status_t::TYPE_MISMATCH);
        return rope_t{make_value({0x99})};
    };
    const vertex_handle_t v = g.register_vertex(path_t("/s"), role_t::STREAM, std::move(h));

    log_t seen;
    auto on_value = [&seen](const rope_t& r) { seen.bytes.push_back(only_byte(r)); };
    check(g.subscribe(path_t("/s"), on_value).has_value(), "subscribe to the stream");

    check(g.write(v, byte_value(0x70)).has_value(), "the admitted stream write succeeds");
    check(seen.bytes.size() == 1 && seen.bytes[0] == 0x99,
          "the drain delivered the NORMALISED entry");
    const auto refused = g.write(v, byte_value(0x71));
    check(!refused && refused.error() == status_t::TYPE_MISMATCH, "the refused write says so");
    check(seen.bytes.size() == 1, "and queued nothing — the ring never saw it");
}

// --- 9. a fan-in delivery -------------------------------------------------------------------

/**
 * @brief Vector 9 — a delivery landing at a filtered target is refused, and counted as DENIED.
 *
 * A local-target edge delivers by WRITING the target, so it passes the target's filter like any
 * other write. Two things are pinned: the target keeps its prior value (the delivery really was
 * refused), and the drop is counted as a POLICY refusal — the same counter the fan-in ACL denial
 * uses — rather than as the memory event every store failure on this leg used to be.
 */
void test_fan_in_delivery_is_filtered_and_counted() {
    std::printf("vector 9 — a fan-in delivery meets the target's filter:\n");
    graph_t g;
    handlers_t h;
    h.on_admit = [](const rope_t& value, const write_ctx_t&) -> admission_t {
        if (only_byte(value) == 0x81) return std::unexpected(status_t::TYPE_MISMATCH);
        return std::nullopt;
    };
    const vertex_handle_t src = g.register_vertex(path_t("/src"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/sink"), role_t::STORED_VALUE, std::move(h));
    check(g.subscribe(path_t("/src"), path_t("/sink")).has_value(), "wire src -> sink");

    check(g.write(src, byte_value(0x80)).has_value(), "the admitted value flows through");
    check(stored_byte(g, path_t("/sink")) == 0x80, "and lands at the target");

    check(g.write(src, byte_value(0x81)).has_value(),
          "the source's own write still succeeds — the refusal is the TARGET's");
    check(stored_byte(g, path_t("/sink")) == 0x80, "the target kept its prior value");
    const auto d = g.delivery_drops();
    check(d.denied == 1, "the refused delivery is counted as a policy denial");
    check(d.out_of_memory == 0, "and NOT as a memory event — nothing was exhausted");
}

// --- 10-12. the app-field plane -------------------------------------------------------------

/**
 * @brief Vector 10 — an app-field filter that accepts stores the written bytes verbatim.
 *
 * The control for 11 and 12, and the compatibility statement: a filter that returns the view it
 * was handed reproduces the pre-seam behaviour exactly, apply seam included.
 */
void test_field_admission_accepts() {
    std::printf("vector 10 — the app-field filter accepts:\n");
    graph_t g;
    int admits = 0;
    int applied = 0;
    handlers_t h;
    h.on_app_field_admit = [&admits](std::string_view name,
                                     const view_t& value) -> tr::graph::result_t<view_t> {
        ++admits;
        check(name == "mode", "the filter is handed the field key below settings.app.");
        return value;
    };
    h.on_app_field_write = [&applied](std::string_view, const view_t&) { ++applied; };
    const vertex_handle_t v = g.register_vertex(path_t("/dev"), role_t::STORED_VALUE, std::move(h));
    std::vector<app_field_t> table;
    table.push_back(app_field_t{.name = "mode", .access = app_access_t::RW});
    g.set_app_fields(v, std::move(table));

    const std::vector<std::byte> eco = value_tlv("eco");
    check(g.write(path_t("/dev:settings.app.mode"), make_value(eco)).has_value(),
          "the admitted field write lands");
    check(admits == 1 && applied == 1, "filter then apply, once each");
    check(field_bytes(g, path_t("/dev:settings.app.mode")) == eco,
          "and the bytes stored are the bytes written");
}

/**
 * @brief Vector 11 — an app-field filter may NORMALISE the bytes that land.
 *
 * What is stored, what reads back and what the apply seam is handed are all the filter's bytes
 * — the last of the three being the one that would let an owner apply a value the graph does not
 * hold.
 */
void test_field_admission_normalises() {
    std::printf("vector 11 — the app-field filter normalises:\n");
    graph_t g;
    std::vector<std::byte> applied_bytes;
    handlers_t h;
    h.on_app_field_admit = [](std::string_view, const view_t&) -> tr::graph::result_t<view_t> {
        return make_value(value_tlv("ECO"));  // the canonical spelling, whatever was written
    };
    h.on_app_field_write = [&applied_bytes](std::string_view, const view_t& value) {
        const std::span<const std::byte> b = value.bytes();
        applied_bytes.assign(b.begin(), b.end());
    };
    const vertex_handle_t v = g.register_vertex(path_t("/dev"), role_t::STORED_VALUE, std::move(h));
    std::vector<app_field_t> table;
    table.push_back(app_field_t{.name = "mode", .access = app_access_t::RW});
    g.set_app_fields(v, std::move(table));

    check(g.write(path_t("/dev:settings.app.mode"), make_value(value_tlv("eco"))).has_value(),
          "the normalised field write lands");
    check(field_bytes(g, path_t("/dev:settings.app.mode")) == value_tlv("ECO"),
          "the NORMALISED bytes are what is stored");
    check(applied_bytes == value_tlv("ECO"),
          "and what the apply seam was handed — it applies what landed");
}

/**
 * @brief Vector 12 — an app-field filter may REFUSE, leaving the prior bytes and firing nothing.
 *
 * The field plane's half of the refusal contract. The apply seam must not fire: there is nothing
 * to apply, and an owner that restructured its children on a write the graph rejected would be
 * the exact inconsistency this seam exists to prevent.
 */
void test_field_admission_refuses() {
    std::printf("vector 12 — the app-field filter refuses:\n");
    graph_t g;
    int applied = 0;
    // The longest encoding this vertex will accept — a filter refusing by SIZE, which is the
    // shape of validation the descriptor table deliberately does not perform for the owner.
    const std::size_t limit = value_tlv("eco").size();
    handlers_t h;
    h.on_app_field_admit = [limit](std::string_view,
                                   const view_t& value) -> tr::graph::result_t<view_t> {
        if (value.bytes().size() > limit) return std::unexpected(status_t::TYPE_MISMATCH);
        return value;
    };
    h.on_app_field_write = [&applied](std::string_view, const view_t&) { ++applied; };
    const vertex_handle_t v = g.register_vertex(path_t("/dev"), role_t::STORED_VALUE, std::move(h));
    std::vector<app_field_t> table;
    table.push_back(app_field_t{.name = "mode", .access = app_access_t::RW});
    g.set_app_fields(v, std::move(table));

    const std::vector<std::byte> eco = value_tlv("eco");
    check(g.write(path_t("/dev:settings.app.mode"), make_value(eco)).has_value(),
          "the admitted field write lands");
    check(applied == 1, "and applied once");

    const auto refused =
        g.write(path_t("/dev:settings.app.mode"), make_value(value_tlv("turbo-mode")));
    check(!refused && refused.error() == status_t::TYPE_MISMATCH,
          "the refused field write answers the filter's status");
    check(field_bytes(g, path_t("/dev:settings.app.mode")) == eco,
          "the field keeps its PRIOR bytes");
    check(applied == 1, "and the apply seam never fired for the refused write");
}

}  // namespace

int main() {
    test_accept_as_written();
    test_normalise_replaces_the_stored_value();
    test_refusal_propagates_and_preserves_the_lkv();
    test_admission_precedes_store_and_delivery();
    test_no_filter_is_unchanged();
    test_assign_takes_the_same_seam();
    test_handler_role_takes_on_write_only();
    test_stream_ring_queues_the_admitted_value();
    test_fan_in_delivery_is_filtered_and_counted();
    test_field_admission_accepts();
    test_field_admission_normalises();
    test_field_admission_refuses();
    return tr::testing::summary("admission_seam");
}
