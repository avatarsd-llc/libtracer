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
#   /opt/mbedtls-src/mbedtls-3.6.5 (source, unbuilt)  build-test-noise (crypto=psa)
# The mbedtls source is baked in because its run-time download was the flake surface too
# (#2667): the GitHub release-asset URL answered 504 for every landing while github.com
# stayed up. It is the tag archive plus the `framework` submodule at the commit the tag
# records, each pinned by sha256, unbuilt: the job edits mbedtls_config.h before it builds.
# g++-15 and gh are baked in because their run-time installs were the flake surface (#2627):
# a PPA add died on `IncompleteRead`, an `apt-get install gh` hit its timeout, each one
# failing a landing before any project code ran. The PPA is added only for that install
# and then removed again, together with software-properties-common, so a job's own
# `apt-get update` never reaches launchpad. The compiler is a snapshot of the PPA at build
# time: to pick up a newer g++-15, edit this file (a new tag) and rebuild.
# CLANG STAYS ON GCC 13. Clang picks the newest GCC installation it finds, so with g++-15
# baked in, a clang a job installs would compile against libstdc++ 15. GenMC's LLVM 19 cannot
# parse those headers (`RAND_MAX` undeclared in stl_algo.h: the #2631 landing). Clang reads a
# default config file named after its driver (clang.cfg, clang++.cfg) from the directory its
# binary lives in, so the last layer writes one per LLVM major noble ships (18-20) that pins
# `--gcc-install-dir` to noble's own GCC 13: the base the image had before g++-15 joined it.
# A job's `apt-get install clang-NN` later fills those directories and leaves the files alone.
# An explicit `--gcc-install-dir` or `--config` on a clang command line still wins.
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

RUN for v in 18 19 20; do \
      mkdir -p "/usr/lib/llvm-$v/bin"; \
      for d in clang clang++; do \
        echo '--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13' > "/usr/lib/llvm-$v/bin/$d.cfg"; \
      done; \
    done

# mbedtls 3.6.5 source for the psa noise leg (#2667). The release tarball is the tag tree
# plus the framework submodule; this assembles the same tree from two pinned archives.
RUN set -e; d=/opt/mbedtls-src; v=3.6.5; fw=457996474728cb8e968ed21953b72f74d2f536b2; \
    mkdir -p "$d"; cd "$d"; \
    curl -fsSL --retry 5 --retry-all-errors -o tag.tar.gz \
      "https://codeload.github.com/Mbed-TLS/mbedtls/tar.gz/refs/tags/mbedtls-$v"; \
    echo "828ed5cde86688ff0c9de511cabd1598544006336759a75aa917cb2c58f1128f  tag.tar.gz" | sha256sum -c -; \
    curl -fsSL --retry 5 --retry-all-errors -o fw.tar.gz \
      "https://codeload.github.com/Mbed-TLS/mbedtls-framework/tar.gz/$fw"; \
    echo "c15bcc24fc43bdf325b0e04fa63270daf6e9488167b94b46547d8134efa9c906  fw.tar.gz" | sha256sum -c -; \
    tar -xzf tag.tar.gz; mv "mbedtls-mbedtls-$v" "mbedtls-$v"; \
    rm -rf "mbedtls-$v/framework"; mkdir "mbedtls-$v/framework"; \
    tar -xzf fw.tar.gz -C "mbedtls-$v/framework" --strip-components=1; \
    rm tag.tar.gz fw.tar.gz; \
    test -f "mbedtls-$v/framework/scripts/mbedtls_framework/__init__.py"; \
    test -f "mbedtls-$v/include/mbedtls/mbedtls_config.h"
