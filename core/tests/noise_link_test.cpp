/**
 * @file
 * @brief The Noise link (RFC-0033, reference/23, #2073): one request and its reply over a Noise
 *        session, on an in-memory carrier where every datagram is counted, and over real UDP
 *        between two graph nodes.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * What a case here has to prove:
 *
 *   - the request and the reply ARE the confirmation and the acknowledgement: four datagrams,
 *     two round trips, and the initiator reports `UP` only after the reply;
 *   - the link reproduces RFC-0033 Appendix A's datagrams byte for byte from its inputs;
 *   - a wrong PSK is refused and counted, the responder sends nothing back, and reading the
 *     refused first message allocates nothing;
 *   - retransmission, the attempt deadline and the pending expiry run only on the
 *     application's `now`;
 *   - the session state is one block from the link's own `memory.state`;
 *   - the `noise_*` nouns are readable as `:stats.link.<child>`.
 *
 * The allocation rows count every `malloc` family call in the process (glibc only, and not
 * under a sanitizer, which owns `malloc` itself); elsewhere they are skipped, not passed.
 */

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_noise.hpp"
#include "libtracer/transport_udp.hpp"
#include "test_support.hpp"
#include "test_values.hpp"
#include "tlv_tree.hpp"  // host-only owning tree (#1829)

/** @name Process-wide allocation counter (glibc, no sanitizer) */
/** @{ */
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
#define LT_NOISE_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define LT_NOISE_SANITIZED 1
#endif

#if defined(__GLIBC__) && !defined(LT_NOISE_SANITIZED)
/** @brief Whether this build counts allocations (glibc, no sanitizer). */
inline constexpr bool kCountsAllocations = true;
/** @brief Allocation calls since process start. Atomic: the UDP cases run receive threads. */
static std::atomic<std::size_t> g_allocations{0};

extern "C" {
void* __libc_malloc(std::size_t);
void* __libc_calloc(std::size_t, std::size_t);
void* __libc_realloc(void*, std::size_t);
void* __libc_memalign(std::size_t, std::size_t);
void __libc_free(void*);

/** @brief Counting `malloc`. */
void* malloc(std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    return __libc_malloc(n);
}
/** @brief Counting `calloc`. */
void* calloc(std::size_t c, std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    return __libc_calloc(c, n);
}
/** @brief Counting `realloc`. */
void* realloc(void* p, std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    return __libc_realloc(p, n);
}
/** @brief Counting `aligned_alloc`. */
void* aligned_alloc(std::size_t a, std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    return __libc_memalign(a, n);
}
/** @brief Counting `posix_memalign`. */
int posix_memalign(void** out, std::size_t a, std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    *out = __libc_memalign(a, n);
    return *out != nullptr ? 0 : 12;  // ENOMEM
}
/** @brief Plain `free`. */
void free(void* p) { __libc_free(p); }
}
#else
/** @brief Whether this build counts allocations (glibc, no sanitizer). */
inline constexpr bool kCountsAllocations = false;
/** @brief Allocation calls since process start (not counted on this build). */
static std::atomic<std::size_t> g_allocations{0};
#endif
/** @} */

namespace {

namespace noise = tr::net::noise;
using noise::counter_t;
using noise::key32_t;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::conn_role_t;
using tr::net::link_state_t;
using tr::net::noise_config_t;
using tr::testing::check;

/** @brief Parse a hex literal (spaces ignored) into bytes; test inputs only. */
std::vector<std::byte> unhex(std::string_view s) {
    std::vector<std::byte> out;
    auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    int hi = -1;
    for (const char c : s) {
        if (c == ' ') continue;
        if (hi < 0) {
            hi = nib(c);
            continue;
        }
        out.push_back(static_cast<std::byte>((hi << 4) | nib(c)));
        hi = -1;
    }
    return out;
}

/** @brief A 32-byte key from hex. */
key32_t key(std::string_view s) {
    key32_t k{};
    const auto b = unhex(s);
    std::memcpy(k.data(), b.data(), k.size());
    return k;
}

/** @brief The Appendix A PSK, `00 01 … 1f`. */
const key32_t kPsk = key("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");

/** @brief A VALUE TLV over @p payload: one complete Tracer frame. */
std::vector<std::byte> value_frame(std::string_view payload) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, tr::wire::type_t::VALUE, tr::wire::opt_t{},
                       std::as_bytes(std::span(payload)));
    return out;
}

/**
 * @brief The application's seams for a test: one PSK under one name, a counter, and either
 *        a list of fixed ephemeral keys (Appendix A) or a deterministic test generator.
 *
 * The generator is NOT a random source; an application injects a cryptographic one.
 */
struct test_host_t {
    key32_t psk = kPsk;                           /**< @brief The one key in the store. */
    std::string_view name = "link-psk";           /**< @brief Its key-store name. */
    std::vector<key32_t> fixed{};                 /**< @brief Ephemeral keys to hand out first. */
    std::size_t used = 0;                         /**< @brief How many of @ref fixed are used. */
    std::uint64_t state = 0x9E3779B97F4A7C15;     /**< @brief The test generator's state. */
    std::uint64_t counter = 0;                    /**< @brief The last counter handed out. */
    std::optional<std::uint64_t> counter_fixed{}; /**< @brief A counter to hand out instead. */

