/**
 * @file
 * @brief Per-endpoint :acl — storage (#81-A) + core-subset enforcement (#81, ADR-0018/ 0020/0026).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The graph stores the raw ACL TLV bytes verbatim AND parses them
 * into ALLOW-only ACEs at write time (a DENY ACE or unsupported flag bits are
 * rejected with TYPE_MISMATCH so subset evaluation never silently weakens the
 * written semantics). Enforcement is opt-in twice over: no subject resolver =>
 * everything allowed (today's behavior); an empty effective ACL => open. With a
 * resolver installed the gates are READ / WRITE / SUBSCRIBE (producer fan-out) /
 * CREATE / READ_ACL / WRITE_ACL, denial = PERMISSION_DENIED, and fan-in
 * re-dispatch is gated by the TARGET's WRITE right under the edge's stored
 * caller context. The remote path (op_resolver_t with an inbound_link) replies
 * kind=ERROR STATUS{ERROR{VALUE 0x0050 tr::access::denied}}.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::acl_right_t;
using tr::graph::delivery_mode_t;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::op_resolver_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::subject_token_t;
using tr::graph::vertex_handle_t;
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

/** @brief A VALUE TLV carrying @p payload verbatim. */
std::vector<std::byte> value_tlv(std::span<const std::byte> payload) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, payload);
    return out;
}

// --- ACL builders (docs/reference/05 §0x0A ACE byte layout) ------------------
struct ace_spec_t {
    std::uint8_t type = 0; /**< ALLOW=0, DENY=1 */
    std::uint8_t flags = 0;
    std::string_view subject;
    std::uint32_t mask = 0;
    std::uint64_t expires_ns = 0; /**< 0 => omit the field */
};

tlv_t u_value(std::uint64_t v, std::size_t width, std::vector<std::vector<std::byte>>& keep) {
    std::vector<std::byte> payload(width);
    tr::detail::store_le(payload, v, width);
    keep.push_back(std::move(payload));
    return tlv_t{.type = type_t::VALUE, .payload = keep.back()};
}

tlv_t name_tlv(std::string_view s, std::vector<std::vector<std::byte>>& keep) {
    keep.push_back(as_bytes(s));
    return tlv_t{.type = type_t::NAME, .payload = keep.back()};
}

/**
 * @brief Encode ACL{ ACL{...ACE...}* } via the typed ADR-0050 surface (encode_acl) — replaces the
 *        hand-rolled byte builder; deliberately-invalid ACLs (a DENY ACE under the ALLOW-only
 *        profile) still encode, since parse_acl is the gate.
 */
std::vector<std::byte> make_acl(std::initializer_list<ace_spec_t> aces) {
    std::vector<tr::graph::ace_t> typed;
    typed.reserve(aces.size());
    for (const ace_spec_t& a : aces) {
        typed.push_back(tr::graph::ace_t{.type = static_cast<tr::graph::ace_type_t>(a.type),
                                         .flags = a.flags,
                                         .subject = as_bytes(a.subject),
                                         .access_mask = a.mask,
                                         .expires_ns = a.expires_ns});
    }
    return tr::graph::encode_acl(typed);
}

constexpr std::uint32_t bit(acl_right_t r) { return static_cast<std::uint32_t>(r); }

/**
 * @brief The test resolver (ADR-0018): a non-empty caller context resolves to its own bytes as the
 *        subject token; an empty (local) context is trusted (nullopt).
 */
std::optional<subject_token_t> caller_is_subject(std::string_view caller) {
    if (caller.empty()) return std::nullopt;
    return as_bytes(caller);
}

/** @brief Write `text` bytes as a VALUE TLV to `v` under `caller`. */
tr::graph::result_t<void> write_u8(graph_t& g, vertex_handle_t v, std::uint8_t x,
                                   std::string_view caller = {}) {
    std::vector<std::byte> out;
    const std::byte payload[1] = {std::byte{x}};
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, payload);
    return g.write(v, make_value(out), caller);
}

// --- FWD builders (the op_resolve_test idiom) --------------------------------
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

std::vector<std::byte> b_fwd(fwd_op_t op, const std::vector<std::byte>& dst,
                             const std::vector<std::byte>& src,
                             const std::vector<std::byte>& payload = {}) {
    std::vector<std::byte> body;
    const std::byte opb[1] = {std::byte{static_cast<std::uint8_t>(op)}};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, opb);
    body.insert(body.end(), dst.begin(), dst.end());
    body.insert(body.end(), src.begin(), src.end());
    body.insert(body.end(), payload.begin(), payload.end());
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

