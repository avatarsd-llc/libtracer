/**
 * @file
 * @brief The PSA Crypto backend of the Noise link (#2072): `LIBTRACER_NOISE_CRYPTO=psa`, the
 *        ESP-IDF default (mbedTLS through the PSA API).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Meets @ref tr::net::noise::crypto_backend over the PSA Crypto API of mbedTLS 3.6 and 4.x.
 * Every key lives in a volatile key slot: a cipher imports its key once, a key pair is
 * imported and agreed with `psa_raw_key_agreement`, and HKDF is the library's own
 * `PSA_ALG_HKDF(PSA_ALG_SHA_256)` derivation.
 *
 * Three build settings matter, and all are the integrator's (#2065):
 *
 *  - **`MBEDTLS_PSA_ASSUME_EXCLUSIVE_BUFFERS`.** Without it, every PSA call copies its buffers
 *    through the heap, which costs allocations per frame. ESP-IDF's mbedTLS port sets it.
 *  - **`MBEDTLS_PSA_STATIC_KEY_SLOTS`** (mbedTLS 3.6.1 and later). Without it, every key
 *    import callocs the key's buffer, and a first message imports its handshake key before
 *    the tag is checked, which RFC-0033 §5.8 forbids. This header refuses to build without it.
 *    It costs `MBEDTLS_PSA_KEY_SLOT_COUNT * MBEDTLS_PSA_STATIC_KEY_SLOT_BUFFER_SIZE` bytes of
 *    static RAM, and the buffer size follows the largest key type the build enables: an RSA
 *    key pair makes it kilobytes (76,616 B of `.bss` at ESP-IDF's defaults on an ESP32-C6).
 *    It excludes `MBEDTLS_PSA_KEY_STORE_DYNAMIC`, which mbedTLS enables by default. ESP-IDF
 *    has no option for either: the app passes them in an `MBEDTLS_USER_CONFIG_FILE`.
 *  - **ChaCha20-Poly1305.** It is off by default in ESP-IDF: enable `CONFIG_MBEDTLS_CHACHA20_C`
 *    and `CONFIG_MBEDTLS_CHACHAPOLY_C`.
 *
 * Keys occupy the library's global key slots, which TLS shares: a session holds two, and a
 * handshake one more while it runs.
 */
#pragma once

#include <psa/crypto.h>

#if !defined(MBEDTLS_PSA_STATIC_KEY_SLOTS)
#error "the PSA Noise backend needs MBEDTLS_PSA_STATIC_KEY_SLOTS: a key import must not allocate"
#endif

#include <cstddef>
#include <cstdint>
#include <span>

#include "libtracer/security_noise_crypto.hpp"

namespace tr::net::noise {

/** @brief mbedTLS through the PSA Crypto API: keys in volatile key slots. */
struct psa_crypto_t {
    /** @brief The backend's name in reports. */
    static constexpr const char* kName = "psa";

    /** @brief `psa_crypto_init`; idempotent. */
    [[nodiscard]] static bool init() { return psa_crypto_init() == PSA_SUCCESS; }

    /** @brief SHA-256 of @p in. */
    [[nodiscard]] static bool hash(std::span<const std::byte> in, key32_t& out) {
        std::size_t len = 0;
        return psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const std::uint8_t*>(in.data()),
                                in.size(), reinterpret_cast<std::uint8_t*>(out.data()), out.size(),
                                &len) == PSA_SUCCESS &&
               len == kHashLen;
    }