    /** @brief The key store. */
    bool resolve_psk(std::string_view ref, key32_t& out) {
        if (ref != name) return false;
        out = psk;
        return true;
    }
    /** @brief The ephemeral source. */
    bool ephemeral(key32_t& out) {
        if (used < fixed.size()) {
            out = fixed[used++];
            return true;
        }
        for (std::size_t i = 0; i < out.size(); i += 8) {
            state += 0x9E3779B97F4A7C15;
            std::uint64_t z = state;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EB;
            z ^= z >> 31;
            std::memcpy(out.data() + i, &z, 8);
        }
        return true;
    }
    /** @brief The initiator counter. */
    std::uint64_t next_counter() { return counter_fixed ? *counter_fixed : ++counter; }
};

/** @brief The addresses of the in-memory wire's two ends. */
constexpr std::uint64_t kAddrA = 0xA;
/** @brief See @ref kAddrA. */
constexpr std::uint64_t kAddrB = 0xB;
/** @brief An address that is neither end, for messages a stranger sends. */
constexpr std::uint64_t kAddrStranger = 0xC;

/** @brief One datagram on the in-memory wire. */
struct datagram_t {
    int from = 0;                 /**< @brief 0 = A, 1 = B. */
    std::uint64_t to = 0;         /**< @brief The address it was sent to. */
    std::vector<std::byte> bytes; /**< @brief Its bytes. */
};

/** @brief The in-memory wire: what each side sent, and what is queued for each side. */
struct mem_wire_t {
    std::vector<datagram_t> log;                   /**< @brief Every datagram, in order. */
    std::array<std::deque<datagram_t>, 2> inbox;   /**< @brief Queued for A (0) and B (1). */
    std::function<bool(const datagram_t&)> drop{}; /**< @brief Lose a datagram when true. */
};

/**
 * @brief An in-memory datagram carrier: a send is logged and queued for the other end, and
 *        the test pumps each queue, so every datagram is visible and the order is exact.
 */
class mem_carrier_t final : public tr::net::transport_t {
   public:
    /** @brief Side @p side (0 = A, 1 = B) of @p wire, accepting datagrams up to @p bound. */
    mem_carrier_t(mem_wire_t& wire, int side, std::size_t bound = 65535)
        : wire_(wire), side_(side), bound_(bound) {}
    /** @brief Unused by the Noise link, which always addresses its sends. */
    void send(std::span<const std::byte> frame) override {
        const std::span<const std::byte> one[1] = {frame};
        send_to(peer_address(), one);
    }
    /** @brief Log and queue one datagram. */
    void send_to(std::uint64_t to, std::span<const std::span<const std::byte>> iov) {
        datagram_t d{.from = side_, .to = to, .bytes = {}};
        for (const auto s : iov) d.bytes.insert(d.bytes.end(), s.begin(), s.end());
        wire_.log.push_back(d);
        if (wire_.drop && wire_.drop(d)) return;
        wire_.inbox[std::size_t(1 - side_)].push_back(std::move(d));
    }
    /** @brief Always up. */
    [[nodiscard]] bool ok() const noexcept { return true; }
    /** @brief The datagram bound. */
    [[nodiscard]] std::size_t effective_max_frame() const noexcept { return bound_; }
    /** @brief The in-flight datagram's source. */
    [[nodiscard]] std::uint64_t inbound_source() const noexcept { return source_; }
    /** @brief The other end. */
    [[nodiscard]] std::uint64_t peer_address() const noexcept {
        return side_ == 0 ? kAddrB : kAddrA;
    }
    /** @brief Deliver every datagram queued for this end; returns how many. */
    std::size_t pump() {
        std::size_t n = 0;
        auto& q = wire_.inbox[std::size_t(side_)];
        while (!q.empty()) {
            datagram_t d = std::move(q.front());
            q.pop_front();
            inject(d.bytes, d.from == 0 ? kAddrA : kAddrB);
            ++n;
        }
        return n;
    }
    /** @brief Deliver @p bytes as a datagram from @p source (a copy: the link decrypts in place).
     */
    void inject(std::vector<std::byte> bytes, std::uint64_t source) {
        inject_in_place(bytes, source);
    }
    /** @brief Deliver @p bytes as they are, with no copy (the link may decrypt them in place). */
    void inject_in_place(std::span<std::byte> bytes, std::uint64_t source) {
        source_ = source;
        rx_.deliver_borrowed(bytes);
    }

   private:
    mem_wire_t& wire_;
    int side_;
    std::size_t bound_;
    std::uint64_t source_ = 0;
};

/** @brief A Noise link over the in-memory wire. */
using mem_link_t = tr::net::noise_link_t<noise::default_crypto_t, test_host_t, mem_carrier_t>;
/** @brief A Noise link over UDP. */
using udp_link_t =
    tr::net::noise_link_t<noise::default_crypto_t, test_host_t, tr::net::udp_transport_t>;

