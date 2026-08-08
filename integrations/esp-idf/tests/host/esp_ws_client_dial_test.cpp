/**
 * @file
 * @brief #959 — the ESP-IDF WebSocket *client* link's DIAL/CONFIG surface, on the host.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #900/#901 receive-path suite and the #952 teardown suite: the
 * REAL chip translation unit (`integrations/esp-idf/libtracer/esp_ws_client_link.cpp`) is
 * compiled against the host fake of `esp_transport_ws` (fake_esp_transport.hpp). No socket
 * is opened at any layer — this plane must never use POSIX sockets (#947).
 *
 * The defect #959 names is an ORDERING one. `set_handshake_headers()` was the only way to
 * supply a board-to-board auth token, and it necessarily ran AFTER the constructor had
 * already spawned the recv thread that dials. So the wiring task's write to
 * `handshake_headers_` was concurrent and unsynchronized with `connect_once()`'s
 * `.empty()`/`.c_str()` read of it: a data race on a `std::string`, where a reallocating
 * assignment can hand `esp_transport_ws_set_config` a `cfg.headers` that is already freed.
 * Nothing in the API decided which side of that race won, so whether the FIRST dial
 * carried the token was undefined — and a dial without one is refused by a peer that gates
 * admission, costing a reconnect backoff before the next dial. First-dial liveness was
 * therefore nondeterministic by construction.
 *
 * WHAT PINS WHAT — read this before reading a green run here as an ordering result:
 *
 *   - The ORDERING is pinned by the COMPILE BREAK, not by any case below. The headers are
 *     a ctor argument and the member is `const`, so a post-spawn write cannot be written:
 *     from outside there is no setter (the static_assert below fails to COMPILE if one is
 *     reintroduced), and inside the ctor an assignment after the spawn does not compile
 *     against a `const` member.
 *   - No runtime case here pins it, and `test_first_dial_carries_the_headers` in
 *     particular does not. Measured, not assumed: a rebuilt pre-fix ordering (headers
 *     dropped from the ctor init-list, member made non-`const`, assigned on the line
 *     AFTER `esp::spawn_thread`) passes this whole suite 20/20 runs on the host it was
 *     tried on — the write wins the race every time there. Read that case as pinning the
 *     OBSERVABLE (the first recorded dial carries the token), which a fix applying the
 *     headers only on a rebuild would break, and as pinning nothing about ordering.
 *   - The race itself is closed by CONSTRUCTION: there is no longer a write to race, so
 *     the `tsan` leg of this target has nothing to find on this seam. A test that hammered
 *     a setter would be pinning the setter, and the setter is what was removed.
 *
 * Two more cases guard what a naive fix would break: a re-dial carries the headers too (a
 * snapshot taken once at construction and not re-read would still pass the first case),
 * and no headers still leaves `cfg.headers` NULL, so the handshake stays byte-for-byte the
 * historical request rather than gaining an empty header block.
 *
 * Also pinned: the `tx_bytes` ceiling is now VISIBLE (#959's second half). The COUNTER
 * landed with #942, and a bump on it does not say which drop happened — `send()`'s
 * `!connected_` arm and its short-write arm bump the same `st_.tx_drops`, so an oversize
 * frame and a peer being down are indistinguishable on that channel. What is new here is
 * the LOG, the only thing that names the frame size and the knob; this suite asserts on
 * both, capturing the log off file descriptor 2.
 */

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "fake_esp_transport.hpp"
#include "libtracer_esp/esp_ws_client_link.hpp"

namespace {

using namespace std::chrono_literals;

/**
 * @brief Does @p link_t still offer a post-construction handshake-header setter?
 *
 * Named as a question so the assertion below reads as the answer. This is the detection
 * half of the ORDERING guard; the file header says why it has to be a compile-time one.
 */
template <class link_t>
concept has_post_ctor_header_setter =
    requires(link_t& link, std::string headers) { link.set_handshake_headers(std::move(headers)); };

/**
 * @brief #959 — there is no post-construction handshake-header surface.
 *
 * The guard for the ordering half of the issue, and a COMPILE-time one on purpose: the
 * runtime cases below cannot tell a write ordered before the spawn from one that races the
 * spawn and wins (file header). Reintroducing `set_handshake_headers()` — the exact
 * surface #959 removed — makes this translation unit fail to compile.
 *
 * What it does NOT cover: the other way a post-spawn write could come back, an assignment
 * inside the ctor after `esp::spawn_thread`. That one is rejected by the member's `const`,
 * in the implementation TU itself, and removing that `const` is the visible change a
 * reviewer sees.
 */
static_assert(!has_post_ctor_header_setter<tr::net::esp_ws_client_link_t>,
              "#959: handshake headers are a ctor argument — a setter necessarily runs after "
              "the recv thread has already dialed");

/**
 * @brief #959 — the headers occupy the FOURTH constructor slot, and a buffer size cannot
 *        land in it by accident.
 *
 * The positive assert is what keeps the negative one from being vacuous: without it, a
 * type that had stopped being constructible at all would satisfy the negative too.
 */
static_assert(std::is_constructible_v<tr::net::esp_ws_client_link_t, std::string, std::uint16_t,
                                      std::string, std::string>,
              "#959: (host, port, ws_path, handshake_headers) must construct");
static_assert(!std::is_constructible_v<tr::net::esp_ws_client_link_t, std::string, std::uint16_t,
                                       std::string, std::size_t>,
              "#959: a std::size_t in the headers slot must be a hard error, not a silently "
              "re-interpreted buffer size");

/** @brief Failed-check counter; main() turns it into the exit status. */
int g_failures = 0;

/** @brief Record one assertion. */
void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL: %s\n", what);
    } else {
        std::printf("  ok:   %s\n", what);
    }
}

