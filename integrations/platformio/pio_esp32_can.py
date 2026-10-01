# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# PlatformIO build hook (referenced from library.json build.extraScript).
#
# On an espressif32 target it compiles the ESP-IDF TWAI can_link_t
# (integrations/esp-idf/libtracer/twai_link.cpp) and exposes its header, so
# `tr::net::transport_can` gets a real on-chip CAN 2.0 bus driver under
# PlatformIO. Construct a `tr::net::twai_link_t{{tx_gpio, rx_gpio, bitrate}}`,
# hand it to a CLASSIC `transport_can`, and register `can_transport_factory()`.
#
# It also owns the package's SOURCE FILTER on EVERY platform (see the SRC_FILTER
# block below): the portable baseline everywhere, minus the portable WS pair on
# espressif32 (#984 — no WebSocket ships there; a udp+tcp-only builtin dispatcher
# is compiled in its place). Beyond that, on a non-espressif32 platform the ESP
# glue is a NO-OP: twai_link needs ESP-IDF TWAI + FreeRTOS headers, which only
# exist on espressif32/espidf. The guard keeps it from affecting any non-esp32
# PlatformIO build.
#
# CI-BUILT, NOT BUS-PROVEN: tests/packaging/pio_esp32_can is a `framework = espidf`
# consumer of the PACKED package that names twai_link_t, a CLASSIC transport_can over
# it, and can_transport_factory(); the `pio-esp32-can` workflow `pio run`s it, so each
# step below — this script running at all, both CPPPATH entries, the BuildSources call
# and its guard — is gated at COMPILE+LINK. So are the `library.json` export entries this
# fixture actually reaches; entries it does not compile against (LICENSE, README.md, and
# any header no translation unit here includes) can be dropped without reddening the job.
# That frames no CAN bit: moving frames on real silicon is still the open on-bus
# sign-off, and nothing in CI energises a pin.
import os

Import("env", "pio_lib_builder")  # noqa: F821  (injected by PlatformIO/SCons at build time)

# The package's source filter lives HERE, not in library.json's `build.srcFilter`:
# PlatformIOLibBuilder.src_filter gives a manifest srcFilter precedence over the
# SRC_FILTER an extra script sets (piolib.py resets env SRC_FILTER right before
# running the script precisely so the script may own it), and #984's espressif32
# exclusions below are PER-ENVIRONMENT — a static manifest string cannot express
# them. Precedence verified against the pinned platformio==6.1.18 the CI job
# installs; bumping that pin is the act that re-validates this assumption.
#
# This is the portable baseline every platform gets — the former manifest filter,
# verbatim: the whole core minus the TUs that need host-only stacks (msquic,
# libwebtransport) or Linux kernel headers (SocketCAN; its stub stays in). The GPU
# backend needs no entry since #1381 — it is a `backends/` tier module and this
# package only exports `core/`, so there is nothing to filter out.
env.Replace(  # noqa: F821
    SRC_FILTER=[
        "+<*>",
        "-<transport_quic.cpp>",
        "-<transport_webtransport.cpp>",
        "-<socketcan_link.cpp>",
    ]
)

# The two LINK MODULES (#1670): the ADR-0044 bus tier and the RFC-0014 S5 liveness engine.
# Both are OFF by default since v0.17.0 — `default_config_t::kBusLinks` and `kSelfHealLinks`
# are `false` — and each pairs a config member with translation units that refuse to compile
# without it (`transport_can.cpp`: a CAN link is a bus by construction; `self_heal_link.cpp`:
# the engine itself). A consumer opts in from platformio.ini, ONE switch per module, and this
# script derives BOTH halves from it, the way the ESP-IDF component derives them from Kconfig:
#
#   [env:...]
#   custom_libtracer_bus_links = yes        ; CAN (+ TWAI on espressif32), peer-named listeners
#   custom_libtracer_self_heal_links = yes  ; engine-managed udp/tcp/ws DIAL connections
#
# The member half is a generated `libtracer/config_override.hpp`, put FIRST on the include
# path. It YIELDS to a fragment the consumer supplies later on the path (the pattern
# core/tests/instrumented uses): it includes that one instead and static_asserts that it binds
# the two members the way these switches say, so a hand-written fragment can carry other knobs
# without the two halves ever disagreeing silently.
def _libtracer_opt_in(name):
    value = str(env.GetProjectOption(name, "no")).strip().lower()  # noqa: F821
    return value in ("1", "y", "yes", "true", "on")


