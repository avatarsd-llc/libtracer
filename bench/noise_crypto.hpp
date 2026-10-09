/**
 * @file
 * @brief A `Noise_NNpsk0_25519_ChaChaPoly_SHA256` handshake and transport cipher, written once
 *        over a pluggable crypto backend, for the Noise link harness (#2065).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The cost the Noise link binding (RFC-0033, #2063; implemented by #2064) adds to a datagram
 * link is two things: the handshake a session opens with, and the AEAD seal and open every
 * frame pays after it. This header spells both exactly as the Noise Protocol Framework
 * (revision 34) states them, so the harness times the real sequence of primitive calls rather
 * than a sum of guessed ones, and it does so over a BACKEND parameter so the same sequence runs
 * on every crypto library a node could ship (`noise_crypto_backends.hpp`).
 *
 * @section pattern The pattern
 *
 *     NNpsk0:
 *       -> psk, e
 *       <- e, ee
 *
 * The protocol name is 36 bytes, longer than HASHLEN, so `h = SHA-256(name)`; the prologue is
 * empty and still mixed. Because a PSK is in play, every `e` token also runs `MixKey(e.pub)`.
 * Both handshake messages carry an encrypted payload, empty by default, so each is
 * `e.pub (32) || tag (16)` = 48 bytes. `Split()` gives the initiator-to-responder key first.
 *
 * @section deterministic Ephemeral keys are injected
 *
 * `write_e` takes the ephemeral PRIVATE key from the caller instead of drawing it from a random
 * source. Two reasons: the cross-backend check below compares whole transcripts byte for byte,
 * which only works with fixed ephemerals; and the timed rows then price the X25519 scalar
 * multiplications without one backend's RNG (a getrandom syscall in one library, a DRBG in
 * another) in the figure. A real link draws them from its RNG; that cost is not in these rows.
 *
 * @section backend The backend contract
 *
 * A backend `B` is a type with only static members plus two nested state types:
 *
 *  - `B::kName` — the row's `system` column;
 *  - `B::init()` — one-time library setup, `false` if the library is unusable;
 *  - `B::hash(in, out)` — SHA-256 of one contiguous input;
 *  - `B::hkdf(ck, ikm, out)` — the Noise `HKDF(ck, ikm, n)` for `out.size() == 32 * n`, which is
 *    RFC 5869 HKDF-SHA-256 with `salt = ck`, an empty `info` and `L = 32 * n`;
 *  - `B::dh_key_t` — an X25519 key pair, with `bool set(priv, pub_out)`,
 *    `bool agree(peer_pub, out)` and a destructor that wipes or releases it;
 *  - `B::aead_t` — a ChaCha20-Poly1305 cipher state keyed once with `bool set_key(k)`, then
 *    `bool seal(n, ad, pt, out)` and `bool open(n, ad, ct, out)` for the 64-bit Noise nonce `n`.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace bench::noise {

/** @brief SHA-256 output length, Noise's HASHLEN. */
inline constexpr std::size_t kHashLen = 32;
/** @brief X25519 key and shared-secret length, Noise's DHLEN. */
inline constexpr std::size_t kDhLen = 32;
/** @brief ChaCha20-Poly1305 key length. */
inline constexpr std::size_t kKeyLen = 32;
/** @brief Poly1305 tag length. */
inline constexpr std::size_t kTagLen = 16;
/** @brief The IETF ChaCha20-Poly1305 nonce length. */
inline constexpr std::size_t kNonceLen = 12;
/** @brief Largest handshake payload this harness carries in a handshake message. */
inline constexpr std::size_t kMaxHandshakePayload = 64;
/** @brief A handshake message's size: the ephemeral key, the payload and its tag. */
inline constexpr std::size_t kHandshakeMsgMax = kDhLen + kMaxHandshakePayload + kTagLen;

