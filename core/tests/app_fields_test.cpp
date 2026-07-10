/**
 * @file
 * @brief RFC-0010 owner app fields: the field descriptor table, `:settings.app.*`
 *        read/write gating, the two-part `:schema`, container reads, the owner apply
 *        seam, and the announce-write convention (§C — a field write never wakes
 *        `await` and never propagates).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Mirrors the RFC's conformance-vector sketches 1–7 as host tests (the declaration
 * surface is a local host API, so the behavior pairs — owner vs caller-attributed —
 * are exercised through graph_t directly; the remote FWD terminus reaches the same
 * field_write/read doors with the inbound link as the caller, covered by acl_test's
 * remote-path pattern).
 */

#include <chrono>
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

#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::acl_right_t;
using tr::graph::app_access_t;
using tr::graph::app_field_t;
using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::subject_token_t;
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

tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief A VALUE TLV over @p payload bytes — the shape an app-field write carries. */
std::vector<std::byte> value_tlv(std::string_view payload) {
    std::vector<std::byte> out;
    const std::vector<std::byte> p = as_bytes(payload);
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}

/** @brief Flatten a read result and compare against the exact expected bytes. */
bool reads_back(const tr::graph::result_t<rope_t>& r, std::span<const std::byte> expect) {
    if (!r) return false;
    const tr::view::view_t flat = r->flatten();
    const std::span<const std::byte> got = flat.bytes();
    return got.size() == expect.size() && std::equal(got.begin(), got.end(), expect.begin());
}

bool fails_with(const tr::graph::result_t<rope_t>& r, status_t s) {
    return !r.has_value() && r.error() == s;
}
bool fails_with(const tr::graph::result_t<void>& r, status_t s) {
    return !r.has_value() && r.error() == s;
}

/** @brief The test resolver (ADR-0018): a non-empty caller context resolves to its own
 *         bytes as the subject token; an empty (local/owner) context is trusted. */
std::optional<subject_token_t> caller_is_subject(std::string_view caller) {
    if (caller.empty()) return std::nullopt;
    return as_bytes(caller);
}

constexpr std::uint32_t bit(acl_right_t r) { return static_cast<std::uint32_t>(r); }

/** @brief An ALLOW ACE list granting @p mask to @p subject, encoded as `:acl` bytes. */
std::vector<std::byte> allow_acl(std::string_view subject, std::uint32_t mask) {
    const std::vector<tr::graph::ace_t> aces{
        tr::graph::ace_t{.subject = as_bytes(subject), .access_mask = mask}};
    return tr::graph::encode_acl(aces);
}

/** @brief The §B.1 descriptor-record members for tests: `NAME "dtype" NAME <tag>`. */
std::vector<std::byte> dtype_desc(std::string_view tag) {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, "dtype");
    tr::wire::emit_name(out, tag);
    return out;
}

/** @brief Decoded child at @p idx is a NAME with payload @p text. */
bool name_at(const tlv_t& parent, std::size_t idx, std::string_view text) {
    if (idx >= parent.children.size()) return false;
    const tlv_t& c = parent.children[idx];
    return c.type == type_t::NAME &&
           tr::detail::as_string_view(c.payload) == text;
}

/** @brief A decoded read result that OWNS its bytes — `tlv_t` payloads are spans into
 *         the decoded buffer, so the buffer must outlive every structural assertion
 *         (a move keeps the vector's heap block, so returning by value is safe). */
struct decoded_t {
    std::vector<std::byte> bytes; /**< @brief The flattened TLV (owning copy). */
    tlv_t tlv;                    /**< @brief Decoded structure; spans point into @ref bytes. */
};

/** @brief Flatten + decode a read result into an owning @ref decoded_t (nullopt on failure). */
std::optional<decoded_t> decode_read(const tr::graph::result_t<rope_t>& r) {
    if (!r) return std::nullopt;
    const tr::view::view_t flat = r->flatten();
    std::optional<decoded_t> out(std::in_place);
    const std::span<const std::byte> b = flat.bytes();
    out->bytes.assign(b.begin(), b.end());
    const auto dec = tr::wire::decode(out->bytes);
    if (!dec) return std::nullopt;
    out->tlv = *dec;
    return out;
}

