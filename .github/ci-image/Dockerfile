# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# CI base image for every `container:` job in core-ci.yml and doc-citations.yml.
#
# WHY. Those jobs used to start from a bare `ubuntu:24.04` and apt-install their tool set
# on every run: an `apt-get update` plus a few hundred MB unpacked into a fresh container
# layer, per job, thrown away when the job ends. On the self-hosted `ci-local` runners that
# is the largest remaining write load on the host's SSD, and it is the step core-ci's
# header calls the wedge surface (p50 0.67 min, max 1.32 over 90 runs). Baking the tool
# set in once removes both.
#
# WHAT IS IN IT. Exactly the union of what those jobs installed — nothing more, so a job
# still fails loudly on a tool it never declared:
#   build-essential cmake ccache git ca-certificates   every build / test leg
#   python3                                            ci-paths, doc-citations
#   clang-format                                       format
#   doxygen                                            doxygen-gate
# g++-15 is NOT here: build-test-gcc15 adds the ubuntu-toolchain-r PPA on top, because a
# PPA compiler baked into an image would silently stop tracking the PPA.
#
# HOW IT IS VERSIONED. The image tag is `noble-` + the first 12 hex chars of this file's
# sha256 (`sha256sum .github/ci-image/Dockerfile | cut -c1-12`). Editing this file
# therefore changes the tag, ci-image.yml builds and pushes it, and every `image:`
# reference must be bumped in the same PR — tools/check_ci_image_tag.py (run by the
# `ci-paths` job) fails the build if any reference disagrees with this file's hash.
# The first push of a PR that edits this file races the image build: re-run the
# container jobs once ci-image.yml is green.
FROM ubuntu:24.04

RUN apt-get update -qq \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
      build-essential cmake ccache git ca-certificates \
      python3 \
      clang-format \
      doxygen \
 && rm -rf /var/lib/apt/lists/*
