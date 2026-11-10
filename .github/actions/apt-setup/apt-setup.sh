#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Point apt's Ubuntu archive and security sources at an ordered mirror list, and make a
# dead mirror fail fast instead of stalling the job. archive.ubuntu.com and
# security.ubuntu.com have timed out over IPv4 from the self-hosted runners while other
# mirrors answered; apt's `mirror+file:` method tries the listed
# mirrors in priority order (without priorities it would pick them at random).
#
# Rewrites only archive.ubuntu.com / security.ubuntu.com URIs (deb822 ubuntu.sources and
# classic sources.list); PPAs, ports and third-party sources are left alone. Idempotent.
# Runs as root (containers) or through sudo (host runner).
set -eu

if [ "$(id -u)" -eq 0 ]; then SUDO=""; else SUDO="sudo"; fi

$SUDO mkdir -p /etc/apt/mirrors /etc/apt/apt.conf.d

printf '%s\tpriority:1\n%s\tpriority:2\n%s\tpriority:3\n' \
  'http://azure.archive.ubuntu.com/ubuntu/' \
  'http://mirrors.edge.kernel.org/ubuntu/' \
  'http://archive.ubuntu.com/ubuntu/' \
  | $SUDO tee /etc/apt/mirrors/ubuntu-archive.list >/dev/null
printf '%s\tpriority:1\n%s\tpriority:2\n%s\tpriority:3\n' \
  'http://azure.archive.ubuntu.com/ubuntu/' \
  'http://mirrors.edge.kernel.org/ubuntu/' \
  'http://security.ubuntu.com/ubuntu/' \
  | $SUDO tee /etc/apt/mirrors/ubuntu-security.list >/dev/null

printf '%s\n' \
  'Acquire::Retries "3";' \
  'Acquire::http::Timeout "15";' \
  'Acquire::https::Timeout "15";' \
  'Acquire::ConnectionAttemptDelayMsec "250";' \
  | $SUDO tee /etc/apt/apt.conf.d/99ci-mirror-timeouts >/dev/null

for f in /etc/apt/sources.list /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources; do
  [ -f "$f" ] || continue
  $SUDO sed -i -E \
    -e 's#https?://([a-z0-9-]+\.)*security\.ubuntu\.com/ubuntu/?#mirror+file:/etc/apt/mirrors/ubuntu-security.list#g' \
    -e 's#https?://([a-z0-9-]+\.)*archive\.ubuntu\.com/ubuntu/?#mirror+file:/etc/apt/mirrors/ubuntu-archive.list#g' \
    "$f"
done
