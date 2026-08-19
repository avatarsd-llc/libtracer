# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Sphinx configuration for the libtracer documentation site.
#
# The source root is the repository root (this conf.py lives in docs/ and is
# pointed at via `sphinx-build -c docs . <out>`), so the root glossary
# (CONTEXT.md), governance docs, and the docs/ tree render as one cohesive site
# with working relative cross-links. `include_patterns` scopes the build to the
# documentation material — the code directories and CLAUDE.md are excluded.

import os
import shutil
import subprocess

project = "libtracer"
project_copyright = "2026, avatarsd LLC"
author = "avatarsd LLC"

extensions = [
    "myst_parser",
    "sphinxcontrib.mermaid",
    "sphinx_design",  # grid / grid-item-card directives for the landing cards
    "breathe",  # render Doxygen XML as in-page C++ source references
    "sphinx.ext.githubpages",  # emit .nojekyll so underscore dirs (_static) serve
    "sphinx_sitemap",  # sitemap.xml (SEO) — needs html_baseurl
    "sphinxext.opengraph",  # Open Graph + Twitter cards + meta descriptions (SEO)
]

# --- SEO ---------------------------------------------------------------------
# sitemap.xml at the site root (single version/lang, so no {version}/{lang}
# prefix). html_baseurl (set below) is the absolute origin every entry uses.
sitemap_url_scheme = "{link}"
# Open Graph / Twitter link previews + auto meta descriptions from each page's
# first paragraph, so shared links render richly and rank better.
ogp_site_url = "https://libtracer.avatarsd.com/"
ogp_site_name = "libtracer"
ogp_type = "website"
ogp_description_length = 200
ogp_enable_meta_description = True
ogp_custom_meta_tags = ['<meta name="twitter:card" content="summary" />']

# Doxygen → Breathe: generate the C++ API XML from the documented core/ headers
# (core/Doxyfile) so `{doxygenclass}` directives can pull the reference impl's
# own declarations into the module docs. The repo root is the Doxygen working
# dir (its INPUT paths are repo-root-relative); the XML lands in docs/_doxygen/xml.
_repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if shutil.which("doxygen"):
    # check=False: a strict (WARN_AS_ERROR) Doxygen failure is the API-doc CI
    # gate (docs.yml), not a reason to abort the whole site build — the XML is
    # still emitted for Breathe either way.
    subprocess.run(["doxygen", "core/Doxyfile"], cwd=_repo_root, check=False)
else:
    print("conf.py: doxygen not found — skipping C++ API source refs (Breathe)")

# The Evidence pages (docs/performance.md, docs/test-report.md) are GENERATED —
# bench/gen_results_page.py and bench/gen_test_report.py write them from the live
# harnesses before sphinx-build runs in CI, and .gitignore keeps them out of the
# tree. A fresh clone therefore has no such files, and the root toctree entries
# that point at them would be dead on every local or non-CI build. Drop a minimal
# placeholder for whichever one is absent: CI never sees this branch (the
# generators run first and their output is what deploys), and a contributor
# previewing the site locally gets a structurally identical tree with an honest
# "generated in CI" note instead of a broken toctree.
for _gen, _title in (
    ("performance.md", "Performance & conformance"),
    ("test-report.md", "Test report"),
):
    _path = os.path.join(_repo_root, "docs", _gen)
    if not os.path.exists(_path):
        with open(_path, "w", encoding="utf-8") as _fh:
            _fh.write(
                f"# {_title}\n\n"
                "```{note}\n"
                "This page is generated from the live harnesses by the documentation\n"
                "workflow. This local build ran without them, so the page is a\n"
                "placeholder — see the published site for the measured figures.\n"
                "```\n"
            )

breathe_projects = {"libtracer": "_doxygen/xml"}
breathe_default_project = "libtracer"
breathe_default_members = ()  # docs opt in per-directive with :members:

# Markdown-only sources; index.md (at the source root) is the landing page, so the
# site root URL lands on it directly.
source_suffix = {".md": "markdown"}
root_doc = "index"

# Publish only the public protocol material (allowlist, relative to the source
# root): the descriptive reference suite, the module guide, the normative v1 spec,
# and the glossary. Dev/process docs — ADRs (docs/adr), RFCs (docs/spec/rfcs), and
# the governance pages — are intentionally NOT published here; they live in the
# repository for contributors.
include_patterns = [
    "index.md",
    "docs/getting-started.md",
    "docs/capability-matrix.md",
    "docs/implementations.md",
    "docs/interoperability.md",
    "docs/interop/**",
    # docs/methodology.md is NOT listed: bench/gen_results_page.py splices its
    # prose into docs/performance.md, so listing it here would publish every
    # paragraph twice under two different URLs. The splice is heading-keyed — the
    # generator looks methodology.md's section headings up by name — so renaming a
    # heading there silently drops a section from the published page.
    "docs/performance.md",
    "docs/test-report.md",
    "docs/reference/**",
    # Contributor design programs (concurrency, configuration): published because the
    # measured per-target cost of the reference implementation is material to anyone
    # deploying it. NOT the standard — docs/design/README.md says so in its first line.
    "docs/design/**",
    "docs/modules/**",
    "docs/examples/**",
    "docs/spec/v1.md",
    "docs/spec/index.md",
    "CONTEXT.md",
    # The intent-routed entry funnel (#1382). Appended rather than filed next to
    # getting-started.md so a parallel doc car adding its own entry conflicts on one
    # line at most.
    "docs/start-here.md",
]
exclude_patterns = [
    "_build",
    "**/_build/**",
    "docs/_doxygen/**",  # Breathe consumes this XML; it is not a Sphinx source doc
    "docs/adr/**",
    "docs/spec/rfcs/**",
    # A design program whose measurements are of ONE downstream consumer's firmware —
    # its buffer budget, its release baselines — is that consumer's document, not this
    # project's, and publishing it here disclosed their release-to-release figures. The
    # exclusion is belt-and-braces: the set has been moved out of the tree entirely, and
    # this keeps a future re-add from silently republishing it.
    "docs/design/ram/**",
    "**/LICENSE",
]

