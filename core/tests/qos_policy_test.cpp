/**
 * @file
 * @brief RFC-0022 (as amended): delivery policy is per-subscription; `settings_t` dissolves.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The RFC's conformance sketches (§5) as host tests, plus the ablation each one needs to be
 * worth running. Amendment 1 replaced §3.B-§3.D before any implementation landed, so what is
 * tested here is the AMENDED shape: nothing per-vertex survives as QoS, and nothing is
 * inherited.
 *
 *   1. `subscriber/policy-absent`             — a SUBSCRIBER with no `SETTINGS` child behaves
 *                                               exactly as before, and the edge's stored bytes
 *                                               are byte-identical to the pre-RFC encoder's.
 *   2. `subscriber/policy-durability`         — `durability_request` set => the latched value is
 *                                               delivered on join; unset => it is NOT, on the
 *                                               SAME producer holding the SAME value.
 *   3. `subscriber/policy-reserved-bits`      — bits 6-15 are ignored, not an error, and the
 *                                               honoured bits below them still act.
 *   4. `settings/removed-knob`                — every one of the seven historical knob names
 *                                               answers SCHEMA_NOT_FOUND, the two survivors
 *                                               included; the app-field door beside them still
 *                                               works (the ablation).
 *   5. `settings/read-container-shape`        — `:settings` keeps its container and loses its
 *                                               knobs: `SETTINGS{ [NAME "app" SETTINGS{...}] }`,
 *                                               empty when no app fields are declared.
 *   6. `settings/schema-enumerates-nothing`   — `:schema` carries no protocol-knob entries.
 *   7. `stream/history-depth-host-only`       — `set_history_depth` changes the retained ring
 *                                               depth, and no wire operation reaches it.
 *
 * §5.8 `store/pin-ratio` is deliberately ABSENT: §3.D's amplification predicate is gated by §6
 * on a dual-target measurement that has not been run, so `store_ref_min_bytes` keeps the
 * absolute threshold it shipped with — rehomed owner-side, but otherwise untouched.
 */

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::delivery_policy_t;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::op_resolver_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::view::rope_t;
using tr::wire::opt_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief A view over a fresh owned heap segment holding @p bytes. */
tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

/** @brief A VALUE TLV holding @p n as a @p width-byte little-endian integer, owned. */
tr::view::view_t value_le(std::uint64_t n, std::size_t width) {
    std::vector<std::byte> payload(width);
    tr::detail::store_le(payload, n, width);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, payload);
    return make_value(out);
}

/** @brief A one-byte value, the ordinary write payload used to prove delivery still flows. */
tr::view::view_t byte_value(std::uint8_t b) {
    const std::array<std::byte, 1> one{std::byte{b}};
    return make_value(one);
}

template <class T>
bool fails_with(const tr::graph::result_t<T>& r, status_t s) {
    return !r.has_value() && r.error() == s;
}
bool fails_with(const tr::graph::result_t<void>& r, status_t s) {
    return !r.has_value() && r.error() == s;
}

/** @brief The vertex behind a handle — the bench-side `std::bit_cast` escape hatch, used here
 *         for the one fact no public surface exposes: whether an extension block EXISTS
 *         (`bench_qos_census` uses the same one, for the same reason). */
[[nodiscard]] bool has_ext(const vertex_handle_t& v) {
    return std::bit_cast<tr::graph::vertex_t*>(v)->has_extension_block();
}

/** @brief A flattened, DECODED read result that owns its bytes (the decoded spans point
 *         into @ref bytes, so the pair must travel together). */
struct decoded_t {
    std::vector<std::byte> bytes; /**< @brief The flattened TLV (owning copy). */
    tlv_t tlv;                    /**< @brief Decoded structure; spans point into @ref bytes. */
};

/** @brief The rope behind a read result, whichever surface produced it. */
inline const rope_t& deref_rope(const rope_t& r) noexcept { return r; }
/** @brief Overload for the reference form a value read returns. */
inline const rope_t& deref_rope(const tr::graph::value_ref_t& r) noexcept { return *r; }

/** @brief Flatten + decode a read result into an owning @ref decoded_t (nullopt on failure). */
template <class T>
std::optional<decoded_t> decode_read(const tr::graph::result_t<T>& r) {
    if (!r) return std::nullopt;
    const tr::view::view_t flat = deref_rope(*r).flatten();
    std::optional<decoded_t> out(std::in_place);
    const std::span<const std::byte> b = flat.bytes();
    out->bytes.assign(b.begin(), b.end());
    const auto dec = tr::wire::decode(out->bytes);
    if (!dec) return std::nullopt;
    out->tlv = *dec;
    return out;
}

/** @brief True iff @p t's child @p i is a NAME whose payload spells @p want. */
bool name_at(const tlv_t& t, std::size_t i, std::string_view want) {
    return i < t.children.size() && t.children[i].type == type_t::NAME &&
           tr::detail::as_string_view(t.children[i].payload) == want;
}

