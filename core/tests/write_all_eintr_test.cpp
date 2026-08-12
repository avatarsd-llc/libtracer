/**
 * @file
 * @brief #903 — the full-write helpers must RESUME an interrupted write, not truncate.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `stream_endpoint_t::write_all` / `write_all_iov` carry complete pre-encoded
 * frames on persistent framed streams. A signal that interrupts the blocked
 * `send`/`sendmsg` before any byte moved fails with EINTR on a perfectly healthy
 * connection; abandoning the rest of the buffer there leaves a PARTIAL frame on
 * a live stream and desyncs the peer's framing forever.
 *
 * The instrument is a real EINTR, not a fake: a socketpair with a small
 * SO_SNDBUF, a reader that drains deliberately slowly so the writer spends most
 * of its life blocked in the syscall, and a signaller thread that `pthread_kill`s
 * the WRITER thread with SIGUSR1 every 200 us. The handler is installed WITHOUT
 * SA_RESTART, so every delivery that lands on a blocked, buffer-full send returns
 * -1/EINTR with `off > 0` — exactly the mid-frame truncation case. The reader then
 * counts (and pattern-checks) every byte up to EOF: a helper that resumes delivers
 * the whole payload, one that gives up delivers a prefix.
 */

#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/posix_endpoint.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;

/**
 * @brief Test shim republishing the two protected full-write helpers.
 *
 * They are `protected static` on @ref tr::net::stream_endpoint_t — a derived
 * class may re-export them, which lets the test drive them over a raw
 * socketpair with no transport, framing or recv thread in the way.
 */
struct write_probe_t : tr::net::stream_endpoint_t {
    using tr::net::stream_endpoint_t::write_all;
    using tr::net::stream_endpoint_t::write_all_iov;
};

/** @brief Count of SIGUSR1 deliveries — proves the interrupt instrument is live. */
volatile std::sig_atomic_t g_signals = 0;

/**
 * @brief SIGUSR1 handler: does nothing but count (its point is the EINTR it causes).
 *
 * @param sig The delivered signal (unused).
 */
void on_sigusr1(int sig) {
    (void)sig;
    // Simple assignment, not ++: a compound op on a volatile is deprecated in C++20.
    g_signals = g_signals + 1;
}

constexpr std::size_t kPayload = 256 * 1024; /**< @brief Bytes pushed through the pair. */
constexpr std::size_t kSndBuf = 4096;        /**< @brief Small send buffer → the writer blocks. */
constexpr std::size_t kReadChunk = 2048;     /**< @brief Reader bite size (small = slow drain). */

/** @brief Sleep @p usec microseconds (the reader's and signaller's pacing). */
void sleep_us(long usec) {
    timespec ts{.tv_sec = 0, .tv_nsec = usec * 1000};
    ::nanosleep(&ts, nullptr);
}

/**
 * @brief The payload: a position-derived pattern, so a truncation cannot hide
 *        behind a byte count that happens to match.
 */
std::vector<std::byte> make_payload() {
    std::vector<std::byte> out(kPayload);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<std::byte>((i * 31u + (i >> 8)) & 0xffu);
    return out;
}

/** @brief What one interrupted-write run observed on the reading end. */
struct run_result_t {
    std::size_t received = 0;   /**< @brief Bytes read before EOF. */
    bool pattern_ok = true;     /**< @brief Whether every received byte matched. */
    std::sig_atomic_t sigs = 0; /**< @brief SIGUSR1 deliveries during the write. */
};

/**
 * @brief Drive one full-write helper across a socketpair while signals rain on it.
 *
 * @param use_iov false drives `write_all`, true drives `write_all_iov` (the
 *                sibling that already retried — the control arm).
 * @return What the reader saw.
 */
