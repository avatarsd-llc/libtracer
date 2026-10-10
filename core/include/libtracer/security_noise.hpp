/**
 * @file
 * @brief The `Noise_NNpsk0_25519_ChaChaPoly_SHA256` handshake and transport steps of RFC-0033,
 *        written once over a compile-time crypto backend (#2072).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This is the cryptographic half of the Noise link binding: the symmetric state, the two
 * handshake messages, `Split()` and the transport messages, with the datagram layout of
 * RFC-0033 §5.3. The link half (the slots, the counter and the mark, the replay window, the
 * key phase, the `now` deadlines and the counters) builds on it. Everything here works on
 * caller memory: no allocation, no buffer of its own, no clock and no randomness. The
 * ephemeral private keys come from the caller, who draws them from an injected random source
 * (§5.2).
 *
 * @section noise_pattern The pattern
 *
 *     NNpsk0:
 *       -> psk, e
 *       <- e, ee
 *
 * The protocol name is 36 bytes, longer than `HASHLEN`, so the initial `h` is its SHA-256. The
 * prologue is the 20 bytes `Tracer over Noise v1`. Because a PSK is in play, every `e` token
 * also runs `MixKey(e.pub)` (Noise §9). The state after the prologue and the `psk` token is the
 * same for every handshake on a link, so @ref tr::net::noise::psk_state computes it once and
 * each handshake starts from a copy (§5.8).
 *
 * @section noise_responder_order The responder's order
 *
 * @ref tr::net::noise::handshake_t::read_first checks the first message's tag with no
 * Diffie-Hellman and no key generation. Its caller then checks the counter against its mark
 * and only then calls @ref tr::net::noise::handshake_t::write_second, which draws on the
 * ephemeral key and runs `ee`. A message under the wrong PSK, or a replay, therefore costs one
 * `MixHash`, one HKDF and one tag check (§5.4, §5.8).
 *
 * @section noise_backend The backend
 *
 * `B` meets @ref tr::net::noise::crypto_backend (`security_noise_crypto.hpp`). The build
 * names one, and `tr::net::noise::default_crypto_t` aliases it:
 *
 * | `LIBTRACER_NOISE_CRYPTO` | backend | where |
 * | --- | --- | --- |
 * | `openssl` | `openssl_crypto_t` (`security_noise_openssl.hpp`) | host |
 * | `sodium` | `sodium_crypto_t` (`security_noise_sodium.hpp`) | host |
 * | `psa` | `psa_crypto_t` (`security_noise_psa.hpp`), mbedTLS through PSA | ESP-IDF, host |
 * | `none` (default) | nothing: this module is not compiled | everywhere |
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>

#include "libtracer/security_noise_crypto.hpp"

#if defined(LIBTRACER_NOISE_CRYPTO_OPENSSL)
#include "libtracer/security_noise_openssl.hpp"
#elif defined(LIBTRACER_NOISE_CRYPTO_SODIUM)
#include "libtracer/security_noise_sodium.hpp"
#elif defined(LIBTRACER_NOISE_CRYPTO_PSA)
#include "libtracer/security_noise_psa.hpp"
#endif

namespace tr::net::noise {

/** @brief The protocol name, hashed into the initial `h` and `ck` (RFC-0033 §5.2). */
inline constexpr std::string_view kProtocolName = "Noise_NNpsk0_25519_ChaChaPoly_SHA256";
/** @brief The prologue, `Tracer over Noise v1` (RFC-0033 §5.2). */
inline constexpr std::string_view kPrologue = "Tracer over Noise v1";

/** @brief A first handshake message's datagram: type, `e.pub`, payload (9) and tag. */
inline constexpr std::size_t kFirstMessageBytes = 1 + kDhLen + 9 + kTagLen;
/** @brief A second handshake message's datagram: type, `e.pub`, payload (1) and tag. */
inline constexpr std::size_t kSecondMessageBytes = 1 + kDhLen + 1 + kTagLen;
/** @brief A transport datagram's clear header: the type byte and the u64 nonce. */
inline constexpr std::size_t kTransportHeaderBytes = 1 + 8;
/** @brief What a transport datagram adds to its frame: the header and the tag. */
inline constexpr std::size_t kTransportOverhead = kTransportHeaderBytes + kTagLen;
/** @brief The largest frame one Noise message carries: 65535 minus the tag (§5.3). */
inline constexpr std::size_t kMaxFrameBytes = 65535 - kTagLen;
/** @brief No transport nonce at or above this is sent or decrypted (§5.5). */
inline constexpr std::uint64_t kNonceLimit = std::uint64_t{1} << 62;

