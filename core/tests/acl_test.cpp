/**
 * @file
 * @brief Per-endpoint :acl — storage (#81-A) + core-subset enforcement (#81, ADR-0018/ 0020/0026).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The graph parses an ACL TLV into ALLOW-only ACEs at write time and stores THAT
 * LIST ALONE (a DENY ACE or unsupported flag bits are rejected with TYPE_MISMATCH
 * so subset evaluation never silently weakens the written semantics; a read
 * re-encodes the list, so it cannot describe a different policy — #907).
 * Enforcement is opt-in twice over: no subject resolver =>
 * everything allowed (today's behavior); an empty effective ACL => open. With a
 * resolver installed the gates are READ / WRITE / SUBSCRIBE (producer fan-out) /
 * CREATE / READ_ACL / WRITE_ACL, denial = PERMISSION_DENIED, and fan-in
 * re-dispatch is gated by the TARGET's WRITE right under the edge's stored
 * caller context. The remote path (op_resolver_t with an inbound_link) replies
 * kind=ERROR STATUS{ERROR{VALUE 0x0050 tr::access::denied}}.
 *
 * The resolver's ERROR arm is a DENY at every one of those gates (#905): the
 * trusted channel is the EMPTY caller context and nothing else, settled before
 * the resolver is invoked at all.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
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

/** @brief Bytes of a TLV's outer envelope `<type><opt><u16 length>` — what @ref primitive_acl
 *         strips off an `encode_acl` result to recover the ACE collection alone. */
constexpr std::size_t kTlvEnvelope = 4;

/**
 * @brief @p canonical's ACE collection re-headed as a PRIMITIVE ACL — `opt.pl` cleared (#907).
 *
 * The decoder populates children only under `opt.pl`, so every one of those ACE bytes lands in
 * the node's opaque `payload` and the tree has ZERO children — which `parse_acl` reads as an
 * empty ACE list. One bit apart from a real ACL, and the whole difference between "this vertex
 * is closed to everyone but peer-a" and "this vertex is wide open".
 */
std::vector<std::byte> primitive_acl(std::span<const std::byte> canonical) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::ACL, opt_t{}, canonical.subspan(kTlvEnvelope));
    return out;
}

/** @brief A container ACL whose single child is a VALUE, not an ACE — the outer is structured,
 *         but what it collects is not an ACE collection. */
std::vector<std::byte> acl_with_non_ace_child() {
    const std::byte junk[1] = {std::byte{0x01}};
    std::vector<std::byte> body;
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, junk);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::ACL, opt_t{.pl = true}, body);
    return out;
}

/**
 * @brief One ALLOW ACE with `access_mask` spelled in TWO bytes and no `flags` child at all.
 *
 * Both are legal: `parse_acl` accepts a payload narrower than the parsed width (little-endian
 * zero-extension is exact — the `acl/acl-aces` conformance vector spells the mask this way) and
 * `flags` is optional. Neither is what `encode_acl` emits, which makes this the witness that an
 * `:acl` read RE-ENCODES the parsed ACEs rather than echoing the written bytes (#907).
 */
std::vector<std::byte> acl_narrow_mask(std::string_view subject, std::uint32_t mask) {
    std::vector<std::vector<std::byte>> keep;
    tlv_t ace{.type = type_t::ACL, .opt = opt_t{.pl = true}};
    ace.children.push_back(name_tlv("type", keep));
    ace.children.push_back(u_value(0, 1, keep));
    ace.children.push_back(name_tlv("subject", keep));
    keep.push_back(as_bytes(subject));
    ace.children.push_back(tlv_t{.type = type_t::VALUE, .payload = keep.back()});
    ace.children.push_back(name_tlv("access_mask", keep));
    ace.children.push_back(u_value(mask, 2, keep));
    tlv_t acl{.type = type_t::ACL, .opt = opt_t{.pl = true}};
    acl.children.push_back(std::move(ace));
    return tr::wire::encode(acl);
}

/** @brief The bytes an `:acl` read served under the trusted local context, or `nullopt` if the
 *         read failed (which for `:acl` means NOT_FOUND — no ACL was ever written). */
std::optional<std::vector<std::byte>> acl_readback(graph_t& g, const path_t& p) {
    const auto r = g.read(p);
    if (!r) return std::nullopt;
    const std::span<const std::byte> b = (*r)->only().bytes();
    return std::vector<std::byte>(b.begin(), b.end());
}

/**
 * @brief The test resolver (ADR-0018): the caller context IS the subject token.
 *
 * The empty (local) context never reaches a resolver — `graph_t::acl_allows` settles it
 * as trusted before invoking one (#905) — so the error arm here means DENY, nothing else.
 */
std::expected<subject_token_t, tr::wire::err_t> caller_is_subject(std::string_view caller) {
    return as_bytes(caller);
}

/** @brief The one caller @ref resolver_cannot_name_ghost refuses to name (#905). */
constexpr std::string_view kUnnameable = "ghost";