// ---------------------------------------------------------------------------
// §5.1 + §5.2 + §5.3 — the per-subscription delivery policy.

/**
 * @brief A default subscription behaves exactly as it did before RFC-0022.
 *
 * The ablation is the point: this asserts BOTH halves — no latch on join, and a later write
 * still delivered. A test that only checked "no latch" would pass against a subscribe that
 * silently failed to register at all.
 */
void test_policy_absent_is_todays_behaviour() {
    std::printf("§5.1 policy-absent — a default subscription is unchanged:\n");
    graph_t g;
    const vertex_handle_t src = g.register_vertex(path_t("/p/absent"), role_t::STORED_VALUE);
    (void)g.write(src, byte_value(0x11));  // an LKV EXISTS before the subscribe

    int seen = 0;
    auto sink = [&seen](const rope_t&) { ++seen; };
    const auto sub = g.subscribe(path_t("/p/absent"), sink);
    check(sub.has_value(), "a policy-less subscribe is admitted");
    check(seen == 0, "join delivers nothing (no durability request)");

    (void)g.write(src, byte_value(0x22));
    check(seen == 1, "... and a later write still delivers — the edge is live, not inert");
}

/**
 * @brief `durability_request` is the latch predicate, and it is PER SUBSCRIPTION.
 *
 * Both subscribers sit on the same producer, holding the same value, admitted through the
 * same door. Only the requesting one is replayed. Before RFC-0022 the producer's single
 * `settings.durability` flag decided this for both, so this pair could not be expressed.
 */
void test_policy_durability_is_per_subscriber() {
    std::printf("§5.2 policy-durability — the latch is the subscriber's request:\n");
    graph_t g;
    const vertex_handle_t src = g.register_vertex(path_t("/p/dur"), role_t::STORED_VALUE);
    (void)g.write(src, byte_value(0x5A));

    int latched = 0;
    std::uint8_t latched_byte = 0;
    auto on_latch = [&latched, &latched_byte](const rope_t& v) {
        ++latched;
        latched_byte = std::to_integer<std::uint8_t>(v.only().bytes()[0]);
    };
    const auto durable = g.subscribe(path_t("/p/dur"), on_latch,
                                     delivery_policy_t{delivery_policy_t::kDurabilityRequest});
    check(durable.has_value() && latched == 1, "durability_request => one delivery on join");
    check(latched_byte == 0x5A, "... carrying the producer's current value");

    int plain = 0;
    auto on_plain = [&plain](const rope_t&) { ++plain; };
    const auto volatile_sub = g.subscribe(path_t("/p/dur"), on_plain);
    check(volatile_sub.has_value() && plain == 0,
          "the SAME producer, no request => no delivery on join (the ablation)");

    (void)g.write(src, byte_value(0x77));
    check(latched == 2 && plain == 1, "both edges receive the later write — neither is inert");
}

/**
 * @brief Reserved bits 6–15 are ignored, never rejected — and the honoured bits still act.
 *
 * The second half is what makes this non-vacuous: a subscribe that failed outright would
 * also show "no error surfaced to the caller" if only the return value were checked.
 */
void test_policy_reserved_bits_are_ignored() {
    std::printf("§5.3 policy-reserved-bits — ignored, not an error:\n");
    graph_t g;
    const vertex_handle_t src = g.register_vertex(path_t("/p/rsvd"), role_t::STORED_VALUE);
    (void)g.write(src, byte_value(0x33));

    // Every reserved bit set, plus the durability request underneath them.
    constexpr std::uint16_t kReserved = 0xFFC0;
    int seen = 0;
    auto sink = [&seen](const rope_t&) { ++seen; };
    const auto sub = g.subscribe(path_t("/p/rsvd"), sink,
                                 delivery_policy_t{static_cast<std::uint16_t>(
                                     kReserved | delivery_policy_t::kDurabilityRequest)});
    check(sub.has_value(), "a policy with every reserved bit set is ADMITTED (not an error)");
    check(seen == 1, "... and the honoured durability bit beneath them still latches");

    // The accessors mask: reserved bits must not leak into a field's value.
    constexpr delivery_policy_t all_reserved{kReserved};
    check(all_reserved.reliability() == 0 && all_reserved.priority() == 0 &&
              !all_reserved.durability_request(),
          "reserved bits decode into NO honoured field");
    constexpr delivery_policy_t mixed{static_cast<std::uint16_t>(0xFFC0 | 0x0001 | (5U << 2))};
    check(mixed.reliability() == 1 && mixed.priority() == 5,
          "... while reliability and priority still decode from under them");
}

/** @brief The packed layout is exactly RFC-0022 §3.A's table — asserted at compile time so a
 *         re-pack cannot pass by accident. */