/** @brief What a link's receiver saw. */
struct sink_t {
    std::vector<std::vector<std::byte>> frames;             /**< @brief Every frame delivered. */
    std::function<void(std::span<const std::byte>)> then{}; /**< @brief Run after recording. */
    /** @brief The borrowed-span receiver callable. */
    void operator()(std::span<const std::byte> f) {
        frames.emplace_back(f.begin(), f.end());
        if (then) then(f);
    }
};

/** @brief The config every in-memory case starts from: retry 10, attempt 1000, pending 500. */
noise_config_t mem_config() {
    return noise_config_t{.psk_ref = "link-psk",
                          .retry = 10,
                          .retry_cap = 40,
                          .attempt = 1000,
                          .pending_expiry = 500};
}

/** @brief Two Noise links over one in-memory wire, A dialing B. */
struct mem_pair_t {
    mem_wire_t wire;
    test_host_t host_a;
    test_host_t host_b;
    mem_link_t a;
    mem_link_t b;
    sink_t sink_a;
    sink_t sink_b;

    explicit mem_pair_t(noise_config_t ca = mem_config(), noise_config_t cb = mem_config())
        : a(conn_role_t::DIAL, host_a, ca, wire, 0), b(conn_role_t::LISTEN, host_b, cb, wire, 1) {
        a.set_receiver(sink_a);
        b.set_receiver(sink_b);
    }
    /** @brief Deliver everything queued, both ways, until the wire is quiet. */
    void settle() {
        while (b.carrier().pump() + a.carrier().pump() != 0) {
        }
    }
};

/** @brief The type byte of datagram @p i in the log. */
std::uint8_t type_of(const mem_wire_t& w, std::size_t i) {
    return i < w.log.size() && !w.log[i].bytes.empty()
               ? std::to_integer<std::uint8_t>(w.log[i].bytes[0])
               : 0;
}

/**
 * @brief The acceptance flow: a request on a dormant link fails and dials; the request sent
 *        once the link holds a session is the confirmation and the reply is the
 *        acknowledgement. Four datagrams, two round trips, `UP` only after the reply.
 */
void request_reply_two_round_trips() {
    std::printf("one request and reply in two round trips:\n");
    mem_pair_t n;
    check(n.a.ok() && n.b.ok(), "  both links came up");
    check(n.a.state() == link_state_t::DORMANT && n.b.state() == link_state_t::LISTENING,
          "  DIAL starts dormant, LISTEN listening");
    const auto request = value_frame("ping");
    const auto reply = value_frame("pang");
    n.sink_b.then = [&](std::span<const std::byte>) { n.b.send(std::span(reply)); };

    n.a.send(std::span(request));  // no session: fails at once, and is a demand
    check(n.a.drop_stats().dropped_tx == 1, "  the send on a dormant link is dropped, counted");
    check(n.wire.log.size() == 1 && type_of(n.wire, 0) == 0x01 &&
              n.wire.log[0].bytes.size() == 58 && n.wire.log[0].to == kAddrB,
          "  ... and starts a handshake: a 58-byte first message to the peer");
    check(n.a.state() == link_state_t::DIALING, "  DIALING while the attempt is in flight");

    n.b.carrier().pump();
    check(n.wire.log.size() == 2 && type_of(n.wire, 1) == 0x02 &&
              n.wire.log[1].bytes.size() == 50 && n.wire.log[1].to == kAddrA,
          "  the responder answers at the first message's source with 50 bytes");
    check(n.b.poll(0).session == false, "  the responder sends nothing on a pending session");

    n.a.carrier().pump();
    const auto p = n.a.poll(0);
    check(p.session && p.state == link_state_t::DIALING,
          "  the initiator holds an unacknowledged session: send admitted, not yet UP");
    check(n.wire.log.size() == 2, "  nothing is sent until the application sends");

    n.a.send(std::span(request));  // the confirmation
    check(n.wire.log.size() == 3 && type_of(n.wire, 2) == 0x04 &&
              n.wire.log[2].bytes.size() == request.size() + 25,
          "  the request is the first transport message (the confirmation)");
    n.b.carrier().pump();
    check(n.sink_b.frames.size() == 1 && n.sink_b.frames[0] == request,
          "  the responder delivers the request, byte-identical");
    check(n.wire.log.size() == 4 && n.wire.log[3].bytes.size() == reply.size() + 25 &&
              n.wire.log[3].to == kAddrA,
          "  its reply (sent from the receive callback) is the acknowledgement");
    check(n.a.state() == link_state_t::DIALING, "  the initiator is still not UP before the reply");
    n.a.carrier().pump();
    check(n.sink_a.frames.size() == 1 && n.sink_a.frames[0] == reply, "  the reply is delivered");
    check(n.a.state() == link_state_t::UP, "  UP only after the acknowledgement");
    check(n.wire.log.size() == 4, "  four datagrams: two round trips, nothing else");
    for (const counter_t c : {counter_t::HANDSHAKE_FAILED, counter_t::STALE, counter_t::RETRANSMITS,
                              counter_t::MALFORMED_RX})
        check(n.a.count(c) == 0 && n.b.count(c) == 0, "  no failure counter moved");

    // The session stays up for more traffic, both ways.
    n.a.send(std::span(request));
    n.settle();
    check(n.sink_b.frames.size() == 2 && n.sink_a.frames.size() == 2, "  a second exchange flows");
}

