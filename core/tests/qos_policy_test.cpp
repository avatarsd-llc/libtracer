/**
 * @file
 * @brief RFC-0022: delivery policy is per-subscription; the vertex keeps only storage.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The RFC's six conformance sketches (§5) as host tests, plus the ablation each one needs
 * to be worth running:
 *
 *   1. `subscriber/policy-absent`            — a SUBSCRIBER with no `SETTINGS` child behaves
 *                                              exactly as before, and the edge's stored bytes
 *                                              are byte-identical to the pre-RFC encoder's.
 *   2. `subscriber/policy-durability`        — `durability_request` set ⇒ the latched value is
 *                                              delivered on join; unset ⇒ it is NOT, on the
 *                                              SAME producer holding the SAME value.
 *   3. `subscriber/policy-reserved-bits`     — bits 6–15 are ignored, not an error, and the
 *                                              honoured bits below them still act.
 *   4. `settings/removed-knob`               — every knob RFC-0022 removed answers
 *                                              SCHEMA_NOT_FOUND, on the write AND with the
 *                                              two survivors still accepted beside them.
 *   5. `settings/schema-enumerates-storage`  — `:schema` and `:settings` enumerate EXACTLY the
 *                                              two storage knobs, in that order.
 *   6. `settings/inherit-storage`            — §3.C copy-at-registration: a child inherits by
 *                                              value, a later-registered child likewise, an
 *                                              overriding child stops inheriting (and its own
 *                                              descendants inherit from IT), and a vertex at
 *                                              the defaults still allocates NO extension block.
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
using tr::graph::graph_t;
using tr::graph::kDefaultSettings;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::settings_t;
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
 *         for the one fact no public surface exposes: whether an extension block EXISTS.
 *         `settings()` returns `kDefaultSettings` BY ADDRESS when it does not (#361 §1). */
[[nodiscard]] bool has_ext(const vertex_handle_t& v) {
    return &std::bit_cast<tr::graph::vertex_t*>(v)->settings() != &kDefaultSettings;
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

/** @brief True iff @p t's child @p i is a VALUE whose little-endian payload is @p want. */
bool value_at(const tlv_t& t, std::size_t i, std::uint64_t want) {
    return i < t.children.size() && t.children[i].type == type_t::VALUE &&
           tr::detail::load_le(t.children[i].payload) == want;
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
// §5.4 + §5.5 — the vertex settings surface.

/** @brief Every knob RFC-0022 removed answers SCHEMA_NOT_FOUND; the two survivors still land. */
void test_removed_knobs_are_schema_not_found() {
    std::printf("§5.4 removed-knob — the four inert knobs and durability are gone:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/s/knobs"), role_t::STORED_VALUE);

    for (const char* knob :
         {"deadline_ns", "queue_max_bytes", "reliability", "priority", "durability"}) {
        const std::string p = std::string("/s/knobs:settings.") + knob;
        const auto w = g.write(path_t(p), value_le(1, 8));
        check(fails_with(w, status_t::SCHEMA_NOT_FOUND),
              std::string("`:settings.") + knob + "` write => SCHEMA_NOT_FOUND");
    }

    // The ablation: the same door, the same caller, the two knobs that SURVIVED still land.
    // Without this the loop above would pass just as well against a settings write surface
    // that had been removed wholesale.
    check(g.write(path_t("/s/knobs:settings.history_keep_last"), value_le(9, 4)).has_value() &&
              g.settings(v).history_keep_last == 9,
          "`:settings.history_keep_last` still writes (the surface is not simply closed)");
    check(g.write(path_t("/s/knobs:settings.store_ref_min_bytes"), value_le(64, 4)).has_value() &&
              g.settings(v).store_ref_min_bytes == 64,
          "`:settings.store_ref_min_bytes` still writes");
}

/** @brief `:schema` and `:settings` enumerate EXACTLY the storage knobs. */
void test_schema_enumerates_storage_only() {
    std::printf("§5.5 schema-enumerates-storage — exactly two knobs, no more:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/s/shape"), role_t::STORED_VALUE);
    (void)g.write(path_t("/s/shape:settings.history_keep_last"), value_le(4, 4));
    (void)g.write(path_t("/s/shape:settings.store_ref_min_bytes"), value_le(128, 4));
    (void)v;

    const std::optional<decoded_t> schema = decode_read(g.read(path_t("/s/shape:schema")));
    check(
        schema.has_value() && schema->tlv.type == type_t::POINT && schema->tlv.children.size() == 2,
        ":schema is POINT{ NAME, SETTINGS } — no app table installed");
    const tlv_t* sset =
        (schema && schema->tlv.children.size() == 2) ? &schema->tlv.children[1] : nullptr;
    check(sset != nullptr && sset->type == type_t::SETTINGS && sset->children.size() == 4,
          ":schema's SETTINGS holds exactly 2 knobs (4 children: NAME/VALUE pairs)");
    check(sset != nullptr && name_at(*sset, 0, "history_keep_last") && value_at(*sset, 1, 4) &&
              name_at(*sset, 2, "store_ref_min_bytes") && value_at(*sset, 3, 128),
          ":schema names history_keep_last then store_ref_min_bytes, with their live values");

    const std::optional<decoded_t> settings = decode_read(g.read(path_t("/s/shape:settings")));
    check(settings.has_value() && settings->tlv.type == type_t::SETTINGS &&
              settings->tlv.children.size() == 4,
          ":settings serves the same two knobs (4 children)");
    check(settings.has_value() && name_at(settings->tlv, 0, "history_keep_last") &&
              value_at(settings->tlv, 1, 4) && name_at(settings->tlv, 2, "store_ref_min_bytes") &&
              value_at(settings->tlv, 3, 128),
          ":settings and :schema enumerate the identical set — read and write gate agree");
}

