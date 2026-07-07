#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Single-source-of-truth version stamper for the libtracer core release axis.

The repo-root ``VERSION`` file is the *sole hand-edited* version for the C++
reference core and the platform packaging that ships it. This script stamps that
version into the static package manifests that cannot read it themselves:

  - ``library.json``                                     (PlatformIO)
  - ``integrations/arduino/library.properties``          (Arduino)
  - ``integrations/esp-idf/libtracer/idf_component.yml`` (ESP Component Registry)

``core/CMakeLists.txt`` reads ``VERSION`` directly (and a release git tag wins
over it, so tagged provenance is exact), so it is *not* stamped here. The Rust,
TypeScript, and ROS 2 bindings version independently on their own cadence and are
intentionally left untouched.

Usage::

  tools/sync-version.py           # rewrite the manifests to match VERSION
  tools/sync-version.py --check   # exit 1 if any manifest drifts (the CI gate)

The rewrite is a targeted, format-preserving substitution of just the version
substring — it never reserializes the file, so comments, key order, and
indentation are preserved.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
VERSION_FILE = ROOT / "VERSION"

# Each target: (path, regex with a named group `ver` around the version literal,
# human label). Only the `ver` span is replaced, so surrounding formatting is
# untouched.
TARGETS = [
    (
        ROOT / "library.json",
        re.compile(r'"version"\s*:\s*"(?P<ver>[^"]*)"'),
        "library.json (PlatformIO)",
    ),
    (
        ROOT / "integrations/arduino/library.properties",
        re.compile(r"(?m)^version=(?P<ver>.*)$"),
        "integrations/arduino/library.properties (Arduino)",
    ),
    (
        ROOT / "integrations/esp-idf/libtracer/idf_component.yml",
        re.compile(r'(?m)^version:\s*"(?P<ver>[^"]*)"'),
        "integrations/esp-idf/libtracer/idf_component.yml (ESP Component Registry)",
    ),
]

# MAJOR.MINOR.PATCH with an optional pre-release / build suffix (e.g. 0.3.0-rc1).
SEMVER = re.compile(r"^\d+\.\d+\.\d+([-+.][0-9A-Za-z.-]+)?$")


def read_version():
    if not VERSION_FILE.exists():
        sys.exit(f"error: {VERSION_FILE.relative_to(ROOT)} not found")
    version = VERSION_FILE.read_text(encoding="utf-8").strip()
    if not SEMVER.match(version):
        sys.exit(
            f"error: VERSION {version!r} is not MAJOR.MINOR.PATCH[-suffix]"
        )
    return version


def run(check):
    version = read_version()
    drift = []
    for path, pattern, label in TARGETS:
        if not path.exists():
            sys.exit(f"error: manifest not found: {path.relative_to(ROOT)}")
        text = path.read_text(encoding="utf-8")
        match = pattern.search(text)
        if not match:
            sys.exit(f"error: no version field found in {path.relative_to(ROOT)}")
        current = match.group("ver")
        if current == version:
            continue
        if check:
            drift.append((label, current))
        else:
            new_text = text[: match.start("ver")] + version + text[match.end("ver") :]
            path.write_text(new_text, encoding="utf-8")
            print(f"updated {label}: {current} -> {version}")

    if check:
        if drift:
            print(f"version drift from VERSION={version}:")
            for label, current in drift:
                print(f"  - {label}: has {current!r}, expected {version!r}")
            print("\nfix: python3 tools/sync-version.py")
            sys.exit(1)
        print(f"ok: all core-axis manifests match VERSION={version}")
    else:
        print(f"done: core-axis manifests stamped to {version}")


if __name__ == "__main__":
    extra = [a for a in sys.argv[1:] if a != "--check"]
    if extra:
        sys.exit(f"usage: {sys.argv[0]} [--check]  (unexpected: {' '.join(extra)})")
    run(check="--check" in sys.argv[1:])
