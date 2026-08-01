/**
 * @file
 * @brief Which vertex shapes carry a cold extension block — the census RFC-0022 §3.B
 *        sharpened by deleting the parameter that used to force one.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This file was written to price #617 (intern the QoS profile) against a 24 B `settings_t`.
 * RFC-0022 SUPERSEDED that proposal by deleting the struct outright, so there is no profile
 * left to intern and the three-bucket "default vs custom policy" split it measured has no
 * referent. What survives — and is now a cleaner question — is: **which vertex shapes are
 * forced to allocate an extension block, and by what?**
 *
 * @section why Why the answer changed
 *
 * `adopt_identity` used to skip the allocation only when ALL of: non-STREAM role, no
 * handlers, `settings == kDefaultSettings`, and no existing ext. RFC-0022 §3.B dropped the
 * third condition along with the parameter that carried it, so **strictly more vertices stay
 * extension-less than before**: registration can no longer force the block, and neither can
 * an ancestor (nothing is inherited, §3.F). A vertex allocates one only when it is a STREAM,
 * carries a handler, is given app fields or an `:acl`, or when its OWNER declares one of the
 * two storage magnitudes on it.
 *
 * That last case is the only one that costs a block *for storage*, and it is opt-in per
 * vertex by the owner — where before, one ancestor's override materialised a whole
 * `vertex_ext_t` on every descendant of its subtree.
 *
 * @section how How the census is taken
 *
 * Through `vertex_t::has_extension_block()`, a named observable. It replaced the previous
 * idiom — comparing the ADDRESS `settings()` returned against the shared `kDefaultSettings`
 * constant — because RFC-0022 deleted both halves of that comparison. `vertex_handle_t::get()`
 * is private to `graph_t` by design, so a bench reaches the vertex the way `bench_lkv_slot`
 * already does: the handle is a single `vertex_t*` and nothing else, so the `bit_cast` is
 * exact. Kept to benches and tests — a measurement escape hatch, not an API.
 *
 * @section reading Reading it
 *
 * The output is a ratio, not a latency, and it prices RAM: `sizeof(vertex_ext_t)` per vertex
 * in the ext-bearing bucket. It says nothing about the storage READ — that is one inline load
 * by construction, and `bench_libtracer`'s write/store arms are what measure it.
 */

#include <bit>
#include <cstddef>
#include <cstdio>
#include <string>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;

/** @brief The two buckets a vertex can fall into. */
struct census_t {
    int no_ext = 0;    /**< @brief No extension block — the pay-nothing shape. */
    int ext_borne = 0; /**< @brief Ext-bearing — a whole `vertex_ext_t` per vertex. */
};

/** @brief Classify one vertex by whether its cold block exists. */
void classify(const vertex_handle_t& v, census_t& c) {
    if (std::bit_cast<tr::graph::vertex_t*>(v)->has_extension_block()) {
        ++c.ext_borne;
    } else {
        ++c.no_ext;
    }
}

/** @brief Print one shape's row and fold it into the running total. */
void report(const char* shape, const census_t& c, const char* forces, census_t& total) {
    std::printf("%-24s %-9d %-13d %s\n", shape, c.no_ext, c.ext_borne, forces);
    total.no_ext += c.no_ext;
    total.ext_borne += c.ext_borne;
}

constexpr int kPer = 64;

}  // namespace

int main() {
    std::printf(
        "Extension-block census (RFC-0022 §3.B) — a vertex that allocates a vertex_ext_t\n"
        "pays for it whole. Which shapes are forced to, and by what?\n\n");
    std::printf("%-24s %-9s %-13s %s\n", "shape", "no ext", "ext-bearing", "what forces the ext");

    census_t total;

    // Plain stored-value leaf: the case adopt_identity optimises away.
    {
        graph_t g;
        census_t c;
        for (int i = 0; i < kPer; ++i) {
            const std::string p = "/plain/v" + std::to_string(i);
            classify(g.register_vertex(path_t(p), role_t::STORED_VALUE), c);
        }
        report("stored-value leaf", c, "nothing — no ext at all", total);
    }

    // Handler vertex: an ext is forced by the handler alone.
    {
        graph_t g;
        census_t c;
        for (int i = 0; i < kPer; ++i) {
            const std::string p = "/handler/v" + std::to_string(i);
            tr::graph::handlers_t h;
            h.on_read = [] { return tr::view::view_t{}; };
            classify(g.register_vertex(path_t(p), role_t::HANDLER, std::move(h)), c);
        }
        report("handler", c, "the handler", total);
    }

    // STREAM: the role forces the ext, and the ring depth then rides it for free.
    {
        graph_t g;
        census_t c;
        for (int i = 0; i < kPer; ++i) {
            const std::string p = "/stream/v" + std::to_string(i);
            const vertex_handle_t v = g.register_vertex(path_t(p), role_t::STREAM);
            g.set_history_depth(v, 8);
            classify(v, c);
        }
        report("STREAM, depth 8", c, "the STREAM role (the depth is free)", total);
    }

    // An owner-declared threshold on an ordinary leaf: the ONE shape where storage itself
    // buys the block. Opt-in, per vertex, by the owner.
    {
        graph_t g;
        census_t c;
        for (int i = 0; i < kPer; ++i) {
            const std::string p = "/policy/v" + std::to_string(i);
            const vertex_handle_t v = g.register_vertex(path_t(p), role_t::STORED_VALUE);
            g.set_store_ref_min_bytes(v, 256);
            classify(v, c);
        }
        report("owner-declared pin", c, "the declaration itself", total);
    }

    // RFC-0022 §3.F: a leaf under a declaring ancestor. Before Amendment 1 this shape
    // INHERITED the ancestor's policy by value at registration and materialised a whole
    // vertex_ext_t to hold it — an override's RAM cost was one block per descendant. Nothing
    // is inherited now, so the row reads zero ext-bearing: that IS the finding.
    {
        graph_t g;
        census_t c;
        const vertex_handle_t root = g.register_vertex(path_t("/inh"), role_t::STORED_VALUE);
        g.set_store_ref_min_bytes(root, 256);
        for (int i = 0; i < kPer; ++i) {
            const std::string p = "/inh/v" + std::to_string(i);
            classify(g.register_vertex(path_t(p), role_t::STORED_VALUE), c);
        }
        report("leaf under a declarer", c, "nothing — §3.F inherits nothing", total);
    }

    std::printf("\nTOTAL  no-ext %d | ext-bearing %d\n", total.no_ext, total.ext_borne);

    if (total.ext_borne == 0) {
        std::printf("VERDICT no ext-bearing vertices in this sweep — the census says nothing.\n");
        return 0;
    }
    std::printf("        sizeof(vertex_ext_t) = %zu B per ext-bearing vertex.\n",
                sizeof(tr::graph::vertex_ext_t));
    std::printf(
        "\nSTRUCTURAL — this part does not depend on the shape mix this file happens to\n"
        "construct:\n"
        "  carried for free : a handler vertex and a STREAM. Both are forced to carry an ext\n"
        "                     by something that says NOTHING about storage, so a ring depth\n"
        "                     declared on a STREAM costs no allocation of its own.\n"
        "  paid for         : a leaf whose ext exists only because its OWNER declared a\n"
        "                     storage magnitude on it. That vertex pays a whole vertex_ext_t,\n"
        "                     and it is the only shape that does — a declaration reaches\n"
        "                     exactly the vertex it names, never a subtree.\n");
    return 0;
}
