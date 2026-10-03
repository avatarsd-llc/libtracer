/**
 * @file
 * @brief `tr::function_ref_t` — a non-owning reference to a callable, for a callback that is
 *        called before the function taking it returns.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ADR-0083 Decision 9 (rulings Q10 and Q18) takes `std::function` out of core and gives each of
 * its three jobs one type:
 *
 * - a callback that is **called synchronously** — a visitor, a per-entry walk, a predicate —
 *   takes this `function_ref_t`: two words, no allocation, and it binds to a temporary lambda
 *   because the lambda outlives the call;
 * - a graph **hook** stays the non-owning `{fn, ctx}` slot `%tr::graph::hook_t`
 *   (RFC-0028 D10), whose context the caller keeps alive for as long as it is installed;
 * - any other **stored** callable holds its callable inline, in storage sized at compile time.
 *
 * `function_ref_t` is the C++23 stand-in for C++26 `std::function_ref`, reduced to what core
 * needs: no `const`/`noexcept` qualifiers in the signature and no `nontype` constructor.
 *
 * The class lives in the layer-neutral `tr` namespace (as `sink_slot_t` does): it depends on
 * nothing but the standard library and any layer may take one.
 */
#pragma once

#include <memory>
#include <type_traits>
#include <utility>

namespace tr {

/** @brief Primary template; only the function-type specialization is defined. */
template <class Sig>
class function_ref_t;

/**
 * @brief A non-owning, trivially copyable reference to a callable of signature `R(Args...)`.
 *
 * Holds the callable's address and a trampoline; a function pointer is held by value. It owns
 * nothing, so the referenced callable must outlive every call made through it. That is always
 * true for a parameter (a temporary lives until the end of the full expression that holds the
 * call) and is the caller's burden anywhere else, so do not store one; store a callable with
 * inline storage, or a hook, instead.
 *
 * Never empty: there is no default constructor and no null state, so a call needs no check.
 *
 * @tparam R    The return type.
 * @tparam Args The parameter types.
 */
template <class R, class... Args>
class function_ref_t<R(Args...)> {
   public:
    /**
     * @brief Refer to @p f, which is called as a non-const lvalue (or a const one, when @p f
     *        is const).
     */
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, function_ref_t> &&
                 !std::is_pointer_v<std::remove_cvref_t<F>> &&
                 !std::is_member_pointer_v<std::remove_cvref_t<F>> &&
                 !std::is_function_v<std::remove_reference_t<F>> &&
                 std::is_invocable_r_v<R, std::remove_reference_t<F>&, Args...>)
    constexpr function_ref_t(F&& f) noexcept  // NOLINT(google-explicit-constructor)
        : bound_{.obj = const_cast<void*>(static_cast<const void*>(std::addressof(f)))},
          call_(&call_object<std::remove_reference_t<F>>) {}

    /** @brief Refer to the function @p f, held by value. Precondition: @p f is not null. */
    template <class Fn>
        requires std::is_function_v<Fn> && std::is_invocable_r_v<R, Fn*, Args...>
    function_ref_t(Fn* f) noexcept  // NOLINT(google-explicit-constructor)
        : bound_{.fn = reinterpret_cast<void (*)()>(f)}, call_(&call_function<Fn>) {}

    /**
     * @brief Deleted: assigning a callable would leave this reference pointing at an object
     *        that dies at the end of the statement. Assign another `function_ref_t`.
     */
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, function_ref_t>)
    function_ref_t& operator=(F&&) = delete;

    /** @brief Call the referenced callable. */
    R operator()(Args... args) const { return call_(bound_, std::forward<Args>(args)...); }

   private:
    /** @brief What the reference holds: an object's address, or a function pointer. */
    union bound_t {
        void* obj;    /**< @brief The referenced callable object. */
        void (*fn)(); /**< @brief The referenced function, type-erased. */
    };

    /** @brief The trampoline type: recover the callable from the `bound_t` and call it. */
    using call_fn_t = R (*)(bound_t, Args&&...);

    /** @brief Call the object of type @p F that @p b points at. */
    template <class F>
    static R call_object(bound_t b, Args&&... args) {
        if constexpr (std::is_void_v<R>) {
            (*static_cast<F*>(b.obj))(std::forward<Args>(args)...);
        } else {
            return (*static_cast<F*>(b.obj))(std::forward<Args>(args)...);
        }
    }

    /** @brief Call the function of type @p Fn that @p b holds. */
    template <class Fn>
    static R call_function(bound_t b, Args&&... args) {
        if constexpr (std::is_void_v<R>) {
            reinterpret_cast<Fn*>(b.fn)(std::forward<Args>(args)...);
        } else {
            return reinterpret_cast<Fn*>(b.fn)(std::forward<Args>(args)...);
        }
    }

    bound_t bound_;  /**< @brief The referenced callable. */
    call_fn_t call_; /**< @brief Its trampoline. */
};

}  // namespace tr