static_assert(kFirstMessageBytes == 58 && kSecondMessageBytes == 50 && kTransportOverhead == 25,
              "RFC-0033 §5.3 datagram sizes");

/** @brief The datagram type byte (RFC-0033 §5.3); every other value is reserved. */
enum class msg_type_t : std::uint8_t {
    FIRST = 0x01,             /**< @brief Handshake, first message (`-> psk, e`). */
    SECOND = 0x02,            /**< @brief Handshake, second message (`<- e, ee`). */
    TRANSPORT_PHASE_0 = 0x04, /**< @brief Transport message, key phase 0. */
    TRANSPORT_PHASE_1 = 0x05, /**< @brief Transport message, key phase 1. */
};

/**
 * @brief Why a step refused, mapped by the link onto RFC-0033 §5.9's counters.
 *
 * `MALFORMED` and `NONCE_BOUND` are `malformed_rx`. `AUTH`, `PAYLOAD` and `LOW_ORDER` on a
 * handshake message are `noise_handshake_failed`; `AUTH` on a transport message is
 * `noise_decrypt_failed`. `STATE` is a caller error, and `BACKEND` a library failure.
 */
enum class refusal_t : std::uint8_t {
    MALFORMED,   /**< @brief Wrong size or reserved type byte; nothing was decrypted. */
    AUTH,        /**< @brief The tag did not verify. */
    PAYLOAD,     /**< @brief An authentic handshake payload with reserved flag bits set. */
    LOW_ORDER,   /**< @brief The peer's ephemeral key gave an all-zero X25519 result. */
    NONCE_BOUND, /**< @brief A transport nonce at or above 2^62. */
    STATE,       /**< @brief The step was called out of order for this role. */
    BACKEND,     /**< @brief The crypto library failed. */
};

/** @brief Which side of the handshake a state plays; a DIAL link initiates. */
enum class role_t : std::uint8_t {
    INITIATOR, /**< @brief Writes the first message, reads the second. */
    RESPONDER, /**< @brief Reads the first message, writes the second. */
};

/** @brief The first message's 9-byte payload (RFC-0033 §5.2). */
struct first_payload_t {
    std::uint8_t flags = 0;    /**< @brief Bit 0 `fresh`; bits 7-1 zero. */
    std::uint64_t counter = 0; /**< @brief The initiator counter, u64 little-endian on the wire. */
};

/** @brief The 9 payload bytes of @p p: the flags byte, then the counter little-endian. */
[[nodiscard]] constexpr std::array<std::byte, 9> encode_first_payload(first_payload_t p) noexcept {
    std::array<std::byte, 9> out{};
    out[0] = static_cast<std::byte>(p.flags);
    for (std::size_t i = 0; i < 8; ++i)
        out[1 + i] = static_cast<std::byte>((p.counter >> (8 * i)) & 0xFF);
    return out;
}

/**
 * @brief Noise's SymmetricState (Noise §5.2): the chaining key, the handshake hash and the
 *        handshake cipher key with its nonce.
 *
 * The cipher key is held as bytes, and each `EncryptAndHash` / `DecryptAndHash` keys a
 * backend cipher for its one message, so the state is a plain value: it copies, and a copy
 * of the state after the PSK starts each handshake. Its destructor wipes it.
 *
 * @tparam B The crypto backend.
 */
template <crypto_backend B>
class symmetric_state_t {
   public:
    /** @brief The largest input @ref mix_hash takes: a public key or a hash. */
    static constexpr std::size_t kMaxMixHash = 64;

