/**
 * @file
 * @brief #1485 — the vertex-count scaling sweep, built to **DECOMPOSE** the topic-count growth
 *        the #1480 fairness audit measured rather than to reproduce it.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * @section what The curve this exists to explain
 *
 * The zenoh fairness audit measured libtracer's `inproc-path` p50 moving **140 -> 190 ns
 * (+36 %) across 1 -> 8192 topics** against Zenoh's 220 -> 230 ns (+5 %). That is a curve with
 * no explanation attached, and a sweep that reproduces it adds nothing. So this binary does not
 * publish "cost at N topics"; it publishes the **candidate legs measured in isolation**, so the
 * growth can be attributed to one of them and classified:
 *
 *  - **bucket 1 — mitigable**: an implementation cost a numbers-driven change could remove;
 *  - **bucket 2 — spec cost**: a consequence of the protocol as specified;
 *  - **bucket 3 — the designated residency wall**: every distinct address is resident state,
 *    which is what buys LKV / `await` / composed reads / vertex-ACL. A designed trade.
 *
 * @section legs The four decomposition legs
 *
 * Every leg runs against the SAME population of N registered vertices, in ONE process, so no
 * cross-build layout sensitivity (up to +9.8 % on an untouched leg on this host — see
 * bench/README.md) can be mistaken for a scaling effect.
 *
 *  1. **`scale-store` / `scale-deliver`** — a write through a **pre-bound
 *     @ref tr::graph::vertex_handle_t**, touching all N vertices in a shuffled order. No
 *     resolution of any kind happens inside the timed window. Growth here is cache/locality of
 *     *vertex state* and nothing else: this is the leg that would indicate the residency wall.
 *     `-store` is fan-0 (the write and its LKV publish alone); `-deliver` is fan-1, which is the
 *     shape the audit's `inproc-path` row measured.
 *  2. **`scale-resolve`** — `find(key)` alone, over the same N addresses in the same shuffled
 *     order, with **no write at all**. This is the by-address resolution a caller pays *only if
 *     it re-resolves per operation*. `scale-write-key` is the composed `find` + bound write, and
 *     it exists so `resolve + store ~= write-key` can be checked rather than assumed. If the
 *     growth lives here, the audit's row was measuring a **benchmark shape**, not a product
 *     cost — an application binds once — and it must be labelled as such.
 *  3. **`scale-find-fixed`** — `find()` against a probe address of **fixed depth and fixed
 *     per-level fan-out**, sitting in a subtree whose shape does not change as N grows. Per
 *     ADR-0057 (one segment per node, per-level `lower_bound` over sorted children) the descent
 *     work for this address is *identical* at every N, so this row **should be flat**. It is
 *     measured rather than assumed; a non-flat result is itself the finding, and would say the
 *     cost is whole-graph state pressure rather than descent. A `-cold` twin pollutes the cache
 *     with a bulk walk **outside** the timed bracket before each sample.
 *  4. **`scale-store-one` / `scale-resolve-one`** — the **working-set control**: the same N
 *     resident vertices, but only **one** of them ever touched. This is the decisive arm. If
 *     growth tracks the resident set with a touched set of 1, the cost is *residency*; if it
 *     appears only when the touched set grows, it is cache behaviour of the *traffic*, and the
 *     model is not what is paying.
 *
 * @section body The measurements the issue body asks for, beside the decomposition
 *
 *  - `scale-register` — @ref tr::graph::graph_t::register_vertex_key descent, sampled over the
 *    **tail of the population build itself** (see @ref build) so it is the marginal cost at
 *    population ~N and leaves no vertices behind that would perturb the other arms.
 *  - `scale-vertex-slot` — @ref tr::graph::graph_t::vertex_slot, the one *known* O(total
 *    vertices) leg (`core/src/graph.cpp:849`, a `std::deque<vertex_t*>` chunk-walk under a
 *    shared `map_mutex_` hold). Mint-time only. #1486 holds the ruling on memoizing it; this
 *    only puts the number on it and deliberately proposes nothing.
 *  - `scale-enumerate` — @ref tr::graph::graph_t::for_each_vertex, documented control-plane-only.
 *  - `RESULT_SCALE_RAM` — **measured** resident bytes per vertex (glibc `mallinfo2` live-balance
 *    delta plus process RSS delta across the registration bracket), never the ~96 B + slot +
 *    parent + header figure computed from `sizeof`.
 *
 * @section discipline Bench discipline
 *
 * **Best-of-rounds, never median-of-rounds** — contamination on this host is one-sided, and a
 * median-of-rounds reduction once turned an instrument that agreed with itself to +/-0.34 % into
 * one reading -33 %..+54 % on unchanged code. Every arm keeps `min p50`, `min mean` and `max
 * throughput` independently across rounds, which is the same per-metric rule `best_of_rounds.py`
 * and `perf_gate.py` use.
 *
 * **Arm order is reversed on alternate rounds.** An always-same-first ordering manufactured an
 * apparent 12.55-vs-8.07 M/s win on this host that vanished on the flip, so the order is flipped
 * inside the binary and the best of each arm is taken across both. An arm whose two orders
 * disagree on the sign of a trend is unresolved and no verdict may be drawn from it.
 *
 * **No point here is gated.** These arms are new and their run-to-run stability is unproven, and
 * `perf_gate.py`'s `POINTS` is a promise about stability, not a wish. Registering one would also
 * require the matching `docs/methodology.md` and `bench/gen_results_page.py` entries in the same
 * commit (`PointsAreDocumented`). This binary is diagnostic.
 *
 * @section env Knobs
 *
 * | env | default | meaning |
 * | --- | --- | --- |
 * | `SCALE_LADDER` | `1000,10000,100000,1000000` | the population ladder |
 * | `SCALE_ROUNDS` | `3` | rounds per population; best-of is taken over them |
 * | `SCALE_SUB_MAX_N` | `100000` | largest N at which the fan-1 `-deliver` arm is built |
 * | `SCALE_OPS` | `200000` | floor on timed operations per latency arm |
 *
 * `SCALE_SUB_MAX_N` exists because the fan-1 arm needs an edge block **per vertex**, and the
 * largest population in any other bench in this tree today is **100**. Where an arm is skipped
 * the binary says so on stderr and emits nothing, rather than emitting a row that silently
 * describes a different topology.
 */
