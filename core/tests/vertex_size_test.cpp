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
 * 248 B on x86-64) so routine churn passes but re-inlining a cold member (handlers,
 * history, the ACL trio) fails the build — the "silent regression" this gate exists to
 * catch. Tightening a ceiling after a diet increment is expected; RAISING one is a
 * reviewed decision (#361).
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::settings_t;
using tr::graph::vertex_t;

/** @brief Assert-independent check (RelWithDebInfo defines NDEBUG — a plain `assert`
 *         would silently skip the probes there): print and hard-exit on failure. */
void require(bool ok, const char* what) {
    if (!ok) {
        std::printf("  [FAIL] %s\n", what);
        std::exit(1);
    }
}

/** @brief The 64-bit hot-core ceiling (measured 112 B post-#380-§1 children collapse;
 *         144 post-packing, 168 post-§3, 160 post-§2, 248 post-§1, 536 pre-split). */
constexpr std::size_t kMax64 = 120;
/** @brief The 32-bit (MCU target) hot-core ceiling — pointer-halved with the same headroom. */
constexpr std::size_t kMax32 = 72;

static_assert(sizeof(void*) != 8 || sizeof(vertex_t) <= kMax64,
              "vertex_t grew past the 64-bit RAM-diet gate (#361) — move the new member "
              "behind vertex_ext_t, don't inline it");
static_assert(sizeof(void*) != 4 || sizeof(vertex_t) <= kMax32,
              "vertex_t grew past the 32-bit RAM-diet gate (#361) — move the new member "
              "behind vertex_ext_t, don't inline it");

/** @brief A view over a fresh, owned heap segment holding @p bytes (as graph_test). */
tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

/** @brief A VALUE TLV wrapping @p payload, as an owned view (the field-write shape). */
tr::view::view_t value_tlv(std::span<const std::byte> payload) {
    tr::wire::tlv_t t{.type = tr::wire::type_t::VALUE, .payload = payload};
    return make_value(tr::wire::encode(t));
}

/** @brief A leaf with all-default identity must not allocate the cold block: its settings
 *         must come back as the shared defaults constant, by address. */
void default_leaf_shares_default_settings() {
    graph_t g;
    const auto h = g.register_vertex(path_t("/diet/leaf"), role_t::STORED_VALUE);
    require(&g.settings(h) == &tr::graph::kDefaultSettings,
            "&g.settings(h) == &tr::graph::kDefaultSettings");
}

/** @brief Non-default QoS at registration must yield a private settings copy (the cold
 *         block), not mutate the shared defaults. */
void non_default_settings_get_private_copy() {
    graph_t g;
    settings_t s;
    s.durability = 1;
    const auto h =
        g.register_vertex(path_t("/diet/durable"), role_t::STORED_VALUE, handlers_t{}, s);
    require(&g.settings(h) != &tr::graph::kDefaultSettings,
            "&g.settings(h) != &tr::graph::kDefaultSettings");
    require(g.settings(h).durability == 1, "g.settings(h).durability == 1");
    require(tr::graph::kDefaultSettings.durability == 0,
            "tr::graph::kDefaultSettings.durability == 0");
}

/** @brief A `:settings` field write on a default leaf must transparently allocate the cold
 *         block and land the field (the lazy-allocation seam of #361 §1). */
void late_settings_write_allocates_lazily() {
    graph_t g;
    const auto h = g.register_vertex(path_t("/diet/late"), role_t::STORED_VALUE);
    require(&g.settings(h) == &tr::graph::kDefaultSettings,
            "&g.settings(h) == &tr::graph::kDefaultSettings");
    const auto payload = std::array<std::byte, 1>{std::byte{7}};
    const auto w = g.write(path_t("/diet/late:settings.priority"), value_tlv(payload));
    require(w.has_value(), "w.has_value()");
    require(g.settings(h).priority == 7, "g.settings(h).priority == 7");
    require(&g.settings(h) != &tr::graph::kDefaultSettings,
            "&g.settings(h) != &tr::graph::kDefaultSettings");
}

}  // namespace

/** @brief Run the gate's runtime probes and print the measured sizes for the CI log. */
int main() {
    std::printf("sizeof(vertex_t)      = %zu (gate: <= %zu on this ABI)\n", sizeof(vertex_t),
                sizeof(void*) == 8 ? kMax64 : kMax32);
    std::printf("sizeof(vertex_ext_t)  = %zu (lazily allocated, cold)\n",
                sizeof(tr::graph::vertex_ext_t));
    default_leaf_shares_default_settings();
    non_default_settings_get_private_copy();
    late_settings_write_allocates_lazily();
    std::puts("vertex_size_test: OK");
    return 0;
}
