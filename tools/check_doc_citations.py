#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Verify that the docs' `file:line` code citations still point at what they claim.

The documentation cites exact source locations — CONTEXT.md (the canonical
glossary), the module pages, and the design notes. Line numbers drift silently
when the cited file gains lines above them: the citation still resolves, still
looks precise, and now points at unrelated code. #725/#726 found this rot in
hand-maintained doc summaries, #727 found 11 stale citations in CONTEXT.md, and
#728 found 29 more across the design and module pages — every one of them in
`graph.cpp` or `fwd_router.cpp`, the two files that churn. This makes it fail
loudly instead.

Each entry below pins a citation to a substring the cited line must contain. When
code moves, this fails AND reports the line the anchor moved to, so the fix is
mechanical rather than a re-investigation.

Usage:  python3 tools/check_doc_citations.py
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
    ("core/src/graph.cpp:1758", "graph_t::set_identity"),
    ("core/src/graph.cpp:1784", "graph_t::read_identity"),
    ("core/src/graph.cpp:2117", 'field.steps[0].name == "identity"'),
    ("core/src/transport_vertex.cpp:64", 'cfg.name("kind")'),
    ("core/src/transport_vertex.cpp:99", "register_child_type"),
    ("core/src/transport_vertex.cpp:128", "register_transport_type"),
    ("core/src/transport_vertex.cpp:133", "transport_vertex_t::register_module"),
    ("core/src/transport_vertex.cpp:157", "SCHEMA_NOT_FOUND", "transport_vertex_t::module_for"),
    # fwd-router.md's "Signature source" line — bare :NNN shorthands that had ALL rotted
    # silently (they cited the pre-#739 header). Anchored so they cannot rot again.
    # zero-copy-and-flatten.md's rope-tier citations and ADR-0072's stale-comment pointer —
    # all four had rotted on main and were re-asserted by a mechanical +24 shift (#768 verify).
    ("core/include/libtracer/fwd_router.hpp:522", "Terminus over a MULTI-LINK rope"),
    ("core/include/libtracer/fwd_router.hpp:528", "64 KB / 2 links"),
    ("core/include/libtracer/fwd_router.hpp:540", "The forward hop, read entirely by OFFSET"),
    ("core/include/libtracer/fwd_router.hpp:432", "Slot addresses are NOT stable"),
    ("core/include/libtracer/fwd_router.hpp:139", "explicit fwd_router_t"),
    ("core/include/libtracer/fwd_router.hpp:198", "bool add_child"),
    ("core/include/libtracer/fwd_router.hpp:248", "subscribe_toward"),
    ("core/include/libtracer/fwd_router.hpp:258", "using reply_fn_t"),
    ("core/include/libtracer/fwd_router.hpp:269", "using stale_label_fn_t"),
    ("core/include/libtracer/child_registry.hpp:209", "bool add(std::string name"),
    ("core/include/libtracer/child_registry.hpp:458", "resolve_peer"),
    ("core/include/libtracer/child_registry.hpp:473", "bool erase"),
    ("core/include/libtracer/child_registry.hpp:499", "entry_by_name"),
    ("core/include/libtracer/child_registry.hpp:520", "by_name"),
    ("core/include/libtracer/child_registry.hpp:561", "std::size_t size()"),
    ("core/include/libtracer/child_registry.hpp:571", "live_size"),
    ("core/src/transport_vertex.cpp:160", "transport_vertex_t::provide_link"),
    ("core/src/transport_vertex.cpp:213", "routing key IS the mount path"),
    ("core/src/transport_vertex.cpp:220", "qualified += name"),
    ("core/src/transport_vertex.cpp:239", "grouping vertex, created lazily"),
    ("core/src/transport_vertex.cpp:247", "register_vertex_key(mod_key"),
    ("core/src/transport_vertex.cpp:327", "if (constructed)"),
    ("core/src/transport_vertex.cpp:329", "link_state_t::LISTENING : link_state_t::UP"),
    ("core/include/libtracer/transport_vertex.hpp:266", "result_t<void> register_module"),
    ("core/include/libtracer/transport_vertex.hpp:96", "enum class link_state_t"),
    ("core/src/graph.cpp:1499", "field.steps.size() != 1", 'step0.name == "subscribers"'),
    ("core/src/graph.cpp:1566", "field.steps.size() != 1 || !plain_step(step0)"),
    ("core/src/graph.cpp:1603", "field.steps.size() != 1", 'step0.name == "children"'),
    ("core/src/graph.cpp:1524", "step0.wildcard"),
    ("core/src/graph.cpp:2083", '"children" && !field.steps[0].wildcard'),
    ("core/src/graph.cpp:2183", "!field.steps[0].wildcard", 'field.steps[0].name == "subscribers"'),
    ("core/src/op_resolve_walk.hpp:255", "enum class index_mode_t"),
    ("core/src/op_resolve_walk.hpp:645", 'field.steps[0].name != "subscribers"'),
    ("core/include/libtracer/mem_heap.hpp:149", "try_assign"),
    ("core/include/libtracer/view.hpp:26", "namespace tr::view"),
    ("core/include/libtracer/frame.hpp:23", "namespace tr::wire"),
    ("core/include/libtracer/graph.hpp:43", "namespace tr::graph"),
    ("core/include/libtracer/transport.hpp:29", "namespace tr::net"),
    ("core/include/libtracer/backend.hpp:40", "enum class io_dir_t"),
    ("core/include/libtracer/backend.hpp:101", "class mem_backend_t"),
    ("core/include/libtracer/backend.hpp:145", "before_io"),
    ("core/include/libtracer/grammar.hpp:210", "receiver-resource depth bound"),
    ("core/src/graph.cpp:1025", "!arena"),
    ("core/include/libtracer/segment.hpp:78", "struct segment_t"),
    # --- the design + module pages (#728). Every one of these had drifted. ---
    ("core/src/graph.cpp:733", "has_registered_child()"),
    ("core/src/graph.cpp:823", "void graph_t::fan_out"),
    ("core/src/graph.cpp:946", "graph_t::write_impl"),
    ("core/src/graph.cpp:1008", "value.materialize(*value_backend_)", "graph_t::write_branch"),
    ("core/src/graph.cpp:1009", "head.empty() && value.total_length()", "graph_t::write_branch"),
    ("core/src/graph.cpp:1021", "std::array<std::byte, 4096> stack;"),
    ("core/src/graph.cpp:1022", "bump_source_t src(stack"),
    ("core/src/graph.cpp:1024", "decode_into(head.bytes(), src)"),
    ("core/src/graph.cpp:1040", "std::vector<std::byte> root_key;"),
    ("core/src/graph.cpp:1041", "try_build_key(v, root_key)"),
    ("core/src/graph.cpp:1043", "try_assign(parse_key, root_key)"),
    ("core/src/graph.cpp:1245", "value.materialize(*value_backend_)", "field_write read it back"),
    ("core/src/graph.cpp:1478", "result_t<void> graph_t::field_write"),
    ("core/src/graph.cpp:1605", "acl_right_t::CREATE", 'step0.name == "children"'),
    ("core/src/fwd_router.cpp:1413", "fwd_router_t::deliver_remote"),
    ("core/src/fwd_router.cpp:1441", "value.materialize(*flat_)"),
    ("core/src/fwd_router.cpp:1442", "flatten OOM"),
    ("core/src/fwd_router.cpp:1446", "try_encode_compact", "fwd_router_t::deliver_remote"),
    ("core/src/fwd_router.cpp:1478", "std::vector<std::span<const std::byte>> iov;", "fwd_router_t::deliver_remote"),
    # #730 — the two INGRESS flatten guards. Anchored because the whole point of the
    # seam is that these are testable; a citation to them silently rotting would be the
    # first step back to "the guard nobody can prove still works".
    ("core/src/fwd_router.cpp:1094", "route_flat.empty()"),
    ("core/src/fwd_router.cpp:1114", "payload_flat.empty()"),
    ("core/src/fwd_router.cpp:1107", "frame.subrope(head->child1_off, head->child1_total).materialize", "case type_t::COMPACT"),
    ("core/src/fwd_router.cpp:767", "frame.subrope(0, frame.total_length()).materialize"),
    ("core/include/libtracer/vertex.hpp:2387", "vertex_t* parent_"),
]


