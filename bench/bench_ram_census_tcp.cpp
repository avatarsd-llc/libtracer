/**
 * @file
 * @brief The 100-vertex RAM census over the canonical TCP transport — how many heap
 *        bytes libtracer itself holds, per stage, for a realistic small-node graph
 *        with a real remote peer on the other end of a real socket.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The question: *how much RAM does a 100-vertex graph cost — values 4..64 bytes each,
 * mixed int / array / STREAM shapes — with the canonical TCP transport on a unix host?*
 * The deliverable is **bytes USED by libtracer**, measured with a counting allocator —
 * never a free-memory reading, which on glibc measures the arena, not the library.
 *
 * `bench_conn_ram` prices ONE CONNECTION and constructs its transports by hand; this
 * bench prices the WHOLE NODE and constructs its listener the way the wire does — an
 * in-band `write /net/tcp-server/conn <- SPEC{name, config{kind=tcp, port}}` to the
 * module's creator endpoint (ADR-0027, RFC-0014; the module segment carries the role, so
 * the SPEC has no `type` and no `role` pair). Nothing is hand-wired into the router, so
 * every byte counted below is on a production path (the RFC-0014 lesson: two silent
 * misroutes shipped because no test used the production wiring).
 *
 * ### The five stages (each a LIVE-balance reading, relative to the pre-graph baseline)
 *
 *   S_A  an empty `graph_t`                                   (the fixed graph core)
 *   S_B1 + 100 vertices REGISTERED, none written yet          (per-vertex IDENTITY)
 *   S_B  + each of those written once with its value          (per-vertex + VALUE)
 *   S_C  + `fwd_router_t` + `transport_vertex_t` + the SPEC-created TCP listener
 *   S_D  + a peer PROCESS connected and one FWD round-trip verified
 *   S_E  + steady state after N mixed FWD read/write ops from that peer
 *
 * `S_B - S_A` over 100 is the per-vertex amortized cost; the same census run with an
 * all-4-byte and an all-64-byte value mix gives the value-size sensitivity.
 *
 * ### Two processes, one real socket
 *
 * The peer is a SEPARATE PROCESS — forked ONCE at the top of `main`, before any thread
 * or graph exists, and driven per repetition over a pipe. It is a full libtracer node
 * (its own `graph_t` + `fwd_router_t` + `transport_vertex_t`) whose TCP client is also
 * created from a SPEC written to `/net/tcp-client/conn`, and it drives ops through its own
 * router's egress. So the measured process contains exactly ONE libtracer instance: no
 * `loopback_channel_t`, no in-process shortcut, and not one counted byte belongs to
 * the peer.
 *
 * The fork happens before the measured process is threaded on purpose: forking a
 * process whose other threads may hold the malloc lock is how a counting-allocator
 * bench deadlocks.
 *
 * ### Instrument
 *
 * A global `operator new`/`delete` override in ALL variants — sized, array, aligned,
 * and **nothrow** (the LKV slot allocates through the nothrow form; missing it is how
 * a census reads a false 0.00). Each reading is the LIVE balance: `malloc_usable_size`
 * added on allocation, subtracted on free, so transient churn (vector regrowth, TLV
 * emit temporaries, parse scratch) cancels and what remains is *held* memory. Blocks
 * (allocs − frees) are reported beside the bytes.
 *
 * Two rules every arm obeys: (1) a NULL arm — two readings with nothing but the
 * quiesce between them — is printed with the rest and must read 0, so the reader can
 * confirm the instrument is not itself allocating inside a window; (2) every arm runs
 * `--reps` times INTERLEAVED and reports the MEDIAN with min/max, and the first
 * repetition is DISCARDED (it pays one-time lazy initialisation — locale, iostream
 * facets, the first arena growth).
 *
 * @warning What this does NOT count: pthread STACKS. The TCP listener spawns a receive
 * thread whose stack is `mmap`ed, not `malloc`ed, so no `operator new` counter can see
 * it — on glibc a few resident pages of an 8 MiB mapping, on ESP-IDF a FreeRTOS task
 * stack that IS heap and dominates every number here. Read the thread count alongside
 * the bytes. Nor does it count `.bss`/`.data` (static RAM), which the footprint
 * sentinel prices separately.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if __has_include(<malloc.h>)
#include <malloc.h>
#define BENCH_HAS_USABLE_SIZE 1
#endif

#include "libtracer/byteorder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

// --- the counting allocator override (all variants) --------------------------

namespace {

/** @brief Live usable-size balance while armed — the steady-state heap the process holds. */
std::atomic<long long> g_live{0};
/** @brief High-water mark of @ref g_live — catches TRANSIENT per-stage buffers. */
std::atomic<long long> g_peak{0};
/** @brief Blocks allocated / freed while armed; their difference is the LIVE block count. */
std::atomic<long long> g_allocs{0};
std::atomic<long long> g_frees{0};
/** @brief Counting is on. */
std::atomic<bool> g_armed{false};

