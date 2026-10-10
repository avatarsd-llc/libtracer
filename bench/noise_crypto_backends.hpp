/**
 * @file
 * @brief The crypto backends the Noise link harness prices (#2065): libsodium, OpenSSL and
 *        the PSA Crypto API of mbedTLS (the library ESP-IDF ships), each behind its own switch.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Each backend satisfies the contract in `noise_crypto.hpp` and is compiled only when the build
 * found its library: `LIBTRACER_BENCH_HAVE_SODIUM`, `LIBTRACER_BENCH_HAVE_OPENSSL`,
 * `LIBTRACER_BENCH_HAVE_PSA`. Each one is written the way an application would use that
 * library, not bent to look like the others:
 *
 *  - **libsodium** is stateless: the AEAD state is the 32-byte key, X25519 is two scalar
 *    multiplications on caller memory, and HKDF is built from its HMAC-SHA-256 (1.0.18, the
 *    version Ubuntu 24.04 ships, predates `crypto_kdf_hkdf_sha256`).
 *  - **OpenSSL** keeps one `EVP_CIPHER_CTX` per cipher state, keyed once and re-nonced per
 *    frame; X25519 goes through `EVP_PKEY`; HKDF is built from the one-shot `HMAC()`.
 *  - **PSA** (mbedTLS 3.6 and 4.x) holds every key in a key slot: a cipher state imports its key
 *    once, a DH key pair is imported and agreed with `psa_raw_key_agreement`, and HKDF is the
 *    library's own `PSA_ALG_HKDF` derivation. The same code builds for ESP-IDF, where the
 *    harness's ESP app (`bench/esp_noise_crypto/`) links it for the flash and RAM rows.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "noise_crypto.hpp"

#if defined(LIBTRACER_BENCH_HAVE_SODIUM)
#include <sodium.h>
#endif
#if defined(LIBTRACER_BENCH_HAVE_OPENSSL)
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#endif
#if defined(LIBTRACER_BENCH_HAVE_PSA)
#include <psa/crypto.h>
#endif

namespace bench::noise {

#if defined(LIBTRACER_BENCH_HAVE_SODIUM)
/** @brief libsodium: stateless calls on caller memory. */
struct sodium_backend_t {
    /** @brief The row's `system` column. */
    static constexpr const char* kName = "libsodium";

    /** @brief `sodium_init`; false only if the library cannot run here. */
    [[nodiscard]] static bool init() { return sodium_init() >= 0; }

    /** @brief SHA-256 of @p in. */
    static void hash(std::span<const std::byte> in, key32_t& out) {
        crypto_hash_sha256(reinterpret_cast<unsigned char*>(out.data()),
                           reinterpret_cast<const unsigned char*>(in.data()), in.size());
    }

    /** @brief HMAC-SHA-256 with a 32-byte key. */
    [[nodiscard]] static bool hmac(const key32_t& key, std::span<const std::byte> data,
                                   key32_t& out) {
        crypto_auth_hmacsha256_state st;
        crypto_auth_hmacsha256_init(&st, reinterpret_cast<const unsigned char*>(key.data()),
                                    key.size());
        crypto_auth_hmacsha256_update(&st, reinterpret_cast<const unsigned char*>(data.data()),
                                      data.size());
        crypto_auth_hmacsha256_final(&st, reinterpret_cast<unsigned char*>(out.data()));
        sodium_memzero(&st, sizeof(st));
        return true;
    }

    /** @brief Noise HKDF over @ref hmac. */
    [[nodiscard]] static bool hkdf(const key32_t& ck, std::span<const std::byte> ikm,
                                   std::span<std::byte> out) {
        return hkdf_from_hmac(&hmac, ck, ikm, out);
    }

    /** @brief An X25519 key pair: the private scalar, kept in the object. */
    struct dh_key_t {
        key32_t priv{}; /**< @brief The private scalar (clamped by the library on use). */

        /** @brief Take @p p as the private key and compute its public key. */
        [[nodiscard]] bool set(const key32_t& p, key32_t& pub) {
            priv = p;
            return crypto_scalarmult_base(reinterpret_cast<unsigned char*>(pub.data()),
                                          reinterpret_cast<const unsigned char*>(priv.data())) == 0;
        }

        /** @brief X25519 with @p peer; false on an all-zero (low-order) result. */
        [[nodiscard]] bool agree(const key32_t& peer, key32_t& out) const {
            return crypto_scalarmult(reinterpret_cast<unsigned char*>(out.data()),
                                     reinterpret_cast<const unsigned char*>(priv.data()),
                                     reinterpret_cast<const unsigned char*>(peer.data())) == 0;
        }

