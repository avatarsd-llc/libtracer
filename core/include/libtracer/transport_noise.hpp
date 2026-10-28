/**
 * @file
 * @brief The Noise link of RFC-0033 (reference/23): frames travel inside one
 *        `Noise_NNpsk0_25519_ChaChaPoly_SHA256` session over a datagram carrier (#2073).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This is the link half of the binding, built on the crypto half in `security_noise.hpp`: the
 * slots, the initiator counter and the responder's mark, the replay window, the key phase, the
 * `now` deadlines and the `:stats` counters. The router sees an ordinary @ref tr::net::transport_t;
 * the frames it sends and receives are byte-identical to the frames the carrier would carry
 * without the binding (reference/23 §1).
 *
 * @section noise_link_shape The shape
 *
 * `noise_link_t<B, Host, Carrier>` owns its carrier (a @ref tr::net::udp_transport_t on a host)
 * and takes three compile-time choices:
 *
 *  - `B`, the crypto backend (`tr::net::noise::default_crypto_t`, `LIBTRACER_NOISE_CRYPTO`);
 *  - `Host`, the application's seams (@ref tr::net::noise::link_host): the key store that
 *    resolves the PSK reference, the random source the ephemeral keys come from, and the
 *    initiator counter. The library has no key, no entropy and no clock of its own;
 *  - `Carrier`, the datagram link underneath (@ref tr::net::noise::datagram_carrier).
 *
 * A build that names no backend compiles none of this.
 *
 * @section noise_link_time Time
 *
 * The link reads no clock and arms no timer (reference/23 §7). Every deadline is checked when
 * the application calls @ref tr::net::noise_link_t::poll with its own `now`, and `poll` answers
 * the next deadline. A deadline configured as 0 is never due; without `poll` calls nothing
 * expires by time and nothing is retransmitted.
 *
 * @section noise_link_rtt One request and its reply
 *
 * A send on a link with no session fails at once, counted in `dropped_tx`, and on the DIAL side
 * it starts a handshake (a demand; @ref tr::net::noise_link_t::dial demands without a frame).
 * When the second message arrives the initiator holds an unacknowledged session and `poll`
 * reports `session`; a request sent then is the confirmation, and the responder's reply is the
 * acknowledgement, after which the link reports `UP`. One request and reply take two round
 * trips (reference/23 §4). While the session stays unacknowledged, each retry deadline `poll`
 * passes sends an empty confirmation on it (§4 step 7), so an application that sends nothing
 * is confirmed too.
 *
 * @section noise_link_memory Memory
 *
 * All session state (the PSK state, the handshake cipher, two session slots, the handshake
 * slot and its stored messages) is ONE block drawn from the link's `memory.state` when the link
 * is built, and released when it is destroyed. A handshake fills a slot that already exists, so
 * nothing on the receive path allocates, and a first message under a wrong PSK costs one
 * `MixHash`, one HKDF and one tag check, and no memory (reference/23 §8). A transport message is
 * decrypted in place in the carrier's receive buffer. A send draws one block of `frame + 25`
 * bytes from `memory.io`, seals into it and returns it; an exhausted store drops the frame,
 * counted in `dropped_tx`.
 *
 * @section noise_link_scope What this slice leaves out
 *
 * This build keeps no previous session (reference/23 §6 makes it optional), so a message in a
 * replaced session's phase is counted in `noise_stale`. Rekey, `rekey_after` / `reject_after`
 * and the generation rule are #2074; persisting the counter and the mark is #2075. A link that
 * reaches its attempt deadline returns to `DORMANT` until the next demand, as an unheld link
 * does (reference/23 §7). One peer per link (§8).
 */
#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "libtracer/grammar.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/link_kind.hpp"
#include "libtracer/security_noise.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/transport_factory.hpp"