/** @brief The full protocol name, hashed into the initial `h` and `ck`. */
inline constexpr std::string_view kProtocolName = "Noise_NNpsk0_25519_ChaChaPoly_SHA256";

/** @brief A 32-byte value: a hash, a key, a public key or a shared secret. */
using key32_t = std::array<std::byte, 32>;

/**
 * @brief The Noise ChaChaPoly nonce: four zero bytes, then the 64-bit counter little-endian.
 * @param n The Noise nonce counter.
 * @return The 12-byte AEAD nonce.
 */
[[nodiscard]] inline std::array<std::byte, kNonceLen> chachapoly_nonce(std::uint64_t n) {
    std::array<std::byte, kNonceLen> out{};
    for (std::size_t i = 0; i < 8; ++i) out[4 + i] = static_cast<std::byte>((n >> (8 * i)) & 0xFF);
    return out;
}

/**
 * @brief The Noise `HKDF(ck, ikm, n)` built from one HMAC-SHA-256 primitive, for a backend
 *        whose library has HMAC but no HKDF call.
 *
 * `temp = HMAC(ck, ikm)`, `out1 = HMAC(temp, 0x01)`, `out_i = HMAC(temp, out_{i-1} || i)`.
 *
 * @param hmac `bool(const key32_t& key, std::span<const std::byte> data, key32_t& out)`.
 * @param ck   The chaining key.
 * @param ikm  The input key material (empty for `Split`).
 * @param out  `32 * n` bytes, `n` in 1..3.
 * @return False if an HMAC call failed.
 */
template <class Hmac>
[[nodiscard]] bool hkdf_from_hmac(Hmac&& hmac, const key32_t& ck, std::span<const std::byte> ikm,
                                  std::span<std::byte> out) {
    key32_t temp{};
    if (!hmac(ck, ikm, temp)) return false;
    std::array<std::byte, kHashLen + 1> in{};
    std::size_t in_len = 0;
    key32_t block{};
    for (std::size_t i = 0; i * kHashLen < out.size(); ++i) {
        in[in_len] = static_cast<std::byte>(i + 1);
        if (!hmac(temp, std::span<const std::byte>(in.data(), in_len + 1), block)) return false;
        std::memcpy(out.data() + i * kHashLen, block.data(), kHashLen);
        std::memcpy(in.data(), block.data(), kHashLen);
        in_len = kHashLen;
    }
    return true;
}

/**
 * @brief The Noise CipherState: a key and its nonce counter, over a backend's AEAD state.
 * @tparam B The crypto backend (see the file comment).
 */
template <class B>
class cipher_state_t {
   public:
    /** @brief Key the state and reset the nonce (Noise `InitializeKey`). */
    [[nodiscard]] bool initialize_key(const key32_t& k) {
        n_ = 0;
        has_key_ = aead_.set_key(k);
        return has_key_;
    }

    /** @brief True once a key is set (Noise `HasKey`). */
    [[nodiscard]] bool has_key() const { return has_key_; }

    /**
     * @brief Noise `EncryptWithAd`: seal @p pt into @p out (`pt.size() + kTagLen` bytes) and
     *        advance the nonce. Without a key the plaintext is copied through, as Noise states.
     */
    [[nodiscard]] bool encrypt_with_ad(std::span<const std::byte> ad, std::span<const std::byte> pt,
                                       std::byte* out) {
        if (!has_key_) {
            if (!pt.empty()) std::memcpy(out, pt.data(), pt.size());
            return true;
        }
        if (!aead_.seal(n_, ad, pt, out)) return false;
        ++n_;
        return true;
    }

    /**
     * @brief Noise `DecryptWithAd`: open @p ct into @p out (`ct.size() - kTagLen` bytes) and
     *        advance the nonce; false on an authentication failure, nonce unchanged.
     */
    [[nodiscard]] bool decrypt_with_ad(std::span<const std::byte> ad, std::span<const std::byte> ct,
                                       std::byte* out) {
        if (!has_key_) {
            if (!ct.empty()) std::memcpy(out, ct.data(), ct.size());
            return true;
        }
        if (ct.size() < kTagLen || !aead_.open(n_, ad, ct, out)) return false;
        ++n_;
        return true;
    }

