/**
 * @file
 * @brief The peer-path counted metrics (#2137): user-space copies, socket syscalls and
 *        voluntary task switches per relayed frame, and idle wakeups per second.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * These are the WIDE (x86 host) rows of the counted-metrics ledger
 * (the JSON pin files under `bench/ledger/`, gated by `bench/counted_ledger.py`). Every figure is a
 * COUNT, not a time: a count does not move with host load, so the ledger can gate it per PR on a
 * hosted runner without an A/A pair.
 *
 * Relay rows. Node B (this process) is measured. It has an ingress link `in`, a vertex
 * `/sensor/temp` (STORED_VALUE) with one local callback subscriber, and one remote
 * subscriber bound with `subscribe_toward` to node C. Writer A and node C run in a
 * second process (this binary re-executed with `--peer`), so nothing they do lands in
 * B's counters. Each frame is a `FWD{WRITE}` carrying one VALUE of the row's payload
 * size; A sends it, B decodes it, stores it, delivers it locally and forwards it to C.
 * The run is PACED: the next frame leaves A only after C has received the previous one
 * and B's local subscriber has seen it, so every receive reads exactly one frame and a
 * count does not depend on how the kernel batched bytes.
 *
 *   `<k>-relay-cli` rows: A reaches B over link kind k, and B reaches C over a client
 *   link of the same kind (B dials C).
 *   `<k>-relay-srv` rows: C dials a second server link on B, and B forwards over it.
 *   `udp-relay-<out>` rows: A writes over UDP, and B forwards over `<out>` (tcp-cli,
 *   tcp-srv, ws-cli, ws-srv or udp). Every UDP link here sets `max_frame` to 32 KiB, so
 *   the 4 KiB and 16 KiB datagrams are admitted; at the 1,472 B default they are refused.
 *
 * What is counted, inside the armed window only:
 *   - `copy_bytes@N` / `copy_calls@N`: bytes and calls through `memcpy` / `memmove`
 *     (and their fortified `_chk` forms), interposed in this executable. libtracer is a
 *     static archive, so every variable-size copy it makes binds here. Constant-size
 *     copies the compiler inlines are invisible, which hides only headers. The WebSocket
 *     client's masking pass (`ws::detail::mask_copy`, #2145) copies the payload with word
 *     loads and stores, not memcpy, so this executable compiles its own transport_ws.cpp
 *     with LIBTRACER_COUNT_MASK_COPY naming `bench_note_mask_copy`, and each copying mask
 *     pass is counted as one copy of its size. A masking pass in place (the receive
 *     unmask) copies nothing and is not counted.
 *   - `syscalls@N`: calls B's link threads make to every libc syscall wrapper the peer
 *     path can reach: socket I/O (recv*, send*, read*, write*, recvmmsg, sendmmsg),
 *     waiting (poll, ppoll, select, pselect, epoll_wait, epoll_pwait, epoll_pwait2,
 *     epoll_ctl, eventfd_read, eventfd_write), connection setup and teardown (accept,
 *     accept4, connect, shutdown, close, setsockopt, getsockopt) and yielding or sleeping
 *     (sched_yield, nanosleep, clock_nanosleep), interposed the same way. The main thread
 *     is excluded: it is the harness, and it only drives the pacing pipe. NOT counted:
 *     futex. glibc issues it from inside pthread (mutexes, condition variables) with an
 *     inline syscall, so no wrapper sees it, and an unprivileged process cannot count it
 *     from the kernel (`perf_event_paranoid` 2 hides the syscall tracepoints). The
 *     peer-cost study measured 0.00 futex calls per frame on every link with strace.
 *   - `vcsw@N`: voluntary context switches of every thread except main, read from
 *     `/proc/self/task/<tid>/status`. Main only blocks on the harness pipe.
 *
 * Idle rows (`<kind>-idle-32 wakeups_per_s`): one process, a server and 32 dialers (UDP:
 * 32 link pairs), no traffic, and the voluntary switches of every non-main thread over a
 * one-second window. Today that is the receive timeouts that exist only to re-check a
 * stop flag; the ledger's target for it is 0.
 *
 * Output: `RESULT arm=<arm> metric=<metric> n=<reps> median=<v> min=<v> max=<v>`, one
 * line per row, the median over `--reps` interleaved repetitions.
 *
 *     ./bench_peer_counts [--reps=3] [--frames=200] [--no-idle]
 */
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <spawn.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/fwd_router.hpp"
#include "libtracer/mem_poly_ptr.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_ws.hpp"