tr::graph::result_t<tr::view::rope_t> resolve_bytes(op_resolver_t& resolver,
                                                    std::span<const std::byte> fwd,
                                                    std::string_view inbound_link = {}) {
    const auto arena = tr::wire::decode_into(fwd, *std::pmr::get_default_resource());
    if (!arena) return std::unexpected(status_t::INVALID_PATH);
    return resolver.resolve(*arena, inbound_link);
}

/**
 * @brief The reply kind and — for kind=ERROR — the registered u16 code of the STATUS{ERROR{VALUE
 *        u16 LE}} payload (RFC-0002 §C).
 */
struct reply_info_t {
    reply_kind_t kind{};
    std::uint16_t code = 0;
};
reply_info_t reply_info(const tr::view::rope_t& reply) {
    const tr::view::view_t flat = reply.flatten();
    const auto dec = tr::wire::decode(flat.bytes());
    reply_info_t out;
    if (!dec || dec->children.size() < 4) return out;
    out.kind =
        static_cast<reply_kind_t>(tr::detail::load_le<std::uint8_t>(dec->children[3].payload));
    if (out.kind == reply_kind_t::ERROR && dec->children.size() >= 5) {
        const tlv_t& status = dec->children[4];
        if (status.type == type_t::STATUS && !status.children.empty() &&
            status.children[0].type == type_t::ERROR && !status.children[0].children.empty()) {
            out.code = tr::detail::load_le<std::uint16_t>(status.children[0].children[0].payload);
        }
    }
    return out;
}

bool denied(const tr::graph::result_t<void>& r) {
    return !r.has_value() && r.error() == status_t::PERMISSION_DENIED;
}
template <class T>
bool denied(const tr::graph::result_t<T>& r) {
    return !r.has_value() && r.error() == status_t::PERMISSION_DENIED;
}

/** @brief True iff @p r failed with exactly @p s — the discriminating form: a gate-order
 *         assertion must distinguish SCHEMA_NOT_FOUND from PERMISSION_DENIED, which a bare
 *         "it failed" check cannot. */
bool fails_with(const tr::graph::result_t<void>& r, status_t s) {
    return !r.has_value() && r.error() == s;
}
template <class T>
bool fails_with(const tr::graph::result_t<T>& r, status_t s) {
    return !r.has_value() && r.error() == s;
}

// ---------------------------------------------------------------------------
void test_storage_roundtrip() {
    std::printf(":acl storage + round-trip (#81-A):\n");
    graph_t g;
    const auto path = path_t::parse("/x");
    (void)g.register_vertex(*path, role_t::STORED_VALUE);

    {
        const auto r = g.read(path_t("/x:acl"));
        check(!r.has_value() && r.error() == status_t::NOT_FOUND, "unset :acl reads NOT_FOUND");
    }

    const std::vector<std::byte> acl =
        make_acl({{.subject = "peer-a", .mask = bit(acl_right_t::READ)}});
    {
        const auto w = g.write(path_t("/x:acl"), make_value(acl));
        check(w.has_value(), "writing a valid ALLOW-only ACL TLV to :acl succeeds");
        const auto r = g.read(path_t("/x:acl"));
        const bool eq = r.has_value() && r->only().bytes().size() == acl.size() &&
                        std::equal(acl.begin(), acl.end(), r->only().bytes().begin());
        check(eq, "read :acl returns the stored bytes verbatim");
    }

    {  // a non-ACL TLV is rejected; storage unchanged
        tlv_t value{.type = type_t::VALUE, .payload = std::span<const std::byte>(acl).first(0)};
        const std::vector<std::byte> not_acl = tr::wire::encode(value);
        const auto w = g.write(path_t("/x:acl"), make_value(not_acl));
        check(!w.has_value() && w.error() == status_t::TYPE_MISMATCH,
              "writing a non-ACL TLV to :acl returns TYPE_MISMATCH");
        const auto r = g.read(path_t("/x:acl"));
        const bool unchanged = r.has_value() && r->only().bytes().size() == acl.size() &&
                               std::equal(acl.begin(), acl.end(), r->only().bytes().begin());
        check(unchanged, "rejected write leaves the stored ACL unchanged");
    }
}

