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
NON_SOURCE_DIRS = ("_build", "build", "node_modules", ".claude", ".git", "target", "dist")
# ...and the same, for any `build-<something>` sibling. The exact name "build" is not the
# only one that appears: this repo's agent workflow mandates `build-agent` (`.gitignore`
# covers `build-*/`), and a generated `build-agent/generated/include/libtracer/config.hpp`
# makes `config.hpp` an AMBIGUOUS basename, which the gate reports as stale citations that
# do not exist. Green-vs-red then depended on whether someone had configured a build in the
# tree, which is exactly the kind of environment-sensitivity a gate must not have.
NON_SOURCE_DIR_PREFIXES = ("build-",)


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
    ("core/include/libtracer/tlv.hpp:63", "struct opt_t"),
    ("core/include/libtracer/tlv.hpp:30", "enum class type_t"),
    ("core/src/graph.cpp:1990", "graph_t::set_identity"),
    ("core/src/graph.cpp:2016", "graph_t::read_identity"),
    ("core/src/graph.cpp:2398", 'field.steps[0].name == "identity"'),
    ("core/src/transport_vertex.cpp:64", 'cfg.name("kind")'),
    ("core/src/transport_vertex.cpp:99", "register_child_type"),
    ("core/src/transport_vertex.cpp:128", "register_transport_type"),
    ("core/src/transport_vertex.cpp:133", "transport_vertex_t::register_module"),
    ("core/src/transport_vertex.cpp:167", "SCHEMA_NOT_FOUND", "transport_vertex_t::module_for"),
    # fwd-router.md's "Signature source" line — bare :NNN shorthands that had ALL rotted
    # silently (they cited the pre-#739 header). Anchored so they cannot rot again.
    # zero-copy-and-flatten.md's rope-tier citations and ADR-0072's stale-comment pointer —
    # all four had rotted on main and were re-asserted by a mechanical +24 shift (#768 verify).
    ("core/include/libtracer/fwd_router.hpp:786", "Terminus over a MULTI-LINK rope"),
    ("core/include/libtracer/fwd_router.hpp:792", "64 KB / 2 links"),
    ("core/include/libtracer/fwd_router.hpp:804", "The forward hop, read entirely by OFFSET"),
    ("core/include/libtracer/fwd_router.hpp:604", "Slot addresses are NOT stable"),
    ("core/include/libtracer/fwd_router.hpp:177", "explicit fwd_router_t"),
    ("core/include/libtracer/fwd_router.hpp:246", "bool add_child"),
    ("core/include/libtracer/fwd_router.hpp:405", "using reply_fn_t"),
    ("core/include/libtracer/child_registry.hpp:209", "bool add(std::string name"),
    ("core/include/libtracer/child_registry.hpp:458", "resolve_peer"),
    ("core/include/libtracer/child_registry.hpp:473", "bool erase"),
    ("core/include/libtracer/child_registry.hpp:499", "entry_by_name"),
    ("core/include/libtracer/child_registry.hpp:520", "by_name"),
    ("core/include/libtracer/child_registry.hpp:561", "std::size_t size()"),
    ("core/include/libtracer/child_registry.hpp:571", "live_size"),
    ("core/src/transport_vertex.cpp:170", "transport_vertex_t::provide_link"),
    ("core/src/transport_vertex.cpp:223", "routing key IS the mount path"),
    ("core/src/transport_vertex.cpp:230", "qualified += name"),
    ("core/src/transport_vertex.cpp:249", "grouping vertex, created lazily"),
    ("core/src/transport_vertex.cpp:257", "register_vertex_key(mod_key"),
    ("core/src/transport_vertex.cpp:347", "if (!router_.add_child(qualified, *link))"),
    ("core/src/transport_vertex.cpp:350", "return std::unexpected(status_t::BACKPRESSURE);",
     "if (!router_.add_child(qualified, *link))"),
    ("core/src/transport_vertex.cpp:356", "pending_links_.erase(pl)"),
    ("core/src/transport_vertex.cpp:370", "if (constructed)"),
    ("core/src/transport_vertex.cpp:372", "link_state_t::LISTENING : link_state_t::UP"),
    ("core/include/libtracer/transport_vertex.hpp:267", "result_t<void> register_module"),
    ("core/include/libtracer/transport_vertex.hpp:96", "enum class link_state_t"),
    ("core/src/graph.cpp:1719", "field.steps.size() != 1", 'step0.name == "subscribers"'),
    ("core/src/graph.cpp:1798", "field.steps.size() != 1 || !plain_step(step0)"),
    ("core/src/graph.cpp:1835", "field.steps.size() != 1", 'step0.name == "children"'),
    ("core/src/graph.cpp:1744", "step0.wildcard"),
    ("core/src/graph.cpp:2156", "view::segment_alloc(backend, folded_hdr_len(body_len))"),
    ("core/src/graph.cpp:2202", "folded_point_header(hdr_backend, name.size())"),
    ("core/src/graph.cpp:2214", "folded_point_header(hdr_backend, members_len)"),
    ("core/src/graph.cpp:2331", "folded_point_header(hdr_backend, n.body_len)"),
    ("core/src/graph.cpp:2368", "return read_children_folded(vh);"),
    ("core/src/graph.cpp:2364", '"children" && !field.steps[0].wildcard'),
    ("core/src/graph.cpp:2464", "!field.steps[0].wildcard", 'field.steps[0].name == "subscribers"'),
    ("core/src/op_resolve_walk.hpp:353", "enum class index_mode_t"),
    ("core/src/op_resolve_walk.hpp:1014", 'field.steps[0].name != "subscribers"'),
    ("core/include/libtracer/mem_heap.hpp:217", "try_assign"),
    ("core/include/libtracer/view.hpp:26", "namespace tr::view"),
    ("core/include/libtracer/frame.hpp:23", "namespace tr::wire"),
    ("core/include/libtracer/graph.hpp:49", "namespace tr::graph"),
    ("core/include/libtracer/transport.hpp:31", "namespace tr::net"),
    ("core/include/libtracer/backend.hpp:40", "enum class io_dir_t"),
    ("core/include/libtracer/backend.hpp:101", "class mem_backend_t"),
    ("core/include/libtracer/backend.hpp:145", "before_io"),
    ("core/include/libtracer/grammar.hpp:290", "receiver-resource depth bound"),
    # CONTEXT.md quotes the AMENDED meaning of `nesting_too_deep` twice. Its citation was
    # `:210-216` — right until `24ea6d5` inserted the PATH_REF codec above and shifted the
    # whole block +10, after which it landed on `walk_frame_t` and no doc pinned it.
    ('core/include/libtracer/grammar.hpp:300',
     '`TLV_NESTING_TOO_DEEP` ("exceeds this receiver'),
    ("core/src/graph.cpp:1183", "!arena"),
    ("core/include/libtracer/segment.hpp:78", "struct segment_t"),
    # --- the design + module pages (#728). Every one of these had drifted. ---
    ("core/src/graph.cpp:872", "has_registered_child()"),
    ("core/src/graph.cpp:962", "void graph_t::fan_out"),
    ("core/src/graph.cpp:1092", "graph_t::write_impl"),
    ("core/src/graph.cpp:1166", "value.materialize(*value_backend_)", "graph_t::write_branch"),
    ("core/src/graph.cpp:1167", "head.empty() && value.total_length()", "graph_t::write_branch"),
    ("core/src/graph.cpp:1179", "std::array<std::byte, 4096> stack;"),
    ("core/src/graph.cpp:1180", "bump_source_t src(stack"),
    ("core/src/graph.cpp:1182", "decode_into(head.bytes(), src)"),
    ("core/src/graph.cpp:1198", "std::vector<std::byte> root_key;"),
    ("core/src/graph.cpp:1199", "try_build_key(v, root_key)"),
    ("core/src/graph.cpp:1201", "try_assign(parse_key, root_key)"),
    ("core/src/graph.cpp:1403", "value.materialize(*value_backend_)", "field_write read it back"),
    ("core/src/graph.cpp:1698", "result_t<void> graph_t::field_write"),
    ("core/src/graph.cpp:1837", "acl_right_t::CREATE", 'step0.name == "children"'),
    ("core/src/fwd_router.cpp:1760", "fwd_router_t::deliver_remote"),
    ("core/src/fwd_router.cpp:1792", "value.materialize(*flat_)"),
    ("core/src/fwd_router.cpp:1793", "flatten OOM"),
    ("core/src/fwd_router.cpp:1797", "try_encode_compact", "fwd_router_t::deliver_remote"),
    ("core/src/fwd_router.cpp:1829", "std::vector<std::span<const std::byte>> iov;", "fwd_router_t::deliver_remote"),
    # #730 — the two INGRESS flatten guards. Anchored because the whole point of the
    # seam is that these are testable; a citation to them silently rotting would be the
    # first step back to "the guard nobody can prove still works".
    ("core/src/fwd_router.cpp:1426", "route_flat.empty()"),
    ("core/src/fwd_router.cpp:1446", "payload_flat.empty()"),
    ("core/src/fwd_router.cpp:1439", "frame.subrope(head->child1_off, head->child1_total).materialize", "case type_t::COMPACT"),
    ("core/src/fwd_router.cpp:1038", "frame.subrope(0, frame.total_length()).materialize"),
    # #766/#793 — the terminus resolver's three rope-tier draws, and the two allocations the
    # seam docs name as NOT covered by `flat`. These were cited by four doc pages and anchored
    # by none, so #793's own edits to `op_resolve_view.cpp` shifted every one of them without
    # the gate noticing — the exact rot class this file exists for.
    ("core/src/op_resolve_view.cpp:136", "sub.flatten(flat)"),
    # #801 — the SPAN tier's ownership copy, cited by allocation-and-backpressure.md.
    ("core/src/op_resolve_walk.hpp:210", "view_t own_wire(mem::mem_backend_t& flat)"),
    ("core/src/op_resolve_view.cpp:146", "over_bytes(sub.only().bytes(), flat)"),
    ("core/src/op_resolve_view.cpp:254", "wire().materialize(backend())"),
    ("core/src/op_resolve_walk.hpp:600", "view::segment_alloc(egress, head_len)"),
    ("core/src/op_resolve_walk.hpp:703", "rope_t or_backpressure"),
    ("core/src/fwd_router.cpp:1344", "decode_into(frame, rx_for(inbound_ctx))"),
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
    # bench/bench_lkv_slot.cpp
    ('bench/bench_lkv_slot.cpp:193', 'class model_sp_atomic_t {'),
    ('bench/bench_lkv_slot.cpp:342', 'class model_hazard_t {'),
    ('bench/bench_lkv_slot.cpp:456', 'class model_hazard_ref_t {'),
    # core/examples/wire_codec.cpp
    ('core/examples/wire_codec.cpp:71', 'std::printf("encoded POINT{VALUE,VALUE}+CRC: %zu bytes\\n", wire.size());'),
    ('core/examples/wire_codec.cpp:94', 'constexpr int kIters = 50000;'),
    # core/include/libtracer/backend.hpp
    ('core/include/libtracer/backend.hpp:58', "* @brief The address space a backend's bytes live in."),
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
    ('core/include/libtracer/config.hpp.in:86',
     'static constexpr std::size_t kVertexLockStripes = @LIBTRACER_VERTEX_LOCK_STRIPES@;'),
    ('core/include/libtracer/config.hpp.in:109',
     'static constexpr std::size_t kCacheLineBytes = @LIBTRACER_CACHE_LINE_BYTES@;'),
    ('core/include/libtracer/config.hpp.in:136', 'static constexpr std::size_t kHazardReaderSlots = @LIBTRACER_HAZARD_READER_SLOTS@;'),
    ('core/include/libtracer/config.hpp.in:176', 'static constexpr std::size_t kMaxVertexBytes64 = 120;'),
    ('core/include/libtracer/config.hpp.in:212', 'static constexpr std::uint32_t kPinPayloadRatio = 0;'),
    ('core/include/libtracer/config.hpp.in:221', 'using acl_policy_t = @LIBTRACER_ACL_POLICY@;'),
    ('core/include/libtracer/config.hpp.in:188', 'static constexpr std::size_t kMaxVertexBytes32 = 80;'),
    ('core/include/libtracer/config.hpp.in:234',
     '* `-DLIBTRACER_LKV_SLOT=<type>`. The named type must satisfy the policy contract in'),
    ('core/include/libtracer/config.hpp.in:237', 'using lkv_slot_t = @LIBTRACER_LKV_SLOT@;'),
    ('core/include/libtracer/config.hpp.in:246', 'using config_t = default_config_t;'),
    ('core/include/libtracer/config.hpp.in:248',
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
    ('core/include/libtracer/frame.hpp:142',
     '[[nodiscard]] inline std::expected<tlv_t, err_t> decode(const view::view_t& v) {'),
    # core/include/libtracer/fwd_frame_view.hpp
    # core/include/libtracer/fwd_router.hpp
    ('core/include/libtracer/fwd_router.hpp:98',
     "* @param flat  The byte backend EVERY rope flatten on the router's forward AND terminus"),
    ('core/include/libtracer/fwd_router.hpp:178',
     'std::pmr::memory_resource* mr = std::pmr::get_default_resource(),'),
    ('core/include/libtracer/fwd_router.hpp:179', 'mem::block_source_t* rx = &mem::heap_source(),'),
    ('core/include/libtracer/fwd_router.hpp:180', 'mem::mem_backend_t* flat = &mem::heap_backend(),'),
    ('core/include/libtracer/fwd_router.hpp:421',
     "* Invoked (with the `FWD{REPLY}` frame as a @ref view::rope_t) when a REPLY's first"),
    ('core/include/libtracer/fwd_router.hpp:943',
     '[[nodiscard]] mem::block_source_t& rx_for(const child_rx_ctx_t* ctx) const noexcept {'),
    # core/include/libtracer/grammar.hpp
    ('core/include/libtracer/grammar.hpp:388',
     '* call stack, docs/reference/01 §Iterative parsing requirement): the walk keeps'),
    # core/include/libtracer/graph.hpp
    ('core/include/libtracer/graph.hpp:84',
     '// There is no in-process dispatch-depth cap: a SUBSCRIBER delivery TERMINATES at its'),
    ('core/include/libtracer/graph.hpp:281',
     '* @param ctl The #551 nothrow seam every FAILABLE allocation draws from — the ones a'),
    ('core/include/libtracer/graph.hpp:294',
     'explicit graph_t(std::pmr::memory_resource* mr = std::pmr::get_default_resource(),'),
    ('core/include/libtracer/graph.hpp:307',
     '[[nodiscard]] mem::block_source_t& control_source() const noexcept { return *ctl_; }'),
    ('core/include/libtracer/graph.hpp:362',
     '* already-retired or unregistered vertex succeeds and does nothing. The root cannot be'),
    ('core/include/libtracer/graph.hpp:770',
     '[[nodiscard]] result_t<value_ref_t> read(vertex_handle_t v, std::string_view caller = {}) const;'),
('core/include/libtracer/graph.hpp:857',
     '[[nodiscard]] result_t<value_ref_t> await(vertex_handle_t v, std::chrono::nanoseconds timeout,'),
    ('core/include/libtracer/graph.hpp:944', '[[nodiscard]] result_t<rope_t> read_subtree_folded(vertex_handle_t v,'),
    ('core/include/libtracer/graph.hpp:1000', 'template <typename F>'),
    ('core/include/libtracer/graph.hpp:1283', 'struct delivery_drops_t {'),
    ('core/include/libtracer/graph.hpp:1285', 'std::uint64_t no_target = 0;'),
    ('core/include/libtracer/graph.hpp:1287', 'std::uint64_t denied = 0;'),
    ('core/include/libtracer/graph.hpp:1290', 'std::uint64_t out_of_memory = 0;'),
    ('core/include/libtracer/graph.hpp:1294', 'std::uint64_t fan_out_truncated = 0;'),
    ('core/include/libtracer/graph.hpp:1305', '[[nodiscard]] delivery_drops_t delivery_drops() const noexcept;'),
    ('core/include/libtracer/graph.hpp:1337', 'void fan_out(vertex_t* v, const rope_t& value);'),
    ('core/include/libtracer/graph.hpp:1343', 'void dispatch_edge(const edge_view_t& e, const rope_t& value);'),
    ('core/include/libtracer/graph.hpp:1349', 'void dispatch_edge_target(const edge_view_t& e, const rope_t& value);'),
    ('core/include/libtracer/graph.hpp:1350', 'void dispatch_edge_remote(const edge_view_t& e, const rope_t& value);'),
    ('core/include/libtracer/graph.hpp:1367', 'void bubble_up(vertex_t* v, const rope_t& value);'),
    ('core/include/libtracer/graph.hpp:1371', 'void deliver_vertex(vertex_t* v, const rope_t& value);'),
    ('core/include/libtracer/graph.hpp:1543', 'std::pmr::memory_resource* mr_ = std::pmr::get_default_resource();'),
    ('core/include/libtracer/graph.hpp:1552', 'mem::mem_backend_t* value_backend_ = &mem::heap_backend();'),
    ('core/include/libtracer/graph.hpp:1608',
     '*         to migrate. Kept a DIFFERENT type from `mr_` on purpose (see'),
    ('core/include/libtracer/graph.hpp:1611',
     '*         LAST on purpose: no hot path reads it, so declaring it here keeps'),
    ('core/include/libtracer/graph.hpp:1617', 'mem::block_source_t* ctl_ = &mem::heap_source();'),
    # core/include/libtracer/lkv_slot.hpp
    ('core/include/libtracer/lkv_slot.hpp:100', '* **Lock-free BY CONTRACT, and spin-locked in practice.**'),
    ('core/include/libtracer/lkv_slot.hpp:101',
     '* `std::atomic<std::shared_ptr<T>>::is_lock_free()` returns 0 on libstdc++, so both load'),
    # core/include/libtracer/mem_borrowed.hpp
    ('core/include/libtracer/mem_borrowed.hpp:39',
     'void destroy(view::segment_t* seg) noexcept override { delete seg; }  // control block only'),
    # core/include/libtracer/mem_heap.hpp
    ('core/include/libtracer/mem_heap.hpp:338',
     '[[nodiscard]] inline std::optional<view_t> over_bytes(std::span<const std::byte> bytes) noexcept {'),
    # core/include/libtracer/mem_pool.hpp
    ('core/include/libtracer/mem_pool.hpp:170', 'class synchronized_pool_t final : public mem_backend_t {'),
    # core/include/libtracer/mem_source.hpp
    ('core/include/libtracer/mem_source.hpp:138', '[[nodiscard]] block_source_t& heap_source() noexcept;'),
    ('core/include/libtracer/mem_source.hpp:159', '[[nodiscard]] block_source_t& null_source() noexcept;'),
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
    ('core/include/libtracer/mem_source.hpp:184', 'class bump_source_t final : public block_source_t {'),
    ('core/include/libtracer/mem_source.hpp:192',
     '[[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {',
     ': block_source_t("bump"), buf_(buffer), upstream_(&upstream) {}'),
    ('core/include/libtracer/mem_source.hpp:224', 'void reset() noexcept { used_ = 0; }'),
    ('core/include/libtracer/mem_source.hpp:316', 'class pool_source_t final : public block_source_t {'),
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
    ('core/include/libtracer/path.hpp:248', 'static constexpr std::size_t kInlineBytes = 16;'),
    # core/include/libtracer/receiver_slot.hpp
    ('core/include/libtracer/receiver_slot.hpp:138', 'const view::view_t flat = frame.materialize(backend);'),
    # core/include/libtracer/rope.hpp
    ('core/include/libtracer/rope.hpp:56', 'void append(view_t v) {'),
    ('core/include/libtracer/rope.hpp:163',
     '* @brief The single contiguous link — the consumer\'s explicit "this value is'),
    ('core/include/libtracer/rope.hpp:165',
     '* @note Precondition: `link_count() == 1` (debug-asserted). A consumer that'),
    ('core/include/libtracer/rope.hpp:182',
     '[[nodiscard]] view_t materialize(mem::mem_backend_t& backend = mem::heap_backend()) const {'),
    ('core/include/libtracer/rope.hpp:218', '[[nodiscard]] rope_t subrope(std::size_t off, std::size_t len) const {'),
    ('core/include/libtracer/rope.hpp:247',
     '[[nodiscard]] std::vector<std::span<const std::byte>> to_iovec() const {'),
    ('core/include/libtracer/rope.hpp:255',
     '* @brief Nothrow @ref to_iovec — fill @p out with one span per link (no copy),'),
    ('core/include/libtracer/rope.hpp:264',
     '[[nodiscard]] bool try_to_iovec(std::vector<std::span<const std::byte>>& out) const noexcept {'),
    ('core/include/libtracer/rope.hpp:287', 'static constexpr std::size_t kInline = 2;'),
    # core/include/libtracer/rope_decode.hpp
    ('core/include/libtracer/rope_decode.hpp:17',
     '* SINK NOTE: this validates STRUCTURE + CRC over a rope; it does not yet'),
    ('core/include/libtracer/rope_decode.hpp:57', 'class rope_cursor {'),
    # core/include/libtracer/segment.hpp
    ('core/include/libtracer/segment.hpp:21', '#ifndef LIBTRACER_NO_ATOMIC'),
    ('core/include/libtracer/segment.hpp:44', '#ifdef LIBTRACER_NO_ATOMIC'),
    ('core/include/libtracer/segment.hpp:52',
     'void inc_relaxed() noexcept { count_.fetch_add(1, std::memory_order_relaxed); }'),
    ('core/include/libtracer/segment.hpp:54', 'return count_.fetch_sub(1, std::memory_order_acq_rel);'),
    ('core/include/libtracer/segment.hpp:57', 'return count_.load(std::memory_order_acquire);'),
    ('core/include/libtracer/segment.hpp:74',
     '* @note `bytes` is writable at the type level, but whether writes are *legal*'),
    ('core/include/libtracer/segment.hpp:81',
     'std::span<std::byte> bytes; /**< @brief The backing bytes this segment holds a reference to. */'),
    ('core/include/libtracer/segment.hpp:82',
     'mem::mem_space_t space; /**< @brief Address space (HOST/DEVICE), inherited from @ref backend. */'),
    ('core/include/libtracer/segment.hpp:116', '[[nodiscard]] static segment_ptr_t adopt(segment_t* seg) noexcept {'),
    ('core/include/libtracer/segment.hpp:120',
     '[[nodiscard]] static segment_ptr_t retain(segment_t* seg) noexcept {'),
    ('core/include/libtracer/segment.hpp:124',
     '/** @brief Clone — a new shared reference to the same segment (relaxed increment). */'),
    ('core/include/libtracer/segment.hpp:126', 'if (seg_) seg_->refcount.inc_relaxed();'),
    ('core/include/libtracer/segment.hpp:137',
     "/** @brief Drop this reference (acq_rel); fires the backend's `destroy` at zero. */"),
    ('core/include/libtracer/segment.hpp:139', 'if (seg_ && seg_->refcount.dec_acq_rel() == 1) {'),
    ('core/include/libtracer/segment.hpp:154',
     '/** @brief Current refcount — debug / metrics only (acquire load), NOT a sync primitive. */'),
    # core/include/libtracer/status.hpp
    ('core/include/libtracer/status.hpp:25', 'enum class status_t {'),
    # core/include/libtracer/tlv.hpp
    ('core/include/libtracer/tlv.hpp:42', 'ROUTER = 0x0D,      /**< @brief Router-wrapped frame. */'),
    # core/include/libtracer/tlv_arena.hpp
    ('core/include/libtracer/tlv_arena.hpp:8',
     "* span points into the caller's input buffer — the arena holds structure"),
    ('core/include/libtracer/tlv_arena.hpp:30',
     '* @brief One decoded TLV node in a @ref tlv_arena_t (structure only, zero-copy).'),
    ('core/include/libtracer/tlv_arena.hpp:38', 'struct arena_tlv_t {'),
    ('core/include/libtracer/tlv_arena.hpp:115',
     '* NOTHROW end to end (#588). This function is on the wire RX path and reachable'),
    # core/include/libtracer/transport.hpp
    ('core/include/libtracer/transport.hpp:35', 'using peer_id_t = std::array<std::byte, 16>;'),
    ('core/include/libtracer/transport.hpp:65', 'class bus_link_t {'),
    ('core/include/libtracer/transport.hpp:256',
     'virtual void send(std::span<const std::span<const std::byte>> iov) {'),
    ('core/include/libtracer/transport.hpp:373',
     '[[nodiscard]] virtual bool delivers_ropes() const { return false; }',
     'rx_.set_rope([](void* c, view::rope_t f) { (*static_cast<F*>(c))(std::move(f)); }, &sink);'),
    # core/include/libtracer/transport_can.hpp
    ('core/include/libtracer/transport_can.hpp:456', '[[nodiscard]] bus_link_t* bus() override { return this; }'),
    ('core/include/libtracer/transport_can.hpp:475',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }'),
    # core/include/libtracer/transport_quic.hpp
    ('core/include/libtracer/transport_quic.hpp:153',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }'),
    # core/include/libtracer/transport_tcp.hpp
    ('core/include/libtracer/transport_tcp.hpp:151',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }',
     'tcp_transport_t& operator=(const tcp_transport_t&) = delete;'),
    ('core/include/libtracer/transport_tcp.hpp:277',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }',
     'transport_tcp_server& operator=(const transport_tcp_server&) = delete;'),
    # core/include/libtracer/transport_udp.hpp
    ('core/include/libtracer/transport_udp.hpp:55', 'static constexpr std::size_t kMaxDatagram = 65536;'),
    ('core/include/libtracer/transport_udp.hpp:91',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }'),
    # core/include/libtracer/transport_vertex.hpp
    ('core/include/libtracer/transport_vertex.hpp:77',
     'enum class conn_role_t : std::uint8_t { DIAL = 0, LISTEN = 1 };'),
    ('core/include/libtracer/transport_vertex.hpp:120',
     "* §5 leanness ruling): a kind's PRIVATE config (e.g. quic's `cert`/`key` PEM paths) never"),
    ('core/include/libtracer/transport_vertex.hpp:139',
     'std::uint32_t backoff_ms = 0;         /**< @brief DIAL self-heal retry interval (RFC-0014 §4);'),
    ('core/include/libtracer/transport_vertex.hpp:142',
     'std::uint32_t connect_timeout_ms = 0; /**< @brief DIAL connect-attempt deadline (RFC-0014 §4):'),
    # core/include/libtracer/transport_webtransport.hpp
    ('core/include/libtracer/transport_webtransport.hpp:158',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }'),
    # core/include/libtracer/transport_ws.hpp
    ('core/include/libtracer/transport_ws.hpp:217',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }',
     'void send(std::span<const std::span<const std::byte>> iov) override;'),
    ('core/include/libtracer/transport_ws.hpp:439',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }',
     'transport_ws_client& operator=(const transport_ws_client&) = delete;'),
    # core/include/libtracer/vertex.hpp
    ('core/include/libtracer/vertex.hpp:165',
     "* Holding one keeps that value alive, exactly as the reader's own reference did before. Under"),
    ('core/include/libtracer/vertex.hpp:170', 'class value_ref_t {'),
    ('core/include/libtracer/vertex.hpp:597', 'using subscriber_fn_t = void (*)(void* ctx, const rope_t& value);'),
    ('core/include/libtracer/vertex.hpp:784', 'class edge_snapshot_t {'),
    ('core/include/libtracer/vertex.hpp:787', 'static constexpr std::size_t kCapacity = 8;'),
    ('core/include/libtracer/vertex.hpp:997',
     '// kVertexLockStripes and kCacheLineBytes are ordinary constexprs shared by every TU'),
    ('core/include/libtracer/vertex.hpp:1041', 'static_assert(alignof(vertex_stripe_t) == kStripeAlign,'),
    ('core/include/libtracer/vertex.hpp:1057',
     'inline constinit std::array<vertex_stripe_t, kVertexLockStripes> vertex_stripes{};'),
    ('core/include/libtracer/vertex.hpp:1065', 'static std::array<vertex_stripe_t, kVertexLockStripes> stripes{};'),
    ('core/include/libtracer/vertex.hpp:1075', 'return (h >> 6) % kVertexLockStripes;'),
    ('core/include/libtracer/vertex.hpp:1079', 'inline vertex_stripe_t& vertex_stripe_of(const void* v) noexcept {'),
    ('core/include/libtracer/vertex.hpp:1232',
     'static constexpr std::size_t kInlineFanout = edge_snapshot_t::kCapacity;'),
    ('core/include/libtracer/vertex.hpp:1411', '[[nodiscard]] bool has_registered_child() const noexcept {'),
    ('core/include/libtracer/vertex.hpp:1950', 'struct snapshot_drops_t {'),
    ('core/include/libtracer/vertex.hpp:2000',
     'std::size_t snapshot_edges(edge_snapshot_t& inline_buf, std::vector<edge_view_t>& overflow,'),
    ('core/include/libtracer/vertex.hpp:2732', 'const bool use_heap ='),
    ('core/include/libtracer/vertex.hpp:2738',
     '// OOM fallback (reserve failed on a wide list): the inline prefix delivers,'),
    ('core/include/libtracer/vertex.hpp:2744',
     'if (src[i].active.load(std::memory_order_acquire)) ++drops.truncated;'),
    ('core/include/libtracer/vertex.hpp:2749', '++drops.out_of_memory;'),
    ('core/include/libtracer/vertex.hpp:3048',
     'static_assert(sizeof(void*) != 8 || sizeof(vertex_t) <= config_t::kMaxVertexBytes64,'),
    ('core/include/libtracer/vertex.hpp:3051',
     'static_assert(sizeof(void*) != 4 || sizeof(vertex_t) <= config_t::kMaxVertexBytes32,'),
    # core/src/frame.cpp
    ('core/src/frame.cpp:119', 'std::array<grammar::walk_frame_t<grammar::span_cursor>, 8> slots;'),
    ('core/src/frame.cpp:120', 'grammar::walk_stack_t<grammar::span_cursor> stack(slots, &mem::heap_source());'),
    # core/src/fwd_router.cpp
    ('core/src/fwd_router.cpp:522',
     'bool fwd_router_t::add_child(std::string name, transport_t& link, mem::block_source_t* rx) {'),
    ('core/src/fwd_router.cpp:987',
     'void fwd_router_t::on_frame_rope_impl(std::string_view inbound_name, view::rope_t frame,'),
    ('core/src/fwd_router.cpp:993', 'if (frame.link_count() == 1) {'),
    ('core/src/fwd_router.cpp:1056', '// A REPLY that reaches its originator here is handed to the sink rope-native'),
    ('core/src/fwd_router.cpp:1392',
     'void fwd_router_t::on_control_rope(std::string_view inbound_name, view::rope_t frame) {'),
    ('core/src/fwd_router.cpp:1405', 'const auto head = peek_control(cur, wire::grammar::crc_check_t::VERIFY);'),
    ('core/src/fwd_router.cpp:1418', 'const view_t route_flat ='),
    ('core/src/fwd_router.cpp:1419',
     'frame.subrope(head->child1_off, head->child1_total).materialize(*flat_);',
     'void fwd_router_t::resolve_terminus_rope(std::string_view inbound_name, view::rope_t frame) {'),
    ('core/src/fwd_router.cpp:1775', "// dropped fresh ADVERTISE self-heals via the peer's HANDLE_NACK (§E.1)."),
    ('core/src/fwd_router.cpp:1813',
     'constexpr std::array<std::byte, 5> op_tlv{std::byte{0x01}, std::byte{0x00}, std::byte{0x01},'),
    # core/src/graph.cpp
    ('core/src/graph.cpp:163', 'const view_t& frame_view, std::vector<std::byte> key,'),
    ('core/src/graph.cpp:634', 'graph_t::delivery_drops_t graph_t::delivery_drops() const noexcept {'),
    ('core/src/graph.cpp:641', 'void graph_t::count_drop(drop_reason_t why, std::uint64_t n) noexcept {'),
    ('core/src/graph.cpp:661',
     'void graph_t::count_snapshot_drops(const vertex_t::snapshot_drops_t& drops) noexcept {'),
    ('core/src/graph.cpp:897', '[[nodiscard]] bool try_clone_rope(rope_t& dst, const rope_t& src) noexcept {'),
    ('core/src/graph.cpp:910', 'if (target == nullptr) {'),
    ('core/src/graph.cpp:917', 'if (!acl_allows(target, e.caller, acl_right_t::WRITE)) {'),
    ('core/src/graph.cpp:921', '// Delivery TERMINATES at the target (ADR-0051 / RFC-0007): apply exactly the'),
    ('core/src/graph.cpp:929', 'if (!try_clone_rope(clone, value)) {'),
    ('core/src/graph.cpp:952', 'inline void graph_t::dispatch_edge(const edge_view_t& e, const rope_t& value) {'),
    ('core/src/graph.cpp:995',
     '// snapshot_edges re-checks the width under the lock, so a race on the count only costs a'),
    ('core/src/graph.cpp:1002', 'static thread_local std::vector<edge_view_t> tls_buf;'),
    ('core/src/graph.cpp:1012',
     'const std::size_t n = v->snapshot_edges(inline_buf, tls_buf, drops);'),
    ('core/src/graph.cpp:1030',
     'const std::size_t n = v->snapshot_edges(inline_buf, heap_buf, drops);'),
    ('core/src/graph.cpp:1038',
     'result_t<std::shared_ptr<const rope_t>> graph_t::store_value(vertex_t* v, rope_t value) {'),
    ('core/src/graph.cpp:1059', 'void graph_t::bubble_up(vertex_t* v, const rope_t& value) {'),
    ('core/src/graph.cpp:1101', '// A handler stores no LKV (the user handler consumes the value), so the'),
    ('core/src/graph.cpp:1121', 'count_drop(drop_reason_t::OUT_OF_MEMORY, v->own_subs());'),
    ('core/src/graph.cpp:1129', '// Deliver the just-appended ring entry and advance the drain cursor, so a later'),
    ('core/src/graph.cpp:1134', '// no notify reclone of the rope on the hot write path.'),
    ('core/src/graph.cpp:1155',
     'result_t<void> graph_t::write_branch(vertex_t* v, const rope_t& value, std::string_view caller,'),
    ('core/src/graph.cpp:1176',
     "// The overflow leg draws from the graph's injected control seam, not the global heap:"),
    ('core/src/graph.cpp:1272', 'if (v->listeners_above() > 0) bubble_up(v, value);', 'fan_out(v, value);'),
    ('core/src/graph.cpp:1390',
     'result_t<void> graph_t::write(vertex_handle_t v, rope_t value, std::string_view caller) {'),
    ('core/src/graph.cpp:1892', 'result_t<void> graph_t::create_child(vertex_t* parent, const view_t& spec_value) {'),
    ('core/src/graph.cpp:2495', 'result_t<void> graph_t::write(const path_t& path, rope_t value) {'),
    # core/src/op_resolve_walk.hpp
    ('core/src/op_resolve_walk.hpp:76', "*        the u16 the kind=ERROR reply's ERROR{VALUE} identity carries."),
    ('core/src/op_resolve_walk.hpp:542',
     'void tlv_sliced(std::span<const std::byte> wire) {  // trailer-sliced whole-TLV copy (§4)'),
    ('core/src/op_resolve_walk.hpp:609', 'out.tlv_sliced(reply_dst_wire);'),
    ('core/src/op_resolve_walk.hpp:992',
     'if (!req.src.spans_intact()) return std::unexpected(status_t::BACKPRESSURE);'),
    ('core/src/op_resolve_walk.hpp:1079', 'if (!req.dst.spans_intact()) return reply_error(status_t::BACKPRESSURE);'),
    ('core/src/op_resolve_walk.hpp:912', 'if (value.total_length() == 0)'),
    # core/src/path.cpp
    # core/src/posix_endpoint.cpp
    ('core/src/posix_endpoint.cpp:218',
     'void stream_endpoint_t::write_all_iov(int fd, ::iovec* vec, std::size_t count) {'),
    ('core/src/posix_endpoint.cpp:136', 'return ::sendmsg(fd, msg, MSG_NOSIGNAL);'),
    # core/src/rope.cpp
    ('core/src/rope.cpp:15', 'if (!all_host()) return view_t{};'),
    ('core/src/rope.cpp:22', 'if (!b.empty()) std::memcpy(seg->bytes.data() + pos, b.data(), b.size());'),
    # core/src/rope_decode.cpp
    ('core/src/rope_decode.cpp:32', 'std::expected<void, err_t> check_frame(const view::rope_t& r) {'),
    ('core/src/rope_decode.cpp:46', 'std::expected<void, err_t> validate_rope(const view::rope_t& r) {'),
    # core/src/tlv_arena.cpp
    ('core/src/tlv_arena.cpp:130', 'std::array<grammar::walk_frame_t<grammar::span_cursor>, 8> slots;'),
    ('core/src/tlv_arena.cpp:131', 'grammar::walk_stack_t<grammar::span_cursor> stack(slots, &src);'),
    # core/src/transport_tcp.cpp
    ('core/src/transport_tcp.cpp:59',
     '*        MEASURED (`bench_transport_iov`): the fallback fires at exactly **17'),
    ('core/src/transport_tcp.cpp:62',
     "*        `bench_forward_heap`'s `allocs=0` gate cannot see it: that bench drives"),
    ('core/src/transport_tcp.cpp:181', 'bool tcp_transport_t::read_exact(int fd, std::byte* dst, std::size_t len) {'),
    ('core/src/transport_tcp.cpp:201', 'std::array<std::byte, 4096> scratch;'),
    # zero-copy-and-flatten.md quotes this comment's tail verbatim, so the anchor carries the
    # QUOTED line — pinning `serve()`'s signature two constructs up passed while the citation
    # pointed at code the doc never quotes.
    ('core/src/transport_tcp.cpp:223',
     '// buffer, no copy; feeding recv chunks through feed() would add one).'),
    ('core/src/transport_tcp.cpp:243', 'if (!read_exact(fd, seg->bytes.data(), len)) return;'),
    ('core/src/transport_tcp.cpp:457', 'std::array<std::byte, 4096> chunk;',
     'void transport_tcp_server::service_peer(session_t& s) {'),
    # core/src/transport_udp.cpp
    ('core/src/transport_udp.cpp:138',
     'const std::size_t rx_cap = std::min(kMaxDatagram, backend_->max_segment_size());'),
    # core/src/transport_vertex.cpp
    ('core/src/transport_vertex.cpp:53',
     '*        NAME <utf8>, NAME "kind" NAME <utf8>, NAME "port" VALUE u16, NAME "role" VALUE u8'),
    ('core/src/transport_vertex.cpp:74', '[[nodiscard]] view_t link_state_value(link_state_t state) {'),
    ('core/src/transport_vertex.cpp:103',
     'graph_.register_child_type(',
     'return make_connection(std::move(key), config, conn_role_t::DIAL);'),
    ('core/src/transport_vertex.cpp:149',
     'result_t<std::string> transport_vertex_t::module_for(std::string_view kind,'),
    ('core/src/transport_vertex.cpp:203', 'std::string staged_key;'),
    ('core/src/transport_vertex.cpp:240',
     '// Compose the mount key: `<net_root>/<module>/<name>`, replacing the flat key the'),
    # core/src/transport_ws.cpp
    ('core/src/transport_ws.cpp:86',
     'assemble_result_t on_data(ws::opcode_t op, bool fin, std::span<const std::byte> payload,'),
    ('core/src/transport_ws.cpp:100',
     'const std::optional<tr::view::view_t> link = tr::view::over_bytes(payload, backend);'),
    ('core/src/transport_ws.cpp:150', 'constexpr std::size_t kMaxServerIov = kMaxInlineIov;'),
    ('core/src/transport_ws.cpp:272', '// no flatten, no re-copy (server frames are UNMASKED, RFC 6455 §5.1). Lock'),
    ('core/src/transport_ws.cpp:280',
     'std::array<::iovec, kMaxServerIov + 1> inline_vec;',
     'listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);'),
    ('core/src/transport_ws.cpp:406', 'std::array<std::byte, 4096> chunk;',
     'void transport_ws_server::service_peer(session_t& s) {'),
    ('core/src/transport_ws.cpp:793', 'std::array<std::byte, 4096> chunk;',
     'void transport_ws_client::serve(int fd, std::vector<std::byte> pipelined) {'),
    # core/tests/registry_teardown_test.cpp
    ('core/tests/registry_teardown_test.cpp:289', 'void test_digest_paths_agree() {'),
    # core/tests/tlv_arena_test.cpp
    ('core/tests/tlv_arena_test.cpp:293', 'const std::vector<std::byte> deep_bytes = encode(nested(100));'),
    # integrations/esp-idf/libtracer/httpd_ws_link.cpp
    ('integrations/esp-idf/libtracer/httpd_ws_link.cpp:69',
     '* reply, and (the deep path) the whole /unit batch-apply transaction. The device'),
    ('integrations/esp-idf/libtracer/httpd_ws_link.cpp:76', 'constexpr std::size_t kHttpdTaskStack = 12288;'),
    ('integrations/esp-idf/libtracer/httpd_ws_link.cpp:294', 'if (chunk.empty()) return true;'),
    ('integrations/esp-idf/libtracer/httpd_ws_link.cpp:300',
     'if (len_ != 0) std::memcpy(grown.get(), bytes_.get(), len_);'),
    # integrations/esp-idf/libtracer/include/libtracer_esp/httpd_ws_link.hpp
    ('integrations/esp-idf/libtracer/include/libtracer_esp/httpd_ws_link.hpp:48',
     '*     apply overflows the 4 KB httpd default — see kHttpdTaskStack).'),

    # --- re-added from the v0.7.1 docs sweep (absent from main's table) ---
    ('core/include/libtracer/view_can.hpp:100', 'out.frames_.push_back(payload.subview(off, n));'),
    ('core/include/libtracer/fwd_frame_view.hpp:846', 'inline constexpr std::size_t kFwdMaxIov = 9;'),
    ('core/include/libtracer/graph.hpp:980', '* @param ctx Caller-owned context; must outlive every possible delivery (edges are'),
    ('core/include/libtracer/graph.hpp:1203', '[[nodiscard]] result_t<value_ref_t> read(const path_t& path) const;'),
    ('core/include/libtracer/graph.hpp:1209', '[[nodiscard]] result_t<value_ref_t> await(const path_t& path, std::chrono::nanoseconds timeout);'),
    ('core/include/libtracer/mem_heap.hpp:157', '[[nodiscard]] inline bool try_grow(std::size_t bytes, F&& grow) noexcept {'),
    ('core/include/libtracer/mem_heap.hpp:183', '[[nodiscard]] bool try_reserve(std::vector<T>& v, std::size_t n) noexcept {'),
    ('core/include/libtracer/mem_heap.hpp:375', '[[nodiscard]] inline std::optional<view_t> over_bytes(std::span<const std::byte> bytes,'),
    ('core/include/libtracer/path.hpp:156', 'explicit path_t(std::string_view text);'),
    ('core/include/libtracer/transport_tcp.hpp:284', '[[nodiscard]] bus_link_t* bus() override { return peer_named_ ? this : nullptr; }'),
    ('core/include/libtracer/transport_ws.hpp:233', '[[nodiscard]] bus_link_t* bus() override { return peer_named_ ? this : nullptr; }'),
    ('core/include/libtracer/edge_pin.hpp:153', 'class pin_t {'),
    ('core/src/fwd_router.cpp:629', 'link.set_rope_receiver('),
    ('core/src/fwd_router.cpp:583', 'bus->set_peer_rope_receiver('),
    ('core/src/graph.cpp:689', 'vertex_t* graph_t::find_ptr(std::span<const std::byte> key) const {'),
    ('core/src/graph.cpp:690', 'const std::shared_lock lock(map_mutex_);'),
    ('core/src/graph.cpp:1683', 's.target_key.reset();'),
    ('core/src/path.cpp:96', 'if (!valid_segment(seg)) return std::unexpected(status_t::INVALID_PATH);'),
    ('core/src/path.cpp:112', 'if (step.empty()) return std::unexpected(status_t::INVALID_PATH);'),
    ('core/src/route_handle.cpp:82', 't.ingress.push_back(ingress_entry_t{.label = label, .binding = std::move(binding)});'),
    ('core/src/route_handle.cpp:179', 't->egress.push_back(egress_entry_t{', 'bool route_handle_t::record_egress(std::string_view out_link, std::uint16_t label,'),
    ('core/src/route_handle.cpp:236', 't->egress.push_back(egress_entry_t{', 'std::pair<std::uint16_t, bool> route_handle_t::ensure_egress(std::string_view out_link,'),
    ('core/src/transport_can.cpp:313', 'tr::view::view_can_frames_t::split(*payload, cfg_.mode);'),
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
_EXTS = "|".join(re.escape(s[1:]) for s in SOURCE_SUFFIXES)
DOC_EXTS = "md|rst"
CITATION_RE = re.compile(
    r"`?((?:[A-Za-z0-9_./-]*/)?[A-Za-z0-9_][A-Za-z0-9_.-]*\.(?:" + _EXTS + r")):([\d,\-]+)`?"
    r"|((?:[A-Za-z0-9_./-]*/)?[A-Za-z0-9_][A-Za-z0-9_.-]*\.(?:" + DOC_EXTS + r")):[\d,\-]+"
    r"|`:([\d,\-]+)`"
)


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
            last, spec = resolved, m.group(2)
        elif m.group(3):
            last = None  # a non-source citation ends the inheritance run
            continue
        elif last:
            spec = m.group(4)
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
            spans.append((last, lo, hi))
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
            last = None  # a non-source citation ends the inheritance run
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
    """
    root = root or REPO
    git = ["git", "-C", str(root)]
    changed = subprocess.run(git + ["diff", "--name-only", rev, "--"],
                             capture_output=True, text=True, check=True).stdout.split("\n")
    maps, notes = {}, []
    for rel in (c.strip() for c in changed if c.strip()):
        if not rel.endswith(SOURCE_SUFFIXES) or any(
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
    total, historical, held_all = 0, 0, list(notes)
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
        held_all += [f"{rel}: {p}:{n} — no derivable shift, re-pin by hand" for p, n in held]
        if not moves:
            continue
        total += len(moves)
        for p, old, new in moves:
            print(f"REPIN {rel}: {p}:{old} -> {new}")
        if apply:
            path.write_text(new_text)
    for note in dict.fromkeys(held_all):
        print(f"HOLD  {note}")
    verb = "rewritten" if apply else "would move (dry run; pass --apply to write)"
    print(f"\n{total} citation(s) {verb}; {len(dict.fromkeys(held_all))} held for a human.")
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