    /** @brief The next nonce this state will use. */
    [[nodiscard]] std::uint64_t nonce() const { return n_; }

    /** @brief The backend state, for a row that drives the AEAD directly. */
    [[nodiscard]] typename B::aead_t& aead() { return aead_; }

   private:
    typename B::aead_t aead_{};
    std::uint64_t n_ = 0;
    bool has_key_ = false;
};

/** @brief Which side of the handshake a state plays. */
enum class role_t : std::uint8_t {
    INITIATOR, /**< @brief Sends `psk, e`. */
    RESPONDER  /**< @brief Answers `e, ee`. */
};

/**
 * @brief One side's NNpsk0 HandshakeState (with its SymmetricState folded in).
 *
 * The four message steps are `write_msg1` / `read_msg1` / `write_msg2` / `read_msg2`, called
 * in that order across the two sides; `split` then yields the transport pair. Each step
 * returns the bytes written or read, or 0 on failure.
 *
 * @tparam B The crypto backend (see the file comment).
 */
template <class B>
class handshake_t {
   public:
    /**
     * @brief Begin a handshake (Noise `Initialize` with an empty prologue).
     * @param role This side.
     * @param psk  The pre-shared key.
     * @param e_priv This side's ephemeral PRIVATE key (see @ref deterministic).
     */
    [[nodiscard]] bool start(role_t role, const key32_t& psk, const key32_t& e_priv) {
        role_ = role;
        psk_ = psk;
        e_priv_ = e_priv;
        const auto name = std::as_bytes(std::span(kProtocolName.data(), kProtocolName.size()));
        B::hash(name, h_);
        ck_ = h_;
        return mix_hash({});
    }

    /** @brief Initiator: `-> psk, e` plus the encrypted @p payload; returns the message size. */
    [[nodiscard]] std::size_t write_msg1(std::span<const std::byte> payload, std::byte* out) {
        if (role_ != role_t::INITIATOR || !mix_key_and_hash(psk_)) return 0;
        return write_e_and_payload(payload, out, /*with_ee=*/false);
    }

    /** @brief Responder: read `psk, e`; the decrypted payload lands in @p payload_out. */
    [[nodiscard]] std::size_t read_msg1(std::span<const std::byte> msg, std::byte* payload_out) {
        if (role_ != role_t::RESPONDER || !mix_key_and_hash(psk_)) return 0;
        return read_e_and_payload(msg, payload_out, /*with_ee=*/false);
    }

    /** @brief Responder: `<- e, ee` plus the encrypted @p payload; returns the message size. */
    [[nodiscard]] std::size_t write_msg2(std::span<const std::byte> payload, std::byte* out) {
        if (role_ != role_t::RESPONDER) return 0;
        return write_e_and_payload(payload, out, /*with_ee=*/true);
    }

    /** @brief Initiator: read `e, ee`; the decrypted payload lands in @p payload_out. */
    [[nodiscard]] std::size_t read_msg2(std::span<const std::byte> msg, std::byte* payload_out) {
        if (role_ != role_t::INITIATOR) return 0;
        return read_e_and_payload(msg, payload_out, /*with_ee=*/true);
    }

    /**
     * @brief Noise `Split`: key @p send and @p recv for this side's direction.
     * @return False if the HKDF or a key setup failed.
     */
    [[nodiscard]] bool split(cipher_state_t<B>& send, cipher_state_t<B>& recv) {
        std::array<std::byte, 2 * kHashLen> out{};
        if (!B::hkdf(ck_, {}, out)) return false;
        key32_t k1{};
        key32_t k2{};
        std::memcpy(k1.data(), out.data(), kHashLen);
        std::memcpy(k2.data(), out.data() + kHashLen, kHashLen);
        const bool init = role_ == role_t::INITIATOR;
        return send.initialize_key(init ? k1 : k2) && recv.initialize_key(init ? k2 : k1);
    }

