# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# One image for every node of the #408 decentralized mesh testbed. The four
# compose services differ ONLY in their `command:` — a node's identity, its
# listeners and its ctrl port are argv, and nothing else about it is special.
#
# Build context is the REPO ROOT (compose.yml sets it), so `core/` is copied whole:
# the node links the ordinary `libtracer` target with the built-in socket factories,
# exactly as any host embedding would.
#
# Stage 1 builds; stage 2 carries only the binary, so a node image holds no
# toolchain and the four containers stay cheap to start.

FROM ubuntu:24.04 AS build

# ubuntu-24.04's default g++-13 covers the C++23 the core needs (core-ci's
# baseline; its gcc-15 job is a separate forward-compat gate, not a requirement).
RUN apt-get update -qq \
    && apt-get install -y -qq --no-install-recommends \
        cmake g++ make ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY core/ core/

# Only the mesh_node target — the rest of the test suite is not needed in an image
# whose sole job is to be one node of a mesh.
RUN cmake -S core -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    && cmake --build build --target mesh_node -j "$(nproc)"

FROM ubuntu:24.04

# The node is a plain dynamically-linked C++ binary: libstdc++ + libgcc only.
RUN apt-get update -qq \
    && apt-get install -y -qq --no-install-recommends libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/tests/mesh_node /usr/local/bin/mesh_node

# Unbuffered stdout so the READY line reaches `docker compose logs` immediately
# (the binary flushes it explicitly, but keep the pipe honest for diagnostics too).
ENV STDBUF=0
ENTRYPOINT ["/usr/local/bin/mesh_node"]
