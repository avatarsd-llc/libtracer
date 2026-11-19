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
  the receive side into the arm's router thunk, a remote edge into the router's delivery
  lambda, a woken await into its thunk, the router (and only fwd_router.cpp's file-local
  helpers) into the link's send, and CAN into its raw link; an edge that resolves no caller or
  no target fails the run;
* each arm leaves out the other arm's thunk, the platform's frames and the reported-only paths;
* the gate fails a row over its pin, a build by another compiler, a walk that misses an arm's
  anchor frame, and an unbounded dynamic frame on the path; a recursion is cut and named.

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import contextlib
import io
import os
import pathlib
import sys
import tempfile
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
WAKE = "tr::graph::vertex_t::wake_waiters()"
REMOTE_EDGE = "tr::graph::graph_t::dispatch_edge_remote(G)"
AWAIT_THUNK = "tr::net::fwd_router_t::await_fired_thunk(void*, tr::graph::await_waiter_t&)"
REMOTE = ("tr::net::fwd_router_t::fwd_router_t(H)::{lambda(void*, tr::graph::remote_delivery_t "
          "const&, tr::graph::value_t const&)#1}")
ANSWER = "tr::net::fwd_router_t::answer_await(I)"
DELIVER = "tr::net::fwd_router_t::deliver_remote(J)"
CAN_SEND = "tr::net::can_transport_t::send(K)"
TWAI = "tr::net::twai_link_t::write_raw(L)"
ASSERT = "__assert_func"
TOOLCHAIN = gate.PINNED_TOOLCHAIN


def ci(nodes, edges, tu="a.cpp", qual=None):
    """@brief One `.ci` file: {name: frame bytes} nodes, [(src, dst)] edges, prefixed by @p tu.

    @p qual maps a name to its GCC qualifier (default `static`).
    """
    qual = qual or {}
    out = [f'graph: {{ title: "{tu}"']
    for n, b in nodes.items():
        out.append(f'node: {{ title: "/src/{tu}:{n}" label: "{n}\\n/src/{tu}:1:1\\n{b} bytes '
                   f'({qual.get(n, "static")})" }}')
    for s, d in edges:
        dst = gate.INDIRECT if d == gate.INDIRECT else f"/src/{tu}:{d}"
        out.append(f'edge: {{ sourcename: "/src/{tu}:{s}" targetname: "{dst}" label: "x" }}')
    return "\n".join(out + ["}"])


def run(texts, arm, reported_only=False):
    """@brief The gate's walk over @p texts: (total, [(bytes, name)]) for @p arm."""
    frames, edges = gate.load(texts)
    names = {s: s for s in set(frames) | set(edges)}
    r = gate.worst_path(frames, edges, names, arm, reported_only)
    return r["total"], [(b, n) for b, _q, n in r["path"]]


def walk(texts, arm):
    """@brief The gate's full result for @p arm."""
    frames, edges = gate.load(texts)
    return gate.worst_path(frames, edges, {s: s for s in set(frames) | set(edges)}, arm)


def gate_main(texts, target="esp32c6", toolchain=TOOLCHAIN):
    """@brief Run the gate's `main` over @p texts in a temp build dir: (exit code, output).

    The names are already demangled, so `cat` stands in for c++filt.
    """
    with tempfile.TemporaryDirectory() as d:
        for i, t in enumerate(texts):
            pathlib.Path(d, f"t{i}.ci").write_text(t)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = gate.main(["--build", d, "--toolchain", toolchain, "--target", target,
                            "--cxxfilt", "cat"])
        return rc, out.getvalue()


# The hook, send and CAN nodes sit off the path (nothing calls them) unless a test links them,
# so every indirect edge resolves and the base path stays the receive and terminus path.
FRAMES = {RECV: 200, INGRESS: 250, SPAN: 64, ROPE: 80, IMPL: 600, ROPE_IMPL: 608, TERM: 560,
          RESOLVE: 880, WRITE: 128, BRANCH: 4500, SEND: 176, ASSERT: 300, WAKE: 32,
          REMOTE_EDGE: 48, AWAIT_THUNK: 16, REMOTE: 0, ANSWER: 240, DELIVER: 208, CAN_SEND: 48,
          TWAI: 48}