// ---------------------------------------------------------------------------
// §5.6 — storage-policy inheritance (§3.C).

/** @brief Copy-at-registration inheritance, its overrides, and the no-ext default. */
void test_storage_policy_inheritance() {
    std::printf("§5.6 inherit-storage — copy at registration, override by subtree:\n");
    graph_t g;

    // A default vertex allocates NO extension block. This is the invariant §3.B promises to
    // preserve, and the reason the inherited value must be a plain `settings_t{}` when the
    // ancestry is at the defaults.
    const vertex_handle_t plain = g.register_vertex(path_t("/i/plain"), role_t::STORED_VALUE);
    check(!has_ext(plain), "a default vertex allocates NO extension block");
    const vertex_handle_t plain_kid =
        g.register_vertex(path_t("/i/plain/kid"), role_t::STORED_VALUE);
    check(!has_ext(plain_kid), "... and neither does its child (inheriting the defaults is free)");

    settings_t policy;
    policy.store_ref_min_bytes = 256;
    policy.history_keep_last = 4;
    const vertex_handle_t parent =
        g.register_vertex(path_t("/i/root"), role_t::STORED_VALUE, {}, policy);
    check(g.settings(parent).store_ref_min_bytes == 256, "the overriding parent holds its policy");

    // A child registered AFTER the parent's override inherits BY VALUE.
    const vertex_handle_t child = g.register_vertex(path_t("/i/root/a"), role_t::STORED_VALUE);
    check(g.settings(child).store_ref_min_bytes == 256 && g.settings(child).history_keep_last == 4,
          "a child inherits the parent's storage policy by value");
    check(has_ext(child), "... which materialises its extension block (the RAM an override buys)");

    // A LATER child inherits identically — inheritance is not a one-shot at first descent.
    const vertex_handle_t later = g.register_vertex(path_t("/i/root/b"), role_t::STORED_VALUE);
    check(g.settings(later).store_ref_min_bytes == 256,
          "a later-registered child inherits likewise");

    // A grandchild under an intermediate PLACEHOLDER inherits too: the placeholder carried
    // the value down. Without that, a deep registration would silently drop to the defaults.
    const vertex_handle_t deep =
        g.register_vertex(path_t("/i/root/mid/deep"), role_t::STORED_VALUE);
    check(g.settings(deep).store_ref_min_bytes == 256,
          "a grandchild under a fresh intermediate inherits (the placeholder carries it)");

    // An OVERRIDING child stops inheriting, and its own subtree inherits from IT.
    settings_t own;
    own.store_ref_min_bytes = 32;
    const vertex_handle_t overrider =
        g.register_vertex(path_t("/i/root/c"), role_t::STORED_VALUE, {}, own);
    check(g.settings(overrider).store_ref_min_bytes == 32, "an overriding child keeps its own");
    const vertex_handle_t under = g.register_vertex(path_t("/i/root/c/x"), role_t::STORED_VALUE);
    check(g.settings(under).store_ref_min_bytes == 32,
          "... and ITS children inherit the override, not the grandparent's");

    // A `:settings` write is an override too: it reaches the descendants that were still
    // carrying the old value, and stops at the one that has its own.
    check(g.write(path_t("/i/root:settings.store_ref_min_bytes"), value_le(512, 4)).has_value(),
          "the parent overrides its threshold through the `:settings` door");
    check(g.settings(parent).store_ref_min_bytes == 512, "the written vertex takes the new value");
    check(g.settings(child).store_ref_min_bytes == 512 &&
              g.settings(later).store_ref_min_bytes == 512 &&
              g.settings(deep).store_ref_min_bytes == 512,
          "every INHERITING descendant follows it");
    check(g.settings(overrider).store_ref_min_bytes == 32 &&
              g.settings(under).store_ref_min_bytes == 32,
          "the overriding subtree does NOT follow it (the ablation: the push prunes)");

    // And an unrelated default subtree is untouched — the override grows only what opted in.
    check(!has_ext(plain) && !has_ext(plain_kid),
          "an unrelated default subtree gains no extension block");
}