// ---------------------------------------------------------------------------
// RFC-0010 sketch 1 — declare + read + write, initial value, verbatim bytes.
void test_declare_read_write() {
    std::printf("declare / read / write (RFC-0010 sketch 1):\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/ctrl/pid"), role_t::STORED_VALUE);

    const std::vector<std::byte> initial = value_tlv("kp0");
    std::vector<app_field_t> table;
    table.push_back(app_field_t{.name = "kp",
                                .access = app_access_t::RW,
                                .descriptor = dtype_desc("f32"),
                                .value = initial});
    g.set_app_fields(v, std::move(table));

    check(reads_back(g.read(path_t("/ctrl/pid:settings.app.kp")), initial),
          "initial value reads back verbatim");

    const std::vector<std::byte> updated = value_tlv("kp1");
    check(g.write(path_t("/ctrl/pid:settings.app.kp"), make_value(updated)).has_value(),
          "owner (local) write to the declared field succeeds");
    check(reads_back(g.read(path_t("/ctrl/pid:settings.app.kp")), updated),
          "written bytes read back verbatim");

    // The handle+field form (the FWD-terminus door) serves the identical bytes.
    const path_t p("/ctrl/pid:settings.app.kp");
    check(reads_back(g.read(v, p.field()), updated), "handle-form field read: same bytes");
}

// ---------------------------------------------------------------------------
// RFC-0010 sketch 2 — undeclared stays ENOTTY, before and after the table lands.
void test_undeclared_enotty() {
    std::printf("undeclared stays ENOTTY (RFC-0010 sketch 2):\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/dev/x"), role_t::STORED_VALUE);
    (void)v;
    const std::vector<std::byte> val = value_tlv("v");

    check(fails_with(g.write(path_t("/dev/x:settings.app.kp"), make_value(val)),
                     status_t::SCHEMA_NOT_FOUND),
          "app-field write before any table: SCHEMA_NOT_FOUND");
    check(fails_with(g.read(path_t("/dev/x:settings.app.kp")), status_t::SCHEMA_NOT_FOUND),
          "app-field read before any table: SCHEMA_NOT_FOUND");

    std::vector<app_field_t> table;
    table.push_back(app_field_t{.name = "kp", .access = app_access_t::RW});
    g.set_app_fields(v, std::move(table));

    check(fails_with(g.write(path_t("/dev/x:settings.app.undeclared"), make_value(val)),
                     status_t::SCHEMA_NOT_FOUND),
          "undeclared sibling after install: SCHEMA_NOT_FOUND (the table opens only its names)");
    check(fails_with(g.read(path_t("/dev/x:settings.app.undeclared")), status_t::SCHEMA_NOT_FOUND),
          "undeclared sibling read: SCHEMA_NOT_FOUND");
    check(fails_with(g.write(path_t("/dev/x:frobnicate"), make_value(val)),
                     status_t::SCHEMA_NOT_FOUND),
          "bare unknown field unchanged: SCHEMA_NOT_FOUND");
    check(fails_with(g.write(path_t("/dev/x:settings.nope"), make_value(val)),
                     status_t::SCHEMA_NOT_FOUND),
          "unknown protocol knob unchanged: SCHEMA_NOT_FOUND");
    check(fails_with(g.write(path_t("/dev/x:settings.app"), make_value(val)),
                     status_t::SCHEMA_NOT_FOUND),
          "bare :settings.app container write has no surface");
}

