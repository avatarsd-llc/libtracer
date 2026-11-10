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
 * the application's clock reaches the link through @ref tr::net::noise_link_t::sweep (directly,
 * or through the node's @ref tr::net::transport_vertex_t::sweep), and `sweep` answers the next
 * deadline. A deadline configured as 0 is never due; without `sweep` calls nothing expires by
 * time and nothing is retransmitted.
 *
 * @section noise_link_rtt One request and its reply
 *
 * A send on a link with no session fails at once, counted in `dropped_tx`, and on the DIAL side
 * it starts a handshake (a demand; @ref tr::net::noise_link_t::dial demands without a frame).
 * When the second message arrives the initiator holds an unacknowledged session and
 * @ref tr::net::noise_link_t::link_up reports it; a request sent then is the confirmation, and
 * the responder's reply is the acknowledgement, after which the link reports `UP`. One request
 * and reply take two round trips (reference/23 §4). While the session stays unacknowledged,
 * each retry deadline `sweep` passes sends an empty confirmation on it (§4 step 7), so an
 * application that sends nothing is confirmed too.
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
 * A link that reaches its attempt deadline returns to `DORMANT` until the next demand, as an
 * unheld link does (reference/23 §7). One peer per link (§8).
 *
 * @section noise_link_mark The counter and the mark across reboots
 *
 * Both survive a reboot only through the application (reference/23 §4). The initiator's
 * `next_counter()` is a wall clock or a monotonic counter that survives reboots, or else a
 * @ref tr::net::noise::boot_counter_t over a boot counter the application persists. The
 * responder's mark lives in RAM unless `Host` is a @ref tr::net::noise::mark_store, a
 * compile-time choice: the link then restores the mark when it is built and persists it at
 * each change. @ref tr::net::noise_link_t::clear_mark clears it, and
 * @ref tr::net::noise_link_t::replace_psk resets it to 0 with the new key.
 *
 * @section noise_link_rekey Rekey and the session boundary
 *
 * A rekey is a new handshake that only the initiator starts: at `rekey_after`, at a send nonce
 * of 2^60, or on @ref tr::net::noise_link_t::rekey. The old session stays current until the
 * new one is acknowledged, so the link stays `UP`, and once replaced it stays as the previous
 * session, receive-only, so frames in flight under the old keys still land (reference/23 §6).
 * The previous session and the unconfirmed one never exist together, so they share one slot
 * and overlap costs no RAM. A session boundary (reference/23 §10: a session discarded with no
 * replacement, or a session whose peer started fresh becoming current) fires the link-down
 * notifier before any frame of the new session is delivered; a routine rekey fires nothing.
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
#include <type_traits>
#include <utility>

#include "libtracer/grammar.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/link_kind.hpp"
#include "libtracer/security_noise.hpp"
#include "libtracer/sync.hpp"
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
 * @brief A host that keeps a responder's mark across reboots (reference/23 §4): two more
 *        seams, detected at compile time. A host without them keeps the mark in RAM only.
 *
 *  - `restore_mark(ref)` answers the mark last persisted for the PSK named `ref`, 0 when none.
 *    Called once, when a LISTEN link is built.
 *  - `persist_mark(ref, mark)` stores it and answers whether the write succeeded. Called each
 *    time the mark changes: once per accepted first message (after its PSK tag verified, before
 *    the answer is sent, so a peer without the PSK causes no write), and with 0 on
 *    @ref tr::net::noise_link_t::clear_mark and @ref tr::net::noise_link_t::replace_psk.
 *    A write that fails refuses the message: it is counted in `noise_handshake_failed` and
 *    answered by nothing (reference/23 §9). The mark in RAM still rises, so the message is not
 *    admitted later either; the initiator's next attempt, with a higher counter, writes again.
 *
 * Both are called under the link's lock. A link of such a host keeps its `psk_ref` view, so
 * the string must outlive the link. A key store that replaces a key while no link holds it
 * clears that key's persisted mark too: the mark is only meaningful under the PSK it was
 * accepted with.
 */
template <class H>
concept mark_store = requires(H& h, std::string_view ref, std::uint64_t mark) {
    { h.restore_mark(ref) } -> std::same_as<std::uint64_t>;
    { h.persist_mark(ref, mark) } -> std::same_as<bool>;
};