namespace tr::net {

namespace noise {

/**
 * @brief The application's seams a Noise link calls (reference/23 §2, §4): a compile-time
 *        policy, so the link pays no indirect call for any of them.
 *
 *  - `resolve_psk(ref, out)` resolves the PSK reference to its 32 bytes, from the
 *    application's key store; false when the store has no such key. Called once, when the link
 *    is built. The key bytes never come from the wire.
 *  - `ephemeral(out)` writes 32 bytes from a cryptographic random source: one fresh ephemeral
 *    private key per call, never reused (reference/23 §2, RFC-0033 Amendment 1). False when
 *    the source fails, and the handshake step is then not taken.
 *  - `next_counter()` is the initiator counter: strictly increasing across every attempt an
 *    initiator makes under one PSK, the first at least 1 (reference/23 §4).
 *
 * A link calls them under its own lock; a host shared by several links serialises itself.
 */
template <class H>
concept link_host = requires(H& h, std::string_view ref, key32_t& key) {
    { h.resolve_psk(ref, key) } -> std::same_as<bool>;
    { h.ephemeral(key) } -> std::same_as<bool>;
    { h.next_counter() } -> std::same_as<std::uint64_t>;
};

/**
 * @brief The datagram link a Noise link runs over (reference/23 §Terms, the carrier).
 *
 * One datagram is delivered to the borrowed-span receiver, and its span is the carrier's own
 * receive buffer, writable for the call, which the Noise link decrypts in place (§8):
 * @ref tr::net::udp_transport_t states that contract. Addresses are opaque packed values, 0
 * meaning none: `inbound_source()` is the in-flight datagram's source (valid in the
 * callback), `peer_address()` the configured peer a DIAL link sends to, and `send_to` sends
 * one datagram to a given address, so the Noise link, not the carrier, decides where a
 * session's traffic goes (§4 "Peer address"). `effective_max_frame()` is the largest datagram
 * the carrier accepts.
 */
template <class C>
concept datagram_carrier =
    std::derived_from<C, transport_t> &&
    requires(C& c, const C& cc, std::uint64_t a, std::span<const std::span<const std::byte>> iov) {
        { cc.ok() } -> std::convertible_to<bool>;
        { cc.effective_max_frame() } -> std::convertible_to<std::size_t>;
        { cc.inbound_source() } -> std::same_as<std::uint64_t>;
        { cc.peer_address() } -> std::same_as<std::uint64_t>;
        c.send_to(a, iov);
    };

/**
 * @brief A Noise link's counters: reference/23 §11's ten nouns, in that order, then the two
 *        universal ones the link adds to its carrier's.
 */
enum class counter_t : std::uint8_t {
    HANDSHAKE_FAILED,   /**< @brief `noise_handshake_failed`: tag, flags byte or all-zero `ee`. */
    HANDSHAKE_REPLAYED, /**< @brief `noise_handshake_replayed`: a counter not above the mark. */
    PENDING_SUPERSEDED, /**< @brief `noise_pending_superseded`: a newer first message won. */
    PENDING_EXPIRED,    /**< @brief `noise_pending_expired`: a pending session timed out. */
    RETRANSMITS,        /**< @brief `noise_retransmits`: a first or stored second message resent. */
    DECRYPT_FAILED,     /**< @brief `noise_decrypt_failed`: a transport tag failed. */
    REPLAYED,           /**< @brief `noise_replayed`: refused by the replay window. */
    STALE,              /**< @brief `noise_stale`: well formed, for state this side lacks. */
    EXPIRED,            /**< @brief `noise_expired`: a session discarded by age or nonce limit. */
    OVERSIZE,           /**< @brief `noise_oversize`: a frame above `max_frame`, refused at send. */
    MALFORMED_RX,       /**< @brief Added to the carrier's `malformed_rx` (§9's first row). */
    DROPPED_TX,         /**< @brief Added to the carrier's `dropped_tx`: no session, no store. */
    COUNT,              /**< @brief The number of counters. */
};

/** @brief The `:stats.link.<child>` noun of each of the first ten @ref counter_t values. */
inline constexpr std::array<std::string_view, 10> kCounterNouns = {
    "noise_handshake_failed",
    "noise_handshake_replayed",
    "noise_pending_superseded",
    "noise_pending_expired",
    "noise_retransmits",
    "noise_decrypt_failed",
    "noise_replayed",
    "noise_stale",
    "noise_expired",
    "noise_oversize",
};

/** @brief The replay window's width `W`: one word (reference/23 §5 requires at least 64). */
inline constexpr std::uint64_t kReplayWindow = 64;

/**
 * @brief One session: its two transport keys, the send nonce, the replay window and the
 *        address its traffic goes to (reference/23 §Terms, §5).
 */
template <crypto_backend B>
struct session_t {
    transport_cipher_t<B> cipher{}; /**< @brief The two directions' keys. */
    std::uint64_t send_nonce = 0;   /**< @brief The next nonce to seal under. */
    std::uint64_t highest = 0;      /**< @brief The highest nonce accepted, once @ref any holds. */
    std::uint64_t seen = 0;         /**< @brief Bit `i` marks nonce `highest - i` accepted. */
    std::uint64_t peer = 0;         /**< @brief Where this session's traffic is sent. */
    std::uint8_t phase = 0;         /**< @brief The key phase, 0 or 1. */
    bool live = false;              /**< @brief The slot holds a session. */
    bool any = false;               /**< @brief A transport message has been accepted. */

    /** @brief Whether nonce @p n is refused before any decryption (§5 step 3). */
    [[nodiscard]] bool replay_refused(std::uint64_t n) const noexcept {
        if (!any || n > highest) return false;
        const std::uint64_t d = highest - n;
        return d >= kReplayWindow || ((seen >> d) & 1U) != 0;
    }

