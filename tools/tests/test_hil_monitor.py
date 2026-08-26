#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the ESP32-C6 HIL verdict reader and image stager (#1557).

WHY THESE EXIST WITHOUT HARDWARE. The HIL job's judgement is a pure function over
a serial transcript (`hil_monitor.analyse`) and a pure function over a build
manifest (`hil_flash.esptool_argv`); the serial port and the subprocess are the
parts that contain no decisions. So every rule the on-silicon gate enforces is
testable on a hosted runner — and it must be, because the ONE machine that could
test it end to end is the one the gate is meant to police.

The transcripts below are the real shapes: a healthy `full_node` boot, the #1532
reset loop (a SILENT-level IDF assert prints nothing — the only evidence is the
second ROM banner), a panic, and a boot that reached `app_main` and then failed a
wire leg.

`version-consistency.yml` runs `unittest discover -s tools/tests` on a bare python
with no pip install, so importing `hil_monitor` must not pull in `pyserial`. That
constraint is itself asserted below.
"""

from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "hil"))

import hil_flash  # noqa: E402
import hil_monitor  # noqa: E402

HEALTHY = """\
ESP-ROM:esp32c6-20220919
Build:Sep 19 2022
SPIWP:0xee
entry 0x40800000
I (25) boot: ESP-IDF v6.0 2nd stage bootloader
I (300) cpu_start: Starting scheduler.
libtracer full node (ESP-IDF) starting
device node up: /sensor/temp + udp listener /net/udp-server/host (port 47301), one-slab recipe
host peer: dialing the device node over real datagrams (127.0.0.1:47301)
  [PASS] SPEC{name=dev, kind=udp, 127.0.0.1} at /net/udp-client/conn creates the dialing connection
  [PASS] FWD{READ} round-trip: /dev/sensor/temp == 21
  [PASS] a durability_request subscribe latched the current value
  [PASS] device: write /sensor/temp = 22
  [PASS] remote fan-out delivered 22 to the host peer (await fired)
label source: 384/2048 B used, 3/12 size classes, 0 block(s) overflowed
full_node self-proof: OK (0 failures)
"""

BOOT_LOOP = """\
ESP-ROM:esp32c6-20220919
I (300) cpu_start: Starting scheduler.
libtracer full node (ESP-IDF) starting
ESP-ROM:esp32c6-20220919
I (300) cpu_start: Starting scheduler.
libtracer full node (ESP-IDF) starting
ESP-ROM:esp32c6-20220919
"""

PANIC = """\
ESP-ROM:esp32c6-20220919
libtracer full node (ESP-IDF) starting
assert failed: pthread_self pthread.c:428 (res == pdTRUE)
Backtrace: 0x40800000
"""

WIRE_FAILURE = """\
ESP-ROM:esp32c6-20220919
libtracer full node (ESP-IDF) starting
device node up: /sensor/temp + udp listener /net/udp-server/host (port 47301)
  [PASS] SPEC{name=dev, kind=udp, 127.0.0.1} at /net/udp-client/conn creates the dialing connection
  [FAIL] FWD{READ} round-trip: /dev/sensor/temp == 21
  [PASS] a durability_request subscribe latched the current value
full_node self-proof: FAILED (1 failure)
"""

TRUNCATED = """\
ESP-ROM:esp32c6-20220919
libtracer full node (ESP-IDF) starting
device node up: /sensor/temp + udp listener /net/udp-server/host (port 47301)
  [PASS] SPEC{name=dev, kind=udp, 127.0.0.1} at /net/udp-client/conn creates the dialing connection
