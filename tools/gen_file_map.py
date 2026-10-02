#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Generate docs/modules/file-map.md: every public header, its module page, its purpose.

The purpose column is each header's own file-level ``@file`` / ``@brief`` block. The
module page comes from PAGES below, the one hand-kept assignment. ``--check`` fails
(exit 1) when the committed page is stale, when a public header has no row in PAGES,
when PAGES names a header or page that does not exist, or when a header has no
``@file`` / ``@brief`` block. Stdlib only; same shape as tools/gen_record_index.py.
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "modules" / "file-map.md"
HEADER_GLOBS = ("core/include/libtracer/*.hpp", "backends/*/include/libtracer/*.hpp")
BLOB = "https://github.com/avatarsd-llc/libtracer/blob/main/"

# Module page (under docs/modules/) -> the public headers it owns. Every public header
# appears exactly once; a new header fails --check until it is placed here.
PAGES = {
    "index.md": ["tracer.hpp"],
    "config.md": ["config.hpp", "reclaim.hpp", "reader_guard.hpp"],
    "status.md": ["status.hpp", "error.hpp"],
    "instrumentation.md": ["pin_instrument.hpp"],
    "segment.md": ["segment.hpp"],
    "backends.md": [
        "backend.hpp", "mem_heap.hpp", "mem_borrowed.hpp", "mem_pool.hpp", "mem_source.hpp",
        "mem_source_alloc.hpp", "mem_source_backend.hpp", "mem_source_pmr.hpp",
        "mem_source_sync.hpp", "mem_cuda.hpp",
    ],
    "views.md": ["view.hpp", "rope.hpp"],
    "frame-codec.md": [
        "tlv.hpp", "frame.hpp", "tlv_emit.hpp", "tlv_arena.hpp", "tlv_view.hpp", "grammar.hpp",
        "rope_decode.hpp", "byteorder.hpp", "crc.hpp", "batch.hpp", "playout.hpp",
        "packed_path.hpp", "path_element.hpp", "path_ref.hpp", "key_view.hpp",
        "length_prefix_framer.hpp",
    ],
    "path.md": ["path.hpp"],
    "graph.md": [
        "graph.hpp", "vertex.hpp", "value.hpp", "hook.hpp", "app_fields.hpp", "subscriber.hpp",
        "vertex_stripe.hpp", "lkv_slot.hpp", "edge_pin.hpp", "qsbr.hpp", "link_id.hpp",
        "rmw_counter.hpp", "thread_id.hpp",
    ],
    "security-acl.md": ["security_acl.hpp", "acl_ace.hpp"],
    "fwd-router.md": [
        "fwd_router.hpp", "fwd_frame_view.hpp", "op_resolve.hpp", "route_handle.hpp",
        "child_registry.hpp", "transport_vertex.hpp", "path_label.hpp", "path_label_table.hpp",
        "sink_slot.hpp",
    ],
    "transport.md": [
        "transport.hpp", "peer_handle.hpp", "loopback.hpp", "transport_udp.hpp",
        "transport_tcp.hpp", "transport_ws.hpp", "ws.hpp", "transport_quic.hpp",
        "transport_webtransport.hpp", "posix_endpoint.hpp", "iov_table.hpp", "tx_handoff.hpp",
        "receiver_slot.hpp", "self_heal_link.hpp", "builtin_transports.hpp",
    ],
    "connection-config.md": ["conn_spec.hpp", "config_reader.hpp", "tls_profile.hpp"],
    "can.md": ["can.hpp", "can_reassembly.hpp", "can_tx_pool.hpp", "view_can.hpp", "transport_can.hpp"],
}

# The first `@file` block, its `@brief` running to a blank comment line, a new command or
# the block's end.
BRIEF_RE = re.compile(r"^ \* @file\n \* @brief (.*?)\n \*(?:\n| @|/)", re.M | re.S)


def brief_of(path):
    """Return the one-line @brief of a header's leading file block, or raise ValueError."""
    m = BRIEF_RE.search(path.read_text(encoding="utf-8"))
    if not m:
        raise ValueError(f"{path.relative_to(ROOT)}: no /** @file @brief */ block")
    return " ".join(line.lstrip(" *") for line in m.group(1).splitlines()).replace("|", "\\|")


def render():
    """Return the full page as a string, or raise ValueError listing every problem."""
    headers = sorted(p for g in HEADER_GLOBS for p in ROOT.glob(g))
    page_of, errors = {}, []
    for page, names in PAGES.items():
        if not (OUT.parent / page).exists():
            errors.append(f"PAGES names a missing page docs/modules/{page}")
        for name in names:
            if name in page_of:
                errors.append(f"{name}: assigned to both {page_of[name]} and {page}")
            page_of[name] = page
    by_name = {}
    for h in headers:
        if h.name in by_name:
            errors.append(f"{h.name}: two public headers share this name")
        by_name[h.name] = h
    errors += [f"{n}: public header with no row in PAGES" for n in sorted(set(by_name) - set(page_of))]
    errors += [f"{n}: in PAGES but no such public header" for n in sorted(set(page_of) - set(by_name))]
    rows = []
    for h in headers:
        try:
            brief = brief_of(h)
        except ValueError as e:
            errors.append(str(e))
            continue
        rel = h.relative_to(ROOT).as_posix()
        page = page_of.get(h.name)
        if page:
            rows.append(f"| [`{rel}`]({BLOB}{rel}) | [{page[:-3]}]({page}) | {brief} |")
    if errors:
        raise ValueError("\n".join(errors))
    return "\n".join([
        "<!-- GENERATED by tools/gen_file_map.py from each header's @file @brief. "
        "Do not edit; run the script. -->",
        "",
        "# File map",
        "",
        "Every public header of the reference C++ implementation, the module page that "
        "documents it, and the header's own one-line purpose (its `@file` `@brief`). The page "
        "is generated by `tools/gen_file_map.py`, and CI fails when a public header has no row "
        "or the page drifts. To place a new header, add it to `PAGES` in that script and run it.",
        "",
        "| Header | Module page | Purpose |",
        "| ---- | ---- | ---- |",
        *rows,
        "",
    ])


def main():
    """Write the page, or with --check compare it against the committed file."""
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if the committed page is stale")
    args = ap.parse_args()
    try:
        page = render()
    except ValueError as e:
        print(f"gen_file_map: {e}", file=sys.stderr)
        return 1
    if args.check:
        if not OUT.exists() or OUT.read_text(encoding="utf-8") != page:
            print(f"gen_file_map: {OUT.relative_to(ROOT)} is stale; "
                  "run `python3 tools/gen_file_map.py` and commit the result", file=sys.stderr)
            return 1
        return 0
    OUT.write_text(page, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
