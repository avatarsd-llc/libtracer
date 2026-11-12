/**
 * @file
 * @brief The host fake of ESP-IDF's `tcp_transport` byte stream — the driver the WS
 *        *client* link's suites script.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The declarations the chip TU compiles against live in `fake_idf/esp_transport*.h`;
 * this is the model behind them, plus the levers a test needs: script the frames the
 * peer sends, wait for them to be drained, and read back the stack of the thread that
 * dialed.
 *
 * Since #2484 the link reads RAW BYTES and frames them with core's RFC 6455 rules, so the
 * fake is a WebSocket SERVER's end of one TCP stream, nothing more:
 *
 *   - the link's first write on a connection is its opening-handshake request. The fake
 *     parses it (path, Sec-WebSocket-Key, any extra header lines), records what that dial
 *     asked for, and queues a `101 Switching Protocols` with the matching
 *     Sec-WebSocket-Accept on that connection — or a wrong one, under
 *     @ref reject_upgrades;
 *   - every scripted frame is encoded as a server would put it on the wire (unmasked,
 *     2/4/10-byte header) and served from the same stream right behind the `101`, so a frame
 *     queued before the dial arrives in the same read as the response, as a server's
 *     push-on-connect can;
 *   - a read is capped at the buffer the caller offers and serves whatever bytes are
 *     queued, across frame boundaries, exactly as `tcp_read` does; 0 is a timeout and a
 *     negative value the end of the stream;
 *   - closing an upgraded connection discards what was still queued for it: a new
 *     connection is a new stream.
 *
 * Handles have IDENTITY (#1058): every `esp_transport_tcp_init` mints a distinct value and
 * liveness is tracked per handle, because a teardown that lands mid-dial puts two
 * connections on this fake at once — an orphaned dial still holding the one it built while
 * the link that started it is gone and a fresh link has dialed its own. Each handle has its
 * own handshake response; the scripted frames go to the connection upgraded last. A
 * single-connection model would score the orphan's release as a misuse against the live
 * link, and @ref handle_misuse is the counter the whole teardown suite rests on.
 *
 * There is NO socket in this fake, at any layer: nothing is opened, and every byte the
 * link reads comes from the script above. The ESP-IDF WebSocket plane must never use
 * POSIX sockets (#947), and a fake that opened one would quietly make the suite prove the
 * wrong thing. `esp_transport_get_socket` therefore answers -1 unless a suite opts in
 * with @ref set_socket_fd, which hands back a descriptor NUMBER — enough to reach the
 * link's best-effort socket-option block (#957), with `setsockopt` interposed on the
 * opting-in target so no option reaches the kernel either. A frame is one non-blocking
 * `send()` on that socket (#2787); every client target wraps `send` into this fake, which
 * writes it to the connection whose socket the link resolved last, whatever the number.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "libtracer/ws.hpp"

namespace fake_ws {

/** @brief One scripted inbound WebSocket frame, as the peer would put it on the wire. */
struct frame_t {
    tr::net::ws::opcode_t op;       /**< @brief Its opcode (CONT for a continuation). */
    bool fin;                       /**< @brief Its FIN bit. */
    std::vector<std::byte> payload; /**< @brief Its payload, served across as many
                                     *         reads as the reader's buffer forces. */
};

/** @brief Build a frame of @p len bytes filled with a @p seed-derived pattern, so a
 *         delivered message can be checked byte-for-byte against what was sent. */
frame_t make_frame(tr::net::ws::opcode_t op, bool fin, std::size_t len, std::uint8_t seed);

/** @brief Drop all fake state: script, connection bookkeeping, armed pthread config. */
void reset();

/**
 * @brief Queue @p frames for the link's recv thread to read.
 *
 * Appends to whatever is still unread, and wakes a thread parked in
 * `esp_transport_poll_read`. Until the first call, `poll_read` reports "no data", so a
 * test can construct the link, install its receiver, and only then start the stream.
 */
void push_frames(std::vector<frame_t> frames);

/** @brief True once every queued frame has been fully read (or discarded by a close). */
[[nodiscard]] bool drained();

/** @brief Answer every later handshake with a Sec-WebSocket-Accept that does not match its
 *         key, as a server that is not speaking RFC 6455 to this client would. */
void reject_upgrades(bool on);

/**
 * @brief Answer every later handshake with this status line and these header lines, then
 *        the key's Sec-WebSocket-Accept line and the closing CRLF (#2515);
 *        `std::nullopt` restores the default `101`.
 *
 * @param answer The status line without its CRLF, and the header lines, each CRLF-ended.
 */
void answer_upgrades_with(std::optional<std::pair<std::string, std::string>> answer);

/** @brief Opening-handshake requests received since the reset. */
[[nodiscard]] int handshakes();

