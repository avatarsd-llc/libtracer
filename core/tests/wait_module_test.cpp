/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
/**
 * @file
 * @brief #2147: the wait module's conformance suite, and the links on it.
 *
 * Built twice: `wait_module_test` runs it against the build's adapter (epoll on a Linux host),
 * `wait_module_poll_test` against the poll adapter (`wait_poll.cpp`, the lwIP sockets one),
 * linked ahead of the library so the archive's adapter is never pulled in. Both must pass the
 * same assertions (ADR-0071's two-implementations rule).
 *
 * The idle checks count the driving thread's voluntary context switches from
 * `/proc/self/task/<tid>/status`: a wait with no timeout makes none while nothing is ready.
 * Each starts counting only once the thread is asleep (@ref asleep), not after a fixed pause:
 * the wake an attach or a send rings is a real one, and on a loaded host it can still be
 * pending when a pause ends (#2776).
 */

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/wait.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;
using namespace std::chrono_literals;

/** @brief Voluntary context switches of thread @p tid so far. */
long voluntary_switches(pid_t tid) {
    std::ifstream in("/proc/self/task/" + std::to_string(tid) + "/status");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("voluntary_ctxt_switches:", 0) == 0) return std::stol(line.substr(24));
    return -1;
}

/** @brief Thread @p tid's scheduler state letter (`R`, `S`, ...), or 0. */
char sched_state(pid_t tid) {
    std::ifstream in("/proc/self/task/" + std::to_string(tid) + "/stat");
    std::string stat;
    std::getline(in, stat);
    const std::size_t comm_end = stat.rfind(')');  // the name may hold spaces or ')'
    return comm_end != std::string::npos && comm_end + 2 < stat.size() ? stat[comm_end + 2] : '\0';
}

/** @brief True iff @p nr is a readiness wait: what a wait module or a link thread blocks in. */
bool is_wait_syscall(long nr) {
    const long waits[] = {
#ifdef SYS_poll
        SYS_poll,
#endif
#ifdef SYS_select
        SYS_select,
#endif
#ifdef SYS_epoll_wait
        SYS_epoll_wait,
#endif
#ifdef SYS_epoll_pwait2
        SYS_epoll_pwait2,
#endif
        SYS_ppoll,        SYS_pselect6, SYS_epoll_pwait};
    return std::find(std::begin(waits), std::end(waits), nr) != std::end(waits);
}

/**
 * @brief True iff thread @p tid is asleep in its readiness wait: state `S` before and after
 *        reading `/proc/self/task/<tid>/syscall`, which names a wait.
 *
 * Asleep there after the test's last attach, arm, send or detach has returned means the
 * thread has taken every wake those rang: each ring is queued before its call returns, and a
 * wait with a readable doorbell or socket returns at once instead of sleeping. So the next
 * voluntary switch is a wake from nothing. State `S` alone is not enough: a thread asleep on
 * a lock the test's own call holds is `S` too, and wakes into its wait right after (#2776).
 */
bool asleep(pid_t tid) {
    if (sched_state(tid) != 'S') return false;
    std::ifstream in("/proc/self/task/" + std::to_string(tid) + "/syscall");
    long nr = -1;
    if (!(in >> nr) || !is_wait_syscall(nr)) return false;
    return sched_state(tid) == 'S';
}

/** @brief The ids of the process's threads. */
std::vector<pid_t> thread_ids() {
    std::vector<pid_t> ids;
    for (const auto& e : std::filesystem::directory_iterator("/proc/self/task"))
        ids.push_back(static_cast<pid_t>(std::stol(e.path().filename().string())));
    return ids;
}

/**
 * @brief The threads that exist now and are not in @p old: the ones spawned since.
 *
 * A count of threads cannot stand in for this. A thread another test joined a moment ago
 * can still be counted (its task outlives the join by a few microseconds; ~1% of joins on
 * this host), so a count taken then is one too high and a later count looks like a thread
 * vanished. Comparing ids sees only what was spawned, whatever a runtime or an exiting
 * thread does to the count.
 */
