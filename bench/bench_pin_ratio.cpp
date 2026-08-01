/**
 * @file
 * @brief RFC-0022 §6 — the WRITE store leg, copy versus pinned subview, over a
 *        (payload_bytes x segment_bytes) grid, with every arm interleaved inside one process.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * §6 refuses to let §3.D's on-by-default pinning land on an assertion, and §8 Q3 leaves
 * `kPinPayloadRatio`'s value to this measurement rather than to argument. This is the host
 * half's store-leg microbench; `bench_pin_net` is the two-process delivery-counted half.
 *
 * @section pin_bench_unit What one timed sample is
 *
 * `op_resolver_t::resolve` over a pre-decoded arena and an OWNING frame view — the whole
 * resolver leg of one FWD{WRITE}, of which the store branch is the only thing that differs
 * between arms. Frame construction, segment allocation and the TLV arena decode sit OUTSIDE
 * the timed window because a real transport pays them identically in every arm; the pinned
 * arm's deferred cost (the segment it keeps alive is not returned to the allocator until the
 * next write displaces the value) lands inside the NEXT sample's window, which is where
 * steady state actually charges it.
 *
 * @section pin_bench_grid Why the grid spans absolute size and not only ratio
 *
 * ADR-0041 §Brick-2 and ADR-0042 §3 already measured that a copy beats pinning under a few
 * hundred bytes even at amplification ~1 — `view::borrow`'s ~32 B control block against a
 * ~30 B `memcpy`. §3.D's predicate is PURE RATIO, so it pins those cells regardless. The grid
 * therefore includes small-absolute cells specifically so that contradiction is measured
 * rather than inherited.
 *
 * @section pin_bench_reach The reachability instrument, and how to break its line
 *
 * Pinning needs an owning, view-delivered frame AND a trailer-less opt byte; an arm that
 * satisfies neither reports a clean "no regression" on nothing (the `fold-b4` lesson). Each
 * cell therefore reports `pins`/`copies` counted TWO independent ways: segment-pointer
 * identity between the stored value and the frame — an OUTCOME, and the only instrument
 * available on an untouched-main control binary — and, when built with
 * `LIBTRACER_PIN_INSTRUMENT`, the decision site's own counters. `--calibrate` breaks the line
 * on purpose: it drives a CRC-trailered payload and a borrowed (span-delivered) frame through
 * the arm that pins everything and requires zero pins from both instruments, so an inert
 * instrument fails loudly before any cell is believed.
 *
 * @section pin_bench_arms The arms, and why they share one process
 *
 * Arm A is a build of THIS SOURCE against untouched `origin/main` — a separate binary, because
 * that is what "control" means. Arms B (sentinel), C (pin-always) and D<K> (the ratio sweep)
 * are this binary, rotating per round, because measuring them as separate invocations is what
 * produced the recorded 2.8x swing on identical code. The per-vertex `store_ref_min_bytes`
 * u32 carries K, which is also why arm A compiles: on main the same u32 is the old absolute
 * threshold, and 0 means "copy" under both predicates.
 *
 * Usage:
 *   bench_pin_ratio --rounds=N [--arms=B,C,D2,D4,D8,D64,D1024] [--calibrate] [--round0=i]
 */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bench_common.hpp"
#if __has_include("libtracer/pin_instrument.hpp")
#include "libtracer/pin_instrument.hpp"
/**
 * @brief This build's decision site evaluates RFC-0022 §3.D's ratio predicate.
 *
 * Absent, it is untouched `origin/main` and the u32 is the OLD absolute `store_ref_min_bytes`
 * threshold. The two read the same field with opposite meanings, so the calibration set has to
 * fork on it — running main's binary against §3.D's expectations would report five loud
 * failures about nothing.
 */
#define BENCH_HAS_RATIO_PREDICATE 1
#else
// Arm A builds this same source against untouched origin/main, where the decision site and
// its counters do not exist. The segment-pointer-identity instrument does, which is exactly
// why the control arm was given an OUTCOME instrument and not a counter.
#define LIBTRACER_TICK_PIN() ((void)0)
#endif
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::op_resolver_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::wire::opt_t;
using tr::wire::type_t;

// --- wire builders (canonical bytes via the production emit helpers) ---------

/** @brief A NAME TLV. */
std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, s);
    return out;
}

/** @brief A PATH TLV over `segs`. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) {
        const std::vector<std::byte> n = b_name(s);
        body.insert(body.end(), n.begin(), n.end());
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

/** @brief A one-byte VALUE TLV (the FWD's leading op discriminant). */
std::vector<std::byte> b_u8_value(std::uint8_t v) {
    const std::byte b{v};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}