/**
 * @brief An initiator counter for a node with no clock that survives its reboots
 *        (reference/23 §4): a persisted boot counter as the high half and an attempt counter
 *        in RAM as the low half.
 *
 * The application raises @ref boot and persists it once per boot, before the first attempt
 * (one write per boot), and returns @ref next from its `next_counter()`. Counters then rise
 * across reboots under one PSK without a write per attempt. If that write fails, the
 * application makes no handshake attempt this boot: a boot number that was not persisted can
 * be handed out again after the next reboot, and its counters would repeat.
 */
struct boot_counter_t {
    std::uint32_t boot = 0;    /**< @brief This boot's number, persisted by the application. */
    std::uint32_t attempt = 0; /**< @brief The attempts made this boot. */

    /** @brief The next counter, `boot << 32 | attempt`; 0, which every mark refuses, once this
     *         boot's 2^32 - 1 attempts are spent. */
    [[nodiscard]] constexpr std::uint64_t next() noexcept {
        if (attempt == UINT32_MAX) return 0;
        return (std::uint64_t{boot} << 32) | ++attempt;
    }
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
 * @brief A Noise link's counters: reference/23 §11's ten nouns, in that order. The two
 *        universal ones it adds to its carrier's (`malformed_rx`, `dropped_tx`) are kept in
 *        its @ref tr::net::drop_tally_t instead (#2482).
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

/** @brief The send nonce at which an initiator starts a rekey (reference/23 §5). */
inline constexpr std::uint64_t kRekeyNonce = std::uint64_t{1} << 60;

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
     *         built; borrowed for the constructor only, unless `Host` is a
     *         @ref tr::net::noise::mark_store. The key itself is never config. */
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
    /** @brief Initiator: the current session's age at which a rekey starts (§6, §7). */
    std::uint64_t rekey_after = 0;
    /** @brief Both: the current session's age at which it is discarded and the link goes down. */
    std::uint64_t reject_after = 0;
    /** @brief Both: how long a replaced session keeps receiving. 0 keeps it until the next
     *         handshake needs its phase. */
    std::uint64_t previous_expiry = 0;
    /** @brief `state`: the slot block, drawn once at build. `io`: one seal block per send. */
    link_memory_t memory{};
};

/** @brief The test-only door @ref noise_link_t befriends: a nonce limit is unreachable by
 *         sending. Declared here, defined only by a test. */
struct noise_link_test_door_t;

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
 * @note Thread-safety: the carrier's receive thread, senders and `sweep` may run concurrently.
 *       One lock guards the session state. A frame is delivered with the lock released, so a
 *       receiver may send (an echo, a reply) from inside its callback.
 */
template <noise::crypto_backend B, noise::link_host Host, noise::datagram_carrier Carrier>
class noise_link_t final : public transport_t {
    static_assert(noise::mark_store<Host> ||
                      !(
                          requires(Host& h, std::string_view r) { h.restore_mark(r); } ||
                          requires(Host& h, std::string_view r, std::uint64_t m) {
                              h.persist_mark(r, m);
                          }),
                  "a host that restores or persists the mark must do both (noise::mark_store)");

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
          rekey_after_(config.rekey_after),
          reject_after_(config.reject_after),
          previous_expiry_(config.previous_expiry),
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
        if constexpr (noise::mark_store<Host>) {
            ref_ = config.psk_ref;
            if (role_ == conn_role_t::LISTEN) mark_ = host_.restore_mark(ref_);
        }
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
        if (len == 0) return drops_.count_dropped_tx();
        const std::size_t bytes = len + kOverhead;
        auto* const buf = static_cast<std::byte*>(io_src_->try_alloc(bytes, alignof(std::byte)));
        if (buf == nullptr) return drops_.count_dropped_tx();
        // One span seals straight from the caller's bytes; a gather is copied in first and
        // sealed in place, so either way the frame is read once.
        const std::span<const std::byte> frame = iov.size() == 1 ? iov[0] : gather(iov, buf, len);
        std::uint64_t to = 0;
        bool boundary = false;
        {
            const std::lock_guard lock(mu_);
            to = seal(frame, std::span<std::byte>(buf, bytes));
            boundary = std::exchange(boundary_, false);
        }
        if (to != 0) send_datagram(to, std::span<const std::byte>(buf, bytes));
        io_src_->release(buf, bytes, alignof(std::byte));
        if (boundary) notify_down();
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
     * @brief Ask for a rekey now (reference/23 §6): a no-op on LISTEN, with no session to
     *        replace, or while a handshake is in flight.
     */
    void rekey() {
        const std::lock_guard lock(mu_);
        if (rekey_startable()) start_attempt();
    }