        ~dh_key_t() { sodium_memzero(priv.data(), priv.size()); }
    };

    /** @brief A ChaCha20-Poly1305 (IETF) state: the key. */
    struct aead_t {
        key32_t key{}; /**< @brief The cipher key. */

        /** @brief Store the key. */
        [[nodiscard]] bool set_key(const key32_t& k) {
            key = k;
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
                       reinterpret_cast<const unsigned char*>(key.data())) == 0;
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
                       reinterpret_cast<const unsigned char*>(key.data())) == 0;
        }

        ~aead_t() { sodium_memzero(key.data(), key.size()); }
    };
};
#endif  // LIBTRACER_BENCH_HAVE_SODIUM

#if defined(LIBTRACER_BENCH_HAVE_OPENSSL)
/** @brief OpenSSL 3 libcrypto: `EVP_CIPHER_CTX` per cipher state, `EVP_PKEY` for X25519. */
struct openssl_backend_t {
    /** @brief The row's `system` column. */
    static constexpr const char* kName = "openssl";

    /** @brief Nothing to set up in OpenSSL 3. */
    [[nodiscard]] static bool init() { return true; }

    /** @brief SHA-256 of @p in (the one-shot, which needs no context). */
    static void hash(std::span<const std::byte> in, key32_t& out) {
        SHA256(reinterpret_cast<const unsigned char*>(in.data()), in.size(),
               reinterpret_cast<unsigned char*>(out.data()));
    }

    /** @brief HMAC-SHA-256 with a 32-byte key, through the one-shot `HMAC()`. */
    [[nodiscard]] static bool hmac(const key32_t& key, std::span<const std::byte> data,
                                   key32_t& out) {
        unsigned int len = 0;
        return HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
                    reinterpret_cast<const unsigned char*>(data.data()), data.size(),
                    reinterpret_cast<unsigned char*>(out.data()), &len) != nullptr &&
               len == kHashLen;
    }

    /** @brief Noise HKDF over @ref hmac. */
    [[nodiscard]] static bool hkdf(const key32_t& ck, std::span<const std::byte> ikm,
                                   std::span<std::byte> out) {
        return hkdf_from_hmac(&hmac, ck, ikm, out);
    }

    /** @brief An X25519 key pair held as an `EVP_PKEY`. */
    struct dh_key_t {
        EVP_PKEY* key = nullptr; /**< @brief The private key, or null before @ref set. */

        /** @brief Import @p p as the private key and export its public key. */
        [[nodiscard]] bool set(const key32_t& p, key32_t& pub) {
            EVP_PKEY_free(key);
            key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                               reinterpret_cast<const unsigned char*>(p.data()),
                                               p.size());
            std::size_t len = pub.size();
            return key != nullptr &&
                   EVP_PKEY_get_raw_public_key(key, reinterpret_cast<unsigned char*>(pub.data()),
                                               &len) == 1 &&
                   len == kDhLen;
        }

        /** @brief X25519 with @p peer through `EVP_PKEY_derive`. */
        [[nodiscard]] bool agree(const key32_t& peer, key32_t& out) const {
            EVP_PKEY* pk = EVP_PKEY_new_raw_public_key(
                EVP_PKEY_X25519, nullptr, reinterpret_cast<const unsigned char*>(peer.data()),
                peer.size());
            EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(key, nullptr);
            std::size_t len = out.size();
            const bool ok =
                pk != nullptr && ctx != nullptr && EVP_PKEY_derive_init(ctx) == 1 &&
                EVP_PKEY_derive_set_peer(ctx, pk) == 1 &&
                EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char*>(out.data()), &len) == 1 &&
                len == kDhLen;
            EVP_PKEY_CTX_free(ctx);
            EVP_PKEY_free(pk);
            return ok;
        }

        dh_key_t() = default;
        dh_key_t(const dh_key_t&) = delete;
        dh_key_t& operator=(const dh_key_t&) = delete;
        ~dh_key_t() { EVP_PKEY_free(key); }
    };

    /** @brief A ChaCha20-Poly1305 state: one `EVP_CIPHER_CTX`, keyed once. */
    struct aead_t {
        EVP_CIPHER_CTX* ctx = nullptr; /**< @brief The keyed context. */

        /** @brief Allocate the context (first time) and key it. */
        [[nodiscard]] bool set_key(const key32_t& k) {
            if (ctx == nullptr) ctx = EVP_CIPHER_CTX_new();
            return ctx != nullptr &&
                   EVP_CipherInit_ex(ctx, EVP_chacha20_poly1305(), nullptr,
                                     reinterpret_cast<const unsigned char*>(k.data()), nullptr,
                                     -1) == 1;
        }

        /** @brief Seal @p pt under nonce @p n; @p out takes `pt.size() + kTagLen` bytes. */
        [[nodiscard]] bool seal(std::uint64_t n, std::span<const std::byte> ad,
                                std::span<const std::byte> pt, std::byte* out) const {
            const auto nonce = chachapoly_nonce(n);
            int len = 0;
            auto* o = reinterpret_cast<unsigned char*>(out);
            if (EVP_CipherInit_ex(ctx, nullptr, nullptr, nullptr,
                                  reinterpret_cast<const unsigned char*>(nonce.data()), 1) != 1)
                return false;
            if (!ad.empty() && EVP_CipherUpdate(ctx, nullptr, &len,
                                                reinterpret_cast<const unsigned char*>(ad.data()),
                                                static_cast<int>(ad.size())) != 1)
                return false;
            if (!pt.empty() &&
                EVP_CipherUpdate(ctx, o, &len, reinterpret_cast<const unsigned char*>(pt.data()),
                                 static_cast<int>(pt.size())) != 1)
                return false;
            int fin = 0;
            if (EVP_CipherFinal_ex(ctx, o + pt.size(), &fin) != 1) return false;
            return EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, static_cast<int>(kTagLen),
                                       o + pt.size()) == 1;
        }

        /** @brief Open @p ct under nonce @p n into @p out; false on a bad tag. */
        [[nodiscard]] bool open(std::uint64_t n, std::span<const std::byte> ad,
                                std::span<const std::byte> ct, std::byte* out) const {
            const auto nonce = chachapoly_nonce(n);
            const std::size_t body = ct.size() - kTagLen;
            int len = 0;
            auto* o = reinterpret_cast<unsigned char*>(out);
            const auto* c = reinterpret_cast<const unsigned char*>(ct.data());
            if (EVP_CipherInit_ex(ctx, nullptr, nullptr, nullptr,
                                  reinterpret_cast<const unsigned char*>(nonce.data()), 0) != 1)
                return false;
            if (!ad.empty() && EVP_CipherUpdate(ctx, nullptr, &len,
                                                reinterpret_cast<const unsigned char*>(ad.data()),
                                                static_cast<int>(ad.size())) != 1)
                return false;
            if (body != 0 && EVP_CipherUpdate(ctx, o, &len, c, static_cast<int>(body)) != 1)
                return false;
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, static_cast<int>(kTagLen),
                                    const_cast<unsigned char*>(c + body)) != 1)
                return false;
            int fin = 0;
            return EVP_CipherFinal_ex(ctx, o + body, &fin) == 1;
        }

        aead_t() = default;
        aead_t(const aead_t&) = delete;
        aead_t& operator=(const aead_t&) = delete;
        ~aead_t() { EVP_CIPHER_CTX_free(ctx); }
    };
};
#endif  // LIBTRACER_BENCH_HAVE_OPENSSL

