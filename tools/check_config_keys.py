#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Gate the connection-config key reference against the source that reads the keys.

`docs/modules/connection-config.md` is the reference home for the SPEC `config`
SETTINGS keys a `/net/<module>/conn` creation carries — the universal ones parsed into
`conn_settings_t` and the *kind-private* ones each transport factory parses for
itself (the ADR-0043 §5 leanness ruling). A hand-maintained table of those keys is
exactly the artifact this repo has watched rot before, so the page does not carry
one: it carries marker-delimited blocks, and this tool derives the truth from the
source and fails when the two disagree.

Derivation. A connection config is read through `config_reader_t` (the pair walk
lives in `tr::wire`, #985; the transport plane spells it `wire::config_reader_t`),
whose typed accessors take the key as a string literal:

    const wire::config_reader_t cfg(raw_config);
    if (const auto v = cfg.name("ca")) ...      -> key "ca",  value spelling `NAME`
    if (const auto v = cfg.flag("insecure")) ...-> key "insecure", `VALUE` u8 (flag)

So for every file that CONSTRUCTS a reader, or takes one by reference, the key set
and each key's wire value type are recoverable by reading the accessor calls on that
reader's variable. The page declares the same per kind: a block names the factory's
file and, after it, any shared file the factory hands its reader to (`quic` and
`webtransport` both pass theirs to `select_tls_profile` in `msquic_endpoint.hpp`,
which reads `tls`). The check is a two-way set comparison plus a per-key
value-spelling comparison.

What a shared file contributes is tracked PER FUNCTION, not per file (#2569). A
shared file is read through by more than one kind, so a key it reads counts for a
kind only when that kind's own file calls the function that reads it: a helper in
`msquic_endpoint.hpp` that only `webtransport` calls must not make a `quic` row
pass. Concretely, a shared file's keys are attributed to the function whose
parameter list takes the reader by reference (`select_tls_profile(...,
const wire::config_reader_t& cfg)`), plus any such function of the same file that
one calls; a kind's block gets its own file's keys and the keys of the shared
functions its own file calls (`name(` outside comments). Overloads merge by name.
A shared file that reads a key OUTSIDE such a function (a reader it constructs
itself, a lambda) cannot be attributed and is a failure, never a fallback to the
whole-file union; so is a reader function that reads keys and that no block's own
file calls, whose keys would otherwise go unchecked. And a shared entry must be a
header that no block owns: a transport source is one kind's, so its keys would leak
into the other kind, and naming it as shared does not count as documenting it.
Calls are found with comments and string/char literals blanked; member and
qualified calls count.

Three ways the page can fail, all of them the ways it would go stale:
  * a key read in source that the page's block for that file does not list;
  * a key the page lists that the file no longer reads (a stale row);
  * a file that reads keys and is named by no block at all (a whole kind
    undocumented — the "one kind documented, the others implied to have none"
    failure the page exists to prevent).
Two more refuse a block whose shape would let a dead key through:
  * a "shared" file that is not a header, or is another block's own (kind) file;
  * a shared file reading a key the gate cannot attribute to a reader-taking function;
  * a reader-taking function in a shared file that reads keys no kind calls it for.

Scope, stated because an unreached completeness claim is itself a defect: this
gate covers CONNECTION config only. Since #985 hoisted the walk to `tr::wire`,
`graph_t::create_child` and the SUBSCRIBER QoS walk (`core/src/graph_fields.cpp`) read
their grammar through the SAME `config_reader_t` type — but a creation-SPEC
envelope and a per-subscription QoS block are not connection config, so that file
is excluded below rather than documented on this page. The ACL walk still carries
its own reject-unknown-keys code (#906) and never constructs a reader.

  tools/check_config_keys.py            # the gate: page vs source (exit 1 on drift)
  tools/check_config_keys.py --list     # print the derived inventory and exit 0
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PAGE = ROOT / "docs" / "modules" / "connection-config.md"

# Where a connection config can be read. `core/src` holds every transport factory
# and the universal-key parse; the header tree is scanned too so a reader that
# moves into a header does not silently leave the gate's field of view.
SOURCE_DIRS = (ROOT / "core" / "src", ROOT / "core" / "include")
SOURCE_SUFFIXES = (".cpp", ".hpp")

# The reader's own header declares the accessors; it reads no keys of its own.
# `graph_fields.cpp` constructs readers too (#985, #1711), but over the creation-SPEC envelope and
# the SUBSCRIBER QoS SETTINGS — not connection config, hence not this page's subject.
EXCLUDED = {
    ROOT / "core" / "include" / "libtracer" / "config_reader.hpp",
    ROOT / "core" / "src" / "graph_fields.cpp",
}

# `const config_reader_t cfg(raw_config);` -> the variable every accessor hangs off.
READER_CTOR = re.compile(r"\bconfig_reader_t\s+([A-Za-z_]\w*)\s*\(")
# `const wire::config_reader_t& cfg` -> a reader a shared helper is handed by its caller.
READER_REF = re.compile(r"\bconfig_reader_t\s*&\s*([A-Za-z_]\w*)\b")
# A reader construction line carrying this marker (as a trailing comment ON the ctor line
# — the exemption is matched per line, so a marker in a preceding comment block does not
# reach it) reads something that is NOT connection
# config, so the keys it takes are not this page's subject. It is the per-reader form of
# the whole-file `EXCLUDED` above, needed since RFC-0014 S2b put the creation-SPEC
# ENVELOPE walk (`name`, `config`) in `transport_vertex.cpp` — the same file that also
# holds the universal-key parse the page DOES document, so the file cannot be excluded
# wholesale. The exemption is by VARIABLE NAME within the file, so an exempted reader
# must not share its name with a documented one in the same translation unit.
NOT_CONFIG = re.compile(r"config-keys:\s*not-connection-config")
# `cfg.flag("insecure")` -> (receiver, accessor, key).
ACCESSOR = re.compile(r"\b([A-Za-z_]\w*)\s*\.\s*(name|u8|u16|u32|flag)\s*\(\s*\"([^\"]*)\"\s*\)")

# How the page must spell the wire value each accessor reads. The accessor IS the
# type declaration — `flag` is `u8` read as nonzero-is-true, which the page has to
# say, because "u8" alone would not tell a peer that 2 means true.
VALUE_SPELLING = {
    "name": "`NAME` utf-8",
    "u8": "`VALUE` u8",
    "u16": "`VALUE` u16",
    "u32": "`VALUE` u32",
    "flag": "`VALUE` u8 (flag)",
}

# `<!-- config-keys:begin core/src/transport_quic.cpp core/src/msquic_endpoint.hpp -->`
# … `<!-- config-keys:end -->`: the kind's own file first, then the shared files it reads
# through.
BLOCK_BEGIN = re.compile(r"<!--\s*config-keys:begin\s+(\S+(?:\s+[^\s-]\S*)*)\s*-->")
BLOCK_END = re.compile(r"<!--\s*config-keys:end\s*-->")
# A table row's first cell, `| `insecure` | … `, and its second (the value spelling).
ROW = re.compile(r"^\|\s*`([^`]+)`\s*\|\s*([^|]*?)\s*\|")


def source_files():
    """Every candidate source file, repo-relative-sorted for stable output."""
    out = []
    for d in SOURCE_DIRS:
        if not d.is_dir():
            continue
        for p in sorted(d.rglob("*")):
            if p.suffix in SOURCE_SUFFIXES and p not in EXCLUDED:
                out.append(p)
    return out


def derive_keys(text):
    """The `{key: accessor}` a single translation unit reads, or `{}` for no reader.

    Only accessor calls on a variable declared as a `config_reader_t` count, so a
    same-named method on an unrelated object (`cfg.path`, `reader.u32` on some
    future type) cannot smuggle a phantom key into the derived set. A file that
    constructs no reader derives nothing — which is how `builtin_transport_udp.cpp`
    proves it has no kind-private keys rather than merely being unmentioned.

    A construction line carrying a trailing `config-keys: not-connection-config` marker
    is skipped: its reader walks a different grammar (the creation-SPEC envelope), so its
    keys belong to no kind and to no row on this page.
    """
    readers = set(READER_CTOR.findall(text)) | set(READER_REF.findall(text))
    # Drop the readers marked as reading something other than connection config.
    for line in text.splitlines():
        if NOT_CONFIG.search(line):
            readers.difference_update(READER_CTOR.findall(line))
    if not readers:
        return {}
    keys = {}
    for recv, accessor, key in ACCESSOR.findall(text):
        if recv in readers:
            keys[key] = accessor
    return keys


# A string literal (kept) or a comment (blanked): the call search must not see `f(` in a
# comment, and brace matching must not see a brace in one.
_LEXEME = re.compile(r'"(?:\\.|[^"\\\n])*"|//[^\n]*|/\*.*?\*/', re.DOTALL)
_TRAILING_NAME = re.compile(r"([A-Za-z_]\w*)\s*$")


def strip_comments(text):
    """@p text with every comment replaced by spaces (newlines kept), string literals intact."""

    def blank(m):
        tok = m.group(0)
        return tok if tok.startswith('"') else re.sub(r"[^\n]", " ", tok)

    return _LEXEME.sub(blank, text)


# A string or character literal in comment-stripped code.
_LITERAL = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'')


def strip_literals(text):
    """@p text with comments AND string/char literal contents blanked: the call search's input.

    A log message such as `"select_tls_profile(failed)"` is not a call.
    """
    return _LITERAL.sub(lambda m: m.group(0)[0] + " " * (len(m.group(0)) - 2) + m.group(0)[-1],
                        strip_comments(text))


def _match(text, open_at, opener, closer):
    """Index of the @p closer balancing the @p opener at @p open_at, or -1."""
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == opener:
            depth += 1
        elif text[i] == closer:
            depth -= 1
            if depth == 0:
                return i
    return -1


def shared_functions(text):
    """`({function: {key: accessor}}, unattributed, reach)` for a shared file.

    A function is one whose PARAMETER LIST takes a `config_reader_t` by reference; its
    keys are the accessor calls on that parameter inside its body, plus the keys of the
    other such functions of this file it calls. `unattributed` is the `{key: accessor}`
    the file reads that no such function accounts for — a reader the file constructs
    itself, or a by-reference reader in a lambda — which the caller turns into a failure.
    `reach` maps each function to the set of this file's reader functions a call to it
    runs (itself included), so the caller can tell which functions some kind exercises.
    """
    code = strip_comments(text)
    bodies = {}  # name -> [(param, body text)]
    spans = []
    for m in READER_REF.finditer(code):
        depth, open_at = 0, -1
        for i in range(m.start() - 1, -1, -1):
            if code[i] == ")":
                depth += 1
            elif code[i] == "(":
                if depth == 0:
                    open_at = i
                    break
                depth -= 1
        name = _TRAILING_NAME.search(code[:open_at]) if open_at > 0 else None
        close_at = _match(code, open_at, "(", ")") if name else -1
        if close_at < 0:
            continue
        tail = re.match(r"[^;{]*", code[close_at + 1 :])
        brace = close_at + 1 + tail.end()
        if brace >= len(code) or code[brace] != "{":
            continue  # a declaration, not a definition
        end = _match(code, brace, "{", "}")
        if end < 0:
            continue
        bodies.setdefault(name.group(1), []).append((m.group(1), code[brace:end]))
        spans.append((brace, end, m.group(1)))

    direct = {}
    for fn, defs in bodies.items():
        keys = direct.setdefault(fn, {})
        for param, body in defs:
            for recv, accessor, key in ACCESSOR.findall(body):
                if recv == param:
                    keys[key] = accessor
    resolved, reach = {}, {}
    for fn in bodies:
        keys, seen, todo = {}, set(), [fn]
        while todo:
            cur = todo.pop()
            if cur in seen:
                continue
            seen.add(cur)
            keys.update(direct[cur])
            for _, body in bodies[cur]:
                body = strip_literals(body)
                todo.extend(callee for callee in bodies if calls(body, callee))
        resolved[fn] = keys
        reach[fn] = seen

    covered = set()
    for brace, end, param in spans:
        for m in ACCESSOR.finditer(code, brace, end):
            if m.group(1) == param:
                covered.add(m.start())
    readers = set(READER_CTOR.findall(code)) | set(READER_REF.findall(code))
    for line in code.splitlines():
        if NOT_CONFIG.search(line):
            readers.difference_update(READER_CTOR.findall(line))
    unattributed = {}
    for m in ACCESSOR.finditer(code):
        if m.group(1) in readers and m.start() not in covered:
            unattributed[m.group(3)] = m.group(2)
    return resolved, unattributed, reach


def calls(text, function):
    """Whether literal-stripped @p text calls @p function (`function(` as a whole word).

    A qualified (`ns::f(`) or member (`ep.f(`, `ep->f(`) call counts: a reader-taking
    helper may be a member of an endpoint type, and a miss here is caught anyway by the
    "no kind calls it" failure rather than passing silently.
    """
    return re.search(rf"(?<!\w){re.escape(function)}\s*\(", text) is not None


def parse_page(text):
    """The `{(source path, ...): {key: value-spelling}}` the page declares.

    A block is keyed by the tuple of files its marker names, the kind's own file first.

    Returns `(blocks, errors)`. A malformed block is an error rather than an empty
    set: silently reading zero keys out of a broken marker would make the gate pass
    for the wrong reason.
    """
    blocks = {}
    errors = []
    current = None
    for lineno, line in enumerate(text.splitlines(), start=1):
        begin = BLOCK_BEGIN.search(line)
        if begin:
            if current is not None:
                errors.append(f"{PAGE.name}:{lineno}: nested config-keys:begin")
            current = tuple(begin.group(1).split())
            if any(k[0] == current[0] for k in blocks):
                errors.append(f"{PAGE.name}:{lineno}: duplicate block for {current[0]}")
            blocks[current] = {}
            continue
        if BLOCK_END.search(line):
            if current is None:
                errors.append(f"{PAGE.name}:{lineno}: config-keys:end with no begin")
            current = None
            continue
        if current is None:
            continue
        row = ROW.match(line)
        if row:
            blocks[current][row.group(1)] = row.group(2).strip()
    if current is not None:
        errors.append(f"{PAGE.name}: unterminated block for {current[0]}")
    return blocks, errors


def check(page_text=None):
    """Compare page against source. Returns a list of human-readable failures.

    @p page_text stands in for the committed page (the unit tests' seam); None reads it.
    """
    if page_text is None:
        if not PAGE.is_file():
            return [f"missing page: {PAGE.relative_to(ROOT)}"]
        page_text = PAGE.read_text(encoding="utf-8")
    blocks, failures = parse_page(page_text)

    derived = {}
    for path in source_files():
        keys = derive_keys(path.read_text(encoding="utf-8"))
        if keys:
            derived[str(path.relative_to(ROOT))] = keys

    owners = {files[0] for files in blocks}
    # A shared entry must be a header no block owns. A transport source (`.cpp`) is one
    # kind's translation unit, never read through by another kind; refusing it here also
    # keeps it from counting as "named" below, so naming it as shared cannot exempt it
    # from the "file named by no block" check.
    refused = {
        rel for files in blocks for rel in files[1:]
        if rel != files[0] and (rel in owners or not rel.endswith(".hpp"))
    }
    named = {rel for files in blocks for rel in files} - refused
    shared_seen = {}  # shared file -> (functions, unattributed, reach, credited functions)
    for rel in sorted(derived):
        if rel not in named:
            failures.append(
                f"{rel} reads connection-config keys ({', '.join(sorted(derived[rel]))}) "
                f"but {PAGE.relative_to(ROOT)} has no config-keys block for it"
            )
    for files in sorted(blocks):
        missing = [rel for rel in files if not (ROOT / rel).is_file()]
        for rel in missing:
            failures.append(f"{PAGE.relative_to(ROOT)}: block names a missing file: {rel}")
        if missing:
            continue
        rel = " + ".join(files)
        own = files[0]
        want = dict(derived.get(own, {}))
        own_code = strip_literals((ROOT / own).read_text(encoding="utf-8"))
        for shared in files[1:]:
            if shared in refused:
                failures.append(
                    f"{rel}: names {shared} as shared, but a shared file must be a header "
                    f"no block owns — a transport source is one kind's, never read through"
                )
                continue
            if shared not in shared_seen:
                text = (ROOT / shared).read_text(encoding="utf-8")
                shared_seen[shared] = (*shared_functions(text), set())
            functions, _, reach, credited = shared_seen[shared]
            for fn in sorted(functions):
                if calls(own_code, fn):
                    want.update(functions[fn])
                    credited.update(reach[fn])
        have = blocks[files]
        for key in sorted(set(want) - set(have)):
            failures.append(f"{rel}: reads key `{key}` that the page does not document")
        for key in sorted(set(have) - set(want)):
            failures.append(f"{rel}: page documents key `{key}` that the file does not read")
        for key in sorted(set(want) & set(have)):
            spelling = VALUE_SPELLING[want[key]]
            if have[key] != spelling:
                failures.append(
                    f"{rel}: key `{key}` is read with .{want[key]}() — the page must "
                    f"spell its value {spelling!r}, not {have[key]!r}"
                )
    for shared in sorted(shared_seen):
        functions, unattributed, _, credited = shared_seen[shared]
        for key in sorted(unattributed):
            failures.append(
                f"{shared}: reads key `{key}` outside a function that takes a "
                f"config_reader_t by reference, so no kind can be credited with it"
            )
        for fn in sorted(set(functions) - credited):
            if functions[fn]:
                failures.append(
                    f"{shared}: {fn}() reads keys ({', '.join(sorted(functions[fn]))}) but no "
                    f"block naming this file has an own file that calls it"
                )
    return failures


def main(argv):
    if "--list" in argv:
        for path in source_files():
            keys = derive_keys(path.read_text(encoding="utf-8"))
            if keys:
                rel = path.relative_to(ROOT)
                for key in sorted(keys):
                    print(f"{rel}\t{key}\t{VALUE_SPELLING[keys[key]]}")
        return 0
    failures = check()
    for f in failures:
        print(f"config-key drift: {f}", file=sys.stderr)
    if failures:
        print(
            f"\n{len(failures)} problem(s). The source is right and the page is wrong: "
            f"update docs/modules/connection-config.md.",
            file=sys.stderr,
        )
        return 1
    print("config keys: docs/modules/connection-config.md matches the source it documents")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