    /** @brief The handshake hash `h` (the channel binding), for the cross-backend check. */
    [[nodiscard]] const key32_t& handshake_hash() const { return h_; }

   private:
    /** @brief `h = HASH(h || data)`. */
    [[nodiscard]] bool mix_hash(std::span<const std::byte> data) {
        std::array<std::byte, kHashLen + kHandshakeMsgMax> buf{};
        if (data.size() > kHandshakeMsgMax) return false;
        std::memcpy(buf.data(), h_.data(), kHashLen);
        if (!data.empty()) std::memcpy(buf.data() + kHashLen, data.data(), data.size());
        B::hash(std::span<const std::byte>(buf.data(), kHashLen + data.size()), h_);
        return true;
    }

    /** @brief `ck, k = HKDF(ck, ikm, 2)`; `InitializeKey(k)`. */
    [[nodiscard]] bool mix_key(std::span<const std::byte> ikm) {
        std::array<std::byte, 2 * kHashLen> out{};
        if (!B::hkdf(ck_, ikm, out)) return false;
        key32_t k{};
        std::memcpy(ck_.data(), out.data(), kHashLen);
        std::memcpy(k.data(), out.data() + kHashLen, kHashLen);
        return cs_.initialize_key(k);
    }

    /** @brief `ck, temp_h, k = HKDF(ck, ikm, 3)`; `MixHash(temp_h)`; `InitializeKey(k)`. */
    [[nodiscard]] bool mix_key_and_hash(std::span<const std::byte> ikm) {
        std::array<std::byte, 3 * kHashLen> out{};
        if (!B::hkdf(ck_, ikm, out)) return false;
        key32_t k{};
        std::memcpy(ck_.data(), out.data(), kHashLen);
        std::memcpy(k.data(), out.data() + 2 * kHashLen, kHashLen);
        return mix_hash(std::span<const std::byte>(out.data() + kHashLen, kHashLen)) &&
               cs_.initialize_key(k);
    }

    /** @brief The `e` token (PSK mode, so also `MixKey(e.pub)`), optional `ee`, then the
     *         payload under `EncryptAndHash`. */
    [[nodiscard]] std::size_t write_e_and_payload(std::span<const std::byte> payload,
                                                  std::byte* out, bool with_ee) {
        if (payload.size() > kMaxHandshakePayload) return 0;
        key32_t pub{};
        if (!e_.set(e_priv_, pub)) return 0;
        std::memcpy(out, pub.data(), kDhLen);
        if (!mix_hash(pub) || !mix_key(pub)) return 0;
        if (with_ee && !mix_dh()) return 0;
        std::byte* const ct = out + kDhLen;
        if (!cs_.encrypt_with_ad(h_, payload, ct)) return 0;
        const std::size_t ct_len = payload.size() + (cs_.has_key() ? kTagLen : 0);
        if (!mix_hash(std::span<const std::byte>(ct, ct_len))) return 0;
        return kDhLen + ct_len;
    }

    /** @brief Read the peer's `e` token, optional `ee`, then `DecryptAndHash` the payload. */
    [[nodiscard]] std::size_t read_e_and_payload(std::span<const std::byte> msg,
                                                 std::byte* payload_out, bool with_ee) {
        if (msg.size() < kDhLen + kTagLen || msg.size() > kHandshakeMsgMax) return 0;
        std::memcpy(re_.data(), msg.data(), kDhLen);
        if (!mix_hash(re_) || !mix_key(re_)) return 0;
        if (with_ee && !mix_dh()) return 0;
        const auto ct = msg.subspan(kDhLen);
        if (!cs_.decrypt_with_ad(h_, ct, payload_out)) return 0;
        if (!mix_hash(ct)) return 0;
        return msg.size();
    }

