/**
 * @file
 * @brief The RFC-0022 §3.C latency gate: the two storage knobs must stay ONE inline load.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0022 §3.C makes a child's storage policy a COPY taken at registration, explicitly
 * rather than an ancestor walk, because `store_ref_min_bytes` is read on **every write**
 * (`op_resolve.hpp`) and `history_keep_last` on **every store** (`vertex.hpp`). A walk on
 * those paths is disqualifying under this project's latency-first ordering, and the whole
 * of the design's cost claim is that neither read got slower.
 *
 * That claim is falsifiable in exactly one shape, which is what this file measures.
 *
 * @section legs The legs
 *
 *   `settings-d1` / `settings-d8`   The policy read itself, on a vertex 1 and 8 levels deep.
 *                                   **This pair is the falsifier.** One inline load does not
 *                                   care how deep the vertex sits; an ancestor walk is O(depth)
 *                                   and the two rows separate. A single-depth row could not
 *                                   tell a walk from a load.
 *   `stream-store`                  `graph_t::write` on a STREAM vertex — `history_keep_last`
 *                                   read in situ, on the real store path, per store.
 *   `plain-write`                   `graph_t::write` on a STORED_VALUE vertex — the write hot
 *                                   path, which the change must not have touched at all.
 *   `control-codec`                 A TLV encode+decode round trip. It touches **no** graph
 *                                   settings, no vertex, no subscription: RFC-0022 cannot
 *                                   move it. It is the INVARIANT CONTROL (#464) — if this
 *                                   leg drifts between the two binaries by more than the
 *                                   effect being claimed, the run measured the machine, not
 *                                   the change, and must be thrown away.
 *
 * @section how How to run it as an A/B
 *
 * Build this file against `main` and against the branch, then interleave the two BINARIES
 * round-robin on a pinned core, taking the median of ≥15 invocations each:
 *
 * ```
 * taskset -c 3 ./bench_storage_policy --rounds 5     # A, then B, then A, … 15+ times each
 * ```
 *
 * Each invocation prints one `RESULT` line per leg per round; the driver medians them. The
 * interleave is what makes a thermal or governor drift show up in BOTH arms instead of
 * being attributed to the change.
 *
 * @section reading Reading it
 *
 * A ratio is only meaningful beside the control leg's ratio. The gate is: no leg regresses
 * beyond the control leg's own drift, and `settings-d8 / settings-d1` stays ≈ 1.0 — the
 * depth-independence that says "copy at registration", not "walk at read".
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::settings_t;
using tr::graph::vertex_handle_t;
using tr::view::rope_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Iterations per timed leg — large enough that the timer's own cost is noise. */
constexpr std::size_t kIters = 400'000;

/** @brief A rope over a fresh owned heap segment holding @p n bytes of payload. */
rope_t make_value(std::size_t n) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(n);
    std::memset(seg->bytes.data(), 0x5A, n);
    return rope_t{tr::view::view_t::over(std::move(seg))};
}

/**
 * @brief Register `/<root>` with @p policy, then `depth-1` levels of children under it, and
 *        hand back the DEEPEST one.
 *
 * The children state no policy of their own, so each INHERITS by value (RFC-0022 §3.C) —
 * which is exactly the shape a walk-based resolution would have to climb. The deepest
 * vertex therefore holds the same value as the root, reached by one load rather than by
 * eight parent hops.
 */
vertex_handle_t deep_vertex(graph_t& g, const std::string& root, std::size_t depth,
                            const settings_t& policy) {
    std::string p = "/" + root;
    vertex_handle_t v = g.register_vertex(path_t(p), role_t::STORED_VALUE, {}, policy);
    for (std::size_t i = 1; i < depth; ++i) {
        p += "/l" + std::to_string(i);
        v = g.register_vertex(path_t(p), role_t::STORED_VALUE);
    }
    return v;
}

/** @brief One timed leg: run @p body @p kIters times and print its ns/op RESULT row. */
template <typename F>
void leg(std::string_view name, std::size_t round, F&& body) {
    const std::uint64_t t0 = bench::now_ns();
    for (std::size_t i = 0; i < kIters; ++i) body(i);
    const std::uint64_t dt = bench::now_ns() - t0;
    std::printf("RESULT leg=%.*s round=%zu ns_per_op=%.3f\n", static_cast<int>(name.size()),
                name.data(), round, static_cast<double>(dt) / static_cast<double>(kIters));
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t rounds = 5;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string_view(argv[i]) == "--rounds")
            rounds = static_cast<std::size_t>(std::atoll(argv[i + 1]));

    graph_t g;

    // The two policy-read legs. Both vertices carry a non-default policy so the read
    // actually reaches an extension block rather than the shared defaults constant; the
    // deep one sits 8 levels down, which is what an ancestor walk would charge for.
    settings_t policy;
    policy.store_ref_min_bytes = 256;
    policy.history_keep_last = 4;
    const vertex_handle_t shallow =
        g.register_vertex(path_t("/d1"), role_t::STORED_VALUE, {}, policy);
    const vertex_handle_t deep = deep_vertex(g, "d8", 8, policy);  // inherits by value

    // The two real paths.
    settings_t ring;
    ring.history_keep_last = 16;  // a real bounded ring: retention work on every store
    const vertex_handle_t stream =
        g.register_vertex(path_t("/bench/stream"), role_t::STREAM, {}, ring);
    const vertex_handle_t plain = g.register_vertex(path_t("/bench/plain"), role_t::STORED_VALUE);

    // ONE pre-made value, cloned per iteration. A fresh `heap_alloc` per write would put a
    // malloc on both write legs and make the allocator's state — not the write path — the
    // dominant term and the dominant noise source. A rope copy is a refcount bump, which is
    // what a real caller's value costs anyway.
    const rope_t sample = make_value(8);

    // The control leg's fixture: a TLV the codec round-trips, touching no graph state.
    std::vector<std::byte> payload(64, std::byte{0x5A});
    std::vector<std::byte> encoded;
    tr::wire::emit_tlv(encoded, type_t::VALUE, opt_t{}, payload);

    std::printf("bench_storage_policy — RFC-0022 §3.C: the storage knobs stay one inline load\n");
    std::printf("iters/leg=%zu rounds=%zu\n", kIters, rounds);

    std::uint64_t sink = 0;
    for (std::size_t r = 0; r < rounds; ++r) {
        leg("settings-d1", r,
            [&](std::size_t) { sink += g.settings(shallow).store_ref_min_bytes; });
        leg("settings-d8", r, [&](std::size_t) { sink += g.settings(deep).store_ref_min_bytes; });
        leg("stream-store", r,
            [&](std::size_t) { sink += g.write(stream, rope_t(sample)).has_value() ? 1 : 0; });
        leg("plain-write", r,
            [&](std::size_t) { sink += g.write(plain, rope_t(sample)).has_value() ? 1 : 0; });
        leg("control-codec", r, [&](std::size_t) {
            const auto dec = tr::wire::decode(encoded);
            sink += dec.has_value() ? dec->payload.size() : 0;
        });
    }
    std::printf("checksum=%llu\n", static_cast<unsigned long long>(sink));
    return 0;
}