extern char** environ;

namespace {

/** @brief True only inside the measured window of the measured process. */
std::atomic<bool> g_armed{false};
std::atomic<long long> g_copy_bytes{0};
std::atomic<long long> g_copy_calls{0};
std::atomic<long long> g_syscalls{0};
/** @brief Set on the harness's main thread, whose calls are never charged to node B. */
thread_local bool t_harness = false;

/** @brief Charge one copy to the window. */
inline void note_copy(std::size_t n) noexcept {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    g_copy_bytes.fetch_add(static_cast<long long>(n), std::memory_order_relaxed);
    g_copy_calls.fetch_add(1, std::memory_order_relaxed);
}

/** @brief Charge one syscall made by one of B's link threads to the window. */
inline void note_syscall() noexcept {
    if (g_armed.load(std::memory_order_relaxed) && !t_harness)
        g_syscalls.fetch_add(1, std::memory_order_relaxed);
}

/** @brief A forward byte copy that never calls memcpy itself. */
void* copy_forward(void* d, const void* s, std::size_t n) noexcept {
    auto* dd = static_cast<unsigned char*>(d);
    const auto* ss = static_cast<const unsigned char*>(s);
    while (n >= 8) {
        std::uint64_t w;
        __builtin_memcpy(&w, ss, 8);
        __builtin_memcpy(dd, &w, 8);
        dd += 8;
        ss += 8;
        n -= 8;
    }
    while (n-- != 0) *dd++ = *ss++;
    return d;
}

/** @brief An overlap-safe byte move that never calls memmove itself. */
void* move_bytes(void* d, const void* s, std::size_t n) noexcept {
    auto* dd = static_cast<unsigned char*>(d);
    const auto* ss = static_cast<const unsigned char*>(s);
    if (dd < ss) {
        while (n-- != 0) *dd++ = *ss++;
    } else {
        dd += n;
        ss += n;
        while (n-- != 0) *--dd = *--ss;
    }
    return d;
}

/** @brief The next definition of a libc symbol this executable interposes. */
template <class F>
F next_fn(const char* name) noexcept {
    return reinterpret_cast<F>(::dlsym(RTLD_NEXT, name));
}

}  // namespace

// --- interposition -------------------------------------------------------------------
// Each wrapper counts, then calls the real libc entry point. The fortified `_chk`
// forms are counted the same way and forwarded to the plain call.

extern "C" void* memcpy(void* __restrict d, const void* __restrict s, std::size_t n) {
    note_copy(n);
    return copy_forward(d, s, n);
}
extern "C" void bench_note_mask_copy(std::size_t n) noexcept { note_copy(n); }
extern "C" void* memmove(void* d, const void* s, std::size_t n) {
    note_copy(n);
    return move_bytes(d, s, n);
}
extern "C" void* __memcpy_chk(void* d, const void* s, std::size_t n, std::size_t) {
    note_copy(n);
    return copy_forward(d, s, n);
}
extern "C" void* __memmove_chk(void* d, const void* s, std::size_t n, std::size_t) {
    note_copy(n);
    return move_bytes(d, s, n);
}