"""

MANIFEST = {
    "write_flash_args": ["--flash_mode", "dio", "--flash_size", "2MB", "--flash_freq", "80m"],
    "flash_files": {
        "0x0": "bootloader/bootloader.bin",
        "0x10000": "full_node.bin",
        "0x8000": "partition_table/partition-table.bin",
    },
    "extra_esptool_args": {"after": "hard_reset", "before": "default_reset", "stub": True,
                           "chip": "esp32c6"},
}


class AnalyseTest(unittest.TestCase):
    """@brief The verdict rules, one test per way an on-silicon boot can go wrong."""

    def test_healthy_boot_passes_with_no_reasons(self):
        """@brief The whole point: banner + node up + OK verdict + one ROM banner == pass."""
        r = hil_monitor.analyse(HEALTHY)
        self.assertTrue(r["pass"], r["reasons"])
        self.assertEqual(r["reasons"], [])
        self.assertTrue(r["reached_app_main"])
        self.assertTrue(r["node_up"])
        self.assertTrue(r["self_proof_ok"])
        self.assertEqual(r["self_proof_failures"], 0)
        self.assertEqual(r["rom_banners"], 1)
        self.assertEqual(len(r["checks"]), 5)
        self.assertTrue(all(c["ok"] for c in r["checks"]))

    def test_boot_loop_fails_even_though_app_main_was_reached(self):
        """@brief The #1532 signature. A silent reset prints no error — the SECOND banner is it."""
        r = hil_monitor.analyse(BOOT_LOOP)
        self.assertFalse(r["pass"])
        self.assertEqual(r["rom_banners"], 3)
        self.assertEqual(r["app_main_entries"], 2)
        self.assertTrue(any("boot loop" in reason for reason in r["reasons"]))

    def test_a_second_life_that_passes_is_still_a_failure(self):
        """@brief A node that reset and then printed OK must not launder the reset into a pass."""
        r = hil_monitor.analyse(BOOT_LOOP + HEALTHY)
        self.assertFalse(r["pass"])
        self.assertTrue(r["self_proof_ok"])  # the OK line IS there
        self.assertTrue(any("boot loop" in reason for reason in r["reasons"]))

    def test_panic_is_named_in_the_reasons(self):
        """@brief A diagnosed death is reported as itself, not as "no OK line"."""
        r = hil_monitor.analyse(PANIC)
        self.assertFalse(r["pass"])
        self.assertIn("assert failed", r["panic_markers"])
        self.assertTrue(any("panic on silicon" in reason for reason in r["reasons"]))

    def test_wire_failure_names_the_failing_leg(self):
        """@brief Boot fine, wire broken — the FAILING leg's own text lands in the reason."""
        r = hil_monitor.analyse(WIRE_FAILURE)
        self.assertFalse(r["pass"])
        self.assertTrue(r["reached_app_main"])
        self.assertTrue(r["node_up"])
        self.assertFalse(r["self_proof_ok"])
        self.assertEqual(r["self_proof_failures"], 1)
        self.assertTrue(any("FWD{READ} round-trip" in reason for reason in r["reasons"]))

    def test_truncated_run_is_a_timeout_not_a_pass(self):
        """@brief No verdict line means no verdict — never an implicit pass."""
        r = hil_monitor.analyse(TRUNCATED)
        self.assertFalse(r["pass"])
        self.assertFalse(r["self_proof_seen"])
        self.assertTrue(any("never terminated" in reason for reason in r["reasons"]))

    def test_silence_is_a_failure_of_its_own_kind(self):
        """@brief An empty capture is "the device is not there", distinct from a crash."""
        r = hil_monitor.analyse("")
        self.assertFalse(r["pass"])
        self.assertTrue(any("no console output" in reason for reason in r["reasons"]))

    def test_never_reached_app_main(self):
        """@brief The bootloader ran and the app did not — bring-up broke before libtracer."""
        r = hil_monitor.analyse("ESP-ROM:esp32c6-20220919\nI (25) boot: ESP-IDF v6.0\n")
        self.assertFalse(r["pass"])
        self.assertTrue(any("never reached app_main" in reason for reason in r["reasons"]))

    def test_node_never_came_up(self):
        """@brief app_main entered, bring-up returned false — a distinct, nameable failure."""
        r = hil_monitor.analyse(
            "ESP-ROM:esp32c6-20220919\nlibtracer full node (ESP-IDF) starting\n"
            "FAIL: device node bring-up\n")
        self.assertFalse(r["pass"])
        self.assertTrue(any("never brought the node up" in reason for reason in r["reasons"]))

    def test_summary_renders_the_verdict_and_the_legs(self):
        """@brief The step summary must carry the evidence, not only the boolean."""
        text = hil_monitor.render_summary(hil_monitor.analyse(WIRE_FAILURE), "/dev/hil-esp32c6")
        self.assertIn("FAIL", text)
        self.assertIn("/dev/hil-esp32c6", text)
        self.assertIn("FWD{READ} round-trip", text)

    def test_cli_can_judge_a_transcript_offline(self):
        """@brief `--analyse-file` re-reads an uploaded transcript with no hardware in the loop."""
        with tempfile.TemporaryDirectory() as tmp, io.StringIO() as quiet:
            # The CLI prints the whole summary; captured so the suite stays readable.
            with contextlib.redirect_stdout(quiet):
                src = Path(tmp) / "t.txt"
                src.write_text(HEALTHY, encoding="utf-8")
                out = Path(tmp) / "r.json"
                rc = hil_monitor.main(["--analyse-file", str(src), "--json", str(out)])
                src.write_text(BOOT_LOOP, encoding="utf-8")
                rc_bad = hil_monitor.main(["--analyse-file", str(src)])
            self.assertEqual(rc, 0)
            self.assertTrue(json.loads(out.read_text())["pass"])
            self.assertEqual(rc_bad, 1)
            self.assertIn("::error::", quiet.getvalue())

    def test_module_imports_without_pyserial(self):
        """@brief version-consistency runs this suite on a bare python — the import must be lazy."""
        self.assertNotIn("serial", sys.modules)


