#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Collate RESULT lines from bench_libtracer / bench_zenoh into a side-by-side
# table grouped by payload size.
#
# RESULT<TAB>system<TAB>mode<TAB>size<TAB>msgs_per_s<TAB>mb_per_s<TAB>p50ns<TAB>p99ns<TAB>meanns
import sys

rows = []
for line in sys.stdin:
    f = line.rstrip("\n").split("\t")
    if len(f) == 9 and f[0] == "RESULT":
        rows.append(
            dict(sys=f[1], mode=f[2], size=int(f[3]), mps=float(f[4]),
                 mbps=float(f[5]), p50=int(f[6]), p99=int(f[7]), mean=int(f[8])))

order = ["libtracer/inproc", "libtracer/loopback", "libtracer/net", "zenoh/inproc", "zenoh/net"]
sizes = sorted({r["size"] for r in rows})


def fmt_int(n):
    return f"{n:,}"


hdr = f"{'payload':>8} {'path':<20} {'msgs/s':>14} {'MB/s':>10} {'p50':>10} {'p99':>10} {'mean':>10}"
print(hdr)
print("-" * len(hdr))
for s in sizes:
    for key in order:
        sysn, mode = key.split("/")
        m = next((r for r in rows if r["size"] == s and r["sys"] == sysn and r["mode"] == mode), None)
        if not m:
            continue
        print(f"{str(s) + 'B':>8} {key:<20} {fmt_int(round(m['mps'])):>14} "
              f"{m['mbps']:>10.1f} {m['p50'] / 1000:>9.2f}µ {m['p99'] / 1000:>9.2f}µ "
              f"{m['mean'] / 1000:>9.2f}µ")
    print()

# Speed-up summary (libtracer/inproc vs zenoh/inproc), where both exist.
print("relative (libtracer/inproc vs zenoh/inproc), same payload:")
for s in sizes:
    lt = next((r for r in rows if r["size"] == s and r["sys"] == "libtracer" and r["mode"] == "inproc"), None)
    zn = next((r for r in rows if r["size"] == s and r["sys"] == "zenoh"), None)
    if lt and zn and zn["mps"] > 0 and lt["mean"] > 0:
        print(f"  {str(s) + 'B':>6}: throughput x{lt['mps'] / zn['mps']:.1f}, "
              f"latency(mean) x{zn['mean'] / lt['mean']:.1f} lower")