EDGES = [(RECV, INGRESS), (INGRESS, gate.INDIRECT), (SPAN, IMPL), (ROPE, ROPE_IMPL),
         (ROPE_IMPL, IMPL), (IMPL, TERM), (TERM, RESOLVE), (TERM, gate.INDIRECT),
         (RESOLVE, WRITE), (WRITE, BRANCH), (WRITE, ASSERT), (WAKE, gate.INDIRECT),
         (REMOTE_EDGE, gate.INDIRECT), (AWAIT_THUNK, ANSWER), (ANSWER, gate.INDIRECT),
         (REMOTE, DELIVER), (DELIVER, gate.INDIRECT), (CAN_SEND, gate.INDIRECT)]


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

    def test_a_fired_await_walks_into_its_answer_and_the_link_send(self):
        frames = dict(FRAMES, **{SEND: 3000})
        _total, path = run([ci(frames, EDGES + [(WRITE, WAKE)])], "borrowed")
        self.assertEqual([n for _, n in path][-5:], [WRITE, WAKE, AWAIT_THUNK, ANSWER, SEND])

    def test_a_remote_edge_walks_into_the_router_lambda_and_its_delivery(self):
        frames = dict(FRAMES, **{SEND: 3000})
        _total, path = run([ci(frames, EDGES + [(WRITE, REMOTE_EDGE)])], "borrowed")
        self.assertEqual([n for _, n in path][-4:], [REMOTE_EDGE, REMOTE, DELIVER, SEND])

    def test_a_frame_that_calls_only_an_app_handler_resolves_no_hook(self):
        gated = "tr::graph::graph_t::read_handler_gated(M) const"
        frames = dict(FRAMES, **{SEND: 0, gated: 3000})
        edges = EDGES + [(WRITE, gated), (gated, gate.INDIRECT)]
        _total, path = run([ci(frames, edges)], "borrowed")
        self.assertEqual(path[-1][1], gated)

    def test_a_file_local_send_caller_counts_only_in_the_router_tu(self):
        mangled = "_ZN2tr3net12_GLOBAL__N_14helpEv"
        frames = dict(FRAMES, **{SEND: 3000})
        for tu, sends in (("fwd_router.cpp", True), ("other.cpp", False)):
            texts = [ci(frames, EDGES),
                     ci({mangled: 16, WRITE: 128}, [(mangled, gate.INDIRECT), (WRITE, mangled)],
                        tu=tu)]
            loaded = gate.load(texts)
            names = {s: s for s in set(loaded[0]) | set(loaded[1])}
            key = f"{tu}:{mangled}"
            names[key] = f"{tu}:tr::net::(anonymous namespace)::help()"
            with self.subTest(tu=tu):
                self.assertIn(key, loaded[0])
                r = gate.worst_path(*loaded, names, "borrowed")
                # Through the helper, the send sits below the deep resolver; elsewhere the
                # worst path is the terminus's own send and never visits the helper.
                self.assertEqual(names[key] in [n for _b, _q, n in r["path"]], sends)

    def test_a_can_send_walks_into_its_raw_link(self):
        frames = dict(FRAMES, **{SEND: 0, CAN_SEND: 3000})
        _total, path = run([ci(frames, EDGES)], "borrowed")
        self.assertEqual([n for _, n in path][-3:], [TERM, CAN_SEND, TWAI])

    def test_an_edge_that_resolves_nothing_fails_closed(self):
        # Each edge's one target or caller renamed away: either router thunk, the remote
        # edge's dispatcher, the await's waker, the CAN raw link.
        for gone in ({AWAIT_THUNK}, {REMOTE}, {REMOTE_EDGE}, {WAKE}, {TWAI}):
            frames = {k: v for k, v in FRAMES.items() if k not in gone}
            edges = [e for e in EDGES if not gone & set(e)]
            with self.subTest(gone=sorted(gone)), self.assertRaises(KeyError):
                run([ci(frames, edges)], "borrowed")
        frames = {k: v for k, v in FRAMES.items() if k != TWAI}
        rc, out = gate_main([ci(frames, [e for e in EDGES if TWAI not in e])])
        self.assertEqual(rc, 1)
        self.assertIn("'CAN raw link' resolved", out)

    def test_main_lists_the_edges_it_resolved(self):
        _rc, out = gate_main([ci(FRAMES, EDGES)])
        self.assertIn("- remote delivery: 1 caller(s) -> 1 target(s)", out)
        self.assertIn("- await fired: 1 caller(s) -> 1 target(s)", out)
        self.assertIn(f"  from {gate._base(DELIVER)}", out)
        self.assertIn("- router sends: 3 caller(s) -> 2 target(s)", out)

    def test_inline_frames_merge_across_tus_at_their_largest(self):
        other = ci({TERM: 700}, [], tu="b.cpp")
        total, _ = run([ci(FRAMES, EDGES), other], "borrowed")
        self.assertEqual(total, 200 + 250 + 64 + 600 + 700 + 880 + 128)

    def test_reported_only_rows_walk_the_cut_paths(self):
        total, path = run([ci(FRAMES, EDGES)], "borrowed", reported_only=True)
        self.assertEqual(path[-1][1], BRANCH)
        self.assertEqual(total, 200 + 250 + 64 + 600 + 560 + 880 + 128 + 4500)

    def test_platform_cut_matches_the_name_without_its_parameters(self):
        name = "tr::graph::graph_t::write2(heap_caps_t)"
        frames = dict(FRAMES, **{name: 9000})
        _total, path = run([ci(frames, EDGES + [(WRITE, name)])], "borrowed")
        self.assertEqual(path[-1][1], name)

    def test_recursion_is_cut_and_named(self):
        r = walk([ci(FRAMES, EDGES + [(WRITE, IMPL)])], "borrowed")
        self.assertIn((WRITE, IMPL), r["recursion"])

    def test_qualifier_is_kept_and_unbounded_dynamic_is_flagged(self):
        r = walk([ci(FRAMES, EDGES, qual={RESOLVE: "dynamic"})], "borrowed")
        self.assertIn((880, "dynamic", RESOLVE), r["path"])
        self.assertEqual(r["unbounded"], [RESOLVE])
        r = walk([ci(FRAMES, EDGES, qual={RESOLVE: "dynamic,bounded"})], "borrowed")
        self.assertEqual(r["unbounded"], [])

    def test_missing_anchor_is_named(self):
        frames = {k: v for k, v in FRAMES.items() if k != TERM}
        edges = [e for e in EDGES if TERM not in e] + [(IMPL, RESOLVE)]
        r = walk([ci(frames, edges)], "borrowed")
        self.assertEqual(r["missing"], [gate.ANCHORS["borrowed"][2]])

    def test_main_passes_at_or_under_the_pins(self):
        rc, out = gate_main([ci(FRAMES, EDGES)])
        self.assertEqual(rc, 0, out)
        self.assertIn("re-pin down", out)  # the fixture is far under the real pins

    def test_main_fails_a_row_over_its_pin(self):
        frames = dict(FRAMES, **{RESOLVE: 20000})
        rc, out = gate_main([ci(frames, EDGES)])
        self.assertEqual(rc, 1)
        self.assertIn("over its pin", out)

    def test_main_fails_another_toolchain_on_esp32c6_only(self):
        rc, out = gate_main([ci(FRAMES, EDGES)], toolchain="g++ 99")
        self.assertEqual(rc, 1)
        self.assertIn("not the pinned toolchain", out)
        rc, _ = gate_main([ci(FRAMES, EDGES)], target="esp32s3", toolchain="g++ 99")
        self.assertEqual(rc, 0)

    def test_main_fails_a_missing_anchor_and_an_unbounded_frame(self):
        frames = {k: v for k, v in FRAMES.items() if k != TERM}
        edges = [e for e in EDGES if TERM not in e] + [(IMPL, RESOLVE)]
        rc, out = gate_main([ci(frames, edges)])
        self.assertEqual(rc, 1)
        self.assertIn("missing anchor", out)
        rc, out = gate_main([ci(FRAMES, EDGES, qual={IMPL: "dynamic"})])
        self.assertEqual(rc, 1)
        self.assertIn("unbounded dynamic frame", out)

    def test_main_fails_without_call_graph_files(self):
        rc, out = gate_main([])
        self.assertEqual(rc, 1)
        self.assertIn("no -fcallgraph-info", out)

    def test_missing_root_is_an_error(self):
        frames = {k: v for k, v in FRAMES.items() if k != RECV}
        with self.assertRaises(KeyError):
            run([ci(frames, [e for e in EDGES if RECV not in e])], "borrowed")


if __name__ == "__main__":
    unittest.main()
