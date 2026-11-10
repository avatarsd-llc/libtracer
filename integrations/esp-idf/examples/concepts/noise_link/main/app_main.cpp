/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT: a Noise link (RFC-0033) carries one request and its reply inside an
 *        NNpsk0 session over UDP, with every seam the application's own.
 *
 * Two links in one image talk over 127.0.0.1: a DIAL link (the initiator) and a LISTEN link
 * (the responder), each over its own `udp_transport_t`. The DIAL side sends one request; the
 * LISTEN side echoes it as the reply. The request is the session's confirmation and the reply
 * its acknowledgement, so the initiator reports `UP` after two round trips (reference/23 §4).
 *
 * The library holds no key, no entropy and no clock. The application gives it:
 *  - the key store (`resolve_psk`), here one compiled-in PSK. A real node keeps it in its own
 *    secure storage;
 *  - the random source (`ephemeral`), here PSA's, which ESP-IDF backs with the hardware RNG;
 *  - the initiator counter (`next_counter`), here a `boot_counter_t`. A real node raises and
 *    persists the boot number once per boot (in NVS, for example) before its first attempt;
 *  - the clock, as the `now` passed to `sweep`.
 *
 * Board-only step, not run here: to reach a host or a second board instead, give the DIAL link
 * that peer's address and run a LISTEN link with the same PSK there, once the board has joined a
 * network (Wi-Fi or Ethernet bring-up, which is the application's and not libtracer's).
 */

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/mem_source.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/transport_noise.hpp"
#include "libtracer/transport_udp.hpp"
#include "psa/crypto.h"
#include "sdkconfig.h"
#if !CONFIG_IDF_TARGET_LINUX
#include "esp_netif.h"
#endif

namespace {

namespace noise = tr::net::noise;

/** @brief An example PSK. Never ship a key in source: a node resolves it from its key store. */
constexpr std::array<std::uint8_t, 32> kExamplePsk = {
    0x6e, 0x6f, 0x69, 0x73, 0x65, 0x2d, 0x6c, 0x69, 0x6e, 0x6b, 0x2d, 0x65, 0x78, 0x61, 0x6d, 0x70,
    0x6c, 0x65, 0x2d, 0x6b, 0x65, 0x79, 0x2d, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};

/** @brief The application's seams (`noise::link_host`). */
struct app_host_t {
    noise::boot_counter_t counter{.boot = 1}; /**< @brief Boot 1: a real node persists it. */

