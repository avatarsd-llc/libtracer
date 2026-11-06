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
* a malformed marker block is an ERROR, not an empty declaration that passes;
* a shared file's keys count for a kind only through the functions that kind's own
  file calls, and a block cannot name another kind's own file as shared (#2569).

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import os
import pathlib
import sys
import tempfile
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

    def test_a_reader_taken_by_reference_is_a_reader(self):
        # A shared helper is handed the factory's reader (`select_tls_profile`, #2440).
        src = (
            "graph::result_t<const tls_profile_t*> select(const wire::config_reader_t& cfg) {\n"
            '    const auto n = cfg.name("tls");\n'
            "}\n"
        )
        self.assertEqual(cck.derive_keys(src), {"tls": "name"})

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
            blocks[("core/src/x.cpp",)],
            {"ca": "`NAME` utf-8", "insecure": "`VALUE` u8 (flag)"},
        )

    def test_a_block_names_the_kind_file_then_the_shared_files(self):
        page = "\n".join(
            [
                "<!-- config-keys:begin core/src/x.cpp core/src/shared.hpp -->",
                "| `tls` | `NAME` utf-8 | DIAL | | |",
                "<!-- config-keys:end -->",
            ]
        )
        blocks, errors = cck.parse_page(page)
        self.assertEqual(errors, [])
        self.assertEqual(blocks, {("core/src/x.cpp", "core/src/shared.hpp"): {"tls": "`NAME` utf-8"}})

    def test_rows_outside_a_block_are_not_declarations(self):
        page = "| `stray` | `NAME` utf-8 |\n"
        blocks, errors = cck.parse_page(page)
        self.assertEqual(blocks, {})
        self.assertEqual(errors, [])

    def test_a_block_with_no_rows_declares_the_empty_set(self):
        page = "<!-- config-keys:begin core/src/u.cpp -->\nprose only\n<!-- config-keys:end -->"
        blocks, errors = cck.parse_page(page)
        self.assertEqual(errors, [])
        self.assertEqual(blocks, {("core/src/u.cpp",): {}})

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


class SharedFunctions(unittest.TestCase):
    """Per-function attribution inside a shared file (#2569)."""

    def test_keys_belong_to_the_function_that_takes_the_reader(self):
        src = """
            inline int select(std::span<const p_t> ps, const wire::config_reader_t& cfg) noexcept {
                const auto n = cfg.name("tls");
            }
            inline void wt_only(const config_reader_t& r) {
                if (const auto v = r.name("path")) {}
            }
        """
        functions, unattributed, _ = cck.shared_functions(src)
        self.assertEqual(functions, {"select": {"tls": "name"}, "wt_only": {"path": "name"}})
        self.assertEqual(unattributed, {})

    def test_a_declaration_is_not_a_definition(self):
        functions, _, _ = cck.shared_functions("int select(const config_reader_t& cfg);\n")
        self.assertEqual(functions, {})

    def test_a_call_inside_the_shared_file_carries_its_callee_keys(self):
        src = """
            inline void inner(const config_reader_t& r) { (void)r.u32("max_frame"); }
            inline void outer(const config_reader_t& cfg) { (void)cfg.name("tls"); inner(cfg); }
        """
        functions, _, reach = cck.shared_functions(src)
        self.assertEqual(functions["outer"], {"tls": "name", "max_frame": "u32"})
        self.assertEqual(functions["inner"], {"max_frame": "u32"})
        self.assertEqual(reach["outer"], {"outer", "inner"})

    def test_a_reader_the_shared_file_builds_itself_is_unattributed(self):
        src = """
            inline void parse(const wire::tlv_t* raw) {
                const config_reader_t cfg(raw);
                (void)cfg.name("ca");
            }
        """
        functions, unattributed, _ = cck.shared_functions(src)
        self.assertEqual(functions, {})
        self.assertEqual(unattributed, {"ca": "name"})

    def test_a_mention_in_a_comment_or_literal_is_not_a_call(self):
        code = cck.strip_literals(
            '/** @brief see wt_only(cfg) */\n// wt_only(cfg)\nlog("wt_only(cfg) failed");\n'
        )
        self.assertFalse(cck.calls(code, "wt_only"))

    def test_qualified_and_member_calls_are_calls(self):
        self.assertTrue(cck.calls("auto s = net::wt_only(cfg);", "wt_only"))
        self.assertTrue(cck.calls("auto s = ep.wt_only(cfg);", "wt_only"))
        self.assertTrue(cck.calls("auto s = ep->wt_only(cfg);", "wt_only"))
        self.assertFalse(cck.calls("auto s = not_wt_only(cfg);", "wt_only"))