/** @brief How many times a TCP connect was attempted (the link re-dials on every drop). */
[[nodiscard]] int connect_count();

/** @brief The path the last handshake requested. */
[[nodiscard]] std::string last_ws_path();

/**
 * @brief The extra header lines each handshake request carried, in dial order —
 *        `std::nullopt` for a request with none beyond the standard Upgrade lines.
 *
 * PER DIAL, not "the last one", and that is the #959 observable: the only way to supply a
 * handshake token ran after the recv thread had already been spawned, which left it
 * UNDEFINED whether the FIRST dial carried one. A `last_headers()` accessor cannot answer
 * that either way — by the time a test read it the second dial would have carried the
 * header and the link would look correct. Sampled from each request as it is written, so
 * entry `n` is what handshake `n` actually carried.
 */
[[nodiscard]] std::vector<std::optional<std::string>> dial_headers();

/**
 * @brief Make `esp_transport_get_socket` answer @p fd instead of -1 (reset restores -1).
 *
 * A descriptor NUMBER, not a socket — nothing is opened and no byte moves through it.
 * Its only purpose is to make the link's best-effort socket-option block reachable, so a
 * suite can assert which options a dial applies; the target that opts in also interposes
 * `setsockopt`, so no option reaches the kernel. Every other suite leaves this at -1 and
 * behaves exactly as before.
 */
void set_socket_fd(int fd);

/**
 * @brief The stack size of the thread that made the FIRST `esp_transport_connect` since
 *        @ref reset, read by `pthread_getattr_np` inside the call; 0 if none dialed yet.
 *
 * The #900 observable: the recv thread dials, so this is the stack the `recv_stack` knob
 * actually reached. On the host the link's `tr::static_thread_t` is a pthread sized through
 * its attr (#2288), which is what this reads back.
 */
[[nodiscard]] std::size_t dial_stack();

/** @name The blocking-discipline levers (#952). */
/** @{ */

/** @brief Make every dial FAIL immediately, so the link spends its reconnect backoff. */
void fail_connects(bool on);

/**
 * @brief Make every dial BLOCK for the timeout it asked for, then fail.
 *
 * What a dial to an unreachable peer does on silicon, and the only way to put the recv
 * thread inside `esp_transport_connect` while a test tears the link down. The park is
 * RESOLVABLE — see @ref release_connects — so a suite can decide when the orphaned dial
 * of a torn-down link comes back, instead of paying the whole bound to find out.
 */
void hang_connects(bool on);

/**
 * @brief Resolve every dial parked by @ref hang_connects, and every later one, at once:
 *        @p succeed picks whether they report a completed handshake or a failure.
 *
 * The lever the #1058 cases turn. A teardown that lands mid-dial cannot cancel the dial,
 * so the ORPHANED `esp_transport_connect` comes back later on a thread whose link is
 * already gone — and both of its outcomes have to release the transport pair exactly
 * once. This says WHEN that resolution happens, so the interleaving is scripted rather
 * than raced against a 2.5 s bound. STICKY until @ref reset, so a dial entered after the
 * release resolves immediately instead of parking again.
 */
void release_connects(bool succeed);

/** @brief How many callers are parked inside `esp_transport_connect` right now — the
 *         orphan census a mid-dial teardown case reads. */
[[nodiscard]] int dialers_inside();

/** @brief Handles created and not yet destroyed — the RELEASE oracle: a link (or an
 *         orphaned dial) that released its transport pair leaves this at 0. */
[[nodiscard]] int live_handles();

/**
 * @brief Wait, up to @p limit, until the fake holds NO transport handle and NO parked
 *        dialer. @return whether it got there.
 *
 * Every case that destroys a link must end here, and not as tidiness (#1456). A teardown
 * that lands mid-dial cannot cancel the dial, so the link DETACHES its recv thread
 * (`esp_ws_client_link.cpp`, the #1058 bound): the orphan comes back up to a dial bound
 * later and only then releases its transport pair on this fake. A case that returns
 * before that leaves a thread running at process exit, and `main` returning under it
 * destroys this fake's function-local static state — `~std::mutex` and `~std::map` under
 * a thread that is inside `esp_transport_destroy`, which is undefined behaviour and the
 * TSan race #1456 reported.
 *
 * It is also the release oracle itself: "the bound was released exactly once" is
 * `live_handles() == 0` with @ref handle_misuse at 0. A timeout here is therefore a
 * genuine release defect, never a reason to skip the wait — report it as a failure.
 */
[[nodiscard]] bool wait_drained(std::chrono::milliseconds limit = std::chrono::seconds(5));

/** @brief `esp_transport_close` calls that reached a live handle since the reset — the
 *         "a dial that resolved SUCCESS was closed, not merely destroyed" oracle. */
[[nodiscard]] int close_count();

/** @brief The `timeout_ms` the last dial asked for — the bound the link chose. */
[[nodiscard]] int last_connect_timeout_ms();