    /** @brief The key store: one PSK, under one name. */
    bool resolve_psk(std::string_view ref, noise::key32_t& out) {
        if (ref != "example-psk") return false;
        std::memcpy(out.data(), kExamplePsk.data(), out.size());
        return true;
    }
    /** @brief A fresh ephemeral private key from PSA's random source. */
    bool ephemeral(noise::key32_t& out) {
        return psa_generate_random(reinterpret_cast<std::uint8_t*>(out.data()), out.size()) ==
               PSA_SUCCESS;
    }
    /** @brief Strictly increasing across attempts, and across reboots once `boot` persists. */
    std::uint64_t next_counter() { return counter.next(); }
};

/** @brief A Noise link over UDP, on the build's backend (PSA here). */
using link_t = tr::net::noise_link_t<noise::default_crypto_t, app_host_t, tr::net::udp_transport_t>;

/** @brief The inbound datagram cap: every receive block is this size (see one_link). */
constexpr std::size_t kMaxDatagram = 1024;

/** @brief The application's monotonic clock, in nanoseconds: the `now` every sweep takes. */
std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

/** @brief The links' config: retries from 20 ms up to 200 ms, a 2 s attempt. */
tr::net::noise_config_t noise_config() {
    return tr::net::noise_config_t{.psk_ref = "example-psk",
                                   .retry = 20'000'000,
                                   .retry_cap = 200'000'000,
                                   .attempt = 2'000'000'000,
                                   .pending_expiry = 2'000'000'000};
}

/** @brief The carrier's config: datagrams up to @ref kMaxDatagram. */
tr::net::udp_config_t udp_config() { return tr::net::udp_config_t{.max_frame = kMaxDatagram}; }

/** @brief Failed checks so far. */
int g_failures = 0;

/** @brief Print @p what with its verdict and count a failure. */
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

/** @brief Print the verdict; on the `linux` target also exit with it, so CI can run this. */
void finish() {
    std::printf("RESULT %s\n", g_failures == 0 ? "ok" : "FAIL");
#if CONFIG_IDF_TARGET_LINUX
    std::exit(g_failures == 0 ? 0 : 1);
#endif
}

/** @brief Sweep both links with the clock until @p done holds or 3 s pass. */
template <class Pred>
bool drive(link_t& a, link_t& b, Pred done) {
    const std::uint64_t start = now_ns();
    while (now_ns() - start < 3'000'000'000ULL) {
        const std::uint64_t now = now_ns();
        (void)a.sweep(now, 0);
        (void)b.sweep(now, 0);
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}

}  // namespace

extern "C" void app_main(void) {
#if !CONFIG_IDF_TARGET_LINUX
    ESP_ERROR_CHECK(esp_netif_init());  // starts lwIP; the links' sockets need it
#endif
    const psa_status_t init = psa_crypto_init();  // the links call it too; it is idempotent
    check(init == PSA_SUCCESS, "PSA crypto starts: the backend and the random source are ready");
    tr::mem::block_source_t& pool = tr::mem::net_source();

    // What a link's session state costs: a Noise link draws one block from `memory.state` (the
    // net sub-pool by default) beyond what its UDP carrier draws.
    std::size_t carrier_bytes = 0;
    {
        const std::size_t before = pool.stats().in_use;
        tr::net::udp_transport_t plain(0, {}, 0, udp_config());
        carrier_bytes = pool.stats().in_use - before;
    }
    app_host_t host_a;
    app_host_t host_b;
    const std::size_t before = pool.stats().in_use;
    link_t b(tr::net::conn_role_t::LISTEN, host_b, noise_config(), std::uint16_t{0},
             std::string_view{}, std::uint16_t{0}, udp_config());
    const std::size_t link_bytes = pool.stats().in_use - before;
    link_t a(tr::net::conn_role_t::DIAL, host_a, noise_config(), std::uint16_t{0},
             std::string_view{"127.0.0.1"}, b.carrier().local_port(), udp_config());
    check(a.ok() && b.ok(), "both links came up: PSK resolved, state drawn, carriers bound");
    std::printf("per link: %zu B object, %zu B session state (one block for both sessions)\n",
                sizeof(link_t), link_bytes - carrier_bytes);

    // The responder echoes each frame back as the reply; the initiator records it.
    auto echo = [&b](std::span<const std::byte> frame) { b.send(frame); };
    b.set_receiver(echo);
    std::atomic<bool> replied{false};
    auto on_reply = [&replied](std::span<const std::byte>) { replied.store(true); };
    a.set_receiver(on_reply);

    (void)a.sweep(now_ns(), 0);  // deadlines count from the last `now` a link was given
    (void)b.sweep(now_ns(), 0);
    const std::uint64_t t0 = now_ns();
    a.dial();
    check(drive(a, b, [&a] { return a.link_up(); }),
          "the initiator holds a session after one round trip");
    const std::uint64_t t1 = now_ns();
    std::printf("handshake, both ends in this image: %llu us\n",
                static_cast<unsigned long long>((t1 - t0) / 1000));
    check(a.state() == tr::net::link_state_t::DIALING, "not UP yet: the session is unacknowledged");

    std::vector<std::byte> request;
    const std::string_view text = "hello over Noise";
    tr::wire::emit_tlv(request, tr::wire::type_t::VALUE, tr::wire::opt_t{},
                       std::as_bytes(std::span(text)));
    a.send(request);  // the confirmation
    check(drive(a, b, [&replied] { return replied.load(); }),
          "the reply arrives (the acknowledgement)");
    check(a.state() == tr::net::link_state_t::UP, "the initiator is UP after two round trips");
    check(b.count(noise::counter_t::HANDSHAKE_FAILED) == 0 &&
              b.count(noise::counter_t::DECRYPT_FAILED) == 0,
          "nothing was refused");
    finish();
}