// `BENCH_INTERPOSE` wraps a declaration glibc makes without `__THROW`, `BENCH_INTERPOSE_NT`
// one it makes with it (the definition must repeat the `noexcept`). A wrapper whose libc
// has no such entry point fails the call with ENOSYS, as the kernel would.
#define BENCH_INTERPOSE_IMPL(ret, name, params, args, spec) \
    extern "C" ret name params spec {                       \
        static auto real = next_fn<ret(*) params>(#name);   \
        note_syscall();                                     \
        if (real == nullptr) {                              \
            errno = ENOSYS;                                 \
            return static_cast<ret>(-1);                    \
        }                                                   \
        return real args;                                   \
    }
#define BENCH_INTERPOSE(ret, name, params, args) BENCH_INTERPOSE_IMPL(ret, name, params, args, )
#define BENCH_INTERPOSE_NT(ret, name, params, args) \
    BENCH_INTERPOSE_IMPL(ret, name, params, args, noexcept)

// Socket and file I/O.
BENCH_INTERPOSE(ssize_t, read, (int fd, void* b, size_t n), (fd, b, n))
BENCH_INTERPOSE(ssize_t, write, (int fd, const void* b, size_t n), (fd, b, n))
BENCH_INTERPOSE(ssize_t, readv, (int fd, const struct iovec* v, int c), (fd, v, c))
BENCH_INTERPOSE(ssize_t, writev, (int fd, const struct iovec* v, int c), (fd, v, c))
BENCH_INTERPOSE(ssize_t, recv, (int fd, void* b, size_t n, int f), (fd, b, n, f))
BENCH_INTERPOSE(ssize_t, recvfrom,
                (int fd, void* __restrict b, size_t n, int f, struct sockaddr* __restrict a,
                 socklen_t* __restrict l),
                (fd, b, n, f, a, l))
BENCH_INTERPOSE(ssize_t, recvmsg, (int fd, struct msghdr* m, int f), (fd, m, f))
BENCH_INTERPOSE(int, recvmmsg,
                (int fd, struct mmsghdr* v, unsigned int n, int f, struct timespec* t),
                (fd, v, n, f, t))
BENCH_INTERPOSE(ssize_t, send, (int fd, const void* b, size_t n, int f), (fd, b, n, f))
BENCH_INTERPOSE(ssize_t, sendto,
                (int fd, const void* b, size_t n, int f, const struct sockaddr* a, socklen_t l),
                (fd, b, n, f, a, l))
BENCH_INTERPOSE(ssize_t, sendmsg, (int fd, const struct msghdr* m, int f), (fd, m, f))
BENCH_INTERPOSE(int, sendmmsg, (int fd, struct mmsghdr* v, unsigned int n, int f), (fd, v, n, f))
// Waiting.
BENCH_INTERPOSE(int, poll, (struct pollfd * p, nfds_t n, int t), (p, n, t))
BENCH_INTERPOSE(int, ppoll,
                (struct pollfd * p, nfds_t n, const struct timespec* t, const sigset_t* s),
                (p, n, t, s))
BENCH_INTERPOSE(int, select,
                (int n, fd_set* __restrict r, fd_set* __restrict w, fd_set* __restrict e,
                 struct timeval* __restrict t),
                (n, r, w, e, t))
BENCH_INTERPOSE(int, pselect,
                (int n, fd_set* __restrict r, fd_set* __restrict w, fd_set* __restrict e,
                 const struct timespec* __restrict t, const sigset_t* __restrict s),
                (n, r, w, e, t, s))
BENCH_INTERPOSE(int, epoll_wait, (int e, struct epoll_event* v, int m, int t), (e, v, m, t))
BENCH_INTERPOSE(int, epoll_pwait, (int e, struct epoll_event* v, int m, int t, const sigset_t* s),
                (e, v, m, t, s))
BENCH_INTERPOSE(int, epoll_pwait2,
                (int e, struct epoll_event* v, int m, const struct timespec* t, const sigset_t* s),
                (e, v, m, t, s))
BENCH_INTERPOSE_NT(int, epoll_ctl, (int e, int op, int fd, struct epoll_event* v), (e, op, fd, v))
BENCH_INTERPOSE(int, eventfd_read, (int fd, eventfd_t* v), (fd, v))
BENCH_INTERPOSE(int, eventfd_write, (int fd, eventfd_t v), (fd, v))
// Connection setup and teardown.
BENCH_INTERPOSE(int, accept, (int fd, struct sockaddr* __restrict a, socklen_t* __restrict l),
                (fd, a, l))
BENCH_INTERPOSE(int, accept4,
                (int fd, struct sockaddr* __restrict a, socklen_t* __restrict l, int f),
                (fd, a, l, f))
BENCH_INTERPOSE(int, connect, (int fd, const struct sockaddr* a, socklen_t l), (fd, a, l))
BENCH_INTERPOSE_NT(int, shutdown, (int fd, int how), (fd, how))
BENCH_INTERPOSE(int, close, (int fd), (fd))
BENCH_INTERPOSE_NT(int, setsockopt, (int fd, int lv, int nm, const void* v, socklen_t l),
                   (fd, lv, nm, v, l))
BENCH_INTERPOSE_NT(int, getsockopt,
                   (int fd, int lv, int nm, void* __restrict v, socklen_t* __restrict l),
                   (fd, lv, nm, v, l))
// Yielding and sleeping.
BENCH_INTERPOSE_NT(int, sched_yield, (void), ())
BENCH_INTERPOSE(int, nanosleep, (const struct timespec* r, struct timespec* m), (r, m))
BENCH_INTERPOSE(int, clock_nanosleep,
                (clockid_t c, int f, const struct timespec* r, struct timespec* m), (c, f, r, m))

extern "C" ssize_t __read_chk(int fd, void* b, size_t n, size_t) { return read(fd, b, n); }
extern "C" ssize_t __recv_chk(int fd, void* b, size_t n, size_t, int f) {
    return recv(fd, b, n, f);
}
extern "C" ssize_t __recvfrom_chk(int fd, void* __restrict b, size_t n, size_t, int f,
                                  struct sockaddr* __restrict a, socklen_t* __restrict l) {
    return recvfrom(fd, b, n, f, a, l);
}
extern "C" int __poll_chk(struct pollfd* p, nfds_t n, int t, size_t) { return poll(p, n, t); }

#undef BENCH_INTERPOSE_NT
#undef BENCH_INTERPOSE
#undef BENCH_INTERPOSE_IMPL

namespace {

using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::transport_t;

// --- harness pipe I/O: raw syscalls, so the counters never see them ------------------

/** @brief Write all of @p s to @p fd without going through the interposed wrappers. */
void pipe_put(int fd, std::string_view s) {
    while (!s.empty()) {
        const long n = ::syscall(SYS_write, fd, s.data(), s.size());
        if (n <= 0) std::_Exit(90);
        s.remove_prefix(static_cast<std::size_t>(n));
    }
}

/** @brief Read one '\n'-terminated line from @p fd (the newline is dropped). */
std::string pipe_line(int fd) {
    std::string out;
    char c = 0;
    while (::syscall(SYS_read, fd, &c, 1) == 1) {
        if (c == '\n') return out;
        out.push_back(c);
    }
    std::_Exit(91);
}

/** @brief Read one byte from @p fd. */
char pipe_byte(int fd) {
    char c = 0;
    if (::syscall(SYS_read, fd, &c, 1) != 1) std::_Exit(92);
    return c;
}

/** @brief Voluntary context switches per thread id, read from /proc. */
std::map<int, long> voluntary_switches() {
    std::map<int, long> out;
    DIR* d = ::opendir("/proc/self/task");
    if (d == nullptr) return out;
    while (dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        char path[320];
        std::snprintf(path, sizeof path, "/proc/self/task/%s/status", e->d_name);
        FILE* f = std::fopen(path, "r");
        if (f == nullptr) continue;
        char line[256];
        long v = 0;
        while (std::fgets(line, sizeof line, f) != nullptr)
            (void)std::sscanf(line, "voluntary_ctxt_switches: %ld", &v);
        std::fclose(f);
        out[std::atoi(e->d_name)] = v;
    }
    ::closedir(d);
    return out;
}

/** @brief Switches made between two snapshots by threads other than main. */
long switches_off_main(const std::map<int, long>& a, const std::map<int, long>& b) {
    long sum = 0;
    const int self = ::getpid();
    for (const auto& [tid, v] : b) {
        if (tid == self) continue;
        auto it = a.find(tid);
        if (it != a.end()) sum += v - it->second;
    }
    return sum;
}

// --- frames ---------------------------------------------------------------------------

/** @brief An encoded VALUE TLV of @p n bytes. */
std::vector<std::byte> value_tlv(std::size_t n) {
    std::vector<std::byte> payload(n, std::byte{0x5A});
    tr::wire::tlv_t t{.type = tr::wire::type_t::VALUE, .payload = payload};
    return tr::wire::encode(t);
}

/** @brief A `FWD{WRITE}` frame to `/sensor/temp` carrying @p value. */
std::vector<std::byte> fwd_write(std::span<const std::byte> value) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, tr::wire::type_t::VALUE, tr::wire::opt_t{},
                       std::span<const std::byte>(&op, 1));
    std::vector<std::byte> dst;
    (void)tr::wire::emit_path_segment(dst, "sensor");
    (void)tr::wire::emit_path_segment(dst, "temp");
    tr::wire::emit_tlv(body, tr::wire::type_t::PATH, tr::wire::opt_t{}, dst);
    tr::wire::emit_tlv(body, tr::wire::type_t::PATH, tr::wire::opt_t{},
                       std::span<const std::byte>{});
    body.insert(body.end(), value.begin(), value.end());
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, tr::wire::type_t::FWD, tr::wire::opt_t{.pl = true}, body);
    return frame;
}

