#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Register the ESP32-C6 HIL self-hosted runner on the studio host (#1557).
#
# This is the ONE step no agent can take: it needs a registration token that is
# valid for an hour and is minted by a repo admin. Everything else the HIL plane
# needs — the udev rule, the flash/monitor tooling, the workflow — is in the tree
# and dormant until a runner carrying the label below comes online.
#
# WHAT IT DOES, in order:
#   1. installs the udev rule and verifies the device path exists and is usable
#      by THIS user (no root at job time — see 99-libtracer-hil.rules);
#   2. downloads and unpacks the GitHub Actions runner into its own directory,
#      SEPARATE from any existing runner on the host (a second registration in an
#      existing runner's directory replaces it);
#   3. configures it with the HIL label and installs it as a systemd service.
#
# WHY A SEPARATE RUNNER RATHER THAN A LABEL ON THE BENCH RUNNER: the bench runner
# is a MEASUREMENT instrument whose whole validity rests on the host being quiet
# (perf-local.yml's quiescence guard). A HIL job is minutes of USB traffic and a
# python install; sharing one runner process would serialise the two pools
# together and let a HIL run sit inside a bench window. Separate runner, separate
# label, separate concurrency group.
#
# Usage:
#   tools/hil/register_hil_runner.sh --token <REGISTRATION_TOKEN> [options]
#
#   --token TOKEN     required; from Settings -> Actions -> Runners -> New
#                     self-hosted runner, or:
#                       gh api -X POST repos/avatarsd-llc/libtracer/actions/runners/registration-token --jq .token
#   --dir PATH        runner install directory (default ~/actions-runner-hil)
#   --label LABEL     the distinguishing label (default hil-esp32c6)
#   --name NAME       runner name (default <hostname>-hil)
#   --version VER     runner version (default: latest released)
#   --skip-udev       do not touch /etc/udev (rule already installed)
#
# AFTER THIS SCRIPT SUCCEEDS, one more act is required and it is deliberate: set
# the repository variable that arms the workflow —
#
#   gh variable set HIL_ENABLED --body true --repo avatarsd-llc/libtracer
#
# Until then every HIL job no-ops green. See tools/hil/README.md.

set -euo pipefail

REPO_URL="https://github.com/avatarsd-llc/libtracer"
RUNNER_DIR="${HOME}/actions-runner-hil"
LABEL="hil-esp32c6"
RUNNER_NAME="$(hostname)-hil"
RUNNER_VERSION=""
TOKEN=""
SKIP_UDEV=0
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

die() { echo "error: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --token)   TOKEN="${2:-}"; shift 2 ;;
    --dir)     RUNNER_DIR="${2:-}"; shift 2 ;;
    --label)   LABEL="${2:-}"; shift 2 ;;
    --name)    RUNNER_NAME="${2:-}"; shift 2 ;;
    --version) RUNNER_VERSION="${2:-}"; shift 2 ;;
    --skip-udev) SKIP_UDEV=1; shift ;;
    -h|--help) sed -n '5,45p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

[ -n "$TOKEN" ] || die "--token is required (see --help)"
[ "$(id -u)" -ne 0 ] || die "run this as the RUNNER user, not root (it sudo's for the two steps that need it)"

# --- 1. the device -----------------------------------------------------------
if [ "$SKIP_UDEV" -eq 0 ]; then
  echo "==> installing the udev rule for user $(id -un)"
  sed "s/github-runner/$(id -un)/" "${HERE}/99-libtracer-hil.rules" \
    | sudo tee /etc/udev/rules.d/99-libtracer-hil.rules >/dev/null
  sudo udevadm control --reload-rules
  sudo udevadm trigger --subsystem-match=tty
  sleep 1
fi

if [ -e /dev/hil-esp32c6 ]; then
  ls -l /dev/hil-esp32c6
  [ -r /dev/hil-esp32c6 ] && [ -w /dev/hil-esp32c6 ] \
    || die "/dev/hil-esp32c6 exists but $(id -un) cannot read+write it — check the rule's OWNER"
  echo "==> device OK: /dev/hil-esp32c6"
else
  echo "warning: /dev/hil-esp32c6 does not exist — plug the ESP32-C6 devkit in and re-run" >&2
  echo "         (registration continues; the workflow will fail its device-present check)" >&2
fi

# --- 2. the runner package ---------------------------------------------------
if [ -z "$RUNNER_VERSION" ]; then
  RUNNER_VERSION="$(curl -fsSL https://api.github.com/repos/actions/runner/releases/latest \
    | sed -n 's/.*"tag_name": *"v\([^"]*\)".*/\1/p' | head -1)"
  [ -n "$RUNNER_VERSION" ] || die "could not determine the latest runner version; pass --version"
fi
echo "==> runner v${RUNNER_VERSION} into ${RUNNER_DIR}"

[ -e "${RUNNER_DIR}/config.sh" ] || {
  mkdir -p "$RUNNER_DIR"
  TARBALL="actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz"
  curl -fsSL -o "${RUNNER_DIR}/${TARBALL}" \
    "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/${TARBALL}"
  tar xzf "${RUNNER_DIR}/${TARBALL}" -C "$RUNNER_DIR"
  rm -f "${RUNNER_DIR}/${TARBALL}"
}

# --- 3. configure + install the service --------------------------------------
cd "$RUNNER_DIR"
if [ -f .runner ]; then
  echo "==> ${RUNNER_DIR} is already configured; leaving it alone"
else
  ./config.sh --unattended --replace \
    --url "$REPO_URL" \
    --token "$TOKEN" \
    --name "$RUNNER_NAME" \
    --labels "$LABEL" \
    --work _work
fi

sudo ./svc.sh install "$(id -un)"
sudo ./svc.sh start
sudo ./svc.sh status || true

cat <<EOF

==> registered: name=${RUNNER_NAME} label=${LABEL}

Remaining act (deliberately manual — it is what arms the workflow):

  gh variable set HIL_ENABLED --body true --repo avatarsd-llc/libtracer

Then smoke it:

  gh workflow run hil-esp32c6.yml --repo avatarsd-llc/libtracer
EOF