/** @brief Raise the recorded peak to @p now if it is higher. */
void bump_peak(long long now) {
    long long seen = g_peak.load(std::memory_order_relaxed);
    while (now > seen && !g_peak.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
    }
}

/** @brief `malloc` plus the counting side effects (usable size, block count, peak). */
void* counted_alloc(std::size_t size) {
    void* p = std::malloc(size != 0 ? size : 1);
    if (g_armed.load(std::memory_order_relaxed) && p != nullptr) {
        g_allocs.fetch_add(1, std::memory_order_relaxed);
#ifdef BENCH_HAS_USABLE_SIZE
        const auto usable = static_cast<long long>(malloc_usable_size(p));
        bump_peak(g_live.fetch_add(usable, std::memory_order_relaxed) + usable);
#endif
    }
    return p;
}

/** @brief `free` plus the counting side effects. */
void counted_free(void* p) {
    if (p == nullptr) return;
    if (g_armed.load(std::memory_order_relaxed)) {
        g_frees.fetch_add(1, std::memory_order_relaxed);
#ifdef BENCH_HAS_USABLE_SIZE
        g_live.fetch_sub(static_cast<long long>(malloc_usable_size(p)), std::memory_order_relaxed);
#endif
    }
    std::free(p);
}

}  // namespace