/** @brief The link kinds a relay row can run on. */
enum class kind_t { TCP, WS, UDP };

/** @brief The `--peer` argument spelling of @p k. */
const char* kind_name(kind_t k) {
    return k == kind_t::TCP ? "tcp" : k == kind_t::WS ? "ws" : "udp";
}

/** @brief @p s parsed back from @ref kind_name. */
kind_t kind_of(std::string_view s) {
    return s == "tcp" ? kind_t::TCP : s == "ws" ? kind_t::WS : kind_t::UDP;
}

/**
 * @brief A relay row: the ingress kind (A to B), the egress kind (B to C), and whether B
 *        forwards over a server link C dialed. UDP has no server side, so a UDP egress is
 *        always B's own link aimed at C.
 */
struct relay_t {
    const char* arm;
    kind_t in;
    kind_t out;
    bool srv_egress;
};

constexpr relay_t kRelays[] = {
    {"tcp-relay-cli", kind_t::TCP, kind_t::TCP, false},
    {"tcp-relay-srv", kind_t::TCP, kind_t::TCP, true},
    {"ws-relay-cli", kind_t::WS, kind_t::WS, false},
    {"ws-relay-srv", kind_t::WS, kind_t::WS, true},
    {"udp-relay-tcp-cli", kind_t::UDP, kind_t::TCP, false},
    {"udp-relay-tcp-srv", kind_t::UDP, kind_t::TCP, true},
    {"udp-relay-ws-cli", kind_t::UDP, kind_t::WS, false},
    {"udp-relay-ws-srv", kind_t::UDP, kind_t::WS, true},
    {"udp-relay-udp", kind_t::UDP, kind_t::UDP, false},
};
constexpr std::size_t kPayloads[] = {64, 1024, 4096, 16384};

