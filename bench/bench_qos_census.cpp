/**
 * @file
 * @brief Which vertices carry an extension block, and how many of those hold QoS that is
 *        byte-identical to the default — the census #617 turns on.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `vertex_ext_t` stores a full inline `settings_t` (24 B, and it does not shrink on rv32 —
 * every member is a fixed-width scalar). #617 proposes replacing it with a `const settings_t*`.
 * That is a WIN for a vertex whose QoS is default, which then points at the existing
 * `kDefaultSettings` constant and stores 4 B instead of 24; it is a small LOSS for a vertex
 * with custom QoS, which pays a pointer PLUS a 24 B allocation where it used to pay 24 B
 * inline. So the whole proposal reduces to one ratio, and nothing had measured it.
 *
 * @section how How the census is taken without touching vertex_t
 *
 * `vertex_t::settings()` returns `kDefaultSettings` **by reference** when the vertex has no
 * extension block, and `ext->settings` when it does. So the two facts separate by comparing an
 * address against a value:
 *
 *   - `&v.settings() == &kDefaultSettings`  ⇒ NO ext. Already optimal; #617 cannot improve it.
 *   - address differs, value equals default  ⇒ ext-bearing, DEFAULT QoS. **The waste.** 24 B
 *     that are byte-identical to a constant.
 *   - address differs, value differs         ⇒ ext-bearing, CUSTOM QoS. Costs 4 B more under
 *     the proposal.
 *
 * No accessor was added for this. A census that required widening the surface it measures
 * would be a worse instrument.
 *
 * @section shapes What forces an extension block
 *
 * `adopt_identity` skips the allocation only when ALL of: non-STREAM role, no handlers,
 * `settings == kDefaultSettings`, and no existing ext. So an ext is forced by being a STREAM,
 * by carrying a handler, or by holding an ACL — **independently of QoS**. Those are exactly the
 * shapes that can be ext-bearing while their QoS is untouched, and the shapes swept below.
 *
 * @section reading Reading it
 *
 * The verdict is a ratio, not a latency. It says which of #617's two steps is worth doing:
 * point-at-the-default alone (no intern table, no lifetime protocol, no copy-on-write), or the
 * full intern machinery that also shares distinct profiles. It does NOT say whether the extra
 * indirection on the settings read is affordable — `settings()` is read on the write hot path,
 * and that is `bench_libtracer`'s question, not this file's.
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

/** @brief The three buckets a vertex can fall into, and what #617 does to each. */
struct census_t {
    int no_ext = 0;      /**< @brief No extension block — already 0 B of settings. */
    int ext_default = 0; /**< @brief Ext-bearing, default QoS — 24 B of a constant. */
    int ext_custom = 0;  /**< @brief Ext-bearing, custom QoS — the case that costs more. */
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
        "QoS census (#617) — an ext-bearing vertex with DEFAULT QoS stores 24 B that are\n"
        "byte-identical to a constant. Which shapes are those, and how many?\n\n");
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

    // Handler vertex, QoS untouched: an ext is forced by the handler alone.
    {
        graph_t g;
        census_t c;
        for (int i = 0; i < kPer; ++i) {
            const std::string p = "/handler/v" + std::to_string(i);
            tr::graph::handlers_t h;
            h.on_read = [] { return tr::view::view_t{}; };
            classify(g.register_vertex(path_t(p), role_t::HANDLER, std::move(h)), c);
        }
        std::printf("%-22s %-9d %-13d %-12d %s\n", "handler, default QoS", c.no_ext, c.ext_default,
                    c.ext_custom, "the handler");
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
                    c.ext_custom, "STREAM role AND non-default QoS");
        total.no_ext += c.no_ext;
        total.ext_default += c.ext_default;
        total.ext_custom += c.ext_custom;
    }

    // STREAM at default depth: the role forces the ext, the QoS is untouched.
    {
        graph_t g;
        census_t c;
        for (int i = 0; i < kPer; ++i) {
            const std::string p = "/streamd/v" + std::to_string(i);
            classify(g.register_vertex(path_t(p), role_t::STREAM), c);
        }
        std::printf("%-22s %-9d %-13d %-12d %s\n", "STREAM, default QoS", c.no_ext, c.ext_default,
                    c.ext_custom, "the STREAM role alone");
        total.no_ext += c.no_ext;
        total.ext_default += c.ext_default;
        total.ext_custom += c.ext_custom;
    }

    // Explicit QoS on an ordinary leaf: ext forced BY the settings, so it can never be default.
    {
        graph_t g;
        census_t c;
        settings_t s;
        s.deadline_ns = 1000000;
        for (int i = 0; i < kPer; ++i) {
            const std::string p = "/qos/v" + std::to_string(i);
            classify(g.register_vertex(path_t(p), role_t::STORED_VALUE, {}, s), c);
        }
        std::printf("%-22s %-9d %-13d %-12d %s\n", "explicit deadline", c.no_ext, c.ext_default,
                    c.ext_custom, "the settings themselves");
        total.no_ext += c.no_ext;
        total.ext_default += c.ext_default;
        total.ext_custom += c.ext_custom;
    }

    const int ext = total.ext_default + total.ext_custom;
    std::printf("\nTOTAL  no-ext %d | ext-bearing %d  (default QoS %d, custom %d)\n", total.no_ext,
                ext, total.ext_default, total.ext_custom);

    if (ext == 0) {
        std::printf("VERDICT no ext-bearing vertices in this sweep — the census says nothing.\n");
        return 0;
    }
    // The ratio above is an artifact of the shapes THIS FILE constructs, so it is not the
    // verdict. What is mix-INDEPENDENT is the break-even. Under #617 step 1 on rv32 a
    // default-QoS vertex goes 24 B -> 4 B (saves 20) and a custom-QoS one goes 24 B -> 4 B plus
    // a 24 B allocation (costs 4). So 20*d > 4*(ext - d), i.e. it pays whenever more than ONE
    // IN SIX ext-bearing vertices holds default QoS. That threshold is a property of the
    // sizes, not of any deployment.
    std::printf(
        "VERDICT step 1 (point at kDefaultSettings — no intern table, no lifetime\n"
        "        protocol, no copy-on-write) pays whenever more than 1 in 6 (16.7%%) of\n"
        "        ext-bearing vertices hold DEFAULT QoS. 20 B saved each against 4 B\n"
        "        lost each on rv32. Mix-independent.\n\n");
    std::printf(
        "        STRUCTURAL — this part does not depend on the mix:\n"
        "          recoverable : a handler vertex, and a STREAM at default depth. Both\n"
        "                        are forced to carry an ext by something that says\n"
        "                        NOTHING about QoS, then store 24 B of a constant.\n"
        "          never       : a vertex whose ext was forced BY its settings cannot\n"
        "                        be in the default bucket by construction, so it is\n"
        "                        always the 4 B loss.\n"
        "        Whether a real node clears 1-in-6 is that node's shape mix — but the\n"
        "        first bucket holds every handler and every default-depth STREAM.\n");
    (void)ext;
    return 0;
}
