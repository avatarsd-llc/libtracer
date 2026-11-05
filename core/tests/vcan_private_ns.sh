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
# It needs unprivileged user namespaces (which a kernel sysctl or an AppArmor policy
# can restrict) and the host's `vcan` module already loaded (a namespace cannot load
# modules). LIBTRACER_VCAN_NS picks what happens when either is missing:
#   fallback (default) -- say so and run the test in the caller's network namespace,
#                         as before: on the host's shared vcan0 if there is one, or
#                         the test's own clean self-skip if there is not. For Docker
#                         jobs and hosts without kernel CAN.
#   required           -- fail with exit 3 (not a pass, not ctest's skip code). For CI
#                         hosts that must give every run its own bus.
set -u
[ "$#" -ge 1 ] || { echo "usage: $0 <test-binary> [args...]" >&2; exit 2; }
mode="${LIBTRACER_VCAN_NS:-fallback}"
case "$mode" in
fallback | required) ;;
*) echo "::error::vcan_private_ns: LIBTRACER_VCAN_NS='$mode' is neither fallback nor required" >&2; exit 2 ;;
esac

mk='ip link add dev vcan0 type vcan && ip link set up vcan0'
if unshare --user --map-root-user --net sh -c "$mk" >/dev/null 2>&1; then
    echo "vcan_private_ns: running on a private vcan0 in its own network namespace"
    exec unshare --user --map-root-user --net sh -c "$mk && exec \"\$@\"" sh "$@"
fi
why="cannot make a private vcan0: unprivileged user namespaces may be restricted"
why="$why (kernel sysctl or AppArmor), unshare or ip may be missing, or the vcan module is not loaded"
if [ "$mode" = required ]; then
    echo "::error::vcan_private_ns: $why; LIBTRACER_VCAN_NS=required, so this run fails rather than share the host's vcan0 (#2523)"
    exit 3
fi
echo "vcan_private_ns: $why; running in the caller's network namespace (LIBTRACER_VCAN_NS=fallback)"
exec "$@"