/** @brief The UDP receive cap every UDP link here sets, so a 16 KiB frame is admitted. */
constexpr std::size_t kUdpMaxFrame = 32 * 1024;

/** @brief One relay measurement, per frame. */
struct relay_sample_t {
    double copy_bytes = 0, copy_calls = 0, syscalls = 0, vcsw = 0;
    bool ok = false;
};

/** @brief A counting sink: the rope receiver every measured endpoint installs. */
struct count_sink_t {
    std::atomic<std::uint64_t>* n;
    void operator()(tr::view::rope_t) const { n->fetch_add(1, std::memory_order_release); }
};

/** @brief The bound port of a server link of either stream kind, or of a UDP link. */
std::uint16_t port_of(kind_t k, transport_t& t) {
    if (k == kind_t::TCP) return static_cast<tr::net::tcp_server_transport_t&>(t).local_port();
    if (k == kind_t::WS) return static_cast<tr::net::ws_server_transport_t&>(t).local_port();
    return static_cast<tr::net::udp_transport_t&>(t).local_port();
}

/** @brief A UDP link bound to an ephemeral port, aimed at @p port (0: learns its peer). */
tr::mem::poly_ptr_t<transport_t> udp_of(std::uint16_t port) {
    return tr::mem::make_poly<tr::net::udp_transport_t>(
        tr::mem::heap_source(), std::uint16_t{0}, port != 0 ? "127.0.0.1" : "", port,
        tr::net::udp_config_t{.max_frame = kUdpMaxFrame});
}

/** @brief The listening side of kind @p k: a server link, or a UDP link that learns its peer. */
tr::mem::poly_ptr_t<transport_t> server_of(kind_t k) {
    if (k == kind_t::TCP)
        return tr::mem::make_poly<tr::net::tcp_server_transport_t>(tr::mem::heap_source(),
                                                                   std::uint16_t{0});
    if (k == kind_t::WS)
        return tr::mem::make_poly<tr::net::ws_server_transport_t>(tr::mem::heap_source(),
                                                                  std::uint16_t{0});
    return udp_of(0);
}

