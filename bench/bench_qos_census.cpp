/**
 * @file
 * @brief Which vertices carry an extension block, and how many of those hold a storage
 *        policy byte-identical to the default — the census RFC-0022 §3.C turns on.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This file was written to price #617 (intern the QoS profile) against a 24 B `settings_t`.
 * RFC-0022 SUPERSEDED that proposal by deleting most of the struct: `settings_t` is now the
 * two storage magnitudes, 8 B, and the four delivery knobs an intern table would have shared
 * no longer exist. What survives is the question the census actually answers, which RFC-0022
 * made sharper rather than moot: **which vertex shapes are forced to carry an extension block
 * by something that says nothing about their storage policy** — because §3.C's
 * copy-at-registration inheritance materialises an ext block on every descendant of an
 * overriding vertex, so an override's RAM cost is exactly the ext blocks it creates.
 *
 * @section how How the census is taken without touching vertex_t
 *
 * `vertex_t::settings()` returns `kDefaultSettings` **by reference** when the vertex has no
 * extension block, and `ext->settings` when it does. So the two facts separate by comparing an
 * address against a value:
 *
 *   - `&v.settings() == &kDefaultSettings`  ⇒ NO ext. The pay-nothing shape.
 *   - address differs, value equals default  ⇒ ext-bearing, DEFAULT policy. 8 B that are
 *     byte-identical to a constant, carried for a reason unrelated to storage.
 *   - address differs, value differs         ⇒ ext-bearing, CUSTOM policy. The ext is
 *     carrying something.
 *
 * No accessor was added for this. A census that required widening the surface it measures
 * would be a worse instrument.
 *
 * @section shapes What forces an extension block
 *
 * `adopt_identity` skips the allocation only when ALL of: non-STREAM role, no handlers,
 * `settings == kDefaultSettings`, and no existing ext. So an ext is forced by being a STREAM,
 * by carrying a handler, by holding an ACL, or — since RFC-0022 §3.C — by INHERITING a
 * non-default storage policy from an ancestor. Those are the shapes swept below.
 *
 * @section reading Reading it
 *
 * The output is a ratio, not a latency, and it prices RAM: `sizeof(vertex_ext_t)` per vertex
 * in the ext-bearing buckets. It says nothing about the settings READ — that is one inline
 * load by construction (§3.C is copy-at-registration precisely so it stays one), and
 * `bench_libtracer`'s write/store arms are what measure it.
 */

#include <bit>
#include <cstddef>
#include <cstdio>
#include <string>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::kDefaultSettings;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::settings_t;
using tr::graph::vertex_handle_t;

/** @brief The three buckets a vertex can fall into. */
struct census_t {
    int no_ext = 0;      /**< @brief No extension block — 0 B of settings, and no block. */
    int ext_default = 0; /**< @brief Ext-bearing, default policy — 8 B of a constant. */
    int ext_custom = 0;  /**< @brief Ext-bearing, custom policy — the ext carries something. */
};

/**
 * @brief Classify one vertex by comparing the settings ADDRESS, then the settings VALUE.
 *
 * `vertex_handle_t::get()` is private to `graph_t` by design, so a bench reaches the vertex the
 * way `bench_lkv_slot` already does — the handle is a single `vertex_t*` and nothing else, so
 * the cast is exact. Kept to benches: it is a measurement escape hatch, not an API.
 */
void classify(const vertex_handle_t& v, census_t& c) {
    const settings_t& s = std::bit_cast<tr::graph::vertex_t*>(v)->settings();
    if (&s == &kDefaultSettings) {
        ++c.no_ext;
    } else if (s == kDefaultSettings) {
        ++c.ext_default;
    } else {
        ++c.ext_custom;
    }
}

constexpr int kPer = 64;

}  // namespace

