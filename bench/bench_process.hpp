/**
 * @file
 * @brief Process shape for the gated benches (#1803): fixed allocator state and one fresh
 *        process per sweep family.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * @section why_process Why a row needs its own process
 *
 * Every row of the default sweep used to share one process heap, aged by every row ahead of
 * it in a fixed order. glibc's allocator is stateful in ways a row cannot see: the dynamic
 * mmap threshold slides up the first time a large mmapped chunk is freed, the trim threshold
 * follows it, and the free lists a 1 KiB request lands in depend on what the rows before it
 * left behind. So a row's value was partly a function of its POSITION in the sweep — which is
 * how a 1 KiB regression showed up in the full sweep and not in isolation.
 *
 * Two levers remove that dependence, and this header holds both:
 *   - @ref pin_allocator_state re-executes the binary under one fixed `GLIBC_TUNABLES`
 *     string, so the thresholds are constants rather than history;
 *   - @ref run_family_process runs one family of the sweep in a fresh child process, so the
 *     heap every family starts from is the same empty one, whatever ran before it.
 *
 * A heap state that IS wanted (an aged, fragmented heap) is then built on purpose inside its
 * own family and reported under its own row name, never inherited from sweep order.
 */
#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#if defined(__linux__)
#include <sched.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;  // NOLINT(readability-redundant-declaration): POSIX, not in <unistd.h>
#endif

namespace bench {

/**
 * @brief The one allocator configuration every gated bench process runs under.
 *
 * Each value is pinned because its glibc default MOVES during a run:
 *   - `mmap_threshold=131072` — the default's starting value (128 KiB). Setting it explicitly
 *     switches off the dynamic threshold, which otherwise climbs (up to 32 MiB) the first time
 *     a row frees an mmapped block, changing where every later large request is served from.
 *   - `trim_threshold=33554432` — 32 MiB of free top-of-heap is kept instead of being handed
 *     back to the kernel, so a row does not pay `brk` shrink/grow page faults that depend on
 *     how much the previous row left at the top of the heap.
 *   - `arena_max=8` — arenas are otherwise created on contention, up to 8 per core, so the
 *     count a multi-threaded row runs with depended on the host's core count and on which
 *     threads collided first. 8 covers the largest thread count any family starts.
 */
inline constexpr const char* kAllocTunables =
    "glibc.malloc.mmap_threshold=131072:glibc.malloc.trim_threshold=33554432:"
    "glibc.malloc.arena_max=8";

/** @brief Set in the re-executed process, so the re-exec happens at most once. */
inline constexpr const char* kAllocPinnedEnv = "LIBTRACER_BENCH_ALLOC_PINNED";

/**
 * @brief Re-execute this process under @ref kAllocTunables unless it already runs under them.
 *
 * `GLIBC_TUNABLES` is read by the dynamic loader at process start, so it cannot be applied
 * from inside a running process: the only way to fix it is to set it and exec again. The
 * marker variable @ref kAllocPinnedEnv makes that a one-shot even if the loader were to
 * rewrite the tunables string. On a non-glibc or non-Linux host this is a no-op and the
 * process runs with whatever allocator it has.
 *
 * Call it first thing in `main`, before anything is written to stdout: `execv` discards the
 * process image, and with it any unflushed output.
 *
 * @param argv The process's own argv, passed through unchanged.
 */
inline void pin_allocator_state(char** argv) {
#if defined(__linux__) && defined(__GLIBC__)
    if (std::getenv(kAllocPinnedEnv) != nullptr) {
        const char* cur = std::getenv("GLIBC_TUNABLES");
        if (cur == nullptr || std::strcmp(cur, kAllocTunables) != 0)
            std::fprintf(stderr, "WARN allocator tunables differ from the pinned set: %s\n",
                         cur != nullptr ? cur : "(unset)");
        return;
    }
    if (setenv("GLIBC_TUNABLES", kAllocTunables, 1) != 0 || setenv(kAllocPinnedEnv, "1", 1) != 0)
        return;
    execv("/proc/self/exe", argv);
    // Only reached when the exec failed: run on, unpinned, and say so on stderr.
    std::fprintf(stderr, "WARN allocator tunables not applied (exec: %s)\n", std::strerror(errno));
#else
    (void)argv;
#endif
}

/**
 * @brief Print the allocator state this process runs under (#1903): `ALLOC <state> <tunables>`.
 *
 * One tab-separated line on stdout, ahead of the process's rows, so the transcript records the
 * settings every row was measured under instead of leaving them implied by the source. `state`
 * is `pinned` when the process runs under exactly @ref kAllocTunables (the re-exec of
 * @ref pin_allocator_state took effect) and `unpinned` otherwise; `tunables` is the
 * `GLIBC_TUNABLES` string the process actually has, or `-` when it has none. Every RESULT
 * parser skips the line on its tag; `perf_gate.py` prints it under the verdict and
 * `host_guard.py stamp` writes it onto each history point.
 *
 * HEAP-NEUTRAL by construction: the line is formatted into a stack buffer and written with
 * `write(2)`, never through `stdout`. A family child prints nothing on stdout before its first
 * row, so a `printf` here made stdio allocate its buffer (4 KiB on a pipe) AHEAD of the timed
 * rows instead of after the first one, shifting every later allocation's address; that moved
 * `inproc/64/1024/1` p50 by +3-5% with no library change. The `fflush` only pushes out a line
 * the caller already buffered, so this one cannot overtake it; it allocates nothing.
 */
inline void emit_alloc_state() {
    const char* cur = std::getenv("GLIBC_TUNABLES");
    const bool pinned = std::getenv(kAllocPinnedEnv) != nullptr && cur != nullptr &&
                        std::strcmp(cur, kAllocTunables) == 0;
    char line[512];
    const int n =
        std::snprintf(line, sizeof line, "ALLOC\t%s\t%s\n", pinned ? "pinned" : "unpinned",
                      cur != nullptr && *cur != '\0' ? cur : "-");
    if (n <= 0) return;
    const std::size_t len = std::min(static_cast<std::size_t>(n), sizeof line - 1);
    std::fflush(stdout);
#if defined(__linux__)
    for (std::size_t off = 0; off < len;) {
        const ssize_t w = ::write(STDOUT_FILENO, line + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        off += static_cast<std::size_t>(w);
    }
#else
    std::fwrite(line, 1, len, stdout);
    std::fflush(stdout);
#endif
}

/**
 * @brief Narrow this process to ONE logical CPU, the lowest of its affinity mask (#1906).
 *
 * A single-threaded family calls it before its first row. The methodology times every
 * single-threaded measurement on one logical CPU: given several, the scheduler may migrate
 * the thread mid-window, and the row then carries the migration. Whoever started the
 * process (`bench_conditions.py`, `taskset`, the default sweep) picks the SET; this picks the
 * one CPU inside it, so a family is pinned however it was started. A multi-threaded family
 * does not call it: it keeps the whole mask and sizes its threads from it
 * (`bench::usable_cpus`).
 *
 * Two syscalls and nothing else: no allocation, no output, no file, so the heap the first
 * row starts from is unchanged. A mask that cannot be read or set leaves the process as it
 * was; a mask of one CPU is a no-op.
 */
inline void pin_to_one_cpu() {
#if defined(__linux__)
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof mask, &mask) != 0) return;
    for (int c = 0; c < CPU_SETSIZE; ++c) {
        if (!CPU_ISSET(c, &mask)) continue;
        cpu_set_t one;
        CPU_ZERO(&one);
        CPU_SET(c, &one);
        (void)sched_setaffinity(0, sizeof one, &one);
        return;
    }
#endif
}

