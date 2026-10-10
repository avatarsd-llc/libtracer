/**
 * @file
 * @brief The Noise link's crypto backend seam (#2072): the build's backend meets the
 *        contract, runs RFC-0033's NNpsk0 handshake and transport steps, refuses what the
 *        RFC says to refuse, and allocates nothing before the PSK is proven nor per frame.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Built only when `LIBTRACER_NOISE_CRYPTO` names a backend. It tests that backend
 * (`tr::net::noise::default_crypto_t`) and nothing else, so each CI leg proves its own
 * backend. The RFC-0033 Appendix A transcript is checked byte for byte by
 * `conformance_runner` from `tests/conformance/vectors/v1/noise/`; this suite covers the
 * primitives under it and the refusals around it.
 *
 * The allocation rows count every `malloc` family call in the process (glibc only, and not
 * under a sanitizer, which owns `malloc` itself), so a library's internal allocations are
 * counted as well as the module's own.
 */

#include "libtracer/security_noise.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "test_support.hpp"

using tr::testing::check;

namespace noise = tr::net::noise;
using crypto_t = noise::default_crypto_t;

/** @name Process-wide allocation counter (glibc interposition) */
/** @{ */
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
#define LT_NOISE_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define LT_NOISE_SANITIZED 1
#endif

#if defined(__GLIBC__) && !defined(LT_NOISE_SANITIZED)
/** @brief Whether this build counts allocations (glibc, no sanitizer). */
inline constexpr bool kCountsAllocations = true;
/** @brief Allocation calls since process start: `malloc`, `calloc`, `realloc`, aligned. */
static std::size_t g_allocations = 0;

extern "C" {
void* __libc_malloc(std::size_t);
void* __libc_calloc(std::size_t, std::size_t);
void* __libc_realloc(void*, std::size_t);
void* __libc_memalign(std::size_t, std::size_t);
void __libc_free(void*);

/** @brief Counting `malloc`. */
void* malloc(std::size_t n) {
    ++g_allocations;
    return __libc_malloc(n);
}
/** @brief Counting `calloc`. */
void* calloc(std::size_t c, std::size_t n) {
    ++g_allocations;
    return __libc_calloc(c, n);
}
/** @brief Counting `realloc`. */
void* realloc(void* p, std::size_t n) {
    ++g_allocations;
    return __libc_realloc(p, n);
}
/** @brief Counting `aligned_alloc`. */
void* aligned_alloc(std::size_t a, std::size_t n) {
    ++g_allocations;
    return __libc_memalign(a, n);
}
/** @brief Counting `posix_memalign`. */
int posix_memalign(void** out, std::size_t a, std::size_t n) {
    ++g_allocations;
    *out = __libc_memalign(a, n);
    return *out != nullptr ? 0 : 12;  // ENOMEM
}
/** @brief Plain `free`. */
void free(void* p) { __libc_free(p); }
}
#else
/** @brief Whether this build counts allocations (glibc, no sanitizer). */
inline constexpr bool kCountsAllocations = false;
/** @brief Allocation calls since process start (not counted on this build). */
static std::size_t g_allocations = 0;
#endif
/** @} */