    /** @brief `MixKey(DH(e, re))` — the `ee` token. On the responder `e` was just set by
     *         `write_e_and_payload`; on the initiator it was set in `write_msg1`. */
    [[nodiscard]] bool mix_dh() {
        key32_t shared{};
        return e_.agree(re_, shared) && mix_key(shared);
    }

    role_t role_ = role_t::INITIATOR;
    key32_t psk_{};
    key32_t e_priv_{};
    key32_t h_{};
    key32_t ck_{};
    key32_t re_{};
    typename B::dh_key_t e_{};
    cipher_state_t<B> cs_{};
};

/** @brief One side's transport-phase session: the two cipher states `Split` produced. */
template <class B>
struct session_t {
    cipher_state_t<B> send; /**< @brief This side's outbound direction. */
    cipher_state_t<B> recv; /**< @brief This side's inbound direction. */
};

/** @brief Every byte a completed handshake produced, for the cross-backend comparison. */
struct transcript_t {
    std::array<std::byte, kHandshakeMsgMax> msg1{}; /**< @brief `-> psk, e` on the wire. */
    std::array<std::byte, kHandshakeMsgMax> msg2{}; /**< @brief `<- e, ee` on the wire. */
    std::size_t msg1_len = 0;                       /**< @brief Bytes of @ref msg1. */
    std::size_t msg2_len = 0;                       /**< @brief Bytes of @ref msg2. */
    key32_t h{};                                    /**< @brief The final handshake hash. */
    /** @brief A 64-byte frame the initiator sealed after `Split`, with its tag. */
    std::array<std::byte, 64 + kTagLen> first_frame{};
};

/**
 * @brief Run one whole handshake between an initiator on backend @p I and a responder on
 *        backend @p R, then send one frame each way, and record the transcript.
 *
 * Mixed backends are the interop check: a handshake only completes when both sides derived the
 * same keys and the same `h`, because each payload's AD is `h`.
 *
 * @return False if any step failed, either side's `h` disagreed, or a frame did not open.
 */
template <class I, class R>
[[nodiscard]] bool run_handshake(const key32_t& psk, const key32_t& ie, const key32_t& re,
                                 transcript_t& t) {
    handshake_t<I> init;
    handshake_t<R> resp;
    std::array<std::byte, kMaxHandshakePayload> scratch{};
    if (!init.start(role_t::INITIATOR, psk, ie) || !resp.start(role_t::RESPONDER, psk, re))
        return false;
    t.msg1_len = init.write_msg1({}, t.msg1.data());
    if (t.msg1_len == 0) return false;
    if (resp.read_msg1(std::span(t.msg1.data(), t.msg1_len), scratch.data()) == 0) return false;
    t.msg2_len = resp.write_msg2({}, t.msg2.data());
    if (t.msg2_len == 0) return false;
    if (init.read_msg2(std::span(t.msg2.data(), t.msg2_len), scratch.data()) == 0) return false;
    if (init.handshake_hash() != resp.handshake_hash()) return false;
    t.h = init.handshake_hash();

    session_t<I> a;
    session_t<R> b;
    if (!init.split(a.send, a.recv) || !resp.split(b.send, b.recv)) return false;
    std::array<std::byte, 64> pt{};
    for (std::size_t i = 0; i < pt.size(); ++i) pt[i] = static_cast<std::byte>(i);
    std::array<std::byte, 64> back{};
    if (!a.send.encrypt_with_ad({}, pt, t.first_frame.data())) return false;
    if (!b.recv.decrypt_with_ad({}, t.first_frame, back.data()) || back != pt) return false;
    std::array<std::byte, 64 + kTagLen> reply{};
    if (!b.send.encrypt_with_ad({}, pt, reply.data())) return false;
    return a.recv.decrypt_with_ad({}, reply, back.data()) && back == pt;
}

}  // namespace bench::noise
