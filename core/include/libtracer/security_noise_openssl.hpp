/**
 * @file
 * @brief The OpenSSL 3 crypto backend of the Noise link (#2072): `LIBTRACER_NOISE_CRYPTO=openssl`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Meets @ref tr::net::noise::crypto_backend over libcrypto. A cipher keeps one
 * `EVP_CIPHER_CTX`, keyed once and re-nonced per frame, so `seal` and `open` allocate
 * nothing. X25519 goes through `EVP_PKEY`, and HKDF is built from the one-shot `HMAC()`. Key
 * setup and the handshake's hashing allocate inside libcrypto; `security_noise_test` reports
 * how much. The fastest AEAD of the measured host backends at 1 KiB and above (#2065).
 */
#pragma once

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "libtracer/security_noise_crypto.hpp"

namespace tr::net::noise {

/** @brief libcrypto (OpenSSL 3): `EVP_CIPHER_CTX` per cipher, `EVP_PKEY` for X25519. */
struct openssl_crypto_t {
    /** @brief The backend's name in reports. */
    static constexpr const char* kName = "openssl";

    /** @brief Nothing to set up in OpenSSL 3. */
    [[nodiscard]] static bool init() { return true; }

    /** @brief SHA-256 of @p in, through the one-shot `SHA256()`. */
    [[nodiscard]] static bool hash(std::span<const std::byte> in, key32_t& out) {
        return SHA256(reinterpret_cast<const unsigned char*>(in.data()), in.size(),
                      reinterpret_cast<unsigned char*>(out.data())) != nullptr;
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
        dh_key_t() = default;
        dh_key_t(const dh_key_t&) = delete;
        dh_key_t& operator=(const dh_key_t&) = delete;
        ~dh_key_t() { EVP_PKEY_free(key_); }

        /** @brief Import @p priv as the private key and export its public key. */
        [[nodiscard]] bool set(const key32_t& priv, key32_t& pub) {
            EVP_PKEY_free(key_);
            key_ = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                                reinterpret_cast<const unsigned char*>(priv.data()),
                                                priv.size());
            std::size_t len = pub.size();
            return key_ != nullptr &&
                   EVP_PKEY_get_raw_public_key(key_, reinterpret_cast<unsigned char*>(pub.data()),
                                               &len) == 1 &&
                   len == kDhLen;
        }

        /** @brief X25519 with @p peer; libcrypto refuses an all-zero result itself. */
        [[nodiscard]] bool agree(const key32_t& peer, key32_t& out) const {
            EVP_PKEY* pk = EVP_PKEY_new_raw_public_key(
                EVP_PKEY_X25519, nullptr, reinterpret_cast<const unsigned char*>(peer.data()),
                peer.size());
            EVP_PKEY_CTX* ctx = key_ != nullptr ? EVP_PKEY_CTX_new(key_, nullptr) : nullptr;
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

       private:
        EVP_PKEY* key_ = nullptr; /**< @brief The private key, or null before @ref set. */
    };

    /** @brief A ChaCha20-Poly1305 cipher: one `EVP_CIPHER_CTX`, keyed once. */
    struct aead_t {
        aead_t() = default;
        aead_t(const aead_t&) = delete;
        aead_t& operator=(const aead_t&) = delete;
        ~aead_t() { EVP_CIPHER_CTX_free(ctx_); }

        /** @brief Allocate the context on first use and key it. */
        [[nodiscard]] bool set_key(const key32_t& k) {
            if (ctx_ == nullptr) ctx_ = EVP_CIPHER_CTX_new();
            return ctx_ != nullptr &&
                   EVP_CipherInit_ex(ctx_, EVP_chacha20_poly1305(), nullptr,
                                     reinterpret_cast<const unsigned char*>(k.data()), nullptr,
                                     -1) == 1;
        }

        /** @brief Seal @p pt under nonce @p n; @p out takes `pt.size() + kTagLen` bytes. */
        [[nodiscard]] bool seal(std::uint64_t n, std::span<const std::byte> ad,
                                std::span<const std::byte> pt, std::byte* out) const {
            const auto nonce = chachapoly_nonce(n);
            auto* o = reinterpret_cast<unsigned char*>(out);
            int len = 0;
            int fin = 0;
            return ctx_ != nullptr &&
                   EVP_CipherInit_ex(ctx_, nullptr, nullptr, nullptr,
                                     reinterpret_cast<const unsigned char*>(nonce.data()),
                                     1) == 1 &&
                   (ad.empty() ||
                    EVP_CipherUpdate(ctx_, nullptr, &len,
                                     reinterpret_cast<const unsigned char*>(ad.data()),
                                     static_cast<int>(ad.size())) == 1) &&
                   (pt.empty() ||
                    EVP_CipherUpdate(ctx_, o, &len,
                                     reinterpret_cast<const unsigned char*>(pt.data()),
                                     static_cast<int>(pt.size())) == 1) &&
                   EVP_CipherFinal_ex(ctx_, o + pt.size(), &fin) == 1 &&
                   EVP_CIPHER_CTX_ctrl(ctx_, EVP_CTRL_AEAD_GET_TAG, static_cast<int>(kTagLen),
                                       o + pt.size()) == 1;
        }

        /** @brief Open @p ct under nonce @p n into @p out; false on a bad tag. */
        [[nodiscard]] bool open(std::uint64_t n, std::span<const std::byte> ad,
                                std::span<const std::byte> ct, std::byte* out) const {
            if (ctx_ == nullptr || ct.size() < kTagLen) return false;
            const auto nonce = chachapoly_nonce(n);
            const std::size_t body = ct.size() - kTagLen;
            auto* o = reinterpret_cast<unsigned char*>(out);
            const auto* c = reinterpret_cast<const unsigned char*>(ct.data());
            // The tag is copied out first: an in-place open overwrites nothing past `body`,
            // but SET_TAG takes a non-const pointer.
            unsigned char tag[kTagLen];
            for (std::size_t i = 0; i < kTagLen; ++i) tag[i] = c[body + i];
            int len = 0;
            int fin = 0;
            return EVP_CipherInit_ex(ctx_, nullptr, nullptr, nullptr,
                                     reinterpret_cast<const unsigned char*>(nonce.data()),
                                     0) == 1 &&
                   (ad.empty() ||
                    EVP_CipherUpdate(ctx_, nullptr, &len,
                                     reinterpret_cast<const unsigned char*>(ad.data()),
                                     static_cast<int>(ad.size())) == 1) &&
                   (body == 0 || EVP_CipherUpdate(ctx_, o, &len, c, static_cast<int>(body)) == 1) &&
                   EVP_CIPHER_CTX_ctrl(ctx_, EVP_CTRL_AEAD_SET_TAG, static_cast<int>(kTagLen),
                                       tag) == 1 &&
                   EVP_CipherFinal_ex(ctx_, o + body, &fin) == 1;
        }

       private:
        EVP_CIPHER_CTX* ctx_ =
            nullptr; /**< @brief The keyed context, or null before @ref set_key. */
    };
};

static_assert(crypto_backend<openssl_crypto_t>);

}  // namespace tr::net::noise
