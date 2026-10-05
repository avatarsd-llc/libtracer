#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Gate the `_t` rule on every type a public header declares (#1723).

WHY THIS EXISTS. `core/STYLE.md` §Type and value naming makes `snake_case_t` the
spelling of every class, struct, union, enum and alias, and an agent learns the
convention from the code it reads. Ten public types had drifted off it before #1723
renamed them; this gate keeps the next one from landing.

WHAT IT READS. The public header trees (`core/include`, `backends/*/include`, the
ESP-IDF component's `include`). A declaration is a line that opens with `class`,
`struct`, `union` or `enum` (after an optional `template <...>` or attribute), or a
`using NAME =` alias. It is a line scan, not a parse: a declaration split across lines
before its name is not seen, which errs toward passing.

WHAT IT ALLOWS. Names ending in `_t`, names the standard library's protocols require a
type to spell (`iterator`, `value_type`, ...), and the entries in `ALLOWED`, each with
the reason it stands.

Usage:
    python3 tools/check_type_names.py      # gate (exit 1 on a violation)
"""
import re
import sys
from pathlib import Path

ROOTS = ("core/include", "backends", "integrations/esp-idf/libtracer/include")
"""@brief Public header trees, relative to the repository root."""

DECL_RE = re.compile(
    r"^\s*(?:template\s*<.*>\s*)?(?:class|struct|union|enum(?:\s+class|\s+struct)?)\s+"
    r"(?:(?:\[\[[^\]]*\]\]|alignas\([^)]*\))\s+)*([A-Za-z_]\w*)\b(?!\s*::)"
)
"""@brief A class/struct/union/enum declaration; group 1 is the declared name."""

ALIAS_RE = re.compile(r"^\s*(?:template\s*<.*>\s*)?using\s+([A-Za-z_]\w*)\s*=")
"""@brief A `using NAME =` alias; group 1 is the alias name."""

STD_PROTOCOL = frozenset({
    "iterator", "const_iterator", "reverse_iterator", "const_reverse_iterator",
    "value_type", "difference_type", "size_type", "reference", "const_reference",
    "pointer", "const_pointer", "element_type", "iterator_concept", "iterator_category",
    "is_transparent", "type", "allocator_type", "key_type", "mapped_type",
})
"""@brief Member names the standard library's traits and concepts look up by spelling."""

ALLOWED = {
    # The pre-v0.18.0 spellings #1723 keeps as aliases for one release.
    "transport_ws_server": "#1723 alias, removed in v0.19.0",
    "transport_ws_client": "#1723 alias, removed in v0.19.0",
    "transport_tcp_server": "#1723 alias, removed in v0.19.0",
    "transport_can": "#1723 alias, removed in v0.19.0",
    "length_prefix_framer": "#1723 alias, removed in v0.19.0",
    "stack_writer": "#1723 alias, removed in v0.19.0",
    "span_cursor": "#1723 alias, removed in v0.19.0",
    "rope_cursor": "#1723 alias, removed in v0.19.0",
    "crc32c_state": "#1723 alias, removed in v0.19.0",
    "crc16_ccitt_state": "#1723 alias, removed in v0.19.0",
    # Outside #1723's list; standing until a ticket renames them.
    "backend_tag": "tr::mem enum outside #1723's list",
    "sized_guard": "detail metafunction spelled as a standard trait (`::type`)",
    "guard_type": "member alias read by spelling (`publishes_under`, #1715)",
    "iovec": "the POSIX `struct iovec`, forward-declared",
}
"""@brief Names allowed without the suffix, each with the reason it stands."""

IGNORED_FIRST = frozenset({"alignas"})
"""@brief Tokens the declaration regex can capture that are not names."""


def violations(text: str) -> list[tuple[int, str]]:
    """@brief Every (line number, name) in @p text that breaks the `_t` rule."""
    out = []
    for no, line in enumerate(text.splitlines(), 1):
        if line.lstrip().startswith(("//", "*", "/*")):
            continue
        for rx in (DECL_RE, ALIAS_RE):
            m = rx.match(line)
            if not m:
                continue
            name = m.group(1)
            if (name.endswith("_t") or name in STD_PROTOCOL or name in ALLOWED
                    or name in IGNORED_FIRST or not name[0].islower()):
                continue
            out.append((no, name))
    return out


def headers(root: Path):
    """@brief The public headers under the gated trees."""
    for r in ROOTS:
        base = root / r
        if r == "backends":
            yield from sorted(base.glob("*/include/**/*.hpp"))
        elif base.is_dir():
            yield from sorted(base.rglob("*.hpp"))


def main() -> int:
    """@brief Report each violation as `file:line: name`; exit 1 when any exist."""
    root = Path(__file__).resolve().parent.parent
    bad = []
    for h in headers(root):
        for no, name in violations(h.read_text(encoding="utf-8")):
            bad.append(f"{h.relative_to(root)}:{no}: `{name}` lacks the `_t` suffix")
    for b in bad:
        print(b)
    if bad:
        print("\nPublic types are `snake_case_t` (core/STYLE.md §Type and value naming).")
        return 1
    print("type names: every public type follows the _t rule")
    return 0


if __name__ == "__main__":
    sys.exit(main())
