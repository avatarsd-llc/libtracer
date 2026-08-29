/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * An L4 graph vertex: a named, addressable position holding a value, a bounded
 * history, or a user handler (docs/reference/11 §roles). Pinned in place (the
 * atomic LKV slot is non-movable, and the address indexes the lock-stripe
 * table); always handled via a
 * vertex_handle_t returned by graph_t::register_vertex (ADR-0056). The read/write LKV hot path is
 * lock-free (an atomic shared_ptr swap, the orderings M2 already pays for); the
 * shared lock STRIPE (#361 §2, `vertex_stripe_of`) guards only the history ring,
 * the subscriber list (M3b), the ACL state, and the await waiter accounting.
 *
 * This header is the vertex CORE and nothing else (#868). It used to fuse five concerns into
 * one 3421-line hub that `graph.hpp` — and therefore every net-plane TU — pulled whole, so a
 * change to any one of them re-read all five. They now sit where they belong and are included
 * back here, because `vertex_t` embeds or owns each of them:
 *
 *   - libtracer/app_fields.hpp    — the RFC-0010 field tables (ADR-0058 storage classes)
 *   - libtracer/subscriber.hpp    — the subscription edges: slots, snapshots, published array
 *   - libtracer/vertex_stripe.hpp — the process-global lock-stripe table
 *   - libtracer/acl_ace.hpp       — the ACE records, which `vertex_ext_t` stores by value
 *
 * The ACE records get their own header rather than joining `security_acl.hpp`: the records
 * are UPSTREAM of this file and the evaluation/codec that reads them is DOWNSTREAM, so
 * fusing the two would pull the codec into every net-plane TU. See `acl_ace.hpp`.
 *
 * Including this header therefore still declares everything it declared before EXCEPT the
 * ACL policies and codec: the split is a code MOVE, and the re-includes above are what keeps
 * it one.
 */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "libtracer/acl_ace.hpp"
#include "libtracer/app_fields.hpp"
#include "libtracer/config.hpp"
#include "libtracer/edge_pin.hpp"
#include "libtracer/lkv_slot.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/path.hpp"
#include "libtracer/path_ref.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/status.hpp"
#include "libtracer/subscriber.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/vertex_stripe.hpp"
#include "libtracer/view.hpp"

namespace tr::graph {

/**
 * @brief A build that binds `%hazard_slot_t` must be on a target whose claim table is
 *        lock-free (#899).
 *
 * This is the one place both halves of the question are visible: `%lkv_slot_t` (the binding,
 * from the generated `%config.hpp`) and `detail_hp::claim_word_t` (the table's word, from
 * `%lkv_slot.hpp`). Asserting it inside `%lkv_slot.hpp` instead is what broke both esp32c3
 * legs — that header is pulled in by every consumer of a vertex, including `rv32imc` targets
 * that have no atomic instructions at all and never bind `%hazard_slot_t`, and a
 * `static_assert` is evaluated when the header is PARSED, not when the domain is emitted.
 *
 * A thread that failed to claim re-probes this table on every operation, so a probe that took
 * a libatomic lock would serialize the very readers `%hazard_slot_t` exists to keep
 * lock-free — the binding would be actively worse than the `sp_atomic_slot_t` default rather
 * than merely no better.
 */
static_assert(!std::is_same_v<lkv_slot_t, hazard_slot_t> || detail_hp::kClaimWords == 0 ||
                  std::atomic<detail_hp::claim_word_t>::is_always_lock_free,
              "this target binds hazard_slot_t but cannot claim a hazard index without taking "
              "a lock — bind sp_atomic_slot_t here, or build for a target with lock-free "
              "atomics of pointer width");

/**
 * @brief The memory order of the DELIVERY-SKIP Dekker pair (#635, #1140) — the one order in
 *        this library whose value is a correctness precondition on the TARGET rather than a
 *        local choice, so it is named once instead of spelled at each of its two sites.
 *
 * The pair is `vertex_t::own_subs_ordered` / `vertex_t::bump_own_subs`, and the argument for
 * `seq_cst` is written out at the former: a publisher that SKIPS a delivery on a zero count
 * must be in one total order with a subscriber that is concurrently taking ADR-0049's
 * durability latch, or the new subscriber latches the pre-write value and the publish that
 * raced it reaches nobody. Both halves take this order, so the pairing has one spelling and
 * cannot be half-weakened by an edit that reaches only one site.
 *
 * It is `constexpr` and it is asserted below because relaxing it is UNTESTABLE where CI
 * mostly runs: on x86-64 the `seq_cst` LKV store lowers to a locked `xchg`, so even a relaxed
 * load of zero cannot observe the pre-store world and the suite stays green through the
 * ablation. The `ubuntu-24.04-arm` leg (#1140) is the coverage half of that answer; this
 * constant and @ref tr::graph::kWeaklyOrdered are the refusal half.
 */
inline constexpr std::memory_order kDeliverySkipOrder = std::memory_order_seq_cst;

/**
 * @brief A weakly-ordered target may not weaken the delivery-skip pair (#1143).
 *
 * The precondition #1140 could previously only state in prose, now compiler-checked: on a
 * target that reorders a later relaxed load ahead of an earlier `seq_cst` store — every
 * shipped MCU target, and any many-core aarch64 host — the skip gate's two halves must share
 * one total order, which nothing weaker than `seq_cst` gives them. Ablating
 * `kDeliverySkipOrder` now fails the BUILD on those targets instead of passing the suite
 * on a TSO host and shipping a lost delivery to the ones CI cannot run.
 *
 * A build that sets @ref tr::graph::kWeaklyOrdered to `false` states that its target orders
 * that pair in hardware and takes the ablation's consequences on itself; nothing here selects
 * a weaker order on its behalf.
 */
static_assert(!kWeaklyOrdered || kDeliverySkipOrder == std::memory_order_seq_cst,
              "this target is weakly ordered (tr::graph::kWeaklyOrdered), so the delivery-skip "
              "gate vertex_t::own_subs_ordered and its subscriber-side bump must both be "
              "seq_cst: anything weaker leaves a publisher's skip and a concurrent "
              "subscriber's ADR-0049 latch load out of one total order, and the racing publish "
              "reaches nobody (#635, #1140). Restore kDeliverySkipOrder, or -- only for a "
              "target whose hardware really does order it -- set kWeaklyOrdered = false in the "
              "libtracer/config_override.hpp fragment.");

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

// L1 types this layer consumes (upward dependency on tr::view, docs/adr/0016 §2).
using view::rope_t;
using view::segment_ptr_t;
using view::view_t;

/** @brief A vertex's behavioral role (docs/reference/11 §roles). Byte-wide: it packs
 *         into `vertex_t`'s flag byte group (#361 diet — 3 values need no int). */
enum class role_t : std::uint8_t {
    STORED_VALUE, /**< @brief Role 1: last-writer-wins; holds the last-written value. */
    STREAM,       /**< @brief Role 2: the CONSUMER's bounded history ring — a queue the
                       RECEIVING vertex owns (RFC-0025 §4.6.1 Amendment 2: "a producer
                       never queues"), bounded in BYTES by that vertex's own injected
                       `tr::mem::block_source_t` and retained to a depth declared
                       owner-side by `graph_t::set_history_depth` (RFC-0022 §3.C). */
    HANDLER,      /**< @brief Roles 3-7: user `on_read` / `on_write` supplies the behavior. */
};

/**
 * @brief An owning reference to a vertex's PUBLISHED value — what @ref graph_t::read and
 *        @ref graph_t::await hand back.
 *
 * The value a vertex publishes is already refcounted: the LKV slot holds it as a
 * `std::shared_ptr<const rope_t>`, and the policy contract in `%lkv_slot.hpp` fixes that shape
 * because `load()` must return an OWNING handle. A read therefore has a choice — hand the
 * caller that reference, or copy the rope out of it. Copying is not free: a rope copy clones
 * one `segment_ptr_t` per link, and each clone is a contended refcount RMW on a line every
 * reader of that vertex shares, so it costs more as links grow AND as readers grow.
 *
 * Measured on the real path, both arms alternating inside ONE binary (24-thread host, 102
 * paired samples): median **1.37x** aggregate, 89/102 samples favouring the reference, and p50
 * improving most where it hurts most — 2,104 ns to 1,193 ns at sixteen readers on one shared
 * vertex. The composed BRANCH read, which must build a value rather than share one, measured
 * **1.00x (15/30)**: the shape that cannot benefit does not pay either.
 *
 * The rule this draws: **a read of a PUBLISHED value returns a reference to it; a read that
 * COMPOSES a new value returns the value.** That is why @ref graph_t::read_children_folded and
 * its siblings still return a `rope_t` — there is no published object for them to reference.
 *
 * Holding one keeps that value alive, exactly as the reader's own reference did before. Under
 * an injected `std::pmr::memory_resource` that is a real obligation: the value was allocated
 * from the graph's resource, so an outstanding reference pins it (ADR-0069, deferred
 * reclamation).
 */
class value_ref_t {
   public:
    value_ref_t() = default;

    /** @brief Wrap a published value's handle. */
    explicit value_ref_t(std::shared_ptr<const rope_t> p) noexcept : p_(std::move(p)) {}

    /**
     * @brief Take ownership of a freshly COMPOSED value, giving it a published value's shape.
     *
     * The composed branch read builds a rope no vertex published; this is what lets it answer
     * the same signature. It allocates a control block, which the published path does not —
     * measured neutral (1.00x over 30 paired samples), because a subtree walk dominates it.
     */
    [[nodiscard]] static value_ref_t composed(rope_t&& r) {
        return value_ref_t{std::make_shared<const rope_t>(std::move(r))};
    }

    /** @brief The referenced value. Undefined if this reference is empty. */
    [[nodiscard]] const rope_t& operator*() const noexcept { return *p_; }
    /** @brief Member access on the referenced value. */
    [[nodiscard]] const rope_t* operator->() const noexcept { return p_.get(); }
    /** @brief The referenced value, or null. */
    [[nodiscard]] const rope_t* get() const noexcept { return p_.get(); }
    /** @brief Whether this reference names a value. */
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(p_); }

   private:
    std::shared_ptr<const rope_t> p_;
};

/**
 * @brief The per-call context a write carries into a HANDLER's `on_write` (#375).
 *
 * A HANDLER is the one seam where application code REACTS to a write, so it is the one
 * seam that needs to know WHO wrote. The graph already resolved that identity one stack
 * frame earlier — the ACL gate (`graph_t::acl_allows`) runs immediately before the handler,
 * on the same value — so this type hands the handler the datum the gate just used rather
 * than making it re-derive one. It is the ACL subject-table integration point (ADR-0018 —
 * authorization over a pluggable subject token; ADR-0082 for why the subject is a claim of
 * its own and not a spelling of `peer_named`): a handler that keys its own policy off
 * @ref subject keys it off exactly what the vertex's `:acl` was evaluated against.
 *
 * @note This sentence used to cite **RFC-0010**, which is the wrong document — RFC-0010 is
 *       *owner-writable application property fields* (the field descriptor table, the
 *       reserved `settings.app` namespace and owner-defined `:schema`) and says nothing
 *       about subjects or access control. The subject-token model is ADR-0018's; the
 *       decoupling of that token from peer addressing is ADR-0082's, which itself points
 *       back at this comment as the integration point it feeds. Corrected with #375 Part 2.
 *
 * @warning LIFETIME — @ref subject is BORROWED for the duration of the call, the SAME
 *          contract the `rope_t&` alongside it carries: COPY IF RETAINED. It views bytes
 *          owned by the router's inbound frame or by the caller's own storage, and both are
 *          gone the moment `on_write` returns. Stashing the `string_view` in a member, a
 *          map key, or a queued work item is a DANGLING read, not merely a stale one. Take
 *          a `std::string` (or the token's bytes) if the identity must outlive the call.
 */
struct write_ctx_t {
    /**
     * @brief The resolved SUBJECT token of the writer — the ACL model's `subject → rights`
     *        principal (CONTEXT.md §Access control, ADR-0018).
     *
     * EMPTY means the LOCAL HOST: the owner's own in-process write through the graph API.
     * That is not a magic string but the very discriminator the ACL gate runs on — the empty
     * caller context is the trusted-by-convention channel `graph_t::acl_allows` short-circuits
     * BEFORE any resolver runs (#905), and a remote writer, which always carries a non-empty
     * context, cannot spell it. Prefer @ref is_local_owner to comparing against `""`.
     *
     * @note There is no `OWNER@` sentinel and there must not be one: ADR-0020's erratum
     *       (#1033) withdrew that name because no evaluator ever special-cased it, so an
     *       `OWNER@` ACE matched nobody and LOCKED the vertex it was written to delegate.
     *       The owner sentinel here is the EMPTY token, which no ACE can spell.
     *
     * @note Non-empty, this is the operation's caller context exactly as the gate saw it.
     *       The token is PLUGGABLE (ADR-0018, ADR-0045 raw-key ed25519 TOFU) — a stronger
     *       credential slots in without changing this seam or the ACL model.
     */
    std::string_view subject;

    /** @brief True iff this write came from the LOCAL HOST (the owner's own API call) —
     *         i.e. @ref subject is the empty owner token. */
    [[nodiscard]] constexpr bool is_local_owner() const noexcept { return subject.empty(); }
};

/**
 * @brief One row of a handler vertex's payload-type → required-ACL-right table
 *        (RFC-0014 Amendment 2).
 *
 * The selector is the written TLV's **type only** — never its content. That is what keeps the
 * declaration ahead of the gate: `graph_t::write_impl` reads one byte of the value's leading
 * link to pick the right, and no user code runs before the ACL check.
 */
struct payload_right_t {
    wire::type_t type; /**< @brief The written value's LEADING TLV type. */
    acl_right_t right; /**< @brief The right the write gate demands for that type. */
};

/**
 * @brief What an ADMISSION filter decided about one write, BEFORE it becomes state.
 *
 * Three outcomes in one value, and no fourth:
 *
 * - a value holding `std::nullopt` — **accept as written**. The rope the writer handed in is
 *   stored unchanged; this is the zero-work answer and the one a pure validator gives.
 * - a value holding a rope — **accept, normalised**. THAT rope is stored instead, and it is
 *   what every later reader sees: the last-known-value, an `await` wake, a composed read, and
 *   every subscriber delivery. The filter owns the canonical form; the writer's spelling never
 *   lands anywhere.
 * - `std::unexpected(status)` — **refuse**. Nothing is stored, no subscriber is delivered, the
 *   prior last-known-value stands, and @p status reaches the writer exactly as a HANDLER's
 *   `on_write` refusal does (`TYPE_MISMATCH` for a value the vertex cannot represent,
 *   `PERMISSION_DENIED` for a policy the ACL cannot express, etc.).
 */
using admission_t = result_t<std::optional<rope_t>>;

/**
 * @brief User behavior for a Handler-role vertex.
 *
 * `on_children` additionally applies to ANY role: when set, a read of the vertex's
 * `:children[]` field serves this synthesized member listing (a complete POINT TLV view)
 * INSTEAD of enumerating registered child vertices — the ADR-0044 seam by which a
 * transport/connection vertex lists its live bus peers without ever creating a vertex for
 * them. The value seam is rope-typed (ADR-0053 §6): `on_read` supplies the vertex value as
 * the rope it is (a contiguous scalar is the single-link case), `on_write` receives the
 * written value without a flatten copy.
 */
struct handlers_t {
    std::function<result_t<rope_t>()> on_read; /**< @brief Supplies the vertex value on read. */
    /** @brief Receives the written value and the writer's @ref write_ctx_t (#375). Both
     *         arguments are borrowed for the call only — copy if retained. */
    std::function<result_t<void>(const rope_t&, const write_ctx_t&)> on_write;
    std::function<result_t<view_t>()> on_children; /**< @brief Synthesized `:children[]` listing. */
    /**
     * @brief The ADMISSION seam of a RETAINING vertex: runs BEFORE the write becomes state,
     *        and decides whether — and in what form — it does (`admission_t`).
     *
     * The gap it closes. `on_write` is the HANDLER role's seam, and a HANDLER retains nothing:
     * choosing it to validate a write meant giving up the last-known-value, the `await` wake and
     * the whole composed-read surface that make a `STORED_VALUE` the graph-authoritative form of
     * a datum. So a consumer had to pick RETENTION or VALIDATION and could not have both. This
     * seam is the same refusal power on the storing roles, taken at the one place every store
     * goes through (`graph_t::store_value`) rather than bolted onto one door.
     *
     * WHERE it runs, exactly: after the write gate's ACL decision, before `vertex_t::store`,
     * therefore before the sequence bump, before any `await` wake, before a STREAM ring
     * admission and before ANY subscriber delivery — local, bubbled or remote. A refused write
     * is unobservable except as the writer's error; a normalised one is observable ONLY in its
     * normalised form.
     *
     * WHICH writes it sees: every write that would STORE at this vertex, whatever the door.
     * `write` and `assign` alike, a `FWD{WRITE}` terminus, a delivery landing here from an
     * inbound edge (the fan-in write), and this vertex's own slice of a branch-POINT
     * decomposition. Admission is a property OF THE VERTEX, not of a channel — a vertex whose
     * invariant only held against remote writers would not hold. The owner's own writes are
     * included and are told apart by `write_ctx_t::is_local_owner`, which is what a filter that
     * wants to admit the owner unconditionally tests.
     *
     * WHICH it does NOT see: a HANDLER-role vertex (`on_write` is its seam and already has this
     * power — installing both, the role's `on_write` runs and this does not), and the app-field
     * plane, which is a different plane with its own seam (@ref on_app_field_admit).
     *
     * COST. Unset ⇒ **nothing**: the store path tests one bit of a flags word the write path
     * already holds and never loads the seam block. That bit is the whole per-vertex cost.
     *
     * @warning Both arguments are BORROWED for the call — the same contract `on_write` carries.
     *          Copy what you retain. The seam runs on the WRITER's thread with no vertex lock
     *          held, so it may re-enter the graph, and it is on the hot write path: a filter
     *          that blocks blocks the writer.
     */
    std::function<admission_t(const rope_t&, const write_ctx_t&)> on_admit;
    /**
     * @brief The app-field plane's admission seam (RFC-0010 §A.3), the field-shaped twin of
     *        @ref on_admit — runs BEFORE a declared `:settings.app.<name>` write stores its
     *        bytes, and may refuse it or normalise it.
     *
     * Called with the field's key (below `settings.app.`) and the written TLV, after the ACL
     * gate and after the RFC-0010 §A.3 writability check, before `app_field_store`. Return the
     * view handed in to store it verbatim (the pre-existing behaviour), a DIFFERENT view to
     * store those bytes instead, or `std::unexpected(status)` to refuse — in which case nothing
     * is stored, the field keeps its prior bytes, @ref on_app_field_write does NOT fire, and the
     * status is the writer's answer.
     *
     * @warning A returned view is READ during the call that returned it — the store copies the
     *          bytes out before returning — so it may point at storage the filter owns, but that
     *          storage must outlive the return. Unset ⇒ bytes store verbatim, as before.
     */
    std::function<result_t<view_t>(std::string_view name, const view_t& value)> on_app_field_admit;
    /**
     * @brief The owner apply seam (RFC-0010 §A.3): fires after a declared
     *        `:settings.app.<name>` field write stored its bytes, with the field's key
     *        (below `settings.app.`) and the written TLV — OUTSIDE the vertex lock, so it
     *        may re-enter the graph (apply the config, restructure children, then ANNOUNCE
     *        the change with an ordinary data write per §C — the field write itself never
     *        wakes `await` and never propagates). Unset ⇒ the bytes just store (a passive
     *        metadata field).
     */
    std::function<void(std::string_view name, const view_t& value)> on_app_field_write;
    /**
     * @brief OPTIONAL payload-type → required-ACL-right table (RFC-0014 Amendment 2) — the
     *        general contract by which a control vertex demands something other than plain
     *        `WRITE` for a given written TLV type.
     *
     * Empty (the default) ⇒ every write to the vertex gates on `acl_right_t::WRITE`, which is
     * both today's behaviour and today's cost: a vertex that declares nothing carries not one
     * byte for this (the rows are moved out at registration and live on the graph — see
     * `graph_t::declare_payload_rights`), and `graph_t::write_impl` stops at one relaxed
     * flag-bit test on a word the write path already holds. A row whose
     * `%payload_right_t::type` equals the written value's leading TLV type supplies the
     * right demanded instead; an unmatched type (and a value whose leading link cannot be read,
     * e.g. a device-memory link) falls back to `WRITE`. Rows are scanned in order, first match
     * wins — the table is a handful of entries on a control vertex, never a hot-path structure.
     *
     * The refusal is still the ONE write gate's, counted into the single-sited
     * `delivery_drops_t::denied`: this declaration changes WHICH right is demanded, never
     * where the demand is made. The transport creator endpoint is the first user
     * (`SPEC`→`CREATE`, `NAME`→`WRITE`, RFC-0014 §5); an application subtree-owner that
     * implements create-on-write is the next (RFC-0003).
     */
    std::vector<payload_right_t> payload_rights;
};

/**
 * @brief The internal, lazily-allocated STORAGE of a vertex's VALUE seam (ADR-0058 Step 2)
 *        — the seams `handlers_t` carries minus the two app-field ones.
 *
 * Split off from the public @ref handlers_t input so a vertex that installs none of the
 * four never allocates these ~96 B of `std::function`: the block lives behind a lazily
 * published pointer in the extension block, null unless at least one of `on_read`,
 * `on_write`, `on_children` was given. Allocation is keyed on
 * that PRESENCE, not on `role_t` — `adopt_identity` never consults the role — so a `STORED_VALUE`
 * vertex given an `on_children` (the `/net/<module>/<name>` identity vertex of a bus link) does
 * carry one, and a `HANDLER` vertex registered with an empty @ref handlers_t does not.
 * Which of the three is ever CONSULTED is a separate, per-seam question: `on_read` /
 * `on_write` run only on a HANDLER-role target, while `on_children` serves the
 * synthesized listing whatever the role. `on_app_field_write`
 * co-occurs with app fields, not the value seam, so it moved to @ref app_field_group_t.
 * The two ADMISSION filters live on the GRAPH, not here, for the reason
 * `graph_t::admissions_` states — the same reason the payload-right rows do.
 * Set once at registration (`vertex_t::adopt_identity`), read lock-free thereafter.
 */
struct value_handlers_t {
    std::function<result_t<rope_t>()> on_read; /**< @brief Supplies the vertex value on read. */
    /** @brief Receives the written value and the writer's @ref write_ctx_t (#375). Both
     *         arguments are borrowed for the call only — copy if retained. */
    std::function<result_t<void>(const rope_t&, const write_ctx_t&)> on_write;
    std::function<result_t<view_t>()> on_children; /**< @brief Synthesized `:children[]` listing. */
};

/**
 * @brief Per-VERTEX propagation policy (value-agnostic; RFC-0008 §C).
 *
 * Governs whether an ANCESTOR's propagate sweep includes this vertex — NOT a
 * per-subscriber value filter (there is no byte comparison; ADR-0053 §1, a vertex never
 * parses its bytes). `assign` and a DIRECT propagate on the vertex itself are never gated
 * by it. Held as vertex state (default IF_NEWER); wire config via the vertex `:settings`
 * is deferred. Numeric filtering (deadband) remains an application filter vertex (ADR-0021
 * sibling), never a field here.
 */
enum class delivery_mode_t : std::uint8_t {
    /** @brief Default: an ancestor sweep includes this vertex only if it was assigned since
     *         the last covering sweep — the structural coalescing flush (RFC-0008 §B). */
    IF_NEWER = 0,
    /** @brief An ancestor sweep ALWAYS includes this vertex's current value (a sweep-driven
     *         keepalive; the producer's timer sets the rate). */
    UNCONDITIONAL = 1,
    /** @brief An ancestor sweep NEVER includes it; deliverable only by a direct propagate
     *         on the vertex itself. */
    EXPLICIT = 2,
};

/**
 * @brief How a `graph_t::propagate` sweep EMITS what it selected (RFC-0025 §4.1.2, Amendment 3
 *        clause 5) — a producer-side choice about framing, never a subscription negotiation.
 *
 * Orthogonal to `%delivery_mode_t` — that mode decides WHICH vertices a sweep selects,
 * this one decides how the selection reaches the wire. Neither is a per-subscriber knob, and
 * neither is readable or writable by a peer — the producer owns cadence and framing (RFC-0005
 * §Motivation-3, RFC-0025 §3).
 */
enum class emission_mode_t : std::uint8_t {
    /** @brief The DEFAULT, unchanged: one `FWD{WRITE}` per selected vertex (RFC-0008 §D). */
    PER_VERTEX = 0,
    /** @brief ONE branch-write frame for the swept subtree — the RFC-0016 `POINT` tree, node
     *         shape byte-for-byte RFC-0005 §B's, root carrying its leading `NAME`. One frame
     *         per SUBTREE and never a container across several (the retired-LIST ban,
     *         RFC-0005 §E). */
    FOLD = 1,
};

/**
 * @brief One entry of a receiving vertex's STREAM ring: the value, plus the RESERVATION it
 *        was admitted under (RFC-0025 §4.6.1 clause 3).
 *
 * The reservation is the whole point, and the thing most easily misread. Admission calls
 * `tr::mem::block_source_t::try_alloc(retained_bytes)` on the RECEIVING vertex's own source
 * and holds the block until the entry retires (trim, drain-past, revert, destruction), at
 * which point it is released. That bounds **admission**, in bytes, against a budget the
 * receiver injected.
 *
 * It does **NOT** bound PLACEMENT. The payload never moves: @ref value stays exactly the
 * `shared_ptr` the publish handed out, in whatever allocator the value backend gave it, so
 * the zero-copy handoff is preserved and a ring append is still a refcount bump. Physical
 * placement migration is the later #873 family, explicitly out of scope here. A reader who
 * assumes the ring's bytes physically move into the injected source will be wrong, and the
 * wrongness is expensive.
 */
struct ring_entry_t {
    /** @brief The published value — a refcount share of the LKV, never a byte copy. */
    std::shared_ptr<const rope_t> value;
    /** @brief The admission reservation, or `nullptr` for an entry admitted at zero cost.
     *         Released with @ref bytes and @ref kAlign, the sized-reclaim contract. */
    void* token = nullptr;
    /** @brief The reserved width, as passed to `try_alloc` — required to release it. */
    std::size_t bytes = 0;
    /** @brief True iff a shed happened immediately BEFORE this entry: the in-order
     *         `tr::flow::address_shift_gap` marker of RFC-0025 §4.4/§4.5, so a consumer
     *         draining the ring learns where the discontinuity is, not merely that one
     *         happened. */
    bool gap_before = false;
    /** @brief The alignment every reservation is taken and released at. */
    static constexpr std::size_t kAlign = alignof(std::max_align_t);
};

/**
 * @brief The per-entry ring overhead charged ON TOP of the payload bytes: the deque slot and
 *        the control-block share the retention actually costs.
 *
 * Named, not a magic constant at the call site, and — unlike the `kRingAppendProbe = 1024`
 * guess it replaces (RFC-0025 §4.6.2) — it prices the ENTRY rather than libstdc++'s deque
 * internals, and it is charged against a source the embedder actually injected instead of
 * against the global heap nobody was watching.
 */
inline constexpr std::size_t kRingEntryOverhead = sizeof(ring_entry_t) + 2 * sizeof(void*);

/** @brief Release @p e's admission reservation back to @p src (sized reclaim); a no-op for a
 *         zero-cost entry. The ONE spelling of the release half of the charge/release pair. */
inline void release_reservation(tr::mem::block_source_t& src, const ring_entry_t& e) noexcept {
    if (e.token != nullptr) src.release(e.token, e.bytes, ring_entry_t::kAlign);
}

/**
 * @brief A receiving vertex's whole STREAM-ring state, LAZILY allocated as one block
 *        (RFC-0025 §4.6.1 clause 3) — the entries, the injected source they are charged
 *        against, the pressure arm, and the gap census.
 *
 * Grouped behind ONE pointer on purpose, and that is the difference between a seam every
 * ext-bearing vertex pays for and one only the receivers do. `%vertex_ext_t` already held a
 * lazy pointer for the ring's entries; hanging the source, the arm and the two counters off
 * that same pointer keeps `sizeof(vertex_ext_t)` EXACTLY where it was, so a vertex with app
 * fields, an `:acl` or a handler — which allocates the cold block for reasons of its own and
 * will never admit a stream entry — pays **zero** additional bytes. The RAM census
 * (`vertex_app5`, `vertex_app5_static`, `reg_escape`) is the gate that says so, and it caught
 * the four-loose-members spelling of this at +32 B.
 */
struct ring_state_t {
    /** @brief The queued entries, oldest first, each holding its admission reservation. */
    std::deque<ring_entry_t> entries;
    /** @brief This receiver's OWN injected source — the seam admissions are charged against.
     *
     *         Null until the first admission or an explicit `graph_t::set_ring_source`, at
     *         which point the graph-level default (itself defaulting to
     *         `tr::mem::heap_source()`) is BOUND here and stays bound: the destructor and
     *         every trim release against this exact source, and a sized reclaim cannot be
     *         served by a source that did not hand the block out. Per-injection-point, never
     *         a shared pool — ADR-0079's amendment measured a folded source collapsing to
     *         0.01x of its own single-thread rate at T=24. */
    tr::mem::block_source_t* source = nullptr;
    /** @brief Shed points on this ring since it was created — each one a
     *         `tr::flow::address_shift_gap` (RFC-0025 §4.5: "a detected discontinuity in an
     *         ordered flow"), surfaced to the consumer IN ORDER through
     *         `vertex_t::drain_unflushed`'s gap out-param and kept here for the census. */
    std::uint64_t gaps = 0;
    /** @brief How much of @ref gaps a consumer has already been told about, so
     *         `vertex_t::drain_unflushed` reports each shed point EXACTLY ONCE, in order, at
     *         the drain that follows it. */
    std::uint64_t gaps_drained = 0;
    /** @brief The §4.4 pressure arm this receiver binds under: `false` (the default) is
     *         BEST-EFFORT — a refused admission sheds the oldest entry whole, accounts the
     *         loss and raises a gap; `true` is RELIABLE — the admission is refused outright
     *         and the local producer is answered `BACKPRESSURE`, with nothing shed and no
     *         growth past the byte bound. Declared owner-side through
     *         `graph_t::set_ring_source`; it is NOT a new knob on the wire. This declaration
     *         IS the ruled selector: RFC-0025's 2026-08-24 §4.4 selector erratum (#1204) makes
     *         the receiving vertex's own arm the one §4.4 binds, and demotes the subscription's
     *         `reliability` bits to carried-verbatim-read-by-nothing. Receiver-pays (Amendment
     *         2, §4.6.1): the party that funds the ring's bytes declares what its overflow
     *         means. */
    bool reliable = false;

    /** @brief Release every held reservation and empty the ring — the ONE place the
     *         charge/release pairing is closed, shared by the destructor, the placeholder
     *         revert and `graph_t::set_ring_source`'s rebind. Idempotent. */
    void release_all() noexcept {
        if (source != nullptr)
            for (const ring_entry_t& e : entries) release_reservation(*source, e);
        entries.clear();
    }

    /** @brief Hand every reservation back before the block dies. Dropping the deque without
     *         releasing them would leak the whole ring's byte budget on every teardown. */
    ~ring_state_t() { release_all(); }
    ring_state_t() = default;
    ring_state_t(const ring_state_t&) = delete;
    ring_state_t& operator=(const ring_state_t&) = delete;
};

/**
 * @brief The lazily-allocated COLD half of a vertex (issue #361 §1): every member a plain
 *        STORED_VALUE leaf with default storage policy, no handlers, and no `:acl` never touches.
 *
 * ADR-0021 rule 2 ("the machinery is pay-for-what-you-use") applied to RAM: the common
 * MCU leaf keeps `vertex_t::ext_` null and pays nothing here. Allocated at most once —
 * at registration when the identity needs it (STREAM role or user handlers), or later
 * under the vertex mutex on the first `:acl` write or owner-side storage declaration —
 * and never freed before the vertex (the insert-only ADR-0057 lifetime), so a published
 * pointer stays valid for every reader.
 */
struct vertex_ext_t {
    /** @brief The VALUE seam (on_read/on_write/on_children), LAZILY allocated (ADR-0058
     *         Step 2) iff one of the three was installed at registration — handler
     *         PRESENCE, not role — so a plain leaf / app-field vertex keeps this null
     *         and never pays the ~96 B.
     *
     *         **Read lock-free** (@ref vertex_t::handlers loads it with no stripe lock,
     *         on the hot path). It is therefore an ATOMIC pointer, not a `unique_ptr`:
     *         registration publishes with `store(release)` and retirement
     *         (`vertex_t::revert_to_placeholder`) swaps it to `nullptr` with
     *         `exchange(acq_rel)`. A swapped-out
     *         block is **never freed under a concurrent reader** — the graph parks it, and
     *         the embedder frees the park through `graph_t::collect()` (#576; ADR-0057's
     *         insert-only discipline extended to the seam: emptied, never dangled). Keeping
     *         the park OFF the per-vertex block costs an app-field / leaf vertex zero extra
     *         bytes. The live block here is freed by this ext's destructor. */
    std::atomic<value_handlers_t*> handlers{nullptr};
    /** @brief The RECEIVER's STREAM ring state (docs/reference/11 role 2), LAZILY allocated on
     *         the first append or the first `graph_t::set_ring_source` (#388): an empty
     *         libstdc++ `std::deque` allocates its ~512 B map node at CONSTRUCTION, which
     *         every ext-bearing vertex (handlers, app fields, `:acl`, an owner-declared
     *         storage magnitude) paid even though only the STREAM role ever appends. Null ⇒
     *         no ring. Guarded by the vertex mutex.
     *
     *         ONE pointer for the whole of @ref ring_state_t — the entries, the injected
     *         source, the pressure arm and the gap census — so RFC-0025 §4.6.1's byte bound
     *         moves `sizeof(vertex_ext_t)` by NOTHING and a non-receiving ext-bearing vertex
     *         pays nothing for it. */
    std::unique_ptr<ring_state_t> ring;
    /** @brief The `:acl` parsed into core-subset ACEs at write time (#81) — the ONLY stored
     *         ACL state (#907); guarded by the vertex mutex. `graph_t::acl_allows` evaluates
     *         this list and `graph_t::read_acl` RE-ENCODES it, so read-back is canonical by
     *         construction and cannot describe something other than what is enforced. The
     *         retired verbatim byte copy could: a shape that parsed to no ACEs cleared
     *         enforcement while still reading back as a payload — ACEs apparently present
     *         (⇒ closed) on an open vertex. */
    std::vector<ace_t> aces;
    /** @brief The ADR-0050 cached effective-ACE merge (own + INHERIT-flagged ancestor ACEs,
     *         pre-merged in evaluation order); guarded by the vertex mutex, rebuilt lazily
     *         whenever `acl_gen` turns ODD. Only the MERGE is cached, never a verdict —
     *         expiry evaluates at check time against the caller's now. */
    std::vector<ace_t> eff_aces;
    /** @brief `:acl`-invalidation counter AND cache-validity stamp in ONE word (ADR-0078):
     *         ODD ⇒ `eff_aces` is stale, EVEN ⇒ it is the merge published for exactly this
     *         value. Every invalidator (@ref vertex_t::set_acl, the placeholder revert,
     *         @ref vertex_t::mark_acl_cache_dirty) advances it lock-free to the next ODD
     *         value; a rebuilder publishes by CAS-ing the value it snapshotted BEFORE its walk
     *         to that value + 1, so an invalidation landing anywhere in the rebuild defeats
     *         the CAS — there is no second word whose store could be lost. Folding the stamp
     *         into the counter keeps the gate's fast path at ONE atomic load, exactly what the
     *         retired dirty flag cost; a separate stamp word measured ~1% on `acl-inherit-d4`.
     *         32-bit: a wrap onto a stale-but-EVEN value needs 2^31 `:acl` writes on ONE
     *         vertex, unreachable at control-plane rates. Starts at 1 — never built ⇒ stale. */
    std::atomic<std::uint32_t> acl_gen{1};
    /** @brief True once an `:acl` has been written here; guarded by the vertex mutex and read
     *         only by the `:acl` read-back (#907). The one fact @ref aces cannot carry: an
     *         EMPTY container ACL is the sanctioned clear-enforcement write, and it must keep
     *         reading back as an empty ACL rather than as the NOT_FOUND of a vertex that was
     *         never given one — a distinction the retired byte copy drew implicitly, by being
     *         non-empty. Lands in the padding beside `%acl_gen`: zero extra bytes. */
    bool acl_present = false;
    /**
     * @brief STREAM ring depth — how many entries the receiver's @ref ring retains
     *        (RFC-0022 §3.C).
     *
     * OWNER-SIDE state, not protocol QoS: it encodes what the APPLICATION wants retained,
     * which no peer can supply. It is the retention INTENT; the BOUND is the bytes the
     * receiving vertex's injected source will fund (RFC-0025 §4.6), and the two compose —
     * the intent retires an entry before the bound charges the next one, and a shortfall
     * surfaces through §4.4's pressure contract rather than as a silent shrink. Declared
     * host-side through
     * `graph_t::set_history_depth`, exactly like the delivery mode; it has **no wire
     * surface at all** — neither readable nor writable remotely. Guarded by the vertex
     * mutex, which already guards the ring it bounds, and re-read on every append
     * (the `%store` verb) under that same hold. Costs a STREAM vertex zero extra bytes:
     * a STREAM identity always allocates this block anyway.
     */
    std::uint32_t history_keep_last = 1;
    /**
     * @brief RFC-0022 §3.D pin amplification RATIO `K` (ADR-0042 §3): a view-delivered,
     *        trailer-less WRITE whose `payload_bytes * K >= segment_bytes` is stored as a
     *        SUBVIEW of the inbound frame (refcount pin, zero copy) instead of the one-copy
     *        trailer-sliced store. 0 (@ref tr::graph::kPinNever, the default) NEVER pins.
     *
     * A ratio, NOT a byte count — the predicate prices what a pin would hold (the whole
     * segment) against the payload it holds it for, which an absolute threshold could not
     * see. Owner-side like @ref history_keep_last, and for the same reason: it is a
     * deployment copy/pin trade, not a quality-of-service property, so it lost its remote
     * write surface with the rest of the RFC-0022 §3.B removal. Declared through
     * `graph_t::set_pin_payload_ratio`. Read on every view-delivered write
     * (`%op_resolve_walk.hpp`) with no lock, so it stays ONE inline load off this block.
     *
     * @note This overrides the per-target `config_t::kPinPayloadRatio`, which Amendment 2
     *       fixes at the sentinel on both targets; the override exists so §6-style arms
     *       rotate inside one process. Left unset, behaviour is exactly what shipped
     *       before this RFC.
     */
    std::uint32_t pin_payload_ratio = 0;
    /** @brief The RFC-0010 APP-FIELD group (ADR-0058 Step 2) — the descriptor table plus
     *         its `on_app_field_write` apply seam, LAZILY allocated: a vertex with no app
     *         fields and no apply seam keeps this null. Guarded by the vertex mutex,
     *         insert-only. Null ⇒ the closed `ENOTTY` default (pre-RFC `:schema` shape). */
    std::unique_ptr<app_field_group_t> app;
    /** @brief STREAM drain cursor (RFC-0008 §E): ring APPENDS not yet flushed, so a
     *         propagate drains only what was appended; guarded by the vertex mutex. NOT a
     *         `write_seq_` delta (#925) — that bumps on a SHED append, fabricating a tail. */
    std::uint64_t appended_since_flush = 0;

    /** @brief Free the live handler block. `handlers` is a raw atomic pointer (for lock-free
     *         reads) so it no longer self-frees; this closes that. Blocks parked by retirement
     *         live on the graph, not here. The ring's reservations go back through
     *         `~ring_state_t`, which owns that pairing. Runs from `~vertex_t`'s `delete ext_`. */
    ~vertex_ext_t() { delete handlers.load(std::memory_order_acquire); }
    vertex_ext_t() = default;
    vertex_ext_t(const vertex_ext_t&) = delete;
    vertex_ext_t& operator=(const vertex_ext_t&) = delete;
};

/** @brief Declared here so @ref vertex_t can befriend the #1285 member-offset gate; defined
 *         just after the type it measures. */
struct vertex_layout_gate_t;

/**
 * @brief An L4 graph vertex: a named, addressable position holding a value, a bounded
 *        history, or a user handler (docs/reference/11 §roles).
 *
 * Pinned in place (the atomic last-known-value slot + mutex + condvar are non-movable) and
 * always handled via a `vertex_handle_t` returned by `graph_t::register_vertex` (ADR-0056). The
 * read/write hot path takes no vertex lock (an atomic shared_ptr swap); the mutex guards only the
 * history ring, the subscriber list, and the await waiter accounting. Non-copyable.
 *
 * The public surface is a VERB interface — reading the stored value (@ref read_stored),
 * readiness (@ref note_write / @ref wait_for_change / the seq cursors), edges
 * (@ref add_edge / @ref clear_edge / @ref snapshot_edges), and ACL state (@ref set_acl /
 * @ref with_acl / @ref with_aces / @ref with_effective_aces) — each verb taking the vertex mutex
 * internally (the LKV slot stays
 * lock-free). `graph_t` keeps what SPANS vertices: routing, ancestor walks, fan-out
 * dispatch legs, the effective-ACL walk, admission, and the field surface.
 *
 * The PUBLISHING half of storage is not on that surface: `%store`, like the map-lock mutators,
 * is private with `graph_t` as its sole friend (#867, #1300), because every publish must pass
 * `graph_t::store_value` — the one seam that gates the write, injects the ADR-0039 resource and
 * counts what the publish shed. Owners reach the stored value's queue semantics through
 * `graph_t::history` / `graph_t::drain_unflushed` / `graph_t::mark_flushed` instead.
 */
class vertex_t {
   public:
    /** @brief The no-heap small-fan-out snapshot width (@ref snapshot_edges buffer size). */
    static constexpr std::size_t kInlineFanout = edge_snapshot_t::kCapacity;

    /**
     * @brief What @ref add_edge answers when the edge could NOT be admitted — the injected
     *        resource is exhausted (#477: the writer soft-fails by value; a `bad_alloc` under
     *        `-fno-exceptions` would be an `abort()`, and admission is reachable from a peer's
     *        bytes since RFC-0014).
     *
     * A caller that sees it must NOT count a listener: nothing was appended and nothing was
     * published, so the vertex is exactly as it was.
     */
    static constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

    /** @brief Construct a vertex with its role, own canonical NAME record (ADR-0057 — one
     *         segment, not the full key), and handlers. The cold extension block is
     *         allocated only if this identity needs one (#361 §1). */
    vertex_t(role_t role, path_key_t name, handlers_t handlers)
        : name_(std::move(name)), role_(role) {
        adopt_identity(role, std::move(handlers));
    }

    vertex_t(const vertex_t&) = delete;
    vertex_t& operator=(const vertex_t&) = delete;

    /** @brief Free the cold extension block (allocated at most once, ADR-0057 lifetime) and
     *         flush the edge block's published + parked arrays (`edge_block_t`'s destructor
     *         states the outlive-the-publishers contract that makes this safe). */
    ~vertex_t() {
        delete ext_.load(std::memory_order_acquire);
        delete edges_.load(std::memory_order_acquire);
    }

    /**
     * @brief This vertex's behavioral role.
     *
     * A RELAXED atomic load (#1477). The read is lock-free and on the write hot path, while
     * the writers (`%fill`, @ref revert_to_placeholder) run under the graph's unique map
     * lock — so the load is unordered by construction and may observe either the retiring
     * occupant's role or the placeholder default. What the atomic buys is that the race is
     * WELL-DEFINED rather than UB; it does NOT serialise a write against a retire (see the
     * doctrine note at `graph_t::write`).
     */
    [[nodiscard]] role_t role() const noexcept { return role_.load(std::memory_order_relaxed); }
    /** @brief This vertex's own canonical NAME record (its single path segment, ADR-0057);
     *         empty at the root. The full key is a parent-walk concatenation
     *         (`graph_t`'s `build_key`). */
    [[nodiscard]] const path_key_t& name() const noexcept { return name_; }
    /**
     * @brief Whether the lazily-allocated cold extension block EXISTS on this vertex (#361 §1).
     *
     * A RAM-census observable, not a data-plane predicate: it is the one fact about the
     * pay-for-what-you-use split that no functional surface reveals, and `bench_qos_census`
     * plus the RFC-0022 host tests are its only callers. It used to be spelled by comparing
     * `settings()`'s returned ADDRESS against the shared `kDefaultSettings` constant; RFC-0022
     * deleted both, so the question needs a name of its own rather than an idiom.
     */
    [[nodiscard]] bool has_extension_block() const noexcept {
        return ext_.load(std::memory_order_acquire) != nullptr;
    }

    /**
     * @brief This vertex's declared pin amplification ratio `K` (0 ⇒ never pin, the default).
     *
     * ONE inline load and nothing more: it is read on EVERY view-delivered write
     * (`%op_resolve_walk.hpp`), so it may never become an ancestor walk. Nothing is
     * inherited (RFC-0022 §3.F) — a vertex that was never given a ratio answers 0,
     * whatever its ancestors hold.
     */
    [[nodiscard]] std::uint32_t pin_payload_ratio() const noexcept {
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        return e != nullptr ? e->pin_payload_ratio : 0;
    }
    /** @brief This vertex's user handlers (Handler role behavior + the `on_children` seam);
     *         an all-empty shared constant when no extension block. */
    [[nodiscard]] const value_handlers_t& handlers() const noexcept {
        static const value_handlers_t kNoHandlers{};
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        if (e == nullptr) return kNoHandlers;
        // Lock-free acquire load of the atomic seam pointer. It is published once at
        // registration and, on retirement, swapped to nullptr — the swapped-out block is
        // parked (never freed) so the reference we return here stays valid even if a
        // concurrent retire fires between this load and the caller's deref. A load that
        // races the swap sees either the old block (still alive, parked) or nullptr; both
        // are safe.
        const value_handlers_t* h = e->handlers.load(std::memory_order_acquire);
        return h != nullptr ? *h : kNoHandlers;
    }

    /** @brief A copy of this vertex's owner apply seam (RFC-0010 §A.3), or empty when none —
     *         taken under the vertex lock so the caller can fire it OUTSIDE the lock (the
     *         seam may re-enter the graph). Empty ⇒ declared field writes just store. */
    [[nodiscard]] std::function<void(std::string_view, const view_t&)> on_app_field_write() {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        return (e != nullptr && e->app) ? e->app->on_app_field_write
                                        : std::function<void(std::string_view, const view_t&)>{};
    }

    // -- Composite tree links (ADR-0057) -------------------------------------------------
    //
    // The graph is a Composite of vertices: each node owns its children (one non-moving
    // unique_ptr allocation per child, so vertex_t* stay stable for the graph's lifetime)
    // and points at its parent. Children/registered are guarded by graph_t's map lock
    // (unique for mutation, shared for walks); the parent pointer and name bytes are
    // immutable after construction, so parent-chain walks (bubbling, the ACL inheritance
    // walk) run LOCK-FREE.

    /** @brief The owning parent node (`nullptr` only at the graph root).
     *  @note Immutable once linked — safe to walk without any lock. */
    [[nodiscard]] vertex_t* parent() const noexcept { return parent_; }

    /** @brief True once a registration filled this node; false for a placeholder — a
     *         structural intermediate level that `find` / `read_children` must not surface
     *         (matching the flat-map behavior where missing intermediates did not exist).
     *  @note Read/written under the graph's map lock. */
    [[nodiscard]] bool registered() const noexcept { return registered_; }

    /**
     * @brief True iff this vertex is a `:children[]` MEMBER of its parent — registered and
     *        not enumeration-hidden (RFC-0014 §3, S4).
     *
     * The enumeration-hide seam RFC-0014 §3 says "the implementation must add": the RFC-0014
     * creator endpoint `<net_root>/<module>/conn` is a real, registered, addressable vertex —
     * `find` resolves it, a `SPEC`/`NAME` write executes on it, `conn:schema` reads it — but
     * it is NOT one of the module's connections, and a topology walker that treats every
     * `:children[]` member as a link descends into a control vertex with no peer behind it
     * (the concrete bug the TypeScript client had to work around in #1302).
     *
     * Deliberately NARROWER than "invisible": hiding is a member-listing property only.
     * `registered()` — which governs `find`, retirement walks, the branch/leaf fork
     * (@ref has_registered_child) and the owner-side `for_each_vertex` census — is untouched,
     * because RFC-0014 §6 makes `read <module>/conn:schema` the *sanctioned* creatability
     * probe: the endpoint has to stay addressable precisely BECAUSE it is unlisted.
     *
     * @note Read/written under the graph's map lock, like @ref registered.
     */
    [[nodiscard]] bool enumerable_member() const noexcept {
        return registered_ && !test_flag(flag_t::ENUM_HIDDEN, std::memory_order_relaxed);
    }

    /**
     * @brief This vertex's retirement generation (ADR-0062).
     *
     * Bumped every time retirement re-virginizes this object, so a holder of a CACHED
     * resolution can tell "the same vertex" from "the same address, a new occupant". A
     * handle alone cannot: the vertex map is pinned and insert-only, so a stale handle stays
     * usable and would silently address the revived path's new owner.
     */
    [[nodiscard]] std::uint32_t retire_gen() const noexcept {
        return retire_gen_.load(std::memory_order_acquire);
    }

   private:
    friend class graph_t;                // sole caller of the map-lock mutators below (#867).
    friend struct vertex_layout_gate_t;  // reads the private member offsets the #1285 gate pins.

    /**
     * @brief Fill this node with a registration's identity: set the role and handlers, and
     *        mark it @ref registered.
     *
     * Called under the graph's UNIQUE map lock — either on a freshly constructed node or on
     * a placeholder being registered in place (the allocation never moves, ADR-0057).
     */
    void fill(role_t role, handlers_t handlers) {
        role_.store(role, std::memory_order_relaxed);  // atomic since #1477 — see @ref role
        adopt_identity(role, std::move(handlers));
        registered_ = true;
        // Maintain the parent's lock-free fork bit (#652). Setting is unconditional and
        // idempotent; the root has no parent, and nothing asks about the root's parent.
        if (parent_ != nullptr) parent_->set_flag(flag_t::REGISTERED_CHILD, true);
    }

    /** @brief Flip this vertex back to a placeholder (invisible to `find`) — retirement's
     *         inverse of the `registered_ = true` in `fill`. Map-lock state; the caller
     *         (`graph_t::retire`) MUST hold the graph map lock, same as `fill`'s writer.
     *         Pairs with @ref revert_to_placeholder, which clears the vertex's own state. */
    void mark_unregistered() noexcept {
        if (!registered_) return;  // `retire_subtree` walks placeholders too
        registered_ = false;
        // Clearing needs to know whether any SIBLING is still registered, so it recomputes
        // rather than decrementing. That is a walk of the parent's children — but only at
        // retirement, under the unique map lock the caller already holds, and only for a
        // vertex that was actually registered. A counter would avoid the walk and cost four
        // bytes of `vertex_t`, which the size gate does not have to spare.
        if (parent_ != nullptr) parent_->refresh_registered_child();
    }

    /**
     * @brief Drop this vertex out of its parent's `:children[]` listing without touching
     *        @ref registered — the RFC-0014 §3 hide seam. Unique-map-lock callers only
     *        (@ref graph_t::hide_from_enumeration is the sole one).
     *
     * One-way on purpose: the only unhide is retirement, which re-virginizes the vertex and
     * clears the bit with the rest of its identity. A vertex whose listing status could flip
     * back and forth would be a second, mutable source of truth about what a module contains.
     */
    void mark_enumeration_hidden() noexcept { set_flag(flag_t::ENUM_HIDDEN, true); }

    /**
     * @brief Record that this vertex DECLARED a payload-right table (RFC-0014 Amendment 2).
     *
     * The rows themselves are the graph's — see `graph_t::declare_payload_rights` for why
     * they are not here. This bit is the whole of the per-vertex cost, and it is why a vertex
     * that declares nothing pays not one byte and not one dereference for the feature: the
     * write gate reads this already-hot flags word and stops.
     * Set by the graph under the map lock at registration; cleared by
     * @ref revert_to_placeholder, so a retired vertex's rows can never gate its successor.
     */
    void mark_payload_rights() noexcept { set_flag(flag_t::PAYLOAD_RIGHTS, true); }

    /** @brief True iff this vertex declared a payload-right table (RFC-0014 Amendment 2) —
     *         the write gate's one-load precondition for consulting the graph's rows. */
    [[nodiscard]] bool has_payload_rights() const noexcept {
        return test_flag(flag_t::PAYLOAD_RIGHTS, std::memory_order_relaxed);
    }

    /**
     * @brief Record that this vertex installed an ADMISSION filter (@ref handlers_t::on_admit /
     *        @ref handlers_t::on_app_field_admit).
     *
     * The filters themselves are the graph's — see `graph_t::admissions_` for why they are not
     * here — and this bit is the whole of the per-vertex cost. Set by the graph under the map
     * lock at registration; cleared by @ref revert_to_placeholder, so a retired vertex's filter
     * can never judge its successor's writes.
     */
    void mark_admission() noexcept { set_flag(flag_t::ADMISSION, true); }

    /**
     * @brief True iff this vertex installed an admission filter (@ref handlers_t::on_admit) —
     *        the store path's one-load precondition for consulting the graph's filter list.
     *
     * The same shape, and the same argument, as `has_payload_rights`: a vertex that installed
     * no filter pays one relaxed test of an already-hot word and no dereference, so the seam
     * costs the plain leaf write nothing — and, the point of the placement, it costs the
     * SEAM-BEARING vertices nothing either.
     */
    [[nodiscard]] bool has_admission() const noexcept {
        return test_flag(flag_t::ADMISSION, std::memory_order_relaxed);
    }

   public:
    /** @brief Recompute `flag_t::REGISTERED_CHILD`. Unique-map-lock callers only. */
    void refresh_registered_child() noexcept {
        bool any = false;
        for_each_child([&any](const vertex_t& c) { any = any || c.registered(); });
        set_flag(flag_t::REGISTERED_CHILD, any);
    }

    /**
     * @brief True iff at least one DIRECT child is registered — the branch/leaf fork of the
     *        plain read surface, answered without taking the graph's map lock (#652).
     *
     * This used to be `graph_t::has_registered_child`, which took `map_mutex_` shared and
     * walked the child list to compute the same predicate. That lock was the single largest
     * term on the read path and, being process-wide, it capped **every** read in the process
     * at roughly 20 M/s no matter how many cores or how disjoint the vertices: short-circuit
     * it and twenty-four readers on distinct vertices go from 19.7 to 165.3 M ops/s. A
     * blocking lock does not collapse the way a spin lock does — it plateaus — which is
     * exactly why this was invisible for so long: a flat aggregate reads like "scales fine"
     * until you notice that flat across a 24x thread range means each thread is 24x slower.
     *
     * The counter is mutated only by `fill` and `mark_unregistered`, both of which run
     * under the graph's UNIQUE map lock, so mutations are already serialized; the atomic is
     * what makes the *read* race-free. A reader concurrent with a registration may observe
     * either side of it — exactly as it could when the fork took a shared lock, since the
     * API orders a `read` against a concurrent `register_vertex` no more strongly than this.
     * The composed branch read re-acquires the map lock for its own walk, so the ordering
     * that walk depends on is not this counter's to provide. One observable follows from
     * that gap: a read racing the retirement of the LAST registered child may see the bit
     * set here and then find no registered child under the walk's lock, composing the
     * root alone — a POINT with zero child records. That reply is LEGAL, byte-identical
     * to the fully READ-ACL-pruned reply RFC-0016 §B produces deterministically (erratum
     * 2026-08-13, #1030) — a transient in frequency, not a new shape.
     */
    [[nodiscard]] bool has_registered_child() const noexcept {
        return test_flag(flag_t::REGISTERED_CHILD, std::memory_order_acquire);
    }

   private:
    /**
     * @brief Adopt @p child into this node's child list and link its parent pointer.
     *
     * The list block is lazily allocated on the FIRST child (#380 §1): a leaf — the
     * common MCU vertex — keeps `children_` null and pays exactly one pointer. The
     * list stays sorted by name record, so a wide composite resolves a child in
     * O(log children). The `vertex_t` itself never moves (only owning pointers do).
     * @note Called under the graph's UNIQUE map lock.
     * @return The adopted child (its stable address).
     */
    vertex_t* add_child(std::unique_ptr<vertex_t> child) {
        child->parent_ = this;
        vertex_t* raw = child.get();
        if (!children_) children_ = std::make_unique<children_t>();
        std::vector<std::unique_ptr<vertex_t>>& sorted = children_->sorted;
        const auto pos =
            std::lower_bound(sorted.begin(), sorted.end(), child->name().bytes(),
                             [](const std::unique_ptr<vertex_t>& c, std::span<const std::byte> n) {
                                 return std::ranges::lexicographical_compare(c->name().bytes(), n);
                             });
        sorted.insert(pos, std::move(child));
        return raw;
    }

   public:
    /**
     * @brief The child whose own NAME record equals @p record byte-for-byte, or `nullptr` —
     *        one level of the O(segments) resolution walk (ADR-0057).
     * @note Called under the graph's map lock (shared suffices).
     */
    [[nodiscard]] vertex_t* child_by_record(std::span<const std::byte> record) const noexcept {
        if (!children_) return nullptr;
        const std::vector<std::unique_ptr<vertex_t>>& sorted = children_->sorted;
        const auto it =
            std::lower_bound(sorted.begin(), sorted.end(), record,
                             [](const std::unique_ptr<vertex_t>& c, std::span<const std::byte> r) {
                                 return std::ranges::lexicographical_compare(c->name().bytes(), r);
                             });
        if (it == sorted.end()) return nullptr;
        const bool matches = std::ranges::equal((*it)->name().bytes(), record);
        return matches ? it->get() : nullptr;
    }

    /**
     * @brief Run @p f over every child (placeholders included), in sorted name-record
     *        order — member enumeration and the RFC-0005 subtree-counter walks.
     * @note Called under the graph's map lock (shared suffices); @p f must not mutate
     *       the tree.
     */
    template <typename F>
    void for_each_child(F&& f) const {
        if (!children_) return;
        for (const std::unique_ptr<vertex_t>& c : children_->sorted) f(*c);
    }

    /**
     * @brief Run @p f over every DESCENDANT (this vertex excluded), pre-order, **iteratively**.
     *
     * The subtree counterpart of @ref for_each_child, and the reason it exists is stack safety:
     * the four subtree walks in `graph.cpp` were **self-recursion**, one frame per graph level at
     * 32–208 B a level, and graph depth is a vertex's path segment count — which nothing on the
     * wire path bounds. `kMaxSegments` is enforced only in `path_t::parse`, the *local* string
     * builder; `graph_t::ensure_vertex` takes raw key bytes and counts nothing, so a peer could
     * already create a vertex deep enough to overflow the stack of a walk it then triggers
     * (`:subscribers[]`, RETIRE, `:acl`). See #690.
     *
     * **Descends with no auxiliary storage at all** — no explicit stack, so nothing to allocate
     * and nothing to fail. It ascends via the parent link and re-finds its position among its
     * siblings by binary search on its own NAME record, which the `sorted` list already supports
     * (the same `lower_bound` `child_by_record` uses). That costs O(log children) per ascent
     * instead of the O(1) an explicit stack would give, and buys back an error channel the two
     * `void` callers could not have carried without a signature change.
     *
     * @note Same contract as @ref for_each_child — called under the graph's map lock, and @p f
     *       MUST NOT mutate the tree — the walk holds no snapshot and re-reads `sorted` on every
     *       ascent, so an insertion mid-walk would move the position it is about to resume from.
     */
    template <typename F>
    void for_each_descendant(F&& f) {
        vertex_t* cur = first_child();
        if (cur == nullptr) return;
        for (;;) {
            f(*cur);
            if (vertex_t* const down = cur->first_child(); down != nullptr) {
                cur = down;
                continue;
            }
            // Leaf: climb until some ancestor has a next sibling. `this` is the sentinel and is
            // never visited — but the sibling test must come FIRST even when the parent IS
            // `this`, or the walk returns at the end of the leftmost spine and never reaches
            // this vertex's second child. (It did exactly that; `edge_eviction` caught it.)
            for (;;) {
                vertex_t* const up = cur->parent_;
                if (up == nullptr) return;
                if (vertex_t* const sib = up->next_sibling_of(*cur); sib != nullptr) {
                    cur = sib;
                    break;
                }
                if (up == this) return;  // no sibling left at the top level — subtree exhausted
                cur = up;
            }
        }
    }

    // -- storage & readiness ----------------------------------------------------------

    /**
     * @brief What a `%store` SHED under allocation pressure — reported BY REFERENCE,
     *        never counted here (#1003).
     *
     * The same division of labour @ref snapshot_drops_t states for the fan-out plane, for the
     * same reason: `vertex_t` is the storage layer and owns no counters, so it reports the
     * tally and `graph_t` folds it through the single exhaustive counting door. A shed the
     * storage layer knows about and the graph never hears of is exactly the defect — a whole
     * STREAM fan-out was abandoned under memory pressure while `graph_t::delivery_drops()`,
     * the one observable, read zero.
     *
     * The width is the CALLER's call, not this struct's: whether a shed append cost a
     * delivery depends on whether the ring drain *was* the delivery (it is for the write and
     * sweep paths; it is not for a branch notify, which fans the slice out eagerly and then
     * flushes the cursor). See `graph_t::count_store_drops`.
     */
    struct store_drops_t {
        /** @brief The RECEIVER's STREAM ring could not admit: its injected source declined the
         *         reservation and the ring had nothing left to shed, so the entry never
         *         entered the ring. The LKV publish ABOVE it still landed — the write succeeds
         *         (RFC-0008 §E, bounded-lossy history), and what is lost is the delivery a
         *         later drain would have made. */
        bool ring_append = false;
        /** @brief How many queued entries the best-effort arm SHED to make room (RFC-0025
         *         §4.4: "shed the oldest, whole, never partial"). Each one is both a lost
         *         delivery and a `tr::flow::address_shift_gap` point; silence here is the one
         *         behaviour the RFC forbids. */
        std::uint64_t ring_shed = 0;
        /** @brief Did this store shed anything? The ONE test a clean write pays. */
        [[nodiscard]] bool any() const noexcept { return ring_append || ring_shed != 0; }
    };

   private:
    // #1300: the storage funnel is graph_t's, for the same reason the map-lock mutators above
    // are — `store()` is reachable ONLY through `graph_t::store_value`, the one seam that
    // gates the write, injects the ADR-0039 resource, and folds `store_drops_t` into
    // `delivery_drops()`. A caller that reached it directly would publish a value the graph
    // never counted, never gated and never marked pending. The RFC-0008 §E coverage that used
    // to need it on a bare vertex is now `graph_t::drain_unflushed` / `mark_flushed` /
    // `history`, handle-based. The `friend class graph_t` above is the sole exemption; no new
    // friend was added, deliberately — a friend named in an installed public header is
    // claimable by any user who defines a class of that name.
    /**
     * @brief Publish @p value as this vertex's state: swap the last-known-value (lock-free),
     *        bump the write sequence, and wake awaiters. **A PRODUCER NEVER QUEUES.**
     *
     * ONE tail, for every role (RFC-0025 §4.6.1 Amendment 2, clause 1). The producer-side ring
     * machinery this verb used to run — the `kRingAppendProbe = 1024` heuristic, the deque
     * append under the stripe mutex, and the second stripe acquisition the drain then paid —
     * is GONE, not made optional: §4.6.2 measured it at a **fixed +29 ns (+54 %) per write,
     * independent of depth**, and four writers on one vertex at **1.73 M/s against 4.59 M/s**
     * lock-free. A tax that every producer pays for a service only some consumers want belongs
     * to the consumer, so the queue moved to the RECEIVING vertex (%ring_admit) and is
     * bounded there in BYTES by that vertex's own injected source.
     *
     * One allocation (`make_shared`): the rope's inline small-buffer holds the
     * single-link trivial case, so a scalar write costs exactly what the `view_t`
     * slot cost (ADR-0053 §6). Not for Handler-role writes — the graph runs
     * `handlers().on_write` and calls @ref note_write instead.
     *
     * @note **Cross-writer total order is no longer implied here.** The stripe mutex used to
     *       serialize STREAM appends, so ring order doubled as a global order across writers.
     *       It does not any more: order across producers is ADR-0019's per-producer HLC
     *       stamp, read off the value, and a receiver ring fed by N producers
     *       orders by stamp rather than minting a sequence of its own. An embedder that read a
     *       global order off append order must read it off the stamp instead.
     * @param value The value to publish (moved into the LKV slot).
     * @param mr    The ADR-0039 injected resource the LKV control block + rope are
     *              allocated from (#361 §5) — the graph passes its own; `nullptr`
     *              (the default, and every direct caller) keeps plain `make_shared`.
     *              Lifetime: the resource must outlive every `shared_ptr` obtained
     *              from this vertex — the same "handles do not outlive the graph's
     *              memory" contract the injection seam already imposes.
     * @return The published LKV pointer — exactly what a concurrent @ref read_stored
     *         observes — so the write path can deliver the stored value (RFC-0008 §D
     *         "deliver exactly what was stored") without recloning the rope.
     * @retval nullptr The LKV control-block allocation failed (OOM): NOTHING was
     *         published (#477 nothrow soft-fail — the graph maps this to
     *         `BACKPRESSURE`; the store verb never aborts the node).
     */
    std::shared_ptr<const rope_t> store(rope_t value, std::pmr::memory_resource* mr = nullptr) {
        std::shared_ptr<const rope_t> sp = try_make_lkv(std::move(value), mr);
        if (!sp) return nullptr;  // OOM: nothing published — the caller soft-fails (#477)
        // Publish the new last-known-value (lock-free by CONTRACT; see lkv_). A slot that
        // reclaims lazily has to allocate to publish, so this can decline — and when it does,
        // NOTHING was published: fail exactly as an LKV allocation failure does, rather than
        // returning a handle to a value the vertex is not actually holding.
        if (!lkv_.store(sp)) return nullptr;  // #477 soft-fail — the graph maps it to BACKPRESSURE

        // WAITERLESS PUBLISH: no ring to append and nobody in `await` ⇒ take no lock at all
        // (#555). #370 already skipped the condvar CALL on this path; the mutex itself was
        // what remained, and it was not free: measured, it sits immediately downstream of the
        // `lkv_` atomic publish, so the two serializing regions land back-to-back on the
        // critical dependency chain and cost ~38 cycles of the write's ~335. (The stripe lock
        // in `snapshot_edges` overlaps the tail and measures zero — this one does not.)
        //
        // Why this cannot lose a wakeup. Both sides are seq_cst, so they share one total
        // order. WRITER: bump `write_seq_`, THEN read `waiters`. WAITER: publish `++waiters`,
        // THEN read `write_seq_` to evaluate its predicate. If the writer reads `waiters == 0`
        // it is ordered before the waiter's store, hence before the waiter's read of the
        // sequence — so the waiter observes the bump and `wait_for` returns on its FIRST
        // predicate evaluation, without ever blocking. If instead the waiter got there first,
        // the writer reads a non-zero count and takes the slow path below, which acquires the
        // stripe mutex the waiter must hold to block. Neither interleaving leaves a sleeper.
        //
        // A spurious slow path is harmless: `waiters` is per STRIPE, so an unrelated vertex's
        // awaiter makes this publish take the lock and notify needlessly. That is the same
        // collision `vertex_stripe_t` already documents (a spurious wake plus a re-check,
        // never a correctness change).
        vertex_stripe_t& st = vertex_stripe_of(this);  // one lookup per verb (#370)
        write_seq_.fetch_add(1, std::memory_order_seq_cst);
        if (st.waiters.load(std::memory_order_seq_cst) == 0) return sp;
        const std::lock_guard lock(st.m);
        vertex_stripe_cv(vertex_stripe_index(this)).notify_all();
        return sp;
    }

    /**
     * @brief Admit @p sp into this RECEIVING vertex's STREAM ring, charging the admission
     *        against @p src — the byte bound of RFC-0025 §4.6.1 clause 3.
     *
     * Charging is **reservation ADMISSION, not placement**. `try_alloc(retained_bytes)` on
     * @p src reserves a block that is never written to and is held until the entry retires;
     * the payload stays exactly where the publish put it and the append is still a refcount
     * bump (@ref ring_entry_t). What the injected source therefore bounds is how much this
     * receiver may have OUTSTANDING, in bytes, on a budget it chose — which is the thing the
     * retired `kRingAppendProbe = 1024` guess could not do: it priced libstdc++'s deque
     * internals against the global heap, and no injected source ever saw it.
     *
     * The §4.4 pressure contract, binding HERE (clause 4), selected by the receiver's own
     * declared arm (`ring_state_t::reliable`, set through `graph_t::set_ring_source`):
     *
     * - **best-effort** — a refused reservation SHEDS THE OLDEST entry, whole and never
     *   partial, releases its reservation and retries; each shed accounts a lost delivery
     *   (@ref store_drops_t::ring_shed) and marks a `tr::flow::address_shift_gap` IN ORDER at
     *   the shed point (@ref ring_entry_t::gap_before). A ring that cannot fund even an empty
     *   admission declines it and says so (@ref store_drops_t::ring_append). Latency stays
     *   bounded; completeness is sacrificed knowingly.
     * - **reliable** — the admission is refused, NOTHING is shed and the ring does not grow
     *   past its byte bound; the caller answers `BACKPRESSURE` to the rate-aware producer,
     *   which slows. LOCAL producers only in v1: there is no wire carrier for backpressure,
     *   which waits on the credit window §4.6.1 clause 7 parks as the v2 escalation.
     *
     * @param sp       The published value to queue (refcount share; the caller keeps its own).
     * @param bytes    The retained width to reserve — payload plus %kRingEntryOverhead.
     * @param src      The graph-resolved effective source; BOUND into `ring_state_t::source`
     *                 on first use so every later release reaches the source that served it.
     * @param drops    Out: what this admission SHED — set, never cleared, so the caller owns
     *                 the zeroing. `graph_t::count_store_drops` is the seam that must not forget.
     * @return true iff the entry was queued. False is the RELIABLE refusal — and only that, so
     *         a caller can map it straight to `BACKPRESSURE` without re-deriving the arm.
     */
    bool ring_admit(const std::shared_ptr<const rope_t>& sp, std::size_t bytes,
                    tr::mem::block_source_t& src, store_drops_t* drops) {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        if (e == nullptr) return true;  // no ext, no ring — nothing to admit into, nothing shed
        if (!e->ring) e->ring = std::make_unique<ring_state_t>();  // first append (#388 lazy)
        ring_state_t& r = *e->ring;
        // Bind the source ONCE. A release must reach the source that served the block (sized
        // reclaim), so a vertex that has already charged keeps charging the same seam even if
        // the graph default is later re-injected; `graph_t::set_ring_source` is the only
        // rebind, and it drains the ring first.
        if (r.source == nullptr) r.source = &src;
        tr::mem::block_source_t& source = *r.source;

        // The DEPTH intent retires BEFORE the byte bound charges. Order matters: a ring already
        // at its declared depth is going to drop its oldest entry for this append either way,
        // so releasing that reservation FIRST is what funds the new one out of the receiver's
        // own steady-state budget. Charging first and trimming after would make a source sized
        // for exactly N entries refuse the N+1th and shed under §4.4 — pressure behaviour on a
        // ring that was never actually over its bound. Both bounds compose (RFC-0025 §4.6);
        // this is the composition.
        //
        // And the retiring entry's reservation is HANDED STRAIGHT ON when it is the same shape
        // the new one needs. A reservation is fungible bytes at a fixed size and alignment, so
        // releasing a block of exactly `bytes` only to ask the same source for a block of
        // exactly `bytes` is a round trip that buys nothing. Carrying it over makes a
        // steady-state uniform stream cost the source ZERO calls per write — where the
        // `kRingAppendProbe` heuristic this replaces did an allocate-and-free on EVERY write,
        // forever, and told nobody anything.
        const std::size_t keep = e->history_keep_last != 0 ? e->history_keep_last : 1;
        void* carried = nullptr;
        while (r.entries.size() >= keep) {
            ring_entry_t& oldest = r.entries.front();
            if (carried == nullptr && oldest.token != nullptr && oldest.bytes == bytes) {
                carried = oldest.token;
            } else {
                release_reservation(source, oldest);
            }
            r.entries.pop_front();
        }

        const bool arm_reliable = r.reliable;
        void* token = carried != nullptr ? carried : source.try_alloc(bytes, ring_entry_t::kAlign);
        std::uint64_t shed = 0;
        if (token == nullptr && !arm_reliable && !r.entries.empty()) {
            // Best-effort: shed **THE OLDEST** whole entry — §4.4's word is singular and it is
            // load-bearing. Shedding in a loop until the source relents would empty the whole
            // ring on a source that has gone to zero, destroying every queued delivery to fund
            // an admission that still fails. One per admission bounds the damage to what the
            // pressure actually cost, and a source that stays dead converges the ring to empty
            // one write at a time instead of in one stroke.
            release_reservation(source, r.entries.front());
            r.entries.pop_front();
            ++shed;
            token = source.try_alloc(bytes, ring_entry_t::kAlign);
        }
        if (token == nullptr) {
            // Nothing admitted. Under the reliable arm nothing was shed either, and the caller
            // turns our `false` into BACKPRESSURE. Under best-effort the ring was already
            // emptied above, so the loss is real and is accounted rather than silent.
            if (drops != nullptr && !arm_reliable) drops->ring_append = true;
            if (drops != nullptr) drops->ring_shed += shed;
            r.gaps += shed;
            return !arm_reliable;
        }
        r.entries.push_back(ring_entry_t{.value = sp,  // refcount bump — the caller keeps `sp`
                                         .token = token,
                                         .bytes = bytes,
                                         .gap_before = shed != 0});
        ++e->appended_since_flush;  // the drain counts APPENDS, not seq (#925)
        if (drops != nullptr) drops->ring_shed += shed;
        r.gaps += shed;
        return true;
    }

   public:
    /**
     * @brief Record a Handler-role write: bump the write sequence and wake awaiters
     *        (the vertex stores no value — the user handler consumed it).
     */
    void note_write() {
        vertex_stripe_t& st = vertex_stripe_of(this);  // one lookup per verb (#370)
        write_seq_.fetch_add(1, std::memory_order_seq_cst);
        if (st.waiters.load(std::memory_order_seq_cst) == 0) return;  // waiterless (#555)
        const std::lock_guard lock(st.m);
        vertex_stripe_cv(vertex_stripe_index(this)).notify_all();
    }

    /** @brief The stored last-known-value (lock-free; null ⇒ never assigned / Handler role). */
    [[nodiscard]] std::shared_ptr<const rope_t> read_stored() const { return lkv_.load(); }

    /**
     * @brief Block until the write sequence moves past @p seq0 or @p timeout elapses.
     * @param seq0    The @ref current_seq snapshot the caller waits to see surpassed.
     * @param timeout The maximum wait.
     * @return true iff a change was observed (`write_seq_ != seq0`); false on timeout.
     */
    [[nodiscard]] bool wait_for_change(std::uint64_t seq0, std::chrono::nanoseconds timeout) {
        const std::size_t idx = vertex_stripe_index(this);
        vertex_stripe_t& st = vertex_stripe_at(idx);
        std::unique_lock lock(st.m);
        // Register on the stripe's waiter count. Still mutated under st.m, but the count is
        // now also read by a publish that takes NO lock (#555), so the store must be seq_cst
        // and must land BEFORE this thread reads `write_seq_` in the predicate below. That
        // ordering is the waiter's half of the Dekker pair documented on @ref store: a
        // publisher that saw zero here is ordered before this store, therefore before the
        // predicate's read, so the predicate observes its bump and `wait_for` returns without
        // blocking. RAII so a throwing wait can never leak a phantom waiter.
        struct waiter_scope_t {
            std::atomic<int>& n;
            explicit waiter_scope_t(std::atomic<int>& c) : n(c) {
                n.fetch_add(1, std::memory_order_seq_cst);
            }
            ~waiter_scope_t() { n.fetch_sub(1, std::memory_order_seq_cst); }
        } scope(st.waiters);
        return vertex_stripe_cv(idx).wait_for(
            lock, timeout, [&] { return write_seq_.load(std::memory_order_seq_cst) != seq0; });
    }

    /** @brief The current write sequence (bumped per assign — the await predicate base). */
    [[nodiscard]] std::uint64_t current_seq() const {
        // Lock-free (#555): the sequence is atomic, and a publish no longer holds the stripe
        // mutex while bumping it — so taking the lock here would synchronize against nothing.
        return write_seq_.load(std::memory_order_seq_cst);
    }

    /**
     * @brief Advance the STREAM drain cursor to "now" WITHOUT draining (RFC-0008 §E):
     *        an eager delivery already flushed the ring, so a later sweep must not
     *        re-deliver.
     */
    void mark_flushed() {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        if (vertex_ext_t* e = ext_.load(std::memory_order_acquire)) e->appended_since_flush = 0;
    }

    /**
     * @brief Drain the STREAM ring entries appended since the last flush, in order —
     *        a queue, not a coalesce (RFC-0008 §E) — and advance the drain cursor.
     *
     * Snapshots under the lock into @p out (caller storage; overwritten); the caller
     * delivers OUTSIDE the lock. Entries trimmed out of the keep-last ring before this
     * drain are lost (bounded history). The snapshot growth is NOTHROW (#477): on OOM
     * the drain returns 0 WITHOUT advancing the cursor, so the entries re-drain on the
     * next covering flush — deferred, never lost, never an abort.
     *
     * @note #981 residual: "never an abort" holds where the growth THROWS. Under
     *       `-fno-exceptions` `%tr::detail::try_reserve` degrades to probe-then-commit — the
     *       probe block is freed before `reserve` takes one and a context switch in that
     *       window makes the `reserve` abort() the node (#850). The snapshot cannot take the
     *       ADR-0065 `%tr::mem::block_array_t` seam: its element is a `std::shared_ptr`,
     *       which the seam's memcpy relocation would tear, and @p out is caller storage of a
     *       type this signature fixes.
     * @note Counts ring APPENDS, never a `write_seq_` delta (#925): that sequence is the
     *       await/readiness cursor and bumps on a SHED append too, so the surplus would
     *       re-take an ALREADY-FLUSHED entry — a drain removes nothing from the ring.
     * @param out       Caller storage the drained entries are assigned into (overwritten).
     * @param gap_before Optional out: shed points observed on this ring since the previous
     *                  drain — the in-order `tr::flow::address_shift_gap` signal of RFC-0025
     *                  §4.4/§4.5. Non-zero means entries the consumer would have seen are
     *                  MISSING immediately before this batch. Written unconditionally when
     *                  non-null, including on the zero-drain returns, so a consumer that
     *                  polls a quiet ring still learns about a shed.
     * @return The number of entries drained (0 ⇒ nothing appended since the last flush,
     *         or the snapshot could not be allocated — retry on the next flush).
     */
    std::size_t drain_unflushed(std::vector<std::shared_ptr<const rope_t>>& out,
                                std::uint64_t* gap_before = nullptr) {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        if (e == nullptr) {
            if (gap_before != nullptr) *gap_before = 0;
            return 0;  // no ring — nothing was ever appended
        }
        if (gap_before != nullptr) {
            *gap_before = e->ring ? e->ring->gaps - e->ring->gaps_drained : 0;
            if (e->ring) e->ring->gaps_drained = e->ring->gaps;
        }
        // A non-zero count implies a ring: the counter is bumped only where the append
        // lands (which creates it), and `retire` clears the two together.
        if (e->appended_since_flush == 0 || !e->ring) return 0;
        std::deque<ring_entry_t>& entries = e->ring->entries;
        const auto take = static_cast<std::ptrdiff_t>(
            std::min<std::uint64_t>(e->appended_since_flush, entries.size()));
        // Nothrow-reserve BEFORE the cursor reset: a failed snapshot leaves the appends
        // marked un-flushed (deferred delivery), instead of a throwing assign (#477).
        if (!tr::detail::try_reserve(out, static_cast<std::size_t>(take))) return 0;
        e->appended_since_flush = 0;
        out.clear();
        for (auto it = entries.end() - take; it != entries.end(); ++it)
            out.push_back(it->value);  // within capacity — refcount shares, no byte copy
        return out.size();
    }

    /** @brief The STREAM ring contents, oldest first — each entry a rope clone (refcount
     *         bumps, no byte copy). */
    [[nodiscard]] std::vector<rope_t> history_snapshot() {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        std::vector<rope_t> out;
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        if (e == nullptr || !e->ring) return out;
        out.reserve(e->ring->entries.size());
        for (const ring_entry_t& entry : e->ring->entries) out.push_back(*entry.value);
        return out;
    }

    /** @brief Total bytes this receiver currently holds RESERVED against its injected ring
     *         source — the byte bound's observable (RFC-0025 §4.6.1 clause 3). Zero on a
     *         vertex that has never admitted an entry. */
    [[nodiscard]] std::size_t ring_reserved_bytes() const {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        if (e == nullptr || !e->ring) return 0;
        std::size_t n = 0;
        for (const ring_entry_t& entry : e->ring->entries) n += entry.bytes;
        return n;
    }

    /** @brief Shed points on this receiver's ring since registration — the cumulative
     *         `tr::flow::address_shift_gap` census (RFC-0025 §4.4: a shed with no accounting
     *         is non-conforming). */
    [[nodiscard]] std::uint64_t ring_gap_count() const {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        return e != nullptr && e->ring ? e->ring->gaps : 0;
    }

    // -- subscription edges -----------------------------------------------------------

    /**
     * @brief Append a subscription edge; atomically snapshot the transient-local
     *        durability latch when @p latch is non-null.
     *
     * Under ONE lock hold: the slot is appended, and — iff THIS subscriber requested
     * durability (`policy.durability_request()`, RFC-0022 §3.A) and the vertex already
     * holds an LKV — the value plus the new edge's dispatch view are snapshotted into
     * @p latch, so a concurrent `clear_edge` can never slip between append and latch. The
     * caller dispatches the latch OUTSIDE the lock (RFC-0004 §D / ADR-0049).
     *
     * The predicate is the SUBSCRIBER's, not the vertex's: before RFC-0022 one
     * `settings.durability` flag latched for every subscriber of a vertex, including the
     * ones that never asked.
     *
     * An INACTIVE slot is REUSED before the list grows (RFC-0009 §D.2: "a cleared
     * slot MAY be reused by a later append") — the reclamation half of eviction:
     * a churning link (unsubscribe / peer departure, then re-subscribe) reoccupies
     * its freed slots instead of growing `subs_` without bound. Slot indices of
     * ACTIVE edges are never renumbered (§D.2 stability); only a slot already
     * cleared can come to mean a new edge.
     * The edge is PUBLISHED under the same lock hold, in the position the slot append itself
     * holds (#635): the caller's `own_subs_` seq_cst bump still precedes this whole verb, so
     * ADR-0049's Dekker pairing against a skipping publisher is byte-for-byte the one #708
     * landed. Displaced arrays are scanned AFTER the lock is dropped, on this thread.
     *
     * @return The occupied slot's index (the `:subscribers[N]` slot number), or @ref kNoSlot
     *         when the edge array could not be allocated — nothing was admitted and the
     *         previously published array is untouched, so the vertex is unchanged.
     */
    std::size_t add_edge(subscriber_t s, edge_latch_t* latch = nullptr) {
        edge_block_t* b = nullptr;
        std::size_t idx = kNoSlot;
        {
            const std::lock_guard lock(vertex_stripe_of(this).m);
            b = ensure_edges();
            if (b == nullptr) return kNoSlot;  // OOM on the block itself: admit nothing
            std::vector<subscriber_t>& subs = b->slots;
            idx = subs.size();
            for (std::size_t i = 0; i < subs.size(); ++i) {
                if (!subs[i].active) {
                    idx = i;
                    break;
                }
            }
            if (idx == subs.size())
                subs.push_back(std::move(s));
            else
                subs[idx] = std::move(s);  // reuse frees the cleared slot's leftovers
            if (!try_publish_edges(*b)) {
                // The new edge could not be published. ROLL THE SLOT BACK rather than leave a
                // subscriber the fan-out will never see: the array still on the slot lists
                // exactly the pre-existing actives, so undoing the append leaves publisher and
                // master consistent with each other and with the caller's kNoSlot answer.
                subscriber_t reclaimed;
                reclaimed.active = false;
                subs[idx] = std::move(reclaimed);
                return kNoSlot;
            }
            if (latch != nullptr && subs[idx].policy.durability_request()) {
                if (std::shared_ptr<const rope_t> lkv = lkv_.load()) {
                    latch->value = std::move(lkv);
                    latch->edge = edge_view_of(subs[idx]);
                }
            }
        }
        scan_retired_edges(*b);  // outside the lock, on the mutator's thread — never waits
        return idx;
    }

    /**
     * @brief Deactivate the edge slot @p idx (unsubscribe — a cleared `:subscribers[N]`).
     * @param retired_ctx Optional out-parameter receiving the slot's `callback_ctx` — the one
     *        leg of a dispatch snapshot the library holds no owning copy of, so ADR-0080's
     *        reclamation seam needs it back to hand to a release hook. Read UNDER the lock,
     *        before the shell displaces the slot, because after that the pair is gone. Written
     *        only on the `true` return; left untouched when nothing was cleared.
     * @return true iff the slot existed and was active (the caller then adjusts the
     *         RFC-0005 listener bookkeeping).
     */
    bool clear_edge(std::size_t idx, void** retired_ctx = nullptr) {
        edge_block_t* b = nullptr;
        {
            const std::lock_guard lock(vertex_stripe_of(this).m);
            b = edges_locked();
            if (b == nullptr) return false;
            std::vector<subscriber_t>& subs = b->slots;
            if (idx >= subs.size() || !subs[idx].active) return false;
            if (retired_ctx != nullptr) *retired_ctx = subs[idx].callback_ctx;
            // RECLAIM in place, not merely deactivate. Flipping `active` alone left the slot's
            // `target_key` buffer, its `source_view` segment pin and the whole cold `remote`
            // half resident until an unrelated `add_edge` happened to land on this index — so
            // an unsubscribed edge kept a frame segment alive indefinitely. This is the same
            // move-an-inert-shell reclaim @ref evict_link_edges already performs under this
            // very lock, and it is safe for the same reason: an @ref edge_view_t snapshot HOLDS
            // the target key and the whole cold half by refcount (ADR-0041 §2, #1448), so
            // releasing the pin here can never dangle a dispatch already in flight.
            //
            // The 80-byte slot SHELL stays — RFC-0009 §D.2 makes `:subscribers[]` indices
            // stable, so the vector must not shrink; only the retained state is freed.
            subscriber_t reclaimed;    // an inert shell: no view, no route, no cold half
            reclaimed.active = false;  // the slot is free for add_edge reuse
            subs[idx] = std::move(reclaimed);
            // Stop delivering to it AT ONCE and unconditionally (#635): the monotone bit is
            // allocation-free, so an unsubscribe can never fail to take effect. Republishing
            // is what actually releases the published entry's own refcount clones, and it is
            // allowed to fail — the bit already made that a memory question, not a
            // correctness one.
            deactivate_published(*b, idx);
            (void)try_publish_edges(*b);
        }
        scan_retired_edges(*b);
        return true;
    }

    /** @brief Outcome of @ref replace_edge — tells the caller which bookkeeping it owes. */
    enum class edge_replace_t {
        OUT_OF_RANGE,    /**< @brief No slot @p idx exists; nothing was written. */
        FILLED_EMPTY,    /**< @brief The slot existed but was cleared — this is an ADD. */
        REPLACED_ACTIVE, /**< @brief A live edge was swapped out; the listener count is unchanged.
                          */
    };

    /**
     * @brief Replace the edge occupying slot @p idx (RFC-0009 §D.1), snapshotting the
     *        transient-local durability latch under the SAME single lock hold as
     *        @ref add_edge.
     *
     * §D.1 makes an indexed `:subscribers[N]` write of a `SUBSCRIBER` *replace* that
     * slot rather than destroy it. The latch is taken here, not by the caller, for the
     * reason @ref add_edge gives: a concurrent @ref clear_edge must not be able to slip
     * between the write and the snapshot. The caller dispatches OUTSIDE the lock.
     *
     * Move-assigning the slot reclaims the displaced edge's `source_view` segment pin and
     * cold `remote` half in place, exactly as @ref clear_edge does — a replace must not
     * leak the frame segment the old edge pinned.
     *
     * **This never grows `subs_`.** An out-of-range @p idx is refused rather than
     * back-filled with inactive shells: the index arrives off the wire, so growing on
     * demand would let a peer allocate an arbitrary number of slots with a single
     * `:subscribers[65535]` write. Slot indices are stable per §D.2, so a slot that does
     * not exist yet is not addressable.
     *
     * @param idx   The `:subscribers[N]` slot number.
     * @param s     The replacing edge.
     * @param latch Optional durability latch; snapshotted iff the REPLACING subscriber
     *              requested durability (RFC-0022 §3.A) and the vertex holds an LKV.
     * @return Which case applied — see @ref edge_replace_t.
     */
    edge_replace_t replace_edge(std::size_t idx, subscriber_t s, edge_latch_t* latch = nullptr) {
        edge_block_t* b = nullptr;
        edge_replace_t result = edge_replace_t::OUT_OF_RANGE;
        {
            const std::lock_guard lock(vertex_stripe_of(this).m);
            b = edges_locked();
            if (b == nullptr) return edge_replace_t::OUT_OF_RANGE;
            std::vector<subscriber_t>& subs = b->slots;
            if (idx >= subs.size()) return edge_replace_t::OUT_OF_RANGE;
            const bool was_active = subs[idx].active;
            subs[idx] = std::move(s);  // reclaims the displaced edge's pins in place
            // The OLD edge must stop receiving before the new one starts, and that half is
            // infallible; the republish that installs the REPLACEMENT may soft-fail on OOM, in
            // which case the slot holds the new edge and the publisher delivers to neither
            // until the next successful mutation (#477 — a dropped delivery, never a delivery
            // to a torn-down `callback_ctx`).
            deactivate_published(*b, idx);
            (void)try_publish_edges(*b);
            if (latch != nullptr && subs[idx].policy.durability_request()) {
                if (std::shared_ptr<const rope_t> lkv = lkv_.load()) {
                    latch->value = std::move(lkv);
                    latch->edge = edge_view_of(subs[idx]);
                }
            }
            result = was_active ? edge_replace_t::REPLACED_ACTIVE : edge_replace_t::FILLED_EMPTY;
        }
        scan_retired_edges(*b);
        return result;
    }

    /**
     * @brief Deactivate AND reclaim every active subscriber edge stored against the
     *        link @p link — the per-vertex half of peer-departure eviction (RFC-0009
     *        §D, extended to link teardown).
     *
     * Matches each active slot on the link it was ADMITTED over: the cold half's
     * `subscriber_remote_t::link` when it carries one, and otherwise its
     * `subscriber_remote_t::caller` — the two spellings the two admission doors
     * leave behind for the SAME fact. `subscribe_wire` (the `SUBSCRIBE` op and the
     * wire `:subscribers[]` append) stores both; `graph_t::field_write`'s
     * `:subscribers[]` / `:subscribers[N]` arms store ONLY the context, because
     * those edges deliver to a LOCAL target and have no return route to send over.
     * Matching `link` alone therefore left a field-write-admitted edge permanently
     * un-evictable — active, counted, and still fanning out to its target under a
     * gate context whose session had departed (#943). ADR-0018 defines that context
     * as this node's NAME for the inbound link a remote `FWD` arrived on, i.e. the
     * same name space @p link is spelled in, so the fallback compares like with
     * like. A local edge still never matches a real link name: a local door passes
     * the EMPTY context and stores no cold half at all (the one exception,
     * `parse_subscriber_tlv`'s `delivery_compact` opt-in, leaves both spellings
     * empty). An EMPTY @p link matches nothing at all and returns 0 — a link with no
     * name never subscribed, and without that rule the empty key compared equal to
     * exactly those empty spellings and reclaimed every local `delivery_compact`
     * edge on the vertex (#1056). Unlike @ref clear_edge, a matched slot
     * is RECLAIMED, not just flagged: the stored SUBSCRIBER view, the return-route
     * refcount pin, the target key, and the whole `subscriber_remote_t` block are
     * released in place (the slot shell stays — §D.2 index stability — and
     * @ref add_edge reuses it). An in-flight delivery is unaffected: its
     * @ref edge_view_t snapshot HOLDS the target key and the whole `subscriber_remote_t`
     * by refcount (ADR-0041 §2, #1448), so releasing the slot's pin here never dangles a
     * dispatch — the record it reads outlives this eviction by construction.
     * @return The number of edges evicted (the caller unwinds exactly this many
     *         from the RFC-0005 listener bookkeeping).
     */
    std::size_t evict_link_edges(std::string_view link) {
        // The EMPTY key matches NOTHING (#1056). Every local door leaves both spellings empty,
        // so without this an empty parameter compared EQUAL to a local edge's admitting link
        // and reclaimed it — reachable for the `delivery_compact` opt-in, the one local shape
        // that carries a cold half at all. A non-empty key is unaffected: an empty
        // `admitted_over` can never equal it, so this only ever short-circuits the no-op case.
        if (link.empty()) return 0;
        edge_block_t* b = nullptr;
        std::size_t n = 0;
        {
            const std::lock_guard lock(vertex_stripe_of(this).m);
            b = edges_locked();
            if (b == nullptr) return 0;
            std::vector<subscriber_t>& subs = b->slots;
            for (std::size_t i = 0; i < subs.size(); ++i) {
                subscriber_t& s = subs[i];
                if (!s.active || s.remote == nullptr) continue;
                // The link this edge was ADMITTED over — see the declaration comment. Not
                // `link` alone: a `graph_t::field_write` admission stores the inbound link
                // ONLY as the gate context, so keying on the delivery link skipped it
                // forever (#943). No copy: both members are `std::string`.
                const std::string& admitted_over =
                    s.remote->link.empty() ? s.remote->caller : s.remote->link;
                if (admitted_over != link) continue;
                subscriber_t reclaimed;       // an inert shell: no view, no route, no cold half
                reclaimed.active = false;     // the slot is free for add_edge reuse
                s = std::move(reclaimed);     // frees the old slot's retained state in place
                deactivate_published(*b, i);  // infallible: the departed peer stops receiving
                ++n;
            }
            if (n != 0) (void)try_publish_edges(*b);
        }
        if (n != 0) scan_retired_edges(*b);
        return n;
    }

    /**
     * @brief Reclaim the remote edges whose delivery @p link AND stored return @p route both
     *        match — the per-vertex half of `graph_t::evict_route_edges` (#1223 step 5).
     *
     * The narrow sibling of @ref evict_link_edges — where that one reclaims EVERY edge a
     * departed link admitted, this one reclaims exactly the edge(s) whose next hop refused
     * the stored route with an addressed `tr::path::invalid` (RFC-0020) — the one wire
     * observation a producer gets about a route whose terminal session departed. Matching
     * `link` alone would evict every edge sharing the mount; matching the route alone would
     * let any link speak for another's edges — both keys are required, and the route compare
     * is BYTE-equal on the stored PATH TLV (the same bytes `deliver_remote` emits as the
     * delivery `dst`, which are the bytes the refusing hop echoes back — see
     * `reject_bus_name_hop`'s swap). Only `subscribe_wire`-door edges qualify: a field-write
     * edge stores no route, and `route` never compares equal to its empty view. An EMPTY
     * @p link or @p route matches nothing, as in @ref evict_link_edges (#1056).
     *
     * @param link  This node's NAME for the link the refusal arrived on (== the edge's
     *              delivery link).
     * @param route The refused route — the whole TLV bytes echoed by the rejecting hop: a
     *              canonical PATH, or (RFC-0024 §7.1 amendment 1) the bound `PATH_REF` a
     *              reverse-list delivery was refused as.
     * @param bound_echo True ⇔ @p route is the `PATH_REF` form — the caller classified the
     *              echo's type byte (this header stays wire-type-agnostic), and the match
     *              runs against the stored reverse list's emitted suffix instead of the
     *              canonical return route.
     * @return The number of edges evicted (the caller unwinds exactly this many from the
     *         RFC-0005 listener bookkeeping).
     */
    std::size_t evict_route_edges(std::string_view link, std::span<const std::byte> route,
                                  bool bound_echo = false) {
        if (link.empty() || route.empty()) return 0;
        edge_block_t* b = nullptr;
        std::size_t n = 0;
        {
            const std::lock_guard lock(vertex_stripe_of(this).m);
            b = edges_locked();
            if (b == nullptr) return 0;
            std::vector<subscriber_t>& subs = b->slots;
            for (std::size_t i = 0; i < subs.size(); ++i) {
                subscriber_t& s = subs[i];
                if (!s.active || s.remote == nullptr) continue;
                // The delivery link, not the admission fallback: only a `subscribe_wire`
                // edge has a route to be refused, and that door populates `link` and the
                // route together (see subscriber_remote_t::return_route's invariant).
                if (s.remote->link != link) continue;
                bool hit = false;
                if (!bound_echo) {
                    const std::span<const std::byte> stored = s.remote->return_route.bytes();
                    hit = stored.size() == route.size() &&
                          std::equal(stored.begin(), stored.end(), route.begin());
                } else if (!s.remote->reverse_route.empty()) {
                    // The BOUND twin (RFC-0024 §7.1 amendment 1): a delivery that rode the
                    // reverse list is refused as a `PATH_REF` echo — the emitted `dst`,
                    // which is the stored list MINUS the element this node consumed
                    // locally. Matched ELEMENT-WISE against the stored suffix rather than
                    // whole-TLV byte-equal, because the refusing hop re-encodes the echo
                    // and only the 8-byte element array is canonical by grammar
                    // (`opt.PL`/`LL` MUST be 0 — RFC-0024 §4.2); comparing re-encoded
                    // header bytes would couple eviction to an encoder detail. Both bodies
                    // sit behind fixed 4-byte headers for the same grammar reason.
                    const std::span<const std::byte> rev = s.remote->reverse_route.bytes();
                    const std::span<const std::byte> echo_body =
                        route.size() > 4 ? route.subspan(4) : std::span<const std::byte>{};
                    const std::span<const std::byte> rev_tail =
                        rev.size() > 4 + wire::kPathRefElementBytes
                            ? rev.subspan(4 + wire::kPathRefElementBytes)
                            : std::span<const std::byte>{};
                    hit = !rev_tail.empty() && echo_body.size() == rev_tail.size() &&
                          std::equal(rev_tail.begin(), rev_tail.end(), echo_body.begin());
                }
                if (!hit) continue;
                subscriber_t reclaimed;       // an inert shell: no view, no route, no cold half
                reclaimed.active = false;     // the slot is free for add_edge reuse
                s = std::move(reclaimed);     // frees the old slot's retained state in place
                deactivate_published(*b, i);  // the refused route stops receiving
                ++n;
            }
            if (n != 0) (void)try_publish_edges(*b);
        }
        if (n != 0) scan_retired_edges(*b);
        return n;
    }

    /**
     * @brief What a snapshot DECLINED to hand back: the deliveries a vertex shed before
     *        the graph could dispatch them (#896).
     *
     * `snapshot_edges` is allowed to come back short, and the way it can is a specified
     * drop rather than an abort (#477). A drop nobody counts, though, is indistinguishable
     * from a delivery that never had to happen — which is how a whole fan-out could be
     * shed under memory pressure while `graph_t::delivery_drops()`, the one observable,
     * read zero. `vertex_t` owns no counters (it is the storage layer, not the
     * instrumentation layer): it reports the tally by reference and `graph_t::fan_out`
     * folds it into the graph's per-cause counters at the frame that owns them.
     *
     * **There used to be a second cause, and #1448 deleted the failure, not the report.**
     * A per-edge `out_of_memory` counted the edges whose owning link / caller copies could
     * not be allocated. @ref edge_view_t no longer copies them — it takes a refcount share
     * of the immutable cold half — so the per-edge snapshot reaches no allocator on any
     * edge shape and cannot fail. What remains is the capacity degrade: a fan-out wider
     * than the inline snapshot, on a heap that would not lend it a buffer. The
     * OUT_OF_MEMORY *delivery* cause is untouched and still counted from the legs that can
     * still hit it (`graph_t::dispatch_edge_target`'s rope clone, the store legs).
     */
    struct snapshot_drops_t {
        /** @brief Edges past the inline prefix, abandoned because the overflow buffer for a
         *         wide fan-out could not be reserved — the capacity degrade. */
        std::uint32_t truncated = 0;
        /** @brief Did this snapshot shed anything? The ONE test a clean fan-out pays. */
        [[nodiscard]] bool any() const noexcept { return truncated != 0; }
    };

    /**
     * @brief Snapshot every ACTIVE edge's dispatch view into caller storage — the
     *        snapshot-under-pin half of the snapshot/dispatch-after-release discipline.
     *
     * Small fan-out (the common case, ≤ `kInlineFanout`) placement-constructs into
     * @p inline_buf — no heap allocation AND no dead stack zeroing per publish; a
     * larger subscriber list reserves @p overflow once and fills it instead (then
     * @p overflow is non-empty and holds ALL views).
     *
     * The ONE way this can come back short is NOTHROW (#477 — this runs on the writer
     * thread's fan-out, where a bad_alloc is an abort() under `-fno-exceptions`): an
     * unreservable @p overflow degrades the snapshot to the first `kInlineFanout` views in
     * @p inline_buf, and the rest of this delivery is dropped. It is TALLIED into @p drops
     * (@ref snapshot_drops_t) so the caller can report it; it is not silent (#896).
     * The per-edge copy itself cannot fail at all since #1448 — it is two pointer copies
     * and two refcounts on EVERY edge shape, remote included — so the whole snapshot
     * reaches an allocator only for that one overflow reservation.
     * **NO LOCK (#635).** The source is the vertex's PUBLISHED, immutable-after-publish edge
     * array, read under a bounded per-participant EDGE PIN (`%edge_pin.hpp`) whose scope is
     * this copy loop and nothing else — released before the caller's first `dispatch_edge`, so
     * a subscriber callback that re-enters the graph always finds this thread's cell empty
     * (`pin_t` asserts it). What this deletes is the stripe mutex, which serialised the
     * publishes of every vertex that merely HASHED to the same stripe: measured at ×16.6 with
     * NEGATIVE scaling past four threads. What it does not add is any shared-cacheline RMW —
     * the announcement is a `seq_cst` store to this thread's own isolated cell, which is the
     * whole reason a refcounted published array was rejected instead.
     *
     * Fallback: a thread that cannot claim a pin (more publishers than
     * `kEdgePinSlots`) copies the CURRENT array under the stripe mutex — safe
     * because displacing an array requires that same lock. Correctness never depends on the
     * constant; only scaling does.
     * @param inline_buf The caller's raw stack buffer (cleared on entry).
     * @param overflow   The heap fallback for large fan-out (cleared on entry).
     * @param drops      Out: what this snapshot SHED (@ref snapshot_drops_t), zeroed on
     *                   entry. By reference, not optional — a caller that may not see the
     *                   shed count is the #896 defect itself.
     * @return The number of views snapshotted (into whichever buffer was used).
     */
    std::size_t snapshot_edges(edge_snapshot_t& inline_buf, std::vector<edge_view_t>& overflow,
                               snapshot_drops_t& drops) {
        inline_buf.clear();
        overflow.clear();
        drops = snapshot_drops_t{};
        edge_block_t* b = edges_.load(std::memory_order_acquire);
        if (b == nullptr) return 0;  // never subscribed: no block was ever allocated
        detail_ep::pin_t pin;
        if (!pin.valid()) {  // domain exhausted: the pre-#635 path, for these threads only
            const std::lock_guard lock(vertex_stripe_of(this).m);
            return copy_published(b->pub.load(std::memory_order_acquire), inline_buf, overflow,
                                  drops);
        }
        const std::size_t n = copy_published(pin.acquire(b->pub), inline_buf, overflow, drops);
        pin.release();  // BEFORE the caller dispatches — the invariant `pin_t` asserts
        return n;
    }

    /**
     * @brief The stored SUBSCRIBER TLV view of the active slot @p idx (a `:subscribers[N]`
     *        read) — a refcount clone, no byte copy; `nullopt` for a missing / inactive /
     *        TLV-less (in-process sugar) slot.
     */
    [[nodiscard]] std::optional<view_t> edge_source(std::size_t idx) {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        const edge_block_t* b = edges_locked();
        if (b == nullptr) return std::nullopt;
        const std::vector<subscriber_t>& subs = b->slots;
        if (idx < subs.size() && subs[idx].active && subs[idx].source_view.owner)
            return subs[idx].source_view;  // clone (refcount bump)
        return std::nullopt;
    }

    /** @brief Every active slot's stored SUBSCRIBER view, in slot order (the
     *         `:subscribers[]` array read) — each a refcount clone. */
    [[nodiscard]] std::vector<view_t> edge_sources() {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        std::vector<view_t> out;
        const edge_block_t* b = edges_locked();
        if (b == nullptr) return out;
        out.reserve(b->slots.size());
        for (const subscriber_t& s : b->slots)
            if (s.active && s.source_view.owner) out.push_back(s.source_view);
        return out;
    }

    // -- ACL state (#81, ADR-0018/0020) -------------------------------------------------

    /**
     * @brief Restore this vertex to the state an unregistered PLACEHOLDER carries — the
     *        `unregistered ⇒ carries no state` invariant retirement re-establishes
     *        (RFC-0009
     *        §B.6). Clears **everything a `fill()` installs plus everything it leaves
     *        behind**, so a later revive of this address inherits nothing of the retired
     *        owner: the value seam (swap-and-park, never freed — a lock-free reader may
     *        still hold the old pointer), the stored value and history, the `:acl` (own
     *        ACEs + the cached merge), the app-field table, the storage policy, the role,
     *        and the delivery mode. **Survives** by design: `write_seq_` (monotonic per
     *        address; a reset would regress the readiness cursors), `listeners_above_`
     *        (counts ANCESTOR subscribers, which retiring THIS vertex never touched — the
     *        graph adjusts it for cleared descendant edges), and the allocation / name /
     *        links (ADR-0057 insert-only — emptied, never freed or detached).
     *
     * @note `registered_` is NOT touched here — it is map-lock state the graph flips. The
     *       caller MUST hold the graph map lock. This RETURNS the swapped-out value-seam
     *       block (or nullptr) rather than freeing it: a lock-free reader may still hold
     *       the old pointer, so the graph parks it and the embedder frees the park through
     *       `graph_t::collect()` (#576). The per-vertex stripe lock is taken internally.
     *
     * @return the detached seam block to park, or nullptr if this vertex had none.
     */
    [[nodiscard]] value_handlers_t* revert_to_placeholder() {
        // Atomics first — no lock needed, and clearing own ACEs before anything else is
        // fail-closed: the graph's bearing-ancestor walk (the OWN_ACES bit) skips this vertex
        // immediately, so a concurrent gated op on a descendant stops seeing the retired
        // owner's policy at once (it climbs to the live ancestor instead).
        // Bump BEFORE anything else is torn down (ADR-0062): a holder comparing generations
        // must see the invalidation no later than it could observe the reverted state, so a
        // cached resolution can never be used against a vertex already mid-revert.
        //
        // SATURATING, never wrapping (RFC-0024 §4.4 rule 3, normative in §9.3). A wrapped
        // generation is #603's misroute with the guard instead of the address: a stale
        // bound-path element would compare EQUAL again and the operation would land on the
        // vertex's successor. The CAS loop is the whole of the rule — at the ceiling the
        // counter stops, the vertex becomes permanently unbindable, and every mint for it
        // falls back to the canonical form (the same degrade the label allocator takes at
        // exhaustion). Contended only against another retire of the SAME vertex, which the
        // map lock already excludes, so the loop is uncontended in practice.
        for (std::uint32_t g = retire_gen_.load(std::memory_order_relaxed);
             g != kGenerationSaturated;) {
            if (retire_gen_.compare_exchange_weak(g, saturating_next_generation(g),
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed))
                break;
        }
        set_flag(flag_t::OWN_ACES, false);
        // The hide bit is part of the retiring occupant's identity, not of the address: the
        // next registration at this key is a different vertex kind and must be listed unless
        // it asks not to be (RFC-0014 §3 / S4).
        set_flag(flag_t::ENUM_HIDDEN, false);
        // Same argument for the payload-right declaration (RFC-0014 Amendment 2): it belongs
        // to the retiring occupant, not to the address. The graph's rows for this vertex stay
        // parked and unreachable — nothing consults them while this bit is clear — and a
        // re-registration that declares again publishes its own, newer rows.
        set_flag(flag_t::PAYLOAD_RIGHTS, false);
        // And for the admission filters, for the third time the same reason: they belong to the
        // retiring occupant, not to the address. The graph's node for this vertex stays parked
        // and unreachable — nothing consults it while this bit is clear — and a re-registration
        // that installs a filter again prepends its own, newer node.
        set_flag(flag_t::ADMISSION, false);
        lkv_.clear(std::memory_order_release);  // a mid-read reader holds its own
                                                // reference — safe under either policy.
        own_subs_.store(0, std::memory_order_relaxed);
        // The placeholder default (see graph.cpp). Relaxed, and atomic since #1477: the
        // lock-free readers on the write path (`write_impl`'s role forks) run with no map
        // lock, so a plain byte store here was a data race — UB, exactly like the
        // `delivery_mode_` byte #895 had to move for the same reason.
        role_.store(role_t::STORED_VALUE, std::memory_order_relaxed);
        // graph drops the unconditional_ entry (graph_t::retire, once the map lock is out).
        delivery_mode_.store(delivery_mode_t::IF_NEWER, std::memory_order_relaxed);
        value_handlers_t* detached = nullptr;
        if (vertex_ext_t* e = ext_.load(std::memory_order_acquire); e != nullptr) {
            // The value seam is read lock-free — swap it out atomically and hand the old
            // block back to the caller to PARK (never free it under a possible concurrent
            // reader). The remaining ext fields are mutated under the stripe lock.
            detached = e->handlers.exchange(nullptr, std::memory_order_acq_rel);
            const std::lock_guard lock(vertex_stripe_of(this).m);
            // `~ring_state_t` hands every held reservation back to the source that served it,
            // so dropping the block here cannot leak the ring's byte budget.
            e->ring.reset();
            e->acl_present = false;
            e->aces.clear();
            e->eff_aces.clear();
            invalidate_acl_cache(*e);  // ADR-0078: nothing here a rebuilder can clobber
            e->history_keep_last = 1;
            e->pin_payload_ratio = 0;
            e->app.reset();
            e->appended_since_flush = 0;  // cleared WITH `ring` — the drain's invariant
        }
        // The edge block is stripe-guarded; clear it in its own critical section (both it and
        // the ext block may be absent). The graph has already adjusted descendant
        // listeners_above_ for these edges before calling us. Publishing the EMPTY array
        // allocates nothing, so retirement can never fail to stop delivering.
        edge_block_t* b = nullptr;
        {
            const std::lock_guard lock(vertex_stripe_of(this).m);
            b = edges_locked();
            if (b != nullptr) {
                b->slots.clear();
                (void)try_publish_edges(*b);  // slots are empty ⇒ publishes null, cannot fail
            }
        }
        if (b != nullptr) scan_retired_edges(*b);
        return detached;
    }

    /**
     * @brief Store this vertex's `:acl` as typed ACEs — the ONLY stored ACL state (#907).
     *
     * Storing replaces, and marks the ACL PRESENT: an empty list is the sanctioned
     * clear-enforcement write (⇒ no restrictions) and still reads back as an empty ACL,
     * not as the NOT_FOUND of a vertex that never had one. Takes no raw bytes, because
     * there is no second copy to fall out of step with the list evaluation walks — an
     * `:acl` read re-encodes from here.
     */
    void set_acl(std::vector<ace_t> aces) {
        vertex_ext_t& e = ensure_ext();
        const std::lock_guard lock(vertex_stripe_of(this).m);
        e.aces = std::move(aces);
        e.acl_present = true;
        // Lock-free bearing flag (#361 §3): the graph's nearest-bearing-ancestor walk
        // reads it without touching any stripe. Publish under the lock, before the
        // generation bump, same ordering discipline as the ACE list itself.
        set_flag(flag_t::OWN_ACES, !e.aces.empty());
        // Publish-then-invalidate (ADR-0050 cache protocol, ADR-0078 counter): the new ACEs
        // are visible under m_ BEFORE the counter turns odd, so a rebuild that observes the
        // new value always reads the new list. Turning it odd is the WHOLE invalidation —
        // there is no second flag a concurrent rebuilder could clear over it — and it also
        // defeats the publish CAS of any rebuild already in flight over the OLD list, which
        // is what stops a stale merge being stamped current.
        invalidate_acl_cache(e);
    }

    /**
     * @brief Run @p f over this vertex's whole `:acl` state — the presence bit and the
     *        parsed ACE list, read together under ONE hold — the read-back accessor (#907).
     *
     * The caller re-encodes the list it is handed (`graph::encode_acl` lives a layer up and
     * cannot be named from here), which is what makes an `:acl` read canonical: it serves a
     * projection of the SAME list `acl_allows` evaluates, so the two can no longer disagree.
     * Presence and list travel together because a clear that landed between two accessors
     * would otherwise be served as an ACL that no longer exists.
     *
     * @p f must not re-enter this vertex — the lock is held.
     * @return Whatever @p f returns.
     */
    template <typename F>
    auto with_acl(F&& f) -> decltype(f(false, std::declval<const std::vector<ace_t>&>())) {
        static const std::vector<ace_t> kNoAces{};
        const std::lock_guard lock(vertex_stripe_of(this).m);
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        if (e == nullptr) return f(false, kNoAces);
        return f(e->acl_present, e->aces);
    }

    /**
     * @brief Run @p f over this vertex's parsed ACE list under the vertex lock — the
     *        zero-copy evaluation accessor (`graph_t::acl_allows` hands the list to the
     *        pure ADR-0050 policy without snapshotting subject bytes per gated op).
     *
     * @p f must not re-enter this vertex (the lock is held) — it is a pure evaluation
     * over the list, per the ADR-0050 policy contract (no locks/clock/graph inside).
     * @return Whatever @p f returns.
     */
    template <typename F>
    auto with_aces(F&& f) -> decltype(f(std::declval<const std::vector<ace_t>&>())) {
        static const std::vector<ace_t> kNoAces{};
        const std::lock_guard lock(vertex_stripe_of(this).m);
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        return f(e != nullptr ? e->aces : kNoAces);
    }

    /**
     * @brief Mark this vertex's cached effective-ACE merge stale (ADR-0050/0078).
     *
     * Raised by the graph on every `:acl` write for the WRITTEN vertex's whole
     * subtree (subtree-precise invalidation via the ADR-0057 child links —
     * wiring-frequency); @ref set_acl raises it for the written vertex itself.
     * The next @ref with_effective_aces on a marked vertex rebuilds lazily.
     * @note Lock-free (one uncontended CAS) — callable under the graph's map lock
     *       during the subtree walk without touching any vertex mutex.
     */
    void mark_acl_cache_dirty() noexcept {
        // No extension block ⇒ no cached merge exists to invalidate; a block created
        // later starts stale, so a concurrent first-gated-op cannot miss this mark
        // (its rebuild reads ancestor ACEs already published before this walk).
        if (vertex_ext_t* e = ext_.load(std::memory_order_acquire)) {
            // ADR-0078: advancing the counter is the ENTIRE mark. The
            // `acl_cache_dirty.store(true)` that used to follow it could be clobbered by a
            // rebuilder clearing that same flag, pinning a stale merge as clean FOREVER (#880).
            invalidate_acl_cache(*e);
        }
    }

   private:
    /**
     * @brief Advance @p e's ACL-cache counter to the next ODD value — the whole of an
     *        invalidation, and the only write to it outside a publish (ADR-0078).
     * @note Lock-free and callable with NO vertex mutex held; that is the point, since the
     *       subtree fan-out from an ancestor `:acl` write runs under only the graph's map lock.
     */
    static void invalidate_acl_cache(vertex_ext_t& e) noexcept {
        // ALWAYS advance, even when the counter is already odd (already stale): a rebuilder
        // that snapshotted the current odd value would otherwise still win its publish CAS and
        // stamp a merge assembled BEFORE this mark as current. +1 from even, +2 from odd.
        for (std::uint32_t g = e.acl_gen.load(std::memory_order_relaxed);;) {
            if (e.acl_gen.compare_exchange_weak(g, g + 1 + (g & 1u), std::memory_order_release,
                                                std::memory_order_relaxed))
                break;
        }
    }

   public:
    /**
     * @brief Evaluate against this vertex's cached effective-ACE merge, rebuilding
     *        it first iff it is stale — the ADR-0050 cached-merge verb.
     *
     * Staleness is ONE bit of ONE word (ADR-0078): `%vertex_ext_t::acl_gen` is odd. When it
     * is, that odd value and this vertex's own parsed ACEs are SNAPSHOTTED and @p rebuild runs
     * with the stripe lock RELEASED (#361 §2) — the graph's rebuild walks the immutable parent
     * chain taking each ancestor's @ref with_aces one stripe lock at a time, never nested, so
     * an ancestor sharing this vertex's stripe cannot self-deadlock. Back under the lock the
     * rebuilder publishes by CAS-ing that snapshot to snapshot + 1 (even), then @p eval runs.
     *
     * Race resolution (rebuild vs concurrent `:acl` write): every invalidator — @ref set_acl,
     * the placeholder revert, the subtree mark an ancestor `:acl` write fans out (@ref
     * mark_acl_cache_dirty) — advances that one counter via `%invalidate_acl_cache` after
     * publishing its ACEs, lock-free, and does NOTHING else. The recheck and the publish are
     * therefore the SAME atomic operation, so an invalidation landing anywhere in the rebuild
     * defeats the CAS. That is the whole of the coherence argument, and it is what the retired
     * `{acl_gen, acl_cache_dirty}` pair could not give: there they were two ops, and a mark
     * landing between them was overwritten by `dirty = false`, pinning a stale merge as clean
     * FOREVER (#880) — a revoked policy still enforced. A failed CAS also discards a `merged`
     * that may be TORN across the write rather than answering from it. The one premise left is
     * that the counter does not WRAP onto a stale-but-even value (`%vertex_ext_t::acl_gen`).
     *
     * @param rebuild `std::vector<ace_t>(const std::vector<ace_t>& own)` — the
     *                fresh merge over a snapshot of this vertex's own ACEs; runs
     *                UNLOCKED (it may take other vertices' stripes freely).
     * @param eval    Pure evaluation over the cached merge. A BARE descendant evaluates
     *                the merge's `kAceInherit` **subsequence**, which `eval` selects with
     *                `effective_acl_t::allows`'s `required_flags` rather than receiving a
     *                second, pre-projected list — filtering in place is order-identical
     *                and costs no storage. ADR-0050 policy contract: no locks/clock/graph
     *                inside.
     * @return Whatever @p eval returns.
     */
    template <typename Rebuild, typename Eval>
    auto with_effective_aces(Rebuild&& rebuild, Eval&& eval)
        -> decltype(eval(std::declval<const std::vector<ace_t>&>())) {
        vertex_ext_t& e = ensure_ext();  // gated eval caches its merge here (fresh ⇒ stale)
        std::unique_lock lock(vertex_stripe_of(this).m);
        while (true) {
            // The fast path is ONE acquire load and a parity test — what the retired dirty
            // flag cost, which is why the published stamp lives in this word rather than
            // beside it (a second load measured ~1% on the acl-inherit-d4 gate bench).
            const std::uint32_t gen = e.acl_gen.load(std::memory_order_acquire);
            if ((gen & 1u) == 0) break;             // even ⇒ the cached merge is current
            const std::vector<ace_t> own = e.aces;  // snapshot; rebuild runs unlocked
            lock.unlock();
            std::vector<ace_t> merged = rebuild(static_cast<const std::vector<ace_t>&>(own));
            lock.lock();
            std::uint32_t expected = gen;
            if (!e.acl_gen.compare_exchange_strong(expected, gen + 1, std::memory_order_release,
                                                   std::memory_order_relaxed))
                continue;  // an :acl write raced the walk — drop the possibly-torn merge
            // The word says FRESH before the merge lands, but the stripe lock spans both and
            // every reader of eff_aces holds it, so no one can observe the gap.
            e.eff_aces = std::move(merged);
            break;
        }
        return eval(static_cast<const std::vector<ace_t>&>(e.eff_aces));
    }

    // -- application property fields (RFC-0010) ------------------------------------------

    /**
     * @brief Install (or replace) the field descriptor table — the OWNER naming the holes
     *        in the closed `ENOTTY` default (RFC-0010 §A.2), one more store-verbatim verb
     *        on this seam (the `set_acl` pattern).
     *
     * Replacement takes effect atomically with respect to concurrent field operations on
     * this vertex (one lock hold). An empty @p table uninstalls — the vertex reverts to
     * the closed surface, including the pre-RFC synthesized `:schema` shape — and, on a
     * vertex that never had an extension block, allocates nothing (#361 §1: a leaf with
     * no app fields pays nothing).
     */
    void set_app_fields(std::vector<app_field_t> table) {
        if (table.empty() && ext_.load(std::memory_order_acquire) == nullptr) return;
        app_field_table_t built = build_owning_table(std::move(table));
        vertex_ext_t& e = ensure_ext();
        const std::lock_guard lock(vertex_stripe_of(this).m);
        install_app_table(e, std::move(built));
    }

    /**
     * @brief Install a BORROWED descriptor table (ADR-0058): the slots view the caller's
     *        @p table storage directly — zero declaration RAM. The array AND the `name` /
     *        `descriptor` bytes it points at MUST outlive the vertex (static/flash storage);
     *        @ref borrowed_fields_t is what constrains the argument's shape to match.
     *        Declaration only; values are written later via the field-write surface. Same
     *        uninstall-on-empty and allocate-nothing-on-empty-leaf semantics as
     *        @ref set_app_fields.
     */
    void set_app_fields_static(borrowed_fields_t table) {
        if (table.empty() && ext_.load(std::memory_order_acquire) == nullptr) return;
        app_field_table_t built;
        built.slots = table.slots();  // viewed in place — this install allocates NOTHING here
        vertex_ext_t& e = ensure_ext();
        const std::lock_guard lock(vertex_stripe_of(this).m);
        install_app_table(e, std::move(built));
    }

    /** @brief The declared access of the app field @p name (`nullopt` ⇒ undeclared —
     *         the graph's `SCHEMA_NOT_FOUND`). */
    [[nodiscard]] std::optional<app_access_t> app_field_access(std::string_view name) {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        const std::ptrdiff_t i = find_app_slot(e, name);
        if (i < 0) return std::nullopt;
        return e->app->table.slots[static_cast<std::size_t>(i)].access;
    }

    /**
     * @brief Store @p bytes verbatim into the DECLARED app field @p name (RFC-0010 §D —
     *        bytes in, bytes out; no dtype/range validation, the descriptor is consumer
     *        self-description).
     * @return false iff @p name is not declared (e.g. a concurrent table replacement
     *         removed it between the caller's gate and this store).
     */
    bool app_field_store(std::string_view name, std::span<const std::byte> bytes) {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        const std::ptrdiff_t i = find_app_slot(e, name);
        if (i < 0) return false;
        app_field_table_t& t = e->app->table;
        // Class-③ value store: allocated on the FIRST write to any field on this vertex
        // (#389 lazy pattern) — a declared-but-never-written table never pays for it.
        if (t.values == nullptr)
            t.values = std::make_unique<std::vector<std::vector<std::byte>>>(t.slots.size());
        (*t.values)[static_cast<std::size_t>(i)].assign(bytes.begin(), bytes.end());
        return true;
    }

    /** @brief One app-field read outcome — the graph maps these onto the RFC-0002
     *         identities (`SCHEMA_NOT_FOUND` / `NOT_FOUND`). */
    enum class app_read_t {
        UNDECLARED, /**< @brief No such field in the table (or no table) — `ENOTTY`. */
        WRITE_ONLY, /**< @brief Declared `wo` — no read surface (RFC-0010 §A.4). */
        UNSET,      /**< @brief Declared but never written and no initial value. */
        OK,         /**< @brief Value copied out. */
    };

    /** @brief Read the app field @p name into @p out (the stored TLV bytes, verbatim);
     *         @p out is written only on @ref app_read_t::OK. */
    [[nodiscard]] app_read_t app_field_get(std::string_view name, std::vector<std::byte>& out) {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        const std::ptrdiff_t i = find_app_slot(e, name);
        if (i < 0) return app_read_t::UNDECLARED;
        const app_field_table_t& t = e->app->table;
        const std::size_t idx = static_cast<std::size_t>(i);
        if (t.slots[idx].access == app_access_t::WO) return app_read_t::WRITE_ONLY;
        if (t.values == nullptr || (*t.values)[idx].empty()) return app_read_t::UNSET;
        out = (*t.values)[idx];
        return app_read_t::OK;
    }

    /** @brief A consistent copy of the whole descriptor table, in install order — the
     *         container-read / `:schema` snapshot (control-plane cold; empty ⇒ no table
     *         installed). */
    [[nodiscard]] std::vector<app_field_t> app_fields_snapshot() {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        if (e == nullptr || e->app == nullptr) return {};
        const app_field_table_t& t = e->app->table;
        std::vector<app_field_t> out;
        out.reserve(t.slots.size());
        // Materialise an OWNING copy under the lock (ADR-0058): the resident table is
        // view-slots, but the emit/`:schema` path uses the snapshot AFTER releasing the
        // lock, so it must own its bytes. Cold control-plane copy, freed immediately —
        // the RAM win is in the resident storage, not this transient.
        for (std::size_t i = 0; i < t.slots.size(); ++i) {
            app_field_t f;
            f.name.assign(t.slots[i].name);
            f.access = t.slots[i].access;
            f.descriptor.assign(t.slots[i].descriptor.begin(), t.slots[i].descriptor.end());
            if (t.values != nullptr && i < t.values->size()) f.value = (*t.values)[i];
            out.push_back(std::move(f));
        }
        return out;
    }

    // -- owner-side storage declarations & propagation policy ----------------------------

    /**
     * @brief Set the STREAM ring depth (RFC-0022 §3.C) — owner-side, never over the wire.
     *
     * Allocates the extension block if this vertex has none (a STREAM vertex always has
     * one already). Taken under the vertex mutex, which is the same lock the ring append
     * re-reads it under, so a depth change and a concurrent store cannot interleave
     * halfway.
     * @param keep Entries to retain; 0 is normalised to 1 by the ring trim.
     */
    void set_history_depth(std::uint32_t keep) {
        vertex_ext_t& e = ensure_ext();
        const std::lock_guard lock(vertex_stripe_of(this).m);
        e.history_keep_last = keep;
    }

    /** @brief The STREAM ring depth this vertex retains (1 when never declared). */
    [[nodiscard]] std::uint32_t history_depth() const noexcept {
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        return e != nullptr ? e->history_keep_last : 1;
    }

    /**
     * @brief Bind this RECEIVING vertex's own ring source and §4.4 pressure arm — the seam of
     *        RFC-0025 §4.6.1 clause 3, owner-side and with no wire surface.
     *
     * Sited on `%vertex_ext_t`'s lazily-allocated ring block, never on `%vertex_t` itself: a
     * STREAM identity already allocates the extension block, and the whole of the ring's state
     * hangs off the one lazy pointer that block already held — so `sizeof(%vertex_t)` does not
     * move, `sizeof(%vertex_ext_t)` does not move either, and a vertex that never receives
     * pays nothing. The #1285 ratchet and the RAM census are both untouched.
     *
     * REBINDING DRAINS. Reservations are released to the source that served them (sized
     * reclaim), so a rebind first hands every queued entry's block back to the OLD source and
     * empties the ring; the queue restarts on the new budget. Wiring-time by intent — the
     * "configure before frames flow" contract `set_history_depth` and `set_delivery_mode`
     * already carry.
     *
     * @param src      The source this receiver's admissions are charged against. `nullptr`
     *                 unbinds, so the next admission re-resolves the graph-level default.
     * @param reliable The §4.4 arm: false (default) best-effort — shed oldest, account the
     *                 loss, raise a gap; true reliable — refuse the admission and answer the
     *                 local producer `BACKPRESSURE`, shedding nothing.
     */
    void set_ring_source(tr::mem::block_source_t* src, bool reliable) {
        vertex_ext_t& e = ensure_ext();
        const std::lock_guard lock(vertex_stripe_of(this).m);
        // Nothing declared and no ring yet ⇒ nothing to record: do not allocate the ring block
        // for a call that restores the default. The whole point of hanging this state off the
        // lazy pointer is that a vertex which never receives never pays for it.
        if (e.ring == nullptr) {
            if (src == nullptr && !reliable) return;
            e.ring = std::make_unique<ring_state_t>();
        }
        e.ring->release_all();  // reservations go back to the source that served them
        e.appended_since_flush = 0;
        e.ring->source = src;
        e.ring->reliable = reliable;
    }

    /** @brief This receiver's bound ring source, or `nullptr` while it still draws the
     *         graph-level default (nothing admitted and nothing declared). */
    [[nodiscard]] tr::mem::block_source_t* ring_source() const noexcept {
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        return e != nullptr && e->ring ? e->ring->source : nullptr;
    }

    /**
     * @brief Set the RFC-0022 §3.D pin amplification ratio `K` (ADR-0042 §3) — owner-side,
     *        never over the wire. 0 (@ref tr::graph::kPinNever) never pins.
     *
     * Published under the vertex mutex; the write-path reader (@ref pin_payload_ratio)
     * takes no lock, because a ratio changing under a concurrent write only decides
     * WHICH correct store shape that write takes.
     */
    void set_pin_payload_ratio(std::uint32_t k) {
        vertex_ext_t& e = ensure_ext();
        const std::lock_guard lock(vertex_stripe_of(this).m);
        e.pin_payload_ratio = k;
    }

    /**
     * @brief How this vertex participates in an ANCESTOR's propagate sweep (RFC-0008 §C).
     *
     * Relaxed, and deliberately racy against a concurrent `set_delivery_mode`: the assign
     * path reads it lock-free as a FAST PATH only (`graph_t::mark_pending`), and whichever
     * of the two values it observes there, the decision that actually places the vertex in
     * a sweep set is re-taken under the graph's sweep lock. ATOMIC because
     * `set_delivery_mode` may run concurrently on another thread (#895) while this read holds
     * NO lock — which is the whole reason it needs to be atomic, and what distinguishes it
     * from the other plain members of the same byte group: `registered_` is map-lock state on
     * both sides (see `mark_unregistered`), so a plain `bool` is correct there.
     */
    [[nodiscard]] delivery_mode_t delivery_mode() const noexcept {
        return delivery_mode_.load(std::memory_order_relaxed);
    }
    /** @brief Set the propagation policy — wiring-time, via `graph_t::set_delivery_mode`
     *         (which also maintains the sweep's UNCONDITIONAL membership, and holds its
     *         sweep lock across this store so the two stay one decision). */
    void set_delivery_mode(delivery_mode_t mode) noexcept {
        delivery_mode_.store(mode, std::memory_order_relaxed);
    }

    // -- RFC-0005 listener bookkeeping (lock-free counters) ------------------------------

    /** @brief True iff this vertex has its OWN parsed ACEs (#361 §3) — the lock-free
     *         predicate of the graph's nearest-bearing-ancestor walk. Relaxed read: a
     *         racing `:acl` write is observed by the next gated op at worst, the same
     *         window the dirty-flag protocol already tolerates. */
    [[nodiscard]] bool has_own_aces() const noexcept {
        return test_flag(flag_t::OWN_ACES, std::memory_order_relaxed);
    }

    /** @brief This vertex's own active-slot count (what a subtree walk sums). */
    [[nodiscard]] std::uint32_t own_subs() const noexcept {
        return own_subs_.load(std::memory_order_relaxed);
    }
    /**
     * @brief The same count under `seq_cst` — the SUBSCRIBE half of a Dekker pair, and the
     *        only read that may be used to SKIP a DELIVERY (#635, #1140).
     *
     * A relaxed read is fine for every consumer that only decides how much work to do
     * (@ref own_subs above). It is NOT fine for one that decides whether to deliver at all:
     * a publisher that skips `snapshot_edges` on a zero count must be ordered against a
     * subscribe that is concurrently taking ADR-0049's durability latch, or the new
     * subscriber gets the latch's OLD value and never sees the publish that raced it.
     *
     * "Skip a delivery" covers both halves of the write path, EAGER and DEFERRED. #635 fixed
     * the eager one (`graph_t::fan_out`'s snapshot skip); #1140 fixed the deferred one
     * (`graph_t::mark_pending`, where a skipped mark leaves the vertex in no sweep set, so
     * the next covering `propagate` delivers it nowhere). The distinction between skipping a
     * fan-out and skipping a mark is bookkeeping — the lost delivery is the same, so the same
     * read is required. Only the OWN half; the ancestor count keeps its relaxed load, see
     * @ref listeners_above.
     *
     * The pairing is the one `%store` already documents for `waiters`. PUBLISHER: store
     * the LKV, THEN load this count. SUBSCRIBER: bump this count, THEN load the LKV into
     * the latch. Both sides `seq_cst`, so they share one total order: a publisher that
     * reads zero is ordered before the subscriber's bump, hence before the subscriber's
     * latch load — so the latch carries the value the skipped fan-out would have delivered.
     * The other interleaving (count already bumped, slot not yet appended) costs one
     * pointless lock acquisition that snapshots nothing, never a lost delivery.
     *
     * Both halves take `kDeliverySkipOrder`, which a weakly-ordered target
     * (@ref tr::graph::kWeaklyOrdered) `static_assert`s is still `seq_cst` — so the argument
     * above is a build failure when it stops holding, not only a paragraph (#1143).
     */
    [[nodiscard]] std::uint32_t own_subs_ordered() const noexcept {
        return own_subs_.load(kDeliverySkipOrder);
    }
    /**
     * @brief Adjust the own active-slot count by @p delta (subscribe/unsubscribe).
     * @note `seq_cst`, not relaxed: this is the subscriber's half of the pair
     *       @ref own_subs_ordered describes. Subscribe is control-plane-cold, so the
     *       stronger order costs nothing that is measured.
     */
    void bump_own_subs(std::int32_t delta) noexcept {
        own_subs_.fetch_add(static_cast<std::uint32_t>(delta), kDeliverySkipOrder);
    }
    /** @brief The active subscriber slots on strict ancestors — the one relaxed load the
     *         write hot path pays before deciding whether to walk ancestors at all.
     *  @note  Relaxed BY RULING even where it gates a SKIP, so there is no `_ordered` twin
     *         (#854, measured and REFUTED): a stale zero here is indistinguishable from the
     *         write linearizing before the racing subtree subscribe, because ADR-0049's latch
     *         snapshots the subscribed ANCESTOR's own LKV (@ref add_edge) and never a
     *         descendant's — so unlike @ref own_subs_ordered's near-axis pair there is no
     *         forbidden observation to exclude, and the `seq_cst` candidate doubled the idle
     *         write's rv32 fence count to exclude nothing. */
    [[nodiscard]] std::uint32_t listeners_above() const noexcept {
        return listeners_above_.load(std::memory_order_relaxed);
    }
    /** @brief Adjust the ancestor-listener count by @p delta (an ancestor's edge came/went). */
    void bump_listeners_above(std::int32_t delta) noexcept {
        listeners_above_.fetch_add(static_cast<std::uint32_t>(delta), std::memory_order_relaxed);
    }
    /** @brief Seed the ancestor-listener count at creation (the newborn's O(depth) sum). */
    void init_listeners_above(std::uint32_t count) noexcept {
        listeners_above_.store(count, std::memory_order_relaxed);
    }

   private:
    /** @brief Bits packed into `flags_` — see its declaration for why they share a byte. */
    enum class flag_t : std::uint8_t {
        OWN_ACES = 1U << 0,         /**< @brief `ext_` holds a non-empty own-ACE list (#361 §3). */
        REGISTERED_CHILD = 1U << 1, /**< @brief At least one DIRECT child is registered (#652). */
        ENUM_HIDDEN = 1U << 2,      /**< @brief Registered, addressable, but NOT a `:children[]`
                                     *          member (RFC-0014 §3, S4). */
        PAYLOAD_RIGHTS = 1U << 3,   /**< @brief This vertex DECLARED a payload-type →
                                     *          required-ACL-right table (RFC-0014 Am. 2). The
                                     *          rows live on the graph, not here; this bit is
                                     *          what keeps the write gate from looking for them
                                     *          on the vertices that declared none. */
        ADMISSION = 1U << 4,        /**< @brief This vertex installed an `on_admit` filter. The
                                     *          seam lives in the value-seam block; this bit is
                                     *          what keeps the store path from loading that
                                     *          block on the vertices that installed none. */
    };

    /** @brief Set or clear @p f. An RMW, because the bits have different writers. */
    void set_flag(flag_t f, bool on) noexcept {
        const auto bit = static_cast<std::uint8_t>(f);
        if (on) {
            flags_.fetch_or(bit, std::memory_order_release);
        } else {
            flags_.fetch_and(static_cast<std::uint8_t>(~bit), std::memory_order_release);
        }
    }

    /** @brief Read @p f under @p order. */
    [[nodiscard]] bool test_flag(flag_t f, std::memory_order order) const noexcept {
        return (flags_.load(order) & static_cast<std::uint8_t>(f)) != 0;
    }

    /**
     * @brief The `%store` LKV allocation (control block + rope), NOTHROW: `nullptr` on
     *        OOM instead of the bad_alloc that abort()s under the MCU profile's
     *        `-fno-exceptions` (#477, the engine-task storm crash class).
     *
     * Host profile (exceptions on): catch — zero cost on the hot success path, no probe
     * race. MCU profile: the `%mem_heap.hpp` probe-then-commit discipline; the probe covers
     * the rope payload + a control-header bound and targets the global heap — exact for
     * the default resource (every production graph today); an ADR-0039 injected @p mr
     * keeps its own contract, the probe being a best-effort proxy for it.
     */
    [[nodiscard]] static std::shared_ptr<const rope_t> try_make_lkv(
        rope_t&& value, std::pmr::memory_resource* mr) noexcept {
        static constexpr std::size_t kCtrlSlack = 4 * sizeof(void*);  // ≥ both mainstream ABIs
#if defined(__cpp_exceptions)
        if (!tr::detail::probe_hook_ok(sizeof(rope_t) + kCtrlSlack)) return nullptr;  // test seam
        try {
            return mr == nullptr
                       ? std::make_shared<const rope_t>(std::move(value))
                       : std::allocate_shared<const rope_t>(
                             std::pmr::polymorphic_allocator<rope_t>(mr), std::move(value));
        } catch (...) {
            // Only the allocation can throw here (the rope move is noexcept), so any
            // exception — bad_alloc or an injected resource's own type — IS the OOM leg.
            return nullptr;
        }
#else
        if (!tr::detail::probe_bytes(sizeof(rope_t) + kCtrlSlack)) return nullptr;
        return mr == nullptr ? std::make_shared<const rope_t>(std::move(value))
                             : std::allocate_shared<const rope_t>(
                                   std::pmr::polymorphic_allocator<rope_t>(mr), std::move(value));
#endif
    }

    // The dispatch view of one slot; call with m_ held. Every field is a pointer copy or a
    // refcount: the target key and the cold half are both immutable shared records, so the
    // view keeps each alive across a concurrent unsubscribe without owning any bytes
    // (ADR-0041 §2's "one copy at subscribe, rope it thereafter", now for the whole half).
    //
    // The cold branch is GONE with #1448 — a null handle assigns as cheaply as a populated
    // one, so there is nothing left to predicate. That also retires the NRVO hazard this
    // comment used to carry: the earlier two-branch double-return brace-initialized the
    // empty cold members and cost ~7 ns/edge (~+7 µs/publish at fan-out 1024, the #385
    // subscriber-cold-split regression). The single named return stays anyway — it is the
    // shape `try_copy_published`'s successor shares.
    [[nodiscard]] edge_view_t edge_view_of(const subscriber_t& s) const {
        edge_view_t e;
        e.callback = s.callback;
        e.callback_ctx = s.callback_ctx;
        e.target_key = s.target_key;
        e.binding = s.binding;
        e.remote = s.remote;
        return e;
    }

    // -- the published edge array (#635) ------------------------------------------------
    //
    // Everything below runs with the stripe lock held EXCEPT copy_published, which is the
    // pinned reader's copy loop.

    /** @brief This vertex's edge block, or null when nothing was ever subscribed. Call with
     *         the stripe lock held (the relaxed load is enough under it — only ensure_edges
     *         ever publishes, and only under the same lock). */
    [[nodiscard]] edge_block_t* edges_locked() const noexcept {
        return edges_.load(std::memory_order_relaxed);
    }

    /** @brief This vertex's edge block, allocating it on first subscribe. Call with the
     *         stripe lock held.
     *  @return Null on OOM (#477 — the caller soft-fails; the vertex is unchanged). */
    [[nodiscard]] edge_block_t* ensure_edges() noexcept {
        edge_block_t* b = edges_.load(std::memory_order_relaxed);
        if (b != nullptr) return b;
        b = new (std::nothrow) edge_block_t{};
        if (b == nullptr) return nullptr;
        edges_.store(b, std::memory_order_release);  // pairs with snapshot_edges' acquire
        return b;
    }

    /**
     * @brief Flip the published entry mirroring slot @p idx to INACTIVE. Call with the stripe
     *        lock held.
     *
     * Allocation-free and therefore infallible, which is the point: an unsubscribe must stop
     * a delivery even when the compacting republish behind it cannot allocate. The published
     * array mirrors the slot table one-for-one (no compaction), so the index maps straight
     * through — the same identity RFC-0009 §D.2 already guarantees for `:subscribers[N]`.
     */
    static void deactivate_published(edge_block_t& b, std::size_t idx) noexcept {
        edge_pub_t* p = b.pub.load(std::memory_order_relaxed);
        if (p == nullptr || idx >= p->count) return;
        p->entries()[idx].active.store(false, std::memory_order_release);
    }

    /**
     * @brief Rebuild and PUBLISH this block's edge array from its slot table, retiring the
     *        displaced one. Call with the stripe lock held; the caller runs
     *        `scan_retired_edges` afterwards, outside the lock.
     *
     * The array mirrors the slot table one-for-one so that `deactivate_published` can index
     * straight through; an inactive slot contributes an EMPTY entry, so a cleared edge's
     * refcount clones are released here rather than lingering behind a flipped bit.
     *
     * **ONE allocation total** (#1442): the array itself. The rebuild is O(slots) and always
     * will be — #635 bought lock-free fan-out with an immutable published array, so appending
     * the k-th subscriber rebuilds k+1 entries and retires k, and that is arithmetic rather
     * than a defect. What used to ride on top of it was not: a REMOTE entry's cold half was
     * DEEP-COPIED here (`new (std::nothrow) pub_remote_t` plus `try_assign` of the link and
     * caller) per pre-existing entry, per admission, and `%scan_retired_edges` freed every one
     * of them again. The cold half is immutable after admission, so each of those copies
     * reproduced something byte-identical to what it was retiring — ~940 instructions per
     * pre-existing edge against a ~158 inherent floor at 65 links. Per entry the loop now
     * copies four words and takes two refcounts (the target key and the cold half).
     * @retval false The array could not be allocated — NOTHING was published and the array on
     *         the slot is exactly as it was. There is no longer a second, per-edge OOM leg:
     *         the cold half's one allocation happens at ADMISSION, behind the door's own
     *         `BACKPRESSURE`, and a republish reaches no allocator but this one.
     */
    [[nodiscard]] bool try_publish_edges(edge_block_t& b) noexcept {
        edge_pub_t* np = nullptr;
        if (!b.slots.empty()) {
            np = alloc_edge_pub(b.slots.size());
            if (np == nullptr) return false;
            pub_edge_t* dst = np->entries();
            for (const subscriber_t& s : b.slots) {
                ::new (static_cast<void*>(dst + np->count)) pub_edge_t{};
                pub_edge_t& e = dst[np->count];
                ++np->count;  // constructed ⇒ destroy_edge_pub can always unwind it
                if (!s.active) {
                    e.active.store(false, std::memory_order_relaxed);
                    continue;
                }
                e.callback = s.callback;
                e.callback_ctx = s.callback_ctx;
                e.target_key = s.target_key;  // refcount clone — nothrow
                e.binding = s.binding;
                // The cold half is SHARED, not copied (#1442): immutable after admission, so
                // the entry names the slot's record instead of reproducing it. One relaxed
                // increment, nothrow, and no `#981` string-copy residual to carry here — the
                // two `try_assign` probe windows that used to live on this line are gone from
                // the republish entirely. #1448 then took the same two off the DELIVERY path
                // (`copy_entry`), so no holder of this record copies its bytes any more.
                e.remote = s.remote;
            }
        }
        // seq_cst, not release: this exchange and the pinned reader's validating load must
        // share ONE total order for the announce/scan protocol to hold (see pin_t::acquire).
        if (edge_pub_t* old = b.pub.exchange(np, std::memory_order_seq_cst); old != nullptr)
            retire_push(b.retired, old);
        return true;
    }

    /**
     * @brief Copy every ACTIVE entry of @p p into the caller's buffers — the pinned reader's
     *        whole critical section, and the only work an edge pin covers.
     *
     * Bounded, provably non-re-entrant and — since #1448 — reaching an allocator at exactly
     * ONE place, the wide-fan-out overflow reservation below. The per-entry copy is four
     * words and two refcounts on EVERY edge shape (`%copy_entry`): the remote edge's two
     * `std::string` copies are gone, so there is no per-edge probe left to fail and no
     * per-edge OOM leg to take.
     *
     * The one remaining shed TALLIES into @p drops at the site the delivery is actually
     * abandoned — not at the caller's frame, which cannot tell a truncated snapshot from a
     * short subscriber list (#896).
     */
    [[nodiscard]] static std::size_t copy_published(const edge_pub_t* p,
                                                    edge_snapshot_t& inline_buf,
                                                    std::vector<edge_view_t>& overflow,
                                                    snapshot_drops_t& drops) noexcept {
        if (p == nullptr) return 0;
        // #981 residual: the wide-fan-out overflow buffer keeps `try_reserve`'s
        // `-fno-exceptions` probe window — a task switch between the probe's free and the
        // `reserve` abort()s the node (#850). `edge_view_t` still holds refcounted handles,
        // so `block_array_t` (memcpy relocation) cannot hold it — the handles would be
        // relocated without their destructors and the counts would leak. The inline prefix
        // below is the mitigation that exists today: a fan-out up to `kCapacity` reaches no
        // allocator at all, and since #1448 that is true whatever the edges are.
        const bool use_heap =
            p->count > edge_snapshot_t::kCapacity && tr::detail::try_reserve(overflow, p->count);
        const pub_edge_t* src = p->entries();
        std::size_t n = 0;
        for (std::uint32_t i = 0; i < p->count; ++i) {
            if (!src[i].active.load(std::memory_order_acquire)) continue;
            // OOM fallback (reserve failed on a wide list): the inline prefix delivers,
            // the remainder of this fan-out is dropped — never an abort. Walk the tail
            // rather than breaking blind: the abandoned edges are N deliveries, and a
            // counter that said "1" for a truncated fan-out of N would be its own defect.
            if (!use_heap && n == edge_snapshot_t::kCapacity) {
                for (; i < p->count; ++i)
                    if (src[i].active.load(std::memory_order_acquire)) ++drops.truncated;
                break;
            }
            edge_view_t e;
            copy_entry(src[i], e);
            if (use_heap)
                overflow.push_back(std::move(e));  // reserved above — no reallocation
            else
                inline_buf.push_back(std::move(e));
            ++n;
        }
        return n;
    }

    /**
     * @brief Copy one published entry into a dispatch view — the DELIVERY hot path's whole
     *        per-edge cost, and it cannot fail (#1448).
     *
     * Two pointer copies and two refcounts, with **no branch on the edge shape**: the cold
     * half is a share, so a remote edge costs a relaxed increment where it used to cost two
     * probe-guarded `std::string` copies plus two `view_t` clones, per edge, PER DELIVERY.
     * The predecessor was `try_copy_published`, and its `false` leg — the one thing on this
     * path that could allocate — is what disappeared; `copy_published` therefore has no
     * per-edge OOM tally left to keep.
     *
     * Named `copy_entry` rather than anything prefixed `copy_published`: `bench/symbol_ratchet.py`
     * matches its pins by demangled PREFIX, and a sibling called `copy_published_entry` would
     * be silently summed into the `vertex_t::copy_published` pin.
     */
    static void copy_entry(const pub_edge_t& in, edge_view_t& out) noexcept {
        out.callback = in.callback;
        out.callback_ctx = in.callback_ctx;
        out.target_key = in.target_key;  // refcount clone — nothrow
        out.binding = in.binding;        // two words, trivially copyable
        out.remote = in.remote;          // refcount clone of the shared cold half — nothrow
    }

    /** @brief The slot index of the descriptor-table entry named @p name, or `-1` (no
     *         entry / no extension block). Call with the stripe lock held. Linear: an
     *         owner's table is small (RFC-0010 targets MCU vertices), field ops are
     *         control-plane. */
    [[nodiscard]] static std::ptrdiff_t find_app_slot(vertex_ext_t* e, std::string_view name) {
        if (e == nullptr || e->app == nullptr) return -1;
        const std::span<const app_field_slot_t> slots = e->app->table.slots;
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (slots[i].name == name) return static_cast<std::ptrdiff_t>(i);
        return -1;
    }

    /** @brief Install @p built as this vertex's descriptor table (ADR-0058 Step 2). Call
     *         with the stripe lock held. Allocates the lazy app-field group iff needed —
     *         an empty table on a vertex with no group is a no-op (nothing to uninstall),
     *         so a group is never created just to hold an empty table; an existing group's
     *         `on_app_field_write` apply seam is preserved across a table replacement. */
    void install_app_table(vertex_ext_t& e, app_field_table_t built) {
        if (built.slots.empty() && e.app == nullptr) return;
        if (e.app == nullptr) e.app = std::make_unique<app_field_group_t>();
        e.app->table = std::move(built);
    }

    /** @brief Pack an owning @p table into one @ref app_field_table_t (ADR-0058): the
     *         name+descriptor bytes are concatenated into a single `backing` buffer (one
     *         allocation for the whole table) with the slots viewing into it; any initial
     *         values are moved into the lazy value store. `backing`'s address is stable
     *         across the table's moves, so the slot views stay valid. */
    [[nodiscard]] static app_field_table_t build_owning_table(std::vector<app_field_t> table) {
        app_field_table_t t;
        if (table.empty()) return t;
        std::size_t total = 0;
        bool any_value = false;
        for (const app_field_t& f : table) {
            total += f.name.size() + f.descriptor.size();
            any_value = any_value || !f.value.empty();
        }
        t.backing.resize(total);
        t.owned_slots = std::make_unique<app_field_slot_t[]>(table.size());
        std::size_t si = 0;
        std::size_t off = 0;
        for (const app_field_t& f : table) {
            const std::size_t noff = off;
            std::copy(f.name.begin(), f.name.end(),
                      reinterpret_cast<char*>(t.backing.data()) + noff);
            off += f.name.size();
            const std::size_t doff = off;
            std::copy(f.descriptor.begin(), f.descriptor.end(), t.backing.data() + doff);
            off += f.descriptor.size();
            t.owned_slots[si++] = app_field_slot_t{
                std::string_view(reinterpret_cast<const char*>(t.backing.data()) + noff,
                                 f.name.size()),
                f.access, std::span<const std::byte>(t.backing.data() + doff, f.descriptor.size())};
        }
        t.slots = std::span<const app_field_slot_t>(t.owned_slots.get(), table.size());
        if (any_value) {
            t.values = std::make_unique<std::vector<std::vector<std::byte>>>(table.size());
            for (std::size_t i = 0; i < table.size(); ++i)
                (*t.values)[i] = std::move(table[i].value);
        }
        return t;
    }

    /**
     * @brief The extension block, creating it on first need (race-free CAS publish).
     *
     * Callable under any lock regime: allocation races between the registration path
     * (graph map lock) and the field-write verbs (vertex mutex) resolve by
     * compare-exchange — the loser frees its candidate and adopts the winner's block.
     * The pointer is never cleared once published (ADR-0057 insert-only lifetime), so
     * lock-free readers (@ref pin_payload_ratio / @ref handlers) stay valid forever.
     */
    vertex_ext_t& ensure_ext() {
        vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        if (e != nullptr) return *e;
        auto fresh = std::make_unique<vertex_ext_t>();
        vertex_ext_t* expected = nullptr;
        if (ext_.compare_exchange_strong(expected, fresh.get(), std::memory_order_acq_rel,
                                         std::memory_order_acquire))
            return *fresh.release();
        return *expected;  // another thread won the publish; fresh is freed here
    }

    /**
     * @brief Install a registration's identity (constructor + `fill`): allocate the
     *        extension block iff this identity needs one — STREAM role (history ring) or
     *        any user handler — and store the cold members there. A plain leaf allocates
     *        nothing (#361 §1).
     *
     * RFC-0022 §3.B dropped the third condition, "a non-default storage policy", along with
     * the parameter that carried it, so STRICTLY MORE vertices stay extension-less than
     * before: registration can no longer force the cold block onto a vertex, and the two
     * owner-side magnitudes materialise it only if an owner actually declares one.
     */
    void adopt_identity(role_t role, handlers_t handlers) {
        const bool has_handlers = handlers.on_read || handlers.on_write || handlers.on_children ||
                                  handlers.on_app_field_write;
        if (role != role_t::STREAM && !has_handlers &&
            ext_.load(std::memory_order_acquire) == nullptr)
            return;
        vertex_ext_t& e = ensure_ext();
        // Split the public input into its two lazy groups (ADR-0058 Step 2): the value
        // seam only when one of its three is set; the app-field group's apply seam only
        // when given. Registration is single-threaded for this vertex, so no lock here.
        if (handlers.on_read || handlers.on_write || handlers.on_children) {
            // Publish the seam atomically. `fill` only ever runs on an UNREGISTERED node
            // (register_vertex_key returns PATH_IN_USE otherwise), and such a node's seam
            // is null — a fresh placeholder never had one, and retirement already swapped a
            // retired node's out. So the prior is provably null and a plain release store
            // suffices; the store races only the lock-free reader, which the release
            // ordering covers.
            e.handlers.store(
                new value_handlers_t{std::move(handlers.on_read), std::move(handlers.on_write),
                                     std::move(handlers.on_children)},
                std::memory_order_release);
        }
        if (handlers.on_app_field_write) {
            if (e.app == nullptr) e.app = std::make_unique<app_field_group_t>();
            e.app->on_app_field_write = std::move(handlers.on_app_field_write);
        }
    }

    // Members are laid out in descending-alignment groups (#361 diet: zero interior
    // padding — 8-byte, then 4-byte, then flag bytes), with everything the write hot
    // path touches (LKV slot, subs, ext, seq, counters, mode flags) in the first ~96
    // bytes and the wide Composite child storage at the tail.
    //
    // WITHIN the leading 8-byte group the LKV slot comes FIRST, and that is load-bearing
    // rather than cosmetic (#1285). The slot is 16 bytes wide with an alignment of only 8,
    // so at an offset of 24 — where it sat while `name_` led the group — its two words
    // straddle a 64-byte cache line for exactly one of the four block alignments glibc can
    // hand out (`address % 64 == 32`). That placement doubles the coherence footprint of
    // every publish and was measured at x0.34 throughput with 1.9x the cache misses on the
    // 8-thread single-vertex write arm. Starting the slot at a 16-byte-aligned offset makes
    // the straddle unreachable for ANY 16-byte-aligned block, and offset 0 is the one such
    // offset that needs no padding to reach: the reorder is free, `sizeof(vertex_t)` is
    // unchanged on both ABIs, and @ref vertex_layout_gate_t pins it. `alignas(16)` on the
    // member would NOT be free — it leaves an 8-byte hole and spends the #361 ratchet.

    // The stored value is a rope (ADR-0053 §6): a contiguous scalar is a single-link
    // rope (small-buffer inline, no extra alloc), a chunked stream keeps its links.
    /** @brief The last-known value, held through the slot policy this target bound
     *         (`tr::graph::lkv_slot_t` in `%config.hpp`; ADR-0069 §1). The default binding is
     *         `sp_atomic_slot_t` — today's `std::atomic<std::shared_ptr<const rope_t>>`,
     *         which is lock-free by CONTRACT and spin-locked in practice. `%lkv_slot.hpp`
     *         documents that caveat and the contract any replacement must satisfy. Do not
     *         read "lock-free" here as "no serializing operation". */
    lkv_slot_t lkv_{};

    path_key_t name_;  // own canonical NAME record (one segment; empty at the root) — the
                       // full key is rendered on demand by walking parent_ (ADR-0057);
                       // immutable once the node is linked (lock-free parent walks)

    // The fan-out edges (#635). Null until this vertex is first subscribed to, which is the
    // overwhelming majority of an MCU node's vertices — where an always-present empty
    // std::vector cost 24 B (12 on rv32) of pure header. Allocated once by ensure_edges under
    // the stripe lock, published with a release store, freed by ~vertex_t and NEVER displaced,
    // which is why the publish path may load it with a plain acquire and no pin.
    std::atomic<edge_block_t*> edges_{nullptr};
    // The lazily-allocated cold half (#361 §1): handlers, STREAM ring, the ACL state +
    // ADR-0050 effective-merge cache, the owner-side storage magnitudes, and the stream
    // drain cursor.
    // Null for the common default leaf. Published once by ensure_ext (CAS), never
    // cleared; freed by the destructor.
    std::atomic<vertex_ext_t*> ext_{nullptr};
    std::atomic<std::uint64_t> write_seq_{0};  // bumped per assign; await waits for an increment,
                                               // and it is the value-agnostic "newer" signal a
                                               // sweep reads (RFC-0008 §B). Guarded by m_.

    // Subtree-subscription bookkeeping (RFC-0005): every subscription observes its
    // vertex AND all descendants, so a write must fan out to ancestor subscribers
    // too ("vertical bubbling"). These lock-free counters keep the idle write path
    // near-free: `listeners_above_` counts ACTIVE subscriber slots on strict
    // ancestors (maintained by graph_t at subscribe/unsubscribe — a subtree walk at
    // control-plane frequency — and summed from ancestors at vertex creation), so
    // the write hot path pays exactly one relaxed load before deciding whether to
    // walk ancestors at all. `own_subs_` is this vertex's own active-slot count —
    // what the subtree walk and the creation-time sum read.
    std::atomic<std::uint32_t> own_subs_{0};
    std::atomic<std::uint32_t> listeners_above_{0};

    // -- flag bytes (one 4-byte group; all byte-wide by design) ------------------------
    // The behavioral role (byte-wide enum).
    // ATOMIC (#1477), for the same reason `delivery_mode_` below is (#895) and with the same
    // shape: the write path forks on `role()` with NO map lock held, while `fill` and
    // `revert_to_placeholder` store to it under the graph's unique map lock. A plain byte
    // there was a data race — UB, not a benign stale read — and it was the LAST plain member
    // of this four-byte group that a lock-free reader touches. Byte-wide as an atomic too,
    // so the group stays four bytes and `sizeof(vertex_t)` does not move.
    // Relaxed on both ends: making the race defined is the whole of the fix. Ordering a
    // write against a concurrent retire is the CALLING PLANE's job (the doctrine note at
    // `graph_t::write`), which is why no map lock appears on the hot path here.
    std::atomic<role_t> role_;
    // How this vertex participates in an ANCESTOR's propagate sweep (RFC-0008 §C).
    // Set at wiring time via graph_t::set_delivery_mode (the "configure before frames
    // flow" contract, like the storage policy); read on the assign path. Default IF_NEWER.
    // ATOMIC (#895): the assign path reads it with no lock held while set_delivery_mode
    // writes it under the graph's sweep lock, so a plain byte here was a data race — UB,
    // not a benign torn read. Byte-wide as an atomic too, so the group stays four bytes.
    std::atomic<delivery_mode_t> delivery_mode_{delivery_mode_t::IF_NEWER};
    // Three lock-free predicates, packed into ONE byte so the flag group stays exactly four
    // bytes wide and `sizeof(vertex_t)` stays at the size the ratchets pin (96 B on x86-64,
    // 72 B on rv32 — the #361 diet's measurement as re-taken by the #1487 census) — the size
    // gate's own failure message says to put a new member behind vertex_ext_t rather than
    // inline it, and a bit costs less than either. (`ENUM_HIDDEN`, the RFC-0014 §3 hide seam,
    // is the third: it went here rather than beside `registered_` for exactly that reason.)
    // Written under a lock (a different one per bit), read lock-free off hot paths, so the
    // writes are RMWs and compose.
    std::atomic<std::uint8_t> flags_{0};
    bool registered_ = false;  // false => placeholder intermediate (invisible to find)
    /**
     * @brief Bumped every time this vertex is re-virginized by retirement (ADR-0062).
     *
     * A `vertex_handle_t` never dangles — the vertex map is pinned and insert-only — but
     * `retire()` RE-VIRGINIZES the object in place, so a handle cached across a retire+revive
     * would address the path's NEW occupant while believing it holds the old one. That is a
     * confused deputy across an ownership boundary, not a stale read. A holder that caches a
     * resolution stamps this counter alongside it and compares before use; a mismatch is
     * handled exactly as a stale route-handle label (drop, observe, NACK, re-advertise), so
     * no second invalidation mechanism exists.
     *
     * Read lock-free off the delivery path while `revert_to_placeholder` writes it, hence
     * atomic. Placed here rather than in `%vertex_ext_t` deliberately: the ext block is
     * LAZILY allocated, so a generation living there would be absent for exactly the plain
     * leaves that retire most often. 32-bit wrap needs 2^32 retirements of one vertex.
     */
    std::atomic<std::uint32_t> retire_gen_{0};

    // Composite tree links (ADR-0057) at the cold tail. parent_ is immutable once the
    // node is linked (lock-free parent walks); children/registered_ are guarded by
    // graph_t's map lock. Children are owned via non-moving unique_ptr allocations —
    // vertex_t* stay stable for the graph's lifetime (the insert-only invariant
    // vertex_handle_t relies on) — in ONE sorted heap list (O(log children)
    // resolution), whose block is lazily allocated on the first child so a LEAF pays
    // exactly one null pointer (#380 §1). Vertices are never erased (retire-LIST
    // deferred; see ADR-0057 lifetime).
    vertex_t* parent_ = nullptr;

    /** @brief The lazily-allocated child list (null for every leaf): the owned children,
     *         sorted by their canonical NAME record bytes. */
    struct children_t {
        std::vector<std::unique_ptr<vertex_t>> sorted; /**< @brief Sorted owned children. */
    };
    std::unique_ptr<children_t> children_;

    /** @brief First child in sorted name-record order, or null for a leaf (@ref
     * for_each_descendant). */
    [[nodiscard]] vertex_t* first_child() const noexcept {
        if (!children_ || children_->sorted.empty()) return nullptr;
        return children_->sorted.front().get();
    }

    /**
     * @brief The child that follows @p c among THIS vertex's children, or null if @p c is last.
     *
     * Finds @p c by binary search on its own NAME record rather than by a stored index, which is
     * what lets @ref for_each_descendant ascend with no auxiliary storage. The list is kept
     * sorted by that record (`add_child`), so this is the same `lower_bound` @ref
     * child_by_record performs — O(log children).
     *
     * Identity is by ADDRESS, not by name: `lower_bound` lands on the first record that is not
     * less than @p c's, and a vertex's own entry is necessarily at or after that point. Comparing
     * pointers rather than trusting the first hit keeps this correct even if two children ever
     * shared a record, where a name compare would silently return the wrong sibling.
     */
    [[nodiscard]] vertex_t* next_sibling_of(const vertex_t& c) const noexcept {
        if (!children_) return nullptr;
        const std::vector<std::unique_ptr<vertex_t>>& sorted = children_->sorted;
        auto it =
            std::lower_bound(sorted.begin(), sorted.end(), c.name().bytes(),
                             [](const std::unique_ptr<vertex_t>& e, std::span<const std::byte> n) {
                                 return std::ranges::lexicographical_compare(e->name().bytes(), n);
                             });
        while (it != sorted.end() && it->get() != &c) ++it;
        if (it == sorted.end()) return nullptr;  // not our child — caller error, walk stops
        ++it;
        return it == sorted.end() ? nullptr : it->get();
    }
};

/**
 * @brief The cache-line straddle gate (#1285), enforced beside the type it constrains.
 *
 * `lkv_` is the contended word of the write hot path: with the default `sp_atomic_slot_t`
 * binding this libstdc++ keeps the spin lock as the LSB of the slot's second word, so N
 * concurrent writers do `lock cmpxchg` on one address inside the slot. The slot is 16 bytes
 * wide but only 8-byte aligned, so an offset that is 8-aligned-but-not-16 lets its two words
 * land on DIFFERENT 64-byte cache lines for one of the four block alignments glibc can return
 * — doubling the coherence footprint of every publish (measured x0.34 throughput, 1.9x cache
 * misses, at `address % 64 == 32` with the slot at offset 24).
 *
 * Pinning the offset to a multiple of 16 makes that placement unreachable: any 16-byte-aligned
 * block puts a 16-aligned interior offset back on a 16-byte boundary, and 16 bytes starting on
 * a 16-byte boundary cannot cross a 64-byte one. This is a LAYOUT invariant, not an allocation
 * one — 64-byte-aligning the vertex itself is a separate, RAM-costing decision that belongs to
 * #873 / ADR-0079's placement store. The gate lives in the header for the same reason the size
 * ratchets below do: every translation unit on every target evaluates it under its own binding,
 * so a future member reorder cannot silently reintroduce the straddle.
 *
 * `offsetof` on a non-standard-layout type is conditionally supported; GCC and Clang both
 * accept it and warn under `-Winvalid-offsetof`, which is suppressed narrowly here.
 */
struct vertex_layout_gate_t {
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
    static_assert(offsetof(vertex_t, lkv_) % 16 == 0,
                  "vertex_t::lkv_ must start at a 16-byte-aligned offset (#1285) — otherwise "
                  "the 16-byte, 8-aligned atomic slot straddles a 64-byte cache line for one "
                  "malloc placement in four and the contended write path loses ~3x. Reorder "
                  "the members to restore it; do NOT pad or alignas, that spends the #361 "
                  "RAM ratchet asserted below");
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
};

/**
 * @brief The RAM-diet gate (#361 §8), enforced beside the type it constrains.
 *
 * This is the whole of the diet's enforcement, and it is deliberately HERE rather than in a
 * test. A `static_assert` in the header is evaluated by every translation unit that includes it
 * — so every target, every configuration, every consumer's build checks its OWN binding, for
 * free, with no CI job to remember to add. It previously lived in `vertex_size_test.cpp`, where
 * it gated exactly one configuration: the 32-bit arm was never evaluated at all, because no CI
 * leg cross-compiled that test, while the ESP-IDF legs compiled `vertex_t` itself on every PR.
 *
 * The bounds are members of @ref config_t, so a target that must carry a bigger vertex says so
 * in its configuration, where the change is visible in a diff, rather than by editing a test
 * until it passes.
 *
 * They are RATCHETS, not ceilings: each is pinned to the size actually measured, so the gate
 * catches the next added byte instead of the next 24. A ceiling held above the measurement
 * answers only "did you regress past a fixed point" and is silent on whether the type got
 * leaner — which let 16 B reclaimed on the 64-bit arm, and 8 B on the 32-bit one, sit
 * unnoticed and re-spendable. Pinning keeps every reclaimed byte by construction, at the cost
 * of one number to lower in whichever commit shrinks the struct.
 */
static_assert(sizeof(void*) != 8 || sizeof(vertex_t) <= config_t::kMaxVertexBytes64,
              "vertex_t grew past the 64-bit RAM-diet ratchet (#361) — move the new member "
              "behind vertex_ext_t, don't inline it. The ratchet is PINNED to the measured "
              "size, so it has no headroom by construction: any added member fails here by "
              "design, and shrinking vertex_t means lowering the number in the same commit");
static_assert(sizeof(void*) != 4 || sizeof(vertex_t) <= config_t::kMaxVertexBytes32,
              "vertex_t grew past the 32-bit RAM-diet ratchet (#361) — move the new member "
              "behind vertex_ext_t, don't inline it. Same pinned-to-measurement rule as the "
              "64-bit arm, and this is the target where the bytes actually matter");

}  // namespace tr::graph
