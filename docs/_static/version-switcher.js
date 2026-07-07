// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// Version switcher for the Furo sidebar.
//
// Reads a generated `versions.json` at the docs root (one entry per version
// subtree actually deployed — see tools/gen_versions_json.py), renders a picker
// in the sidebar, and remembers the choice in localStorage. Base-path agnostic:
// everything resolves against the Sphinx-injected DOCUMENTATION_OPTIONS.URL_ROOT,
// so it works both at github.io/libtracer/ and on a custom domain served at /.
//
// Switching to another version tries to land on the SAME page in that version
// (HEAD-probed; falls back to the version's landing page if that page did not
// exist yet in the target version).
(function () {
  "use strict";

  var STORAGE_KEY = "libtracer-docs-version";

  function urlRoot() {
    try {
      if (window.DOCUMENTATION_OPTIONS && DOCUMENTATION_OPTIONS.URL_ROOT != null) {
        return DOCUMENTATION_OPTIONS.URL_ROOT || "./";
      }
    } catch (e) {}
    return "./";
  }

  // Absolute URL of the CURRENT version's doc root (where this page's tree lives).
  function currentVersionRoot() {
    return new URL(urlRoot(), window.location.href);
  }

  // The set of deployed versions is manifested at the SITE root, one level above
  // the current version subtree only when we are inside a /vX.Y.Z/ tree. We locate
  // it by walking up from the current version root until we find versions.json.
  // Simplest robust approach: the manifest is emitted at the site root and every
  // version links to it via an absolute path recorded in the manifest itself, so
  // we fetch it relative to the current version root first, then one level up.
  function fetchManifest() {
    var here = currentVersionRoot();
    var candidates = [new URL("versions.json", here).href, new URL("../versions.json", here).href];
    return candidates.reduce(function (p, url) {
      return p.catch(function () {
        return fetch(url, { cache: "no-cache" }).then(function (r) {
          if (!r.ok) throw new Error("no manifest at " + url);
          return r.json().then(function (data) {
            data.__base = new URL(".", url); // remember where the manifest lives = site root
            return data;
          });
        });
      });
    }, Promise.reject());
  }

  function pathAfter(root) {
    var here = window.location.pathname;
    var base = root.pathname;
    return here.indexOf(base) === 0 ? here.slice(base.length) : "";
  }

  // Which manifest entry are we currently viewing? The one whose root path is the
  // longest prefix of the current URL (so /v0.3.0/foo picks v0.3.0, not latest).
  function detectCurrent(versions, siteRoot) {
    var path = window.location.pathname;
    var best = null;
    var bestLen = -1;
    versions.forEach(function (v) {
      var vroot = new URL(v.path || "", siteRoot).pathname;
      if (path.indexOf(vroot) === 0 && vroot.length > bestLen) {
        best = v;
        bestLen = vroot.length;
      }
    });
    return best || versions[0];
  }

  function go(target, siteRoot, current) {
    if (!target || target === current) return;
    try {
      localStorage.setItem(STORAGE_KEY, target.slug);
    } catch (e) {}
    var currentRoot = new URL(current.path || "", siteRoot);
    var rel = pathAfter(currentRoot); // e.g. docs/reference/00-overview.html
    var samePage = new URL((target.path || "") + rel, siteRoot).href + window.location.hash;
    var landing = new URL(target.path || "", siteRoot).href;
    // Prefer the same page in the target version; fall back to its landing page.
    fetch(samePage, { method: "HEAD" })
      .then(function (r) {
        window.location.assign(r.ok ? samePage : landing);
      })
      .catch(function () {
        window.location.assign(landing);
      });
  }

  function render(container, data) {
    var versions = (data && data.versions) || [];
    if (!versions.length) return;
    var siteRoot = data.__base || currentVersionRoot();
    var current = detectCurrent(versions, siteRoot);

    var wrap = document.createElement("div");
    wrap.className = "lt-version-switcher";

    var label = document.createElement("span");
    label.className = "lt-version-label";
    label.textContent = "Version";
    wrap.appendChild(label);

    if (versions.length === 1) {
      // Nothing to switch between yet — show the current version as a chip so the
      // control is visibly present and grows into a picker when releases deploy.
      var chip = document.createElement("span");
      chip.className = "lt-version-chip";
      chip.textContent = current.name;
      wrap.appendChild(chip);
    } else {
      var select = document.createElement("select");
      select.className = "lt-version-select";
      select.setAttribute("aria-label", "Choose documentation version");
      versions.forEach(function (v) {
        var opt = document.createElement("option");
        opt.value = v.slug;
        opt.textContent = v.name;
        if (v.slug === current.slug) opt.selected = true;
        select.appendChild(opt);
      });
      select.addEventListener("change", function () {
        var chosen = versions.filter(function (v) { return v.slug === select.value; })[0];
        go(chosen, siteRoot, current);
      });
      wrap.appendChild(select);
    }

    container.appendChild(wrap);
  }

  function mount() {
    // Furo sidebar: place the switcher right under the brand/title.
    var host =
      document.querySelector(".sidebar-brand") ||
      document.querySelector(".sidebar-sticky") ||
      document.querySelector(".sidebar-scroll");
    if (!host) return;
    var slot = document.createElement("div");
    slot.className = "lt-version-slot";
    if (host.classList.contains("sidebar-brand") && host.parentNode) {
      host.parentNode.insertBefore(slot, host.nextSibling);
    } else {
      host.insertBefore(slot, host.firstChild);
    }
    fetchManifest()
      .then(function (data) { render(slot, data); })
      .catch(function () { /* no manifest (e.g. local preview) — silently skip */ });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", mount);
  } else {
    mount();
  }
})();
