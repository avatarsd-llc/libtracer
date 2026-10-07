#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Generate docs/adr-rfc-index.md from the status key every ADR and RFC carries.

Each record (``docs/adr/NNNN-*.md``, ``docs/spec/rfcs/NNNN-*.md``) carries exactly
one machine-readable key directly under its title::

    <!-- status: accepted; superseded-in-part-by: ADR-0047 -->

``status`` is one of STATUSES. ``superseded-by`` names the records that replace this
one whole; ``superseded-in-part-by`` names records that replace a section of it while
the rest stands. The prose status line below the key stays the human record; the key
is what this script and agents read.

``--check`` fails (exit 1) when the committed index differs from what the keys
produce, when a key is missing or malformed, when a supersession names a record that
does not exist, or when a numbering gap has no entry in GAPS. Stdlib only.
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "adr-rfc-index.md"
SERIES = (("ADR", ROOT / "docs" / "adr"), ("RFC", ROOT / "docs" / "spec" / "rfcs"))
STATUSES = ("draft", "proposed", "in-comment", "accepted", "superseded", "rejected", "withdrawn")
LINK_KEYS = ("superseded-by", "superseded-in-part-by")
KEY_RE = re.compile(r"^<!-- (status: .*?) -->$", re.M)
REF_RE = re.compile(r"^(ADR|RFC)-(\d{4})$")

# Numbers deliberately never issued. A new gap fails --check until it is explained here,
# so a skipped number is always a decision, never an accident.
GAPS = {
    ("RFC", 12): "Used by the dtype/direction draft of PR #416, which was closed unmerged.",
    ("RFC", 15): "Used by PR #446, withdrawn under the type-agnosticism gate.",
}
GAP_NOTE = (
    "Neither gap is reusable: each number carries history in a closed pull request, "
    "so reissuing it would make two different documents answer to one name. RFC-0014 "
    "was once listed as a third gap, but it was later issued as a real document. A new "
    "record takes the next number after the highest one in use."
)


def parse_record(series, path):
    """Return the index row for one record file, or raise ValueError."""
    text = path.read_text(encoding="utf-8")
    keys = KEY_RE.findall(text)
    if len(keys) != 1:
        raise ValueError(f"{path.relative_to(ROOT)}: expected one status key, found {len(keys)}")
    fields = {}
    for part in keys[0].split(";"):
        name, _, value = part.partition(":")
        name, value = name.strip(), value.strip()
        if name in fields or name not in ("status",) + LINK_KEYS or not value:
            raise ValueError(f"{path.relative_to(ROOT)}: bad status-key field {part.strip()!r}")
        fields[name] = value
    if fields.get("status") not in STATUSES:
        raise ValueError(f"{path.relative_to(ROOT)}: status must be one of {', '.join(STATUSES)}")
    title = re.search(r"^# (.+)$", text, re.M).group(1).strip()
    title = re.sub(r"^(RFC \d{4} — |ADR-\d{4} — |\d+\. )", "", title)
    return {
        "id": f"{series}-{path.name[:4]}",
        "num": int(path.name[:4]),
        "series": series,
        "path": path.relative_to(OUT.parent).as_posix(),
        "title": title.replace("|", "\\|"),
        "status": fields["status"],
        **{k: [r.strip() for r in fields.get(k, "").split(",") if r.strip()] for k in LINK_KEYS},
    }


