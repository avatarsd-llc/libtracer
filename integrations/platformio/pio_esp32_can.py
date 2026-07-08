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
# On every other platform this is a NO-OP: twai_link needs ESP-IDF TWAI +
# FreeRTOS headers, which only exist on espressif32/espidf. The guard keeps this
# from affecting any non-esp32 PlatformIO build.
#
# BEST-EFFORT: the twai_link sources themselves are CI-built via the ESP-IDF
# managed component, but this PlatformIO glue is NOT yet verified on a physical
# board or in CI. Tracking: https://github.com/avatarsd-llc/libtracer/issues (see
# the "pio esp32 CAN verification" issue). If it does not pick up on your board,
# add twai_link.cpp to your project's src or use the ESP-IDF component directly.
import os

Import("env")  # noqa: F821  (env is injected by PlatformIO/SCons at build time)

# Only the Espressif 32 platform ships the TWAI + FreeRTOS headers twai_link needs.
if env.get("PIOPLATFORM", "") == "espressif32":
    # This script lives at <pkg>/integrations/platformio/; the package root is
    # three levels up. twai_link lives in the ESP-IDF component tree, not core/.
    _pkg = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    _twai_dir = os.path.join(_pkg, "integrations", "esp-idf", "libtracer")
    _twai_src = os.path.join(_twai_dir, "twai_link.cpp")
    if os.path.isfile(_twai_src):
        # Expose libtracer_esp/twai_link.hpp and compile the one glue TU.
        env.Append(CPPPATH=[os.path.join(_twai_dir, "include")])
        env.BuildSources(
            os.path.join("$BUILD_DIR", "libtracer_twai"),
            _twai_dir,
            src_filter=["-<*>", "+<twai_link.cpp>"],
        )