_bus_links = _libtracer_opt_in("custom_libtracer_bus_links")
_self_heal_links = _libtracer_opt_in("custom_libtracer_self_heal_links")
if not _bus_links:
    env.Append(  # noqa: F821
        SRC_FILTER=["-<transport_can.cpp>", "-<socketcan_link_stub.cpp>"]
    )
if not _self_heal_links:
    env.Append(SRC_FILTER=["-<self_heal_link.cpp>"])  # noqa: F821

_cfg_dir = env.subst(os.path.join("$BUILD_DIR", "libtracer_config"))  # noqa: F821
_cfg_path = os.path.join(_cfg_dir, "libtracer", "config_override.hpp")
_bus = "true" if _bus_links else "false"
_heal = "true" if _self_heal_links else "false"
_cfg_text = f"""/**
 * @file
 * @brief GENERATED by integrations/platformio/pio_esp32_can.py — this build's link modules
 *        (#1670) from `custom_libtracer_bus_links` / `custom_libtracer_self_heal_links`.
 */
#pragma once

#if defined(__has_include_next)
#if __has_include_next(<libtracer/config_override.hpp>)
#define LIBTRACER_PIO_FRAGMENT_YIELDS 1
#endif
#endif

#if defined(LIBTRACER_PIO_FRAGMENT_YIELDS)
#include_next <libtracer/config_override.hpp>
static_assert(tr::graph::config_t::kBusLinks == {_bus},
              "libtracer: your config_override.hpp binds kBusLinks differently from "
              "platformio.ini's custom_libtracer_bus_links; make the two agree.");
static_assert(tr::graph::config_t::kSelfHealLinks == {_heal},
              "libtracer: your config_override.hpp binds kSelfHealLinks differently from "
              "platformio.ini's custom_libtracer_self_heal_links; make the two agree.");
#else
namespace tr::graph {{

/** @brief The defaults, with the link modules platformio.ini opted in to. */
struct pio_config_t : default_config_t {{
    static constexpr bool kBusLinks = {_bus};
    static constexpr bool kSelfHealLinks = {_heal};
}};

using config_t = pio_config_t;

}}  // namespace tr::graph
#endif
"""
os.makedirs(os.path.dirname(_cfg_path), exist_ok=True)
_cfg_prev = None
if os.path.isfile(_cfg_path):
    with open(_cfg_path, encoding="utf-8") as _f:
        _cfg_prev = _f.read()
if _cfg_prev != _cfg_text:  # write only on change, so an unchanged build rebuilds nothing
    with open(_cfg_path, "w", encoding="utf-8") as _f:
        _f.write(_cfg_text)
# Library env AND the consumer's: every TU of the image must resolve the same fragment.
env.PrependUnique(CPPPATH=[_cfg_dir])  # noqa: F821
DefaultEnvironment().PrependUnique(CPPPATH=[_cfg_dir])  # noqa: F821

