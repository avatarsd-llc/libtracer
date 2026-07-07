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
]

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
    "docs/performance.md",
    "docs/test-report.md",
    "docs/reference/**",
    "docs/modules/**",
    "docs/examples/**",
    "docs/spec/v1.md",
    "docs/spec/README.md",
    "CONTEXT.md",
]
exclude_patterns = [
    "_build",
    "**/_build/**",
    "docs/_doxygen/**",  # Breathe consumes this XML; it is not a Sphinx source doc
    "docs/adr/**",
    "docs/spec/rfcs/**",
    "**/LICENSE",
]

# MyST: render GitHub-flavored ```mermaid fences as mermaid directives; enable a
# few common extensions; emit heading anchors so in-page links resolve.
myst_enable_extensions = ["colon_fence", "deflist", "tasklist"]
myst_fence_as_directive = ["mermaid"]
myst_heading_anchors = 3

# Cross-links into the code tree (../../core/, etc.) are not documents; don't fail
# the build over them. Run with -W later once links are polished.
suppress_warnings = ["myst.xref_missing"]

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
