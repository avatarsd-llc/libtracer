/**
 * @file
 * @brief The libsodium crypto backend of the Noise link (#2072): `LIBTRACER_NOISE_CRYPTO=sodium`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Meets @ref tr::net::noise::crypto_backend over libsodium's stateless calls on caller
 * memory: a cipher is its 32-byte key, a key pair its private scalar, and HKDF is built from
 * `crypto_auth_hmacsha256` (1.0.18, the version Ubuntu 24.04 ships, has no HKDF call). Nothing
 * allocates, in the handshake or per frame.
 */
#pragma once

#include <sodium.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "libtracer/security_noise_crypto.hpp"

namespace tr::net::noise {

/** @brief libsodium: stateless calls, keys held in the backend's own objects. */
struct sodium_crypto_t {
    /** @brief The backend's name in reports. */
    static constexpr const char* kName = "sodium";

    /** @brief `sodium_init`; idempotent, false only if the library cannot run here. */
    [[nodiscard]] static bool init() { return sodium_init() >= 0; }

    /** @brief SHA-256 of @p in. */
    [[nodiscard]] static bool hash(std::span<const std::byte> in, key32_t& out) {
        return crypto_hash_sha256(reinterpret_cast<unsigned char*>(out.data()),
                                  reinterpret_cast<const unsigned char*>(in.data()),
                                  in.size()) == 0;
    }

    /** @brief HMAC-SHA-256 with a 32-byte key. */
    [[nodiscard]] static bool hmac(const key32_t& key, std::span<const std::byte> data,
                                   key32_t& out) {
        crypto_auth_hmacsha256_state st;
        const bool ok =
            crypto_auth_hmacsha256_init(&st, reinterpret_cast<const unsigned char*>(key.data()),
                                        key.size()) == 0 &&
            crypto_auth_hmacsha256_update(&st, reinterpret_cast<const unsigned char*>(data.data()),
                                          data.size()) == 0 &&
            crypto_auth_hmacsha256_final(&st, reinterpret_cast<unsigned char*>(out.data())) == 0;
        sodium_memzero(&st, sizeof(st));
        return ok;
    }

    /** @brief Noise HKDF over @ref hmac. */
    [[nodiscard]] static bool hkdf(const key32_t& ck, std::span<const std::byte> ikm,
                                   std::span<std::byte> out) {
        return hkdf_from_hmac(&hmac, ck, ikm, out);
    }

    /** @brief An X25519 key pair: the private scalar. */
    struct dh_key_t {
        dh_key_t() = default;
        dh_key_t(const dh_key_t&) = delete;
        dh_key_t& operator=(const dh_key_t&) = delete;
        ~dh_key_t() { sodium_memzero(priv_.data(), priv_.size()); }

        /** @brief Take @p priv as the private key and compute its public key. */
        [[nodiscard]] bool set(const key32_t& priv, key32_t& pub) {
            priv_ = priv;
            return crypto_scalarmult_base(reinterpret_cast<unsigned char*>(pub.data()),
                                          reinterpret_cast<const unsigned char*>(priv_.data())) ==
                   0;
        }

        /** @brief X25519 with @p peer; libsodium refuses an all-zero result itself. */
        [[nodiscard]] bool agree(const key32_t& peer, key32_t& out) const {
            return crypto_scalarmult(reinterpret_cast<unsigned char*>(out.data()),
                                     reinterpret_cast<const unsigned char*>(priv_.data()),
                                     reinterpret_cast<const unsigned char*>(peer.data())) == 0;
        }

       private:
        key32_t priv_{}; /**< @brief The private scalar (clamped by the library on use). */
    };

    /** @brief A ChaCha20-Poly1305 (IETF) cipher: its key. */
    struct aead_t {
        aead_t() = default;
        aead_t(const aead_t&) = delete;
        aead_t& operator=(const aead_t&) = delete;
        ~aead_t() { sodium_memzero(key_.data(), key_.size()); }

        /** @brief Store the key. */
        [[nodiscard]] bool set_key(const key32_t& k) {
            key_ = k;
            return true;
        }

        /** @brief Seal @p pt under nonce @p n; @p out takes `pt.size() + kTagLen` bytes. */
        [[nodiscard]] bool seal(std::uint64_t n, std::span<const std::byte> ad,
                                std::span<const std::byte> pt, std::byte* out) const {
            const auto nonce = chachapoly_nonce(n);
            unsigned long long len = 0;
            return crypto_aead_chacha20poly1305_ietf_encrypt(
                       reinterpret_cast<unsigned char*>(out), &len,
                       reinterpret_cast<const unsigned char*>(pt.data()), pt.size(),
                       reinterpret_cast<const unsigned char*>(ad.data()), ad.size(), nullptr,
                       reinterpret_cast<const unsigned char*>(nonce.data()),
                       reinterpret_cast<const unsigned char*>(key_.data())) == 0;
        }

        /** @brief Open @p ct under nonce @p n into @p out; false on a bad tag. */
        [[nodiscard]] bool open(std::uint64_t n, std::span<const std::byte> ad,
                                std::span<const std::byte> ct, std::byte* out) const {
            const auto nonce = chachapoly_nonce(n);
            unsigned long long len = 0;
            return crypto_aead_chacha20poly1305_ietf_decrypt(
                       reinterpret_cast<unsigned char*>(out), &len, nullptr,
                       reinterpret_cast<const unsigned char*>(ct.data()), ct.size(),
                       reinterpret_cast<const unsigned char*>(ad.data()), ad.size(),
                       reinterpret_cast<const unsigned char*>(nonce.data()),
                       reinterpret_cast<const unsigned char*>(key_.data())) == 0;
        }

       private:
        key32_t key_{}; /**< @brief The cipher key. */
    };
};

static_assert(crypto_backend<sodium_crypto_t>);

}  // namespace tr::net::noise
