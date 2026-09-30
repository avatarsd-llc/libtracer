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
# CACHE (#1603). A released subtree is a pure function of (tag, docs toolchain,
# this script), so rebuilding every one of them on every run recomputed constants
# for ~13 minutes. When RELEASE_DOCS_CACHE names a directory, each tag is built
# ONCE into `$RELEASE_DOCS_CACHE/<tag>/` and later runs copy it instead. docs.yml
# persists that directory with actions/cache, keyed on a toolchain fingerprint
# (pip freeze + doxygen --version + this script's hash) plus the ordered tag list:
# a toolchain change invalidates everything, and a new tag restores the previous
# entry by prefix and builds exactly one subtree. Each cached tag carries a marker:
#   .built   the subtree is complete; copy it
#   .failed  this tag's docs do not build with this toolchain; skip it with the
#            same warning instead of paying for the failure on every run
# A directory with neither marker is an interrupted build and is rebuilt. Cached
# tags that no longer exist are pruned. Without RELEASE_DOCS_CACHE the script
# builds every tag, as it always has.
#
# Usage: [RELEASE_DOCS_CACHE=<dir>] tools/assemble_release_docs.sh <built-site-dir>
set -u

out="${1:?usage: assemble_release_docs.sh <built-site-dir>}"
out="$(cd "$out" && pwd)" || exit 1

tags="$(git tag --list | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' || true)"
if [ -z "$tags" ]; then
    echo "assemble_release_docs: no release tags — nothing to assemble"
    exit 0
fi

cache="${RELEASE_DOCS_CACHE:-}"
if [ -n "$cache" ]; then
    mkdir -p "$cache" && cache="$(cd "$cache" && pwd)" || exit 1
    for dir in "$cache"/v*; do
        [ -d "$dir" ] || continue
        if ! printf '%s\n' "$tags" | grep -qxF "$(basename "$dir")"; then
            echo "assemble_release_docs: pruning cached $(basename "$dir")/ (tag gone)"
            rm -rf "$dir"
        fi
    done
fi

# Copies a finished cached subtree into the site, without its marker.
publish_cached() {
    cp -a "$cache/$1" "$out/$1" && rm -f "$out/$1/.built"
}

built=0
reused=0
for tag in $tags; do
    if [ -n "$cache" ]; then
        dest="$cache/$tag"
        if [ -f "$dest/.built" ]; then
            publish_cached "$tag"
            echo "assemble_release_docs: reused cached $tag/"
            reused=$((reused + 1))
            continue
        fi
        if [ -f "$dest/.failed" ]; then
            echo "::warning::docs build for $tag failed (cached verdict) — version omitted from the switcher"
            continue
        fi
        rm -rf "$dest"
    else
        dest="$out/$tag"
    fi

    wt="$(mktemp -d)"
    echo "::group::docs for $tag"
    # Doctrees go to the throwaway worktree: they are build state, not site
    # content, and would otherwise be cached and deployed inside every subtree.
    if git worktree add --detach "$wt" "$tag" &&
        (cd "$wt" && sphinx-build -b html -d "$wt/.doctrees" -c docs . "$dest"); then
        echo "assemble_release_docs: built $tag/"
        built=$((built + 1))
        if [ -n "$cache" ]; then
            touch "$dest/.built"
            publish_cached "$tag"
        fi
    else
        echo "::warning::docs build for $tag failed — version omitted from the switcher"
        rm -rf "${dest:?}"
        if [ -n "$cache" ]; then
            mkdir -p "$dest" && touch "$dest/.failed"
        fi
    fi
    echo "::endgroup::"
    git worktree remove --force "$wt" 2>/dev/null || rm -rf "$wt"
done
echo "assemble_release_docs: $built built, $reused reused from cache"
