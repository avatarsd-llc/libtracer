#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Capture an ESP32-C6's serial console and render a BOOLEAN verdict on the boot (#1557, #1532).

WHAT THIS IS FOR. `esp-idf.yml` proves the ESP plane COMPILES and LINKS, and since
#1532 it even reads the objects for the one call that only fails on silicon
(`pthread_self` from an unregistered task). What no hosted runner can do is
*execute* an instruction on the chip. This is the reader for the job that does:
flash a `full_node` image onto a real ESP32-C6, watch its console, and answer one
question — did the node reach `app_main` and answer a write over the wire?

WHY THE `full_node` EXAMPLE IS THE PROBE. On a chip with no Wi-Fi SSID configured
(the default), `full_node`'s `app_main` runs the SAME self-proof CI runs on the
`linux` target: a device node plus an in-process host peer talking over REAL
datagrams through lwIP's loopback netif — `FWD{READ}` round trip, a
`:subscribers[]` subscribe write, a durability latch, and a producer fan-out
observed with `graph.await`. So the boolean this file reports is not "the chip
printed something"; it is "the chip resolved a path, answered a READ over the
wire, accepted a WRITE, and fanned a delivery back" — which is exactly the
on-silicon claim #1532 is held open for.

WHAT COUNTS AS A FAILURE, and why each one is named separately rather than folded
into "no OK line":

  * **no console output at all** — the device is not there, the port is wrong, or
    the flash did not take. Distinct from a crash: nothing ran.
  * **a boot loop** — the ROM banner appearing more than once inside one capture
    window. This is the #1532 SHAPE precisely: an IDF assert under the default
    SILENT assert level is a bare reset, with no message, and the only evidence is
    that the boot happens again. A run that reaches `app_main` twice is a failure
    even if the second attempt prints an OK line.
  * **a panic** — `Guru Meditation`, `assert failed`, `abort() was called`,
    `StoreProhibited`, a `Backtrace:`. Named because the transcript then carries
    a diagnosis and the summary should surface it rather than bury it.
  * **reached `app_main` but the self-proof FAILED** — the wire round trip is what
    broke, not the boot. The per-check `[PASS]`/`[FAIL]` lines are carried into
    the result so the failing leg names itself.
  * **timed out mid-run** — output started and the terminating line never came.

STDLIB ONLY AT IMPORT TIME. `version-consistency.yml` runs
`unittest discover -s tools/tests` on a bare python with no pip install, so
`pyserial` is imported INSIDE `read_console` rather than at module scope. Every
line of judgement lives in @ref analyse, which is a pure function over the
transcript text and is what the unit tests exercise — the serial plumbing needs
hardware and is therefore the part that must contain no decisions.

Usage:
    python3 tools/hil/hil_monitor.py --port /dev/hil-esp32c6 \
        --transcript hil-transcript.txt --json hil-result.json
    python3 tools/hil/hil_monitor.py --analyse-file transcript.txt   # offline re-read
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from pathlib import Path

DEFAULT_PORT = "/dev/hil-esp32c6"
"""@brief The stable device path the shipped udev rule creates (tools/hil/99-libtracer-hil.rules)."""

DEFAULT_BAUD = 115200
"""@brief ESP-IDF's default console baud rate; `full_node` does not change it."""

BANNER = "libtracer full node (ESP-IDF) starting"
"""@brief First line of `app_main`. Its presence IS "the node reached app_main"."""

NODE_UP = "device node up:"
"""@brief Printed after bring-up: the sensor vertex exists and the udp listener was config-created."""

SELF_PROOF_RE = re.compile(r"full_node self-proof: (OK|FAILED) \((\d+) failures?\)")
"""@brief The terminating verdict line. Reading it ends the capture — nothing after it is evidence."""

CHECK_RE = re.compile(r"\[(PASS|FAIL)\]\s+(.*?)\s*$")
"""@brief One self-proof leg. Carried into the result so a failure names the leg, not just the count."""

ROM_BANNER_RE = re.compile(r"ESP-ROM:esp32")
"""@brief The mask-ROM banner. TWO of them in one window is a reset loop — the #1532 signature."""

PANIC_MARKERS = (
    "Guru Meditation",
    "assert failed",
    "abort() was called",
    "StoreProhibited",
    "LoadProhibited",
    "IllegalInstruction",
    "Backtrace:",
    "rst cause:panic",
)
"""@brief Substrings that mean the chip diagnosed its own death. Any one of them fails the run."""


