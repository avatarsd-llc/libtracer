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
  var FMT = { ns: fmtNs, rate: fmtRate, num: fmtNum, bytes: fmtBytes, count: fmtCount, mb: fmtMB };

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

  function lookup(se) { var m = {}; se.pts.forEach(function (p) { m[p[0]] = p[1]; }); return m; }

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

  // ---------------------------------------------------------------- trend --
  function renderTrend(c, suite) {
    var N = suite.shas.length;
    var W = 900, H = 330, m = { l: 64, r: 14, t: 26, b: 46 };
    var pw = W - m.l - m.r, ph = H - m.t - m.b;
    function X(i) { return m.l + (N <= 1 ? pw / 2 : (i / (N - 1)) * pw); }
    var all = [];
    c.series.forEach(function (s) { s.pts.forEach(function (p) { if (!c.log || p[1] > 0) all.push(p[1]); }); });
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
    (suite.releases || []).forEach(function (r) { if (r.i < N) s += relMark(X(r.i), m.t, m.t + ph, r); });
    (suite.instruments || []).forEach(function (r) { if (r.i < N) s += instrMark(X(r.i), m.t, m.t + ph, r); });
    c.series.forEach(function (se) {
      var cc = col(se.ci);
      var pts = se.pts.map(function (p) { return X(p[0]).toFixed(1) + "," + Y(p[1]).toFixed(1); }).join(" ");
      s += '<polyline fill="none" stroke="' + cc + '" stroke-width="2" stroke-linejoin="round" stroke-linecap="round" points="' + pts + '"/>';
      se.pts.forEach(function (p, i3) {
        s += '<circle cx="' + X(p[0]).toFixed(1) + '" cy="' + Y(p[1]).toFixed(1) + '" r="' + (i3 === se.pts.length - 1 ? 3.4 : 2.2) + '" fill="' + cc + '"/>';
      });
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
        ? m.l + (Math.log10(v) - Math.log10(xmin)) / (Math.log10(xmax) - Math.log10(xmin)) * pw
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
    xs.forEach(function (v) {
      var x = X(v);
      s += '<line class="ph-gl" x1="' + x.toFixed(1) + '" y1="' + m.t + '" x2="' + x.toFixed(1) + '" y2="' + (m.t + ph) + '"/>';
      s += '<text class="ph-tick" x="' + x.toFixed(1) + '" y="' + (m.t + ph + 16) + '" text-anchor="middle">' + xf(v) + "</text>";
    });
    s += '<rect class="ph-frame" x="' + m.l + '" y="' + m.t + '" width="' + pw + '" height="' + ph + '"/>';
    s += '<text class="ph-axtitle" x="' + (m.l + pw / 2) + '" y="' + (H - 6) + '" text-anchor="middle">' + ((c.px && c.px.label) || "") + "</text>";
    s += '<text class="ph-axtitle" transform="translate(15 ' + (m.t + ph / 2) + ') rotate(-90)" text-anchor="middle">' + c.ylabel + "</text>";
    c.series.forEach(function (se) {
      if (!se.pts.length) return;
      var line = se.pts.map(function (p) { return X(p[0]).toFixed(1) + "," + Y(p[1]).toFixed(1); }).join(" ");
      s += '<polyline fill="none" stroke="' + col(se.ci) + '" stroke-width="2.2" stroke-linejoin="round" stroke-linecap="round" points="' + line + '"/>';
      se.pts.forEach(function (p, i) {
        s += '<circle cx="' + X(p[0]).toFixed(1) + '" cy="' + Y(p[1]).toFixed(1) + '" r="' + (i === se.pts.length - 1 ? 4 : 2.6) + '" fill="' + col(se.ci) + '"/>';
      });
      var lp = se.pts[se.pts.length - 1];
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
    card.innerHTML = "<h4>" + c.title + "</h4>" + '<div class="ph-tabs"></div>'
      + '<p class="cond">' + c.cond + "</p>" + mrow
      + '<p class="ph-metblurb"></p>'
      + '<div class="ph-legend">' + legend + "</div>"
      + '<div class="ph-plot"></div><div class="ph-tip" style="display:none"></div>'
      + (c.reading ? '<p class="ph-reading">' + c.reading + "</p>" : "");
    host.appendChild(card);
    var plot = card.querySelector(".ph-plot"), tip = card.querySelector(".ph-tip");
    var tabhost = card.querySelector(".ph-tabs"), blurb = card.querySelector(".ph-metblurb");
    var byIdx = c.series.map(lookup);
    var suite, idxs, multi, view = "trend";

    // Re-derive everything the active metric decides: its suite's commit axis, whether
    // there are enough dense points for the sweep/heatmap/3D views, and the view tabs.
    // A metric with sparser history can lose those views, so the tab row is rebuilt
    // rather than fixed at card creation.
    function rebind() {
      suite = param ? null : D.suites[c.suite];
      var N = suite ? suite.shas.length : 0;
      idxs = suite ? denseIdxs(c, N) : [];
      multi = !param && !!c.px && idxs.length >= 2 && c.series.length >= 2;
      var views = multi ? ["trend", "sweep", "heatmap", "3D"] : ["trend"];
      if (views.indexOf(view) < 0) view = "trend";
      tabhost.innerHTML = views.length > 1 ? views.map(function (v) {
        return '<button class="ph-tab' + (v === view ? " on" : "") + '" data-v="' + v + '">' + v + "</button>";
      }).join("") : "";
      tabhost.querySelectorAll(".ph-tab").forEach(function (b) {
        b.addEventListener("click", function () {
          tabhost.querySelectorAll(".ph-tab").forEach(function (x) { x.classList.remove("on"); });
          b.classList.add("on"); view = b.dataset.v; show(view);
        });
      });
      byIdx = c.series.map(lookup);
      blurb.textContent = mets[mi].blurb || "";
    }

    function show(view) {
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
          head = "<div class='sha'><code>" + suite.shas[i] + "</code>" + rel + "</div>"
            + (suite.msgs && suite.msgs[i] ? "<div class='msg'>" + suite.msgs[i] + "</div>" : "");
          rows = c.series.map(function (se, si) {
            var v = byIdx[si][i];
            return v === undefined ? "" : '<div><span class="dot" style="background:' + col(se.ci) + '"></span>'
              + se.label + " <b>" + g.yf(v) + "</b></div>";
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

  function boot() {
    document.querySelectorAll(".ph-hist").forEach(function (root) {
      var el = root.querySelector("script.ph-data"), host = root.querySelector(".ph-charts");
      if (!el || !host) return;
      var D;
      try { D = JSON.parse(el.textContent); } catch (e) { return; }
      draw(D, host);
    });
  }
  // This script is inlined into the LAST chapter block, so every host div is
  // already parsed by the time it runs; the readyState guard covers the case
  // where a future change moves it earlier.
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot);
  else boot();
})();