    /** @brief Record nonce @p n as accepted, once its tag has verified (§5 step 5). */
    void accept(std::uint64_t n) noexcept {
        if (any && n <= highest) {
            seen |= std::uint64_t{1} << (highest - n);
            return;
        }
        const std::uint64_t shift = any ? n - highest : kReplayWindow;
        seen = shift >= kReplayWindow ? 1U : (seen << shift) | 1U;
        highest = n;
        any = true;
    }

    /** @brief Start a fresh session in phase @p p toward @p address; the keys are set apart. */
    void arm(std::uint8_t p, std::uint64_t address) noexcept {
        send_nonce = highest = seen = 0;
        any = false;
        phase = p;
        peer = address;
        live = true;
    }

    /** @brief Discard the session: its keys are dropped and the slot is empty. */
    void discard() {
        cipher.clear();
        live = false;
    }
};

}  // namespace noise

/**
 * @brief A Noise link's kind-private config (the lean rule: none of it is in the shared
 *        connection settings). Deadlines are in the application's `now` units; 0 is never due.
 */
struct noise_config_t {
    /** @brief The PSK's key-store name, resolved through `Host::resolve_psk` while the link is
     *         built; borrowed for the constructor only. The key itself is never config. */
    std::string_view psk_ref{};
    /** @brief The largest Tracer frame (the universal `max_frame` key): 0 means the carrier's
     *         datagram bound less 25. The link's datagram bound is `max_frame + 25`; over IPv4 a
     *         frame is at most 65482 bytes (reference/23 §3). */
    std::size_t max_frame = 0;
    /** @brief Initiator: the first retry interval; it doubles on each retry up to
     *         @ref retry_cap. */
    std::uint64_t retry = 0;
    /** @brief Initiator: the largest retry interval; 0 keeps every interval at @ref retry. */
    std::uint64_t retry_cap = 0;
    /** @brief Initiator: the attempt deadline, from the attempt's start (§7). */
    std::uint64_t attempt = 0;
    /** @brief Responder: how long a pending session waits for its confirmation. */
    std::uint64_t pending_expiry = 0;
    /** @brief `state`: the slot block, drawn once at build. `io`: one seal block per send. */
    link_memory_t memory{};
};

/** @brief What @ref noise_link_t::poll answers. */
struct noise_poll_t {
    /** @brief The next deadline in `now` units, 0 when none is armed: call again by then. */
    std::uint64_t next = 0;
    /** @brief A send is admitted now: the link holds a session, acknowledged or not. */
    bool session = false;
    /** @brief The connection's RFC-0014 liveness (reference/23 §10). */
    link_state_t state = link_state_t::DORMANT;
};

/**
 * @brief A Noise link (RFC-0033, reference/23) over a datagram carrier it owns.
 *
 * DIAL is the initiator and LISTEN the responder; they never swap. See the file comment for the
 * request/reply flow, the memory and what this slice leaves out.
 *
 * @tparam B       The crypto backend.
 * @tparam Host    The application's key store, random source and counter (@ref
 * tr::net::noise::link_host).
 * @tparam Carrier The datagram link underneath (@ref tr::net::noise::datagram_carrier).
 *
 * @note Thread-safety: the carrier's receive thread, senders and `poll` may run concurrently.
 *       One lock guards the session state. A frame is delivered with the lock released, so a
 *       receiver may send (an echo, a reply) from inside its callback.
 */
template <noise::crypto_backend B, noise::link_host Host, noise::datagram_carrier Carrier>
class noise_link_t final : public transport_t {
   public:
    /**
     * @brief Build the link: draw its state from `config.memory.state`, resolve its PSK and
     *        compute the PSK state, then build the carrier from @p carrier_args.
     *
     * Any failure leaves the link inert (@ref ok false): a carrier that did not come up or
     * whose datagram bound is below a first message's 58 bytes (§3), an exhausted state store,
     * an unknown PSK reference, or a backend that cannot start.
     *
     * @param role DIAL initiates, LISTEN answers.
     * @param host Must outlive the link.
     */
    template <class... A>
    noise_link_t(conn_role_t role, Host& host, const noise_config_t& config, A&&... carrier_args)
        : role_(role),
          host_(host),
          retry_(config.retry),
          retry_cap_(std::max(config.retry, config.retry_cap)),
          attempt_(config.attempt),
          pending_expiry_(config.pending_expiry),
          state_src_(&config.memory.state_or_default()),
          io_src_(&config.memory.io_or_default()) {
        if (!B::init()) return;
        void* const p = state_src_->try_alloc(sizeof(state_t), alignof(state_t));
        if (p == nullptr) return;
        st_ = new (p) state_t{};
        noise::key32_t psk{};
        const bool keyed =
            host_.resolve_psk(config.psk_ref, psk) && noise::psk_state(psk, st_->keyed);
        noise::wipe(psk);
        carrier_.emplace(std::forward<A>(carrier_args)...);
        const std::size_t bound = carrier_->effective_max_frame();
        max_frame_ = std::min({config.max_frame == 0 ? noise::kMaxFrameBytes : config.max_frame,
                               noise::kMaxFrameBytes, bound - std::min(bound, kOverhead)});
        if (!keyed || !carrier_->ok() || bound < noise::kFirstMessageBytes) {
            release_state();
            return;
        }
        current_ = &st_->slots[0];
        next_ = &st_->slots[1];
        carrier_->set_receiver(&on_carrier, this);
    }

