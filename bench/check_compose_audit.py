#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Assert the audit pass of ``run_compose.sh`` — deliveries and wire use, never a rate.

Issue #1151. ``bench/run_compose.sh`` is ~700 lines of shell and C++ whose value is its
*construction* — two processes, deliveries counted at the receiver, the publisher audited
for real wire use — and until now nothing executed it, so a refactor could break any of the
three properties with every check still green.

What this asserts, per width, for the **libtracer** arm:

1. ``SYSCALL_AUDIT ... verdict=PASS`` — the publisher reached the wire. ``UNAVAILABLE`` (no
   ``strace``) is a FAILURE here, not a pass: a guard that silently downgrades to measuring
   nothing is the exact shape this issue exists to remove.
2. ``COMPOSE_AUDIT ... bad=0 ... PASS`` — values arrived and none were malformed.
3. ``values == messages * K`` — composition actually happened *on the wire*. This is the one
   assertion that reads the composite send itself: drop it and K values arrive as K messages.
4. no ``COMPOSE_POINT_FAIL`` for the libtracer arm.

The Zenoh arm is optional by design (``bench/fetch_zenoh.sh`` vendors it), matching how
``perf.yml`` treats the Zenoh control series: its rows are checked when present and its
absence is never a failure.

No rate is read, and none is published — the audit pass runs the publisher under ``strace``,
where a timing would mean nothing.

    ./check_compose_audit.py --widths 1 4 --log audit.log
"""

import argparse
import re
import sys

ARM = "libtracer"


def parse(text):
    """@brief Split the harness transcript into the three record kinds this gate reads."""
    syscall, compose, fails = [], [], []
    for line in text.splitlines():
        f = line.rstrip("\n").split("\t")
        if f[0] == "SYSCALL_AUDIT":
            kv = dict(p.split("=", 1) for p in f[2:] if "=" in p)
            syscall.append({"label": f[1] if len(f) > 1 else "", **kv})
        elif f[0] == "COMPOSE_AUDIT":
            kv = dict(p.split("=", 1) for p in f[4:] if "=" in p)
            compose.append(
                {
                    "system": f[1] if len(f) > 1 else "",
                    "mode": f[2] if len(f) > 2 else "",
                    "width": f[3] if len(f) > 3 else "",
                    "verdict": f[-1],
                    **kv,
                }
            )
        elif f[0] == "COMPOSE_POINT_FAIL":
            fails.append(line)
    return syscall, compose, fails


def check(text, widths):
    """@brief Return the list of failure strings; empty means the audit stands."""
    syscall, compose, fails = parse(text)
    bad = []

    for line in fails:
        if f"\t{ARM}\t" in line:
            bad.append(f"harness reported a failed point: {line.strip()}")

    for k in widths:
        # 1 — the publisher reached the wire. The label is "<engine>-K<width>".
        label = f"{ARM}-K{k}"
        rows = [r for r in syscall if r["label"] == label]
        if not rows:
            bad.append(f"K={k}: no SYSCALL_AUDIT row for {label} — the audit pass did not run")
        for r in rows:
            if r.get("verdict") != "PASS":
                bad.append(
                    f"K={k}: SYSCALL_AUDIT verdict={r.get('verdict')} "
                    f"(sends={r.get('sends')} floor={r.get('floor')})"
                )

        # 2 and 3 — values arrived intact, and they arrived COMPOSED.
        rows = [r for r in compose if r["system"] == ARM and r["width"] == str(k)]
        if not rows:
            bad.append(f"K={k}: no COMPOSE_AUDIT row for {ARM} — the receiver counted nothing")
        for r in rows:
            if r["verdict"] != "PASS" or r.get("bad") != "0":
                bad.append(
                    f"K={k}: COMPOSE_AUDIT {r['verdict']} "
                    f"(values={r.get('values')} bad={r.get('bad')})"
                )
                continue
            messages = int(r.get("messages", 0))
            values = int(r.get("values", 0))
            if messages == 0:
                bad.append(f"K={k}: COMPOSE_AUDIT messages=0 — nothing was observed on the wire")
            elif values != messages * k:
                bad.append(
                    f"K={k}: values/message = {values}/{messages} — expected exactly {k} "
                    f"values per message; composition did not happen on the wire"
                )
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--widths", type=int, nargs="+", required=True)
    ap.add_argument("--log", required=True, help="run_compose.sh transcript (stdout+stderr)")
    args = ap.parse_args()

    with open(args.log, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()

    bad = check(text, args.widths)
    if bad:
        print("COMPOSE_AUDIT_GATE\tFAIL")
        for b in bad:
            print(f"  - {b}")
        return 1
    print(f"COMPOSE_AUDIT_GATE\tPASS\twidths={' '.join(str(k) for k in args.widths)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
