/**
 * @file
 * @brief The staged `inproc` run (#1905); see inproc_staged.hpp.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
#include "inproc_staged.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <span>
#include <string>
#include <utility>

#include "bench_common.hpp"
#include "delivery_count.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/tracer.hpp"

namespace bench {

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::view::view_t;

/** @brief Per-message owned heap view — `bench_libtracer.cpp`'s allocating path, verbatim. */
view_t owned_view(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/**
 * @brief Owned values a producer built ahead of a timed region, handed out one per write.
 *
 * The producer's buffers, each a segment allocation and an S-byte copy (@ref owned_view), are
 * built once, @ref kStagedBuffers of them, and held until the row ends. @ref fill hands
 * out references to them in turn, untimed, and a timed write takes the next one with
 * @ref take, so a window pays everything the graph does with an owned value, dropping its
 * reference to the value it replaces included, and none of the buffer's allocation, copy or
 * free. Freeing inside the window was tried: a window that freed its batch of 4-16 KiB blocks
 * back to back measured glibc coalescing them, 2-3x the write itself, a cost a producer that
 * builds one value at a time never pays.
 */
class staged_values_t {
   public:
    /** @param tlv The bytes every value carries. */
    explicit staged_values_t(std::span<const std::byte> tlv) : tlv_(tlv) {}

    /** @brief Stage the next @p n values (untimed); builds the buffers on first use. */
    void fill(std::size_t n) {
        if (bufs_.empty())
            for (std::size_t b = 0; b < kStagedBuffers; ++b) bufs_.push_back(owned_view(tlv_));
        vals_.clear();
        vals_.reserve(n);
        for (std::size_t i = 0; i < n; ++i) vals_.push_back(bufs_[turn_++ % kStagedBuffers]);
        next_ = 0;
    }

    /** @brief The next staged value; at most as many calls as the last @ref fill staged. */
    [[nodiscard]] view_t take() { return std::move(vals_[next_++]); }

   private:
    std::span<const std::byte> tlv_;
    std::vector<view_t> bufs_; /**< @brief The producer's buffers, built once. */
    std::vector<view_t> vals_; /**< @brief Staged and not yet handed out. */
    std::size_t next_ = 0;     /**< @brief The next value @ref take hands out. */
    std::size_t turn_ = 0;     /**< @brief Which buffer the next staged value refers to. */
};

/**
 * @brief @ref emit_batch_row for a staged row (#1905): @p stage builds each window's inputs
 *        off the clock (@ref time_staged_batches).
 */
template <typename Stage, typename Op>
void emit_staged_batch_row(const char* mode, std::size_t S, std::size_t F, std::size_t E,
                           Stage&& stage, Op&& op, std::size_t lat_n) {
    constexpr std::uint64_t kBudgetNs = 10'000'000'000ULL;  // a backstop; lat_n ends the loop
    std::size_t i = 0;
    const batch_timing_t t = time_staged_batches(stage, [&] { op(i++); }, kBudgetNs, lat_n);
    const std::string batch_mode = std::string(mode) + "-batch";
    emit_batch("libtracer", batch_mode.c_str(), S, F, E, 0.0, 0.0, 0.0, t);
}

}  // namespace

void run_inproc_staged(std::size_t S, std::size_t F, std::size_t E, bool by_path, const char* mode,
                       std::uint64_t budget, std::uint64_t latbudget, tr::mem::block_source_t* src,
                       bool quantized, bool batch, const std::vector<std::byte>& tlv) {
    graph_t g{src != nullptr ? *src : tr::mem::heap_source()};
    std::vector<vertex_handle_t> verts;
    std::vector<path_t> paths;
    verts.reserve(E);
    paths.reserve(E);
    std::atomic<std::uint64_t> recv{0};
    auto cb = [&](const tr::graph::value_t&) { recv.fetch_add(1, std::memory_order_relaxed); };
    for (std::size_t e = 0; e < E; ++e) {
        path_t path = *path_t::parse("/bench/v" + std::to_string(e));
        auto v = g.register_vertex(path, role_t::STORED_VALUE);
        for (std::size_t f = 0; f < F; ++f) (void)g.subscribe(path, cb);
        verts.push_back(v);
        paths.push_back(std::move(path));
    }
    staged_values_t stage(tlv);
    const auto put = [&](std::size_t i) {
        if (by_path)
            (void)g.write(paths[i % E], stage.take());
        else
            (void)g.write(verts[i % E], stage.take());
    };
    const auto fill = [&](std::size_t n) { stage.fill(n); };

    const std::size_t MSGS = publishes_for(F, budget);
    const std::size_t LATN = publishes_for(F, latbudget);
    for (std::size_t i = 0; i < 1000; ++i) {  // warmup
        stage.fill(1);
        put(i);
    }
    if (!quantized) {
        emit_staged_batch_row(mode, S, F, E, fill, put, LATN);
        return;
    }

    // The bulk window is the sum of the write runs; each run's values are staged between runs.
    recv.store(0);
    std::uint64_t ns = 0;
    for (std::size_t i = 0; i < MSGS;) {
        const std::size_t n = std::min(kStagedChunk, MSGS - i);
        stage.fill(n);
        const auto a = now_ns();
        for (const std::size_t end = i + n; i < end; ++i) put(i);
        ns += now_ns() - a;
    }
    const double secs = ns / 1e9;
    const std::uint64_t want = static_cast<std::uint64_t>(MSGS) * F;
    const double pub_s = MSGS / secs;
    const double deliv_s =
        delivered_rate(mode, S, F, E, want, recv.load(std::memory_order_relaxed), secs);
    const double mb_s = deliv_s * static_cast<double>(S) / 1e6;

    // Clock-quantized like @ref run_inproc's per-op phase; read the batch twin for small deltas.
    Latency lat;
    lat.reserve(LATN);
    for (std::size_t i = 0; i < LATN; ++i) {
        stage.fill(1);  // the producer's value, staged before the clock starts
        const auto a = now_ns();
        put(i);
        lat.add(now_ns() - a);
    }
    emit("libtracer", mode, S, F, E, pub_s, deliv_s, mb_s, lat.summarize());
    if (batch) emit_staged_batch_row(mode, S, F, E, fill, put, LATN);
}

}  // namespace bench