    symmetric_state_t() = default;
    /** @brief Copy the state (the handshake start). */
    symmetric_state_t(const symmetric_state_t&) = default;
    /** @brief Copy-assign the state. */
    symmetric_state_t& operator=(const symmetric_state_t&) = default;
    ~symmetric_state_t() {
        wipe(ck_);
        wipe(k_);
    }

    /**
     * @brief Noise `InitializeSymmetric(protocol_name)` for a name longer than `HASHLEN`.
     * @return False if the name is not longer than 32 bytes or the hash failed.
     */
    [[nodiscard]] bool initialize(std::span<const std::byte> protocol_name) {
        has_key_ = false;
        n_ = 0;
        if (protocol_name.size() <= kHashLen || !B::hash(protocol_name, h_)) return false;
        ck_ = h_;
        return true;
    }

    /** @brief `h = HASH(h || data)`, for @p data up to @ref kMaxMixHash bytes. */
    [[nodiscard]] bool mix_hash(std::span<const std::byte> data) {
        std::array<std::byte, kHashLen + kMaxMixHash> buf{};
        if (data.size() > kMaxMixHash) return false;
        std::memcpy(buf.data(), h_.data(), kHashLen);
        if (!data.empty()) std::memcpy(buf.data() + kHashLen, data.data(), data.size());
        return B::hash(std::span<const std::byte>(buf.data(), kHashLen + data.size()), h_);
    }

    /** @brief `ck, k = HKDF(ck, ikm, 2)`, then `InitializeKey(k)`. */
    [[nodiscard]] bool mix_key(std::span<const std::byte> ikm) {
        std::array<std::byte, 2 * kHashLen> out{};
        const bool ok = B::hkdf(ck_, ikm, out);
        std::memcpy(ck_.data(), out.data(), kHashLen);
        std::memcpy(k_.data(), out.data() + kHashLen, kHashLen);
        wipe(out);
        has_key_ = ok;
        n_ = 0;
        return ok;
    }

    /** @brief `ck, temp_h, k = HKDF(ck, ikm, 3)`, `MixHash(temp_h)`, `InitializeKey(k)`. */
    [[nodiscard]] bool mix_key_and_hash(std::span<const std::byte> ikm) {
        std::array<std::byte, 3 * kHashLen> out{};
        bool ok = B::hkdf(ck_, ikm, out);
        std::memcpy(ck_.data(), out.data(), kHashLen);
        std::memcpy(k_.data(), out.data() + 2 * kHashLen, kHashLen);
        ok = ok && mix_hash(std::span<const std::byte>(out.data() + kHashLen, kHashLen));
        wipe(out);
        has_key_ = ok;
        n_ = 0;
        return ok;
    }

    /**
     * @brief Noise `EncryptAndHash`: seal @p pt with `h` as associated data into @p out
     *        (`pt.size() + kTagLen` bytes), then mix the ciphertext into `h`.
     *
     * NNpsk0's first token is `psk`, so a key is always set before any payload: the unkeyed
     * pass-through Noise defines for other patterns is refused here instead of carried.
     */
    [[nodiscard]] bool encrypt_and_hash(std::span<const std::byte> pt, std::byte* out) {
        const std::size_t ct_len = pt.size() + kTagLen;
        typename B::aead_t aead;
        if (!has_key_ || ct_len > kMaxMixHash || !aead.set_key(k_) || !aead.seal(n_, h_, pt, out))
            return false;
        ++n_;
        return mix_hash(std::span<const std::byte>(out, ct_len));
    }

    /**
     * @brief Noise `DecryptAndHash`: open @p ct with `h` as associated data into @p out
     *        (`ct.size() - kTagLen` bytes), then mix the ciphertext into `h`.
     * @return False on a bad tag, or with no key set; `h` and the nonce are then unchanged.
     */
    [[nodiscard]] bool decrypt_and_hash(std::span<const std::byte> ct, std::byte* out) {
        typename B::aead_t aead;
        if (!has_key_ || ct.size() < kTagLen || ct.size() > kMaxMixHash || !aead.set_key(k_) ||
            !aead.open(n_, h_, ct, out))
            return false;
        ++n_;
        return mix_hash(ct);
    }

