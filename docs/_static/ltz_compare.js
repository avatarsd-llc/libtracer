// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// Draws the libtracer-vs-Zenoh absolute-value comparison charts on the Performance
// page. Reads the measured data (emitted by bench/render_compare.py during the docs
// build) from the <script type="application/json" id="ltz-data"> block, and renders
// one inline SVG per chart plus a raw-number table. Absolute axes only — no ratios.
// Log axes get a real log grid (minor lines at 2..9 x 10^n, and at each power of two
// on the base-2 x axis). Vanilla JS, no dependencies.
(function () {
  var el = document.getElementById("ltz-data");
  if (!el) return;
  var DATA;
  try { DATA = JSON.parse(el.textContent); } catch (e) { return; }

  function fmtRate(v) { return v >= 1e6 ? (v / 1e6).toFixed(v < 1e7 ? 1 : 0) + " M" : v >= 1e3 ? (v / 1e3).toFixed(0) + " k" : "" + Math.round(v); }
  function fmtNs(v) { return v >= 1e6 ? (v / 1e6).toFixed(v < 1e7 ? 1 : 0) + " ms" : v >= 1e3 ? (v / 1e3).toFixed(v < 1e4 ? 1 : 0) + " µs" : Math.round(v) + " ns"; }
  function fmtBytes(v) { return v >= 1024 ? (v / 1024) + " KB" : v + " B"; }
  function fmtMB(v) { return v >= 1000 ? (v / 1000).toFixed(1) + " GB/s" : Math.round(v) + " MB/s"; }
  function fmtCount(v) { return v >= 1000 ? (v / 1000) + "k" : "" + v; }
  var FMT = { rate: fmtRate, ns: fmtNs, bytes: fmtBytes, mb: fmtMB, count: fmtCount };

  function logTicks(min, max) {
    var lo = Math.floor(Math.log10(min)), hi = Math.ceil(Math.log10(max)), t = [];
    for (var e = lo; e <= hi; e++) t.push(Math.pow(10, e));
    return t;
  }
  function linTicks(min, max) {
    var span = max - min || max, step = Math.pow(10, Math.floor(Math.log10(span))), n = span / step;
    var s = n < 2 ? step / 2 : n < 5 ? step : step * 2;
    var lo = Math.floor(min / s) * s, hi = Math.ceil(max / s) * s, t = [];
    for (var v = lo; v <= hi + 1e-9; v += s) t.push(v);
    return t;
  }

  var COL = { libtracer: "var(--ltz-lt)", zenoh: "var(--ltz-zn)" };

  function chart(cfg) {
    var W = 560, H = 350, m = { l: 66, r: 18, t: 14, b: 46 };
    var pw = W - m.l - m.r, ph = H - m.t - m.b;
    var xs = cfg.x.vals, xmin = Math.min.apply(null, xs), xmax = Math.max.apply(null, xs);
    var X = function (v) {
      return cfg.x.log
        ? m.l + (Math.log2(v) - Math.log2(xmin)) / (Math.log2(xmax) - Math.log2(xmin)) * pw
        : m.l + (v - xmin) / (xmax - xmin) * pw;
    };
    var all = [];
    cfg.series.forEach(function (s) { s.pts.forEach(function (p) { all.push(p[1]); }); });
    var ymin = Math.min.apply(null, all), ymax = Math.max.apply(null, all), yt;
    if (cfg.y.log) { yt = logTicks(ymin, ymax); ymin = yt[0]; ymax = yt[yt.length - 1]; }
    else { yt = linTicks(ymin, ymax); ymin = yt[0]; ymax = yt[yt.length - 1]; }
    var Y = function (v) {
      return cfg.y.log
        ? m.t + (1 - (Math.log10(v) - Math.log10(ymin)) / (Math.log10(ymax) - Math.log10(ymin))) * ph
        : m.t + (1 - (v - ymin) / (ymax - ymin)) * ph;
    };
    var yf = FMT[cfg.y.fmt], xf = FMT[cfg.x.fmt];
    var s = '<svg viewBox="0 0 ' + W + " " + H + '" role="img" aria-label="' + cfg.title + '">';
    // minor log grid first
    if (cfg.y.log) {
      var e0 = Math.round(Math.log10(ymin)), e1 = Math.round(Math.log10(ymax));
      for (var e = e0; e < e1; e++) for (var k = 2; k <= 9; k++) {
        var v = k * Math.pow(10, e); if (v <= ymin || v >= ymax) continue; var y = Y(v);
        s += '<line class="ltz-glm" x1="' + m.l + '" y1="' + y.toFixed(1) + '" x2="' + (W - m.r) + '" y2="' + y.toFixed(1) + '"/>';
      }
    }
    if (cfg.x.log) {
      for (var p = 1; p <= xmax; p *= 2) {
        if (p <= xmin || p >= xmax) continue; var x = X(p);
        s += '<line class="ltz-glm" x1="' + x.toFixed(1) + '" y1="' + m.t + '" x2="' + x.toFixed(1) + '" y2="' + (m.t + ph) + '"/>';
      }
    }
    yt.forEach(function (v) {
      var y = Y(v);
      s += '<line class="ltz-gl" x1="' + m.l + '" y1="' + y.toFixed(1) + '" x2="' + (W - m.r) + '" y2="' + y.toFixed(1) + '"/>';
      s += '<text class="ltz-tick" x="' + (m.l - 8) + '" y="' + (y + 3).toFixed(1) + '" text-anchor="end">' + yf(v) + "</text>";
    });
    xs.forEach(function (v) {
      var x = X(v);
      s += '<line class="ltz-gl" x1="' + x.toFixed(1) + '" y1="' + m.t + '" x2="' + x.toFixed(1) + '" y2="' + (m.t + ph) + '"/>';
      s += '<text class="ltz-tick" x="' + x.toFixed(1) + '" y="' + (m.t + ph + 16) + '" text-anchor="middle">' + xf(v) + "</text>";
    });
    s += '<rect class="ltz-frame" x="' + m.l + '" y="' + m.t + '" width="' + pw + '" height="' + ph + '"/>';
    s += '<text class="ltz-axtitle" x="' + (m.l + pw / 2) + '" y="' + (H - 6) + '" text-anchor="middle">' + cfg.x.label + "</text>";
    s += '<text class="ltz-axtitle" transform="translate(15 ' + (m.t + ph / 2) + ') rotate(-90)" text-anchor="middle">' + cfg.y.label + "</text>";
    cfg.series.forEach(function (se) {
      var pts = se.pts.map(function (p) { return X(p[0]).toFixed(1) + "," + Y(p[1]).toFixed(1); }).join(" ");
      s += '<polyline fill="none" stroke="' + se.color + '" stroke-width="2.2" stroke-linejoin="round" stroke-linecap="round" points="' + pts + '"/>';
      se.pts.forEach(function (p, i) {
        var last = i === se.pts.length - 1;
        s += '<circle cx="' + X(p[0]).toFixed(1) + '" cy="' + Y(p[1]).toFixed(1) + '" r="' + (last ? 4 : 2.6) + '" fill="' + se.color + '"/>';
      });
      var lp = se.pts[se.pts.length - 1];
      s += '<text x="' + (X(lp[0]) - 6).toFixed(1) + '" y="' + (Y(lp[1]) - 8).toFixed(1) + '" text-anchor="end" class="ltz-tick" style="fill:' + se.color + ';font-weight:700">' + yf(lp[1]) + "</text>";
    });
    s += "</svg>";
    return s;
  }

  var host = document.getElementById("ltz-charts");
  if (host) DATA.charts.forEach(function (c) {
    var series = ["libtracer", "zenoh"].map(function (sys) {
      return { name: sys, color: COL[sys], pts: (c.series[sys] || []) };
    }).filter(function (s) { return s.pts.length; });
    var xvals = (c.series.libtracer || c.series.zenoh || []).map(function (p) { return p[0]; });
    var card = document.createElement("div");
    card.className = "ltz-card";
    card.innerHTML = "<h4>" + c.title + '</h4><p class="cond">' + c.cond + "</p>"
      + chart({ title: c.title, x: { vals: xvals, log: c.x.log, fmt: c.x.fmt, label: c.x.label }, y: c.y, series: series })
      + '<p class="ltz-reading">' + c.reading + "</p>";
    host.appendChild(card);
  });

  var tb = document.querySelector("#ltz-raw tbody");
  if (tb && DATA.table) DATA.table.forEach(function (r, i) {
    var tr = document.createElement("tr");
    if (r.grp) tr.className = "grp";
    var dot = r.system === "libtracer" ? "var(--ltz-lt)" : "var(--ltz-zn)";
    tr.innerHTML = "<td>" + (r.sweep || "") + "</td>"
      + '<td class="sys"><span class="dot" style="background:' + dot + '"></span>' + r.system + "</td>"
      + "<td>" + r.x + "</td><td>" + r.throughput + "</td><td>" + r.bandwidth + "</td><td>" + r.p50 + "</td>";
    tb.appendChild(tr);
  });
})();
