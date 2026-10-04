/**
 * @file
 * @brief `tr::mem::string_t` — the core's failable name/string store: an owning, NUL-terminated
 *        character string drawn from a `block_source_t`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * One of the three core containers ADR-0083 Decision 2 names (#1776), next to the vector
 * (`block_array_t`, `mem_source.hpp`) and the sorted map (`mem_sorted_map.hpp`). It is what a
 * vertex name, a link name or a configured path becomes once it no longer sits in a
 * `std::string`: the bytes come from the injected source, and a refused allocation is a
 * `false` the caller turns into BACKPRESSURE rather than a `std::bad_alloc`. Nothing migrates
 * onto it in the ticket that adds it; the directory batches do that.
 */
#pragma once

#include <compare>
#include <cstddef>
#include <cstring>
#include <string_view>

#include "libtracer/mem_source.hpp"

namespace tr::mem {

/**
 * @brief An owning, NUL-terminated string whose every byte comes from a @ref block_source_t,
 *        and whose growth reports refusal by value (ADR-0083 Decision 2).
 *
 * The block is sized EXACTLY by @ref assign (a name is written once and read many times, so
 * slack is wasted RAM on a small node) and doubled by @ref append. A refused call leaves the
 * string as it was. Non-copyable, because a copy is a failable allocation: copy with
 * `dst.assign(src)`, which says so. Movable, which steals the block.
 *
 * Reads go through @ref view, or the implicit conversion to `std::string_view`; @ref c_str is
 * for the C APIs that want a terminator. Comparison is byte-wise against any
 * `std::string_view`, so a @ref sorted_map_t keyed by `string_t` can be searched with a view
 * without building a key.
 *
 * Four words: the source, the block, the length and the capacity.
 */
class string_t {
   public:
    /** @brief An empty string that will draw its storage from @p src. Allocates nothing. */
    explicit string_t(block_source_t& src) noexcept : src_(&src) {}
    /** @brief Returns the block, if one was taken. */
    ~string_t() { release_block(); }

    /** @brief Non-copyable — a copy is a failable allocation; use @ref assign. */
    string_t(const string_t&) = delete;
    /** @brief Non-assignable by copy; use @ref assign. */
    string_t& operator=(const string_t&) = delete;
    /** @brief Steal @p o's block; @p o is left empty, on the same source. */
    string_t(string_t&& o) noexcept : src_(o.src_), data_(o.data_), size_(o.size_), cap_(o.cap_) {
        o.data_ = nullptr;
        o.size_ = o.cap_ = 0;
    }
    /** @brief Release this string's block, then steal @p o's. */
    string_t& operator=(string_t&& o) noexcept {
        if (this != &o) {
            release_block();
            src_ = o.src_;
            data_ = o.data_;
            size_ = o.size_;
            cap_ = o.cap_;
            o.data_ = nullptr;
            o.size_ = o.cap_ = 0;
        }
        return *this;
    }

    /**
     * @brief Replace the contents with @p s. @p s may view this string's own bytes.
     *
     * Reuses the block when it is big enough; otherwise takes a block of exactly
     * `s.size() + 1` bytes.
     *
     * @retval false The source refused — the string is unchanged.
     */
    [[nodiscard]] bool assign(std::string_view s) noexcept {
        if (s.size() <= cap_) {
            if (!s.empty()) std::memmove(data_, s.data(), s.size());
            size_ = s.size();
            terminate();
            return true;
        }
        return rebuild(s.size(), std::string_view{}, s);
    }

    /**
     * @brief Append @p s. @p s may view this string's own bytes.
     * @retval false The source refused — the string is unchanged.
     */
    [[nodiscard]] bool append(std::string_view s) noexcept {
        const std::size_t need = size_ + s.size();
        if (need <= cap_) {
            if (!s.empty()) std::memmove(data_ + size_, s.data(), s.size());
            size_ = need;
            terminate();
            return true;
        }
        return rebuild(need < 2 * cap_ ? 2 * cap_ : need, view(), s);
    }

    /**
     * @brief Ensure room for @p n characters (plus the terminator) without growing again.
     * @retval false The source refused — the string is unchanged.
     */
    [[nodiscard]] bool reserve(std::size_t n) noexcept {
        return n <= cap_ || rebuild(n, view(), std::string_view{});
    }

    /** @brief Make the string empty; the block is kept for reuse. */
    void clear() noexcept {
        size_ = 0;
        terminate();
    }

    /** @brief The contents, valid until the next mutating call. */
    [[nodiscard]] std::string_view view() const noexcept { return {c_str(), size_}; }
    /** @brief The contents as a view — the spelling every `std::string_view` parameter takes. */
    operator std::string_view() const noexcept {
        return view();
    }  // NOLINT(google-explicit-constructor)
    /** @brief The NUL-terminated contents; `""` while no block has been taken. */
    [[nodiscard]] const char* c_str() const noexcept { return data_ != nullptr ? data_ : ""; }
    /** @brief Length in bytes, excluding the terminator. */
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    /** @brief Characters the current block holds, excluding the terminator. */
    [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }
    /** @brief True when the length is zero. */
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    /** @brief The source this string draws from. */
    [[nodiscard]] block_source_t& source() const noexcept { return *src_; }

    /** @brief Byte-wise equality with any view. */
    friend bool operator==(const string_t& a, std::string_view b) noexcept { return a.view() == b; }
    /** @brief Byte-wise equality of two strings. */
    friend bool operator==(const string_t& a, const string_t& b) noexcept {
        return a.view() == b.view();
    }
    /** @brief Byte-wise ordering against any view (the `std::string_view` ordering). */
    friend std::strong_ordering operator<=>(const string_t& a, std::string_view b) noexcept {
        return a.view() <=> b;
    }
    /** @brief Byte-wise ordering of two strings. */
    friend std::strong_ordering operator<=>(const string_t& a, const string_t& b) noexcept {
        return a.view() <=> b.view();
    }

   private:
    /** @brief Write the terminator after the last character, when a block exists. */
    void terminate() noexcept {
        if (data_ != nullptr) data_[size_] = '\0';
    }

    /** @brief Return the block to the source. */
    void release_block() noexcept {
        if (data_ != nullptr) src_->release(data_, cap_ + 1, alignof(char));
    }

    /**
     * @brief Move to a fresh block of @p cap characters holding @p head then @p tail.
     *
     * Both views may point into the old block: they are copied before it is released.
     *
     * @retval false The source refused — the string is untouched.
     */
    [[gnu::noinline]] [[nodiscard]] bool rebuild(std::size_t cap, std::string_view head,
                                                 std::string_view tail) noexcept {
        auto* fresh = static_cast<char*>(src_->try_alloc(cap + 1, alignof(char)));
        if (fresh == nullptr) return false;
        if (!head.empty()) std::memcpy(fresh, head.data(), head.size());
        if (!tail.empty()) std::memcpy(fresh + head.size(), tail.data(), tail.size());
        release_block();
        data_ = fresh;
        size_ = head.size() + tail.size();
        cap_ = cap;
        terminate();
        return true;
    }

    block_source_t* src_;  /**< @brief Where the block comes from; never null. */
    char* data_ = nullptr; /**< @brief The block (`cap_ + 1` bytes), or null before the first. */
    std::size_t size_ = 0; /**< @brief Characters held, excluding the terminator. */
    std::size_t cap_ = 0;  /**< @brief Characters the block holds, excluding the terminator. */
};

}  // namespace tr::mem