/** @brief Spin until @p pred holds or @p limit elapses. @return whether it held. */
template <typename pred_t>
bool wait_until(pred_t pred, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

/**
 * @brief Run @p body with file descriptor 2 redirected to a temp file. @return what was
 *        written there while it ran (empty if the capture could not be set up).
 *
 * Descriptor-level (`dup2`), NOT `freopen`: the recv thread shares this process's `stderr`
 * stream, glibc serializes concurrent writes through it, and swapping the descriptor
 * underneath is safe where replacing the stream object would not be. The cost is that
 * anything the recv thread logs inside the window also lands in the buffer — harmless,
 * because the caller searches for a substring rather than comparing the whole capture.
 */
template <typename body_t>
std::string capture_stderr(body_t&& body) {
    std::fflush(stderr);
    const int saved = ::dup(STDERR_FILENO);
    std::FILE* const sink = std::tmpfile();
    if (saved < 0 || sink == nullptr) {
        // Capture unavailable: still run the body, and let the caller's assertion fail
        // loudly rather than silently skip.
        body();
        if (saved >= 0) ::close(saved);
        if (sink != nullptr) std::fclose(sink);
        return {};
    }
    ::dup2(::fileno(sink), STDERR_FILENO);
    body();
    std::fflush(stderr);
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    std::rewind(sink);
    std::string captured;
    char chunk[256];
    std::size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof chunk, sink)) > 0) captured.append(chunk, n);
    std::fclose(sink);
    return captured;
}

/** @brief Buffer size every case here sizes its frames against — this suite is about the
 *         dial, so the sizes only have to be large enough not to be the subject. */
constexpr std::size_t kBufBytes = 64;

/** @brief The token header a board-to-board dial carries. CRLF-terminated, which is the
 *         form `esp_transport_ws` emits verbatim. Not a real credential — the shape is
 *         the whole point. */
const std::string kToken = "X-Graph-Token: abc123\r\n";

/**
 * @brief #959 — the FIRST recorded dial carries the handshake headers.
 *
 * This pins the OBSERVABLE, not the ordering. Indexing at zero rejects a fix that applies
 * the headers only when the transport pair is rebuilt on a reconnect — which reading "the
 * last dial" would let through. It does NOT separate the shipped fix from a post-spawn
 * write that wins the race: a rebuilt pre-fix ordering passed this case 20/20 runs on the
 * host it was tried on. The ordering is guarded by the compile break asserted above; see
 * the file header.
 */
void test_first_dial_carries_the_headers() {
    std::printf("#959 the FIRST dial carries the handshake headers:\n");
    fake_ws::reset();
    {
        tr::net::esp_ws_client_link_t link("127.0.0.1", 8080, "/ws", kToken, kBufBytes, kBufBytes,
                                           0);
        check(wait_until([] { return fake_ws::connect_count() >= 1; }, 2s), "the link dialed");
        const auto dials = fake_ws::dial_headers();
        check(!dials.empty(), "a dial was recorded");
        // Indexed at ZERO deliberately: reading "the last headers" would pass on the
        // broken surface as soon as a re-dial had happened.
        check(!dials.empty() && dials[0].has_value(), "dial ONE set cfg.headers");
        check(!dials.empty() && dials[0].value_or("") == kToken, "and it carried the token");
        check(fake_ws::last_ws_path() == "/ws", "the path went out on the same dial");
    }
}

/**
 * @brief #959 — a RE-dial carries them too.
 *
 * The fix must not be "snapshot the token once and hand it to the first dial": the recv
 * thread rebuilds the transport pair on every reconnect (connect_once destroys and
 * re-creates both handles for a clean frame state), so the headers have to be re-applied
 * to each new `esp_transport_ws` handle or a peer that gates admission refuses every
 * reconnect after the first. The first dial is failed on purpose so the second one is a
 * genuine rebuild rather than the same handle re-used.
 */
