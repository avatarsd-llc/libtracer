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

The docs spell a citation three ways, and all three are read here (#803). A full
repo-relative path (`core/src/graph.cpp:956`), the design pages' established
BASENAME shorthand (`graph.hpp:1291`), and a bare continuation (`:371`) that
inherits the file most recently named. The shorthand is resolved against a map of
every source basename in the tree; a basename carried by two files is an ERROR,
not a guess — the doc must spell the full path. Generated headers (`config.hpp.in`)
count as sources, so the configuration pages' knob citations are pinnable too.

Coverage is the pin list, not the doc set: a citation with no entry below is NOT checked,
so `OK N verified` counts the pins, never "every citation in the docs". Build and tooling
files are not sources and used to be unreadable here at all, which is how two rotted
`LIBTRACER_NO_ATOMIC` citations sat beside a verified one in the same sentence (#1052).
@ref CITABLE_NON_SOURCE_PATHS enrols the ones whose citations ARE pinned — an explicit
allowlist, because covering non-source files wholesale is a maintainer's call.

What #1052 left open, and #1095 closed: everything OUTSIDE that allowlist was a FALSE
GREEN, not merely unchecked. A line-numbered citation of a `.yml`, a `.txt` or a `.mjs`
matched nothing here, so the gate exited 0 whether it resolved or not — and a PR read that
silence as "no cited file shifted lines" while its own diff had moved a cited workflow 18
lines. @ref unverifiable_citations makes that class an ERROR: a citation carrying a line
number is verified, or the author must enrol the file or drop the line number. A token
naming no file in the tree stays ignored — `127.0.0.1:47301` is not a citation.

Historical genres are deliberately NOT enrolled. `docs/adr/`, `docs/spec/` and
`docs/research/` are dated records of a decision: their citations describe the tree as it
stood, some already point past today's EOF, and pinning them would demand rewriting
history on every refactor.

`--repin` is the other half (#836): when a source edit HAS moved cited lines, it rewrites
every citation spelling from a line map instead of leaving a `sed` sweep to find them.
Three rules are load-bearing there, each paid for:

* **One pass.** A re-pin builds the whole map first and rewrites the ORIGINAL text once.
  A sequential pass feeds its own output — rewrite `1114 -> 1118` and the next rule in
  the same sweep sees `1118` and moves it again. That is how one sweep turned 51 stale
  citations into 60.
* **Both endpoints, every element.** `file:1113-1118` is two line numbers and
  `file:181,339` is two more. Mapping only the head is what leaves inverted ranges like
  `graph.cpp:1118-1114` behind, so a spec that cannot be mapped end to end is reported
  and left ALONE rather than half-applied.
* **"Verify by content" is vacuous.** Checking that the new line holds the text the old
  line held proves nothing under a uniform shift: there, old and new hold identical text
  for EVERY line, whether or not that line needed moving. The real check is whether the
  citation ALREADY RESOLVES in the current tree — which is what the anchor table
  answers, and why an anchor that still resolves contributes a fixed point and is never
  rewritten.
* **The dated genres are read-only.** `--repin` writes only the LIVING doc surfaces, for
  the same reason those genres are not enrolled above: an ADR or an RFC cites the tree as
  it stood, and moving its citations forward rewrites the record. Their moves are counted
  and reported, never applied.

Usage:  python3 tools/check_doc_citations.py
        python3 tools/check_doc_citations.py --repin [--from-rev REV] [--apply]
Exits non-zero on the first stale citation, listing every one it found.
Gated by `.github/workflows/doc-citations.yml`; unit tests in
`tools/tests/test_check_doc_citations.py`.
"""

import argparse
import difflib
import functools
import os
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

# Extensions a citation may name. `.hpp.in` is the CMake-configured header template
# (`config.hpp.in`) — the only place the compile-time knobs are *declared*, and what
# the configuration pages therefore cite; the generated `config.hpp` is a build
# artifact and is not in the tree.
SOURCE_SUFFIXES = (".hpp.in", ".hpp", ".cpp", ".cc", ".hh", ".h")

# Directories that hold no citable source: build output, vendored deps, worktrees.
# `.pio` is PlatformIO's per-project cache: `pio run` in the packaging fixture unpacks the
# library UNDER TEST into `.pio/libdeps/<env>/libtracer/`, which is a second copy of every
# core source. Without this the gate turns red the moment anyone runs that fixture locally,
# for the same reason `build-` is here — a basename with two paths is ambiguous, and the
# gate must not depend on which builds someone happens to have run in the tree.
NON_SOURCE_DIRS = (
    "_build", "build", "node_modules", ".claude", ".git", "target", "dist", ".pio",
)
# ...and the same, for any `build-<something>` sibling. The exact name "build" is not the
# only one that appears: this repo's agent workflow mandates `build-agent` (`.gitignore`
# covers `build-*/`), and a generated `build-agent/generated/include/libtracer/config.hpp`
# makes `config.hpp` an AMBIGUOUS basename, which the gate reports as stale citations that
# do not exist. Green-vs-red then depended on whether someone had configured a build in the
# tree, which is exactly the kind of environment-sensitivity a gate must not have.
#
# `bench-` is here for the identical reason (#1050): the bench tree is configured separately
# from the core one and so cannot reuse an occupied `build-*` name, which makes `bench-agent`
# the second sanctioned prefix in `.gitignore`. A `cmake -S bench -B bench-agent` renders the
# same `generated/.../config.hpp` one level deeper and turned this gate red. This tuple and
# the `.gitignore` prefix list describe the same set and have to be changed together.
NON_SOURCE_DIR_PREFIXES = ("build-", "bench-")


def _is_non_source_part(part: str) -> bool:
    """@brief True if a path component names a directory holding no citable source."""
    return part in NON_SOURCE_DIRS or part.startswith(NON_SOURCE_DIR_PREFIXES)

# (cited location, substring the target line must contain[, scope])
#
# `scope` disambiguates an anchor whose text repeats — the three `:field` depth gates
# are the same statement in three branches. It must appear in the SCOPE_LINES above a
# candidate for that candidate to count, which is what turns "candidates [4 lines]"
# into a single actionable answer.
SCOPE_LINES = 80

ANCHORS = [
    ("core/include/libtracer/tlv.hpp:69", "struct opt_t"),
    ("core/include/libtracer/tlv.hpp:30", "enum class type_t"),
    ('core/include/libtracer/tlv.hpp:48', 'ROUTER = 0x0D, /**< @brief Router-wrapped frame. */'),
    ("core/src/graph.cpp:2248", "graph_t::set_identity"),
    ("core/src/graph.cpp:2274", "graph_t::read_identity"),
    ("core/src/graph.cpp:2693", 'field.steps[0].name == "identity"'),
    ("core/src/transport_vertex.cpp:59", 'cfg.name("kind")'),
    ("core/src/transport_vertex.cpp:94", "register_child_type"),
    ("core/src/transport_vertex.cpp:123", "register_transport_type"),
    ("core/src/transport_vertex.cpp:128", "transport_vertex_t::register_module"),
    ("core/src/transport_vertex.cpp:162", "SCHEMA_NOT_FOUND", "transport_vertex_t::module_for"),
    ("core/src/transport_vertex.cpp:205", "transport_vertex_t::provide_link"),
    ("core/src/transport_vertex.cpp:288", "routing key IS the mount path"),
    ("core/src/transport_vertex.cpp:295", "qualified += name"),
    ("core/src/transport_vertex.cpp:314", "structural vertex, created lazily"),
    ("core/src/transport_vertex.cpp:322", "register_vertex_key(mod_key"),
    ("core/src/transport_vertex.cpp:414", "if (!router_.add_child(qualified, *link))"),
    ("core/src/transport_vertex.cpp:423", "pending_links_.erase(pl)"),
    ("core/src/transport_vertex.cpp:437", "if (constructed)"),
    ("core/src/transport_vertex.cpp:439", "link_state_t::LISTENING : link_state_t::UP"),
    ('core/src/transport_vertex.cpp:69', '[[nodiscard]] view_t link_state_value(link_state_t state) {'),
    ('core/src/transport_vertex.cpp:248', 'std::string module;'),
    # fwd-router.md's "Signature source" line — bare :NNN shorthands that had ALL rotted
    # silently (they cited the pre-#739 header). Anchored so they cannot rot again.
    # zero-copy-and-flatten.md's rope-tier citations and ADR-0072's stale-comment pointer —
    # all four had rotted on main and were re-asserted by a mechanical +24 shift (#768 verify).
    ("core/include/libtracer/fwd_router.hpp:798", "Terminus over a MULTI-LINK rope"),
    ("core/include/libtracer/fwd_router.hpp:804", "64 KB / 2 links"),
    ("core/include/libtracer/fwd_router.hpp:850", "The forward hop, read entirely by OFFSET"),
    ("core/include/libtracer/fwd_router.hpp:177", "explicit fwd_router_t"),
    ("core/include/libtracer/fwd_router.hpp:257", "bool add_child"),
    ("core/include/libtracer/fwd_router.hpp:417", "using reply_fn_t"),
    ('core/include/libtracer/fwd_router.hpp:179', 'mem::block_source_t* rx = &mem::heap_source(),'),
    ('core/include/libtracer/fwd_router.hpp:180', 'mem::mem_backend_t* flat = &mem::heap_backend(),'),
    # ("core/include/libtracer/fwd_router.hpp", "Slot addresses are NOT stable") — anchor
    # DROPPED (#892). Its only citer was ADR-0072's `fwd_router.hpp:596-605`, and an ADR is a
    # DATED record that `--repin` deliberately never rewrites. So the anchor tracked a line the
    # live tree moves while its citation is frozen by policy: any edit above it orphans the
    # anchor and reds the gate, with no correct re-pin available on either side. An anchor
    # exists to keep a LIVE doc citation from rotting; this text has no live citer left.
    ("core/include/libtracer/child_registry.hpp:267", "bool add(std::string name"),
    ("core/include/libtracer/child_registry.hpp:517", "resolve_peer"),
    ("core/include/libtracer/child_registry.hpp:532", "bool erase"),
    ("core/include/libtracer/child_registry.hpp:565", "entry_by_name"),
    ("core/include/libtracer/child_registry.hpp:586", "by_name"),
    ("core/include/libtracer/child_registry.hpp:627", "std::size_t size()"),
    ("core/include/libtracer/child_registry.hpp:637", "live_size"),
    ("core/src/transport_vertex.cpp:417", "return std::unexpected(status_t::BACKPRESSURE);",
     "if (!router_.add_child(qualified, *link))"),
    ("core/include/libtracer/transport_vertex.hpp:289", "result_t<void> register_module"),
    ("core/include/libtracer/transport_vertex.hpp:97", "enum class link_state_t"),
    ("core/src/graph.cpp:1931", "sel == field_sel_t::TAIL", 'step0.name == "subscribers"'),
    ("core/src/graph.cpp:2048", "!whole_field(field)", 'step0.name == "acl"'),
    ("core/src/graph.cpp:2088", "field_selector(field) != field_sel_t::APPEND"),
    ("core/src/graph.cpp:1982", "sel == field_sel_t::WILDCARD"),
    ("core/src/graph.cpp:2414", "view::segment_alloc(backend, folded_hdr_len(body_len))"),
    ("core/src/graph.cpp:2460", "folded_point_header(hdr_backend, name.size())"),
    ("core/src/graph.cpp:2472", "folded_point_header(hdr_backend, members_len)"),
    ("core/src/graph.cpp:2589", "folded_point_header(hdr_backend, n.body_len)"),
    ("core/src/graph.cpp:2630", "return read_children_folded(vh);"),
    ("core/src/graph.cpp:2627", "sel == field_sel_t::WHOLE || sel == field_sel_t::APPEND"),
    ("core/src/graph.cpp:2773", "field_selector(field) == field_sel_t::SLOT"),
    ("core/src/op_resolve_walk.hpp:322", "enum class index_mode_t"),
    ("core/src/op_resolve_walk.hpp:850", 'field.steps[0].name != "subscribers"'),
    ("core/src/op_resolve_walk.hpp:163", "view_t own_wire(mem::mem_backend_t& flat)"),
    ("core/src/op_resolve_walk.hpp:521", "rope_t or_backpressure"),
    ('core/src/op_resolve_walk.hpp:915', 'if (!req.dst.spans_intact()) return reply_error(status_t::BACKPRESSURE);'),
    ("core/include/libtracer/mem_heap.hpp:217", "try_assign"),
    ('core/include/libtracer/mem_heap.hpp:157', '[[nodiscard]] inline bool try_grow(std::size_t bytes, F&& grow) noexcept {'),
    ('core/include/libtracer/mem_heap.hpp:183', '[[nodiscard]] bool try_reserve(std::vector<T>& v, std::size_t n) noexcept {'),
    ('core/include/libtracer/mem_heap.hpp:377', '[[nodiscard]] inline std::optional<view_t> over_bytes(std::span<const std::byte> bytes,'),
    ("core/include/libtracer/view.hpp:26", "namespace tr::view"),
    ("core/include/libtracer/frame.hpp:23", "namespace tr::wire"),
    ("core/include/libtracer/graph.hpp:49", "namespace tr::graph"),
    ('core/include/libtracer/graph.hpp:980', '[[nodiscard]] result_t<rope_t> read_subtree_folded(vertex_handle_t v,'),
    ('core/include/libtracer/graph.hpp:1036', 'template <typename F>'),
    ('core/include/libtracer/graph.hpp:1332', 'struct delivery_drops_t {'),
    ('core/include/libtracer/graph.hpp:1334', 'std::uint64_t no_target = 0;'),
    ('core/include/libtracer/graph.hpp:1336', 'std::uint64_t denied = 0;'),
    ('core/include/libtracer/graph.hpp:1339', 'std::uint64_t out_of_memory = 0;'),
    ('core/include/libtracer/graph.hpp:1343', 'std::uint64_t fan_out_truncated = 0;'),
    ('core/include/libtracer/graph.hpp:1354', '[[nodiscard]] delivery_drops_t delivery_drops() const noexcept;'),
    ('core/include/libtracer/graph.hpp:1396', 'void fan_out(vertex_t* v, const rope_t& value);'),
    ('core/include/libtracer/graph.hpp:1402', 'void dispatch_edge(const edge_view_t& e, const rope_t& value);'),
    ('core/include/libtracer/graph.hpp:1408', 'void dispatch_edge_target(const edge_view_t& e, const rope_t& value);'),
    ('core/include/libtracer/graph.hpp:1409', 'void dispatch_edge_remote(const edge_view_t& e, const rope_t& value);'),
    ('core/include/libtracer/graph.hpp:1426', 'void bubble_up(vertex_t* v, const rope_t& value);'),
    ('core/include/libtracer/graph.hpp:1430', 'void deliver_vertex(vertex_t* v, const rope_t& value);'),
    ('core/include/libtracer/graph.hpp:1608', 'std::pmr::memory_resource* mr_ = std::pmr::get_default_resource();'),
    ('core/include/libtracer/graph.hpp:1617', 'mem::mem_backend_t* value_backend_ = &mem::heap_backend();'),
    ('core/include/libtracer/graph.hpp:1685', 'mem::block_source_t* ctl_ = &mem::heap_source();'),
    ('core/include/libtracer/graph.hpp:1016', '* @param ctx Caller-owned context; must outlive every possible delivery (edges are'),
    ('core/include/libtracer/graph.hpp:1239', '[[nodiscard]] result_t<value_ref_t> read(const path_t& path) const;'),
    ('core/include/libtracer/graph.hpp:1245', '[[nodiscard]] result_t<value_ref_t> await(const path_t& path, std::chrono::nanoseconds timeout);'),
    ("core/include/libtracer/transport.hpp:32", "namespace tr::net"),
    ('core/include/libtracer/transport.hpp:36', 'using peer_id_t = std::array<std::byte, 16>;'),
    ('core/include/libtracer/transport.hpp:66', 'class bus_link_t {'),
    ("core/include/libtracer/backend.hpp:40", "enum class io_dir_t"),
    ("core/include/libtracer/backend.hpp:101", "class mem_backend_t"),
    ("core/include/libtracer/backend.hpp:145", "before_io"),
    ('core/include/libtracer/backend.hpp:58', "* @brief The address space a backend's bytes live in."),
    ("core/include/libtracer/grammar.hpp:330", "receiver-resource depth bound"),
    # CONTEXT.md quotes the AMENDED meaning of `nesting_too_deep` twice. Its citation was
    # `:210-216` — right until `24ea6d5` inserted the PATH_REF codec above and shifted the
    # whole block +10, after which it landed on `walk_frame_t` and no doc pinned it.
    ('core/include/libtracer/grammar.hpp:340',
     '`TLV_NESTING_TOO_DEEP` ("exceeds this receiver'),
    ("core/src/graph.cpp:1305", "!arena"),
    ("core/include/libtracer/segment.hpp:78", "struct segment_t"),
    ('core/include/libtracer/segment.hpp:21', '#ifndef LIBTRACER_NO_ATOMIC'),
    ('core/include/libtracer/segment.hpp:44', '#ifdef LIBTRACER_NO_ATOMIC'),
    ('core/include/libtracer/segment.hpp:54', 'return count_.fetch_sub(1, std::memory_order_acq_rel);'),
    ('core/include/libtracer/segment.hpp:57', 'return count_.load(std::memory_order_acquire);'),
    ('core/include/libtracer/segment.hpp:116', '[[nodiscard]] static segment_ptr_t adopt(segment_t* seg) noexcept {'),
    ('core/include/libtracer/segment.hpp:126', 'if (seg_) seg_->refcount.inc_relaxed();'),
    ('core/include/libtracer/segment.hpp:139', 'if (seg_ && seg_->refcount.dec_acq_rel() == 1) {'),
    # --- the design + module pages (#728). Every one of these had drifted. ---
    ("core/src/graph.cpp:964", "has_registered_child()"),
    ("core/src/graph.cpp:1069", "void graph_t::fan_out"),
    ("core/src/graph.cpp:1199", "graph_t::write_impl"),
    ("core/src/graph.cpp:1284", "value.try_materialize(*value_backend_)", "graph_t::write_branch"),
    ("core/src/graph.cpp:1286", "flatten_err_t::NO_MEMORY", "graph_t::write_branch"),
    ("core/src/graph.cpp:1301", "std::array<std::byte, 4096> stack;"),
    ("core/src/graph.cpp:1302", "bump_source_t src(stack"),
    ("core/src/graph.cpp:1304", "decode_into(head->bytes(), src)"),
    ("core/src/graph.cpp:1320", "std::vector<std::byte> root_key;"),
    ("core/src/graph.cpp:1321", "try_build_key(v, root_key)"),
    ("core/src/graph.cpp:1323", "try_assign(parse_key, root_key)"),
    ("core/src/graph.cpp:1561", "value.try_materialize(*value_backend_)",
     "field_write read an empty head"),
    ("core/src/graph.cpp:1903", "result_t<void> graph_t::field_write"),
    ("core/src/graph.cpp:2090", "acl_right_t::CREATE", 'step0.name == "children"'),
    ("core/src/fwd_router.cpp:1908", "fwd_router_t::deliver_remote"),
    ("core/src/fwd_router.cpp:1946", "value.try_materialize(*flat_)"),
    ("core/src/fwd_router.cpp:1947", "if (!flat) return;", "A REFUSED materialize drops the delivery"),
    ("core/src/fwd_router.cpp:1949", "emit_compact", "fwd_router_t::deliver_remote"),
    ("core/src/fwd_router.cpp:1980", "std::vector<std::span<const std::byte>> iov;", "fwd_router_t::deliver_remote"),
    # #730 — the two INGRESS flatten guards. Anchored because the whole point of the
    # seam is that these are testable; a citation to them silently rotting would be the
    # first step back to "the guard nobody can prove still works".
    ("core/src/fwd_router.cpp:1526", "if (route.empty() && head->child1_total != 0) return;"),
    ("core/src/fwd_router.cpp:1545", "if (payload.empty() && head->child1_total != 0) return;"),
    ("core/src/fwd_router.cpp:1538", "const std::span<const std::byte> payload = contig(head->child1_off, head->child1_total);"),
    ("core/src/fwd_router.cpp:1222", "frame.subrope(0, frame.total_length()).try_materialize"),
    # #766/#793 — the terminus resolver's three rope-tier draws, and the two allocations the
    # seam docs name as NOT covered by `flat`. These were cited by four doc pages and anchored
    # by none, so #793's own edits to `op_resolve_view.cpp` shifted every one of them without
    # the gate noticing — the exact rot class this file exists for.
    ("core/src/op_resolve_view.cpp:140", "sub.flatten(flat)"),
    ("core/src/op_resolve_view.cpp:150", "over_bytes(sub.only().bytes(), flat)"),
    ("core/src/op_resolve_view.cpp:258", "wire().materialize(backend())"),
    # #801 — the SPAN tier's ownership copy, cited by allocation-and-backpressure.md.
    ("core/src/fwd_reply.cpp:123", "view::segment_alloc(egress, head_len)"),
    ('core/src/fwd_reply.cpp:33', "*        the u16 the kind=ERROR reply's ERROR{VALUE} identity carries."),
    ('core/src/fwd_reply.cpp:132', 'out.tlv_sliced(route.dst_wire);'),
    ("core/src/fwd_router.cpp:1444", "decode_into(frame, rx_for(inbound_ctx))"),
    # `vertex.hpp:<parent_>` was pinned here TWICE, and the only doc that cites it is
    # `docs/spec/rfcs/0019` — a historical genre this tool's own header excludes from
    # pinning ("dated records of a decision ... pinning them would demand rewriting
    # history on every refactor"). It could only survive a refactor by rewriting that
    # RFC, which is what the exclusion exists to forbid, so both copies are dropped
    # rather than kept as a pin nothing living can hold up (#896).
    # --- #803: the shorthand + `config.hpp.in` enrolment ---
    #
    # Everything below became pinnable when the resolver learned the design pages'
    # basename shorthand and the `.hpp.in` template. These are the ~250 `file:line`
    # assertions the living doc surfaces make — CONTEXT.md, `docs/design/`,
    # `docs/modules/`, `docs/reference/`, `docs/interop/`, the getting-started page and
    # the testbed register — carried unguarded until now, and the class #802 found rotten
    # by hand. Where a citation names a RANGE the pin takes the most distinctive line
    # inside it, which is why a pin is sometimes one or two lines past the cited head (a
    # doc that points at a `/**` means the block, and `/**` is not an anchor).
    #
    # Deliberately NOT enrolled: `docs/adr/`, `docs/spec/` and `docs/research/`. Those are
    # DATED records of a decision — their citations describe the tree as it stood, and
    # some already point past today's EOF. Pinning them would demand rewriting history
    # every time the code moves, which is the opposite of what a record is for.
    # bench/bench_libtracer.cpp — bench/README.md's "neither retired mode is emitted today"
    # pair. Enrolled because it HAD rotted: the second citation was written against the
    # tree of the day, a later change to `main` moved it 38 lines, and nothing noticed —
    # the same silence #725/#726 found in the module pages, in a file the gate could read
    # all along but had no pin for.
    ('bench/bench_libtracer.cpp:16', '(The `loopback` /'),
    ('bench/bench_libtracer.cpp:1235',
     '// (The `loopback` and n-routers `routers-hN` modes benchmarked the ROUTER-flood'),
    # bench/bench_lkv_slot.cpp
    ('bench/bench_lkv_slot.cpp:193', 'class model_sp_atomic_t {'),
    ('bench/bench_lkv_slot.cpp:342', 'class model_hazard_t {'),
    ('bench/bench_lkv_slot.cpp:456', 'class model_hazard_ref_t {'),
    # core/examples/wire_codec.cpp
    ('core/examples/wire_codec.cpp:71', 'std::printf("encoded POINT{VALUE,VALUE}+CRC: %zu bytes\\n", wire.size());'),
    ('core/examples/wire_codec.cpp:94', 'constexpr int kIters = 50000;'),
    # core/include/libtracer/backend.hpp
    ('core/include/libtracer/backend.hpp:153',
     'virtual void after_io(view::segment_t* /*seg*/, io_dir_t /*dir*/) noexcept {}'),
    ('core/include/libtracer/backend.hpp:188',
     '* @brief Reclaim @p seg through its backend — the module-set destroy dispatch'),
    # core/include/libtracer/can_reassembly.hpp
    ('core/include/libtracer/can_reassembly.hpp:191',
     '[[nodiscard]] std::optional<tr::view::rope_t> assemble(const reassembly_key_t& key) const {'),
    # core/include/libtracer/config.hpp
    ('core/include/libtracer/config.hpp:77', '* CMake: `-DLIBTRACER_VERTEX_LOCK_STRIPES=8`; ESP-IDF: menuconfig'),
    ('core/include/libtracer/config.hpp:86', 'static constexpr std::size_t kVertexLockStripes = 16;'),
    # core/include/libtracer/config.hpp.in
    ('core/include/libtracer/config.hpp.in:63', '* struct my_node_config_t : tr::graph::default_config_t {'),
    ('core/include/libtracer/config.hpp.in:73', 'struct default_config_t {'),
    ('core/include/libtracer/config.hpp.in:136', 'static constexpr std::size_t kHazardReaderSlots = @LIBTRACER_HAZARD_READER_SLOTS@;'),
    ('core/include/libtracer/config.hpp.in:185', 'static constexpr std::size_t kMaxVertexBytes64 = 96;'),
    ('core/include/libtracer/config.hpp.in:227', 'static constexpr std::uint32_t kPinPayloadRatio = 0;'),
    ('core/include/libtracer/config.hpp.in:236', 'using acl_policy_t = @LIBTRACER_ACL_POLICY@;'),
    ('core/include/libtracer/config.hpp.in:203', 'static constexpr std::size_t kMaxVertexBytes32 = 72;'),
    ('core/include/libtracer/config.hpp.in:252', 'using lkv_slot_t = @LIBTRACER_LKV_SLOT@;'),
    ('core/include/libtracer/config.hpp.in:261', 'using config_t = default_config_t;'),
    ('core/include/libtracer/config.hpp.in:86',
     'static constexpr std::size_t kVertexLockStripes = @LIBTRACER_VERTEX_LOCK_STRIPES@;'),
    ('core/include/libtracer/config.hpp.in:109',
     'static constexpr std::size_t kCacheLineBytes = @LIBTRACER_CACHE_LINE_BYTES@;'),
    ('core/include/libtracer/config.hpp.in:249',
     '* `-DLIBTRACER_LKV_SLOT=<type>`. The named type must satisfy the policy contract in'),
    ('core/include/libtracer/config.hpp.in:307',
     'inline constexpr bool kSpinWaitSafe = @LIBTRACER_SPIN_WAIT_SAFE@;'),
    ('core/include/libtracer/config.hpp.in:263',
     '// ---------------------------------------------------------------------------------------------'),
    # core/include/libtracer/crc.hpp
    ('core/include/libtracer/crc.hpp:38', 'constexpr std::array<std::uint32_t, 256> crc32c_table() noexcept {'),
    ('core/include/libtracer/crc.hpp:51', 'constexpr std::array<std::uint16_t, 256> crc16_table() noexcept {'),
    ('core/include/libtracer/crc.hpp:71',
     'constexpr std::array<std::array<std::uint32_t, 256>, 8> crc32c_slice_tables() noexcept {'),
    ('core/include/libtracer/crc.hpp:168',
     '[[nodiscard]] inline std::uint32_t crc32c_update_runtime(std::uint32_t c,'),
    # core/include/libtracer/frame.hpp
    ('core/include/libtracer/frame.hpp:25',
     '// Decode failures reuse the RFC-0002 registry codes (error.hpp) directly — the'),
    ('core/include/libtracer/frame.hpp:197',
     '[[nodiscard]] inline std::expected<tlv_t, err_t> decode(const view::view_t& v) {'),
    # core/include/libtracer/fwd_frame_view.hpp
    # core/include/libtracer/fwd_router.hpp
    ('core/include/libtracer/fwd_router.hpp:98',
     "* @param flat  The byte backend EVERY rope flatten on the router's forward AND terminus"),
    ('core/include/libtracer/fwd_router.hpp:178',
     'std::pmr::memory_resource* mr = std::pmr::get_default_resource(),'),
    ('core/include/libtracer/fwd_router.hpp:433',
     "* Invoked (with the `FWD{REPLY}` frame as a @ref view::rope_t) when a REPLY's first"),
    ('core/include/libtracer/fwd_router.hpp:1017',
     '[[nodiscard]] mem::block_source_t& rx_for(const child_rx_ctx_t* ctx) const noexcept {'),
    # core/include/libtracer/grammar.hpp
    ('core/include/libtracer/grammar.hpp:428',
     '* call stack, docs/reference/01 §Iterative parsing requirement): the walk keeps'),
    # core/include/libtracer/graph.hpp
    ('core/include/libtracer/graph.hpp:84',
     '// There is no in-process dispatch-depth cap: a SUBSCRIBER delivery TERMINATES at its'),
    ('core/include/libtracer/graph.hpp:304',
     '* @param ctl The #551 nothrow seam every FAILABLE allocation draws from — the ones a'),
    ('core/include/libtracer/graph.hpp:317',
     'explicit graph_t(std::pmr::memory_resource* mr = std::pmr::get_default_resource(),'),
    ('core/include/libtracer/graph.hpp:330',
     '[[nodiscard]] mem::block_source_t& control_source() const noexcept { return *ctl_; }'),
    ('core/include/libtracer/graph.hpp:385',
     '* already-retired or unregistered vertex succeeds and does nothing. The root cannot be'),
    ('core/include/libtracer/graph.hpp:806',
     '[[nodiscard]] result_t<value_ref_t> read(vertex_handle_t v, std::string_view caller = {}) const;'),
('core/include/libtracer/graph.hpp:893',
     '[[nodiscard]] result_t<value_ref_t> await(vertex_handle_t v, std::chrono::nanoseconds timeout,'),
    ('core/include/libtracer/graph.hpp:1676',
     '*         to migrate. Kept a DIFFERENT type from `mr_` on purpose (see'),
    ('core/include/libtracer/graph.hpp:1679',
     '*         LAST on purpose: no hot path reads it, so declaring it here keeps'),
    # core/include/libtracer/lkv_slot.hpp
    ('core/include/libtracer/lkv_slot.hpp:100', '* **Lock-free BY CONTRACT, and spin-locked in practice.**'),
    ('core/include/libtracer/lkv_slot.hpp:101',
     '* `std::atomic<std::shared_ptr<T>>::is_lock_free()` returns 0 on libstdc++, so both load'),
    # core/include/libtracer/mem_borrowed.hpp
    ('core/include/libtracer/mem_borrowed.hpp:39',
     'void destroy(view::segment_t* seg) noexcept override { delete seg; }  // control block only'),
    # core/include/libtracer/mem_heap.hpp
    ('core/include/libtracer/mem_heap.hpp:340',
     '[[nodiscard]] inline std::optional<view_t> over_bytes(std::span<const std::byte> bytes) noexcept {'),
    # core/include/libtracer/mem_pool.hpp
    ('core/include/libtracer/mem_pool.hpp:184', 'class synchronized_pool_t final : public mem_backend_t {'),
    # core/include/libtracer/mem_source.hpp
    ('core/include/libtracer/mem_source.hpp:138', '[[nodiscard]] block_source_t& heap_source() noexcept;'),
    ('core/include/libtracer/mem_source.hpp:159', '[[nodiscard]] block_source_t& null_source() noexcept;'),
    ('core/include/libtracer/mem_source.hpp:184', 'class bump_source_t final : public block_source_t {'),
    ('core/include/libtracer/mem_source.hpp:224', 'void reset() noexcept { used_ = 0; }'),
    ('core/include/libtracer/mem_source.hpp:316', 'class pool_source_t final : public block_source_t {'),
    ('core/include/libtracer/mem_source.hpp:170',
     '*       `monotonic_buffer_resource` also spills past its buffer, but it spills to a'),
    ('core/include/libtracer/mem_source.hpp:175',
     '* @warning SCOPE-LIFETIME USE ONLY. A bump block is never reclaimed, so a source that'),
    ('core/include/libtracer/mem_source.hpp:178',
     '*          between operations. It is NOT a long-lived seam: an 8 KiB bump source wired as'),
    ('core/include/libtracer/mem_source.hpp:179',
     "*          a router's `rx` decoded 6 frames and rejected the next 194 — measured. A"),
    ('core/include/libtracer/mem_source.hpp:181',
     '* @note Single-threaded by contract — a bump cursor is not synchronized. Its intended use'),
    ('core/include/libtracer/mem_source.hpp:192',
     '[[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {',
     ': block_source_t("bump"), buf_(buffer), upstream_(&upstream) {}'),
    ('core/include/libtracer/mem_source.hpp:325',
     'pool_source_t(std::span<std::byte> slab, std::span<size_class_t> classes) noexcept'),
    ('core/include/libtracer/mem_source.hpp:376',
     '[[nodiscard]] std::size_t classes_used() const noexcept { return n_; }'),
    ('core/include/libtracer/mem_source.hpp:380',
     '[[nodiscard]] std::size_t overflowed() const noexcept { return overflow_; }'),
    # core/include/libtracer/path.hpp
    ('core/include/libtracer/path.hpp:31', 'inline constexpr std::size_t kMaxSegmentBytes = 64;'),
    ('core/include/libtracer/path.hpp:33', 'inline constexpr std::size_t kMaxPathBytes = 1024;'),
    ('core/include/libtracer/path.hpp:35', 'inline constexpr std::size_t kMaxSegments = 255;'),
    ('core/include/libtracer/path.hpp:37', 'inline constexpr std::size_t kMaxFieldDepth = 8;'),
    ('core/include/libtracer/path.hpp:251', 'static constexpr std::size_t kInlineBytes = 16;'),
    ('core/include/libtracer/path.hpp:159', 'explicit path_t(std::string_view text);'),
    # core/include/libtracer/receiver_slot.hpp
    ('core/include/libtracer/receiver_slot.hpp:143', 'frame.try_materialize(backend);'),
    # core/include/libtracer/rope.hpp
    ('core/include/libtracer/rope.hpp:76', 'void append(view_t v) {'),
    ('core/include/libtracer/rope.hpp:181',
     '* @brief The single contiguous link — the consumer\'s explicit "this value is'),
    ('core/include/libtracer/rope.hpp:183',
     '* @note Precondition: `link_count() == 1` (debug-asserted). A consumer that'),
    ('core/include/libtracer/rope.hpp:204',
     '[[nodiscard]] view_t materialize(mem::mem_backend_t& backend = mem::heap_backend()) const {'),
    ('core/include/libtracer/rope.hpp:257', '[[nodiscard]] rope_t subrope(std::size_t off, std::size_t len) const {'),
    ('core/include/libtracer/rope.hpp:286',
     '[[nodiscard]] std::vector<std::span<const std::byte>> to_iovec() const {'),
    ('core/include/libtracer/rope.hpp:294',
     '* @brief Nothrow @ref to_iovec — fill @p out with one span per link (no copy),'),
    ('core/include/libtracer/rope.hpp:303',
     '[[nodiscard]] bool try_to_iovec(std::vector<std::span<const std::byte>>& out) const noexcept {'),
    ('core/include/libtracer/rope.hpp:373', 'static constexpr std::size_t kInline = 2;'),
    # core/include/libtracer/rope_decode.hpp
    ('core/include/libtracer/rope_decode.hpp:17',
     '* SINK NOTE: this validates STRUCTURE + CRC over a rope; it does not yet'),
    ('core/include/libtracer/rope_decode.hpp:71', 'class rope_cursor {'),
    # core/include/libtracer/segment.hpp
    ('core/include/libtracer/segment.hpp:52',
     'void inc_relaxed() noexcept { count_.fetch_add(1, std::memory_order_relaxed); }'),
    ('core/include/libtracer/segment.hpp:74',
     '* @note `bytes` is writable at the type level, but whether writes are *legal*'),
    ('core/include/libtracer/segment.hpp:81',
     'std::span<std::byte> bytes; /**< @brief The backing bytes this segment holds a reference to. */'),
    ('core/include/libtracer/segment.hpp:82',
     'mem::mem_space_t space; /**< @brief Address space (HOST/DEVICE), inherited from @ref backend. */'),
    ('core/include/libtracer/segment.hpp:120',
     '[[nodiscard]] static segment_ptr_t retain(segment_t* seg) noexcept {'),
    ('core/include/libtracer/segment.hpp:124',
     '/** @brief Clone — a new shared reference to the same segment (relaxed increment). */'),
    ('core/include/libtracer/segment.hpp:137',
     "/** @brief Drop this reference (acq_rel); fires the backend's `destroy` at zero. */"),
    ('core/include/libtracer/segment.hpp:154',
     '/** @brief Current refcount — debug / metrics only (acquire load), NOT a sync primitive. */'),
    # core/include/libtracer/status.hpp
    ('core/include/libtracer/status.hpp:25', 'enum class status_t {'),
    # core/include/libtracer/tlv.hpp
    # core/include/libtracer/tlv_arena.hpp
    ('core/include/libtracer/tlv_arena.hpp:8',
     "* span points into the caller's input buffer — the arena holds structure"),
    ('core/include/libtracer/tlv_arena.hpp:31',
     '* @brief One decoded TLV node in a @ref tlv_arena_t (structure only, zero-copy).'),
    ('core/include/libtracer/tlv_arena.hpp:39', 'struct arena_tlv_t {'),
    ('core/include/libtracer/tlv_arena.hpp:130',
     '* NOTHROW end to end (#588). This function is on the wire RX path and reachable'),
    # core/include/libtracer/transport.hpp
    ('core/include/libtracer/transport.hpp:341',
     'virtual void send(std::span<const std::span<const std::byte>> iov) {'),
    ('core/include/libtracer/transport.hpp:458',
     '[[nodiscard]] virtual bool delivers_ropes() const { return false; }',
     'rx_.set_rope([](void* c, view::rope_t f) { (*static_cast<F*>(c))(std::move(f)); }, &sink);'),
    # core/include/libtracer/transport_can.hpp
    ('core/include/libtracer/transport_can.hpp:491', '[[nodiscard]] bus_link_t* bus() override { return this; }'),
    ('core/include/libtracer/transport_can.hpp:510',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }'),
    # core/include/libtracer/transport_quic.hpp
    ('core/include/libtracer/transport_quic.hpp:153',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }'),
    # core/include/libtracer/transport_tcp.hpp
    ('core/include/libtracer/transport_tcp.hpp:207',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }',
     'tcp_transport_t& operator=(const tcp_transport_t&) = delete;'),
    ('core/include/libtracer/transport_tcp.hpp:372',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }',
     'transport_tcp_server& operator=(const transport_tcp_server&) = delete;'),
    # core/include/libtracer/transport_udp.hpp
    ('core/include/libtracer/transport_udp.hpp:66', 'static constexpr std::size_t kMaxDatagram = 65536;'),
    ('core/include/libtracer/transport_udp.hpp:111',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }'),
    # core/include/libtracer/transport_vertex.hpp
    ('core/include/libtracer/transport_vertex.hpp:78',
     'enum class conn_role_t : std::uint8_t { DIAL = 0, LISTEN = 1 };'),
    ('core/include/libtracer/transport_vertex.hpp:121',
     "* §5 leanness ruling): a kind's PRIVATE config (e.g. quic's `cert`/`key` PEM paths) never"),
    ('core/include/libtracer/transport_vertex.hpp:140',
     'std::uint32_t backoff_ms = 0;         /**< @brief DIAL self-heal retry interval (RFC-0014 §4);'),
    ('core/include/libtracer/transport_vertex.hpp:143',
     'std::uint32_t connect_timeout_ms = 0; /**< @brief DIAL connect-attempt deadline (RFC-0014 §4):'),
    # core/include/libtracer/transport_webtransport.hpp
    ('core/include/libtracer/transport_webtransport.hpp:182',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }'),
    # core/include/libtracer/transport_ws.hpp
    ('core/include/libtracer/transport_ws.hpp:225',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }',
     'void send(std::span<const std::span<const std::byte>> iov) override;'),
    ('core/include/libtracer/transport_ws.hpp:416',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }',
     'transport_ws_client& operator=(const transport_ws_client&) = delete;'),
    # core/include/libtracer/vertex.hpp
    ('core/include/libtracer/vertex.hpp:165',
     "* Holding one keeps that value alive, exactly as the reader's own reference did before. Under"),
    ('core/include/libtracer/vertex.hpp:170', 'class value_ref_t {'),
    ('core/include/libtracer/vertex.hpp:597', 'using subscriber_fn_t = void (*)(void* ctx, const rope_t& value);'),
    ('core/include/libtracer/vertex.hpp:819', 'class edge_snapshot_t {'),
    ('core/include/libtracer/vertex.hpp:822', 'static constexpr std::size_t kCapacity = 8;'),
    ('core/include/libtracer/vertex.hpp:1077', 'static_assert(alignof(vertex_stripe_t) == kStripeAlign,'),
    ('core/include/libtracer/vertex.hpp:1101', 'static std::array<vertex_stripe_t, kVertexLockStripes> stripes{};'),
    ('core/include/libtracer/vertex.hpp:1111', 'return (h >> 6) % kVertexLockStripes;'),
    ('core/include/libtracer/vertex.hpp:1115', 'inline vertex_stripe_t& vertex_stripe_of(const void* v) noexcept {'),
    ('core/include/libtracer/vertex.hpp:1452', '[[nodiscard]] bool has_registered_child() const noexcept {'),
    ('core/include/libtracer/vertex.hpp:2020', 'struct snapshot_drops_t {'),
    ('core/include/libtracer/vertex.hpp:2829', 'const bool use_heap ='),
    ('core/include/libtracer/vertex.hpp:2846', '++drops.out_of_memory;'),
    ('core/include/libtracer/vertex.hpp:1033',
     '// kVertexLockStripes and kCacheLineBytes are ordinary constexprs shared by every TU'),
    ('core/include/libtracer/vertex.hpp:1093',
     'inline constinit std::array<vertex_stripe_t, kVertexLockStripes> vertex_stripes{};'),
    ('core/include/libtracer/vertex.hpp:1268',
     'static constexpr std::size_t kInlineFanout = edge_snapshot_t::kCapacity;'),
    ('core/include/libtracer/vertex.hpp:2070',
     'std::size_t snapshot_edges(edge_snapshot_t& inline_buf, std::vector<edge_view_t>& overflow,'),
    ('core/include/libtracer/vertex.hpp:2835',
     '// OOM fallback (reserve failed on a wide list): the inline prefix delivers,'),
    ('core/include/libtracer/vertex.hpp:2841',
     'if (src[i].active.load(std::memory_order_acquire)) ++drops.truncated;'),
    ('core/include/libtracer/vertex.hpp:3156',
     'static_assert(sizeof(void*) != 8 || sizeof(vertex_t) <= config_t::kMaxVertexBytes64,'),
    ('core/include/libtracer/vertex.hpp:3161',
     'static_assert(sizeof(void*) != 4 || sizeof(vertex_t) <= config_t::kMaxVertexBytes32,'),
    # core/src/frame.cpp
    ('core/src/frame.cpp:119', 'std::array<grammar::walk_frame_t<grammar::span_cursor>, 8> slots;'),
    ('core/src/frame.cpp:120', 'grammar::walk_stack_t<grammar::span_cursor> stack(slots, &mem::heap_source());'),
    # core/src/fwd_router.cpp
    ('core/src/fwd_router.cpp:641',
     'bool fwd_router_t::add_child(std::string name, transport_t& link, mem::block_source_t* rx) {'),
    ('core/src/fwd_router.cpp:1190',
     'void fwd_router_t::on_frame_rope_impl(std::string_view inbound_name, view::rope_t frame,'),
    ('core/src/fwd_router.cpp:1196', 'if (frame.link_count() == 1) {'),
    ('core/src/fwd_router.cpp:1243', '// A REPLY that reaches its originator here is handed to the sink'),
    ('core/src/fwd_router.cpp:1554',
     'void fwd_router_t::on_control_rope(std::string_view inbound_name, view::rope_t frame) {'),
    ('core/src/fwd_router.cpp:1505', 'const auto head = peek_control(cur, wire::grammar::crc_check_t::VERIFY);'),
    ('core/src/fwd_router.cpp:1519', 'const std::span<const std::byte> route = contig(head->child1_off, head->child1_total);'),
    ('core/src/fwd_router.cpp:1569', 'frame.subrope(off, total).try_materialize(*flat_);'),
    ('core/src/fwd_router.cpp:1925',
     "// else. A dropped fresh ADVERTISE self-heals via the peer's HANDLE_NACK (§E.1). NOT yet"),
    ('core/src/fwd_router.cpp:1964',
     'constexpr std::array<std::byte, 5> op_tlv{std::byte{0x01}, std::byte{0x00}, std::byte{0x01},'),
    # core/src/graph.cpp
    ('core/src/graph.cpp:247', 'const view_t& frame_view, std::vector<std::byte> key,'),
    ('core/src/graph.cpp:722', 'graph_t::delivery_drops_t graph_t::delivery_drops() const noexcept {'),
    ('core/src/graph.cpp:729', 'void graph_t::count_drop(drop_reason_t why, std::uint64_t n) noexcept {'),
    ('core/src/graph.cpp:749',
     'void graph_t::count_snapshot_drops(const vertex_t::snapshot_drops_t& drops) noexcept {'),
    ('core/src/graph.cpp:989', '[[nodiscard]] bool try_clone_rope(rope_t& dst, const rope_t& src) noexcept {'),
    ('core/src/graph.cpp:1017', 'if (target == nullptr) {'),
    ('core/src/graph.cpp:1024', 'if (!acl_allows(target, e.caller, acl_right_t::WRITE)) {'),
    ('core/src/graph.cpp:1028', '// Delivery TERMINATES at the target (ADR-0051 / RFC-0007): apply exactly the'),
    ('core/src/graph.cpp:1036', 'if (!try_clone_rope(clone, value)) {'),
    ('core/src/graph.cpp:1059', 'inline void graph_t::dispatch_edge(const edge_view_t& e, const rope_t& value) {'),
    ('core/src/graph.cpp:1102',
     '// snapshot_edges re-checks the width under the lock, so a race on the count only costs a'),
    ('core/src/graph.cpp:1109', 'static thread_local std::vector<edge_view_t> tls_buf;'),
    ('core/src/graph.cpp:1119',
     'const std::size_t n = v->snapshot_edges(inline_buf, tls_buf, drops);'),
    ('core/src/graph.cpp:1137',
     'const std::size_t n = v->snapshot_edges(inline_buf, heap_buf, drops);'),
    ('core/src/graph.cpp:1145',
     'result_t<std::shared_ptr<const rope_t>> graph_t::store_value(vertex_t* v, rope_t&& value) {'),
    ('core/src/graph.cpp:1166', 'void graph_t::bubble_up(vertex_t* v, const rope_t& value) {'),
    ('core/src/graph.cpp:1208', '// A handler stores no LKV (the user handler consumes the value), so the'),
    ('core/src/graph.cpp:1232', 'count_drop(drop_reason_t::OUT_OF_MEMORY, v->own_subs());'),
    ('core/src/graph.cpp:1243', '// Deliver the just-appended ring entry and advance the drain cursor, so a later'),
    ('core/src/graph.cpp:1248', '// no notify reclone of the rope on the hot write path.'),
    ('core/src/graph.cpp:1271',
     'result_t<void> graph_t::write_branch(vertex_t* v, const rope_t& value, std::string_view caller,'),
    ('core/src/graph.cpp:1298',
     "// The overflow leg draws from the graph's injected control seam, not the global heap:"),
    ('core/src/graph.cpp:1403', 'if (v->listeners_above() > 0) bubble_up(v, value);', 'fan_out(v, value);'),
    ('core/src/graph.cpp:1545',
     'result_t<void> graph_t::write(vertex_handle_t v, rope_t value, std::string_view caller) {'),
    ('core/src/graph.cpp:2158', 'result_t<void> graph_t::create_child(vertex_t* parent, const view_t& spec_value) {'),
    ('core/src/graph.cpp:2804', 'result_t<void> graph_t::write(const path_t& path, rope_t value) {'),
    # core/src/op_resolve_walk.hpp
    ('core/src/fwd_reply.hpp:109', 'void tlv_sliced(std::span<const std::byte> wire) {'),
    ('core/src/op_resolve_walk.hpp:804',
     'if (!req.src.spans_intact()) return std::unexpected(status_t::BACKPRESSURE);'),
    # core/src/path.cpp
    # core/src/posix_endpoint.cpp
    ('core/src/posix_endpoint.cpp:225',
     'void stream_endpoint_t::write_all_iov(int fd, std::span<const ::iovec> vec) {'),
    ('core/src/posix_endpoint.cpp:143', 'return ::sendmsg(fd, msg, MSG_NOSIGNAL);'),
    # The multi-peer servers' per-chunk receive scratch — ONE buffer since #871 folded the
    # tcp and ws poll loops into slot_server_t (it used to be one apiece, cited as
    # transport_tcp.cpp:508 and transport_ws.cpp:420).
    ('core/src/posix_endpoint.cpp:448', 'std::array<std::byte, 4096> chunk;',
     'void slot_server_t::service_peer(session_base_t& s) {'),
    # core/src/rope.cpp
    ('core/src/rope.cpp:16', 'if (!all_host()) return std::unexpected(flatten_err_t::NOT_HOST);'),
    ('core/src/rope.cpp:28', 'if (!b.empty()) std::memcpy(seg->bytes.data() + pos, b.data(), b.size());'),
    # core/src/rope_decode.cpp
    ('core/src/rope_decode.cpp:32', 'std::expected<void, err_t> check_frame(const view::rope_t& r) {'),
    ('core/src/rope_decode.cpp:46', 'std::expected<void, err_t> validate_rope(const view::rope_t& r) {'),
    # core/src/tlv_arena.cpp
    ('core/src/tlv_arena.cpp:151', 'std::array<grammar::walk_frame_t<grammar::span_cursor>, 8> slots;'),
    ('core/src/tlv_arena.cpp:152', 'grammar::walk_stack_t<grammar::span_cursor> stack(slots, &src);'),
    # core/src/transport_tcp.cpp
    ('core/src/transport_tcp.cpp:56',
     '*        MEASURED (`bench_transport_iov`): the fallback fires at exactly **17'),
    ('core/src/transport_tcp.cpp:59',
     "*        `bench_forward_heap`'s `allocs=0` gate cannot see it: that bench drives"),
    ('core/src/transport_tcp.cpp:219', 'bool tcp_transport_t::read_exact(int fd, std::byte* dst, std::size_t len) {'),
    ('core/src/transport_tcp.cpp:239', 'std::array<std::byte, 4096> scratch;'),
    ('core/src/transport_tcp.cpp:282', 'if (!read_exact(fd, seg->bytes.data(), len)) return;'),
    # zero-copy-and-flatten.md quotes this comment's tail verbatim, so the anchor carries the
    # QUOTED line — pinning `serve()`'s signature two constructs up passed while the citation
    # pointed at code the doc never quotes.
    ('core/src/transport_tcp.cpp:261',
     '// buffer, no copy; feeding recv chunks through feed() would add one).'),
    # core/src/transport_udp.cpp
    ('core/src/transport_udp.cpp:145',
     'const std::size_t rx_cap = std::min(kMaxDatagram, backend_->max_segment_size());'),
    # core/src/transport_vertex.cpp
    ('core/src/transport_vertex.cpp:48',
     '*        NAME <utf8>, NAME "kind" NAME <utf8>, NAME "port" VALUE u16, NAME "role" VALUE u8'),
    ('core/src/transport_vertex.cpp:98',
     'graph_.register_child_type(',
     'return make_connection(std::move(key), config, conn_role_t::DIAL);'),
    ('core/src/transport_vertex.cpp:144',
     'result_t<std::string> transport_vertex_t::module_for(std::string_view kind,'),
    ('core/src/transport_vertex.cpp:305',
     '// Compose the mount key: `<net_root>/<module>/<name>`, replacing the flat key the'),
    # core/src/transport_ws.cpp
    ('core/src/transport_ws.cpp:86',
     'assemble_result_t on_data(ws::opcode_t op, bool fin, std::span<const std::byte> payload,'),
    ('core/src/transport_ws.cpp:100',
     'const std::optional<tr::view::view_t> link = tr::view::over_bytes(payload, backend);'),
    ('core/src/transport_ws.cpp:150', 'constexpr std::size_t kMaxServerIov = kMaxInlineIov;'),
    ('core/src/transport_ws.cpp:264', '// no flatten, no re-copy (server frames are UNMASKED, RFC 6455 §5.1). Lock'),
    ('core/src/transport_ws.cpp:272', 'std::array<::iovec, kMaxServerIov + 1> gather_inline;'),
    # The broadcast's gather store. Its old scope named the constructor's `::socket` call,
    # which #871 moved out of this TU into slot_server_t::bind_listen; the entry sheds the
    # scope entirely instead, because the array is now spelled `pristine_inline` here and
    # `inline_vec` only in the directed facade — one anchor, one hit, no positional filter.
    ('core/src/transport_ws.cpp:647', 'std::array<std::byte, 4096> chunk;',
     'void transport_ws_client::serve(int fd, std::vector<std::byte> pipelined) {'),
    # core/tests/registry_teardown_test.cpp
    ('core/tests/registry_teardown_test.cpp:275', 'void test_digest_paths_agree() {'),
    # core/tests/tlv_arena_test.cpp
    ('core/tests/tlv_arena_test.cpp:297', 'const std::vector<std::byte> deep_bytes = encode(nested(100));'),
    # integrations/esp-idf/libtracer/httpd_ws_link.cpp
    ('integrations/esp-idf/libtracer/httpd_ws_link.cpp:71',
     '* (F2b, 2026-07-09): the /unit batch apply overflowed 8 KB and needed ~12 KB. It is named'),
    ('integrations/esp-idf/libtracer/include/libtracer_esp/httpd_ws_link.hpp:165',
     'static constexpr std::size_t kRequiredHttpdStack = 12288;'),
    ('integrations/esp-idf/libtracer/httpd_ws_link.cpp:513', 'if (chunk.empty()) return true;'),
    ('integrations/esp-idf/libtracer/httpd_ws_link.cpp:519',
     'if (len_ != 0) std::memcpy(grown.get(), bytes_.get(), len_);'),
    # integrations/esp-idf/libtracer/include/libtracer_esp/httpd_ws_link.hpp
    ('integrations/esp-idf/libtracer/include/libtracer_esp/httpd_ws_link.hpp:52',
     '*     task stack (the batch apply overflows the 4 KB httpd default). The PORT-BINDING'),

    # --- re-added from the v0.7.1 docs sweep (absent from main's table) ---
    ('core/include/libtracer/fwd_frame_view.hpp:868', 'inline constexpr std::size_t kFwdMaxIov = 10;'),
    # ONE `bus()` since #871: both stream servers inherit slot_server_t's (they used to
    # restate it, cited as transport_tcp.hpp:343 and transport_ws.hpp:233).
    ('core/include/libtracer/posix_endpoint.hpp:409',
     '[[nodiscard]] bus_link_t* bus() override { return peer_named_ ? this : nullptr; }'),
    ('core/include/libtracer/edge_pin.hpp:153', 'class pin_t {'),
    ('core/src/fwd_router.cpp:748', 'link.set_rope_receiver('),
    ('core/src/fwd_router.cpp:702', 'bus->set_peer_rope_receiver('),
    ('core/src/graph.cpp:777', 'vertex_t* graph_t::find_ptr(std::span<const std::byte> key) const {'),
    ('core/src/graph.cpp:778', 'const std::shared_lock lock(map_mutex_);'),
    ('core/src/graph.cpp:1889', 's.target_key.reset();'),
    ('core/src/path.cpp:96', 'if (!valid_segment(seg)) return std::unexpected(status_t::INVALID_PATH);'),
    ('core/src/path.cpp:112', 'if (step.empty()) return std::unexpected(status_t::INVALID_PATH);'),
    ('core/src/route_handle.cpp:82', 't.ingress.push_back(ingress_entry_t{.label = label, .binding = std::move(binding)});'),
    ('core/src/route_handle.cpp:182', 't->egress.push_back(egress_entry_t{', 'bool route_handle_t::record_egress(std::string_view out_link, std::uint16_t label,'),
    ('core/src/route_handle.cpp:247', 't->egress.push_back(egress_entry_t{', 'std::pair<std::uint16_t, bool> route_handle_t::ensure_egress(std::string_view out_link,'),
    # --- #1052: the build/tooling citations, now readable (@ref CITABLE_NON_SOURCE_PATHS).
    # `LIBTRACER_NO_ATOMIC` is spelled in three places outside `segment.hpp`, and the two
    # in build files had both rotted: the footprint script's citation (`:93`) had landed on
    # its include-directory assignment, and the test CMake's (`:927,940-941`) on three
    # registrations belonging to entirely different suites — `add_test(NAME
    # fwd_flatten_backend ...)` and the `terminus_egress_backend_test` executable and its
    # link line. Neither file carried a pin, so the gate verified the `segment.hpp` half of
    # that sentence and read as if it had verified the whole of it. These pin the lines
    # the prose actually names, in the two pages that name them: the segment module page
    # and the configuration-space design page.
    ('tools/cortexm0_footprint.py:84', 'REQUIRED_MODULES = ('),
    ('tools/cortexm0_footprint.py:151', 'cxx_flags = ['),
    ('tools/cortexm0_footprint.py:158', '"-DLIBTRACER_NO_ATOMIC",'),
    ('tools/cortexm0_footprint.py:172', '"--specs=nano.specs",'),
    ('core/tests/CMakeLists.txt:1231', 'add_executable(substrate_test_no_atomic'),
    ('core/tests/CMakeLists.txt:1244', 'target_compile_definitions(substrate_test_no_atomic PRIVATE'),
    ('core/tests/CMakeLists.txt:1245', '    LIBTRACER_NO_ATOMIC'),
    # The leading indent is load-bearing: the bare token also appears in the comment
    # three lines above the executable, and an anchor that matches both is not an anchor.

    # --- #1095: the rest of the non-source citations, now that a line-numbered citation
    # of an unverifiable file is an ERROR rather than a false green.
    #
    # 26 line-numbered citations of non-C++ files were live outside the two paths #1052
    # enrolled. Reading each target line against the prose that cites it found SIXTEEN
    # already pointing at unrelated text, none of which any gate could have caught: two
    # landed on a BLANK line and two on a bare `endif()`. Every entry below is pinned to
    # the line the prose actually names, and the citing doc was re-pinned to match.
    #
    # `--repin` does NOT move these (see @ref repin_document): the ANCHORS table is
    # rewritten by source suffix only, so a doc citation that moved without its table
    # entry would leave the two out of step. The gate reds on both and names the page.
    ('.github/workflows/core-ci.yml:113', 'matrix:', '  tsan:'),
    # The flag the prose QUOTES verbatim ("-fsanitize=thread -g -O1"). The range head at
    # 113 is `matrix:`, which the asan job carries too — hence the scope above; this one
    # needs none and is the stronger guard of the two.
    ('.github/workflows/core-ci.yml:122', '-DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1"'),
    ('.github/workflows/footprint-cortexm0.yml:13', '`--mode warn` governs the BUDGET VERDICT only'),
    ('bench/CMakeLists.txt:30', 'bench_libtracer_net (two-process ROUTER-flood bench) was retired'),
    ('bindings/typescript/packages/client/test/mesh-testbed.test.mjs:24',
     "ADDRESSING: a connection's routing key IS its vertex path"),
    ('core/CMakeLists.txt:63', 'option(LIBTRACER_NET_PLANE'),
    ('core/CMakeLists.txt:289', 'option(LIBTRACER_WITH_CUDA "Build the mem_cuda GPU backend'),
    ('core/CMakeLists.txt:312', 'option(LIBTRACER_WITH_QUIC "Configure the libtracer_quic transport module'),
    ('core/CMakeLists.txt:398', 'write_basic_package_version_file('),
    ('core/CMakeLists.txt:409', 'if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING AND EXISTS'),
    ('core/CMakeLists.txt:416', 'option(LIBTRACER_BUILD_EXAMPLES "Build the core examples"'),
    # `docs/examples/index.md` cited the two `if(LIBTRACER_NET_PLANE)` lines (58, 73). That
    # text appears THREE times in this file and the scope filter cannot separate 58 from 73
    # — a scope must sit ABOVE its candidate, and everything above 58 is also above 73. The
    # citation was re-pinned one line down onto the `add_executable` calls, which is what
    # "declared inside `if(LIBTRACER_NET_PLANE)` blocks" actually names anyway.
    ('core/examples/CMakeLists.txt:59', 'add_executable(two_node_fwd two_node_fwd.cpp)'),
    ('core/examples/CMakeLists.txt:74', 'add_executable(tree_of_ropes tree_of_ropes.cpp)'),
    ('core/examples/CMakeLists.txt:87', 'if(BUILD_TESTING)'),
    ('core/examples/CMakeLists.txt:92', 'add_test(NAME example_wire_codec COMMAND wire_codec)'),
    ('integrations/esp-idf/libtracer/CMakeLists.txt:44', 'set(LIBTRACER_SRCS'),
    ('integrations/esp-idf/libtracer/CMakeLists.txt:172', 'if(CONFIG_LIBTRACER_TRANSPORT_CAN)'),
    ('integrations/esp-idf/libtracer/CMakeLists.txt:273', 'set(LIBTRACER_ACL_POLICY allow_only_policy_t)'),
    ('integrations/esp-idf/libtracer/CMakeLists.txt:274', 'set(LIBTRACER_LKV_SLOT sp_atomic_slot_t)'),
    ('integrations/esp-idf/libtracer/CMakeLists.txt:306', 'if(IDF_TARGET STREQUAL "linux")'),
    ('integrations/esp-idf/libtracer/CMakeLists.txt:275', 'set(LIBTRACER_HAZARD_READER_SLOTS 64)'),
    ('integrations/esp-idf/libtracer/CMakeLists.txt:285', 'set(LIBTRACER_EDGE_PIN_SLOTS 8)'),
    ('integrations/esp-idf/libtracer/CMakeLists.txt:290', 'if(CONFIG_FREERTOS_UNICORE)'),
    ('integrations/esp-idf/libtracer/CMakeLists.txt:61',
     '"${LIBTRACER_ROOT}/core/src/route_handle.cpp"'),
    ('integrations/esp-idf/libtracer/CMakeLists.txt:173',
     'list(APPEND LIBTRACER_SRCS "${LIBTRACER_ROOT}/core/src/transport_can.cpp")'),
]


def source_map(root: pathlib.Path = None) -> dict:
    """Map every source BASENAME in the tree to the repo-relative paths carrying it.

    A basename with one path is resolvable shorthand; a basename with two or more is
    ambiguous and a doc citing it is an error, not a coin flip. Built once per run.
    """
    root = root or REPO
    out = {}
    for path in root.rglob("*"):
        if not path.is_file() or not path.name.endswith(SOURCE_SUFFIXES):
            continue
        rel = path.relative_to(root)
        if any(_is_non_source_part(p) for p in rel.parts):
            continue
        out.setdefault(path.name, []).append(rel.as_posix())
    return {name: sorted(paths) for name, paths in out.items()}


# One token: an optional directory prefix, a source basename, `:`, and a line spec —
# or a citation of a NON-source file, or a bare `` `:99` `` continuation. The extension
# alternation is ordered longest-first so `config.hpp.in` does not truncate to
# `config.hpp`.
#
# The DOCUMENT branch exists purely to BREAK inheritance: a page that cites
# `reference/07-host-embedding.md:79` and then writes `` `:285` `` means line 285 of that
# markdown page, not of whatever header it named three paragraphs earlier. Without this
# branch the bare form silently attached to the stale source file and pinned a line
# nothing in the docs was talking about.
#
# Only a DOCUMENT breaks the run. A build file cited mid-sentence
# (`CMakeLists.txt:188`) does not — the configuration page's knob table names
# `config.hpp.in` once and then walks down it in bare `:109` / `:136` refs, with each
# row's CMake column citing a `CMakeLists.txt` line in between. The running citation
# there is the header; the build file is an aside.
#
# #1052: a build or tooling file is not a source, so a citation into one was invisible
# here and therefore unpinnable — `docs/modules/segment.md`'s `LIBTRACER_NO_ATOMIC`
# sentence carried two of them, both rotted onto unrelated lines, sitting beside a
# `segment.hpp` citation the gate DID verify. Enrolment is an explicit ALLOWLIST rather
# than an extension suffix: whether the pin list should cover build and tooling files
# wholesale is a maintainer's call, and every path listed here is one whose citations
# are pinned in ANCHORS below.
#
# An enrolled path is still an ASIDE — it anchors its own lines and never becomes the
# running file — so the knob table's bare `:109` / `:136` continuations keep walking the
# header they name, exactly as before. That also means a doc citing an enrolled path
# must spell it out every time; there is no bare continuation into one.
#
# #1095 widened the list from two paths to nine, and renamed it: a GitHub workflow and a
# `.mjs` test driver are not "build" files. What forced the widening is that enrolment was
# the ONLY way a non-source citation could be checked, and everything outside it was a
# FALSE GREEN — see @ref unverifiable_citations, which now makes that class an error
# instead of silence.
CITABLE_NON_SOURCE_PATHS = (
    ".github/workflows/core-ci.yml",
    ".github/workflows/footprint-cortexm0.yml",
    "bench/CMakeLists.txt",
    "bindings/typescript/packages/client/test/mesh-testbed.test.mjs",
    "core/CMakeLists.txt",
    "core/examples/CMakeLists.txt",
    "core/tests/CMakeLists.txt",
    "integrations/esp-idf/libtracer/CMakeLists.txt",
    "tools/cortexm0_footprint.py",
)
_EXTS = "|".join(re.escape(s[1:]) for s in SOURCE_SUFFIXES)
DOC_EXTS = "md|rst"
# Any `path.ext` at all. The NON-SOURCE branch below matches this and classifies AFTER
# resolving, rather than matching an exact enrolled path: docs spell a non-source citation
# in the same three ways they spell a source one, and two of them are not the full path —
# `integrations/esp-idf/README.md` writes the partial `libtracer/CMakeLists.txt:131` and
# `tests/testbed/README.md` writes the bare basename `mesh-testbed.test.mjs:24-25`. An
# exact-path alternation reads neither, so both would fall through as unverifiable.
_ANY_PATH = r"(?:[A-Za-z0-9_./-]*/)?[A-Za-z0-9_][A-Za-z0-9_.-]*\.[A-Za-z0-9_]+"
# The leading `` `? `` on the DOCUMENT branch is load-bearing, and was not needed until the
# catch-all branch existed. `finditer` takes the EARLIEST match, and only then the earliest
# alternative: with a backtick the catch-all could start one character before the document
# branch could, so `` `docs/reference/07-host-embedding.md:79` `` matched the catch-all
# instead — and a cited page silently stopped BREAKING the inheritance run. Measured on the
# real doc set: four RFCs and one README then dragged 61 bare `:N` continuations onto stale
# source files (0019: 42, 0018: 14, 0024: 3, 0023: 1, tests/testbed/README.md: 1).
CITATION_RE = re.compile(
    r"`?((?:[A-Za-z0-9_./-]*/)?[A-Za-z0-9_][A-Za-z0-9_.-]*\.(?:" + _EXTS + r")):([\d,\-]+)`?"
    r"|`?((?:[A-Za-z0-9_./-]*/)?[A-Za-z0-9_][A-Za-z0-9_.-]*\.(?:" + DOC_EXTS + r")):[\d,\-]+"
    r"|`:([\d,\-]+)`"
    r"|`?(?P<other>" + _ANY_PATH + r"):(?P<otherspec>[\d,\-]+)`?"
)


@functools.lru_cache(maxsize=None)
def enrolled_map() -> dict:
    """Map each enrolled basename to the enrolled paths carrying it.

    Built from @ref CITABLE_NON_SOURCE_PATHS alone, never from the filesystem: enrolment
    is the maintainer's allowlist, so a file that merely exists must not resolve here.
    """
    out = {}
    for path in CITABLE_NON_SOURCE_PATHS:
        out.setdefault(path.rsplit("/", 1)[-1], []).append(path)
    return {name: sorted(paths) for name, paths in out.items()}


def resolve_enrolled(spelling: str) -> str:
    """Resolve a non-source spelling to an ENROLLED path, or None.

    Reads the same three spellings @ref _resolve reads, over the allowlist instead of the
    source tree: the full path, a partial path that singles out one carrier, and a bare
    basename. A spelling that names two enrolled paths resolves to neither — the doc must
    spell enough of the path to pick one.
    """
    hits = enrolled_map().get(spelling.rsplit("/", 1)[-1], ())
    if "/" in spelling:
        hits = [h for h in hits if h == spelling or h.endswith("/" + spelling)]
    return hits[0] if len(hits) == 1 else None


@functools.lru_cache(maxsize=None)
def tree_index() -> dict:
    """Map every basename in the tree to its repo-relative paths (build output excluded).

    Only @ref unverifiable_citations reads this. It answers one question — "does this
    `name:123` token name a REAL FILE?" — which is what separates a citation the gate
    cannot check from a host:port pair. `tests/testbed/README.md` writes
    `127.0.0.1:47301` and `bindings/.../README.md` writes `wss://robot.local:9000`; both
    parse as `path.ext:digits` and neither is a citation. Nothing in the tree is named
    `127.0.0.1` or `robot.local`, and that is the whole discriminator.

    Walks with `os.walk` and PRUNES as it goes, rather than `rglob`-ing everything and
    filtering after. Same result, and the difference is not cosmetic: `rglob` descends into
    `.git` and every `build-*` tree before discarding them, which measured +1.2 s on a gate
    that runs ~1.5 s. Pruning keeps this second walk in the noise.
    """
    out = {}
    for dirpath, dirnames, filenames in os.walk(REPO):
        dirnames[:] = [d for d in dirnames if not _is_non_source_part(d)]
        rel_dir = pathlib.Path(dirpath).relative_to(REPO).as_posix()
        for name in filenames:
            # A FILE whose own name is a non-source part is skipped too, matching what
            # @ref source_map's `rel.parts` test does (it sees the basename as a part).
            if _is_non_source_part(name):
                continue
            out.setdefault(name, []).append(name if rel_dir == "." else f"{rel_dir}/{name}")
    return {name: sorted(paths) for name, paths in out.items()}


def unverifiable_citations(text: str) -> list:
    """Line-numbered citations of files this gate cannot verify — the #1095 false green.

    `SOURCE_SUFFIXES` is what the gate can read, so a line number in anything else was
    invisible: the tool exited 0 whether the anchor resolved or not. #1088 shifted
    `.github/workflows/core-ci.yml` by 18 lines while a design page cited `:95-106` for the
    ThreadSanitizer configuration; after the shift that range named a DIFFERENT job's
    matrix, the gate stayed green, and the PR concluded "no cited file shifted lines" FROM
    THAT SILENCE. A false green is worse than a false red, because nobody goes looking.

    So: a citation carrying a line number is verified, or it is refused. A token naming a
    real file that is neither a source nor enrolled is reported here, and the author must
    enrol the file (@ref CITABLE_NON_SOURCE_PATHS, then pin it in @ref ANCHORS) or drop the
    line number. A token that names no file in the tree is NOT reported — it is an address
    or a file outside the repo, and this tool has never claimed those. An AMBIGUOUS basename
    IS reported: it resolves to nothing, so leaving it out would reopen the same false green
    one step earlier (`CMakeLists.txt:172` has 15 candidate carriers here).
    """
    out = []
    for m in CITATION_RE.finditer(text):
        spelling = m.group("other")
        if not spelling or resolve_enrolled(spelling):
            continue
        hits = tree_index().get(spelling.rsplit("/", 1)[-1], ())
        if "/" in spelling:
            hits = [h for h in hits if h == spelling or h.endswith("/" + spelling)]
        if len(hits) == 1:
            out.append(f"`{spelling}:{m.group('otherspec')}` cites a line in a file this gate "
                       f"cannot verify ({hits[0]}) — enrol it in CITABLE_NON_SOURCE_PATHS and "
                       f"pin it in ANCHORS, or drop the line number")
        elif len(hits) > 1:
            # An AMBIGUOUS non-source basename is the same false green, one step earlier: the
            # spelling resolves to nothing, so the branch above never fires and the citation is
            # accepted in silence. `CMakeLists.txt` has 15 carriers here, so `CMakeLists.txt:172`
            # would have sailed through. Reported with the same remedy plus the disambiguation
            # the source path already demands.
            out.append(f"`{spelling}:{m.group('otherspec')}` cites a line in a file this gate "
                       f"cannot verify, and `{spelling}` is an ambiguous basename "
                       f"({', '.join(hits)}) — cite the full path AND enrol it in "
                       f"CITABLE_NON_SOURCE_PATHS, or drop the line number")
    return out


@functools.lru_cache(maxsize=None)
def _resolve(spelling: str, filemap_key: tuple) -> tuple:
    """Resolve one cited spelling to (repo-relative path, error-or-None)."""
    filemap = dict(filemap_key)
    hits = filemap.get(spelling.rsplit("/", 1)[-1], ())
    if "/" in spelling:
        # A spelled path is the doc's answer to ambiguity, so it is honoured FIRST: a
        # partial path that singles out one of the basename's carriers resolves to it,
        # and only a spelling that still names two of them is an error. A path the map
        # does not carry at all falls back to the filesystem, then to the basename.
        exact = [h for h in hits if h == spelling or h.endswith("/" + spelling)]
        if len(exact) == 1:
            return exact[0], None
        if not exact and (REPO / spelling).is_file():
            return spelling, None
        hits = exact or hits
    if len(hits) == 1:
        return hits[0], None
    if len(hits) > 1:
        return None, f"`{spelling}` is an ambiguous basename ({', '.join(hits)}) — cite the full path"
    return None, None


def citation_spans(context: str, filemap: dict = None) -> tuple:
    """Every cited source SPAN in one doc, as ([(path, lo, hi)], ambiguity errors).

    Handles every spelling the docs actually use: a full path `core/src/graph.cpp:12`,
    the design pages' basename shorthand `graph.hpp:1291`, a generated header
    `config.hpp.in:237`, a range `file.cpp:12-20`, a comma list `file.hpp:145,153`, and
    the UNBACKTICKED form that appears inside annotated code-excerpt blocks. A bare
    `` `:99` `` inherits the most recently named file, which is how the glossary and the
    design pages write sibling citations.

    Every span is normalised to its full repo-relative path, so the ANCHORS table has
    exactly one spelling regardless of how the prose says it.

    An enrolled non-source path (@ref CITABLE_NON_SOURCE_PATHS) anchors its own lines
    without becoming the running file, so it can be pinned without disturbing the bare
    `:N` runs that walk a header down a table.
    """
    filemap = source_map() if filemap is None else filemap
    key = tuple((k, tuple(v)) for k, v in sorted(filemap.items()))
    spans, errors, last = [], [], None
    for m in CITATION_RE.finditer(context):
        if m.group(1):
            resolved, err = _resolve(m.group(1), key)
            if err:
                errors.append(err)
            if not resolved:
                # An unresolvable name is not this tool's business (a doc may cite a
                # file that lives outside the repo); it just cannot anchor a line.
                last = None
                continue
            last, path, spec = resolved, resolved, m.group(2)
        elif m.group(3):
            last = None  # a cited DOCUMENT ends the inheritance run
            continue
        elif m.group("other"):
            # An enrolled non-source path anchors its own lines and leaves `last` alone,
            # so it stays an ASIDE. Anything else here names no enrolled file and pins
            # nothing — @ref unverifiable_citations is what decides whether that silence
            # is legitimate (an address, an out-of-repo file) or a refused citation.
            path = resolve_enrolled(m.group("other"))
            if path is None:
                continue
            spec = m.group("otherspec")
        elif last:
            path, spec = last, m.group(4)
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
            spans.append((path, lo, hi))
    return spans, errors


def cited_locations(context: str, filemap: dict = None) -> tuple:
    """Every cited `path:line` in one doc, as (locations, ambiguity errors).

    A range registers EVERY line in it, not just the first — a doc citing `996-997` is
    citing both, and either may be the line an anchor pins.
    """
    spans, errors = citation_spans(context, filemap)
    return {f"{p}:{n}" for p, lo, hi in spans for n in range(lo, hi + 1)}, errors


def citation_index(docs, filemap: dict = None) -> dict:
    """Map every cited `path:line` to the doc pages that cite it.

    The gate used to report a drifted anchor by its SOURCE location alone, leaving the
    reader to grep ~200 markdown files for whoever pointed at it — and when the citing
    spelling was a comma continuation or a bare `:N`, that grep found nothing and the
    anchor read as orphaned. This is what lets a DRIFT name the stale citation itself.

    `docs` is an iterable of `(name, text)` pairs.
    """
    filemap = source_map() if filemap is None else filemap
    index = {}
    for name, text in docs:
        for loc in cited_locations(text, filemap)[0]:
            index.setdefault(loc, []).append(name)
    return {loc: sorted(set(names)) for loc, names in index.items()}


# --------------------------------------------------------------------------------------
# Re-pin (#836): rewrite the citations a source edit moved, in every spelling, in one pass
# --------------------------------------------------------------------------------------


def line_map_from_texts(old_text: str, new_text: str) -> dict:
    """The exact OLD-line -> NEW-line map between two revisions of one file.

    Every line of `old_text` gets an entry; a line the edit deleted maps to None. A
    replaced block maps 1:1 only when it kept its line COUNT (an in-place reword, where
    identity is certain); a block that changed length has no line identity and maps to
    None, so the citations inside it are reported for a human instead of guessed at.

    This is the complete map a re-pin wants: a range's endpoints and a comma list's
    elements are ordinary lines here, so no spelling can hide a line from it.
    """
    old, new = old_text.split("\n"), new_text.split("\n")
    out = {}
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(a=old, b=new, autojunk=False).get_opcodes():
        if tag == "equal":
            for k in range(i2 - i1):
                out[i1 + k + 1] = j1 + k + 1
        elif tag == "replace":
            same_length = (i2 - i1) == (j2 - j1)
            for k in range(i2 - i1):
                out[i1 + k + 1] = (j1 + k + 1) if same_length else None
        elif tag == "delete":
            for k in range(i2 - i1):
                out[i1 + k + 1] = None
    return out


def anchor_line_maps(anchors: list = None, root: pathlib.Path = None) -> tuple:
    """Line maps derived from the ANCHORS table, as ({path: {old: new}}, notes).

    An anchor whose pinned line STILL HOLDS its text contributes a fixed point (old ==
    new) — not because its text matches something, but because the citation already
    resolves in the current tree. That distinction is the whole discipline: comparing
    the old line's text against the new line's is vacuous under a uniform shift, where
    every line matches whether or not it moved.

    An anchor whose text now sits at exactly one other line yields that move. One that
    is gone, or ambiguous even after its scope filter, yields a note and NO mapping —
    a re-pin that guesses is the failure this tool exists to stop.
    """
    anchors = ANCHORS if anchors is None else anchors
    root = root or REPO
    maps, notes, cache = {}, [], {}
    for entry in anchors:
        loc, anchor = entry[0], entry[1]
        scope = entry[2] if len(entry) > 2 else None
        path, lineno = loc.rsplit(":", 1)
        lineno = int(lineno)
        if path not in cache:
            src = root / path
            cache[path] = src.read_text().split("\n") if src.exists() else None
        lines = cache[path]
        if lines is None:
            notes.append(f"{loc}: file does not exist")
            continue
        if lineno <= len(lines) and anchor in lines[lineno - 1]:
            maps.setdefault(path, {})[lineno] = lineno
            continue
        hits = [i + 1 for i, ln in enumerate(lines) if anchor in ln]
        if scope:
            hits = [h for h in hits if any(scope in x for x in lines[max(0, h - SCOPE_LINES) : h])]
        if len(hits) == 1:
            maps.setdefault(path, {})[lineno] = hits[0]
        else:
            notes.append(f"{loc}: {'ambiguous ' + str(hits) if hits else 'anchor GONE'} — re-pin by hand")
    return maps, notes


def shift_lookup(known: dict):
    """Extend a SPARSE old->new map to any line, or to None where the shift is unproven.

    A doc cites lines no anchor pins — the far end of a range, a sibling in a comma list
    — so the anchored moves have to speak for the lines between them. A line inherits a
    shift only when the pinned lines on BOTH sides of it agree on that shift: a citation
    straddling two different edits has no derivable answer and gets None, which the
    caller reports rather than applies. Before the first pinned line the shift is taken
    as zero (an edit further down cannot move the head of the file); after the last one
    the final shift carries, which is the ordinary "everything below moved by N" tail.
    """
    points = sorted(known)

    def lookup(line: int):
        if line in known:
            return known[line]
        before = [p for p in points if p < line]
        after = [p for p in points if p > line]
        d_before = known[before[-1]] - before[-1] if before and known[before[-1]] is not None else None
        d_after = known[after[0]] - after[0] if after and known[after[0]] is not None else None
        if d_before is None and d_after is None:
            return None
        if d_before is None:
            d_before = 0
        if d_after is None:
            d_after = d_before
        return line + d_before if d_before == d_after else None

    return lookup


def _repin_spec(spec: str, lookup) -> tuple:
    """Rewrite one citation's line spec (`12`, `12-20`, `145,153`), as (spec, moves, held).

    BOTH endpoints of a range and EVERY element of a comma list go through `lookup`.
    Mapping only the head is the defect that leaves inverted ranges like `1118-1114`
    behind, so a mapping that would invert a range is refused outright. Anything that
    cannot be mapped end to end comes back VERBATIM and named in `held`: a half-applied
    re-pin is worse than none, because it reads as done.
    """
    parts, moves, held = [], [], []
    for part in spec.split(","):
        ends = part.split("-")
        tail = part[len(ends[0]):]  # `-20`, a bare `-`, or nothing — preserved verbatim
        if not ends[0].isdigit():
            parts.append(part)
            continue
        lo = int(ends[0])
        hi = int(ends[1]) if len(ends) > 1 and ends[1].isdigit() else None
        # Whatever follows the `lo-hi` tokens, preserved verbatim. `[\d,\-]+` is greedy,
        # so unbackticked prose like `graph.cpp:12-20-style` is captured as the spec
        # `12-20-`; rebuilding the range as f"{lo}-{hi}" alone DROPS that trailing hyphen
        # and silently edits the sentence. The single-line branch below already preserves
        # its `tail`; the range branch has to preserve its own.
        rest = part[len(ends[0]) + 1 + len(ends[1]):] if hi is not None else ""
        # The scanner reads an implausible span as a parse artifact (a hyphenated word
        # beside a citation) and pins only its head. The re-pin cannot tell that apart
        # from a genuinely wide span, and it must not rewrite prose — so it moves
        # NEITHER end and says so. Moving the head alone is the half-application that
        # produced `graph.cpp:755-806` on the first live run: a correct head over a
        # stale tail, which reads as re-pinned.
        if hi is not None and (hi < lo or hi - lo > 40):
            held.append(lo)
            parts.append(part)
            continue
        new_lo = lookup(lo)
        if new_lo is None:
            held.append(lo)
            parts.append(part)
            continue
        if hi is None:
            parts.append(f"{new_lo}{tail}")
            if new_lo != lo:
                moves.append((lo, new_lo))
            continue
        new_hi = lookup(hi)
        if new_hi is None or new_hi < new_lo:
            held.append(hi)
            parts.append(part)
            continue
        parts.append(f"{new_lo}-{new_hi}{rest}")
        moves += [(a, b) for a, b in ((lo, new_lo), (hi, new_hi)) if a != b]
    return ",".join(parts), moves, held


def repin_document(text: str, maps: dict, filemap: dict = None) -> tuple:
    """Re-pin one doc's citations, as (new_text, moves, held).

    Reads exactly the spellings the scanner reads — full path, basename shorthand,
    range, comma list, and the bare `:N` continuation with the same inheritance rules
    (a cited markdown page still ends the run) — because a spelling the re-pin cannot
    see is a citation that goes stale silently while the sweep reports success.

    ONE PASS: every rewrite is collected against the ORIGINAL text and spliced in at the
    end. A sweep that rewrote as it went would re-read its own output and move a
    citation twice — 51 stale citations became 60 exactly that way.
    """
    filemap = source_map() if filemap is None else filemap
    key = tuple((k, tuple(v)) for k, v in sorted(filemap.items()))
    lookups = {p: shift_lookup(m) for p, m in maps.items()}
    edits, moves, held, last = [], [], [], None
    for m in CITATION_RE.finditer(text):
        if m.group(1):
            resolved, _ = _resolve(m.group(1), key)
            if not resolved:
                last = None
                continue
            last, spec, span = resolved, m.group(2), m.span(2)
        elif m.group(3):
            last = None  # a cited DOCUMENT ends the inheritance run
            continue
        elif m.group("other"):
            # An enrolled non-source path is re-pinned by HAND, and is REPORTED as held
            # rather than skipped in silence (#1095). Two independent reasons, both still
            # true after the list grew to nine paths: `revision_line_maps` derives its maps
            # from source files only, so `--from-rev` has no map to move these by; and
            # `ANCHOR_ENTRY_RE` matches source suffixes only, so even under the
            # anchor-derived map the TABLE entry would stay put while the doc citation
            # moved. Half-applying it that way puts doc and anchor out of step, which is
            # strictly worse than leaving both — the gate then reds on both and names the
            # page that cites them.
            enrolled = resolve_enrolled(m.group("other"))
            if enrolled:
                held.append((enrolled, m.group("otherspec")))
            continue
        elif last:
            spec, span = m.group(4), m.span(4)
        else:
            continue
        if last not in lookups:
            continue
        new_spec, spec_moves, spec_held = _repin_spec(spec, lookups[last])
        held += [(last, n) for n in spec_held]
        if new_spec != spec:
            edits.append((span[0], span[1], new_spec))
            moves += [(last, a, b) for a, b in spec_moves]
    if not edits:
        return text, moves, held
    out, prev = [], 0
    for start, end, repl in edits:
        out.append(text[prev:start])
        out.append(repl)
        prev = end
    out.append(text[prev:])
    return "".join(out), moves, held


# The anchor table's own citations. It is written in BOTH quote styles — 92 entries in
# `"..."` and 256 in `'...'`, with `grammar.hpp` split across the two — so a sweep that
# matched one style silently skipped the other; three `grammar.hpp` anchors survived a
# re-pin that way. The backreference makes the quote irrelevant, and requiring an
# opening paren before it keeps the rewrite to element 0 of a tuple, never an anchor's
# quoted TEXT.
ANCHOR_ENTRY_RE = re.compile(
    r"(\(\s*)(['\"])((?:[A-Za-z0-9_./-]*/)?[A-Za-z0-9_][A-Za-z0-9_.-]*\.(?:" + _EXTS + r")):(\d+)\2"
)


def repin_anchor_table(text: str, maps: dict) -> tuple:
    """Re-pin the ANCHORS table itself, as (new_text, moves, held).

    The table is a citation surface like any doc: it pins `path:line` and rots the same
    way. `re.sub` walks the ORIGINAL string once, so this cannot feed its own output
    either.
    """
    lookups = {p: shift_lookup(m) for p, m in maps.items()}
    moves, held = [], []

    def rewrite(m):
        path, lineno = m.group(3), int(m.group(4))
        if path not in lookups:
            return m.group(0)
        new = lookups[path](lineno)
        if new is None:
            held.append((path, lineno))
            return m.group(0)
        if new != lineno:
            moves.append((path, lineno, new))
        return f"{m.group(1)}{m.group(2)}{path}:{new}{m.group(2)}"

    return ANCHOR_ENTRY_RE.sub(rewrite, text), moves, held


def revision_line_maps(rev: str, root: pathlib.Path = None) -> tuple:
    """Exact line maps for every source file that changed since `rev`, as (maps, notes).

    Plumbing: `git show REV:path` supplies the old text, the worktree the new, and
    @ref line_map_from_texts does the rest. This is the mode to use when you KNOW which
    edit moved the lines; the anchor-derived map is the fallback that re-pins only what
    the gate can prove moved.

    An ENROLLED non-source path gets a map here too, and it is never used to rewrite one:
    @ref repin_document declines those before it consults a lookup. It exists so the
    driver can tell a shifted enrolled file from an untouched one and report only the
    former — 32 identical "held" lines on every clean run is how a report gets ignored.
    """
    root = root or REPO
    git = ["git", "-C", str(root)]
    changed = subprocess.run(git + ["diff", "--name-only", rev, "--"],
                             capture_output=True, text=True, check=True).stdout.split("\n")
    maps, notes = {}, []
    for rel in (c.strip() for c in changed if c.strip()):
        if (not rel.endswith(SOURCE_SUFFIXES) and rel not in CITABLE_NON_SOURCE_PATHS) or any(
                _is_non_source_part(p) for p in pathlib.PurePosixPath(rel).parts):
            continue
        old = subprocess.run(git + ["show", f"{rev}:{rel}"], capture_output=True, text=True)
        if old.returncode != 0:
            notes.append(f"{rel}: absent at {rev} — nothing to re-pin from")
            continue
        new = root / rel
        if not new.is_file():
            notes.append(f"{rel}: deleted in the worktree — its citations need a human")
            continue
        maps[rel] = line_map_from_texts(old.stdout, new.read_text())
    return maps, notes


# The genres a re-pin must NOT rewrite — the same three the anchor table refuses to
# enrol, for the same reason. An ADR, an RFC and a research note are DATED records:
# their citations describe the tree as it stood on the day, some already point past
# today's EOF, and moving them forward with the code rewrites the record. A re-pin that
# swept `all_docs()` edited 9 ADRs, 5 RFCs and 2 research notes on its first live run.
HISTORICAL_GENRES = ("docs/adr/", "docs/spec/", "docs/research/")


def is_historical(rel: str) -> bool:
    """True when `rel` (a repo-relative posix path) is a dated record, not a live doc."""
    return rel.startswith(HISTORICAL_GENRES)


def repin(from_rev: str = None, apply: bool = False) -> int:
    """The `--repin` driver: build ONE map, rewrite every LIVING surface once, report."""
    filemap = source_map()
    if from_rev:
        maps, notes = revision_line_maps(from_rev)
    else:
        maps, notes = anchor_line_maps()
    targets = [(REPO / "tools" / "check_doc_citations.py", repin_anchor_table)] + [
        (doc, None) for doc in all_docs()
    ]
    # An enrolled path whose anchors all still resolve has not moved, so its citations
    # need no attention and saying otherwise is noise. Only a path with a MOVED anchor is
    # reported below.
    shifted = {p for p, m in maps.items()
               if any(new is not None and new != old for old, new in m.items())}
    total, historical, held_all, enrolled_held = 0, 0, list(notes), []
    for path, table_fn in targets:
        try:
            text = path.read_text()
        except (OSError, UnicodeDecodeError):
            continue
        rel = path.relative_to(REPO).as_posix()
        if table_fn:
            new_text, moves, held = table_fn(text, maps)
        else:
            new_text, moves, held = repin_document(text, maps, filemap)
        if is_historical(rel):
            # Counted, never written: the number is worth knowing, the edit is not.
            historical += len(moves)
            continue
        # Two different reasons a citation is held, and saying "no derivable shift" for
        # both would misreport the enrolled one as a map gap the tool could close (#1095).
        for p, n in held:
            if p in CITABLE_NON_SOURCE_PATHS:
                if p in shifted:
                    enrolled_held.append(f"{rel}: {p}:{n}")
            else:
                held_all.append(f"{rel}: {p}:{n} — no derivable shift, re-pin by hand")
        if not moves:
            continue
        total += len(moves)
        for p, old, new in moves:
            print(f"REPIN {rel}: {p}:{old} -> {new}")
        if apply:
            path.write_text(new_text)
    for note in dict.fromkeys(held_all):
        print(f"HOLD  {note}")
    # Said plainly rather than skipped in silence (#1095): --repin will not move these,
    # and the reason is structural, not a gap it could close on a later run.
    for note in dict.fromkeys(enrolled_held):
        print(f"MANUAL {note} — enrolled non-source path; --repin does not move these "
              f"(ANCHOR_ENTRY_RE matches source suffixes only, so the table entry would "
              f"stay put while the doc moved). Re-pin the doc AND its anchor, by hand.")
    verb = "rewritten" if apply else "would move (dry run; pass --apply to write)"
    print(f"\n{total} citation(s) {verb}; {len(dict.fromkeys(held_all))} held for a human"
          f"; {len(dict.fromkeys(enrolled_held))} in enrolled non-source paths need a hand re-pin.")
    if historical:
        print(f"      {historical} more sit in {', '.join(g.rstrip('/') for g in HISTORICAL_GENRES)} "
              f"and were left alone — a dated record cites the tree as it stood.")
    return 0


def all_docs() -> list:
    """Every tracked markdown file. `_build` is generated Sphinx output; the
    `.claude/worktrees/fw-pin-*` trees are pinned history. Neither is a source."""
    skip = ("_build", "node_modules", ".claude", ".git")
    # Match skip components against the path RELATIVE to the repo root — an absolute
    # match made the tool skip EVERY doc when run from a `.claude/worktrees/*` checkout,
    # turning the whole check into a vacuous pass there.
    return [p for p in REPO.rglob("*.md")
            if not any(s in p.relative_to(REPO).parts for s in skip)]


def main(argv: list = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--repin", action="store_true",
                        help="rewrite the citations a source edit moved, in every spelling")
    parser.add_argument("--from-rev", metavar="REV",
                        help="derive exact line maps by diffing the tree against REV "
                             "(default: derive them from the anchors the gate proves drifted)")
    parser.add_argument("--apply", action="store_true",
                        help="write the re-pinned files (default: print the plan and change nothing)")
    args = parser.parse_args(argv)
    if args.repin:
        return repin(args.from_rev, args.apply)
    if args.from_rev or args.apply:
        parser.error("--from-rev and --apply are only meaningful with --repin")

    filemap = source_map()
    docs, failures, drifted = [], [], []
    for doc in all_docs():
        try:
            text = doc.read_text()
        except (OSError, UnicodeDecodeError):
            continue
        rel = doc.relative_to(REPO).as_posix()
        docs.append((rel, text))
        failures += [f"{rel}: {e}" for e in dict.fromkeys(cited_locations(text, filemap)[1])]
        # The dated genres are exempt for the same reason they are never anchored: an ADR
        # or an RFC cites the tree AS IT STOOD, so demanding that its citations be
        # verifiable today would demand rewriting the record. Of the line-numbered
        # non-source citations living in those genres, TWENTY-SIX name a Rust or TypeScript
        # binding file; the other two name an ESP-IDF CMakeLists and a conformance script.
        # All of them describe history.
        if not is_historical(rel):
            failures += [f"{rel}: {e}" for e in dict.fromkeys(unverifiable_citations(text))]
    index = citation_index(docs, filemap)
    present = set(index)

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
        # Name the pages that cite it. Without this a comma continuation or a bare `:N`
        # left the reader grepping for a spelling that does not literally appear.
        citers = index.get(loc, [])
        by = f"\n      cited by: {', '.join(citers)}" if citers else ""
        drifted.append(f"{loc}: expected {anchor!r}{where}\n      actual: {lines[lineno - 1].strip()[:90]}{by}")

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
        if drifted:
            print("      `--repin` rewrites the moved ones in every spelling; re-run this gate after.")
        return 1
    print(f"OK    {len(ANCHORS)} doc citations verified against source.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