/** @brief Whether @ref run_family_process can start a child here; otherwise run in-process. */
#if defined(__linux__)
inline constexpr bool kFamilyProcesses = true;
#else
inline constexpr bool kFamilyProcesses = false;
#endif

/**
 * @brief Run `argv0 --family <name>` as a fresh child process and wait for it.
 *
 * The child inherits this process's environment (the pinned tunables included), its CPU
 * affinity (a single-threaded family then narrows it with @ref pin_to_one_cpu) and its stdout, so
 * its RESULT rows land in the same stream, in order, exactly as an in-process family's would.
 * stdout and stderr are flushed first so no buffered line of the parent's can be interleaved after
 * the child's.
 *
 * @param argv0 This binary's own `argv[0]`, passed through for the child's usage text.
 * @param family The family name the child runs.
 * @return The child's exit status (0 on success), or -1 if it could not be started or did
 *         not exit normally.
 */
inline int run_family_process(const char* argv0, std::string_view family) {
#if defined(__linux__)
    std::fflush(stdout);
    std::fflush(stderr);
    std::string name{family};
    char flag[] = "--family";
    char* child_argv[] = {const_cast<char*>(argv0), flag, name.data(), nullptr};
    pid_t pid = 0;
    if (posix_spawn(&pid, "/proc/self/exe", nullptr, nullptr, child_argv, environ) != 0) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#else
    (void)argv0;
    (void)family;
    return -1;
#endif
}

/**
 * @brief This process's resident set right now, in KiB; 0 where it cannot be read.
 *
 * Read from `/proc/self/statm` (resident pages times the page size): the CURRENT figure, which
 * `getrusage` does not give. Paired with the peak in @ref emit_family_rss.
 */
inline std::size_t rss_kb() {
#if defined(__linux__)
    std::FILE* const f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) return 0;
    unsigned long size = 0;
    unsigned long resident = 0;
    const int got = std::fscanf(f, "%lu %lu", &size, &resident);
    std::fclose(f);
    const long page = sysconf(_SC_PAGESIZE);
    return got == 2 && page > 0 ? resident * static_cast<std::size_t>(page) / 1024 : 0;
#else
    return 0;
#endif
}

/**
 * @brief Print one family's RSS delta (#1808): `RSS family=<name> start_kb= peak_kb= delta_kb=`.
 *
 * It replaces the whole-run "max RSS" that `/usr/bin/time -v` reported, which since #1803 was
 * the largest single family's peak and counted the harness itself (the binary, the C++
 * runtime, the latency vectors reserved before timing). `start_kb` is the resident set when the
 * family starts, `peak_kb` the process's high-water mark when it ends (`getrusage`), and
 * `delta_kb` their difference: what the family's own rows added. A plain line on stdout, so it
 * lands in the transcript beside the family's rows; every RESULT parser skips it on its tag.
 *
 * @param family   The family that just ran.
 * @param start_kb @ref rss_kb taken before the family's first row.
 */
inline void emit_family_rss(std::string_view family, std::size_t start_kb) {
#if defined(__linux__)
    rusage ru{};
    if (start_kb == 0 || getrusage(RUSAGE_SELF, &ru) != 0) return;
    const auto peak_kb = static_cast<std::size_t>(ru.ru_maxrss);  // KiB on Linux
    std::printf("RSS family=%.*s start_kb=%zu peak_kb=%zu delta_kb=%zu\n",
                static_cast<int>(family.size()), family.data(), start_kb, peak_kb,
                peak_kb > start_kb ? peak_kb - start_kb : 0);
    std::fflush(stdout);
#else
    (void)family;
    (void)start_kb;
#endif
}

}  // namespace bench
