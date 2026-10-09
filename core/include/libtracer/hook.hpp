/**
 * @file
 * @brief `hook_t` — the ONE callback idiom of the graph's user seams (RFC-0028 D10, slice 7):
 *        a `{fn, ctx}` pair, 16 B on the host, and `thunk`, the helper that points one at a
 *        callable the caller keeps alive.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Before this header the vertex seams (`handlers_t`, the seam block, the app-field apply seam,
 * the admission filters and the child-type catalog) were `std::function`s while the hot edge
 * (`subscriber_fn_t`), the receive slot and the three configuration sinks were already
 * `{fn, ctx}` pairs (ADR-0047). Two idioms, and the heavier one cost 32 B per seam on the host
 * plus a heap block for any capture past the small-buffer size. `hook_t` is the edge's shape
 * given a signature: a plain function pointer whose first argument is the caller's context,
 * and that context.
 *
 * **Lifetime is the caller's.** A hook owns nothing: `ctx` must outlive every call the graph
 * can make through it — for a vertex seam, until the vertex is retired or the graph destroyed,
 * and past a retire for a call that loaded the seam just before it. An owner that frees `ctx`
 * on a threaded node hands that free to `graph_t::park_release`, which runs it once no such
 * call can remain. The usual shapes, in order of preference:
 *
 * @code
 * // 1. A captureless lambda (or free function) and an object you already own:
 * h.on_write = {[](void* self, const value_t& v, const write_ctx_t& w) -> result_t<void> {
 *                   return static_cast<sensor_t*>(self)->apply(v, w);
 *               },
 *               this};
 * // 2. A captureless lambda with the seam's own signature — it has no state, so nothing
 * //    needs to stay alive and a temporary is fine:
 * h.on_children = tr::graph::thunk([]() -> result_t<view_t> { ... });
 * // 3. A capturing lambda kept alive as long as the vertex (a member, or a local that
 * //    outlives the graph in a test):
 * auto on_write = [&](const value_t& v, const write_ctx_t&) -> result_t<void> { ... };
 * h.on_write = tr::graph::thunk(on_write);
 * @endcode
 */
#pragma once

#include <type_traits>

namespace tr::graph {

/**
 * @brief A callable with NO state: an empty, default-constructible type (a captureless
 *        lambda). @ref tr::graph::thunk rebuilds one per call, so its hook needs nothing kept
 * alive.
 */
template <class F>
concept stateless_callable_t = std::is_empty_v<std::remove_cvref_t<F>> &&
                               std::is_default_constructible_v<std::remove_cvref_t<F>>;

/**
 * @brief THE graph callback idiom (RFC-0028 D10): a `{fn, ctx}` pair over the call signature
 *        `Sig`. Declared only; the `R(A...)` specialisation below is the definition.
 */
template <class Sig>
struct hook_t;

/**
 * @brief A `{fn, ctx}` callback slot for the signature `R(A...)`.
 *
 * An aggregate: `{fn, ctx}` builds one, and a captureless lambda whose first parameter is
 * `void*` converts to @ref fn_t implicitly. Default-constructed it is EMPTY (`fn == nullptr`),
 * which every seam treats as "not installed". Trivially copyable — copying a hook copies two
 * words and never allocates.
 */
template <class R, class... A>
struct hook_t<R(A...)> {
    /** @brief The function half: the caller's context first, then the seam's arguments. */
    using fn_t = R (*)(void* ctx, A... args);

    fn_t fn = nullptr;   /**< @brief The callback, or null when the seam is not installed. */
    void* ctx = nullptr; /**< @brief Handed back as @ref fn's first argument; caller-owned. */

    /** @brief True iff a callback is installed. */
    [[nodiscard]] explicit operator bool() const noexcept { return fn != nullptr; }

    /** @brief Call the installed callback. Precondition: `*this` is non-empty. */
    R operator()(A... args) const { return fn(ctx, static_cast<A&&>(args)...); }
};

/**
 * @brief The adapter @ref tr::graph::thunk returns: converts to ANY `hook_t<R(A...)>` whose
 * signature the callable accepts, with `ctx` pointing at that callable — or null for a
 *        %stateless_callable_t, whose trampoline builds its own.
 */
template <class F>
class thunk_t {
   public:
    /** @brief Refer to @p f. Unless `F` is stateless, it must outlive every hook built from
     *         this adapter. */
    explicit thunk_t(F& f) noexcept : f_(&f) {}

    /** @brief The hook: `fn` is a generated trampoline, `ctx` the callable's address (null
     *         when the callable is stateless). */
    template <class R, class... A>
    operator hook_t<R(A...)>() const noexcept {  // NOLINT(google-explicit-constructor)
        if constexpr (stateless_callable_t<F>) {
            return hook_t<R(A...)>{&call<R, A...>, nullptr};
        } else {
            return hook_t<R(A...)>{&call<R, A...>,
                                   const_cast<void*>(static_cast<const volatile void*>(f_))};
        }
    }

   private:
    /** @brief The trampoline: recover the callable from `ctx` (or build the stateless one)
     *         and invoke it. */
    template <class R, class... A>
    static R call(void* ctx, A... args) {
        if constexpr (stateless_callable_t<F>) {
            (void)ctx;
            return std::remove_cvref_t<F>{}(static_cast<A&&>(args)...);
        } else {
            return (*static_cast<F*>(ctx))(static_cast<A&&>(args)...);
        }
    }

    F* f_; /**< @brief The referenced callable. */
};

/**
 * @brief Point a @ref hook_t at a callable (RFC-0028 D10's `tr::graph::thunk<F>`).
 *
 * The hook stores `&f`, so @p f must outlive every call made through it. For a seam that only
 * needs an object you already own (`this`), prefer the two-word aggregate
 * `{captureless_lambda, this}`: nothing extra to keep alive.
 */
template <class F>
[[nodiscard]] thunk_t<F> thunk(F& f) noexcept {
    return thunk_t<F>{f};
}

/**
 * @brief A hook over a STATELESS callable passed as a temporary — safe, because the
 *        trampoline builds the callable itself and `ctx` is null.
 */
template <stateless_callable_t F>
    requires(!std::is_lvalue_reference_v<F>)
[[nodiscard]] thunk_t<std::remove_cvref_t<F>> thunk(F&& f) noexcept {
    static_assert(std::is_empty_v<std::remove_cvref_t<F>>);
    return thunk_t<std::remove_cvref_t<F>>{f};  // f_ is never dereferenced for a stateless F
}

/** @brief Deleted: a hook over a temporary callable WITH state would dangle when the statement
 *         ends. Keep the callable alive in a named object and pass that. */
template <class F>
    requires(!stateless_callable_t<F>)
void thunk(const F&& f) = delete;

}  // namespace tr::graph
