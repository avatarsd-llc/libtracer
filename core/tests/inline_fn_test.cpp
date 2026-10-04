/**
 * @file
 * @brief Unit tests for the heap-free stored callable (inline_fn.hpp, #1671).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Pins the slot's contract: empty and null states, lambda and function-pointer
 * storage, copy keeps both slots live, the size is capacity plus one pointer and
 * the slot itself is trivially copyable. The compile-time rejection of an
 * oversized or resource-owning callable is pinned by the `inline_fn_reject_*`
 * ctest entries, which compile `inline_fn_reject.cpp` and expect the diagnostic.
 */

#include "libtracer/inline_fn.hpp"

#include <cstdio>
#include <type_traits>

#include "test_support.hpp"

namespace {

using tr::testing::check;

/** @brief The slot shape the CAN receive seam uses. */
using fn_t = tr::inline_fn_t<void(int&)>;

static_assert(sizeof(fn_t) == fn_t::kCapacity + sizeof(void*),
              "the slot is its buffer plus one invoke pointer");
static_assert(std::is_trivially_copyable_v<fn_t>, "a copy is a byte copy");
static_assert(std::is_trivially_destructible_v<fn_t>, "destruction is free");

/** @brief A plain free function for the function-pointer case. */
void add_seven(int& x) { x += 7; }

}  // namespace

int main() {
    std::printf("inline_fn — heap-free stored callable:\n");

    fn_t empty;
    check(!empty, "default-constructed slot is empty");
    fn_t null_slot = nullptr;
    check(!null_slot, "nullptr-constructed slot is empty");
    void (*null_fp)(int&) = nullptr;
    fn_t from_null_fp = null_fp;
    check(!from_null_fp, "a null function pointer yields an empty slot");

    int hits = 0;
    int* where = &hits;
    fn_t by_lambda = [where](int& x) {
        ++*where;
        x += 1;
    };
    check(static_cast<bool>(by_lambda), "a capturing lambda is stored");
    int v = 0;
    by_lambda(v);
    check(v == 1 && hits == 1, "invoking runs the stored lambda");

    fn_t copy = by_lambda;
    copy(v);
    by_lambda(v);
    check(v == 3 && hits == 3, "a copy and its source both stay live");

    int a = 0;
    int b = 0;
    fn_t two_refs = [&a, &b](int& x) {
        a += x;
        b -= x;
    };
    int five = 5;
    two_refs(five);
    check(a == 5 && b == -5, "two by-reference captures fit the default capacity");

    fn_t by_pointer = &add_seven;
    int w = 0;
    by_pointer(w);
    check(w == 7, "a function pointer is stored and invoked");

    int counter = 0;
    fn_t stateful = [n = 0, &counter](int&) mutable { counter = ++n; };
    int unused = 0;
    stateful(unused);
    stateful(unused);
    check(counter == 2, "a mutable lambda keeps its state across calls");

    copy = nullptr;
    check(!copy, "assigning nullptr empties the slot");
    copy = by_pointer;
    copy(w);
    check(w == 14, "an emptied slot accepts a new callable");

    return tr::testing::summary("inline_fn");
}
