/**
 * @file
 * @brief The PEER-DRIVEN allocation guards in `transport_webtransport.cpp`, gated: an
 *        out-of-process peer lets the injector be armed at the SERVER only.
 *
 * Two vectors from #1182 (stream adoption, handshake accumulation) and two from #934 (the
 * extended CONNECT's `:path` copy and its 200 response) — the last two being the sites the
 * #934 quic/webtransport ingress audit found still THROWING on a pre-auth path, where a
 * `bad_alloc` unwinds into libmsquic's C frames and takes the node down rather than
 * refusing the peer.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ## Why this needed a second process
 *
 * #1108 shipped three webtransport fixes UNGATED, and the reason was the instruments, not the
 * fixes. Both allocation sites here run on msquic worker threads, and neither injection seam
 * this tree owns could be aimed at one side of an in-process client/server pair:
 *
 *  - the `operator new` injector in `transport_alloc_softfail_test.cpp` is `thread_local` to
 *    the arming thread, so a worker never observes it — that file names this blind spot for
 *    its own two PONG sites;
 *  - `tr::detail::probe_fail_hook` IS process-wide and `try_reserve` consults it, so it does
 *    reach a worker — and also the in-process peer. Measured on #1108's branch: armed, with
 *    the server guard reverted, peak `live_streams()` never rose above baseline because the
 *    CLIENT's own opens were being refused. The test measured the client and passed
 *    vacuously on unguarded server code.
 *
 * So the peer moves out of the process (`wt_peer_driver`, `fork`+`exec`), and the hook armed
 * here reaches only this process's workers. It is the same `raw_wt_client_t` every other
 * classifier vector drives (raw_wt_client.hpp), so the gated peer cannot drift from them.
 *
 * ## Why each vector is not vacuous
 *
 * Every case runs the SAME sequence twice — once unarmed, once armed — and the unarmed arm is
 * a positive control that must show the effect the armed arm must not:
 *
 *  - adoption: unarmed, the peer's stream MUST reach the server (`base+1` while it is open).
 *    A harness that stopped driving the server would fail there instead of passing quietly.
 *  - accumulate: unarmed, the same two writes MUST NOT abort the stream, so the abort in the
 *    armed arm can only be the guard.
 *
 * The refusal COUNTER is the third leg: an armed arm that never reached a `try_reserve` at all
 * would satisfy "nothing was adopted" for the wrong reason, so each armed arm also asserts the
 * injector actually fired.
 *
 * ## The sampling contract (#1182's second trap)
 *
 * `live_streams()` is sampled WHILE the peer's stream is open, never after `close_stream`:
 * since #1163 a finished stream reclaims its own ctx, so a post-close sample reads the
 * baseline whether or not the stream was ever adopted — which is how the first attempt at
 * this test passed on broken code.
 *
 * ## Arming is a flag, not a pointer write
 *
 * `probe_fail_hook` is installed ONCE in `main`, before any transport (hence any msquic
 * thread) exists, and the mid-test arming flips an `std::atomic<bool>` the hook reads. Writing
 * the function pointer between cases would be a genuine data race against the workers that
 * read it — a race TSan would be right to report, and one the suppression file deliberately
 * does not cover (it suppresses races INSIDE libmsquic, never libtracer's own).
 *
 * ## Assessment — should `quic_test` / `webtransport_test` migrate to this driver? (#1182)
 *
 * No, not wholesale. The out-of-process peer buys exactly one thing: a failure injector that
 * can tell the two sides apart. Every other vector in those suites asserts on protocol
 * behaviour the in-process client already observes directly and far more cheaply — a stream
 * handle, an abort flag, delivered bytes — and moving them behind a line protocol would trade
 * direct assertions for string parsing, add a process launch per case, and put the peer's
 * state out of reach of the debugger. The driver is worth its cost only where a
 * process-scoped seam has to be aimed. That assessment has now been taken up twice: #934's
 * two sites extended this file rather than migrating the suites, exactly as it said they
 * should, and the extension cost was one optional `SESSION` argument plus one `WAITSHUT`
 * verb on the driver.
 */

#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/mem_heap.hpp"
#include "libtracer/transport_webtransport.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
using tr::net::webtransport_transport_t;
using tr::testing::check;

