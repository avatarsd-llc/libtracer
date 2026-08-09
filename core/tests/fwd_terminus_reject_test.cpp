/**
 * @file
 * @brief The terminus resolver's malformed-`FWD` rejection surface — where every remote
 *        operation lands, and which nothing asserted.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A mutation sweep over `core/src/op_resolve_walk.hpp` wrapped each structural rejection as
 * `if (false && (COND))` — matching parens, because `if (false && A || B)` would still fire
 * for the `||` conditions here — rebuilt, and ran the whole suite. **Two of eleven were
 * caught.** Nine guards could have been deleted and every test would still pass.
 *
 * `parse_fwd` is the door every remote `read` / `write` / `await` / subscribe enters
 * through. Its checks are what stand between a peer's bytes and a `graph_t` mutation, and
 * they were reachable only through well-formed frames — the existing FWD tests all build
 * their frames with the same helper that cannot produce a wrong one.
 *
 * @section shapes What is asserted
 *
 * Rejection takes **two different shapes**, and which one applies is not arbitrary — it is
 * whether the resolver has a trustworthy return route yet:
 *
 *   - **`parse_fwd` failures drop BY VALUE** (`resolve()` → `INVALID_PATH`, no frame). The
 *     frame is malformed at the level that carries `src`, so there is nowhere to reply to.
 *   - **Everything after it ANSWERS** — an assembled reply with `kind=ERROR` and the
 *     registered code. `src` parsed, so the peer can be told why: a non-`NAME` inside a
 *     `dst` `PATH` and an over-deep `FIELD` both come back as `tr::path::invalid` (0x0021),
 *     a payload-less `WRITE` as `tr::schema::type_mismatch` (0x0030).
 *
 * Both shapes are pinned below. The split is the interesting part: it is the difference
 * between a peer that learns its frame was rejected and one that sees a silent timeout.
 *
 * Every case is followed by a **positive control** on the same resolver — a well-formed
 * `FWD{READ}` that must still resolve. A guard mutated into rejecting everything would
 * satisfy the rejection checks and fail there.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::op_resolver_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;

std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, s);
    return out;
}

std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

/** @brief An opaque `VALUE` TLV over @p raw. */
std::vector<std::byte> b_value(std::span<const std::byte> raw) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, raw);
    return out;
}

std::vector<std::byte> b_value_u8(std::uint8_t v) {
    const std::array<std::byte, 1> raw{std::byte{v}};
    return b_value(raw);
}

/**
 * @brief An `FWD` from an arbitrary child run — so the children can be WRONG on purpose.
 *
 * @ref tr::testing::b_fwd emits a correct op/dst/src triple by construction, which is exactly
 * why `parse_fwd`'s type checks had never been reached; this is the envelope alone.
 */
using tr::testing::fwd_envelope;

/** @brief A well-formed `FWD`, the shape everything else deviates from. */
using tr::testing::b_fwd;

std::vector<std::byte> cat(std::initializer_list<std::vector<std::byte>> parts) {
    std::vector<std::byte> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

tr::graph::result_t<tr::view::rope_t> resolve_bytes(op_resolver_t& r,
                                                    std::span<const std::byte> fwd) {
    const auto arena = tr::wire::decode_into(fwd, tr::mem::heap_source());
    if (!arena) return std::unexpected(status_t::INVALID_PATH);
    return r.resolve(*arena, {});
}

/** @brief True when `resolve` refused BY VALUE with `INVALID_PATH` (no reply assembled). */
bool refused(const tr::graph::result_t<tr::view::rope_t>& r) {
    return !r.has_value() && r.error() == status_t::INVALID_PATH;
}

/** @brief The reply's `kind`, and for `ERROR` the registered u16 code (RFC-0002 §C). */
struct reply_info_t {
    std::uint8_t kind = 0xFF;
    std::uint16_t code = 0;
};

reply_info_t reply_info(const tr::view::rope_t& reply) {
    const tr::view::view_t flat = reply.flatten();
    const auto dec = tr::wire::decode(flat.bytes());
    reply_info_t out;
    if (!dec || dec->children.size() < 4) return out;
    out.kind = tr::detail::load_le<std::uint8_t>(dec->children[3].payload);
    if (dec->children.size() >= 5) {
        const tr::wire::tlv_t& status = dec->children[4];
        if (status.type == type_t::STATUS && !status.children.empty() &&
            status.children[0].type == type_t::ERROR && !status.children[0].children.empty()) {
            out.code = tr::detail::load_le<std::uint16_t>(status.children[0].children[0].payload);
        }
    }
    return out;
}

/** @brief True when `resolve` ANSWERED with `kind=ERROR` carrying @p code. */
bool answered_error(const tr::graph::result_t<tr::view::rope_t>& r, std::uint16_t code) {
    if (!r) return false;
    const reply_info_t info = reply_info(*r);
    return info.kind == static_cast<std::uint8_t>(tr::graph::reply_kind_t::ERROR) &&
           info.code == code;
}

/** @brief The positive control: a well-formed READ must still resolve on this resolver. */
void still_works(op_resolver_t& r) {
    const auto ok = resolve_bytes(r, b_fwd(fwd_op_t::READ, b_path({"sensor"}), b_path({"back"})));
    check(ok.has_value(), "  ...and a well-formed FWD{READ} still resolves");
}

/** @brief Send @p bad, require a by-value `INVALID_PATH`, then require the resolver usable. */
void must_refuse(op_resolver_t& r, const char* what, const std::vector<std::byte>& bad) {
    check(refused(resolve_bytes(r, bad)), what);
    still_works(r);
}

}  // namespace