    /**
     * @brief Responder: clear the mark to 0 (reference/23 §4), persisting it through a
     *        @ref tr::net::noise::mark_store host. A no-op on DIAL.
     *
     * This admits an initiator that restarted its counter, such as a replacement for one that
     * lost it. It also lets every first message recorded under this PSK through the counter
     * check again, each at most once and in increasing counter order. An initiator whose
     * counter is below one used earlier under this PSK is recovered by @ref replace_psk.
     * @return False when a mark store's write fails; the mark in RAM is 0 regardless.
     */
    bool clear_mark() {
        const std::lock_guard lock(mu_);
        return set_mark(0);
    }

    /**
     * @brief Replace the link's PSK with the key named @p ref, and on LISTEN reset the mark
     *        to 0 (reference/23 §4: a counter is only meaningful under its PSK).
     *
     * The next first message is written or checked under the new key. Sessions, and a
     * handshake in flight, keep the keys they began with. False, with nothing changed, when
     * the link is inert or the key store has no such key. Also false when a mark store's write
     * of the reset mark fails: the new key is then in place and the mark in RAM is 0, and
     * @ref clear_mark retries the write.
     * @param ref Borrowed for the call only, unless `Host` is a @ref tr::net::noise::mark_store.
     */
    bool replace_psk(std::string_view ref) {
        const std::lock_guard lock(mu_);
        if (st_ == nullptr) return false;
        noise::key32_t psk{};
        noise::symmetric_state_t<B> keyed{};
        const bool resolved = host_.resolve_psk(ref, psk) && noise::psk_state(psk, keyed);
        noise::wipe(psk);
        if (!resolved) return false;
        st_->keyed = keyed;
        if constexpr (noise::mark_store<Host>) ref_ = ref;
        return set_mark(0);
    }

    /**
     * @brief Run every deadline that @p now has passed, and answer the next (reference/23 §7).
     *
     * Retransmits a first message, sends an empty confirmation on a session still
     * unacknowledged at its retry deadline, ends an attempt at its deadline, or expires a
     * pending session; starts a rekey at `rekey_after`, discards a session at `reject_after`
     * and a previous session at its expiry. A session's own deadlines count from the first
     * sweep that sees it current. The session and the liveness it leaves are @ref link_up and
     * @ref state.
     * @param now      The application's monotonic clock, in the config's units.
     * @param deadline Unused: every Noise window is its own configured span (§7).
     * @return The next armed deadline, 0 when none is: call again by then.
     */
    std::uint64_t sweep(std::uint64_t now, std::uint64_t deadline) override {
        (void)deadline;
        sweep_t sw;
        bool boundary = false;
        {
            const std::lock_guard lock(mu_);
            now_ = now;
            if (st_ != nullptr) run_deadlines();
            // A due rekey with a handshake in flight waits for that handshake's own deadlines.
            const std::uint64_t rekey = rekey_startable() ? aged(rekey_after_) : 0;
            for (const std::uint64_t t :
                 {retry_at_, attempt_at_, pending_at_, rekey, aged(reject_after_), previous_at_})
                sw.due(t);
            boundary = std::exchange(boundary_, false);
        }
        if (boundary) notify_down();
        return sw.next;
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

    /** @brief The carrier's three counters, plus what this link drops itself (its tally). */
    [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override {
        transport_drop_stats_t d = carrier_ ? carrier_->drop_stats() : transport_drop_stats_t{};
        const transport_drop_stats_t own = drops_.snapshot();
        d.malformed_rx += own.malformed_rx;
        d.dropped_tx += own.dropped_tx;
        return d;
    }

    /** @brief The ten `noise_*` nouns of reference/23 §11. */
    void kind_stats(graph::stats_block_t& out) const noexcept override {
        const std::lock_guard lock(mu_);
        for (std::size_t i = 0; i < noise::kCounterNouns.size(); ++i)
            out.add(noise::kCounterNouns[i], counts_[i]);
    }

    /** @brief One `:stats` noun's value; this link's own drops are in @ref drop_stats. */
    [[nodiscard]] std::uint64_t count(noise::counter_t c) const {
        const std::lock_guard lock(mu_);
        return counts_[std::size_t(c)];
    }

    /** @brief The carrier receives from its constructor; a later start is passed on. */
    bool start_receiving() override { return carrier_ ? carrier_->start_receiving() : true; }

   private:
    /** @brief What a transport datagram adds to its frame: type, nonce and tag. */
    static constexpr std::size_t kOverhead = noise::kTransportOverhead;

    /** @brief The PSK name member's type when `Host` keeps no mark: nothing. */
    struct no_ref_t {};

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
        bool boundary = false;
        {
            const std::lock_guard lock(mu_);
            frame = dispatch(datagram);
            boundary = std::exchange(boundary_, false);
        }
        if (boundary) notify_down();  // before any frame of the session it starts
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
                drops_.count_malformed_rx();
                return {};
        }
    }

