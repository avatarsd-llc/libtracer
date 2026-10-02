/**
 * @file
 * @brief The vertex handle, the vertex slot and the retirement-generation helpers: the names
 *        code needs to hold or compare a vertex reference without the graph.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A leaf on purpose (#1707): it includes no graph or vertex header, so code that only stores
 * a handle or checks a generation (the route handle, for one) does not parse `graph_t`.
 * `graph.hpp` and `vertex.hpp` include it back, so their users see no change.
 */
#pragma once

#include <cstdint>
#include <type_traits>

namespace tr::graph {

class graph_t;   // fwd-decl: vertex_handle_t names it as its sole constructing friend.
class vertex_t;  // fwd-decl: the handle wraps a pointer and never dereferences it here.

/**
 * @brief A non-owning, non-null, opaque handle to a graph vertex (ADR-0056).
 *
 * The caller-held result of @ref graph_t::register_vertex / @ref graph_t::find and the
 * token handed back into every `graph_t` data op (read / write / await / assign /
 * propagate / subscribe / history / field-write). Pointer-sized and trivially copyable,
 * so it loads and passes exactly like the `vertex_t*` it replaces — identical codegen —
 * but it exposes no `operator*` or raw-pointer accessor: a `vertex_t` is opaque L4 state,
 * never dereferenced by callers. Constructed ONLY by @ref graph_t (the `friend`), which
 * owns the pinned, pointer-stable, insert-only vertex map — so a handle always names a
 * live vertex for the graph's lifetime. There is no invalid/null state; "no such vertex"
 * is modelled by the `std::optional<vertex_handle_t>` @ref graph_t::find returns.
 */
class vertex_handle_t {
   public:
    /** @brief Two handles compare equal iff they name the same vertex. (`!=` is synthesized.) */
    [[nodiscard]] friend bool operator==(vertex_handle_t a, vertex_handle_t b) noexcept {
        return a.ptr_ == b.ptr_;
    }

   private:
    friend class graph_t;  // sole constructor + the only code that unwraps to `vertex_t*`.
    explicit vertex_handle_t(vertex_t* ptr) noexcept : ptr_(ptr) {}
    [[nodiscard]] vertex_t* get() const noexcept { return ptr_; }
    vertex_t* ptr_;
};

// The ADR-0056 zero-overhead claim, enforced: a handle is exactly a pointer.
static_assert(std::is_trivially_copyable_v<vertex_handle_t>);
static_assert(sizeof(vertex_handle_t) == sizeof(vertex_t*));

/**
 * @brief One node-scoped vertex reference — a slot index AND the generation stamping it
 *        (RFC-0024 §4.4 / §6.4).
 *
 * The pair is the unit a mint hands out, never two separately-read numbers: an index without
 * the generation that was current when it was read is not a reference to a vertex, it is a
 * reference to whatever the slot holds later. Retirement moves the generation and takes the
 * graph's map lock uniquely, so reading both under one hold is what makes the pair name a
 * single tenancy of the slot.
 */
struct vertex_slot_t {
    std::uint32_t index = 0;      /**< @brief Position in the node-scoped vertex index. */
    std::uint32_t generation = 0; /**< @brief The slot's retirement generation at that moment. */

    /** @brief Value equality — both fields, since either alone is not a reference. */
    [[nodiscard]] friend constexpr bool operator==(vertex_slot_t, vertex_slot_t) = default;
};

/**
 * @brief The terminal value of a vertex's retirement generation (RFC-0024 §4.4 rule 3).
 *
 * The generation SATURATES here rather than wrapping. Wrapping is the #603 failure class
 * transposed onto the guard: a stale reference whose generation came back around compares
 * equal again and the operation is delivered into whatever now occupies the vertex — a
 * misroute, not a drop. At this value the counter stops moving, the vertex is permanently
 * unbindable, and every bound-path mint for it declines and leaves the caller on the
 * canonical form, which always works.
 */
inline constexpr std::uint32_t kGenerationSaturated = 0xFFFFFFFFu;

/**
 * @brief The generation after @p g — saturating at `kGenerationSaturated` (RFC-0024 §4.4).
 *
 * The whole of the no-wrap rule, as one total function, so the rule can be exercised at the
 * ceiling: reaching it through the retire path takes 2^32 retirements of one vertex, which no
 * test performs, and a guard nothing can reach is a guard nothing is checking.
 */
[[nodiscard]] constexpr std::uint32_t saturating_next_generation(std::uint32_t g) noexcept {
    return g == kGenerationSaturated ? kGenerationSaturated : g + 1;
}

static_assert(saturating_next_generation(0) == 1);
static_assert(saturating_next_generation(kGenerationSaturated - 1) == kGenerationSaturated);
// The clause the whole element shape rests on: at the ceiling the counter STOPS. If this ever
// read 0, a stale bound-path element would compare equal again and the operation would land on
// the vertex's successor — #603's misroute, with the guard in place of the address.
static_assert(saturating_next_generation(kGenerationSaturated) == kGenerationSaturated);

/**
 * @brief Does a bound-path element stamped @p element_gen still name the tenancy a slot whose
 *        stamp is @p slot_gen is holding (RFC-0024 §5.1 step 2)?
 *
 * The whole of the deref's generation rule, as one total function, for the reason
 * `saturating_next_generation` is one: the interesting case is the CEILING, and reaching
 * it through the retire path takes 2^32 retirements of one vertex, which no test performs.
 * A guard nothing can reach is a guard nothing is checking — and this one had that shape.
 *
 * Below the ceiling the rule is plain equality, and it is safe because generations only move
 * forward: a stale element compares lower and can never become valid again by waiting. AT the
 * ceiling the counter stops, so that argument stops with it. A saturated element would keep
 * comparing equal to its slot for the rest of the node's life, through every subsequent
 * retire and revive — tenant A's operations delivered into B, then C, then D, with staleness
 * detection permanently dead for that slot. So saturation is refused OUTRIGHT, on the side
 * that honours an element and not only on the side that issues one; that is what makes
 * "permanently unbindable" (§4.4 rule 3) a property of the vertex.
 */
[[nodiscard]] constexpr bool bound_generation_matches(std::uint32_t slot_gen,
                                                      std::uint32_t element_gen) noexcept {
    return element_gen != kGenerationSaturated && element_gen == slot_gen;
}

static_assert(bound_generation_matches(0, 0));
static_assert(!bound_generation_matches(1, 0), "a stale element compares lower and is refused");
static_assert(!bound_generation_matches(0, 1), "a forged-ahead element is refused too");
// THE clause the saturation rule rests on, and the one a plain `==` gets wrong: at the ceiling
// the slot and the element agree and the answer is STILL no. Written as `==` this reads true,
// and the element would validate forever across every retire — #603's misroute with the guard
// in place of the address.
static_assert(!bound_generation_matches(kGenerationSaturated, kGenerationSaturated));

}  // namespace tr::graph