/**
 * @brief A VALUE TLV carrying `n` payload bytes.
 *
 * @param crc Sets the CRC trailer bit, which makes the payload structurally unpinnable
 *            (ADR-0042 §3: a referenced store cannot patch the opt byte in a shared frame).
 *            The `--calibrate` line-break uses it.
 */
std::vector<std::byte> b_value(std::size_t n, bool crc = false) {
    std::vector<std::byte> p(n);
    for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<std::byte>(i & 0xFF);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{.cr = crc}, p);
    return out;
}

/** @brief Assemble a FWD{WRITE} frame targeting `/sensor/blob` (RFC-0004 §B child order). */
std::vector<std::byte> b_fwd_write(const std::vector<std::byte>& payload) {
    std::vector<std::byte> body;
    const auto app = [&body](const std::vector<std::byte>& s) {
        body.insert(body.end(), s.begin(), s.end());
    };
    app(b_u8_value(static_cast<std::uint8_t>(fwd_op_t::WRITE)));
    app(b_path({"sensor", "blob"}));
    app(b_path({"reply-ep"}));
    app(payload);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

/**
 * @brief A view over a FRESH owned heap segment of exactly `segment_bytes`, whose first
 *        `frame.size()` bytes are the frame — the shape a view-delivering transport hands up.
 *
 * The view is narrowed to the frame length, exactly as `udp_transport_t` narrows its
 * `kMaxDatagram` RX segment with `subview(0, n)`. That gap between the delivered LENGTH and
 * the allocated SEGMENT is the whole subject of §3.D, so the bench must be able to open it.
 */
tr::view::view_t frame_view_over(std::span<const std::byte> frame, std::size_t segment_bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(segment_bytes);
    std::memcpy(seg->bytes.data(), frame.data(), frame.size());
    return tr::view::view_t::over(std::move(seg)).subview(0, frame.size());
}

/**
 * @brief RFC-0022 §3.D's reserved sentinel: never pin.
 *
 * Spelled locally rather than as `tr::graph::kPinNever` so this file compiles unchanged
 * against untouched `origin/main` as the control arm. 0 means "copy" under BOTH predicates —
 * main's `store_ref_min_bytes > 0` gate and §3.D's sentinel — which is what makes arm A and
 * arm B comparable at all.
 */
constexpr std::uint32_t kSentinel = 0;

/** @brief One arm: a label and the K it drives the decision site with. */
struct arm_t {
    const char* label;
    std::uint32_t k;
};

/** @brief One grid cell's outcome for one arm in one round. */
struct cell_result_t {
    bench::Latency::Summary lat{};
    std::uint64_t pins = 0;   /**< stores whose segment IS the frame's (outcome instrument) */
    std::uint64_t copies = 0; /**< stores whose segment is a fresh one */
#ifdef LIBTRACER_PIN_INSTRUMENT
    std::uint64_t site_pins = 0;    /**< the decision site's own pin counter */
    std::uint64_t site_copies = 0;  /**< ... and its copy counter */
    std::uint64_t site_refused = 0; /**< predicate said pin, reader could not */
#endif
};

/** @brief Iterations per cell — enough that the p50 is a distribution, cheap enough for 10+ rounds.
 */
constexpr std::size_t kItersPerCell = 20000;

/**
 * @brief Drive `kItersPerCell` FWD{WRITE} resolutions at one (payload, segment, K) point.
 *
 * @param borrowed Deliver the frame as a BORROWED span (no owning view) — the definitionally
 *                 inert negative control the `--calibrate` line-break needs.
 */
cell_result_t run_cell(std::size_t payload_bytes, std::size_t segment_bytes, std::uint32_t k,
                       bool crc = false, bool borrowed = false) {
    graph_t g;
    op_resolver_t resolver(g);
    tr::graph::settings_t s;
    s.store_ref_min_bytes = k;  // the arm's K (on origin/main: the old absolute threshold)
    const tr::graph::vertex_handle_t v =
        g.register_vertex(path_t("/sensor/blob"), role_t::STORED_VALUE, {}, s);

    const std::vector<std::byte> frame = b_fwd_write(b_value(payload_bytes, crc));
    const std::size_t seg_bytes = std::max(segment_bytes, frame.size());

#ifdef LIBTRACER_PIN_INSTRUMENT
    tr::graph::instrument::reset();
#endif
    cell_result_t out;
    out.lat.n = 0;
    bench::Latency lat;
    lat.reserve(kItersPerCell);

    for (std::size_t i = 0; i < kItersPerCell; ++i) {
        // Untimed: what a transport pays identically in every arm.
        tr::view::view_t fv = frame_view_over(frame, seg_bytes);
        const auto arena = tr::wire::decode_into(fv.bytes(), tr::mem::heap_source());
        if (!arena) continue;

        const std::uint64_t t0 = bench::now_ns();
        auto reply = resolver.resolve(*arena, {}, borrowed ? nullptr : &fv);
        const std::uint64_t t1 = bench::now_ns();
        lat.add(t1 - t0);
        if (!reply) continue;

        // The outcome instrument: did the store land ON the frame's segment?
        const auto rd = g.read(v);
        if (rd && (*rd)->link_count() == 1 && (*rd)->only().owner.get() == fv.owner.get())
            ++out.pins;
        else
            ++out.copies;
    }
    out.lat = lat.summarize();
#ifdef LIBTRACER_PIN_INSTRUMENT
    out.site_pins = tr::graph::instrument::g_pin_hits;
    out.site_copies = tr::graph::instrument::g_copy_hits;
    out.site_refused = tr::graph::instrument::g_pin_refused;
#endif
    return out;
}

/**
 * @brief The (payload, segment) grid.
 *
 * `segment = 0` means "exact fit" — the segment is the frame, amplification ~1, the
 * FWD-delivery shape ADR-0042 measured. The absolute segment sizes are the ones a real
 * transport imposes: 2,048 is a plausible constrained RX pool slot, and 65,536 is
 * `udp_transport_t::kMaxDatagram`, what the reference transport actually allocates per
 * datagram whatever its length.
 */
constexpr std::size_t kPayloads[] = {8, 32, 64, 256, 1024, 4096, 16384};
constexpr std::size_t kSegments[] = {0, 2048, 65536};

/** @brief `RESULT_PIN` — one arm at one grid cell in one round. Tab-separated, own tag. */
void emit_pin(int round, const char* arm, std::uint32_t k, std::size_t payload, std::size_t segment,
              const cell_result_t& r) {
    std::printf("RESULT_PIN\t%d\t%s\t%u\t%zu\t%zu\t%llu\t%llu\t%llu\t%llu\t%llu\t%zu", round, arm,
                k, payload, segment, static_cast<unsigned long long>(r.lat.p50),
                static_cast<unsigned long long>(r.lat.p99),
                static_cast<unsigned long long>(r.lat.mean),
                static_cast<unsigned long long>(r.pins), static_cast<unsigned long long>(r.copies),
                r.lat.n);
#ifdef LIBTRACER_PIN_INSTRUMENT
    std::printf("\t%llu\t%llu\t%llu", static_cast<unsigned long long>(r.site_pins),
                static_cast<unsigned long long>(r.site_copies),
                static_cast<unsigned long long>(r.site_refused));
#else
    std::printf("\t-\t-\t-");
#endif
    std::printf("\n");
    std::fflush(stdout);
}

/** @brief Every arm this binary knows; `--arms` selects a subset, in this order. */
const arm_t kAllArms[] = {
    {"A-control", kSentinel},
    {"B-sentinel", kSentinel},
    {"D2", 2},
    {"D4", 4},
    {"D8", 8},
    {"D64", 64},
    {"D1024", 1024},
    {"C-pin-always", 0xFFFFFFFFu},
};

/**
 * @brief Break the instrument's line before believing any cell.
 *
 * Forced-wrong branches through an arm that intends to pin — each must report ZERO pins — plus
 * a positive case that must report ALL pins, so "zero everywhere" cannot pass as calibration.
 * Run on every invocation, not once at authoring time.
 *
 * The set forks on @ref BENCH_HAS_RATIO_PREDICATE because the control binary is untouched
 * `origin/main`, whose predicate is the absolute threshold, not the ratio. Two assertions hold
 * in BOTH builds and are the ones arm A actually rests on: a trailer-less owning frame at
 * K = 0 copies every store, and no build ever pins a CRC-trailered or span-delivered payload.
 *
 * @return 0 on success; non-zero is a refusal to report any number from this binary.
 */
int calibrate() {
    int bad = 0;
    const auto expect = [&bad](const char* what, std::uint64_t got, std::uint64_t want) {
        const bool ok = got == want;
        std::printf("CALIBRATE\t%s\t%s\tgot=%llu\twant=%llu\n", ok ? "PASS" : "FAIL", what,
                    static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
        if (!ok) ++bad;
    };

    // --- true in every build: the sentinel copies, and the structural blockers block --------
    const cell_result_t sentinel = run_cell(1024, 0, kSentinel);
    expect("K=0 (sentinel / arm A+B) never pins", sentinel.pins, 0);
    expect("K=0 copies every store", sentinel.copies, sentinel.lat.n);

    // The arm that intends to pin everything, spelled for this build's predicate.
#ifdef BENCH_HAS_RATIO_PREDICATE
    constexpr std::uint32_t kPinsEverything = 0xFFFFFFFFu;  // ratio: payload * K >= any segment
#else
    constexpr std::uint32_t kPinsEverything = 1;  // absolute: payload >= 1 byte
#endif
    const cell_result_t pos = run_cell(1024, 0, kPinsEverything);
    expect("pin-intending arm on a trailer-less owning frame pins every store", pos.copies, 0);
    expect("... and its pin count is the sample count", pos.pins, pos.lat.n);

    const cell_result_t crc = run_cell(1024, 0, kPinsEverything, /*crc=*/true);
    expect("CRC-trailered payload never pins", crc.pins, 0);
    const cell_result_t borrowed = run_cell(1024, 0, kPinsEverything, false, /*borrowed=*/true);
    expect("span-delivered (borrowed) frame never pins", borrowed.pins, 0);

    // --- the predicate's own declining direction, which is where the two builds differ -----
#ifdef BENCH_HAS_RATIO_PREDICATE
    // Ratio: a 64 B payload cannot clear a 64 KB segment at K = 2 (64 * 2 << 65,536).
    expect("K=2 against a 64 KB segment never pins", run_cell(64, 65536, 2).pins, 0);
#else
    // Absolute: a 64 B payload cannot clear a 100,000-byte threshold, whatever the segment.
    expect("threshold=100000 against a 64 B payload never pins", run_cell(64, 65536, 100000).pins,
           0);
#endif

#ifdef LIBTRACER_PIN_INSTRUMENT
    expect("decision-site counter agrees with the outcome (positive)", pos.site_pins, pos.pins);
    expect("decision-site counter agrees with the outcome (CRC)", crc.site_pins, 0);
    // A borrowed frame has no owning segment, so `segment_bytes` answers 0 and the ratio
    // declines it BEFORE `pin_wire` is asked — it is a copy, not a refusal. `g_pin_refused` is
    // therefore a tripwire that must read zero on the span tier for as long as those two
    // agree; a non-zero here means one started answering for a frame the other did not, which
    // would make every pin count in this file suspect.
    expect("borrowed frame is DECLINED by segment_bytes==0, not refused", borrowed.site_refused, 0);
    expect("borrowed frame is counted as a copy", borrowed.site_copies, borrowed.lat.n);
#endif
    return bad;
}

}  // namespace

int main(int argc, char** argv) {
    int rounds = 10;
    int round0 = 0;
    bool do_calibrate = false;
    std::vector<arm_t> arms(std::begin(kAllArms), std::end(kAllArms));

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--rounds=", 0) == 0)
            rounds = std::atoi(a.c_str() + 9);
        else if (a.rfind("--round0=", 0) == 0)
            round0 = std::atoi(a.c_str() + 9);
        else if (a == "--calibrate")
            do_calibrate = true;
        else if (a.rfind("--arms=", 0) == 0) {
            arms.clear();
            std::string list = a.substr(7);
            std::size_t p = 0;
            while (p <= list.size()) {
                const std::size_t c = std::min(list.find(',', p), list.size());
                const std::string tok = list.substr(p, c - p);
                for (const arm_t& x : kAllArms)
                    if (tok == x.label || (tok.size() && std::string(x.label).rfind(tok, 0) == 0))
                        arms.push_back(x);
                p = c + 1;
            }
        }
    }

    // Reachability before numbers, every run — not once at authoring time.
    if (calibrate() != 0) {
        std::fprintf(stderr,
                     "bench_pin_ratio: instrument calibration FAILED; refusing to report\n");
        return 2;
    }
    if (do_calibrate) return 0;

    std::printf(
        "# RESULT_PIN round arm K payload segment p50ns p99ns meanns pins copies n "
        "site_pins site_copies site_refused\n");
    for (int r = 0; r < rounds; ++r) {
        // Rotate the arm order every round: arm i leads round i. Interleaving is the whole
        // defence against the recorded 2.8x sequential swing, and a fixed order inside a
        // round would just move the confound one level down.
        const std::size_t n = arms.size();
        for (std::size_t j = 0; j < n; ++j) {
            const arm_t& arm = arms[(static_cast<std::size_t>(r + round0) + j) % n];
            for (std::size_t payload : kPayloads)
                for (std::size_t segment : kSegments)
                    emit_pin(r + round0, arm.label, arm.k, payload, segment,
                             run_cell(payload, segment, arm.k));
        }
    }
    return 0;
}