    /** @brief Noise `Split()`: @p k1 encrypts initiator to responder, @p k2 the reverse. */
    [[nodiscard]] bool split(key32_t& k1, key32_t& k2) const {
        std::array<std::byte, 2 * kHashLen> out{};
        const bool ok = B::hkdf(ck_, {}, out);
        std::memcpy(k1.data(), out.data(), kHashLen);
        std::memcpy(k2.data(), out.data() + kHashLen, kHashLen);
        wipe(out);
        return ok;
    }

    /** @brief The chaining key `ck`. */
    [[nodiscard]] const key32_t& chaining_key() const noexcept { return ck_; }
    /** @brief The handshake hash `h`; after the last message, the channel binding. */
    [[nodiscard]] const key32_t& handshake_hash() const noexcept { return h_; }
    /** @brief The handshake cipher key `k`, meaningful only while @ref has_key holds. */
    [[nodiscard]] const key32_t& cipher_key() const noexcept { return k_; }
    /** @brief Whether a cipher key is set (Noise `HasKey`). */
    [[nodiscard]] bool has_key() const noexcept { return has_key_; }

   private:
    key32_t ck_{};         /**< @brief The chaining key. */
    key32_t h_{};          /**< @brief The handshake hash. */
    key32_t k_{};          /**< @brief The handshake cipher key. */
    std::uint64_t n_ = 0;  /**< @brief The handshake cipher nonce. */
    bool has_key_ = false; /**< @brief Whether the cipher key is set. */
};

/**
 * @brief The symmetric state every handshake on a link starts from: `InitializeSymmetric`,
 *        `MixHash(prologue)` and the `psk` token's `MixKeyAndHash(psk)` (RFC-0033 §5.8).
 *
 * The link computes it once when it is built, and again only when its PSK is replaced.
 */
template <crypto_backend B>
[[nodiscard]] bool psk_state(const key32_t& psk, symmetric_state_t<B>& out) {
    return out.initialize(std::as_bytes(std::span(kProtocolName))) &&
           out.mix_hash(std::as_bytes(std::span(kPrologue))) && out.mix_key_and_hash(psk);
}

/** @brief A transport datagram's clear header, read before any decryption (§5.5). */
struct transport_header_t {
    std::uint8_t phase = 0;  /**< @brief The key phase, 0 or 1 (from the type byte). */
    std::uint64_t nonce = 0; /**< @brief The nonce, below @ref kNonceLimit. */
};

/**
 * @brief Read a transport datagram's type and nonce, with no decryption: the steps that come
 *        before the replay pre-check (RFC-0033 §5.5 steps 1 and 2).
 * @retval MALFORMED   Fewer than 25 bytes, or a type byte other than 0x04 / 0x05.
 * @retval NONCE_BOUND The nonce is at or above 2^62.
 */
[[nodiscard]] inline std::expected<transport_header_t, refusal_t> parse_transport_header(
    std::span<const std::byte> datagram) noexcept {
    if (datagram.size() < kTransportOverhead) return std::unexpected(refusal_t::MALFORMED);
    const auto type = static_cast<std::uint8_t>(datagram[0]);
    if (type != std::uint8_t(msg_type_t::TRANSPORT_PHASE_0) &&
        type != std::uint8_t(msg_type_t::TRANSPORT_PHASE_1))
        return std::unexpected(refusal_t::MALFORMED);
    std::uint64_t n = 0;
    for (std::size_t i = 0; i < 8; ++i) n |= std::uint64_t(datagram[1 + i]) << (8 * i);
    if (n >= kNonceLimit) return std::unexpected(refusal_t::NONCE_BOUND);
    return transport_header_t{.phase = std::uint8_t(type & 1), .nonce = n};
}

/**
 * @brief The two transport keys of one session, keyed into the backend once at `Split()`:
 *        seal toward the peer, open from it (Noise §11.4, explicit nonces).
 *
 * Nonce choice, the replay window and the key phase are the link's (RFC-0033 §5.5, §5.6);
 * this type applies a given nonce and phase. `seal` and `open` allocate nothing.
 *
 * @tparam B The crypto backend.
 */
template <crypto_backend B>
class transport_cipher_t {
   public:
    /** @brief Key both directions (what `Split()` feeds). */
    [[nodiscard]] bool set_keys(const key32_t& send, const key32_t& recv) {
        return send_.set_key(send) && recv_.set_key(recv);
    }