void* operator new(std::size_t size) {
    void* p = counted_alloc(size);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size) {
    void* p = counted_alloc(size);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return counted_alloc(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return counted_alloc(size);
}
void* operator new(std::size_t size, std::align_val_t) { return operator new(size); }
void* operator new[](std::size_t size, std::align_val_t) { return operator new(size); }
void* operator new(std::size_t size, std::align_val_t, const std::nothrow_t&) noexcept {
    return counted_alloc(size);
}
void* operator new[](std::size_t size, std::align_val_t, const std::nothrow_t&) noexcept {
    return counted_alloc(size);
}
void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, std::size_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::size_t) noexcept { counted_free(p); }
void operator delete(void* p, std::align_val_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { counted_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { counted_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { counted_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { counted_free(p); }

// --- harness -----------------------------------------------------------------

namespace {

using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::tcp_transport_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Read the live balance. */
long long live() { return g_live.load(std::memory_order_relaxed); }
/** @brief Live block count: blocks allocated minus blocks freed while armed. */
long long blocks() {
    return g_allocs.load(std::memory_order_relaxed) - g_frees.load(std::memory_order_relaxed);
}
/** @brief Re-base the peak tracker on the current balance. */
void reset_peak() { g_peak.store(live(), std::memory_order_relaxed); }
/** @brief Read the peak tracker. */
long long peak() { return g_peak.load(std::memory_order_relaxed); }

/** @brief The settle a receive thread needs before a balance is stable. */
void quiesce() { std::this_thread::sleep_for(200ms); }

/** @brief How many vertices the census graph holds. */
constexpr std::size_t kVertices = 100;

/** @brief The value-size mix an arm writes into its vertices. */
enum class mix_t {
    MIXED, /**< @brief 4..64 B: int scalars, arrays (PL=1 homogeneous children), STREAM values. */
    SMALL, /**< @brief Every value 4 B — the low end of the sensitivity sweep. */
    LARGE, /**< @brief Every value 64 B — the high end of the sensitivity sweep. */
};

/** @brief One repetition's five stage readings, in bytes and in live blocks. */
struct sample_t {
    long long b_a = 0;    /**< @brief S_A: the empty graph_t. */
    long long b_b1 = 0;   /**< @brief S_B1: + kVertices vertices REGISTERED, none written yet. */
    long long b_b = 0;    /**< @brief S_B: + each of those written once with its value. */
    long long b_c = 0;    /**< @brief S_C: + router + transport vertex + SPEC-created listener. */
    long long b_d = 0;    /**< @brief S_D: + the peer process connected, one round-trip verified. */
    long long b_e = 0;    /**< @brief S_E: + steady state after the mixed op storm. */
    long long peak_e = 0; /**< @brief The high-water balance seen during the op storm. */
    long long blk_a = 0;  /**< @brief Live blocks at S_A. */
    long long blk_b1 = 0; /**< @brief Live blocks at S_B1. */
    long long blk_b = 0;  /**< @brief Live blocks at S_B. */
    long long blk_c = 0;  /**< @brief Live blocks at S_C. */
    long long blk_d = 0;  /**< @brief Live blocks at S_D. */
    long long blk_e = 0;  /**< @brief Live blocks at S_E. */
    bool ok = false;      /**< @brief Every stage reached its expected state. */
};

// --- wire helpers (the tcp_test shapes, verbatim in intent) -------------------

/** @brief Wrap @p bytes in an owning heap segment view — what `graph_t::write` stores. */
view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/** @brief A flat opaque VALUE TLV carrying @p n payload bytes — the int-scalar shape. */
std::vector<std::byte> scalar_value(std::size_t n, std::uint8_t fill) {
    std::vector<std::byte> payload(n, std::byte{fill});
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, payload);
    return out;
}

/**
 * @brief A structured (`opt.PL=1`) VALUE whose children are @p n / 4 homogeneous 4-byte
 *        VALUEs — the ARRAY shape (array-ness is an L4 schema property; on the wire an
 *        array is just a PL=1 TLV with homogeneous children, ADR-0008).
 */
std::vector<std::byte> array_value(std::size_t n, std::uint8_t fill) {
    std::vector<std::byte> children;
    const std::size_t count = std::max<std::size_t>(1, n / 4);
    for (std::size_t i = 0; i < count; ++i) {
        std::vector<std::byte> elem(4, std::byte{static_cast<std::uint8_t>(fill + i)});
        tr::wire::emit_tlv(children, type_t::VALUE, opt_t{}, elem);
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{.pl = true}, children);
    return out;
}

/**
 * @brief The connection-creation SPEC (ADR-0027 / reference 05) — the tcp_test shape.
 *
 * SPEC{ NAME "name" <name>, SETTINGS "config"{ NAME "port" VALUE u16,
 *       NAME "kind" NAME "tcp" [, NAME "addr" NAME <addr>] } }
 *
 * There is no `type` and no `role` pair: this payload is written to a module's creator
 * endpoint `/net/<module>/conn`, and the module was declared for exactly one (kind, role),
 * so the path already fixes both (RFC-0014 S7).
 */
view_t tcp_conn_spec(std::string_view name, std::uint16_t port, std::string_view addr = {}) {
    return tr::net::conn_spec(name, port, "tcp", addr);
}

/** @brief FWD{ op=WRITE, dst=<segs...>, src=<empty>, payload } — a remote write (ADR-0040). */
std::vector<std::byte> fwd_write(const std::vector<std::string>& dst,
                                 std::span<const std::byte> payload_value_tlv) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    std::vector<std::byte> dst_segs;
    for (const std::string& s : dst) (void)tr::wire::emit_path_segment(dst_segs, s);
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{}, dst_segs);
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{}, std::span<const std::byte>{});
    body.insert(body.end(), payload_value_tlv.begin(), payload_value_tlv.end());
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

/** @brief FWD{ op=READ, dst, src } — a remote read whose REPLY source-routes back. */
std::vector<std::byte> fwd_read(const std::vector<std::string>& dst,
                                const std::vector<std::string>& src) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::READ)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    std::vector<std::byte> dst_segs;
    for (const std::string& s : dst) (void)tr::wire::emit_path_segment(dst_segs, s);
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{}, dst_segs);
    std::vector<std::byte> src_segs;
    for (const std::string& s : src) (void)tr::wire::emit_path_segment(src_segs, s);
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{}, src_segs);
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

