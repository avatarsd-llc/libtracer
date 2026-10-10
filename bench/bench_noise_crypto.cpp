/**
 * @file
 * @brief The Noise link harness's CRYPTO arm (#2065): what a
 *        `Noise_NNpsk0_25519_ChaChaPoly_SHA256` session costs per frame and per handshake, on
 *        every crypto backend this build found, with the RAM each one holds.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The Noise link binding (RFC-0033 in #2063, implemented by #2064) adds two costs to a datagram
 * link: a handshake per session and a ChaCha20-Poly1305 seal and open per frame. This bench
 * prices both on each candidate backend before the binding picks one, so the choice is made on
 * numbers: latency, throughput, heap and stack. Flash is measured on ESP-IDF by the companion
 * app in `bench/esp_noise_crypto/`, since a host shared library's size says nothing about an
 * MCU image.
 *
 * @section checks Checks before any number
 *
 * The run refuses to publish a row unless every compiled backend first:
 *  - reproduces the RFC 7748 §6.1 X25519 vector (both public keys and the shared secret);
 *  - produces a byte-identical handshake transcript (both messages, the final `h` and the
 *    first sealed transport frame) to every other backend, from the same PSK and ephemerals;
 *  - completes a handshake and a frame each way with every other backend on the far side.
 * Three independent implementations agreeing on every byte is what stands in for an external
 * Noise test vector until RFC-0033 ships its own (#2063). A failed check prints `FATAL` and
 * exits 1 with no rows.
 *
 * @section rows Rows
 *
 * `RESULT` (and `RESULT_TAIL`) rows, `system` = the backend, `fanout` = `endpoints` = 1:
 *  - `crypto-aead-seal` / `crypto-aead-open` — one frame's seal or open at 64 B, 1 KiB, 4 KiB,
 *    16 KiB and 65491 B (the IPv4 UDP datagram bound, 65507 B, less the 16-byte tag);
 *    `pub/s` = frames/s, `MB/s` = plaintext megabytes/s;
 *  - `crypto-x25519` — one X25519 agreement; `crypto-x25519-pub` — import a private key and
 *    derive its public key (the `e` token's cost without the RNG); `crypto-hkdf3` — one
 *    3-output Noise HKDF (the `psk` token);
 *  - `crypto-hs-init` / `crypto-hs-resp` — one side's whole handshake, `Initialize` through
 *    `Split`; `crypto-hs-full` — both sides back to back, the CPU a loopback handshake costs.
 *
 * `NOISE_RAM backend metric value` rows (tab-separated):
 *  - `session_object_bytes` — `sizeof` one side's transport session (two cipher states);
 *  - `session_heap_bytes` — heap one side's session holds after `Split`;
 *  - `hs_heap_peak_init` / `hs_heap_peak_resp`, `hs_allocs_init` / `hs_allocs_resp` — heap
 *    high-water and allocation calls of one side's handshake;
 *  - `frame_allocs_x1000` — allocation calls per 1000 frames (a 1 KiB seal plus open);
 *  - `hs_stack_peak` / `frame_stack_peak` — stack high-water of a whole handshake and of a
 *    16 KiB seal plus open, measured on a painted stack, less an empty thread's own use.
 *
 * @section honest What this is NOT
 *
 * One thread, host CPU, warm caches: the per-frame rows are the floor a link pays, not a link.
 * The ephemerals are injected (see `noise_crypto.hpp`), so the RNG is in no row. The p99 of a
 * sub-microsecond row carries the clock's own cost (the `CLOCK` line says how much).
 *
 *     bench_noise_crypto            # every row
 *     bench_noise_crypto --quick    # a tenth of the time budget (a smoke run)
 *     bench_noise_crypto --backend=libsodium   # one backend's rows (the checks still run all)
 *     bench_noise_crypto --list     # the compiled backends' names
 *     LIBTRACER_BENCH_SECONDS=0.5   # time budget per row (default 0.25 s)
 */

#include <pthread.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "bench_common.hpp"
#include "malloc_probe.hpp"
#include "noise_crypto.hpp"
#include "noise_crypto_backends.hpp"

