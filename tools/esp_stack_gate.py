#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Static stack gate for the esp32c6 WebSocket client's receive path (#2871).

The application sizes a receive task's stack once, on NARROW targets, and the outgoing
WebSocket client delivers every message in-call on that task: the router, the terminus, the
graph write, its fan-out and the replies they send all run on it. Growth on that path that
nothing gates turns a library upgrade into a stack overflow on the device with no build-time
signal; from v0.21.0 to 51ef6574e it grew by about 1 KiB before anyone noticed (#2871).

This reads the GCC `-fcallgraph-info=su` files (`*.ci`: each function's frame, its
static/dynamic qualifier and its call edges) that esp-idf.yml's transport-footprint job writes
for its image, which links the client link and attaches it to a router. From
`esp_ws_client_link_t::recv_loop` it takes the worst static path, frame by frame, for each
delivery arm:

* **borrowed** (no `memory.rx`): the span receiver thunk, `on_frame_impl`, the arena terminus;
* **owning** (`memory.rx` set): the rope receiver thunk and `on_frame_rope_impl`.

Each of those two rows is compared with its pin in STACK_PINS, the way tools/esp_size_gate.py
compares the per-transport flash rows: on esp32c6, built by PINNED_TOOLCHAIN (shared with that
gate, since both read the same pinned-digest image). A row over its pin fails; a row under it
passes and says to re-pin down. Raising a pin needs a maintainer ruling. Every in-call path
is pinned, branch writes included (#2879): their decode draws from the graph's table source,
not a stack array, so no path is left out of a row.

A row fails, whatever its total, when the walk did not reach the arm's anchor frames
(ANCHORS: a renamed function or a lost edge must not shrink the path into a pass), or when a
frame on the path is dynamic and unbounded (an `alloca` or a VLA: its depth is not static).

What the path counts, and what it leaves out, is fixed here so the number means one thing:

* Indirect calls are resolved where the library makes them on this task (INDIRECT_EDGES): the
  receiver slot into the router's thunk for the arm, the graph's write hooks into the router's
  remote-delivery and await-fired thunks, and the router's sends into each link's `send`.
* Every other indirect call is left out: an application handler or a subscriber's `on_write`
  is the application's frame, not the library's.
* The platform's frames (newlib, FreeRTOS, lwIP, the heap, locks, logging, assert) are left out
  (PLATFORM_CUTS, matched on the function's name without its parameters): their depth is
  IDF's and does not move with a library change.
* A call back into a function already on the path is cut there and named in the report. The
  walk memoizes each function's worst path below it, so a function first reached under a cut
  keeps that answer when it is reached again elsewhere: it can leave out a path through an
  ancestor. On this graph that only drops a recursion, which no finite stack holds anyway.
* Every indirect edge must resolve at least one caller and one target, or the run fails: a
  renamed hook must not shrink the path into a pass. The report lists what each resolved.

Exit 0 = every pinned row within its pin (or reported only, off esp32c6), 1 = broken input,
a row over its pin, a missing anchor, an unbounded dynamic frame, or an esp32c6 build by
another compiler. Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from esp_size_gate import PINNED_TARGET, PINNED_TOOLCHAIN  # noqa: E402  (one toolchain pin)

# The worst static path in bytes, per pinned row, measured on transport_footprint @ esp32c6
# with PINNED_TOOLCHAIN in the image esp-idf.yml pins by digest (#2830). A raise needs a
# maintainer ruling; a cut re-pins down.
STACK_PINS = {
    # #2871: 6,048 B at 9964668f0, cut by 832 B. The arena tier no longer copies selector names
    # into a 512 B buffer, the reply table carries 4 stack entries, not 16, and the framing's
    # body/data/hand_up frames merge into the ingress's. (On the receive and terminus path
    # alone, before the graph's hooks were resolved: 4,080 B at v0.21.0, 5,232 B at
    # 9964668f0, 4,496 B here.)
    # #2879 (Ruling 128): the row widens to branch writes, which no longer carry a 4 KiB stack
    # array. Between write_impl and fan_out the worst path is now write_branch (384) and
    # fan_out_slice (80) instead of handler_write_deliver, deliver_vertex and bubble_up (288),
    # so the pin moves to 5,392 B; no frame that was pinned before grew. The path through a
    # branch write fell from 9,536 B to 5,392 B.
    "borrowed": 5392,
    # #2871: 6,864 B at 9964668f0, cut by 352 B by the same reply table and framing merges; the
    # multi-link terminus (rope tier) keeps its name copy. (Receive and terminus path alone:
    # 5,776 B at v0.21.0, 6,048 B at 9964668f0, 5,792 B here.)
    # #2879 (Ruling 128): the same widening, +176 B by the same two frames; the path through a
    # branch write fell from 10,832 B to 6,688 B.
    "owning": 6688,
}

ROOT = r"^tr::net::esp_ws_client_link_t::recv_loop\(\)$"
# Thunks the router installs per link (fwd_router_t::attach_link): one of the two per arm.
SPAN_THUNK = r"(attach_link|add_child)\(.*lambda\(void\*, std::span"
ROPE_THUNK = r"(attach_link|add_child)\(.*lambda\(void\*, tr::view::rope_t"
# (callers, targets): an indirect call made by a caller matching the first is resolved to
# every function matching the second.
INDIRECT_EDGES = (
    # The receive side's sink call, into the router's receiver thunks.
    ("receiver thunk",
     r"^tr::net::esp_ws_client_link_t::(recv_loop|framing_end_t)|receiver_slot|frame_ingress"
     r"|ws_framing", f"{SPAN_THUNK}|{ROPE_THUNK}"),
    # The graph's write hooks the router installs. A remote edge's delivery is made only by
    # `dispatch_edge_remote` (kept out of line), into the router's remote-delivery lambda.
    ("remote delivery", r"^([^(]* )?tr::graph::graph_t::dispatch_edge_remote\(",
     r"^tr::net::fwd_router_t::fwd_router_t\(.*lambda\(void\*, tr::graph::remote_delivery_t"),
    # A fired await is woken by `vertex_t::wake_waiters`, small enough to land in its callers:
    # the vertex's own members and the graph's write and publish steps. Frames that call only
    # an application handler (`read_handler_gated`) are not callers: a ratchet must not carry
    # a path that cannot run, or a real one could grow into its margin unseen.
    ("await fired", r"^([^(]* )?tr::graph::(vertex_t::|graph_t::(handler_write|publish_value"
     r"|relay_write)\()", r"^tr::net::fwd_router_t::await_fired_thunk\("),
    # The router's sends into each link's own (a reply or a refusal, a remote delivery, an
    # await's answer, a forward hop): any router frame, member or file-local, may make one.
    # A file-local caller is matched with its TU, so another file's helpers do not count.
    ("router sends", r"^([^(]* )?tr::net::fwd_router_t::"
     r"|^fwd_router\.cpp:([^(]* )?tr::net::\(anonymous namespace\)::",
     r"^tr::net::(esp_ws_client_link_t|httpd_ws_link_t|can_transport_t)::send(_on)?\("),
    # The CAN transport's frames into its raw link, the TWAI link this image links.
    ("CAN raw link", r"^tr::net::can_transport_t::", r"^tr::net::twai_link_t::write_raw\("),
)
PLATFORM_CUTS = (r"__assert_func|esp_log|^abort$|heap_caps|malloc|calloc|realloc|pvPortMalloc"
                 r"|xQueue|xTask|vPort|lock_acquire|_lock_|pthread_|gettimeofday"
                 r"|esp_system_abort|connect_once|printf|__cxa|operator new|std::__throw"
                 r"|^lwip_|^netconn_|^tcpip_|^sys_|^[a-z_]+_socket$|^esp_|^rtc_|^getrandom$")
# Each arm leaves out the other arm's receiver and thunk.
ARM_CUTS = {
    "borrowed": rf"deliver_rope|receiver_slot_t<>::deliver\(|{ROPE_THUNK}",
    "owning": rf"deliver_borrowed|{SPAN_THUNK}",
}
# Frames the walk must reach on each arm, or the row fails.
ANCHORS = {
    "borrowed": (SPAN_THUNK, r"^tr::net::fwd_router_t::on_frame_impl\(",
                 r"^tr::net::fwd_router_t::resolve_terminus\(",
                 r"^tr::graph::op_resolver_t::resolve\("),
    "owning": (ROPE_THUNK, r"^tr::net::fwd_router_t::on_frame_rope_impl\(",
               r"^tr::net::fwd_router_t::resolve_terminus",
               r"^tr::graph::op_resolver_t::resolve\("),
}

NODE_RE = re.compile(r'node: \{ title: "([^"]+)" label: "([^"]*)"')
EDGE_RE = re.compile(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"')
FRAME_RE = re.compile(r"\\n(\d+) bytes \(([a-z,]+)\)")
INDIRECT = "__indirect_call"


def _norm(title: str) -> str:
    """A node's symbol: GCC prefixes a TU-local or inline one with its source file's path.

    The path is dropped, so an inline function emitted in several TUs is one node, except for
    an anonymous-namespace symbol: that one is TU-local by definition and keeps its file's
    base name (`fwd_router.cpp:_ZN...`, demangled as `fwd_router.cpp:tr::net::(anonymous
    namespace)::...`), so an edge can name the TU it means.
    """
    m = re.match(r"^(?:[^:]*/)?([^:/]*\.(?:c|cc|cpp|cxx|s|S)):", title)
    if not m:
        return title
    rest = title[m.end():]
    return f"{m.group(1)}:{rest}" if "_GLOBAL__N_" in rest else rest


def _base(name: str) -> str:
    """A demangled name without its parameter list (the text from its first `(`)."""
    return re.split(r"\((?!anonymous namespace\))", name, maxsplit=1)[0]


def load(ci_texts: list[str]) -> tuple[dict[str, tuple[int, str]], dict[str, set[str]]]:
    """Merge `.ci` files into ({symbol: (frame bytes, qualifier)}, {symbol: callees}).

    An inline function emitted in several TUs keeps its largest frame.
    """
    frames: dict[str, tuple[int, str]] = {}
    edges: dict[str, set[str]] = {}
    for txt in ci_texts:
        for title, label in NODE_RE.findall(txt):
            m = FRAME_RE.search(label)
            if m:
                sym = _norm(title)
                got = (int(m.group(1)), m.group(2))
                if got[0] >= frames.get(sym, (-1, ""))[0]:
                    frames[sym] = got
        for src, dst in EDGE_RE.findall(txt):
            edges.setdefault(_norm(src), set()).add(_norm(dst))
    return frames, edges


def demangle(symbols: list[str], cxxfilt: str) -> dict[str, str]:
    """Demangled names, in one c++filt run."""
    out = subprocess.run([cxxfilt], input="\n".join(symbols), capture_output=True, text=True,
                         check=True).stdout.split("\n")
    return dict(zip(symbols, out))


def worst_path(frames: dict[str, tuple[int, str]], edges: dict[str, set[str]],
               names: dict[str, str], arm: str) -> dict:
    """The worst static path from ROOT for @p arm.

    @return {"total", "path": [(bytes, qualifier, name)], "missing": [anchor regex],
             "unbounded": [name], "recursion": [(caller, callee)]}.
    """
    name = lambda s: names.get(s, s)  # noqa: E731
    edges = {k: set(v) for k, v in edges.items()}
    syms = sorted(set(frames) | set(edges))
    resolved = []
    for label, callers, targets in INDIRECT_EDGES:
        dsts = {s for s in syms if s in frames and re.search(targets, name(s))}
        srcs = [s for s in syms if INDIRECT in edges.get(s, ()) and re.search(callers, name(s))]
        # Fail closed: an edge that resolves nothing would shrink every row into a pass.
        if not dsts or not srcs:
            raise KeyError(f"indirect edge {label!r} resolved {len(srcs)} caller(s) and "
                           f"{len(dsts)} target(s); both must be found")
        for s in srcs:
            edges[s] |= dsts
        resolved.append((label, sorted(name(c) for c in srcs), sorted(name(d) for d in dsts)))
    platform = re.compile(PLATFORM_CUTS)
    cut = re.compile(ARM_CUTS[arm])
    roots = [s for s in syms if s in frames and re.search(ROOT, name(s))]
    if len(roots) != 1:
        raise KeyError(f"expected one {ROOT!r} in the call graph, found {len(roots)}")
    memo: dict[str, tuple[int, list[str]]] = {}
    on_stack: list[str] = []
    cycles: set[tuple[str, str]] = set()
    sys.setrecursionlimit(max(10000, sys.getrecursionlimit()))

    def best(s: str) -> tuple[int, list[str]]:
        if s in memo:
            return memo[s]
        on_stack.append(s)
        below: tuple[int, list[str]] = (0, [])
        for d in edges.get(s, ()):
            if d in on_stack:
                cycles.add((s, d))
                continue
            if platform.search(_base(name(d))) or cut.search(name(d)):
                continue
            r = best(d)
            if r[0] > below[0]:
                below = r
        on_stack.pop()
        memo[s] = (frames.get(s, (0, ""))[0] + below[0], [s] + below[1])
        return memo[s]

    total, path = best(roots[0])
    on_path = set(path)
    return {
        "total": total,
        "path": [(*frames.get(s, (0, "none")), name(s)) for s in path],
        "missing": [a for a in ANCHORS[arm] if not any(re.search(a, name(s)) for s in path)],
        "unbounded": [name(s) for s in path if frames.get(s, (0, ""))[1] == "dynamic"],
        "recursion": sorted((name(a), name(b)) for a, b in cycles if a in on_path),
        "edges": resolved,
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--build", required=True, help="the image's build dir (searched for *.ci)")
    ap.add_argument("--toolchain", required=True, help="the compiler's --version first line")
    ap.add_argument("--target", required=True)
    ap.add_argument("--cxxfilt", default="c++filt")
    ap.add_argument("--out-json")
    args = ap.parse_args(argv)

    texts = [p.read_text(errors="replace") for p in pathlib.Path(args.build).rglob("*.ci")]
    if not texts:
        print(f"::error::no -fcallgraph-info=su output (*.ci) under {args.build}")
        return 1
    try:
        frames, edges = load(texts)
        names = demangle(sorted(set(frames) | set(edges)), args.cxxfilt)
        rows = {arm: worst_path(frames, edges, names, arm) for arm in STACK_PINS}
    except (KeyError, OSError, subprocess.CalledProcessError) as e:
        print(f"::error::esp32c6 WS client rx stack gate could not read the call graph: {e}")
        return 1

    compared = args.target == PINNED_TARGET
    toolchain_ok = args.toolchain == PINNED_TOOLCHAIN
    lines = ["## WebSocket client rx path, worst static stack (#2871)", "",
             f"| Row | Stack | {PINNED_TARGET} pin | Verdict |", "|---|---:|---:|---|"]
    failed = []
    for row, r in rows.items():
        pin = STACK_PINS[row]
        broken = ([f"missing anchor {a}" for a in r["missing"]]
                  + [f"unbounded dynamic frame in {n[:80]}" for n in r["unbounded"]])
        if broken:
            verdict = "FAIL (" + "; ".join(broken) + ")"
            failed.append(f"{row}: " + "; ".join(broken))
        elif not compared:
            verdict = f"REPORTED (pins hold for {PINNED_TARGET} only)"
        elif not toolchain_ok:
            verdict = "FAIL (not the pinned toolchain)"
            failed.append(f"{row}: not the pinned toolchain")
        elif r["total"] > pin:
            verdict = "FAIL (over its pin: a raise needs a maintainer ruling)"
            failed.append(f"{row}: {r['total']:,} B > pin {pin:,} B")
        else:
            under = pin - r["total"]
            verdict = "PASS" if under == 0 else f"PASS ({under:,} B under: re-pin down)"
        lines.append(f"| {row} | {r['total']:,} B | {pin:,} B | {verdict} |")
    lines += ["", f"Toolchain: `{args.toolchain}`; pins measured with `{PINNED_TOOLCHAIN}`.", "",
              "Indirect edges resolved:", ""]
    for label, srcs, dsts in next(iter(rows.values()))["edges"]:
        lines.append(f"- {label}: {len(srcs)} caller(s) -> {len(dsts)} target(s)")
    lines += ["", "<details><summary>Indirect edges: callers and targets</summary>", "", "```"]
    for label, srcs, dsts in next(iter(rows.values()))["edges"]:
        lines += [f"{label}:"] + [f"  from {_base(c)}" for c in srcs]
        lines += [f"  to   {_base(d)}" for d in dsts]
    lines += ["```", "</details>", ""]
    for row, r in rows.items():
        lines += [f"<details><summary>{row}: {r['total']:,} B</summary>", "", "```"]
        acc = 0
        for b, qual, n in r["path"]:
            acc += b
            lines.append(f"{b:6d} {acc:6d} {qual:16s} {n[:150]}")
        for a, b in r["recursion"]:
            lines.append(f"recursion cut: {a[:70]} -> {b[:70]}")
        lines += ["```", "</details>", ""]
    report = "\n".join(lines)
    print(report)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as f:
            f.write(report + "\n")
    if args.out_json:
        with open(args.out_json, "w", encoding="utf-8") as f:
            json.dump({"target": args.target, "toolchain": args.toolchain,
                       "rows": [{"row": row, "arm": row, "stack_bytes": r["total"],
                                 "pin_bytes": STACK_PINS[row],
                                 "missing_anchors": r["missing"], "recursion": r["recursion"],
                                 "path": [{"frame": b, "qualifier": q, "fn": n}
                                          for b, q, n in r["path"]]}
                                for row, r in rows.items()]}, f, indent=2)
    if failed:
        print(f"::error::the esp32c6 WebSocket client rx stack rows failed (#2871): "
              f"{'; '.join(failed)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
