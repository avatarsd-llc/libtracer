#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Build + run the mem_cuda GPU tests in a CUDA container on the LOCAL GPU
# (docs/adr/0024). Requires Docker with GPU access via CDI
# (`nvidia-ctk cdi generate` → `--device=nvidia.com/gpu=all`). This is a LOCAL
# developer test — it is never run in CI (CI runners have no GPU).
#
# The backend is a TIER MODULE since #1381: this configures `backends/cuda`, which is its
# own CMake project and pulls core in as a subdirectory. There is no `-DLIBTRACER_WITH_CUDA`
# any more — the tier IS the option.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="${CUDA_IMAGE:-nvidia/cuda:12.6.2-devel-ubuntu24.04}"

echo ">> mem_cuda GPU test in ${image}"
docker run --rm --device=nvidia.com/gpu=all -v "${repo}:/src:ro" -w /tmp/build "${image}" bash -ec '
  apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq cmake g++ >/dev/null
  cmake -S /src/backends/cuda -B /tmp/build -DBUILD_TESTING=ON \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
  cmake --build /tmp/build -j
  ctest --test-dir /tmp/build --output-on-failure -R cuda
'
