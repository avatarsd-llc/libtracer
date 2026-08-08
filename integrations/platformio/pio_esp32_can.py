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
# CI-BUILT, NOT BUS-PROVEN: tests/packaging/pio_esp32_can is a `framework = espidf`
# consumer of the PACKED package that names twai_link_t, a CLASSIC transport_can over
# it, and can_transport_factory(); the `pio-esp32-can` workflow `pio run`s it, so each
# step below — this script running at all, both CPPPATH entries, the BuildSources call
# and its guard — is gated at COMPILE+LINK, and so is library.json's export list.
# That frames no CAN bit: moving frames on real silicon is still the open on-bus
# sign-off, and nothing in CI energises a pin.
import os

Import("env", "pio_lib_builder")  # noqa: F821  (injected by PlatformIO/SCons at build time)

# Only the Espressif 32 platform ships the TWAI + FreeRTOS headers twai_link needs.
if env.get("PIOPLATFORM", "") == "espressif32":
    # PlatformIO exec()s a library extraScript as an SCons SConscript, and SCons
    # runs it against SConscript globals where `__file__` is NOT defined — deriving
    # the package root from it raises NameError and fails the whole build. The
    # exported library builder carries that root directly (LibBuilderBase.path, an
    # absolute path with any symlink already resolved), so take it from there and
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

    if os.path.isfile(_twai_src):
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