/**
 * @brief RFC-0033 Appendix A through the link: the same inputs give the same datagrams.
 *
 * All four transport messages match A.3 byte for byte: the initiator's empty confirmation
 * (n=0, sent at its retry deadline) and its `ping` (n=1), then the responder's empty
 * acknowledgement (n=0, §4 step 7) and its `pang` (n=1), as RFC-0033's erratum of 2026-10-11
 * lists them.
 */
void appendix_a_through_the_link() {
    std::printf("RFC-0033 Appendix A, reproduced by the link:\n");
    noise_config_t ca = mem_config();
    mem_wire_t wire;
    test_host_t host_a;
    test_host_t host_b;
    host_a.fixed = {key("202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f")};
    host_b.fixed = {key("404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f")};
    host_a.counter_fixed = (std::uint64_t{1} << 32) | 1;
    mem_link_t a(conn_role_t::DIAL, host_a, ca, wire, 0);
    mem_link_t b(conn_role_t::LISTEN, host_b, mem_config(), wire, 1);
    sink_t sink_a;
    sink_t sink_b;
    a.set_receiver(sink_a);
    b.set_receiver(sink_b);
    (void)a.poll(0);
    (void)b.poll(0);

    a.dial();
    check(wire.log.size() == 1 &&
              wire.log[0].bytes ==
                  unhex("01 358072d6365880d1aeea329adf9121383851ed21a28e3b75e965d0d2cd166254"
                        "305dfcc173612b6516 04ae630d4c171a9f19e1d41b12b973f2"),
          "  msg1 is A.3's 58 bytes");
    b.carrier().pump();
    check(wire.log.size() == 2 &&
              wire.log[1].bytes ==
                  unhex("02 79a631eede1bf9c98f12032cdeadd0e7a079398fc786b88cc846ec89af85a51a"
                        "94 5968a975a98d942d263ea658be0b1b91"),
          "  msg2 is A.3's 50 bytes");
    a.carrier().pump();
    (void)a.poll(10);  // the retry deadline: the empty confirmation
    check(wire.log.size() == 3 &&
              wire.log[2].bytes == unhex("04 0000000000000000 b215c81b607068db3638292cc0825965"),
          "  the empty confirmation is A.3's (n=0)");
    const auto ping = unhex("0100040070696e67");
    a.send(std::span(ping));
    check(wire.log.size() == 4 &&
              wire.log[3].bytes ==
                  unhex("04 0100000000000000 843c7ef02f4b3184 8add30c2b08aa1eef0985b2201f08621"),
          "  the ping is A.3's (n=1)");
    b.carrier().pump();
    check(wire.log.size() == 5 &&
              wire.log[4].bytes == unhex("04 0000000000000000 4a986111144b7702d0a867a37da80f23"),
          "  the responder's empty acknowledgement is A.3's (n=0)");
    check(sink_b.frames.size() == 1 && sink_b.frames[0] == ping, "  the responder delivers ping");
    const auto pang = unhex("0100040070616e67");
    b.send(std::span(pang));
    check(wire.log.size() == 6 &&
              wire.log[5].bytes ==
                  unhex("04 0100000000000000 d4df8a687acd1ca4 14ed6223ec12519b15a8e6edc0328c18"),
          "  the pang is A.3's (n=1)");
    a.carrier().pump();
    check(a.state() == link_state_t::UP && sink_a.frames.size() == 1 && sink_a.frames[0] == pang,
          "  the initiator is UP and receives pang");
}

/** @brief A wrong PSK: refused at the tag, counted, nothing sent back, no allocation. */
void wrong_key_is_refused_silently() {
    std::printf("a wrong PSK is refused, counted and silent:\n");
    mem_pair_t n;
    n.host_b.psk[0] ^= std::byte{1};  // too late for b: it resolved its key when built
    test_host_t other;
    other.psk = n.host_b.psk;
    mem_link_t b2(conn_role_t::LISTEN, other, mem_config(), n.wire, 1);
    n.a.dial();
    check(n.wire.log.size() == 1, "  the initiator sends its first message");
    const std::vector<std::byte> first = n.wire.inbox[1].front().bytes;
    n.wire.inbox[1].clear();
    // Warm the path once, then count: the refused read must allocate nothing.
    b2.carrier().inject(first, kAddrA);
    std::vector<std::byte> probe = first;
    const std::size_t before = g_allocations.load();
    b2.carrier().inject_in_place(probe, kAddrA);
    const std::size_t allocated = g_allocations.load() - before;
    check(b2.count(counter_t::HANDSHAKE_FAILED) == 2, "  each refused first message is counted");
    check(n.wire.log.size() == 1, "  the responder sends nothing: no reply in clear, no reply");
    check(b2.state() == link_state_t::LISTENING && !b2.poll(0).session, "  and holds no session");
    if (kCountsAllocations)
        check(allocated == 0, "  reading a first message under a wrong PSK allocates nothing");
    else
        std::printf("  (allocation row skipped: not counted on this build)\n");
    check(n.a.poll(0).state == link_state_t::DIALING && !n.a.poll(0).session,
          "  the initiator sees only a handshake that never completes");
    check(n.a.drop_stats().malformed_rx == 0 && n.a.count(counter_t::STALE) == 0,
          "  ... and learns nothing from the responder");
}