class FlashArgvTest(unittest.TestCase):
    """@brief The flash argument vector is DERIVED from the build's manifest, never transcribed."""

    def test_offsets_are_ascending_numerically_not_lexically(self):
        """@brief `0x10000` sorts before `0x8000` as TEXT; flashing in that order is wrong."""
        argv = hil_flash.esptool_argv(MANIFEST, "/dev/hil-esp32c6")
        offsets = [a for a in argv if a.startswith("0x")]
        self.assertEqual(offsets, ["0x0", "0x8000", "0x10000"])

    def test_chip_port_and_reset_behaviour_come_from_the_manifest(self):
        """@brief Nothing about the layout is hardcoded in the tool."""
        argv = hil_flash.esptool_argv(MANIFEST, "/dev/ttyACM0", baud=115200)
        self.assertEqual(argv[:6], ["--chip", "esp32c6", "--port", "/dev/ttyACM0",
                                    "--baud", "115200"])
        self.assertIn("--before", argv)
        self.assertIn("default_reset", argv)
        self.assertIn("hard_reset", argv)
        self.assertIn("write_flash", argv)
        self.assertNotIn("--no-stub", argv)
        for flag in ("--flash_mode", "dio", "--flash_size", "2MB"):
            self.assertIn(flag, argv)

    def test_no_stub_is_emitted_when_the_manifest_disables_it(self):
        """@brief The one esptool flag whose ABSENCE is the default and whose presence is opt-in."""
        manifest = json.loads(json.dumps(MANIFEST))
        manifest["extra_esptool_args"]["stub"] = False
        self.assertIn("--no-stub", hil_flash.esptool_argv(manifest, "/dev/x"))

    def test_stage_copies_every_named_file_at_its_relative_path(self):
        """@brief A staged image must be flashable as-is: the manifest's relative paths survive."""
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp) / "build"
            (build / "bootloader").mkdir(parents=True)
            (build / "partition_table").mkdir(parents=True)
            (build / "flasher_args.json").write_text(json.dumps(MANIFEST), encoding="utf-8")
            for rel in MANIFEST["flash_files"].values():
                (build / rel).write_bytes(b"\x00" * 16)

            out = Path(tmp) / "image"
            staged = hil_flash.stage(build, out, {"note": "unit"})
            self.assertEqual(sorted(staged), sorted(MANIFEST["flash_files"].values()))
            for rel in MANIFEST["flash_files"].values():
                self.assertTrue((out / rel).is_file(), rel)
            self.assertTrue((out / "flasher_args.json").is_file())
            self.assertEqual(json.loads((out / "image.json").read_text())["note"], "unit")

    def test_stage_refuses_a_manifest_whose_payload_was_not_built(self):
        """@brief A half-staged image flashes a device into the very boot loop the job detects."""
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp) / "build"
            build.mkdir()
            (build / "flasher_args.json").write_text(json.dumps(MANIFEST), encoding="utf-8")
            with self.assertRaises(SystemExit):
                hil_flash.stage(build, Path(tmp) / "image")


if __name__ == "__main__":
    unittest.main()
