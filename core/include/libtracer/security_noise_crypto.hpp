/**
 * @file
 * @brief The crypto backend seam of the Noise link (RFC-0033, #2072): the contract a
 *        backend meets, and the helpers every backend shares.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The core stays crypto-free (ADR-0045, ADR-0086). The `security_noise` module brings the
 * primitives of `Noise_NNpsk0_25519_ChaChaPoly_SHA256` from a library the host already ships,
 * through one backend type chosen at compile time. There is no runtime switch: a build names
 * one backend (`LIBTRACER_NOISE_CRYPTO`), `security_noise.hpp` aliases it as
 * `tr::net::noise::default_crypto_t`, and the handshake and transport templates are
 * instantiated over it. A build that names none compiles none of this module and links no
 * crypto library.
 *
 * @section noise_backend_contract The backend contract
 *
 * A backend `B` is a type with static members and two nested state types (the
 * @ref tr::net::noise::crypto_backend concept spells it):
 *
 *  - `B::init()`: one-time library setup, idempotent; false when the library cannot run.
 *  - `B::hash(in, out)`: SHA-256 of one contiguous input.
 *  - `B::hkdf(ck, ikm, out)`: Noise's `HKDF(ck, ikm, n)` for `out.size() == 32 * n`, `n` in
 *    2..3. That is RFC 5869 HKDF-SHA-256 with `salt = ck`, an empty `info` and `L = 32 * n`.
 *  - `B::dh_key_t`: an X25519 key pair. `set(priv, pub_out)` takes the private key and
 *    writes its public key. `agree(peer_pub, out)` writes the raw X25519 output, and may
 *    answer false when the library refuses the point or the result is all zero. Its
 *    destructor wipes or releases the key.
 *  - `B::aead_t`: a ChaCha20-Poly1305 cipher, keyed with `set_key(k)` and re-keyed in place
 *    as often as the caller likes, then `seal(n, ad, pt, out)` (writes `pt.size() + 16`
 *    bytes) and `open(n, ad, ct, out)` (writes `ct.size() - 16` bytes, false on a bad tag).
 *    `n` is the 64-bit Noise nonce; the backend builds the 12-byte nonce with
 *    @ref tr::net::noise::chachapoly_nonce. Both accept `out` equal to the input pointer (in
 *    place); any other overlap is undefined.
 *
 * **Allocation.** `hash`, `hkdf`, and an `aead_t`'s `set_key`, `seal` and `open` MUST NOT
 * allocate. They are every call a first message makes before its PSK tag is proven, which
 * costs no memory (RFC-0033 §5.8), and every call a frame makes. Constructing an `aead_t`
 * may allocate: the link builds its handshake cipher once, beside its PSK state, and a
 * session's two ciphers once per session. `init` and `dh_key_t` may allocate, because key
 * generation and the Diffie-Hellman run only on a responder whose first message proved the
 * PSK, and on an initiator that has a handshake in flight. `core/tests/security_noise_test.cpp`
 * asserts the zero rows and reports the rest per backend.
 *
 * **One call at a time.** An `aead_t` holds per-call state on some backends (OpenSSL keeps
 * one cipher context), so `set_key`, `seal` and `open` are non-const and take one call at a
 * time per `aead_t`. A link serialises each direction of a session, or keeps one cipher per
 * sending thread: two `seal` calls racing on one context would encrypt under each other's
 * nonce.
 *
 * The protocol rules that do not depend on the library (the all-zero X25519 refusal, the
 * nonce bound, the datagram layout) live in `security_noise.hpp`, written once over `B`.
 */
#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace tr::net::noise {

/** @brief SHA-256 output length, Noise's `HASHLEN`. */
inline constexpr std::size_t kHashLen = 32;
/** @brief X25519 key and shared-secret length, Noise's `DHLEN`. */
inline constexpr std::size_t kDhLen = 32;
/** @brief ChaCha20-Poly1305 key length. */
inline constexpr std::size_t kKeyLen = 32;
/** @brief Poly1305 tag length. */
inline constexpr std::size_t kTagLen = 16;
/** @brief The IETF ChaCha20-Poly1305 nonce length. */
inline constexpr std::size_t kNonceLen = 12;

/** @brief A 32-byte value: a hash, a chaining key, a cipher key, a public key or a DH output. */
using key32_t = std::array<std::byte, 32>;

/**
 * @brief The Noise ChaChaPoly nonce: 32 zero bits, then the 64-bit counter little-endian
 *        (Noise §12.3).
 */
[[nodiscard]] constexpr std::array<std::byte, kNonceLen> chachapoly_nonce(
    std::uint64_t n) noexcept {
    std::array<std::byte, kNonceLen> out{};
    for (std::size_t i = 0; i < 8; ++i) out[4 + i] = static_cast<std::byte>((n >> (8 * i)) & 0xFF);
    return out;
}

/**
 * @brief Overwrite @p bytes with zeros through a volatile pointer, so the store is not
 *        elided as dead.
 */
inline void wipe(std::span<std::byte> bytes) noexcept {
    volatile std::byte* p = bytes.data();
    for (std::size_t i = 0; i < bytes.size(); ++i) p[i] = std::byte{0};
}