/**
 * @brief Fill the peer's TCP window (#2787): every frame `send()` takes nothing and fails
 *        with EAGAIN, as lwIP's does under `MSG_DONTWAIT`. Passing false opens it again.
 *
 * Nothing parks: a link whose write waited on the window would show up as a sender that
 * does not return, which is what #2787 removed. A handshake request is not a frame and is
 * never held.
 */
void hold_writes(bool on);

/**
 * @brief Let one frame `send()` take at most @p bytes (0 = all, the reset), modelling a
 *        window with room for only part of a frame (#2787).
 */
void cap_writes(std::size_t bytes);

/**
 * @brief Park every frame `send()` until this is called with false (#2338). The released
 *        send fails (EPIPE), as one on a connection that broke.
 *
 * A writer that is INSIDE the transport while a test tears the link down: what makes "the
 * handles were destroyed under a sender" a measurement rather than a timing.
 */
void pin_writes(bool on);

/** @brief How many callers are parked inside a frame `send()` right now. */
[[nodiscard]] int writers_inside();

/** @brief How many frame `send()` calls have been ENTERED since the reset —
 *         counted before any handle check, so a call on a dead handle still counts. */
[[nodiscard]] int writes_started();

/** @brief Whole frames the peer has decoded off the stream since the reset, however many
 *         `send()` calls each took. */
[[nodiscard]] int frames_written();

/** @brief The flags of the last frame `send()`, -1 before the first: `MSG_DONTWAIT` is how a
 *         write never waits (#2787). */
[[nodiscard]] int last_send_flags();

/** @brief The `timeout_ms` the last `esp_transport_write` asked for: the opening
 *         handshake's, the one write the link still bounds. */
[[nodiscard]] int last_write_timeout_ms();

/**
 * @brief Operations that touched a handle they must not have — the use-after-free
 *        counter, and the whole point of the teardown suite.
 *
 * Three things count: an operation on a NULL handle, an operation on a handle already
 * passed to `esp_transport_destroy`, and a write that was INSIDE the transport when its
 * handle was destroyed under it (the real call dereferences the handle throughout, so a
 * destroy mid-call is a use-after-free even though the pointer was live on entry).
 */
[[nodiscard]] int handle_misuse();

/** @brief Payload bytes the last whole frame carried, for the ordinary-path
 *         control — unmasked, as the peer would read them. A handshake request is not a
 *         frame, and none of the write counters below count one. */
[[nodiscard]] std::vector<std::byte> last_write_payload();

/** @name The per-frame write cost (#2265). */
/** @{ */

/** @brief The WS opcode of the last whole frame, -1 before the first. */
[[nodiscard]] int last_write_op();

/** @brief Whether that frame carried the MASK bit, as every client frame must. */
[[nodiscard]] bool last_write_masked();

/**
 * @brief lwIP calls the frame writes made since the reset: one `send()` each (#2787), where
 *        `tcp_write` was a poll and a send.
 */
[[nodiscard]] int lwip_calls();

/** @brief Bytes handed to lwIP's send since the reset: whole frames, header included. */
[[nodiscard]] std::size_t wire_bytes();

/** @brief Park every `esp_transport_read` until released, NOT bounded by its timeout;
 *         passing false releases them. */
void hold_reads(bool on);

/** @brief How many callers are parked inside `esp_transport_read` right now. */
[[nodiscard]] int readers_inside();

/**
 * @brief Park every `esp_transport_get_socket` until called with false.
 *
 * The link asks for the descriptor right after a dial resolves and before it publishes the
 * connection, so this holds a dial in exactly the window a teardown racing a resolved dial
 * lands in (#1606).
 */
void park_get_socket(bool on);

/** @brief True while a caller is parked by @ref park_get_socket. */
[[nodiscard]] bool get_socket_parked();

/** @} */

/** @} */

/**
 * @brief Drive @p link's `sweep` from a fake application clock until @p pred holds or
 *        @p limit of wall time passes (#2337). @return whether @p pred held.
 *
 * The link's reconnect backoff runs on the application's `now`, so a case that waits for a
 * re-dial after a backoff has to move a clock. Each turn moves this suite's clock one
 * backoff forward and sweeps: the first turn that finds the link backing off stamps the
 * backoff's end, and the next turn passes it. The clock only ever moves forward, across
 * every case in the process.
 */
template <class link_t, class pred_t>
bool sweep_until(link_t& link, pred_t pred, std::chrono::milliseconds limit) {
    static std::uint64_t now = 1;
    const std::uint64_t step = static_cast<std::uint64_t>(link_t::timing().reconnect_backoff_ms);
    const auto give_up = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= give_up) return pred();
        now += step;
        (void)link.sweep(now, now);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}  // namespace fake_ws