/**
 * @brief A resolver whose ERROR arm is reachable: it cannot name @ref kUnnameable.
 *
 * The shape an integrator writes for "this caller presented an identity I do not recognise
 * — a stale link name, a revoked peer, a lookup that failed." Under the predecessor
 * `std::optional` signature the only spelling for that was `nullopt`, which the graph read
 * as FULLY TRUSTED and waved through every gate, `WRITE_ACL` and `CREATE` included (#905).
 */
std::expected<subject_token_t, tr::wire::err_t> resolver_cannot_name_ghost(
    std::string_view caller) {
    if (caller == kUnnameable) return std::unexpected(tr::wire::err_t::ACCESS_DENIED);
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

/** @brief A `:children[]` SPEC creating a STORED_VALUE child named @p name (ADR-0017). */
std::vector<std::byte> child_spec(std::string_view name) {
    std::vector<std::byte> body;
    tr::wire::emit_name(body, "type");
    tr::wire::emit_name(body, "stored_value");
    tr::wire::emit_name(body, "name");
    tr::wire::emit_name(body, name);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);
    return out;
}

using tr::testing::b_fwd;

tr::graph::result_t<tr::view::rope_t> resolve_bytes(op_resolver_t& resolver,
                                                    std::span<const std::byte> fwd,
                                                    std::string_view inbound_link = {}) {
    const auto arena = tr::wire::decode_into(fwd, tr::mem::heap_source());
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
        const bool eq = r.has_value() && (*r)->only().bytes().size() == acl.size() &&
                        std::equal(acl.begin(), acl.end(), (*r)->only().bytes().begin());
        check(eq, "read :acl returns the written bytes (they are already canonical)");
    }

    {  // a non-ACL TLV is rejected; storage unchanged
        tlv_t value{.type = type_t::VALUE, .payload = std::span<const std::byte>(acl).first(0)};
        const std::vector<std::byte> not_acl = tr::wire::encode(value);
        const auto w = g.write(path_t("/x:acl"), make_value(not_acl));
        check(!w.has_value() && w.error() == status_t::TYPE_MISMATCH,
              "writing a non-ACL TLV to :acl returns TYPE_MISMATCH");
        const auto r = g.read(path_t("/x:acl"));
        const bool unchanged = r.has_value() && (*r)->only().bytes().size() == acl.size() &&
                               std::equal(acl.begin(), acl.end(), (*r)->only().bytes().begin());
        check(unchanged, "rejected write leaves the stored ACL unchanged");
    }
}

// ---------------------------------------------------------------------------
/**
 * @brief The OUTER `:acl` container's shape, and a read-back that cannot misdescribe it (#907).
 *
 * The severe direction of a lenient ACL parse is not a rejected write — it is an ACCEPTED one
 * that enforces nothing. A PRIMITIVE ACL carries its whole ACE collection as opaque payload, so
 * it decoded with zero children and stored as an EMPTY ACE list: enforcement CLEARED, the most
 * permissive outcome the field has, on a write that reads like it installs a policy. And
 * because the raw bytes were kept beside the parsed list and served back verbatim, an auditor
 * reading `:acl` saw a non-empty ACL — ACEs present, which under the any-present-ACE-closes
 * rule means CLOSED — on a vertex that was wide open. Stored and evaluated disagreed.
 *
 * Both halves are closed here, and the second is the one that generalises: read-back is a
 * RE-ENCODE of the stored ACEs, so there is no second copy left to disagree with the list
 * `acl_allows` walks — not for this shape, and not for the next one.
 */
