/**
 * @file
 * @brief Downstream PlatformIO consumer that exercises the `espressif32` TWAI
 *        packaging glue — a COMPILE+LINK fixture, no hardware.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This is the consumer half of the `pio-esp32-can` CI job (see the sibling
 * platformio.ini and .github/workflows/pio-esp32-can.yml). It names exactly the
 * three things `integrations/platformio/pio_esp32_can.py` has to make reachable
 * on an `espressif32` + `framework = espidf` target:
 *
 *  - `#include "libtracer_esp/twai_link.hpp"` — the header the hook exposes by
 *    appending the ESP-IDF component's `include/` to `CPPPATH`. A broken
 *    `extraScript` path (the hook not running, or `_pkg` resolving to the wrong
 *    directory) fails this translation unit at COMPILE time.
 *  - constructing `tr::net::twai_link_t` — its out-of-line constructor and
 *    destructor live in `twai_link.cpp`, the one out-of-tree TU the hook feeds
 *    to `env.BuildSources`. If that call stops contributing an object file to
 *    the program, this fails at LINK time with an undefined reference.
 *  - handing the link to a CLASSIC `tr::net::transport_can` and registering
 *    `tr::net::can_transport_factory()` — the shape the hook's own file comment
 *    tells a consumer to write, so the packaged `srcFilter` has to carry the
 *    portable CAN plane (`transport_can.cpp` + `socketcan_link_stub.cpp`) too.
 *
 * Nothing here runs on silicon: `app_main` calls `build_can_stack()` only
 * behind a `volatile` flag that is never set, so the TWAI controller is never
 * brought up. The flag is `volatile` precisely so the compiler cannot fold the
 * branch away and drop the references — the link step is the assertion.
 */
#include <cstdio>
#include <memory>
#include <utility>

#include "libtracer/builtin_transports.hpp"
#include "libtracer/transport_can.hpp"
#include "libtracer/transport_vertex.hpp"
#include "libtracer_esp/twai_link.hpp"

namespace {

/**
 * @brief Never set. `volatile` so the compiler must keep the guarded call —
 *        and therefore every symbol it reaches — in the linked image.
 */
volatile bool g_run_hardware = false;

/**
 * @brief Build the ESP32 CAN stack the PlatformIO hook is supposed to enable.
 *
 * Never invoked (see @ref g_run_hardware); its purpose is to make the compiler
 * and linker resolve the TWAI link, the CLASSIC `transport_can` over it, and
 * the catalog factory.
 */
void build_can_stack() {
    tr::net::twai_link_config_t link_cfg{};
    link_cfg.tx_gpio = 4;
    link_cfg.rx_gpio = 5;
    link_cfg.bitrate = 500000;

    auto link = std::make_unique<tr::net::twai_link_t>(link_cfg);
    const bool link_ok = link->ok();

    // TWAI is classic-only, so the transport above it must be CLASSIC framing.
    tr::net::transport_can_config_t can_cfg{};
    can_cfg.node = 1;
    can_cfg.mode = tr::view::can_frame_mode_t::CLASSIC;
    tr::net::transport_can can{std::move(link), can_cfg};

    // The ADR-0027 catalog entry a deployed node registers.
    tr::net::transport_vertex_t::transport_factory_t factory = tr::net::can_transport_factory();

    std::printf("twai link ok=%d transport=%p factory=%d\n", static_cast<int>(link_ok),
                static_cast<const void*>(&can), static_cast<int>(static_cast<bool>(factory)));
}

}  // namespace

/**
 * @brief ESP-IDF entry point. Reports that the image linked; never touches the bus.
 */
extern "C" void app_main(void) {
    if (g_run_hardware) build_can_stack();
    // #984's gate half: name register_builtin_transports, the dispatcher the hook
    // swaps on espressif32 (core's full-node udp+tcp+ws form is filtered out; the
    // udp+tcp-only integrations/platformio/builtin_transports_udp_tcp.cpp replaces
    // it). Taking the address forces the linker to resolve the definition and every
    // register_*_transport it calls — before the swap that dragged the portable
    // transport_ws pair into this image (the before-figure check_esp_ws_plane.py's
    // --ws-plane none run is measured against); after it, an undefined reference
    // here means the hook failed to compile the replacement TU. Without this line
    // nothing in the fixture reached the built-in catalog and --gc-sections alone
    // could have made the symbol gate pass vacuously.
    std::printf("builtin dispatcher=%p\n",
                reinterpret_cast<void*>(&tr::net::register_builtin_transports));
    std::printf("libtracer PlatformIO espressif32 TWAI packaging fixture: linked\n");
}
