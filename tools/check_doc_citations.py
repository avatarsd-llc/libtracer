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
not a guess — the doc must spell the full path. Generated headers (`config.hpp`)
count as sources, so the configuration pages' knob citations are pinnable too.

Coverage USED to be the pin list rather than the doc set, and that is what #1243 closed. A
citation with no entry below was not checked, so a citation a PR *introduced* pointed
wherever it liked — at a comment, at a blank line, at a transposed line number — and rode a
green `OK N verified` out of CI, because the verify pass only ever walked @ref ANCHORS. Five
of them did exactly that on one 2026-08-13 hygiene run. @ref unanchored_citations closes the
loop in the failing direction: every cited `file:line` span in a LIVING doc must be pinned by
an anchor somewhere inside it, exactly as the reverse rule already fails a pin no doc cites.
Adding a citation therefore means adding its anchor in the same PR — one rule, both ways,
and the pin count is a COVERAGE count rather than a sample.

Build and tooling files are not sources and used to be unreadable here at all, which is how
two rotted `LIBTRACER_NO_ATOMIC` citations sat beside a verified one in one sentence (#1052).
@ref CITABLE_NON_SOURCE_PATHS enrols the ones whose citations ARE pinned — an explicit
allowlist, because covering non-source files wholesale is a maintainer's call.

What #1052 left open, and #1095 closed: everything OUTSIDE that allowlist was a FALSE
GREEN, not merely unchecked. A line-numbered citation of a `.yml`, a `.txt` or a `.mjs`
matched nothing here, so the gate exited 0 whether it resolved or not — and a PR read that
silence as "no cited file shifted lines" while its own diff had moved a cited workflow 18
lines. @ref unverifiable_citations makes that class an ERROR: a citation carrying a line
number is verified, or the author must enrol the file or drop the line number. A token
naming no file in the tree stays ignored — `127.0.0.1:47301` is not a citation.

SYMBOL citations (#1705) are the other spelling, and the one the design and module pages now
use: `` `core/src/graph.cpp:graph_t::write_impl` `` names the code instead of its line, and
@ref symbol_citations checks it by SEARCH — it fails when the symbol is gone from the cited
file or names more than one place in it. It needs no anchor and no re-pin, so an edit above
the cited code is free; a rename still turns the gate red, naming the citing page. Line
citations keep working, anchored as below, in the pages not yet migrated.

Historical genres are deliberately NOT enrolled. `docs/adr/`, `docs/spec/` and
`docs/research/` are dated records of a decision: their citations describe the tree as it
stood, some already point past today's EOF, and pinning them would demand rewriting
history on every refactor.

`--repin` is the other half (#836): when a source edit HAS moved cited lines, it rewrites
every citation spelling from a line map instead of leaving a `sed` sweep to find them.
The rules below are load-bearing there, each paid for:

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
* **`--from-rev` refuses a SECOND application.** That mode's map is derived from source
  files alone, so re-pinning the docs does not change it: running it again against the
  same `REV` moves every pin by the same delta twice. The result is syntactically fine,
  is reported as "N citation(s) rewritten", and is wrong — it bit twice before it was
  named (#1591). The tell is the anchor table, which the first run re-pins too: an anchor
  that resolves IN PLACE at a line the revision map still says must move can only mean
  the delta has been applied already. A run that sees that writes nothing and exits
  non-zero. The anchor-derived mode needs no guard — its map comes from those same
  anchors, so a second run sees fixed points everywhere and moves nothing.
* **A HOLD is a verdict, not a remark.** `--repin` used to exit 0 whether it re-pinned
  everything or held half of it for a human, so a rebase procedure that ran it and looked at
  the exit status read "held" as "done" and shipped stale citations (#1243). A run that ends
  with anything held now exits non-zero and says so on its last line.

Usage:  python3 tools/check_doc_citations.py
        python3 tools/check_doc_citations.py --repin [--from-rev REV] [--apply]
Exits non-zero on the first stale citation, listing every one it found; `--repin` exits
non-zero when it held a citation back rather than re-pinning it.
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
# (`config.hpp`) — the only place the compile-time knobs are *declared*, and what
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
# WHAT A GREEN RUN MEANS, and what it does not (#1271). Every check below is MECHANICAL:
# it proves a citation points at the text someone once decided it should point at, and that
# every cited span is pinned by somebody. It proves NOTHING about whether the cited line
# supports the sentence citing it. Adding an anchor is the one moment of judgement in the
# whole loop, and after it the pin is carried forward by every re-pin forever — so an anchor
# added merely to silence a FAIL, without reading the sentence, launders a wrong citation
# into a permanently green one. `custom-device.md` cited the subscriber-append BACKPRESSURE
# arm for a claim about the CREATE ACL gate and stayed green across two mechanical re-pins.
# When you add an entry here, the question to answer is "does this line support that
# sentence?", not "does the gate pass?"
#
# `scope` disambiguates an anchor whose text repeats — the three `:field` depth gates
# are the same statement in three branches. It must appear in the SCOPE_LINES above a
# candidate for that candidate to count, which is what turns "candidates [4 lines]"
# into a single actionable answer. A `!`-prefixed scope inverts the test: the text must
# NOT appear above the candidate. That is the only way to select the EARLIER of two
# identical lines closer together than SCOPE_LINES, because a discriminator sitting
# BETWEEN them is above the later one and above the earlier one's window too — see
# `transport_vertex.cpp:94` / `:98`, which are byte-identical four lines apart.
#
# AMBIGUITY IS A FAILURE, not a footnote (#1271). Multi-hit candidates used to be computed
# only when an anchor had DRIFTED, so an anchor that still resolved in place was accepted
# however many other lines in its scope matched it too. That is how a re-pin aimed a
# paragraph about `deliver_remote`'s default full-route leg at its bound leg: the text
# occurs twice in that function, the scope named the function, and the pin moved faithfully
# to the wrong one. @ref anchor_hits is now run on the in-place path as well, and an anchor
# matching more than once inside its scope is reported so the author tightens it.
SCOPE_LINES = 80


def anchor_hits(lines: list, anchor: str, scope: str = None) -> list:
    """Every 1-based line in `lines` whose text matches `anchor` inside `scope`.

    `scope` is the disambiguator documented above: a plain string must appear in the
    SCOPE_LINES lines ABOVE a candidate for it to count, and a `!`-prefixed one must not.
    With no scope every textual match counts, which is what makes a repeated anchor with no
    scope ambiguous — the condition the verify pass refuses.

    One helper for all three callers (the drift report, the re-pin line maps, and the
    in-place ambiguity check) so "which lines does this anchor mean?" has exactly one
    answer. It used to be spelled inline twice, and the third caller is the one #1271 found
    missing.
    """
    hits = [i + 1 for i, ln in enumerate(lines) if anchor in ln]
    if not scope:
        return hits
    negated = scope.startswith("!")
    needle = scope[1:] if negated else scope
    return [h for h in hits
            if any(needle in x for x in lines[max(0, h - SCOPE_LINES):h]) != negated]

ANCHORS = [
    # The RFC-0022 §3.B pair, cited by the #1392 erratum: the WRITE arm's terminal fall-through
    # (every flat `:settings.<knob>` name ⇒ SCHEMA_NOT_FOUND, caller-independently) and the READ
    # container that survives it. The write arm is pinned on its EXPLANATORY COMMENT, not on the
    # `return std::unexpected(status_t::SCHEMA_NOT_FOUND);` four lines below it: that return text
    # occurs 21× in graph.cpp, its enclosing `field_write` signature is 254 lines up (far outside
    # SCOPE_LINES), and no scope separates it from the next one — so the comment is the only
    # unambiguous pin, and it is also the line that states the rule the doc sentence asserts.
    ("core/src/graph.cpp:3977", "Everything else under `settings`"),
    ("core/src/graph.cpp:4176", "result_t<view_t> graph_t::read_settings(vertex_t* v) const {"),
    ("core/src/transport_vertex.cpp:57", 'cfg.name("kind")'),
    # The two `graph_.register_child_type("client"/"listener", …)` anchors that used to live
    # here — the pair that needed a NEGATIVE scope to tell apart, being byte-identical four
    # lines apart — are GONE: RFC-0014 S7 retired the `:children[]` connection-creation door,
    # so there is no such call in this file any more and nothing left to disambiguate.
    ("core/src/transport_vertex.cpp:258",
     "void transport_vertex_t::register_transport_type(std::string kind, transport_factory_t factory) {"),
    ("core/src/transport_vertex.cpp:282",
     "transport_types_.insert_or_assign(std::move(kind), "
     "kind_entry_t{std::move(factory), traits});"),
    ("core/src/transport_vertex.cpp:285", "transport_vertex_t::register_module"),
    # Three lines now spell SCHEMA_NOT_FOUND in this file — `module_for_locked`'s
    # declared-only refusal (this one), `declaration_for_locked`'s unsupported-(module, kind)
    # refusal that RFC-0014 S2b added below it, and the unregistered-kind refusal in creation.
    # No scope separates all three, so the anchor is the WHOLE indented statement instead: the
    # other two carry a leading `if (…)` on the same line and no longer match.
    ("core/src/transport_vertex.cpp:337",
     "    return std::unexpected(status_t::SCHEMA_NOT_FOUND);"),
    # RFC-0014 S2b's creator endpoint: the mint site CONTEXT.md cites for "declaring a
    # module mints /net/<module>/conn", and the dispatch site it cites for "the written
    # TLV's TYPE selects the operation".
    ("core/src/transport_vertex.cpp:364", "transport_vertex_t::mint_module_locked"),
    ("core/src/transport_vertex.cpp:430", "transport_vertex_t::endpoint_write"),
    ("core/src/transport_vertex.cpp:598", "transport_vertex_t::provide_link"),
    ("core/src/transport_vertex.cpp:618", "routing key IS the mount path"),
    # The qualified-key compose repeats since S2b: the creation path builds it here, and
    # the endpoint's `NAME` remove builds the same key earlier in the file. The mount-path
    # comment sits directly above THIS one and below the other.
    ("core/src/transport_vertex.cpp:625", "qualified += name", "routing key IS the mount path"),
    ("core/src/transport_vertex.cpp:643", "structural vertex, created lazily"),
    # Two module-vertex mints since S2b — `register_module`'s eager one and creation's
    # lazy one. The lazy-mint comment selects this (later) one.
    ("core/src/transport_vertex.cpp:819", "pending_links_.erase(pl)"),
    ("core/src/transport_vertex.cpp:838", "if (constructed)"),
    ("core/src/transport_vertex.cpp:841", "? link_state_t::LISTENING"),
    ('core/src/transport_vertex.cpp:84', '[[nodiscard]] view_t link_state_value(link_state_t state) {'),
    # ("core/include/libtracer/fwd_router.hpp", "Slot addresses are NOT stable") — anchor
    # DROPPED (#892). Its only citer was ADR-0072's `fwd_router.hpp:596-605`, and an ADR is a
    # DATED record that `--repin` deliberately never rewrites. So the anchor tracked a line the
    # live tree moves while its citation is frozen by policy: any edit above it orphans the
    # anchor and reds the gate, with no correct re-pin available on either side. An anchor
    # exists to keep a LIVE doc citation from rotting; this text has no live citer left.
    ("core/src/transport_vertex.cpp:813", "return std::unexpected(status_t::BACKPRESSURE);",
     "if (!router_.add_child(qualified, *link))"),
    ("core/include/libtracer/transport_vertex.hpp:498", "result_t<void> register_module"),
    ("core/include/libtracer/transport_vertex.hpp:115", "enum class link_state_t"),
    # The #1392 erratum's two pins for "a connection's config is creation-time and const":
    # the record itself, and its ONLY accessor — whose `const conn_settings_t*` return is the
    # whole no-reconfiguration-door argument, so the anchor keeps the `const` in it.
    ("core/include/libtracer/transport_vertex.hpp:634",
     "const conn_settings_t* settings_of(std::string_view name) const;"),
    # The synthesized `:children[]` a bus connection answers accepted-peer enumeration from —
    # the fact that replaced reference/13's stale "`:children[]` / `:settings`".
    ("core/src/transport_vertex.cpp:742", "handlers.on_children = {"),
    ("core/src/graph.cpp:3893", "field_selector(field) != field_sel_t::APPEND"),
    ('core/include/libtracer/mem_heap.hpp:150', '[[nodiscard]] inline bool try_grow(std::size_t bytes, F&& grow) noexcept {'),
    ("core/include/libtracer/graph.hpp:60", "namespace tr::graph {"),
    ('core/include/libtracer/graph.hpp:1994', 'template <typename F>'),
    ('core/include/libtracer/graph.hpp:2398', 'struct delivery_drops_t {'),
    ('core/include/libtracer/graph.hpp:2430', '[[nodiscard]] delivery_drops_t delivery_drops() const noexcept;'),
    # The graph-level DEFAULT receiver-ring source (#1462, RFC-0025 §4.6.1) — cited by
    # docs/design/allocation-and-backpressure.md's seam table beside the other three.
    ('core/include/libtracer/transport.hpp:46', 'using peer_id_t = std::array<std::byte, 16>;'),
    ("core/include/libtracer/segment.hpp:80", "struct segment_t"),
    # --- the design + module pages (#728). Every one of these had drifted. ---
    ("core/src/graph.cpp:2779", "std::array<std::byte, 4096> stack;"),
    ("core/src/graph.cpp:2780", "bump_source_t src(stack"),
    ("core/src/graph.cpp:3895", "acl_right_t::CREATE", 'step0.name == "children"'),
    ("core/src/fwd_router.cpp:3610", "val.try_materialize(*flat_)"),
    ("core/src/fwd_router.cpp:3613", "emit_compact", "fwd_router_t::deliver_remote"),
    # `vertex.hpp:<parent_>` was pinned here TWICE, and the only doc that cites it is
    # `docs/spec/rfcs/0019` — a historical genre this tool's own header excludes from
    # pinning ("dated records of a decision ... pinning them would demand rewriting
    # history on every refactor"). It could only survive a refactor by rewriting that
    # RFC, which is what the exclusion exists to forbid, so both copies are dropped
    # rather than kept as a pin nothing living can hold up (#896).
    # --- #803: the shorthand + `config.hpp` enrolment ---
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
    ('bench/bench_libtracer.cpp:1504',
     '// (The `loopback` and n-routers `routers-hN` modes benchmarked the ROUTER-flood'),
    # core/examples/wire_codec.cpp
    ('core/examples/wire_codec.cpp:71', 'std::printf("encoded POINT{VALUE,VALUE}+CRC: %zu bytes\\n", wire.size());'),
    ('core/examples/wire_codec.cpp:94', 'constexpr int kIters = 50000;'),
    # core/include/libtracer/config.hpp
    ('core/include/libtracer/config.hpp:102', '* Override fragment: `static constexpr std::size_t kVertexLockStripes = 8;`; ESP-IDF:'),
    ('core/include/libtracer/config.hpp:112', 'static constexpr std::size_t kVertexLockStripes = 16;'),
    # core/include/libtracer/config.hpp
    ('core/include/libtracer/config.hpp:285', 'using acl_policy_t = allow_only_policy_t;'),
    ('core/include/libtracer/config.hpp:112',
     'static constexpr std::size_t kVertexLockStripes = 16;'),
    # Was pinned to the :316 banner rule, one of three IDENTICAL comment rules in this header —
    # an anchor no scope could ever separate. Re-pinned inside the SAME cited span
    # (the derived-spelling block the table cites) to the first derived spelling, which is unique.
    # core/include/libtracer/fwd_router.hpp
    ('core/include/libtracer/fwd_router.hpp:138',
     'struct router_planes_t {'),
    # core/include/libtracer/graph.hpp
    ('core/include/libtracer/graph.hpp:796',
     'explicit graph_t(mem::block_source_t& src = mem::heap_source(), graph_hooks_t hooks = {});'),
    ('core/include/libtracer/graph.hpp:910',
     '* already-retired or unregistered vertex succeeds and does nothing. The root cannot be'),
    ('core/include/libtracer/graph.hpp:1632',
     '[[nodiscard]] result_t<value_ref_t> read(vertex_handle_t v, std::string_view caller = {}) const;'),
('core/include/libtracer/graph.hpp:1770',
     '[[nodiscard]] result_t<value_ref_t> await(vertex_handle_t v, std::chrono::nanoseconds timeout,'),
    # core/include/libtracer/mem_heap.hpp
    ('core/include/libtracer/mem_heap.hpp:472',
     '[[nodiscard]] inline std::optional<view_t> over_bytes(std::span<const std::byte> bytes) noexcept {'),
    # core/include/libtracer/mem_pool.hpp
    ('core/include/libtracer/mem_pool.hpp:161', 'class synchronized_pool_t final : public mem_backend_t {'),
    # core/include/libtracer/mem_source.hpp
    ('core/include/libtracer/mem_source.hpp:364',
     '*          between operations. It is NOT a long-lived seam: an 8 KiB bump source wired as'),
    ('core/include/libtracer/mem_source.hpp:365',
     "*          a router's `rx` decoded 6 frames and rejected the next 194 — measured. A"),
    ('core/include/libtracer/mem_source.hpp:568',
     'pool_source_t(std::span<std::byte> slab, std::span<size_class_t> classes) noexcept'),
    ('core/include/libtracer/mem_source.hpp:630',
     '[[nodiscard]] std::size_t classes_used() const noexcept { return n_; }'),
    ('core/include/libtracer/mem_source.hpp:641',
     '[[nodiscard]] std::size_t overflowed() const noexcept { return overflow_; }'),
    # core/include/libtracer/path.hpp
    ('core/include/libtracer/path.hpp:42', 'inline constexpr std::size_t kMaxSegments = 255;'),
    # core/include/libtracer/rope.hpp
    ('core/include/libtracer/rope.hpp:208',
     '* @brief The single contiguous link — the consumer\'s explicit "this value is'),
    ('core/include/libtracer/rope.hpp:210',
     '* @note Precondition: `link_count() == 1` (debug-asserted). A consumer that'),
    # core/include/libtracer/segment.hpp
    ('core/include/libtracer/segment.hpp:54',
     'void inc_relaxed() noexcept { count_.fetch_add(1, std::memory_order_relaxed); }'),
    # core/include/libtracer/transport.hpp
    ('core/include/libtracer/transport.hpp:528',
     'virtual void send(std::span<const std::span<const std::byte>> iov) {'),
    # core/include/libtracer/transport_udp.hpp
    ('core/include/libtracer/transport_udp.hpp:92', 'static constexpr std::size_t kMaxDatagram = 65536;'),
    # core/include/libtracer/transport_vertex.hpp
    ('core/include/libtracer/transport_vertex.hpp:139',
     "* §5 leanness ruling): a kind's PRIVATE config (e.g. quic's `tls` profile name) never"),
    ('core/include/libtracer/transport_vertex.hpp:170',
     'std::uint32_t backoff_ms = 0;         /**< @brief DIAL self-heal retry interval (RFC-0014 §4);'),
    ('core/include/libtracer/transport_vertex.hpp:174',
     'std::uint32_t connect_timeout_ms = 0; /**< @brief DIAL connect-attempt deadline (RFC-0014 §4):'),
    # RE-HOMED (#1461/#1462). The enumerator's docstring was REWRITTEN by RFC-0025 §4.6.1
    # Amendment 2 — the ring is the CONSUMER's, not a producer-side one — so the line the old
    # anchor pinned no longer exists anywhere. Re-homed to the rewritten first line, which
    # still says what every citing doc claims of it: role 2 is a bounded history ring whose
    # retained depth is declared owner-side. Derived by grepping the merged file, never by
    # adding a shift to the old number.
    ('core/include/libtracer/vertex.hpp:260',
     "STREAM,       /**< @brief Role 2: the CONSUMER's bounded history ring"),
    ('core/include/libtracer/value.hpp:633',
     "* Holding one keeps the value's block alive — and under an injected `block_source_t` that is a"),
    ('core/include/libtracer/subscriber.hpp:160', 'using subscriber_fn_t = void (*)(void* ctx, const value_t& value);'),
    # core/src/graph.cpp
    ('core/src/graph.cpp:2103', 'if (target == nullptr) {',
     'target = find_ptr(*e.target_key);'),
    ('core/src/graph.cpp:2368',
     '// A handler stores no LKV (the user handler consumes the value), so there is no'),
    # RE-HOMED (#1461/#1462). "the just-appended ring entry" described a PRODUCER-side append
    # that RFC-0025 §4.6.1 Amendment 2 deleted. The eager write on a STREAM still drains that
    # vertex's ring and advances its cursor — it is the RECEIVER's ring now — so the pin moves
    # to the rewritten sentence, the same fact about the same code. Derived by grep, not by
    # arithmetic on the old line number.
    ('core/src/graph.cpp:3986', 'result_t<void> graph_t::create_child(vertex_t* parent, const view_t& spec_value) {'),
    # core/src/posix_endpoint.cpp
    ('core/src/posix_endpoint.cpp:294',
     'write_result_t stream_endpoint_t::write_all_iov(int fd, std::span<const ::iovec> vec,'),
    ('core/src/posix_endpoint.cpp:181', 'return ::sendmsg(fd, msg, MSG_NOSIGNAL);'),
    # The multi-peer servers' per-chunk receive scratch — ONE buffer since #871 folded the
    # tcp and ws poll loops into slot_server_t (it used to be one apiece, cited as
    # transport_tcp.cpp:508 and transport_ws.cpp:420).
    # core/src/transport_tcp.cpp
    ('core/src/transport_tcp.cpp:60',
     "*        `bench_forward_heap`'s `allocs=0` gate cannot see it: that bench drives"),
    # zero-copy-and-flatten.md quotes this comment's tail verbatim, so the anchor carries the
    # QUOTED line — pinning `serve()`'s signature two constructs up passed while the citation
    # pointed at code the doc never quotes.
    # core/src/transport_udp.cpp
    ('core/src/transport_udp.cpp:146',
     'const std::size_t rx_cap = std::min(kMaxDatagram, backend_->max_segment_size());'),
    # core/src/transport_vertex.cpp
    ('core/src/transport_vertex.cpp:54',
     'void parse_config(const tlv_t* config, conn_settings_t& s) {'),

    # --- re-added from the v0.7.1 docs sweep (absent from main's table) ---
    ('core/include/libtracer/fwd_frame_view.hpp:1053', 'inline constexpr std::size_t kFwdMaxIov = 10;'),
    # ONE `bus()` since #871: both stream servers inherit slot_server_t's (they used to
    # restate it, cited as transport_tcp.hpp:343 and transport_ws.hpp:233).
    # The #1438 PROVIDER half: which arm a concrete stream server derives from is the binding's
    # choice, so the facet is absent from a bus-less listener's LAYOUT and not merely withheld.
    # The #375-deliverable-3 bus-module seam: the knob, and the ONE door the routing plane
    # asks the facet through.
    ('core/src/fwd_router.cpp:1198', 'link.set_rope_receiver('),
    # Was pinned to :990's `shared_lock` — seventeen identical lines in this file. Re-pinned
    # inside the SAME cited span (`graph.cpp:989-990`) to the signature that takes the lock.
    ('core/src/graph.cpp:3663', 'delivery_link.assign(split.link);'),

    # --- #1095: the rest of the non-source citations, now that a line-numbered citation
    # of an unverifiable file is an ERROR rather than a false green.
    #
    # 26 line-numbered citations of non-C++ files were live outside the two paths #1052
    # enrolled. Reading each target line against the prose that cites it found SIXTEEN
    # already pointing at unrelated text, none of which any gate could have caught: two
    # landed on a BLANK line and two on a bare `endif()`. Every entry below is pinned to
    # the line the prose actually names, and the citing doc was re-pinned to match.
    #
    # `--repin` DOES move these, since #1592: @ref ANCHOR_ENTRY_RE matches the enrolled
    # spellings by exact path, so a table entry travels with the doc citation it pins.
    # Before that it did not, and moving one without the other left the two out of step —
    # which is why the old rule was to move neither and let the gate red on both.
    # Anchored on the job's own `name:` rather than on `matrix:`. #1376 added a SECOND
    # TSan job (`tsan-reclaim-qsbr`) with an identical `matrix:` block, which made the old
    # anchor ambiguous inside its own `  tsan:` scope — the scope runs to EOF, not to the
    # next job. A rendered job name is unique by construction and needs no scope at all.
    # The flag the prose QUOTES verbatim ("-fsanitize=thread -g -O1"). #1376's qsbr leg
    # quotes the same flags with a trailing `-I`, so this anchor is the EXACT full line,
    # which the qsbr leg's is not — no scope needed.
    ('bench/CMakeLists.txt:54', 'bench_libtracer_net (two-process ROUTER-flood bench) was retired'),
    ('bindings/typescript/packages/client/test/mesh-testbed.test.mjs:25',
     "ADDRESSING: a connection's routing key IS its vertex path"),
    ('core/CMakeLists.txt:67', 'option(LIBTRACER_NET_PLANE'),
    # The GPU backend's build moved out of core into its own tier project (#1381), so what
    # docs/modules/backends.md cites is the tier's target, not a core option.
    ('core/CMakeLists.txt:310', 'option(LIBTRACER_WITH_QUIC "Configure the libtracer_quic transport module'),
    ('core/CMakeLists.txt:392', 'write_basic_package_version_file('),
    ('core/CMakeLists.txt:403', 'if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING AND EXISTS'),
    ('core/CMakeLists.txt:417', 'option(LIBTRACER_BUILD_EXAMPLES "Build the core examples"'),
    # `docs/examples/index.md` cited the two `if(LIBTRACER_NET_PLANE)` lines (58, 73). That
    # text appears THREE times in this file and the scope filter cannot separate 58 from 73
    # — a scope must sit ABOVE its candidate, and everything above 58 is also above 73. The
    # citation was re-pinned one line down onto the `add_executable` calls, which is what
    # "declared inside `if(LIBTRACER_NET_PLANE)` blocks" actually names anyway.
    ('core/examples/CMakeLists.txt:59', 'add_executable(two_node_fwd two_node_fwd.cpp)'),
    ('core/examples/CMakeLists.txt:74', 'add_executable(tree_of_ropes tree_of_ropes.cpp)'),
    ('core/examples/CMakeLists.txt:87', 'if(BUILD_TESTING)'),
    ('core/examples/CMakeLists.txt:92', 'add_test(NAME example_wire_codec COMMAND wire_codec)'),
    ('integrations/esp-idf/libtracer/CMakeLists.txt:169', 'if(CONFIG_LIBTRACER_TRANSPORT_CAN)'),
    # #1470: the S5 liveness-engine knobs the config-space table points at.
    ('integrations/esp-idf/libtracer/CMakeLists.txt:170',
     'list(APPEND LIBTRACER_SRCS ${LIBTRACER_SOURCES_TRANSPORT_CAN})'),

    # --- #1243: the backfill that made the pin list a COVERAGE list.
    #
    # 71 citations were live in the doc surfaces and pinned by NOTHING. The verify pass
    # walked the table, so every one of them was accepted without ever being read — which is
    # how five newly-written citations pointing at a comment, a blank line and a transposed
    # line number rode a green `OK 382` out of one hygiene run. @ref unanchored_citations now
    # fails a cited span no anchor covers, and these are the anchors that debt bought.
    #
    # TWELVE of the 71 could not be pinned as written, because the cited line was not the line
    # the prose describes. Those citations were re-pinned in the SAME commit — a phantom
    # `twai_link.hpp:455` a bare `:455` inherited across an out-of-repo `.c` citation, a
    # `:2152`/`:2154` pair that had rotted off the COMPACT emit calls onto `subscribe_toward`,
    # a `graph.cpp:1098` on a bare `}`, a `transport_vertex.cpp:152` on a blank line, and a
    # `fwd_router.cpp:1066` that named a queue splice instead of the `deref_vertex_slot` the
    # sentence points at. Every one of them was invisible to the gate before this.
    #
    # The last two of the twelve are the rule earning its keep in the wild rather than in a
    # test. The ESP component CHANGELOG's #963.4 entry says three IDF components are "named by
    # headers under `include/libtracer_esp/`" — a claim only an `#include` line can support.
    # Two of the three cited a `#include <atomic>` and a stray doc-comment instead, and stayed
    # invisible until #1160 shifted `httpd_ws_link.hpp` and this gate reddened on the rebase.
    # They now pin `esp_http_server.h` and `esp_transport.h`, the lines that carry the claim.
    ('bench/bench_forward_heap.cpp:8',
     '* @warning **What this gate does NOT cover.** It drives `capture_transport_t`, a stub link that'),
    ('core/include/libtracer/can.hpp:361',
     'if (path_len > kAdvertiseMaxPathLen) return std::nullopt;  // wedge bound (see constant)'),
    ('core/include/libtracer/graph.hpp:1634',
     "* @brief Write a resolved vertex's value: `assign` then deliver (RFC-0008 §D)."),
    ('core/include/libtracer/graph.hpp:1642',
     '* @brief Field-write by handle: resolve the @ref vertex_handle_t and @ref field_path_t'),
    ('core/include/libtracer/graph.hpp:2457',
     'void count_external_drop(external_drop_t why, std::uint64_t n) noexcept;'),
    ('core/include/libtracer/path.hpp:57',
     "* separates field levels, `[` / `]` delimit the grammar's index suffix (which sits"),
    ('core/include/libtracer/tlv.hpp:71', 'PATH_REF = 0x14,'),
    ('core/include/libtracer/subscriber.hpp:109', 'struct delivery_policy_t {'),
    ('core/include/libtracer/vertex.hpp:499', 'enum class delivery_mode_t : std::uint8_t {'),
    ('core/include/libtracer/vertex.hpp:3031', 'const std::size_t doff = off;'),
    ('core/include/libtracer/vertex.hpp:3110',
     '// padding — 8-byte, then 4-byte, then flag bytes), with everything the write hot'),
    # Three lines now spell this table: the FORWARD hop's rope arm (this one) and the two
    # TERMINUS reply gathers #1570 migrated onto the same seam. The forward arm is the only
    # one under the rope-source comment, which is what selects it.
    ('core/src/fwd_router.cpp:2759',
     'mem::block_array_t<std::span<const std::byte>> iov{rx_for(inbound_ctx)};',
     '// Rope source: a region may cross several links, so the sub-span count is only known'),
    ('core/src/fwd_router.cpp:3612', 'if (fresh) emit_advertise(*link, label, route);'),
    ('core/src/graph.cpp:1196', 'return acl_allows(v.get(), caller, right);'),
    ('core/src/graph.cpp:1642',
     'if (ancestor != nullptr && !acl_allows(ancestor, caller, acl_right_t::CREATE))'),
    ('core/src/graph.cpp:1953',
     '// The wildcard spelling is RESERVED in the subject-token space (#908): the wire has one'),
    ('core/src/graph.cpp:1954',
     '// spelling for a subject, so a principal that could BE `EVERYONE@` is indistinguishable'),
    ('core/src/graph.cpp:2059',
     'if (!acl_allows(v, caller, acl_right_t::READ))',
     'result_t<value_ref_t> graph_t::read(vertex_handle_t vh, std::string_view caller) const {'),
    # Re-aimed (#1584): 09-memory-substrate.md's scope-lifetime bump-source example cited
    # `2532-2533` — the branch plan vector and the `parse_branch_node` call — where the
    # composition it describes is the stack buffer and the bump built over `*ctl_`.
    ('core/src/graph.cpp:2779', 'std::array<std::byte, 4096> stack;'),
    ('core/src/graph.cpp:2780', 'mem::bump_source_t src(stack, *ctl_);'),
    ('core/src/graph.cpp:2996', 'if (!detail::try_assign(copy, k)) return false;'),
    ('core/src/graph.cpp:3093',
     '// Empty-set fast path (the per-eager-write case when nobody uses assign+propagate):'),
    # ('core/src/graph.cpp:3249', '// status (ADR-0060 §3), …') — anchor DROPPED (#1271). It
    # existed only to pin `custom-device.md`'s creation-gate citation, which named the
    # subscriber-append BACKPRESSURE arm for a claim about the `:children[]` CREATE gate. The
    # citation now points at the gate itself (`graph.cpp:2419-2423`, already pinned above), so
    # this text has no live citer left.
    ('core/src/graph.cpp:3795', 'if (!tlv) return std::unexpected(status_t::TYPE_MISMATCH);'),
    ('core/src/graph.cpp:3989',
     '// (NAME key, NAME/SETTINGS value), read through the ONE pair-consuming walk,'),
    ('core/src/graph.cpp:4176', 'result_t<view_t> graph_t::read_settings(vertex_t* v) const {'),
    ('core/src/op_resolve_walk.hpp:311',
     'p.mint_request = (op_byte & kFwdOpFlagMintRequest) != 0;'),
    ('core/src/op_resolve_walk.hpp:317',
     '// elements, at or under the count bound). What an element MEANS is settled at the deref, in'),
    ('core/src/op_resolve_walk.hpp:667',
     '// `vertex_slot` returns the index and the generation TOGETHER, from one lock hold. Read'),
    ('core/src/transport_tcp.cpp:52',
     '*        count is chosen by the sending peer) and answered by DROPPING the'),
    ('integrations/esp-idf/libtracer/include/libtracer_esp/esp_ws_client_link.hpp:203',
     '#include "esp_transport.h"'),
    ('integrations/esp-idf/libtracer/include/libtracer_esp/httpd_ws_link.hpp:173',
     '#include "esp_http_server.h"'),
    ('integrations/esp-idf/libtracer/include/libtracer_esp/twai_link.hpp:36',
     '#include "esp_twai.h"'),
    # --- #1504: the backpressure & sizing guide's stage / seam / retention pins ---
    # docs/reference/22-backpressure-and-sizing.md cites each bounded stage, the member
    # that observes it, and the five LKV-consuming planes of its retention table.
    # core/src/graph.cpp
    ('core/src/graph.cpp:2040',
     '[[gnu::noinline]] result_t<value_ref_t> graph_t::read_handler_gated(vertex_t* v) const {'),
    ('core/src/graph.cpp:2074',
     'value_ref_t sp = v->read_stored();  // lock-free'),
    ('core/src/graph.cpp:2368',
     '// A handler stores no LKV (the user handler consumes the value), so there is no'),
    ('core/src/graph.cpp:2662',
     '// Deliver exactly what was stored (RFC-0008 §D): the published LKV pointer —'),
    ('core/src/graph.cpp:2733',
     'if (!retains(v, role)) return std::unexpected(status_t::SCHEMA_NOT_FOUND);'),
    # --- #1477: the write-vs-retire doctrine and the atomic `role_` it rests on ---
    ('core/src/graph.cpp:2929', 'void graph_t::deliver_current(vertex_t* v) {'),
    ('core/src/graph.cpp:2946',
     "// The sweep root's OWN delivery is unconditional (below), and it reads the LKV"),
    ('core/src/graph.cpp:3211',
     'if (!sp) return std::unexpected(status_t::NOT_FOUND);  // never assigned'),
    ('core/src/graph.cpp:4505',
     'n.lkv = w.v->read_stored();  // ONE atomic load per node'),
    # core/include/libtracer/graph.hpp
    ('core/include/libtracer/graph.hpp:642',
     '* source until it retires. Per-injection-point, never a shared pool: one receiver running'),
    ('core/include/libtracer/graph.hpp:646',
     'mem::block_source_t* ring_source = nullptr;'),
    ('core/include/libtracer/graph.hpp:1747',
     '[[nodiscard]] result_t<std::size_t> ring_reserved_bytes(vertex_handle_t v) const;'),
    ('core/include/libtracer/graph.hpp:630',
     '* copies always. What sharing costs on a POOLED RX backend: the shared value BORROWS a'),
    # core/include/libtracer/vertex.hpp
    ('core/include/libtracer/vertex.hpp:1406',
     '* - **reliable** — the admission is refused, NOTHING is shed and the ring does not grow'),
    ('core/include/libtracer/vertex.hpp:1420',
     'bool ring_admit(const value_ref_t& sp, std::size_t bytes,'),
    ('core/include/libtracer/vertex.hpp:1434',
     '// The DEPTH intent retires BEFORE the byte bound charges. Order matters: a ring already'),
    ('core/include/libtracer/vertex.hpp:1672',
     '* durability (`policy.durability_request()`, RFC-0022 §3.A) and the vertex already'),
    # core/include/libtracer/mem_source.hpp
    ('core/include/libtracer/mem_source.hpp:77', 'struct source_stats_t {'),
    ('core/include/libtracer/mem_source.hpp:228',
     '[[nodiscard]] virtual source_stats_t stats() const noexcept { return {}; }'),
    # core/include/libtracer/fwd_router.hpp
    ('core/include/libtracer/fwd_router.hpp:87', 'struct router_stats_t {'),
    ('core/include/libtracer/fwd_router.hpp:441',
     '[[nodiscard]] router_stats_t drop_stats() const noexcept {'),
    ('core/include/libtracer/fwd_router.hpp:464',
     '[[nodiscard]] mem::block_source_t& label_source() const noexcept { return *label_src_; }'),
    ('core/include/libtracer/fwd_router.hpp:467',
     '[[nodiscard]] mem::block_source_t& rx_source() const noexcept { return *rx_; }'),
    ('core/include/libtracer/fwd_router.hpp:469',
     '[[nodiscard]] mem::mem_backend_t& flatten_backend() const noexcept { return *flat_; }'),
    # core/include/libtracer/route_handle.hpp
    ('core/include/libtracer/route_handle.hpp:246',
     'std::size_t max_bindings_per_link = 0)'),
    ('core/include/libtracer/route_handle.hpp:625',
     '[[nodiscard]] std::size_t labels_used(std::string_view link) const;'),
    # core/include/libtracer/transport.hpp
    ('core/include/libtracer/transport.hpp:511',
     '[[nodiscard]] virtual transport_drop_stats_t drop_stats() const noexcept { return {}; }'),
    # integrations/esp-idf/libtracer/httpd_ws_link.cpp
    ('integrations/esp-idf/libtracer/httpd_ws_link.cpp:3251',
     'std::size_t httpd_ws_link_t::tx_slot_capacity() const noexcept { return tx_pool_slots_; }'),
    ('integrations/esp-idf/libtracer/httpd_ws_link.cpp:3093',
     'void httpd_ws_link_t::send_in_call(const session_ref_t& to,'),
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
# alternation is ordered longest-first so `config.hpp` does not truncate to
# `config.hpp`.
#
# The DOCUMENT branch exists purely to BREAK inheritance: a page that cites
# `reference/07-host-embedding.md:79` and then writes `` `:285` `` means line 285 of that
# markdown page, not of whatever header it named three paragraphs earlier. Without this
# branch the bare form silently attached to the stale source file and pinned a line
# nothing in the docs was talking about.
#
# Only a DOCUMENT breaks the run. A build file cited mid-sentence
# (`CMakeLists.txt:189`) does not — the configuration page's knob table names
# `config.hpp` once and then walks down it in bare `:109` / `:136` refs, with each
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
    "backends/cuda/CMakeLists.txt",
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
#
# The last branch is the SYMBOL citation (#1705): `` `core/src/graph.cpp:graph_t::propagate` ``
# names the code by what it IS rather than by where it sits, so an unrelated edit above it
# never forces a re-pin. It is backticked on both sides (a prose `file.h: No such file`
# is not a citation), the text after the colon starts with neither a digit nor a space,
# which keeps it disjoint from every line-numbered branch above, and it may wrap onto ONE
# more line, as a Markdown code span can. @ref symbol_citations
# resolves it by search, not by the anchor table.
CITATION_RE = re.compile(
    r"`?((?:[A-Za-z0-9_./-]*/)?[A-Za-z0-9_][A-Za-z0-9_.-]*\.(?:" + _EXTS + r")):([\d,\-]+)`?"
    r"|`?((?:[A-Za-z0-9_./-]*/)?[A-Za-z0-9_][A-Za-z0-9_.-]*\.(?:" + DOC_EXTS + r")):[\d,\-]+"
    r"|`:([\d,\-]+)`"
    r"|`?(?P<other>" + _ANY_PATH + r"):(?P<otherspec>[\d,\-]+)`?"
    r"|`(?P<sympath>" + _ANY_PATH + r"):(?P<symbol>[^\s`\d][^`\n]*(?:\n[^`\n]*)?)`"
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


# --------------------------------------------------------------------------------------
# Symbol citations (#1705): name the code, not its line number
# --------------------------------------------------------------------------------------
#
# A line citation is a pointer into a moving file, so every edit above it costs a re-pin
# commit — about a quarter of the non-merge commits since August were nothing else. A
# SYMBOL citation, `` `graph.cpp:graph_t::propagate` ``, names what the code IS, and is
# checked by SEARCH: it resolves when the cited file holds that symbol exactly once, and it
# fails, naming the citing page, when the symbol is gone or names more than one place. No
# anchor, no re-pin, and a rename still turns the gate red.
#
# Two spellings, told apart by shape:
#
# * A SYMBOL — an identifier, optionally qualified (`graph_t::propagate`, `tr::wire::decode`,
#   `kVertexLockStripes`), optionally with a trailing `()` to say "the function". It
#   matches whole identifiers in CODE only (comments and string literals are blanked first),
#   so a stale comment that still mentions a renamed symbol does not keep it alive. A
#   qualified symbol also matches its out-of-line spelling with template arguments
#   (`graph_t<C>::propagate`). When it occurs more than once — a member is declared once and
#   USED many times — the occurrences are narrowed to DECLARATIONS (@ref _is_declaration),
#   and a qualified symbol further to the declarations whose innermost enclosing
#   class/struct/namespace is its last qualifier.
# * A SUBSTRING — anything else (`if (depth > kMaxDepth)`, `struct opt_t`). Matched
#   literally against the raw lines, comments included, and must sit on exactly one line.
#   This is the escape hatch when a symbol is genuinely ambiguous (overloads) or when the
#   sentence is about one statement rather than one declaration.
#
# The narrowing is a heuristic over text, not a parser. Its failure mode is LOUD — a
# symbol it cannot single out is reported as ambiguous, and the author qualifies it or
# cites a longer substring — never a silent pick.
SYMBOL_RE = re.compile(r"^~?[A-Za-z_]\w*(?:::~?[A-Za-z_]\w*)*$")
# Files whose comments and literals are blanked before a SYMBOL is searched for.
C_LIKE_SUFFIXES = SOURCE_SUFFIXES
# A token that, written immediately before a name, makes it a declaration of that name.
_DECL_KEYWORDS = frozenset(
    ("class", "struct", "union", "enum", "namespace", "using", "typedef", "define", "concept"))
# Identifiers that end an EXPRESSION prefix rather than a type: `return foo(x)` calls foo.
_NON_TYPE_WORDS = frozenset((
    "return", "else", "case", "throw", "co_return", "co_yield", "co_await", "delete", "new",
    "goto", "sizeof", "alignof", "decltype", "typeid", "not", "and", "or", "if", "while",
    "for", "switch", "do", "noexcept", "requires", "static_assert",
))
_SCOPE_OPENER_RE = re.compile(
    r"\b(?:class|struct|union|namespace|enum(?:\s+(?:class|struct))?)\s+"
    r"(?:\[\[[^\]]*\]\]\s*)?([A-Za-z_][\w:]*)")
_LEXEME_RE = re.compile(r'/\*|//|"|\'')
_STRING_RE = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])\'')


def _code_lines(lines: list) -> list:
    """`lines` with comments and string/char literals blanked — the text a SYMBOL is
    searched in. Line count and numbering are preserved; block comments may span lines."""
    out, in_block = [], False
    for line in lines:
        text, i, code = line, 0, []
        while i < len(text):
            if in_block:
                end = text.find("*/", i)
                if end < 0:
                    i = len(text)
                    break
                in_block, i = False, end + 2
                continue
            m = _LEXEME_RE.search(text, i)
            if not m:
                code.append(text[i:])
                break
            code.append(text[i:m.start()])
            tok = m.group()
            if tok == "//":
                break
            if tok == "/*":
                in_block, i = True, m.end()
                continue
            lit = _STRING_RE.match(text, m.start())
            if lit:
                code.append(" ")
                i = lit.end()
            else:
                code.append(tok)  # a digit separator (`1'000`) or a stray quote
                i = m.end()
        out.append("".join(code))
    return out


def _scopes(code: list) -> list:
    """For each line, the stack of enclosing brace scopes AT ITS START, innermost last.

    A scope opened by `class`/`struct`/`union`/`enum`/`namespace` carries that name (the
    last component, so `namespace tr::wire` reads as `wire`); every other block (a function
    body, a lambda, an initializer) carries None.
    """
    stack, pending, out = [], "", []
    for line in code:
        out.append(tuple(stack))
        for ch in line:
            if ch == "{":
                names = _SCOPE_OPENER_RE.findall(pending)
                stack.append(names[-1].rsplit("::", 1)[-1] if names else None)
                pending = ""
            elif ch == "}":
                if stack:
                    stack.pop()
                pending = ""
            elif ch == ";":
                pending = ""
            else:
                pending += ch
        pending += " "
    return out


def _is_declaration(code: list, idx: int, start: int, end: int) -> bool:
    """True when the name at `code[idx][start:end]` is being DECLARED, not used.

    The rule is the shape of a C++ declaration: the name is preceded by a type (an
    identifier that is not an expression keyword, or `>`, `*`, `&`, `]`) or by a declaring
    keyword (`class`, `using`, `#define`, ...), and followed by what can follow a declarator
    (`(`, `;`, `{`, `=`, `[`, `,`, a single `:`, or the end of the line). An enumerator has
    no type, so a name opening a line that ends in `,` or carries no `;` counts too. A
    declaration whose return type sits on the line above reads its prefix from there.
    """
    line = code[idx]
    prefix, suffix = line[:start].rstrip(), line[end:].lstrip()
    # Inside an open parameter list the name is a PARAMETER (`slot_server_t(bool peer_named)`)
    # and declares nothing a citation can mean.
    if prefix.count("(") > prefix.count(")"):
        return False
    if not prefix:
        if re.match(r"(?:=(?!=)|,|$)", suffix) and (line.rstrip().endswith(",") or ";" not in line):
            return True
        j = idx - 1
        while j >= 0 and not code[j].strip():
            j -= 1
        prefix = code[j].rstrip() if j >= 0 else ""
    # An out-of-line definition, `result_t<void> graph_t<C>::write(`: the qualifier chain is
    # part of the declarator, so the type that decides it sits before the chain.
    prefix = re.sub(r"(?:~?[A-Za-z_]\w*(?:<[^;{}()]*>)?\s*::\s*)+$", "", prefix).rstrip()
    # ...and so do the attribute specifiers a type head may carry: `struct alignas(N) x_t`.
    prefix = re.sub(r"(?:\s*(?:alignas\([^)]*\)|\[\[[^\]]*\]\]))+$", "", prefix).rstrip()
    if not re.match(r"(?:\(|;|\{|=(?!=)|\[|,|:(?!:)|$|final\b|override\b|const\b|noexcept\b)",
                    suffix):
        return False
    # A pointer or reference declarator (`mem_backend_t* value_backend_`) is a declaration
    # only when a TYPE stands before the `*`/`&`; `return *value_backend_` is a use.
    m = re.search(r"([A-Za-z_]\w*|\]\]|(?<!-)>)\s*[*&]*$", prefix)
    if not m:
        return False
    tok = m.group(1)
    if tok in _DECL_KEYWORDS:
        return True
    return tok not in _NON_TYPE_WORDS


def symbol_hits(lines: list, needle: str, c_like: bool = True) -> list:
    """Every 1-based line of `lines` the citation text `needle` resolves to (see above).

    One hit is a resolved citation; none is a GONE symbol; more than one is ambiguous. An
    OVERLOADED function is singled out by the start of its parameter list —
    `graph_t::unsubscribe(subscription_t` — which keeps only the declarations whose name is
    followed by that text (whitespace-insensitive).
    """
    head, paren, args = needle.partition("(")
    if not SYMBOL_RE.match(head):
        return [i + 1 for i, ln in enumerate(lines) if needle in ln]
    args = "" if args in ("", ")") else args
    code = _code_lines(lines) if c_like else lines
    parts = head.split("::")
    literal = re.compile(r"(?<![\w~])" + r"(?:<[^;{}()]*>)?::".join(map(re.escape, parts))
                         + r"(?!\w)")
    hits = [i + 1 for i, ln in enumerate(code) if literal.search(ln)]
    if len(hits) == 1 and len(parts) == 1 and not args:
        return hits
    # Narrow to DECLARATIONS of the last component. A qualified symbol always takes this
    # path, even with one literal hit: `return slot_server_t::peer_named();` is a use, and the
    # declaration it names sits unqualified inside `class slot_server_t`.
    name = re.compile(r"(?<![\w~])" + re.escape(parts[-1]) + r"(?!\w)")
    scopes = _scopes(code) if len(parts) > 1 else None
    decls = []
    for i, ln in enumerate(code):
        for m in name.finditer(ln):
            # A constructor (`graph_t::graph_t`) has no type before it, so the declaration
            # rule cannot see it; inside its own class, a name opening a call-shaped line is it.
            ctor = (len(parts) > 1 and parts[-1] == parts[-2]
                    and re.match(r"\s*(?:(?:explicit|constexpr|inline)\s+)*$", ln[:m.start()])
                    and ln[m.end():].lstrip().startswith("("))
            if not ctor and not _is_declaration(code, i, m.start(), m.end()):
                continue
            if scopes is not None:
                # Qualified AT this occurrence (an out-of-line definition), or declared
                # directly inside the scope the last qualifier names.
                inner = scopes[i][-1] if scopes[i] else None
                here = any(lm.end() == m.end() for lm in literal.finditer(ln))
                if not here and inner != parts[-2]:
                    continue
            decls.append(i + 1)
            break
    # A type's name is also its constructors' name. Cited bare (`bump_source_t`) it means the
    # TYPE, so a class/struct/union/enum definition wins over the constructors declared in
    # it; `bump_source_t::bump_source_t` still names a constructor.
    if not (len(parts) > 1 and parts[-1] == parts[-2]):
        types = [h for h in decls
                 if re.search(r"\b(?:class|struct|union|enum)(?:\s+(?:class|struct))?"
                              r"(?:\s+alignas\([^)]*\)|\s+\[\[[^\]]*\]\])*\s+"
                              + re.escape(parts[-1]) + r"(?!\w)", code[h - 1])
                 and not code[h - 1].rstrip().endswith(";")]
        decls = types or decls
    found = decls or hits
    if args:
        want = re.sub(r"\s+", "", args)
        # The parameter list may wrap, so the text after the name runs on for two lines.
        found = [h for h in found
                 if any(re.sub(r"\s+", "", " ".join([code[h - 1][m.end():]] + code[h:h + 2]))
                        .startswith("(" + want)
                        for m in name.finditer(code[h - 1]))]
        # `static_assert(sizeof(void*) != 8` has the same shape and is a SUBSTRING: when no
        # declaration takes the parameter text, the needle is matched literally instead.
        if not found:
            return [i + 1 for i, ln in enumerate(lines) if needle in ln]
    return found


def _symbol_target(spelling: str, key: tuple) -> tuple:
    """Resolve the FILE of a symbol citation, as (repo-relative path, error-or-None).

    A source spelling goes through @ref _resolve, so the basename shorthand and the
    ambiguity rule are the same as for a line citation — and a source-suffixed spelling
    naming no file is an ERROR here, because a symbol citation exists only to be checked.
    Any other spelling must name exactly one file in the tree; one that names none (an
    address, `0.0.0.0:<port>`) is not a citation at all.
    """
    if spelling.endswith(SOURCE_SUFFIXES):
        resolved, err = _resolve(spelling, key)
        if resolved or err:
            return resolved, err
        return None, f"`{spelling}` names no source file in the tree"
    enrolled = resolve_enrolled(spelling)
    if enrolled:
        return enrolled, None
    hits = tree_index().get(spelling.rsplit("/", 1)[-1], ())
    if "/" in spelling:
        hits = [h for h in hits if h == spelling or h.endswith("/" + spelling)]
    if len(hits) > 1:
        return None, f"`{spelling}` is an ambiguous basename ({', '.join(hits)}) — cite the full path"
    return (hits[0], None) if hits else (None, None)


def _symbol_run_file(spelling: str, key: tuple, last):
    """The running file after a symbol citation of `spelling` — the line-citation rule.

    A source file becomes the running file, an enrolled non-source path is an aside that
    leaves it alone, and anything else (a document) ends the run.
    """
    if spelling.endswith(SOURCE_SUFFIXES):
        return _resolve(spelling, key)[0]
    return last if resolve_enrolled(spelling) else None


def symbol_citations(text: str, filemap: dict = None) -> list:
    """Every symbol citation in one doc that does not resolve to exactly one place.

    The verify pass for the #1705 spelling: no anchor, no line number — the cited file is
    searched (@ref symbol_hits), and a GONE or AMBIGUOUS symbol is reported with the
    candidate lines, so the author can see what the citation now names.
    """
    filemap = source_map() if filemap is None else filemap
    key = tuple((k, tuple(v)) for k, v in sorted(filemap.items()))
    out, contents = [], {}
    for m in CITATION_RE.finditer(text):
        spelling, needle = m.group("sympath"), m.group("symbol")
        if not spelling:
            continue
        # A code span may wrap onto the next line; Markdown reads that break as a space.
        needle = " ".join(needle.split())
        path, err = _symbol_target(spelling, key)
        if err:
            out.append(err)
        if not path:
            continue
        if path not in contents:
            try:
                contents[path] = (REPO / path).read_text().split("\n")
            except (OSError, UnicodeDecodeError):
                contents[path] = None
        lines = contents[path]
        if lines is None:
            out.append(f"`{spelling}:{needle}` cites a file this gate cannot read ({path})")
            continue
        hits = symbol_hits(lines, needle, path.endswith(C_LIKE_SUFFIXES))
        if not hits:
            out.append(f"`{spelling}:{needle}` — {needle!r} is GONE from {path}: the symbol was "
                       f"renamed or removed. Re-point the citation at what the sentence "
                       f"describes now.")
        elif len(hits) > 1:
            out.append(f"`{spelling}:{needle}` is AMBIGUOUS — {needle!r} names {len(hits)} "
                       f"places in {path} (lines {hits[:8]}). Qualify it (`type_t::name`), "
                       f"or cite a longer substring that occurs once.")
    return out


def citation_spans(context: str, filemap: dict = None) -> tuple:
    """Every cited source SPAN in one doc, as ([(path, lo, hi)], ambiguity errors).

    Handles every spelling the docs actually use: a full path `core/src/graph.cpp:12`,
    the design pages' basename shorthand `graph.hpp:1291`, a generated header
    `config.hpp:237`, a range `file.cpp:12-20`, a comma list `file.hpp:145,153`, and
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
        elif m.group("sympath"):
            # A symbol citation pins no line — @ref symbol_citations checks it — but it
            # NAMES a file, so it moves the inheritance run exactly as a line citation of
            # the same file would. Leaving `last` alone would hand a following bare `:N` to
            # whichever file was named before it.
            last = _symbol_run_file(m.group("sympath"), key, last)
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


def unanchored_citations(text: str, anchor_locs, filemap: dict = None) -> list:
    """Cited spans in one doc that no @ref ANCHORS entry pins — the #1243 false green.

    The verify pass walks the PIN LIST: it proves that every anchor still holds, and says
    nothing whatever about a citation with no anchor. So the one citation a reviewer cannot
    check by eye — the NEW one, written in the PR under review — was the one citation the
    gate did not check either. Five wrong ones (a comment line, a transposed line number)
    shipped green that way on 2026-08-13.

    This is the same rule the dead-pin check already applies in reverse ("pinned here but no
    doc cites it any more"), pointed the other way: a cited span is pinned, or it is refused.
    A RANGE counts as pinned when an anchor sits on ANY line inside it — a doc that points at
    a `/**` means the block, and the table's convention is to pin the most distinctive line
    within a cited range rather than its head.

    `anchor_locs` is the set of `path:line` strings the table pins. Only LIVING docs are
    asked: the dated genres (@ref HISTORICAL_GENRES) are never anchored, for the reason this
    file's header gives.
    """
    spans, _ = citation_spans(text, filemap)
    out = []
    for path, lo, hi in dict.fromkeys(spans):
        if any(f"{path}:{n}" in anchor_locs for n in range(lo, hi + 1)):
            continue
        cite = f"{path}:{lo}" + (f"-{hi}" if hi != lo else "")
        out.append(f"`{cite}` is cited here but pinned by no ANCHORS entry — add an anchor "
                   f"pinning the cited line's text, or drop the line number")
    return out


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
        hits = anchor_hits(lines, anchor, scope)
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
        elif m.group("sympath"):
            # Never rewritten — a symbol does not move with its line — but it moves the
            # run, by the same rule the scanner applies (@ref citation_spans).
            last = _symbol_run_file(m.group("sympath"), key, last)
            continue
        elif m.group("other"):
            # An enrolled non-source path is re-pinned like any other now (#1592). Both
            # halves of the old refusal are gone: `revision_line_maps` builds maps for the
            # enrolled paths, and `ANCHOR_ENTRY_RE` matches their table entries by exact
            # spelling — so doc and anchor move TOGETHER, which was the property whose
            # absence made half-applying worse than leaving both alone (#1095). What has
            # not changed is that an enrolled path never becomes the running file: `last`
            # is left where it was, so a bare `:N` after one still inherits the source file
            # named before it.
            #
            # With no map for it the citation is still HELD and reported, not silently
            # skipped — the driver drops that report when the file did not move at all.
            enrolled = resolve_enrolled(m.group("other"))
            if not enrolled:
                continue
            if enrolled not in lookups:
                held.append((enrolled, m.group("otherspec")))
                continue
            other_spec, other_moves, other_held = _repin_spec(m.group("otherspec"),
                                                              lookups[enrolled])
            held += [(enrolled, n) for n in other_held]
            if other_spec != m.group("otherspec"):
                edits.append((*m.span("otherspec"), other_spec))
                moves += [(enrolled, a, b) for a, b in other_moves]
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
#
# The ENROLLED non-source paths are matched too, by exact spelling (#1592). They used to
# be excluded, which made every edit above ~line 1780 of `core/tests/CMakeLists.txt` a
# hand edit in two places — the citing page AND the table — because moving one without
# the other reds the gate. Both of Car M's PRs paid it. An exact alternation is the right
# shape here and NOT in `CITATION_RE`: a doc writes a non-source citation in three
# spellings, two of them partial, but the anchor table always writes the full path.
_ENROLLED_ALT = "|".join(re.escape(p) for p in CITABLE_NON_SOURCE_PATHS)
ANCHOR_ENTRY_RE = re.compile(
    r"(\(\s*)(['\"])((?:[A-Za-z0-9_./-]*/)?[A-Za-z0-9_][A-Za-z0-9_.-]*\.(?:" + _EXTS + r")"
    r"|" + _ENROLLED_ALT + r"):(\d+)\2"
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

    An ENROLLED non-source path gets a map here too, and since #1592 it is USED: @ref
    repin_document moves those citations and @ref ANCHOR_ENTRY_RE moves their table
    entries, so the two stay in step — which is the property whose absence made the old
    refusal the right answer. The map also still tells a shifted enrolled file from an
    untouched one, which is what keeps a clean run from printing 32 identical "held"
    lines nobody reads.
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


def already_repinned(rev_maps: dict, root: pathlib.Path = None) -> list:
    """The paths whose pins have ALREADY absorbed `rev_maps`, sorted — a SECOND run.

    `--from-rev REV` is not idempotent, and the reason is structural: its map is derived
    from source files alone, so re-pinning the docs does not change it. Run it, apply it,
    run it again against the same `REV` and every pin moves by the same delta a second
    time — output that is syntactically fine, is reported as "N citation(s) rewritten",
    and is wrong. It has bitten twice (session memory records it as an operator gotcha;
    Car M hit it again on PR #1588 and recovered by resetting the repin-only files).

    A clean verify pass is NOT the test. The first run can legitimately leave a HOLD, and
    then the gate is still red while every other pin has moved — which is exactly the
    tree a second run corrupts wholesale.

    The ANCHORS table is the test, because the first run re-pins it too:

      * BEFORE any run, an anchor in a shifted file names the line the text used to sit
        on. `anchor_line_maps` finds the text elsewhere, so the anchor is NOT a fixed
        point — and the revision map agrees that the line moved.
      * AFTER a run, the anchor names where the text actually IS: a fixed point. The
        revision map, unchanged, still says that line number must move.

    So "an anchor that resolves in place, at a line this map would move" is the
    signature, and it cannot fire on a first run. An anchor sitting ABOVE the edit is a
    fixed point too, but the map leaves it alone, so it is silent — which is what keeps
    a file with edits below its anchors from being flagged.
    """
    anchored, _ = anchor_line_maps(root=root)
    flagged = []
    for path, shifts in rev_maps.items():
        fixed = anchored.get(path)
        if not fixed:
            continue
        if any(new == old and shifts.get(old, old) != old for old, new in fixed.items()):
            flagged.append(path)
    return sorted(flagged)


def repin(from_rev: str = None, apply: bool = False) -> int:
    """The `--repin` driver: build ONE map, rewrite every LIVING surface once, report."""
    filemap = source_map()
    if from_rev:
        maps, notes = revision_line_maps(from_rev)
        # A second application of the same delta silently corrupts every pin the first
        # one fixed (#1591). Refused, not warned about, and refused BEFORE anything is
        # written — half a double-repin is not better than all of it.
        if repeated := already_repinned(maps):
            print(f"REFUSE  this tree has ALREADY absorbed the {from_rev} delta — "
                  f"re-applying it would corrupt every pin the first run fixed (#1591).")
            for path in repeated:
                print(f"        {path}: its anchors resolve where they are pinned, and this "
                      f"map still says those lines moved.")
            print(f"        Nothing was written. If a LATER source edit moved lines again, "
                  f"commit the")
            print(f"        first re-pin and pass --from-rev HEAD — the delta is measured from "
                  f"where the")
            print(f"        docs currently sit, not from where the branch started. If the first "
                  f"run left a")
            print(f"        HOLD, fix that one citation by hand; do not re-run this.")
            return 1
    else:
        maps, notes = anchor_line_maps()
    targets = [(REPO / "tools" / "check_doc_citations.py", repin_anchor_table)] + [
        (doc, None) for doc in all_docs()
    ]
    # A path whose anchors all still resolve, at the same lines, has not moved: its citations
    # need no attention and saying otherwise is noise — 32 identical "held" lines on every
    # clean run is how a report gets ignored. IN PLAY is broader than "moved": a line the map
    # sends to None (a deleted or rewritten block) is exactly a citation that needs a human,
    # so it counts too. On a clean tree every anchor is a fixed point, nothing is in play, and
    # a hold that survives that filter is a real one — which is what lets a HOLD carry the
    # exit status below (#1243) without reddening every clean run.
    shifted = {p for p, m in maps.items()
               if any(new is None or new != old for old, new in m.items())}
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
        # ONE reason a citation is held, since #1592 made the enrolled non-source paths
        # ordinary: the map has no derivable shift for that line. The second reason
        # #1095 had to report separately — "this path's TABLE entry cannot follow, so
        # moving the doc alone would put the two out of step" — no longer exists.
        for p, n in held:
            if p not in shifted:
                continue  # the file did not move; there is nothing to re-pin and nothing to say
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
    verb = "rewritten" if apply else "would move (dry run; pass --apply to write)"
    n_held = len(dict.fromkeys(held_all))
    print(f"\n{total} citation(s) {verb}; {n_held} held for a human.")
    if historical:
        print(f"      {historical} more sit in {', '.join(g.rstrip('/') for g in HISTORICAL_GENRES)} "
              f"and were left alone — a dated record cites the tree as it stood.")
    # A HOLD is part of the VERDICT (#1243). This printed its holds and then exited 0, so a
    # rebase procedure that gated on the exit status read "held" as "done" and carried stale
    # citations through — the same false green the verify pass had, one command over. A run
    # that left anything for a human says so where a shell can see it.
    if n_held:
        print(f"HELD  {n_held} citation(s) were NOT re-pinned and need a hand — "
              f"this run is INCOMPLETE (exit 1). Re-run this gate after fixing them.")
        return 1
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
    anchor_locs = {entry[0] for entry in ANCHORS}
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
            # The other half of the same rule (#1243): a citation the gate CAN read but no
            # anchor pins is not verified either, and used to pass in silence. Named with the
            # doc, so the fix is "add this anchor", not "go find who cites this".
            failures += [f"{rel}: {e}"
                         for e in dict.fromkeys(unanchored_citations(text, anchor_locs, filemap))]
            # Symbol citations (#1705) are checked by search, not by the table: a GONE or
            # AMBIGUOUS symbol fails here, named with the page that cites it.
            failures += [f"{rel}: {e}" for e in dict.fromkeys(symbol_citations(text, filemap))]
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
            # Resolves in place — but does it resolve UNIQUELY? An anchor matching two lines
            # in its scope pins neither: the next re-pin picks whichever the line map lands
            # on, and the author's judgement about WHICH of them the prose meant was never
            # recorded. Reported here rather than left to the drift path, which is the only
            # place the candidate set used to be computed at all (#1271).
            candidates = anchor_hits(lines, anchor, scope)
            if len(candidates) > 1:
                failures.append(
                    f"{loc}: AMBIGUOUS — {anchor!r} matches {len(candidates)} lines in scope "
                    f"{candidates}. Tighten the anchor text, or add a scope that appears in "
                    f"the {SCOPE_LINES} lines above the intended line (a `!`-prefixed scope "
                    f"selects the line it does NOT appear above).")
            continue
        # Drifted — find where the anchor went, so the fix is mechanical.
        hits = anchor_hits(lines, anchor, scope)
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
