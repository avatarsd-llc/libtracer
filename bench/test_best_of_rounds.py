#!/usr/bin/env python3
"""@brief Tests for best_of_rounds.py — the best-of-rounds reduction behind the comparison charts.

The property that matters here is not arithmetic, it is EVEN-HANDEDNESS: the reduction must
do exactly the same thing to the Zenoh rows as to the libtracer rows. A reduction that
quietly favoured one `system` value would be invisible in the published charts, which show
only the reduced output, so it is pinned by test rather than by reading.
"""
import io
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
TOOL = HERE / "best_of_rounds.py"


def row(system, mode, size, fan, ep, pub, deliv, mb, p50, p99, mean):
    """@brief One 12-field RESULT line in the harness's tab-separated contract."""
    return "\t".join(
        ["RESULT", system, mode, str(size), str(fan), str(ep),
         str(pub), str(deliv), str(mb), str(p50), str(p99), str(mean)])


def run(lines):
    """@brief Feed lines to the tool and return its stdout rows split into fields."""
    p = subprocess.run([sys.executable, str(TOOL)], input="\n".join(lines) + "\n",
                       capture_output=True, text=True, check=True)
    return [ln.split("\t") for ln in p.stdout.splitlines() if ln.strip()]


def test_takes_best_per_metric():
    """@brief Max on the throughput columns, min on the latency columns, per point."""
    out = run([
        row("libtracer", "inproc", 64, 1, 1, 100, 100, 1.0, 50, 60, 55),
        row("libtracer", "inproc", 64, 1, 1, 200, 200, 2.0, 40, 70, 45),
    ])
    assert len(out) == 1, out
    f = out[0]
    assert f[6] == "200" and f[7] == "200" and f[8] == "2.0", f  # throughput: max
    assert f[9] == "40" and f[10] == "60" and f[11] == "45", f   # latency: min (per column)


def test_both_engines_reduced_identically():
    """@brief The reduction is symmetric — the Zenoh arm gets the same best-of the libtracer arm does.

    This is the fairness property of the whole comparison. Both engines are handed the
    identical pair of rounds; the two reduced rows must agree on every measured column.
    """
    out = run([
        row("libtracer", "inproc", 64, 1, 1, 100, 100, 1.0, 50, 60, 55),
        row("zenoh", "inproc", 64, 1, 1, 100, 100, 1.0, 50, 60, 55),
        row("libtracer", "inproc", 64, 1, 1, 200, 200, 2.0, 40, 70, 45),
        row("zenoh", "inproc", 64, 1, 1, 200, 200, 2.0, 40, 70, 45),
    ])
    assert len(out) == 2, out
    lt = [f for f in out if f[1] == "libtracer"][0]
    zn = [f for f in out if f[1] == "zenoh"][0]
    assert lt[6:] == zn[6:], (lt, zn)


def test_points_are_kept_distinct():
    """@brief Different (system, mode, size, fan, ep) tuples never collapse into one row."""
    out = run([
        row("libtracer", "inproc", 64, 1, 1, 100, 100, 1.0, 50, 60, 55),
        row("libtracer", "inproc", 64, 8, 1, 900, 900, 9.0, 10, 20, 15),
        row("libtracer", "inproc-path", 64, 1, 1, 300, 300, 3.0, 30, 40, 35),
    ])
    assert len(out) == 3, out
    assert [f[2] + "/" + f[4] for f in out] == ["inproc/1", "inproc/8", "inproc-path/1"], out


def test_tail_rows_pass_through_unreduced():
    """@brief RESULT_TAIL keeps its first observation; a p999 is a jitter floor, not a best-case."""
    tail = lambda p999: "\t".join(  # noqa: E731
        ["RESULT_TAIL", "libtracer", "net-udp", "64", "1", "1", "20000", "50", "60", str(p999),
         "900", "1"])
    out = run([tail(500), tail(100)])
    assert len(out) == 1, out
    assert out[0][9] == "500", out  # the FIRST p999, not the minimum


def test_order_is_preserved():
    """@brief First-seen order survives the reduction, so the charts' series stay ordered."""
    out = run([
        row("libtracer", "inproc", 1, 1, 1, 10, 10, 0.1, 10, 10, 10),
        row("zenoh", "inproc", 1, 1, 1, 10, 10, 0.1, 10, 10, 10),
        row("libtracer", "inproc", 8192, 1, 1, 10, 10, 0.1, 10, 10, 10),
        row("libtracer", "inproc", 1, 1, 1, 99, 99, 0.9, 1, 1, 1),
    ])
    assert [(f[1], f[3]) for f in out] == [
        ("libtracer", "1"), ("zenoh", "1"), ("libtracer", "8192")], out


def main():
    """@brief Run every test_* in this module, reporting the first failure loudly."""
    tests = [v for k, v in sorted(globals().items())
             if k.startswith("test_") and callable(v)]
    for t in tests:
        t()
        print(f"ok  {t.__name__}")
    print(f"{len(tests)} passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
