// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// Draws the UNIFIED per-family benchmark-history charts on the Performance page.
// Data (emitted by bench/render_history.py at docs-build time) comes from the
// <script type="application/json" id="ph-data"> block. One chart per FAMILY,
// every series of the family on shared axes — and for families whose series
// differ by a NUMERIC parameter (fan-out, payload, topics, fold width, threads)
// the same three-axis data (commit x parameter x value) is viewable four ways:
//
//   trend    value vs commit, one line per parameter        (default)
//   sweep    value vs parameter, one line per commit (recency-faded, releases hot)
//   heatmap  commit x parameter grid, color = value
//   3D       isometric surface, commit x parameter x value
//
// Release tags arrive as {i, label, approx} per suite and are drawn as labeled
// dashed verticals ('≈' = tag commit is not itself a recorded point; the marker
// sits at the nearest following recorded commit). Series colors are assigned
// once per label page-wide (the generator embeds a global color index), so
// "fan 8" is the same color on the latency and the throughput chart.
//
// Every commit-axis view is drawn over a selectable commit RANGE (two sliders per card,
// full by default), and a family whose series pair across two engines adds a RATIO
// toggle: the per-commit quotient of the two arms, computed here from same-run pairs.
//
// Each block carries TWO payloads — the bench-local (fixed pinned host) store, which
// the page opens on, and the GitHub-hosted store (the portability envelope) — and a
// global source selector switches every chart between them in place, remembering an
// explicit choice in localStorage. The two stores answer different questions and are
// never drawn on one axis.
// Vanilla JS + inline SVG only — self-contained, no CDN, theme-aware via CSS vars.
(function () {

  var NPAL = 12;
  function col(ci) { return "var(--ph-c" + (ci % NPAL) + ")"; }

  function fmtNs(v) { return v >= 1e6 ? (v / 1e6).toFixed(v < 1e7 ? 1 : 0) + " ms" : v >= 1e3 ? (v / 1e3).toFixed(v < 1e4 ? 1 : 0) + " µs" : (v >= 100 ? Math.round(v) : +v.toFixed(1)) + " ns"; }
  function fmtRate(v) { return v >= 1e6 ? (v / 1e6).toFixed(v < 1e7 ? 1 : 0) + " M/s" : v >= 1e3 ? (v / 1e3).toFixed(0) + " k/s" : Math.round(v) + "/s"; }
  function fmtNum(v) { return v >= 1e6 ? (v / 1e6).toFixed(1) + " M" : v >= 1e3 ? (v / 1e3).toFixed(1) + " k" : (v === Math.round(v) ? "" + v : +v.toFixed(1)); }
  function fmtBytes(v) { return v >= 1024 ? (v / 1024) + " KB" : v + " B"; }
  function fmtCount(v) { return v >= 1000 ? (v / 1000) + "k" : "" + v; }
  // fmtMB is the one formatter the deleted ltz_compare.js had and this one did not.
  function fmtMB(v) { return v >= 1000 ? (v / 1000).toFixed(1) + " GB/s" : Math.round(v) + " MB/s"; }
  // A dimensionless quotient. Printed with the '×' so it can never be mistaken for the
  // absolute number it was divided out of.
  function fmtRatio(v) { return (v >= 100 ? Math.round(v) : v >= 10 ? v.toFixed(1) : v.toFixed(2)) + "×"; }
  /** @brief Width of the trend charts' "last N merges" zoom (#1801). */
  var LAST = 30;
  /** @brief Default trailing window, in recorded points, of the noise band (#1848). */
  var BAND_N = 10;
  /** @brief The windows the band picker offers; 0 turns the band off. */
  var BAND_NS = [0, 5, 10, 20, 30];
  /** @brief A coefficient of variation as a percentage, one decimal below 10%. */
  function fmtPct(v) { v *= 100; return (v < 10 ? v.toFixed(1) : Math.round(v)) + "%"; }
  var FMT = { ns: fmtNs, rate: fmtRate, num: fmtNum, bytes: fmtBytes, count: fmtCount, mb: fmtMB, ratio: fmtRatio };

  function logTicks(min, max) {
    var lo = Math.floor(Math.log10(min)), hi = Math.ceil(Math.log10(max)), t = [];
    for (var e = lo; e <= hi; e++) t.push(Math.pow(10, e));
    return t;
  }
  function linTicks(min, max) {
    var span = max - min || max || 1, step = Math.pow(10, Math.floor(Math.log10(span))), n = span / step;
    var s = n < 2 ? step / 2 : n < 5 ? step : step * 2;
    var lo = Math.floor(min / s) * s, hi = Math.ceil(max / s) * s, t = [];
    for (var v = lo; v <= hi + 1e-9; v += s) t.push(v);
    return t;
  }

  // sequential color ramp for heatmap / 3D fills (deep blue -> teal -> warm),
  // readable on both themes.
  var RAMP = [[28, 42, 110], [37, 99, 201], [42, 156, 148], [118, 190, 74], [238, 198, 62]];
  function ramp(t) {
    t = Math.max(0, Math.min(1, t));
    var x = t * (RAMP.length - 1), i = Math.min(RAMP.length - 2, Math.floor(x)), f = x - i;
    function mix(a, b) { return Math.round(a + (b - a) * f); }
    return "rgb(" + mix(RAMP[i][0], RAMP[i + 1][0]) + "," + mix(RAMP[i][1], RAMP[i + 1][1]) + "," + mix(RAMP[i][2], RAMP[i + 1][2]) + ")";
  }

  // family-wide value normalizer (log when the family axis is log; zeros clamp)
  function normalizer(c) {
    var all = [];
    c.series.forEach(function (s) { s.pts.forEach(function (p) { if (!c.log || p[1] > 0) all.push(p[1]); }); });
    if (!all.length) all = [1];
    var lo = Math.min.apply(null, all), hi = Math.max.apply(null, all);
    if (c.log) {
      var l0 = Math.log10(lo), l1 = Math.log10(hi);
      return { lo: lo, hi: hi, n: function (v) { return l1 <= l0 ? 0.5 : (Math.log10(Math.max(v, lo)) - l0) / (l1 - l0); } };
    }
    return { lo: lo, hi: hi, n: function (v) { return hi <= lo ? 0.5 : (v - lo) / (hi - lo); } };
  }

  // entry indices where EVERY series of the family has a value — the dense
  // commit sub-grid the sweep / heatmap / 3D views are honest over.
  function denseIdxs(c, N) {
    var have = c.series.map(function (s) {
      var m = {}; s.pts.forEach(function (p) { m[p[0]] = true; }); return m;
    });
    var out = [];
    for (var i = 0; i < N; i++) {
      var ok = true;
      for (var s = 0; s < have.length; s++) if (!have[s][i]) { ok = false; break; }
      if (ok) out.push(i);
    }
    return out;
  }

  /** @brief The trailing-window dispersion of one series: its noise band (#1848).
   *
   * For every recorded point that has at least @p n recorded points behind it (itself
   * included), the window is the series' own last @p n values, and the result row is
   * `[slot, p10, p90, min, max, cv]`: the 10th and 90th percentiles (linear interpolation
   * between order statistics), the window's extremes, and its coefficient of variation
   * (sample standard deviation over mean). A series with fewer than @p n points gets no
   * band at all rather than a band over a shorter window, so every band on a chart
   * describes the same amount of history.
   *
   * The window counts RECORDED points, not slots: a contaminated sample is already
   * omitted from `pts`, so it cannot widen the band it would otherwise dominate.
   * Self-contained on purpose: bench/test_render_history.py runs it under node against a
   * series with a known spread.
   */
  function windowBand(pts, n) {
    var out = [];
    if (!(n >= 2) || pts.length < n) return out;
    function q(w, f) {
      var x = f * (w.length - 1), lo = Math.floor(x), hi = Math.ceil(x);
      return w[lo] + (w[hi] - w[lo]) * (x - lo);
    }
    for (var j = n - 1; j < pts.length; j++) {
      var w = [], sum = 0, ss = 0;
      for (var k = j - n + 1; k <= j; k++) { w.push(pts[k][1]); sum += pts[k][1]; }
      var mean = sum / n;
      w.forEach(function (v) { ss += (v - mean) * (v - mean); });
      w.sort(function (a, b) { return a - b; });
      out.push([pts[j][0], q(w, 0.1), q(w, 0.9), w[0], w[n - 1], mean ? Math.sqrt(ss / (n - 1)) / Math.abs(mean) : 0]);
    }
    return out;
  }

  function lookup(se) { var m = {}; se.pts.forEach(function (p) { m[p[0]] = p[1]; }); return m; }

  // ---------------------------------------------------------------- ratio --
  /** @brief The per-commit quotient chart of a paired family, or null if it has no pairs.
   *
   * Offered only on the bench-local store, where both arms of a `(zenoh )?` point come
   * from the same pass on the same pinned machine, so the quotient divides that machine's
   * speed on the day out of the answer to first order. The cancellation is partial:
   * measured at p50 over bench-local's full store of 12 runs, the quotient's spread is
   * about a third below the libtracer line's own. The hosted store keeps the best of
   * three runners *per series*, so its two arms need not share a pass; render_history
   * builds that payload without `ratio` and its cards carry no toggle (#1769).
   *
   * Pairing is therefore strict and cheap: same store, same suite, same commit index,
   * same shape key (`rk`). A commit where only one arm recorded a value contributes no
   * point rather than an interpolated one — a half-measured pair is not a comparison.
   *
   * Polarity depends on which direction the underlying metric runs, so it is stated on
   * the y-axis in words rather than left to the reader: for a latency metric a quotient
   * above 1 means Zenoh took longer, i.e. libtracer is faster; for throughput the same
   * statement is a quotient below 1.
   */
  function ratioChart(c) {
    var num = {}, den = {};
    c.series.forEach(function (se) {
      if (se.arm === "num") num[se.rk] = se;
      else if (se.arm === "den") den[se.rk] = se;
    });
    var out = [];
    Object.keys(den).forEach(function (k) {
      var n = num[k], d = den[k];
      if (!n) return;
      var dm = lookup(d), pts = [];
      n.pts.forEach(function (p) {
        var dv = dm[p[0]];
        if (dv === undefined || !(dv > 0)) return;
        pts.push([p[0], p[1] / dv]);
      });
      if (pts.length) out.push({ label: k, ci: d.ci, pts: pts, pv: d.rpv });
    });
    if (!out.length) return null;
    out.sort(function (a, b) { return a.pv - b.pv; });
    var thr = c.suite === "throughput";
    return {
      id: c.id, section: c.section, title: c.title, cond: c.cond, src: c.src,
      suite: c.suite, fmt: "ratio", log: true, unity: true, px: c.ratio.px, series: out,
      ylabel: "zenoh ÷ libtracer · " + (thr ? "<1 = libtracer faster" : ">1 = libtracer faster")
    };
  }

  /** @brief A dashed reference line at the value 1 — the ratio view's break-even.
   *
   * Emitted only for the ratio chart. On an absolute chart 1 is an arbitrary value; on a
   * quotient it is the whole question, and a chart that makes the reader find it by
   * reading tick labels has buried its own answer.
   */
  function unityMark(Y, x0, x1, lo, hi) {
    if (!(1 >= lo && 1 <= hi)) return "";
    var y = Y(1).toFixed(1);
    return '<line class="ph-unity" x1="' + x0 + '" y1="' + y + '" x2="' + x1 + '" y2="' + y + '"/>'
      + '<text class="ph-unitylab" x="' + (x0 + 6) + '" y="' + (+y - 4) + '">parity (1×)</text>';
  }

  // ---------------------------------------------------------------- range --
  /** @brief The chart and suite restricted to recorded commits [a, b].
   *
   * A slice, not a filter flag: every renderer already reads a commit axis as "index 0 to
   * shas.length - 1", so re-basing the points and the markers onto a shorter axis reuses
   * all four views unchanged rather than teaching each of them about a window. Release and
   * instrument markers outside the window are dropped (they mark nothing on this axis);
   * the ones inside are re-based with it.
   *
   * The full range returns the originals untouched, so the default view costs nothing.
   */
  function sliceRange(c, suite, a, b) {
    var N = suite.shas.length;
    if (a <= 0 && b >= N - 1) return { c: c, suite: suite };
    function shift(marks) {
      return (marks || []).filter(function (r) { return r.i >= a && r.i <= b; })
        .map(function (r) { return { i: r.i - a, label: r.label, approx: r.approx }; });
    }
    var c2 = {};
    for (var k in c) if (Object.prototype.hasOwnProperty.call(c, k)) c2[k] = c[k];
    c2.series = c.series.map(function (se) {
      var s2 = {};
      for (var k2 in se) if (Object.prototype.hasOwnProperty.call(se, k2)) s2[k2] = se[k2];
      s2.pts = se.pts.filter(function (p) { return p[0] >= a && p[0] <= b; })
        .map(function (p) { return [p[0] - a, p[1]]; });
      if (se.band) {
        s2.band = se.band.filter(function (t) { return t[0] >= a && t[0] <= b; })
          .map(function (t) { return [t[0] - a].concat(t.slice(1)); });
      }
      if (se.steps) {
        s2.steps = se.steps.filter(function (t) { return t[0] >= a && t[0] <= b; })
          .map(function (t) { return [t[0] - a, t[1], t[2]]; });
      }
      return s2;
    });
    return {
      c: c2,
      suite: {
        shas: suite.shas.slice(a, b + 1),
        msgs: (suite.msgs || []).slice(a, b + 1),
        dates: suite.dates ? suite.dates.slice(a, b + 1) : undefined,
        host: suite.host,
        hosts: suite.hosts ? suite.hosts.slice(a, b + 1) : undefined,
        releases: shift(suite.releases), instruments: shift(suite.instruments)
      }
    };
  }

  function relMark(x, y0, y1, r) {
    return '<line class="ph-rel" x1="' + x.toFixed(1) + '" y1="' + y0 + '" x2="' + x.toFixed(1) + '" y2="' + y1 + '"/>' +
      '<text class="ph-rellab" x="' + (x + 4).toFixed(1) + '" y="' + (y0 + 11) + '">' + (r.approx ? "≈ " : "") + r.label + "</text>";
  }

  // An INSTRUMENT marker, visually distinct from a release. A release says the code
  // changed; this says the thing doing the MEASURING changed, so points on either side are
  // not comparable. The release line is already dashed, so this is DOTTED and a different
  // hue, and labelled at the foot of the plot rather than the head — a chart carrying both
  // must not read as one kind of event.
  function instrMark(x, y0, y1, r) {
    return '<line class="ph-instr" x1="' + x.toFixed(1) + '" y1="' + y0 + '" x2="' + x.toFixed(1) + '" y2="' + y1 + '"/>' +
      '<text class="ph-instrlab" x="' + (x + 4).toFixed(1) + '" y="' + (y1 - 4) + '">⚙</text>' +
      '<title>' + r.label + ' — the benchmark itself changed here; points either side are not directly comparable</title>';
  }

  /** @brief A sustained-step marker (#1770): a triangle in the series' colour at the first
   * commit that measured the new level, pointing the way the row moved.
   *
   * Drawn at the NEW level, not on the line: the step's first point may be a contaminated
   * sample the line omits, and the level is what the step is about. The detector and its
   * rule live in bench/step_detect.py; this only draws what the payload carries.
   */
  function stepMark(x, y, cc, se, t, yf) {
    var up = t[2] > t[1], d = up ? -1 : 1, r = 6;
    var pct = ((t[2] / t[1] - 1) * 100).toFixed(0);
    var pts = x.toFixed(1) + "," + (y + d * r).toFixed(1) + " " + (x - r).toFixed(1) + "," + (y - d * r * 0.6).toFixed(1) +
      " " + (x + r).toFixed(1) + "," + (y - d * r * 0.6).toFixed(1);
    return '<polygon class="ph-step" points="' + pts + '" fill="' + cc + '"><title>sustained step · ' + se.label + ": " +
      yf(t[1]) + " → " + yf(t[2]) + " (" + (up ? "+" : "") + pct + "%)</title></polygon>";
  }

  /*
   * Gap markers (#1890): a commit inside a series' span with no trusted value for it is
   * drawn as missing, not as nothing. A faint dashed bridge joins the recorded points
   * either side, and each missing slot gets a hollow ring at the bridge's height. The
   * bridge is dashed and the ring hollow so neither reads as a measurement; the ring's
   * title names the commit and says the value was hidden or not recorded.
   */
  function gapMarks(se, X, Y, cc, suite) {
    var s = "";
    for (var j = 1; j < se.pts.length; j++) {
      var a = se.pts[j - 1], b = se.pts[j];
      if (b[0] === a[0] + 1) continue;
      s += '<line class="ph-gap" stroke="' + cc + '" x1="' + X(a[0]).toFixed(1) + '" y1="' + Y(a[1]).toFixed(1) +
        '" x2="' + X(b[0]).toFixed(1) + '" y2="' + Y(b[1]).toFixed(1) + '"/>';
      for (var k = a[0] + 1; k < b[0]; k++) {
        var f = (k - a[0]) / (b[0] - a[0]);
        s += '<circle class="ph-gapdot" stroke="' + cc + '" cx="' + X(k).toFixed(1) + '" cy="' +
          (Y(a[1]) + (Y(b[1]) - Y(a[1])) * f).toFixed(1) + '" r="2.2"><title>' + se.label + " · " +
          (suite.shas[k] || "") + ": no trusted value</title></circle>";
      }
    }
    return s;
  }

  // ---------------------------------------------------------------- trend --
  function renderTrend(c, suite) {
    var N = suite.shas.length;
    var W = 900, H = 330, m = { l: 64, r: 14, t: 26, b: 46 };
    var pw = W - m.l - m.r, ph = H - m.t - m.b;
    function X(i) { return m.l + (N <= 1 ? pw / 2 : (i / (N - 1)) * pw); }
    var all = [];
    c.series.forEach(function (s) {
      s.pts.forEach(function (p) { if (!c.log || p[1] > 0) all.push(p[1]); });
      // A window may reach back past the start of a sliced range, so its extremes can lie
      // outside the visible points; the domain takes them in rather than clipping the band.
      (s.band || []).forEach(function (t) { [t[3], t[4]].forEach(function (v) { if (!c.log || v > 0) all.push(v); }); });
    });
    if (!all.length) all = [1];
    var ymin = Math.min.apply(null, all), ymax = Math.max.apply(null, all), yt;
    var floor = ymin;
    if (c.log) { yt = logTicks(ymin, ymax); } else { yt = linTicks(ymin, ymax); }
    ymin = yt[0]; ymax = yt[yt.length - 1];
    function Y(v) {
      if (c.log) {
        var vv = v > 0 ? v : floor;
        return m.t + (1 - (Math.log10(vv) - Math.log10(ymin)) / (Math.log10(ymax) - Math.log10(ymin))) * ph;
      }
      return m.t + (1 - (v - ymin) / (ymax - ymin)) * ph;
    }
    var yf = FMT[c.fmt] || fmtNum;
    var s = '<svg viewBox="0 0 ' + W + " " + H + '" role="img" aria-label="' + c.title + '">';
    if (c.log) {
      var e0 = Math.round(Math.log10(ymin)), e1 = Math.round(Math.log10(ymax));
      for (var e = e0; e < e1; e++) for (var k = 2; k <= 9; k++) {
        var v = k * Math.pow(10, e); if (v <= ymin || v >= ymax) continue;
        s += '<line class="ph-glm" x1="' + m.l + '" y1="' + Y(v).toFixed(1) + '" x2="' + (W - m.r) + '" y2="' + Y(v).toFixed(1) + '"/>';
      }
    }
    yt.forEach(function (v) {
      var y = Y(v);
      s += '<line class="ph-gl" x1="' + m.l + '" y1="' + y.toFixed(1) + '" x2="' + (W - m.r) + '" y2="' + y.toFixed(1) + '"/>';
      s += '<text class="ph-tick" x="' + (m.l - 8) + '" y="' + (y + 3).toFixed(1) + '" text-anchor="end">' + yf(v) + "</text>";
    });
    var step = Math.max(1, Math.ceil((N - 1) / 6)), ticks = [];
    for (var i = 0; i < N; i += step) ticks.push(i);
    if (ticks[ticks.length - 1] !== N - 1) ticks.push(N - 1);
    ticks.forEach(function (i2) {
      var x = X(i2);
      s += '<line class="ph-gl" x1="' + x.toFixed(1) + '" y1="' + m.t + '" x2="' + x.toFixed(1) + '" y2="' + (m.t + ph) + '"/>';
      s += '<text class="ph-tick" transform="translate(' + x.toFixed(1) + " " + (m.t + ph + 12) + ') rotate(-35)" text-anchor="end">' + suite.shas[i2] + "</text>";
    });
    s += '<rect class="ph-frame" x="' + m.l + '" y="' + m.t + '" width="' + pw + '" height="' + ph + '"/>';
    s += '<text class="ph-axtitle" transform="translate(13 ' + (m.t + ph / 2) + ') rotate(-90)" text-anchor="middle">' + c.ylabel + "</text>";
    if (c.unity) s += unityMark(Y, m.l, W - m.r, ymin, ymax);
    (suite.releases || []).forEach(function (r) { if (r.i < N) s += relMark(X(r.i), m.t, m.t + ph, r); });
    (suite.instruments || []).forEach(function (r) { if (r.i < N) s += instrMark(X(r.i), m.t, m.t + ph, r); });
    // Noise bands go UNDER every line (#1848): p10-p90 shaded, min and max as faint
    // whiskers. Broken into runs of consecutive slots exactly like the lines are, so a
    // band never spans a commit its series did not record.
    c.series.forEach(function (se) {
      var cc = col(se.ci), runs = [], run = [];
      (se.band || []).forEach(function (t, j) {
        if (j && t[0] !== se.band[j - 1][0] + 1) { runs.push(run); run = []; }
        run.push(t);
      });
      if (run.length) runs.push(run);
      runs.forEach(function (r) {
        if (r.length < 2) return;
        function at(col2) { return r.map(function (t) { return X(t[0]).toFixed(1) + "," + Y(t[col2]).toFixed(1); }); }
        s += '<polygon class="ph-bandfill" fill="' + cc + '" points="' + at(2).concat(at(1).reverse()).join(" ") + '"/>';
        s += '<polyline class="ph-whisk" stroke="' + cc + '" points="' + at(4).join(" ") + '"/>';
        s += '<polyline class="ph-whisk" stroke="' + cc + '" points="' + at(3).join(" ") + '"/>';
      });
    });
    c.series.forEach(function (se) {
      var cc = col(se.ci);
      // One polyline per run of consecutive slots (#1801): the axis is one slot per commit
      // for every series of the store, so a commit this series did not record is a GAP in
      // its slot, never a straight segment that reads as a measured trend across it.
      var runs = [], run = [];
      se.pts.forEach(function (p, j) {
        if (j && p[0] !== se.pts[j - 1][0] + 1) { runs.push(run); run = []; }
        run.push(X(p[0]).toFixed(1) + "," + Y(p[1]).toFixed(1));
      });
      if (run.length) runs.push(run);
      runs.forEach(function (r) {
        if (r.length > 1) s += '<polyline fill="none" stroke="' + cc + '" stroke-width="2" stroke-linejoin="round" stroke-linecap="round" points="' + r.join(" ") + '"/>';
      });
      se.pts.forEach(function (p, i3) {
        s += '<circle cx="' + X(p[0]).toFixed(1) + '" cy="' + Y(p[1]).toFixed(1) + '" r="' + (i3 === se.pts.length - 1 ? 3.4 : 2.2) + '" fill="' + cc + '"/>';
      });
      s += gapMarks(se, X, Y, cc, suite);
    });
    // Step markers go on top of every line, so a marker is never hidden under a neighbour.
    c.series.forEach(function (se) {
      (se.steps || []).forEach(function (t) { if (t[0] < N) s += stepMark(X(t[0]), Y(t[2]), col(se.ci), se, t, yf); });
    });
    s += '<line class="ph-cross" x1="0" y1="' + m.t + '" x2="0" y2="' + (m.t + ph) + '" style="display:none"/>';
    s += "</svg>";
    return { svg: s, hover: { kind: "trend", W: W, m: m, pw: pw, N: N, yf: yf } };
  }

  // ---------------------------------------------------------------- sweep --
  // value vs parameter, one polyline per recorded commit: older commits fade,
  // the newest is bold, release-tagged commits are highlighted + labeled.
  function renderSweep(c, suite, idxs) {
    var W = 900, H = 330, m = { l: 64, r: 14, t: 26, b: 44 };
    var pw = W - m.l - m.r, ph = H - m.t - m.b;
    var pvs = c.series.map(function (s) { return s.pv; });
    var pmin = Math.min.apply(null, pvs), pmax = Math.max.apply(null, pvs);
    function X(p) {
      if (c.px.log) return m.l + (Math.log2(p) - Math.log2(pmin)) / ((Math.log2(pmax) - Math.log2(pmin)) || 1) * pw;
      return m.l + (p - pmin) / ((pmax - pmin) || 1) * pw;
    }
    var all = [];
    c.series.forEach(function (s) { s.pts.forEach(function (p) { if (!c.log || p[1] > 0) all.push(p[1]); }); });
    var ymin = Math.min.apply(null, all), ymax = Math.max.apply(null, all);
    var yt = c.log ? logTicks(ymin, ymax) : linTicks(ymin, ymax);
    ymin = yt[0]; ymax = yt[yt.length - 1];
    function Y(v) {
      if (c.log) return m.t + (1 - (Math.log10(Math.max(v, ymin)) - Math.log10(ymin)) / (Math.log10(ymax) - Math.log10(ymin))) * ph;
      return m.t + (1 - (v - ymin) / (ymax - ymin)) * ph;
    }
    var yf = FMT[c.fmt] || fmtNum, xf = FMT[c.px.fmt] || fmtNum;
    var s = '<svg viewBox="0 0 ' + W + " " + H + '" role="img" aria-label="' + c.title + ' (parameter sweep per commit)">';
    yt.forEach(function (v) {
      var y = Y(v);
      s += '<line class="ph-gl" x1="' + m.l + '" y1="' + y.toFixed(1) + '" x2="' + (W - m.r) + '" y2="' + y.toFixed(1) + '"/>';
      s += '<text class="ph-tick" x="' + (m.l - 8) + '" y="' + (y + 3).toFixed(1) + '" text-anchor="end">' + yf(v) + "</text>";
    });
    pvs.forEach(function (p) {
      var x = X(p);
      s += '<line class="ph-gl" x1="' + x.toFixed(1) + '" y1="' + m.t + '" x2="' + x.toFixed(1) + '" y2="' + (m.t + ph) + '"/>';
      s += '<text class="ph-tick" x="' + x.toFixed(1) + '" y="' + (m.t + ph + 16) + '" text-anchor="middle">' + xf(p) + "</text>";
    });
    s += '<rect class="ph-frame" x="' + m.l + '" y="' + m.t + '" width="' + pw + '" height="' + ph + '"/>';
    s += '<text class="ph-axtitle" x="' + (m.l + pw / 2) + '" y="' + (H - 4) + '" text-anchor="middle">' + c.px.label + "</text>";
    s += '<text class="ph-axtitle" transform="translate(13 ' + (m.t + ph / 2) + ') rotate(-90)" text-anchor="middle">' + c.ylabel + "</text>";
    if (c.unity) s += unityMark(Y, m.l, W - m.r, ymin, ymax);
    var maps = c.series.map(lookup);
    var relAt = {};
    (suite.releases || []).forEach(function (r) { relAt[r.i] = r; });
    idxs.forEach(function (i, k) {
      var t = idxs.length <= 1 ? 1 : k / (idxs.length - 1);
      var last = k === idxs.length - 1, rel = relAt[i];
      var pts = c.series.map(function (se, si) { return X(se.pv).toFixed(1) + "," + Y(maps[si][i]).toFixed(1); }).join(" ");
      var cls = rel ? "ph-swrel" : (last ? "ph-swlast" : "ph-swold");
      var op = rel || last ? 1 : (0.14 + 0.5 * t);
      s += '<polyline class="' + cls + '" fill="none" opacity="' + op.toFixed(2) + '" points="' + pts + '">'
        + "<title>" + suite.shas[i] + (rel ? " — " + (rel.approx ? "≈ " : "") + rel.label : "") + "</title></polyline>";
      if (rel || last) {
        var lx = X(c.series[c.series.length - 1].pv), ly = Y(maps[c.series.length - 1][i]);
        s += '<text class="ph-swlab' + (rel ? " rel" : "") + '" x="' + (lx - 4).toFixed(1) + '" y="' + (ly - 6).toFixed(1) + '" text-anchor="end">'
          + (rel ? (rel.approx ? "≈ " : "") + rel.label + " · " : "") + suite.shas[i] + "</text>";
      }
    });
    return { svg: s, hover: null };
  }

  // -------------------------------------------------------------- heatmap --
  function renderHeat(c, suite, idxs) {
    var P = c.series.length, N = idxs.length;
    var W = 900, H = 90 + P * 26 + 60, m = { l: 96, r: 60, t: 26, b: 54 };
    var pw = W - m.l - m.r, ph = P * 26;
    var cw = pw / N;
    var nm = normalizer(c);
    var yf = FMT[c.fmt] || fmtNum;
    var maps = c.series.map(lookup);
    var s = '<svg viewBox="0 0 ' + W + " " + (m.t + ph + m.b) + '" role="img" aria-label="' + c.title + ' (heatmap)">';
    c.series.forEach(function (se, j) {
      var y = m.t + j * 26;
      s += '<text class="ph-tick" x="' + (m.l - 8) + '" y="' + (y + 17) + '" text-anchor="end">' + se.label + "</text>";
      idxs.forEach(function (i, k) {
        var v = maps[j][i];
        if (v === undefined) return;
        s += '<rect x="' + (m.l + k * cw).toFixed(1) + '" y="' + y + '" width="' + (cw + 0.5).toFixed(1) + '" height="26" fill="' + ramp(nm.n(v)) + '">'
          + "<title>" + suite.shas[i] + " · " + se.label + " = " + yf(v) + "</title></rect>";
      });
    });
    var step = Math.max(1, Math.ceil(N / 6));
    for (var k = 0; k < N; k += step) {
      var x = m.l + (k + 0.5) * cw;
      s += '<text class="ph-tick" transform="translate(' + x.toFixed(1) + " " + (m.t + ph + 12) + ') rotate(-35)" text-anchor="end">' + suite.shas[idxs[k]] + "</text>";
    }
    s += '<rect class="ph-frame" x="' + m.l + '" y="' + m.t + '" width="' + pw + '" height="' + ph + '"/>';
    (suite.releases || []).forEach(function (r) {
      var k2 = idxs.indexOf(r.i);
      if (k2 < 0) return;
      s += relMark(m.l + (k2 + 0.5) * cw, m.t - 2, m.t + ph, r);
    });
    // color legend (right edge): min -> max on the family scale
    var lx = W - m.r + 16;
    for (var q = 0; q < 40; q++) {
      s += '<rect x="' + lx + '" y="' + (m.t + ph - (q + 1) * (ph / 40)).toFixed(1) + '" width="12" height="' + (ph / 40 + 0.5).toFixed(1) + '" fill="' + ramp(q / 39) + '"/>';
    }
    s += '<text class="ph-tick" x="' + (lx + 16) + '" y="' + (m.t + 8) + '">' + yf(nm.hi) + "</text>";
    s += '<text class="ph-tick" x="' + (lx + 16) + '" y="' + (m.t + ph) + '">' + yf(nm.lo) + "</text>";
    s += "</svg>";
    return { svg: s, hover: null };
  }

  // ------------------------------------------------------------------- 3D --
  // isometric surface: u = commit (right/down), w = parameter (right/up),
  // height = normalized value. Quads painted back-to-front, wireframe on top.
  function render3D(c, suite, idxs) {
    var W = 900, H = 380;
    var N = idxs.length, P = c.series.length;
    var ax = 300, ay = 96, bx = 150, by = -104, zh = 150;
    var ox = 56, oy = H - 88;
    var nm = normalizer(c);
    var maps = c.series.map(lookup);
    var yf = FMT[c.fmt] || fmtNum, xf = FMT[(c.px && c.px.fmt) || "num"] || fmtNum;
    function PT(k, j, z) {
      var u = N <= 1 ? 0.5 : k / (N - 1), w = P <= 1 ? 0.5 : j / (P - 1);
      return [ox + u * ax + w * bx, oy + u * ay + w * by - z * zh];
    }
    function pstr(p) { return p[0].toFixed(1) + "," + p[1].toFixed(1); }
    var s = '<svg viewBox="0 0 ' + W + " " + H + '" role="img" aria-label="' + c.title + ' (3D surface)">';
    // base grid (z=0)
    for (var j = 0; j < P; j++) s += '<polyline class="ph-glm" fill="none" points="' + pstr(PT(0, j, 0)) + " " + pstr(PT(N - 1, j, 0)) + '"/>';
    for (var k = 0; k < N; k++) s += '<polyline class="ph-glm" fill="none" points="' + pstr(PT(k, 0, 0)) + " " + pstr(PT(k, P - 1, 0)) + '"/>';
    // release markers: a dashed wall line across the parameter axis at the commit
    (suite.releases || []).forEach(function (r) {
      var k2 = idxs.indexOf(r.i);
      if (k2 < 0) return;
      s += '<polyline class="ph-rel" fill="none" points="' + pstr(PT(k2, 0, 0)) + " " + pstr(PT(k2, P - 1, 0)) + '"/>';
      var lp = PT(k2, P - 1, 0);
      s += '<text class="ph-rellab" x="' + (lp[0] + 4).toFixed(1) + '" y="' + (lp[1] - 4).toFixed(1) + '">' + (r.approx ? "≈ " : "") + r.label + "</text>";
    });
    // surface quads, painter order: far (large j, small k screen-top) first
    var quads = [];
    for (var j2 = 0; j2 < P - 1; j2++) for (var k2 = 0; k2 < N - 1; k2++) {
      var vs = [maps[j2][idxs[k2]], maps[j2][idxs[k2 + 1]], maps[j2 + 1][idxs[k2 + 1]], maps[j2 + 1][idxs[k2]]];
      if (vs.some(function (v) { return v === undefined; })) continue;
      var zs = vs.map(function (v) { return nm.n(v); });
      var p1 = PT(k2, j2, zs[0]), p2 = PT(k2 + 1, j2, zs[1]), p3 = PT(k2 + 1, j2 + 1, zs[2]), p4 = PT(k2, j2 + 1, zs[3]);
      var depth = (p1[1] + p2[1] + p3[1] + p4[1]) / 4;
      var avg = (zs[0] + zs[1] + zs[2] + zs[3]) / 4;
      quads.push({ d: depth, pts: [p1, p2, p3, p4].map(pstr).join(" "), t: avg });
    }
    quads.sort(function (a, b) { return a.d - b.d; });
    quads.forEach(function (q) {
      s += '<polygon class="ph-quad" fill="' + ramp(q.t) + '" points="' + q.pts + '"/>';
    });
    // per-parameter ridge lines in the series' own colors (ties 3D to the legend)
    c.series.forEach(function (se, j3) {
      var pts = [];
      for (var k3 = 0; k3 < N; k3++) {
        var v = maps[j3][idxs[k3]];
        if (v === undefined) continue;
        pts.push(pstr(PT(k3, j3, nm.n(v))));
      }
      if (pts.length > 1) s += '<polyline fill="none" stroke="' + col(se.ci) + '" stroke-width="1.8" opacity="0.95" points="' + pts.join(" ") + '"><title>' + se.label + "</title></polyline>";
    });
    // axis labels + endpoint hints
    var a0 = PT(0, 0, 0), a1 = PT(N - 1, 0, 0), b1 = PT(N - 1, P - 1, 0);
    s += '<text class="ph-axtitle" x="' + ((a0[0] + a1[0]) / 2).toFixed(1) + '" y="' + ((a0[1] + a1[1]) / 2 + 26).toFixed(1) + '" text-anchor="middle">commits ' + suite.shas[idxs[0]] + " → " + suite.shas[idxs[N - 1]] + "</text>";
    s += '<text class="ph-axtitle" x="' + ((a1[0] + b1[0]) / 2 + 10).toFixed(1) + '" y="' + ((a1[1] + b1[1]) / 2 + 14).toFixed(1) + '" text-anchor="start">' + (c.px ? c.px.label + " " + xf(c.series[0].pv) + " → " + xf(c.series[P - 1].pv) : "series") + "</text>";
    s += '<text class="ph-axtitle" x="' + (ox - 44) + '" y="' + (oy - zh - 8) + '">↑ ' + c.ylabel + " (" + yf(nm.lo) + " → " + yf(nm.hi) + (c.log ? ", log" : "") + ")</text>";
    s += "</svg>";
    return { svg: s, hover: null };
  }


  // ------------------------------------------------------------ param x-axis --
  // A chart whose x-axis is a measured PARAMETER rather than a commit index:
  // one line per series (engine, mode), x = the swept value. Structurally the
  // same object as a trend chart — same axes code, same formatters, same colors,
  // same legend — which is the whole reason the comparison charts stopped having
  // their own renderer. `pts` are [x, value] instead of [commit_idx, value].
  function renderParam(c) {
    var W = 900, H = 330, m = { l: 66, r: 18, t: 26, b: 48 };
    var pw = W - m.l - m.r, ph = H - m.t - m.b;
    var xs = [], all = [];
    c.series.forEach(function (s) {
      s.pts.forEach(function (p) { if (xs.indexOf(p[0]) < 0) xs.push(p[0]); all.push(p[1]); });
    });
    xs.sort(function (a, b) { return a - b; });
    if (!xs.length || !all.length) return { svg: "", hover: null };
    var xmin = xs[0], xmax = xs[xs.length - 1];
    var xlog = !!(c.px && c.px.log) && xmin > 0 && xmax > xmin;
    function X(v) {
      if (xmax <= xmin) return m.l + pw / 2;
      return xlog
        ? m.l + (Math.log2(v) - Math.log2(xmin)) / (Math.log2(xmax) - Math.log2(xmin)) * pw
        : m.l + (v - xmin) / (xmax - xmin) * pw;
    }
    var ymin = Math.min.apply(null, all), ymax = Math.max.apply(null, all), yt;
    if (c.log && ymin > 0) { yt = logTicks(ymin, ymax); } else { yt = linTicks(ymin, ymax); }
    ymin = yt[0]; ymax = yt[yt.length - 1];
    var ylog = !!c.log && ymin > 0;
    function Y(v) {
      if (ylog) return m.t + (1 - (Math.log10(Math.max(v, ymin)) - Math.log10(ymin)) / (Math.log10(ymax) - Math.log10(ymin))) * ph;
      return m.t + (1 - (v - ymin) / (ymax - ymin)) * ph;
    }
    var yf = FMT[c.fmt] || fmtNum, xf = (c.px && FMT[c.px.fmt]) || fmtNum;
    var s = '<svg viewBox="0 0 ' + W + " " + H + '" role="img" aria-label="' + c.title + '">';
    s += '<line class="ph-cross" x1="0" y1="' + m.t + '" x2="0" y2="' + (m.t + ph) + '" style="display:none"/>';
    if (ylog) {
      var e0 = Math.round(Math.log10(ymin)), e1 = Math.round(Math.log10(ymax));
      for (var e = e0; e < e1; e++) for (var k = 2; k <= 9; k++) {
        var v = k * Math.pow(10, e); if (v <= ymin || v >= ymax) continue;
        s += '<line class="ph-glm" x1="' + m.l + '" y1="' + Y(v).toFixed(1) + '" x2="' + (W - m.r) + '" y2="' + Y(v).toFixed(1) + '"/>';
      }
    }
    yt.forEach(function (v) {
      var y = Y(v);
      s += '<line class="ph-gl" x1="' + m.l + '" y1="' + y.toFixed(1) + '" x2="' + (W - m.r) + '" y2="' + y.toFixed(1) + '"/>';
      s += '<text class="ph-tick" x="' + (m.l - 8) + '" y="' + (y + 3).toFixed(1) + '" text-anchor="end">' + yf(v) + "</text>";
    });
    // Every point gets a grid line; a label only where it clears the last one drawn, so a
    // dense sweep (#1890: 32 payload sizes) stays readable instead of overprinting.
    var lastX = -Infinity;
    xs.forEach(function (v) {
      var x = X(v);
      s += '<line class="ph-gl" x1="' + x.toFixed(1) + '" y1="' + m.t + '" x2="' + x.toFixed(1) + '" y2="' + (m.t + ph) + '"/>';
      if (x - lastX < 44) return;
      lastX = x;
      s += '<text class="ph-tick" x="' + x.toFixed(1) + '" y="' + (m.t + ph + 16) + '" text-anchor="middle">' + xf(v) + "</text>";
    });
    s += '<rect class="ph-frame" x="' + m.l + '" y="' + m.t + '" width="' + pw + '" height="' + ph + '"/>';
    s += '<text class="ph-axtitle" x="' + (m.l + pw / 2) + '" y="' + (H - 6) + '" text-anchor="middle">' + ((c.px && c.px.label) || "") + "</text>";
    s += '<text class="ph-axtitle" transform="translate(15 ' + (m.t + ph / 2) + ') rotate(-90)" text-anchor="middle">' + c.ylabel + "</text>";
    c.series.forEach(function (se) {
      if (!se.pts.length) return;
      var line = se.pts.map(function (p) { return X(p[0]).toFixed(1) + "," + Y(p[1]).toFixed(1); }).join(" ");
      // `dash` marks the second pick of a history-picker compare (#1771): same engine
      // color, dashed line, hollow points, so the two picks read apart without a legend.
      s += '<polyline fill="none" stroke="' + col(se.ci) + '" stroke-width="2.2" stroke-linejoin="round" stroke-linecap="round"'
        + (se.dash ? ' stroke-dasharray="7 5"' : "") + ' points="' + line + '"/>';
      se.pts.forEach(function (p, i) {
        s += '<circle cx="' + X(p[0]).toFixed(1) + '" cy="' + Y(p[1]).toFixed(1) + '" r="' + (i === se.pts.length - 1 ? 4 : 2.6) + '" '
          + (se.dash ? 'fill="none" stroke="' + col(se.ci) + '" stroke-width="1.6"' : 'fill="' + col(se.ci) + '"') + "/>";
      });
      var lp = se.pts[se.pts.length - 1];
      if (se.dash) return;  // a compare pick's end value is read on hover, not overprinted
      s += '<text x="' + (X(lp[0]) - 6).toFixed(1) + '" y="' + (Y(lp[1]) - 8).toFixed(1) + '" text-anchor="end" class="ph-tick" style="fill:' + col(se.ci) + ';font-weight:700">' + yf(lp[1]) + "</text>";
    });
    s += "</svg>";
    // Hover is not decoration here: it is the ONLY place the interior points of a
    // comparison sweep can be read. Only the last point of each series carries a
    // printed label, and the generated one-line reading under the chart gives the
    // sweep's endpoints — so without this, a five-point series published three
    // numbers and hid two. The in-page absolute-number table used to cover that
    // gap; hover is what earns the right to have removed it.
    return { svg: s, hover: { kind: "param", W: W, m: m, pw: pw, xs: xs, X: X, xf: xf, yf: yf } };
  }

  // -------------------------------------------------------------- assembly --
  // The page emits one .ph-hist block per chapter, each carrying its own chart
  // payload and its own host div, so a chart sits beside the prose that explains
  // it. Every block is drawn by this same code path — the sectioning is where
  // the charts LAND, never how they are rendered.
  function draw(D, host) {
    D.charts.forEach(function (c) {
    // A card carries every METRIC the family actually recorded (p50 / p99 /
    // ns-per-delivery / throughput) and switches between them in place. Before this,
    // each family hardcoded a single metric and 160 of 323 recorded series were drawn
    // on no chart at all. Metrics live in one card rather than four because a
    // full-width single-column page cannot absorb 4x the cards.
    //
    // Switching metric can change the SUITE (throughput lives in the bigger-is-better
    // store), and a suite carries its own commit axis — so `suite`, `fmt`, `ylabel`
    // and `series` are all re-read per metric rather than fixed per card.
    var mets = (c.metrics && c.metrics.length) ? c.metrics
      : [{ name: c.ylabel, suite: c.suite, fmt: c.fmt, ylabel: c.ylabel, series: c.series }];
    var mi = 0;
    function useMetric(i) {
      mi = i;
      c.series = mets[i].series; c.suite = mets[i].suite;
      c.fmt = mets[i].fmt; c.ylabel = mets[i].ylabel;
      // A metric switch can change the SUITE, and the two suites are recorded
      // independently — index 40 of one is a different commit from index 40 of the other,
      // so a carried-over window would silently point somewhere else.
      r0 = 0; r1 = -1;
    }
    useMetric(0);
    // A param-x chart carries its own axis in its points and has no commit
    // dimension, so it needs neither a suite nor the multi-view tabs.
    var param = c.xkind === "param";
    if (!param && !D.suites[c.suite]) return;
    if (!c.series.length) return;
    var card = document.createElement("div");
    card.className = "ph-card";
    var legend = c.series.map(function (se) {
      return '<span class="item"><span class="sw" style="background:' + col(se.ci) + '"></span>' + se.label + "</span>";
    }).join("");
    // The metric row is emitted even for a single metric, so every card states in
    // words which number is on the y-axis instead of leaving it to the axis label.
    var mrow = '<div class="ph-mets">' + mets.map(function (m, i) {
      return '<button class="ph-met' + (i === 0 ? " on" : "") + '" data-m="' + i + '">' + m.name + "</button>";
    }).join("") + "</div>";
    // Every card names the code that produced it. A chart whose harness you cannot
    // find is a number you cannot check.
    // c.src is a LIST: a chart may draw series from more than one harness (the
    // libtracer-vs-Zenoh card does), and naming one would credit it with the other's work.
    var srcs = c.src ? (Array.isArray(c.src) ? c.src : [c.src]) : [];
    var src = srcs.map(function (f) {
      return '<a class="ph-src" href="https://github.com/avatarsd-llc/libtracer/blob/main/'
        + f + '">' + f.replace(/^bench\//, "") + "</a>";
    }).join(" + ");
    card.innerHTML = "<h4>" + c.title + "</h4>" + '<div class="ph-tabs"></div>'
      + '<p class="cond">' + c.cond + (src ? ' \u00b7 measured by ' + src : "") + "</p>" + mrow
      + '<p class="ph-metblurb"></p>'
      + '<div class="ph-range"></div><div class="ph-pick"></div><div class="ph-bandsel"></div>'
      + '<div class="ph-legend">' + legend + "</div>"
      + '<div class="ph-plot"></div><div class="ph-tip" style="display:none"></div>'
      + (c.reading ? '<p class="ph-reading">' + c.reading + "</p>" : "");
    host.appendChild(card);
    var plot = card.querySelector(".ph-plot"), tip = card.querySelector(".ph-tip");
    var tabhost = card.querySelector(".ph-tabs"), blurb = card.querySelector(".ph-metblurb");
    var rangehost = card.querySelector(".ph-range");
    var suite, idxs, multi, byIdx = [], view = "trend";
    // Ratio and range are card state, not render state: switching view must not lose
    // either. The range is held as absolute indices into the ACTIVE metric's suite and
    // reset when the metric changes, because the two suites are recorded independently
    // and index 40 of one is not index 40 of the other.
    var ratioOn = false, r0 = 0, r1 = -1;
    // The noise band's trailing window, in recorded points (0 = off). Card state like the
    // range, and kept across a metric switch: it counts points, not commits of one suite.
    // Only a payload that says `bands` (bench-local, where every point shares one host)
    // draws one; on the hosted store a band would mostly measure the runner mix.
    var bands = !!D.bands && !param, bandN = BAND_N, bandAt = [];

    // The chart the views are actually drawn from: the active metric's series, optionally
    // turned into the per-commit quotient, then restricted to the selected commit range.
    // Every downstream consumer — renderers, dense-index derivation, hover rows — reads
    // THIS, so a view, a tooltip and a legend can never disagree about which numbers are
    // on screen.
    var A = { c: c, suite: null };

    // History picker (#1771). A comparison sweep that the bench-local store banks carries
    // its banked lines in `c.hist` and the commit list in `D.hist.picks`, so the card can
    // redraw the same axes from any recorded pass. `pa` is the pick on screen (-1 = this
    // build's own pass, the only data the card had before), `pb` the compare pick drawn
    // dashed over it. It opens on the newest commit any of the card's lines recorded.
    var H = (param && c.hist && D.hist) ? D.hist.picks : null, live = c.series;
    var have = H ? H.map(function (_, k) {
      return c.hist.some(function (h) { return !!h.v[k]; });
    }) : [];
    var pa = have.lastIndexOf(true), pb = -1, cmp = false;
    if (H) {
      for (var k0 = pa - 1; k0 >= 0 && pb < 0; k0--) if (have[k0] && H[k0].rel) pb = k0;
      if (pb < 0) pb = have.indexOf(true);
    }
    /** @brief A pick's name in the picker, legend and reading: its release tag, then sha. */
    function pickName(k) { return k < 0 ? "this build" : (H[k].rel ? H[k].rel + " " : "") + H[k].sha; }
    /** @brief One pick's lines: this build's pass (k < 0) or a banked commit. In compare
     * mode every label names its pick, since two lines per engine share one color. */
    function pickLines(k, dash) {
      var src = k < 0 ? live : c.hist.map(function (h) {
        var pts = [];
        if (h.v[k]) h.xs.forEach(function (x, j) { if (h.v[k][j] !== null) pts.push([x, h.v[k][j]]); });
        return { key: h.key, label: h.label, ci: h.ci, pts: pts };
      });
      return src.filter(function (se) { return se.pts.length; }).map(function (se) {
        return { key: se.key, ci: se.ci, pts: se.pts, dash: dash,
          label: cmp ? se.label + " @ " + pickName(k) : se.label };
      });
    }
    /** @brief The sweep's endpoints per engine for the picks on screen, in the words of the
     * build-time reading, which describes this build's pass only. */
    function pickReading(ch) {
      var yf = FMT[ch.fmt] || fmtNum, xf = (ch.px && FMT[ch.px.fmt]) || fmtNum;
      return (cmp ? [pa, pb] : [pa]).map(function (k) {
        var parts = pickLines(k, false).filter(function (se) {
          return se.key === "libtracer" || se.key === "zenoh";
        }).map(function (se) {
          var a = se.pts[0], b = se.pts[se.pts.length - 1];
          return "<b>" + (se.key === "zenoh" ? "Zenoh " : "libtracer ") + yf(a[1]) + " → " + yf(b[1])
            + "</b> (" + xf(a[0]) + " → " + xf(b[0]) + ")";
        });
        return "At <code>" + pickName(k) + "</code>: " + (parts.join("; ") || "no value recorded") + ".";
      }).join(" ");
    }
    /** @brief The two pick selects and the compare toggle, built once per card. Release
     * tags lead in their own group and are marked 🏷 in the full commit list. */
    function drawPick() {
      var host = card.querySelector(".ph-pick");
      if (!H) return;
      function opts(sel) {
        var ks = [];
        for (var k = H.length - 1; k >= 0; k--) if (have[k]) ks.push(k);
        function o(k, mark) {
          return '<option value="' + k + '"' + (mark && k === sel ? " selected" : "") + ">"
            + (k >= 0 && H[k].rel ? "🏷 " : "") + pickName(k) + (k >= 0 && H[k].msg ? " · " + H[k].msg : "") + "</option>";
        }
        var rel = ks.filter(function (k) { return H[k].rel; });
        return '<option value="-1"' + (sel < 0 ? " selected" : "") + ">this build (the pass above)</option>"
          + (rel.length ? '<optgroup label="releases">' + rel.map(function (k) { return o(k, false); }).join("") + "</optgroup>" : "")
          + '<optgroup label="bench-local commits, newest first">' + ks.map(function (k) { return o(k, true); }).join("") + "</optgroup>";
      }
      host.innerHTML = '<span class="ph-rlab">point</span><select class="ph-pa" aria-label="banked commit">' + opts(pa) + "</select>"
        + '<label class="ph-cmp"><input type="checkbox" class="ph-cmpon"> compare</label>'
        + '<select class="ph-pb" aria-label="commit to compare against" disabled>' + opts(pb) + "</select>";
      var sa = host.querySelector(".ph-pa"), sb = host.querySelector(".ph-pb"), on = host.querySelector(".ph-cmpon");
      sa.addEventListener("change", function () { pa = +sa.value; rebind(); show(view); });
      sb.addEventListener("change", function () { pb = +sb.value; rebind(); show(view); });
      on.addEventListener("change", function () { cmp = on.checked; sb.disabled = !cmp; rebind(); show(view); });
    }
    drawPick();

    /** @brief The noise band's window picker, built once per card that can draw a band. */
    function drawBandSel() {
      if (!bands) return;
      var host = card.querySelector(".ph-bandsel");
      host.innerHTML = '<span class="ph-rlab">noise band</span><select class="ph-bandn" aria-label="noise band window">'
        + BAND_NS.map(function (n) {
          return '<option value="' + n + '"' + (n === bandN ? " selected" : "") + ">"
            + (n ? "last " + n + " points" : "off") + "</option>";
        }).join("") + "</select>"
        + '<span class="ph-bandnote">trend view · p10–p90 shaded, min/max faint · cv = window '
        + "std/mean · none for a series with fewer points than the window</span>";
      var sel = host.querySelector(".ph-bandn");
      sel.addEventListener("change", function () { bandN = +sel.value; rebind(); show(view); });
    }
    drawBandSel();

    function derive() {
      var base = (ratioOn && c.ratio) ? (ratioChart(c) || c) : c;
      if (H) base = Object.assign({}, c, { series: pickLines(pa, false).concat(cmp ? pickLines(pb, true) : []) });
      // The band is computed over the WHOLE series before the range slice, so the first
      // points of a narrowed range still carry a full window behind them.
      if (bands && bandN) {
        base = Object.assign({}, base, { series: base.series.map(function (se) {
          return Object.assign({}, se, { band: windowBand(se.pts, bandN) });
        }) });
      }
      var full = param ? null : D.suites[base.suite];
      if (!full) return { c: base, suite: null, full: null };
      if (r1 < 0 || r1 > full.shas.length - 1) r1 = full.shas.length - 1;
      if (r0 > r1) r0 = r1;
      if (r0 < 0) r0 = 0;
      var sl = sliceRange(base, full, r0, r1);
      sl.full = full;
      return sl;
    }

    /** @brief The commit-range control: two sliders over the active suite's commits.
     *
     * Two `<input type="range">` and a text readout — no library, nothing fetched, which
     * the inlined-assets doctrine on this page requires. It is a control rather than a
     * fixed window because the two stores have wildly different lengths (the hosted store
     * is hundreds of commits deep, the bench-local one a dozen), and because the question
     * "what changed since the last release" and the question "what is the long shape"
     * want different windows over the same series.
     */
    function drawRange(full) {
      if (!full || full.shas.length < 3) { rangehost.innerHTML = ""; return; }
      var N = full.shas.length;
      // Dragging a slider re-renders the card, and re-rendering must NOT rebuild the
      // slider the pointer is currently on — replacing the element mid-drag drops the
      // drag. So an existing control over the same axis is UPDATED in place; only a
      // changed axis length (a metric switch, a store switch) rebuilds it.
      var i0e = rangehost.querySelector(".ph-r0");
      if (i0e && +i0e.max === N - 1) {
        i0e.value = r0;
        rangehost.querySelector(".ph-r1").value = r1;
        rangehost.querySelector(".ph-rout").innerHTML =
          "<code>" + full.shas[r0] + "</code> → <code>" + full.shas[r1] + "</code> · "
          + (r1 - r0 + 1) + " of " + N;
        rangehost.querySelector(".ph-rfull").disabled = (r0 === 0 && r1 === N - 1);
        rangehost.querySelector(".ph-rlast").disabled = (r1 === N - 1 && r0 === Math.max(0, N - LAST));
        return;
      }
      rangehost.innerHTML = '<span class="ph-rlab">commits</span>'
        + '<input type="range" class="ph-r0" min="0" max="' + (N - 1) + '" value="' + r0 + '" aria-label="first commit">'
        + '<input type="range" class="ph-r1" min="0" max="' + (N - 1) + '" value="' + r1 + '" aria-label="last commit">'
        + '<span class="ph-rout"><code>' + full.shas[r0] + '</code> → <code>' + full.shas[r1] + '</code> · '
        + (r1 - r0 + 1) + ' of ' + N + "</span>"
        + '<button class="ph-rfull"' + (r0 === 0 && r1 === N - 1 ? " disabled" : "") + ">full</button>"
        + '<button class="ph-rfull ph-rlast"' + (N <= LAST ? " disabled" : "") + ">last " + LAST + "</button>";
      var i0 = rangehost.querySelector(".ph-r0"), i1 = rangehost.querySelector(".ph-r1");
      function moved() {
        // The two ends may cross on the way past each other; clamping rather than
        // swapping keeps the handle the reader is dragging under the cursor.
        r0 = Math.min(+i0.value, +i1.value); r1 = Math.max(+i0.value, +i1.value);
        rebind(); show(view);
      }
      i0.addEventListener("input", moved);
      i1.addEventListener("input", moved);
      rangehost.querySelector(".ph-rfull").addEventListener("click", function () {
        r0 = 0; r1 = N - 1; rebind(); show(view);
      });
      // The recent window at full resolution: one slot per merge across the plot width.
      rangehost.querySelector(".ph-rlast").addEventListener("click", function () {
        r0 = Math.max(0, N - LAST); r1 = N - 1; rebind(); show(view);
      });
    }

    // Re-derive everything the active metric decides: its suite's commit axis, whether
    // there are enough dense points for the sweep/heatmap/3D views, and the view tabs.
    // A metric with sparser history can lose those views, so the tab row is rebuilt
    // rather than fixed at card creation.
    function rebind() {
      A = derive();
      suite = A.suite;
      var N = suite ? suite.shas.length : 0;
      idxs = suite ? denseIdxs(A.c, N) : [];
      multi = !param && !!A.c.px && idxs.length >= 2 && A.c.series.length >= 2;
      var views = multi ? ["trend", "sweep", "heatmap", "3D"] : ["trend"];
      if (views.indexOf(view) < 0) view = "trend";
      var html = views.length > 1 ? views.map(function (v) {
        return '<button class="ph-tab' + (v === view ? " on" : "") + '" data-v="' + v + '">' + v + "</button>";
      }).join("") : "";
      // The ratio toggle is a THIRD question, next to which view and which metric: whether
      // the card shows the two engines' absolute lines or the quotient between them.
      if (c.ratio) html += '<button class="ph-rtoggle' + (ratioOn ? " on" : "") + '">' + c.ratio.label + "</button>";
      tabhost.innerHTML = html;
      tabhost.querySelectorAll(".ph-tab").forEach(function (b) {
        b.addEventListener("click", function () {
          tabhost.querySelectorAll(".ph-tab").forEach(function (x) { x.classList.remove("on"); });
          b.classList.add("on"); view = b.dataset.v; show(view);
        });
      });
      var rt = tabhost.querySelector(".ph-rtoggle");
      if (rt) rt.addEventListener("click", function () { ratioOn = !ratioOn; rebind(); show(view); });
      // The legend follows the active chart: in ratio mode a pair of engine lines has
      // collapsed into one quotient line, and a legend still naming both arms would
      // describe a chart that is not there.
      // With a band on, each legend item carries its series' CV over the window ending at
      // the range's last point: "is this row noisy" as a number.
      card.querySelector(".ph-legend").innerHTML = A.c.series.map(function (se) {
        var last = se.band && se.band.length ? se.band[se.band.length - 1] : null;
        return '<span class="item"><span class="sw' + (se.dash ? " dash" : "") + '" style="'
          + (se.dash ? "border-color:" : "background:") + col(se.ci) + '"></span>' + se.label
          + (last ? ' <span class="ph-cv" title="coefficient of variation over the last ' + bandN
            + ' points">cv ' + fmtPct(last[5]) + "</span>" : "") + "</span>";
      }).join("");
      var rd = card.querySelector(".ph-reading");
      if (H && rd) rd.innerHTML = (pa < 0 && !cmp) ? c.reading : pickReading(A.c);
      drawRange(A.full);
      byIdx = A.c.series.map(lookup);
      bandAt = A.c.series.map(function (se) {
        var m2 = {}; (se.band || []).forEach(function (t) { m2[t[0]] = t; }); return m2;
      });
      blurb.textContent = ratioOn && c.ratio
        ? "Both arms of each point come from one pass on one pinned machine (bench-local), so "
          + "its speed on the day divides out of this quotient to first order. The quotient "
          + "damps the spread the absolute lines carry, but does not escape it: measured, its "
          + "spread is about a third below the libtracer line's own (full 12-run store). The "
          + "hosted store is not same-pass (best of three runners per series), so it offers no "
          + "ratio. A commit where only one arm recorded a value contributes no point."
        : (mets[mi].blurb || "");
    }

    function show(view) {
      var c = A.c, suite = A.suite;
      if (!suite && !param) { plot.innerHTML = ""; return; }
      var r = param ? renderParam(c)
        : view === "sweep" ? renderSweep(c, suite, idxs)
        : view === "heatmap" ? renderHeat(c, suite, idxs)
          : view === "3D" ? render3D(c, suite, idxs)
            : renderTrend(c, suite);
      plot.innerHTML = r.svg;
      tip.style.display = "none";
      if (!r.hover) return;
      var g = r.hover, svg = plot.querySelector("svg"), cross = plot.querySelector(".ph-cross");
      if (!cross) return;
      // Both chart kinds hover the same way — snap to the nearest x, draw the
      // crosshair, list every series' value there. They differ only in what an x
      // IS: a recorded commit, or a swept parameter value.
      function nearest(ev) {
        var rc = svg.getBoundingClientRect();
        var fx = (ev.clientX - rc.left) / rc.width * g.W;
        if (g.kind === "param") {
          var best = 0, bd = Infinity;
          for (var k = 0; k < g.xs.length; k++) {
            var d = Math.abs(g.X(g.xs[k]) - fx);
            if (d < bd) { bd = d; best = k; }
          }
          return { i: best, x: g.X(g.xs[best]) };
        }
        var i = Math.round((fx - g.m.l) / g.pw * (g.N - 1));
        if (i < 0) i = 0; if (i > g.N - 1) i = g.N - 1;
        return { i: i, x: g.m.l + (g.N <= 1 ? g.pw / 2 : (i / (g.N - 1)) * g.pw) };
      }
      svg.addEventListener("mousemove", function (ev) {
        var hit = nearest(ev), i = hit.i;
        cross.setAttribute("x1", hit.x); cross.setAttribute("x2", hit.x); cross.style.display = "";
        var head, rows;
        if (g.kind === "param") {
          var xv = g.xs[i];
          head = "<div class='sha'><code>" + ((c.px && c.px.label) || "x") + " " + g.xf(xv) + "</code></div>";
          rows = c.series.map(function (se) {
            var hitpt = null;
            se.pts.forEach(function (pt) { if (pt[0] === xv) hitpt = pt; });
            return hitpt === null ? "" : '<div><span class="dot" style="background:' + col(se.ci) + '"></span>'
              + se.label + " <b>" + g.yf(hitpt[1]) + "</b></div>";
          }).join("");
        } else {
          var rel = (suite.releases || []).filter(function (rr) { return rr.i === i; })
            .map(function (rr) { return ' <span class="rel">🏷 ' + (rr.approx ? "≈ " : "") + rr.label + "</span>"; }).join("");
          // The HOST the point was measured on, when the store records one. Only the
          // bench-local store does, and it is the whole reason that store is readable
          // as an absolute trend: "same silicon every point" is a claim the reader must
          // be able to check per point, not take on the chapter's word.
          var host = suite.host || (suite.hosts && suite.hosts[i]) || "";
          var day = suite.dates && suite.dates[i] ? " <span class='date'>" + suite.dates[i] + "</span>" : "";
          head = "<div class='sha'><code>" + suite.shas[i] + "</code>" + day + rel + "</div>"
            + (suite.msgs && suite.msgs[i] ? "<div class='msg'>" + suite.msgs[i] + "</div>" : "")
            + (host ? "<div class='host'>" + host + "</div>" : "");
          rows = c.series.map(function (se, si) {
            var v = byIdx[si][i], bt = bandAt[si] && bandAt[si][i];
            // Inside the series' span, a missing value is said, not skipped (#1890).
            if (v === undefined && se.pts.length && i > se.pts[0][0] && i < se.pts[se.pts.length - 1][0])
              return '<div class="ph-nogap"><span class="dot" style="border:1px solid ' + col(se.ci) + '"></span>'
                + se.label + " <i>no trusted value</i></div>";
            return v === undefined ? "" : '<div><span class="dot" style="background:' + col(se.ci) + '"></span>'
              + se.label + " <b>" + g.yf(v) + "</b>"
              + (bt ? ' <span class="ph-cv">cv ' + fmtPct(bt[5]) + " · p10–p90 " + g.yf(bt[1]) + "–" + g.yf(bt[2])
                + " over " + bandN + "</span>" : "") + "</div>";
          }).join("");
        }
        tip.innerHTML = head + rows;
        tip.style.display = "";
      });
      svg.addEventListener("mouseleave", function () { cross.style.display = "none"; tip.style.display = "none"; });
    }
    // View tabs are wired inside rebind() (they are rebuilt per metric). Metric
    // buttons are wired once — the row itself never changes.
    card.querySelectorAll(".ph-met").forEach(function (b) {
      b.addEventListener("click", function () {
        card.querySelectorAll(".ph-met").forEach(function (x) { x.classList.remove("on"); });
        b.classList.add("on");
        useMetric(+b.dataset.m);
        rebind();
        show(view);
      });
    });
    rebind();
    show(view);
    });
  }

  // ---------------------------------------------------------------- source --
  // The page charts two independent stores of the same benchmarks: the bench-local
  // series (one pinned self-hosted CPU, the trend instrument and the default) and the
  // GitHub-hosted series (a portability envelope — consecutive points swing up to ~50%,
  // bimodally, best of three runners per series). They
  // answer different questions and are never mixed on one axis, so the page offers a
  // SELECTOR, not an overlay. Both payloads are embedded by render_history.html_blocks,
  // so switching is a re-render, not a fetch — the page stays self-contained.
  // The key was renamed when the default flipped to bench-local (#1769): the old page
  // wrote its default back on every visit, so a stored "hosted" under the old key records
  // that a reader opened the page, not that they chose the hosted store.
  var SRC_KEY = "ph-source-v2";

  /** @brief The stored source preference, defaulting to the bench-local store.
   *
   * `local` is the default because it is the trend instrument: the hosted store's
   * consecutive points swing by up to about 50%, and a 2x step in one row (the
   * 2026-10-01 1 KiB LKV heap regression, 27 -> 54 ns on bench-local) disappears in that
   * noise. Only an explicit click is remembered. localStorage may be unavailable
   * (file:// in some browsers, privacy modes) — that degrades to the default, never to
   * an exception.
   */
  function readSource() {
    try { return localStorage.getItem(SRC_KEY) === "hosted" ? "hosted" : "local"; }
    catch (e) { return "local"; }
  }
  /** @brief Persist the source preference; silently a no-op where storage is denied.
   *
   * The mirror of `readSource`: localStorage may be unavailable (file:// in some
   * browsers, privacy modes), and a preference is optional by nature — failing to
   * remember it must never break the switch itself.
   */
  function writeSource(src) {
    try { localStorage.setItem(SRC_KEY, src); } catch (e) { /* preference is optional */ }
  }

  /** @brief Draw one chart block from the selected store, or say why it is empty.
   *
   * An absent family is reported in words rather than as a bare empty grid: the
   * bench-local store started far later than the hosted one and does not carry every
   * family (the memory probes, for one), and a chapter that simply went blank on switch
   * would read as "the numbers vanished" instead of "this store has no such series".
   */
  function renderRoot(root, src) {
    var host = root.querySelector(".ph-charts");
    if (!host) return;
    var D = src === "local" ? root._phLocal : root._phHosted;
    host.innerHTML = "";
    var note = root.querySelector(".ph-count");
    if (!D || !D.charts || !D.charts.length) {
      host.innerHTML = '<p class="ph-empty">No '
        + (src === "local" ? "bench-local" : "GitHub-hosted")
        + " series for this chapter yet — the store carries no chartable family here."
        + " Switch source above to see the other store.</p>";
      if (note) note.textContent = "0 family charts · 0 series";
      return;
    }
    if (note) {
      var nser = 0;
      D.charts.forEach(function (c) {
        (c.metrics || [{ series: c.series }]).forEach(function (v) { nser += v.series.length; });
      });
      note.textContent = D.charts.length + " family charts · " + nser + " series";
    }
    draw(D, host);
  }

  /** @brief Switch every chart block on the page to one store, remembering it if asked.
   *
   * The selector is GLOBAL even though the markup repeats it per chapter: a page whose
   * chapters could sit on different stores would put two incomparable instruments under
   * one set of conclusions. Every block re-renders and every button row restates the
   * same answer. `remember` is set only for a reader's click: persisting the boot-time
   * default would pin every visitor to whatever the default was on their first visit.
   */
  function setSource(src, remember) {
    if (remember) writeSource(src);
    document.querySelectorAll(".ph-hist.ph-sourced").forEach(function (root) {
      root.querySelectorAll(".ph-srcbtn").forEach(function (b) {
        b.classList.toggle("on", b.dataset.src === src);
        b.setAttribute("aria-pressed", b.dataset.src === src ? "true" : "false");
      });
      renderRoot(root, src);
    });
  }

  function boot() {
    document.querySelectorAll(".ph-hist").forEach(function (root) {
      var el = root.querySelector("script.ph-data"), host = root.querySelector(".ph-charts");
      if (!el || !host) return;
      var D;
      try { D = JSON.parse(el.textContent); } catch (e) { return; }
      // A block with no source selector (the comparison charts emit the same root shape)
      // is drawn exactly as before — one payload, no switching.
      if (!root.classList.contains("ph-sourced")) { draw(D, host); return; }
      var lel = root.querySelector("script.ph-data-local"), L = null;
      try { L = lel ? JSON.parse(lel.textContent) : null; } catch (e) { L = null; }
      root._phHosted = D;
      root._phLocal = L;
      root.querySelectorAll(".ph-srcbtn").forEach(function (b) {
        b.addEventListener("click", function () {
          if (b.disabled) return;
          setSource(b.dataset.src, true);
        });
      });
    });
    var roots = document.querySelectorAll(".ph-hist.ph-sourced");
    if (!roots.length) return;
    var src = readSource();
    // The bench-local default is honoured only if the build actually embedded that store
    // (a fork or an offline build has none); otherwise the page opens on hosted.
    if (src === "local" && !Array.prototype.some.call(roots, function (r) { return !!r._phLocal; })) {
      src = "hosted";
    }
    setSource(src, false);
  }
  // This script is inlined into the LAST chapter block, so every host div is
  // already parsed by the time it runs; the readyState guard covers the case
  // where a future change moves it earlier.
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot);
  else boot();
})();