// --- the census graph --------------------------------------------------------

/** @brief The name of census vertex @p i — a flat single-segment path, `/v000`.. */
std::string vname(std::size_t i) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "v%03zu", i);
    return std::string(buf);
}

/**
 * @brief The value-payload byte count vertex @p i carries under @p mix.
 *
 * MIXED walks the 4..64 B range the question names (4, 8, 16, 32, 64); SMALL and LARGE
 * pin every vertex to one end of it so the two arms differ in exactly one variable.
 */
std::size_t value_bytes_for(std::size_t i, mix_t mix) {
    if (mix == mix_t::SMALL) return 4;
    if (mix == mix_t::LARGE) return 64;
    static constexpr std::array<std::size_t, 5> kSizes{4, 8, 16, 32, 64};
    return kSizes[i % kSizes.size()];
}

/**
 * @brief Register @p kVertices vertices and write each one's value exactly once.
 *
 * The shape rotates through the three kinds the question names: an int scalar (a flat
 * opaque VALUE), an ARRAY (a PL=1 VALUE with homogeneous 4-byte children), and a STREAM
 * -role vertex (the bounded history ring — the only role that appends). The STREAM
 * vertices get an explicit owner-side retention depth: `set_history_depth` is a
 * construction parameter with no wire surface (RFC-0022 §3.C), not a synthetic bound.
 */
void register_census_vertices(graph_t& g, std::vector<tr::graph::vertex_handle_t>& handles) {
    for (std::size_t i = 0; i < kVertices; ++i) {
        const role_t role = i % 3 == 2 ? role_t::STREAM : role_t::STORED_VALUE;
        tr::graph::vertex_handle_t vh =
            g.register_vertex(path_t(std::string("/") + vname(i)), role);
        if (role == role_t::STREAM) g.set_history_depth(vh, 4);
        handles.push_back(vh);
    }
}

/** @brief Write each registered vertex's value exactly once (the S_B1 → S_B split). */
void write_census_values(graph_t& g, mix_t mix) {
    for (std::size_t i = 0; i < kVertices; ++i) {
        const std::size_t n = value_bytes_for(i, mix);
        const std::vector<std::byte> v = i % 3 == 1 ? array_value(n, static_cast<std::uint8_t>(i))
                                                    : scalar_value(n, static_cast<std::uint8_t>(i));
        (void)g.write(path_t(std::string("/") + vname(i)), owned(v));
    }
}

// --- the peer process --------------------------------------------------------