/**
 * @brief The write path still reads `store_ref_min_bytes` off ONE inline load, and the store
 *        path `history_keep_last` — asserted behaviourally, since the cost claim is the bench's.
 *
 * What is checkable here is that the value a hot read sees is the vertex's own copy, not
 * something resolved by walking: mutate an ancestor's ext AFTER the child exists and the
 * child's own load is what changes (the push wrote it), never a walk that would have found it
 * regardless of whether the push ran.
 */
void test_hot_reads_see_the_local_copy() {
    std::printf("§3.C — the hot readers see a LOCAL copy, not an ancestor walk:\n");
    graph_t g;
    settings_t policy;
    policy.history_keep_last = 3;
    (void)g.register_vertex(path_t("/h/root"), role_t::STORED_VALUE, {}, policy);
    const vertex_handle_t stream = g.register_vertex(path_t("/h/root/s"), role_t::STREAM);
    check(g.settings(stream).history_keep_last == 3, "the STREAM child inherited the ring depth");
    for (std::uint8_t i = 1; i <= 5; ++i) (void)g.write(stream, byte_value(i));
    const auto hist = g.history(stream);
    check(hist.has_value() && hist->size() == 3,
          "... and the ring TRIMMED to it — the inherited value is the one the store path read");
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

int latches_for(graph_t& g, const path_t& producer, std::string_view vector_case) {
    g_client_writes = 0;
    tr::graph::handlers_t h;
    h.on_write = [](const rope_t&) -> tr::graph::result_t<void> {
        ++g_client_writes;
        return {};
    };
    (void)g.register_vertex(path_t("/client"), role_t::HANDLER, std::move(h));
    tr::graph::field_path_t field;
    field.steps.push_back(
        tr::graph::field_step_t{.name = "subscribers", .indexed = true, .append = true});
    const std::optional<vertex_handle_t> v = g.find(producer.key());
    if (!v) return -1;
    const auto w = g.write(*v, field, make_value(vector_bytes(vector_case)));
    return w.has_value() ? g_client_writes : -1;
}

/** @brief The §5 vectors, exercised against the implementation that must honour them. */
void test_conformance_vectors() {
    std::printf("§5 vectors — the bytes on disk, against the reference implementation:\n");

    // §5.5: read_schema on a default vertex named `temp` must emit the vector byte for byte.
    {
        graph_t g;
        (void)g.register_vertex(path_t("/temp"), role_t::STORED_VALUE);
        const std::optional<decoded_t> got = decode_read(g.read(path_t("/temp:schema")));
        const std::vector<std::byte> want = vector_bytes("settings/schema-enumerates-storage");
        check(got.has_value() && got->bytes == want,
              "settings/schema-enumerates-storage == read_schema's bytes, exactly");
    }

    // §5.6: the `:settings` an INHERITING child serves must equal the vector.
    {
        graph_t g;
        settings_t policy;
        policy.history_keep_last = 4;
        policy.store_ref_min_bytes = 256;
        (void)g.register_vertex(path_t("/inh"), role_t::STORED_VALUE, {}, policy);
        (void)g.register_vertex(path_t("/inh/kid"), role_t::STORED_VALUE);
        const std::optional<decoded_t> got = decode_read(g.read(path_t("/inh/kid:settings")));
        const std::vector<std::byte> want = vector_bytes("settings/inherit-storage");
        check(got.has_value() && got->bytes == want,
              "settings/inherit-storage == the inheriting child's :settings bytes, exactly");
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

}  // namespace

/** @brief Run the RFC-0022 conformance sketches. */
int main() {
    test_policy_absent_is_todays_behaviour();
    test_policy_durability_is_per_subscriber();
    test_policy_reserved_bits_are_ignored();
    test_policy_bit_layout();
    test_removed_knobs_are_schema_not_found();
    test_schema_enumerates_storage_only();
    test_storage_policy_inheritance();
    test_hot_reads_see_the_local_copy();
    test_conformance_vectors();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
