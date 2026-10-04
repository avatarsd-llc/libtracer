/**
 * @file
 * @brief Negative-compile fixture: `inline_fn_t` rejects callables it cannot hold inline.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Never built as a target. The `inline_fn_reject_*` ctest entries compile it
 * with `-fsyntax-only` and one `INLINE_FN_REJECT_*` macro, and pass only when the
 * compiler emits the matching `static_assert` message: there is no run-time heap
 * fallback to test, so the rejection itself is the contract.
 */

#include <memory>

#include "libtracer/inline_fn.hpp"

/** @brief Instantiate the constructor with the case selected by the macro. */
void reject_case() {
#if defined(INLINE_FN_REJECT_OVERSIZE)
    long a = 1, b = 2, c = 3;
    tr::inline_fn_t<void()> f = [a, b, c] { (void)(a + b + c); };
#elif defined(INLINE_FN_REJECT_OWNING)
    auto owner = std::make_shared<int>(1);
    tr::inline_fn_t<void()> f = [owner] { (void)*owner; };
#else
#error "select a case with -DINLINE_FN_REJECT_OVERSIZE or -DINLINE_FN_REJECT_OWNING"
#endif
    f();
}
