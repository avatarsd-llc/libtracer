#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Single-source-of-truth version stamper for a unified-lockstep libtracer release.

The repo-root ``VERSION`` file is the *sole hand-edited* version. Every publishable
artifact is versioned in lockstep from it, so one ``vX.Y.Z`` tag means ``X.Y.Z``
everywhere and no registry ever sees a version collision:

  Core packaging (static manifests that cannot read git):
    - library.json                                     (PlatformIO)
    - integrations/arduino/library.properties          (Arduino)
    - integrations/esp-idf/libtracer/idf_component.yml (ESP Component Registry)
  TypeScript packages (version + internal @avatarsd-llc/* dep ranges):
    - bindings/typescript/packages/{core,client,transport-ws,transport-webtransport}
  Rust crate:
    - bindings/rust/Cargo.toml

``core/CMakeLists.txt`` reads ``VERSION`` directly (a release git tag wins over it),
so it is not stamped here. The private TS monorepo root and the ROS 2 stub
(``bindings/ros2``, unreleased) are intentionally left alone.

Usage::

  tools/sync-version.py           # rewrite every artifact to match VERSION
  tools/sync-version.py --check   # exit 1 if anything drifts (the CI gate)

Substitutions are targeted (only the version substring / dep-range value is
replaced), so comments, key order, and formatting are preserved.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
VERSION_FILE = ROOT / "VERSION"

# MAJOR.MINOR.PATCH with an optional pre-release / build suffix (e.g. 0.3.0-rc1).
SEMVER = re.compile(r"^\d+\.\d+\.\d+([-+.][0-9A-Za-z.-]+)?$")

# --- static core manifests: replace the named-group `ver` span ----------------
CORE_MANIFESTS = [
    (ROOT / "library.json", re.compile(r'"version"\s*:\s*"(?P<ver>[^"]*)"'), "library.json (PlatformIO)"),
    (ROOT / "integrations/arduino/library.properties", re.compile(r"(?m)^version=(?P<ver>.*)$"), "arduino library.properties"),
    (ROOT / "integrations/esp-idf/libtracer/idf_component.yml", re.compile(r'(?m)^version:\s*"(?P<ver>[^"]*)"'), "esp-idf idf_component.yml"),
]

# --- TypeScript packages (non-private): version + internal dep ranges ----------
TS_PACKAGES = [
    ROOT / "bindings/typescript/packages/core/package.json",
    ROOT / "bindings/typescript/packages/client/package.json",
    ROOT / "bindings/typescript/packages/transport-ws/package.json",
    ROOT / "bindings/typescript/packages/transport-webtransport/package.json",
]
PKG_VERSION = re.compile(r'"version"\s*:\s*"(?P<ver>[^"]*)"')
# An internal @avatarsd-llc/* dependency line: key is the scoped name, value a range.
INTERNAL_DEP = re.compile(r'(?P<key>"@avatarsd-llc/[a-z0-9-]+")(?P<sep>\s*:\s*")(?P<val>[^"]*)(?P<end>")')

RUST_CARGO = ROOT / "bindings/rust/Cargo.toml"
CARGO_VERSION = re.compile(r'(?m)^version\s*=\s*"(?P<ver>[^"]*)"')


def read_version():
    if not VERSION_FILE.exists():
        sys.exit(f"error: {VERSION_FILE.relative_to(ROOT)} not found")
    version = VERSION_FILE.read_text(encoding="utf-8").strip()
    if not SEMVER.match(version):
        sys.exit(f"error: VERSION {version!r} is not MAJOR.MINOR.PATCH[-suffix]")
    return version


def dep_range(version):
    """The caret-equivalent range for an internal dep: ``>=X.Y.Z <UPPER``.

    For 0.x the compatible ceiling is the next minor; for >=1.0 the next major —
    matching the ``>=0.1.0 <0.2.0`` form the packages already use.
    """
    base = version.split("-")[0].split("+")[0]
    major, minor, _patch = (base.split(".") + ["0", "0", "0"])[:3]
    upper = f"0.{int(minor) + 1}.0" if major == "0" else f"{int(major) + 1}.0.0"
    return f">={version} <{upper}"


def run(check):
    version = read_version()
    rng = dep_range(version)
    drift = []  # (label, current, expected)
    changed = []  # (label, old, new)

    def stamp_span(path, pattern, target, label):
        if not path.exists():
            sys.exit(f"error: not found: {path.relative_to(ROOT)}")
        text = path.read_text(encoding="utf-8")
        m = pattern.search(text)
        if not m:
            sys.exit(f"error: no version field in {path.relative_to(ROOT)} ({label})")
        cur = m.group("ver")
        if cur == target:
            return
        if check:
            drift.append((label, cur, target))
        else:
            path.write_text(text[: m.start("ver")] + target + text[m.end("ver") :], encoding="utf-8")
            changed.append((label, cur, target))

    # 1. static core manifests
    for path, pattern, label in CORE_MANIFESTS:
        stamp_span(path, pattern, version, label)

    # 2. TS package versions + every internal @avatarsd-llc/* dep range
    for path in TS_PACKAGES:
        if not path.exists():
            sys.exit(f"error: not found: {path.relative_to(ROOT)}")
        label = f"ts/{path.parent.name}"
        stamp_span(path, PKG_VERSION, version, label + " version")
        text = path.read_text(encoding="utf-8")
        out, last, touched = [], 0, False
        for m in INTERNAL_DEP.finditer(text):
            val = m.group("val")
            # Only rewrite concrete published-facing ranges; leave workspace links
            # (`*` / `workspace:*` in devDependencies) alone.
            if val == rng or val == "*" or val.startswith("workspace:"):
                continue
            touched = True
            if check:
                drift.append((f"{label} dep {m.group('key')}", m.group("val"), rng))
            else:
                out.append(text[last : m.start("val")])
                out.append(rng)
                last = m.end("val")
        if touched and not check:
            out.append(text[last:])
            path.write_text("".join(out), encoding="utf-8")
            changed.append((f"{label} internal dep ranges", "…", rng))

    # 3. Rust crate
    stamp_span(RUST_CARGO, CARGO_VERSION, version, "rust Cargo.toml")

    if check:
        if drift:
            print(f"version drift from VERSION={version}:")
            for label, cur, exp in drift:
                print(f"  - {label}: has {cur!r}, expected {exp!r}")
            print("\nfix: python3 tools/sync-version.py")
            sys.exit(1)
        print(f"ok: every artifact matches VERSION={version} (internal deps at {rng!r})")
    else:
        for label, old, new in changed:
            print(f"updated {label}: {old} -> {new}")
        print(f"done: all artifacts stamped to {version}")


if __name__ == "__main__":
    extra = [a for a in sys.argv[1:] if a != "--check"]
    if extra:
        sys.exit(f"usage: {sys.argv[0]} [--check]  (unexpected: {' '.join(extra)})")
    run(check="--check" in sys.argv[1:])
