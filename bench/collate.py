#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Collate RESULT lines from bench_libtracer / bench_zenoh into focused, side-by-side
# tables: fan-out scaling, payload scaling, endpoint (topic) scaling, and the
# transport/mixed paths — plus a libtracer-vs-zenoh relative summary.
#
# RESULT \t system \t mode \t size \t fanout \t endpoints \t pub_s \t deliv_s \t
#        mb_s \t p50ns \t p99ns \t meanns
import sys

rows = []
for line in sys.stdin:
    f = line.rstrip("\n").split("\t")
    if len(f) == 12 and f[0] == "RESULT":
        rows.append(dict(sys=f[1], mode=f[2], size=int(f[3]), fan=int(f[4]), ep=int(f[5]),
                         pub=float(f[6]), deliv=float(f[7]), mbps=float(f[8]),
                         p50=int(f[9]), p99=int(f[10]), mean=int(f[11])))


def pick(**kw):
    return [r for r in rows if all(r[k] == v for k, v in kw.items())]


def n(x):
    return f"{round(x):,}"


def us(ns):
    return f"{ns / 1000:.2f}µs" if ns >= 1000 else f"{ns}ns"


def table(title, items, label):
    print(f"\n## {title}")
    print(f"{label:>12} {'system/mode':<22} {'pub/s':>13} {'deliv/s':>14} {'p50':>10} {'p99':>10}")
    print("-" * 84)
    for val, sel in items:
        for r in sel:
            print(f"{val:>12} {r['sys'] + '/' + r['mode']:<22} {n(r['pub']):>13} "
                  f"{n(r['deliv']):>14} {us(r['p50']):>10} {us(r['p99']):>10}")
        if sel:
            print()


# 1. Fan-out scaling: size=64, endpoints=1, mode inproc (both systems).
fans = sorted({r["fan"] for r in rows if r["mode"] == "inproc" and r["size"] == 64 and r["ep"] == 1})
table("Fan-out scaling (64B, 1 endpoint)",
      [(f, pick(size=64, fan=f, ep=1, mode="inproc")) for f in fans], "subs")

# 2. Payload scaling: fanout=1, endpoints=1.
sizes = sorted({r["size"] for r in rows if r["fan"] == 1 and r["ep"] == 1 and r["size"] > 0})
print("\n## Payload scaling (1 sub, 1 endpoint)")
print(f"{'size':>12} {'system/mode':<22} {'pub/s':>13} {'MB/s':>12} {'p50':>10} {'p99':>10}")
print("-" * 84)
for s in sizes:
    for r in pick(size=s, fan=1, ep=1):
        if r["mode"] in ("inproc", "inproc-borrow", "loopback") or r["sys"] == "zenoh":
            print(f"{str(s) + 'B':>12} {r['sys'] + '/' + r['mode']:<22} {n(r['pub']):>13} "
                  f"{r['mbps']:>12.1f} {us(r['p50']):>10} {us(r['p99']):>10}")
    print()

# 3. Endpoint (topic) scaling: fanout=1, size=64, write-by-path.
eps = sorted({r["ep"] for r in rows if r["mode"] == "inproc-path"})
table("Endpoint/topic scaling (64B, 1 sub, write-by-path)",
      [(e, pick(size=64, fan=1, ep=e, mode="inproc-path")) for e in eps], "topics")

# 4. Mixed workload.
table("Mixed workload (128 topics, varied fan-out + payload)",
      [("mixed", pick(mode="mixed"))], "")

# 5. Side-by-side absolute deliveries/s at matched (size, fan, ep) — no ratios; the
#    published comparison charts (bench/render_compare.py) plot these on shared axes.
print("\n## libtracer vs zenoh — absolute, matched size/fanout/endpoints")
printed = False
for r in rows:
    if r["sys"] != "libtracer" or r["mode"] != "inproc":
        continue
    z = next((q for q in rows if q["sys"] == "zenoh" and q["size"] == r["size"]
              and q["fan"] == r["fan"] and q["ep"] == r["ep"]), None)
    if z and z["deliv"] > 0 and z["p50"] > 0:
        printed = True
        print(f"  size={r['size']}B fan={r['fan']} ep={r['ep']}: "
              f"deliv/s libtracer={n(r['deliv'])} zenoh={n(z['deliv'])}; "
              f"p50 libtracer={us(r['p50'])} zenoh={us(z['p50'])}")
if not printed:
    print("  (no zenoh rows — run ./fetch_zenoh.sh and rebuild for the comparison)")
