/**
 * @file
 * @brief Rope-source differential FUZZER for the wire grammar (ADR-0048 §consequences: "the
 *        differential fuzzers gain a rope-source mode — same bytes split at adversarial link
 *        boundaries MUST decode identically to the contiguous case, including mid-header splits").
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Where rope_decode_test pins a handful of crafted
 * frames, this sweeps thousands of deterministic seeds: each seed yields one byte
 * buffer — a codec-built valid frame, a mutated near-valid frame, or pure random
 * bytes — and the INVARIANT checked for every buffer is
 *
 *     validate_rope(rope-split(buf)).verdict == decode(buf).verdict          AND
 *     validate_rope(...).error() == decode(buf).error()   (when both reject)
 *
 * and, since ADR-0053, the same obligation for the LAZY tier: a FULL lazy walk
 * (tlv_view_t::over + verify + DFS children) must reach decode's verdict too —
 * when everything is accessed, laziness defers work, never changes answers.
 *
 * for EVERY single-cut split (each interior offset — so mid-header, mid-trailer
 * and mid-payload are all hit) plus several random multi-cut splits. A divergence
 * is a genuine rope-cursor vs span-cursor grammar bug; the seed + cut + verdicts
 * are printed and the run exits non-zero. Deterministic (fixed seed sweep, an
 * xorshift PRNG — no wall-clock/rand), so a failure is reproducible from its seed.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>

#include "libtracer/frame.hpp"
#include "libtracer/mem_borrowed.hpp"
#include "libtracer/rope_decode.hpp"
#include "libtracer/tlv_view.hpp"
#include "libtracer/view.hpp"

namespace {

/** @brief Deterministic xorshift64 — reproducible, no wall-clock or global rand(). */
struct rng_t {
    std::uint64_t s;
    std::uint64_t next() noexcept {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    std::uint32_t u32() noexcept { return static_cast<std::uint32_t>(next()); }
    std::uint32_t below(std::uint32_t n) noexcept { return n ? u32() % n : 0; }
};

/**
 * @brief Build a random, well-formed tlv_t.
 *
 * Payload buffers are parked in `store` (a
 * std::deque — stable element addresses across growth) so the borrowed
 * tlv_t::payload spans stay valid until encode() reads them.
 */
tr::wire::tlv_t gen_tlv(rng_t& r, int depth, std::deque<std::vector<std::byte>>& store) {
    tr::wire::tlv_t t;
    t.type = static_cast<tr::wire::type_t>(1 + r.below(0xFE));  // nonzero type code
    const bool structured = depth < 3 && r.below(2) == 0;
    if (structured) {
        t.opt.pl = true;
        const std::uint32_t n = r.below(4);
        for (std::uint32_t i = 0; i < n; ++i) t.children.push_back(gen_tlv(r, depth + 1, store));
        return t;
    }
    const std::uint32_t plen = r.below(20);
    store.emplace_back(plen);
    std::vector<std::byte>& buf = store.back();
    for (auto& x : buf) x = static_cast<std::byte>(r.u32() & 0xFFu);
    t.payload = buf;  // borrows the parked buffer
    // Random trailers — encode() recomputes the CRC value, so any placeholder is fine.
    if (r.below(2) == 0) {
        t.opt.cr = true;
        if (r.below(2) == 0) t.opt.cw = true;  // CRC-16 variant
    }
    if (r.below(3) == 0) {
        t.opt.ts = true;  // absolute 8-byte timestamp (tf left 0)
        tr::wire::trailer_t tr;
        tr.ts =
            tr::wire::timestamp_t{.relative = false, .value = static_cast<std::int64_t>(r.next())};
        t.trailer = tr;
    }
    return t;
}

std::vector<std::byte> gen_buffer(rng_t& r) {
    const std::uint32_t mode = r.below(3);
    if (mode == 0) {
        // Pure random bytes — hammers the reject paths (bad type/opt/length/CRC).
        const std::uint32_t n = r.below(49);
        std::vector<std::byte> b(n);
        for (auto& x : b) x = static_cast<std::byte>(r.u32() & 0xFFu);
        return b;
    }
    std::deque<std::vector<std::byte>> store;
    const std::vector<std::byte> frame = tr::wire::encode(gen_tlv(r, 0, store));
    if (mode == 1) return frame;  // a canonical valid frame
    // mode 2: flip one random byte of a valid frame — near-valid boundary cases.
    std::vector<std::byte> f = frame;
    if (!f.empty()) f[r.below(static_cast<std::uint32_t>(f.size()))] ^= std::byte{0x5A};
    return f;
}

/** @brief Own the per-link byte copies + the rope viewing them (the links borrow `parts`). */
struct split_rope_t {
    std::vector<std::vector<std::byte>> parts;
    tr::view::rope_t rope;
};

split_rope_t split_into(const std::vector<std::byte>& flat, const std::vector<std::size_t>& cuts) {
    split_rope_t out;
    out.parts.reserve(cuts.size() + 1);
    std::size_t prev = 0;
    for (const std::size_t c : cuts) {
        out.parts.emplace_back(flat.begin() + static_cast<std::ptrdiff_t>(prev),
                               flat.begin() + static_cast<std::ptrdiff_t>(c));
        prev = c;
    }
    out.parts.emplace_back(flat.begin() + static_cast<std::ptrdiff_t>(prev), flat.end());
    for (auto& p : out.parts) {
        out.rope.append(tr::view::view_t::over(tr::view::borrow(std::span<std::byte>(p))));
    }
    return out;
}

/**
 * @brief A FULL lazy walk over the view tier (ADR-0053): over() -> verify(root)
 * -> DFS children, verifying each child right after its header parse — the same
 * node order as decode()'s parse_one-then-descend.
 *
 * Its explicit stack grows on the heap, matching decode's heap-spilled walk
 * stack (RFC-0006 — depth is resource-bounded on both sides, no constant). When
 * everything is accessed, lazy defers nothing, so the verdict must equal
 * decode's.
 */
std::expected<void, tr::wire::err_t> lazy_full_walk(const tr::view::rope_t& r) {
    auto root = tr::wire::tlv_view_t::over(r);
    if (!root) return std::unexpected(root.error());
    if (const auto ok = root->verify(); !ok) return std::unexpected(ok.error());
    std::vector<tr::wire::tlv_view_t::children_t> stack;
    if (root->structured()) stack.push_back(root->children());
    while (!stack.empty()) {
        if (stack.back().exhausted()) {
            stack.pop_back();
            continue;
        }
        auto nx = stack.back().next();
        if (!nx) return std::unexpected(nx.error());
        if (!*nx) continue;  // (unreachable: exhausted() checked above)
        const tr::wire::tlv_view_t child = std::move(**nx);
        if (const auto ok = child.verify(); !ok) return std::unexpected(ok.error());
        if (child.structured()) stack.push_back(child.children());
    }
    return {};
}

/**
 * @brief The invariant for one split: BOTH rope-tier readers must agree with decode.
 *
 * One documented divergence: over() anchors bounds BEFORE any CRC (ADR-0053 §4),
 * while decode checks the root CRC before its trailing-bytes check — so a frame
 * with BOTH defects yields FRAME_CRC_FAIL eagerly but FRAME_INVALID lazily.
 * That exact err_t pair (same reject verdict) is accepted; everything else must
 * match exactly.
 */
bool split_agrees(const std::vector<std::byte>& buf, const std::vector<std::size_t>& cuts,
                  bool expect_ok, tr::wire::err_t expect_err, tr::wire::err_t& got_err) {
    split_rope_t sr = split_into(buf, cuts);
    const auto v = tr::wire::validate_rope(sr.rope);
    const bool ok = v.has_value();
    got_err = ok ? tr::wire::err_t{} : v.error();
    if (ok != expect_ok) return false;
    if (!ok && !expect_ok && v.error() != expect_err) return false;

    const auto lazy = lazy_full_walk(sr.rope);
    const bool lok = lazy.has_value();
    if (lok != expect_ok) {
        got_err = lok ? tr::wire::err_t{} : lazy.error();
        return false;
    }
    if (!lok && !expect_ok && lazy.error() != expect_err) {
        const bool documented_reorder = expect_err == tr::wire::err_t::FRAME_CRC_FAIL &&
                                        lazy.error() == tr::wire::err_t::FRAME_INVALID;
        if (!documented_reorder) {
            got_err = lazy.error();
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    constexpr std::uint64_t kSeeds = 4000;
    std::printf("Rope-source grammar fuzzer (validate_rope == decode over %llu seeds):\n",
                static_cast<unsigned long long>(kSeeds));

    std::uint64_t buffers = 0;
    std::uint64_t splits = 0;

    for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
        rng_t r{seed * 0x9E3779B97F4A7C15ull + 1};  // spread seeds across the state space
        const std::vector<std::byte> buf = gen_buffer(r);
        ++buffers;

        const auto dec = tr::wire::decode(buf);
        const bool expect_ok = dec.has_value();
        const tr::wire::err_t expect_err = expect_ok ? tr::wire::err_t{} : dec.error();

        // Every single-cut split (offset 0 == one link; 1..n-1 == 2 links) — this is
        // where mid-header / mid-trailer / mid-payload boundaries live.
        for (std::size_t cut = 0; cut < buf.size(); ++cut) {
            const std::vector<std::size_t> cuts =
                cut == 0 ? std::vector<std::size_t>{} : std::vector<std::size_t>{cut};
            tr::wire::err_t got_err{};
            ++splits;
            if (!split_agrees(buf, cuts, expect_ok, expect_err, got_err)) {
                std::printf(
                    "  [FAIL] seed=%llu cut=%zu buflen=%zu expect_ok=%d expect_err=%d "
                    "got_err=%d\n",
                    static_cast<unsigned long long>(seed), cut, buf.size(),
                    static_cast<int>(expect_ok), static_cast<int>(expect_err),
                    static_cast<int>(got_err));
                return 1;
            }
        }

        // A few random multi-cut (2-3 link) splits for straddle-across-3 coverage.
        for (int t = 0; t < 4 && buf.size() >= 3; ++t) {
            const std::size_t n = buf.size();
            std::size_t a = 1 + r.below(static_cast<std::uint32_t>(n - 1));
            std::size_t b = 1 + r.below(static_cast<std::uint32_t>(n - 1));
            if (a == b) continue;
            if (a > b) std::swap(a, b);
            tr::wire::err_t got_err{};
            ++splits;
            if (!split_agrees(buf, {a, b}, expect_ok, expect_err, got_err)) {
                std::printf(
                    "  [FAIL] seed=%llu cuts={%zu,%zu} buflen=%zu expect_ok=%d "
                    "expect_err=%d got_err=%d\n",
                    static_cast<unsigned long long>(seed), a, b, buf.size(),
                    static_cast<int>(expect_ok), static_cast<int>(expect_err),
                    static_cast<int>(got_err));
                return 1;
            }
        }
    }

    std::printf("  [PASS] %llu buffers, %llu rope splits — all agree with decode\n",
                static_cast<unsigned long long>(buffers), static_cast<unsigned long long>(splits));
    std::printf("ALL PASS\n");
    return 0;
}