void test_subset_rejections() {
    std::printf("core-subset :acl write rejections (ADR-0020 subset, never weakened):\n");
    graph_t g;
    const auto path = path_t::parse("/x");
    (void)g.register_vertex(*path, role_t::STORED_VALUE);
    const auto acl_field = path_t::parse("/x:acl");

    {  // DENY handling depends on the compiled policy (ADR-0020 subset vs full)
        const auto w = g.write(*acl_field, make_value(make_acl({
                                               {.type = 1,  // DENY
                                                .subject = "peer-a",
                                                .mask = bit(acl_right_t::READ)},
                                           })));
        if constexpr (tr::graph::acl_policy_t::kAcceptsDeny) {
            // full host policy (LIBTRACER_ACL_FULL): DENY is a first-class ACE, stored + evaluated
            check(w.has_value(), "the full policy stores an ACL carrying a DENY ACE");
        } else {
            // ALLOW-only subset: DENY is beyond the profile — rejected at write, not
            // parse-but-ignored
            check(!w.has_value() && w.error() == status_t::TYPE_MISMATCH,
                  "an ACL carrying a DENY ACE is rejected with TYPE_MISMATCH");
        }
    }
    {  // flags beyond the single INHERIT bit (e.g. INHERIT_ONLY=0x2) — rejected
        const auto w = g.write(
            *acl_field, make_value(make_acl({
                            {.flags = 0x2, .subject = "peer-a", .mask = bit(acl_right_t::READ)},
                        })));
        check(!w.has_value() && w.error() == status_t::TYPE_MISMATCH,
              "an ACE with flag bits beyond INHERIT is rejected with TYPE_MISMATCH");
    }
    {  // a structurally incomplete ACE (no access_mask) — rejected
        std::vector<std::vector<std::byte>> keep;
        tlv_t ace{.type = type_t::ACL, .opt = opt_t{.pl = true}};
        ace.children.push_back(name_tlv("type", keep));
        ace.children.push_back(u_value(0, 1, keep));
        ace.children.push_back(name_tlv("subject", keep));
        keep.push_back(as_bytes("peer-a"));
        ace.children.push_back(tlv_t{.type = type_t::VALUE, .payload = keep.back()});
        tlv_t acl{.type = type_t::ACL, .opt = opt_t{.pl = true}};
        acl.children.push_back(std::move(ace));
        const auto w = g.write(*acl_field, make_value(tr::wire::encode(acl)));
        check(!w.has_value() && w.error() == status_t::TYPE_MISMATCH,
              "an ACE missing access_mask is rejected with TYPE_MISMATCH");
    }
    {  // rejected writes never installed anything (the DENY write only stores under the full
       // policy)
        const auto r = g.read(*acl_field);
        if constexpr (tr::graph::acl_policy_t::kAcceptsDeny) {
            check(r.has_value(), "the full policy stored the DENY-carrying ACL on :acl");
        } else {
            check(!r.has_value() && r.error() == status_t::NOT_FOUND,
                  "no rejected ACL was stored (:acl still NOT_FOUND)");
        }
    }

    if constexpr (tr::graph::acl_policy_t::kAcceptsDeny) {
        // the full policy actually EVALUATES DENY: an ordered [DENY, ALLOW] on the same bit
        // denies (first-match-per-bit), proving a DENY overrides a later ALLOW — not just parse
        // acceptance but real ADR-0020 evaluation coverage for the LIBTRACER_ACL_FULL config.
        graph_t gf;
        const vertex_handle_t v = gf.register_vertex(path_t("/d"), role_t::STORED_VALUE);
        gf.set_subject_resolver(caller_is_subject);
        const auto w = gf.write(
            path_t("/d:acl"),
            make_value(make_acl({
                {.type = 1, .subject = "peer-a", .mask = bit(acl_right_t::READ)},  // DENY first
                {.type = 0, .subject = "peer-a", .mask = bit(acl_right_t::READ)},  // then ALLOW
            })));
        check(w.has_value(), "the full policy stores an ordered DENY-before-ALLOW ACL");
        check(denied(gf.read(v, "peer-a")),
              "a first-match DENY overrides a later ALLOW on the same bit (READ denied)");
    }
}

void test_open_by_default() {
    std::printf("open by default — no resolver / trusted caller / empty ACL:\n");

    {  // no resolver installed => a restrictive ACL is stored but not enforced
        graph_t g;
        vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
        (void)g.write(
            path_t("/x:acl"),
            make_value(make_acl({{.subject = "only-peer-z", .mask = bit(acl_right_t::READ)}})));
        check(write_u8(g, v, 1, "peer-a").has_value(),
              "no resolver => WRITE allowed despite a non-granting ACL");
        check(g.read(v, "peer-a").has_value(), "no resolver => READ allowed");
    }
    {  // resolver installed, vertex has no ACL => open
        graph_t g;
        g.set_subject_resolver(caller_is_subject);
        vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
        check(write_u8(g, v, 1, "peer-a").has_value(), "resolver + no ACL => WRITE allowed");
        check(g.read(v, "peer-a").has_value(), "resolver + no ACL => READ allowed");
    }
    {  // resolver installed, trusted (local, empty-context) caller => allowed
        graph_t g;
        g.set_subject_resolver(caller_is_subject);
        vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
        (void)g.write(
            path_t("/x:acl"),
            make_value(make_acl({{.subject = "only-peer-z", .mask = bit(acl_right_t::READ)}})));
        check(write_u8(g, v, 1).has_value(),
              "trusted (nullopt-subject) caller bypasses a non-granting ACL");
    }
}