#include <malloc.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/packed_path.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/tracer.hpp"

using namespace bench;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::view::view_t;

namespace {

/** @brief Payload size every arm writes — @ref bench::kRefSize, so the rows read against the
 *         audit's `inproc-path` row at the same payload. */
constexpr std::size_t kPayload = kRefSize;

/** @brief Hard ceiling on a timed arm's operation count, so N=1e6 stays inside a sane window. */
constexpr std::size_t kMaxArmOps = 2'000'000;

/** @brief Ceiling on how many tail registrations the build samples for `scale-register`. */
constexpr std::size_t kRegisterSamples = 20000;

/** @brief How many `vertex_slot` samples to take — the scan is O(N), so this is kept small. */
constexpr std::size_t kSlotSamples = 200;

/** @brief Probe leaves in the fixed-shape subtree used by the ADR-0057 flatness arm. */
constexpr std::size_t kProbeLeaves = 8;

/** @brief Bulk finds performed between two cold-arm samples, to evict the probe subtree. */
constexpr std::size_t kColdPollution = 64;

/** @brief Deterministic xorshift64* — the shuffle and the probe order must not vary by round. */
class rng_t {
   public:
    /** @brief Seed the generator; 0 is replaced, since xorshift has a fixed point at 0. */
    explicit rng_t(std::uint64_t seed) : s_(seed != 0 ? seed : 0x9E3779B97F4A7C15ULL) {}

    /** @brief Next raw 64-bit draw. */
    std::uint64_t next() {
        s_ ^= s_ >> 12;
        s_ ^= s_ << 25;
        s_ ^= s_ >> 27;
        return s_ * 0x2545F4914F6CDD1DULL;
    }

    /** @brief A draw in `[0, n)` (modulo bias is irrelevant to a cache-pollution walk). */
    std::size_t below(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(next() % n); }