// ---------------------------------------------------------------------------
// RFC-0010 sketch 3 — access gating: ro / rw / wo, remote vs owner, gate order.
void test_gating() {
    std::printf("access gating (RFC-0010 sketch 3):\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    const vertex_handle_t v = g.register_vertex(path_t("/dev/y"), role_t::STORED_VALUE);

    std::vector<app_field_t> table;
    table.push_back(app_field_t{.name = "label", .access = app_access_t::RO});
    table.push_back(app_field_t{.name = "kp", .access = app_access_t::RW});
    table.push_back(app_field_t{.name = "secret", .access = app_access_t::WO});
    g.set_app_fields(v, std::move(table));

    const path_t p_label("/dev/y:settings.app.label");
    const path_t p_kp("/dev/y:settings.app.kp");
    const path_t p_secret("/dev/y:settings.app.secret");
    const std::vector<std::byte> val = value_tlv("v");

    // ro: the remote write has NO surface — checked BEFORE the ACL right (the vertex is
    // ACL-open here, so a PERMISSION_DENIED would betray a wrong gate order).
    check(fails_with(g.write(v, p_label.field(), make_value(val), "linkA"),
                     status_t::SCHEMA_NOT_FOUND),
          "remote write to ro field: SCHEMA_NOT_FOUND (caller-independent, gate 1)");
    check(g.write(v, p_label.field(), make_value(val), {}).has_value(),
          "owner write to ro field succeeds (ro constrains remote callers only)");

    // rw under an ACL: linkA holds WRITE+READ, linkB holds nothing.
    check(g.write(path_t("/dev/y:acl"),
                  make_value(allow_acl("linkA", bit(acl_right_t::READ) | bit(acl_right_t::WRITE))))
              .has_value(),
          ":acl granting linkA READ|WRITE lands");
    check(fails_with(g.write(v, p_kp.field(), make_value(val), "linkB"),
                     status_t::PERMISSION_DENIED),
          "remote rw write without the WRITE right: PERMISSION_DENIED (gate 2)");
    check(g.write(v, p_kp.field(), make_value(val), "linkA").has_value(),
          "remote rw write with the WRITE right succeeds");
    check(reads_back(g.read(v, p_kp.field(), "linkA"), val),
          "remote read of the rw field serves the written bytes");

    // wo: writable remotely (with the right), but no read surface anywhere.
    check(g.write(v, p_secret.field(), make_value(val), "linkA").has_value(),
          "remote wo write with the WRITE right succeeds");
    check(fails_with(g.read(v, p_secret.field(), "linkA"), status_t::SCHEMA_NOT_FOUND),
          "remote wo read: SCHEMA_NOT_FOUND (the secret never mirrors back)");
    check(fails_with(g.read(p_secret), status_t::SCHEMA_NOT_FOUND),
          "local wo read: SCHEMA_NOT_FOUND (no read surface, unqualified)");
}

// ---------------------------------------------------------------------------
// RFC-0010 sketch 4 — the two-part :schema; no table => today's shape.
void test_schema_merge() {
    std::printf(":schema merge (RFC-0010 sketch 4):\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/dev/z"), role_t::STORED_VALUE);

    const std::optional<decoded_t> before = decode_read(g.read(path_t("/dev/z:schema")));
    check(before.has_value() && before->tlv.type == type_t::POINT &&
              before->tlv.children.size() == 2 && name_at(before->tlv, 0, "z") &&
              before->tlv.children[1].type == type_t::SETTINGS,
          "no table: POINT{NAME, SETTINGS} — today's synthesized shape, no app member");

    std::vector<app_field_t> table;
    table.push_back(app_field_t{.name = "kp",
                                .access = app_access_t::RW,
                                .descriptor = dtype_desc("f32")});
    table.push_back(app_field_t{.name = "secret", .access = app_access_t::WO});
    g.set_app_fields(v, std::move(table));

    const std::optional<decoded_t> after = decode_read(g.read(path_t("/dev/z:schema")));
    check(after.has_value() && after->tlv.children.size() == 4,
          "with table: two more members (NAME \"app\" + SETTINGS)");
    if (!after || after->tlv.children.size() != 4) return;
    // The synthesized protocol part is byte-stable: same first two members as before.
    check(after->tlv.children[1].type == type_t::SETTINGS &&
              tr::wire::encode(after->tlv.children[1]) == tr::wire::encode(before->tlv.children[1]),
          "protocol part unchanged by the owner part (precedence by position)");
    check(name_at(after->tlv, 2, "app") && after->tlv.children[3].type == type_t::SETTINGS,
          "owner part: NAME \"app\" then SETTINGS");
    const tlv_t& app = after->tlv.children[3];
    // app = { NAME "kp" SETTINGS{...}, NAME "secret" SETTINGS{...} } — install order.
    check(app.children.size() == 4 && name_at(app, 0, "kp") &&
              app.children[1].type == type_t::SETTINGS && name_at(app, 2, "secret"),
          "one NAME+SETTINGS descriptor pair per declared field, install order");
    if (app.children.size() != 4) return;
    const tlv_t& kp_desc = app.children[1];
    // Leading runtime-projected access, then the owner descriptor bytes verbatim.
    check(kp_desc.children.size() == 4 && name_at(kp_desc, 0, "access") &&
              kp_desc.children[1].type == type_t::VALUE &&
              tr::detail::as_string_view(kp_desc.children[1].payload) == "rw" &&
              name_at(kp_desc, 2, "dtype") && name_at(kp_desc, 3, "f32"),
          "descriptor: runtime-projected access=rw, then owner bytes verbatim");
    const tlv_t& secret_desc = app.children[3];
    check(secret_desc.children.size() == 2 && name_at(secret_desc, 0, "access") &&
              tr::detail::as_string_view(secret_desc.children[1].payload) == "wo",
          "wo field is DESCRIBED in :schema (only its value has no read surface)");

    // Uninstall (empty table) restores today's shape byte-for-byte.
    g.set_app_fields(v, {});
    const std::optional<decoded_t> cleared = decode_read(g.read(path_t("/dev/z:schema")));
    check(cleared.has_value() &&
              tr::wire::encode(cleared->tlv) == tr::wire::encode(before->tlv),
          "empty table uninstalls: schema byte-identical to the pre-install read");
}

