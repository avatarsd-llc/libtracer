# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""The PlatformIO generator of ``register_builtin_transports`` (#2439).

PlatformIO has no CMake, so its build script (``pio_esp32_can.py``) cannot call
``libtracer_generate_builtin_transports`` from ``core/cmake/libtracer_sources.cmake``. It
calls :func:`render` instead, over the same template, ``core/src/builtin_transports.cpp.in``.
The rule is the CMake function's: each template call carries a ``// builtin: <kind>``
marker; the call of a listed kind is kept with its marker dropped, every other marked line
is deleted, and a kind the template does not know is an error.
``tools/check_builtin_transports.py`` holds the two generators byte-identical on every kind
set, so keep this module free of PlatformIO imports: the check imports it directly.
"""
from __future__ import annotations

import re

_MARKER_RE = re.compile(r"^(?P<call>[^\n]*)  // builtin: (?P<kind>[a-z0-9_]+)\n", re.M)


def known_kinds(template: str) -> list[str]:
    """Return the built-in kinds the template marks, in template order."""
    return [m.group("kind") for m in _MARKER_RE.finditer(template)]


def render(template: str, kinds: list[str]) -> str:
    """Return the dispatcher calling only the transports in ``kinds``."""
    unknown = sorted(set(kinds) - set(known_kinds(template)))
    if unknown:
        raise ValueError(f"libtracer: no built-in transport {', '.join(unknown)} in the "
                         f"template (known: {', '.join(known_kinds(template))})")
    wanted = set(kinds)
    return _MARKER_RE.sub(
        lambda m: m.group("call") + "\n" if m.group("kind") in wanted else "", template)