/** @brief Dev cert paths — generated once in main() by tools/gen-dev-cert.sh. */
std::string g_cert;
std::string g_key;

// ---- the injector: installed once, armed by flag (see the file header) ----

/** @brief Armed => every probed growth in THIS process is refused. */
std::atomic<bool> g_armed{false};

/** @brief Growth probes the hook refused since the last @ref arm — the proof an armed arm
 *         reached a `try_reserve` at all, rather than passing because nothing allocated. */
std::atomic<int> g_refusals{0};

/**
 * @brief Non-zero => refuse ONLY a growth of exactly this many bytes (#934).
 *
 * The two #1108 vectors below refuse everything, because the site they aim at is the first
 * one their sequence reaches. The #934 vectors cannot: the extended CONNECT walks
 * `on_peer_stream_started` → `accumulate` → `classify_bidi` in that order, so a
 * refuse-everything arming would abort the stream in `accumulate` and the two sites at the
 * END of that walk — the `:path` copy and the 200 response — would never run. Both have a
 * byte count nothing else on the path shares (the peer chooses the `:path` length; the 200
 * response is a 5-byte protocol constant), so an EXACT-SIZE arming aims at them precisely.
 * The `refused_sessions()` counter is the cross-check that the byte count really did land
 * on the intended site and not on a coincidence.
 */
std::atomic<std::size_t> g_armed_size{0};

/** @brief The process-wide OOM injector: refuse while armed — everything, or only the one
 *         byte count @ref g_armed_size names. */
bool refuse_while_armed(std::size_t bytes) noexcept {
    if (!g_armed.load(std::memory_order_acquire)) return true;
    const std::size_t only = g_armed_size.load(std::memory_order_relaxed);
    if (only != 0 && bytes != only) return true;
    g_refusals.fetch_add(1, std::memory_order_relaxed);
    return false;
}

/** @brief Arm the injector and reset the refusal count.
 *  @param only_bytes 0 = refuse every probed growth; otherwise refuse only that size. */
void arm(std::size_t only_bytes = 0) {
    g_refusals.store(0, std::memory_order_relaxed);
    g_armed_size.store(only_bytes, std::memory_order_relaxed);
    g_armed.store(true, std::memory_order_release);
}

/** @brief Disarm the injector. */
void disarm() { g_armed.store(false, std::memory_order_release); }

// ---- the out-of-process peer ----

/**
 * @brief A `wt_peer_driver` child and the synchronous line protocol to it.
 *
 * Every command blocks for its one reply, so a step the test takes is ordered after an
 * acknowledged one. Reads are `poll`-bounded: a wedged driver fails the suite instead of
 * hanging ctest until the harness timeout.
 */
struct peer_driver_t {
    pid_t pid = -1;  /**< @brief The child. */
    int wfd = -1;    /**< @brief Write end of the child's stdin. */
    int rfd = -1;    /**< @brief Read end of the child's stdout. */
    bool ok = false; /**< @brief The child was spawned. */
    std::string buf; /**< @brief Bytes read past the last complete reply line. */

    explicit peer_driver_t(const char* exe) {
        int to_child[2] = {-1, -1};
        int from_child[2] = {-1, -1};
        if (::pipe(to_child) != 0) return;
        if (::pipe(from_child) != 0) {
            ::close(to_child[0]);
            ::close(to_child[1]);
            return;
        }
        pid = ::fork();
        if (pid < 0) return;
        if (pid == 0) {
            // Child: wire the pipes to stdin/stdout and exec. Everything between fork and
            // exec is async-signal-safe, and exec replaces the image, so the parent's msquic
            // threads (which fork did not copy) are never touched.
            ::dup2(to_child[0], STDIN_FILENO);
            ::dup2(from_child[1], STDOUT_FILENO);
            ::close(to_child[0]);
            ::close(to_child[1]);
            ::close(from_child[0]);
            ::close(from_child[1]);
            ::execl(exe, exe, static_cast<char*>(nullptr));
            ::_exit(127);
        }
        ::close(to_child[0]);
        ::close(from_child[1]);
        wfd = to_child[1];
        rfd = from_child[0];
        ok = true;
    }