// ---------------------------------------------------------------------------
// RFC-0010 sketch 5 / §C — a field write is silent; the owner announce delivers.
void test_announce_flow() {
    std::printf("announce-write convention (RFC-0010 sketch 5, §C):\n");
    graph_t g;
    const vertex_handle_t unit = g.register_vertex(path_t("/unit"), role_t::STORED_VALUE);
    (void)unit;
    const vertex_handle_t temp = g.register_vertex(path_t("/unit/temp"), role_t::STORED_VALUE);

    std::vector<app_field_t> table;
    table.push_back(app_field_t{.name = "offset", .access = app_access_t::RW});
    g.set_app_fields(temp, std::move(table));

    int deliveries = 0;
    check(g.subscribe(path_t("/unit"), [](void* ctx, const rope_t&) {
                          ++*static_cast<int*>(ctx);
                      }, &deliveries).has_value(),
          "subtree subscriber above the vertex lands");

    const path_t p("/unit/temp:settings.app.offset");
    const std::uint64_t walks0 = g.ancestor_walks();
    check(g.write(temp, p.field(), make_value(value_tlv("42")), "linkA").has_value(),
          "caller-attributed app-field write lands (ACL open)");
    check(deliveries == 0, "the field write delivered NOTHING (no propagation)");
    check(g.ancestor_walks() == walks0, "the field write never walked ancestors");
    check(fails_with(g.await(temp, std::chrono::milliseconds(30)), status_t::TIMEOUT),
          "await did not wake on the field write (write seq untouched)");

    // The owner applies the change and announces with an ordinary data write (§C).
    check(g.write(temp, rope_t{make_value(value_tlv("announced"))}).has_value(),
          "owner announce write lands");
    check(deliveries == 1, "subscriber received exactly ONE ordinary delivery (the announce)");
    check(reads_back(g.read(p), value_tlv("42")),
          "re-read of the app field after the announce shows the written value");
}

