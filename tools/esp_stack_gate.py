#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Static stack gate for the esp32c6 WebSocket client's receive path (#2871).

The application sizes a receive task's stack once, on NARROW targets, and the outgoing
WebSocket client delivers every message in-call on that task: the router, the terminus, the
graph write and its fan-out all run on it. Growth on that path that nothing gates turns a
library upgrade into a stack overflow on the device with no build-time signal; from v0.21.0
to 51ef6574e it grew by about 1 KiB before anyone noticed (#2871).

This reads the GCC `-fcallgraph-info=su` files (`*.ci`, each node's static frame and its call
edges) that esp-idf.yml's transport-footprint job writes for its image, which links the
client link and attaches it to a router. From `esp_ws_client_link_t::recv_loop` it takes the
worst static path, frame by frame, for each delivery arm:

* **borrowed** (no `memory.rx`): the span receiver, `on_frame_impl` and the arena terminus;
* **owning** (`memory.rx` set): the rope receiver, `on_frame_rope_impl` and either terminus.

Each row is compared with its pin in STACK_PINS, the way tools/esp_size_gate.py compares the
per-transport flash rows: on esp32c6, built by PINNED_TOOLCHAIN (shared with that gate, since
both read the same pinned-digest image), and a row over its pin fails. A row under its pin
passes and says to re-pin down. Raising a pin needs a maintainer ruling.

What the path counts, and what it leaves out, is fixed here so the number means one thing:

* Two indirect calls are resolved, the only ones a static graph cannot see on this path: the
  receiver slot's call into the router's thunk for the arm, and the terminus's `send` of its
  reply back over the link (resolved to the client link's own send).
* Every other indirect call is left out: an application handler or a subscriber's `on_write`
  is the application's frame, not the library's.
* Frames of the platform (newlib, FreeRTOS, the heap, locks, logging, assert) are left out
  (PLATFORM_CUTS). Their depth is IDF's and does not move with a library change.
* `graph_t::write_branch` is left out (BRANCH_CUT): this path is a peer's leaf write (an
  append and its ack). A structured write into a branch vertex is a different path, not this
  gate's row.

Exit 0 = every row within its pin (or reported only, off esp32c6), 1 = broken input, a row
over its pin, or an esp32c6 build by another compiler. Stdlib only.
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

# The worst static path in bytes, per delivery arm, measured on transport_footprint @ esp32c6
# with PINNED_TOOLCHAIN in the image esp-idf.yml pins by digest (#2830). A raise needs a
# maintainer ruling; a cut re-pins down.
STACK_PINS = {
    # #2871: 5,232 B at 9964668f0 (4,080 B at v0.21.0). Cut to 4,496 B: the arena tier no
    # longer copies selector names into a 512 B buffer, the reply table carries 4 stack
    # entries, not 16, and the framing's body/data/hand_up frames merge into the ingress's.
    "borrowed": 4496,
    # #2871: 6,048 B at 9964668f0 (5,776 B at v0.21.0). Cut to 5,792 B by the same reply
    # table and framing merges; the multi-link terminus (rope tier) keeps its name copy.
    "owning": 5792,
}

ROOT = r"^tr::net::esp_ws_client_link_t::recv_loop\(\)$"
# The receive side's functions that call the receiver slot's sink, and the router's thunks
# they reach for each arm (fwd_router_t::attach_link installs one of the two per link).
RX_CALLERS = (r"^tr::net::esp_ws_client_link_t::(recv_loop|framing_end_t)|receiver_slot"
              r"|frame_ingress|ws_framing")
RX_THUNK = r"(attach_link|add_child)\(.*lambda\(void\*, (tr::view::rope_t|std::span)"
# The terminus sends its reply through `transport_t::send`, the client link's own here.
REPLY_CALLERS = r"resolve_terminus"
REPLY_SEND = r"^tr::net::esp_ws_client_link_t::(send|send_on|write_locked)\("
PLATFORM_CUTS = (r"__assert_func|esp_log|^abort|heap_caps|malloc|calloc|realloc|pvPortMalloc"
                 r"|xQueue|xTask|vPort|lock_acquire|_lock_|pthread_|gettimeofday"
                 r"|esp_system_abort|connect_once|printf|__cxa|operator new|std::__throw")
BRANCH_CUT = r"graph_t::write_branch"
# Each arm leaves out the other arm's receive functions.
ARM_CUTS = {
    "borrowed": r"deliver_rope|receiver_slot_t<>::deliver\(|lambda\(void\*, tr::view::rope_t",
    "owning": r"deliver_borrowed|lambda\(void\*, std::span",
}

NODE_RE = re.compile(r'node: \{ title: "([^"]+)" label: "([^"]*)"')
EDGE_RE = re.compile(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"')
FRAME_RE = re.compile(r"\\n(\d+) bytes \((\w+)")
INDIRECT = "__indirect_call"


def _norm(title: str) -> str:
    """A node's symbol: GCC prefixes a TU-local or inline one with its source file's path."""
    return re.sub(r"^[^:]*\.(?:c|cc|cpp|cxx|s|S):", "", title)


def load(ci_texts: list[str]) -> tuple[dict[str, int], dict[str, set[str]]]:
    """Merge `.ci` files into (frame bytes per symbol, call edges per symbol).

    An inline function emitted in several TUs keeps its largest frame.
    """
    frames: dict[str, int] = {}
    edges: dict[str, set[str]] = {}
    for txt in ci_texts:
        for title, label in NODE_RE.findall(txt):
            m = FRAME_RE.search(label)
            if m:
                sym = _norm(title)
                frames[sym] = max(frames.get(sym, 0), int(m.group(1)))
        for src, dst in EDGE_RE.findall(txt):
            edges.setdefault(_norm(src), set()).add(_norm(dst))
    return frames, edges


def demangle(symbols: list[str], cxxfilt: str) -> dict[str, str]:
    """Demangled names, in one c++filt run."""
    out = subprocess.run([cxxfilt], input="\n".join(symbols), capture_output=True, text=True,
                         check=True).stdout.split("\n")
    return dict(zip(symbols, out))


def worst_path(frames: dict[str, int], edges: dict[str, set[str]], names: dict[str, str],
               arm: str) -> tuple[int, list[tuple[int, str]]]:
    """The worst static path from ROOT for @p arm: (total bytes, [(frame, name), ...])."""
    edges = {k: set(v) for k, v in edges.items()}
    syms = sorted(set(frames) | set(edges))
    for callers, targets in ((RX_CALLERS, RX_THUNK), (REPLY_CALLERS, REPLY_SEND)):
        dsts = {s for s in syms if s in frames and re.search(targets, names.get(s, s))}
        for s in syms:
            if INDIRECT in edges.get(s, ()) and re.search(callers, names.get(s, s)):
                edges[s] |= dsts
    cut = re.compile("|".join((PLATFORM_CUTS, BRANCH_CUT, ARM_CUTS[arm])))
    roots = [s for s in syms if s in frames and re.search(ROOT, names.get(s, s))]
    if len(roots) != 1:
        raise KeyError(f"expected one {ROOT!r} in the call graph, found {len(roots)}")
    memo: dict[str, tuple[int, list[str]]] = {}
    on_stack: set[str] = set()
    sys.setrecursionlimit(max(10000, sys.getrecursionlimit()))

    def best(s: str) -> tuple[int, list[str]]:
        if s in memo:
            return memo[s]
        if s in on_stack or (s != roots[0] and cut.search(names.get(s, s))):
            return 0, []
        on_stack.add(s)
        below = max((best(d) for d in edges.get(s, ())), key=lambda r: r[0], default=(0, []))
        on_stack.discard(s)
        memo[s] = (frames.get(s, 0) + below[0], [s] + below[1])
        return memo[s]

    total, path = best(roots[0])
    return total, [(frames.get(s, 0), names.get(s, s)) for s in path]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--build", required=True, help="the image's build dir (searched for *.ci)")
    ap.add_argument("--toolchain", required=True, help="the compiler's --version first line")
    ap.add_argument("--target", required=True)
    ap.add_argument("--cxxfilt", default="c++filt")
    ap.add_argument("--out-json")
    args = ap.parse_args()

    texts = [p.read_text(errors="replace") for p in pathlib.Path(args.build).rglob("*.ci")]
    if not texts:
        print(f"::error::no -fcallgraph-info=su output (*.ci) under {args.build}")
        return 1
    try:
        frames, edges = load(texts)
        syms = sorted(set(frames) | set(edges))
        names = demangle(syms, args.cxxfilt)
        rows = {arm: worst_path(frames, edges, names, arm) for arm in STACK_PINS}
    except (KeyError, OSError, subprocess.CalledProcessError) as e:
        print(f"::error::esp32c6 WS client rx stack gate could not read the call graph: {e}")
        return 1

    compared = args.target == PINNED_TARGET
    toolchain_ok = args.toolchain == PINNED_TOOLCHAIN
    lines = ["## WebSocket client rx path, worst static stack (#2871)", "",
             f"| Arm | Stack | {PINNED_TARGET} pin | Verdict |", "|---|---:|---:|---|"]
    over = []
    for arm, (total, _path) in rows.items():
        pin = STACK_PINS[arm]
        if not compared:
            verdict = f"REPORTED (pins hold for {PINNED_TARGET} only)"
        elif not toolchain_ok:
            verdict = "FAIL (not the pinned toolchain)"
            over.append(f"{arm}: not the pinned toolchain")
        elif total > pin:
            verdict = "FAIL (over its pin: a raise needs a maintainer ruling)"
            over.append(f"{arm}: {total:,} B > pin {pin:,} B")
        else:
            verdict = "PASS" if total == pin else f"PASS ({pin - total:,} B under: re-pin down)"
        lines.append(f"| {arm} | {total:,} B | {pin:,} B | {verdict} |")
    lines += ["", f"Toolchain: `{args.toolchain}`; pins measured with `{PINNED_TOOLCHAIN}`.", ""]
    for arm, (total, path) in rows.items():
        lines += [f"<details><summary>{arm}: {total:,} B</summary>", "", "```"]
        acc = 0
        for b, name in path:
            acc += b
            lines.append(f"{b:6d} {acc:6d}  {name[:160]}")
        lines += ["```", "</details>", ""]
    report = "\n".join(lines)
    print(report)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as f:
            f.write(report + "\n")
    if args.out_json:
        with open(args.out_json, "w", encoding="utf-8") as f:
            json.dump({"target": args.target, "toolchain": args.toolchain,
                       "rows": [{"arm": a, "stack_bytes": t, "pin_bytes": STACK_PINS[a],
                                 "path": [{"frame": b, "fn": n} for b, n in p]}
                                for a, (t, p) in rows.items()]}, f, indent=2)
    if over:
        print(f"::error::the esp32c6 WebSocket client rx stack rows failed their pins (#2871): "
              f"{'; '.join(over)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