    ~peer_driver_t() {
        if (!ok) return;
        // QUIT lets the driver destroy its client (closing handles, draining callbacks) so
        // the server sees a clean departure rather than a killed peer.
        (void)cmd("QUIT");
        ::close(wfd);
        int status = 0;
        for (int i = 0; i < 200 && ::waitpid(pid, &status, WNOHANG) == 0; ++i)
            std::this_thread::sleep_for(10ms);
        if (::waitpid(pid, &status, WNOHANG) == 0) {
            ::kill(pid, SIGKILL);
            (void)::waitpid(pid, &status, 0);
        }
        ::close(rfd);
    }

    /** @brief Send @p line and return its one reply ("" on a dead or wedged driver). */
    std::string cmd(const std::string& line) {
        if (!ok) return {};
        const std::string out = line + "\n";
        if (::write(wfd, out.data(), out.size()) != static_cast<ssize_t>(out.size())) return {};
        return read_line();
    }

    /** @brief One reply line, bounded by @p budget. */
    std::string read_line(std::chrono::milliseconds budget = 15000ms) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        for (;;) {
            if (const auto nl = buf.find('\n'); nl != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                return line;
            }
            const auto left = deadline - std::chrono::steady_clock::now();
            if (left <= std::chrono::milliseconds::zero()) return {};
            pollfd p{rfd, POLLIN, 0};
            const int rc =
                ::poll(&p, 1,
                       static_cast<int>(
                           std::chrono::duration_cast<std::chrono::milliseconds>(left).count()));
            if (rc <= 0) return {};
            char chunk[512];
            const ssize_t n = ::read(rfd, chunk, sizeof(chunk));
            if (n <= 0) return {};
            buf.append(chunk, static_cast<std::size_t>(n));
        }
    }

    /** @brief `OPEN` a unidirectional stream, returning its tag (or nullopt).
     *  @param immediate Announce the stream with no payload (guard 1's vector). */
    std::optional<std::size_t> open_uni(bool immediate = false) {
        const std::string r = cmd(immediate ? "OPENI" : "OPEN");
        std::istringstream is(r);
        std::string verb;
        std::size_t tag = 0;
        std::string status;
        is >> verb >> tag >> status;
        if (verb != "OPEN" || status != "ok") return std::nullopt;
        return tag;
    }

    /** @brief Write @p hex on the stream @p tag names. */
    [[nodiscard]] bool write_hex(std::size_t tag, std::string_view hex) {
        std::ostringstream os;
        os << "WRITE " << tag << ' ' << hex;
        return cmd(os.str()) == "WRITE ok";
    }

    /** @brief Whether the peer has seen the server refuse the stream @p tag names. */
    [[nodiscard]] bool is_aborted(std::size_t tag) {
        std::ostringstream os;
        os << "ISABORT " << tag;
        return cmd(os.str()) == "ISABORT 1";
    }

    /** @brief Block (in the DRIVER) until the stream @p tag names is refused. */
    [[nodiscard]] bool wait_aborted(std::size_t tag, std::chrono::milliseconds budget) {
        std::ostringstream os;
        os << "WAITABORT " << tag << ' ' << budget.count();
        return cmd(os.str()) == "WAITABORT 1";
    }

    /** @brief Whether the peer has seen the server tear the whole CONNECTION down — the
     *         shape a count-then-close refusal takes on this side (#934). */
    [[nodiscard]] bool wait_shutdown(std::chrono::milliseconds budget) {
        std::ostringstream os;
        os << "WAITSHUT " << budget.count();
        return cmd(os.str()) == "WAITSHUT 1";
    }

    /** @brief Send the extended CONNECT requesting @p path. True when the peer got it out
     *         (the SERVER decides whether a session follows). */
    [[nodiscard]] bool session(std::string_view path) {
        std::ostringstream os;
        os << "SESSION 127.0.0.1:0 " << path;
        std::istringstream is(cmd(os.str()));
        std::string verb;
        std::size_t tag = 0;
        std::string status;
        is >> verb >> tag >> status;
        return verb == "SESSION" && status == "ok";
    }

    /** @brief Close the stream @p tag names (abort + close, so the server sees it finish). */
    void close(std::size_t tag) {
        std::ostringstream os;
        os << "CLOSE " << tag;
        (void)cmd(os.str());
    }
};