   private:
    std::uint64_t s_;
};

/** @brief A VALUE TLV carrying @p payload bytes — the same body `bench_libtracer` writes. */
std::vector<std::byte> value_tlv(std::size_t payload) {
    const std::vector<std::byte> p(payload, std::byte{0xAB});
    tr::wire::tlv_t t{};
    t.type = tr::wire::type_t::VALUE;
    t.payload = p;
    return tr::wire::encode(t);
}

/** @brief Per-write owned heap view — the allocating path, matching `bench_libtracer`'s `inproc`
 *         rather than its `-borrow` twin, because the audit's row was the allocating one. */
view_t owned_view(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/**
 * @brief Build a canonical PATH-payload key — the concatenated NAME records `find` and
 *        `register_vertex_key` take, with no `path_t` parse anywhere near a timed window.
 */
std::vector<std::byte> make_key(std::span<const std::string> segments) {
    std::vector<std::byte> key;
    for (const std::string& s : segments)
        if (!tr::wire::emit_path_segment(key, std::string_view(s))) return {};
    return key;
}

/** @brief `/scale/g<a>/h<b>/v<c>` — the bulk population's key, depth 4 at every N. */
std::vector<std::byte> bulk_key(std::size_t a, std::size_t b, std::size_t c) {
    const std::string segs[] = {"scale", "g" + std::to_string(a), "h" + std::to_string(b),
                                "v" + std::to_string(c)};
    return make_key(std::span<const std::string>(segs, 4));
}

/** @brief The spelled form of @ref bulk_key — needed only by the setup-time `subscribe` sugar,
 *         which has no key-taking or handle-taking twin. */
std::string bulk_path(std::size_t a, std::size_t b, std::size_t c) {
    return "/scale/g" + std::to_string(a) + "/h" + std::to_string(b) + "/v" + std::to_string(c);
}

/** @brief `/probe/pa/pb/p<j>` — the FIXED-shape address the flatness arm resolves. Its descent
 *         visits 2 root children, then 1, then 1, then @ref kProbeLeaves, at every N. */
std::vector<std::byte> probe_key(std::size_t j) {
    const std::string segs[] = {"probe", "pa", "pb", "p" + std::to_string(j)};
    return make_key(std::span<const std::string>(segs, 4));
}

/** @brief Integer cube root, rounded up — the per-level fan-out that reaches N in three levels. */
std::size_t cube_fanout(std::size_t n) {
    std::size_t f = 1;
    while (f * f * f < n) ++f;
    return f;
}

/**
 * @brief glibc's live allocated-bytes balance (`uordblks`).
 *
 * Deliberately NOT an `operator new` interposer: that is what `bench_ram_census_tcp` and
 * `bench_store_escape` use, and it charges a relaxed atomic to every allocation in the process —
 * including the one allocation each timed write makes. Keeping the memory census out of the
 * allocator keeps the latency arms in this same binary honest.
 */
long long live_heap_bytes() {
#if defined(__GLIBC__)
    const struct mallinfo2 mi = mallinfo2();
    return static_cast<long long>(mi.uordblks);
#else
    return 0;
#endif
}

/**
 * @brief Process RSS in bytes, from `/proc/self/statm` — the figure `mallinfo2` cannot give,
 *        because bytes handed out and pages actually touched are not the same quantity.
 *
 * @warning An RSS **delta** is only trustworthy for the FIRST population a process builds. The
 *          previous population's pages are returned to the allocator, not to the kernel, so the
 *          next build satisfies itself out of pages the process already holds and the delta
 *          reads near zero — which is a fact about `malloc`'s arenas, not about a vertex. Run
 *          one population per process (`run_scale_sweep.sh` does exactly that) when the RSS
 *          column is the one being read; `heap_per_vertex` is valid at every position.
 */
long long rss_bytes() {
    std::FILE* f = std::fopen("/proc/self/statm", "re");
    if (f == nullptr) return 0;
    long long total = 0;
    long long resident = 0;
    const int got = std::fscanf(f, "%lld %lld", &total, &resident);
    (void)std::fclose(f);
    if (got != 2) return 0;
    return resident * static_cast<long long>(::sysconf(_SC_PAGESIZE));
}

/**
 * @brief The fan-1 arm's delivery sink, at namespace scope because @ref
 *        tr::graph::graph_t::subscribe binds a callable **by address** and a temporary (or a
 *        local in @ref build) would dangle the moment the population outlived its builder.
 *
 * One object serves every subscription: what the fan-1 arm prices is the graph's dispatch to an
 * edge, not the body of a user handler, and N distinct empty functors would only add N
 * allocations to a population already being measured for its footprint.
 */
struct sink_t {
    /** @brief Consume one delivery and do nothing measurable with it. */
    void operator()(const tr::view::rope_t&) const noexcept {}
};
sink_t g_sink; /**< @brief The single, address-stable delivery sink. */

/** @brief One arm's best-of-rounds accumulator — per METRIC, matching `best_of_rounds.py`. */
struct best_t {
    std::string mode;       /**< @brief The `mode` column; a row is self-describing. */
    std::size_t fanout = 0; /**< @brief Subscribers per vertex on this arm. */
    Latency::Summary lat{}; /**< @brief Best-of latency across rounds. */
    double ops_s = 0.0;     /**< @brief Best-of throughput across rounds. */
    bool seen = false;      /**< @brief False if the arm never ran (skipped at this N). */

    /** @brief Fold one round in: the LOWEST latency and the HIGHEST throughput survive. */
    void fold(const Latency::Summary& s, double ops) {
        if (!seen) {
            lat = s;
            ops_s = ops;
            seen = true;
            return;
        }
        lat.p50 = std::min(lat.p50, s.p50);
        lat.p99 = std::min(lat.p99, s.p99);
        lat.mean = std::min(lat.mean, s.mean);
        lat.p999 = std::min(lat.p999, s.p999);
        lat.max = std::min(lat.max, s.max);
        lat.n = std::max(lat.n, s.n);
        lat.tail_ok = lat.n >= kTailSampleFloor;
        ops_s = std::max(ops_s, ops);
    }
};

/** @brief The whole measurable state for one population size. */
struct population_t {
    std::size_t n = 0;                          /**< @brief Registered bulk vertices. */
    std::size_t fanout = 0;                     /**< @brief Per-level bulk fan-out (3 levels). */
    graph_t g{};                                /**< @brief The graph under test. */
    std::vector<std::vector<std::byte>> keys;   /**< @brief Bulk keys, in registration order. */
    std::vector<vertex_handle_t> handles;       /**< @brief Bulk handles, parallel to `keys`. */
    std::vector<std::size_t> order;             /**< @brief A fixed shuffle of `[0, n)`. */
    std::vector<std::vector<std::byte>> probes; /**< @brief The fixed-shape probe keys. */
    bool subscribed = false;                    /**< @brief Whether `-deliver` may run. */
    Latency::Summary reg{};                     /**< @brief Tail-of-build registration latency. */
    long long heap_total = 0;                   /**< @brief `mallinfo2` delta across the build. */
    long long rss_total = 0;                    /**< @brief RSS delta across the build. */
    long long heap_per_vertex = 0;              /**< @brief `mallinfo2` delta / n. */
    long long rss_per_vertex = 0;               /**< @brief RSS delta / n. */
};

/**
 * @brief Time @p op over @p ops iterations, returning per-op latency samples and a bulk rate.
 *
 * The bulk rate and the latency come from the SAME loop rather than two: at these per-op costs a
 * second, differently-shaped loop would measure a second workload, and the two numbers would not
 * describe the same thing. The clock's own cost (~22 ns per sample on this host) is charged to
 * every sample, so these rows are read for their MOVEMENT across N, never as absolutes against a
 * batch-amortized row.
 */
template <typename Op>
std::pair<Latency::Summary, double> time_arm(Op&& op, std::size_t ops) {
    Latency lat;
    lat.reserve(ops);
    const std::uint64_t t0 = now_ns();
    for (std::size_t i = 0; i < ops; ++i) {
        const std::uint64_t a = now_ns();
        op(i);
        lat.add(now_ns() - a);
    }
    const double secs = static_cast<double>(now_ns() - t0) / 1e9;
    return {lat.summarize(), secs > 0.0 ? static_cast<double>(ops) / secs : 0.0};
}

/** @brief Emit one decomposition row. `endpoints` carries N — the axis this whole file sweeps. */
void emit_scale(const best_t& b, std::size_t n) {
    emit("libtracer", b.mode.c_str(), kPayload, b.fanout, n, b.ops_s, b.ops_s,
         b.ops_s * static_cast<double>(kPayload) / 1e6, b.lat);
    emit_tail("libtracer", b.mode.c_str(), kPayload, b.fanout, n, b.lat);
}

/** @brief Read a comma-separated population ladder out of `SCALE_LADDER`. */
std::vector<std::size_t> ladder_from_env() {
    const char* raw = std::getenv("SCALE_LADDER");
    if (raw == nullptr) return {1000, 10000, 100000, 1000000};
    std::vector<std::size_t> out;
    std::string_view s(raw);
    while (!s.empty()) {
        const std::size_t comma = s.find(',');
        const std::string tok{s.substr(0, comma)};
        if (!tok.empty())
            out.push_back(static_cast<std::size_t>(std::strtoull(tok.c_str(), nullptr, 10)));
        if (comma == std::string_view::npos) break;
        s.remove_prefix(comma + 1);
    }
    if (out.empty()) out.push_back(1000);
    return out;
}

/** @brief Read a positive integer knob out of the environment, or @p fallback. */
std::size_t env_size(const char* name, std::size_t fallback) {
    const char* raw = std::getenv(name);
    if (raw == nullptr) return fallback;
    const unsigned long long v = std::strtoull(raw, nullptr, 10);
    return v == 0 ? fallback : static_cast<std::size_t>(v);
}

/**
 * @brief Build the population: N bulk vertices, the fixed-shape probe subtree, and (optionally)
 *        one subscriber per bulk vertex.
 *
 * @section ram Why the RAM bracket sits where it does
 *
 * It is taken around the **registration loop only**. Every key the harness will hold is built and
 * stored BEFORE the baseline reading, so the delta is graph-side bytes and not the harness's own
 * `std::vector<std::vector<std::byte>>`. That distinction is the whole point of measuring rather
 * than computing: a `sizeof`-derived figure cannot see the child vectors, the key copies, the
 * slot deque or the allocator's own size-class rounding, and that is where the bytes are.
 *
 * @section reg Why `scale-register` is sampled HERE and not as a round-robin arm
 *
 * Registration is the one measurement that MUTATES the population it is measuring. A round-robin
 * arm minting even 2000 probe vertices would triple the N=1000 population before the other arms
 * ran, so the sweep's own axis would move under it. Sampling the **tail of the build** costs
 * nothing extra, leaves nothing behind, and is a strictly better estimator anyway: it yields up
 * to @ref kRegisterSamples samples at population ~N in one shot, where a best-of-3 round-robin
 * would have yielded three. The trade is that this arm gets no round flip — it is a single
 * observation window per N, and its p50/min are read as such.
 */
std::unique_ptr<population_t> build(std::size_t n, std::size_t sub_max_n) {
    auto p = std::make_unique<population_t>();
    p->n = n;
    p->fanout = cube_fanout(n);

    // Fixed-shape probe subtree first, so its descent never depends on what came after it.
    for (std::size_t j = 0; j < kProbeLeaves; ++j) {
        std::vector<std::byte> k = probe_key(j);
        if (k.empty() || !p->g.register_vertex_key(k, role_t::STORED_VALUE).has_value())
            return nullptr;
        p->probes.push_back(std::move(k));
    }

    const std::size_t f = p->fanout;
    p->keys.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        p->keys.push_back(bulk_key(i / (f * f), (i / f) % f, i % f));

    p->handles.reserve(n);
    p->order.resize(n);
    for (std::size_t i = 0; i < n; ++i) p->order[i] = i;
    rng_t rng{0xC0FFEEULL};
    for (std::size_t i = n; i > 1; --i) std::swap(p->order[i - 1], p->order[rng.below(i)]);

    // Sample the registration cost over the SECOND HALF of the build, capped: the question is
    // the marginal descent cost at a large population, and a tail of n/10 leaves only 100
    // samples at N=1000 — too few for a p50 to mean much next to the 20 000 the top of the
    // ladder gets.
    const std::size_t sample_from =
        n - std::min(n, std::max<std::size_t>(1, std::min(n / 2, kRegisterSamples)));
    Latency reg;
    reg.reserve(n - sample_from);

    const long long heap0 = live_heap_bytes();
    const long long rss0 = rss_bytes();
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t a = now_ns();
        auto r = p->g.register_vertex_key(p->keys[i], role_t::STORED_VALUE);
        const std::uint64_t took = now_ns() - a;
        if (!r.has_value()) {
            std::fprintf(stderr, "[scale] registration failed at i=%zu of N=%zu\n", i, n);
            return nullptr;
        }
        p->handles.push_back(*r);
        if (i >= sample_from) reg.add(took);
    }
    p->heap_total = live_heap_bytes() - heap0;
    p->rss_total = rss_bytes() - rss0;
    p->heap_per_vertex = p->heap_total / static_cast<long long>(n);
    p->rss_per_vertex = p->rss_total / static_cast<long long>(n);
    p->reg = reg.summarize();

    if (n <= sub_max_n) {
        for (std::size_t i = 0; i < n; ++i) {
            // `subscribe` is the path-taking sugar, so this pays one parse per vertex — setup
            // cost, outside every timed window. There is no key- or handle-taking local
            // subscribe door, which is why the fan-1 arm costs a parse per vertex to build.
            const auto path = path_t::parse(bulk_path(i / (f * f), (i / f) % f, i % f));
            if (!path.has_value()) return nullptr;
            const auto s = p->g.subscribe(*path, g_sink);
            if (!s.has_value()) {
                std::fprintf(stderr, "[scale] subscribe failed at i=%zu of N=%zu\n", i, n);
                return nullptr;
            }
        }
        p->subscribed = true;
    } else {
        std::fprintf(stderr,
                     "[scale] N=%zu > SCALE_SUB_MAX_N=%zu — the fan-1 `scale-deliver` arm is NOT "
                     "built at this population and emits nothing. This is a HARNESS limit (one "
                     "edge block per vertex), not a product limit; the fan-0 `scale-store` arm "
                     "covers the same axis without it.\n",
                     n, sub_max_n);
    }
    return p;
}

/** @brief The arms, as an index the round loop can walk forwards and backwards. */
enum class arm_t : std::size_t {
    STORE_WIDE = 0,  /**< @brief Leg 1: bound handle, touched set = N. */
    STORE_ONE,       /**< @brief Leg 4: bound handle, touched set = 1. */
    DELIVER_WIDE,    /**< @brief Leg 1 at fan-1 — the audit's topology. */
    DELIVER_ONE,     /**< @brief Leg 4 at fan-1. */
    RESOLVE_WIDE,    /**< @brief Leg 2: `find()` only, touched set = N. */
    RESOLVE_ONE,     /**< @brief Leg 4 applied to resolution. */
    WRITE_KEY,       /**< @brief Leg 2 composed: `find()` + bound write. */
    FIND_FIXED,      /**< @brief Leg 3: ADR-0057 flatness, warm. */
    FIND_FIXED_COLD, /**< @brief Leg 3: same address, cache polluted between samples. */
    VERTEX_SLOT,     /**< @brief The known O(N) reverse scan. */
    ENUMERATE,       /**< @brief `for_each_vertex`, control-plane only. */
    COUNT            /**< @brief Arm count; never an arm. */
};

/** @brief The `mode` column for @p a. */
const char* arm_name(arm_t a) {
    switch (a) {
        case arm_t::STORE_WIDE:
            return "scale-store";
        case arm_t::STORE_ONE:
            return "scale-store-one";
        case arm_t::DELIVER_WIDE:
            return "scale-deliver";
        case arm_t::DELIVER_ONE:
            return "scale-deliver-one";
        case arm_t::RESOLVE_WIDE:
            return "scale-resolve";
        case arm_t::RESOLVE_ONE:
            return "scale-resolve-one";
        case arm_t::WRITE_KEY:
            return "scale-write-key";
        case arm_t::FIND_FIXED:
            return "scale-find-fixed";
        case arm_t::FIND_FIXED_COLD:
            return "scale-find-fixed-cold";
        case arm_t::VERTEX_SLOT:
            return "scale-vertex-slot";
        case arm_t::ENUMERATE:
            return "scale-enumerate";
        case arm_t::COUNT:
            break;
    }
    return "scale-unknown";
}

/** @brief Subscribers per vertex on arm @p a — the `fanout` column. */
std::size_t arm_fanout(arm_t a) {
    return (a == arm_t::DELIVER_WIDE || a == arm_t::DELIVER_ONE) ? 1 : 0;
}

/** @brief Run arm @p a once over @p p and fold the observation into @p best. */
void run_arm(arm_t a, population_t& p, const std::vector<std::byte>& tlv, std::size_t ops,
             best_t& best) {
    const std::size_t n = p.n;
    const std::vector<std::size_t>& ord = p.order;
    const std::size_t one = ord.empty() ? 0 : ord[0];

    switch (a) {
        case arm_t::STORE_WIDE:
        case arm_t::DELIVER_WIDE: {
            if (a == arm_t::DELIVER_WIDE && !p.subscribed) return;
            const auto [lat, rate] = time_arm(
                [&](std::size_t i) { (void)p.g.write(p.handles[ord[i % n]], owned_view(tlv)); },
                ops);
            best.fold(lat, rate);
            return;
        }
        case arm_t::STORE_ONE:
        case arm_t::DELIVER_ONE: {
            if (a == arm_t::DELIVER_ONE && !p.subscribed) return;
            const auto [lat, rate] = time_arm(
                [&](std::size_t) { (void)p.g.write(p.handles[one], owned_view(tlv)); }, ops);
            best.fold(lat, rate);
            return;
        }
        case arm_t::RESOLVE_WIDE: {
            const auto [lat, rate] = time_arm(
                [&](std::size_t i) {
                    if (!p.g.find(p.keys[ord[i % n]]).has_value()) std::abort();
                },
                ops);
            best.fold(lat, rate);
            return;
        }
        case arm_t::RESOLVE_ONE: {
            const auto [lat, rate] = time_arm(
                [&](std::size_t) {
                    if (!p.g.find(p.keys[one]).has_value()) std::abort();
                },
                ops);
            best.fold(lat, rate);
            return;
        }
        case arm_t::WRITE_KEY: {
            const auto [lat, rate] = time_arm(
                [&](std::size_t i) {
                    const auto v = p.g.find(p.keys[ord[i % n]]);
                    if (!v.has_value()) std::abort();
                    (void)p.g.write(*v, owned_view(tlv));
                },
                ops);
            best.fold(lat, rate);
            return;
        }
        case arm_t::FIND_FIXED: {
            const auto [lat, rate] = time_arm(
                [&](std::size_t i) {
                    if (!p.g.find(p.probes[i % kProbeLeaves]).has_value()) std::abort();
                },
                ops);
            best.fold(lat, rate);
            return;
        }
        case arm_t::FIND_FIXED_COLD: {
            // The pollution walk is OUTSIDE the bracket: what is timed is one descent over a
            // probe subtree whose own lines were just evicted by traffic to the bulk population.
            // Fewer samples, because each one costs @ref kColdPollution bulk descents.
            rng_t rng{0xBADC0DEULL};
            const std::size_t cold_ops = std::max<std::size_t>(2000, ops / 20);
            Latency lat;
            lat.reserve(cold_ops);
            std::size_t sink = 0;
            const std::uint64_t t0 = now_ns();
            for (std::size_t i = 0; i < cold_ops; ++i) {
                for (std::size_t k = 0; k < kColdPollution; ++k)
                    sink += p.g.find(p.keys[rng.below(n)]).has_value() ? 1U : 0U;
                const std::uint64_t s = now_ns();
                const bool ok = p.g.find(p.probes[i % kProbeLeaves]).has_value();
                lat.add(now_ns() - s);
                if (!ok) std::abort();
            }
            const double secs = static_cast<double>(now_ns() - t0) / 1e9;
            if (sink == 0) std::abort();
            best.fold(lat.summarize(), secs > 0.0 ? static_cast<double>(cold_ops) / secs : 0.0);
            return;
        }
        case arm_t::VERTEX_SLOT: {
            rng_t rng{0x51075100ULL};
            Latency lat;
            lat.reserve(kSlotSamples);
            const std::uint64_t t0 = now_ns();
            for (std::size_t k = 0; k < kSlotSamples; ++k) {
                const vertex_handle_t h = p.handles[rng.below(n)];
                const std::uint64_t s = now_ns();
                const auto slot = p.g.vertex_slot(h);
                lat.add(now_ns() - s);
                if (!slot.has_value()) std::abort();
            }
            const double secs = static_cast<double>(now_ns() - t0) / 1e9;
            best.fold(lat.summarize(), secs > 0.0 ? static_cast<double>(kSlotSamples) / secs : 0.0);
            return;
        }
        case arm_t::ENUMERATE: {
            // ONE pass per round. It allocates an owned key per registered vertex and then
            // sorts, so at N=1e6 a single pass is seconds. The row publishes the per-VERTEX
            // figure: the whole pass divided by the number of vertices it visited.
            std::size_t visited = 0;
            const std::uint64_t s = now_ns();
            p.g.for_each_vertex([&](tr::wire::key_view_t, vertex_handle_t) { ++visited; });
            const std::uint64_t took = now_ns() - s;
            if (visited == 0) std::abort();
            Latency lat;
            lat.add(took / visited);
            const double secs = static_cast<double>(took) / 1e9;
            best.fold(lat.summarize(), secs > 0.0 ? static_cast<double>(visited) / secs : 0.0);
            return;
        }
        case arm_t::COUNT:
            return;
    }
}

/**
 * @brief Emit the measured memory census and the build-time registration row.
 *
 * `RESULT_SCALE_RAM` gets its own tag, deliberately. Every RESULT parser in the tree gates on
 * `f[0] == "RESULT"` plus a 12-field arity, and `perf_gate.py`'s memory regex is `^RESULT `
 * (with the space); `RESULT_SCALE_RAM` matches none of them, so a bytes row can never be
 * ingested as a throughput row.
 */
void emit_census(const population_t& p) {
    std::printf(
        "RESULT_SCALE_RAM\tn=%zu\tfanout=%zu\theap_total=%lld\theap_per_vertex=%lld\t"
        "rss_total=%lld\trss_per_vertex=%lld\tsubscribed=%d\n",
        p.n, p.fanout, p.heap_total, p.heap_per_vertex, p.rss_total, p.rss_per_vertex,
        p.subscribed ? 1 : 0);
    emit("libtracer", "scale-register", kPayload, 0, p.n, 0.0, 0.0, 0.0, p.reg);
    emit_tail("libtracer", "scale-register", kPayload, 0, p.n, p.reg);
    std::fflush(stdout);
}

}  // namespace