void test_policy_bit_layout() {
    std::printf("§3.A bit layout:\n");
    static_assert(sizeof(delivery_policy_t) == 2, "the delivery policy is two bytes");
    static_assert(delivery_policy_t{0x0001}.reliability() == 1, "bits 0-1 are reliability");
    static_assert(delivery_policy_t{0x0003}.reliability() == 3, "bits 0-1 are reliability");
    static_assert(delivery_policy_t{0x001C}.priority() == 7, "bits 2-4 are priority");
    static_assert(delivery_policy_t{0x0020}.durability_request(), "bit 5 is durability_request");
    static_assert(!delivery_policy_t{0x001F}.durability_request(), "bit 5 only");
    check(true, "the §3.A packing is pinned by static_assert (see the source)");
}

// ---------------------------------------------------------------------------
// §5.4 + §5.5 + §5.6 + §5.7 — the vertex settings surface, after `settings_t` dissolved.

/** @brief EVERY historical `:settings.<knob>` name answers SCHEMA_NOT_FOUND — the two
 *         survivors included, since they are owner-side state now, not knobs. */
void test_removed_knobs_are_schema_not_found() {
    std::printf("§5.4 removed-knob — the whole flat knob namespace is withdrawn:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/s/knobs"), role_t::STORED_VALUE);

    for (const char* knob : {"deadline_ns", "queue_max_bytes", "reliability", "priority",
                             "durability", "history_keep_last", "store_ref_min_bytes"}) {
        const std::string p = std::string("/s/knobs:settings.") + knob;
        const auto w = g.write(path_t(p), value_le(1, 8));
        check(fails_with(w, status_t::SCHEMA_NOT_FOUND),
              std::string("`:settings.") + knob + "` write => SCHEMA_NOT_FOUND");
        // The READ half of the same name, which was never implemented and still is not.
        check(fails_with(g.read(path_t(p)), status_t::SCHEMA_NOT_FOUND),
              std::string("`:settings.") + knob + "` read  => SCHEMA_NOT_FOUND");
    }

    // THE ABLATION. A loop that only ever expects SCHEMA_NOT_FOUND would pass just as well
    // against a `:settings` door deleted wholesale — app fields, `:acl`, everything. So the
    // reserved `app` subkey beside it must still take a write and serve it back.
    std::vector<tr::graph::app_field_t> table;
    table.push_back(tr::graph::app_field_t{.name = "kp", .access = tr::graph::app_access_t::RW});
    g.set_app_fields(v, std::move(table));
    check(g.write(path_t("/s/knobs:settings.app.kp"), value_le(7, 4)).has_value(),
          "ablation: `:settings.app.kp` still writes (the door is alive, not deleted)");
    check(g.read(path_t("/s/knobs:settings.app.kp")).has_value(), "ablation: ... and reads back");
}

/** @brief The bare `:settings` read KEEPS its container and LOSES its knobs (§4). */
void test_settings_container_keeps_its_shape() {
    std::printf("§5.5 read-container-shape — the container survives, the knobs do not:\n");
    graph_t g;
    const vertex_handle_t bare = g.register_vertex(path_t("/s/bare"), role_t::STORED_VALUE);
    (void)bare;

    // A vertex that declares nothing reads an EMPTY container — honest, not absent.
    const std::optional<decoded_t> empty = decode_read(g.read(path_t("/s/bare:settings")));
    check(empty.has_value() && empty->tlv.type == type_t::SETTINGS && empty->tlv.children.empty(),
          "a vertex with no app fields reads an EMPTY `SETTINGS{}` (not SCHEMA_NOT_FOUND)");

    // A vertex that DOES reads `SETTINGS{ NAME "app" SETTINGS{...} }` — and nothing before it.
    const vertex_handle_t v = g.register_vertex(path_t("/s/shape"), role_t::STORED_VALUE);
    std::vector<tr::graph::app_field_t> table;
    table.push_back(tr::graph::app_field_t{.name = "kp", .access = tr::graph::app_access_t::RW});
    g.set_app_fields(v, std::move(table));
    check(g.write(path_t("/s/shape:settings.app.kp"), value_le(7, 4)).has_value(),
          "the owner's app field takes a value");

    const std::optional<decoded_t> full = decode_read(g.read(path_t("/s/shape:settings")));
    check(
        full.has_value() && full->tlv.type == type_t::SETTINGS && full->tlv.children.size() == 2,
        ":settings is `SETTINGS{ NAME \"app\", SETTINGS }` — TWO children, no knobs ahead of them");
    check(full.has_value() && name_at(full->tlv, 0, "app"),
          "... whose first child is the reserved `app` subkey (RFC-0010 §A.4 survives)");
}