// ---- observation ----

/**
 * @brief The highest `live_streams()` over @p window, sampled TIGHTLY.
 *
 * Nothing else goes in this loop — in particular no command to the driver. An earlier version
 * polled the peer's abort flag between samples, and a round trip to another process is long
 * enough that a SHORT-LIVED adoption (adopted, then aborted by the other guard, then reaped
 * by #1163's reclamation) slipped between two reads: the vector passed on code with its guard
 * removed. Guard 1's vector now also keeps the adoption permanent — see `OPENI` — so this
 * measures a steady state rather than racing one.
 */
std::size_t peak_streams(const webtransport_transport_t& t, std::chrono::milliseconds window) {
    std::size_t peak = 0;
    const auto deadline = std::chrono::steady_clock::now() + window;
    do {
        peak = std::max(peak, t.live_streams());
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return std::max(peak, t.live_streams());
}

/** @brief Block until the LISTEN side reports its session up. */
[[nodiscard]] bool wait_session(const webtransport_transport_t& t,
                                std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!t.session_up()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

/** @brief Block until `live_streams()` reaches @p want (adoption / reclamation settle). */
[[nodiscard]] bool wait_streams(const webtransport_transport_t& t, std::size_t want,
                                std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (t.live_streams() != want) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

/**
 * @brief Bring up a listener + an out-of-process peer with an established session.
 * @return The baseline `live_streams()`, or nullopt when the fixture failed (every caller
 *         reports that itself, so a broken fixture never reads as a passing guard).
 */
std::optional<std::size_t> establish(webtransport_transport_t& listener, peer_driver_t& peer) {
    if (!peer.ok) return std::nullopt;
    std::ostringstream os;
    os << "CONNECT " << listener.local_port();
    if (peer.cmd(os.str()) != "CONNECT ok") return std::nullopt;
    std::istringstream is(peer.cmd("SESSION 127.0.0.1:0"));
    std::string verb;
    std::size_t tag = 0;
    std::string status;
    is >> verb >> tag >> status;
    if (verb != "SESSION" || status != "ok") return std::nullopt;
    if (!wait_session(listener, 5000ms)) return std::nullopt;
    return listener.live_streams();
}

/**
 * @brief GUARD 1 — `on_peer_stream_started`'s `try_reserve(ctxs, ...)`.
 *
 * The stream is announced with msquic's IMMEDIATE flag and carries NO payload, so the only
 * server path it provokes is adoption: no RECEIVE follows it, `accumulate` is never called,
 * and guard 2 cannot influence the outcome. That also makes the observable a STEADY STATE —
 * an adopted ctx just sits in `ctxs` — rather than a window that has to be caught.
 *
 * Armed, the ctx-list growth is refused and the count stays at the baseline. Remove that
 * `try_reserve` and the `push_back` grows the list through the real allocator (the hook gates
 * the PROBE, not `operator new`): the stream is adopted, the count sits at `base+1`, and the
 * assertion below reds — as does the refusal counter, since the removed call is the only
 * thing that would have consulted the injector.
 *
 * ## Why the ARMED arm runs first
 *
 * `try_reserve(v, n)` returns true WITHOUT consulting the hook when `n <= v.capacity()`, and
 * `ctxs` only ever grows by `reserve(size + 1)` — so capacity tracks size exactly, and every
 * adoption really does probe. Cycling a stream first breaks that: #1163's reclamation erases
 * an entry and leaves capacity one ahead, so the next adoption fits and never reaches the
 * injector. Measured — with the control arm first, this vector passed on code with the guard
 * REMOVED. The refusal-counter assertion is what caught it, and is why it stays.
 */
void test_peer_stream_adoption_guard_is_gated() {
    std::printf("WebTransport — the peer-stream ctx growth is failable (#1108/#1182):\n");
    webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
    check(listener.ok(), "listener started (ALPN h3, ephemeral port, dev cert)");
    peer_driver_t peer(LIBTRACER_WT_PEER_DRIVER);
    check(peer.ok, "the peer driver runs in a SEPARATE process (the injector is ours alone)");

    const std::optional<std::size_t> base = establish(listener, peer);
    check(base.has_value(), "session established through the out-of-process peer");
    if (!base) return;
    check(*base > 0, "the live session holds its own streams (a zero baseline would be vacuous)");

    // Gated arm FIRST (see the header): the injector is armed in the SERVER's process only.
    arm();
    const std::optional<std::size_t> tag = peer.open_uni(/*immediate=*/true);
    const std::size_t armed_peak = tag ? peak_streams(listener, 2000ms) : 0;
    const bool armed_aborted = tag && peer.is_aborted(*tag);
    disarm();

    check(tag.has_value(), "armed: the peer still opened its stream (its process is unaffected)");
    check(g_refusals.load(std::memory_order_relaxed) > 0,
          "the injector fired in the server process (an unreached try_reserve would be vacuous)");
    check(armed_peak == *base,
          "ARMED: the ctx growth was refused, so the stream was NEVER adopted");
    check(armed_aborted, "and the peer saw that refusal as a stream abort");
    check(listener.session_up(), "the refusal was stream-scoped — the session survived (#919)");
    if (tag) peer.close(*tag);

    // Control arm: unarmed, the SAME announcement must reach the server and STAY adopted —
    // so the armed arm's silence is the guard, not a peer that stopped driving.
    const auto ctl_start = std::chrono::steady_clock::now();
    const std::optional<std::size_t> ctl = peer.open_uni(/*immediate=*/true);
    check(ctl.has_value(), "control: the peer announced a payload-free stream (IMMEDIATE)");
    if (!ctl) return;
    const bool adopted = wait_streams(listener, *base + 1, 5000ms);
    const auto ctl_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - ctl_start);
    check(adopted, "UNARMED: the peer's stream IS adopted — the count rises while it is open");
    check(ctl_ms < 500ms,
          "and it was prompt — the armed window above dwarfed the time adoption needs");
    peer.close(*ctl);
}

/**
 * @brief GUARD 2 — `accumulate`'s `try_reserve(c.acc, ...)`.
 *
 * The stream is adopted BEFORE the injector is armed, so guard 1 is out of the picture and
 * this vector is about the handshake buffer alone. Armed, the growth that would hold the next
 * chunk is refused and the stream (not the connection) is aborted. Remove that `try_reserve`
 * and the `insert` reallocates through the real allocator: no abort, and the assertion reds.
 */
void test_handshake_accumulate_guard_is_gated() {
    std::printf("WebTransport — the handshake accumulate growth is failable (#1108/#1182):\n");
    webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
    check(listener.ok(), "listener started (ALPN h3, ephemeral port, dev cert)");
    peer_driver_t peer(LIBTRACER_WT_PEER_DRIVER);
    check(peer.ok, "the peer driver runs in a SEPARATE process");

    const std::optional<std::size_t> base = establish(listener, peer);
    check(base.has_value(), "session established through the out-of-process peer");
    if (!base) return;

    // Control arm: the SAME two writes, unarmed, must not abort anything — so the abort in
    // the gated arm can only be the guard, never the bytes.
    const std::optional<std::size_t> ctl = peer.open_uni();
    check(ctl.has_value(), "control: the peer opened a unidirectional stream");
    if (!ctl) return;
    check(peer.write_hex(*ctl, "40"), "control: first handshake byte written");
    check(wait_streams(listener, *base + 1, 5000ms), "control: the stream was adopted");
    check(peer.write_hex(*ctl, "41"), "control: second handshake byte written");
    check(!peer.wait_aborted(*ctl, 1000ms),
          "UNARMED: the two-byte handshake completes — the stream is not refused");
    peer.close(*ctl);
    check(wait_streams(listener, *base, 5000ms), "control: the cycled stream was reclaimed");

    // Gated arm.
    const std::optional<std::size_t> tag = peer.open_uni();
    check(tag.has_value(), "the peer opened a unidirectional stream");
    if (!tag) return;
    check(peer.write_hex(*tag, "40"), "first handshake byte written");
    check(wait_streams(listener, *base + 1, 5000ms),
          "the stream was ADOPTED before the injector is armed (so guard 1 is not in play)");

    arm();
    const bool wrote = peer.write_hex(*tag, "41");
    const bool aborted = wrote && peer.wait_aborted(*tag, 5000ms);
    disarm();

    check(wrote, "second handshake byte written (the peer's process is unaffected)");
    check(g_refusals.load(std::memory_order_relaxed) > 0,
          "the injector fired in the server process");
    check(aborted,
          "ARMED: the handshake buffer could not grow, so THIS STREAM was refused (#1108)");
    check(listener.session_up(),
          "and the session the peer had already established stayed up — OOM is stream-scoped");
    peer.close(*tag);
}

// ---- #934: the extended-CONNECT answer path, which USED to abort the node ----

/**
 * @brief Bring a listener + out-of-process peer to the point just BEFORE the extended
 *        CONNECT: QUIC is up, the server has presented its H3 face, nothing is armed yet.
 * @return False when the fixture failed (the caller reports it, so a broken fixture never
 *         reads as a passing guard).
 */
[[nodiscard]] bool connect_only(webtransport_transport_t& listener, peer_driver_t& peer) {
    if (!peer.ok) return false;
    std::ostringstream os;
    os << "CONNECT " << listener.local_port();
    return peer.cmd(os.str()) == "CONNECT ok";
}

/** @brief Block until the LISTEN side has counted @p want refused CONNECTs. */
[[nodiscard]] bool wait_refused(const webtransport_transport_t& t, std::uint64_t want,
                                std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (t.refused_sessions() < want) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

/**
 * @brief GUARD 3 — the `:path` copy in the extended-CONNECT answer (#934).
 *
 * Before this car that line was `path = std::string(req_path)`: a THROWING, PEER-SIZED
 * allocation on an msquic stream callback, i.e. inside libmsquic's C frames, in a module
 * with no `catch` anywhere. On a tight heap an unauthenticated peer — one QUIC handshake,
 * one bidi stream, one HEADERS frame, no ACL, no subscription — took the whole process
 * down. This vector could not even be WRITTEN against that code: there is no assertion for
 * "and then the test binary terminated".
 *
 * Armed at the ONE byte count the `:path` copy asks for (its length + the NUL), the copy is
 * refused; the answer is count-then-close, so `refused_sessions()` moves, no session comes
 * up, the peer sees the connection go down — and this process is still here to say so.
 * Unarmed, the identical CONNECT establishes a session and `session_path()` returns the
 * very path that was refused, which is what makes the armed arm's silence the guard rather
 * than a peer that stopped driving. Each arm gets a FRESH listener, so the `path` member's
 * capacity starts at SSO in both and the ordering carries no hidden state.
 */
void test_connect_path_copy_is_failable() {
    std::printf("WebTransport — the extended CONNECT's :path copy refuses, never aborts (#934):\n");
    // 512 bytes of `:path`, so the copy asks for 513 (the NUL) — a byte count nothing else
    // on this walk shares: the accumulate reserve takes the whole ~559-byte CONNECT frame
    // and the ctx-list reserve takes a multiple of a pointer.
    const std::string long_path = "/" + std::string(511, 'a');
    const std::size_t copy_bytes = long_path.size() + 1;

    {
        webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
        check(listener.ok(), "armed: listener started (ALPN h3, ephemeral port, dev cert)");
        peer_driver_t peer(LIBTRACER_WT_PEER_DRIVER);
        check(peer.ok, "armed: the peer driver runs in a SEPARATE process");
        check(connect_only(listener, peer), "armed: QUIC up, H3 face presented, nothing armed");

        arm(copy_bytes);
        const bool sent = peer.session(long_path);
        const bool counted = sent && wait_refused(listener, 1, 5000ms);
        const bool peer_saw_close = counted && peer.wait_shutdown(5000ms);
        disarm();

        check(sent, "armed: the peer sent its extended CONNECT (its process is unaffected)");
        check(g_refusals.load(std::memory_order_relaxed) > 0,
              "the injector fired in the server process (an unreached try_assign is vacuous)");
        check(counted, "ARMED: the CONNECT was REFUSED and counted (refused_sessions())");
        check(!listener.session_up(), "and no session was left half-established");
        check(peer_saw_close, "the peer saw the count-then-CLOSE (#934's 2026-08-15 ruling)");
        check(listener.session_path().empty(),
              "the refused :path was never recorded (try_assign leaves dst unchanged)");
    }

    // Control arm: the SAME CONNECT, unarmed, MUST establish — so the armed arm's refusal
    // is the guard and not a path this server rejects for some other reason.
    {
        webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
        check(listener.ok(), "control: listener started");
        peer_driver_t peer(LIBTRACER_WT_PEER_DRIVER);
        check(peer.ok, "control: the peer driver runs in a SEPARATE process");
        check(connect_only(listener, peer), "control: QUIC up");
        check(peer.session(long_path), "control: the peer sent the same extended CONNECT");
        check(wait_session(listener, 5000ms), "UNARMED: the session IS established");
        check(listener.session_path() == long_path, "and the 512-byte :path was recorded");
        check(listener.refused_sessions() == 0, "with nothing refused");
    }
}

/**
 * @brief GUARD 4 — the 200 response's one owned copy (#934).
 *
 * The response BYTES are now a `constexpr` view of static storage (`wt_h3::
 * status_200_headers_frame`), so the vector and the field-section vector that used to be
 * built per CONNECT are deleted rather than guarded. What survives is the single copy
 * msquic owns until SEND_COMPLETE — unavoidable, because the seam's spans are borrowed only
 * for the `StreamSend` call — and that copy is what this vector refuses.
 *
 * Armed at exactly 5 bytes (the frame's fixed length: HEADERS type, length, and the 3-byte
 * `:status: 200` section), `send_ctx_t::make_raw` returns null, `send_raw` answers false,
 * and the CONNECT is refused with the same count-then-close. Unarmed, the identical
 * sequence establishes the session.
 */
void test_connect_response_send_is_failable() {
    std::printf("WebTransport — the 200 response's owned copy refuses, never aborts (#934):\n");
    {
        webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
        check(listener.ok(), "armed: listener started");
        peer_driver_t peer(LIBTRACER_WT_PEER_DRIVER);
        check(peer.ok, "armed: the peer driver runs in a SEPARATE process");
        check(connect_only(listener, peer), "armed: QUIC up");

        // The root path fits `path`'s SSO buffer, so guard 3 never probes and this vector is
        // about the response copy alone.
        arm(5);
        const bool sent = peer.session("/");
        const bool counted = sent && wait_refused(listener, 1, 5000ms);
        const bool peer_saw_close = counted && peer.wait_shutdown(5000ms);
        disarm();

        check(sent, "armed: the peer sent its extended CONNECT");
        check(g_refusals.load(std::memory_order_relaxed) > 0,
              "the injector fired in the server process");
        check(counted, "ARMED: the 200 could not be copied, so the CONNECT was counted-and-closed");
        check(!listener.session_up(), "and the session never came up");
        check(peer_saw_close, "the peer saw the connection close rather than the node die");
    }

    {
        webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
        check(listener.ok(), "control: listener started");
        peer_driver_t peer(LIBTRACER_WT_PEER_DRIVER);
        check(peer.ok, "control: the peer driver runs in a SEPARATE process");
        check(connect_only(listener, peer), "control: QUIC up");
        check(peer.session("/"), "control: the peer sent the same extended CONNECT");
        check(wait_session(listener, 5000ms), "UNARMED: the session IS established");
        check(listener.refused_sessions() == 0, "with nothing refused");
    }
}

}  // namespace

int main() {
    // The injector is installed ONCE, here, before any transport (hence any msquic thread)
    // exists: the arming a case does is an atomic flag, never a write to this pointer.
    tr::detail::probe_fail_hook = &refuse_while_armed;

    char tmpl[] = "/tmp/libtracer-wtgate-XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (dir == nullptr) {
        std::printf("FAIL: mkdtemp\n");
        return 1;
    }
    const std::string cmd = std::string("sh ") + LIBTRACER_DEV_CERT_SCRIPT + " " + dir;
    if (std::system(cmd.c_str()) != 0) {
        std::printf("FAIL: gen-dev-cert.sh\n");
        return 1;
    }
    g_cert = std::string(dir) + "/cert.pem";
    g_key = std::string(dir) + "/key.pem";

    test_peer_stream_adoption_guard_is_gated();
    test_handshake_accumulate_guard_is_gated();
    test_connect_path_copy_is_failable();
    test_connect_response_send_is_failable();

    tr::detail::probe_fail_hook = nullptr;
    return tr::testing::summary("webtransport_alloc_gate");
}