int main() {
    const std::vector<std::size_t> ladder = ladder_from_env();
    const std::size_t rounds = env_size("SCALE_ROUNDS", 3);
    const std::size_t sub_max_n = env_size("SCALE_SUB_MAX_N", 100000);
    const std::size_t ops_floor = env_size("SCALE_OPS", 200000);
    const std::vector<std::byte> tlv = value_tlv(kPayload);

    std::printf("NOTE bench_scale_sweep rounds=%zu ops_floor=%zu sub_max_n=%zu payload=%zu\n",
                rounds, ops_floor, sub_max_n, kPayload);
    std::fflush(stdout);

    for (const std::size_t n : ladder) {
        const std::unique_ptr<population_t> p = build(n, sub_max_n);
        if (!p) {
            std::fprintf(stderr, "[scale] N=%zu could not be built — skipped\n", n);
            continue;
        }
        emit_census(*p);

        // The wide arms must actually touch all N vertices, or "touched set = N" is a claim the
        // loop does not honour: at N=1e6 a 200k-op arm touches a fifth of the population and
        // would understate exactly the effect being hunted. Capped so the window stays sane.
        const std::size_t ops = std::min(kMaxArmOps, std::max(ops_floor, n));

        std::vector<best_t> best(static_cast<std::size_t>(arm_t::COUNT));
        for (std::size_t a = 0; a < best.size(); ++a) {
            best[a].mode = arm_name(static_cast<arm_t>(a));
            best[a].fanout = arm_fanout(static_cast<arm_t>(a));
        }

        // Warm every arm once, unrecorded, so no arm pays another arm's first-touch faults.
        // ENUMERATE is excluded: its pass is seconds at the top of the ladder and it allocates
        // a key per vertex, so a warmup would cost more than the measurement it protects.
        for (std::size_t a = 0; a < best.size(); ++a) {
            if (static_cast<arm_t>(a) == arm_t::ENUMERATE) continue;
            best_t scratch;
            scratch.mode = best[a].mode;
            run_arm(static_cast<arm_t>(a), *p, tlv, std::min<std::size_t>(ops, 5000), scratch);
        }

        for (std::size_t r = 0; r < rounds; ++r) {
            // ORDER FLIP. An always-same-first ordering manufactured an apparent 12.55-vs-8.07
            // M/s win on this host that vanished on the flip, so both orders are run and the
            // best of each arm is taken across them.
            const bool forward = (r % 2) == 0;
            for (std::size_t k = 0; k < best.size(); ++k) {
                const std::size_t a = forward ? k : best.size() - 1 - k;
                run_arm(static_cast<arm_t>(a), *p, tlv, ops, best[a]);
            }
        }

        for (const best_t& b : best)
            if (b.seen) emit_scale(b, n);
    }
    return 0;
}
