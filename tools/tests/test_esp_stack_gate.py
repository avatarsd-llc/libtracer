#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the esp32c6 WebSocket client rx stack gate (`tools/esp_stack_gate.py`, #2871).

The gate reads GCC `-fcallgraph-info=su` output, so these drive it with that output's shape,
built by a small helper, without a cross toolchain. The names here are already demangled, so an
identity "demangler" stands in for c++filt:

* a TU-local node's file prefix is dropped, and an inline function emitted in two TUs keeps
  its larger frame;
* the worst path follows direct calls, and an indirect call only where the gate resolves one:
  the receive side into the arm's router thunk, and the terminus into the link's send;
* each arm leaves out the other arm's thunk, the platform's frames and the branch write.

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import os
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))

import esp_stack_gate as gate  # noqa: E402

RECV = "tr::net::esp_ws_client_link_t::recv_loop()"
INGRESS = "int tr::net::frame_ingress_t<14u, 0u>::advance<X>(Y)"
SPAN = "tr::net::fwd_router_t::attach_link(Z)::{lambda(void*, std::span<std::byte const>)#2}"
ROPE = "tr::net::fwd_router_t::attach_link(Z)::{lambda(void*, tr::view::rope_t)#1}"
IMPL = "tr::net::fwd_router_t::on_frame_impl(A)"
ROPE_IMPL = "tr::net::fwd_router_t::on_frame_rope_impl(A)"
TERM = "tr::net::fwd_router_t::resolve_terminus(B)"
RESOLVE = "tr::graph::op_resolver_t::resolve(C)"
WRITE = "tr::graph::graph_t::write(D)"
BRANCH = "tr::graph::graph_t::write_branch(E)"
SEND = "tr::net::esp_ws_client_link_t::send(F)"
ASSERT = "__assert_func"


def ci(nodes, edges, tu="a.cpp"):
    """@brief One `.ci` file: {name: frame bytes} nodes, [(src, dst)] edges, prefixed by @p tu."""
    out = [f'graph: {{ title: "{tu}"']
    for n, b in nodes.items():
        out.append(f'node: {{ title: "/src/{tu}:{n}" label: "{n}\\n/src/{tu}:1:1\\n{b} bytes '
                   f'(static)" }}')
    for s, d in edges:
        dst = gate.INDIRECT if d == gate.INDIRECT else f"/src/{tu}:{d}"
        out.append(f'edge: {{ sourcename: "/src/{tu}:{s}" targetname: "{dst}" label: "x" }}')
    return "\n".join(out + ["}"])


def run(texts, arm):
    frames, edges = gate.load(texts)
    names = {s: s for s in set(frames) | set(edges)}
    return gate.worst_path(frames, edges, names, arm)


FRAMES = {RECV: 200, INGRESS: 250, SPAN: 64, ROPE: 80, IMPL: 600, ROPE_IMPL: 608, TERM: 560,
          RESOLVE: 880, WRITE: 128, BRANCH: 4500, SEND: 176, ASSERT: 300}
EDGES = [(RECV, INGRESS), (INGRESS, gate.INDIRECT), (SPAN, IMPL), (ROPE, ROPE_IMPL),
         (ROPE_IMPL, IMPL), (IMPL, TERM), (TERM, RESOLVE), (TERM, gate.INDIRECT),
         (RESOLVE, WRITE), (WRITE, BRANCH), (WRITE, ASSERT)]


class StackGateTest(unittest.TestCase):
    def test_borrowed_arm_walks_the_span_thunk_to_the_leaf_write(self):
        total, path = run([ci(FRAMES, EDGES)], "borrowed")
        self.assertEqual([n for _, n in path], [RECV, INGRESS, SPAN, IMPL, TERM, RESOLVE, WRITE])
        self.assertEqual(total, 200 + 250 + 64 + 600 + 560 + 880 + 128)

    def test_owning_arm_takes_the_rope_thunk(self):
        total, path = run([ci(FRAMES, EDGES)], "owning")
        self.assertIn(ROPE_IMPL, [n for _, n in path])
        self.assertNotIn(SPAN, [n for _, n in path])
        self.assertEqual(total, 200 + 250 + 80 + 608 + 600 + 560 + 880 + 128)

    def test_reply_send_counts_when_it_is_the_deeper_branch(self):
        frames = dict(FRAMES, **{SEND: 2000})
        _total, path = run([ci(frames, EDGES)], "borrowed")
        self.assertEqual(path[-1][1], SEND)

    def test_other_indirect_calls_are_not_resolved(self):
        edges = EDGES + [(RESOLVE, gate.INDIRECT)]
        frames = dict(FRAMES, **{SEND: 2000})
        _total, path = run([ci(frames, edges)], "borrowed")
        self.assertEqual([n for _, n in path][-2:], [TERM, SEND])

    def test_inline_frames_merge_across_tus_at_their_largest(self):
        other = ci({TERM: 700}, [], tu="b.cpp")
        total, _ = run([ci(FRAMES, EDGES), other], "borrowed")
        self.assertEqual(total, 200 + 250 + 64 + 600 + 700 + 880 + 128)

    def test_missing_root_is_an_error(self):
        frames = {k: v for k, v in FRAMES.items() if k != RECV}
        with self.assertRaises(KeyError):
            run([ci(frames, [e for e in EDGES if RECV not in e])], "borrowed")


if __name__ == "__main__":
    unittest.main()