    /**
     * @brief Write one transport datagram: the type byte for @p phase, nonce @p n, then
     *        @p frame sealed with empty associated data, and its tag.
     *
     * @p frame may already sit at `out.data() + kTransportHeaderBytes`, which seals in place;
     * any other overlap with @p out is undefined.
     *
     * @return The datagram size, `frame.size() + kTransportOverhead`.
     * @retval MALFORMED   @p phase is not 0 or 1, the frame is above @ref kMaxFrameBytes, or
     *                     @p out is too small.
     * @retval NONCE_BOUND @p n is at or above 2^62.
     */
    [[nodiscard]] std::expected<std::size_t, refusal_t> seal(std::uint8_t phase, std::uint64_t n,
                                                             std::span<const std::byte> frame,
                                                             std::span<std::byte> out) const {
        if (n >= kNonceLimit) return std::unexpected(refusal_t::NONCE_BOUND);
        if (phase > 1 || frame.size() > kMaxFrameBytes ||
            out.size() < frame.size() + kTransportOverhead)
            return std::unexpected(refusal_t::MALFORMED);
        if (!send_.seal(n, {}, frame, out.data() + kTransportHeaderBytes))
            return std::unexpected(refusal_t::BACKEND);
        out[0] = static_cast<std::byte>(std::uint8_t(msg_type_t::TRANSPORT_PHASE_0) | phase);
        for (std::size_t i = 0; i < 8; ++i)
            out[1 + i] = static_cast<std::byte>((n >> (8 * i)) & 0xFF);
        return frame.size() + kTransportOverhead;
    }

    /**
     * @brief Check the tag of @p datagram and decrypt it in place.
     * @param h The header @ref parse_transport_header read from @p datagram.
     * @return The plaintext, inside @p datagram at offset @ref kTransportHeaderBytes; empty
     *         for a confirmation or an acknowledgement.
     * @retval AUTH The tag did not verify (the bytes are then unspecified).
     */
    [[nodiscard]] std::expected<std::span<std::byte>, refusal_t> open(
        const transport_header_t& h, std::span<std::byte> datagram) const {
        if (datagram.size() < kTransportOverhead) return std::unexpected(refusal_t::MALFORMED);
        const auto ct = datagram.subspan(kTransportHeaderBytes);
        if (!recv_.open(h.nonce, {}, ct, ct.data())) return std::unexpected(refusal_t::AUTH);
        return ct.first(ct.size() - kTagLen);
    }

   private:
    typename B::aead_t send_{}; /**< @brief Toward the peer. */
    typename B::aead_t recv_{}; /**< @brief From the peer. */
};

/**
 * @brief One side's NNpsk0 HandshakeState over RFC-0033's datagrams.
 *
 * The initiator calls @ref write_first, then @ref read_second; the responder calls
 * @ref read_first, then @ref write_second. Either then calls @ref split. A refused step
 * leaves the state where it was, so a link can read a datagram into a scratch copy and keep
 * its handshake slot untouched when it is refused.
 *
 * @tparam B The crypto backend.
 */
template <crypto_backend B>
class handshake_t {
   public:
    /**
     * @brief Start a handshake from the link's PSK state (@ref psk_state).
     * @param role This side.
     * @param keyed The state after the prologue and the PSK; copied.
     */
    handshake_t(role_t role, const symmetric_state_t<B>& keyed) : role_(role), ss_(keyed) {}
    handshake_t(const handshake_t&) = delete;
    handshake_t& operator=(const handshake_t&) = delete;

