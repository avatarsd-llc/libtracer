/**
 * @file
 * @brief The OpenSSL 3 crypto backend of the Noise link (#2072): `LIBTRACER_NOISE_CRYPTO=openssl`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Meets @ref tr::net::noise::crypto_backend over libcrypto. A cipher allocates one
 * `EVP_CIPHER_CTX` when it is built, and `set_key`, `seal` and `open` re-key and re-nonce
 * that context in place, so none of them allocates. The hash is the low-level `SHA256_*`
 * calls on a stack context, because the one-shot `SHA256()` and `HMAC()` fetch and allocate
 * a context per call; HMAC and HKDF are built over it. X25519 goes through `EVP_PKEY` and
 * allocates, after the PSK is proven. The fastest AEAD of the measured host backends at
 * 1 KiB and above (#2065).
 *
 * The `SHA256_*` calls are deprecated in OpenSSL 3 but present unless the library was built
 * with `no-deprecated`, which this backend refuses.
 */
#pragma once

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#if defined(OPENSSL_NO_DEPRECATED_3_0)
#error "the OpenSSL Noise backend needs SHA256_Init/Update/Final (no allocation per call)"
#endif

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

    /** @brief SHA-256 of @p in, on a stack `SHA256_CTX` (no allocation). */
    [[nodiscard]] static bool hash(std::span<const std::byte> in, key32_t& out) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        SHA256_CTX c;
        const bool ok = SHA256_Init(&c) == 1 && SHA256_Update(&c, in.data(), in.size()) == 1 &&
                        SHA256_Final(reinterpret_cast<unsigned char*>(out.data()), &c) == 1;
#pragma GCC diagnostic pop
        OPENSSL_cleanse(&c, sizeof(c));
        return ok;
    }

    /** @brief Noise HKDF over @ref hash, through the shared stack HMAC. */
    [[nodiscard]] static bool hkdf(const key32_t& ck, std::span<const std::byte> ikm,
                                   std::span<std::byte> out) {
        return hkdf_from_hmac([](const key32_t& k, std::span<const std::byte> d,
                                 key32_t& o) { return hmac_from_hash(&hash, k, d, o); },
                              ck, ikm, out);
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

    /** @brief A ChaCha20-Poly1305 cipher: one `EVP_CIPHER_CTX`, allocated when it is built. */
    struct aead_t {
        /** @brief Allocate the context and set its cipher; a failure shows at @ref set_key. */
        aead_t() : ctx_(EVP_CIPHER_CTX_new()) {
            if (ctx_ != nullptr && EVP_CipherInit_ex(ctx_, EVP_chacha20_poly1305(), nullptr,
                                                     nullptr, nullptr, 1) != 1) {
                EVP_CIPHER_CTX_free(ctx_);
                ctx_ = nullptr;
            }
        }
        aead_t(const aead_t&) = delete;
        aead_t& operator=(const aead_t&) = delete;
        ~aead_t() { EVP_CIPHER_CTX_free(ctx_); }

        /** @brief Key the context in place, replacing any previous key (no allocation). */
        [[nodiscard]] bool set_key(const key32_t& k) {
            return ctx_ != nullptr &&
                   EVP_CipherInit_ex(ctx_, nullptr, nullptr,
                                     reinterpret_cast<const unsigned char*>(k.data()), nullptr,
                                     -1) == 1;
        }

        /** @brief Seal @p pt under nonce @p n; @p out takes `pt.size() + kTagLen` bytes. */
        [[nodiscard]] bool seal(std::uint64_t n, std::span<const std::byte> ad,
                                std::span<const std::byte> pt, std::byte* out) {
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
                                std::span<const std::byte> ct, std::byte* out) {
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
        EVP_CIPHER_CTX* ctx_; /**< @brief The context, or null if building it failed. */
    };
};

static_assert(crypto_backend<openssl_crypto_t>);

}  // namespace tr::net::noise