def cited_locations(context: str) -> set:
    """Every `path:line` citation in CONTEXT.md, with bare `:line` resolved to the last path.

    Handles every spelling the docs actually use: `file.hpp:12`, a range
    `file.cpp:12-20` (which registers EVERY line in it, not just the first — a doc
    citing `996-997` is citing both), a comma list `file.hpp:145,153`, and the
    UNBACKTICKED form that appears inside annotated code-excerpt blocks. A bare
    `` `:99` `` inherits the most recently named file, which is how CONTEXT.md
    writes sibling citations.
    """
    tok = re.compile(r"`?(core/[a-z_/]+\.(?:hpp|cpp)):([\d,\-]+)`?|`:([\d,\-]+)`")
    found, last = set(), None
    for m in tok.finditer(context):
        if m.group(1):
            last, spec = m.group(1), m.group(2)
        elif last:
            spec = m.group(3)
        else:
            continue
        for part in spec.split(","):
            ends = part.split("-")
            if not ends[0].isdigit():
                continue
            lo = int(ends[0])
            hi = int(ends[1]) if len(ends) > 1 and ends[1].isdigit() else lo
            # A citation is a pointer, not a listing — an implausible span is a
            # parse artifact (a hyphenated word), so ignore it rather than flood.
            if hi < lo or hi - lo > 40:
                hi = lo
            for n in range(lo, hi + 1):
                found.add(f"{last}:{n}")
    return found


def all_docs() -> list:
    """Every tracked markdown file. `_build` is generated Sphinx output; the
    `.claude/worktrees/fw-pin-*` trees are pinned history. Neither is a source."""
    skip = ("_build", "node_modules", ".claude", ".git")
    # Match skip components against the path RELATIVE to the repo root — an absolute
    # match made the tool skip EVERY doc when run from a `.claude/worktrees/*` checkout,
    # turning the whole check into a vacuous pass there.
    return [p for p in REPO.rglob("*.md")
            if not any(s in p.relative_to(REPO).parts for s in skip)]


def main() -> int:
    present = set()
    for doc in all_docs():
        try:
            present |= cited_locations(doc.read_text())
        except (OSError, UnicodeDecodeError):
            continue
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
            failures.append(f"{loc}: pinned here but no doc cites it any more — drop the anchor")

    for f in failures:
        print(f"FAIL  {f}")
    for d in drifted:
        print(f"DRIFT {d}")

    if failures or drifted:
        print(f"\n{len(failures) + len(drifted)} stale citation(s) in the docs.")
        return 1
    print(f"OK    {len(ANCHORS)} doc citations verified against source.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
