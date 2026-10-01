#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Hold every build of core to the ONE core source list (#1702).

``core/cmake/libtracer_sources.cmake`` is the only place a ``core/src/*.cpp`` is
named. ``core/CMakeLists.txt`` and the ESP-IDF component
(``integrations/esp-idf/libtracer/CMakeLists.txt``) ``include()`` it and select
its groups; PlatformIO cannot include CMake, so it compiles ``core/src`` by filter
(``+<*>`` minus the ``DENY_*`` lists in ``integrations/platformio/pio_esp32_can.py``).

This check fails when:

* a ``core/src/*.cpp`` on disk is in no group, a group names a file that is gone,
  or a file is in two groups;
* either CMake consumer names a core source itself instead of using a group, or
  leaves a group unmentioned (every group is a decision each consumer makes; the
  ESP-IDF opt-outs are listed in ``ESP_IDF_OPT_OUT`` below);
* a PlatformIO ``DENY_*`` list does not name exactly the files of the groups it
  stands for (``PIO_DENY`` below), or the script carries a ``-<file.cpp>`` filter
  outside those lists.

Pure file parsing, no build: it runs in seconds wherever Python 3 runs.
"""
from __future__ import annotations

import ast
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CORE_SRC = ROOT / "core" / "src"
SOURCE_LIST = ROOT / "core" / "cmake" / "libtracer_sources.cmake"
CORE_CMAKE = ROOT / "core" / "CMakeLists.txt"
ESP_CMAKE = ROOT / "integrations" / "esp-idf" / "libtracer" / "CMakeLists.txt"
PIO_SCRIPT = ROOT / "integrations" / "platformio" / "pio_esp32_can.py"

# Groups the ESP-IDF component deliberately never compiles.
ESP_IDF_OPT_OUT = {
    "QUIC",  # the msquic-backed libtracer_quic module: host-only, never on a chip
}

# Each PlatformIO deny-list and the source groups it must name exactly.
PIO_DENY = {
    # Host-only stacks (msquic) and Linux kernel headers (SocketCAN; its stub stays in).
    "DENY_BASELINE": {"QUIC", "SOCKETCAN_LINUX"},
    # The bus-links opt-in (#1670) is off: no CAN transport and no CAN link.
    "DENY_WITHOUT_BUS_LINKS": {"TRANSPORT_CAN", "SOCKETCAN_STUB"},
    # The self-heal-links opt-in (#1670) is off.
    "DENY_WITHOUT_SELF_HEAL_LINKS": {"SELF_HEAL_LINKS"},
    # espressif32 (#984): no portable WS pair, and no full-node dispatcher naming it.
    "DENY_ESPRESSIF32": {"TRANSPORT_WS", "BUILTIN_WS", "BUILTIN_DISPATCHER"},
}

_SET_RE = re.compile(r"\bset\(\s*LIBTRACER_SOURCES_(\w+)\s+(.*?)\)", re.S)
_ENTRY_RE = re.compile(r'^"\$\{_libtracer_src\}/([A-Za-z0-9_]+\.cpp)"$')
_DIRECT_RE = re.compile(r"\bsrc/([A-Za-z0-9_]+\.cpp)\b")
_GROUP_REF_RE = re.compile(r"\$\{LIBTRACER_SOURCES_(\w+)\}")
_DENY_ITEM_RE = re.compile(r"^-<([^>]+\.cpp)>$")


def _strip_cmake_comments(text: str) -> str:
    """Drop `#`-to-end-of-line comments (good enough for the lists parsed here)."""
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def _rel(path: pathlib.Path) -> str:
    return str(path.relative_to(ROOT))


def parse_groups(errors: list[str]) -> dict[str, list[str]]:
    """Return {group: [file, ...]} from the shared source list."""
    text = _strip_cmake_comments(SOURCE_LIST.read_text("utf-8"))
    groups: dict[str, list[str]] = {}
    for name, body in _SET_RE.findall(text):
        files = []
        for token in body.split():
            match = _ENTRY_RE.match(token)
            if not match:
                errors.append(f"{_rel(SOURCE_LIST)}: LIBTRACER_SOURCES_{name}: entry {token!r} "
                              'is not of the form "${_libtracer_src}/<file>.cpp"')
                continue
            files.append(match.group(1))
        if name in groups:
            errors.append(f"{_rel(SOURCE_LIST)}: LIBTRACER_SOURCES_{name} is set twice")
        groups[name] = files
    if not groups:
        errors.append(f"{_rel(SOURCE_LIST)}: no LIBTRACER_SOURCES_<GROUP> found")
    return groups


def check_groups(groups: dict[str, list[str]], errors: list[str]) -> None:
    """The groups partition core/src/*.cpp exactly."""
    owner: dict[str, str] = {}
    for group, files in groups.items():
        for name in files:
            if name in owner:
                errors.append(f"{name} is in both LIBTRACER_SOURCES_{owner[name]} and "
                              f"LIBTRACER_SOURCES_{group}")
            owner.setdefault(name, group)
    on_disk = {p.name for p in CORE_SRC.glob("*.cpp")}
    for name in sorted(on_disk - owner.keys()):
        errors.append(f"core/src/{name} is in no group of {_rel(SOURCE_LIST)}: add it to "
                      "the group of the module it belongs to")
    for name in sorted(owner.keys() - on_disk):
        errors.append(f"{_rel(SOURCE_LIST)} names core/src/{name}, which does not exist")


def check_cmake_consumer(path: pathlib.Path, groups: dict[str, list[str]],
                         opt_out: set[str], errors: list[str]) -> None:
    """A CMake consumer uses groups only, and decides on every group."""
    text = _strip_cmake_comments(path.read_text("utf-8"))
    for name in sorted(set(_DIRECT_RE.findall(text))):
        errors.append(f"{_rel(path)} names src/{name} directly: select its "
                      f"LIBTRACER_SOURCES_* group from {_rel(SOURCE_LIST)} instead")
    referenced = set(_GROUP_REF_RE.findall(text))
    for group in sorted(referenced - groups.keys()):
        errors.append(f"{_rel(path)} uses LIBTRACER_SOURCES_{group}, which "
                      f"{_rel(SOURCE_LIST)} does not define")
    for group in sorted(groups.keys() - referenced - opt_out):
        errors.append(f"{_rel(path)} never uses LIBTRACER_SOURCES_{group}: compile it "
                      "under the right condition, or record the opt-out in "
                      f"{_rel(pathlib.Path(__file__))}")
    for group in sorted(opt_out & referenced):
        errors.append(f"{_rel(path)} uses LIBTRACER_SOURCES_{group}, which is recorded as an "
                      "opt-out: drop the opt-out or the use")


def check_platformio(groups: dict[str, list[str]], errors: list[str]) -> None:
    """Each DENY_* list names exactly its groups' files, and nothing else denies a file."""
    tree = ast.parse(PIO_SCRIPT.read_text("utf-8"), filename=str(PIO_SCRIPT))
    found: dict[str, list[str]] = {}
    inside_lists: set[int] = set()
    for node in tree.body:
        if (isinstance(node, ast.Assign) and len(node.targets) == 1
                and isinstance(node.targets[0], ast.Name)
                and node.targets[0].id.startswith("DENY_")):
            name = node.targets[0].id
            if not isinstance(node.value, ast.List) or not all(
                    isinstance(e, ast.Constant) and isinstance(e.value, str)
                    for e in node.value.elts):
                errors.append(f"{_rel(PIO_SCRIPT)}: {name} must be a literal list of file names")
                continue
            found[name] = [e.value for e in node.value.elts]
            inside_lists.update(id(e) for e in node.value.elts)

    for node in ast.walk(tree):
        if (isinstance(node, ast.Constant) and isinstance(node.value, str)
                and id(node) not in inside_lists and _DENY_ITEM_RE.match(node.value)):
            errors.append(f"{_rel(PIO_SCRIPT)}:{node.lineno}: {node.value!r} denies a source "
                          "outside the DENY_* lists; add it to the list it belongs to")

    for name in sorted(found.keys() - PIO_DENY.keys()):
        errors.append(f"{_rel(PIO_SCRIPT)}: {name} is not mapped to source groups in "
                      f"{_rel(pathlib.Path(__file__))} (PIO_DENY)")
    for name, want_groups in sorted(PIO_DENY.items()):
        if name not in found:
            errors.append(f"{_rel(PIO_SCRIPT)}: {name} is missing")
            continue
        unknown = sorted(want_groups - groups.keys())
        if unknown:
            errors.append(f"PIO_DENY[{name}] names undefined group(s): {', '.join(unknown)}")
            continue
        want = {f for g in want_groups for f in groups[g]}
        have = found[name]
        if len(have) != len(set(have)):
            errors.append(f"{_rel(PIO_SCRIPT)}: {name} lists a file twice")
        group_names = " + ".join(f"LIBTRACER_SOURCES_{g}" for g in sorted(want_groups))
        for extra in sorted(set(have) - want):
            errors.append(f"{_rel(PIO_SCRIPT)}: {name} denies {extra}, which is not in "
                          f"{group_names}")
        for missing in sorted(want - set(have)):
            errors.append(f"{_rel(PIO_SCRIPT)}: {name} must deny {missing} "
                          f"(it is in {group_names})")


def main() -> int:
    for path in (CORE_SRC, SOURCE_LIST, CORE_CMAKE, ESP_CMAKE, PIO_SCRIPT):
        if not path.exists():
            print(f"error: expected {path} to exist", file=sys.stderr)
            return 2

    errors: list[str] = []
    groups = parse_groups(errors)
    check_groups(groups, errors)
    check_cmake_consumer(CORE_CMAKE, groups, set(), errors)
    check_cmake_consumer(ESP_CMAKE, groups, ESP_IDF_OPT_OUT, errors)
    check_platformio(groups, errors)

    if errors:
        print(f"ERROR: the core source list and its consumers disagree ({len(errors)}):",
              file=sys.stderr)
        for line in errors:
            print(f"  - {line}", file=sys.stderr)
        return 1
    total = sum(len(files) for files in groups.values())
    print(f"ok: {total} core sources in {len(groups)} groups; core CMake, ESP-IDF and the "
          f"{len(PIO_DENY)} PlatformIO deny-lists agree with them.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