    /** @brief Noise HKDF as `PSA_ALG_HKDF(PSA_ALG_SHA_256)` with `salt = ck`, empty `info`. */
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
        (void)psa_key_derivation_abort(&op);
        return ok;
    }

    /** @brief An X25519 key pair in a volatile key slot. */
    struct dh_key_t {
        dh_key_t() = default;
        dh_key_t(const dh_key_t&) = delete;
        dh_key_t& operator=(const dh_key_t&) = delete;
        ~dh_key_t() { (void)psa_destroy_key(id_); }

        /** @brief Import @p priv as a Montgomery key pair and export its public key. */
        [[nodiscard]] bool set(const key32_t& priv, key32_t& pub) {
            (void)psa_destroy_key(id_);
            id_ = PSA_KEY_ID_NULL;
            psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
            psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
            psa_set_key_bits(&a, 255);
            psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE);
            psa_set_key_algorithm(&a, PSA_ALG_ECDH);
            std::size_t len = 0;
            return psa_import_key(&a, reinterpret_cast<const std::uint8_t*>(priv.data()),
                                  priv.size(), &id_) == PSA_SUCCESS &&
                   psa_export_public_key(id_, reinterpret_cast<std::uint8_t*>(pub.data()),
                                         pub.size(), &len) == PSA_SUCCESS &&
                   len == kDhLen;
        }

        /** @brief X25519 with @p peer through `psa_raw_key_agreement`. */
        [[nodiscard]] bool agree(const key32_t& peer, key32_t& out) const {
            std::size_t len = 0;
            return psa_raw_key_agreement(PSA_ALG_ECDH, id_,
                                         reinterpret_cast<const std::uint8_t*>(peer.data()),
                                         peer.size(), reinterpret_cast<std::uint8_t*>(out.data()),
                                         out.size(), &len) == PSA_SUCCESS &&
                   len == kDhLen;
        }

       private:
        psa_key_id_t id_ = PSA_KEY_ID_NULL; /**< @brief The slot, or null before @ref set. */
    };

    /** @brief A ChaCha20-Poly1305 cipher: its key, imported once into a volatile slot. */
    struct aead_t {
        aead_t() = default;
        aead_t(const aead_t&) = delete;
        aead_t& operator=(const aead_t&) = delete;
        ~aead_t() { (void)psa_destroy_key(id_); }

        /** @brief Import @p k, replacing any previous key. */
        [[nodiscard]] bool set_key(const key32_t& k) {
            (void)psa_destroy_key(id_);
            id_ = PSA_KEY_ID_NULL;
            psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
            psa_set_key_type(&a, PSA_KEY_TYPE_CHACHA20);
            psa_set_key_bits(&a, 256);
            psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
            psa_set_key_algorithm(&a, PSA_ALG_CHACHA20_POLY1305);
            return psa_import_key(&a, reinterpret_cast<const std::uint8_t*>(k.data()), k.size(),
                                  &id_) == PSA_SUCCESS;
        }

        /** @brief Seal @p pt under nonce @p n; @p out takes `pt.size() + kTagLen` bytes. */
        [[nodiscard]] bool seal(std::uint64_t n, std::span<const std::byte> ad,
                                std::span<const std::byte> pt, std::byte* out) {
            const auto nonce = chachapoly_nonce(n);
            std::size_t len = 0;
            return psa_aead_encrypt(id_, PSA_ALG_CHACHA20_POLY1305,
                                    reinterpret_cast<const std::uint8_t*>(nonce.data()),
                                    nonce.size(), reinterpret_cast<const std::uint8_t*>(ad.data()),
                                    ad.size(), reinterpret_cast<const std::uint8_t*>(pt.data()),
                                    pt.size(), reinterpret_cast<std::uint8_t*>(out),
                                    pt.size() + kTagLen, &len) == PSA_SUCCESS;
        }

        /** @brief Open @p ct under nonce @p n into @p out; false on a bad tag. */
        [[nodiscard]] bool open(std::uint64_t n, std::span<const std::byte> ad,
                                std::span<const std::byte> ct, std::byte* out) {
            if (ct.size() < kTagLen) return false;
            const auto nonce = chachapoly_nonce(n);
            std::size_t len = 0;
            return psa_aead_decrypt(id_, PSA_ALG_CHACHA20_POLY1305,
                                    reinterpret_cast<const std::uint8_t*>(nonce.data()),
                                    nonce.size(), reinterpret_cast<const std::uint8_t*>(ad.data()),
                                    ad.size(), reinterpret_cast<const std::uint8_t*>(ct.data()),
                                    ct.size(), reinterpret_cast<std::uint8_t*>(out),
                                    ct.size() - kTagLen, &len) == PSA_SUCCESS;
        }

       private:
        psa_key_id_t id_ = PSA_KEY_ID_NULL; /**< @brief The slot, or null before @ref set_key. */
    };
};

static_assert(crypto_backend<psa_crypto_t>);

}  // namespace tr::net::noise