/**
 * @brief A lost second message: the initiator retransmits the same 58 bytes on `now`, the
 *        responder answers with its stored second message, and the exchange completes.
 */
void retransmit_on_now() {
    std::printf("retransmission runs on the application's now:\n");
    mem_pair_t n;
    (void)n.a.poll(0);
    int lost = 0;
    n.wire.drop = [&lost](const datagram_t& d) {
        return d.from == 1 && std::to_integer<std::uint8_t>(d.bytes[0]) == 0x02 && lost++ == 0;
    };
    n.a.dial();
    n.b.carrier().pump();  // msg2 is lost
    n.a.carrier().pump();
    check(!n.a.poll(9).session && n.wire.log.size() == 2, "  before the retry deadline: nothing");
    const auto p = n.a.poll(10);
    check(n.wire.log.size() == 3 && n.wire.log[2].bytes == n.wire.log[0].bytes,
          "  at the deadline the first message is resent, byte-identical");
    check(p.next == 30, "  the next retry is at twice the interval");
    check(n.a.count(counter_t::RETRANSMITS) == 1, "  counted in noise_retransmits (initiator)");
    n.b.carrier().pump();
    check(n.wire.log.size() == 4 && n.wire.log[3].bytes == n.wire.log[1].bytes,
          "  the responder resends its stored second message, unchanged");
    check(n.b.count(counter_t::RETRANSMITS) == 1 && n.b.count(counter_t::HANDSHAKE_REPLAYED) == 0,
          "  counted in noise_retransmits (responder), never as a replay");
    n.a.carrier().pump();
    check(n.a.poll(11).session, "  the initiator now holds a session");
    const auto request = value_frame("ping");
    n.sink_b.then = [&](std::span<const std::byte> f) { n.b.send(f); };
    n.a.send(std::span(request));
    n.settle();
    check(n.a.state() == link_state_t::UP && n.sink_a.frames.size() == 1,
          "  and the exchange completes");
}

/** @brief The attempt deadline ends an attempt; the next demand starts a fresh one. */
void attempt_deadline() {
    std::printf("the attempt deadline, and an unacknowledged session's empty confirmations:\n");
    mem_pair_t n;
    (void)n.a.poll(0);
    n.a.dial();
    (void)n.a.poll(10);  // retransmit 1, next at 30
    (void)n.a.poll(30);  // retransmit 2, next at 70 (cap 40)
    (void)n.a.poll(70);  // retransmit 3, next at 110
    check(n.a.count(counter_t::RETRANSMITS) == 3, "  the interval doubles up to its cap");
    check(n.a.poll(999).state == link_state_t::DIALING, "  still dialing before the deadline");
    check(n.a.poll(1000).state == link_state_t::DORMANT, "  dormant at the attempt deadline");
    const std::size_t sent = n.wire.log.size();
    check(n.a.poll(5000).next == 0 && n.wire.log.size() == sent,
          "  an unheld link starts nothing on its own");
    n.a.dial();
    check(n.wire.log.size() == sent + 1 && n.wire.log.back().bytes != n.wire.log[0].bytes,
          "  a new demand starts a new attempt: new ephemeral key, new counter");
    n.wire.inbox[1].clear();
    n.wire.inbox[1].push_back(n.wire.log.back());
    n.b.carrier().pump();
    n.a.carrier().pump();
    // Unacknowledged and the application sends nothing: each retry sends an empty confirmation.
    const std::size_t before = n.wire.log.size();
    (void)n.a.poll(5010);
    check(n.wire.log.size() == before + 1 && n.wire.log.back().bytes.size() == 25,
          "  an unacknowledged session gets an empty confirmation at its retry deadline");
    n.b.carrier().pump();
    check(n.wire.log.back().from == 1 && n.wire.log.back().bytes.size() == 25,
          "  the responder answers it with one empty message");
    n.a.carrier().pump();
    check(n.a.state() == link_state_t::UP, "  which acknowledges the session");
}

/** @brief A pending session expires on the responder's `now`; its confirmation is then stale. */
void pending_expiry() {
    std::printf("a pending session expires on the responder's now:\n");
    mem_pair_t n;
    (void)n.b.poll(100);
    n.a.dial();
    n.b.carrier().pump();
    n.a.carrier().pump();
    check(n.b.poll(599).next == 600, "  the pending session's expiry is the next deadline");
    (void)n.b.poll(600);
    check(n.b.count(counter_t::PENDING_EXPIRED) == 1, "  counted in noise_pending_expired");
    n.a.send(std::span<const std::byte>(value_frame("late")));
    n.b.carrier().pump();
    check(n.b.count(counter_t::STALE) == 1 && n.sink_b.frames.empty(),
          "  the late confirmation names no session: noise_stale, not delivered");
}

