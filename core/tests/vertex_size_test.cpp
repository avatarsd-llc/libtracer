/**
 * @file
 * @brief The vertex RAM-diet regression gate (#361 §8): compile-time ceilings on
 *        `sizeof(vertex_t)` and the hot/cold split invariants, plus runtime probes that
 *        prove the cold extension block is NOT allocated for the common default leaf and
 *        IS allocated exactly when the identity needs it.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The ceilings are per-pointer-width, with headroom over the measured value (post-split:
 * 112 B on x86-64) so routine churn passes but re-inlining a cold member (handlers,
 * history, the ACL trio) fails the build — the "silent regression" this gate exists to
 * catch. Tightening a ceiling after a diet increment is expected; RAISING one is a
 * reviewed decision (#361).
 */

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_t;

/** @brief Assert-independent check (RelWithDebInfo defines NDEBUG — a plain `assert`
 *         would silently skip the probes there): print and hard-exit on failure. */
void require(bool ok, const char* what) {
    if (!ok) {
        std::printf("  [FAIL] %s\n", what);
        std::exit(1);
    }
}

// The `sizeof(vertex_t)` ceilings are NOT asserted here any more. They live with the type, in
// `vertex.hpp`, keyed on `config_t::kMaxVertexBytes{64,32}` — so every build on every target
// evaluates them under its own configuration. Asserting them here gated exactly one binding,
// and never the 32-bit one at all: no CI leg cross-compiled this test, while the ESP-IDF legs
// compiled `vertex_t` itself on every PR. What remains below is what a header cannot express —
// the runtime probes that prove the cold extension block is not allocated for a default leaf.

/**
 * @brief The stripe table's footprint gate: a padded stripe must be EXACTLY one padding
 *        unit wide.
 *
 * The table is `kVertexLockStripes` stripes of static RAM, so a member that pushes a stripe
 * one byte past the line silently DOUBLES it — the same class of invisible regression the
 * `vertex_t` ceilings above exist to catch, in the one global mutable buffer libtracer links
 * into a node. Skipped where padding is switched off (`kCacheLineBytes == 0`): with no
 * isolation requested the stripe is simply its payload, whatever that rounds to.
 */
static_assert(tr::graph::kCacheLineBytes == 0 ||
                  sizeof(tr::graph::vertex_stripe_t) == tr::graph::kCacheLineBytes,
              "a padded stripe must occupy exactly one cache line — a stripe that spans two "
              "doubles the process-wide table for no false-sharing gain");

/** @brief The census predicate this gate turns on: whether the lazily-allocated cold block
 *         exists on the vertex a handle names. */
[[nodiscard]] bool has_ext(tr::graph::vertex_handle_t h) {
    return std::bit_cast<vertex_t*>(h)->has_extension_block();
}

/** @brief A leaf with all-default identity must not allocate the cold block. */
void default_leaf_allocates_no_ext() {
    graph_t g;
    const auto h = g.register_vertex(path_t("/diet/leaf"), role_t::STORED_VALUE);
    require(!has_ext(h), "a default leaf allocates no vertex_ext_t");
}

/** @brief REGISTRATION can no longer force the cold block (RFC-0022 §3.B): the settings
 *         parameter that used to is gone, so strictly more vertices stay extension-less
 *         than before. A STREAM identity and a handler still allocate one — the ablation
 *         that keeps this from passing against a `has_ext` stuck at false. */
void registration_cannot_force_the_ext() {
    graph_t g;
    const auto plain = g.register_vertex(path_t("/diet/plain"), role_t::STORED_VALUE);
    require(!has_ext(plain), "a plain registration allocates no vertex_ext_t");
    const auto stream = g.register_vertex(path_t("/diet/stream"), role_t::STREAM);
    require(has_ext(stream), "a STREAM identity still allocates one");
    handlers_t h;
    h.on_read = []() -> tr::graph::result_t<tr::view::rope_t> {
        return std::unexpected(tr::graph::status_t::NOT_FOUND);
    };
    const auto handler = g.register_vertex(path_t("/diet/handler"), role_t::HANDLER, std::move(h));
    require(has_ext(handler), "a handler-bearing identity still allocates one");
}

/** @brief An OWNER-side storage declaration on a default leaf must transparently allocate
 *         the cold block and land the value (the lazy-allocation seam of #361 §1). */
void late_declaration_allocates_lazily() {
    graph_t g;
    const auto h = g.register_vertex(path_t("/diet/late"), role_t::STORED_VALUE);
    require(!has_ext(h), "the leaf starts extension-less");
    g.set_store_ref_min_bytes(h, 64);
    require(has_ext(h), "the declaration allocated the cold block");
    require(g.store_ref_min_bytes(h) == 64, "g.store_ref_min_bytes(h) == 64");
}

}  // namespace

/** @brief Run the gate's runtime probes and print the measured sizes for the CI log. */
int main() {
    std::printf("sizeof(vertex_t)      = %zu (gate: <= %zu on this ABI)\n", sizeof(vertex_t),
                sizeof(void*) == 8 ? tr::graph::config_t::kMaxVertexBytes64
                                   : tr::graph::config_t::kMaxVertexBytes32);
    std::printf("sizeof(vertex_ext_t)  = %zu (lazily allocated, cold)\n",
                sizeof(tr::graph::vertex_ext_t));
    default_leaf_allocates_no_ext();
    registration_cannot_force_the_ext();
    late_declaration_allocates_lazily();
    std::puts("vertex_size_test: OK");
    return 0;
}