def analyse(text: str) -> dict:
    """@brief Turn a raw serial transcript into the verdict record. PURE — the whole judgement.

    Returns a dict with a `pass` boolean, the individual observations behind it, and
    a `reasons` list naming every failure in the author's words. `reasons` is empty
    exactly when `pass` is true.
    """
    lines = text.splitlines()
    rom_banners = sum(1 for line in lines if ROM_BANNER_RE.search(line))
    app_mains = sum(1 for line in lines if BANNER in line)
    panics = sorted({m for m in PANIC_MARKERS if m in text})
    checks = [
        {"ok": m.group(1) == "PASS", "what": m.group(2)}
        for m in (CHECK_RE.search(line) for line in lines)
        if m
    ]

    proof = SELF_PROOF_RE.search(text)
    result = {
        "pass": False,
        "reached_app_main": app_mains > 0,
        "node_up": any(NODE_UP in line for line in lines),
        "self_proof_seen": proof is not None,
        "self_proof_ok": bool(proof) and proof.group(1) == "OK",
        "self_proof_failures": int(proof.group(2)) if proof else None,
        "rom_banners": rom_banners,
        "app_main_entries": app_mains,
        "panic_markers": panics,
        "checks": checks,
        "lines": len(lines),
        "reasons": [],
    }

    reasons: list[str] = result["reasons"]
    if not text.strip():
        reasons.append(
            "no console output at all — the device did not run, the port is wrong, "
            "or the flash did not take"
        )
    # The boot-loop test comes BEFORE the self-proof test on purpose: a node that
    # reset and then passed on its second life is a #1532 failure, not a pass.
    if rom_banners > 1 or app_mains > 1:
        reasons.append(
            f"boot loop — {rom_banners} ROM banner(s) and {app_mains} app_main entry/entries in "
            f"one capture window (the #1532 signature: a SILENT-level IDF assert is a bare reset)"
        )
    if panics:
        reasons.append("panic on silicon: " + ", ".join(panics))
    if text.strip() and not result["reached_app_main"]:
        reasons.append(f"never reached app_main — the banner {BANNER!r} never appeared")
    elif not result["node_up"]:
        reasons.append(
            f"reached app_main but never brought the node up — {NODE_UP!r} never appeared "
            "(bring-up returned false: vertex registration or the in-band udp listener SPEC write)"
        )
    elif not result["self_proof_seen"]:
        reasons.append(
            "reached app_main and brought the node up, but the self-proof never terminated "
            "— captured until the timeout with no verdict line"
        )
    elif not result["self_proof_ok"]:
        failed = [c["what"] for c in checks if not c["ok"]]
        reasons.append(
            f"the on-silicon self-proof FAILED with {result['self_proof_failures']} failure(s): "
            + ("; ".join(failed) if failed else "no [FAIL] leg was captured")
        )

    result["pass"] = not reasons
    return result


def render_summary(result: dict, port: str) -> str:
    """@brief Markdown for `$GITHUB_STEP_SUMMARY` — the verdict, then the evidence behind it."""
    verdict = "PASS" if result["pass"] else "FAIL"
    out = [
        "### ESP32-C6 HIL — `full_node` boot repro (#1532)",
        "",
        f"**{verdict}** — port `{port}`, {result['lines']} console line(s) captured.",
        "",
        "| observation | value |",
        "| --- | --- |",
        f"| reached `app_main` | {result['reached_app_main']} |",
        f"| node brought up | {result['node_up']} |",
        f"| self-proof verdict | {'OK' if result['self_proof_ok'] else 'not OK'} |",
        f"| self-proof failures | {result['self_proof_failures']} |",
        f"| ROM banners (>1 = boot loop) | {result['rom_banners']} |",
        f"| panic markers | {', '.join(result['panic_markers']) or 'none'} |",
    ]
    if result["checks"]:
        out += ["", "| leg | result |", "| --- | --- |"]
        out += [
            f"| {c['what']} | {'PASS' if c['ok'] else 'FAIL'} |" for c in result["checks"]
        ]
    if result["reasons"]:
        out += ["", "Why it failed:", ""] + [f"- {r}" for r in result["reasons"]]
    return "\n".join(out) + "\n"