/** @brief First-message replay, the roles, reserved types and the frame rule. */
void refusals() {
    std::printf("refusals, each counted where reference/23 §9 says:\n");
    mem_pair_t n;
    n.sink_b.then = [&](std::span<const std::byte> f) { n.b.send(f); };
    n.a.dial();
    n.settle();
    n.a.send(std::span<const std::byte>(value_frame("ping")));
    n.settle();
    check(n.a.state() == link_state_t::UP, "  (setup: the link is up)");
    const std::size_t sent = n.wire.log.size();
    const auto first = n.wire.log[0].bytes;
    const auto second = n.wire.log[1].bytes;
    const auto request = n.wire.log[2].bytes;

    std::vector<std::byte> replay = first;
    const std::size_t before = g_allocations.load();
    n.b.carrier().inject_in_place(replay, kAddrA);
    const std::size_t allocated = g_allocations.load() - before;
    check(n.b.count(counter_t::HANDSHAKE_REPLAYED) == 1 && n.wire.log.size() == sent,
          "  a replayed first message stops at the counter, unanswered");
    if (kCountsAllocations)
        check(allocated == 0, "  ... before any Diffie-Hellman: it allocates nothing");
    n.b.carrier().inject(request, kAddrStranger);
    check(n.b.count(counter_t::REPLAYED) == 1 && n.sink_b.frames.size() == 1,
          "  a copy of the newest frame is refused by the replay window");
    auto tampered = request;
    tampered[12] ^= std::byte{1};
    tampered[1] = std::byte{9};  // a fresh nonce, so the window passes it to the tag
    n.b.carrier().inject(tampered, kAddrStranger);
    check(n.b.count(counter_t::DECRYPT_FAILED) == 1, "  a tampered message fails its tag");
    n.b.send(std::span<const std::byte>(value_frame("pang")));
    check(n.wire.log.size() == sent + 1 && n.wire.log.back().to == kAddrA,
          "  the replayed and tampered messages from a stranger never moved B's peer (§4)");
    n.b.carrier().inject(second, kAddrA);
    n.a.carrier().inject(first, kAddrB);
    check(n.b.count(counter_t::STALE) == 1 && n.a.count(counter_t::STALE) == 1,
          "  a message for the other role is stale (roles never swap)");
    n.a.carrier().inject(second, kAddrB);
    check(n.a.count(counter_t::STALE) == 2,
          "  a second message with no attempt in flight is stale");
    n.b.carrier().inject(unhex("03 00"), kAddrA);
    n.b.carrier().inject(std::vector<std::byte>(first.begin(), first.end() - 1), kAddrA);
    check(n.b.drop_stats().malformed_rx == 2,
          "  a reserved type, or a short first message, is malformed");
    const std::string raw = "not a frame";
    n.a.send(std::as_bytes(std::span(raw)));
    n.b.carrier().pump();
    check(n.b.drop_stats().malformed_rx == 3 && n.sink_b.frames.size() == 1,
          "  a plaintext that is not exactly one frame is malformed, not delivered");
    check(n.wire.log.size() == sent + 2, "  none of these drew an answer");
    const std::size_t dropped = n.a.drop_stats().dropped_tx;
    n.a.send(std::span<const std::byte>{});
    check(n.a.drop_stats().dropped_tx == dropped + 1 && n.wire.log.size() == sent + 2,
          "  an empty frame is refused at send: an empty plaintext is the protocol's (§3)");
}

/** @brief A frame above `max_frame` is refused at send, whole, and counted. */
void oversize_refused() {
    std::printf("a frame above max_frame is refused at send:\n");
    noise_config_t c = mem_config();
    c.max_frame = 64;
    mem_pair_t n(c, mem_config());
    check(n.a.max_frame() == 64, "  max_frame is the configured one");
    n.a.dial();
    n.settle();
    const std::vector<std::byte> big(65, std::byte{0x5A});
    const std::size_t sent = n.wire.log.size();
    n.a.send(std::span(big));
    check(n.a.count(counter_t::OVERSIZE) == 1 && n.wire.log.size() == sent,
          "  counted in noise_oversize; nothing is sent, nothing truncated");
    mem_wire_t w;
    test_host_t h;
    mem_link_t tiny(conn_role_t::DIAL, h, mem_config(), w, 0, std::size_t{57});
    check(!tiny.ok(), "  a carrier whose bound is below 58 bytes cannot carry a Noise link");
}

/** @brief A block source that counts what it hands out, over the heap. */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    counting_source_t() : block_source_t("counting") {}
    void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        ++allocs;
        live += bytes;
        return tr::mem::heap_source().try_alloc(bytes, align);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        live -= bytes;
        tr::mem::heap_source().release(p, bytes, align);
    }
    std::size_t allocs = 0; /**< @brief Blocks handed out. */
    std::size_t live = 0;   /**< @brief Bytes not yet returned. */
};

