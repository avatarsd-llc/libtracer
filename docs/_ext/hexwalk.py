# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""The ``{hex-walk}`` directive: a worked example's bytes, read from a conformance vector (#2372).

A concept page never types hex (#2374). It names a vector and labels the vector's leaves; the
build reads ``tests/conformance/vectors/v1/<name>/input.bin``, walks its TLVs by the header
layout of reference 01 (type, opt, u16 or u32 length, optional trailers) and renders one
collapsible row per TLV, nested as the TLVs nest. A changed vector changes the page; a vector
that no longer decodes, or no longer matches the page's labels, fails the build.

    ```{hex-walk} stream/batch-composed-standalone
    :caption: One framed batch, standalone.
    u64: the base, the sample time of frame 0 in ns
    i32[]: the offsets of each sample from the base, in ns
    u16: sample 0
    ```

Each content line labels one leaf (a TLV with ``opt.PL = 0``), in byte order, as
``<format>: <label>``. Formats: ``u8``…``u64``, ``i8``…``i64``, an array of either
(``i32[]``), ``str`` (UTF-8), ``path`` (a PATH body: segment records and PAIR escapes, one
row each, #2386) and ``hex``. Standard library and docutils only.
"""
import html
import pathlib
import re
import struct
from dataclasses import dataclass

TYPE_NAMES = {
    0x01: "VALUE", 0x02: "NAME", 0x03: "DESCRIPTION", 0x04: "SUBSCRIBER", 0x06: "PATH",
    0x07: "POINT", 0x08: "ERROR", 0x09: "STATUS", 0x0A: "ACL", 0x0B: "SETTINGS",
    0x0C: "TIME", 0x0E: "SPEC", 0x80: "BATCH",
}
# Reference 01, options bitfield: bit 7 R, 6 PL, 5 TS, 4 CR, 3 LL, 2 CW, 1 TF, 0 R.
PL, TS, CR, LL, CW, TF, RESERVED = 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x81
OPT_NAMES = ((PL, "PL"), (TS, "TS"), (CR, "CR"), (LL, "LL"), (CW, "CW"), (TF, "TF"))
_INT = {"u8": "<B", "u16": "<H", "u32": "<I", "u64": "<Q",
        "i8": "<b", "i16": "<h", "i32": "<i", "i64": "<q"}
# Every byte the walk shows sits in one of these, so a test can read the bytes back.
BYTES = re.compile(r'<code class="lt-hw-bytes">([0-9a-f ]*)</code>')


@dataclass
class Node:
    offset: int
    type: int
    opt: int
    length: int
    header: int
    trailer: int
    raw: bytes
    children: list | None
    fmt: str = ""
    label: str = ""

    @property
    def payload(self):
        return self.raw[self.header:self.header + self.length]

    @property
    def end(self):
        return self.offset + self.header + self.length + self.trailer


def decode(data, base=0):
    """The TLVs of @p data, in order; a structured payload is decoded as children."""
    nodes, at = [], 0
    while at < len(data):
        if len(data) - at < 4:
            raise ValueError(f"offset {base + at}: {len(data) - at} bytes left, a header needs 4")
        type_, opt = data[at], data[at + 1]
        if opt & RESERVED:
            raise ValueError(f"offset {base + at}: reserved opt bit set (0x{opt:02x})")
        header = 6 if opt & LL else 4
        length = int.from_bytes(data[at + 2:at + header], "little")
        trailer = ((4 if opt & TF else 8) if opt & TS else 0) + ((2 if opt & CW else 4) if opt & CR else 0)
        end = at + header + length + trailer
        if end > len(data):
            raise ValueError(f"offset {base + at}: TLV runs to {base + end}, past {base + len(data)}")
        raw = data[at:end]
        children = decode(raw[header:header + length], base + at + header) if opt & PL else None
        nodes.append(Node(base + at, type_, opt, length, header, trailer, raw, children))
        at = end
    return nodes


def leaves(nodes):
    for n in nodes:
        if n.children is None:
            yield n
        else:
            yield from leaves(n.children)


def label(nodes, lines):
    """Pair each leaf with its ``<format>: <label>`` line, in order; refuse a count mismatch."""
    found = list(leaves(nodes))
    if len(found) != len(lines):
        raise ValueError(f"{len(found)} leaf TLV(s) in the vector, {len(lines)} label line(s)")
    for n, line in zip(found, lines):
        fmt, sep, text = line.partition(":")
        if not sep or not text.strip():
            raise ValueError(f"label line {line!r} is not '<format>: <label>'")
        n.fmt, n.label = fmt.strip(), text.strip()
        render_value(n.fmt, n.payload)  # refuse a format that does not fit, at label time
    return found


PAIR_KIND = 0x16  # Reference 05 §0x06: the path element PAIR, an escape record of 8 bytes.


def path_records(body):
    """The records of a PATH @p body, as (bytes, reading); refuse one that does not tile."""
    out, at = [], 0
    while at < len(body):
        n = body[at]
        if n:
            end = at + 1 + n
            text = None if end > len(body) else 'NAME "' + body[at + 1:end].decode("utf-8") + '"'
        else:
            end = at + 3 + (body[at + 2] if at + 2 < len(body) else 0)
            esc = body[at:end]
            if end > len(body) or len(esc) < 3:
                text = None
            elif esc[1] == PAIR_KIND and esc[2] == 8:
                index, gen = struct.unpack("<II", esc[3:])
                text = f"PAIR(index {index}, generation {gen})"
            else:
                text = f"escape kind 0x{esc[1]:02x}, {esc[2]} bytes"
        if text is None:
            raise ValueError(f"PATH record at body offset {at} runs past the {len(body)}-byte body")
        out.append((body[at:end], text))
        at = end
    return out


def render_value(fmt, payload):
    """@p payload read as @p fmt, for a reader."""
    if fmt == "path":
        recs = path_records(payload)
        if not recs:
            return "empty"
        return "".join("/" + (t[6:-1] if t.startswith("NAME ") else t) for _, t in recs)
    if fmt == "str":
        return '"' + payload.decode("utf-8") + '"'
    if fmt == "hex":
        return payload.hex(" ")
    many = fmt.endswith("[]")
    code = _INT.get(fmt.removesuffix("[]"))
    if code is None:
        raise ValueError(f"unknown format {fmt!r}")
    width = struct.calcsize(code)
    if len(payload) % width or (not many and len(payload) != width):
        raise ValueError(f"format {fmt!r} does not fit {len(payload)} payload bytes")
    values = [v for (v,) in struct.iter_unpack(code, payload)]
    return "[" + ", ".join(map(str, values)) + "]" if many else str(values[0])


def _bytes(b):
    return f'<code class="lt-hw-bytes">{b.hex(" ")}</code>'


def _opt(opt):
    names = [n for bit, n in OPT_NAMES if opt & bit]
    return "none set" if not names else ", ".join(names)


def _row(n, depth):
    name = TYPE_NAMES.get(n.type, f"type 0x{n.type:02x}")
    head = n.raw[:n.header]
    shape = "structured" if n.children is not None else "opaque"
    what = (f"{name}, {shape}, {n.length}-byte payload" if n.children is not None
            else f"{name}: {html.escape(n.label)} = <strong>{html.escape(render_value(n.fmt, n.payload))}</strong>")
    fields = (f'<dl class="lt-hw-fields"><dt>offset</dt><dd>{n.offset}</dd>'
              f"<dt>type</dt><dd><code>0x{n.type:02x}</code> {name}</dd>"
              f"<dt>opt</dt><dd><code>0x{n.opt:02x}</code>: {_opt(n.opt)}</dd>"
              f"<dt>length</dt><dd>{n.length} (u{8 * (n.header - 2)} LE)</dd>")
    if n.fmt == "path":
        fields += "".join(f"<dt>record</dt><dd><code>{r.hex(' ')}</code> {html.escape(t)}</dd>"
                          for r, t in path_records(n.payload)) or "<dt>records</dt><dd>none</dd>"
    elif n.children is None:
        fields += f"<dt>payload</dt><dd>{html.escape(n.fmt)} LE</dd>"
    fields += "</dl>"
    body = "".join(_row(c, depth + 1) for c in n.children) if n.children is not None else ""
    payload = "" if n.children is not None else " " + _bytes(n.payload)
    trailer = ("" if not n.trailer else
               f'<div class="lt-hw-trailer">trailer {_bytes(n.raw[n.header + n.length:])}</div>')
    is_open = " open" if depth == 0 else ""
    return (f'<details class="lt-hw-tlv"{is_open}><summary>{_bytes(head)}{payload} '
            f'<span class="lt-hw-what">{what}</span></summary>{fields}{body}{trailer}</details>')


def render(nodes):
    total = sum(n.end - n.offset for n in nodes)
    return (f'<div class="lt-hexwalk" role="group" aria-label="hex walk, {total} bytes">'
            + "".join(_row(n, 0) for n in nodes) + "</div>")


def setup(app):
    from docutils import nodes as dn
    from docutils.parsers.rst import Directive, directives

    class HexWalk(Directive):
        required_arguments = 1
        has_content = True
        option_spec = {"caption": directives.unchanged}

        def run(self):
            env = self.state.document.settings.env
            path = pathlib.Path(env.srcdir) / "tests/conformance/vectors/v1" / self.arguments[0] / "input.bin"
            env.note_dependency(str(path))
            try:
                data = path.read_bytes()
                nodes = decode(data)
                label(nodes, [line for line in self.content if line.strip()])
            except (OSError, ValueError) as exc:
                raise self.error(f"hex-walk {self.arguments[0]}: {exc}")
            hexline = data.hex()
            out = dn.container(classes=["lt-hexwalk-figure"])
            if "caption" in self.options:
                para = dn.paragraph(classes=["lt-hexwalk-caption"])
                para += dn.Text(self.options["caption"] + f" Vector {self.arguments[0]}, "
                                f"{len(data)} bytes.")
                out += para
            out += dn.raw("", render(nodes), format="html")
            block = dn.literal_block(hexline, hexline, classes=["lt-hexwalk-hex"])
            block["language"] = "text"
            out += block
            return [out]

    app.add_directive("hex-walk", HexWalk)
    app.add_css_file("hexwalk.css")
    return {"parallel_read_safe": True, "parallel_write_safe": True}
