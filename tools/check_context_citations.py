#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Verify that CONTEXT.md's `file:line` code citations still point at what they claim.

CONTEXT.md is the canonical glossary, and it cites exact source locations. Line
numbers drift silently when the cited file gains lines above them: the citation
still resolves, still looks precise, and now points at unrelated code. Two audits
(#725, #726) found the same rot in hand-maintained doc summaries; this makes the
CONTEXT.md instance of it fail loudly instead.

Each entry below pins a citation to a substring the cited line must contain. When
code moves, this fails AND reports the line the anchor moved to, so the fix is
mechanical rather than a re-investigation.

Usage:  python3 tools/check_context_citations.py
Exits non-zero on the first stale citation, listing every one it found.
"""

import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

# (cited location, substring the target line must contain[, scope])
#
# `scope` disambiguates an anchor whose text repeats — the three `:field` depth gates
# are the same statement in three branches. It must appear in the SCOPE_LINES above a
# candidate for that candidate to count, which is what turns "candidates [4 lines]"
# into a single actionable answer.
SCOPE_LINES = 80

ANCHORS = [
    ("core/include/libtracer/tlv.hpp:59", "struct opt_t"),
    ("core/include/libtracer/tlv.hpp:28", "enum class type_t"),
    ("core/src/graph.cpp:1747", "graph_t::set_identity"),
    ("core/src/graph.cpp:1773", "graph_t::read_identity"),
    ("core/src/graph.cpp:2119", 'field.steps[0].name == "identity"'),
    ("core/src/transport_vertex.cpp:64", 'cfg.name("kind")'),
    ("core/src/transport_vertex.cpp:99", "register_child_type"),
    ("core/src/transport_vertex.cpp:128", "register_transport_type"),
    ("core/src/transport_vertex.cpp:150", '"-client" : "-server"'),
    ("core/include/libtracer/transport_vertex.hpp:96", "enum class link_state_t"),
    ("core/src/graph.cpp:1458", "field.steps.size() != 1", 'step0.name == "subscribers"'),
    ("core/src/graph.cpp:1525", "field.steps.size() != 1 || !plain_step(step0)"),
    ("core/src/graph.cpp:1562", "field.steps.size() != 1", 'step0.name == "children"'),
    ("core/src/graph.cpp:1483", "step0.wildcard"),
    ("core/src/graph.cpp:2085", '"children" && !field.steps[0].wildcard'),
    ("core/src/graph.cpp:2163", "!field.steps[0].wildcard", 'field.steps[0].name == "subscribers"'),
    ("core/src/op_resolve_walk.hpp:237", "enum class index_mode_t"),
    ("core/src/op_resolve_walk.hpp:594", 'field.steps[0].name != "subscribers"'),
    ("core/include/libtracer/mem_heap.hpp:149", "try_assign"),
    ("core/include/libtracer/view.hpp:26", "namespace tr::view"),
    ("core/include/libtracer/frame.hpp:23", "namespace tr::wire"),
    ("core/include/libtracer/graph.hpp:43", "namespace tr::graph"),
    ("core/include/libtracer/transport.hpp:29", "namespace tr::net"),
    ("core/include/libtracer/backend.hpp:40", "enum class io_dir_t"),
    ("core/include/libtracer/backend.hpp:101", "class mem_backend_t"),
    ("core/include/libtracer/backend.hpp:145", "before_io"),
    ("core/include/libtracer/grammar.hpp:210", "receiver-resource depth bound"),
    ("core/src/graph.cpp:1013", "!arena"),
    ("core/include/libtracer/segment.hpp:78", "struct segment_t"),
]


def cited_locations(context: str) -> set:
    """Every `path:line` citation in CONTEXT.md, with bare `:line` resolved to the last path.

    Handles the three spellings the document actually uses: `file.hpp:12`, a range
    `file.cpp:12-20`, and a comma list `file.hpp:145,153`. A bare `` `:99` `` inherits
    the most recently named file, which is how CONTEXT.md writes sibling citations.
    """
    tok = re.compile(r"`(core/[a-z_/]+\.(?:hpp|cpp)):([\d,\-]+)`|`:([\d,\-]+)`")
    found, last = set(), None
    for m in tok.finditer(context):
        if m.group(1):
            last, spec = m.group(1), m.group(2)
        elif last:
            spec = m.group(3)
        else:
            continue
        for part in spec.split(","):
            start = part.split("-")[0]
            if start.isdigit():
                found.add(f"{last}:{start}")
    return found


def main() -> int:
    context = (REPO / "CONTEXT.md").read_text()
    present = cited_locations(context)
    failures, drifted = [], []

    for entry in ANCHORS:
        loc, anchor = entry[0], entry[1]
        scope = entry[2] if len(entry) > 2 else None
        path, lineno = loc.rsplit(":", 1)
        lineno = int(lineno)
        src = (REPO / path)
        if not src.exists():
            failures.append(f"{loc}: file does not exist")
            continue
        lines = src.read_text().split("\n")
        if lineno > len(lines):
            failures.append(f"{loc}: past EOF ({len(lines)} lines)")
            continue
        if anchor in lines[lineno - 1]:
            continue
        # Drifted — find where the anchor went, so the fix is mechanical.
        hits = [i + 1 for i, ln in enumerate(lines) if anchor in ln]
        if scope:
            hits = [h for h in hits if any(scope in x for x in lines[max(0, h - SCOPE_LINES) : h])]
        where = f" -> now at {hits[0]}" if len(hits) == 1 else f" -> candidates {hits}" if hits else " -> anchor GONE"
        drifted.append(f"{loc}: expected {anchor!r}{where}\n      actual: {lines[lineno - 1].strip()[:90]}")

    # An anchor that no longer appears in CONTEXT.md is a dead pin.
    for entry in ANCHORS:
        loc = entry[0]
        path, lineno = loc.rsplit(":", 1)
        if loc not in present and f"{path}:{lineno}" not in present:
            failures.append(f"{loc}: pinned here but CONTEXT.md no longer cites it — drop the anchor")

    for f in failures:
        print(f"FAIL  {f}")
    for d in drifted:
        print(f"DRIFT {d}")

    if failures or drifted:
        print(f"\n{len(failures) + len(drifted)} stale citation(s) in CONTEXT.md.")
        return 1
    print(f"OK    {len(ANCHORS)} CONTEXT.md citations verified against source.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