/** @brief The session state is one block from `memory.state`; each send one from `memory.io`. */
void state_from_the_links_own_source() {
    std::printf("session state comes from the link's own source:\n");
    counting_source_t state_a;
    counting_source_t io_a;
    counting_source_t state_b;
    counting_source_t io_b;
    noise_config_t ca = mem_config();
    ca.memory.state = &state_a;
    ca.memory.io = &io_a;
    noise_config_t cb = mem_config();
    cb.memory.state = &state_b;
    cb.memory.io = &io_b;
    {
        mem_pair_t n(ca, cb);
        check(state_a.allocs == 1 && state_b.allocs == 1 && state_a.live > 0,
              "  one state block per link, drawn when it is built");
        std::printf("  state block per link: %zu B\n", state_a.live);
        n.sink_b.then = [&](std::span<const std::byte> f) { n.b.send(f); };
        n.a.dial();
        n.settle();
        for (int i = 0; i < 3; ++i) n.a.send(std::span<const std::byte>(value_frame("ping")));
        n.settle();
        check(n.sink_a.frames.size() == 3, "  (three exchanges completed)");
        check(state_a.allocs == 1 && state_b.allocs == 1,
              "  a handshake and its frames draw nothing more from the state store");
        check(io_a.allocs == 3 && io_b.allocs == 3 && io_a.live == 0 && io_b.live == 0,
              "  one seal block per frame from memory.io, each returned after its send");
        n.b.carrier().inject(n.wire.log[0].bytes, kAddrA);  // a replayed first message
        check(state_b.allocs == 1 && io_b.allocs == 3, "  a refused first message draws nothing");
    }
    check(state_a.live == 0 && state_b.live == 0, "  the state block is returned with the link");
    test_host_t h;
    h.name = "other";
    mem_wire_t w;
    mem_link_t unknown(conn_role_t::DIAL, h, mem_config(), w, 0);
    check(!unknown.ok(), "  an unknown PSK reference leaves the link inert");
}

/** @brief The test application's clock: steady nanoseconds (the link has none of its own). */
std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

/** @brief Poll until @p pred holds or @p budget passes, driving both links with `now`. */
template <class Pred>
bool drive(udp_link_t& a, udp_link_t& b, Pred pred, std::chrono::milliseconds budget) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < budget) {
        const std::uint64_t now = now_ns();
        (void)a.poll(now);
        (void)b.poll(now);
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return pred();
}

/** @brief A READ reply's record. */
struct got_t {
    std::atomic<int> calls{0};    /**< @brief Replies seen. */
    std::mutex mu;                /**< @brief Guards @ref bytes. */
    std::vector<std::byte> bytes; /**< @brief The last reply. */
};

/** @brief The record sink. */
void on_got(void* ctx, const tr::view::rope_t& reply) {
    auto* g = static_cast<got_t*>(ctx);
    const tr::view::view_t m = reply.materialize();
    {
        const std::lock_guard lock(g->mu);
        g->bytes.assign(m.bytes().begin(), m.bytes().end());
    }
    g->calls.fetch_add(1);
}

/** @brief The u64 value of @p noun in a decoded `:stats` block, or ~0. */
std::uint64_t stat(const tr::wire::tlv_t& block, std::string_view noun) {
    for (std::size_t i = 0; i + 1 < block.children.size(); i += 2) {
        if (tr::detail::as_string_view(block.children[i].payload) != noun) continue;
        if (block.children[i + 1].payload.size() != 8) break;
        return tr::detail::load_le<std::uint64_t>(block.children[i + 1].payload);
    }
    return ~std::uint64_t{0};
}

/** @brief A decoded stats block and the bytes its spans point into. */
struct stats_read_t {
    std::vector<std::byte> bytes;
    tr::wire::tlv_t tlv;
};

/** @brief Read `/sensor/temp:stats.link.<child>` on @p g and decode it. */
std::optional<stats_read_t> stats_block(graph_t& g, const char* path) {
    const auto p = path_t::parse(path);
    if (!p) return std::nullopt;
    const auto v = g.find(p->key());
    if (!v) return std::nullopt;
    const auto r = g.read(*v, p->field(), {});
    if (!r) return std::nullopt;
    const tr::view::view_t flat = (*r)->flatten();
    std::optional<stats_read_t> out(std::in_place);
    out->bytes.assign(flat.bytes().begin(), flat.bytes().end());
    auto decoded = tr::wire::decode(out->bytes);
    if (!decoded) return std::nullopt;
    out->tlv = std::move(*decoded);
    return out;
}