#if defined(LIBTRACER_BENCH_HAVE_PSA)
/** @brief mbedTLS through the PSA Crypto API: every key lives in a volatile key slot. */
struct psa_backend_t {
    /** @brief The row's `system` column. */
    static constexpr const char* kName = "mbedtls-psa";

    /** @brief `psa_crypto_init`. */
    [[nodiscard]] static bool init() { return psa_crypto_init() == PSA_SUCCESS; }

    /** @brief SHA-256 of @p in. */
    static void hash(std::span<const std::byte> in, key32_t& out) {
        std::size_t len = 0;
        (void)psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const std::uint8_t*>(in.data()),
                               in.size(), reinterpret_cast<std::uint8_t*>(out.data()), out.size(),
                               &len);
    }

    /** @brief Noise HKDF as the library's `PSA_ALG_HKDF(PSA_ALG_SHA_256)` derivation. */
    [[nodiscard]] static bool hkdf(const key32_t& ck, std::span<const std::byte> ikm,
                                   std::span<std::byte> out) {
        psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
        const bool ok =
            psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256)) == PSA_SUCCESS &&
            psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT,
                                           reinterpret_cast<const std::uint8_t*>(ck.data()),
                                           ck.size()) == PSA_SUCCESS &&
            psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET,
                                           reinterpret_cast<const std::uint8_t*>(ikm.data()),
                                           ikm.size()) == PSA_SUCCESS &&
            psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, nullptr, 0) ==
                PSA_SUCCESS &&
            psa_key_derivation_output_bytes(&op, reinterpret_cast<std::uint8_t*>(out.data()),
                                            out.size()) == PSA_SUCCESS;
        psa_key_derivation_abort(&op);
        return ok;
    }

    /** @brief An X25519 key pair in a volatile key slot. */
    struct dh_key_t {
        psa_key_id_t id = PSA_KEY_ID_NULL; /**< @brief The slot, or null before @ref set. */

        /** @brief Import @p p as a Montgomery key pair and export its public key. */
        [[nodiscard]] bool set(const key32_t& p, key32_t& pub) {
            (void)psa_destroy_key(id);
            psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
            psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
            psa_set_key_bits(&a, 255);
            psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE);
            psa_set_key_algorithm(&a, PSA_ALG_ECDH);
            std::size_t len = 0;
            return psa_import_key(&a, reinterpret_cast<const std::uint8_t*>(p.data()), p.size(),
                                  &id) == PSA_SUCCESS &&
                   psa_export_public_key(id, reinterpret_cast<std::uint8_t*>(pub.data()),
                                         pub.size(), &len) == PSA_SUCCESS &&
                   len == kDhLen;
        }

        /** @brief X25519 with @p peer through `psa_raw_key_agreement`. */
        [[nodiscard]] bool agree(const key32_t& peer, key32_t& out) const {
            std::size_t len = 0;
            return psa_raw_key_agreement(PSA_ALG_ECDH, id,
                                         reinterpret_cast<const std::uint8_t*>(peer.data()),
                                         peer.size(), reinterpret_cast<std::uint8_t*>(out.data()),
                                         out.size(), &len) == PSA_SUCCESS &&
                   len == kDhLen;
        }

        dh_key_t() = default;
        dh_key_t(const dh_key_t&) = delete;
        dh_key_t& operator=(const dh_key_t&) = delete;
        ~dh_key_t() { (void)psa_destroy_key(id); }
    };

    /** @brief A ChaCha20-Poly1305 state: the key, imported once into a volatile slot. */
    struct aead_t {
        psa_key_id_t id = PSA_KEY_ID_NULL; /**< @brief The slot, or null before @ref set_key. */

        /** @brief Import @p k (replacing any previous key). */
        [[nodiscard]] bool set_key(const key32_t& k) {
            (void)psa_destroy_key(id);
            id = PSA_KEY_ID_NULL;
            psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
            psa_set_key_type(&a, PSA_KEY_TYPE_CHACHA20);
            psa_set_key_bits(&a, 256);
            psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
            psa_set_key_algorithm(&a, PSA_ALG_CHACHA20_POLY1305);
            return psa_import_key(&a, reinterpret_cast<const std::uint8_t*>(k.data()), k.size(),
                                  &id) == PSA_SUCCESS;
        }

        /** @brief Seal @p pt under nonce @p n; @p out takes `pt.size() + kTagLen` bytes. */
        [[nodiscard]] bool seal(std::uint64_t n, std::span<const std::byte> ad,
                                std::span<const std::byte> pt, std::byte* out) const {
            const auto nonce = chachapoly_nonce(n);
            std::size_t len = 0;
            return psa_aead_encrypt(id, PSA_ALG_CHACHA20_POLY1305,
                                    reinterpret_cast<const std::uint8_t*>(nonce.data()),
                                    nonce.size(), reinterpret_cast<const std::uint8_t*>(ad.data()),
                                    ad.size(), reinterpret_cast<const std::uint8_t*>(pt.data()),
                                    pt.size(), reinterpret_cast<std::uint8_t*>(out),
                                    pt.size() + kTagLen, &len) == PSA_SUCCESS;
        }

        /** @brief Open @p ct under nonce @p n into @p out; false on a bad tag. */
        [[nodiscard]] bool open(std::uint64_t n, std::span<const std::byte> ad,
                                std::span<const std::byte> ct, std::byte* out) const {
            const auto nonce = chachapoly_nonce(n);
            std::size_t len = 0;
            return psa_aead_decrypt(id, PSA_ALG_CHACHA20_POLY1305,
                                    reinterpret_cast<const std::uint8_t*>(nonce.data()),
                                    nonce.size(), reinterpret_cast<const std::uint8_t*>(ad.data()),
                                    ad.size(), reinterpret_cast<const std::uint8_t*>(ct.data()),
                                    ct.size(), reinterpret_cast<std::uint8_t*>(out),
                                    ct.size() - kTagLen, &len) == PSA_SUCCESS;
        }

        aead_t() = default;
        aead_t(const aead_t&) = delete;
        aead_t& operator=(const aead_t&) = delete;
        ~aead_t() { (void)psa_destroy_key(id); }
    };
};
#endif  // LIBTRACER_BENCH_HAVE_PSA

}  // namespace bench::noise