std::vector<pid_t> spawned_since(const std::vector<pid_t>& old) {
    std::vector<pid_t> fresh;
    for (const pid_t id : thread_ids())
        if (std::find(old.begin(), old.end(), id) == old.end()) fresh.push_back(id);
    return fresh;
}

/** @brief Poll @p done for up to 2 s. */
template <class Pred>
bool eventually(Pred done) {
    for (int i = 0; i < 200; ++i) {
        if (done()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return done();
}

/** @brief A module driven by a thread of the test's own, which records its tid. */
struct driven_t {
    tr::net::wait_module_t module;
    std::atomic<pid_t> tid{0};
    std::thread driver;
    driven_t() {
        driver = std::thread([this] {
            tid.store(static_cast<pid_t>(::syscall(SYS_gettid)));
            module.run();
        });
        (void)eventually([this] { return tid.load() != 0; });
    }
    ~driven_t() {
        module.stop();
        driver.join();
    }
};

/** @brief One socketpair end on a source that counts its turns. */
struct counted_t {
    tr::net::wait_source_t src;
    int pair[2] = {-1, -1};
    std::atomic<int> reads{0};
    std::atomic<int> writes{0};
    counted_t() {
        (void)::socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
        src.fd.store(pair[0]);
        src.ctx = this;
        src.fire = [](void* ctx, tr::net::wait_source_t&, std::uint8_t ready) {
            auto& c = *static_cast<counted_t*>(ctx);
            if (tr::net::readable(ready)) {
                char b[64];
                (void)::recv(c.pair[0], b, sizeof(b), MSG_DONTWAIT);
                c.reads.fetch_add(1);
            }
            if (tr::net::writable(ready)) c.writes.fetch_add(1);
        };
    }
    ~counted_t() {
        ::close(pair[0]);
        ::close(pair[1]);
    }
};

void test_ready_and_stop() {
    std::printf("-- a ready source fires on the driver; stop wakes a blocked run\n");
    auto d = std::make_unique<driven_t>();
    check(d->module.ok(), "the module opens");
    counted_t c;
    check(d->module.attach(c.src), "a source attaches");
    (void)::send(c.pair[1], "x", 1, 0);
    check(eventually([&] { return c.reads.load() == 1; }), "readable fires once per arrival");
    d->module.detach(c.src);
    (void)::send(c.pair[1], "x", 1, 0);
    std::this_thread::sleep_for(50ms);
    check(c.reads.load() == 1, "a detached source never fires again");
    const auto t0 = std::chrono::steady_clock::now();
    d.reset();  // stop + join
    check(std::chrono::steady_clock::now() - t0 < 1s, "stop wakes the blocked wait at once");
}

void test_write_interest() {
    std::printf("-- write interest is armed and disarmed\n");
    driven_t d;
    counted_t c;
    check(d.module.attach(c.src), "attached");
    std::this_thread::sleep_for(30ms);
    check(c.writes.load() == 0, "no write turn while it is not armed");
    d.module.want_write(c.src, true);
    check(eventually([&] { return c.writes.load() > 0; }), "an armed, writable socket fires");
    d.module.want_write(c.src, false);
    // Asleep, not after a pause: an armed writable socket never lets the driver sleep, so a
    // driver asleep has taken the disarm.
    check(eventually([&] { return asleep(d.tid.load()); }), "the driver is asleep");
    const int seen = c.writes.load();
    std::this_thread::sleep_for(50ms);
    check(c.writes.load() == seen, "disarmed, it stops");
    d.module.detach(c.src);
}

/**
 * @brief A crossing arm and disarm never strand a held frame: write interest ends registered
 *        as the flag last says.
 *
 * The link's shape, hammered: a sender arms after holding a frame while the driver disarms in
 * its write turn, and an arm lands while the source is being attached (a send held between a
 * link's start and its attach). After each arm the driver must take a write turn (here, one
 * that disarms), or the frame would sit held with the socket writable and no turn to come.
 */
void test_write_interest_converges() {
    std::printf("-- a crossing arm and disarm leave write interest as last armed\n");
    driven_t d;
    struct disarming_t {
        tr::net::wait_module_t* module;
        counted_t c;
    } x{&d.module, {}};
    x.c.src.ctx = &x;
    x.c.src.fire = [](void* ctx, tr::net::wait_source_t& self, std::uint8_t ready) {
        auto& y = *static_cast<disarming_t*>(ctx);
        if (!tr::net::writable(ready)) return;
        y.c.writes.fetch_add(1);
        y.module->want_write(self, false);  // the driver's disarm once the frame is out
    };
    check(d.module.attach(x.c.src), "attached");
    // The sender spins on the flag and arms the instant the driver's disarm clears it, so
    // the arm lands between the driver's flip and its registration. A stranded arm leaves
    // the flag set with no write turn to come: the spin's bound catches it.
    const auto disarmed = [&] {
        const auto until = std::chrono::steady_clock::now() + 2s;
        while (x.c.src.write.load())
            if (std::chrono::steady_clock::now() > until) return false;
        return true;
    };
    bool converged = true;
    for (int i = 0; i < 20000 && converged; ++i) {
        d.module.want_write(x.c.src, true);  // the sender's arm, crossing the last disarm
        converged = disarmed();
    }
    check(converged, "every arm crossing a disarm got its write turn");
    for (int i = 0; i < 2000 && converged; ++i) {
        d.module.detach(x.c.src);
        std::thread arm([&] { d.module.want_write(x.c.src, true); });
        converged = d.module.attach(x.c.src);
        arm.join();
        converged = converged && disarmed();
    }
    check(converged, "every arm crossing an attach got its write turn");
    d.module.detach(x.c.src);
}

void test_detach_waits_out_a_callback() {
    std::printf("-- a foreign detach waits out the callback in flight\n");
    driven_t d;
    struct slow_t {
        tr::net::wait_source_t src;
        int pair[2] = {-1, -1};
        std::atomic<bool> inside{false};
        std::atomic<bool> done{false};
    } s;
    (void)::socketpair(AF_UNIX, SOCK_STREAM, 0, s.pair);
    s.src.fd.store(s.pair[0]);
    s.src.ctx = &s;
    s.src.fire = [](void* ctx, tr::net::wait_source_t&, std::uint8_t) {
        auto& me = *static_cast<slow_t*>(ctx);
        char b[8];
        (void)::recv(me.pair[0], b, sizeof(b), MSG_DONTWAIT);
        me.inside.store(true);
        std::this_thread::sleep_for(100ms);
        me.done.store(true);
    };
    check(d.module.attach(s.src), "attached");
    (void)::send(s.pair[1], "x", 1, 0);
    check(eventually([&] { return s.inside.load(); }), "the callback is running");
    d.module.detach(s.src);
    check(s.done.load(), "detach returned only after the callback finished");
    ::close(s.pair[0]);
    ::close(s.pair[1]);
}

void test_detach_waits_for_its_own_source_only() {
    std::printf("-- a foreign detach does not wait for another source's callback\n");
    // Source A's callback blocks on a lock the detaching thread holds while it detaches source
    // B. Waiting for the whole batch would deadlock (ruling 44); waiting for B alone returns.
    driven_t d;
    counted_t b;
    struct blocked_t {
        tr::net::wait_source_t src;
        int pair[2] = {-1, -1};
        std::mutex* held = nullptr;
        std::atomic<bool> inside{false};
        std::atomic<bool> give_up{false};
    } a;
    std::mutex held;
    (void)::socketpair(AF_UNIX, SOCK_STREAM, 0, a.pair);
    a.src.fd.store(a.pair[0]);
    a.src.ctx = &a;
    a.held = &held;
    a.src.fire = [](void* ctx, tr::net::wait_source_t&, std::uint8_t) {
        auto& me = *static_cast<blocked_t*>(ctx);
        char buf[8];
        (void)::recv(me.pair[0], buf, sizeof(buf), MSG_DONTWAIT);
        me.inside.store(true);
        // A bounded wait, so a regression fails the check below instead of hanging the test.
        while (!me.held->try_lock()) {
            if (me.give_up.load()) return;
            std::this_thread::yield();
        }
        me.held->unlock();
    };
    check(d.module.attach(a.src) && d.module.attach(b.src), "attached");
    std::atomic<bool> detached{false};
    std::thread detacher([&] {
        const std::lock_guard lock(held);
        (void)::send(a.pair[1], "x", 1, 0);
        (void)eventually([&] { return a.inside.load(); });
        d.module.detach(b.src);
        detached.store(true);
        (void)eventually([&] { return a.give_up.load(); });
    });
    check(eventually([&] { return detached.load(); }),
          "detaching B returned while A's callback waits on the detacher's lock");
    a.give_up.store(true);
    detacher.join();
    d.module.detach(a.src);
    ::close(a.pair[0]);
    ::close(a.pair[1]);
}

void test_detach_inside_callback() {
    std::printf("-- a source may detach itself and its neighbour from its callback\n");
    driven_t d;
    counted_t a;
    counted_t b;
    struct self_t {
        tr::net::wait_source_t src;
        int pair[2] = {-1, -1};
        tr::net::wait_module_t* module = nullptr;
        counted_t* other = nullptr;
        std::atomic<int> fired{0};
    } s;
    (void)::socketpair(AF_UNIX, SOCK_STREAM, 0, s.pair);
    s.src.fd.store(s.pair[0]);
    s.src.ctx = &s;
    s.module = &d.module;
    s.other = &b;
    s.src.fire = [](void* ctx, tr::net::wait_source_t& self, std::uint8_t) {
        auto& me = *static_cast<self_t*>(ctx);
        me.fired.fetch_add(1);
        me.module->detach(self);
        me.module->detach(me.other->src);
    };
    check(d.module.attach(a.src) && d.module.attach(b.src) && d.module.attach(s.src), "attached");
    (void)::send(s.pair[1], "x", 1, 0);
    (void)::send(b.pair[1], "x", 1, 0);
    (void)::send(a.pair[1], "x", 1, 0);
    check(eventually([&] { return a.reads.load() == 1; }), "the survivor is still served");
    std::this_thread::sleep_for(50ms);
    check(s.fired.load() == 1, "the self-detached source fired once and never again");
    (void)::send(b.pair[1], "y", 1, 0);
    std::this_thread::sleep_for(50ms);
    check(b.reads.load() <= 1, "the neighbour it detached is not served after the detach");
    d.module.detach(a.src);
    ::close(s.pair[0]);
    ::close(s.pair[1]);
}

void test_idle_sources_wake_nobody() {
    std::printf("-- 32 idle sources: the driver wakes 0 times\n");
    driven_t d;
    std::vector<std::unique_ptr<counted_t>> idle;
    for (int i = 0; i < 32; ++i) {
        idle.push_back(std::make_unique<counted_t>());
        check(d.module.attach(idle.back()->src), "attached");
    }
    check(eventually([&] { return asleep(d.tid.load()); }), "the driver is asleep");
    const long before = voluntary_switches(d.tid.load());
    std::this_thread::sleep_for(500ms);
    const long after = voluntary_switches(d.tid.load());
    std::printf("   driver voluntary switches over 500 ms idle: %ld\n", after - before);
    check(before >= 0 && after == before, "no idle wakeup");
    for (auto& c : idle) d.module.detach(c->src);
}

void test_links_share_one_module() {
    std::printf("-- 32 TCP dialers and 32 UDP peers on one module: no thread, no idle wakeup\n");
    driven_t d;
    std::atomic<int> delivered{0};
    auto rx = [&](std::span<const std::byte>) { delivered.fetch_add(1); };
    const std::vector<pid_t> old = thread_ids();
    tr::net::tcp_server_transport_t server(0, {.wait = &d.module});
    check(server.ok(), "the server listens on the module");
    server.set_receiver(rx);
    std::vector<std::unique_ptr<tr::net::tcp_transport_t>> dialers;
    std::vector<std::unique_ptr<tr::net::udp_transport_t>> udps;
    for (int i = 0; i < 32; ++i) {
        dialers.push_back(std::make_unique<tr::net::tcp_transport_t>(
            "127.0.0.1", server.local_port(), tr::net::tcp_config_t{.wait = &d.module}));
        dialers.back()->set_receiver(rx);
        check(dialers.back()->ok() && dialers.back()->start_receiving(), "a dialer is up");
        udps.push_back(std::make_unique<tr::net::udp_transport_t>(
            0, "127.0.0.1", 9, tr::net::udp_config_t{.wait = &d.module}));
        check(udps.back()->ok(), "a UDP link is up");
    }
    check(spawned_since(old).empty(), "65 links spawned no thread");
    const std::array<std::byte, 4> frame{};
    for (auto& t : dialers) t->send(std::span<const std::byte>(frame));
    check(eventually([&] { return delivered.load() == 32; }), "every dialer's frame is delivered");
    server.send(std::span<const std::byte>(frame));
    check(eventually([&] { return delivered.load() == 64; }), "the broadcast reaches every dialer");
    check(eventually([&] { return asleep(d.tid.load()); }), "the driver is asleep");
    const long before = voluntary_switches(d.tid.load());
    std::this_thread::sleep_for(500ms);
    const long after = voluntary_switches(d.tid.load());
    std::printf("   driver voluntary switches over 500 ms idle: %ld\n", after - before);
    check(before >= 0 && after == before, "no idle wakeup at 32 dialers and 32 UDP peers");
    // A peer that departs is seen on the module: the dialer's link goes down.
    server.shut_down();
    check(eventually([&] { return !dialers[0]->link_up(); }),
          "a departed peer brings its dialer down");
    for (auto& t : dialers) t->shut_down();
    for (auto& u : udps) u->shut_down();
}

void test_fallback_thread_is_idle() {
    std::printf("-- the thread-per-link fallback: quiet links' threads wake 0 times\n");
    // Only the threads the links add are counted, so a runtime's helper thread (a
    // sanitizer's, say) is not taken for a link waking.
    const std::vector<pid_t> old = thread_ids();
    tr::net::tcp_server_transport_t server(0, {});
    tr::net::tcp_transport_t dialer("127.0.0.1", server.local_port(), {});
    tr::net::udp_transport_t udp(0, "127.0.0.1", 9, {});
    check(server.ok() && dialer.ok() && dialer.start_receiving() && udp.ok(),
          "the fallback links are up, each on a thread of its own");
    const std::vector<pid_t> links = spawned_since(old);
    check(links.size() == 3, "three link threads, one per fallback link");
    // A frame across the TCP pair, so the server has accepted the dialer: the listener turns
    // readable whenever the loopback stack delivers the handshake's last ACK, which on a
    // loaded host can be after the server's thread first went to sleep (#2776).
    std::atomic<int> delivered{0};
    auto rx = [&](std::span<const std::byte>) { delivered.fetch_add(1); };
    server.set_receiver(rx);
    const std::array<std::byte, 4> frame{};
    dialer.send(std::span<const std::byte>(frame));
    check(eventually([&] { return delivered.load() == 1; }), "the server accepted the dialer");
    const auto switches = [&links] {
        long sum = 0;
        for (const pid_t id : links) sum += voluntary_switches(id);
        return sum;
    };
    check(eventually([&] {
              return std::all_of(links.begin(), links.end(), [](pid_t id) { return asleep(id); });
          }),
          "every link thread is asleep");
    const long before = switches();
    std::this_thread::sleep_for(500ms);
    const long after = switches();
    std::printf("   link threads' voluntary switches over 500 ms idle: %ld\n", after - before);
    check(after == before, "no idle wakeup on any link thread (the 100 ms quanta are gone)");
}

}  // namespace

int main() {
    std::printf("== #2147: one wait module per node ==\n");
    test_ready_and_stop();
    test_write_interest();
    test_write_interest_converges();
    test_detach_waits_out_a_callback();
    test_detach_waits_for_its_own_source_only();
    test_detach_inside_callback();
    test_idle_sources_wake_nobody();
    test_links_share_one_module();
    test_fallback_thread_is_idle();
    return tr::testing::summary("wait_module_test");
}
