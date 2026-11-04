#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Run a real-vcan test on a PRIVATE vcan0 (#2523).
#
# Usage: vcan_private_ns.sh <test-binary> [args...]
#
# transport_can_vcan counts every node on the bus, so two runs sharing the host's
# vcan0 (two sanitizer jobs on one self-hosted runner host, or two local ctest runs)
# see each other's nodes and fail the enumeration checks. This wrapper gives each run
# its own bus: a fresh user + network namespace (no root, no sudo: the caller is root
# only inside it) holding its own vcan0, which disappears with the namespace when the
# test exits. Nothing on the host is created, so nothing needs deleting.
#
# It needs unprivileged user namespaces and the host's `vcan` module already loaded
# (a namespace cannot load modules). Where either is missing, as in a Docker
# container under the default seccomp profile, it says so and runs the test in the
# caller's own network namespace, exactly as before: on the host's shared vcan0 if
# there is one, or the test's own clean self-skip if there is not.
set -u
[ "$#" -ge 1 ] || { echo "usage: $0 <test-binary> [args...]" >&2; exit 2; }

mk='ip link add dev vcan0 type vcan && ip link set up vcan0'
if unshare --user --map-root-user --net sh -c "$mk" >/dev/null 2>&1; then
    echo "vcan_private_ns: running on a private vcan0 in its own network namespace"
    exec unshare --user --map-root-user --net sh -c "$mk && exec \"\$@\"" sh "$@"
fi
echo "vcan_private_ns: no private namespace here (no user namespaces or no vcan module);" \
    "running in the caller's network namespace"
exec "$@"