    noise_link_t(const noise_link_t&) = delete;
    noise_link_t& operator=(const noise_link_t&) = delete;

    /** @brief Stop the carrier (joining its receive thread) before the state goes. */
    ~noise_link_t() {
        carrier_.reset();
        release_state();
    }

    /** @brief True when the link came up: its state, its PSK and its carrier. */
    [[nodiscard]] bool ok() const noexcept { return st_ != nullptr; }
    /** @brief The carrier, for its own accessors (a UDP carrier's `local_port`). */
    [[nodiscard]] Carrier& carrier() noexcept { return *carrier_; }
    /** @brief The carrier, read-only. */
    [[nodiscard]] const Carrier& carrier() const noexcept { return *carrier_; }
    /** @brief The largest frame a send accepts; a larger one is refused (`noise_oversize`). */
    [[nodiscard]] std::size_t max_frame() const noexcept { return max_frame_; }

    /** @brief Seal and send one frame, as one transport message (§3). */
    void send(std::span<const std::byte> frame) override {
        const std::span<const std::byte> one[1] = {frame};
        send(std::span<const std::span<const std::byte>>(one));
    }

    /**
     * @brief Seal and send the gathered spans as one frame, as one transport message.
     *
     * Best effort, like every link: a frame above @ref max_frame is refused (`noise_oversize`);
     * an empty one (an empty plaintext is the protocol's own message, reference/23 §3), one
     * sent with no session, or one with an exhausted `memory.io`, is dropped (`dropped_tx`).
     * With no session on the DIAL side the send is also a demand: it starts a handshake.
     */
    void send(std::span<const std::span<const std::byte>> iov) override {
        std::size_t len = 0;
        for (const std::span<const std::byte> s : iov) len += s.size();
        if (len > max_frame_) return bump_locked(noise::counter_t::OVERSIZE);
        if (len == 0) return bump_locked(noise::counter_t::DROPPED_TX);
        const std::size_t bytes = len + kOverhead;
        auto* const buf = static_cast<std::byte*>(io_src_->try_alloc(bytes, alignof(std::byte)));
        if (buf == nullptr) return bump_locked(noise::counter_t::DROPPED_TX);
        // One span seals straight from the caller's bytes; a gather is copied in first and
        // sealed in place, so either way the frame is read once.
        const std::span<const std::byte> frame = iov.size() == 1 ? iov[0] : gather(iov, buf, len);
        std::uint64_t to = 0;
        {
            const std::lock_guard lock(mu_);
            to = seal(frame, std::span<std::byte>(buf, bytes));
        }
        if (to != 0) send_datagram(to, std::span<const std::byte>(buf, bytes));
        io_src_->release(buf, bytes, alignof(std::byte));
    }

    /**
     * @brief Demand the link without a frame: on the DIAL side, start a handshake if there is
     *        no session and none is in flight. A no-op on LISTEN.
     */
    void dial() {
        const std::lock_guard lock(mu_);
        demand();
    }

    /**
     * @brief Run every deadline that @p now has passed, and report the link (reference/23 §7).
     *
     * Retransmits a first message, sends an empty confirmation on a session still
     * unacknowledged at its retry deadline, ends an attempt at its deadline, or expires a
     * pending session.
     * @param now The application's monotonic clock, in the config's units.
     */
    noise_poll_t poll(std::uint64_t now) {
        const std::lock_guard lock(mu_);
        now_ = now;
        if (st_ != nullptr) run_deadlines();
        return {next_deadline(), sending_session() != nullptr, state_locked()};
    }

    /** @brief The connection's RFC-0014 liveness (reference/23 §10). */
    [[nodiscard]] link_state_t state() const {
        const std::lock_guard lock(mu_);
        return state_locked();
    }

    /** @brief True while a send is admitted: the link holds a session. */
    [[nodiscard]] bool link_up() const noexcept override {
        const std::lock_guard lock(mu_);
        return sending_session() != nullptr;
    }

