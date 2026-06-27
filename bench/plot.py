#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Render the system-dynamics response surfaces from grid.csv (produced by
# `bench_libtracer grid` / `bench_zenoh grid`, collated by grid.sh) into 2D line
# plots, 3D surfaces, and libtracer/zenoh speedup heatmaps under bench/figures/.
#
#   bench/.venv/bin/python plot.py [grid.csv] [figures_dir]
import csv
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from mpl_toolkits.mplot3d import Axes3D  # noqa: F401,E402

CSV = sys.argv[1] if len(sys.argv) > 1 else "grid.csv"
OUT = sys.argv[2] if len(sys.argv) > 2 else "figures"
os.makedirs(OUT, exist_ok=True)

rows = []
with open(CSV) as f:
    for r in csv.DictReader(f):
        rows.append({k: (v if k == "system" else int(float(v))) for k, v in r.items()})

SYS = ["libtracer", "zenoh"]
COL = {"libtracer": "#1f77b4", "zenoh": "#d62728"}


def grid(system, slice_axis, value, fixed):
    """Return (xs, sizes, Z[size][x]) for one system over a 2D slice.
    slice_axis is 'fanout' or 'endpoints'; `fixed` is the value of the other axis."""
    other = "endpoints" if slice_axis == "fanout" else "fanout"
    sel = [r for r in rows if r["system"] == system and r[other] == fixed]
    sizes = sorted({r["size"] for r in sel})
    xs = sorted({r[slice_axis] for r in sel})
    Z = np.full((len(sizes), len(xs)), np.nan)
    for r in sel:
        Z[sizes.index(r["size"]), xs.index(r[slice_axis])] = r[value]
    return xs, sizes, Z


def line_plot(slice_axis, value, ylabel, title, fname, fixed=1, logy=True):
    fig, ax = plt.subplots(figsize=(8, 5.5))
    for system in SYS:
        xs, sizes, Z = grid(system, slice_axis, value, fixed)
        if not xs:
            continue
        cmap = plt.cm.Blues if system == "libtracer" else plt.cm.Reds
        for i, s in enumerate(sizes):
            ax.plot(xs, Z[i], marker="o", ms=3, lw=1.4,
                    color=cmap(0.35 + 0.6 * i / max(1, len(sizes) - 1)),
                    ls="-" if system == "libtracer" else "--",
                    label=f"{system} {s}B" if (s in (sizes[0], sizes[-1])) else None)
    ax.set_xscale("log", base=2)
    if logy:
        ax.set_yscale("log")
    ax.set_xlabel(slice_axis + (" (subscribers/endpoint)" if slice_axis == "fanout" else " (topics)"))
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=8, ncol=2)
    fig.tight_layout()
    fig.savefig(f"{OUT}/{fname}", dpi=130)
    plt.close(fig)
    print("wrote", fname)


def surface(slice_axis, value, zlabel, title, fname, fixed=1):
    fig = plt.figure(figsize=(12, 5.5))
    for j, system in enumerate(SYS):
        xs, sizes, Z = grid(system, slice_axis, value, fixed)
        if not xs:
            continue
        ax = fig.add_subplot(1, 2, j + 1, projection="3d")
        X, Y = np.meshgrid(np.log2(xs), np.log2(sizes))
        ax.plot_surface(X, Y, np.log10(Z), cmap="viridis", edgecolor="none", alpha=0.95)
        ax.set_xlabel(f"log2({slice_axis})")
        ax.set_ylabel("log2(size B)")
        ax.set_zlabel(f"log10({zlabel})")
        ax.set_title(f"{system}")
    fig.suptitle(title)
    fig.tight_layout()
    fig.savefig(f"{OUT}/{fname}", dpi=130)
    plt.close(fig)
    print("wrote", fname)


def heatmap(slice_axis, value, title, fname, fixed=1, invert=False):
    """libtracer/zenoh ratio over (size x slice_axis). invert=True for latency
    (zenoh/libtracer, i.e. 'how many x lower libtracer's latency')."""
    xs, sizes, Zl = grid("libtracer", slice_axis, value, fixed)
    _, _, Zz = grid("zenoh", slice_axis, value, fixed)
    if not xs or np.all(np.isnan(Zz)):
        print("skip", fname, "(no zenoh data)")
        return
    R = (Zz / Zl) if invert else (Zl / Zz)
    fig, ax = plt.subplots(figsize=(8, 5.5))
    im = ax.imshow(R, origin="lower", aspect="auto", cmap="RdYlGn",
                   norm=matplotlib.colors.LogNorm(vmin=max(0.1, np.nanmin(R)), vmax=np.nanmax(R)))
    ax.set_xticks(range(len(xs)))
    ax.set_xticklabels(xs)
    ax.set_yticks(range(len(sizes)))
    ax.set_yticklabels([f"{s}B" for s in sizes])
    ax.set_xlabel(slice_axis)
    ax.set_ylabel("payload")
    ax.set_title(title)
    for i in range(len(sizes)):
        for k in range(len(xs)):
            if not np.isnan(R[i, k]):
                ax.text(k, i, f"{R[i, k]:.1f}x", ha="center", va="center", fontsize=7)
    fig.colorbar(im, ax=ax, label="libtracer advantage (x)")
    fig.tight_layout()
    fig.savefig(f"{OUT}/{fname}", dpi=130)
    plt.close(fig)
    print("wrote", fname)


# --- fan-out dynamics (endpoints = 1) ---
line_plot("fanout", "deliv_s", "deliveries/s (log)", "Throughput vs fan-out (libtracer solid, zenoh dashed)", "throughput_vs_fanout.png")
line_plot("fanout", "p50_ns", "p50 latency ns (log)", "Latency vs fan-out (libtracer solid, zenoh dashed)", "latency_vs_fanout.png")
surface("fanout", "deliv_s", "deliveries/s", "Throughput surface over payload x fan-out", "surface_throughput_fanout.png")
heatmap("fanout", "deliv_s", "libtracer/zenoh throughput speedup (payload x fan-out)", "speedup_throughput_fanout.png")
heatmap("fanout", "p50_ns", "libtracer latency advantage = zenoh p50 / libtracer p50", "speedup_latency_fanout.png", invert=True)

# --- endpoint/topic dynamics (fan-out = 1) ---
line_plot("endpoints", "pub_s", "publishes/s (log)", "Throughput vs topic count (libtracer solid, zenoh dashed)", "throughput_vs_endpoints.png")
surface("endpoints", "pub_s", "publishes/s", "Throughput surface over payload x topic count", "surface_throughput_endpoints.png")
heatmap("endpoints", "pub_s", "libtracer/zenoh throughput speedup (payload x topics)", "speedup_throughput_endpoints.png")

print("done ->", OUT)
