// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
/**
 * @file instr_count.hpp
 * @brief A user-space retired-instruction counter for the benches (#2030).
 *
 * The gate reads the allocator rows (`cliff-alloc-heap`, `lkv-alloc-heap`) by instructions
 * per operation, because their time moves with where the linker put the code (a 32 B shift
 * of identical code is x1.20) while an instruction count does not. This is the counter: one
 * `perf_event_open` of `PERF_COUNT_HW_INSTRUCTIONS` on the calling thread, user mode only
 * (so it works at `perf_event_paranoid` 2), with no library timer or clock read in it.
 * Where the host refuses the event the counter reports `available() == false` and the bench
 * prints no row, which the gate says out loud instead of passing in silence.
 */
#pragma once

#include <cstdint>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace bench {

/** @brief Counts user-mode instructions retired by the calling thread between start/stop. */
class instr_counter_t {
   public:
    instr_counter_t() {
#if defined(__linux__) && defined(__NR_perf_event_open)
        perf_event_attr attr{};
        attr.type = PERF_TYPE_HARDWARE;
        attr.size = sizeof(attr);
        attr.config = PERF_COUNT_HW_INSTRUCTIONS;
        attr.disabled = 1;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        fd_ = static_cast<int>(syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0));
#endif
    }
    ~instr_counter_t() {
#if defined(__linux__)
        if (fd_ >= 0) close(fd_);
#endif
    }
    instr_counter_t(const instr_counter_t&) = delete;
    instr_counter_t& operator=(const instr_counter_t&) = delete;

    /** @brief Whether the host gave us a counter. */
    [[nodiscard]] bool available() const { return fd_ >= 0; }

    /** @brief Zero and start counting. */
    void start() {
#if defined(__linux__)
        ioctl(fd_, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0);
#endif
    }

    /** @brief Stop counting; @return instructions retired since @ref start. */
    std::uint64_t stop() {
        std::uint64_t n = 0;
#if defined(__linux__)
        ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0);
        if (read(fd_, &n, sizeof(n)) != static_cast<long>(sizeof(n))) n = 0;
#endif
        return n;
    }

   private:
    int fd_ = -1;
};

}  // namespace bench
