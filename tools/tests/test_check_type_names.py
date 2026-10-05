# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for tools/check_type_names.py (#1723)."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import check_type_names as ctn  # noqa: E402


class ViolationsTest(unittest.TestCase):
    """@brief The line scan flags unsuffixed declarations and nothing else."""

    def names(self, text):
        """@brief The flagged names in @p text."""
        return [n for _, n in ctn.violations(text)]

    def test_flags_each_declaration_kind(self):
        """@brief class, struct, union, enum, enum class, template and alias forms."""
        text = "\n".join([
            "class widget {",
            "struct gadget;",
            "union bits {",
            "enum colour : int {",
            "enum class mode : std::uint8_t {",
            "template <std::size_t N> class buffer {",
            "struct alignas(64) slot {",
            "struct [[nodiscard]] token {",
            "using handle = int;",
            "template <class T> using box = T;",
        ])
        self.assertEqual(self.names(text), ["widget", "gadget", "bits", "colour", "mode",
                                            "buffer", "slot", "token", "handle", "box"])

    def test_passes_conforming_and_protocol_names(self):
        """@brief `_t` names, standard protocol members and allowed names pass."""
        text = "\n".join([
            "class widget_t {",
            "struct alignas(64) slot_t {",
            "using value_type = std::byte;",
            "    using iterator = const T*;",
            "using rope_cursor = rope_cursor_t;",
            "template <class C> struct sized_guard {",
        ])
        self.assertEqual(self.names(text), [])

    def test_ignores_comments_and_out_of_line_names(self):
        """@brief Prose, `friend class`, and `class X::Y` definitions are not declarations."""
        text = "\n".join([
            " * class widget is described here",
            "// struct gadget",
            "    friend class widget_t;",
            "class outer_t::inner {",
        ])
        self.assertEqual(self.names(text), [])

    def test_tree_is_clean(self):
        """@brief The repository's public headers pass today."""
        self.assertEqual(ctn.main(), 0)


if __name__ == "__main__":
    unittest.main()