def reset_target(ser) -> None:
    """@brief Pulse the devkit's auto-reset lines so the capture starts at the ROM banner.

    RTS drives EN (reset) and DTR drives IO0 (boot mode) through the devkit's
    two-transistor circuit; holding IO0 HIGH (dtr=False) while pulsing EN reboots
    into the APP rather than the download stub. On a board whose console is the
    chip's built-in USB-Serial/JTAG the same CDC control lines carry the same
    meaning, so one sequence covers both wirings.

    Best effort by construction: a port that refuses the control lines still yields
    a usable capture (the flash step already left the chip resetting), and refusing
    to monitor because a line could not be toggled would trade a real observation
    for a plumbing complaint.
    """
    try:
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
        time.sleep(0.05)
    except Exception as exc:  # noqa: BLE001 — see the docstring: best effort, never fatal
        print(f"::warning::could not toggle the reset lines on the port ({exc}) — "
              "capturing whatever the running image prints")


def read_console(port: str, baud: int, timeout_s: float, do_reset: bool = True) -> str:
    """@brief Capture the console until the self-proof verdict line lands or `timeout_s` elapses.

    The read stops on the TERMINATING line rather than always burning the full
    window: on a healthy node the whole boot plus self-proof is a few seconds, and a
    job that always waited out its timeout would make the device the bottleneck it
    is not. A short grace read follows the verdict so the trailing census lines make
    it into the transcript.

    `pyserial` is imported HERE and not at module scope — see the module docstring.
    """
    import serial  # noqa: PLC0415 — deliberate: keeps the module importable without pyserial

    chunks: list[str] = []
    deadline = time.monotonic() + timeout_s
    with serial.Serial(port, baud, timeout=0.5) as ser:
        if do_reset:
            reset_target(ser)
        grace_until = None
        while time.monotonic() < deadline:
            data = ser.read(4096)
            if data:
                chunks.append(data.decode("utf-8", errors="replace"))
                if grace_until is None and SELF_PROOF_RE.search("".join(chunks)):
                    # The verdict landed; take one more second so the trailing
                    # label-source census is in the transcript, then stop.
                    grace_until = time.monotonic() + 1.0
            if grace_until is not None and time.monotonic() >= grace_until:
                break
    return "".join(chunks)


def _emit(result: dict, port: str, transcript: str, args) -> int:
    """@brief Write every output this job publishes and return the process exit code."""
    if args.transcript:
        Path(args.transcript).write_text(transcript, encoding="utf-8")
    if args.json:
        Path(args.json).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")

    summary = render_summary(result, port)
    print(summary)
    step_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if step_summary:
        with open(step_summary, "a", encoding="utf-8") as fh:
            fh.write(summary)
    step_output = os.environ.get("GITHUB_OUTPUT")
    if step_output:
        with open(step_output, "a", encoding="utf-8") as fh:
            fh.write(f"pass={'true' if result['pass'] else 'false'}\n")

    if result["pass"]:
        print("::notice::on-silicon boot repro PASSED — the C6 reached app_main and "
              "answered a write over the wire (#1532)")
        return 0
    for reason in result["reasons"]:
        print(f"::error::on-silicon boot repro FAILED: {reason}")
    return 1


def main(argv: list[str] | None = None) -> int:
    """@brief CLI entry point: capture (or re-read) a transcript and render the verdict."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", default=os.environ.get("HIL_PORT", DEFAULT_PORT))
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--timeout", type=float, default=90.0,
                        help="seconds to capture before giving up on the verdict line")
    parser.add_argument("--no-reset", action="store_true",
                        help="do not pulse DTR/RTS before capturing")
    parser.add_argument("--transcript", default=None, help="write the raw console capture here")
    parser.add_argument("--json", default=None, help="write the machine-readable verdict here")
    parser.add_argument("--analyse-file", default=None,
                        help="skip the serial port and judge an existing transcript")
    args = parser.parse_args(argv)

    if args.analyse_file:
        transcript = Path(args.analyse_file).read_text(encoding="utf-8", errors="replace")
    else:
        try:
            transcript = read_console(args.port, args.baud, args.timeout, not args.no_reset)
        except Exception as exc:  # noqa: BLE001 — a port failure is a RESULT here, not a traceback
            print(f"::error::could not open the HIL console on {args.port}: {exc}")
            print(f"::error::check the udev rule (tools/hil/99-libtracer-hil.rules) and that the "
                  f"devkit is plugged in")
            return 1
    return _emit(analyse(transcript), args.port, transcript, args)


if __name__ == "__main__":
    sys.exit(main())