run_result_t run_interrupted_write(bool use_iov) {
    const std::vector<std::byte> payload = make_payload();

    int sv[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return {};
    const int snd = static_cast<int>(kSndBuf);
    ::setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
    ::setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &snd, sizeof(snd));

    g_signals = 0;
    std::atomic<bool> writing{true};
    std::atomic<pthread_t> writer_tid{};
    std::atomic<bool> tid_ready{false};

    run_result_t res;

    // Reader: drain in small bites with a pause between them, so the writer
    // spends most of the transfer blocked in send with a full buffer.
    std::thread reader([&] {
        std::vector<std::byte> buf(kReadChunk);
        std::size_t off = 0;
        for (;;) {
            const ssize_t n = ::read(sv[1], buf.data(), buf.size());
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;  // EOF — the writer shut its half down
            for (ssize_t i = 0; i < n; ++i) {
                if (off + static_cast<std::size_t>(i) >= payload.size() ||
                    buf[static_cast<std::size_t>(i)] != payload[off + static_cast<std::size_t>(i)])
                    res.pattern_ok = false;
            }
            off += static_cast<std::size_t>(n);
            sleep_us(300);
        }
        res.received = off;
    });

    std::thread writer([&] {
        writer_tid.store(::pthread_self(), std::memory_order_release);
        tid_ready.store(true, std::memory_order_release);
        if (use_iov) {
            // Three gather entries — the same payload, sent as one record.
            std::vector<std::byte> copy = payload;
            const std::size_t third = payload.size() / 3;
            ::iovec vec[3];
            vec[0] = {copy.data(), third};
            vec[1] = {copy.data() + third, third};
            vec[2] = {copy.data() + 2 * third, payload.size() - 2 * third};
            write_probe_t::write_all_iov(sv[0], std::span<const ::iovec>(vec, 3));
        } else {
            write_probe_t::write_all(sv[0], std::span<const std::byte>(payload));
        }
        writing.store(false, std::memory_order_release);
        ::shutdown(sv[0], SHUT_WR);
    });

    // Signaller: hammer the WRITER thread only, so the reader's read(2) is never
    // the one interrupted.
    std::thread signaller([&] {
        while (!tid_ready.load(std::memory_order_acquire)) sleep_us(50);
        const pthread_t target = writer_tid.load(std::memory_order_acquire);
        while (writing.load(std::memory_order_acquire)) {
            ::pthread_kill(target, SIGUSR1);
            sleep_us(200);
        }
    });

    writer.join();
    signaller.join();
    reader.join();
    ::close(sv[0]);
    ::close(sv[1]);

    res.sigs = g_signals;
    return res;
}

}  // namespace

/**
 * @brief Runs the #903 interrupted-write cases.
 *
 * @return 0 when every check passed, 1 otherwise.
 */
int main() {
    // No SA_RESTART: an interrupted blocking send must surface EINTR rather than
    // be restarted by the kernel — that is the whole instrument.
    struct sigaction sa {};
    sa.sa_handler = &on_sigusr1;
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGUSR1, &sa, nullptr);

    std::printf("write_all EINTR (#903)\n");

    const run_result_t plain = run_interrupted_write(false);
    std::printf("  write_all:     %zu/%zu bytes, %d signals\n", plain.received, kPayload,
                static_cast<int>(plain.sigs));
    check(plain.sigs > 0, "instrument live: SIGUSR1 was delivered during write_all");
    check(plain.received == kPayload, "write_all resumes an interrupted write (whole frame lands)");
    check(plain.pattern_ok, "write_all delivers the payload byte-for-byte");

    const run_result_t iov = run_interrupted_write(true);
    std::printf("  write_all_iov: %zu/%zu bytes, %d signals\n", iov.received, kPayload,
                static_cast<int>(iov.sigs));
    check(iov.sigs > 0, "instrument live: SIGUSR1 was delivered during write_all_iov");
    check(iov.received == kPayload, "write_all_iov resumes an interrupted write (control arm)");
    check(iov.pattern_ok, "write_all_iov delivers the payload byte-for-byte");

    // A closed/absent peer stays a silent no-op under the shared policy.
    write_probe_t::write_all(-1, std::span<const std::byte>(make_payload()));
    check(true, "write_all on a negative fd is a no-op");

    return tr::testing::summary("write_all_eintr");
}