namespace {

using bench::Latency;
using bench::now_ns;
using bench::noise::key32_t;

/** @brief The frame sizes every AEAD row runs: the ticket's ladder plus the datagram bound. */
constexpr std::size_t kFrameSizes[] = {64, 1024, 4096, 16384, 65507 - bench::noise::kTagLen};

/** @brief Fewest samples a row takes, whatever its budget. */
constexpr std::size_t kMinSamples = 1000;

/** @brief Most samples a row takes (the reservation). */
constexpr std::size_t kMaxSamples = 400000;

/** @brief Distinct ephemeral keys the handshake rows rotate through. */
constexpr std::size_t kEphemerals = 64;

/** @brief Pre-sealed frames an open row cycles through, each under its own nonce. */
constexpr std::size_t kOpenRing = 32;

/** @brief Time budget per row in seconds (`LIBTRACER_BENCH_SECONDS`, `--quick` divides by 10). */
double g_budget_s = 0.25;

/** @brief Call @p f with each compiled-in backend type, in a fixed order. */
template <class F>
void for_each_backend(F&& f) {
#if defined(LIBTRACER_BENCH_HAVE_SODIUM)
    f.template operator()<bench::noise::sodium_backend_t>();
#endif
#if defined(LIBTRACER_BENCH_HAVE_OPENSSL)
    f.template operator()<bench::noise::openssl_backend_t>();
#endif
#if defined(LIBTRACER_BENCH_HAVE_PSA)
    f.template operator()<bench::noise::psa_backend_t>();
#endif
    (void)f;
}

/** @brief Parse a lowercase hex string of exactly 64 digits into a key. */
[[nodiscard]] key32_t from_hex(std::string_view hex) {
    key32_t k{};
    const auto nib = [](char c) {
        return static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
    };
    for (std::size_t i = 0; i < k.size(); ++i)
        k[i] = static_cast<std::byte>((nib(hex[2 * i]) << 4) | nib(hex[2 * i + 1]));
    return k;
}

/** @brief A fixed, well-spread key: SplitMix64 from @p seed. Deterministic on purpose. */
[[nodiscard]] key32_t fixed_key(std::uint64_t seed) {
    key32_t k{};
    for (std::size_t i = 0; i < k.size(); i += 8) {
        seed += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = seed;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z ^= z >> 31;
        for (std::size_t j = 0; j < 8; ++j)
            k[i + j] = static_cast<std::byte>((z >> (8 * j)) & 0xFF);
    }
    return k;
}

/** @brief The PSK every handshake row uses. */
const key32_t kPsk = fixed_key(0x2065);

/** @brief Report a failed pre-check and stop the run with no rows. */
[[noreturn]] void fatal(const char* backend, const char* what) {
    std::printf("FATAL\t%s\t%s\n", backend, what);
    std::fflush(stdout);
    std::exit(1);
}

/** @brief RFC 7748 §6.1: Alice's and Bob's keys and their shared secret. */
template <class B>
void check_x25519_vector() {
    const key32_t a_priv =
        from_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const key32_t a_pub =
        from_hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    const key32_t b_priv =
        from_hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    const key32_t b_pub =
        from_hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    const key32_t shared =
        from_hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    typename B::dh_key_t a;
    typename B::dh_key_t b;
    key32_t pa{};
    key32_t pb{};
    key32_t sa{};
    key32_t sb{};
    if (!a.set(a_priv, pa) || pa != a_pub) fatal(B::kName, "rfc7748 alice public key");
    if (!b.set(b_priv, pb) || pb != b_pub) fatal(B::kName, "rfc7748 bob public key");
    if (!a.agree(b_pub, sa) || sa != shared) fatal(B::kName, "rfc7748 alice shared secret");
    if (!b.agree(a_pub, sb) || sb != shared) fatal(B::kName, "rfc7748 bob shared secret");
}

/** @brief Every backend's transcript must equal the first one's; every pair must interoperate. */
void check_agreement() {
    bool have_ref = false;
    bench::noise::transcript_t ref{};
    const key32_t ie = fixed_key(1);
    const key32_t re = fixed_key(2);
    for_each_backend([&]<class B>() {
        bench::noise::transcript_t t{};
        if (!bench::noise::run_handshake<B, B>(kPsk, ie, re, t)) fatal(B::kName, "self handshake");
        if (t.msg1_len != 48 || t.msg2_len != 48) fatal(B::kName, "handshake message length");
        if (!have_ref) {
            ref = t;
            have_ref = true;
            return;
        }
        if (t.msg1 != ref.msg1 || t.msg2 != ref.msg2) fatal(B::kName, "transcript differs");
        if (t.h != ref.h) fatal(B::kName, "handshake hash differs");
        if (t.first_frame != ref.first_frame) fatal(B::kName, "first transport frame differs");
    });
    for_each_backend([&]<class I>() {
        for_each_backend([&]<class R>() {
            bench::noise::transcript_t t{};
            if (!bench::noise::run_handshake<I, R>(kPsk, ie, re, t))
                fatal(I::kName, "cross-backend handshake failed");
        });
    });
}

/**
 * @brief Sample @p op until the row's budget is spent (at least @ref kMinSamples, at most
 *        @ref kMaxSamples), one clock pair per call.
 */
template <class Op>
[[nodiscard]] Latency::Summary sample(Op&& op) {
    for (int i = 0; i < 64; ++i) op();  // warm-up: caches, key schedules, lazy init
    Latency lat;
    lat.reserve(kMaxSamples);
    const std::uint64_t budget = static_cast<std::uint64_t>(g_budget_s * 1e9);
    const std::uint64_t t0 = now_ns();
    std::size_t n = 0;
    while (n < kMinSamples || (n < kMaxSamples && now_ns() - t0 < budget)) {
        const std::uint64_t a = now_ns();
        op();
        lat.add(now_ns() - a);
        ++n;
    }
    return lat.summarize();
}

/** @brief Emit one timing row and its tail; rates come from the mean. */
void emit_row(const char* backend, const char* mode, std::size_t size, const Latency::Summary& s) {
    const double ops = s.mean > 0 ? 1e9 / static_cast<double>(s.mean) : 0.0;
    bench::emit(backend, mode, size, 1, 1, ops, ops, ops * static_cast<double>(size) / 1e6, s);
    bench::emit_tail(backend, mode, size, 1, 1, s);
}

/** @brief The AEAD rows: seal and open at every frame size. */
template <class B>
void aead_rows() {
    for (const std::size_t size : kFrameSizes) {
        std::vector<std::byte> pt(size, std::byte{0x5A});
        std::vector<std::byte> ct(size + bench::noise::kTagLen);
        typename B::aead_t aead;
        if (!aead.set_key(fixed_key(3))) fatal(B::kName, "aead key");
        std::uint64_t n = 0;
        bool ok = true;
        const auto seal = sample([&] { ok &= aead.seal(n++, {}, pt, ct.data()); });
        if (!ok) fatal(B::kName, "seal failed");
        emit_row(B::kName, "crypto-aead-seal", size, seal);

        std::vector<std::vector<std::byte>> ring(kOpenRing, std::vector<std::byte>(ct.size()));
        for (std::size_t i = 0; i < kOpenRing; ++i)
            if (!aead.seal(i, {}, pt, ring[i].data())) fatal(B::kName, "seal for open ring");
        std::vector<std::byte> back(size);
        std::size_t k = 0;
        const auto open = sample([&] {
            const std::size_t i = k++ % kOpenRing;
            ok &= aead.open(i, {}, ring[i], back.data());
        });
        if (!ok || back != pt) fatal(B::kName, "open failed");
        emit_row(B::kName, "crypto-aead-open", size, open);
    }
}

/** @brief The primitive rows the handshake is made of. */
template <class B>
void primitive_rows() {
    std::vector<key32_t> eph(kEphemerals);
    for (std::size_t i = 0; i < kEphemerals; ++i) eph[i] = fixed_key(100 + i);
    std::vector<key32_t> pubs(kEphemerals);
    {
        typename B::dh_key_t k;
        for (std::size_t i = 0; i < kEphemerals; ++i)
            if (!k.set(eph[i], pubs[i])) fatal(B::kName, "x25519 public key");
    }
    bool ok = true;
    std::size_t i = 0;
    key32_t pub{};
    const auto pub_row = sample([&] {
        typename B::dh_key_t k;
        ok &= k.set(eph[i++ % kEphemerals], pub);
    });
    emit_row(B::kName, "crypto-x25519-pub", 32, pub_row);

    typename B::dh_key_t mine;
    if (!mine.set(fixed_key(7), pub)) fatal(B::kName, "x25519 key");
    key32_t shared{};
    const auto dh_row = sample([&] { ok &= mine.agree(pubs[i++ % kEphemerals], shared); });
    emit_row(B::kName, "crypto-x25519", 32, dh_row);

    std::array<std::byte, 3 * bench::noise::kHashLen> out{};
    key32_t ck = fixed_key(8);
    const auto hkdf_row = sample([&] {
        ok &= B::hkdf(ck, kPsk, out);
        ck[0] = out[0];  // chain, so no call can be hoisted
    });
    emit_row(B::kName, "crypto-hkdf3", 96, hkdf_row);
    if (!ok) fatal(B::kName, "primitive failed");
}

/** @brief The handshake rows: each side's handshake, and both back to back. */
template <class B>
void handshake_rows() {
    std::vector<key32_t> eph(kEphemerals);
    for (std::size_t i = 0; i < kEphemerals; ++i) eph[i] = fixed_key(1000 + i);
    Latency init_lat;
    Latency resp_lat;
    Latency full_lat;
    init_lat.reserve(kMaxSamples);
    resp_lat.reserve(kMaxSamples);
    full_lat.reserve(kMaxSamples);
    const std::uint64_t budget = static_cast<std::uint64_t>(g_budget_s * 4 * 1e9);
    const std::uint64_t t0 = now_ns();
    std::array<std::byte, bench::noise::kHandshakeMsgMax> m1{};
    std::array<std::byte, bench::noise::kHandshakeMsgMax> m2{};
    std::array<std::byte, bench::noise::kMaxHandshakePayload> scratch{};
    for (std::size_t n = 0; n < 16 + kMinSamples / 4 || (n < kMaxSamples && now_ns() - t0 < budget);
         ++n) {
        bench::noise::handshake_t<B> init;
        bench::noise::handshake_t<B> resp;
        bench::noise::session_t<B> a;
        bench::noise::session_t<B> b;
        const std::uint64_t s0 = now_ns();
        const bool i1 = init.start(bench::noise::role_t::INITIATOR, kPsk, eph[n % kEphemerals]);
        const std::size_t l1 = init.write_msg1({}, m1.data());
        const std::uint64_t s1 = now_ns();
        const bool r1 =
            resp.start(bench::noise::role_t::RESPONDER, kPsk, eph[(n + 1) % kEphemerals]);
        const std::size_t r_read = resp.read_msg1(std::span(m1.data(), l1), scratch.data());
        const std::size_t l2 = resp.write_msg2({}, m2.data());
        const bool r2 = resp.split(b.send, b.recv);
        const std::uint64_t s2 = now_ns();
        const std::size_t i_read = init.read_msg2(std::span(m2.data(), l2), scratch.data());
        const bool i2 = init.split(a.send, a.recv);
        const std::uint64_t s3 = now_ns();
        if (!i1 || l1 == 0 || !r1 || r_read == 0 || l2 == 0 || !r2 || i_read == 0 || !i2)
            fatal(B::kName, "handshake failed");
        if (n < 16) continue;  // warm-up
        init_lat.add((s1 - s0) + (s3 - s2));
        resp_lat.add(s2 - s1);
        full_lat.add(s3 - s0);
    }
    emit_row(B::kName, "crypto-hs-init", 0, init_lat.summarize());
    emit_row(B::kName, "crypto-hs-resp", 0, resp_lat.summarize());
    emit_row(B::kName, "crypto-hs-full", 0, full_lat.summarize());
}

/** @brief What the painted-stack thread is handed and hands back. */
template <class F>
struct painted_run_t {
    F* f = nullptr;               /**< @brief The work. */
    unsigned char* mem = nullptr; /**< @brief The painted stack, lowest address first. */
    std::size_t size = 0;         /**< @brief Its bytes. */
    std::size_t used = 0;         /**< @brief Bytes below the top the work touched. */
};

/**
 * @brief Run @p f on a thread whose stack is painted first; return the bytes it touched.
 *
 * The scan runs ON that thread, right after @p f returns and before the thread exits, so the
 * exit path (which on glibc reaches deeper than a small operation) never overwrites the mark,
 * and depth is counted from the entry frame. The caller subtracts an empty run's figure.
 */
template <class F>
[[nodiscard]] std::size_t painted_stack_use(F&& f) {
    using fn_t = std::remove_reference_t<F>;
    constexpr std::size_t kStack = 512 * 1024;
    constexpr unsigned char kPaint = 0xA5;
    auto* mem = static_cast<unsigned char*>(std::aligned_alloc(4096, kStack));
    if (mem == nullptr) return 0;
    std::memset(mem, kPaint, kStack);
    painted_run_t<fn_t> run{&f, mem, kStack, 0};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, mem, kStack);
    pthread_t t;
    auto trampoline = [](void* p) -> void* {
        auto* r = static_cast<painted_run_t<fn_t>*>(p);
        // Thread start-up can reach deeper than the call into f, so the paint below this frame
        // is renewed here, by a loop in this frame, and depth is counted from this frame.
        auto* top = static_cast<unsigned char*>(__builtin_frame_address(0)) - 256;
        for (volatile unsigned char* q = r->mem; q < top; ++q) *q = kPaint;
        (*r->f)();
        std::size_t untouched = 0;
        while (untouched < r->size && r->mem[untouched] == kPaint) ++untouched;
        r->used = static_cast<std::size_t>(top - (r->mem + untouched)) + 256;
        return nullptr;
    };
    if (pthread_create(&t, &attr, trampoline, &run) == 0) pthread_join(t, nullptr);
    pthread_attr_destroy(&attr);
    std::free(mem);
    return run.used;
}

