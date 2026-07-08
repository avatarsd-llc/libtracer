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
  var el = document.getElementById("ph-data");
  if (!el) return;
  var D;
  try { D = JSON.parse(el.textContent); } catch (e) { return; }

  var NPAL = 12;
  function col(ci) { return "var(--ph-c" + (ci % NPAL) + ")"; }

  function fmtNs(v) { return v >= 1e6 ? (v / 1e6).toFixed(v < 1e7 ? 1 : 0) + " ms" : v >= 1e3 ? (v / 1e3).toFixed(v < 1e4 ? 1 : 0) + " µs" : (v >= 100 ? Math.round(v) : +v.toFixed(1)) + " ns"; }
  function fmtRate(v) { return v >= 1e6 ? (v / 1e6).toFixed(v < 1e7 ? 1 : 0) + " M/s" : v >= 1e3 ? (v / 1e3).toFixed(0) + " k/s" : Math.round(v) + "/s"; }
  function fmtNum(v) { return v >= 1e6 ? (v / 1e6).toFixed(1) + " M" : v >= 1e3 ? (v / 1e3).toFixed(1) + " k" : (v === Math.round(v) ? "" + v : +v.toFixed(1)); }
  function fmtBytes(v) { return v >= 1024 ? (v / 1024) + " KB" : v + " B"; }
  function fmtCount(v) { return v >= 1000 ? (v / 1000) + "k" : "" + v; }
  var FMT = { ns: fmtNs, rate: fmtRate, num: fmtNum, bytes: fmtBytes, count: fmtCount };

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

  // ---------------------------------------------------------------- trend --
  function renderTrend(c, suite) {
    var N = suite.shas.length;
    var W = 560, H = 330, m = { l: 64, r: 14, t: 26, b: 46 };
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
    var W = 560, H = 330, m = { l: 64, r: 14, t: 26, b: 44 };
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
    var W = 560, H = 90 + P * 26 + 60, m = { l: 96, r: 60, t: 26, b: 54 };
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
    var W = 560, H = 380;
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

  // -------------------------------------------------------------- assembly --
  var host = document.getElementById("ph-charts");
  if (!host) return;
  D.charts.forEach(function (c) {
    var suite = D.suites[c.suite];
    if (!suite || !c.series.length) return;
    var N = suite.shas.length;
    var idxs = denseIdxs(c, N);
    var multi = !!c.px && idxs.length >= 2 && c.series.length >= 2;
    var card = document.createElement("div");
    card.className = "ph-card";
    var legend = c.series.map(function (se) {
      return '<span class="item"><span class="sw" style="background:' + col(se.ci) + '"></span>' + se.label + "</span>";
    }).join("");
    var views = multi ? ["trend", "sweep", "heatmap", "3D"] : ["trend"];
    var tabs = views.length > 1 ? '<div class="ph-tabs">' + views.map(function (v, i) {
      return '<button class="ph-tab' + (i === 0 ? " on" : "") + '" data-v="' + v + '">' + v + "</button>";
    }).join("") + "</div>" : "";
    card.innerHTML = "<h4>" + c.title + "</h4>" + tabs + '<p class="cond">' + c.cond + "</p>"
      + '<div class="ph-legend">' + legend + "</div>"
      + '<div class="ph-plot"></div><div class="ph-tip" style="display:none"></div>';
    host.appendChild(card);
    var plot = card.querySelector(".ph-plot"), tip = card.querySelector(".ph-tip");
    var byIdx = c.series.map(lookup);

    function show(view) {
      var r = view === "sweep" ? renderSweep(c, suite, idxs)
        : view === "heatmap" ? renderHeat(c, suite, idxs)
          : view === "3D" ? render3D(c, suite, idxs)
            : renderTrend(c, suite);
      plot.innerHTML = r.svg;
      tip.style.display = "none";
      if (!r.hover || r.hover.kind !== "trend") return;
      var g = r.hover, svg = plot.querySelector("svg"), cross = plot.querySelector(".ph-cross");
      svg.addEventListener("mousemove", function (ev) {
        var rc = svg.getBoundingClientRect();
        var fx = (ev.clientX - rc.left) / rc.width * g.W;
        var i = Math.round((fx - g.m.l) / g.pw * (g.N - 1));
        if (i < 0) i = 0; if (i > g.N - 1) i = g.N - 1;
        var x = g.m.l + (g.N <= 1 ? g.pw / 2 : (i / (g.N - 1)) * g.pw);
        cross.setAttribute("x1", x); cross.setAttribute("x2", x); cross.style.display = "";
        var rel = (suite.releases || []).filter(function (rr) { return rr.i === i; })
          .map(function (rr) { return ' <span class="rel">🏷 ' + (rr.approx ? "≈ " : "") + rr.label + "</span>"; }).join("");
        var rows = c.series.map(function (se, si) {
          var v = byIdx[si][i];
          return v === undefined ? "" : '<div><span class="dot" style="background:' + col(se.ci) + '"></span>'
            + se.label + " <b>" + g.yf(v) + "</b></div>";
        }).join("");
        tip.innerHTML = "<div class='sha'><code>" + suite.shas[i] + "</code>" + rel + "</div>"
          + (suite.msgs && suite.msgs[i] ? "<div class='msg'>" + suite.msgs[i] + "</div>" : "") + rows;
        tip.style.display = "";
      });
      svg.addEventListener("mouseleave", function () { cross.style.display = "none"; tip.style.display = "none"; });
    }
    card.querySelectorAll(".ph-tab").forEach(function (b) {
      b.addEventListener("click", function () {
        card.querySelectorAll(".ph-tab").forEach(function (x) { x.classList.remove("on"); });
        b.classList.add("on");
        show(b.dataset.v);
      });
    });
    show("trend");
  });
})();