/** @brief `:schema`'s synthesized protocol part enumerates NOTHING, and is therefore complete. */
void test_schema_enumerates_nothing() {
    std::printf("§5.6 schema-enumerates-nothing — an empty knob enumeration:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/s/sch"), role_t::STORED_VALUE);

    const std::optional<decoded_t> schema = decode_read(g.read(path_t("/s/sch:schema")));
    check(
        schema.has_value() && schema->tlv.type == type_t::POINT && schema->tlv.children.size() == 2,
        ":schema is POINT{ NAME, SETTINGS } — no app table installed");
    const tlv_t* sset =
        (schema && schema->tlv.children.size() == 2) ? &schema->tlv.children[1] : nullptr;
    check(sset != nullptr && sset->type == type_t::SETTINGS && sset->children.empty(),
          ":schema's SETTINGS is EMPTY — zero protocol-knob entries");

    // The ablation: the owner part must still appear when a table IS installed, or "empty"
    // would be indistinguishable from "read_schema stopped emitting the settings part".
    const vertex_handle_t v = g.register_vertex(path_t("/s/sch2"), role_t::STORED_VALUE);
    std::vector<tr::graph::app_field_t> table;
    table.push_back(tr::graph::app_field_t{.name = "kp", .access = tr::graph::app_access_t::RW});
    g.set_app_fields(v, std::move(table));
    const std::optional<decoded_t> owned = decode_read(g.read(path_t("/s/sch2:schema")));
    check(owned.has_value() && owned->tlv.children.size() == 4 && name_at(owned->tlv, 2, "app"),
          "ablation: the OWNER part still follows it (POINT{ NAME, SETTINGS, NAME \"app\", "
          "SETTINGS })");
}

// ---------------------------------------------------------------------------
// §5.7 — the ring depth is owner-side, and nothing is inherited (§3.F).

/** @brief `set_history_depth` drives the ring; no wire operation reaches it. */
void test_history_depth_is_host_only() {
    std::printf("§5.7 history-depth-host-only — owner-side, no wire surface:\n");
    graph_t g;
    const vertex_handle_t stream = g.register_vertex(path_t("/h/s"), role_t::STREAM);

    // Default depth is 1: the ring keeps the last value only.
    for (std::uint8_t i = 1; i <= 4; ++i) (void)g.write(stream, byte_value(i));
    const auto shallow = g.history(stream);
    check(shallow.has_value() && shallow->size() == 1, "an undeclared ring keeps ONE entry");

    // The owner declares a depth; the STORE PATH must read the new value, not merely report it.
    g.set_history_depth(stream, 3);
    for (std::uint8_t i = 1; i <= 5; ++i) (void)g.write(stream, byte_value(i));
    const auto deep = g.history(stream);
    check(deep.has_value() && deep->size() == 3,
          "set_history_depth(3) => the ring TRIMS to three (the store path read it)");

    // And no wire operation reaches it — write or read, whatever the caller.
    check(fails_with(g.write(path_t("/h/s:settings.history_keep_last"), value_le(9, 4)),
                     status_t::SCHEMA_NOT_FOUND),
          "a `:settings.history_keep_last` write answers SCHEMA_NOT_FOUND");
    const auto after = g.history(stream);
    check(after.has_value() && after->size() == 3, "... and did not change the depth");
    const std::optional<decoded_t> settings = decode_read(g.read(path_t("/h/s:settings")));
    check(settings.has_value() && settings->tlv.children.empty(),
          "... nor does a bare `:settings` read carry the value");
}

/** @brief NOTHING is inherited (§3.F), and a registration can no longer force the cold block. */
void test_nothing_is_inherited() {
    std::printf("§3.F — no inheritance, and no registration-forced extension block:\n");
    graph_t g;
    const vertex_handle_t parent = g.register_vertex(path_t("/i/root"), role_t::STORED_VALUE);
    g.set_store_ref_min_bytes(parent, 256);
    g.set_history_depth(parent, 4);
    check(g.store_ref_min_bytes(parent) == 256, "the parent holds its own declared threshold");

    // A child registered UNDER it inherits nothing — the whole hole Amendment 1 closed.
    const vertex_handle_t child = g.register_vertex(path_t("/i/root/a"), role_t::STORED_VALUE);
    check(g.store_ref_min_bytes(child) == 0, "a child inherits NOTHING (§3.F)");
    check(!has_ext(child), "... and pays no extension block for the parent's declaration");

    // A child registered LATER, and one under a fresh intermediate placeholder, likewise.
    g.set_store_ref_min_bytes(parent, 512);
    const vertex_handle_t later = g.register_vertex(path_t("/i/root/b"), role_t::STORED_VALUE);
    const vertex_handle_t deep =
        g.register_vertex(path_t("/i/root/mid/deep"), role_t::STORED_VALUE);
    check(g.store_ref_min_bytes(later) == 0 && g.store_ref_min_bytes(deep) == 0,
          "a later child and a grandchild under a placeholder inherit nothing either");
    check(g.store_ref_min_bytes(parent) == 512,
          "... while the parent's own later declaration still lands on the parent");

    // The RAM half of §3.B: registration cannot force the cold block any more, because the
    // parameter that used to is gone. A STREAM or a handler still does — that is the ablation.
    const vertex_handle_t plain = g.register_vertex(path_t("/i/plain"), role_t::STORED_VALUE);
    check(!has_ext(plain), "a plain leaf allocates NO extension block");
    const vertex_handle_t stream = g.register_vertex(path_t("/i/stream"), role_t::STREAM);
    check(has_ext(stream),
          "ablation: a STREAM identity still allocates one (has_ext is not stuck)");
    // A declaration is what materialises it — the owner pays only when the owner asks.
    g.set_store_ref_min_bytes(plain, 8);
    check(has_ext(plain) && g.store_ref_min_bytes(plain) == 8,
          "an owner-side declaration materialises the block, and takes effect");
}