/** @brief The dialing side of kind @p k, aimed at 127.0.0.1:@p port. */
tr::mem::poly_ptr_t<transport_t> client_of(kind_t k, std::uint16_t port) {
    if (k == kind_t::TCP)
        return tr::mem::make_poly<tr::net::tcp_transport_t>(tr::mem::heap_source(), "127.0.0.1",
                                                            port);
    if (k == kind_t::WS)
        return tr::mem::make_poly<tr::net::ws_client_transport_t>(tr::mem::heap_source(),
                                                                  "127.0.0.1", port);
    return udp_of(port);
}

/**
 * @brief The peer process: writer A and node C. Driven line by line over two pipes.
 *
 * Protocol: peer prints `C <port>` (0 when C dials B instead), reads `B <in> <c_srv>`,
 * dials, prints `R`, then for every `f` it reads sends one frame and answers `a` once
 * C has received it; `q`, or the pipe closing, ends it.
 */
int peer_main(kind_t in, kind_t out, bool srv_egress, std::size_t payload, int in_fd, int out_fd) {
    std::atomic<std::uint64_t> c_rx{0};
    count_sink_t sink{&c_rx};
    tr::mem::poly_ptr_t<transport_t> c_server;
    if (!srv_egress) {
        c_server = server_of(out);
        c_server->set_rope_receiver(sink);
    }
    pipe_put(out_fd, "C " + std::to_string(c_server ? port_of(out, *c_server) : 0) + "\n");
    unsigned in_port = 0, c_port = 0;
    if (std::sscanf(pipe_line(in_fd).c_str(), "B %u %u", &in_port, &c_port) != 2) return 3;
    tr::mem::poly_ptr_t<transport_t> c_client;
    if (srv_egress) {
        c_client = client_of(out, static_cast<std::uint16_t>(c_port));
        c_client->set_rope_receiver(sink);
        c_client->start_receiving();
    }
    tr::mem::poly_ptr_t<transport_t> a = client_of(in, static_cast<std::uint16_t>(in_port));
    a->start_receiving();
    std::this_thread::sleep_for(300ms);
    pipe_put(out_fd, "R\n");
    const std::vector<std::byte> frame = fwd_write(value_tlv(payload));
    for (;;) {
        const char c = pipe_byte(in_fd);
        if (c == 'q') break;
        const std::uint64_t want = c_rx.load(std::memory_order_acquire) + 1;
        const auto until = std::chrono::steady_clock::now() + 5s;
        (void)a->send(frame);
        while (c_rx.load(std::memory_order_acquire) < want &&
               std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        pipe_put(out_fd, c_rx.load(std::memory_order_acquire) >= want ? "a" : "x");
    }
    std::_Exit(0);  // skip teardown: the parent has its numbers
}

/**
 * @brief The peer process of one relay row and its two pipe ends, owned: on every exit path
 *        both ends close and the child is reaped.
 *
 * Closing the write end is what stops the child: its next pipe read sees end-of-file and it
 * exits, so the reap that follows cannot hang on a child still waiting for a command.
 */
struct peer_proc_t {
    pid_t pid = -1;
    int to_peer = -1;
    int from_peer = -1;

    peer_proc_t() = default;
    peer_proc_t(const peer_proc_t&) = delete;
    peer_proc_t& operator=(const peer_proc_t&) = delete;
    ~peer_proc_t() {
        if (to_peer >= 0) ::close(to_peer);
        if (from_peer >= 0) ::close(from_peer);
        int status = 0;
        if (pid > 0) (void)::waitpid(pid, &status, 0);
    }
};

/** @brief Spawn this binary as the peer process for one relay row into @p p. */
bool spawn_peer(const char* self, const relay_t& r, std::size_t payload, peer_proc_t& p) {
    int down[2] = {-1, -1}, up[2] = {-1, -1};
    if (::pipe2(down, O_CLOEXEC) != 0) return false;
    p.to_peer = down[1];
    if (::pipe2(up, O_CLOEXEC) != 0) {
        ::close(down[0]);
        return false;
    }
    p.from_peer = up[0];
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, down[0], 3);
    posix_spawn_file_actions_adddup2(&fa, up[1], 4);
    std::string in = kind_name(r.in);
    std::string out = kind_name(r.out);
    std::string egress = r.srv_egress ? "srv" : "cli";
    std::string size = std::to_string(payload);
    char* argv[] = {const_cast<char*>(self),
                    const_cast<char*>("--peer"),
                    in.data(),
                    out.data(),
                    egress.data(),
                    size.data(),
                    nullptr};
    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, self, &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(down[0]);
    ::close(up[1]);
    if (rc != 0) return false;
    p.pid = pid;
    return true;
}

