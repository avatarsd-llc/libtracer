/**
 * @file
 * @brief #948 — a malformed `send`/`sendmsg` call is NOT a dead socket, and must not
 *        silently swallow the rest of the frame.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `stream_endpoint_t::write_all_iov` treated EVERY non-EINTR failure as "peer gone" and
 * dropped the remainder of the frame without a sound. That conflation is what turned one
 * unimplemented flag on one platform's `sendmsg` — which answers `EOPNOTSUPP`, not a
 * disconnect — into an invisible TOTAL outage: the socket stayed up, the handshake and the
 * pings (single-buffer `send`, a different syscall) kept working, and every data frame
 * vanished. An `EOPNOTSUPP`/`EINVAL` means *our call was malformed*; the socket is healthy
 * and the bytes are still deliverable.
 *
 * The instrument is the `tr::detail::write_fault_inject_hook` seam, because a host kernel
 * emits none of these errnos here — glibc honours every flag the helpers pass, so a test
 * that only drove real sockets would be vacuous by construction (it would classify nothing).
 * The hook fakes ONE rejected attempt; the helpers then run for real over a socketpair, and
 * the reader is the judge: a fixed implementation delivers the whole frame and books the
 * fault as malformed-call, the old one delivers nothing and books it as a disconnect.
 *
 * Four arms:
 *   1. `sendmsg` rejected once  → the multi-span frame still lands, counted malformed.
 *   2. `send` rejected once     → same, on the symmetric single-buffer helper.
 *   3. a REAL dead socket       → drops silently and is NOT counted (the split is a split,
 *                                 not a rename of every failure to "malformed").
 *   4. a PERSISTENT rejection   → terminates (bounded re-attempt), counted twice.
 */

#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/posix_endpoint.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;

/**
 * @brief Test shim republishing the two protected full-write helpers.
 *
 * They are `protected static` on @ref tr::net::stream_endpoint_t — a derived class may
 * re-export them, which lets the test drive them over a raw socketpair with no transport,
 * framing or recv thread in the way (the same shim `write_all_eintr_test` uses).
 */
struct write_probe_t : tr::net::stream_endpoint_t {
    using tr::net::stream_endpoint_t::write_all;
    using tr::net::stream_endpoint_t::write_all_iov;
};

int g_fake_errno = 0; /**< @brief The errno the seam injects while `g_fake_left > 0`. */
int g_fake_left = 0;  /**< @brief Attempts still to be rejected (< 0 = every attempt). */
int g_fake_fired = 0; /**< @brief Injections actually performed (instrument liveness). */

/**
 * @brief The injection seam's hook: fail the next attempt, or let the real syscall run.
 */
int fake_write_fault() noexcept {
    if (g_fake_left == 0) return 0;
    if (g_fake_left > 0) --g_fake_left;
    ++g_fake_fired;
    return g_fake_errno;
}

/**
 * @brief Arm the seam for @p attempts rejections with @p err (negative = unbounded).
 */
void arm_injection(int err, int attempts) {
    g_fake_errno = err;
    g_fake_left = attempts;
    g_fake_fired = 0;
    tr::detail::write_fault_inject_hook = &fake_write_fault;
}

/** @brief Disarm the seam (production state — no test may leave it installed). */
void disarm_injection() {
    tr::detail::write_fault_inject_hook = nullptr;
    g_fake_left = 0;
}

constexpr std::size_t kSpans = 3;    /**< @brief Gather entries in the multi-span arm. */
constexpr std::size_t kSpanLen = 64; /**< @brief Bytes per gather entry (one frame fits the
                                                 socket buffer, so no reader thread is
                                                 needed and the arms stay deterministic). */

/**
 * @brief The payload: a position-derived pattern, so a truncation cannot hide behind a byte
 *        count that happens to match.
 */
std::vector<std::byte> make_payload() {
    std::vector<std::byte> out(kSpans * kSpanLen);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<std::byte>((i * 31u + (i >> 8)) & 0xffu);
    return out;
}

/** @brief What one write arm delivered, and what the write-fault tally made of it. */
struct arm_result_t {
    std::size_t received = 0;    /**< @brief Bytes the peer read before EOF. */
    bool pattern_ok = false;     /**< @brief Whether bytes arrived AND every one matched —
                                             false on an empty read, so a dropped frame can
                                             never pass this check vacuously. */
    std::uint64_t malformed = 0; /**< @brief Malformed-call faults booked during the arm. */
    int last_errno = 0;          /**< @brief The errno the tally recorded last. */
    int injected = 0;            /**< @brief Injections that actually fired. */
};

/**
 * @brief Drive one full-write helper across a socketpair and read back what arrived.
 *
 * @param use_iov false drives `write_all` (`send`), true drives `write_all_iov` (`sendmsg`).
 * @param kill_peer true closes the reading end BEFORE the write — a REAL dead socket, no
 *        injection involved.
 */