/** @brief Emit one `NOISE_RAM` row. */
void emit_ram(const char* backend, const char* metric, long long value) {
    std::printf("NOISE_RAM\t%s\t%s\t%lld\n", backend, metric, value);
    std::fflush(stdout);
}

/** @brief The RAM rows: session size, handshake heap and allocations, stack high-water. */
template <class B>
void ram_rows() {
    namespace mp = bench::malloc_probe;
    emit_ram(B::kName, "session_object_bytes",
             static_cast<long long>(sizeof(bench::noise::session_t<B>)));
    std::array<std::byte, bench::noise::kHandshakeMsgMax> m1{};
    std::array<std::byte, bench::noise::kHandshakeMsgMax> m2{};
    std::array<std::byte, bench::noise::kMaxHandshakePayload> scratch{};
    bool ok = true;
    {
        bench::noise::handshake_t<B> init;
        bench::noise::handshake_t<B> resp;
        bench::noise::session_t<B> a;
        bench::noise::session_t<B> b;
        // The initiator's two windows straddle the responder's; its peak is the larger of its
        // first window's and its second window's on top of what the first left live.
        mp::arm();
        ok &= init.start(bench::noise::role_t::INITIATOR, kPsk, fixed_key(11));
        const std::size_t l1 = init.write_msg1({}, m1.data());
        const mp::reading_t i1 = mp::disarm();
        mp::arm();
        ok &= resp.start(bench::noise::role_t::RESPONDER, kPsk, fixed_key(12));
        ok &= resp.read_msg1(std::span(m1.data(), l1), scratch.data()) != 0;
        const std::size_t l2 = resp.write_msg2({}, m2.data());
        ok &= resp.split(b.send, b.recv);
        const mp::reading_t r = mp::disarm();
        mp::arm();
        ok &= init.read_msg2(std::span(m2.data(), l2), scratch.data()) != 0;
        const mp::reading_t i2 = mp::disarm();
        mp::arm();
        ok &= init.split(a.send, a.recv);
        const mp::reading_t sess = mp::disarm();
        ok &= l1 != 0 && l2 != 0;
        emit_ram(B::kName, "session_heap_bytes", sess.live);
        emit_ram(B::kName, "hs_heap_peak_init",
                 std::max({i1.peak, i1.live + i2.peak, i1.live + i2.live + sess.peak}));
        emit_ram(B::kName, "hs_heap_peak_resp", r.peak);
        emit_ram(B::kName, "hs_allocs_init", i1.allocs + i2.allocs + sess.allocs);
        emit_ram(B::kName, "hs_allocs_resp", r.allocs);
    }
    {
        constexpr std::size_t kFrames = 1000;
        std::vector<std::byte> pt(1024, std::byte{0x11});
        std::vector<std::byte> ct(pt.size() + bench::noise::kTagLen);
        std::vector<std::byte> back(pt.size());
        typename B::aead_t aead;
        ok &= aead.set_key(fixed_key(13));
        ok &= aead.seal(0, {}, pt, ct.data()) && aead.open(0, {}, ct, back.data());  // warm
        mp::arm();
        for (std::size_t i = 0; i < kFrames; ++i)
            ok &= aead.seal(i + 1, {}, pt, ct.data()) && aead.open(i + 1, {}, ct, back.data());
        const mp::reading_t f = mp::disarm();
        emit_ram(B::kName, "frame_allocs_x1000", f.allocs);
    }
    // An empty run's figure (the entry frame's own share), subtracted from both rows.
    const std::size_t floor = painted_stack_use([] {});
    const std::size_t hs = painted_stack_use([&] {
        bench::noise::transcript_t t{};
        ok &= bench::noise::run_handshake<B, B>(kPsk, fixed_key(14), fixed_key(15), t);
    });
    const std::size_t frame = painted_stack_use([&] {
        std::vector<std::byte> pt(16384, std::byte{0x22});
        std::vector<std::byte> ct(pt.size() + bench::noise::kTagLen);
        typename B::aead_t aead;
        ok &= aead.set_key(fixed_key(16)) && aead.seal(0, {}, pt, ct.data()) &&
              aead.open(0, {}, ct, pt.data());
    });
    emit_ram(B::kName, "hs_stack_peak", static_cast<long long>(hs - std::min(hs, floor)));
    emit_ram(B::kName, "frame_stack_peak", static_cast<long long>(frame - std::min(frame, floor)));
    if (!ok) fatal(B::kName, "ram row work failed");
}

}  // namespace