class GateOnAFakeTree(unittest.TestCase):
    """The whole gate over a throwaway tree: two kinds sharing one helper header."""

    SHARED = """
        inline int select_tls(const wire::config_reader_t& cfg) { return cfg.name("tls") ? 1 : 0; }
        inline int wt_path(const wire::config_reader_t& cfg) { return cfg.name("path") ? 1 : 0; }
    """
    QUIC = """
        // wt_path(cfg) is webtransport's, not ours.
        const wire::config_reader_t cfg(raw_config);
        (void)cfg.flag("insecure");
        (void)select_tls(cfg);
    """
    WT = """
        const wire::config_reader_t cfg(raw_config);
        (void)cfg.flag("insecure");
        (void)select_tls(cfg);
        (void)wt_path(cfg);
    """

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        root = pathlib.Path(self._tmp.name)
        src = root / "core" / "src"
        src.mkdir(parents=True)
        (src / "shared.hpp").write_text(self.SHARED)
        (src / "quic.cpp").write_text(self.QUIC)
        (src / "wt.cpp").write_text(self.WT)
        (root / "docs" / "modules").mkdir(parents=True)
        saved = {n: getattr(cck, n) for n in ("ROOT", "PAGE", "SOURCE_DIRS", "EXCLUDED")}
        self.addCleanup(lambda: [setattr(cck, n, v) for n, v in saved.items()])
        self.addCleanup(self._tmp.cleanup)
        cck.ROOT = root
        cck.PAGE = root / "docs" / "modules" / "connection-config.md"
        cck.SOURCE_DIRS = (src,)
        cck.EXCLUDED = set()

    @staticmethod
    def page(quic_files, quic_rows, wt_rows=("tls", "insecure", "path")):
        spell = {"tls": "`NAME` utf-8", "path": "`NAME` utf-8", "ca": "`NAME` utf-8",
                 "insecure": "`VALUE` u8 (flag)"}
        lines = [f"<!-- config-keys:begin {quic_files} -->"]
        lines += [f"| `{k}` | {spell[k]} |" for k in quic_rows]
        lines += ["<!-- config-keys:end -->",
                  "<!-- config-keys:begin core/src/wt.cpp core/src/shared.hpp -->"]
        lines += [f"| `{k}` | {spell[k]} |" for k in wt_rows]
        lines += ["<!-- config-keys:end -->"]
        return "\n".join(lines)

    def test_the_honest_page_passes(self):
        page = self.page("core/src/quic.cpp core/src/shared.hpp", ("tls", "insecure"))
        self.assertEqual(cck.check(page), [])

    def test_a_key_only_the_other_kind_calls_into_fails(self):
        # Case 1 of #2569: `path` is read in the shared file, but only by a helper `wt`
        # calls. Under a per-file union `quic` documenting it would pass.
        page = self.page("core/src/quic.cpp core/src/shared.hpp", ("tls", "insecure", "path"))
        failures = cck.check(page)
        self.assertEqual(
            failures,
            ["core/src/quic.cpp + core/src/shared.hpp: page documents key `path` that the file does not read"],
        )

    def test_another_kinds_source_as_shared_is_refused(self):
        # Case 2 of #2569: listing `wt.cpp` as shared would union its `path` into `quic`.
        page = self.page("core/src/quic.cpp core/src/wt.cpp", ("insecure", "path"))
        failures = cck.check(page)
        self.assertTrue(any("names core/src/wt.cpp as shared" in f for f in failures), failures)
        self.assertTrue(any("documents key `path`" in f for f in failures), failures)

    def test_a_shared_function_no_kind_calls_fails(self):
        # The per-file union used to fail an unreached helper's key as undocumented; per
        # function it must still fail, not drop out of view.
        (cck.ROOT / "core" / "src" / "shared.hpp").write_text(
            self.SHARED + 'inline int orphan(const config_reader_t& c) { return c.u16("port") ? 1 : 0; }\n'
        )
        page = self.page("core/src/quic.cpp core/src/shared.hpp", ("tls", "insecure"))
        self.assertEqual(
            cck.check(page),
            ["core/src/shared.hpp: orphan() reads keys (port) but no block naming this file "
             "has an own file that calls it"],
        )

    def test_a_member_call_credits_the_helper(self):
        (cck.ROOT / "core" / "src" / "quic.cpp").write_text(self.QUIC + "(void)ep.wt_path(cfg);\n")
        page = self.page("core/src/quic.cpp core/src/shared.hpp", ("tls", "insecure", "path"))
        self.assertEqual(cck.check(page), [])

    def test_a_call_in_a_log_string_does_not_credit_the_helper(self):
        (cck.ROOT / "core" / "src" / "quic.cpp").write_text(self.QUIC + 'log("wt_path(cfg)");\n')
        page = self.page("core/src/quic.cpp core/src/shared.hpp", ("tls", "insecure", "path"))
        self.assertIn(
            "core/src/quic.cpp + core/src/shared.hpp: page documents key `path` that the file does not read",
            cck.check(page),
        )

    def test_a_transport_source_with_no_block_is_refused_as_shared(self):
        # A `.cpp` no block owns, named only as shared, is refused AND still reported as
        # undocumented: a shared entry is not a block for the file.
        (cck.ROOT / "core" / "src" / "can.cpp").write_text(
            'const config_reader_t reader(raw);\n(void)reader.name("ifname");\n'
        )
        page = self.page("core/src/quic.cpp core/src/shared.hpp core/src/can.cpp", ("tls", "insecure"))
        failures = cck.check(page)
        self.assertTrue(any("names core/src/can.cpp as shared" in f for f in failures), failures)
        self.assertTrue(any(f.startswith("core/src/can.cpp reads connection-config keys") for f in failures), failures)

    def test_a_dead_key_still_fails(self):
        page = self.page("core/src/quic.cpp core/src/shared.hpp", ("tls", "insecure", "ca"))
        failures = cck.check(page)
        self.assertEqual(
            failures,
            ["core/src/quic.cpp + core/src/shared.hpp: page documents key `ca` that the file does not read"],
        )

    def test_a_shared_key_no_function_owns_fails(self):
        (cck.ROOT / "core" / "src" / "shared.hpp").write_text(
            self.SHARED + 'inline void own(const wire::tlv_t* t) { const config_reader_t c(t); (void)c.name("ca"); }\n'
        )
        page = self.page("core/src/quic.cpp core/src/shared.hpp", ("tls", "insecure"))
        failures = cck.check(page)
        self.assertTrue(any("shared.hpp: reads key `ca` outside a function" in f for f in failures), failures)