int main() {
    std::printf(
        "Storage-policy census (RFC-0022 §3.C) — an ext-bearing vertex with the DEFAULT\n"
        "policy stores 8 B that are byte-identical to a constant, inside a block it was\n"
        "forced to allocate for another reason. Which shapes are those, and how many?\n\n");
    std::printf("%-22s %-9s %-13s %-12s %s\n", "shape", "no ext", "ext+default", "ext+custom",
                "what forces the ext");

    census_t total;

    // Plain stored-value leaf: the case adopt_identity already optimises away.
    {
        graph_t g;
        census_t c;
        for (int i = 0; i < kPer; ++i) {
            const std::string p = "/plain/v" + std::to_string(i);
            classify(g.register_vertex(path_t(p), role_t::STORED_VALUE), c);
        }
        std::printf("%-22s %-9d %-13d %-12d %s\n", "stored-value leaf", c.no_ext, c.ext_default,
                    c.ext_custom, "nothing — no ext at all");
        total.no_ext += c.no_ext;
        total.ext_default += c.ext_default;
        total.ext_custom += c.ext_custom;
    }

    // Handler vertex, policy untouched: an ext is forced by the handler alone.
    {
        graph_t g;
        census_t c;
        for (int i = 0; i < kPer; ++i) {
            const std::string p = "/handler/v" + std::to_string(i);
            tr::graph::handlers_t h;
            h.on_read = [] { return tr::view::view_t{}; };
            classify(g.register_vertex(path_t(p), role_t::HANDLER, std::move(h)), c);
        }
        std::printf("%-22s %-9d %-13d %-12d %s\n", "handler, default policy", c.no_ext,
                    c.ext_default, c.ext_custom, "the handler");
        total.no_ext += c.no_ext;
        total.ext_default += c.ext_default;
        total.ext_custom += c.ext_custom;
    }

    // STREAM with an explicit ring depth — the shape that lands in the COSTS-MORE bucket,
    // because history_keep_last is itself a settings field.
    {
        graph_t g;
        census_t c;
        settings_t s;
        s.history_keep_last = 8;
        for (int i = 0; i < kPer; ++i) {
            const std::string p = "/stream/v" + std::to_string(i);
            classify(g.register_vertex(path_t(p), role_t::STREAM, {}, s), c);
        }
        std::printf("%-22s %-9d %-13d %-12d %s\n", "STREAM, keep_last=8", c.no_ext, c.ext_default,
                    c.ext_custom, "STREAM role AND a non-default policy");
        total.no_ext += c.no_ext;
        total.ext_default += c.ext_default;
        total.ext_custom += c.ext_custom;
    }

    // STREAM at default depth: the role forces the ext, the policy is untouched.
    {
        graph_t g;
        census_t c;
        for (int i = 0; i < kPer; ++i) {
            const std::string p = "/streamd/v" + std::to_string(i);
            classify(g.register_vertex(path_t(p), role_t::STREAM), c);
        }
        std::printf("%-22s %-9d %-13d %-12d %s\n", "STREAM, default policy", c.no_ext,
                    c.ext_default, c.ext_custom, "the STREAM role alone");
        total.no_ext += c.no_ext;
        total.ext_default += c.ext_default;
        total.ext_custom += c.ext_custom;
    }

    // An explicit policy on an ordinary leaf: the ext is forced BY the settings, so this
    // shape can never land in the default bucket.
    {
        graph_t g;
        census_t c;
        settings_t s;
        s.store_ref_min_bytes = 256;
        for (int i = 0; i < kPer; ++i) {
            const std::string p = "/policy/v" + std::to_string(i);
            classify(g.register_vertex(path_t(p), role_t::STORED_VALUE, {}, s), c);
        }
        std::printf("%-22s %-9d %-13d %-12d %s\n", "explicit threshold", c.no_ext, c.ext_default,
                    c.ext_custom, "the settings themselves");
        total.no_ext += c.no_ext;
        total.ext_default += c.ext_default;
        total.ext_custom += c.ext_custom;
    }

    // RFC-0022 §3.C: a leaf under an OVERRIDING ancestor. Nothing about this vertex asks
    // for an ext — it is a plain default leaf — but it inherits a non-default policy BY
    // VALUE at registration, so it materialises one. This is the RAM an override buys, and
    // it is the whole of what an override costs.
    {
        graph_t g;
        census_t c;
        settings_t s;
        s.store_ref_min_bytes = 256;
        (void)g.register_vertex(path_t("/inh"), role_t::STORED_VALUE, {}, s);
        for (int i = 0; i < kPer; ++i) {
            const std::string p = "/inh/v" + std::to_string(i);
            classify(g.register_vertex(path_t(p), role_t::STORED_VALUE), c);
        }
        std::printf("%-22s %-9d %-13d %-12d %s\n", "leaf under override", c.no_ext, c.ext_default,
                    c.ext_custom, "the INHERITED policy (§3.C)");
        total.no_ext += c.no_ext;
        total.ext_default += c.ext_default;
        total.ext_custom += c.ext_custom;
    }

    const int ext = total.ext_default + total.ext_custom;
    std::printf("\nTOTAL  no-ext %d | ext-bearing %d  (default policy %d, custom %d)\n",
                total.no_ext, ext, total.ext_default, total.ext_custom);

    if (ext == 0) {
        std::printf("VERDICT no ext-bearing vertices in this sweep — the census says nothing.\n");
        return 0;
    }
    std::printf("        sizeof(vertex_ext_t) = %zu B, of which settings_t is %zu B.\n",
                sizeof(tr::graph::vertex_ext_t), sizeof(settings_t));
    std::printf(
        "\nSTRUCTURAL — this part does not depend on the shape mix this file happens to\n"
        "construct:\n"
        "  carried for free : a handler vertex, and a STREAM at default depth. Both are\n"
        "                     forced to carry an ext by something that says NOTHING about\n"
        "                     storage, so their 8 B of policy costs no allocation of its own.\n"
        "  paid for         : a leaf whose ext exists only because it holds — or INHERITS,\n"
        "                     RFC-0022 §3.C — a non-default policy. That vertex pays a whole\n"
        "                     vertex_ext_t. An override's RAM cost is one such block per\n"
        "                     inheriting descendant, which is why §3.C grows only the subtree\n"
        "                     that opted in.\n");
    (void)ext;
    return 0;
}
