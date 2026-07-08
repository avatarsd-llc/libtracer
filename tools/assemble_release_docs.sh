#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Assemble released-version documentation subtrees into an already-built site.
#
# For every release tag `vMAJOR.MINOR.PATCH`, build that tag's Sphinx docs from a
# detached git worktree into `<built-site-dir>/vX.Y.Z/`, so the sidebar version
# switcher (docs/_static/version-switcher.js + tools/gen_versions_json.py) has a
# real subtree to offer. Runs on EVERY docs deploy — actions/deploy-pages replaces
# the whole site per main push, so assembling only at release time would vanish
# on the next deploy; rebuilding from tags each time is the only shape that
# survives wholesale redeploys.
#
# A tag whose docs fail to build is SKIPPED with a warning (partial output is
# removed), keeping the versions.json manifest honest: it only ever lists
# subtrees that exist. Requires: full git history + tags in the checkout
# (fetch-depth: 0), and the same python env the main build used (conf.py runs
# doxygen itself when present, check=False).
#
# Usage: tools/assemble_release_docs.sh <built-site-dir>
set -u

out="${1:?usage: assemble_release_docs.sh <built-site-dir>}"
out="$(cd "$out" && pwd)" || exit 1

tags="$(git tag --list | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' || true)"
if [ -z "$tags" ]; then
    echo "assemble_release_docs: no release tags — nothing to assemble"
    exit 0
fi

for tag in $tags; do
    wt="$(mktemp -d)"
    echo "::group::docs for $tag"
    if git worktree add --detach "$wt" "$tag" &&
        (cd "$wt" && sphinx-build -b html -c docs . "$out/$tag"); then
        echo "assemble_release_docs: built $tag/"
    else
        echo "::warning::docs build for $tag failed — version omitted from the switcher"
        rm -rf "${out:?}/$tag"
    fi
    echo "::endgroup::"
    git worktree remove --force "$wt" 2>/dev/null || rm -rf "$wt"
done