    /** @brief Count a refused handshake message: malformed, or a failed handshake. */
    void refuse(noise::refusal_t r) noexcept {
        if (r == noise::refusal_t::MALFORMED) return drops_.count_malformed_rx();
        bump(noise::counter_t::HANDSHAKE_FAILED);
    }

    /**
     * @brief Responder: a first message, in §4's normative order: the byte-identical
     *        retransmit, the PSK proof, the flags and the counter, and only then the answer.
     */
    void on_first(std::span<const std::byte> datagram) {
        if (datagram.size() != noise::kFirstMessageBytes) return drops_.count_malformed_rx();
        if (role_ == conn_role_t::DIAL) return bump(noise::counter_t::STALE);
        const std::uint64_t source = carrier_->inbound_source();
        if (stage_ == stage_t::PENDING &&
            std::memcmp(datagram.data(), st_->first.data(), st_->first.size()) == 0) {
            bump(noise::counter_t::RETRANSMITS);
            return send_datagram(source, st_->second);
        }
        noise::handshake_t<B> hs(noise::role_t::RESPONDER, st_->keyed, st_->hs_cipher);
        const auto payload = hs.read_first(datagram);
        if (!payload) return refuse(payload.error());
        if (payload->counter <= mark_) return bump(noise::counter_t::HANDSHAKE_REPLAYED);
        // The mark rises whether or not this handshake completes (§4 step 3). A failed write
        // refuses the message: silent, counted, no Diffie-Hellman.
        if (!set_mark(payload->counter)) return bump(noise::counter_t::HANDSHAKE_FAILED);
        answer(hs, datagram, source, (payload->flags & 1U) != 0);
    }

    /**
     * @brief Responder: draw a fresh ephemeral key, write the second message in the new key
     *        phase, keep the result as the pending session and send it to @p source (§4 step 4).
     *        The pending session takes the previous session's slot and phase (§6).
     * @param fresh The initiator held no session as it began this attempt.
     */
    void answer(noise::handshake_t<B>& hs, std::span<const std::byte> first, std::uint64_t source,
                bool fresh) {
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
        previous_at_ = 0;
        stage_ = stage_t::IDLE;
        if (!hs.split(next_->cipher)) return bump(noise::counter_t::HANDSHAKE_FAILED);
        next_->arm(phase, source);
        peer_fresh_ = fresh;
        std::memcpy(st_->first.data(), first.data(), st_->first.size());
        st_->second = second;
        stage_ = stage_t::PENDING;
        pending_at_ = deadline(pending_expiry_);
        send_datagram(source, st_->second);
    }