namespace {

/** @brief Parse a hex literal into bytes (test inputs only; no validation). */
template <std::size_t N>
std::array<std::byte, N> hex(std::string_view s) {
    std::array<std::byte, N> out{};
    auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (std::size_t i = 0; i < N; ++i)
        out[i] = static_cast<std::byte>((nib(s[2 * i]) << 4) | nib(s[2 * i + 1]));
    return out;
}

/** @brief A 32-byte key whose byte i is `base + i` (the RFC-0033 Appendix A inputs). */
noise::key32_t ramp(int base) {
    noise::key32_t k{};
    for (std::size_t i = 0; i < k.size(); ++i) k[i] = static_cast<std::byte>(base + int(i));
    return k;
}

/** @brief Counts the backend's X25519 work, to show what runs before the PSK is proven. */
struct dh_census_t {
    static inline int sets = 0;   /**< @brief `dh_key_t::set` calls (key generation). */
    static inline int agrees = 0; /**< @brief `dh_key_t::agree` calls (Diffie-Hellman). */
};

/**
 * @brief The build's backend with its X25519 calls counted: a compile-time policy wrapping
 *        another, which is all an injected test seam needs to be.
 */
struct counting_crypto_t : crypto_t {
    /** @brief The wrapped key pair, counting each call. */
    struct dh_key_t : crypto_t::dh_key_t {
        /** @brief Counted `set`. */
        [[nodiscard]] bool set(const noise::key32_t& p, noise::key32_t& pub) {
            ++dh_census_t::sets;
            return crypto_t::dh_key_t::set(p, pub);
        }
        /** @brief Counted `agree`. */
        [[nodiscard]] bool agree(const noise::key32_t& peer, noise::key32_t& out) const {
            ++dh_census_t::agrees;
            return crypto_t::dh_key_t::agree(peer, out);
        }
    };
};
static_assert(noise::crypto_backend<counting_crypto_t>);

/** @brief One completed handshake: both sides' transport ciphers and their handshake hashes. */
template <class B>
struct pair_t {
    noise::transport_cipher_t<B> initiator; /**< @brief The initiator's session keys. */
    noise::transport_cipher_t<B> responder; /**< @brief The responder's session keys. */
    noise::key32_t h_initiator{};           /**< @brief The initiator's handshake hash. */
    noise::key32_t h_responder{};           /**< @brief The responder's handshake hash. */
    bool ok = false;                        /**< @brief Every step succeeded. */
};

/** @brief Run the NNpsk0 handshake end to end with Appendix A's inputs. */
template <class B>
void handshake(pair_t<B>& out) {
    noise::symmetric_state_t<B> base;
    const noise::key32_t psk = ramp(0);
    if (!noise::psk_state(psk, base)) return;
    typename B::aead_t hc;  // the link's handshake cipher, built once beside its PSK state
    noise::handshake_t<B> i(noise::role_t::INITIATOR, base, hc);
    noise::handshake_t<B> r(noise::role_t::RESPONDER, base, hc);
    std::array<std::byte, noise::kFirstMessageBytes> m1{};
    std::array<std::byte, noise::kSecondMessageBytes> m2{};
    const noise::first_payload_t p1{.flags = 0x01, .counter = (1ULL << 32) | 1};
    out.ok = i.write_first(ramp(32), p1, m1).has_value();
    const auto got1 = r.read_first(m1);
    out.ok = out.ok && got1 && got1->flags == p1.flags && got1->counter == p1.counter;
    out.ok = out.ok && r.write_second(ramp(64), 0x02, m2).has_value();
    const auto got2 = i.read_second(m2);
    out.ok = out.ok && got2 && *got2 == 0x02;
    out.ok = out.ok && i.split(out.initiator).has_value() && r.split(out.responder).has_value();
    out.h_initiator = i.symmetric_state().handshake_hash();
    out.h_responder = r.symmetric_state().handshake_hash();
}

/** @brief The primitives: SHA-256, X25519 (RFC 7748 §6.1) and the Noise nonce layout. */
void test_primitives() {
    std::printf("Primitives (the build's backend):\n");
    check(crypto_t::init(), "the backend initialises");

    noise::key32_t h{};
    const std::array abc{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    check(crypto_t::hash(abc, h) &&
              h == hex<32>("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "SHA-256(\"abc\") is the FIPS 180-2 value");

    const auto a_priv = hex<32>("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const auto b_priv = hex<32>("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    const auto a_pub = hex<32>("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    const auto b_pub = hex<32>("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    const auto shared = hex<32>("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    crypto_t::dh_key_t a;
    crypto_t::dh_key_t b;
    noise::key32_t pa{};
    noise::key32_t pb{};
    noise::key32_t sa{};
    noise::key32_t sb{};
    check(a.set(a_priv, pa) && pa == a_pub && b.set(b_priv, pb) && pb == b_pub,
          "X25519 public keys are RFC 7748 §6.1's");
    check(a.agree(pb, sa) && b.agree(pa, sb) && sa == shared && sb == shared,
          "X25519 shared secret is RFC 7748 §6.1's, from both sides");

    constexpr auto n = noise::chachapoly_nonce(0x0807060504030201ULL);
    check(n == hex<12>("000000000102030405060708"),
          "the Noise nonce is 32 zero bits then the counter little-endian");
}

/** @brief Both sides complete the handshake and reach the same hash and the same keys. */
void test_handshake_round_trip() {
    std::printf("Handshake round trip:\n");
    pair_t<crypto_t> p;
    handshake(p);
    check(p.ok, "the four handshake steps and both splits succeed");
    check(p.h_initiator == p.h_responder, "both sides reach the same handshake hash");

    std::array<std::byte, 64 + noise::kTransportOverhead> d{};
    const std::array frame{std::byte{0x01}, std::byte{0x00}, std::byte{0x04}, std::byte{0x00},
                           std::byte{'p'},  std::byte{'i'},  std::byte{'n'},  std::byte{'g'}};
    const auto n = p.initiator.seal(0, 1, frame, d);
    check(n && *n == frame.size() + noise::kTransportOverhead, "a frame seals to frame + 25 B");
    const auto hd = noise::parse_transport_header(std::span(d.data(), *n));
    check(hd && hd->phase == 0 && hd->nonce == 1, "its header reads back phase 0, nonce 1");
    const auto pt = p.responder.open(*hd, std::span(d.data(), *n));
    check(pt && std::equal(pt->begin(), pt->end(), frame.begin(), frame.end()) &&
              pt->data() == d.data() + noise::kTransportHeaderBytes,
          "the responder opens it in place to the same frame");
}

/** @brief Nothing a wrong or replayed first message reaches costs a Diffie-Hellman. */
void test_no_dh_before_psk_proof() {
    std::printf("No X25519 before the PSK is proven (RFC-0033 §5.8):\n");
    noise::symmetric_state_t<counting_crypto_t> good;
    noise::symmetric_state_t<counting_crypto_t> wrong;
    check(noise::psk_state(ramp(0), good) && noise::psk_state(ramp(1), wrong),
          "two PSK states build");
    crypto_t::aead_t hc;
    noise::handshake_t<counting_crypto_t> i(noise::role_t::INITIATOR, good, hc);
    std::array<std::byte, noise::kFirstMessageBytes> m1{};
    check(i.write_first(ramp(32), {.flags = 1, .counter = 7}, m1).has_value(),
          "the initiator writes its first message");

    dh_census_t::sets = dh_census_t::agrees = 0;
    noise::handshake_t<counting_crypto_t> r_wrong(noise::role_t::RESPONDER, wrong, hc);
    const auto refused = r_wrong.read_first(m1);
    check(!refused && refused.error() == noise::refusal_t::AUTH,
          "a first message under another PSK is refused as AUTH");
    noise::handshake_t<counting_crypto_t> r_good(noise::role_t::RESPONDER, good, hc);
    const auto accepted = r_good.read_first(m1);
    check(accepted && accepted->counter == 7, "the same message under the right PSK is read");
    check(dh_census_t::sets == 0 && dh_census_t::agrees == 0,
          "neither the refusal nor the accepted read ran any X25519 work");
    std::array<std::byte, noise::kSecondMessageBytes> m2{};
    check(r_good.write_second(ramp(64), 0x02, m2).has_value() && dh_census_t::sets == 1 &&
              dh_census_t::agrees == 1,
          "the answer runs exactly one key generation and one agreement");
}

/** @brief Sizes, type bytes, flags, tampering and the all-zero X25519 result are refused. */
void test_handshake_refusals() {
    std::printf("Handshake refusals:\n");
    noise::symmetric_state_t<crypto_t> base;
    check(noise::psk_state(ramp(0), base), "the PSK state builds");
    crypto_t::aead_t hc;
    noise::handshake_t<crypto_t> i(noise::role_t::INITIATOR, base, hc);
    std::array<std::byte, noise::kFirstMessageBytes> m1{};
    check(i.write_first(ramp(32), {.flags = 1, .counter = 1}, m1).has_value(),
          "a first message is written");

    const auto read1 = [&](std::span<const std::byte> d) {
        noise::handshake_t<crypto_t> r(noise::role_t::RESPONDER, base, hc);
        return r.read_first(d);
    };
    std::vector<std::byte> longer(m1.begin(), m1.end());
    longer.push_back(std::byte{0});
    check(read1(std::span(m1).first(57)).error() == noise::refusal_t::MALFORMED &&
              read1(longer).error() == noise::refusal_t::MALFORMED,
          "a first message of 57 or 59 bytes is MALFORMED");
    auto retyped = m1;
    retyped[0] = std::byte{0x02};
    check(read1(retyped).error() == noise::refusal_t::MALFORMED,
          "a first message with another type byte is MALFORMED");
    bool every_bit_refused = true;
    for (std::size_t byte = 1; byte < m1.size(); ++byte) {
        auto t = m1;
        t[byte] ^= std::byte{0x01};
        every_bit_refused = every_bit_refused && read1(t).error() == noise::refusal_t::AUTH;
    }
    check(every_bit_refused, "a flipped bit in the key, payload or tag of msg1 is AUTH");

    noise::handshake_t<crypto_t> bad_flags(noise::role_t::INITIATOR, base, hc);
    std::array<std::byte, noise::kFirstMessageBytes> mf{};
    check(bad_flags.write_first(ramp(32), {.flags = 0x02, .counter = 1}, mf).has_value() &&
              read1(mf).error() == noise::refusal_t::PAYLOAD,
          "an authentic first message with flag bits 7-1 set is PAYLOAD");

    // A low-order ephemeral key (u = 0), written by a holder of the PSK: the tag verifies,
    // and the responder must refuse at its Diffie-Hellman (§5.2).
    noise::symmetric_state_t<crypto_t> forger = base;
    std::array<std::byte, noise::kFirstMessageBytes> low{};
    low[0] = std::byte{0x01};
    const noise::key32_t zero{};
    const auto p = noise::encode_first_payload({.flags = 1, .counter = 9});
    check(forger.mix_hash(zero) && forger.mix_key(zero) &&
              forger.encrypt_and_hash(hc, p, low.data() + 1 + noise::kDhLen),
          "a first message with a low-order ephemeral key is forged under the PSK");
    noise::handshake_t<crypto_t> r(noise::role_t::RESPONDER, base, hc);
    std::array<std::byte, noise::kSecondMessageBytes> m2{};
    const auto read_low = r.read_first(low);
    const auto wrote = r.write_second(ramp(64), 0x02, m2);
    check(read_low.has_value() && !wrote && wrote.error() == noise::refusal_t::LOW_ORDER,
          "its tag verifies, and the all-zero X25519 result refuses the answer as LOW_ORDER");
    check(m2 == std::array<std::byte, noise::kSecondMessageBytes>{},
          "nothing is written for the refused answer");

    // The second message: a flipped bit anywhere is refused, and so are bad flags.
    noise::handshake_t<crypto_t> r2(noise::role_t::RESPONDER, base, hc);
    std::array<std::byte, noise::kSecondMessageBytes> good2{};
    check(r2.read_first(m1).has_value() && r2.write_second(ramp(64), 0x02, good2).has_value(),
          "an answer is written");
    bool every_bit_refused2 = true;
    for (std::size_t byte = 1; byte < good2.size(); ++byte) {
        noise::handshake_t<crypto_t> i2(noise::role_t::INITIATOR, base, hc);
        std::array<std::byte, noise::kFirstMessageBytes> again{};
        (void)i2.write_first(ramp(32), {.flags = 1, .counter = 1}, again);
        auto t = good2;
        t[byte] ^= std::byte{0x80};
        const auto got = i2.read_second(t);
        every_bit_refused2 =
            every_bit_refused2 && !got &&
            (got.error() == noise::refusal_t::AUTH || got.error() == noise::refusal_t::LOW_ORDER);
    }
    check(every_bit_refused2, "a flipped bit in the key, payload or tag of msg2 is refused");
    noise::handshake_t<crypto_t> r3(noise::role_t::RESPONDER, base, hc);
    noise::handshake_t<crypto_t> i3(noise::role_t::INITIATOR, base, hc);
    std::array<std::byte, noise::kFirstMessageBytes> m1b{};
    std::array<std::byte, noise::kSecondMessageBytes> m2b{};
    (void)i3.write_first(ramp(32), {.flags = 1, .counter = 1}, m1b);
    (void)r3.read_first(m1b);
    (void)r3.write_second(ramp(64), 0x04, m2b);
    check(i3.read_second(m2b).error() == noise::refusal_t::PAYLOAD,
          "an authentic second message with flag bits 7-2 set is PAYLOAD");

    noise::handshake_t<crypto_t> out_of_order(noise::role_t::RESPONDER, base, hc);
    check(out_of_order.write_second(ramp(64), 0x02, m2).error() == noise::refusal_t::STATE,
          "answering before reading a first message is STATE");
    noise::transport_cipher_t<crypto_t> unused;
    check(out_of_order.split(unused).error() == noise::refusal_t::STATE,
          "splitting before the handshake completes is STATE");
}

/** @brief Transport messages: layout, in place and out of place, tampering, the nonce bound. */
void test_transport() {
    std::printf("Transport messages:\n");
    pair_t<crypto_t> p;
    handshake(p);
    check(p.ok, "a session is established");

    const std::vector<std::byte> empty;
    std::array<std::byte, noise::kTransportOverhead> conf{};
    const auto n0 = p.initiator.seal(0, 0, empty, conf);
    const auto h0 = noise::parse_transport_header(conf);
    const auto e0 = p.responder.open(h0.value_or(noise::transport_header_t{}), conf);
    check(n0 && *n0 == 25 && h0 && e0 && e0->empty(),
          "an empty confirmation is 25 B and opens empty");

    // In place: the frame already sits at the datagram's payload offset.
    std::vector<std::byte> d(16384 + noise::kTransportOverhead);
    for (std::size_t k = 0; k < 16384; ++k)
        d[noise::kTransportHeaderBytes + k] = static_cast<std::byte>(k * 7);
    const std::vector<std::byte> want(d.begin() + noise::kTransportHeaderBytes,
                                      d.end() - noise::kTagLen);
    const auto sealed =
        p.responder.seal(1, 5, std::span(d).subspan(noise::kTransportHeaderBytes, 16384), d);
    check(sealed && *sealed == d.size() && d[0] == std::byte{0x05},
          "a 16 KiB frame seals in place under key phase 1 (type 0x05)");
    const auto hd = noise::parse_transport_header(d);
    const auto opened = p.initiator.open(hd.value_or(noise::transport_header_t{}), d);
    check(hd && opened && std::equal(opened->begin(), opened->end(), want.begin(), want.end()),
          "and opens in place to the same frame");

    // Tampering: the nonce, the ciphertext and the tag each fail the tag check.
    std::array<std::byte, 64 + noise::kTransportOverhead> g{};
    std::array<std::byte, 64> f{};
    f.fill(std::byte{0x5A});
    (void)p.initiator.seal(0, 3, f, g);
    bool all_refused = true;
    for (std::size_t byte = 1; byte < g.size(); ++byte) {
        auto t = g;
        t[byte] ^= std::byte{0x10};
        const auto th = noise::parse_transport_header(t);
        all_refused = all_refused && (!th || !p.responder.open(*th, t));
    }
    check(all_refused, "a flipped bit in the nonce, ciphertext or tag is refused");

    // Layout refusals, before any decryption.
    check(noise::parse_transport_header(std::span(g).first(24)).error() ==
              noise::refusal_t::MALFORMED,
          "a transport datagram below 25 B is MALFORMED");
    auto t = g;
    t[0] = std::byte{0x03};
    check(noise::parse_transport_header(t).error() == noise::refusal_t::MALFORMED,
          "a reserved type byte is MALFORMED");
    t = g;
    t[8] = std::byte{0x40};  // nonce = 2^62 (byte 7 of the LE nonce)
    check(noise::parse_transport_header(t).error() == noise::refusal_t::NONCE_BOUND,
          "a nonce of 2^62 is refused before decryption");
    check(p.initiator.seal(0, noise::kNonceLimit, f, g).error() == noise::refusal_t::NONCE_BOUND,
          "sealing at nonce 2^62 is refused");
    check(p.initiator.seal(2, 0, f, g).error() == noise::refusal_t::MALFORMED,
          "a key phase other than 0 or 1 is refused");
    check(p.initiator.seal(0, 0, f, std::span(g).first(64 + 24)).error() ==
              noise::refusal_t::MALFORMED,
          "a destination below frame + 25 B is refused");
}

/**
 * @brief Allocations: none before a first message's PSK tag is proven and none per frame, on
 *        every backend (RFC-0033 §5.8); the per-link and per-session costs are reported.
 */
void test_allocations() {
    std::printf("Allocations (process-wide malloc count):\n");
    if (!kCountsAllocations) {
        std::printf("  [SKIP] not counted on this build (sanitizer or non-glibc)\n");
        return;
    }
    // Per link: the PSK state and the handshake cipher, built once.
    const std::size_t a0 = g_allocations;
    noise::symmetric_state_t<crypto_t> base;
    (void)noise::psk_state(ramp(0), base);
    crypto_t::aead_t hc;
    const std::size_t a1 = g_allocations;

    noise::handshake_t<crypto_t> i(noise::role_t::INITIATOR, base, hc);
    noise::handshake_t<crypto_t> r(noise::role_t::RESPONDER, base, hc);
    std::size_t mark = g_allocations;
    noise::transport_cipher_t<crypto_t> ti;
    noise::transport_cipher_t<crypto_t> tr_;
    const std::size_t session_ciphers = (g_allocations - mark) / 2;
    std::array<std::byte, noise::kFirstMessageBytes> m1{};
    std::array<std::byte, noise::kSecondMessageBytes> m2{};
    std::size_t init_allocs = 0;
    std::size_t resp_allocs = 0;
    mark = g_allocations;
    (void)i.write_first(ramp(32), {.flags = 1, .counter = 1}, m1);
    init_allocs += g_allocations - mark;
    mark = g_allocations;
    (void)r.read_first(m1);
    (void)r.write_second(ramp(64), 0x02, m2);
    (void)r.split(tr_);
    resp_allocs += g_allocations - mark;
    mark = g_allocations;
    (void)i.read_second(m2);
    const bool split_ok = i.split(ti).has_value();
    init_allocs += g_allocations - mark;
    std::printf(
        "  [INFO] %s: per link (PSK state + handshake cipher) %zu, per session ciphers %zu, "
        "initiator handshake %zu, responder handshake %zu\n",
        crypto_t::kName, a1 - a0, session_ciphers, init_allocs, resp_allocs);
    check(split_ok, "the counted handshake completes");

    // What a first message costs the responder before its counter is checked: a handshake
    // from the link's PSK state, and the tag check with a link's handshake cipher that has
    // never been keyed (a link's first datagram). Under another PSK it is refused; under the
    // right one (a replay) it stops at the counter.
    noise::symmetric_state_t<crypto_t> other;
    (void)noise::psk_state(ramp(1), other);
    const auto read_cost = [&](const noise::symmetric_state_t<crypto_t>& keyed, bool& read) {
        crypto_t::aead_t fresh;
        const std::size_t before = g_allocations;
        noise::handshake_t<crypto_t> h(noise::role_t::RESPONDER, keyed, fresh);
        read = h.read_first(m1).has_value();
        return g_allocations - before;
    };
    bool wrong_read = true;
    bool replay_read = false;
    const std::size_t wrong = read_cost(other, wrong_read);
    const std::size_t replay = read_cost(base, replay_read);
    std::printf(
        "  [INFO] %s: a first message read: %zu allocations refused (wrong PSK), %zu "
        "accepted (a replay)\n",
        crypto_t::kName, wrong, replay);
    check(!wrong_read && wrong == 0,
          "a first message under another PSK is refused with no allocation");
    check(replay_read && replay == 0, "a replayed first message is read with no allocation");

    bool zero_per_frame = true;
    for (const std::size_t size : {std::size_t{0}, std::size_t{64}, std::size_t{1024},
                                   std::size_t{4096}, std::size_t{16384}, std::size_t{65519}}) {
        std::vector<std::byte> d(size + noise::kTransportOverhead);
        const std::size_t before = g_allocations;
        bool ok = true;
        for (std::uint64_t n = 0; n < 64; ++n) {
            const auto s =
                ti.seal(0, n, std::span(d).subspan(noise::kTransportHeaderBytes, size), d);
            const auto h = noise::parse_transport_header(d);
            ok = ok && s && h && tr_.open(*h, d);
        }
        const std::size_t per = g_allocations - before;
        std::printf("  [INFO] %s: %zu B frame, 64 seal+open: %zu allocations\n", crypto_t::kName,
                    size, per);
        zero_per_frame = zero_per_frame && ok && per == 0;
    }
    check(zero_per_frame, "seal and open allocate nothing, 0 B to 65519 B frames");
}

}  // namespace

int main() {
    test_primitives();
    test_handshake_round_trip();
    test_no_dh_before_psk_proof();
    test_handshake_refusals();
    test_transport();
    test_allocations();
    return tr::testing::summary("security_noise_test");
}
