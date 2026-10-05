/**
 * @file
 * @brief The exact-count rows #1808 adds to `bench_forward_heap`: RAM per edge, per link and
 *        per 1 KiB value, blocks per write per payload size, and the STREAM write's
 *        stripe-lock sections.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Every row here is a count, not a clock: it is printed once per binary and `perf_gate.py`
 * ratchets it against main exactly. Fractions survive because every per-unit figure is
 * printed multiplied by 1000 (a `_x1000` field): 1.5 blocks per edge is `1500`, where an
 * integer division would print `1` and hide half a block.
 *
 * The three line shapes, all on stdout:
 *
 *     RESULT ramprobe <what> blocks_x1000=<n> bytes_x1000=<n> n=<units>
 *     RESULT writeblocks <owned|rope2> S=<size> seam_x1000=<n> seam_bytes_x1000=<n>
 *            heap_x1000=<n> n=<writes>
 *     RESULT streamlock <case> sections_x1000=<n> heap_x1000=<n> delivered_x1000=<n> n=<writes>
 *     RESULT slabfoot <what> live_x1000=<n> blocks_x1000=<n> n=<units> (ungated)
 *
 * (`writeblocks` is one line; it is wrapped here only. The `w4` streamlock row has no
 * `heap_x1000` field: see `stream_locks` in exact_rows.cpp.)
 *
 * `slabfoot` is the one row nothing gates: the edge probes' window on a DEFAULT graph, whose
 * tables come from the host slab pool and are counted a whole slab at a time (#1778). The
 * gated `ramprobe` edge rows draw per object instead; see `ram_edges_on` in exact_rows.cpp.
 *
 * They lead with a field other than `allocs=`, so the history emitter's zeroheap parser does
 * not read them as that series; it charts them through its own parsers.
 */
#pragma once

namespace exact_rows {

/**
 * @brief Print every exact-count row (see the file comment).
 *
 * Runs on the calling thread except for the four-writer STREAM case, which starts its own.
 * Uses `bench_forward_heap`'s counting `operator new` (heap_probe.hpp) and the
 * `--wrap=pthread_mutex_lock` counter this binary links with.
 *
 * @return 0, or 2 when a fixture did not do what its row claims (the rows are then not
 *         printed, and the binary fails as for every other blind fixture).
 */
[[nodiscard]] int print_all();

}  // namespace exact_rows