    /** @brief Initiator: a second message ends the attempt's first half (§4 step 3). */
    void on_second(std::span<const std::byte> datagram) {
        if (datagram.size() != noise::kSecondMessageBytes) return drops_.count_malformed_rx();
        if (role_ == conn_role_t::LISTEN || stage_ != stage_t::AWAIT_SECOND)
            return bump(noise::counter_t::STALE);
        const auto flags = st_->hs->read_second(datagram);
        if (!flags) return refuse(flags.error());
        const auto phase = static_cast<std::uint8_t>(*flags & 1U);
        next_->discard();  // the previous session, whose phase the new one takes (§6)
        previous_at_ = 0;
        const bool split = st_->hs->split(next_->cipher).has_value();
        st_->hs.reset();
        stage_ = stage_t::IDLE;
        if (!split) return bump(noise::counter_t::HANDSHAKE_FAILED);
        // The responder answered in the current session's phase: it no longer holds that one.
        if (current_->live && current_->phase == phase) lose_current();
        next_->arm(phase, peer_);
        peer_fresh_ = (*flags & 2U) != 0;
        stage_ = stage_t::UNACKED;
        retry_step_ = retry_;
        retry_at_ = deadline(retry_step_);
        // On a rekey frames stay on the old session, so nothing else would confirm this one
        // (§4 steps 4 and 6). On a first handshake the application's request may be the
        // confirmation (§4 step 5), and the retry deadline covers one that sends nothing.
        if (current_->live) send_empty(*next_);
    }

    /**
     * @brief A transport message, in §5's order: size and type, nonce bound, replay pre-check,
     *        tag, window update, then the plaintext rule. The first one on the next session
     *        promotes it (a confirmation on LISTEN, an acknowledgement on DIAL).
     * @return The frame to deliver, decrypted in place; empty when there is none.
     */
    std::span<std::byte> on_transport(std::span<std::byte> datagram) {
        if (datagram.size() > max_frame_ + kOverhead) return malformed();
        const auto h = noise::parse_transport_header(datagram);
        if (!h) return malformed();
        noise::session_t<B>* const s = session_for(h->phase);
        if (s == nullptr) return bump_none(noise::counter_t::STALE);
        if (s->replay_refused(h->nonce)) return bump_none(noise::counter_t::REPLAYED);
        const auto pt = s->cipher.open(*h, datagram);
        if (!pt) return bump_none(noise::counter_t::DECRYPT_FAILED);
        s->accept(h->nonce);
        if (s == next_ && stage_ != stage_t::IDLE && stage_ != stage_t::AWAIT_SECOND) promote();
        // A previous session only receives (§6): it neither moves the peer nor answers. Only an
        // authenticated message moves where the responder sends (§4 "Peer address").
        const bool answers = role_ == conn_role_t::LISTEN && s == current_;
        if (answers) s->peer = carrier_->inbound_source();
        if (pt->empty()) {
            if (answers) send_empty(*s);  // the answer to an empty message
            return {};
        }
        if (!one_frame(*pt)) return malformed();
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
     * @brief The next session becomes current and the one it replaces becomes previous, in
     *        the same slot (§6). A session whose peer started fresh is a session boundary
     *        (§10): the session it replaces belongs to the peer's earlier life, so it is
     *        discarded rather than kept to receive (§6 makes keeping one optional).
     */
    void promote() {
        std::swap(current_, next_);
        stage_ = stage_t::IDLE;
        retry_at_ = attempt_at_ = pending_at_ = previous_at_ = 0;
        dated_ = false;
        lost_ = false;
        boundary_ = boundary_ || peer_fresh_;
        if (peer_fresh_) next_->discard();
    }

    /** @brief Whether the next slot holds the previous session, not a handshake's (§6). */
    [[nodiscard]] bool holds_previous() const noexcept {
        return next_->live && (stage_ == stage_t::IDLE || stage_ == stage_t::AWAIT_SECOND);
    }

    /** @brief Discard the previous session, if the next slot holds one. */
    void discard_previous() {
        if (holds_previous()) next_->discard();
        previous_at_ = 0;
    }

    /** @brief Whether a rekey can start: an initiator's live session, no handshake in flight. */
    [[nodiscard]] bool rekey_startable() const noexcept {
        return role_ == conn_role_t::DIAL && st_ != nullptr && stage_ == stage_t::IDLE &&
               current_->live;
    }

    /** @brief The current session is gone; its deadlines go with it (§10 `RECONNECTING`). */
    void lose_current() {
        current_->discard();
        dated_ = false;
        lost_ = true;
    }

    /** @brief Discard the current session by age or nonce limit: the link goes down (§10). A
     *         session past its limit takes the previous one with it. */
    void expire_current() {
        lose_current();
        discard_previous();
        bump(noise::counter_t::EXPIRED);
        boundary_ = true;
    }

    /**
     * @brief Seal @p frame into @p out on @p s, drawing its nonce. A session at the nonce limit
     *        is spent and discarded (§5): a current one going down with the link, an
     *        unacknowledged one with its attempt.
     * @return False when the frame was dropped (and counted).
     */
    bool seal_on(noise::session_t<B>& s, std::span<const std::byte> frame,
                 std::span<std::byte> out) {
        if (s.send_nonce >= noise::kNonceLimit) {
            drops_.count_dropped_tx();
            if (&s == current_) {
                expire_current();
            } else {
                bump(noise::counter_t::EXPIRED);
                abandon();
            }
            return false;
        }
        if (!s.cipher.seal(s.phase, s.send_nonce, frame, out)) {
            drops_.count_dropped_tx();
            return false;
        }
        ++s.send_nonce;
        return true;
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
            drops_.count_dropped_tx();
            demand();  // a send on a link with no session is a demand (§1)
            return 0;
        }
        if (!seal_on(*s, frame, out)) return 0;
        const std::uint64_t to = s->peer;
        if (s->send_nonce >= noise::kRekeyNonce && rekey_startable()) start_attempt();  // §5
        return to;
    }