/** @brief Wait until @p n reaches @p want, or give up after @p limit. */
bool await_count(const std::atomic<std::uint64_t>& n, std::uint64_t want,
                 std::chrono::milliseconds limit) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (n.load(std::memory_order_acquire) < want) {
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::yield();
    }
    return true;
}

/** @brief Measure one relay row in this process (node B). */
relay_sample_t run_relay(const char* self, const relay_t& r, std::size_t payload,
                         std::size_t frames) {
    relay_sample_t out;
    // Declared before node B, so B's links are gone before the child is reaped.
    peer_proc_t peer;
    if (!spawn_peer(self, r, payload, peer)) return out;
    unsigned c_port = 0;
    if (std::sscanf(pipe_line(peer.from_peer).c_str(), "C %u", &c_port) != 1) return out;
    graph_t gb;
    tr::net::fwd_router_t rb(gb);
    tr::mem::poly_ptr_t<transport_t> to_c =
        r.srv_egress ? server_of(r.out) : client_of(r.out, static_cast<std::uint16_t>(c_port));
    const std::string mount = std::string("net/") + kind_name(r.out) + "/c";
    tr::mem::poly_ptr_t<transport_t> in = server_of(r.in);
    if (!rb.attach_link(mount, *to_c) || !rb.attach_link("in", *in)) return out;
    to_c->start_receiving();  // a DIAL link receives once armed (#2247); a no-op on a server
    (void)gb.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    std::atomic<std::uint64_t> local{0};
    auto on_temp = [&](const tr::graph::value_t&) {
        local.fetch_add(1, std::memory_order_release);
    };
    auto sub = gb.subscribe(path_t("/sensor/temp"), on_temp);
    if (!sub) return out;
    auto bound =
        rb.subscribe_toward(path_t("/sensor/temp"), path_t(("/" + mount + "/sink/temp").c_str()));
    if (!bound) return out;
    pipe_put(peer.to_peer, "B " + std::to_string(port_of(r.in, *in)) + " " +
                               std::to_string(r.srv_egress ? port_of(r.out, *to_c) : 0) + "\n");
    if (pipe_line(peer.from_peer) != "R") return out;

    bool ok = true;
    auto drive = [&](std::size_t count) {
        for (std::size_t i = 0; i < count && ok; ++i) {
            const std::uint64_t want = local.load(std::memory_order_acquire) + 1;
            pipe_put(peer.to_peer, "f");
            ok = pipe_byte(peer.from_peer) == 'a' && await_count(local, want, 5000ms);
        }
    };
    drive(16);  // warm-up: lazy draws, slot creation, buffer growth
    std::this_thread::sleep_for(20ms);
    g_copy_bytes = 0;
    g_copy_calls = 0;
    g_syscalls = 0;
    const auto s0 = voluntary_switches();
    g_armed.store(true);
    drive(frames);
    g_armed.store(false);
    const auto s1 = voluntary_switches();
    pipe_put(peer.to_peer, "q");
    const double f = static_cast<double>(frames);
    out.copy_bytes = static_cast<double>(g_copy_bytes.load()) / f;
    out.copy_calls = static_cast<double>(g_copy_calls.load()) / f;
    out.syscalls = static_cast<double>(g_syscalls.load()) / f;
    out.vcsw = static_cast<double>(switches_off_main(s0, s1)) / f;
    out.ok = ok;
    return out;
}