void test_gated_ops() {
    std::printf("every gated op, allow + deny (resolver installed):\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    (void)write_u8(g, v, 7);  // seed an LKV (trusted local write)

    // peer-a: READ+WRITE; peer-s: SUBSCRIBE; peer-c: CREATE; peer-r: READ_ACL;
    // peer-w: WRITE_ACL. Installed by the trusted local caller.
    const auto install = [&] {
        return g.write(
            path_t("/x:acl"),
            make_value(make_acl({
                {.subject = "peer-a", .mask = bit(acl_right_t::READ) | bit(acl_right_t::WRITE)},
                {.subject = "peer-s", .mask = bit(acl_right_t::SUBSCRIBE)},
                {.subject = "peer-c", .mask = bit(acl_right_t::CREATE)},
                {.subject = "peer-r", .mask = bit(acl_right_t::READ_ACL)},
                {.subject = "peer-w", .mask = bit(acl_right_t::WRITE_ACL)},
            })));
    };
    check(install().has_value(), "trusted local caller installs the :acl");

    // READ
    check(g.read(v, "peer-a").has_value(), "READ allowed for a READ-granted subject");
    check(denied(g.read(v, "peer-s")), "READ denied without the READ bit");
    // WRITE
    check(write_u8(g, v, 1, "peer-a").has_value(), "WRITE allowed for a WRITE-granted subject");
    check(denied(write_u8(g, v, 2, "peer-r")), "WRITE denied without the WRITE bit");
    // AWAIT rides the READ right (checked up front)
    {
        const auto r = g.await(v, std::chrono::nanoseconds(1), "peer-s");
        check(!r.has_value() && r.error() == status_t::PERMISSION_DENIED,
              "AWAIT denied without the READ bit");
    }
    // SUBSCRIBE — the producer-side :subscribers[] append gate
    {
        std::vector<std::byte> sub;
        tr::wire::emit_tlv(sub, type_t::SUBSCRIBER, opt_t{.pl = true}, b_path({"sink"}));
        const auto field = path_t::parse("/x:subscribers[]");
        check(g.write(v, field->field(), make_value(sub), "peer-s").has_value(),
              ":subscribers[] append allowed for a SUBSCRIBE-granted subject");
        check(denied(g.write(v, field->field(), make_value(sub), "peer-a")),
              ":subscribers[] append denied without the SUBSCRIBE bit");
    }
    // CREATE — the :children[] gate (ADR-0017)
    {
        std::vector<std::byte> body;
        tr::wire::emit_name(body, "type");
        tr::wire::emit_name(body, "stored_value");
        tr::wire::emit_name(body, "name");
        tr::wire::emit_name(body, "kid");
        std::vector<std::byte> spec;
        tr::wire::emit_tlv(spec, type_t::SPEC, opt_t{.pl = true}, body);
        const auto field = path_t::parse("/x:children[]");
        check(denied(g.write(v, field->field(), make_value(spec), "peer-a")),
              ":children[] create denied without the CREATE bit");
        check(g.write(v, field->field(), make_value(spec), "peer-c").has_value(),
              ":children[] create allowed for a CREATE-granted subject");
        check(g.find(path_t::parse("/x/kid")->key()).has_value(), "the child was created");
    }
    // READ_ACL — its own right, distinct from READ
    {
        const auto acl_field = path_t::parse("/x:acl");
        check(g.read(v, acl_field->field(), "peer-r").has_value(),
              ":acl read allowed for a READ_ACL-granted subject");
        check(denied(g.read(v, acl_field->field(), "peer-a")),
              ":acl read denied without READ_ACL (READ alone is not enough)");
    }
    // WRITE_ACL — precisely the admin right
    {
        const auto acl_field = path_t::parse("/x:acl");
        const std::vector<std::byte> next =
            make_acl({{.subject = "peer-w", .mask = bit(acl_right_t::WRITE_ACL)}});
        check(denied(g.write(v, acl_field->field(), make_value(next), "peer-a")),
              ":acl write denied without WRITE_ACL");
        check(g.write(v, acl_field->field(), make_value(next), "peer-w").has_value(),
              ":acl write allowed for a WRITE_ACL-granted subject (delegation works)");
    }
    // EVERYONE@ matches any resolved subject
    {
        (void)g.write(
            path_t("/x:acl"),
            make_value(make_acl({{.subject = "EVERYONE@", .mask = bit(acl_right_t::READ)}})));
        check(g.read(v, "some-random-peer").has_value(),
              "EVERYONE@ grants the bit to any resolved subject");
        check(denied(write_u8(g, v, 3, "some-random-peer")),
              "EVERYONE@ grant is still bit-scoped (WRITE stays denied)");
    }
}

/**
 * @brief Gate order on the flat protocol-knob write surface: the NAME resolves BEFORE the
 *        ACL right, so an unknown knob is SCHEMA_NOT_FOUND regardless of caller.
 *
 * The normative rule is the docs/reference/05 §`0x0B` validation clause — "unknown NAMEs
 * MUST be ... rejected with `ERROR{tr::schema::not_found}` if in the core namespace" —
 * which carries no caller qualifier, so a denied caller may not convert it into
 * PERMISSION_DENIED. The `settings.app.` branch already resolves the name first (its own
 * gate-1 test asserts the ENOTTY of an undeclared name), and the terminal branch has no
 * gate at all; this pins the flat branch to the same order, which is what makes the rule
 * hold uniformly across all three.
 *
 * The discriminating case is a caller the ACL DENIES writing a name that does not exist:
 * only a name-first gate can answer SCHEMA_NOT_FOUND there. The converse — a known knob,
 * denied caller — must still be PERMISSION_DENIED, or the hoist would have leaked the
 * write itself.
 */
void test_flat_knob_name_before_acl() {
    std::printf("flat protocol knobs: the NAME resolves before the ACL right:\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    // peer-a holds WRITE; peer-none holds nothing. Installed by the trusted local caller.
    check(g.write(path_t("/x:acl"),
                  make_value(make_acl({{.subject = "peer-a", .mask = bit(acl_right_t::WRITE)}})))
              .has_value(),
          "trusted local caller installs the :acl");

    const std::array<std::byte, 8> le{std::byte{0x88}, std::byte{0x13}};  // 5000 LE
    const auto knob = path_t::parse("/x:settings.deadline_ns");
    const auto unknown = path_t::parse("/x:settings.bogus");

    // The property under test: an unknown NAME is caller-independent.
    check(fails_with(g.write(v, unknown->field(), make_value(value_tlv(le)), "peer-none"),
                     status_t::SCHEMA_NOT_FOUND),
          "unknown knob + denied caller: SCHEMA_NOT_FOUND (the name gates first)");
    check(fails_with(g.write(v, unknown->field(), make_value(value_tlv(le)), "peer-a"),
                     status_t::SCHEMA_NOT_FOUND),
          "unknown knob + granted caller: SCHEMA_NOT_FOUND");
    check(fails_with(g.write(v, unknown->field(), make_value(value_tlv(le)), {}),
                     status_t::SCHEMA_NOT_FOUND),
          "unknown knob + trusted local caller: SCHEMA_NOT_FOUND (all three agree)");

    // The ACL still gates every knob that DOES exist — the hoist reorders, it never opens.
    check(denied(g.write(v, knob->field(), make_value(value_tlv(le)), "peer-none")),
          "known knob + denied caller: PERMISSION_DENIED (the write stays gated)");
    check(g.write(v, knob->field(), make_value(value_tlv(le)), "peer-a").has_value(),
          "known knob + granted caller: the write lands");
    check(g.settings(v).deadline_ns == 5000, "... and it actually took effect");

    // A knob NAME is exactly two plain steps. `settings.reliability.bogus` and
    // `settings.reliability[2]` name NO knob — the read surface already rejects both
    // (it checks `plain_step` on every step), and before the name-first hoist the write
    // surface silently ACCEPTED them as writes to `reliability`, ignoring the tail.
    check(fails_with(g.write(v, path_t::parse("/x:settings.reliability.bogus")->field(),
                             make_value(value_tlv(le)), "peer-a"),
                     status_t::SCHEMA_NOT_FOUND),
          "a trailing step names no knob: SCHEMA_NOT_FOUND (the tail is not ignored)");
    check(fails_with(g.write(v, path_t::parse("/x:settings.reliability[2]")->field(),
                             make_value(value_tlv(le)), "peer-a"),
                     status_t::SCHEMA_NOT_FOUND),
          "a selector step names no knob: SCHEMA_NOT_FOUND (no knob has an indexed surface)");
    check(g.settings(v).reliability == 0, "... and neither malformed write reached `reliability`");
}

/**
 * @brief The `:acl` and `:schema` field surfaces are addressed WHOLE — a trailing step or
 *        an `[N]`/`[]` selector names nothing, on read and on write.
 *
 * The write half is the one that bites: `set_acl` REPLACES, so an unresolved shape is not a
 * harmless no-op. Before the bound, `write :acl.bogus` reached `set_acl` and silently
 * swapped the vertex's entire access-control list for the payload — a caller with
 * `WRITE_ACL` who typoed the field name, or emitted a stray selector, overwrote the ACL and
 * (if the new list did not grant them) locked themselves out. That lockout is also what
 * makes the bug easy to misdiagnose: reading `:acl` back as the ORIGINAL admin fails
 * BECAUSE the defect fired, which reads like "the write did nothing". This test reads back
 * as the subject the bogus payload grants, which is the only caller who can still see it.
 */
void test_acl_and_schema_are_addressed_whole() {
    std::printf(":acl / :schema are addressed whole — no member or slot addressing:\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);

    const std::vector<std::byte> original =
        make_acl({{.subject = "admin",
                   .mask = bit(acl_right_t::WRITE_ACL) | bit(acl_right_t::READ_ACL) |
                           bit(acl_right_t::READ)}});
    check(g.write(path_t("/x:acl"), make_value(original)).has_value(), "the real :acl lands");

    // A DIFFERENT acl, granting someone else — if a bogus shape reaches set_acl, this
    // replaces the list and hands /x to `usurper`.
    const std::vector<std::byte> usurping =
        make_acl({{.subject = "usurper",
                   .mask = bit(acl_right_t::WRITE_ACL) | bit(acl_right_t::READ_ACL) |
                           bit(acl_right_t::READ)}});

    for (const char* p : {"/x:acl.bogus", "/x:acl[0]", "/x:acl[]"}) {
        const auto fp = path_t::parse(p);
        check(fp.has_value(), "the malformed :acl path parses (so the branch is reachable)");
        if (!fp) continue;
        check(fails_with(g.write(v, fp->field(), make_value(usurping), "admin"),
                         status_t::SCHEMA_NOT_FOUND),
              "a non-whole :acl write names nothing: SCHEMA_NOT_FOUND (it must NOT replace)");
        // The decisive assertion: `usurper` was never granted anything, so if it can read
        // the :acl, the bogus write replaced the list.
        check(denied(g.read(v, path_t::parse("/x:acl")->field(), "usurper")),
              "... and the usurping ACL did NOT take effect");
    }
    check(g.read(v, path_t::parse("/x:acl")->field(), "admin").has_value(),
          "the original admin still holds READ_ACL — the list is intact");

    // The read half: whole or nothing, for both fields.
    check(fails_with(g.read(v, path_t::parse("/x:acl[7]")->field(), "admin"),
                     status_t::SCHEMA_NOT_FOUND),
          ":acl[7] is SCHEMA_NOT_FOUND — an ACE is not separately addressable");
    check(fails_with(g.read(v, path_t::parse("/x:schema[7]")->field(), "admin"),
                     status_t::SCHEMA_NOT_FOUND),
          ":schema[7] is SCHEMA_NOT_FOUND — the schema is one POINT, not an array");
    check(g.read(v, path_t::parse("/x:schema")->field(), "admin").has_value(),
          "the bare :schema read still works");
}

void test_expiry() {
    std::printf("ACE expiry (expires_ns, absolute ns since epoch):\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    (void)write_u8(g, v, 7);

    const std::uint64_t far_future = ~std::uint64_t{0} >> 1;  // ~year 2262
    (void)g.write(
        path_t("/x:acl"),
        make_value(make_acl({
            {.subject = "peer-live", .mask = bit(acl_right_t::READ), .expires_ns = far_future},
            {.subject = "peer-stale", .mask = bit(acl_right_t::READ), .expires_ns = 1},
        })));
    check(g.read(v, "peer-live").has_value(), "a non-expired ACE grants");
    check(denied(g.read(v, "peer-stale")), "an expired ACE grants nothing (denied)");
}

void test_inheritance() {
    std::printf("inheritance — effective ACL = own + INHERIT-flagged ancestor ACEs:\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    vertex_handle_t child = g.register_vertex(path_t("/dev/temp"), role_t::STORED_VALUE);
    vertex_handle_t grandchild = g.register_vertex(path_t("/dev/temp/raw"), role_t::STORED_VALUE);
    (void)write_u8(g, child, 7);
    (void)write_u8(g, grandchild, 7);

    // The composite ACL: peer-i READ with INHERIT (covers the subtree);
    // peer-l WRITE without INHERIT (that vertex only).
    (void)g.write(
        path_t("/dev:acl"),
        make_value(make_acl({
            {.flags = tr::graph::kAceInherit, .subject = "peer-i", .mask = bit(acl_right_t::READ)},
            {.subject = "peer-l", .mask = bit(acl_right_t::WRITE)},
        })));

    check(g.read(child, "peer-i").has_value(), "child READ allowed via the INHERIT ACE");
    check(g.read(grandchild, "peer-i").has_value(),
          "grandchild READ allowed via the INHERIT ACE (whole subtree)");
    check(denied(write_u8(g, child, 1, "peer-i")),
          "the inherited grant is bit-scoped — child WRITE denied");
    check(write_u8(g, *g.find(path_t::parse("/dev")->key()), 1, "peer-l").has_value(),
          "the non-INHERIT WRITE ACE applies on /dev itself");
    check(denied(write_u8(g, child, 1, "peer-l")),
          "the non-INHERIT WRITE ACE does NOT travel to the child (closed by the "
          "inherited ACE, no matching grant)");

    // Non-INHERIT ancestor ACEs do NOT propagate: a child whose effective ACL would be
    // ONLY the parent's non-INHERIT ACE stays open.
    (void)g.register_vertex(path_t("/base"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/base/leaf"), role_t::STORED_VALUE);
    (void)g.write(path_t("/base:acl"),
                  make_value(make_acl({{.subject = "peer-x", .mask = bit(acl_right_t::READ)}})));
    const auto leaf = *g.find(path_t::parse("/base/leaf")->key());
    check(write_u8(g, leaf, 1, "anyone").has_value(),
          "non-INHERIT parent ACE does not propagate — the leaf stays open");
    // ... while the parent itself is closed by that same ACE.
    check(denied(write_u8(g, *g.find(path_t::parse("/base")->key()), 1, "anyone")),
          "the same ACE closes the parent itself (WRITE denied)");

    // Own ACEs + inherited ACEs combine: the child grants peer-o WRITE; peer-i keeps
    // its inherited READ.
    (void)g.write(path_t("/dev/temp:acl"),
                  make_value(make_acl({{.subject = "peer-o", .mask = bit(acl_right_t::WRITE)}})));
    check(write_u8(g, child, 2, "peer-o").has_value(), "own ACE grants WRITE on the child");
    check(g.read(child, "peer-i").has_value(),
          "inherited READ still applies alongside the child's own ACL");
}

void test_two_acl_fan_in() {
    std::printf("two-ACL gating (ADR-0026) — fan-out SUBSCRIBE + fan-in WRITE:\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    vertex_handle_t src = g.register_vertex(path_t("/src"), role_t::STORED_VALUE);
    vertex_handle_t dst = g.register_vertex(path_t("/dst"), role_t::STORED_VALUE);

    // The producer authorizes its subscribers: /src:acl grants SUBSCRIBE to link-a
    // (and WRITE to the local producer path — writes arrive under the trusted local
    // context here, so only SUBSCRIBE matters for the edge).
    (void)g.write(
        path_t("/src:acl"),
        make_value(make_acl({{.subject = "link-a", .mask = bit(acl_right_t::SUBSCRIBE)}})));
    // The target authorizes its writers: /dst:acl grants WRITE to link-b ONLY.
    (void)g.write(path_t("/dst:acl"),
                  make_value(make_acl({{.subject = "link-b", .mask = bit(acl_right_t::WRITE)}})));

    // Subscribe /src -> /dst as link-a (allowed by the producer's SUBSCRIBE grant).
    std::vector<std::byte> sub;
    tr::wire::emit_tlv(sub, type_t::SUBSCRIBER, opt_t{.pl = true}, b_path({"dst"}));
    const auto sub_field = path_t::parse("/src:subscribers[]");
    check(g.write(src, sub_field->field(), make_value(sub), "link-a").has_value(),
          "fan-out gate: link-a may subscribe (producer's :acl SUBSCRIBE)");
    check(denied(g.write(src, sub_field->field(), make_value(sub), "link-z")),
          "fan-out gate: link-z may not subscribe");

    // Fan-in gate: a write to /src re-dispatches to /dst under the edge's stored
    // caller (link-a) — which /dst's :acl does NOT grant WRITE ⇒ delivery dropped.
    check(write_u8(g, src, 42).has_value(), "trusted local write to the producer succeeds");
    check(!g.read(dst).has_value(),
          "fan-in gate: delivery into /dst dropped (edge caller lacks the target's WRITE)");

    // Open the target to link-a ⇒ the next delivery lands.
    (void)g.write(path_t("/dst:acl"),
                  make_value(make_acl({{.subject = "link-a", .mask = bit(acl_right_t::WRITE)}})));
    check(write_u8(g, src, 43).has_value(), "second write to the producer succeeds");
    const auto delivered = g.read(dst);
    check(delivered.has_value(), "fan-in gate: delivery lands once the target grants WRITE");

    // The local subscribe() sugar is gated too (empty context is trusted under this
    // resolver, so it passes; a resolver may map local callers to a subject instead).
    check(g.subscribe(path_t("/src"), path_t("/dst")).has_value(),
          "local subscribe() sugar passes as the trusted local caller");
}

void test_remote_path() {
    std::printf("remote path — FWD terminus consults the ACL (0x0050 tr::access::denied):\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    op_resolver_t resolver(g);
    vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    (void)write_u8(g, v, 7);
    (void)g.write(path_t("/x:acl"), make_value(make_acl({
                                        {.subject = "link-ok",
                                         .mask = bit(acl_right_t::READ) | bit(acl_right_t::WRITE) |
                                                 bit(acl_right_t::SUBSCRIBE)},
                                    })));

    std::vector<std::byte> payload;
    const std::byte one[1] = {std::byte{1}};
    tr::wire::emit_tlv(payload, type_t::VALUE, opt_t{}, one);

    {  // FWD WRITE denied => kind=ERROR STATUS{ERROR{VALUE 0x0050}}
        const auto fwd = b_fwd(fwd_op_t::WRITE, b_path({"x"}), b_path({"ret"}), payload);
        const auto reply = resolve_bytes(resolver, fwd, "link-bad");
        const reply_info_t info = reply_info(*reply);
        check(reply.has_value() && info.kind == reply_kind_t::ERROR && info.code == 0x0050,
              "denied FWD WRITE replies kind=ERROR STATUS{ERROR{VALUE 0x0050}}");
    }
    {  // the same WRITE from the granted link succeeds
        const auto fwd = b_fwd(fwd_op_t::WRITE, b_path({"x"}), b_path({"ret"}), payload);
        const auto reply = resolve_bytes(resolver, fwd, "link-ok");
        check(reply.has_value() && reply_info(*reply).kind == reply_kind_t::RESULT,
              "granted FWD WRITE replies kind=RESULT");
    }
    {  // FWD READ denied the same way
        const auto fwd = b_fwd(fwd_op_t::READ, b_path({"x"}), b_path({"ret"}));
        const auto reply = resolve_bytes(resolver, fwd, "link-bad");
        const reply_info_t info = reply_info(*reply);
        check(reply.has_value() && info.kind == reply_kind_t::ERROR && info.code == 0x0050,
              "denied FWD READ replies kind=ERROR STATUS{ERROR{VALUE 0x0050}}");
    }
    {  // a remote subscribe (SUBSCRIBER into :subscribers[]) — denied vs granted link.
        // Build FWD WRITE with a FIELD selector: NAME "subscribers" + index_mode=ELEMENT.
        std::vector<std::byte> field_body;
        tr::wire::emit_name(field_body, "subscribers");
        const std::byte mode[1] = {std::byte{1}};  // index_mode ELEMENT (append)
        tr::wire::emit_tlv(field_body, type_t::VALUE, opt_t{}, mode);
        std::vector<std::byte> field;
        tr::wire::emit_tlv(field, type_t::FIELD, opt_t{.pl = true}, field_body);

        std::vector<std::byte> sub;
        tr::wire::emit_tlv(sub, type_t::SUBSCRIBER, opt_t{.pl = true}, b_path({"sink"}));

        std::vector<std::byte> body;
        const std::byte opb[1] = {std::byte{static_cast<std::uint8_t>(fwd_op_t::WRITE)}};
        tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, opb);
        const std::vector<std::byte> dst = b_path({"x"});
        const std::vector<std::byte> ret = b_path({"ret"});
        body.insert(body.end(), dst.begin(), dst.end());
        body.insert(body.end(), field.begin(), field.end());
        body.insert(body.end(), ret.begin(), ret.end());
        body.insert(body.end(), sub.begin(), sub.end());
        std::vector<std::byte> fwd;
        tr::wire::emit_tlv(fwd, type_t::FWD, opt_t{.pl = true}, body);

        const auto denied_reply = resolve_bytes(resolver, fwd, "link-bad");
        const reply_info_t info = reply_info(*denied_reply);
        check(denied_reply.has_value() && info.kind == reply_kind_t::ERROR && info.code == 0x0050,
              "denied remote subscribe replies kind=ERROR STATUS{ERROR{VALUE 0x0050}}");
        const auto ok_reply = resolve_bytes(resolver, fwd, "link-ok");
        check(ok_reply.has_value() && reply_info(*ok_reply).kind == reply_kind_t::RESULT,
              "granted remote subscribe replies kind=RESULT");
    }
}

}  // namespace

int main() {
    test_storage_roundtrip();
    test_subset_rejections();
    test_open_by_default();
    test_gated_ops();
    test_flat_knob_name_before_acl();
    test_acl_and_schema_are_addressed_whole();
    test_expiry();
    test_inheritance();
    test_two_acl_fan_in();
    test_remote_path();
    std::printf(g_failures == 0 ? "\nACL: PASS\n" : "\nACL: FAIL (%d)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