arm_result_t run_arm(bool use_iov, bool kill_peer = false) {
    const std::vector<std::byte> payload = make_payload();
    arm_result_t res;

    int sv[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return res;
    if (kill_peer) ::close(sv[1]);

    const tr::net::write_fault_stats_t before = tr::net::write_fault_stats();

    std::vector<std::byte> copy = payload;
    if (use_iov) {
        std::array<::iovec, kSpans> vec{};
        for (std::size_t i = 0; i < kSpans; ++i) vec[i] = {copy.data() + i * kSpanLen, kSpanLen};
        write_probe_t::write_all_iov(sv[0], std::span<const ::iovec>(vec));
    } else {
        write_probe_t::write_all(sv[0], std::span<const std::byte>(copy));
    }

    const tr::net::write_fault_stats_t after = tr::net::write_fault_stats();
    res.malformed = after.malformed_calls - before.malformed_calls;
    res.last_errno = after.last_errno;
    res.injected = g_fake_fired;

    ::shutdown(sv[0], SHUT_WR);
    if (!kill_peer) {
        bool mismatch = false;
        std::vector<std::byte> buf(payload.size() + 1);
        for (;;) {
            const ssize_t n = ::read(sv[1], buf.data(), buf.size());
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            for (ssize_t i = 0; i < n; ++i) {
                const std::size_t at = res.received + static_cast<std::size_t>(i);
                if (at >= payload.size() || buf[static_cast<std::size_t>(i)] != payload[at])
                    mismatch = true;
            }
            res.received += static_cast<std::size_t>(n);
        }
        res.pattern_ok = !mismatch && res.received > 0;
        ::close(sv[1]);
    }
    ::close(sv[0]);
    return res;
}

}  // namespace

/**
 * @brief Runs the #948 write-fault classification cases.
 *
 * @return 0 when every check passed, 1 otherwise.
 */
int main() {
    const std::size_t total = kSpans * kSpanLen;
    std::printf("write-fault classification (#948)\n");

    // 1. sendmsg rejected ONCE with EOPNOTSUPP — the lwIP shape exactly.
    arm_injection(EOPNOTSUPP, 1);
    const arm_result_t iov = run_arm(true);
    disarm_injection();
    std::printf("  sendmsg/EOPNOTSUPP x1: %zu/%zu bytes, %llu malformed, errno %d, %d injected\n",
                iov.received, total, static_cast<unsigned long long>(iov.malformed), iov.last_errno,
                iov.injected);
    check(iov.injected == 1, "instrument live: the seam rejected exactly one sendmsg");
    check(iov.received == total, "write_all_iov does NOT drop the frame on a malformed call");
    check(iov.pattern_ok, "write_all_iov delivers the multi-span payload byte-for-byte");
    check(iov.malformed == 1, "the fault is classified malformed-call (counted), not peer-gone");
    check(iov.last_errno == EOPNOTSUPP, "the surfaced errno names the defect (EOPNOTSUPP)");

    // 2. The symmetric single-buffer helper: one policy, both syscalls.
    arm_injection(EOPNOTSUPP, 1);
    const arm_result_t plain = run_arm(false);
    disarm_injection();
    std::printf("  send/EOPNOTSUPP x1:    %zu/%zu bytes, %llu malformed\n", plain.received, total,
                static_cast<unsigned long long>(plain.malformed));
    check(plain.injected == 1, "instrument live: the seam rejected exactly one send");
    check(plain.received == total, "write_all does NOT drop the buffer on a malformed call");
    check(plain.pattern_ok, "write_all delivers the payload byte-for-byte");
    check(plain.malformed == 1, "write_all books the same malformed-call classification");

    // 3. A REAL dead socket (peer closed, no injection): still silent, still NOT a defect.
    const arm_result_t dead_iov = run_arm(true, /*kill_peer=*/true);
    const arm_result_t dead_send = run_arm(false, /*kill_peer=*/true);
    std::printf("  dead peer:             %llu / %llu malformed\n",
                static_cast<unsigned long long>(dead_iov.malformed),
                static_cast<unsigned long long>(dead_send.malformed));
    check(dead_iov.malformed == 0, "a dead socket is peer-gone, NOT counted as a malformed call");
    check(dead_send.malformed == 0, "same for write_all — the split is a split, not a rename");

    // 4. A PERSISTENT rejection must terminate (bounded re-attempt) and be counted every time.
    arm_injection(EINVAL, -1);
    const arm_result_t stuck = run_arm(true);
    disarm_injection();
    std::printf("  sendmsg/EINVAL always: %zu/%zu bytes, %llu malformed, errno %d\n",
                stuck.received, total, static_cast<unsigned long long>(stuck.malformed),
                stuck.last_errno);
    check(stuck.malformed == 2,
          "a deterministic malformed call is re-attempted ONCE, then given up");
    check(stuck.received == 0, "a persistently malformed call abandons the frame (no spin)");
    check(stuck.last_errno == EINVAL, "the surfaced errno tracks the latest defect");

    check(tr::detail::write_fault_inject_hook == nullptr, "the injection seam is left disarmed");

    return tr::testing::summary("write_fault_class");
}