    /** @brief The carrier's three counters, plus what this link drops itself. */
    [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override {
        transport_drop_stats_t d = carrier_ ? carrier_->drop_stats() : transport_drop_stats_t{};
        const std::lock_guard lock(mu_);
        d.malformed_rx += counts_[std::size_t(noise::counter_t::MALFORMED_RX)];
        d.dropped_tx += counts_[std::size_t(noise::counter_t::DROPPED_TX)];
        return d;
    }

    /** @brief The ten `noise_*` nouns of reference/23 §11. */
    void kind_stats(graph::stats_block_t& out) const noexcept override {
        const std::lock_guard lock(mu_);
        for (std::size_t i = 0; i < noise::kCounterNouns.size(); ++i)
            out.add(noise::kCounterNouns[i], counts_[i]);
    }

    /** @brief One counter's value (the `:stats` nouns, plus this link's own drops). */
    [[nodiscard]] std::uint64_t count(noise::counter_t c) const {
        const std::lock_guard lock(mu_);
        return counts_[std::size_t(c)];
    }

    /** @brief The carrier receives from its constructor; a later start is passed on. */
    bool start_receiving() override { return carrier_ ? carrier_->start_receiving() : true; }

   private:
    /** @brief What a transport datagram adds to its frame: type, nonce and tag. */
    static constexpr std::size_t kOverhead = noise::kTransportOverhead;

    /** @brief Where the handshake slot stands. */
    enum class stage_t : std::uint8_t {
        IDLE,         /**< @brief No handshake: the slot is empty. */
        AWAIT_SECOND, /**< @brief Initiator: the first message is out. */
        UNACKED,      /**< @brief Initiator: the session in `next_` awaits its acknowledgement. */
        PENDING,      /**< @brief Responder: the session in `next_` awaits its confirmation. */
    };

    /** @brief Everything one link holds about its sessions: one block from `memory.state`. */
    struct state_t {
        noise::symmetric_state_t<B> keyed{}; /**< @brief The state after the prologue and PSK. */
        typename B::aead_t hs_cipher{};      /**< @brief The handshake cipher (§8). */
        std::array<noise::session_t<B>, 2> slots{}; /**< @brief The current and next sessions. */
        std::optional<noise::handshake_t<B>> hs{};  /**< @brief The initiator's handshake. */
        std::array<std::byte, noise::kFirstMessageBytes> first{};   /**< @brief Stored msg 1. */
        std::array<std::byte, noise::kSecondMessageBytes> second{}; /**< @brief Stored msg 2. */
    };

    /** @brief Copy @p iov (@p len bytes) into @p buf after the transport header. */
    [[nodiscard]] static std::span<const std::byte> gather(
        std::span<const std::span<const std::byte>> iov, std::byte* buf, std::size_t len) {
        std::size_t off = noise::kTransportHeaderBytes;
        for (const std::span<const std::byte> s : iov) {
            if (!s.empty()) std::memcpy(buf + off, s.data(), s.size());
            off += s.size();
        }
        return {buf + noise::kTransportHeaderBytes, len};
    }

    /** @brief The carrier's receive callback: one datagram, on the carrier's thread. */
    static void on_carrier(void* ctx, std::span<const std::byte> datagram) {
        static_cast<noise_link_t*>(ctx)->on_datagram(datagram);
    }

    /** @brief Handle one datagram under the lock, then deliver its frame without it. */
    void on_datagram(std::span<const std::byte> in) {
        // The carrier's own receive buffer, writable for this call (noise::datagram_carrier):
        // a transport message is decrypted in place in it (§8).
        const std::span<std::byte> datagram(const_cast<std::byte*>(in.data()), in.size());
        std::span<std::byte> frame;
        {
            const std::lock_guard lock(mu_);
            frame = dispatch(datagram);
        }
        if (!frame.empty()) rx_.deliver_borrowed(frame);
    }

    /** @brief Route a datagram by its type byte (§3); a reserved type is malformed. */
    std::span<std::byte> dispatch(std::span<std::byte> datagram) {
        switch (datagram.empty() ? 0 : std::to_integer<std::uint8_t>(datagram[0])) {
            case std::uint8_t(noise::msg_type_t::FIRST):
                on_first(datagram);
                return {};
            case std::uint8_t(noise::msg_type_t::SECOND):
                on_second(datagram);
                return {};
            case std::uint8_t(noise::msg_type_t::TRANSPORT_PHASE_0):
            case std::uint8_t(noise::msg_type_t::TRANSPORT_PHASE_1):
                return on_transport(datagram);
            default:
                bump(noise::counter_t::MALFORMED_RX);
                return {};
        }
    }

    /** @brief The counter a refused handshake message steps: malformed, or a failed handshake. */
    [[nodiscard]] static noise::counter_t refused(noise::refusal_t r) noexcept {
        return r == noise::refusal_t::MALFORMED ? noise::counter_t::MALFORMED_RX
                                                : noise::counter_t::HANDSHAKE_FAILED;
    }

    /**
     * @brief Responder: a first message, in §4's normative order: the byte-identical
     *        retransmit, the PSK proof, the flags and the counter, and only then the answer.
     */
    void on_first(std::span<const std::byte> datagram) {
        if (datagram.size() != noise::kFirstMessageBytes)
            return bump(noise::counter_t::MALFORMED_RX);
        if (role_ == conn_role_t::DIAL) return bump(noise::counter_t::STALE);
        const std::uint64_t source = carrier_->inbound_source();
        if (stage_ == stage_t::PENDING &&
            std::memcmp(datagram.data(), st_->first.data(), st_->first.size()) == 0) {
            bump(noise::counter_t::RETRANSMITS);
            return send_datagram(source, st_->second);
        }
        noise::handshake_t<B> hs(noise::role_t::RESPONDER, st_->keyed, st_->hs_cipher);
        const auto payload = hs.read_first(datagram);
        if (!payload) return bump(refused(payload.error()));
        if (payload->counter <= mark_) return bump(noise::counter_t::HANDSHAKE_REPLAYED);
        mark_ = payload->counter;  // whether or not this handshake completes (§4 step 3)
        answer(hs, datagram, source);
    }

    /**
     * @brief Responder: draw a fresh ephemeral key, write the second message in the new key
     *        phase, keep the result as the pending session and send it to @p source (§4 step 4).
     */
    void answer(noise::handshake_t<B>& hs, std::span<const std::byte> first, std::uint64_t source) {
        const bool held = current_->live;
        const auto phase = static_cast<std::uint8_t>(held ? current_->phase ^ 1U : 0U);
        const auto flags = static_cast<std::uint8_t>(phase | (held ? 0U : 2U));
        noise::key32_t e{};
        std::array<std::byte, noise::kSecondMessageBytes> second{};
        const bool written = host_.ephemeral(e) && hs.write_second(e, flags, second).has_value();
        noise::wipe(e);
        if (!written) return bump(noise::counter_t::HANDSHAKE_FAILED);
        if (stage_ == stage_t::PENDING) bump(noise::counter_t::PENDING_SUPERSEDED);
        next_->discard();
        stage_ = stage_t::IDLE;
        if (!hs.split(next_->cipher)) return bump(noise::counter_t::HANDSHAKE_FAILED);
        next_->arm(phase, source);
        std::memcpy(st_->first.data(), first.data(), st_->first.size());
        st_->second = second;
        stage_ = stage_t::PENDING;
        pending_at_ = deadline(pending_expiry_);
        send_datagram(source, st_->second);
    }

    /** @brief Initiator: a second message ends the attempt's first half (§4 step 3). */
    void on_second(std::span<const std::byte> datagram) {
        if (datagram.size() != noise::kSecondMessageBytes)
            return bump(noise::counter_t::MALFORMED_RX);
        if (role_ == conn_role_t::LISTEN || stage_ != stage_t::AWAIT_SECOND)
            return bump(noise::counter_t::STALE);
        const auto flags = st_->hs->read_second(datagram);
        if (!flags) return bump(refused(flags.error()));
        const auto phase = static_cast<std::uint8_t>(*flags & 1U);
        const bool split = st_->hs->split(next_->cipher).has_value();
        st_->hs.reset();
        stage_ = stage_t::IDLE;
        if (!split) return bump(noise::counter_t::HANDSHAKE_FAILED);
        // The responder answered in the current session's phase: it no longer holds that one.
        if (current_->live && current_->phase == phase) current_->discard();
        next_->arm(phase, peer_);
        stage_ = stage_t::UNACKED;
        retry_step_ = retry_;
        retry_at_ = deadline(retry_step_);
    }

    /**
     * @brief A transport message, in §5's order: size and type, nonce bound, replay pre-check,
     *        tag, window update, then the plaintext rule. The first one on the next session
     *        promotes it (a confirmation on LISTEN, an acknowledgement on DIAL).
     * @return The frame to deliver, decrypted in place; empty when there is none.
     */
    std::span<std::byte> on_transport(std::span<std::byte> datagram) {
        if (datagram.size() > max_frame_ + kOverhead)
            return bump_none(noise::counter_t::MALFORMED_RX);
        const auto h = noise::parse_transport_header(datagram);
        if (!h) return bump_none(noise::counter_t::MALFORMED_RX);
        noise::session_t<B>* const s = session_for(h->phase);
        if (s == nullptr) return bump_none(noise::counter_t::STALE);
        if (s->replay_refused(h->nonce)) return bump_none(noise::counter_t::REPLAYED);
        const auto pt = s->cipher.open(*h, datagram);
        if (!pt) return bump_none(noise::counter_t::DECRYPT_FAILED);
        s->accept(h->nonce);
        // Only an authenticated message moves where the responder sends (§4 "Peer address").
        if (role_ == conn_role_t::LISTEN) s->peer = carrier_->inbound_source();
        if (s == next_) promote();
        if (pt->empty()) {
            if (role_ == conn_role_t::LISTEN) send_empty(*s);  // the answer to an empty message
            return {};
        }
        if (!one_frame(*pt)) return bump_none(noise::counter_t::MALFORMED_RX);
        return *pt;
    }

    /** @brief Whether @p pt is exactly one complete Tracer frame, nothing left over (§3). */
    [[nodiscard]] static bool one_frame(std::span<const std::byte> pt) {
        const auto h = wire::grammar::parse_header(wire::grammar::span_cursor_t{pt},
                                                   wire::grammar::crc_check_t::DEFER);
        return h.has_value() && h->total == pt.size();
    }

    /** @brief The session a key phase names, if this side holds one (§6: never two). */
    [[nodiscard]] noise::session_t<B>* session_for(std::uint8_t phase) const noexcept {
        if (current_->live && current_->phase == phase) return current_;
        if (next_->live && next_->phase == phase) return next_;
        return nullptr;
    }

    /**
     * @brief The next session becomes current. The one it replaces is discarded: this build
     *        keeps no previous session (§6), so the slots swap and the old keys are dropped.
     */
    void promote() {
        std::swap(current_, next_);
        next_->discard();
        stage_ = stage_t::IDLE;
        retry_at_ = attempt_at_ = pending_at_ = 0;
    }

    /** @brief The session a send goes out on: the current one, or on a first handshake the
     *         initiator's unacknowledged one (§4 step 5); a responder never sends on pending. */
    [[nodiscard]] noise::session_t<B>* sending_session() const noexcept {
        if (st_ == nullptr) return nullptr;
        if (current_->live) return current_;
        return stage_ == stage_t::UNACKED ? next_ : nullptr;
    }

    /**
     * @brief Seal @p frame into @p out on the sending session, drawing its nonce.
     * @return The address to send to; 0 when the frame was dropped (and counted).
     */
    std::uint64_t seal(std::span<const std::byte> frame, std::span<std::byte> out) {
        noise::session_t<B>* const s = sending_session();
        if (s == nullptr) {
            bump(noise::counter_t::DROPPED_TX);
            demand();  // a send on a link with no session is a demand (§1)
            return 0;
        }
        if (!s->cipher.seal(s->phase, s->send_nonce, frame, out)) {
            // In practice only the 2^62 nonce limit refuses here. Discarding the session and
            // counting `noise_expired` (reference/23 §5) come with rekey, #2074.
            bump(noise::counter_t::DROPPED_TX);
            return 0;
        }
        ++s->send_nonce;
        return s->peer;
    }

    /** @brief Send one empty transport message on @p s: a confirmation or an answer. */
    void send_empty(noise::session_t<B>& s) {
        std::array<std::byte, kOverhead> buf{};
        if (!s.cipher.seal(s.phase, s.send_nonce, {}, buf))
            return bump(noise::counter_t::DROPPED_TX);
        ++s.send_nonce;
        send_datagram(s.peer, buf);
    }

    /** @brief Hand one datagram to the carrier, addressed. */
    void send_datagram(std::uint64_t to, std::span<const std::byte> datagram) {
        const std::span<const std::byte> one[1] = {datagram};
        carrier_->send_to(to, std::span<const std::span<const std::byte>>(one));
    }

    /** @brief DIAL with no session and no attempt in flight: start one. */
    void demand() {
        if (role_ == conn_role_t::DIAL && st_ != nullptr && stage_ == stage_t::IDLE &&
            !current_->live)
            start_attempt();
    }

    /** @brief Initiator: a fresh ephemeral key and the next counter; send the first message
     *         and keep it, byte for byte, for its retransmissions (§4 steps 1-2). */
    void start_attempt() {
        noise::key32_t e{};
        st_->hs.emplace(noise::role_t::INITIATOR, st_->keyed, st_->hs_cipher);
        const noise::first_payload_t payload{.flags = std::uint8_t(current_->live ? 0U : 1U),
                                             .counter = host_.next_counter()};
        const bool written = host_.ephemeral(e) && st_->hs->write_first(e, payload, st_->first);
        noise::wipe(e);
        if (!written) {
            st_->hs.reset();
            return;
        }
        stage_ = stage_t::AWAIT_SECOND;
        peer_ = carrier_->peer_address();
        retry_step_ = retry_;
        retry_at_ = deadline(retry_step_);
        attempt_at_ = deadline(attempt_);
        send_datagram(peer_, st_->first);
    }

    /** @brief The deadlines of the stage the handshake slot is in (§7's table). */
    void run_deadlines() {
        if (stage_ == stage_t::PENDING && due(pending_at_)) {
            bump(noise::counter_t::PENDING_EXPIRED);
            next_->discard();
            stage_ = stage_t::IDLE;
            pending_at_ = 0;
            return;
        }
        if (stage_ != stage_t::AWAIT_SECOND && stage_ != stage_t::UNACKED) return;
        if (due(attempt_at_)) return abandon();
        const bool retry = due(retry_at_);
        if (retry) {
            retry_step_ = std::min(retry_step_ * 2, retry_cap_);
            retry_at_ = deadline(retry_step_);
        }
        if (stage_ == stage_t::AWAIT_SECOND && retry) {
            bump(noise::counter_t::RETRANSMITS);
            send_datagram(peer_, st_->first);
        }
        if (stage_ == stage_t::UNACKED && retry) send_empty(*next_);  // §4 step 7
    }

    /** @brief The attempt deadline passed: end the attempt and its unacknowledged session. */
    void abandon() {
        st_->hs.reset();
        next_->discard();
        stage_ = stage_t::IDLE;
        retry_at_ = attempt_at_ = 0;
    }

    /** @brief The earliest armed deadline, 0 when none is. */
    [[nodiscard]] std::uint64_t next_deadline() const noexcept {
        std::uint64_t next = 0;
        for (const std::uint64_t t : {retry_at_, attempt_at_, pending_at_})
            if (t != 0 && (next == 0 || t < next)) next = t;
        return next;
    }

    /** @brief The liveness byte (§10): a DIAL link is `UP` once its session is acknowledged. */
    [[nodiscard]] link_state_t state_locked() const noexcept {
        if (role_ == conn_role_t::LISTEN)
            return st_ != nullptr ? link_state_t::LISTENING : link_state_t::BIND_FAILED;
        if (st_ != nullptr && current_->live) return link_state_t::UP;
        return stage_ == stage_t::IDLE ? link_state_t::DORMANT : link_state_t::DIALING;
    }

    /** @brief `now + d`, or 0 (never due) when @p d is 0. */
    [[nodiscard]] std::uint64_t deadline(std::uint64_t d) const noexcept {
        return d == 0 ? 0 : now_ + d;
    }

    /** @brief Whether the armed deadline @p t has passed. */
    [[nodiscard]] bool due(std::uint64_t t) const noexcept { return t != 0 && now_ >= t; }

    /** @brief Step counter @p c; the lock is held. */
    void bump(noise::counter_t c) noexcept { ++counts_[std::size_t(c)]; }

    /** @brief Step counter @p c and return no frame. */
    std::span<std::byte> bump_none(noise::counter_t c) noexcept {
        bump(c);
        return {};
    }

    /** @brief Step counter @p c, taking the lock. */
    void bump_locked(noise::counter_t c) {
        const std::lock_guard lock(mu_);
        bump(c);
    }

    /** @brief Destroy the state block and return it to its store. */
    void release_state() noexcept {
        if (st_ == nullptr) return;
        for (noise::session_t<B>& s : st_->slots) s.discard();
        st_->~state_t();
        state_src_->release(st_, sizeof(state_t), alignof(state_t));
        st_ = nullptr;
    }

    conn_role_t role_;               /**< @brief DIAL initiates, LISTEN answers. */
    Host& host_;                     /**< @brief The application's seams. */
    std::uint64_t retry_;            /**< @brief The first retry interval. */
    std::uint64_t retry_cap_;        /**< @brief The largest retry interval. */
    std::uint64_t attempt_;          /**< @brief The attempt deadline's span. */
    std::uint64_t pending_expiry_;   /**< @brief The pending session's span. */
    mem::block_source_t* state_src_; /**< @brief `memory.state`: the slot block's store. */
    mem::block_source_t* io_src_;    /**< @brief `memory.io`: the per-send seal block's store. */
    std::size_t max_frame_ = 0;      /**< @brief The largest frame a send accepts. */

    mutable std::mutex mu_; /**< @brief Guards everything below. */
    state_t* st_ = nullptr; /**< @brief The slot block; null when the link is inert. */
    noise::session_t<B>* current_ = nullptr; /**< @brief The current session's slot. */
    noise::session_t<B>* next_ = nullptr;    /**< @brief The unacknowledged or pending slot. */
    stage_t stage_ = stage_t::IDLE;          /**< @brief The handshake slot's stage. */
    std::uint64_t mark_ = 0;       /**< @brief Responder: the highest counter accepted (§4). */
    std::uint64_t peer_ = 0;       /**< @brief Initiator: the configured peer. */
    std::uint64_t now_ = 0;        /**< @brief The last `now` @ref poll was given. */
    std::uint64_t retry_step_ = 0; /**< @brief The current retry interval. */
    std::uint64_t retry_at_ = 0;   /**< @brief The retry deadline; 0 none. */
    std::uint64_t attempt_at_ = 0; /**< @brief The attempt deadline; 0 none. */
    std::uint64_t pending_at_ = 0; /**< @brief The pending session's expiry; 0 none. */
    /** @brief The counters, indexed by @ref noise::counter_t. Plain words under `mu_`
     *         (core/STYLE.md §Introspection, counting doctrine rule 5). */
    std::array<std::size_t, std::size_t(noise::counter_t::COUNT)> counts_{};

    /** @brief The carrier, built last and destroyed first, so its receive thread is joined
     *         before anything it calls into goes. */
    std::optional<Carrier> carrier_;
};

}  // namespace tr::net