// ---------------------------------------------------------------------------
// RFC-0010 sketch 6 / §D — fields ride the vertex: no vertices, no edges minted.
void test_storage_shape() {
    std::printf("storage shape (RFC-0010 sketch 6, §D):\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/dev/n"), role_t::STORED_VALUE);

    std::vector<app_field_t> table;
    for (int i = 0; i < 16; ++i)
        table.push_back(app_field_t{.name = "f" + std::to_string(i),
                                    .access = app_access_t::RW,
                                    .descriptor = dtype_desc("u32")});
    g.set_app_fields(v, std::move(table));
    bool all_writes_ok = true;
    for (int i = 0; i < 16; ++i) {
        const path_t p("/dev/n:settings.app.f" + std::to_string(i));
        all_writes_ok = all_writes_ok && g.write(p, make_value(value_tlv("x"))).has_value();
    }
    check(all_writes_ok, "all 16 declared fields are writable");

    const std::optional<decoded_t> members = decode_read(g.read(path_t("/dev/n:children")));
    check(members.has_value() && members->tlv.children.empty(),
          "declaring+writing 16 fields minted NO child vertices (member listing empty)");
    check(!g.find(path_t("/dev/n/f0").key()).has_value(),
          "an app field name is not an addressable vertex");
    check(g.read_subscribers(v).has_value() && g.read_subscribers(v)->empty(),
          "no subscriber slots appeared");
}

// ---------------------------------------------------------------------------
// RFC-0010 sketch 7 — container reads: :settings (knobs + app) and :settings.app.
void test_container_reads() {
    std::printf("container reads (RFC-0010 sketch 7):\n");
    graph_t g;
    const vertex_handle_t bare = g.register_vertex(path_t("/dev/bare"), role_t::STORED_VALUE);
    (void)bare;
    const vertex_handle_t v = g.register_vertex(path_t("/dev/c"), role_t::STORED_VALUE);

    check(fails_with(g.read(path_t("/dev/bare:settings.app")), status_t::SCHEMA_NOT_FOUND),
          ":settings.app on a table-less vertex stays closed (SCHEMA_NOT_FOUND)");
    const std::optional<decoded_t> bare_settings = decode_read(g.read(path_t("/dev/bare:settings")));
    check(bare_settings.has_value() && bare_settings->tlv.type == type_t::SETTINGS &&
              bare_settings->tlv.children.size() == 14 && name_at(bare_settings->tlv, 0, "reliability"),
          ":settings on a table-less vertex: the 7 protocol knobs, no app member");

    std::vector<app_field_t> table;
    table.push_back(app_field_t{.name = "written",
                                .access = app_access_t::RW,
                                .value = value_tlv("w")});
    table.push_back(app_field_t{.name = "unwritten", .access = app_access_t::RW});
    table.push_back(app_field_t{.name = "secret",
                                .access = app_access_t::WO,
                                .value = value_tlv("s")});
    g.set_app_fields(v, std::move(table));

    const std::optional<decoded_t> app = decode_read(g.read(path_t("/dev/c:settings.app")));
    check(app.has_value() && app->tlv.type == type_t::SETTINGS && app->tlv.children.size() == 2 &&
              name_at(app->tlv, 0, "written") && app->tlv.children[1].type == type_t::VALUE,
          ":settings.app: the written rw field only — unset omitted, wo never mirrored");

    check(fails_with(g.read(path_t("/dev/c:settings.app.unwritten")), status_t::NOT_FOUND),
          "a declared-but-unset field reads NOT_FOUND (distinct from SCHEMA_NOT_FOUND)");

    const std::optional<decoded_t> full = decode_read(g.read(path_t("/dev/c:settings")));
    check(full.has_value() && full->tlv.children.size() == 16 && name_at(full->tlv, 14, "app") &&
              full->tlv.children[15].type == type_t::SETTINGS &&
              full->tlv.children[15].children.size() == 2,
          ":settings: protocol knobs + nested app record in ONE traversal");
}

