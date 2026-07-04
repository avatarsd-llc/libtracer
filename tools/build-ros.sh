#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Build-verify the rmw_tracer ament package (+ libtracer) in a ROS 2 container,
# locally. Confirms the package + CMake + libtracer (C++23) + the ROS rmw headers
# compile and link together. This is a LOCAL developer check — never run in CI
# (it needs the ROS 2 toolchain). See bindings/ros2/README.md and ADR-0023.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="${ROS_IMAGE:-ros:jazzy-ros-base}"

echo ">> rmw_tracer build-verify in ${image}"
docker run --rm -v "${repo}:/src:ro" "${image}" bash -ec '
  apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    python3-colcon-common-extensions cmake build-essential >/dev/null 2>&1 || true
  # The workspace src IS the repo, so the package CMake'"'"'s ../../core resolves
  # (the package lives at bindings/ros2; core/ is two up). The repo is read-only;
  # colcon builds out-of-source into /tmp/ws/build.
  mkdir -p /tmp/ws
  ln -s /src /tmp/ws/src
  cd /tmp/ws
  source /opt/ros/jazzy/setup.bash
  colcon build --packages-select rmw_tracer --event-handlers console_direct+
'
