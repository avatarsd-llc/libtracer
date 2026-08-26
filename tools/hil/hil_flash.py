#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Stage an ESP-IDF build's flashable image, and flash it onto the HIL device (#1557).

THE SPLIT THIS FILE EXISTS TO SERVE. Building `full_node` needs the multi-GB
ESP-IDF toolchain image; flashing it needs a USB cable. Only the second is
singular, so the two are DIFFERENT JOBS on different runners: a hosted
`container: espressif/idf` job builds and STAGES, the self-hosted device job
downloads the staged image and FLASHES. The device host therefore never installs
ESP-IDF — its whole dependency is `esptool` from pip — which is what keeps the one
machine with the hardware attached from also being the machine that must track a
toolchain.

WHY A STAGING STEP AND NOT "UPLOAD build/". An IDF build tree is hundreds of
megabytes of object files, and the flashable image is four files. More
importantly, `flasher_args.json` names its payloads by paths RELATIVE to the build
directory (`bootloader/bootloader.bin`), so an artifact that flattens them cannot
be flashed. @ref stage copies exactly the files that manifest names, at exactly
the paths it names them, and copies the manifest alongside — the result is a
directory that @ref esptool_argv can drive with no knowledge of how it was built.

THE ARGUMENT VECTOR IS DERIVED, NEVER TRANSCRIBED. Flash mode, flash size, flash
frequency, the chip, the reset behaviour and every (offset, file) pair come out of
the manifest the build wrote. A hand-written offset list is a second source of
truth that goes wrong silently — a partition-table move reflashes an app over a
partition table and the failure surfaces as a mystery boot loop, i.e. as the exact
symptom the job downstream exists to detect. @ref esptool_argv is a pure function
of the manifest for that reason, and it is what the unit tests pin.

esptool 4.x CLI (`write_flash`, underscored options) is what the workflow pins;
esptool 5 renamed the subcommands. The pin lives in the workflow next to the
`pip install`, not here.

Usage:
    python3 tools/hil/hil_flash.py stage --build-dir <build> --out hil-image
    python3 tools/hil/hil_flash.py flash --image hil-image --port /dev/hil-esp32c6
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

MANIFEST = "flasher_args.json"
"""@brief The manifest `idf.py build` writes; the single source of truth for the flash layout."""

DEFAULT_PORT = "/dev/hil-esp32c6"
"""@brief The stable device path the shipped udev rule creates (tools/hil/99-libtracer-hil.rules)."""

DEFAULT_BAUD = 460800
"""@brief Flash-time baud. The console runs at 115200; this is the download link, not the console."""


def esptool_argv(manifest: dict, port: str, baud: int = DEFAULT_BAUD) -> list[str]:
    """@brief Build the esptool argument vector from a build's `flasher_args.json`. PURE.

    Offsets are emitted in ASCENDING numeric order — the manifest's JSON object is
    keyed by hex STRINGS, whose textual order puts `0x10000` before `0x8000`, and an
    out-of-order write is a legal esptool invocation that flashes the wrong layout.
    """
    extra = manifest.get("extra_esptool_args", {})
    argv = ["--chip", str(extra.get("chip", "esp32c6")), "--port", port, "--baud", str(baud)]
    if extra.get("before"):
        argv += ["--before", str(extra["before"])]
    if extra.get("after"):
        argv += ["--after", str(extra["after"])]
    if extra.get("stub") is False:
        argv += ["--no-stub"]
    argv += ["write_flash"]
    argv += [str(a) for a in manifest.get("write_flash_args", [])]
    for offset, rel in sorted(manifest.get("flash_files", {}).items(), key=lambda kv: int(kv[0], 16)):
        argv += [offset, rel]
    return argv


def load_manifest(directory: Path) -> dict:
    """@brief Read `flasher_args.json` out of a build or staged-image directory."""
    path = directory / MANIFEST
    if not path.is_file():
        raise SystemExit(f"error: {path} not found — is {directory} an ESP-IDF build/staged image?")
    return json.loads(path.read_text(encoding="utf-8"))


def stage(build_dir: Path, out_dir: Path, extra_meta: dict | None = None) -> list[str]:
    """@brief Copy the manifest and every file it names into `out_dir`, preserving relative paths.

    Returns the staged relative paths. A named file that the build did not produce is
    an ERROR rather than a skip: a staged image missing its bootloader flashes a
    device into a boot loop, and the job downstream would then report the #1532
    symptom for a packaging bug.
    """
    manifest = load_manifest(build_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(build_dir / MANIFEST, out_dir / MANIFEST)

    staged: list[str] = []
    for rel in manifest.get("flash_files", {}).values():
        src = build_dir / rel
        if not src.is_file():
            raise SystemExit(f"error: {src} is named by {MANIFEST} but was not built")
        dst = out_dir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        staged.append(rel)

    meta = {"staged": staged, **(extra_meta or {})}
    (out_dir / "image.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    return staged


def flash(image_dir: Path, port: str, baud: int) -> int:
    """@brief Run esptool over a staged image. The staged directory is the CWD esptool resolves against."""
    manifest = load_manifest(image_dir)
    argv = [sys.executable, "-m", "esptool"] + esptool_argv(manifest, port, baud)
    print("+ " + " ".join(argv))
    return subprocess.call(argv, cwd=str(image_dir))


def main(argv: list[str] | None = None) -> int:
    """@brief CLI entry point: `stage` packages a build, `flash` writes a package to the device."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_stage = sub.add_parser("stage", help="package a build tree's flashable image")
    p_stage.add_argument("--build-dir", required=True)
    p_stage.add_argument("--out", required=True)
    p_stage.add_argument("--note", default="", help="free-text provenance recorded in image.json")

    p_flash = sub.add_parser("flash", help="write a staged image to the device")
    p_flash.add_argument("--image", required=True)
    p_flash.add_argument("--port", default=os.environ.get("HIL_PORT", DEFAULT_PORT))
    p_flash.add_argument("--baud", type=int, default=DEFAULT_BAUD)

    args = parser.parse_args(argv)
    if args.cmd == "stage":
        meta = {
            "note": args.note,
            "sha": os.environ.get("GITHUB_SHA", ""),
            "run": os.environ.get("GITHUB_RUN_ID", ""),
        }
        staged = stage(Path(args.build_dir), Path(args.out), meta)
        print(f"staged {len(staged)} file(s) into {args.out}: " + ", ".join(staged))
        return 0
    return flash(Path(args.image), args.port, args.baud)


if __name__ == "__main__":
    sys.exit(main())