# MyST: render GitHub-flavored ```mermaid fences as mermaid directives; enable a
# few common extensions; emit heading anchors so in-page links resolve.
myst_enable_extensions = ["colon_fence", "deflist", "tasklist"]
myst_fence_as_directive = ["mermaid"]
myst_heading_anchors = 3

# No warning category is suppressed. myst.xref_missing used to be, to tolerate a
# handful of deliberate links into the unpublished trees (ADRs, RFCs, code) — but
# suppressing the category hid every genuinely broken cross-reference behind them,
# including several inside the normative spec. Those deliberate links are now
# absolute github.com URLs (which MyST leaves alone), so the category is a real
# signal again and the docs job runs with -n -W --keep-going.
#
# Nitpicky mode (-n) is on in CI. One class of nitpick is not actionable: Breathe
# renders each C++ declaration with a cross-reference for every type token in it,
# and Sphinx's C++ domain can only resolve the tokens that some directive on the
# site actually declared. A signature mentioning std::span, a `detail::` helper or
# a type documented on another page therefore emits `cpp:identifier reference
# target not found` no matter how complete the page set is. Ignore that one target
# type; every other nitpick (undefined labels from @ref, missing documents, dead
# toctree entries) still fails the build.
nitpick_ignore_regex = [("cpp:identifier", r".*")]

html_theme = "furo"
html_title = "libtracer"
html_show_sourcelink = False

# Custom domain: the project docs are served at the ROOT of libtracer.avatarsd.com
# (a dedicated subdomain, separate from the company's product pages). The CNAME
# file is copied verbatim into the built-site root by html_extra_path, so every
# Actions deploy re-asserts the domain (GitHub would otherwise drop a domain set
# only in repo settings). html_baseurl is the canonical origin for absolute /
# OpenGraph / sitemap URLs. NB: html_extra_path, like html_static_path, is
# relative to the CONFIG dir (docs/) — so "_extra" is docs/_extra.
html_baseurl = "https://libtracer.avatarsd.com/"
html_extra_path = ["_extra"]

# A few restrained touches layered on the stock furo theme (palette + fonts
# unchanged): tabular figures in tables, softly-framed code/diagram blocks, a
# monospace stack for code. Furo's own light/dark tokens carry the rest.
# NB: html_static_path is relative to the CONFIG dir (docs/, where this conf.py
# lives), unlike include_patterns which are relative to the source root — so this
# is "_static", i.e. docs/_static, not "docs/_static".
html_static_path = ["_static"]
html_css_files = ["custom.css", "version-switcher.css"]
# The sidebar version switcher. It reads a generated versions.json at the site
# root (tools/gen_versions_json.py, emitted by docs.yml after the build) and
# renders a picker only once a released vX.Y.Z subtree is actually deployed; until
# then it shows the current version as a chip. Base-path agnostic (resolves via
# Sphinx's URL_ROOT), so it works on github.io and on a custom domain alike.
html_js_files = ["version-switcher.js"]


# --- Doxygen references no Sphinx directive can ever define -----------------------
# Doxygen emits a <derivedcompoundref> for EVERY subclass of a documented base,
# including subclasses whose namespace EXCLUDE_SYMBOLS drops (core/Doxyfile removes
# `*::detail`). Breathe renders that list as "Subclassed by …" with a reference each,
# so a base like tr::mem::mem_backend_t points at labels that, by policy, no directive
# can ever define. That is not a broken cross-reference — it is the exclusion policy
# showing through — so render those targets as plain literal text instead of warning.
# Scoped to Doxygen ids inside a `detail` namespace: every other undefined label
# (a rotted @ref, an entity no page embeds) still fails the nitpicky build.
import re as _re

from docutils import nodes as _nodes

# A Doxygen id that names a FILE compound (`mem__pool_8hpp`, `rope_8hpp_source`) is
# the second structurally-undefinable class: Breathe prints an `#include` line for
# some entity kinds, and Doxygen auto-links a bare header name in prose, but no
# {doxygen*} directive publishes a file, so no page can ever define that label.
_UNDEFINABLE = _re.compile(r"_1_1detail(_1_1|__)|^[a-z_0-9]+_8hpp(_source)?$")


def _undefinable_doxygen_ref(app, env, node, contnode):
    """Render a Doxygen label no directive can define as literal text, not a warning."""
    if node.get("refdomain") == "std" and node.get("reftype") == "ref":
        target = node.get("reftarget", "")
        if _UNDEFINABLE.search(target):
            return _nodes.literal("", contnode.astext())
    return None


def setup(app):
    app.connect("missing-reference", _undefinable_doxygen_ref)
    return {"parallel_read_safe": True, "parallel_write_safe": True}
