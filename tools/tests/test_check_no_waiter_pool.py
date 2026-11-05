#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the atomic-wait table link check (`tools/check_no_waiter_pool.py`, #2510).

The check reads ``nm -C -S`` output of a linked image, so these drive it end to end through a
stand-in ``nm`` that prints a canned symbol list, without a cross toolchain:

* an image carrying the waiter table fails, in the GCC 13-15 spelling (``__waiter_pool_base``,
  ``__atomic_wait_address``) and in the GCC 16 one (``__waitable_state``, ``__wait_impl``,
  ``__notify_impl``), and the failure names the offending symbol;
* a clean image of either era passes, and libtracer's own ``tr::park_wait`` / ``tr::park_wake``
  are not mistaken for the table;
* an unreadable image (too few symbols) is an error, never a pass.

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import contextlib
import io
import os
import pathlib
import stat
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))

import check_no_waiter_pool as cnw  # noqa: E402

# Filler that any image has: enough lines to clear the unreadable-image floor.
FILLER = [f"4200{i:04x} 00000010 T app_function_{i}" for i in range(150)]

# libtracer's park functions, which every clean image of this tree carries.
PARK = [
    "42010000 00000040 W void tr::park_wait<unsigned int>(std::atomic<unsigned int> const&, "
    "unsigned int, std::memory_order)",
    "42010040 00000030 W void tr::park_wake<unsigned int>(std::atomic<unsigned int> const&)",
    "4080a000 00000040 u tr::detail::park_stripe_of(void const*)::stripes",
]

# GCC 13-15: the header-only pool, a weak function-local static.
GCC13_TABLE = [
    "4080b000 00000800 u std::__detail::__waiter_pool_base::_S_for(void const*)::__w",
    "42011000 00000080 W void std::__detail::__atomic_wait_address_bare<std::__detail::"
    "__waiter<std::integral_constant<bool, true> >::_M_do_wait(int const*, int)::"
    "{lambda()#1}>(int const*, int)",
]

# GCC 16: the table moved into the library behind out-of-line entry points.
GCC16_TABLE = [
    "4080b000 00000800 b std::__detail::(anonymous namespace)::__waitable_state::_S_state_for"
    "(void const*)::states",
    "42011000 000000c0 T std::__detail::__wait_impl(void const*, std::__detail::"
    "__wait_args_base&)",
    "420110c0 00000060 T std::__detail::__notify_impl(void const*, bool, std::__detail::"
    "__wait_args_base const&)",
]


class GateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self.tmp.name)
        self.elf = self.dir / "image.elf"
        self.elf.write_bytes(b"\x7fELF")

    def tearDown(self):
        self.tmp.cleanup()

    def run_gate(self, symbols):
        """@brief Run the check against a stand-in nm printing @p symbols; (rc, stdout, stderr)."""
        listing = self.dir / "symbols.txt"
        listing.write_text("\n".join(symbols) + "\n")
        nm = self.dir / "fake-nm"
        nm.write_text(f"#!/bin/sh\ncat '{listing}'\n")
        nm.chmod(nm.stat().st_mode | stat.S_IXUSR)
        argv = ["check_no_waiter_pool.py", "--elf", str(self.elf), "--nm", str(nm)]
        out, err = io.StringIO(), io.StringIO()
        old = sys.argv
        try:
            sys.argv = argv
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = cnw.main()
        finally:
            sys.argv = old
        return rc, out.getvalue(), err.getvalue()

    def test_gcc13_era_table_fails(self):
        for sym in GCC13_TABLE:
            with self.subTest(sym=sym):
                rc, out, _ = self.run_gate(FILLER + PARK + [sym])
                self.assertEqual(rc, 1)
                self.assertIn(sym, out)

    def test_gcc16_era_table_fails(self):
        for sym in GCC16_TABLE:
            with self.subTest(sym=sym):
                rc, out, _ = self.run_gate(FILLER + PARK + [sym])
                self.assertEqual(rc, 1)
                self.assertIn(sym, out)

    def test_clean_image_passes(self):
        rc, out, _ = self.run_gate(FILLER + PARK)
        self.assertEqual(rc, 0)
        self.assertIn("OK:", out)

    def test_every_fragment_is_exercised(self):
        """@brief Each banned fragment is caught by some fixture symbol, so none is dead."""
        for frag in cnw.BANNED_FRAGMENTS:
            with self.subTest(frag=frag):
                self.assertTrue(any(frag in s for s in GCC13_TABLE + GCC16_TABLE))
        self.assertEqual(cnw.waiter_pool_hits(FILLER + PARK), [])

    def test_unreadable_image_is_an_error(self):
        rc, _, err = self.run_gate(PARK)
        self.assertEqual(rc, 2)
        self.assertIn("unreadable image", err)


if __name__ == "__main__":
    unittest.main()