// ---------------------------------------------------------------------------
// §A.3 — the owner apply seam fires with the key and the written TLV, post-store.
void test_apply_seam() {
    std::printf("owner apply seam (RFC-0010 §A.3):\n");
    graph_t g;

    struct seen_t {
        std::string name;
        std::vector<std::byte> bytes;
        int fired = 0;
    } seen;
    handlers_t h;
    h.on_app_field_write = [&seen](std::string_view name, const tr::view::view_t& value) {
        seen.name.assign(name);
        const std::span<const std::byte> b = value.bytes();
        seen.bytes.assign(b.begin(), b.end());
        ++seen.fired;
    };
    const vertex_handle_t v =
        g.register_vertex(path_t("/dev/h"), role_t::STORED_VALUE, std::move(h));

    std::vector<app_field_t> table;
    table.push_back(app_field_t{.name = "mode", .access = app_access_t::RW});
    g.set_app_fields(v, std::move(table));

    const std::vector<std::byte> val = value_tlv("eco");
    check(g.write(path_t("/dev/h:settings.app.mode"), make_value(val)).has_value(),
          "owner write to the handled field lands");
    check(seen.fired == 1 && seen.name == "mode" && seen.bytes == val,
          "seam fired once with the field key and the written TLV");
    // By the time the seam fires the bytes are already stored (apply reads them back).
    check(seen.fired == 1 && reads_back(g.read(path_t("/dev/h:settings.app.mode")), val),
          "store happens BEFORE the seam (the applied value is readable inside it)");

    const path_t p("/dev/h:settings.app.mode");
    check(g.write(v, p.field(), make_value(value_tlv("max")), "linkA").has_value() &&
              seen.fired == 2 && seen.bytes == value_tlv("max"),
          "seam fires for a caller-attributed write too");
    check(fails_with(g.write(path_t("/dev/h:settings.app.off"), make_value(val)),
                     status_t::SCHEMA_NOT_FOUND) &&
              seen.fired == 2,
          "a rejected write never fires the seam");
}

// ---------------------------------------------------------------------------
// §A.2 — declaration is not one-shot: replacement is atomic and complete.
void test_table_replace() {
    std::printf("table replacement (RFC-0010 §A.2):\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/dev/r"), role_t::STORED_VALUE);

    std::vector<app_field_t> t1;
    t1.push_back(app_field_t{.name = "old", .access = app_access_t::RW});
    g.set_app_fields(v, std::move(t1));
    check(g.write(path_t("/dev/r:settings.app.old"), make_value(value_tlv("1"))).has_value(),
          "field of the first table is writable");

    std::vector<app_field_t> t2;
    t2.push_back(app_field_t{.name = "new", .access = app_access_t::RW});
    g.set_app_fields(v, std::move(t2));
    check(fails_with(g.write(path_t("/dev/r:settings.app.old"), make_value(value_tlv("2"))),
                     status_t::SCHEMA_NOT_FOUND),
          "a replaced-away name reverts to SCHEMA_NOT_FOUND");
    check(fails_with(g.read(path_t("/dev/r:settings.app.old")), status_t::SCHEMA_NOT_FOUND),
          "its stored value is gone with the entry (values ride the table)");
    check(g.write(path_t("/dev/r:settings.app.new"), make_value(value_tlv("3"))).has_value(),
          "the replacement table's field is live");
}

}  // namespace

int main() {
    test_declare_read_write();
    test_undeclared_enotty();
    test_gating();
    test_schema_merge();
    test_announce_flow();
    test_storage_shape();
    test_container_reads();
    test_apply_seam();
    test_table_replace();
    std::printf(g_failures == 0 ? "\napp_fields: PASS\n" : "\napp_fields: FAIL (%d)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