// ---------------------------------------------------------------------------
// The §5 vectors as BYTES: each one is fed to (or compared against) the reference
// implementation, so a vector that drifts from the code fails here rather than sitting
// on disk describing a protocol nobody implements.

/** @brief The raw bytes of a conformance vector's `input.bin`. */
std::vector<std::byte> vector_bytes(std::string_view case_dir) {
    const std::filesystem::path p =
        std::filesystem::path{LIBTRACER_VECTORS_DIR} / case_dir / "input.bin";
    std::ifstream f(p, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    return out;
}

/**
 * @brief Feed a SUBSCRIBER vector through the `:subscribers[]` field-write door — the WIRE
 *        door, byte for byte — and report how many deliveries the join produced.
 *
 * Every vector targets `/client`, so the deliveries land on a Handler vertex registered at
 * that path whose `on_write` counts them. The counter is file-scope because the seam is a
 * plain `std::function` the graph keeps, not something the caller's frame can outlive.
 * @return the delivery count, or -1 if the subscribe itself was refused.
 */
int g_client_writes = 0;
std::uint8_t g_client_last = 0;

/** @brief Register the `/client` Handler every SUBSCRIBER vector targets, counting the
 *         deliveries it receives and remembering the first byte of the last one. */
void register_client(graph_t& g) {
    g_client_writes = 0;
    g_client_last = 0;
    tr::graph::handlers_t h;
    h.on_write = [](const rope_t& v) -> tr::graph::result_t<void> {
        ++g_client_writes;
        const tr::view::view_t flat = v.flatten();
        const std::span<const std::byte> b = flat.bytes();
        if (!b.empty()) g_client_last = std::to_integer<std::uint8_t>(b[0]);
        return {};
    };
    (void)g.register_vertex(path_t("/client"), role_t::HANDLER, std::move(h));
}

/** @brief Append @p vector_case's bytes through the `:subscribers[]` door.
 *  @return true iff the field write was admitted. */
bool append_vector(graph_t& g, const path_t& producer, std::string_view vector_case) {
    tr::graph::field_path_t field;
    field.steps.push_back(
        tr::graph::field_step_t{.name = "subscribers", .indexed = true, .append = true});
    const std::optional<vertex_handle_t> v = g.find(producer.key());
    if (!v) return false;
    return g.write(*v, field, make_value(vector_bytes(vector_case))).has_value();
}

/** @brief Write @p vector_case's bytes AT `:subscribers[idx]` — the RFC-0009 §D.1 REPLACE
 *         door, which is a distinct arm from the append above.
 *  @return true iff the field write was admitted. */
bool replace_vector(graph_t& g, const path_t& producer, std::uint16_t idx,
                    std::string_view vector_case) {
    tr::graph::field_path_t field;
    field.steps.push_back(
        tr::graph::field_step_t{.name = "subscribers", .indexed = true, .index = idx});
    const std::optional<vertex_handle_t> v = g.find(producer.key());
    if (!v) return false;
    return g.write(*v, field, make_value(vector_bytes(vector_case))).has_value();
}

int latches_for(graph_t& g, const path_t& producer, std::string_view vector_case) {
    register_client(g);
    return append_vector(g, producer, vector_case) ? g_client_writes : -1;
}

/** @brief The §5 vectors, exercised against the implementation that must honour them. */
void test_conformance_vectors() {
    std::printf("§5 vectors — the bytes on disk, against the reference implementation:\n");

    // §5.6: read_schema on a default vertex named `temp` must emit the vector byte for byte.
    {
        graph_t g;
        (void)g.register_vertex(path_t("/temp"), role_t::STORED_VALUE);
        const std::optional<decoded_t> got = decode_read(g.read(path_t("/temp:schema")));
        const std::vector<std::byte> want = vector_bytes("settings/schema-enumerates-nothing");
        check(got.has_value() && got->bytes == want,
              "settings/schema-enumerates-nothing == read_schema's bytes, exactly");
    }

    // §5.5: the `:settings` container, in BOTH its shapes.
    {
        graph_t g;
        const vertex_handle_t v = g.register_vertex(path_t("/cs"), role_t::STORED_VALUE);
        const std::optional<decoded_t> bare = decode_read(g.read(path_t("/cs:settings")));
        const std::vector<std::byte> empty_want = {std::byte{0x0B}, std::byte{0x40},
                                                   std::byte{0x00}, std::byte{0x00}};
        check(bare.has_value() && bare->bytes == empty_want,
              "an app-field-less vertex serves the empty container `0B 40 00 00`");

        std::vector<tr::graph::app_field_t> table;
        table.push_back(
            tr::graph::app_field_t{.name = "kp", .access = tr::graph::app_access_t::RW});
        g.set_app_fields(v, std::move(table));
        (void)g.write(path_t("/cs:settings.app.kp"), value_le(7, 4));
        const std::optional<decoded_t> got = decode_read(g.read(path_t("/cs:settings")));
        const std::vector<std::byte> want = vector_bytes("settings/read-container-shape");
        check(got.has_value() && got->bytes == want,
              "settings/read-container-shape == the app-bearing container's bytes, exactly");
    }

    // The TWO-PART :schema vector (RFC-0010 §B.2). Nothing pinned it to the implementation
    // before, so it silently kept the pre-amendment protocol part; now a drift fails here.
    {
        graph_t g;
        const vertex_handle_t v = g.register_vertex(path_t("/temp"), role_t::STORED_VALUE);
        std::vector<tr::graph::app_field_t> table;
        std::vector<std::byte> desc;
        tr::wire::emit_name(desc, "dtype");
        tr::wire::emit_name(desc, "f32");
        table.push_back(tr::graph::app_field_t{
            .name = "kp", .access = tr::graph::app_access_t::RW, .descriptor = std::move(desc)});
        g.set_app_fields(v, std::move(table));
        const std::optional<decoded_t> got = decode_read(g.read(path_t("/temp:schema")));
        const std::vector<std::byte> want = vector_bytes("tlv-types/point-schema-app");
        check(got.has_value() && got->bytes == want,
              "tlv-types/point-schema-app == read_schema's bytes with an app table, exactly");
    }

    // §5.7: the ring-depth vector names the error the removed knob actually answers with.
    {
        const std::vector<std::byte> bytes = vector_bytes("stream/history-depth-host-only");
        const auto dec = tr::wire::decode(bytes);
        check(dec.has_value() && dec->type == type_t::ERROR && !dec->children.empty() &&
                  tr::detail::load_le<std::uint16_t>(dec->children[0].payload) ==
                      static_cast<std::uint16_t>(tr::wire::err_t::SCHEMA_NOT_FOUND),
              "stream/history-depth-host-only carries tr::schema::not_found (0x0031)");
        graph_t g;
        const vertex_handle_t s = g.register_vertex(path_t("/hd"), role_t::STREAM);
        check(fails_with(g.write(path_t("/hd:settings.history_keep_last"), value_le(4, 4)),
                         status_t::SCHEMA_NOT_FOUND),
              "... which is the status the ring-depth write returns");
        g.set_history_depth(s, 2);
        for (std::uint8_t i = 1; i <= 3; ++i) (void)g.write(s, byte_value(i));
        const auto hist = g.history(s);
        check(hist.has_value() && hist->size() == 2,
              "... while the HOST-side declaration does take effect (the vector's other half)");
    }

    // §5.4: the vector names the error the write actually answers with.
    {
        const std::vector<std::byte> bytes = vector_bytes("settings/removed-knob");
        const auto dec = tr::wire::decode(bytes);
        check(dec.has_value() && dec->type == type_t::ERROR && !dec->children.empty() &&
                  tr::detail::load_le<std::uint16_t>(dec->children[0].payload) ==
                      static_cast<std::uint16_t>(tr::wire::err_t::SCHEMA_NOT_FOUND),
              "settings/removed-knob carries tr::schema::not_found (0x0031)");
        graph_t g;
        (void)g.register_vertex(path_t("/rk"), role_t::STORED_VALUE);
        check(fails_with(g.write(path_t("/rk:settings.deadline_ns"), value_le(1, 8)),
                         status_t::SCHEMA_NOT_FOUND),
              "... which is the status the removed-knob write returns");
    }

    // §5.1 / §5.2 / §5.3: the three SUBSCRIBER vectors, through the wire door.
    {
        graph_t g;
        const auto src = g.register_vertex(path_t("/vec/src"), role_t::STORED_VALUE);
        (void)g.write(src, byte_value(0x5A));
        check(latches_for(g, path_t("/vec/src"), "subscriber/policy-absent") == 0,
              "subscriber/policy-absent: admitted, and NO latch");
    }
    {
        graph_t g;
        const auto src = g.register_vertex(path_t("/vec/src"), role_t::STORED_VALUE);
        (void)g.write(src, byte_value(0x5A));
        check(latches_for(g, path_t("/vec/src"), "subscriber/policy-durability") == 1,
              "subscriber/policy-durability: admitted, and exactly one latch");
    }
    {
        graph_t g;
        const auto src = g.register_vertex(path_t("/vec/src"), role_t::STORED_VALUE);
        (void)g.write(src, byte_value(0x5A));
        check(latches_for(g, path_t("/vec/src"), "subscriber/policy-reserved-bits") == 0,
              "subscriber/policy-reserved-bits: admitted (not rejected), bit 5 clear => no latch");
    }
}

/**
 * @brief The RFC-0009 §D.1 REPLACE door honours the REPLACING subscriber's durability
 *        request — the latch arm no append test can reach.
 *
 * `vertex_t::replace_edge` snapshots the latch under the same single lock hold as
 * `add_edge`, predicated on the REPLACING edge's `policy.durability_request()`. Nothing
 * exercised that predicate: every policy case above enters through `[]` (append), which is
 * `add_edge`'s arm, so forcing the replace arm's predicate to `false` left the whole suite
 * green. This drives the WIRE door in both halves — a policy-less SUBSCRIBER appended into
 * slot 0, then a durability-requesting SUBSCRIBER written AT `:subscribers[0]` — and asserts
 * the latched value is delivered on join.
 *
 * Both bytes come from the conformance vectors, so the door is fed exactly what a peer sends.
 */
void test_replace_door_latches() {
    std::printf("§D.1 replace door — the REPLACING subscriber's request is honoured:\n");
    graph_t g;
    const vertex_handle_t src = g.register_vertex(path_t("/rep/src"), role_t::STORED_VALUE);
    (void)g.write(src, byte_value(0x5A));  // the producer's LKV, held before either subscribe
    register_client(g);

    // Slot 0 arrives through `[]` carrying NO policy — nothing is latched.
    check(append_vector(g, path_t("/rep/src"), "subscriber/policy-absent"),
          "a policy-less SUBSCRIBER is appended into slot 0");
    check(g_client_writes == 0, "... and its join latches nothing");

    // The §D.1 REPLACE: the SAME slot, a subscriber that DOES request durability.
    check(replace_vector(g, path_t("/rep/src"), 0, "subscriber/policy-durability"),
          "`:subscribers[0]` takes a SUBSCRIBER — a replace, not a clear");
    check(g_client_writes == 1,
          "the REPLACING subscriber's durability request latches on join (replace_edge's arm)");
    check(g_client_last == 0x5A, "... delivering the producer's CURRENT value");

    // The other sign, on the same door: a replace by a policy-LESS subscriber latches
    // nothing. Without this the count above would also pass against a replace arm that
    // latched unconditionally — it would be measuring "a replace happened", not the predicate.
    check(replace_vector(g, path_t("/rep/src"), 0, "subscriber/policy-absent"),
          "ablation: `:subscribers[0]` takes a policy-less SUBSCRIBER too");
    check(g_client_writes == 1, "... and THAT replace latches nothing — the predicate is READ");

    // And the replacing edge is live, not inert: the next producer write reaches it once.
    (void)g.write(src, byte_value(0x77));
    check(g_client_writes == 2 && g_client_last == 0x77,
          "the surviving edge takes the next write — the replace left exactly ONE live listener");
}

// ---------------------------------------------------------------------------
// The two ERROR vectors, against the reply the RESOLVER actually assembles.

/** @brief A PATH TLV over @p segs, built through the production emit helpers. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

/** @brief A FIELD selector TLV whose steps are the dotted NAMEs @p steps. */
std::vector<std::byte> b_field(std::initializer_list<std::string_view> steps) {
    std::vector<std::byte> body;
    for (std::string_view s : steps) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}

/** @brief A FWD frame in RFC-0004 §B child order: op, dst, [selector], src, [payload]. */
std::vector<std::byte> b_fwd(fwd_op_t op, const std::vector<std::byte>& dst,
                             const std::vector<std::byte>& selector,
                             const std::vector<std::byte>& payload) {
    std::vector<std::byte> body;
    const std::byte opb{static_cast<std::uint8_t>(op)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&opb, 1));
    body.insert(body.end(), dst.begin(), dst.end());
    body.insert(body.end(), selector.begin(), selector.end());
    const std::vector<std::byte> src = b_path({"reply-ep"});
    body.insert(body.end(), src.begin(), src.end());
    body.insert(body.end(), payload.begin(), payload.end());
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

/**
 * @brief Resolve @p frame and re-encode the ERROR child of the reply's STATUS — the exact
 *        bytes a peer receives inside the `kind=ERROR` reply.
 * @return the ERROR TLV bytes, or an empty vector if the reply was not a well-formed error.
 */
std::vector<std::byte> error_child_bytes(op_resolver_t& resolver,
                                         std::span<const std::byte> frame) {
    const auto arena = tr::wire::decode_into(frame, tr::mem::heap_source());
    if (!arena) return {};
    const auto reply = resolver.resolve(*arena, {});
    if (!reply) return {};
    const tr::view::view_t flat = reply->flatten();
    const auto dec = tr::wire::decode(flat.bytes());
    if (!dec || dec->children.size() < 5) return {};
    if (dec->children[3].type != type_t::VALUE || dec->children[3].payload.size() != 1) return {};
    if (std::to_integer<std::uint8_t>(dec->children[3].payload[0]) !=
        static_cast<std::uint8_t>(reply_kind_t::ERROR))
        return {};
    const tlv_t& status = dec->children[4];
    if (status.type != type_t::STATUS || status.children.size() != 1) return {};
    return tr::wire::encode(status.children[0]);
}

/**
 * @brief The `settings/removed-knob` and `stream/history-depth-host-only` vectors are the
 *        bytes the RESOLVER builds — not a shape a document declared.
 *
 * Both vectors used to be hand-written `ERROR{VALUE, DESCRIPTION}` frames, and no code path
 * in the core produced them: `assemble_error` emits `ERROR{VALUE}` and `type_t::DESCRIPTION`
 * has no producer at all. Nothing caught that, because the codec round-trip a harness runs
 * (`encode(decode(input.bin)) == input.bin`) is satisfied by any well-formed TLV, invented or
 * not. So the claim is made HERE, where it can be false: drive the real
 * `FWD{WRITE, :settings.<knob>}` through `op_resolver_t` and byte-compare the reply's ERROR
 * child. This is also the `status_t` → `err_t` mapping gate — the vector now pins the mapped
 * code, not just a hand-typed constant.
 */
void test_removed_knob_reply_bytes() {
    std::printf("§5.4 / §5.7 — the ERROR vectors are the bytes the RESOLVER assembles:\n");
    graph_t g;
    op_resolver_t resolver(g);
    (void)g.register_vertex(path_t("/rk"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/hd"), role_t::STREAM);

    const std::vector<std::byte> one = {std::byte{0x01}};
    std::vector<std::byte> payload;
    tr::wire::emit_tlv(payload, type_t::VALUE, opt_t{}, one);

    // §5.4: a WRITE of a removed knob.
    const std::vector<std::byte> knob_err = error_child_bytes(
        resolver,
        b_fwd(fwd_op_t::WRITE, b_path({"rk"}), b_field({"settings", "deadline_ns"}), payload));
    check(knob_err == vector_bytes("settings/removed-knob"),
          "settings/removed-knob == the ERROR child of the reply a `:settings.deadline_ns` "
          "WRITE gets, exactly");

    // §5.7: the ring depth, on BOTH halves — the vector's own claim is "read OR write".
    const std::vector<std::byte> depth_w =
        error_child_bytes(resolver, b_fwd(fwd_op_t::WRITE, b_path({"hd"}),
                                          b_field({"settings", "history_keep_last"}), payload));
    check(depth_w == vector_bytes("stream/history-depth-host-only"),
          "stream/history-depth-host-only == the WRITE reply's ERROR child, exactly");
    const std::vector<std::byte> depth_r = error_child_bytes(
        resolver,
        b_fwd(fwd_op_t::READ, b_path({"hd"}), b_field({"settings", "history_keep_last"}), {}));
    check(depth_r == vector_bytes("stream/history-depth-host-only"),
          "... and the READ reply's ERROR child is the SAME bytes (the §3.B read/write "
          "agreement, over the wire)");

    // THE ABLATION. A test that only ever compared against one constant would pass against a
    // resolver that answered every request with that constant. A name that IS served must
    // come back as a RESULT, not as this error.
    const std::vector<std::byte> served = error_child_bytes(
        resolver, b_fwd(fwd_op_t::READ, b_path({"rk"}), b_field({"settings"}), {}));
    check(served.empty(),
          "ablation: a bare `:settings` READ is NOT an error reply (the resolver still serves)");
}

}  // namespace

/** @brief Run the RFC-0022 conformance sketches. */
int main() {
    test_policy_absent_is_todays_behaviour();
    test_policy_durability_is_per_subscriber();
    test_policy_reserved_bits_are_ignored();
    test_policy_bit_layout();
    test_removed_knobs_are_schema_not_found();
    test_settings_container_keeps_its_shape();
    test_schema_enumerates_nothing();
    test_history_depth_is_host_only();
    test_nothing_is_inherited();
    test_conformance_vectors();
    test_replace_door_latches();
    test_removed_knob_reply_bytes();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