def collect():
    """Read every record and validate the cross-references between them."""
    records = []
    for series, folder in SERIES:
        for path in sorted(folder.glob("[0-9][0-9][0-9][0-9]-*.md")):
            if path.name.startswith("0000-"):
                continue  # the RFC template
            records.append(parse_record(series, path))
    by_id = {r["id"]: r for r in records}
    errors = []
    for r in records:
        for key in LINK_KEYS:
            for ref in r[key]:
                if not REF_RE.match(ref) or ref not in by_id:
                    errors.append(f"{r['id']}: {key} names unknown record {ref!r}")
        if r["superseded-by"] and r["status"] != "superseded":
            errors.append(f"{r['id']}: superseded-by is set but status is {r['status']}")
    for series, _ in SERIES:
        nums = {r["num"] for r in records if r["series"] == series}
        gaps = {(series, n) for n in range(1, max(nums) + 1) if n not in nums}
        known = {g for g in GAPS if g[0] == series}
        errors += [f"{s}-{n:04d}: numbering gap with no GAPS entry" for s, n in sorted(gaps - known)]
        errors += [f"{s}-{n:04d}: GAPS entry for a number that is in use" for s, n in sorted(known - gaps)]
    if errors:
        raise ValueError("\n".join(errors))
    return records, by_id


def links(ids, by_id, suffix=""):
    """Render a list of record ids as relative links."""
    return ", ".join(f"[{i}]({by_id[i]['path']}){suffix}" for i in ids)


def render():
    """Return the full index page as a string."""
    records, by_id = collect()
    supersedes = {r["id"]: [] for r in records}
    for r in records:
        for key, suffix in (("superseded-by", ""), ("superseded-in-part-by", " (part)")):
            for ref in r[key]:
                supersedes[ref].append((r["id"], suffix))
    out = [
        "<!-- GENERATED by tools/gen_record_index.py from each record's status key. "
        "Do not edit; run the script. -->",
        "",
        "# ADR and RFC index",
        "",
        "Every architecture decision record ([`docs/adr/`](adr/)) and spec-change proposal "
        "([`docs/spec/rfcs/`](spec/rfcs/)) with its status and supersession links. The page is "
        "generated from the status key under each record's title, and CI fails when it drifts. "
        "To change a row, edit the record's key and run `python3 tools/gen_record_index.py`.",
        "",
        "RFCs are change proposals and history, not the standard. The normative specification "
        "is [Protocol v1](spec/v1.md) and the annexes its [§3](spec/v1.md#3-wire-format) "
        "incorporates; an accepted RFC records why a clause reads as it does, and the clause "
        "itself lives in the specification. Where the two differ, the specification wins. "
        "ADRs carry the rationale behind the reference implementation and are not normative "
        "either.",
        "",
        "Statuses: " + ", ".join(f"`{s}`" for s in STATUSES) + ". A record marked "
        "*(part)* still stands except for the section the other record replaced; read both.",
    ]
    for series, title in (("ADR", "Architecture decision records"), ("RFC", "RFCs")):
        out += ["", f"## {title}", "", "| Number | Title | Status | Superseded by | Supersedes |",
                "| ---- | ---- | ---- | ---- | ---- |"]
        for r in (r for r in records if r["series"] == series):
            by = ", ".join(x for x in (links(r["superseded-by"], by_id),
                                       links(r["superseded-in-part-by"], by_id, " (part)")) if x)
            sup = ", ".join(f"[{i}]({by_id[i]['path']}){s}" for i, s in supersedes[r["id"]])
            out.append(f"| [{r['id']}]({r['path']}) | {r['title']} | {r['status']} | {by} | {sup} |")
    out += ["", "## Numbering gaps", "", "| Number | Why it is not issued |", "| ---- | ---- |"]
    out += [f"| {s}-{n:04d} | {why} |" for (s, n), why in sorted(GAPS.items())]
    out += ["", GAP_NOTE, ""]
    return "\n".join(out)


def main():
    """Write the index, or with --check compare it against the committed file."""
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if the committed index is stale")
    args = ap.parse_args()
    try:
        page = render()
    except ValueError as e:
        print(f"gen_record_index: {e}", file=sys.stderr)
        return 1
    if args.check:
        if not OUT.exists() or OUT.read_text(encoding="utf-8") != page:
            print(f"gen_record_index: {OUT.relative_to(ROOT)} is stale; "
                  "run `python3 tools/gen_record_index.py` and commit the result", file=sys.stderr)
            return 1
        return 0
    OUT.write_text(page, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