/** @brief Idle wakeups per second of every non-main thread, @p peers peers, no traffic. */
double run_idle(std::string_view kind, std::size_t peers) {
    std::vector<tr::mem::poly_ptr_t<transport_t>> links;
    std::atomic<std::uint64_t> rx{0};
    count_sink_t sink{&rx};
    if (kind == "udp") {
        for (std::size_t i = 0; i < peers; ++i) {
            auto a = tr::mem::make_poly<tr::net::udp_transport_t>(
                tr::mem::heap_source(), std::uint16_t{0}, "127.0.0.1", std::uint16_t{9});
            a->set_rope_receiver(sink);
            links.push_back(std::move(a));
        }
    } else {
        const kind_t k = kind_of(kind);
        auto s = server_of(k);
        s->set_rope_receiver(sink);
        const std::uint16_t port = port_of(k, *s);
        links.push_back(std::move(s));
        for (std::size_t i = 0; i < peers; ++i) {
            links.push_back(client_of(k, port));
            links.back()->start_receiving();
        }
    }
    std::this_thread::sleep_for(500ms);
    const auto s0 = voluntary_switches();
    const auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(1s);
    const auto s1 = voluntary_switches();
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return static_cast<double>(switches_off_main(s0, s1)) / secs;
}

/** @brief Print one RESULT row: the median over repetitions, with min and max. */
void report(const char* arm, const std::string& metric, std::vector<double> v) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    std::printf("RESULT arm=%s metric=%s n=%zu median=%.2f min=%.2f max=%.2f\n", arm,
                metric.c_str(), v.size(), v[v.size() / 2], v.front(), v.back());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 6 && std::string_view(argv[1]) == "--peer")
        return peer_main(kind_of(argv[2]), kind_of(argv[3]), std::string_view(argv[4]) == "srv",
                         std::strtoul(argv[5], nullptr, 10), 3, 4);
    t_harness = true;  // this thread drives the pipe; B's own threads are the link threads
    std::size_t reps = 3, frames = 200;
    bool idle = true;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a.rfind("--reps=", 0) == 0)
            reps = std::strtoul(argv[i] + 7, nullptr, 10);
        else if (a.rfind("--frames=", 0) == 0)
            frames = std::strtoul(argv[i] + 9, nullptr, 10);
        else if (a == "--no-idle")
            idle = false;
        else {
            std::fprintf(stderr, "usage: %s [--reps=N] [--frames=N] [--no-idle]\n", argv[0]);
            return 2;
        }
    }
    // The binary re-executes itself as the peer process, so it needs its own path.
    char self[4096];
    const ssize_t n = ::readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) return 2;
    self[n] = '\0';

    struct row_t {
        std::vector<double> copy_bytes, copy_calls, syscalls, vcsw;
    };
    std::map<std::pair<std::size_t, std::size_t>, row_t> rows;
    int failed = 0;
    for (std::size_t rep = 0; rep < reps; ++rep) {
        for (std::size_t ri = 0; ri < std::size(kRelays); ++ri) {
            for (std::size_t p : kPayloads) {
                const relay_sample_t s = run_relay(self, kRelays[ri], p, frames);
                if (!s.ok) {
                    std::fprintf(stderr, "warn: %s @%zu did not complete\n", kRelays[ri].arm, p);
                    ++failed;
                    continue;
                }
                row_t& r = rows[{ri, p}];
                r.copy_bytes.push_back(s.copy_bytes);
                r.copy_calls.push_back(s.copy_calls);
                r.syscalls.push_back(s.syscalls);
                r.vcsw.push_back(s.vcsw);
            }
        }
    }
    for (auto& [key, r] : rows) {
        const char* arm = kRelays[key.first].arm;
        const std::string at = "@" + std::to_string(key.second);
        report(arm, "copy_bytes" + at, r.copy_bytes);
        report(arm, "copy_calls" + at, r.copy_calls);
        report(arm, "syscalls" + at, r.syscalls);
        report(arm, "vcsw" + at, r.vcsw);
    }
    if (idle) {
        for (const char* kind : {"tcp", "ws", "udp"}) {
            std::vector<double> v;
            for (std::size_t rep = 0; rep < reps; ++rep) v.push_back(run_idle(kind, 32));
            const std::string arm = std::string(kind) + "-idle-32";
            report(arm.c_str(), "wakeups_per_s", v);
        }
    }
    std::fflush(stdout);
    return failed == 0 ? 0 : 1;
}