int main() {
    std::printf("FWD terminus rejection surface (op_resolve_walk.hpp)\n\n");

    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sensor"), role_t::STORED_VALUE);
    op_resolver_t r(g);

    // Baseline: the vehicle works before anything is deviated.
    still_works(r);

    // :203 — the root must be a STRUCTURED FWD.
    {
        std::vector<std::byte> not_fwd;
        tr::wire::emit_tlv(not_fwd, type_t::VALUE, opt_t{}, b_name("x"));
        must_refuse(r, "a non-FWD root is refused", not_fwd);
        must_refuse(r, "an FWD with PL=0 (opaque, not a child run) is refused",
                    fwd_envelope(cat({b_value_u8(0), b_path({"sensor"}), b_path({"back"})}),
                                 /*structured=*/false));
    }

    // :209 — child[0] (the op discriminant) must be an opaque VALUE.
    must_refuse(r, "an FWD whose op child is a NAME, not a VALUE, is refused",
                fwd_envelope(cat({b_name("read"), b_path({"sensor"}), b_path({"back"})})));

    // :213 — child[1] (dst) must be a PATH.
    must_refuse(r, "an FWD whose dst child is a NAME, not a PATH, is refused",
                fwd_envelope(cat({b_value_u8(static_cast<std::uint8_t>(fwd_op_t::READ)),
                                  b_name("sensor"), b_path({"back"})})));

    // :221 — src must be a PATH, and it is REQUIRED (a two-child FWD has no return route).
    must_refuse(r, "an FWD whose src child is a VALUE, not a PATH, is refused",
                fwd_envelope(cat({b_value_u8(static_cast<std::uint8_t>(fwd_op_t::READ)),
                                  b_path({"sensor"}), b_value_u8(9)})));
    must_refuse(r, "an FWD with no src child at all is refused",
                fwd_envelope(cat(
                    {b_value_u8(static_cast<std::uint8_t>(fwd_op_t::READ)), b_path({"sensor"})})));

    // :253 — a PATH's children MUST be NAME (the invariant RFC-0004 §C restates).
    {
        std::vector<std::byte> bad_path;
        tr::wire::emit_tlv(bad_path, type_t::PATH, opt_t{.pl = true}, b_value_u8(7));
        check(answered_error(resolve_bytes(r, b_fwd(fwd_op_t::READ, bad_path, b_path({"back"}))),
                             0x0021),
              "a dst PATH whose child is a VALUE, not a NAME, is ANSWERED tr::path::invalid");
        still_works(r);
    }

    // :302 — the FIELD chain is depth-bounded at kMaxFieldDepth (8, path.hpp).
    {
        std::vector<std::byte> deep;
        for (int i = 0; i < 9; ++i) {
            const std::string lvl = "l" + std::to_string(i);
            const std::vector<std::byte> n = b_name(lvl);
            deep.insert(deep.end(), n.begin(), n.end());
        }
        std::vector<std::byte> sel;
        tr::wire::emit_tlv(sel, type_t::FIELD, opt_t{.pl = true}, deep);
        check(answered_error(resolve_bytes(r, b_fwd(fwd_op_t::READ, b_path({"sensor"}),
                                                    b_path({"back"}), sel)),
                             0x0021),
              "a FIELD 9 levels deep (kMaxFieldDepth=8) is ANSWERED tr::path::invalid");
        still_works(r);
    }

    // :253 — a FIELD selector's LEVELS must each lead with a NAME. Distinct from :553,
    // which guards a PATH's segments: different TLV, different function, and the PATH case
    // above does not reach this one.
    {
        std::vector<std::byte> bad_field;
        tr::wire::emit_tlv(bad_field, type_t::FIELD, opt_t{.pl = true}, b_value_u8(3));
        check(answered_error(resolve_bytes(r, b_fwd(fwd_op_t::READ, b_path({"sensor"}),
                                                    b_path({"back"}), bad_field)),
                             0x0021),
              "a FIELD level leading with a VALUE, not a NAME, is ANSWERED tr::path::invalid");
        still_works(r);
    }

    // :575 — a REPLY is not a request. It arrives at the ORIGINATOR's reply sink, never at
    // a resolver, so one here is a peer sending the wrong frame to the wrong door.
    must_refuse(r, "an FWD carrying op=REPLY is refused by the request resolver",
                b_fwd(fwd_op_t::REPLY, b_path({"sensor"}), b_path({"back"})));

    // :662 — a WRITE with no payload. Structurally well-formed enough to answer, so unlike
    // the cases above this one comes back as an assembled reply, not a by-value refusal.
    {
        std::printf("a WRITE with no payload answers ERROR{type_mismatch}, not success:\n");
        const auto rep =
            resolve_bytes(r, b_fwd(fwd_op_t::WRITE, b_path({"sensor"}), b_path({"back"})));
        check(rep.has_value(), "  a payload-less WRITE is answered (a reply is assembled)");
        if (rep) {
            const reply_info_t info = reply_info(*rep);
            check(info.kind == static_cast<std::uint8_t>(tr::graph::reply_kind_t::ERROR),
                  "  the reply is kind=ERROR");
            check(info.code == 0x0030, "  carrying tr::schema::type_mismatch (0x0030)");
        }
        still_works(r);
    }

    // The write surface must be intact: none of the above wrote anything to /sensor.
    check(!g.read(*g.find(path_t::parse("/sensor")->key())).has_value(),
          "no malformed frame wrote a value into /sensor");

    return tr::testing::summary("fwd_terminus_reject");
}
