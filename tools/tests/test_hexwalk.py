#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the generated hex walk (`docs/_ext/hexwalk.py`, #2372).

A concept page's worked example shows real bytes, read from a conformance vector at build
time and never typed (#2374). These pin the TLV walk against the header layout of
reference 01 (type, opt, u16 or u32 length, optional trailers), the leaf formats a page
names, and the refusals: a vector that does not decode, a leaf with no label, a label with
no leaf.

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import os
import pathlib
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "docs", "_ext"))

import hexwalk  # noqa: E402

VECTORS = pathlib.Path(REPO) / "tests" / "conformance" / "vectors" / "v1"


class DecodeTest(unittest.TestCase):
    """The TLV tree, from reference 01's header layout."""

    def test_a_leaf(self):
        (node,) = hexwalk.decode(bytes.fromhex("01000200" "6400"))
        self.assertEqual((node.type, node.opt, node.length, node.header), (0x01, 0x00, 2, 4))
        self.assertEqual(node.payload, b"\x64\x00")
        self.assertIsNone(node.children)

    def test_a_structured_record_nests(self):
        (node,) = hexwalk.decode(bytes.fromhex("80400c00" "01000200" "6400" "02000200" "6869"))
        self.assertEqual([c.type for c in node.children], [0x01, 0x02])
        self.assertEqual([c.offset for c in node.children], [4, 10])

    def test_a_long_length(self):
        (node,) = hexwalk.decode(bytes.fromhex("010802000000" "6400"))
        self.assertEqual((node.header, node.length), (6, 2))

    def test_trailers_are_counted(self):
        # TS (absolute, 8 bytes) and CR (CRC-16, 2 bytes) after a 1-byte payload.
        (node,) = hexwalk.decode(bytes.fromhex("01340100" "aa" + "00" * 8 + "0000"))
        self.assertEqual((node.trailer, node.end), (10, 15))

    def test_a_short_vector_is_refused(self):
        with self.assertRaises(ValueError):
            hexwalk.decode(bytes.fromhex("01000400" "6400"))

    def test_a_reserved_bit_is_refused(self):
        with self.assertRaises(ValueError):
            hexwalk.decode(bytes.fromhex("01010000"))


class LabelTest(unittest.TestCase):
    """A page names each leaf, in order, with a format and a label."""

    def test_formats(self):
        self.assertEqual(hexwalk.render_value("u16", b"\x64\x00"), "100")
        self.assertEqual(hexwalk.render_value("u64", bytes.fromhex("00002a36fe9c9717")),
                         "1700000000000000000")
        self.assertEqual(hexwalk.render_value("i32[]", bytes.fromhex("00000000dc05000006ffffff")),
                         "[0, 1500, -250]")
        self.assertEqual(hexwalk.render_value("str", b"adc0"), '"adc0"')

    def test_a_format_that_does_not_fit_is_refused(self):
        with self.assertRaises(ValueError):
            hexwalk.render_value("u32", b"\x64\x00")

    def test_labels_pair_with_leaves_in_order(self):
        nodes = hexwalk.decode(bytes.fromhex("80400c00" "01000200" "6400" "02000200" "6869"))
        leaves = hexwalk.label(nodes, ["u16: sample 0", "str: a name"])
        self.assertEqual([(n.fmt, n.label) for n in leaves], [("u16", "sample 0"), ("str", "a name")])

    def test_a_missing_or_extra_label_is_refused(self):
        nodes = hexwalk.decode(bytes.fromhex("01000200" "6400"))
        with self.assertRaises(ValueError):
            hexwalk.label(nodes, [])
        with self.assertRaises(ValueError):
            hexwalk.label(nodes, ["u16: one", "u16: two"])


class RenderTest(unittest.TestCase):
    """The HTML holds every byte once, in order, and escapes what a page wrote."""

    def test_every_byte_appears_once_in_order(self):
        data = (VECTORS / "stream" / "batch-composed-standalone" / "input.bin").read_bytes()
        nodes = hexwalk.decode(data)
        hexwalk.label(nodes, ["u64: base", "i32[]: offsets", "u16: s0", "u16: s1", "u16: s2"])
        html = hexwalk.render(nodes)
        self.assertEqual("".join(hexwalk.BYTES.findall(html)).replace(" ", ""), data.hex())

    def test_labels_are_escaped(self):
        nodes = hexwalk.decode(bytes.fromhex("01000200" "6400"))
        hexwalk.label(nodes, ["u16: <b>x</b>"])
        self.assertIn("&lt;b&gt;x&lt;/b&gt;", hexwalk.render(nodes))


if __name__ == "__main__":
    unittest.main()
