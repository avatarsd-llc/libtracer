# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# CI base image for every `container:` job (core-ci.yml, doc-citations.yml and the rest).
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
#   doxygen                                            doxygen-gate, docs-build
#   unzip curl                                         docs-build
#   gcc-arm-none-eabi libnewlib-arm-none-eabi          footprint-cortexm0
#     libstdc++-arm-none-eabi-newlib gh
# The docs-build and footprint-cortexm0 rows (#2348) are baked in so those jobs run no apt
# step: an install step is where both jobs wedged (their headers carry the measurements).
# libnewlib-arm-none-eabi and libstdc++-arm-none-eabi-newlib are named explicitly because
# the install is
# --no-install-recommends and the toolchain only Recommends them (transitively), yet
# nano.specs / nosys.specs and the target's C++ headers come from them.
#   g++-15 gcc-15 (ppa:ubuntu-toolchain-r/test)        build-test-gcc15
# g++-15 and gh are baked in because their run-time installs were the flake surface (#2627):
# a PPA add died on `IncompleteRead`, an `apt-get install gh` hit its timeout, each one
# failing a landing before any project code ran. The PPA is added only for that install
# and then removed again, together with software-properties-common, so a job's own
# `apt-get update` never reaches launchpad. The compiler is a snapshot of the PPA at build
# time: to pick up a newer g++-15, edit this file (a new tag) and rebuild.
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
      unzip curl \
      gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib gh \
 && rm -rf /var/lib/apt/lists/*

RUN apt-get update -qq \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
      software-properties-common \
 && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
      g++-15 gcc-15 \
 && add-apt-repository -y --remove ppa:ubuntu-toolchain-r/test \
 && DEBIAN_FRONTEND=noninteractive apt-get purge -y -qq --auto-remove software-properties-common \
 && rm -rf /var/lib/apt/lists/* /etc/apt/sources.list.d/ubuntu-toolchain-r-*
