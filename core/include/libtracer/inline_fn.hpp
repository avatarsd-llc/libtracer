/**
 * @file
 * @brief `inline_fn_t` — a stored callable with compile-time-sized inline storage.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The one-allocation-seam rulings (ADR-0083 §9, Q10 and Q18) take
 * `std::function` out of core. A hook stays a non-owning `{fn, ctx}` slot
 * (RFC-0028 D10); a stored callable that is NOT a hook — the first one is
 * `can_link_t::rx_fn_t` (#1671) — keeps its owning, type-erased shape but holds
 * the callable in a buffer whose size is fixed at compile time. A callable too
 * large (or too strictly aligned) for that buffer is a compile error, never a
 * run-time fallback to the heap, so the type allocates nothing on any target.
 *
 * The stored callable must be trivially copyable and trivially destructible
 * (a lambda capturing `this`, a pointer or a few references; a plain function
 * pointer). That keeps the slot itself trivially copyable: a copy is a few-word
 * byte copy, destruction is free, and there is no manager pointer to carry or
 * call. A callable owning a resource (a `std::shared_ptr` or `std::string`
 * capture) is rejected at compile time like an oversized one — capture a pointer
 * to the owner instead. The #1671 ledger measured why: a manager-carrying
 * variant cost 176 B of Cortex-M0 `.text` on the receive seam against 78 B for
 * this one (and 270 B for the `std::function` it replaces).
 *
 * The class lives in the layer-neutral `tr` namespace (as `sink_slot_t` does):
 * it depends on nothing but the standard library and any layer may hold one.
 */
#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace tr {

/** @brief The default inline capacity of an @ref inline_fn_t, in bytes: two pointers. */
inline constexpr std::size_t kInlineFnDefaultCapacity = 2 * sizeof(void*);

/** @brief Primary template; only the function-type specialization is defined. */
template <class Sig, std::size_t Capacity = kInlineFnDefaultCapacity>
class inline_fn_t;

/**
 * @brief An owning, trivially copyable, heap-free callable of signature `R(Args...)`.
 *
 * Holds any trivially copyable, trivially destructible callable whose size is at
 * most @p Capacity and whose alignment is at most `alignof(void*)`; anything else
 * fails a `static_assert` at the construction site. Empty when default-constructed
 * or constructed from `nullptr`; invoking an empty one is undefined (check with
 * `operator bool`, as `std::function` callers already do). `sizeof` is
 * @p Capacity plus one pointer.
 *
 * @tparam R        The return type.
 * @tparam Args     The parameter types.
 * @tparam Capacity The inline buffer size in bytes.
 */
template <class R, class... Args, std::size_t Capacity>
class inline_fn_t<R(Args...), Capacity> {
    static_assert(Capacity >= sizeof(void*), "inline_fn_t: Capacity must hold at least a pointer");

   public:
    /** @brief The inline buffer size in bytes. */
    static constexpr std::size_t kCapacity = Capacity;

    /** @brief Construct empty. */
    constexpr inline_fn_t() noexcept = default;

    /** @brief Construct empty (mirrors `std::function(nullptr)`). */
    constexpr inline_fn_t(std::nullptr_t) noexcept {}  // NOLINT(google-explicit-constructor)

    /**
     * @brief Store a copy of @p f inline.
     *
     * Fails to compile when `F` does not fit the buffer, or owns a resource:
     * raise the capacity at the alias that names this type, or capture less (a
     * pointer to a context instead of the context by value). A null function
     * pointer yields an empty slot.
     */
    template <class F, class D = std::decay_t<F>,
              class = std::enable_if_t<!std::is_same_v<D, inline_fn_t> &&
                                       std::is_invocable_r_v<R, D&, Args...>>>
    inline_fn_t(F&& f) noexcept {  // NOLINT(google-explicit-constructor)
        static_assert(sizeof(D) <= Capacity,
                      "inline_fn_t: callable exceeds the inline capacity (no heap fallback)");
        static_assert(alignof(D) <= alignof(void*),
                      "inline_fn_t: callable alignment exceeds the inline buffer's");
        static_assert(std::is_trivially_copyable_v<D> && std::is_trivially_destructible_v<D>,
                      "inline_fn_t: callable must be trivially copyable and destructible "
                      "(capture a pointer to the owner, not an owning object)");
        if constexpr (std::is_pointer_v<D>) {
            if (f == nullptr) return;
        }
        ::new (static_cast<void*>(buf_)) D(std::forward<F>(f));
        invoke_ = &invoke_impl<D>;
    }

    /** @brief Become empty. */
    inline_fn_t& operator=(std::nullptr_t) noexcept {
        invoke_ = nullptr;
        return *this;
    }

    /** @brief True when a callable is stored. */
    [[nodiscard]] explicit operator bool() const noexcept { return invoke_ != nullptr; }

    /** @brief Invoke the stored callable; precondition: not empty. */
    R operator()(Args... args) const { return invoke_(buf_, std::forward<Args>(args)...); }

   private:
    /** @brief Type-erased call through the buffer. */
    using invoke_fn_t = R (*)(const void*, Args&&...);

    /** @brief Call the `D` living in @p buf. */
    template <class D>
    static R invoke_impl(const void* buf, Args&&... args) {
        // The slot's operator() is const, as std::function's is; the stored
        // callable itself is invoked as non-const, which is what a mutable
        // lambda expects. The buffer is never a const object.
        return (*std::launder(static_cast<D*>(const_cast<void*>(buf))))(
            std::forward<Args>(args)...);
    }

    alignas(void*) unsigned char buf_[Capacity]{}; /**< @brief The inline callable. */
    invoke_fn_t invoke_ = nullptr;                 /**< @brief Null when empty. */
};

}  // namespace tr