void test_outer_acl_shape() {
    std::printf("outer :acl container shape + canonical read-back (#907):\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    const vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    const auto acl_field = path_t::parse("/x:acl");
    // A stored value, so a READ that is ALLOWED answers OK rather than NOT_FOUND — the gate
    // rows below distinguish permitted from denied, which an empty vertex cannot show.
    check(write_u8(g, v, 7).has_value(), "the target holds a value (local write, no ACL yet)");

    // A CLOSING policy: peer-a reads the vertex and administers the ACL; peer-b gets nothing.
    const std::vector<std::byte> closed = make_acl({{
        .subject = "peer-a",
        .mask = bit(acl_right_t::READ) | bit(acl_right_t::READ_ACL) | bit(acl_right_t::WRITE_ACL),
    }});
    check(g.write(*acl_field, make_value(closed)).has_value(), "installed a closing :acl");
    check(g.read(v, "peer-a").has_value(), "peer-a may read under it");
    check(denied(g.read(v, "peer-b")), "peer-b may not — the vertex is CLOSED");

    {  // The body's repro: the same ACE collection under a primitive outer header.
        const auto w = g.write(v, acl_field->field(), make_value(primitive_acl(closed)), "peer-a");
        check(fails_with(w, status_t::TYPE_MISMATCH),
              "a PRIMITIVE outer ACL is rejected with TYPE_MISMATCH");
        check(denied(g.read(v, "peer-b")),
              "…enforcement is UNCHANGED — the rejected write did not open the vertex");
        check(acl_readback(g, *acl_field) == closed,
              "…and :acl still reads back exactly the policy that is enforced");
    }
    {  // An empty primitive decodes to the same zero children — same clearing, no payload.
        std::vector<std::byte> bare;
        tr::wire::emit_tlv(bare, type_t::ACL, opt_t{}, std::span<const std::byte>{});
        check(fails_with(g.write(v, acl_field->field(), make_value(bare), "peer-a"),
                         status_t::TYPE_MISMATCH),
              "an EMPTY primitive ACL is rejected too — the builder never emits a primitive one");
        check(denied(g.read(v, "peer-b")), "…enforcement unchanged");
    }
    {  // A STRUCTURED outer whose child is not an ACE. #906's per-entry rule already refuses
       // this, and the row is here because the outer-shape guard must not be mistaken for the
       // whole of the collection's validity — it stays green with that rule in place.
        check(fails_with(
                  g.write(v, acl_field->field(), make_value(acl_with_non_ace_child()), "peer-a"),
                  status_t::TYPE_MISMATCH),
              "a container ACL whose child is not an ACE is rejected");
        check(acl_readback(g, *acl_field) == closed, "…storage untouched by all three");
    }
    {  // Read-back is a re-encode, not an echo: a legal-but-non-canonical write comes back
       // CANONICAL, which is only possible if the read projects the parsed ACE list.
        const std::uint32_t mask = bit(acl_right_t::READ) | bit(acl_right_t::READ_ACL);
        const std::vector<std::byte> narrow = acl_narrow_mask("peer-a", mask);
        const std::vector<std::byte> canonical = make_acl({{.subject = "peer-a", .mask = mask}});
        check(narrow != canonical, "the narrow spelling differs from encode_acl's, byte-wise");
        check(g.write(*acl_field, make_value(narrow)).has_value(),
              "a narrower-but-legal ACE spelling is accepted");
        const auto back = acl_readback(g, *acl_field);
        check(back == canonical, ":acl reads back CANONICAL — the parsed ACEs re-encoded");
        bool reparses = false;
        if (back) {
            if (const auto dec = tr::wire::decode(*back); dec) {
                const auto aces = tr::graph::parse_acl(*dec);
                reparses = aces && aces->size() == 1 && (*aces)[0].access_mask == mask;
            }
        }
        check(reparses, "…and re-parsing it yields exactly the ACE list evaluation walks");
        check(g.read(v, "peer-a").has_value() && denied(g.read(v, "peer-b")),
              "…which is the policy actually enforced");
    }
    {  // The sanctioned clear: a container with zero children. Enforcement goes off, and the
       // field keeps reading back as an ACL that grants nothing — NOT the same fact as a
       // vertex that never had one, which is why the presence bit outlives the ACE list.
        const std::vector<std::byte> empty = tr::graph::encode_acl({});
        check(g.write(*acl_field, make_value(empty)).has_value(),
              "an EMPTY CONTAINER :acl is accepted — clearing stays expressible");
        check(g.read(v, "peer-b").has_value(), "…enforcement is off (no ACE ⇒ open)");
        check(acl_readback(g, *acl_field) == empty,
              "…and :acl reads back as the empty container, not NOT_FOUND");
    }
    (void)g.register_vertex(path_t("/never"), role_t::STORED_VALUE);
    check(fails_with(g.read(path_t("/never:acl")), status_t::NOT_FOUND),
          "a vertex that never had an :acl still reads NOT_FOUND — the two stay distinct");
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
              "the empty-context (local) caller bypasses a non-granting ACL");
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
 * @brief The flat protocol-knob surface is withdrawn CALLER-INDEPENDENTLY on BOTH halves
 *        (RFC-0022 §3.B / §4): every core-namespace `settings.<name>` answers
 *        SCHEMA_NOT_FOUND to a read AND to a write, for every caller, granted or denied.
 *
 * The normative rule is the docs/reference/05 §`0x0B` validation clause — "unknown NAMEs
 * MUST be ... rejected with `ERROR{tr::schema::not_found}` if in the core namespace" —
 * which carries no caller qualifier. With `settings_t` deleted the core namespace is
 * EMPTY, so every name in it is an unknown name and the clause covers the whole branch:
 * a denied caller may not convert the answer into PERMISSION_DENIED, and a granted one
 * may not convert it into a write.
 *
 * The discriminating case is a caller the ACL DENIES: only a name-first answer gives
 * SCHEMA_NOT_FOUND there. The ABLATION is the `settings.app.` branch beside it — same
 * vertex, same two callers, same door — which still gates a DECLARED field on the WRITE
 * right. Without it this test would pass just as well against a `:settings` write door
 * that had been deleted wholesale, app fields and all.
 *
 * The READ half is checked at the SAME three callers, because the two doors used to
 * disagree: the read gate ran BEFORE name resolution, so one removed name answered
 * PERMISSION_DENIED on read and SCHEMA_NOT_FOUND on write — a caller-DEPENDENT split
 * across the halves of one name, which is precisely what §3.B and docs/reference/05
 * §`0x0B` forbid. Its ablation is the same `settings.app.` branch: a declared `rw` field
 * still answers a denied caller PERMISSION_DENIED on read, so the loop below measures the
 * withdrawn namespace and not a read door that stopped gating.
 */
void test_flat_knob_surface_is_withdrawn() {
    std::printf("flat protocol knobs: withdrawn, caller-independently (RFC-0022 §3.B):\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    // peer-a holds WRITE; peer-none holds nothing. Installed by the trusted local caller.
    check(g.write(path_t("/x:acl"),
                  make_value(make_acl({{.subject = "peer-a", .mask = bit(acl_right_t::WRITE)}})))
              .has_value(),
          "trusted local caller installs the :acl");

    const std::array<std::byte, 8> le{std::byte{0x88}, std::byte{0x13}};  // 5000 LE

    // Every name the protocol ever minted under `settings.`, plus one that never existed —
    // all seven RFC-0022 removed, and `bogus`. The answer must not depend on the caller.
    for (const char* name :
         {"history_keep_last", "store_ref_min_bytes", "durability", "reliability", "priority",
          "deadline_ns", "queue_max_bytes", "bogus"}) {
        const auto knob = path_t::parse(std::string("/x:settings.") + name);
        const bool all_three =
            fails_with(g.write(v, knob->field(), make_value(value_tlv(le)), "peer-none"),
                       status_t::SCHEMA_NOT_FOUND) &&
            fails_with(g.write(v, knob->field(), make_value(value_tlv(le)), "peer-a"),
                       status_t::SCHEMA_NOT_FOUND) &&
            fails_with(g.write(v, knob->field(), make_value(value_tlv(le)), {}),
                       status_t::SCHEMA_NOT_FOUND);
        check(all_three, std::string("`:settings.") + name +
                             "`: SCHEMA_NOT_FOUND for denied, granted AND local callers");
        // The READ half of the SAME name, at the SAME three callers. `peer-none` holds
        // nothing at all — not even READ — so it is the caller a gate-first read answers
        // PERMISSION_DENIED, and the one that discriminates a name-first read from a
        // gated one.
        const bool read_all_three =
            fails_with(g.read(v, knob->field(), "peer-none"), status_t::SCHEMA_NOT_FOUND) &&
            fails_with(g.read(v, knob->field(), "peer-a"), status_t::SCHEMA_NOT_FOUND) &&
            fails_with(g.read(v, knob->field(), {}), status_t::SCHEMA_NOT_FOUND);
        check(read_all_three, std::string("`:settings.") + name +
                                  "` READ: SCHEMA_NOT_FOUND for the same three callers");
    }

    // A trailing step / an indexed selector name nothing either — they never did, and the
    // answer stays the same shape now that the bare name names nothing either.
    check(fails_with(g.write(v, path_t::parse("/x:settings.store_ref_min_bytes.bogus")->field(),
                             make_value(value_tlv(le)), "peer-a"),
                     status_t::SCHEMA_NOT_FOUND),
          "a trailing step names nothing: SCHEMA_NOT_FOUND");
    check(fails_with(g.write(v, path_t::parse("/x:settings.store_ref_min_bytes[2]")->field(),
                             make_value(value_tlv(le)), "peer-a"),
                     status_t::SCHEMA_NOT_FOUND),
          "a selector step names nothing: SCHEMA_NOT_FOUND");

    // THE ABLATION. The `settings.app.` branch is untouched by RFC-0022, so on the SAME
    // vertex the SAME two callers still get the RFC-0010 §A.3 answers: a declared `rw`
    // field is PERMISSION_DENIED for the denied caller and lands for the granted one.
    // This is what proves the loop above measured the knob namespace and not a dead door.
    std::vector<tr::graph::app_field_t> table;
    table.push_back(tr::graph::app_field_t{.name = "kp", .access = tr::graph::app_access_t::RW});
    g.set_app_fields(v, std::move(table));
    const auto field = path_t::parse("/x:settings.app.kp");
    check(denied(g.write(v, field->field(), make_value(value_tlv(le)), "peer-none")),
          "ablation: a declared app field is still PERMISSION_DENIED for a denied caller");
    check(g.write(v, field->field(), make_value(value_tlv(le)), "peer-a").has_value(),
          "ablation: ... and still LANDS for a granted one (the door is alive)");
    // The READ ablation. `peer-none` holds no rights at all, so a READ of the app field it
    // is NOT allowed must still be PERMISSION_DENIED — that is what proves the loop's
    // read half measured the withdrawn core namespace and not a read gate that vanished.
    check(denied(g.read(v, field->field(), "peer-none")),
          "ablation: a declared app field READ is still PERMISSION_DENIED for a denied caller");
    // And the bare `:settings` container stays gated too: it is a KNOWN name, so the
    // name-first arm must not have swallowed it.
    const auto container = path_t::parse("/x:settings");
    check(denied(g.read(v, container->field(), "peer-none")),
          "ablation: the bare `:settings` container READ is still gated (a KNOWN name)");
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
        const auto fwd = b_fwd(fwd_op_t::WRITE, b_path({"x"}), b_path({"ret"}), {}, payload);
        const auto reply = resolve_bytes(resolver, fwd, "link-bad");
        const reply_info_t info = reply_info(*reply);
        check(reply.has_value() && info.kind == reply_kind_t::ERROR && info.code == 0x0050,
              "denied FWD WRITE replies kind=ERROR STATUS{ERROR{VALUE 0x0050}}");
    }
    {  // the same WRITE from the granted link succeeds
        const auto fwd = b_fwd(fwd_op_t::WRITE, b_path({"x"}), b_path({"ret"}), {}, payload);
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

/**
 * @brief The ACL gates a mutation sweep found NO test defends.
 *
 * Disabling each `acl_allows` call site in `core/src/graph.cpp` one at a time — turning
 * `if (!acl_allows(...))` into `if (false && !acl_allows(...))` — and running the whole
 * suite caught **14 of 20**. The six that survived meant the gate could be deleted
 * outright and every test would still pass. Five were real; this covers them.
 *
 * (The sixth, `history()`'s gate at `graph.cpp:1255`, passes a hardcoded EMPTY caller
 * because it is a local-only helper with no wire path. Since #905 that is STRUCTURALLY
 * un-deniable — the empty context is the trusted channel, settled before any resolver is
 * consulted — rather than a property of the resolvers we happen to ship. Correct by
 * design, and deliberately not tested here.)
 *
 * Each gate is asserted in BOTH directions. A one-sided "denied" check would pass just as
 * well against a gate wired to refuse everyone, which is the failure this file exists to
 * catch on the other side.
 */
void test_gates_no_test_defended() {
    std::printf("the ACL gates a mutation sweep found undefended:\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    const vertex_handle_t v = g.register_vertex(path_t("/g"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/g/kid"), role_t::STORED_VALUE);
    (void)write_u8(g, v, 7);  // trusted local write seeds an LKV

    check(
        g.write(path_t("/g:acl"),
                make_value(make_acl({{.subject = "peer-a",
                                      .mask = bit(acl_right_t::READ) | bit(acl_right_t::WRITE)}})))
            .has_value(),
        "installed an ALLOW-only :acl granting peer-a READ+WRITE");

    // graph.cpp:973 — assign() is RFC-0008's STATE half. It is a full write of the
    // last-known value and carries its own WRITE gate, separate from write_impl's.
    {
        std::vector<std::byte> one;
        tr::wire::emit_tlv(one, type_t::VALUE, opt_t{}, as_bytes("1"));
        std::vector<std::byte> two;
        tr::wire::emit_tlv(two, type_t::VALUE, opt_t{}, as_bytes("2"));
        check(g.assign(v, tr::view::rope_t{make_value(one)}, "peer-a").has_value(),
              "assign() allowed for a WRITE-granted subject");
        check(denied(g.assign(v, tr::view::rope_t{make_value(two)}, "peer-b")),
              "assign() DENIED without the WRITE bit");
    }

    // graph.cpp:1485 — the `:subscribers[N]` INDEXED write (RFC-0009 D.1 clear/replace).
    // Distinct from the `[]` append gate, which rides SUBSCRIBE and is already covered:
    // this one rides WRITE, and it is what stops an unauthorized peer from clearing a
    // subscriber slot out from under its owner.
    {
        const auto slot = path_t::parse("/g:subscribers[0]");
        std::vector<std::byte> evict;  // the empty-STATUS eviction sentinel
        tr::wire::emit_tlv(evict, type_t::STATUS, opt_t{}, std::span<const std::byte>{});
        check(denied(g.write(v, slot->field(), make_value(evict), "peer-b")),
              "an indexed :subscribers[N] write is DENIED without the WRITE bit");
        check(!denied(g.write(v, slot->field(), make_value(evict), "peer-a")),
              "and is not denied for a WRITE-granted subject");
    }

    // graph.cpp:1945 — read_subtree_folded's ROOT gate. The composed branch read walks
    // descendants and prunes per-child (that inner gate at :2012 was already covered);
    // nothing covered the root itself, so the whole subtree was readable without READ.
    check(g.read_subtree_folded(v, "peer-a").has_value(),
          "a folded subtree read is allowed for a READ-granted subject");
    check(denied(g.read_subtree_folded(v, "peer-b")),
          "a folded subtree read is DENIED at the ROOT without the READ bit");

    // graph.cpp:2087 — the `:children` folded-read gate. It sits ABOVE the shared field
    // gate at :2124 because the folded rope bypasses the single-view wrap, so it needs
    // its own check and had none.
    {
        const auto kids = path_t::parse("/g:children");
        check(g.read(v, kids->field(), "peer-a").has_value(),
              ":children readable by a READ-granted subject");
        check(denied(g.read(v, kids->field(), "peer-b")), ":children DENIED without the READ bit");
    }

    // graph.cpp:2191 — read_subscribers()'s gate. Its own comment calls it "a
    // control-surface read, like `:schema`", but unlike `:schema` it is a DIRECT API, not
    // a field path, so it never passes the :2124 gate above — it is the only thing
    // standing between an unauthorized caller and the whole subscriber list, which is a
    // topology disclosure (who is listening to this vertex, and at what return route).
    //
    // This one was nearly missed: the FIRST mutation run reported it caught. Re-running
    // the mutation alone, three times, showed it survives deterministically — the earlier
    // result was a flake in that run, not a property of the code.
    check(g.read_subscribers(v, "peer-a").has_value(),
          "read_subscribers allowed for a READ-granted subject");
    check(denied(g.read_subscribers(v, "peer-b")),
          "read_subscribers DENIED without the READ bit (topology disclosure)");

    // graph.cpp:2124 — the shared field-read gate, below the pre-auth `:identity` arm.
    // Everything after it (:schema, :settings, :subscribers[N]) depends on it alone.
    {
        const auto schema = path_t::parse("/g:schema");
        check(g.read(v, schema->field(), "peer-a").has_value(),
              ":schema readable by a READ-granted subject");
        check(denied(g.read(v, schema->field(), "peer-b")),
              ":schema DENIED without the READ bit (the shared field-read gate)");
    }
}

/**
 * @brief The resolver's ERROR arm is a DENY at EVERY gate, not a trusted fallback (#905).
 *
 * The vertex here grants `EVERYONE@` every gated right — the most permissive policy the
 * core subset can express — and the unnameable caller is refused anyway, because the
 * refusal happens BEFORE any ACE is looked at. That ordering is the whole fix: the
 * predecessor signature's only non-token answer (`nullopt`) meant "fully trusted", so an
 * integrator's honest "I cannot name this caller" granted `WRITE_ACL` and `CREATE`.
 *
 * Every gate is asserted with an ABLATION on the same door in the same graph — "peer-ok",
 * which the resolver CAN name, must pass it. Without that half a gate wired to refuse
 * everyone would score identically, and a 2026-07-31 mutation sweep found most of these
 * gates carried no test at all. Each is exercised individually: READ, WRITE, SUBSCRIBE,
 * CREATE (both doors — `:children[]` and write-creates), READ_ACL and WRITE_ACL.
 */
void test_resolver_deny_arm_is_denied_at_every_gate() {
    std::printf("the resolver's ERROR arm DENIES at every gate (#905):\n");
    graph_t g;
    g.set_subject_resolver(resolver_cannot_name_ghost);
    const vertex_handle_t v = g.register_vertex(path_t("/g"), role_t::STORED_VALUE);
    (void)write_u8(g, v, 7);  // trusted local write seeds an LKV

    const std::vector<std::byte> open_to_everyone =
        make_acl({{.subject = "EVERYONE@",
                   .mask = bit(acl_right_t::READ) | bit(acl_right_t::WRITE) |
                           bit(acl_right_t::SUBSCRIBE) | bit(acl_right_t::CREATE) |
                           bit(acl_right_t::READ_ACL) | bit(acl_right_t::WRITE_ACL)}});
    check(g.write(path_t("/g:acl"), make_value(open_to_everyone)).has_value(),
          "installed an :acl granting EVERYONE@ every gated right");

    // READ (graph.cpp:809)
    check(g.read(v, "peer-ok").has_value(), "READ: the nameable caller is allowed");
    check(denied(g.read(v, kUnnameable)), "READ: the UNNAMEABLE caller is DENIED");

    // WRITE (graph.cpp:1045)
    check(write_u8(g, v, 1, "peer-ok").has_value(), "WRITE: the nameable caller is allowed");
    check(denied(write_u8(g, v, 2, kUnnameable)), "WRITE: the UNNAMEABLE caller is DENIED");

    // AWAIT rides the READ right, checked before the condvar (graph.cpp:1353).
    {
        const auto ok = g.await(v, std::chrono::nanoseconds(1), "peer-ok");
        check(!denied(ok), "AWAIT: the nameable caller is not denied (it times out instead)");
        check(denied(g.await(v, std::chrono::nanoseconds(1), kUnnameable)),
              "AWAIT: the UNNAMEABLE caller is DENIED before it can camp on the condvar");
    }

    // SUBSCRIBE — the producer-side `:subscribers[]` append gate (graph.cpp:1421).
    {
        std::vector<std::byte> sub;
        tr::wire::emit_tlv(sub, type_t::SUBSCRIBER, opt_t{.pl = true}, b_path({"sink"}));
        const auto field = path_t::parse("/g:subscribers[]");
        check(g.write(v, field->field(), make_value(sub), "peer-ok").has_value(),
              "SUBSCRIBE: the nameable caller may append an edge");
        check(denied(g.write(v, field->field(), make_value(sub), kUnnameable)),
              "SUBSCRIBE: the UNNAMEABLE caller is DENIED");
    }

    // CREATE, door 1 — the in-band `:children[]` append (graph.cpp:1777).
    {
        const auto field = path_t::parse("/g:children[]");
        check(denied(g.write(v, field->field(), make_value(child_spec("ghostkid")), kUnnameable)),
              "CREATE(:children[]): the UNNAMEABLE caller is DENIED");
        check(!g.find(path_t{"/g/ghostkid"}.key()).has_value(),
              "CREATE(:children[]): the denied create made no vertex");
        check(g.write(v, field->field(), make_value(child_spec("okkid")), "peer-ok").has_value(),
              "CREATE(:children[]): the nameable caller may create");
        check(g.find(path_t{"/g/okkid"}.key()).has_value(),
              "CREATE(:children[]): the allowed create really made the vertex");
    }

    // CREATE, door 2 — write-creates, gated on the nearest existing ancestor
    // (graph.cpp:601). A different door from `:children[]`, on the same right.
    {
        const path_t ghost_made{"/g/ghostmade"};
        const path_t ok_made{"/g/okmade"};
        check(denied(g.ensure_vertex(ghost_made.key(), kUnnameable)),
              "CREATE(write-creates): the UNNAMEABLE caller is DENIED");
        check(!g.find(ghost_made.key()).has_value(),
              "CREATE(write-creates): the denied create made no vertex");
        check(g.ensure_vertex(ok_made.key(), "peer-ok").has_value(),
              "CREATE(write-creates): the nameable caller may create");
    }

    // READ_ACL — its own right, distinct from acting on the vertex (graph.cpp:2313).
    {
        const auto acl_field = path_t::parse("/g:acl");
        check(g.read(v, acl_field->field(), "peer-ok").has_value(),
              "READ_ACL: the nameable caller may read :acl");
        check(denied(g.read(v, acl_field->field(), kUnnameable)),
              "READ_ACL: the UNNAMEABLE caller is DENIED");
    }

    // WRITE_ACL — the admin right, and the one the fail-open handed out (graph.cpp:1740).
    // Last, because a successful write REPLACES the policy every check above rode on.
    {
        const auto acl_field = path_t::parse("/g:acl");
        const std::vector<std::byte> hijack =
            make_acl({{.subject = "ghost", .mask = bit(acl_right_t::WRITE_ACL)}});
        check(denied(g.write(v, acl_field->field(), make_value(hijack), kUnnameable)),
              "WRITE_ACL: the UNNAMEABLE caller may NOT rewrite the policy");
        const auto still = g.read(v, acl_field->field(), "peer-ok");
        check(still.has_value() && still->flatten().bytes().size() == open_to_everyone.size(),
              "WRITE_ACL: the denied admin write left the stored ACL in place");
        check(g.write(v, acl_field->field(), make_value(open_to_everyone), "peer-ok").has_value(),
              "WRITE_ACL: the nameable caller may rewrite the policy");
    }
}

/**
 * @brief The ERROR arm also stops REMOTE-EDGE FAN-IN delivery (`graph.cpp:876`, #905).
 *
 * The seventh gate, and the only one no caller can drive directly: a stored edge carries
 * the caller context its subscribe ran under, and every delivery it makes is re-gated at
 * the TARGET under that context. So the interesting moment is an edge that was admitted
 * while its peer was nameable and keeps delivering after it stops being — a revoked link,
 * a resolver that lost its session table. Fail-open made that edge MORE trusted than any
 * live one: it delivered into the target unconditionally.
 *
 * The before/after pair inside one graph is the ablation — the same edge, the same
 * producer write, the only change being the resolver's answer.
 */
void test_deny_arm_stops_remote_fan_in() {
    std::printf("the resolver's ERROR arm stops remote-edge fan-in delivery (#905):\n");
    graph_t g;
    bool revoked = false;
    g.set_subject_resolver(
        [&revoked](std::string_view caller) -> std::expected<subject_token_t, tr::wire::err_t> {
            if (revoked && caller == "link-a")
                return std::unexpected(tr::wire::err_t::ACCESS_DENIED);
            return as_bytes(caller);
        });
    const vertex_handle_t src = g.register_vertex(path_t("/src"), role_t::STORED_VALUE);
    const vertex_handle_t sink = g.register_vertex(path_t("/sink"), role_t::STORED_VALUE);

    // The edge stores "link-a" as its delivery caller. Neither vertex carries an :acl, so
    // nothing but the resolver can refuse anything here.
    std::vector<std::byte> sub;
    tr::wire::emit_tlv(sub, type_t::SUBSCRIBER, opt_t{.pl = true}, b_path({"sink"}));
    const auto sub_field = path_t::parse("/src:subscribers[]");
    check(g.write(src, sub_field->field(), make_value(sub), "link-a").has_value(),
          "link-a subscribes /src -> /sink while it is still nameable");

    check(write_u8(g, src, 0x11).has_value(), "producer write while link-a is nameable");
    check(g.read(sink).has_value(), "fan-in: the delivery LANDED (the edge really works)");
    check(g.delivery_drops().denied == 0, "fan-in: nothing was dropped as denied yet");

    // The peer's identity stops resolving; the edge outlives it.
    revoked = true;
    check(write_u8(g, src, 0x22).has_value(),
          "the producer write itself still succeeds (only the leg drops)");
    check(g.delivery_drops().denied == 1,
          "fan-in: the delivery under the now-UNNAMEABLE caller is DROPPED and counted");
    const auto landed = g.read(sink);
    check(landed.has_value() && (*landed)->only().bytes().back() != std::byte{0x22},
          "fan-in: the second value never reached the target");
}

/**
 * @brief The EMPTY caller context stays trusted, and is settled WITHOUT the resolver (#905).
 *
 * The resolver installed here names nobody at all — every caller it sees is refused. Local
 * (empty-context) operations must still succeed, which they can only do if `acl_allows`
 * short-circuits the empty caller BEFORE invoking it. A remote op always carries its
 * inbound link NAME, so it cannot spell this context.
 */
void test_empty_caller_is_trusted_without_the_resolver() {
    std::printf("the empty (local) caller is trusted without consulting the resolver (#905):\n");
    graph_t g;
    g.set_subject_resolver([](std::string_view) -> std::expected<subject_token_t, tr::wire::err_t> {
        return std::unexpected(tr::wire::err_t::ACCESS_DENIED);
    });
    const vertex_handle_t v = g.register_vertex(path_t("/l"), role_t::STORED_VALUE);

    check(write_u8(g, v, 5).has_value(), "local WRITE succeeds under a name-nobody resolver");
    check(g.read(v).has_value(), "local READ succeeds");
    check(g.write(path_t("/l:acl"),
                  make_value(make_acl({{.subject = "peer-a", .mask = bit(acl_right_t::READ)}})))
              .has_value(),
          "local WRITE_ACL succeeds");
    check(g.read(path_t("/l:acl")).has_value(), "local READ_ACL succeeds");
    check(g.ensure_vertex(path_t{"/l/kid"}.key(), {}).has_value(), "local CREATE succeeds");
    check(g.subscribe(path_t("/l"), path_t("/l/kid")).has_value(),
          "local SUBSCRIBE succeeds (the subscribe() sugar runs under the empty context)");

    // The ablation: the very same resolver refuses every ATTRIBUTED caller.
    check(denied(g.read(v, "peer-a")), "an attributed caller is refused by the same resolver");
    check(denied(write_u8(g, v, 6, "peer-a")), "…on the WRITE gate too");
}

/**
 * @brief The wildcard spelling is RESERVED in the subject-token space (#908): a caller whose
 *        resolver returns `EVERYONE@` is refused, at a guarded vertex and at a bare one.
 *
 * The wire carries ONE spelling for a subject token — the `acl/acl-aces` vector sends `peer-a`
 * and `EVERYONE@` as the same opaque VALUE — so a deployment whose resolver passes a
 * caller-supplied identity through (a username, a cert CN, a peer name) could mint a principal
 * that IS the wildcard, and an ACE meant for that one principal would grant everyone. Nothing
 * used to stop it: `parse_acl` accepts any non-empty subject bytes and no check constrained what
 * a resolver returned, so the reservation existed only in prose. `graph_t::acl_allows` now
 * refuses the token itself — fail closed, like the resolver's own error arm (#905) — so an
 * integrator does not have to know to blacklist it.
 *
 * Two ablations keep the refusals honest: an ORDINARY caller still reads and writes the guarded
 * vertex through the very same wildcard ACE (so this is not a wildcard that stopped working),
 * and an ordinary caller still reads the BARE vertex (so it is not open-by-default breaking).
 */
void test_reserved_wildcard_subject_is_refused() {
    std::printf("the EVERYONE@ spelling is reserved against a resolved subject (#908):\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    const vertex_handle_t guarded = g.register_vertex(path_t("/w"), role_t::STORED_VALUE);
    const vertex_handle_t bare = g.register_vertex(path_t("/o"), role_t::STORED_VALUE);
    (void)write_u8(g, guarded, 7);  // trusted local writes seed both LKVs
    (void)write_u8(g, bare, 7);
    check(
        g.write(path_t("/w:acl"),
                make_value(make_acl({{.subject = "EVERYONE@",
                                      .mask = bit(acl_right_t::READ) | bit(acl_right_t::WRITE)}})))
            .has_value(),
        "installed a wildcard :acl granting EVERYONE@ READ|WRITE");

    // Ablations: the wildcard ACE still grants, and the bare vertex is still open by default.
    check(g.read(guarded, "peer-a").has_value(),
          "an ordinary caller READS the guarded vertex through the wildcard ACE");
    check(write_u8(g, guarded, 1, "peer-a").has_value(), "…and WRITES it");
    check(g.read(bare, "peer-a").has_value(),
          "an ordinary caller READS the bare vertex (open by default)");

    // The reserved token itself, at both gates and at both vertices.
    check(denied(g.read(guarded, "EVERYONE@")),
          "a caller resolving to EVERYONE@ is DENIED READ on the guarded vertex");
    check(denied(write_u8(g, guarded, 2, "EVERYONE@")), "…DENIED WRITE on it too");
    check(denied(g.read(bare, "EVERYONE@")),
          "…and DENIED on the BARE vertex, before the bearing-ancestor walk");
}

}  // namespace

int main() {
    test_storage_roundtrip();
    test_outer_acl_shape();
    test_subset_rejections();
    test_open_by_default();
    test_gated_ops();
    test_flat_knob_surface_is_withdrawn();
    test_acl_and_schema_are_addressed_whole();
    test_expiry();
    test_inheritance();
    test_two_acl_fan_in();
    test_remote_path();
    test_gates_no_test_defended();
    test_resolver_deny_arm_is_denied_at_every_gate();
    test_deny_arm_stops_remote_fan_in();
    test_empty_caller_is_trusted_without_the_resolver();
    test_reserved_wildcard_subject_is_refused();
    std::printf(g_failures == 0 ? "\nACL: PASS\n" : "\nACL: FAIL (%d)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