    /**
     * @brief Initiator: write `-> psk, e` with @p payload into the 58-byte @p out.
     * @param e_priv The ephemeral private key, from the application's random source.
     */
    [[nodiscard]] std::expected<void, refusal_t> write_first(
        const key32_t& e_priv, first_payload_t payload,
        std::span<std::byte, kFirstMessageBytes> out) {
        if (role_ != role_t::INITIATOR || step_ != step_t::FIRST)
            return std::unexpected(refusal_t::STATE);
        key32_t pub{};
        if (!e_.set(e_priv, pub)) return std::unexpected(refusal_t::BACKEND);
        symmetric_state_t<B> next = ss_;
        const auto p = encode_first_payload(payload);
        if (!next.mix_hash(pub) || !next.mix_key(pub) ||
            !next.encrypt_and_hash(p, out.data() + 1 + kDhLen))
            return std::unexpected(refusal_t::BACKEND);
        out[0] = static_cast<std::byte>(msg_type_t::FIRST);
        std::memcpy(out.data() + 1, pub.data(), kDhLen);
        ss_ = next;
        step_ = step_t::SECOND;
        return {};
    }

    /**
     * @brief Responder: check and read a first message, with no Diffie-Hellman.
     * @return The authentic payload; the caller checks its counter before answering.
     * @retval MALFORMED Not 58 bytes, or not type 0x01.
     * @retval AUTH      The PSK tag did not verify.
     * @retval PAYLOAD   Flag bits 7-1 are set.
     */
    [[nodiscard]] std::expected<first_payload_t, refusal_t> read_first(
        std::span<const std::byte> datagram) {
        if (role_ != role_t::RESPONDER || step_ != step_t::FIRST)
            return std::unexpected(refusal_t::STATE);
        if (datagram.size() != kFirstMessageBytes ||
            datagram[0] != static_cast<std::byte>(msg_type_t::FIRST))
            return std::unexpected(refusal_t::MALFORMED);
        symmetric_state_t<B> next = ss_;
        std::array<std::byte, 9> p{};
        key32_t re{};
        std::memcpy(re.data(), datagram.data() + 1, kDhLen);
        if (!next.mix_hash(re) || !next.mix_key(re)) return std::unexpected(refusal_t::BACKEND);
        if (!next.decrypt_and_hash(datagram.subspan(1 + kDhLen), p.data()))
            return std::unexpected(refusal_t::AUTH);
        first_payload_t out{.flags = std::uint8_t(p[0])};
        for (std::size_t i = 0; i < 8; ++i) out.counter |= std::uint64_t(p[1 + i]) << (8 * i);
        if ((out.flags & 0xFE) != 0) return std::unexpected(refusal_t::PAYLOAD);
        ss_ = next;
        re_ = re;
        step_ = step_t::SECOND;
        return out;
    }

    /**
     * @brief Responder: write `<- e, ee` with the @p flags byte into the 50-byte @p out.
     * @param e_priv The ephemeral private key, from the application's random source.
     * @retval LOW_ORDER `DH(e, re)` is all zero; nothing is written.
     */
    [[nodiscard]] std::expected<void, refusal_t> write_second(
        const key32_t& e_priv, std::uint8_t flags, std::span<std::byte, kSecondMessageBytes> out) {
        if (role_ != role_t::RESPONDER || step_ != step_t::SECOND)
            return std::unexpected(refusal_t::STATE);
        key32_t pub{};
        if (!e_.set(e_priv, pub)) return std::unexpected(refusal_t::BACKEND);
        symmetric_state_t<B> next = ss_;
        if (!next.mix_hash(pub) || !next.mix_key(pub)) return std::unexpected(refusal_t::BACKEND);
        if (const auto r = mix_ee(next, re_); !r) return r;
        std::array<std::byte, 1 + kTagLen> ct{};
        const std::array p{static_cast<std::byte>(flags)};
        if (!next.encrypt_and_hash(p, ct.data())) return std::unexpected(refusal_t::BACKEND);
        out[0] = static_cast<std::byte>(msg_type_t::SECOND);
        std::memcpy(out.data() + 1, pub.data(), kDhLen);
        std::memcpy(out.data() + 1 + kDhLen, ct.data(), ct.size());
        ss_ = next;
        step_ = step_t::DONE;
        return {};
    }