void test_every_dial_carries_the_headers() {
    std::printf("#959 a RE-dial carries the headers too:\n");
    fake_ws::reset();
    fake_ws::fail_connects(true);
    {
        tr::net::esp_ws_client_link_t link("127.0.0.1", 8080, "/ws", kToken, kBufBytes, kBufBytes,
                                           0);
        check(wait_until([] { return fake_ws::connect_count() >= 1; }, 2s),
              "the first dial failed");
        fake_ws::fail_connects(false);
        // One reconnect backoff (kReconnectBackoffMs, 1.5 s) separates the two dials.
        check(wait_until([] { return fake_ws::connect_count() >= 2; }, 10s), "the link re-dialed");
        const auto dials = fake_ws::dial_headers();
        check(dials.size() >= 2, "both dials were recorded");
        bool all_carried = !dials.empty();
        for (const auto& h : dials) all_carried = all_carried && h.value_or("") == kToken;
        check(all_carried, "EVERY dial carried the token, not just the first");
    }
}

/**
 * @brief #959 — with no headers, `cfg.headers` stays NULL.
 *
 * "No token" must be the historical request byte-for-byte, not a request with an empty
 * header block appended: the two are different bytes on the wire and a peer parsing the
 * upgrade is entitled to notice.
 */
void test_no_headers_leaves_the_field_null() {
    std::printf("#959 no headers leaves cfg.headers null:\n");
    fake_ws::reset();
    {
        // The default — the same call every existing embedder writes.
        tr::net::esp_ws_client_link_t link("127.0.0.1", 8080, "/ws", {}, kBufBytes, kBufBytes, 0);
        check(wait_until([] { return fake_ws::connect_count() >= 1; }, 2s), "the link dialed");
        const auto dials = fake_ws::dial_headers();
        check(!dials.empty(), "a dial was recorded");
        check(!dials.empty() && !dials[0].has_value(), "and it left cfg.headers null");
    }
}

/**
 * @brief #959 — an outbound frame larger than `tx_bytes` is refused, COUNTED and LOGGED.
 *
 * `transport_t::send` returns void, so nothing is handed back to the caller. The counter
 * (#942) records that a frame died but not which of `send()`'s three drop arms killed it:
 * the `!connected_` arm and the short-write arm bump the same `st_.tx_drops`. The LOG is
 * the only channel that names the frame size and the buffer it exceeded, so this case
 * asserts on both — the counter moves, and the line reaches stderr with both numbers. The
 * ordinary-size control in the same case is what makes it a measurement of the CEILING
 * rather than of sending being broken.
 */
void test_oversize_frame_is_refused_and_counted() {
    std::printf("#959 an oversized outbound frame is counted, not silently binned:\n");
    fake_ws::reset();
    {
        tr::net::esp_ws_client_link_t link("127.0.0.1", 8080, "/ws", {}, kBufBytes, kBufBytes, 0);
        // `ok()`, NOT connect_count(): the fake counts the dial on ENTRY to
        // esp_transport_connect, but the link publishes `connected_` only after the
        // socket-option block and the stats latch that follow it. A send in that window
        // takes the `!connected_` arm and bumps tx_drops, which would make the
        // ordinary-path control below fail perhaps one run in a hundred. The other cases
        // here may wait on the dial count because they only read what the dial REQUESTED.
        check(wait_until([&] { return link.ok(); }, 2s), "the link came up");
        const std::vector<std::byte> ok_frame(kBufBytes, std::byte{0x11});
        link.send(std::span<const std::byte>(ok_frame));
        check(link.stats().c.tx_frames == 1, "a frame that fits is sent");
        check(link.stats().c.tx_drops == 0, "and is not counted as a drop");
        const std::vector<std::byte> big(kBufBytes + 1, std::byte{0x22});
        const std::string logged =
            capture_stderr([&] { link.send(std::span<const std::byte>(big)); });
        check(link.stats().c.tx_drops == 1, "one byte over the ceiling is counted as a drop");
        check(link.stats().c.tx_frames == 1, "and did not reach the transport");
        // Asserted rather than described: the counter above is shared with the peer-down
        // and short-write arms, so this line is the only place the ceiling is nameable.
        const std::string wanted = "outbound frame " + std::to_string(kBufBytes + 1) +
                                   " B exceeds " + std::to_string(kBufBytes) + " B tx buffer";
        check(logged.find(wanted) != std::string::npos,
              "and the drop is LOGGED with both sizes, which the counter cannot say");
    }
}

}  // namespace

int main() {
    std::printf("esp_ws_client_link dial/config host suite (#959):\n");
    test_first_dial_carries_the_headers();
    test_every_dial_carries_the_headers();
    test_no_headers_leaves_the_field_null();
    test_oversize_frame_is_refused_and_counted();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
