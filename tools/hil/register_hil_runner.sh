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
#   3. configures it with the HIL label and installs it as a systemd service;
#   4. installs the sudoers drop-in the workflow's self-heal steps need, and
#      PROVES it by running the exact command those steps run (#1586). Without
#      the drop-in `sudo -n chown -R ...` fails, `|| true` swallows the failure,
#      and the root-owned-leftover cleanup is a silent no-op that resurfaces as a
#      broken `actions/checkout` in an unrelated job.
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
#   --skip-sudoers    do not touch /etc/sudoers.d (the host already grants the
#                     runner user passwordless `chown -R` inside its _work tree);
#                     the proof below still runs, and still fails loudly
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
SKIP_SUDOERS=0
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
    --skip-sudoers) SKIP_SUDOERS=1; shift ;;
    -h|--help) sed -n '5,55p' "${BASH_SOURCE[0]}"; exit 0 ;;
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

# --- 4. the self-heal sudoers grant, and its PROOF ---------------------------
#
# `.github/workflows/hil-esp32c6.yml` opens and closes with
#
#     sudo -n chown -R "$(id -un):$(id -gn)" "$GITHUB_WORKSPACE" "$RUNNER_TEMP" || true
#
# which reclaims root-owned files a CONTAINER job left in the shared `_work` tree
# (#1538). `sudo -n` never prompts and `|| true` swallows the refusal, so a host
# with no rule for this runs the repair as a SILENT no-op — and the leftovers
# come back as a broken `actions/checkout` in some unrelated job hours later
# (#1586). Provisioning it belongs here, with the registration, and it is
# VERIFIED rather than assumed: the check below runs the workflow's own command
# against the workflow's own paths.
WORK="${RUNNER_DIR}/_work"
RUNNER_USER="$(id -un)"
RUNNER_GROUP="$(id -gn)"
# The two paths the workflow passes, created now so the proof has something real to
# chown. The runner creates them on its first job anyway; making them here costs
# nothing and is what lets the verification be an actual `chown` rather than a
# `sudo -l` reading of the policy.
PROBE_WS="${WORK}/libtracer/libtracer"
PROBE_TMP="${WORK}/_temp"
mkdir -p "$PROBE_WS" "$PROBE_TMP"

if [ "$SKIP_SUDOERS" -eq 0 ]; then
  echo "==> installing the self-heal sudoers drop-in for ${RUNNER_USER}"
  STAGED="$(mktemp)"
  trap 'rm -f "$STAGED"' EXIT
  sed -e "s|@USER@|${RUNNER_USER}|g" \
      -e "s|@GROUP@|${RUNNER_GROUP}|g" \
      -e "s|@WORK@|${WORK}|g" \
      "${HERE}/sudoers-libtracer-hil.in" > "$STAGED"
  # Validated BEFORE it goes in place: a syntax error in /etc/sudoers.d breaks sudo
  # for the whole host, which is a much worse outcome than the silent no-op this
  # file exists to fix.
  visudo -cf "$STAGED" >/dev/null || die "generated sudoers drop-in does not parse — not installing it"
  sudo install -m 0440 -o root -g root "$STAGED" /etc/sudoers.d/libtracer-hil
  sudo visudo -c >/dev/null || die "/etc/sudoers.d is now invalid — remove /etc/sudoers.d/libtracer-hil"
fi

echo "==> proving the self-heal grant with the workflow's own command"
if ! sudo -n chown -R "${RUNNER_USER}:${RUNNER_GROUP}" "$PROBE_WS" "$PROBE_TMP" 2>/dev/null; then
  die "$(cat <<MSG
the HIL self-heal step will be a SILENT no-op on this host (#1586).

  ${RUNNER_USER} cannot run, without a password:

    sudo -n chown -R ${RUNNER_USER}:${RUNNER_GROUP} ${PROBE_WS} ${PROBE_TMP}

  The workflow runs exactly that (hil-esp32c6.yml, first and last steps) behind a
  \`|| true\`, so the refusal is swallowed and the root-owned leftovers a container
  job leaves in ${WORK} come back as a broken actions/checkout in another job.

  Fix it by installing the drop-in this script ships:

    sed -e 's|@USER@|${RUNNER_USER}|g' -e 's|@GROUP@|${RUNNER_GROUP}|g' \\
        -e 's|@WORK@|${WORK}|g' tools/hil/sudoers-libtracer-hil.in > /tmp/libtracer-hil
    visudo -cf /tmp/libtracer-hil && sudo install -m 0440 -o root -g root \\
        /tmp/libtracer-hil /etc/sudoers.d/libtracer-hil

  then re-run this script (it is idempotent), or re-run it with --skip-sudoers if
  the host grants the equivalent some other way.
MSG
)"
fi
echo "==> self-heal grant OK"

cat <<EOF

==> registered: name=${RUNNER_NAME} label=${LABEL}

Remaining act (deliberately manual — it is what arms the workflow):

  gh variable set HIL_ENABLED --body true --repo avatarsd-llc/libtracer

Then smoke it:

  gh workflow run hil-esp32c6.yml --repo avatarsd-llc/libtracer
EOF