int main(int argc, char** argv) {
    if (const char* env = std::getenv("LIBTRACER_BENCH_SECONDS"); env != nullptr) {
        const double v = std::strtod(env, nullptr);
        if (v > 0.0) g_budget_s = v;
    }
    std::string_view only;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if (a == "--quick") {
            g_budget_s /= 10.0;
        } else if (a.starts_with("--backend=")) {
            only = a.substr(10);
        } else if (a == "--list") {
            for_each_backend([]<class B>() { std::printf("%s\n", B::kName); });
            return 0;
        } else {
            // An unknown argument must refuse, never fall through to a different run (#1040).
            std::fprintf(stderr,
                         "bench_noise_crypto: unknown argument '%s' (--quick, --backend=<name>, "
                         "--list)\n",
                         argv[i]);
            return 2;
        }
    }
    // The heap rows are only as good as the probe: refuse to run on one that cannot tell a
    // window's own blocks from older ones it frees (#1420).
    if (bench::malloc_probe::kAvailable && !bench::malloc_probe::canary()) {
        std::fprintf(stderr, "FATAL bench_noise_crypto: malloc probe canary failed\n");
        return 1;
    }

    std::size_t compiled = 0;
    std::printf("# bench_noise_crypto: Noise_NNpsk0_25519_ChaChaPoly_SHA256 (#2065)\n");
    std::printf("# backends:");
    for_each_backend([&]<class B>() {
        std::printf(" %s", B::kName);
        ++compiled;
    });
    std::printf("%s | budget %.3f s per row | malloc probe %s\n", compiled == 0 ? " none" : "",
                g_budget_s, bench::malloc_probe::kAvailable ? "on" : "off");
    if (compiled == 0) {
        std::printf("# no crypto backend was found at configure time; nothing to measure\n");
        return 0;
    }
    bench::emit_clock_floor();

    for_each_backend([]<class B>() {
        if (!B::init()) fatal(B::kName, "library init");
        check_x25519_vector<B>();
    });
    check_agreement();
    std::printf("# checks: rfc7748 vector, byte-identical transcripts, cross-backend interop OK\n");

    for_each_backend([only]<class B>() {
        if (!only.empty() && only != B::kName) return;
        aead_rows<B>();
        primitive_rows<B>();
        handshake_rows<B>();
        ram_rows<B>();
    });
    return 0;
}
