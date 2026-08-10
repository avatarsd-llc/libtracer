/**
 * @file
 * @brief The host fake of `esp_transport_ws`'s inbound frame stream — the driver the
 *        WS *client* link's receive-path suite scripts.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The declarations the chip TU compiles against live in `fake_idf/esp_transport*.h`;
 * this is the model behind them, plus the levers a test needs: script the frames the
 * peer sends, wait for them to be drained, and read back what `esp_pthread_set_cfg` was
 * armed with.
 *
 * The ONE behaviour it reproduces exactly, because both bugs under test live on it
 * (transcribed from `components/tcp_transport/transport_ws.c`):
 *
 *   - a read is capped at the buffer the caller offers, and when a frame is bigger than
 *     that, the REST OF THAT SAME FRAME is served by the following reads —
 *     `ws_read` parses a new header only once `frame_state.bytes_remaining` reaches 0
 *     (transport_ws.c:729). Throughout, `_get_read_opcode` / `_get_fin_flag` keep
 *     reporting that one frame's header. So an oversized *unfragmented* message arrives
 *     as several reads that each look like a complete small message, which is how the
 *     tail of a dropped message reached the router as a bogus frame (#901);
 *   - `_get_read_payload_len` is the frame's TOTAL payload length, unchanged while the
 *     frame drains — the only signal that tells the two apart;
 *   - a zero-payload frame returns 0 from `esp_transport_read`, indistinguishable from a
 *     poll timeout (transport_ws.c:763).
 *
 * There is NO socket in this fake, at any layer: nothing is opened, and every byte the
 * link reads comes from the script above. The ESP-IDF WebSocket plane must never use
 * POSIX sockets (#947), and a fake that opened one would quietly make the suite prove the
 * wrong thing. `esp_transport_get_socket` therefore answers -1 unless a suite opts in
 * with @ref set_socket_fd, which hands back a descriptor NUMBER — enough to reach the
 * link's best-effort socket-option block (#957), with `setsockopt` interposed on the
 * opting-in target so no option reaches the kernel either.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "esp_transport_ws.h"

namespace fake_ws {

/** @brief One scripted inbound WebSocket frame, as the peer would put it on the wire. */
struct frame_t {
    ws_transport_opcodes_t op;      /**< @brief Its opcode (CONT for a continuation). */
    bool fin;                       /**< @brief Its FIN bit. */
    std::vector<std::byte> payload; /**< @brief Its payload, served across as many
                                     *         reads as the reader's buffer forces. */
};

/** @brief Build a frame of @p len bytes filled with a @p seed-derived pattern, so a
 *         delivered message can be checked byte-for-byte against what was sent. */
frame_t make_frame(ws_transport_opcodes_t op, bool fin, std::size_t len, std::uint8_t seed);

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

/** @brief True once every queued frame has been fully read. */
[[nodiscard]] bool drained();

/** @brief How many times a connection was opened (the link re-dials on every drop). */
[[nodiscard]] int connect_count();

/** @brief The `ws_path` the last dial requested. */
[[nodiscard]] std::string last_ws_path();

/**
 * @brief The `headers` standing at each `esp_transport_connect`, in dial order —
 *        `std::nullopt` for a dial whose config left the field null.
 *
 * PER DIAL, not "the last one", and that is the #959 observable: the only way to supply a
 * handshake token ran after the recv thread had already been spawned, which left it
 * UNDEFINED whether the FIRST dial carried one. A `last_headers()` accessor cannot answer
 * that either way — by the time a test read it the second dial would have carried the
 * header and the link would look correct. Sampled at connect (config is applied immediately
 * before it), so entry `n` is what dial `n` actually requested.
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
 * @brief The `stack_size` the last ARMING `esp_pthread_set_cfg` carried, 0 if none.
 *
 * The #900 observable: on IDF this is the only way a `std::thread` gets a stack other
 * than `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT`, so a knob that never reaches here is a
 * knob that never reaches the FreeRTOS task.
 */
[[nodiscard]] std::size_t armed_stack();

/** @brief The `thread_name` that arming config carried, empty if none. */
[[nodiscard]] std::string armed_name();

/**
 * @brief True iff the armed config was RESTORED afterwards (the last `set_cfg` put back
 *        a zero stack size) — the leak-onto-later-spawns half of the recipe.
 */
[[nodiscard]] bool cfg_restored();

/** @name The blocking-discipline levers (#952). */
/** @{ */

/** @brief Make every dial FAIL immediately, so the link spends its reconnect backoff. */
void fail_connects(bool on);

/**
 * @brief Make every dial BLOCK for the timeout it asked for, then fail.
 *
 * What a dial to an unreachable peer does on silicon, and the only way to put the recv
 * thread inside `esp_transport_connect` while a test tears the link down.
 */
void hang_connects(bool on);

/** @brief The `timeout_ms` the last dial asked for — the bound the link chose. */
[[nodiscard]] int last_connect_timeout_ms();

/**
 * @brief Park every `esp_transport_write` for the timeout the caller asked for,
 *        modelling a peer whose TCP window has closed. Passing false releases whoever
 *        is parked, as a peer that starts accepting again would.
 *
 * This is what makes "the send serializer is held across the write" observable: while a
 * writer is parked here it is holding the link's `write_m_`, which is exactly the state
 * the destructor used to ignore. The park honours the caller's timeout, so how long a
 * held write can hold that serializer is the LINK's own bound — pre-#952, three times a
 * 4 s literal.
 */
void hold_writes(bool on);

/** @brief How many callers are parked inside `esp_transport_write` right now. */
[[nodiscard]] int writers_inside();

/** @brief How many `esp_transport_write` calls have been ENTERED since the reset —
 *         counted before any handle check, so a call on a dead handle still counts. */
[[nodiscard]] int writes_started();

/** @brief The `timeout_ms` the last write asked for — IDF spends it up to three times
 *         over inside one call, which is the multiplier the link's bound must respect. */
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

/** @brief Payload bytes the last completed write carried, for the ordinary-path control. */
[[nodiscard]] std::vector<std::byte> last_write_payload();

/** @} */

}  // namespace fake_ws