class LivePage(unittest.TestCase):
    """The committed page, against the committed source."""

    def test_gate_is_green_on_main(self):
        self.assertEqual(cck.check(), [])

    def test_the_gate_sees_the_quic_tls_keys(self):
        # The keys #1012 was filed about. If the extractor ever stops seeing them,
        # the whole page could be deleted and the gate would still pass.
        # The kind reads `tls` through the shared `select_tls_profile` (#2440), so the
        # kind's set is the union over the files its page block names.
        blocks, errors = cck.parse_page(cck.PAGE.read_text(encoding="utf-8"))
        self.assertEqual(errors, [])
        files = next(k for k in blocks if k[0] == "core/src/transport_quic.cpp")
        quic = {}
        for rel in files:
            quic.update(cck.derive_keys((cck.ROOT / rel).read_text()))
        self.assertEqual(quic.get("tls"), "name")
        self.assertEqual(quic.get("insecure"), "flag")

    def test_a_dead_key_on_a_shared_block_is_still_caught(self):
        # The union over a block's files must not hide a row neither file reads.
        page = "\n".join(
            [
                "<!-- config-keys:begin core/src/transport_quic.cpp core/src/msquic_endpoint.hpp -->",
                "| `tls` | `NAME` utf-8 | | | |",
                "| `insecure` | `VALUE` u8 (flag) | | | |",
                "| `ca` | `NAME` utf-8 | | | |",
                "<!-- config-keys:end -->",
            ]
        )
        failures = cck.check(page)
        self.assertTrue(any("documents key `ca`" in f for f in failures), failures)
        self.assertFalse(any("`tls`" in f or "`insecure`" in f for f in failures), failures)


if __name__ == "__main__":
    unittest.main()