/** @brief The config of the UDP cases, in nanoseconds: retry 20 ms, attempt 2 s. */
noise_config_t udp_config() {
    return noise_config_t{.psk_ref = "link-psk",
                          .retry = 20'000'000,
                          .retry_cap = 200'000'000,
                          .attempt = 2'000'000'000,
                          .pending_expiry = 2'000'000'000};
}

/**
 * @brief Two graph nodes over real UDP: node A reads `/sensor/temp` on node B through a Noise
 *        link, and B's `:stats.link.a` carries the `noise_*` nouns.
 */
void two_nodes_over_udp() {
    std::printf("two nodes over a Noise link on UDP: a READ and its reply:\n");
    graph_t ga;
    graph_t gb;
    tr::net::fwd_router_t ra(ga);
    tr::net::fwd_router_t rb(gb);
    test_host_t host_a;
    test_host_t host_b;
    // The links after the routers, so they are destroyed first (their carriers' threads join).
    udp_link_t b(conn_role_t::LISTEN, host_b, udp_config(), std::uint16_t{0}, std::string_view{},
                 std::uint16_t{0});
    udp_link_t a(conn_role_t::DIAL, host_a, udp_config(), std::uint16_t{0},
                 std::string_view{"127.0.0.1"}, b.carrier().local_port());
    check(a.ok() && b.ok(), "  both links came up");
    (void)a.poll(now_ns());  // deadlines count from the last `now` a link was given
    (void)b.poll(now_ns());
    const auto temp = gb.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)gb.write(temp, tr::testing::make_value(value_frame("21.5")));
    (void)ra.attach_link("b", a);
    (void)rb.attach_link("a", b);

    got_t dropped;
    tr::net::fwd_router_t::origin_t first{&on_got, &dropped};
    check(ra.originate(first, fwd_op_t::READ, path_t("/b/sensor/temp")).has_value(),
          "  a READ on the dormant link is originated");
    check(a.drop_stats().dropped_tx == 1, "  ... fails at once, counted, and dials");
    (void)ra.cancel(first);
    check(drive(
              a, b, [&] { return a.link_up(); }, std::chrono::milliseconds(3000)),
          "  the initiator holds a session after one round trip");
    check(a.state() == link_state_t::DIALING, "  not UP yet");

    got_t got;
    tr::net::fwd_router_t::origin_t second{&on_got, &got};
    check(ra.originate(second, fwd_op_t::READ, path_t("/b/sensor/temp")).has_value(),
          "  the READ is sent on the unacknowledged session (the confirmation)");
    check(drive(
              a, b, [&] { return got.calls.load() == 1; }, std::chrono::milliseconds(3000)),
          "  the reply arrives (the acknowledgement)");
    check(a.state() == link_state_t::UP, "  the initiator is UP");
    {
        const std::lock_guard lock(got.mu);
        const auto decoded = tr::wire::decode(got.bytes);
        const auto want = value_frame("21.5");
        check(decoded && decoded->children.size() >= 5 &&
                  decoded->children[4].payload.size() == 4 &&
                  std::memcmp(decoded->children[4].payload.data(), want.data() + 4, 4) == 0,
              "  the reply carries B's value");
    }

    const auto block = stats_block(gb, "/sensor/temp:stats.link.a");
    check(block.has_value(), "  B's :stats.link.a reads");
    if (block) {
        check(stat(block->tlv, "noise_handshake_failed") == 0 &&
                  stat(block->tlv, "noise_stale") == 0 && stat(block->tlv, "noise_oversize") == 0 &&
                  stat(block->tlv, "dropped_tx") == 0,
              "  ... with the noise_* nouns beside the universal three");
        // A loaded host can retransmit even on loopback, so the count is only required present.
        check(stat(block->tlv, "noise_retransmits") != ~std::uint64_t{0} &&
                  stat(block->tlv, "noise_expired") == 0,
              "  ... all ten of them");
    }
}

/** @brief A wrong PSK over UDP: the responder counts it, and the initiator never comes up. */
void wrong_key_over_udp() {
    std::printf("a wrong PSK over UDP:\n");
    test_host_t host_a;
    test_host_t host_b;
    host_b.psk[31] ^= std::byte{0x80};
    udp_link_t b(conn_role_t::LISTEN, host_b, udp_config(), std::uint16_t{0}, std::string_view{},
                 std::uint16_t{0});
    udp_link_t a(conn_role_t::DIAL, host_a, udp_config(), std::uint16_t{0},
                 std::string_view{"127.0.0.1"}, b.carrier().local_port());
    (void)a.poll(now_ns());
    (void)b.poll(now_ns());
    a.dial();
    check(drive(
              a, b, [&] { return b.count(counter_t::HANDSHAKE_FAILED) >= 2; },
              std::chrono::milliseconds(3000)),
          "  the responder refuses the first message and its retransmission");
    check(!a.link_up() && a.state() == link_state_t::DIALING,
          "  the initiator never holds a session");
    check(a.count(counter_t::STALE) == 0 && a.drop_stats().malformed_rx == 0 &&
              a.count(counter_t::HANDSHAKE_FAILED) == 0,
          "  the initiator received nothing at all");
}

}  // namespace

int main() {
    request_reply_two_round_trips();
    appendix_a_through_the_link();
    wrong_key_is_refused_silently();
    retransmit_on_now();
    attempt_deadline();
    pending_expiry();
    refusals();
    oversize_refused();
    state_from_the_links_own_source();
    two_nodes_over_udp();
    wrong_key_over_udp();
    return tr::testing::summary("noise_link");
}