    /**
     * @brief Initiator: check and read a second message.
     * @return The authentic flags byte (bit 0 the key phase, bit 1 `fresh`).
     * @retval MALFORMED Not 50 bytes, or not type 0x02.
     * @retval LOW_ORDER `DH(e, re)` is all zero.
     * @retval AUTH      The tag did not verify.
     * @retval PAYLOAD   Flag bits 7-2 are set.
     */
    [[nodiscard]] std::expected<std::uint8_t, refusal_t> read_second(
        std::span<const std::byte> datagram) {
        if (role_ != role_t::INITIATOR || step_ != step_t::SECOND)
            return std::unexpected(refusal_t::STATE);
        if (datagram.size() != kSecondMessageBytes ||
            datagram[0] != static_cast<std::byte>(msg_type_t::SECOND))
            return std::unexpected(refusal_t::MALFORMED);
        symmetric_state_t<B> next = ss_;
        key32_t re{};
        std::memcpy(re.data(), datagram.data() + 1, kDhLen);
        if (!next.mix_hash(re) || !next.mix_key(re)) return std::unexpected(refusal_t::BACKEND);
        if (const auto r = mix_ee(next, re); !r) return std::unexpected(r.error());
        std::array<std::byte, 1> p{};
        if (!next.decrypt_and_hash(datagram.subspan(1 + kDhLen), p.data()))
            return std::unexpected(refusal_t::AUTH);
        const auto flags = std::uint8_t(p[0]);
        if ((flags & 0xFC) != 0) return std::unexpected(refusal_t::PAYLOAD);
        ss_ = next;
        re_ = re;
        step_ = step_t::DONE;
        return flags;
    }

    /** @brief After the last message: key @p out for this side's two directions. */
    [[nodiscard]] std::expected<void, refusal_t> split(transport_cipher_t<B>& out) const {
        if (step_ != step_t::DONE) return std::unexpected(refusal_t::STATE);
        key32_t k1{};
        key32_t k2{};
        const bool init = role_ == role_t::INITIATOR;
        const bool ok = ss_.split(k1, k2) && out.set_keys(init ? k1 : k2, init ? k2 : k1);
        wipe(k1);
        wipe(k2);
        if (!ok) return std::unexpected(refusal_t::BACKEND);
        return {};
    }

    /** @brief The symmetric state so far; after the last message, `h` is the handshake hash. */
    [[nodiscard]] const symmetric_state_t<B>& symmetric_state() const noexcept { return ss_; }

   private:
    /** @brief Which message this side handles next. */
    enum class step_t : std::uint8_t {
        FIRST,  /**< @brief The first message. */
        SECOND, /**< @brief The second message. */
        DONE,   /**< @brief Both messages; `Split()` is next. */
    };

    /** @brief The `ee` token with peer key @p re into @p ss, refusing an all-zero result
     *         (RFC-0033 §5.2). */
    [[nodiscard]] std::expected<void, refusal_t> mix_ee(symmetric_state_t<B>& ss,
                                                        const key32_t& re) const {
        key32_t shared{};
        const bool agreed = e_.agree(re, shared);
        const bool low = !agreed || all_zero(shared);
        const bool ok = !low && ss.mix_key(shared);
        wipe(shared);
        if (low) return std::unexpected(refusal_t::LOW_ORDER);
        if (!ok) return std::unexpected(refusal_t::BACKEND);
        return {};
    }

    role_t role_;                 /**< @brief This side. */
    step_t step_ = step_t::FIRST; /**< @brief The next message. */
    symmetric_state_t<B> ss_;     /**< @brief The symmetric state. */
    typename B::dh_key_t e_{};    /**< @brief This side's ephemeral key pair. */
    key32_t re_{};                /**< @brief The peer's ephemeral public key. */
};

#if defined(LIBTRACER_NOISE_CRYPTO_OPENSSL)
/** @brief The build's crypto backend (`LIBTRACER_NOISE_CRYPTO=openssl`). */
using default_crypto_t = openssl_crypto_t;
#elif defined(LIBTRACER_NOISE_CRYPTO_SODIUM)
/** @brief The build's crypto backend (`LIBTRACER_NOISE_CRYPTO=sodium`). */
using default_crypto_t = sodium_crypto_t;
#elif defined(LIBTRACER_NOISE_CRYPTO_PSA)
/** @brief The build's crypto backend (`LIBTRACER_NOISE_CRYPTO=psa`). */
using default_crypto_t = psa_crypto_t;
#endif

}  // namespace tr::net::noise
