#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Hold every build's ``register_builtin_transports`` to the one template (#2439).

``core/src/builtin_transports.cpp.in`` is the only source of the built-in transport
dispatcher. Two generators turn it into a build's copy: the CMake function
``libtracer_generate_builtin_transports`` (``core/cmake/libtracer_sources.cmake``), which
core and the ESP-IDF component call at configure time, and
``integrations/platformio/libtracer_builtin_transports.py``, which the PlatformIO build
script calls. The ESP-IDF component once kept a third, hand-built copy of the calls and it
drifted: its calls dropped the egress store argument for a year.

This check fails when:

* the CMake and Python generators disagree, byte for byte, on any subset of the
  template's kinds (it runs ``cmake -P``, so it needs cmake on the PATH);
* the template's kinds are not exactly the ``builtin_transport_<kind>.cpp`` glue TUs;
* any file other than the template defines ``register_builtin_transports``, or a build
  file (either CMake consumer, the PlatformIO script) spells a
  ``register_<kind>_transport(`` call itself instead of generating it;
* a build file stops calling its generator.
"""
from __future__ import annotations

import importlib.util
import itertools
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True  # the generator import must leave no __pycache__ in the tree

ROOT = pathlib.Path(__file__).resolve().parent.parent
TEMPLATE = ROOT / "core" / "src" / "builtin_transports.cpp.in"
SOURCE_LIST = ROOT / "core" / "cmake" / "libtracer_sources.cmake"
PY_GENERATOR = ROOT / "integrations" / "platformio" / "libtracer_builtin_transports.py"
# Each build file and the generator call it must make.
CONSUMERS = {
    ROOT / "core" / "CMakeLists.txt": "libtracer_generate_builtin_transports(",
    ROOT / "integrations" / "esp-idf" / "libtracer" / "CMakeLists.txt":
        "libtracer_generate_builtin_transports(",
    ROOT / "integrations" / "platformio" / "pio_esp32_can.py": "_gen.render(",
}
# The trees a dispatcher definition could hide in.
SCANNED_TREES = ("core", "integrations", "backends", "tests", "examples", "bench")
_CALL_RE = re.compile(r"\bregister_[a-z0-9_]+_transport\s*\(")
_DEFINITION_RE = re.compile(r"^void register_builtin_transports\([^;{]*\)\s*\{", re.M)


def _rel(path: pathlib.Path) -> str:
    return str(path.relative_to(ROOT))


def _load_python_generator():
    spec = importlib.util.spec_from_file_location("libtracer_builtin_transports", PY_GENERATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _cmake_render(cmake: str, kinds: tuple[str, ...], workdir: pathlib.Path) -> str:
    out = workdir / ("out_" + "_".join(kinds or ("none",)) + ".cpp")
    script = workdir / "render.cmake"
    script.write_text("cmake_minimum_required(VERSION 3.16)\n"
                      f'include("{SOURCE_LIST.as_posix()}")\n'
                      f'libtracer_generate_builtin_transports("{out.as_posix()}" '
                      f'{" ".join(kinds)})\n', "utf-8")
    proc = subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True,
                          check=False)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or f"cmake -P exited {proc.returncode}")
    return out.read_text("utf-8")


def check_generators(errors: list[str]) -> int:
    """The two generators agree on every subset; return how many subsets were compared."""
    cmake = shutil.which("cmake")
    if cmake is None:
        errors.append("cmake is not on the PATH: the CMake generator cannot be compared")
        return 0
    gen = _load_python_generator()
    template = TEMPLATE.read_text("utf-8")
    kinds = gen.known_kinds(template)
    compared = 0
    with tempfile.TemporaryDirectory() as tmp:
        for n in range(len(kinds) + 1):
            for subset in itertools.combinations(kinds, n):
                want = gen.render(template, list(subset))
                try:
                    have = _cmake_render(cmake, subset, pathlib.Path(tmp))
                except RuntimeError as err:
                    errors.append(f"CMake generator failed on {list(subset)}: {err}")
                    continue
                compared += 1
                if have != want:
                    errors.append(f"the CMake and PlatformIO generators disagree on kinds "
                                  f"{list(subset)}")
                calls = sorted(m.group(0) for m in _CALL_RE.finditer(want))
                if calls != sorted(f"register_{k}_transport(" for k in subset) or re.search(
                        r";  // builtin: \w+$", want, re.M):
                    errors.append(f"the generated dispatcher for {list(subset)} does not "
                                  f"call exactly those kinds: {calls}")
    return compared


def check_kinds(errors: list[str]) -> list[str]:
    """The template marks exactly the glue TUs on disk."""
    kinds = _load_python_generator().known_kinds(TEMPLATE.read_text("utf-8"))
    glue = sorted(p.stem.removeprefix("builtin_transport_")
                  for p in (ROOT / "core" / "src").glob("builtin_transport_*.cpp"))
    if sorted(kinds) != glue:
        errors.append(f"{_rel(TEMPLATE)} marks kinds {sorted(kinds)}, but the glue TUs "
                      f"core/src/builtin_transport_<kind>.cpp are {glue}")
    if len(kinds) != len(set(kinds)):
        errors.append(f"{_rel(TEMPLATE)} marks a kind twice: {kinds}")
    return kinds


def check_one_source(errors: list[str]) -> None:
    """Only the template defines the dispatcher; build files generate, never spell, it."""
    # A file walk, not `git ls-files`: CI runs this in a container over a workspace git may
    # refuse as dubiously owned. Build trees (build*, .pio) hold generated copies.
    for tree in SCANNED_TREES:
        for path in sorted((ROOT / tree).rglob("*")):
            if (path == TEMPLATE or path.suffix not in (".cpp", ".hpp", ".h", ".in")
                    or any(p.startswith(("build", ".")) for p in path.relative_to(ROOT).parts)
                    or not path.is_file()):
                continue
            if _DEFINITION_RE.search(path.read_text("utf-8", errors="replace")):
                errors.append(f"{_rel(path)} defines register_builtin_transports: the "
                              f"dispatcher is generated from {_rel(TEMPLATE)} only")
    for path, call in CONSUMERS.items():
        code = [line.split("#", 1)[0] for line in path.read_text("utf-8").splitlines()]
        text = "\n".join(code)
        for match in _CALL_RE.finditer(text):
            errors.append(f"{_rel(path)} spells {match.group(0)}...) itself: list the kind "
                          "and let the generator write the call")
        if call not in text:
            errors.append(f"{_rel(path)} no longer calls {call}...): every build generates "
                          f"its dispatcher from {_rel(TEMPLATE)}")


def main() -> int:
    for path in (TEMPLATE, SOURCE_LIST, PY_GENERATOR, *CONSUMERS):
        if not path.exists():
            print(f"error: expected {path} to exist", file=sys.stderr)
            return 2
    errors: list[str] = []
    kinds = check_kinds(errors)
    check_one_source(errors)
    compared = check_generators(errors)
    if errors:
        print(f"ERROR: built-in transport registration has more than one source "
              f"({len(errors)}):", file=sys.stderr)
        for line in errors:
            print(f"  - {line}", file=sys.stderr)
        return 1
    print(f"ok: one template ({', '.join(kinds)}); the CMake and PlatformIO generators agree "
          f"on all {compared} kind sets; no build spells the calls itself.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
