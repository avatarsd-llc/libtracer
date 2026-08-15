#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the connection-config key gate's extractor and page parser.

`tools/check_config_keys.py` puts a derivation between the source and
`docs/modules/connection-config.md` — and a derivation is a thing that can be
wrong quietly. If the extractor under-reads, the gate passes while the page is
incomplete, which is precisely the failure the page was created to end (#1012:
four TLS-carrying keys, two of them governing certificate validation, documented
on no page at all).

These pin its decisions against the shapes the real tree actually has:

* the reader variable is not always called `cfg` (`transport_can.cpp` calls it
  `reader`, and its `transport_can_config_t` IS called `cfg` — so a receiver
  filter that ignored the declaration would read `cfg.path = ...` as a key or
  miss `reader.name("ifname")` entirely);
* a file with no reader derives NOTHING rather than everything that looks like an
  accessor — that is what lets `builtin_transport_udp.cpp` prove it has no
  kind-private keys;
* the accessor determines the documented wire type, so `flag` and `u8` must not
  collapse to the same spelling;
* a malformed marker block is an ERROR, not an empty declaration that passes.

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import os
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))

import check_config_keys as cck  # noqa: E402


class DeriveKeys(unittest.TestCase):
    """What the extractor reads out of a translation unit."""

    def test_reads_keys_off_the_declared_reader(self):
        src = """
            const config_reader_t cfg(raw_config);
            if (const auto v = cfg.name("cert")) out.cert = std::string(*v);
            if (const auto v = cfg.flag("insecure")) out.insecure = *v;
        """
        self.assertEqual(cck.derive_keys(src), {"cert": "name", "insecure": "flag"})

    def test_reader_variable_is_not_always_named_cfg(self):
        # transport_can.cpp's real shape: the READER is `reader` and the thing
        # called `cfg` is the transport's config struct.
        src = """
            std::string ifname;
            transport_can_config_t cfg;
            const config_reader_t reader(raw_config);
            if (const auto v = reader.name("ifname")) ifname = std::string(*v);
            if (const auto v = reader.u16("node")) cfg.node = *v;
            cfg.path = "/whatever";
        """
        self.assertEqual(cck.derive_keys(src), {"ifname": "name", "node": "u16"})

    def test_accessor_on_a_non_reader_is_not_a_key(self):
        # A same-named method on an unrelated object must not smuggle in a key.
        src = """
            const config_reader_t cfg(raw_config);
            if (const auto v = cfg.u32("max_peers")) ...;
            something_else.name("not_a_config_key");
        """
        self.assertEqual(cck.derive_keys(src), {"max_peers": "u32"})

    def test_no_reader_derives_nothing(self):
        # builtin_transport_udp.cpp's shape: no reader at all.
        src = """
            vertex.register_transport_type("udp", [](const conn_settings_t& s,
                                                     const wire::tlv_t* /*raw_config*/) {
                return dial_or_listen(s, ...);
            });
        """
        self.assertEqual(cck.derive_keys(src), {})

    def test_marked_reader_is_not_connection_config(self):
        # transport_vertex.cpp's shape since RFC-0014 S2b: the creation-SPEC ENVELOPE walk
        # and the universal-key config walk live in ONE file, so the file cannot be excluded
        # wholesale — the marked reader is exempted and the unmarked one still counts.
        src = """
            const wire::config_reader_t pairs(&spec);  // config-keys: not-connection-config
            const auto n = pairs.name("name");
            const config_reader_t cfg(config);
            if (const auto v = cfg.name("addr")) ...;
        """
        self.assertEqual(cck.derive_keys(src), {"addr": "name"})

    def test_the_marker_must_sit_on_the_construction_line(self):
        # A marker in a PRECEDING comment does not exempt the reader: the rule is per line,
        # so an exemption is always visible next to the thing it exempts.
        src = """
            // config-keys: not-connection-config
            const config_reader_t cfg(config);
            if (const auto v = cfg.name("addr")) ...;
        """
        self.assertEqual(cck.derive_keys(src), {"addr": "name"})

    def test_flag_and_u8_are_distinguishable(self):
        src = """
            const config_reader_t cfg(c);
            if (const auto v = cfg.u8("version")) ...;
            if (const auto v = cfg.flag("fd")) ...;
        """
        keys = cck.derive_keys(src)
        self.assertNotEqual(cck.VALUE_SPELLING[keys["version"]], cck.VALUE_SPELLING[keys["fd"]])


class ParsePage(unittest.TestCase):
    """What the page parser reads out of a marker block."""

    def test_reads_key_and_value_cells(self):
        page = "\n".join(
            [
                "<!-- config-keys:begin core/src/x.cpp -->",
                "| key | value | default |",
                "| --- | --- | --- |",
                "| `ca` | `NAME` utf-8 | empty |",
                "| `insecure` | `VALUE` u8 (flag) | `0` |",
                "<!-- config-keys:end -->",
            ]
        )
        blocks, errors = cck.parse_page(page)
        self.assertEqual(errors, [])
        self.assertEqual(
            blocks["core/src/x.cpp"],
            {"ca": "`NAME` utf-8", "insecure": "`VALUE` u8 (flag)"},
        )

    def test_rows_outside_a_block_are_not_declarations(self):
        page = "| `stray` | `NAME` utf-8 |\n"
        blocks, errors = cck.parse_page(page)
        self.assertEqual(blocks, {})
        self.assertEqual(errors, [])

    def test_a_block_with_no_rows_declares_the_empty_set(self):
        page = "<!-- config-keys:begin core/src/u.cpp -->\nprose only\n<!-- config-keys:end -->"
        blocks, errors = cck.parse_page(page)
        self.assertEqual(errors, [])
        self.assertEqual(blocks, {"core/src/u.cpp": {}})

    def test_unterminated_block_is_an_error(self):
        page = "<!-- config-keys:begin core/src/x.cpp -->\n| `ca` | `NAME` utf-8 |\n"
        _, errors = cck.parse_page(page)
        self.assertTrue(any("unterminated" in e for e in errors), errors)

    def test_duplicate_block_for_one_file_is_an_error(self):
        page = "\n".join(
            [
                "<!-- config-keys:begin core/src/x.cpp -->",
                "<!-- config-keys:end -->",
                "<!-- config-keys:begin core/src/x.cpp -->",
                "<!-- config-keys:end -->",
            ]
        )
        _, errors = cck.parse_page(page)
        self.assertTrue(any("duplicate" in e for e in errors), errors)


class LivePage(unittest.TestCase):
    """The committed page, against the committed source."""

    def test_gate_is_green_on_main(self):
        self.assertEqual(cck.check(), [])

    def test_the_gate_sees_the_quic_trust_pair(self):
        # The keys #1012 was filed about. If the extractor ever stops seeing them,
        # the whole page could be deleted and the gate would still pass.
        quic = cck.derive_keys((cck.ROOT / "core" / "src" / "transport_quic.cpp").read_text())
        self.assertEqual(quic.get("ca"), "name")
        self.assertEqual(quic.get("insecure"), "flag")


if __name__ == "__main__":
    unittest.main()
