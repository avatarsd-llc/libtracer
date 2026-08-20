/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — the failable block seam: exhaustion arrives as a VALUE.
 *
 * `tr::mem::block_source_t` is the L0 seam every allocation a PEER can provoke draws from
 * (ADR-0065). It is three members wide: a nothrow `try_alloc(bytes, align)`, a SIZED
 * `release(p, bytes, align)`, and a `name()`. There is no throwing spelling and no fallback
 * to the global heap — `nullptr` is the entire failure vocabulary, and the caller turns it
 * into whatever reject its own operation owns.
 *
 * Both sides are shown here, because both are the reader's: `heap_source()` is the default
 * a node gets for free, and a source of one's own is how a deployment states its bound. The
 * refusal is provoked on a source with a budget rather than on the platform heap — an
 * allocation the real allocator cannot serve is a sanitizer's fatal error, not a `nullptr`
 * (`core/tests/mem_source_test.cpp`).
 *
 * Runs under ctest as `example_mem_block_source`; returns non-zero on any failed check.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "libtracer/mem_source.hpp"

namespace {

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/**
 * @brief A source of one's own: serve a fixed number of blocks, then refuse — and record
 *        the `(bytes, align)` every reclaim arrived with.
 *
 * The whole seam is these two overrides. Implementing it is what a deployment does to make
 * "how much memory may this node use" its own property rather than the library's.
 */
class budget_source_t final : public tr::mem::block_source_t {
   public:
    /** @brief Serve at most @p budget blocks. */
    explicit budget_source_t(int budget) noexcept
        : tr::mem::block_source_t("budget"), left_(budget) {}

    /** @brief Serve while the budget lasts; `nullptr` — never a throw — once it does not. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (left_ <= 0) return nullptr;
        --left_;
        return ::operator new(bytes, std::align_val_t{align}, std::nothrow);
    }

    /** @brief Sized reclaim — @p bytes and @p align are the ones @ref try_alloc was asked for. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        last_bytes_ = bytes;
        last_align_ = align;
        ::operator delete(p, bytes, std::align_val_t{align});
    }

    std::size_t last_bytes_ = 0; /**< @brief Size the most recent reclaim carried. */
    std::size_t last_align_ = 0; /**< @brief Alignment the most recent reclaim carried. */

   private:
    int left_; /**< @brief Blocks this source will still serve. */
};

}  // namespace

int main() {
    bool ok = true;
    tr::mem::block_source_t& heap = tr::mem::heap_source();
    std::printf("every source names itself; the process-wide default is \"%s\"\n", heap.name());

    // The contract is a compile-time property first: neither half may throw, because the
    // shipping profile builds with -fno-exceptions and a throw there reaches an abort stub.
    static_assert(noexcept(heap.try_alloc(1, 1)), "try_alloc is nothrow");
    static_assert(noexcept(heap.release(nullptr, 1, 1)), "release is nothrow");

    void* const block = heap.try_alloc(96, 64);
    check(ok, block != nullptr, "the default source serves a 96-byte block");
    check(ok, reinterpret_cast<std::uintptr_t>(block) % 64 == 0,
          "aligned to at least the requested boundary");
    std::memset(block, 0xA5, 96);  // writable for its whole length; there is no header to dodge
    heap.release(block, 96, 64);

    budget_source_t budget{2};
    void* const first = budget.try_alloc(48, 8);
    void* const second = budget.try_alloc(48, 8);
    check(ok, first != nullptr && second != nullptr, "a bounded source serves within its budget");
    check(ok, budget.try_alloc(48, 8) == nullptr,
          "past it the answer is nullptr — never a throw, never an abort()");
    check(ok, std::strcmp(budget.name(), "budget") == 0,
          "and it names itself, for a census that has to say WHICH seam ran out");

    // SIZED reclaim: the caller hands back the size and alignment it asked for, which is what
    // lets a bump or pool source carry no per-block header at all (see mem_pool_source).
    budget.release(first, 48, 8);
    budget.release(second, 48, 8);
    check(ok, budget.last_bytes_ == 48 && budget.last_align_ == 8,
          "release carries the ORIGINAL (bytes, align) — the source stores no header to recover "
          "them from");
    return ok ? 0 : 1;
}