/** @brief Write @p n bytes to @p fd, resuming partial writes. */
bool write_all(int fd, const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    while (n != 0) {
        const ssize_t w = ::write(fd, b, n);
        if (w <= 0) return false;
        b += static_cast<std::size_t>(w);
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

/** @brief Read one newline-terminated line from @p fd into @p out. Returns false on EOF. */
bool read_line(int fd, std::string& out) {
    out.clear();
    char c = 0;
    while (true) {
        const ssize_t r = ::read(fd, &c, 1);
        if (r <= 0) return false;
        if (c == '\n') return true;
        out.push_back(c);
    }
}

/** @brief The peer's reply counter — bumped by `fwd_router_t::on_reply`. */
struct reply_sink_t {
    std::atomic<std::size_t> n{0};
};

/**
 * @brief The peer PROCESS body: a full libtracer node that dials the census node's
 *        listener from a SPEC written to `/net/tcp-client/conn` and drives ops over its
 *        own router.
 *
 * Protocol on the pipes, one line each way:
 *   `RUN <port> <ops> <mix>` → dial, one verified round-trip, reply `READY`; wait for
 *                        `GO` (so the census node takes its S_D reading against a
 *                        connected-but-idle peer); then run `<ops>` mixed FWD
 *                        writes/reads whose payload sizes follow `<mix>` — the SAME
 *                        mix the census graph was built with, so an arm's S_E is not
 *                        confounded by the storm rewriting its values at a different
 *                        size; reply `DONE` once the last READ's REPLY has come back
 *                        (TCP order ⇒ every earlier op was processed by then), then
 *                        tear the node down.
 *   `QUIT`             → exit 0.
 *
 * The peer never measures anything: it exists so the census node faces a REAL remote.
 */
int peer_main(int cmd_fd, int rsp_fd) {
    std::string line;
    while (read_line(cmd_fd, line)) {
        if (line == "QUIT") break;
        if (!line.starts_with("RUN ")) continue;
        unsigned port = 0;
        unsigned ops = 0;
        unsigned mix_id = 0;
        if (std::sscanf(line.c_str(), "RUN %u %u %u", &port, &ops, &mix_id) != 3) continue;
        const auto mix = static_cast<mix_t>(mix_id);

        graph_t g;
        tr::net::fwd_router_t router(g);
        tr::net::transport_vertex_t net(g, router);
        (void)net.register_module(std::string(tr::net::kTcpClientSuggestedModule), "tcp",
                                  tr::net::conn_role_t::DIAL);
        reply_sink_t sink;
        router.on_reply(
            [](void* ctx, const tr::view::rope_t&) {
                static_cast<reply_sink_t*>(ctx)->n.fetch_add(1, std::memory_order_relaxed);
            },
            &sink);

        const auto w = g.write(path_t("/net/tcp-client/conn"),
                               tcp_conn_spec("srv", static_cast<std::uint16_t>(port), "127.0.0.1"));
        if (!w.has_value()) {
            (void)write_all(rsp_fd, "FAIL\n", 5);
            continue;
        }

        const std::vector<std::string> mount{"net", std::string(tr::net::kTcpClientSuggestedModule),
                                             "srv"};
        auto dst_for = [&mount](std::size_t i) {
            std::vector<std::string> d = mount;
            d.push_back(vname(i));
            return d;
        };

        // The handshake: one FWD READ whose REPLY proves the census node accepted the
        // connection and its terminus answered. A bare connect() is not observable.
        const std::size_t before = sink.n.load(std::memory_order_relaxed);
        router.on_frame("self", fwd_read(dst_for(0), {"peer-reply"}));
        bool up = false;
        for (int i = 0; i < 400 && !up; ++i) {
            if (sink.n.load(std::memory_order_relaxed) > before)
                up = true;
            else
                std::this_thread::sleep_for(10ms);
        }
        (void)write_all(rsp_fd, up ? "READY\n" : "FAIL\n", up ? 6 : 5);
        if (!up) continue;

        // Wait for the census node to take its S_D reading before the storm starts.
        if (!read_line(cmd_fd, line) || line != "GO") break;

        // The op storm: alternating remote WRITE / READ across all 100 vertices.
        const std::size_t reads_before = sink.n.load(std::memory_order_relaxed);
        std::size_t reads = 0;
        for (std::size_t k = 0; k < ops; ++k) {
            const std::size_t i = k % kVertices;
            if (k % 2 == 0) {
                const std::vector<std::byte> v =
                    scalar_value(value_bytes_for(i, mix), static_cast<std::uint8_t>(k));
                router.on_frame("self", fwd_write(dst_for(i), v));
            } else {
                router.on_frame("self", fwd_read(dst_for(i), {"peer-reply"}));
                ++reads;
            }
        }
        // The final READ's REPLY is the drain barrier: one ordered TCP stream, so every
        // op ahead of it has already been applied at the census node.
        for (int i = 0; i < 2000; ++i) {
            if (sink.n.load(std::memory_order_relaxed) - reads_before >= reads) break;
            std::this_thread::sleep_for(5ms);
        }
        (void)write_all(rsp_fd, "DONE\n", 5);
    }
    return 0;
}

// --- the census arm ----------------------------------------------------------

/** @brief Reserve a free localhost port (a SPEC-created listener refuses port 0). */
std::uint16_t free_port() {
    tcp_transport_t probe{std::uint16_t{0}};
    return probe.local_port();
}

/**
 * @brief One repetition of the census for value mix @p mix, driving the peer process
 *        over @p cmd_fd / @p rsp_fd and running @p ops remote operations.
 */
sample_t run_census(mix_t mix, std::size_t ops, int cmd_fd, int rsp_fd) {
    sample_t s{};
    std::string line;

    const long long base = live();
    const long long base_blk = blocks();

    // --- S_A: the empty graph -----------------------------------------------
    // Declaration order: the router outlives the transport vertex that feeds it, so
    // the sockets (and their recv threads) destruct FIRST.
    graph_t g;
    quiesce();
    s.b_a = live() - base;
    s.blk_a = blocks() - base_blk;

    // --- S_B: 100 vertices, each written once --------------------------------
    // Split in two so the interpretation can separate IDENTITY (registration: the
    // vertex object, its key, its registry entry) from VALUE (the stored rope).
    std::vector<tr::graph::vertex_handle_t> handles;
    handles.reserve(kVertices);
    register_census_vertices(g, handles);
    quiesce();
    s.b_b1 = live() - base;
    s.blk_b1 = blocks() - base_blk;
    write_census_values(g, mix);
    quiesce();
    s.b_b = live() - base;
    s.blk_b = blocks() - base_blk;

    // --- S_C: router + transport vertex + the SPEC-created TCP listener ------
    const std::uint16_t port = free_port();
    tr::net::fwd_router_t router(g);
    tr::net::transport_vertex_t net(g, router);
    (void)net.register_module(std::string(tr::net::kTcpServerSuggestedModule), "tcp",
                              tr::net::conn_role_t::LISTEN);
    const auto wl = g.write(path_t("/net/tcp-server/conn"), tcp_conn_spec("peer", port));
    if (!wl.has_value()) return s;
    if (router.registry().by_name("net/tcp-server/peer") == nullptr) return s;
    quiesce();
    s.b_c = live() - base;
    s.blk_c = blocks() - base_blk;

    // --- S_D: the peer process connects and one round-trip is verified -------
    {
        char cmd[64];
        const int n = std::snprintf(cmd, sizeof(cmd), "RUN %u %zu %u\n",
                                    static_cast<unsigned>(port), ops, static_cast<unsigned>(mix));
        if (!write_all(cmd_fd, cmd, static_cast<std::size_t>(n))) return s;
    }
    if (!read_line(rsp_fd, line) || line != "READY") return s;
    quiesce();
    s.b_d = live() - base;
    s.blk_d = blocks() - base_blk;

    // --- S_E: steady state after the op storm --------------------------------
    reset_peak();
    if (!write_all(cmd_fd, "GO\n", 3)) return s;
    if (!read_line(rsp_fd, line) || line != "DONE") return s;
    quiesce();
    s.b_e = live() - base;
    s.blk_e = blocks() - base_blk;
    s.peak_e = peak() - base;
    s.ok = true;
    return s;
}

/** @brief The NULL arm: two readings with only the quiesce between them. Must read 0. */
long long arm_null() {
    const long long a = live();
    quiesce();
    return live() - a;
}

// --- reporting ---------------------------------------------------------------

/** @brief The median of @p v (upper median for an even count). */
long long median(std::vector<long long> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

/** @brief Emit one RESULT line: median with min/max over the retained repetitions. */
void report(const char* arm, const char* metric, std::vector<long long>& v) {
    if (v.empty()) return;
    auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    std::printf("RESULT arm=%-9s metric=%-14s n=%zu median=%lld min=%lld max=%lld\n", arm, metric,
                v.size(), median(v), *lo, *hi);
}

/** @brief One arm's retained repetitions, column by column. */
struct series_t {
    std::vector<long long> b_a, b_b1, b_b, b_c, b_d, b_e, peak_e;
    std::vector<long long> per_vertex, per_vertex_id, per_vertex_val;
    std::vector<long long> blk_a, blk_b1, blk_b, blk_c, blk_d, blk_e;

    /** @brief Reserve for @p n repetitions so no push_back allocates inside a window. */
    void reserve(std::size_t n) {
        for (auto* c : {&b_a, &b_b1, &b_b, &b_c, &b_d, &b_e, &peak_e, &per_vertex, &per_vertex_id,
                        &per_vertex_val, &blk_a, &blk_b1, &blk_b, &blk_c, &blk_d, &blk_e})
            c->reserve(n);
    }
    /** @brief Retain one repetition. */
    void push(const sample_t& s) {
        constexpr auto kN = static_cast<long long>(kVertices);
        b_a.push_back(s.b_a);
        b_b1.push_back(s.b_b1);
        b_b.push_back(s.b_b);
        per_vertex_id.push_back((s.b_b1 - s.b_a) / kN);
        per_vertex_val.push_back((s.b_b - s.b_b1) / kN);
        blk_b1.push_back(s.blk_b1);
        b_c.push_back(s.b_c);
        b_d.push_back(s.b_d);
        b_e.push_back(s.b_e);
        peak_e.push_back(s.peak_e);
        per_vertex.push_back((s.b_b - s.b_a) / kN);
        blk_a.push_back(s.blk_a);
        blk_b.push_back(s.blk_b);
        blk_c.push_back(s.blk_c);
        blk_d.push_back(s.blk_d);
        blk_e.push_back(s.blk_e);
    }
    /** @brief Print every column of this arm. */
    void emit(const char* arm) {
        report(arm, "S_A_bytes", b_a);
        report(arm, "S_B1_bytes", b_b1);
        report(arm, "S_B_bytes", b_b);
        report(arm, "S_C_bytes", b_c);
        report(arm, "S_D_bytes", b_d);
        report(arm, "S_E_bytes", b_e);
        report(arm, "S_E_peak", peak_e);
        report(arm, "per_vertex", per_vertex);
        report(arm, "per_vertex_id", per_vertex_id);
        report(arm, "per_vertex_val", per_vertex_val);
        report(arm, "S_A_blocks", blk_a);
        report(arm, "S_B1_blocks", blk_b1);
        report(arm, "S_B_blocks", blk_b);
        report(arm, "S_C_blocks", blk_c);
        report(arm, "S_D_blocks", blk_d);
        report(arm, "S_E_blocks", blk_e);
    }
};

}  // namespace

/**
 * @brief Fork the peer, then run every arm `reps` times interleaved and report.
 *
 * `--reps=N` repetitions (the FIRST is discarded), `--ops=N` remote operations in the
 * S_E storm, `--arm=mixed|small|large` to restrict the arms.
 */
int main(int argc, char** argv) {
    std::size_t reps = 6;
    std::size_t ops = 1000;
    std::string only;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if (a.starts_with("--reps="))
            reps = std::strtoul(argv[i] + 7, nullptr, 10);
        else if (a.starts_with("--ops="))
            ops = std::strtoul(argv[i] + 6, nullptr, 10);
        else if (a.starts_with("--arm="))
            only = argv[i] + 6;
    }
    if (reps < 2) reps = 2;

    // The fork happens HERE — before any graph, socket or thread exists in this
    // process. Forking a threaded process under a counting allocator can deadlock in
    // the child on a malloc lock another thread held at fork time.
    int to_peer[2];
    int from_peer[2];
    if (::pipe(to_peer) != 0 || ::pipe(from_peer) != 0) {
        std::fprintf(stderr, "pipe failed\n");
        return 1;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        std::fprintf(stderr, "fork failed\n");
        return 1;
    }
    if (pid == 0) {
        ::close(to_peer[1]);
        ::close(from_peer[0]);
        const int rc = peer_main(to_peer[0], from_peer[1]);
        ::close(to_peer[0]);
        ::close(from_peer[1]);
        std::_Exit(rc);
    }
    ::close(to_peer[0]);
    ::close(from_peer[1]);
    const int cmd_fd = to_peer[1];
    const int rsp_fd = from_peer[0];

    std::printf("# bench_ram_census_tcp vertices=%zu ops=%zu reps=%zu (first discarded)\n",
                kVertices, ops, reps);
    std::printf("# transport: tr::net::transport_tcp_server (kind=tcp, LISTEN) — the canonical\n");
    std::printf("# TCP transport, constructed in-band via write /net/tcp-server/conn <- SPEC\n");
    std::printf("# sizeof: graph_t=%zu vertex_t=%zu tcp_server=%zu transport_vertex_t=%zu\n",
                sizeof(tr::graph::graph_t), sizeof(tr::graph::vertex_t),
                sizeof(tr::net::transport_tcp_server), sizeof(tr::net::transport_vertex_t));
    std::fflush(stdout);

    series_t mixed, small, large;
    std::vector<long long> nulls;
    mixed.reserve(reps);
    small.reserve(reps);
    large.reserve(reps);
    nulls.reserve(reps);

    int failures = 0;
    g_armed.store(true, std::memory_order_seq_cst);
    for (std::size_t r = 0; r < reps; ++r) {
        const bool keep = r != 0;  // discard the first execution
        const long long drift = arm_null();
        if (keep) nulls.push_back(drift);
        if (only.empty() || only == "mixed") {
            const sample_t s = run_census(mix_t::MIXED, ops, cmd_fd, rsp_fd);
            if (!s.ok)
                ++failures;
            else if (keep)
                mixed.push(s);
        }
        if (only.empty() || only == "small") {
            const sample_t s = run_census(mix_t::SMALL, ops, cmd_fd, rsp_fd);
            if (!s.ok)
                ++failures;
            else if (keep)
                small.push(s);
        }
        if (only.empty() || only == "large") {
            const sample_t s = run_census(mix_t::LARGE, ops, cmd_fd, rsp_fd);
            if (!s.ok)
                ++failures;
            else if (keep)
                large.push(s);
        }
    }
    g_armed.store(false, std::memory_order_seq_cst);

    (void)write_all(cmd_fd, "QUIT\n", 5);
    ::close(cmd_fd);
    int wstatus = 0;
    (void)::waitpid(pid, &wstatus, 0);
    ::close(rsp_fd);

    report("null", "drift", nulls);
    mixed.emit("mixed");
    small.emit("small-4b");
    large.emit("large-64b");
    std::printf("# residual_live=%lld residual_blocks=%lld failures=%d\n", live(), blocks(),
                failures);
    return failures == 0 ? 0 : 1;
}
