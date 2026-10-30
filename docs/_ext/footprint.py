# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""The MCU lane's footprint table, read from the size gates' artifacts (#2380).

The figures on the MCU page are never typed. The docs workflow fetches the newest
successful main run's JSON artifact from each size gate (tools/fetch_footprint.py) into
``docs/_generated/footprint``, and ``{footprint-table}`` renders one row per artifact. A
missing artifact renders as "not measured in this build", naming the workflow that
produces it, so a local build without the artifacts still builds.

One row per source, in this order:

- ``footprint-cortexm0.json``: the required-modules node on Cortex-M0, the gated budget
  (tools/cortexm0_footprint.py, footprint-cortexm0.yml).
- ``rv32-footprint.json``: the same node on the ESP32-C6's RV32 core, report-only
  (tools/rv32_footprint.py, esp-idf.yml).
- ``footprint-esp32c6.json``: the libtracer archive inside the ESP-IDF full_node app,
  report-only (tools/esp_size_gate.py, esp-idf.yml).
"""
import json
import os
import pathlib

HEADER = ["Target", "Scale", "What is measured", "Flash", "Static RAM", "Toolchain", "Commit"]

#: (artifact file, target, scale, what is measured, producing workflow,
#:  flash key, RAM key)
SOURCES = [
    ("footprint-cortexm0.json", "Cortex-M0", "NARROW",
     "the required-modules node, gated", "footprint-cortexm0.yml", "flash_bytes", "ram_bytes"),
    ("rv32-footprint.json", "RV32 (ESP32-C6 core)", "NARROW",
     "the same node, report-only", "esp-idf.yml", "flash", "ram"),
    ("footprint-esp32c6.json", "ESP32-C6, ESP-IDF", "MID",
     "libtracer.a in the full_node app, report-only", "esp-idf.yml",
     "component_flash_bytes", "component_static_ram_bytes"),
]

UNMEASURED = "not measured in this build"


def _b(n):
    return f"{n:,} B"


def _row(path, target, scale, what, workflow, flash_key, ram_key):
    if not path.exists():
        return [target, scale, what, UNMEASURED, UNMEASURED,
                f"produced nightly by {workflow}", "none"]
    data = json.loads(path.read_text(encoding="utf-8"))
    flash = data.get(flash_key)
    if not isinstance(flash, int):
        raise ValueError(f"{path.name}: no integer {flash_key!r}")
    flash_cell = _b(flash)
    budget = data.get("max_flash_bytes")
    if isinstance(budget, int):
        flash_cell += f" ({_b(budget)} budget, {_b(budget - flash)} left)"
    ram = data.get(ram_key)
    commit = data.get("commit")
    return [target, scale, what, flash_cell, _b(ram) if isinstance(ram, int) else "n/a",
            data.get("toolchain") or "not recorded", commit[:8] if commit else "not recorded"]


def rows(directory):
    """@brief One row per size-gate source, in SOURCES order, from the artifacts found."""
    directory = pathlib.Path(directory)
    return [_row(directory / name, *rest) for name, *rest in SOURCES]


def setup(app):
    from docutils import nodes as dn
    from docutils.parsers.rst import Directive

    class FootprintTable(Directive):
        has_content = False

        def run(self):
            env = self.state.document.settings.env
            default = pathlib.Path(env.srcdir) / "docs" / "_generated" / "footprint"
            directory = pathlib.Path(os.environ.get("LIBTRACER_FOOTPRINT_DIR") or default)
            for name, *_ in SOURCES:
                env.note_dependency(str(directory / name))
            try:
                body = rows(directory)
            except (OSError, ValueError) as exc:
                raise self.error(f"footprint-table: {exc}")
            table = dn.table(classes=["lt-footprint"])
            group = dn.tgroup(cols=len(HEADER))
            table += group
            for _ in HEADER:
                group += dn.colspec(colwidth=1)
            head = dn.thead()
            group += head
            body_node = dn.tbody()
            group += body_node
            for parent, cells in [(head, HEADER)] + [(body_node, r) for r in body]:
                row = dn.row()
                for text in cells:
                    entry = dn.entry()
                    entry += dn.paragraph(text=text)
                    row += entry
                parent += row
            return [table]

    app.add_directive("footprint-table", FootprintTable)
    return {"parallel_read_safe": True, "parallel_write_safe": True}
