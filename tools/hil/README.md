<!--
SPDX-License-Identifier: Apache-2.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# ESP32-C6 hardware-in-the-loop runner

Everything the on-silicon CI plane needs, except the three acts that require hands
on a device (#1557).

The workflow is `.github/workflows/hil-esp32c6.yml`. It is **dormant**: until a
runner labelled `hil-esp32c6` exists and the repository variable `HIL_ENABLED` is
set to `true`, every dispatch, every labelled PR and every nightly run is a green
no-op on a hosted runner with a notice explaining why. Nothing hangs, nothing
queues, nothing reds `main`.

## Why a device at all

Hosted runners cross-compile the ESP plane, link it, read its objects and census
its flash (`esp-idf.yml`). None of that executes an instruction on the chip, and a
growing set of issues is blocked on exactly that — #1532 (the boot repro), #1502,
#1494, #1504 §3.1, the rv32 `HANDLER` sentinel, #183. See #1557 for the full list.

## What the first slice proves

`full_node` built for `esp32c6` with **no Wi-Fi SSID configured** — the CI default
— runs its whole self-proof on the chip, over lwIP's loopback netif with no radio:

* the node reaches `app_main` and brings up a `/sensor/temp` vertex;
* the UDP listener is created **in band**, by a `SPEC` write to the module's
  creator endpoint — the production path;
* an in-process host peer dials it over **real datagrams** and drives a
  `FWD{READ}` round trip, a `:subscribers[]` subscribe write, a durability latch
  and a producer fan-out observed with `graph.await`.

So the console line `full_node self-proof: OK (0 failures)` is the boolean #1532
is held open for: the node booted **and answered a write over the wire**.
`hil_monitor.py` renders it, and fails separately on a boot loop (the #1532
signature — a SILENT-level IDF assert prints nothing at all, so the only evidence
is a second ROM banner), on a panic, on a node that never reached `app_main`, and
on a self-proof that failed a named leg.

## The pieces

| file | what it is |
| --- | --- |
| `99-libtracer-hil.rules` | udev rule: a stable `/dev/hil-esp32c6` owned by the runner user, so no CI step needs root |
| `register_hil_runner.sh` | installs the rule, downloads and registers the runner as a systemd service |
| `hil_flash.py` | `stage` packages a build's flashable image; `flash` drives esptool from the build's own manifest |
| `hil_monitor.py` | captures the console, renders the verdict, exits non-zero with the reason |
| `../tests/test_hil_monitor.py` | the verdict rules and the flash-argument derivation, tested without hardware |

Build and flash are **different jobs on different runners**: a hosted
`container: espressif/idf` job builds and stages, the device job downloads the
staged image. The machine with the hardware never installs ESP-IDF — its whole
dependency is `esptool` from pip.

## Remaining human steps

Three physical acts, in this order.

### 1. Plug the devkit in

An ESP32-C6 devkit on the studio host's USB. Prefer the board's **USB** port (the
chip's built-in USB-Serial/JTAG) over the UART bridge port: no bridge chip, and
esptool drives reset and boot mode over the same CDC control lines.

### 2. Install the udev rule

```sh
sed 's/github-runner/<runner user>/' tools/hil/99-libtracer-hil.rules \
  | sudo tee /etc/udev/rules.d/99-libtracer-hil.rules
sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=tty
ls -l /dev/hil-esp32c6      # must exist, owned by the runner user
```

The workflow asserts this before it flashes anything: a missing or root-only
device path fails with the rule's path in the annotation, not with an esptool
traceback. Set the repository variable `HIL_PORT` if the symlink must be named
something else (a `/dev/serial/by-id/...` path works and is equally stable).

### 3. Register the runner and arm the plane

Mint a registration token (valid one hour) and run the script **as the runner
user**:

```sh
gh api -X POST repos/avatarsd-llc/libtracer/actions/runners/registration-token --jq .token
tools/hil/register_hil_runner.sh --token <TOKEN>
```

It installs the udev rule (skip with `--skip-udev`), unpacks the runner into
`~/actions-runner-hil` — **a separate directory from any existing runner**;
re-registering inside one replaces it — labels it `hil-esp32c6`, and installs it
as a systemd service.

A separate runner rather than a label on the bench runner, deliberately: the bench
runner's validity rests on the host being quiet (`perf-local.yml`'s quiescence
guard), and minutes of USB traffic inside a bench window is a contaminated sample.

Then arm the plane:

```sh
gh variable set HIL_ENABLED --body true --repo avatarsd-llc/libtracer
gh workflow run hil-esp32c6.yml --repo avatarsd-llc/libtracer     # smoke it
```

To disarm — device unplugged, host rebuilt, board wedged — set `HIL_ENABLED` to
anything else. Everything goes back to a green no-op.

## Running it against a PR

Add the `hil` label to the pull request. Fork PRs never reach the hardware host
(the guard requires a same-repository head branch) — there is no hosted fallback
for a job whose whole purpose is a USB cable.

## Reading a failure

Every run uploads `hil-transcript.txt` (the raw console) and `hil-result.json`
(the machine-readable verdict), on success and on failure, for 90 days. Re-judge
an uploaded transcript with no hardware in the loop:

```sh
python3 tools/hil/hil_monitor.py --analyse-file hil-transcript.txt
```

## Cold-recovering a wedged board

The auto-reset lines recover an application that crashed. They do not recover a
board whose USB stack is gone. #1557 budgets for a switchable USB hub or a relay
on the devkit's power line as the fallback; nothing in this tree assumes one
exists, and adding it is a separate change — the workflow's per-job timeout is
what keeps a wedged board from holding the queue in the meantime.