/** @brief Whether @p k is 32 zero bytes, in time independent of its contents. */
[[nodiscard]] inline bool all_zero(const key32_t& k) noexcept {
    std::byte acc{0};
    for (const std::byte b : k) acc |= b;
    return acc == std::byte{0};
}

/** @brief The longest message @ref hmac_from_hash takes: an HKDF block input is at most 33 bytes.
 */
inline constexpr std::size_t kMaxHmacData = 64;

/**
 * @brief HMAC-SHA-256 (RFC 2104) with a 32-byte key, over a one-shot SHA-256, on the stack.
 *
 * A backend whose library allocates inside its own HMAC call (OpenSSL's `HMAC()` fetches a
 * context) builds its HKDF on this instead, so it needs only a hash that does not allocate.
 *
 * @param hash `bool(std::span<const std::byte> in, key32_t& out)`, SHA-256.
 * @param key  The key.
 * @param data The message, at most @ref kMaxHmacData bytes.
 * @param out  The MAC.
 * @return False if @p data is longer than @ref kMaxHmacData or a hash call failed.
 */
template <class Hash>
[[nodiscard]] bool hmac_from_hash(Hash&& hash, const key32_t& key, std::span<const std::byte> data,
                                  key32_t& out) {
    constexpr std::size_t kBlock = 64;
    std::array<std::byte, kBlock + kMaxHmacData> buf{};
    key32_t inner{};
    bool ok = data.size() <= kMaxHmacData;
    for (std::size_t i = 0; ok && i < kBlock; ++i)
        buf[i] = (i < key.size() ? key[i] : std::byte{0}) ^ std::byte{0x36};
    if (ok && !data.empty()) std::memcpy(buf.data() + kBlock, data.data(), data.size());
    ok = ok && hash(std::span<const std::byte>(buf.data(), kBlock + data.size()), inner);
    for (std::size_t i = 0; ok && i < kBlock; ++i)
        buf[i] = (i < key.size() ? key[i] : std::byte{0}) ^ std::byte{0x5c};
    if (ok) std::memcpy(buf.data() + kBlock, inner.data(), kHashLen);
    ok = ok && hash(std::span<const std::byte>(buf.data(), kBlock + kHashLen), out);
    wipe(buf);
    wipe(inner);
    return ok;
}

/**
 * @brief Noise's `HKDF(ck, ikm, n)` built from one HMAC-SHA-256 primitive, for a backend whose
 *        library has HMAC but no HKDF call.
 *
 * `temp = HMAC(ck, ikm)`, `out1 = HMAC(temp, 0x01)`, `out_i = HMAC(temp, out_{i-1} || i)`.
 *
 * @param hmac `bool(const key32_t& key, std::span<const std::byte> data, key32_t& out)`.
 * @param ck   The chaining key.
 * @param ikm  The input key material (empty for `Split`).
 * @param out  `32 * n` bytes, `n` in 1..3.
 * @return False if @p out is not `32 * n` bytes for `n` in 1..3, or an HMAC call failed.
 */
template <class Hmac>
[[nodiscard]] bool hkdf_from_hmac(Hmac&& hmac, const key32_t& ck, std::span<const std::byte> ikm,
                                  std::span<std::byte> out) {
    key32_t temp{};
    key32_t block{};
    std::array<std::byte, kHashLen + 1> in{};
    std::size_t in_len = 0;
    bool ok = out.size() % kHashLen == 0 && !out.empty() && out.size() <= 3 * kHashLen &&
              hmac(ck, ikm, temp);
    for (std::size_t i = 0; ok && i * kHashLen < out.size(); ++i) {
        in[in_len] = static_cast<std::byte>(i + 1);
        ok = hmac(temp, std::span<const std::byte>(in.data(), in_len + 1), block);
        std::memcpy(out.data() + i * kHashLen, block.data(), kHashLen);
        std::memcpy(in.data(), block.data(), kHashLen);
        in_len = kHashLen;
    }
    wipe(temp);
    wipe(block);
    wipe(in);
    return ok;
}

/**
 * @brief The compile-time contract of a Noise crypto backend, as this file's comment states it.
 */
template <class B>
concept crypto_backend =
    requires(std::span<const std::byte> in, std::span<std::byte> outs, const key32_t& k,
             key32_t& out, typename B::dh_key_t& dh, const typename B::dh_key_t& cdh,
             typename B::aead_t& aead, std::uint64_t n, std::byte* p) {
        { B::init() } -> std::same_as<bool>;
        { B::hash(in, out) } -> std::same_as<bool>;
        { B::hkdf(k, in, outs) } -> std::same_as<bool>;
        { dh.set(k, out) } -> std::same_as<bool>;
        { cdh.agree(k, out) } -> std::same_as<bool>;
        { aead.set_key(k) } -> std::same_as<bool>;
        { aead.seal(n, in, in, p) } -> std::same_as<bool>;
        { aead.open(n, in, in, p) } -> std::same_as<bool>;
    };

}  // namespace tr::net::noise