    /** @brief Send one empty transport message on @p s: a confirmation or an answer. */
    void send_empty(noise::session_t<B>& s) {
        std::array<std::byte, kOverhead> buf{};
        const std::uint64_t to = s.peer;  // seal_on may discard the session
        if (seal_on(s, {}, buf)) send_datagram(to, buf);
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
        run_session_deadlines();
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

    /**
     * @brief The session deadlines of §7. Each is armed by the first sweep that sees its
     *        session, so an age is never measured against a `now` older than the session's.
     */
    void run_session_deadlines() {
        if (current_->live && !dated_) {
            born_ = now_;
            dated_ = true;
        }
        if (holds_previous() && previous_at_ == 0) previous_at_ = deadline(previous_expiry_);
        if (due(aged(reject_after_))) expire_current();
        if (due(previous_at_)) discard_previous();
        if (rekey_startable() && due(aged(rekey_after_))) start_attempt();
    }

    /** @brief End the attempt and its unacknowledged session; a previous session stays. */
    void abandon() {
        st_->hs.reset();
        if (stage_ == stage_t::UNACKED) next_->discard();
        stage_ = stage_t::IDLE;
        retry_at_ = attempt_at_ = 0;
    }

    /** @brief The liveness byte (§10): a DIAL link is `UP` once its session is acknowledged. */
    [[nodiscard]] link_state_t state_locked() const noexcept {
        if (role_ == conn_role_t::LISTEN)
            return st_ != nullptr ? link_state_t::LISTENING : link_state_t::BIND_FAILED;
        if (st_ != nullptr && current_->live) return link_state_t::UP;
        if (stage_ == stage_t::IDLE) return link_state_t::DORMANT;
        return lost_ ? link_state_t::RECONNECTING : link_state_t::DIALING;
    }

    /** @brief `now + d`, or 0 (never due) when @p d is 0. */
    [[nodiscard]] std::uint64_t deadline(std::uint64_t d) const noexcept {
        return d == 0 ? 0 : now_ + d;
    }

    /** @brief When the current session reaches age @p span; 0 when undated or @p span is 0. */
    [[nodiscard]] std::uint64_t aged(std::uint64_t span) const noexcept {
        return dated_ && span != 0 ? born_ + span : 0;
    }

    /** @brief Whether the armed deadline @p t has passed. */
    [[nodiscard]] bool due(std::uint64_t t) const noexcept { return t != 0 && now_ >= t; }

    /** @brief Responder: set the mark and persist it through a mark store; the lock is held.
     *  @return False when the mark store's write failed. */
    bool set_mark(std::uint64_t m) {
        if (role_ != conn_role_t::LISTEN) return true;
        mark_ = m;
        if constexpr (noise::mark_store<Host>) return host_.persist_mark(ref_, m);
        return true;
    }

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

    /** @brief Count a malformed inbound message and return no frame. */
    std::span<std::byte> malformed() noexcept {
        drops_.count_malformed_rx();
        return {};
    }

    /** @brief Destroy the state block and return it to its store. */
    void release_state() noexcept {
        if (st_ == nullptr) return;
        for (noise::session_t<B>& s : st_->slots) s.discard();
        st_->~state_t();
        state_src_->release(st_, sizeof(state_t), alignof(state_t));
        st_ = nullptr;
    }

    friend struct noise_link_test_door_t;  // test-only: moves a send nonce to its limits

    conn_role_t role_;               /**< @brief DIAL initiates, LISTEN answers. */
    Host& host_;                     /**< @brief The application's seams. */
    std::uint64_t retry_;            /**< @brief The first retry interval. */
    std::uint64_t retry_cap_;        /**< @brief The largest retry interval. */
    std::uint64_t attempt_;          /**< @brief The attempt deadline's span. */
    std::uint64_t pending_expiry_;   /**< @brief The pending session's span. */
    std::uint64_t rekey_after_;      /**< @brief The session age that starts a rekey. */
    std::uint64_t reject_after_;     /**< @brief The session age that discards it. */
    std::uint64_t previous_expiry_;  /**< @brief The previous session's span. */
    mem::block_source_t* state_src_; /**< @brief `memory.state`: the slot block's store. */
    mem::block_source_t* io_src_;    /**< @brief `memory.io`: the per-send seal block's store. */
    std::size_t max_frame_ = 0;      /**< @brief The largest frame a send accepts. */

    mutable tr::mutex_t mu_; /**< @brief Guards everything below. */
    state_t* st_ = nullptr;  /**< @brief The slot block; null when the link is inert. */
    noise::session_t<B>* current_ = nullptr; /**< @brief The current session's slot. */
    noise::session_t<B>* next_ = nullptr;    /**< @brief The unacknowledged or pending slot. */
    stage_t stage_ = stage_t::IDLE;          /**< @brief The handshake slot's stage. */
    bool peer_fresh_ = false; /**< @brief The handshake's peer started with no session. */
    bool lost_ = false;       /**< @brief DIAL: a session was lost, none replaced it yet. */
    bool boundary_ = false;   /**< @brief A session boundary awaits its notifier. */
    bool dated_ = false;      /**< @brief `born_` holds the current session's first sweep. */
    std::uint64_t mark_ = 0;  /**< @brief Responder: the highest counter accepted (§4). */
    /** @brief The PSK's key-store name, kept only for a mark store's seams. */
    [[no_unique_address]] std::conditional_t<noise::mark_store<Host>, std::string_view, no_ref_t>
        ref_{};
    std::uint64_t peer_ = 0;        /**< @brief Initiator: the configured peer. */
    std::uint64_t now_ = 0;         /**< @brief The last `now` @ref sweep was given. */
    std::uint64_t retry_step_ = 0;  /**< @brief The current retry interval. */
    std::uint64_t retry_at_ = 0;    /**< @brief The retry deadline; 0 none. */
    std::uint64_t attempt_at_ = 0;  /**< @brief The attempt deadline; 0 none. */
    std::uint64_t pending_at_ = 0;  /**< @brief The pending session's expiry; 0 none. */
    std::uint64_t born_ = 0;        /**< @brief The `now` the current session is aged from. */
    std::uint64_t previous_at_ = 0; /**< @brief The previous session's expiry; 0 none. */
    /** @brief The counters, indexed by @ref noise::counter_t. Plain words under `mu_`
     *         (core/STYLE.md §Introspection, counting doctrine rule 5). */
    std::array<std::size_t, std::size_t(noise::counter_t::COUNT)> counts_{};
    /** @brief This link's own drops (#2482), added to the carrier's by @ref drop_stats. It
     *         sheds nothing inbound for backpressure, so it has no `dropped_rx` word. */
    drop_tally_t<drop_classes_t{.dropped_rx = false}> drops_;

    /** @brief The carrier, built last and destroyed first, so its receive thread is joined
     *         before anything it calls into goes. */
    std::optional<Carrier> carrier_;
};

}  // namespace tr::net
