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
    - bindings/typescript/package-lock.json (npm mirrors each workspace manifest
      into a `packages/<dir>` entry, so it drifts in lockstep or not at all)
  Rust crate:
    - bindings/rust/Cargo.toml

``core/CMakeLists.txt`` reads ``VERSION`` directly (a release git tag wins over it),
so it is not stamped here. The private TS monorepo root and the ROS 2 stub
(``bindings/ros2``, unreleased) are intentionally left alone.

Usage::

  tools/sync-version.py           # rewrite every artifact to match VERSION
  tools/sync-version.py --check   # exit 1 if anything drifts (the CI gate)
  tools/sync-version.py X.Y.Z     # stamp VERSION itself + every artifact to X.Y.Z

The explicit-version form is how the tag-driven release pipeline works: release.yml
derives ``X.Y.Z`` from the pushed ``vX.Y.Z`` tag and each publish job stamps the
checked-out tree with it before packaging, so the git tag — not the committed
manifests — is the version every registry sees. ``X.Y.Z --check`` verifies the
tree (including ``VERSION``) already matches an explicit version.

Substitutions are targeted (only the version substring / dep-range value is
replaced), so comments, key order, and formatting are preserved. The lockfile is
edited the same way — in place, no network, no npm — so a bump needs no registry
round-trip and the file keeps npm's own byte layout everywhere it was not stamped.
"""
import json
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
TS_ROOT = ROOT / "bindings/typescript"
TS_PACKAGES = [
    TS_ROOT / "packages/core/package.json",
    TS_ROOT / "packages/client/package.json",
    TS_ROOT / "packages/transport-ws/package.json",
    TS_ROOT / "packages/transport-webtransport/package.json",
]
# npm mirrors each workspace manifest into the lockfile under its workspace-relative
# directory, so the same two fields live there too and must move together (#862).
TS_LOCKFILE = TS_ROOT / "package-lock.json"
PKG_VERSION = re.compile(r'"version"\s*:\s*"(?P<ver>[^"]*)"')
# An internal @avatarsd-llc/* dependency line: key is the scoped name, value a range.
INTERNAL_DEP = re.compile(r'(?P<key>"@avatarsd-llc/[a-z0-9-]+")(?P<sep>\s*:\s*")(?P<val>[^"]*)(?P<end>")')

RUST_CARGO = ROOT / "bindings/rust/Cargo.toml"
CARGO_VERSION = re.compile(r'(?m)^version\s*=\s*"(?P<ver>[^"]*)"')


def read_version(explicit=None):
    """Resolve the target version: an explicit ``X.Y.Z`` wins over the VERSION file."""
    if explicit is not None:
        if not SEMVER.match(explicit):
            sys.exit(f"error: explicit version {explicit!r} is not MAJOR.MINOR.PATCH[-suffix]")
        return explicit
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


def ts_edits(text, version, rng):
    """Every version-bearing span in one TS manifest body — or one lockfile entry.

    npm copies a workspace's manifest fields verbatim into its lockfile entry, so a
    single definition of "what carries a version here" serves both and the two can
    never disagree about which spans move. Two shapes qualify: the package's own
    ``"version"``, and any *concrete* internal ``@avatarsd-llc/*`` range. Workspace
    links (``*`` / ``workspace:*`` in devDependencies) are not published-facing and
    are left alone; a span already at the target is not an edit.

    @return ``(start, end, current, replacement, what)`` tuples in text order.
    """
    edits = []
    m = PKG_VERSION.search(text)
    if m is not None and m.group("ver") != version:
        edits.append((m.start("ver"), m.end("ver"), m.group("ver"), version, "version"))
    for m in INTERNAL_DEP.finditer(text):
        val = m.group("val")
        if val == rng or val == "*" or val.startswith("workspace:"):
            continue
        edits.append((m.start("val"), m.end("val"), val, rng, f"dep {m.group('key')}"))
    edits.sort()
    return edits


def splice(text, edits):
    """Apply non-overlapping `edits` (as returned by `ts_edits`, in text order)."""
    out, last = [], 0
    for start, end, _cur, new, _what in edits:
        out.append(text[last:start])
        out.append(new)
        last = end
    out.append(text[last:])
    return "".join(out)


def lock_entry_spans(text, keys):
    """Locate the ``packages/<workspace-dir>`` objects inside a package-lock.json.

    Only those entries mirror a stamped manifest; every other ``"version"`` in the
    file belongs to a resolved third-party dependency and must not be touched. The
    object's end comes from the stdlib JSON decoder rather than brace counting, so a
    brace inside a string value cannot desynchronise the span.

    @return ``{key: (start, end)}`` — half-open spans into `text`.
    """
    decoder = json.JSONDecoder()
    spans = {}
    for key in keys:
        m = re.search(r'(?m)^\s*"' + re.escape(key) + r'"\s*:\s*(?=\{)', text)
        if m is None:
            sys.exit(f"error: no {key!r} entry in {TS_LOCKFILE.relative_to(ROOT)} (run: npm install --package-lock-only)")
        _value, end = decoder.raw_decode(text, m.end())
        spans[key] = (m.end(), end)
    return spans


def run(check, explicit=None):
    version = read_version(explicit)
    rng = dep_range(version)
    drift = []  # (label, current, expected)
    changed = []  # (label, old, new)

    # 0. an explicit version stamps (or checks) the VERSION file itself, so a
    #    tag-driven release leaves the packaged tree internally consistent
    #    (core/CMakeLists.txt and any tooling that reads VERSION see X.Y.Z too).
    if explicit is not None:
        cur = VERSION_FILE.read_text(encoding="utf-8").strip() if VERSION_FILE.exists() else "<missing>"
        if cur != version:
            if check:
                drift.append(("VERSION", cur, version))
            else:
                VERSION_FILE.write_text(version + "\n", encoding="utf-8")
                changed.append(("VERSION", cur, version))

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

    def stamp_ts(path, text, edits, prefix):
        """Record `edits` as drift, or splice them into `path` and record them as changes."""
        if not edits:
            return
        if check:
            drift.extend((f"{prefix} {what}", cur, new) for _s, _e, cur, new, what in edits)
        else:
            path.write_text(splice(text, edits), encoding="utf-8")
            changed.extend((f"{prefix} {what}", cur, new) for _s, _e, cur, new, what in edits)

    # 2. TS package versions + every internal @avatarsd-llc/* dep range
    for path in TS_PACKAGES:
        if not path.exists():
            sys.exit(f"error: not found: {path.relative_to(ROOT)}")
        label = f"ts/{path.parent.name}"
        text = path.read_text(encoding="utf-8")
        if PKG_VERSION.search(text) is None:
            sys.exit(f"error: no version field in {path.relative_to(ROOT)} ({label})")
        stamp_ts(path, text, ts_edits(text, version, rng), label)

    # 2b. the npm lockfile mirrors those manifests. It is stamped from the same
    #     `ts_edits` rule applied inside each `packages/<dir>` object — never
    #     file-wide, or the resolved third-party versions would be rewritten too.
    #     Checking it is the load-bearing half: before #862 the gate reported "ok"
    #     while the lockfile still pinned the previous release.
    if not TS_LOCKFILE.exists():
        sys.exit(f"error: not found: {TS_LOCKFILE.relative_to(ROOT)}")
    lock_text = TS_LOCKFILE.read_text(encoding="utf-8")
    lock_keys = [p.parent.relative_to(TS_ROOT).as_posix() for p in TS_PACKAGES]
    lock_edits = []
    for key, (start, end) in lock_entry_spans(lock_text, lock_keys).items():
        for s, e, cur, new, what in ts_edits(lock_text[start:end], version, rng):
            lock_edits.append((start + s, start + e, cur, new, f"{key} {what}"))
    lock_edits.sort()
    stamp_ts(TS_LOCKFILE, lock_text, lock_edits, "ts lockfile")

    # 3. Rust crate
    stamp_span(RUST_CARGO, CARGO_VERSION, version, "rust Cargo.toml")

    if check:
        if drift:
            print(f"version drift from VERSION={version}:")
            for label, cur, exp in drift:
                print(f"  - {label}: has {cur!r}, expected {exp!r}")
            print("\nfix: python3 tools/sync-version.py" + (f" {version}" if explicit else ""))
            sys.exit(1)
        print(f"ok: every artifact matches VERSION={version} (internal deps at {rng!r})")
    else:
        for label, old, new in changed:
            print(f"updated {label}: {old} -> {new}")
        print(f"done: all artifacts stamped to {version}")


if __name__ == "__main__":
    extra = [a for a in sys.argv[1:] if a != "--check"]
    if len(extra) > 1 or any(a.startswith("-") for a in extra):
        sys.exit(f"usage: {sys.argv[0]} [X.Y.Z] [--check]  (unexpected: {' '.join(extra)})")
    run(check="--check" in sys.argv[1:], explicit=extra[0] if extra else None)
