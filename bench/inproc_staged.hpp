/**
 * @file
 * @brief The staged `inproc` run (#1905): the HEAP rows from 1 KiB, timing the write alone.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Its own translation unit on purpose. Added to `bench_libtracer.cpp`, the same code changed
 * how GCC inlined the rest of that file, and the rows it does not touch (`inproc/64/1/1`,
 * `inproc-mt*`) moved 1.5-2% with no change to their source.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "libtracer/mem_source.hpp"

namespace bench {

/**
 * @brief One HEAP `inproc` point from @ref kProducerUntimedFrom up, its values staged off the
 *        clock in every phase.
 *
 * The producer's buffers are built before and freed after every timed region, and each write
 * takes a reference to the next one: the bulk throughput stages a chunk between timed runs, the
 * per-op latency stages one before the clock starts, and the batch twin stages each window's
 * values before it (@ref time_staged_batches). Deliveries are counted at the subscribers, as
 * every `inproc` row's are. The rows are `bench_libtracer`'s `run_inproc` rows, same keys.
 *
 * @param S, F, E    Payload bytes, subscribers per endpoint, endpoints.
 * @param by_path    Write by path (`inproc-path`) instead of by handle.
 * @param mode       The row's mode.
 * @param budget     Delivery budget of the throughput phase.
 * @param latbudget  Delivery budget of the latency phases.
 * @param src        The graph's source; null keeps the heap source, as `run_inproc` does.
 * @param quantized  Publish the quantized row (throughput and per-op latency).
 * @param batch      Publish the `<mode>-batch` twin.
 * @param tlv        The VALUE TLV every write carries.
 */
void run_inproc_staged(std::size_t S, std::size_t F, std::size_t E, bool by_path, const char* mode,
                       std::uint64_t budget, std::uint64_t latbudget, tr::mem::block_source_t* src,
                       bool quantized, bool batch, const std::vector<std::byte>& tlv);

}  // namespace bench