# Only the Espressif 32 platform ships the TWAI + FreeRTOS headers twai_link needs.
if env.get("PIOPLATFORM", "") == "espressif32":
    # #984 (the #947 ruling's PlatformIO half): the portable POSIX-socket WS pair must
    # not reach silicon. On lwIP its egress path is unusable — `write_all_iov` sends
    # with MSG_NOSIGNAL, `lwip_sendmsg` rejects that flag with EOPNOTSUPP, the helper
    # reads it as peer-gone, and every scatter-gather data frame silently vanishes
    # while the handshake and PING/PONG still work (#948). Exclude the pair, and with
    # it core's hand-written full-node dispatcher whose register_ws_transport call is
    # the reference that keeps the WS objects reachable past --gc-sections; the
    # udp+tcp replacement dispatcher is compiled below. Selection by which TU
    # compiles — the no-feature-macro doctrine — same as the ESP-IDF component's
    # CMake does it. Net effect, documented in this directory's README: a PlatformIO
    # espressif32 build ships NO WebSocket transport (the IDF-native links are not
    # packaged here — see #984 for the follow-up that could add them). Gated by the
    # `pio-esp32-can` workflow via tools/check_esp_ws_plane.py --ws-plane none on the
    # linked fixture image.
    env.Append(  # noqa: F821
        SRC_FILTER=[
            "-<transport_ws.cpp>",
            "-<builtin_transport_ws.cpp>",
            "-<builtin_transports.cpp>",
        ]
    )
    # PlatformIO exec()s a library extraScript as an SCons SConscript, and SCons
    # runs it against SConscript globals where `__file__` is NOT defined — deriving
    # the package root from it raises NameError and fails the whole build. The
    # exported library builder carries that root directly (LibBuilderBase.path,
    # which piolib.py builds with os.path.abspath — absolute, but NOT symlink-
    # resolved; do not rely on it being canonical), so take it from there and
    # do no dirname arithmetic at all. twai_link lives in the ESP-IDF component
    # tree, not core/.
    _pkg = pio_lib_builder.path  # noqa: F821
    _twai_dir = os.path.join(_pkg, "integrations", "esp-idf", "libtracer")
    _twai_src = os.path.join(_twai_dir, "twai_link.cpp")

    # Same POSIX-spelling shim the ESP-IDF component puts on its PRIV_INCLUDE_DIRS:
    # IDF's libc declares poll() only under <sys/poll.h>, and core/src/posix_endpoint.cpp
    # (always in this package's srcFilter, and pulled in by the udp/tcp transports)
    # includes the neutral <poll.h>. Without it the package does not compile on a chip
    # target AT ALL, TWAI or not — so it is applied next to the platform check rather
    # than inside the twai_link guard below.
    _compat = os.path.join(_twai_dir, "compat", "include")
    if os.path.isdir(_compat):
        env.Append(CPPPATH=[_compat])

    # The udp+tcp register_builtin_transports dispatcher that replaces core's excluded
    # full-node (udp+tcp+ws) one — see the SRC_FILTER block above. Compiled the same
    # way twai_link.cpp is below, with the same ONCE-per-run guard on the shared
    # DefaultEnvironment (the espidf framework builder constructs library builders
    # twice, so an unguarded BuildSources double-links every symbol in the TU).
    _shared = DefaultEnvironment()  # noqa: F821  (an SCons SConscript global)
    if not _shared.get("LIBTRACER_PIO_DISPATCHER_ADDED"):
        _shared["LIBTRACER_PIO_DISPATCHER_ADDED"] = True
        env.BuildSources(
            os.path.join("$BUILD_DIR", "libtracer_pio_glue"),
            os.path.join(_pkg, "integrations", "platformio"),
            src_filter=["-<*>", "+<builtin_transports_udp_tcp.cpp>"],
        )

    # TWAI is a CAN link, so it rides the bus opt-in with transport_can.cpp (#1670).
    if _bus_links and os.path.isfile(_twai_src):
        # Expose libtracer_esp/twai_link.hpp and compile the one glue TU.
        env.Append(CPPPATH=[os.path.join(_twai_dir, "include")])
        # ONCE per `pio run`, not once per execution of this script. The espidf
        # framework builder constructs the library builders TWICE — once to collect
        # include dirs (after which it clears the __PIO_LIB_BUILDERS cache) and again
        # from BuildProgram — so PlatformIO runs a library extraScript twice on that
        # framework. env.BuildSources appends the compiled node to the SHARED
        # DefaultEnvironment PIOBUILDFILES list, so the second, unguarded call put
        # twai_link.o in the link line twice and every symbol in it came back as
        # "multiple definition". The marker has to live on that same DefaultEnvironment:
        # `env` here is a per-builder clone that does not survive to the second pass.
        _shared = DefaultEnvironment()  # noqa: F821  (an SCons SConscript global)
        if not _shared.get("LIBTRACER_TWAI_SOURCES_ADDED"):
            _shared["LIBTRACER_TWAI_SOURCES_ADDED"] = True
            env.BuildSources(
                os.path.join("$BUILD_DIR", "libtracer_twai"),
                _twai_dir,
                src_filter=["-<*>", "+<twai_link.cpp>"],
            )
