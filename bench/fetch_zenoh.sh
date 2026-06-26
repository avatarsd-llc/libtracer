#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Vendor the prebuilt zenoh-c (lib + C headers) and zenoh-cpp (header-only) into
# bench/vendor/ for the comparison build. Not committed — these are third-party
# binaries. x86_64 Linux; adjust ZVER / the asset name for other platforms.
set -euo pipefail
cd "$(dirname "$0")"
ZVER="1.9.0"
mkdir -p vendor && cd vendor

echo "Fetching zenoh-c ${ZVER} (prebuilt, x86_64-unknown-linux-gnu)…"
curl -sSL -o zenoh-c.zip \
    "https://github.com/eclipse-zenoh/zenoh-c/releases/download/${ZVER}/zenoh-c-${ZVER}-x86_64-unknown-linux-gnu-standalone.zip"
rm -rf zenoh-c && unzip -q -o zenoh-c.zip -d zenoh-c && rm -f zenoh-c.zip

echo "Fetching zenoh-cpp ${ZVER} (header-only)…"
curl -sSL -o zenoh-cpp.tar.gz \
    "https://github.com/eclipse-zenoh/zenoh-cpp/archive/refs/tags/${ZVER}.tar.gz"
rm -rf "zenoh-cpp-${ZVER}" && tar xzf zenoh-cpp.tar.gz && rm -f zenoh-cpp.tar.gz

echo "Done. zenoh-c lib: $(ls zenoh-c/lib/libzenohc.so 2>/dev/null || echo MISSING)"
