# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
#
# Sphinx configuration for the libtracer documentation site.
#
# The source root is the repository root (this conf.py lives in docs/ and is
# pointed at via `sphinx-build -c docs . <out>`), so the root glossary
# (CONTEXT.md), governance docs, and the docs/ tree render as one cohesive site
# with working relative cross-links. `include_patterns` scopes the build to the
# documentation material — the code directories and CLAUDE.md are excluded.

project = "libtracer"
project_copyright = "2026, Avatar LLC"
author = "Avatar LLC"

extensions = [
    "myst_parser",
    "sphinxcontrib.mermaid",
    "sphinx.ext.githubpages",  # emit .nojekyll so underscore dirs (_static) serve
]

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
    "docs/performance.md",
    "docs/reference/**",
    "docs/modules/**",
    "docs/spec/v1.md",
    "docs/spec/README.md",
    "CONTEXT.md",
]
exclude_patterns = [
    "_build",
    "**/_build/**",
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
