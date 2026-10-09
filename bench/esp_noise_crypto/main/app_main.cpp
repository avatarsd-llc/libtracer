/**
 * @file
 * @brief The ESP-IDF app of the Noise link harness's crypto arm (#2065): the NNpsk0 handshake
 *        and the per-frame ChaCha20-Poly1305 of `bench/noise_crypto.hpp` on the chip, over the
 *        mbedTLS PSA backend ESP-IDF ships.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Built for its image size: `idf.py size` of this app against its `none` build (every crypto
 * call compiled out) is the flash and static RAM the backend costs. Flashed, it also prints the
 * same `RESULT` rows `bench_noise_crypto` prints on the host (`esp_timer`, microsecond grain,
 * so the rows are means over a batch), plus the heap one session holds and the main task's
 * stack high-water. Before any row it runs a handshake and checks that both sides agree, so a
 * misbuilt backend prints `FATAL` and nothing else.
 */

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "noise_crypto.hpp"
#include "noise_crypto_backends.hpp"

namespace {

#if defined(LIBTRACER_BENCH_HAVE_PSA)
using backend_t = bench::noise::psa_backend_t;
using bench::noise::key32_t;

/** @brief A fixed key from @p seed (the host bench's keys are not needed bit for bit). */
key32_t fixed_key(std::uint8_t seed) {
    key32_t k{};
    for (std::size_t i = 0; i < k.size(); ++i) k[i] = static_cast<std::byte>(seed * 31 + i * 7);
    return k;
}

/** @brief Print one row in the host bench's 12-column shape; latency columns are the mean. */
void emit(const char* mode, std::size_t size, std::int64_t total_us, int n) {
    const double mean_ns = static_cast<double>(total_us) * 1000.0 / n;
    const double ops = mean_ns > 0 ? 1e9 / mean_ns : 0.0;
    std::printf("RESULT\tmbedtls-psa-esp\t%s\t%zu\t1\t1\t%.0f\t%.0f\t%.3f\t%.0f\t0\t%.0f\n", mode,
                size, ops, ops, ops * static_cast<double>(size) / 1e6, mean_ns, mean_ns);
}

/** @brief The rows: handshake, then seal/open per frame size, then RAM. */
void run() {
    if (!backend_t::init()) {
        std::printf("FATAL\tpsa_crypto_init\n");
        return;
    }
    const key32_t psk = fixed_key(1);
    bench::noise::transcript_t t{};
    if (!bench::noise::run_handshake<backend_t, backend_t>(psk, fixed_key(2), fixed_key(3), t)) {
        std::printf("FATAL\thandshake check\n");
        return;
    }

    constexpr int kHandshakes = 8;
    std::int64_t hs_us = 0;
    for (int i = 0; i < kHandshakes; ++i) {
        const std::int64_t a = esp_timer_get_time();
        (void)bench::noise::run_handshake<backend_t, backend_t>(psk, fixed_key(4 + i),
                                                                fixed_key(40 + i), t);
        hs_us += esp_timer_get_time() - a;
    }
    emit("crypto-hs-full", 0, hs_us, kHandshakes);

    for (const std::size_t size : {std::size_t{64}, std::size_t{1024}, std::size_t{4096},
                                   std::size_t{16384}, std::size_t{65491}}) {
        std::vector<std::byte> pt;
        std::vector<std::byte> ct;
        pt.resize(size);
        ct.resize(size + bench::noise::kTagLen);
        typename backend_t::aead_t aead;
        if (!aead.set_key(fixed_key(9))) return;
        const int n = size >= 16384 ? 20 : 200;
        std::int64_t a = esp_timer_get_time();
        for (int i = 0; i < n; ++i) (void)aead.seal(i, {}, pt, ct.data());
        emit("crypto-aead-seal", size, esp_timer_get_time() - a, n);
        a = esp_timer_get_time();
        for (int i = 0; i < n; ++i) (void)aead.open(n - 1, {}, ct, pt.data());
        emit("crypto-aead-open", size, esp_timer_get_time() - a, n);
    }

    const std::size_t before = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    {
        bench::noise::session_t<backend_t> s;
        (void)s.send.initialize_key(fixed_key(10));
        (void)s.recv.initialize_key(fixed_key(11));
        std::printf("NOISE_RAM\tmbedtls-psa-esp\tsession_heap_bytes\t%d\n",
                    static_cast<int>(before - heap_caps_get_free_size(MALLOC_CAP_DEFAULT)));
    }
    std::printf("NOISE_RAM\tmbedtls-psa-esp\tmain_task_stack_free_min\t%u\n",
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}
#else
/** @brief The baseline image: no crypto at all. */
void run() { std::printf("# esp_noise_crypto: baseline image, no crypto backend\n"); }
#endif

}  // namespace

extern "C" void app_main(void) {
    std::printf("# esp_noise_crypto (#2065)\n");
    run();
}
