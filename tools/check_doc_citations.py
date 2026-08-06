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

Usage:  python3 tools/check_doc_citations.py
Exits non-zero on the first stale citation, listing every one it found.
Gated by `.github/workflows/doc-citations.yml`; unit tests in
`tools/tests/test_check_doc_citations.py`.
"""

import functools
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

# Extensions a citation may name. `.hpp.in` is the CMake-configured header template
# (`config.hpp.in`) — the only place the compile-time knobs are *declared*, and what
# the configuration pages therefore cite; the generated `config.hpp` is a build
# artifact and is not in the tree.
SOURCE_SUFFIXES = (".hpp.in", ".hpp", ".cpp", ".cc", ".hh", ".h")

# Directories that hold no citable source: build output, vendored deps, worktrees.
NON_SOURCE_DIRS = ("_build", "build", "node_modules", ".claude", ".git", "target", "dist")

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
    ("core/src/graph.cpp:1930", "graph_t::set_identity"),
    ("core/src/graph.cpp:1956", "graph_t::read_identity"),
    ("core/src/graph.cpp:2332", 'field.steps[0].name == "identity"'),
    ("core/src/transport_vertex.cpp:64", 'cfg.name("kind")'),
    ("core/src/transport_vertex.cpp:99", "register_child_type"),
    ("core/src/transport_vertex.cpp:128", "register_transport_type"),
    ("core/src/transport_vertex.cpp:133", "transport_vertex_t::register_module"),
    ("core/src/transport_vertex.cpp:157", "SCHEMA_NOT_FOUND", "transport_vertex_t::module_for"),
    # fwd-router.md's "Signature source" line — bare :NNN shorthands that had ALL rotted
    # silently (they cited the pre-#739 header). Anchored so they cannot rot again.
    # zero-copy-and-flatten.md's rope-tier citations and ADR-0072's stale-comment pointer —
    # all four had rotted on main and were re-asserted by a mechanical +24 shift (#768 verify).
    ("core/include/libtracer/fwd_router.hpp:705", "Terminus over a MULTI-LINK rope"),
    ("core/include/libtracer/fwd_router.hpp:711", "64 KB / 2 links"),
    ("core/include/libtracer/fwd_router.hpp:723", "The forward hop, read entirely by OFFSET"),
    ("core/include/libtracer/fwd_router.hpp:570", "Slot addresses are NOT stable"),
    ("core/include/libtracer/fwd_router.hpp:176", "explicit fwd_router_t"),
    ("core/include/libtracer/fwd_router.hpp:238", "bool add_child"),
    ("core/include/libtracer/fwd_router.hpp:385", "using reply_fn_t"),
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
    ("core/src/graph.cpp:1659", "field.steps.size() != 1", 'step0.name == "subscribers"'),
    ("core/src/graph.cpp:1738", "field.steps.size() != 1 || !plain_step(step0)"),
    ("core/src/graph.cpp:1775", "field.steps.size() != 1", 'step0.name == "children"'),
    ("core/src/graph.cpp:1684", "step0.wildcard"),
    ("core/src/graph.cpp:2096", "view::segment_alloc(backend, folded_hdr_len(body_len))"),
    ("core/src/graph.cpp:2142", "folded_point_header(hdr_backend, name.size())"),
    ("core/src/graph.cpp:2154", "folded_point_header(hdr_backend, members_len)"),
    ("core/src/graph.cpp:2265", "folded_point_header(hdr_backend, n.body_len)"),
    ("core/src/graph.cpp:2302", "return read_children_folded(vh);"),
    ("core/src/graph.cpp:2298", '"children" && !field.steps[0].wildcard'),
    ("core/src/graph.cpp:2398", "!field.steps[0].wildcard", 'field.steps[0].name == "subscribers"'),
    ("core/src/op_resolve_walk.hpp:353", "enum class index_mode_t"),
    ("core/src/op_resolve_walk.hpp:1014", 'field.steps[0].name != "subscribers"'),
    ("core/include/libtracer/mem_heap.hpp:149", "try_assign"),
    ("core/include/libtracer/view.hpp:26", "namespace tr::view"),
    ("core/include/libtracer/frame.hpp:23", "namespace tr::wire"),
    ("core/include/libtracer/graph.hpp:47", "namespace tr::graph"),
    ("core/include/libtracer/transport.hpp:31", "namespace tr::net"),
    ("core/include/libtracer/backend.hpp:40", "enum class io_dir_t"),
    ("core/include/libtracer/backend.hpp:101", "class mem_backend_t"),
    ("core/include/libtracer/backend.hpp:145", "before_io"),
    ("core/include/libtracer/grammar.hpp:220", "receiver-resource depth bound"),
    # CONTEXT.md quotes the AMENDED meaning of `nesting_too_deep` twice. Its citation was
    # `:210-216` — right until `24ea6d5` inserted the PATH_REF codec above and shifted the
    # whole block +10, after which it landed on `walk_frame_t` and no doc pinned it.
    ('core/include/libtracer/grammar.hpp:230',
     '`TLV_NESTING_TOO_DEEP` ("exceeds this receiver'),
    ("core/src/graph.cpp:1123", "!arena"),
    ("core/include/libtracer/segment.hpp:78", "struct segment_t"),
    # --- the design + module pages (#728). Every one of these had drifted. ---
    ("core/src/graph.cpp:831", "has_registered_child()"),
    ("core/src/graph.cpp:921", "void graph_t::fan_out"),
    ("core/src/graph.cpp:1044", "graph_t::write_impl"),
    ("core/src/graph.cpp:1106", "value.materialize(*value_backend_)", "graph_t::write_branch"),
    ("core/src/graph.cpp:1107", "head.empty() && value.total_length()", "graph_t::write_branch"),
    ("core/src/graph.cpp:1119", "std::array<std::byte, 4096> stack;"),
    ("core/src/graph.cpp:1120", "bump_source_t src(stack"),
    ("core/src/graph.cpp:1122", "decode_into(head.bytes(), src)"),
    ("core/src/graph.cpp:1138", "std::vector<std::byte> root_key;"),
    ("core/src/graph.cpp:1139", "try_build_key(v, root_key)"),
    ("core/src/graph.cpp:1141", "try_assign(parse_key, root_key)"),
    ("core/src/graph.cpp:1343", "value.materialize(*value_backend_)", "field_write read it back"),
    ("core/src/graph.cpp:1638", "result_t<void> graph_t::field_write"),
    ("core/src/graph.cpp:1777", "acl_right_t::CREATE", 'step0.name == "children"'),
    ("core/src/fwd_router.cpp:1672", "fwd_router_t::deliver_remote"),
    ("core/src/fwd_router.cpp:1704", "value.materialize(*flat_)"),
    ("core/src/fwd_router.cpp:1705", "flatten OOM"),
    ("core/src/fwd_router.cpp:1709", "try_encode_compact", "fwd_router_t::deliver_remote"),
    ("core/src/fwd_router.cpp:1741", "std::vector<std::span<const std::byte>> iov;", "fwd_router_t::deliver_remote"),
    # #730 — the two INGRESS flatten guards. Anchored because the whole point of the
    # seam is that these are testable; a citation to them silently rotting would be the
    # first step back to "the guard nobody can prove still works".
    ("core/src/fwd_router.cpp:1341", "route_flat.empty()"),
    ("core/src/fwd_router.cpp:1361", "payload_flat.empty()"),
    ("core/src/fwd_router.cpp:1354", "frame.subrope(head->child1_off, head->child1_total).materialize", "case type_t::COMPACT"),
    ("core/src/fwd_router.cpp:954", "frame.subrope(0, frame.total_length()).materialize"),
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
    ("core/src/fwd_router.cpp:1259", "decode_into(frame, rx_for(inbound_ctx))"),
    ("core/include/libtracer/vertex.hpp:2886", "vertex_t* parent_"),
    ("core/include/libtracer/vertex.hpp:2886", "vertex_t* parent_"),
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
    ('core/include/libtracer/can_reassembly.hpp:181',
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
    ('core/include/libtracer/frame.hpp:134',
     '[[nodiscard]] inline std::expected<tlv_t, err_t> decode(const view::view_t& v) {'),
    # core/include/libtracer/fwd_frame_view.hpp
    # core/include/libtracer/fwd_router.hpp
    ('core/include/libtracer/fwd_router.hpp:97',
     "* @param flat  The byte backend EVERY rope flatten on the router's forward AND terminus"),
    ('core/include/libtracer/fwd_router.hpp:177',
     'std::pmr::memory_resource* mr = std::pmr::get_default_resource(),'),
    ('core/include/libtracer/fwd_router.hpp:178', 'mem::block_source_t* rx = &mem::heap_source(),'),
    ('core/include/libtracer/fwd_router.hpp:179', 'mem::mem_backend_t* flat = &mem::heap_backend(),'),
    ('core/include/libtracer/fwd_router.hpp:401',
     "* Invoked (with the `FWD{REPLY}` frame as a @ref view::rope_t) when a REPLY's first"),
    ('core/include/libtracer/fwd_router.hpp:834',
     '[[nodiscard]] mem::block_source_t& rx_for(const child_rx_ctx_t* ctx) const noexcept {'),
    # core/include/libtracer/grammar.hpp
    ('core/include/libtracer/grammar.hpp:318',
     '* call stack, docs/reference/01 §Iterative parsing requirement): the walk keeps'),
    # core/include/libtracer/graph.hpp
    ('core/include/libtracer/graph.hpp:82',
     '// There is no in-process dispatch-depth cap: a SUBSCRIBER delivery TERMINATES at its'),
    ('core/include/libtracer/graph.hpp:270',
     '* @param ctl The #551 nothrow seam every FAILABLE allocation draws from — the ones a'),
    ('core/include/libtracer/graph.hpp:283',
     'explicit graph_t(std::pmr::memory_resource* mr = std::pmr::get_default_resource(),'),
    ('core/include/libtracer/graph.hpp:296',
     '[[nodiscard]] mem::block_source_t& control_source() const noexcept { return *ctl_; }'),
    ('core/include/libtracer/graph.hpp:351',
     '* already-retired or unregistered vertex succeeds and does nothing. The root cannot be'),
    ('core/include/libtracer/graph.hpp:759',
     '[[nodiscard]] result_t<value_ref_t> read(vertex_handle_t v, std::string_view caller = {}) const;'),
('core/include/libtracer/graph.hpp:846',
     '[[nodiscard]] result_t<value_ref_t> await(vertex_handle_t v, std::chrono::nanoseconds timeout,'),
    ('core/include/libtracer/graph.hpp:933', '[[nodiscard]] result_t<rope_t> read_subtree_folded(vertex_handle_t v,'),
    ('core/include/libtracer/graph.hpp:989', 'template <typename F>'),
    ('core/include/libtracer/graph.hpp:1255', 'struct delivery_drops_t {'),
    ('core/include/libtracer/graph.hpp:1257', 'std::uint64_t no_target = 0;'),
    ('core/include/libtracer/graph.hpp:1259', 'std::uint64_t denied = 0;'),
    ('core/include/libtracer/graph.hpp:1261', 'std::uint64_t out_of_memory = 0;'),
    ('core/include/libtracer/graph.hpp:1272', '[[nodiscard]] delivery_drops_t delivery_drops() const noexcept;'),
    ('core/include/libtracer/graph.hpp:1304', 'void fan_out(vertex_t* v, const rope_t& value);'),
    ('core/include/libtracer/graph.hpp:1310', 'void dispatch_edge(const edge_view_t& e, const rope_t& value);'),
    ('core/include/libtracer/graph.hpp:1316', 'void dispatch_edge_target(const edge_view_t& e, const rope_t& value);'),
    ('core/include/libtracer/graph.hpp:1317', 'void dispatch_edge_remote(const edge_view_t& e, const rope_t& value);'),
    ('core/include/libtracer/graph.hpp:1320', 'void bubble_up(vertex_t* v, const rope_t& value);'),
    ('core/include/libtracer/graph.hpp:1324', 'void deliver_vertex(vertex_t* v, const rope_t& value);'),
    ('core/include/libtracer/graph.hpp:1495', 'std::pmr::memory_resource* mr_ = std::pmr::get_default_resource();'),
    ('core/include/libtracer/graph.hpp:1504', 'mem::mem_backend_t* value_backend_ = &mem::heap_backend();'),
    ('core/include/libtracer/graph.hpp:1559',
     '*         to migrate. Kept a DIFFERENT type from `mr_` on purpose (see'),
    ('core/include/libtracer/graph.hpp:1562',
     '*         LAST on purpose: no hot path reads it, so declaring it here keeps'),
    ('core/include/libtracer/graph.hpp:1568', 'mem::block_source_t* ctl_ = &mem::heap_source();'),
    # core/include/libtracer/lkv_slot.hpp
    ('core/include/libtracer/lkv_slot.hpp:99', '* **Lock-free BY CONTRACT, and spin-locked in practice.**'),
    ('core/include/libtracer/lkv_slot.hpp:100',
     '* `std::atomic<std::shared_ptr<T>>::is_lock_free()` returns 0 on libstdc++, so both load'),
    # core/include/libtracer/mem_borrowed.hpp
    ('core/include/libtracer/mem_borrowed.hpp:39',
     'void destroy(view::segment_t* seg) noexcept override { delete seg; }  // control block only'),
    # core/include/libtracer/mem_heap.hpp
    ('core/include/libtracer/mem_heap.hpp:267',
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
    ('core/include/libtracer/rope.hpp:129',
     '* @brief The single contiguous link — the consumer\'s explicit "this value is'),
    ('core/include/libtracer/rope.hpp:131',
     '* @note Precondition: `link_count() == 1` (debug-asserted). A consumer that'),
    ('core/include/libtracer/rope.hpp:148',
     '[[nodiscard]] view_t materialize(mem::mem_backend_t& backend = mem::heap_backend()) const {'),
    ('core/include/libtracer/rope.hpp:184', '[[nodiscard]] rope_t subrope(std::size_t off, std::size_t len) const {'),
    ('core/include/libtracer/rope.hpp:213',
     '[[nodiscard]] std::vector<std::span<const std::byte>> to_iovec() const {'),
    ('core/include/libtracer/rope.hpp:221',
     '* @brief Nothrow @ref to_iovec — fill @p out with one span per link (no copy),'),
    ('core/include/libtracer/rope.hpp:230',
     '[[nodiscard]] bool try_to_iovec(std::vector<std::span<const std::byte>>& out) const noexcept {'),
    ('core/include/libtracer/rope.hpp:253', 'static constexpr std::size_t kInline = 2;'),
    # core/include/libtracer/rope_decode.hpp
    ('core/include/libtracer/rope_decode.hpp:17',
     '* SINK NOTE: this validates STRUCTURE + CRC over a rope; it does not yet'),
    ('core/include/libtracer/rope_decode.hpp:56', 'class rope_cursor {'),
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
    ('core/include/libtracer/transport.hpp:353',
     '[[nodiscard]] virtual bool delivers_ropes() const { return false; }',
     'rx_.set_rope([](void* c, view::rope_t f) { (*static_cast<F*>(c))(std::move(f)); }, &sink);'),
    # core/include/libtracer/transport_can.hpp
    ('core/include/libtracer/transport_can.hpp:271', '[[nodiscard]] bus_link_t* bus() override { return this; }'),
    ('core/include/libtracer/transport_can.hpp:290',
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
    ('core/include/libtracer/transport_vertex.hpp:138',
     'std::uint32_t backoff_ms = 0;         /**< @brief DIAL self-heal retry interval (RFC-0014 §4);'),
    ('core/include/libtracer/transport_vertex.hpp:141',
     'std::uint32_t connect_timeout_ms = 0; /**< @brief DIAL connect-attempt deadline (RFC-0014 §4):'),
    # core/include/libtracer/transport_webtransport.hpp
    ('core/include/libtracer/transport_webtransport.hpp:156',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }'),
    # core/include/libtracer/transport_ws.hpp
    ('core/include/libtracer/transport_ws.hpp:167',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }',
     'void send(std::span<const std::span<const std::byte>> iov) override;'),
    ('core/include/libtracer/transport_ws.hpp:325',
     '[[nodiscard]] bool delivers_ropes() const override { return true; }',
     'transport_ws_client& operator=(const transport_ws_client&) = delete;'),
    # core/include/libtracer/vertex.hpp
    ('core/include/libtracer/vertex.hpp:142',
     "* Holding one keeps that value alive, exactly as the reader's own reference did before. Under"),
    ('core/include/libtracer/vertex.hpp:147', 'class value_ref_t {'),
    ('core/include/libtracer/vertex.hpp:573', 'using subscriber_fn_t = void (*)(void* ctx, const rope_t& value);'),
    ('core/include/libtracer/vertex.hpp:760', 'class edge_snapshot_t {'),
    ('core/include/libtracer/vertex.hpp:763', 'static constexpr std::size_t kCapacity = 8;'),
    ('core/include/libtracer/vertex.hpp:973',
     '// kVertexLockStripes and kCacheLineBytes are ordinary constexprs shared by every TU'),
    ('core/include/libtracer/vertex.hpp:1017', 'static_assert(alignof(vertex_stripe_t) == kStripeAlign,'),
    ('core/include/libtracer/vertex.hpp:1033',
     'inline constinit std::array<vertex_stripe_t, kVertexLockStripes> vertex_stripes{};'),
    ('core/include/libtracer/vertex.hpp:1041', 'static std::array<vertex_stripe_t, kVertexLockStripes> stripes{};'),
    ('core/include/libtracer/vertex.hpp:1051', 'return (h >> 6) % kVertexLockStripes;'),
    ('core/include/libtracer/vertex.hpp:1055', 'inline vertex_stripe_t& vertex_stripe_of(const void* v) noexcept {'),
    ('core/include/libtracer/vertex.hpp:1199',
     'static constexpr std::size_t kInlineFanout = edge_snapshot_t::kCapacity;'),
    ('core/include/libtracer/vertex.hpp:1378', '[[nodiscard]] bool has_registered_child() const noexcept {'),
    ('core/include/libtracer/vertex.hpp:1934',
     'std::size_t snapshot_edges(edge_snapshot_t& inline_buf, std::vector<edge_view_t>& overflow) {'),
    ('core/include/libtracer/vertex.hpp:2637', 'const bool use_heap ='),
    ('core/include/libtracer/vertex.hpp:2643',
     '// OOM fallback (reserve failed on a wide list): the inline prefix delivers,'),
    ('core/include/libtracer/vertex.hpp:2944',
     'static_assert(sizeof(void*) != 8 || sizeof(vertex_t) <= config_t::kMaxVertexBytes64,'),
    ('core/include/libtracer/vertex.hpp:2947',
     'static_assert(sizeof(void*) != 4 || sizeof(vertex_t) <= config_t::kMaxVertexBytes32,'),
    # core/src/frame.cpp
    ('core/src/frame.cpp:118', 'std::array<grammar::walk_frame_t<grammar::span_cursor>, 8> slots;'),
    ('core/src/frame.cpp:119', 'grammar::walk_stack_t<grammar::span_cursor> stack(slots, &mem::heap_source());'),
    # core/src/fwd_router.cpp
    ('core/src/fwd_router.cpp:522',
     'bool fwd_router_t::add_child(std::string name, transport_t& link, mem::block_source_t* rx) {'),
    ('core/src/fwd_router.cpp:903',
     'void fwd_router_t::on_frame_rope_impl(std::string_view inbound_name, view::rope_t frame,'),
    ('core/src/fwd_router.cpp:909', 'if (frame.link_count() == 1) {'),
    ('core/src/fwd_router.cpp:972', '// A REPLY that reaches its originator here is handed to the sink rope-native'),
    ('core/src/fwd_router.cpp:1307',
     'void fwd_router_t::on_control_rope(std::string_view inbound_name, view::rope_t frame) {'),
    ('core/src/fwd_router.cpp:1320', 'const auto head = peek_control(cur, wire::grammar::crc_check_t::VERIFY);'),
    ('core/src/fwd_router.cpp:1333', 'const view_t route_flat ='),
    ('core/src/fwd_router.cpp:1334',
     'frame.subrope(head->child1_off, head->child1_total).materialize(*flat_);',
     'void fwd_router_t::resolve_terminus_rope(std::string_view inbound_name, view::rope_t frame) {'),
    ('core/src/fwd_router.cpp:1687', "// dropped fresh ADVERTISE self-heals via the peer's HANDLE_NACK (§E.1)."),
    ('core/src/fwd_router.cpp:1725',
     'constexpr std::array<std::byte, 5> op_tlv{std::byte{0x01}, std::byte{0x00}, std::byte{0x01},'),
    # core/src/graph.cpp
    ('core/src/graph.cpp:163', 'const view_t& frame_view, std::vector<std::byte> key,'),
    ('core/src/graph.cpp:634', 'graph_t::delivery_drops_t graph_t::delivery_drops() const noexcept {'),
    ('core/src/graph.cpp:856', '[[nodiscard]] bool try_clone_rope(rope_t& dst, const rope_t& src) noexcept {'),
    ('core/src/graph.cpp:869', 'if (target == nullptr) {'),
    ('core/src/graph.cpp:876', 'if (!acl_allows(target, e.caller, acl_right_t::WRITE)) {'),
    ('core/src/graph.cpp:880', '// Delivery TERMINATES at the target (ADR-0051 / RFC-0007): apply exactly the'),
    ('core/src/graph.cpp:888', 'if (!try_clone_rope(clone, value)) {'),
    ('core/src/graph.cpp:911', 'inline void graph_t::dispatch_edge(const edge_view_t& e, const rope_t& value) {'),
    ('core/src/graph.cpp:954',
     '// snapshot_edges re-checks the width under the lock, so a race on the count only costs a'),
    ('core/src/graph.cpp:961', 'static thread_local std::vector<edge_view_t> tls_buf;'),
    ('core/src/graph.cpp:970', 'const std::size_t n = v->snapshot_edges(inline_buf, tls_buf);'),
    ('core/src/graph.cpp:983', 'const std::size_t n = v->snapshot_edges(inline_buf, heap_buf);'),
    ('core/src/graph.cpp:990',
     'result_t<std::shared_ptr<const rope_t>> graph_t::store_value(vertex_t* v, rope_t value) {'),
    ('core/src/graph.cpp:1011', 'void graph_t::bubble_up(vertex_t* v, const rope_t& value) {'),
    ('core/src/graph.cpp:1053', '// A handler stores no LKV (the user handler consumes the value), so the'),
    ('core/src/graph.cpp:1069', '// Deliver the just-appended ring entry and advance the drain cursor, so a later'),
    ('core/src/graph.cpp:1074', '// no notify reclone of the rope on the hot write path.'),
    ('core/src/graph.cpp:1095',
     'result_t<void> graph_t::write_branch(vertex_t* v, const rope_t& value, std::string_view caller,'),
    ('core/src/graph.cpp:1116',
     "// The overflow leg draws from the graph's injected control seam, not the global heap:"),
    ('core/src/graph.cpp:1212', 'if (v->listeners_above() > 0) bubble_up(v, value);', 'fan_out(v, value);'),
    ('core/src/graph.cpp:1330',
     'result_t<void> graph_t::write(vertex_handle_t v, rope_t value, std::string_view caller) {'),
    ('core/src/graph.cpp:1832', 'result_t<void> graph_t::create_child(vertex_t* parent, const view_t& spec_value) {'),
    ('core/src/graph.cpp:2429', 'result_t<void> graph_t::write(const path_t& path, rope_t value) {'),
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
    ('core/src/posix_endpoint.cpp:111',
     'void stream_endpoint_t::write_all_iov(int fd, ::iovec* vec, std::size_t count) {'),
    ('core/src/posix_endpoint.cpp:117', 'const ssize_t n = ::sendmsg(fd, &msg, MSG_NOSIGNAL);'),
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
    ('core/src/transport_vertex.cpp:193', 'std::string staged_key;'),
    ('core/src/transport_vertex.cpp:230',
     '// Compose the mount key: `<net_root>/<module>/<name>`, replacing the flat key the'),
    # core/src/transport_ws.cpp
    ('core/src/transport_ws.cpp:62', 'std::optional<tr::view::rope_t> on_data(ws::opcode_t op, bool fin,'),
    ('core/src/transport_ws.cpp:67', 'const std::optional<tr::view::view_t> link = tr::view::over_bytes(payload);'),
    ('core/src/transport_ws.cpp:117', 'constexpr std::size_t kMaxServerIov = kMaxInlineIov;'),
    ('core/src/transport_ws.cpp:237', '// no flatten, no re-copy (server frames are UNMASKED, RFC 6455 §5.1). Lock'),
    ('core/src/transport_ws.cpp:245',
     'std::array<::iovec, kMaxServerIov + 1> inline_vec;',
     'listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);'),
    ('core/src/transport_ws.cpp:371', 'std::array<std::byte, 4096> chunk;',
     'void transport_ws_server::service_peer(session_t& s) {'),
    ('core/src/transport_ws.cpp:691', 'std::array<std::byte, 4096> chunk;',
     'void transport_ws_client::serve(int fd) {'),
    # core/tests/registry_teardown_test.cpp
    ('core/tests/registry_teardown_test.cpp:289', 'void test_digest_paths_agree() {'),
    # core/tests/tlv_arena_test.cpp
    ('core/tests/tlv_arena_test.cpp:293', 'const std::vector<std::byte> deep_bytes = encode(nested(100));'),
    # integrations/esp-idf/libtracer/httpd_ws_link.cpp
    ('integrations/esp-idf/libtracer/httpd_ws_link.cpp:69',
     '* reply, and (the deep path) the whole /unit batch-apply transaction. The device'),
    ('integrations/esp-idf/libtracer/httpd_ws_link.cpp:76', 'constexpr std::size_t kHttpdTaskStack = 12288;'),
    ('integrations/esp-idf/libtracer/httpd_ws_link.cpp:272', 'if (chunk.empty()) return true;'),
    ('integrations/esp-idf/libtracer/httpd_ws_link.cpp:278',
     'if (len_ != 0) std::memcpy(grown.get(), bytes_.get(), len_);'),
    # integrations/esp-idf/libtracer/include/libtracer_esp/httpd_ws_link.hpp
    ('integrations/esp-idf/libtracer/include/libtracer_esp/httpd_ws_link.hpp:38',
     '*     apply overflows the 4 KB httpd default — see kHttpdTaskStack).'),

    # --- re-added from the v0.7.1 docs sweep (absent from main's table) ---
    ('core/include/libtracer/view_can.hpp:100', 'out.frames_.push_back(payload.subview(off, n));'),
    ('core/include/libtracer/fwd_frame_view.hpp:846', 'inline constexpr std::size_t kFwdMaxIov = 9;'),
    ('core/include/libtracer/graph.hpp:969', '* @param ctx Caller-owned context; must outlive every possible delivery (edges are'),
    ('core/include/libtracer/graph.hpp:1181', '[[nodiscard]] result_t<value_ref_t> read(const path_t& path) const;'),
    ('core/include/libtracer/graph.hpp:1187', '[[nodiscard]] result_t<value_ref_t> await(const path_t& path, std::chrono::nanoseconds timeout);'),
    ('core/include/libtracer/mem_heap.hpp:116', '[[nodiscard]] bool try_reserve(std::vector<T>& v, std::size_t n) noexcept {'),
    ('core/include/libtracer/mem_heap.hpp:120', 'v.reserve(n);  // nothrow now'),
    ('core/include/libtracer/mem_heap.hpp:304', '[[nodiscard]] inline std::optional<view_t> over_bytes(std::span<const std::byte> bytes,'),
    ('core/include/libtracer/path.hpp:156', 'explicit path_t(std::string_view text);'),
    ('core/include/libtracer/transport_tcp.hpp:284', '[[nodiscard]] bus_link_t* bus() override { return peer_named_ ? this : nullptr; }'),
    ('core/include/libtracer/transport_ws.hpp:183', '[[nodiscard]] bus_link_t* bus() override { return peer_named_ ? this : nullptr; }'),
    ('core/include/libtracer/edge_pin.hpp:153', 'class pin_t {'),
    ('core/src/fwd_router.cpp:626', 'link.set_rope_receiver('),
    ('core/src/fwd_router.cpp:585', 'bus->set_peer_rope_receiver('),
    ('core/src/graph.cpp:663', 'vertex_t* graph_t::find_ptr(std::span<const std::byte> key) const {'),
    ('core/src/graph.cpp:664', 'const std::shared_lock lock(map_mutex_);'),
    ('core/src/graph.cpp:1623', 's.target_key.reset();'),
    ('core/src/path.cpp:96', 'if (!valid_segment(seg)) return std::unexpected(status_t::INVALID_PATH);'),
    ('core/src/path.cpp:112', 'if (step.empty()) return std::unexpected(status_t::INVALID_PATH);'),
    ('core/src/route_handle.cpp:82', 't.ingress.push_back(ingress_entry_t{.label = label, .binding = std::move(binding)});'),
    ('core/src/route_handle.cpp:179', 't->egress.push_back(egress_entry_t{', 'bool route_handle_t::record_egress(std::string_view out_link, std::uint16_t label,'),
    ('core/src/route_handle.cpp:236', 't->egress.push_back(egress_entry_t{', 'std::pair<std::uint16_t, bool> route_handle_t::ensure_egress(std::string_view out_link,'),
    ('core/src/transport_can.cpp:243', 'tr::view::view_can_frames_t::split(*payload, cfg_.mode);'),
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
        if any(d in rel.parts for d in NON_SOURCE_DIRS):
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
    filemap = source_map()
    present, failures, drifted = set(), [], []
    for doc in all_docs():
        try:
            locs, errs = cited_locations(doc.read_text(), filemap)
        except (OSError, UnicodeDecodeError):
            continue
        present |= locs
        rel = doc.relative_to(REPO).as_posix()
        failures += [f"{rel}: {e}" for e in dict.fromkeys(errs)]

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
