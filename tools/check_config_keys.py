#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Gate the connection-config key reference against the source that reads the keys.

`docs/modules/connection-config.md` is the reference home for the SPEC `config`
SETTINGS keys a `/net:children[]` creation carries — the universal ones parsed into
`conn_settings_t` and the *kind-private* ones each transport factory parses for
itself (the ADR-0043 §5 leanness ruling). A hand-maintained table of those keys is
exactly the artifact this repo has watched rot before, so the page does not carry
one: it carries marker-delimited blocks, and this tool derives the truth from the
source and fails when the two disagree.

Derivation. A connection config is read through `tr::net::config_reader_t`, whose
typed accessors take the key as a string literal:

    const config_reader_t cfg(raw_config);
    if (const auto v = cfg.name("ca")) ...      -> key "ca",  value spelling `NAME`
    if (const auto v = cfg.flag("insecure")) ...-> key "insecure", `VALUE` u8 (flag)

So for every file that CONSTRUCTS a reader, the key set and each key's wire value
type are recoverable by reading the accessor calls on that reader's variable. The
page declares the same per source file, and the check is a two-way set comparison
plus a per-key value-spelling comparison.

Three ways the page can fail, all of them the ways it would go stale:
  * a key read in source that the page's block for that file does not list;
  * a key the page lists that the file no longer reads (a stale row);
  * a file that constructs a reader and has no block at all (a whole kind
    undocumented — the "one kind documented, the others implied to have none"
    failure the page exists to prevent).

Scope, stated because an unreached completeness claim is itself a defect: this
gate covers the `config_reader_t` family only. `graph_t::create_child` and the
SUBSCRIBER QoS / ACL walks read the same positional grammar without that type
(they sit at L4, where `tr::net` cannot be a dependency), and they are NOT
connection config — they are out of this page's subject and out of this check.

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
EXCLUDED = {ROOT / "core" / "include" / "libtracer" / "config_reader.hpp"}

# `const config_reader_t cfg(raw_config);` -> the variable every accessor hangs off.
READER_CTOR = re.compile(r"\bconfig_reader_t\s+([A-Za-z_]\w*)\s*\(")
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

# `<!-- config-keys:begin core/src/transport_quic.cpp -->` … `<!-- config-keys:end -->`
BLOCK_BEGIN = re.compile(r"<!--\s*config-keys:begin\s+(\S+)\s*-->")
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
    """
    readers = set(READER_CTOR.findall(text))
    if not readers:
        return {}
    keys = {}
    for recv, accessor, key in ACCESSOR.findall(text):
        if recv in readers:
            keys[key] = accessor
    return keys


def parse_page(text):
    """The `{source path: {key: value-spelling}}` the page declares.

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
            current = begin.group(1)
            if current in blocks:
                errors.append(f"{PAGE.name}:{lineno}: duplicate block for {current}")
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
        errors.append(f"{PAGE.name}: unterminated block for {current}")
    return blocks, errors


def check():
    """Compare page against source. Returns a list of human-readable failures."""
    if not PAGE.is_file():
        return [f"missing page: {PAGE.relative_to(ROOT)}"]
    blocks, failures = parse_page(PAGE.read_text(encoding="utf-8"))

    derived = {}
    for path in source_files():
        keys = derive_keys(path.read_text(encoding="utf-8"))
        if keys:
            derived[str(path.relative_to(ROOT))] = keys

    for rel in sorted(derived):
        if rel not in blocks:
            failures.append(
                f"{rel} reads connection-config keys ({', '.join(sorted(derived[rel]))}) "
                f"but {PAGE.relative_to(ROOT)} has no config-keys block for it"
            )
    for rel in sorted(blocks):
        src = ROOT / rel
        if not src.is_file():
            failures.append(f"{PAGE.relative_to(ROOT)}: block names a missing file: {rel}")
            continue
        want = derived.get(rel, {})
        have = blocks[rel]
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
