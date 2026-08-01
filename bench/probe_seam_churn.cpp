/**
 * @file
 * @brief The #576 RED-state probe: how many retired value-seam blocks a node still holds
 *        after N register/retire cycles of a handler-bearing vertex.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The defect #576 records is peer-driven, unbounded growth: `graph_t::retire` parked each
 * detached `value_handlers_t` in a vector with one append site and zero release sites, and
 * RFC-0014 made connection create/remove a wire-driven operation, so a peer chooses the
 * rate. This probe makes that state REPRODUCIBLE rather than argued: it counts live seam
 * blocks directly, through a `std::shared_ptr` the handler captures — a block's destruction
 * is exactly when the capture's `use_count` drops.
 *
 * It uses nothing but `register_vertex` / `retire`, so the SAME source builds at any
 * revision. That is the point: build it before the fix and after.
 *
 *     bench/run_seam_ab.sh ... # (builds both arms; or configure bench/ at each revision)
 *     ./probe_seam_churn 500
 *     PROBE cycles=500 live_seam_blocks=500 peak=500   # <- pre-fix: one per retire, forever
 *     PROBE cycles=500 live_seam_blocks=1 peak=65      # <- post-fix: bounded by a scan batch
 *
 * `live_seam_blocks` is read while the graph is still alive; the trailing `after_teardown`
 * figure is read after it is destroyed, which is the other half of #576 (there was no
 * `~graph_t` at all before ADR-0072).
 */
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "libtracer/rope.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;

/** @brief A handler whose on_read captures @p probe — the block-lifetime instrument. */
tr::graph::handlers_t probed_handlers(std::shared_ptr<int> probe) {
    tr::graph::handlers_t h;
    h.on_read = [probe = std::move(probe)]() -> tr::graph::result_t<tr::view::rope_t> {
        return tr::view::rope_t{};
    };
    return h;
}

}  // namespace

int main(int argc, char** argv) {
    const long cycles = argc > 1 ? std::atol(argv[1]) : 500;
    auto probe = std::make_shared<int>(0);
    long peak = 0;
    long live = 0;
    {
        graph_t g;
        for (long i = 0; i < cycles; ++i) {
            const auto vh =
                g.try_register_vertex(path_t("/churn"), role_t::HANDLER, probed_handlers(probe));
            if (!vh.has_value()) {
                std::fprintf(stderr, "FATAL: cycle %ld could not re-register\n", i);
                return 1;
            }
            if (!g.retire(*vh).has_value()) {
                std::fprintf(stderr, "FATAL: cycle %ld could not retire\n", i);
                return 1;
            }
            live = probe.use_count() - 1;  // minus this function's own reference
            if (live > peak) peak = live;
        }
    }
    std::printf("PROBE cycles=%ld live_seam_blocks=%ld peak=%ld after_teardown=%ld\n", cycles, live,
                peak, static_cast<long>(probe.use_count() - 1));
    return 0;
}
