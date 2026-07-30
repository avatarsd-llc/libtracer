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
 */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
#include <utility>
#include <vector>

#include "libtracer/config.hpp"
#include "libtracer/lkv_slot.hpp"
#include "libtracer/path.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/status.hpp"
#include "libtracer/view.hpp"

namespace tr::graph {

// L1 types this layer consumes (upward dependency on tr::view, docs/adr/0016 §2).
using view::rope_t;
using view::segment_ptr_t;
using view::view_t;

/** @brief A vertex's behavioral role (docs/reference/11 §roles). Byte-wide: it packs
 *         into `vertex_t`'s flag byte group (#361 diet — 3 values need no int). */
enum class role_t : std::uint8_t {
    STORED_VALUE, /**< @brief Role 1: last-writer-wins; holds the last-written value. */
    STREAM,       /**< @brief Role 2: bounded history ring sized by `settings.history_keep_last`. */
    HANDLER,      /**< @brief Roles 3-7: user `on_read` / `on_write` supplies the behavior. */
};

/**
 * @brief An owning reference to a vertex's PUBLISHED value — what @ref graph_t::read and
 *        @ref graph_t::await hand back.
 *
 * The value a vertex publishes is already refcounted: the LKV slot holds it as a
 * `std::shared_ptr<const rope_t>`, and the policy contract in `lkv_slot.hpp` fixes that shape
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
 * @brief The mandatory core QoS fields of a vertex (docs/reference/02 §core writable fields).
 *
 * **Members are ordered widest-first, and that ordering is load-bearing.** Every field is a
 * fixed-width scalar, so this struct is the same size on a 32-bit MCU as on a 64-bit host —
 * it never shrinks with the pointer, which makes it the largest non-ACL member of
 * @ref vertex_ext_t on device (32 of 112 B on rv32 before this ordering). Declaration order
 * `u64, u32 x3, u8 x3` packs the 23 bytes of members into **24**; interleaving the `u8`s
 * between the wider fields costs 8 bytes of pure padding on every ext-bearing vertex, on
 * every target.
 *
 * Reordering is safe here and must stay safe: the wire form emits **named** children
 * (`SETTINGS{ NAME "reliability", VALUE u8 }` — reference/05), so it is independent of
 * declaration order, and every construction site value-initializes (`settings_t{}`). Do not
 * introduce positional aggregate initialization — it would silently bind values to the wrong
 * fields the next time this order changes.
 */
struct settings_t {
    std::uint64_t deadline_ns = 0;       /**< @brief 0=off; max ns between writes before a
                                              liveness fault. */
    std::uint32_t history_keep_last = 1; /**< @brief Stream ring depth (>=1). */
    std::uint32_t queue_max_bytes = 0; /**< @brief 0=unbounded; per-subscriber back-pressure cap. */
    /**
     * @brief Store-by-reference threshold (ADR-0042 §3): a view-delivered WRITE whose
     *        payload TLV is >= this many bytes (and carries no trailer bits) is stored as a
     *        SUBVIEW of the inbound frame (refcount pin, zero copy) instead of the one-copy
     *        trailer-sliced store. 0 (the default) DISABLES referencing — pinning
     *        amplification is a per-vertex deployment call.
     */
    std::uint32_t store_ref_min_bytes = 0;
    std::uint8_t reliability = 0; /**< @brief 0=best-effort, 1=reliable. */
    std::uint8_t durability = 0;  /**< @brief 0=volatile, 1=transient-local. */
    std::uint8_t priority = 0;    /**< @brief 0=low .. 255=critical (transport hint, not a
                                       wire bit). */

    /** @brief Memberwise equality — lets registration detect all-defaults settings (which
     *         need no @ref vertex_ext_t allocation, ADR-0021 pay-for-what-you-use). */
    bool operator==(const settings_t&) const = default;
};

/** @brief The all-defaults @ref settings_t a vertex without a @ref vertex_ext_t reports —
 *         one shared constant, never a per-vertex copy. */
inline constexpr settings_t kDefaultSettings{};

/**
 * @brief Owner-declared REMOTE writability of one application property field (RFC-0010
 *        §A.2): what a caller-attributed field write/read may do. The OWNER — a local,
 *        caller-less host API call — always reads and writes its own declared fields;
 *        `ro`/`wo` constrain remote callers only.
 */
enum class app_access_t : std::uint8_t {
    RO = 0, /**< @brief Remote read only — a remote write has no surface (`SCHEMA_NOT_FOUND`). */
    RW = 1, /**< @brief Remote read + write (a write still passes the vertex WRITE gate). */
    WO = 2, /**< @brief Remote write only — no read surface: a secret never mirrors back. */
};

/** @brief The stable `:schema` spelling of an `app_access_t` (`"ro"` / `"rw"` / `"wo"`). */
[[nodiscard]] constexpr std::string_view to_string(app_access_t a) noexcept {
    switch (a) {
        case app_access_t::RO:
            return "ro";
        case app_access_t::RW:
            return "rw";
        case app_access_t::WO:
            return "wo";
    }
    return "ro";
}

/**
 * @brief One entry of a vertex's field descriptor table (RFC-0010 §A.2/§B): declaration,
 *        remote-writability, self-description, and current value of ONE application field
 *        under `:settings.app.` — one record, so the schema can never drift from the gate.
 *
 * The value and descriptor bytes are OPAQUE to the runtime (stored and served verbatim,
 * the `set_acl`/`acl_bytes` store-verbatim pattern): dtype/range validation is the
 * owner's, in its apply seam (@ref handlers_t::on_app_field_write) — the runtime
 * validates only addressing (declared / undeclared, writability): one table lookup.
 */
struct app_field_t {
    /** @brief The field's key below `settings.app.` — a `.`-joined spelling of the field
     *         steps (`"kp"`, `"wifi.ssid"`); the runtime keys the joined string flat. */
    std::string name{};
    app_access_t access = app_access_t::RO; /**< @brief Owner-declared remote writability. */
    /** @brief The §B.1 descriptor record members (dtype/unit/min/max/label…, concatenated
     *         child TLVs) served inside this field's `:schema` entry VERBATIM, after the
     *         runtime-projected `access` member. Never parsed by the runtime. */
    std::vector<std::byte> descriptor{};
    /** @brief The field's current TLV bytes, stored and served verbatim (§D). Empty ⇒
     *         never written (reads `NOT_FOUND`; omitted from container reads). An install
     *         MAY carry an initial value here. */
    std::vector<std::byte> value{};
};

/**
 * @brief One app-field DECLARATION (ADR-0058, class ②): view-shaped, owning nothing.
 *
 * Unlike @ref app_field_t this owns NOTHING: `name` and `descriptor` are VIEWS. For an
 * OWNING install they point into @ref app_field_table_t::backing; for a BORROWED install
 * (@ref graph_t::set_app_fields_static) they point at the caller's own storage, and the
 * caller guarantees the pointed-to bytes — **and the array holding these entries** —
 * outlive the vertex. Pass static storage (flash / `.rodata`), never a stack array or a
 * soon-freed heap block. Either way the storage is immutable for the table's lifetime, so
 * the views stay valid. Declaration only: no initial value (write values after install via
 * the field-write surface).
 *
 * This is ONE type serving both roles. It used to be two — `app_field_static_t` for the
 * install-time shape and `app_field_slot_t` for the runtime's copy of it — which were
 * field-for-field identical, so a borrowed install spent an allocation and a copy
 * converting between them. Unifying them lets a borrowed table be viewed in place
 * (ADR-0058 erratum 1).
 */
struct app_field_slot_t {
    std::string_view name;                   /**< @brief Field key below `settings.app.` (§A.1). */
    app_access_t access = app_access_t::RO;  /**< @brief Owner-declared remote writability. */
    std::span<const std::byte> descriptor{}; /**< @brief §B.1 descriptor bytes, served verbatim. */
};

/** @brief The install-time spelling of @ref app_field_slot_t — the same type. Kept as a name
 *         because it reads better at an owner's `set_app_fields_static` call site, and because
 *         it is the spelling already in the wild (docs, integrations, firmware tables). */
using app_field_static_t = app_field_slot_t;

/**
 * @brief The argument type of a BORROWED app-field install — a table the caller promises
 *        outlives the vertex, constrained at compile time to storage shaped like it does
 *        (ADR-0058 erratum 2).
 *
 * Erratum 1 tightened the borrowed install's contract from "the `name`/`descriptor` bytes
 * must outlive the vertex" to "**the array too**". Because the parameter was a
 * `std::span`, which binds implicitly to any contiguous range, that tightening reached
 * callers as a SILENT change: the same call kept compiling and started dangling. This type
 * closes the common case of that trap. It converts implicitly from a `T[N]` or a
 * `std::array` — the two spellings a `constexpr`/`static` table takes — and NOT from a
 * `std::vector`, so the natural way to build a table dynamically (fill a vector, install
 * it, return) is now a compile error at the call site rather than a use-after-free found
 * later by a downstream test suite. Default-constructed (`{}`) is the empty table, which
 * uninstalls.
 *
 * **What it does NOT prove:** that the storage is `static`. A block-scope `T[N]` binds
 * exactly like a namespace-scope one — C++ cannot express "static storage duration" as a
 * constraint on a parameter. It rejects the container/temporary class of mistake, not
 * every lifetime mistake. A caller whose table really is runtime-sized (a binding mapping
 * a foreign POD array into slots, e.g.) opts out through @ref unchecked, whose name is the
 * point: the lifetime promise moves to the caller, in writing, at the call site.
 */
class borrowed_fields_t {
   public:
    /** @brief The empty table — installs nothing, uninstalls an existing one. */
    constexpr borrowed_fields_t() noexcept = default;

    /**
     * @brief Borrow a C array of declarations — the `static constexpr kFields[]` spelling.
     * @param table The caller's array; it and the bytes it points at MUST outlive the vertex.
     */
    template <std::size_t N>
    constexpr borrowed_fields_t(const app_field_static_t (&table)[N]) noexcept  // NOLINT
        : slots_(table, N) {}

    /**
     * @brief Borrow a `std::array` of declarations — same contract as the C-array form.
     * @param table The caller's array; it and the bytes it points at MUST outlive the vertex.
     */
    template <std::size_t N>
    constexpr borrowed_fields_t(const std::array<app_field_static_t, N>& table) noexcept  // NOLINT
        : slots_(table.data(), N) {}

    /**
     * @brief Borrow an arbitrary span, asserting the lifetime by hand — the escape hatch for
     *        a table whose extent is only known at run time.
     *
     * Use when the storage is genuinely long-lived but not array-shaped at the call site: a
     * language binding filling a `.bss` slot array from a foreign POD table, say. The
     * spelling is deliberately unpleasant — it is the caller taking the promise the implicit
     * constructors would otherwise have checked the shape of.
     *
     * @param table Slots that MUST outlive the vertex, along with the bytes they point at.
     */
    [[nodiscard]] static constexpr borrowed_fields_t unchecked(
        std::span<const app_field_static_t> table) noexcept {
        borrowed_fields_t b;
        b.slots_ = table;
        return b;
    }

    /** @brief The borrowed slots, in owner install order. */
    [[nodiscard]] constexpr std::span<const app_field_static_t> slots() const noexcept {
        return slots_;
    }

    /** @brief True when the table declares no fields — the uninstall case. */
    [[nodiscard]] constexpr bool empty() const noexcept { return slots_.empty(); }

   private:
    std::span<const app_field_static_t> slots_{};
};

/**
 * @brief A vertex's RFC-0010 field descriptor table (ADR-0058): the immutable declaration
 *        (class ②) split from the per-vertex mutable values (class ③).
 *
 * Both install overloads converge here. `set_app_fields_static` leaves `backing` empty and
 * points @ref slots straight at the caller's array — the declaration costs zero RAM, neither
 * bytes nor slots (measured host-side: 392 B / 10 allocs per leaf versus 695 B / 17 for the
 * owning install, against a 136 B bare leaf — the `vertex_app5_static` and `vertex_app5` gate
 * rows). Erratum 1 is what removed the slot copy; an earlier revision of this comment still
 * described it (592 B / 11) after the code had stopped doing it. The owning `set_app_fields`
 * packs the runtime table's name+descriptor bytes into `backing` — ONE allocation for the
 * whole table — and points the slots into it. `backing` is never mutated or reallocated
 * while `slots` reference it (a re-install replaces the whole table under the vertex mutex).
 */
struct app_field_table_t {
    /** @brief Per-field declaration views, in owner install order. Empty ⇒ no table
     *         installed (the closed `ENOTTY` default). Guarded by the vertex mutex.
     *
     *         A SPAN, not a container: a borrowed install points it straight at the caller's
     *         array and allocates nothing for the declaration, which is what ADR-0058 §Step 1.2
     *         promised and did not deliver (it copied into a `std::vector` — see erratum 1). An
     *         owning install points it at @ref owned_slots. Stable across the table's moves for
     *         the same reason `backing` is: a moved `unique_ptr` keeps its heap address, and a
     *         borrowed span points outside the table entirely. */
    std::span<const app_field_slot_t> slots{};
    /** @brief The owning install's slot array; null for a borrowed install. A
     *         `unique_ptr<T[]>` rather than a `vector` so the table stays the same size as
     *         when `slots` was the vector (pointer + span == vector on both host and rv32)
     *         and drops the vector's capacity word. Never resized: a re-install builds a
     *         whole new table and move-assigns it under the stripe lock. */
    std::unique_ptr<app_field_slot_t[]> owned_slots{};
    /** @brief Owned copy of the declaration bytes for the owning install (name then
     *         descriptor, concatenated per field); empty for a borrowed install whose
     *         slots view caller storage. */
    std::vector<std::byte> backing{};
    /** @brief Class-③ per-field values, index-aligned with @ref slots — LAZILY allocated,
     *         null until the first field write on this vertex (#389 pattern). A
     *         declared-but-never-written table costs zero value RAM. `(*values)[i]` empty
     *         ⇒ field i unset. */
    std::unique_ptr<std::vector<std::vector<std::byte>>> values{};
};

/**
 * @brief The lazily-allocated APP-FIELD group of the extension block (ADR-0058 Step 2):
 *        the RFC-0010 descriptor table plus its owner apply seam, together.
 *
 * `on_app_field_write` co-occurs with the field table (it is the table's apply seam), NOT
 * with the value seam of a HANDLER-role vertex — so it lives here, not in
 * @ref value_handlers_t. A vertex with no app fields and no apply seam keeps this group
 * null and pays neither the table nor the ~32 B `std::function`. Allocated on the first of
 * either `set_app_fields*` (the table) or an `on_app_field_write` at registration; guarded
 * by the vertex mutex, insert-only (never freed before the vertex).
 */
struct app_field_group_t {
    app_field_table_t table; /**< @brief The view-slot descriptor table + lazy value store. */
    /** @brief The owner apply seam (RFC-0010 §A.3): fires after a declared field write
     *         stored its bytes, OUTSIDE the vertex lock. Unset ⇒ bytes just store. */
    std::function<void(std::string_view name, const view_t& value)> on_app_field_write;
};

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
    std::function<result_t<void>(const rope_t&)>
        on_write;                                  /**< @brief Receives the written value. */
    std::function<result_t<view_t>()> on_children; /**< @brief Synthesized `:children[]` listing. */
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
};

/**
 * @brief The internal, lazily-allocated STORAGE of a Handler-role vertex's VALUE seam
 *        (ADR-0058 Step 2) — the three seams `handlers_t` carries minus `on_app_field_write`.
 *
 * Split off from the public @ref handlers_t input so a plain STORED_VALUE or app-field
 * vertex never allocates these ~96 B of `std::function`: the value seam is HANDLER-role
 * only (transports / connections / synthesized listings), so it lives behind a
 * `unique_ptr` in the extension block, null for every other role. `on_app_field_write`
 * co-occurs with app fields, not the value seam, so it moved to @ref app_field_group_t.
 * Set once at registration (`vertex_t::adopt_identity`), read lock-free thereafter.
 */
struct value_handlers_t {
    std::function<result_t<rope_t>()> on_read; /**< @brief Supplies the vertex value on read. */
    std::function<result_t<void>(const rope_t&)>
        on_write;                                  /**< @brief Receives the written value. */
    std::function<result_t<view_t>()> on_children; /**< @brief Synthesized `:children[]` listing. */
};

/**
 * @brief One right bit of an ACE `access_mask` (docs/reference/05 §0x0A, ADR-0020).
 *
 * Single-bit values so a gate tests exactly one right; a stored mask may carry any
 * OR of them. `WRITE_ACL` is precisely the `admin` right (modify the ACL / delegate).
 */
enum class acl_right_t : std::uint32_t {
    READ = 0x01,        /**< @brief Read the vertex value / control fields. */
    WRITE = 0x02,       /**< @brief Write the vertex value / control fields (fan-in gate). */
    SUBSCRIBE = 0x04,   /**< @brief Append a `:subscribers[]` edge (fan-out gate). */
    CREATE = 0x08,      /**< @brief Create a child via `:children[]` (ADR-0017). */
    DELETE = 0x10,      /**< @brief Remove a child (reserved; no core surface yet). */
    READ_ACL = 0x20,    /**< @brief Read the `:acl` field. */
    WRITE_ACL = 0x40,   /**< @brief Modify the `:acl` field — the `admin` right. */
    WRITE_OWNER = 0x80, /**< @brief Transfer ownership (reserved; no core surface yet). */
};

/** @brief The one ACE flag the core subset honors: propagate to the subtree (ADR-0020). */
inline constexpr std::uint8_t kAceInherit = 0x1;

/** @brief An ACE's type (ADR-0020): ALLOW grants; DENY refuses (full policy only). */
enum class ace_type_t : std::uint8_t {
    ALLOW = 0, /**< @brief The ACE grants its mask's rights. */
    DENY = 1,  /**< @brief The ACE refuses them — evaluated only by `full_acl_policy_t`
                    (ADR-0050); the ALLOW-only profile rejects DENY at parse time. */
};

/**
 * @brief One parsed ACE of a vertex's `:acl` (ADR-0020 / #81).
 *
 * Evaluation is the pure per-target policy of ADR-0050 (`security_acl.hpp`): the
 * default ALLOW-only MCU profile rejects a DENY ACE (or any flag bit beyond
 * `kAceInherit`) at write time with TYPE_MISMATCH, so stored ACEs never carry
 * semantics the selected evaluator would silently weaken; the full `security_acl`
 * host policy (LIBTRACER_ACL_FULL) stores DENY and evaluates ordered
 * first-match-per-bit.
 */
struct ace_t {
    ace_type_t type = ace_type_t::ALLOW; /**< @brief ALLOW or DENY (policy-gated at parse). */
    std::uint8_t flags = 0;              /**< @brief ACE flags; only `kAceInherit` is accepted. */
    std::vector<std::byte> subject;      /**< @brief Opaque subject token (ADR-0018); the special
                                              subject `"EVERYONE@"` matches any resolved subject. */
    std::uint32_t access_mask = 0; /**< @brief Granted rights (an OR of `acl_right_t` bits). */
    std::uint64_t expires_ns = 0;  /**< @brief Absolute expiry, ns since the UNIX epoch;
                                        0 = never expires. An expired ACE grants nothing. */
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
 * @brief The in-process per-edge delivery sink: a plain `{fn, ctx}` pair (the ADR-0047
 *        hot-path shape, same doctrine as `tr::net::receiver_slot_t`).
 *
 * Snapshotting one under the fan-out lock is a trivial copy — no per-publish
 * `std::function` copy (which heap-allocates once captures exceed the SBO). The value
 * crosses as the rope it is (ADR-0053 §6); the sink may clone links (refcount bumps).
 */
using subscriber_fn_t = void (*)(void* ctx, const rope_t& value);

/**
 * @brief The COLD wire/gate half of a subscription edge (#380 §3), lazily allocated:
 *        the in-process edge — the common MCU wiring shape (callback or local target,
 *        empty caller) — keeps `subscriber_t::remote` null and pays one pointer
 *        instead of ~90 B of route/link/caller state per edge.
 */
struct subscriber_remote_t {
    /**
     * @brief The consumer's accumulated return route (a complete PATH TLV's bytes — the FWD
     *        `src` the subscribe arrived with).
     *
     * Populated ⇒ a REMOTE subscriber: a write hands (@ref link, this route,
     * @ref delivery_compact, value) to the graph's injected remote-delivery sink,
     * which emits the `FWD{WRITE}` (or auto-promoted COMPACT) back over the link (RFC-0004
     * §D/§E.1, ADR-0035 slice 4 / #136). Held as a view over a REFCOUNTED segment (ADR-0041
     * §2): copied once at subscribe, then every delivery snapshot is a refcount clone —
     * O(1) copies over the subscription's life, and an in-flight delivery keeps the route
     * alive across a concurrent unsubscribe. An opaque view, so L4 never depends on tr::net.
     */
    view_t return_route{};
    std::string link; /**< @brief This node's NAME for the link the subscribe arrived on. */
    /**
     * @brief The caller context this edge was created under (#81, ADR-0026 fan-in gate).
     *
     * The inbound link NAME for a remote subscribe, empty for a locally-wired edge. A
     * fan-out re-dispatch into a LOCAL target vertex is gated by the TARGET's `:acl` WRITE
     * right under this context — the subscription's creator is the "writer" the target
     * authorizes. A REMOTE subscriber's fan-in gate runs on the peer instead (its
     * `FWD{WRITE}` terminus checks the same right).
     */
    std::string caller;
    /**
     * @brief Route-handle opt-in (`SUBSCRIBER.qos_settings.delivery_compact`, RFC-0004
     *        §E.1 / ADR-0035 slice 4).
     *
     * When true the consumer requests label-compacted deliveries: the producer MAY
     * advertise a per-link label aliasing this subscriber's return route and thereafter
     * stream lean COMPACT frames instead of full-route `FWD{WRITE}` deliveries. Default
     * false ⇒ stateless full-route delivery, so a cold/one-shot flow allocates no label
     * state.
     */
    bool delivery_compact = false;
};

/**
 * @brief A subscription edge's canonical PATH key — **immutable and refcount-shared**.
 *
 * Shared rather than owned because the dispatch snapshot must outlive a concurrent
 * unsubscribe: @ref vertex_t::snapshot_edges copies each active slot out under the vertex
 * lock so the graph can dispatch OUTSIDE it, and the slot may be cleared in between. A
 * deep copy satisfied that and cost a **malloc + free per edge per delivery** — a
 * `std::vector` has no small-buffer optimisation, so every non-null key allocated, which is
 * the ordinary local-binding case (`/sensor/temp:subscribers[] -> /dev/ctrl0/in/temp`).
 * Refcounting satisfies it for an atomic increment instead, exactly as
 * @ref edge_view_t::return_route already does one field over for the same hazard.
 *
 * Null ⇒ no local re-dispatch target (the callback-only or remote-only edge). The key is
 * built once at admission and never mutated, so sharing it needs no synchronization beyond
 * the control block's own refcount.
 */
using target_key_t = std::shared_ptr<const std::vector<std::byte>>;

/**
 * @brief Wrap @p key as a shared `target_key_t`, NOTHROW — null on OOM or empty input.
 *
 * Mirrors `vertex_t::try_make_lkv`'s probe-then-commit discipline (#477): under the MCU
 * profile a `bad_alloc` is an `abort()`, and admission is reachable from a peer's bytes
 * (RFC-0014 made registration wire-driven), so this soft-fails by value instead.
 * @param key The canonical PATH key bytes; an empty span yields null (no target).
 */
[[nodiscard]] inline target_key_t try_make_target_key(std::vector<std::byte>&& key) noexcept {
    if (key.empty()) return nullptr;
#if defined(__cpp_exceptions)
    try {
        return std::make_shared<const std::vector<std::byte>>(std::move(key));
    } catch (...) {
        return nullptr;  // only the control-block allocation can throw (the move is noexcept)
    }
#else
    // Declared inside the branch that uses it: at -Wextra an unconditional definition is an
    // unused variable on every exception-enabled build, which is most of CI.
    static constexpr std::size_t kCtrlSlack = 4 * sizeof(void*);  // >= both mainstream ABIs
    if (!tr::detail::probe_bytes(sizeof(std::vector<std::byte>) + kCtrlSlack)) return nullptr;
    return std::make_shared<const std::vector<std::byte>>(std::move(key));
#endif
}

/**
 * @brief One subscription edge (M3b).
 *
 * A write to the owning vertex fans out to a target vertex (@ref target_key —
 * spec-faithful re-dispatch) and/or an in-process @ref callback (sugar), per
 * docs/reference/02 §dispatch + 04 §write fanout. An inactive slot models an unsubscribe
 * (a cleared `:subscribers[N]`). The wire/gate members live in the lazily-allocated
 * @ref remote half (#380 §3), so the plain in-process edge costs 80 B, not 160.
 */
struct subscriber_t {
    target_key_t target_key;            /**< @brief Canonical PATH key (null ⇒ callback-only). */
    subscriber_fn_t callback = nullptr; /**< @brief In-process sink fn; null ⇒ target-only
                                             (ADR-0053 §6 rope value). */
    void* callback_ctx = nullptr;       /**< @brief Caller-owned context passed back to
                                             @ref callback; must outlive every delivery. */
    /**
     * @brief The original SUBSCRIBER TLV view this slot was written from, retained zero-copy
     *        (a refcount clone of the field-write payload).
     *
     * Empty for in-process callback sugar that carries no TLV (the local target sugar DOES
     * carry one — ADR-0049 encodes through the field-write door). A `:subscribers[]` read
     * ropes these slot views into the `FWD{REPLY}` with no byte copy (RFC-0004 §D /
     * ADR-0035 slice 2 zero-copy reply rule). Stays HOT (outside @ref remote) precisely
     * because local field-write-door edges carry it.
     */
    view_t source_view{};
    /** @brief The cold wire/gate half (#380 §3) — null for the plain in-process edge;
     *         allocated by @ref ensure_remote when a route/link/caller/compact-flag is
     *         stored (pay-for-what-you-use, ADR-0021). */
    std::unique_ptr<subscriber_remote_t> remote;
    /** @brief Active flag; an active edge receives every propagated value (delivery is
     *         value-agnostic — WHICH vertices a sweep propagates is the vertex's
     *         `delivery_mode_t`, never a per-subscriber byte comparison). */
    bool active = true;

    /** @brief The cold half, allocated on first use (admission-time only — never on a
     *         dispatch path). */
    subscriber_remote_t& ensure_remote() {
        if (!remote) remote = std::make_unique<subscriber_remote_t>();
        return *remote;
    }
};

/**
 * @brief The dispatch-relevant snapshot of one ACTIVE subscription edge.
 *
 * What @ref vertex_t::snapshot_edges copies out under the vertex lock so the graph can
 * dispatch OUTSIDE it (callbacks / re-dispatch re-enter the graph): the `{fn, ctx}`
 * callback pair, owning copies of the link / caller strings (the slot may be cleared
 * concurrently once dispatch runs outside the lock), and refcount CLONES of the target
 * key and the stored return route (ADR-0041 §2 — a bump, not a byte copy; the clone keeps
 * each alive across a concurrent unsubscribe).
 *
 * The target key is shared rather than copied because the copy was a **malloc + free per
 * edge per delivery**: `std::vector` has no small-buffer optimisation, and this snapshot is
 * a fresh stack object per publish, so every local binding allocated on the fan-out path.
 * `link` and `caller` stay owning copies — they are `std::string`, so the short names that
 * dominate ride the SSO buffer and never reach the allocator.
 */
struct edge_view_t {
    subscriber_fn_t callback = nullptr; /**< @brief The in-process sink fn (null ⇒ none). */
    void* callback_ctx = nullptr;       /**< @brief The sink's caller-owned context. */
    target_key_t target_key; /**< @brief Local re-dispatch target (refcount share, not a copy). */
    std::string link;      /**< @brief Remote-delivery link NAME (owning copy; empty ⇒ local). */
    view_t return_route{}; /**< @brief Consumer return route (refcount clone). */
    bool delivery_compact = false; /**< @brief RFC-0004 §E.1 label-compaction opt-in. */
    std::string caller; /**< @brief The edge's stored ACL fan-in context (#81, owning copy). */
};

/**
 * @brief A transient-local durability latch (RFC-0004 §D / Q4): the LKV plus the freshly
 *        admitted edge's dispatch view, both snapshotted atomically with the append.
 *
 * @ref value stays null when no latch fired (volatile producer, or no LKV yet).
 */
struct edge_latch_t {
    std::shared_ptr<const rope_t> value; /**< @brief The latched LKV; null ⇒ no latch. */
    edge_view_t edge;                    /**< @brief The admitted edge's dispatch view. */
};

/**
 * @brief The fixed-capacity stack buffer of @ref edge_view_t dispatch views — the
 *        no-heap small-fan-out half of @ref vertex_t::snapshot_edges.
 *
 * The element storage is RAW (uninitialized) bytes: declaring one on the publish hot
 * path costs nothing, and only the views actually snapshotted are placement-constructed
 * (and destroyed). A default-constructed `std::array<edge_view_t, 8>` here instead
 * zeroed ~900 bytes of stack per publish — GCC lowers that to eight `rep stos` blocks
 * whose microcode startup latency dominated single-subscriber fan-out (the post-v0.3.0
 * fan1 delivery regression). Non-copyable; reused via @ref clear.
 */
class edge_snapshot_t {
   public:
    /** @brief The snapshot width (mirrored as `vertex_t::kInlineFanout`). */
    static constexpr std::size_t kCapacity = 8;

    /** @brief An empty snapshot; the element storage stays uninitialized (the point). */
    edge_snapshot_t() noexcept = default;
    /** @brief Non-copyable — a transient dispatch buffer, never a value. */
    edge_snapshot_t(const edge_snapshot_t&) = delete;
    /** @brief Non-assignable — a transient dispatch buffer, never a value. */
    edge_snapshot_t& operator=(const edge_snapshot_t&) = delete;
    /** @brief Destroy the constructed views (only those actually snapshotted). */
    ~edge_snapshot_t() { clear(); }

    /** @brief Placement-construct @p v as the next view; the caller (the snapshot loop)
     *         keeps the count ≤ @ref kCapacity. */
    void push_back(edge_view_t v) {
        ::new (static_cast<void*>(raw_ + n_ * sizeof(edge_view_t))) edge_view_t(std::move(v));
        ++n_;
    }
    /** @brief Destroy every constructed view; the buffer is reusable afterwards. */
    void clear() noexcept {
        for (std::size_t i = 0; i < n_; ++i) (*this)[i].~edge_view_t();
        n_ = 0;
    }
    /** @brief The number of views constructed. */
    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    /** @brief The @p i-th snapshotted view (@p i < @ref size). */
    [[nodiscard]] edge_view_t& operator[](std::size_t i) noexcept {
        return *std::launder(reinterpret_cast<edge_view_t*>(raw_ + i * sizeof(edge_view_t)));
    }
    /** @brief The @p i-th snapshotted view (@p i < @ref size), const. */
    [[nodiscard]] const edge_view_t& operator[](std::size_t i) const noexcept {
        return *std::launder(reinterpret_cast<const edge_view_t*>(raw_ + i * sizeof(edge_view_t)));
    }

   private:
    /** @brief Uninitialized element storage (constructed views live at the front). */
    alignas(edge_view_t) std::byte raw_[kCapacity * sizeof(edge_view_t)];
    std::size_t n_ = 0; /**< @brief Constructed-view count. */
};

// The stripe count and the padding width both live in libtracer/config.hpp (ADR-0068):
// kVertexLockStripes and kCacheLineBytes are ordinary constexprs shared by every TU
// through ONE header, so a per-target override (CMake cache variable / Kconfig) can never
// diverge across TUs the way a bare `-D` compile definition could (the std::array size
// below, and now the stripe's alignment, would be an ODR violation).

/**
 * @brief The stripe's alignment: @ref kCacheLineBytes, floored at the payload's own
 *        natural alignment.
 *
 * `alignas` may only ever STRENGTHEN alignment — [dcl.align]/5 makes a weaker request
 * ill-formed, and GCC accepts it silently rather than diagnosing it, so a target that sets
 * @ref kCacheLineBytes to 0 or 4 must not hand that number to `alignas` unchecked. Taking
 * the max of the members' own alignments keeps every value of the knob well-formed, and the
 * `static_assert` under the struct proves the request survived.
 */
inline constexpr std::size_t kStripeAlign =
    std::max({kCacheLineBytes, alignof(std::mutex), alignof(std::atomic<int>)});

/**
 * @brief One shared lock stripe: the mutex + condvar a SET of vertices ride
 *        (#361 §2), replacing a per-vertex `std::mutex` + `std::condition_variable`.
 *
 * Why: the blocking primitives were the single largest per-vertex RAM cost on
 * the MCU target — ESP-IDF pthreads lazily allocate a FreeRTOS mutex (~90 B)
 * plus condvar state PER VERTEX on first touch, and the host paid 88 B of
 * struct. The LKV read/write hot path takes no VERTEX lock (the atomic shared_ptr
 * swap), so a stripe serializes only control-plane verbs (ring trim, edge
 * mutation, ACL state, seq/notify) — cross-vertex contention is
 * wiring-frequency, not per-publish. `await` waits on the stripe's condvar
 * with a PER-VERTEX predicate (`write_seq_`), so a collision costs a spurious
 * wake + re-check, never a correctness change.
 */
struct alignas(kStripeAlign) vertex_stripe_t {  // one cache line per stripe where a second
                                                // core exists to false-share with (see
                                                // kStripeAlign); packed tight where none does
    std::mutex m; /**< @brief Serializes the stripe's vertices' verbs. */
    /** @brief Live `await` waiters on this stripe. Mutated only under @ref m, but READ
     *         without it by a publish that never takes the lock at all (#555), so it is
     *         atomic: the waiterless publish skips the mutex, not just the condvar call
     *         that #370 skipped. See @ref vertex_t::store for the ordering argument that
     *         makes the lock-free read safe against a lost wakeup. */
    std::atomic<int> waiters{0};
};

static_assert(alignof(vertex_stripe_t) == kStripeAlign,
              "the stripe's alignas was silently dropped — kStripeAlign must never ask for "
              "less than the payload's natural alignment (see its derivation above)");

/**
 * @brief The stripe table: `constinit` where the platform's `std::mutex` is
 *        constexpr-constructible, so the per-verb lookup is a plain indexed load with
 *        NO function-local-static init-guard check on the hot path (#370). libstdc++
 *        makes the ctor constexpr only when its gthreads port supports static mutex
 *        init (`__GTHREAD_MUTEX_INIT`) — ESP-IDF's does NOT — and libc++'s always is;
 *        the fallback is a guarded function-local static (one predicted branch per
 *        verb — the MCU's constraint is RAM, not that branch). The condvars live in a
 *        separate guarded table (`vertex_stripe_cv`) because `std::condition_variable`
 *        can never be constant-initialized — only the cold await/wake paths reach it.
 */
#if defined(__GTHREAD_MUTEX_INIT) || defined(_LIBCPP_VERSION)
inline constinit std::array<vertex_stripe_t, kVertexLockStripes> vertex_stripes{};

/** @brief The stripe at table slot @p idx (guard-free constant-initialized table). */
inline vertex_stripe_t& vertex_stripe_at(std::size_t idx) noexcept { return vertex_stripes[idx]; }
#else
/** @brief The stripe at table slot @p idx (guarded-static fallback: this platform's
 *         `std::mutex` has no constexpr ctor, so the table cannot be `constinit`). */
inline vertex_stripe_t& vertex_stripe_at(std::size_t idx) noexcept {
    static std::array<vertex_stripe_t, kVertexLockStripes> stripes{};
    return stripes[idx];
}
#endif

/** @brief The stripe slot of a pinned vertex address (ADR-0056/0057 — the address is a
 *         stable identity). Same vertex ⇒ same slot, always. */
inline std::size_t vertex_stripe_index(const void* v) noexcept {
    std::uintptr_t h = reinterpret_cast<std::uintptr_t>(v);
    h ^= h >> 9;  // fold higher entropy into the allocation-aligned low bits
    return (h >> 6) % kVertexLockStripes;
}

/** @brief The stripe a vertex rides (mutex + waiter count). */
inline vertex_stripe_t& vertex_stripe_of(const void* v) noexcept {
    return vertex_stripe_at(vertex_stripe_index(v));
}

/**
 * @brief The stripe's condvar — a SEPARATE guarded-static table, reached only from
 *        `vertex_t::wait_for_change` and from a publish that saw `waiters != 0`:
 *        the waiterless publish (the hot path) never pays this table's init guard.
 */
inline std::condition_variable& vertex_stripe_cv(std::size_t idx) {
    static std::array<std::condition_variable, kVertexLockStripes> cvs;
    return cvs[idx];
}

/**
 * @brief The lazily-allocated COLD half of a vertex (issue #361 §1): every member a plain
 *        STORED_VALUE leaf with default QoS, no handlers, and no `:acl` never touches.
 *
 * ADR-0021 rule 2 ("the machinery is pay-for-what-you-use") applied to RAM: the common
 * MCU leaf keeps `vertex_t::ext_` null and pays nothing here. Allocated at most once —
 * at registration when the identity needs it (STREAM role, user handlers, non-default
 * settings), or later under the vertex mutex on the first `:acl` / `:settings` write —
 * and never freed before the vertex (the insert-only ADR-0057 lifetime), so a published
 * pointer stays valid for every reader.
 */
struct vertex_ext_t {
    /** @brief The VALUE seam (on_read/on_write/on_children), LAZILY allocated (ADR-0058
     *         Step 2): HANDLER-role only, so a plain leaf / app-field vertex keeps this
     *         null and never pays the ~96 B.
     *
     *         **Read lock-free** (@ref vertex_t::handlers loads it with no stripe lock,
     *         on the hot path). It is therefore an ATOMIC pointer, not a `unique_ptr`:
     *         registration publishes with `store(release)` and retirement
     *         (`vertex_t::revert_to_placeholder`) swaps it to `nullptr` with
     *         `exchange(acq_rel)`. A swapped-out
     *         block is **never freed under a concurrent reader** — the graph parks it and
     *         reclaims it only at teardown (`graph_t::retired_seams_`; ADR-0057's
     *         insert-only discipline extended to the seam: emptied, never dangled). Keeping
     *         the park OFF the per-vertex block costs an app-field / leaf vertex zero extra
     *         bytes. The live block here is freed by this ext's destructor. */
    std::atomic<value_handlers_t*> handlers{nullptr};
    /** @brief STREAM ring (docs/reference/11 role 2), LAZILY allocated on the first
     *         append (#388): an empty libstdc++ `std::deque` allocates its ~512 B map
     *         node at CONSTRUCTION, which every ext-bearing vertex (handlers, app
     *         fields, `:acl`, non-default settings) paid even though only the STREAM
     *         role ever appends. Null ⇒ empty ring. Guarded by the vertex mutex. */
    std::unique_ptr<std::deque<std::shared_ptr<const rope_t>>> history;
    /** @brief Raw `:acl` TLV bytes, served back verbatim (#81-A, ADR-0018/0020); guarded by
     *         the vertex mutex. Empty ⇒ no `:acl` set. */
    std::vector<std::byte> acl;
    /** @brief The `:acl` bytes parsed into core-subset ACEs at write time (#81); guarded by
     *         the vertex mutex. `graph_t::acl_allows` evaluates these. */
    std::vector<ace_t> aces;
    /** @brief The ADR-0050 cached effective-ACE merge (own + INHERIT-flagged ancestor ACEs,
     *         pre-merged in evaluation order); guarded by the vertex mutex, rebuilt lazily
     *         when @ref acl_cache_dirty is raised. Only the MERGE is cached, never a
     *         verdict — expiry evaluates at check time against the caller's now. */
    std::vector<ace_t> eff_aces;
    /** @brief Raised ⇒ @ref eff_aces is stale (rebuild lazily; ADR-0050 cache protocol). */
    std::atomic<bool> acl_cache_dirty{true};
    /** @brief Monotonic `:acl`-mutation counter — bumped (ahead of @ref acl_cache_dirty) by
     *         every writer that invalidates the merge (@ref vertex_t::set_acl,
     *         @ref vertex_t::mark_acl_cache_dirty, placeholder revert). A lazy rebuild
     *         (@ref vertex_t::with_effective_aces) snapshots it before the unlocked walk and,
     *         back under the lock, publishes AND clears the dirty flag ONLY if the counter is
     *         unchanged — so a rebuilder never publishes a stale/torn merge or clears over a
     *         newer mark. 32-bit: wrapping needs 2^32 `:acl` writes DURING one rebuild walk
     *         (physically impossible), and it packs into the padding after @ref
     *         acl_cache_dirty so the merge cache costs no extra per-vertex bytes. */
    std::atomic<std::uint32_t> acl_gen{0};
    settings_t settings{}; /**< @brief QoS settings; single-field mutation under the vertex
                                mutex (`vertex_t::update_settings`). */
    /** @brief The RFC-0010 APP-FIELD group (ADR-0058 Step 2) — the descriptor table plus
     *         its `on_app_field_write` apply seam, LAZILY allocated: a vertex with no app
     *         fields and no apply seam keeps this null. Guarded by the vertex mutex,
     *         insert-only. Null ⇒ the closed `ENOTTY` default (pre-RFC `:schema` shape). */
    std::unique_ptr<app_field_group_t> app;
    /** @brief STREAM drain cursor (RFC-0008 §E): the write seq at the last flush, so a
     *         propagate drains only the entries appended since; guarded by the vertex
     *         mutex. */
    std::uint64_t last_flushed_seq = 0;

    /** @brief Free the live handler block. `handlers` is a raw atomic pointer (for
     *         lock-free reads) so it no longer self-frees; this closes that. Blocks parked
     *         by retirement live on the graph, not here. Runs from `~vertex_t`'s
     *         `delete ext_`. */
    ~vertex_ext_t() { delete handlers.load(std::memory_order_acquire); }
    vertex_ext_t() = default;
    vertex_ext_t(const vertex_ext_t&) = delete;
    vertex_ext_t& operator=(const vertex_ext_t&) = delete;
};

/**
 * @brief An L4 graph vertex: a named, addressable position holding a value, a bounded
 *        history, or a user handler (docs/reference/11 §roles).
 *
 * Pinned in place (the atomic last-known-value slot + mutex + condvar are non-movable) and
 * always handled via a `vertex_handle_t` returned by `graph_t::register_vertex` (ADR-0056). The
 * read/write hot path takes no vertex lock (an atomic shared_ptr swap); the mutex guards only the
 * history ring, the subscriber list, and the await waiter accounting. Non-copyable.
 *
 * The public surface is a VERB interface — storage (@ref store / @ref read_stored),
 * readiness (@ref note_write / @ref wait_for_change / the seq cursors), edges
 * (@ref add_edge / @ref clear_edge / @ref snapshot_edges), and ACL state (@ref set_acl /
 * @ref with_aces / @ref with_effective_aces) — each verb taking the vertex mutex
 * internally (the LKV slot stays
 * lock-free). `graph_t` keeps what SPANS vertices: routing, ancestor walks, fan-out
 * dispatch legs, the effective-ACL walk, admission, and the field surface.
 */
class vertex_t {
   public:
    /** @brief The no-heap small-fan-out snapshot width (@ref snapshot_edges buffer size). */
    static constexpr std::size_t kInlineFanout = edge_snapshot_t::kCapacity;

    /** @brief Construct a vertex with its role, own canonical NAME record (ADR-0057 — one
     *         segment, not the full key), QoS settings, and handlers. The cold extension
     *         block is allocated only if this identity needs one (#361 §1). */
    vertex_t(role_t role, path_key_t name, settings_t settings, handlers_t handlers)
        : name_(std::move(name)), role_(role) {
        adopt_identity(role, settings, std::move(handlers));
    }

    vertex_t(const vertex_t&) = delete;
    vertex_t& operator=(const vertex_t&) = delete;

    /** @brief Free the cold extension block (allocated at most once, ADR-0057 lifetime). */
    ~vertex_t() { delete ext_.load(std::memory_order_acquire); }

    /** @brief This vertex's behavioral role. */
    [[nodiscard]] role_t role() const noexcept { return role_; }
    /** @brief This vertex's own canonical NAME record (its single path segment, ADR-0057);
     *         empty at the root. The full key is a parent-walk concatenation
     *         (`graph_t`'s `build_key`). */
    [[nodiscard]] const path_key_t& name() const noexcept { return name_; }
    /** @brief This vertex's QoS settings (`kDefaultSettings` when no extension block —
     *         a default-QoS vertex stores no copy of them). */
    [[nodiscard]] const settings_t& settings() const noexcept {
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        return e != nullptr ? e->settings : kDefaultSettings;
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

    /**
     * @brief Fill this node with a registration's identity: set the role, QoS settings, and
     *        handlers, and mark it @ref registered.
     *
     * Called under the graph's UNIQUE map lock — either on a freshly constructed node or on
     * a placeholder being registered in place (the allocation never moves, ADR-0057).
     */
    void fill(role_t role, settings_t settings, handlers_t handlers) {
        role_ = role;
        adopt_identity(role, settings, std::move(handlers));
        registered_ = true;
        // Maintain the parent's lock-free fork bit (#652). Setting is unconditional and
        // idempotent; the root has no parent, and nothing asks about the root's parent.
        if (parent_ != nullptr) parent_->set_flag(flag_t::REGISTERED_CHILD, true);
    }

    /** @brief Flip this vertex back to a placeholder (invisible to `find`) — retirement's
     *         inverse of the `registered_ = true` in @ref fill. Map-lock state; the caller
     *         (`graph_t::retire`) MUST hold the graph map lock, same as @ref fill's writer.
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
     * The counter is mutated only by @ref fill and @ref mark_unregistered, both of which run
     * under the graph's UNIQUE map lock, so mutations are already serialized; the atomic is
     * what makes the *read* race-free. A reader concurrent with a registration may observe
     * either side of it — exactly as it could when the fork took a shared lock, since the
     * API orders a `read` against a concurrent `register_vertex` no more strongly than this.
     * The composed branch read re-acquires the map lock for its own walk, so the ordering
     * that walk depends on is not this counter's to provide.
     */
    [[nodiscard]] bool has_registered_child() const noexcept {
        return test_flag(flag_t::REGISTERED_CHILD, std::memory_order_acquire);
    }

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

    // -- storage & readiness ----------------------------------------------------------

    /**
     * @brief Store @p value as this vertex's state: publish the last-known-value
     *        (lock-free), append the STREAM ring (keep-last trim), bump the write
     *        sequence, and wake awaiters.
     *
     * One allocation (`make_shared`): the rope's inline small-buffer holds the
     * single-link trivial case, so a scalar write costs exactly what the `view_t`
     * slot cost (ADR-0053 §6). The LKV publish happens BEFORE the lock; only the
     * ring trim + seq bump + notify run under it. Not for Handler-role writes —
     * the graph runs `handlers().on_write` and calls @ref note_write instead.
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
     *         published or appended (#477 nothrow soft-fail — the graph maps this to
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
        if (role_ != role_t::STREAM) {
            write_seq_.fetch_add(1, std::memory_order_seq_cst);
            if (st.waiters.load(std::memory_order_seq_cst) == 0) return sp;
            const std::lock_guard lock(st.m);
            vertex_stripe_cv(vertex_stripe_index(this)).notify_all();
            return sp;
        }

        // STREAM keeps the original shape verbatim: the ring append is real state mutation
        // and must stay under the stripe mutex.
        {
            const std::lock_guard lock(st.m);
            vertex_ext_t* e = ext_.load(std::memory_order_acquire);
            if (e != nullptr) {  // STREAM identity always has ext
                // The ring's deque legs (shell + map + one node per chunk) throw on OOM
                // (#477). Probe one conservative per-append bound first (libstdc++'s
                // 512 B chunk + map/shell slack — every -fno-exceptions MCU target;
                // wider-chunk hosts keep real exceptions) and on failure SKIP the
                // append: the ring is bounded-lossy by contract (drain: "entries
                // trimmed before the drain are lost"), so a pressure-dropped history
                // entry is valid behavior — the LKV above already published.
                static constexpr std::size_t kRingAppendProbe = 1024;
                if (tr::detail::probe_bytes(kRingAppendProbe)) {
                    if (!e->history)  // first append allocates the ring (#388 lazy deque)
                        e->history = std::make_unique<std::deque<std::shared_ptr<const rope_t>>>();
                    e->history->push_back(sp);  // refcount bump — the caller keeps `sp`
                    const std::size_t keep =
                        e->settings.history_keep_last ? e->settings.history_keep_last : 1;
                    while (e->history->size() > keep) e->history->pop_front();
                }
            }
            write_seq_.fetch_add(1, std::memory_order_seq_cst);
            // Waiterless publish skips the condvar entirely (#370): we hold st.m, so a
            // zero count here cannot race a registration.
            if (st.waiters.load(std::memory_order_relaxed) != 0)
                vertex_stripe_cv(vertex_stripe_index(this)).notify_all();
        }
        return sp;
    }

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
        if (vertex_ext_t* e = ext_.load(std::memory_order_acquire))
            e->last_flushed_seq = write_seq_.load(std::memory_order_relaxed);
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
     * @return The number of entries drained (0 ⇒ nothing appended since the last flush,
     *         or the snapshot could not be allocated — retry on the next flush).
     */
    std::size_t drain_unflushed(std::vector<std::shared_ptr<const rope_t>>& out) {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        if (e == nullptr) return 0;  // no ring — nothing was ever appended
        const std::uint64_t now = write_seq_.load(std::memory_order_relaxed);
        if (now == e->last_flushed_seq) return 0;
        const std::uint64_t n_new = now - e->last_flushed_seq;
        if (!e->history) {  // seq advanced but no ring — nothing to drain
            e->last_flushed_seq = now;
            return 0;
        }
        const auto take =
            static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(n_new, e->history->size()));
        // Nothrow-reserve BEFORE the cursor advance: a failed snapshot leaves the ring
        // marked un-flushed (deferred delivery), instead of a throwing assign (#477).
        if (!tr::detail::try_reserve(out, static_cast<std::size_t>(take))) return 0;
        e->last_flushed_seq = now;
        out.assign(e->history->end() - take, e->history->end());  // within capacity
        return out.size();
    }

    /** @brief The STREAM ring contents, oldest first — each entry a rope clone (refcount
     *         bumps, no byte copy). */
    [[nodiscard]] std::vector<rope_t> history_snapshot() {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        std::vector<rope_t> out;
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        if (e == nullptr || !e->history) return out;
        out.reserve(e->history->size());
        for (const auto& sp : *e->history) out.push_back(*sp);
        return out;
    }

    // -- subscription edges -----------------------------------------------------------

    /**
     * @brief Append a subscription edge; atomically snapshot the transient-local
     *        durability latch when @p latch is non-null.
     *
     * Under ONE lock hold: the slot is appended, and — iff this vertex is
     * transient-local (`settings.durability == 1`) and already holds an LKV — the
     * value plus the new edge's dispatch view are snapshotted into @p latch, so a
     * concurrent `clear_edge` can never slip between append and latch. The caller
     * dispatches the latch OUTSIDE the lock (RFC-0004 §D / ADR-0049).
     *
     * An INACTIVE slot is REUSED before the list grows (RFC-0009 §D.2: "a cleared
     * slot MAY be reused by a later append") — the reclamation half of eviction:
     * a churning link (unsubscribe / peer departure, then re-subscribe) reoccupies
     * its freed slots instead of growing `subs_` without bound. Slot indices of
     * ACTIVE edges are never renumbered (§D.2 stability); only a slot already
     * cleared can come to mean a new edge.
     * @return The occupied slot's index (the `:subscribers[N]` slot number).
     */
    std::size_t add_edge(subscriber_t s, edge_latch_t* latch = nullptr) {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        std::size_t idx = subs_.size();
        for (std::size_t i = 0; i < subs_.size(); ++i) {
            if (!subs_[i].active) {
                idx = i;
                break;
            }
        }
        if (idx == subs_.size())
            subs_.push_back(std::move(s));
        else
            subs_[idx] = std::move(s);  // reuse frees the cleared slot's leftovers
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        if (latch != nullptr && e != nullptr && e->settings.durability == 1) {
            if (std::shared_ptr<const rope_t> lkv = lkv_.load()) {
                latch->value = std::move(lkv);
                latch->edge = edge_view_of(subs_[idx]);
            }
        }
        return idx;
    }

    /**
     * @brief Deactivate the edge slot @p idx (unsubscribe — a cleared `:subscribers[N]`).
     * @return true iff the slot existed and was active (the caller then adjusts the
     *         RFC-0005 listener bookkeeping).
     */
    bool clear_edge(std::size_t idx) {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        if (idx >= subs_.size() || !subs_[idx].active) return false;
        // RECLAIM in place, not merely deactivate. Flipping `active` alone left the slot's
        // `target_key` buffer, its `source_view` segment pin and the whole cold `remote`
        // half resident until an unrelated `add_edge` happened to land on this index — so an
        // unsubscribed edge kept a frame segment alive indefinitely. This is the same
        // move-an-inert-shell reclaim @ref evict_link_edges already performs under this very
        // lock, and it is safe for the same reason: an @ref edge_view_t snapshot owns its
        // copies and a refcount clone of the route (ADR-0041 §2), so releasing the pin here
        // can never dangle a dispatch already in flight.
        //
        // The 80-byte slot SHELL stays — RFC-0009 §D.2 makes `:subscribers[]` indices
        // stable, so the vector must not shrink; only the retained state is freed.
        subscriber_t reclaimed;    // an inert shell: no view, no route, no cold half
        reclaimed.active = false;  // the slot is free for add_edge reuse
        subs_[idx] = std::move(reclaimed);
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
     * @param latch Optional durability latch; snapshotted iff the vertex is
     *              transient-local (`settings.durability == 1`) and holds an LKV.
     * @return Which case applied — see @ref edge_replace_t.
     */
    edge_replace_t replace_edge(std::size_t idx, subscriber_t s, edge_latch_t* latch = nullptr) {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        if (idx >= subs_.size()) return edge_replace_t::OUT_OF_RANGE;
        const bool was_active = subs_[idx].active;
        subs_[idx] = std::move(s);  // reclaims the displaced edge's pins in place
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        if (latch != nullptr && e != nullptr && e->settings.durability == 1) {
            if (std::shared_ptr<const rope_t> lkv = lkv_.load()) {
                latch->value = std::move(lkv);
                latch->edge = edge_view_of(subs_[idx]);
            }
        }
        return was_active ? edge_replace_t::REPLACED_ACTIVE : edge_replace_t::FILLED_EMPTY;
    }

    /**
     * @brief Deactivate AND reclaim every active subscriber edge stored against the
     *        link @p link — the per-vertex half of peer-departure eviction (RFC-0009
     *        §D, extended to link teardown).
     *
     * Matches each active slot whose cold half stores @p link as the link the
     * subscribe arrived on (`subscriber_remote_t::link`); a local edge (no cold
     * half, or an empty link) never matches. Unlike @ref clear_edge, a matched slot
     * is RECLAIMED, not just flagged: the stored SUBSCRIBER view, the return-route
     * refcount pin, the target key, and the whole `subscriber_remote_t` block are
     * released in place (the slot shell stays — §D.2 index stability — and
     * @ref add_edge reuses it). An in-flight delivery is unaffected: its
     * @ref edge_view_t snapshot owns copies and a refcount CLONE of the route
     * (ADR-0041 §2), so releasing the slot's pin here never dangles a dispatch.
     * @return The number of edges evicted (the caller unwinds exactly this many
     *         from the RFC-0005 listener bookkeeping).
     */
    std::size_t evict_link_edges(std::string_view link) {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        std::size_t n = 0;
        for (subscriber_t& s : subs_) {
            if (!s.active || s.remote == nullptr || s.remote->link != link) continue;
            subscriber_t reclaimed;    // an inert shell: no view, no route, no cold half
            reclaimed.active = false;  // the slot is free for add_edge reuse
            s = std::move(reclaimed);  // frees the old slot's retained state in place
            ++n;
        }
        return n;
    }

    /**
     * @brief Snapshot every ACTIVE edge's dispatch view into caller storage — the
     *        snapshot-under-lock half of the snapshot/dispatch-outside discipline.
     *
     * Small fan-out (the common case, ≤ `kInlineFanout`) placement-constructs into
     * @p inline_buf — no heap allocation AND no dead stack zeroing per publish; a
     * larger subscriber list reserves @p overflow once and fills it instead (then
     * @p overflow is non-empty and holds ALL views).
     *
     * Every allocation here is NOTHROW (#477 — this runs on the writer thread's
     * fan-out, where a bad_alloc is an abort() under `-fno-exceptions`): an
     * unreservable @p overflow degrades the snapshot to the first `kInlineFanout`
     * views in @p inline_buf (the rest of this delivery is dropped), and an edge
     * whose owning copies cannot be cloned is skipped (that one delivery dropped).
     * The small local fan-out (empty target/link/caller strings) stays allocation-
     * free end to end, so the hot path cannot even reach a probe.
     * @param inline_buf The caller's raw stack buffer (cleared on entry).
     * @param overflow   The heap fallback for large fan-out (cleared on entry).
     * @return The number of views snapshotted (into whichever buffer was used).
     */
    std::size_t snapshot_edges(edge_snapshot_t& inline_buf, std::vector<edge_view_t>& overflow) {
        inline_buf.clear();
        overflow.clear();
        const std::lock_guard lock(vertex_stripe_of(this).m);
        const bool use_heap = subs_.size() > edge_snapshot_t::kCapacity &&
                              tr::detail::try_reserve(overflow, subs_.size());
        std::size_t n = 0;
        for (const subscriber_t& s : subs_) {
            if (!s.active) continue;
            // OOM fallback (reserve failed on a wide list): the inline prefix delivers,
            // the remainder of this fan-out is dropped — never an abort.
            if (!use_heap && n == edge_snapshot_t::kCapacity) break;
            edge_view_t e;
            if (!try_edge_view_of(s, e)) continue;  // OOM: drop this one edge's delivery
            if (use_heap)
                overflow.push_back(std::move(e));  // reserved above — no reallocation
            else
                inline_buf.push_back(std::move(e));
            ++n;
        }
        return n;
    }

    /**
     * @brief The stored SUBSCRIBER TLV view of the active slot @p idx (a `:subscribers[N]`
     *        read) — a refcount clone, no byte copy; `nullopt` for a missing / inactive /
     *        TLV-less (in-process sugar) slot.
     */
    [[nodiscard]] std::optional<view_t> edge_source(std::size_t idx) {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        if (idx < subs_.size() && subs_[idx].active && subs_[idx].source_view.owner)
            return subs_[idx].source_view;  // clone (refcount bump)
        return std::nullopt;
    }

    /** @brief Every active slot's stored SUBSCRIBER view, in slot order (the
     *         `:subscribers[]` array read) — each a refcount clone. */
    [[nodiscard]] std::vector<view_t> edge_sources() {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        std::vector<view_t> out;
        out.reserve(subs_.size());
        for (const subscriber_t& s : subs_)
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
     *        ACEs + the cached merge), the app-field table, the QoS settings, the role,
     *        and the delivery mode. **Survives** by design: `write_seq_` (monotonic per
     *        address; a reset would regress the readiness cursors), `listeners_above_`
     *        (counts ANCESTOR subscribers, which retiring THIS vertex never touched — the
     *        graph adjusts it for cleared descendant edges), and the allocation / name /
     *        links (ADR-0057 insert-only — emptied, never freed or detached).
     *
     * @note `registered_` is NOT touched here — it is map-lock state the graph flips. The
     *       caller MUST hold the graph map lock. This RETURNS the swapped-out value-seam
     *       block (or nullptr) rather than freeing it: a lock-free reader may still hold
     *       the old pointer, so the graph parks it (`graph_t::retired_seams_`) and frees
     *       it at teardown. The per-vertex stripe lock is taken internally.
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
        retire_gen_.fetch_add(1, std::memory_order_release);
        set_flag(flag_t::OWN_ACES, false);
        lkv_.clear(std::memory_order_release);  // a mid-read reader holds its own
                                                // reference — safe under either policy.
        own_subs_.store(0, std::memory_order_relaxed);
        role_ = role_t::STORED_VALUE;                // the placeholder default (see graph.cpp)
        delivery_mode_ = delivery_mode_t::IF_NEWER;  // graph drops the unconditional_ entry
        value_handlers_t* detached = nullptr;
        if (vertex_ext_t* e = ext_.load(std::memory_order_acquire); e != nullptr) {
            // The value seam is read lock-free — swap it out atomically and hand the old
            // block back to the caller to PARK (never free it under a possible concurrent
            // reader). The remaining ext fields are mutated under the stripe lock.
            detached = e->handlers.exchange(nullptr, std::memory_order_acq_rel);
            const std::lock_guard lock(vertex_stripe_of(this).m);
            e->history.reset();
            e->acl.clear();
            e->aces.clear();
            e->eff_aces.clear();
            e->acl_gen.fetch_add(1, std::memory_order_release);
            e->acl_cache_dirty.store(true, std::memory_order_release);
            e->settings = kDefaultSettings;
            e->app.reset();
            e->last_flushed_seq = 0;
        }
        // subs_ is stripe-guarded; clear it in its own critical section (the ext block may
        // be absent, but subs_ always exists). The graph has already adjusted descendant
        // listeners_above_ for these edges before calling us.
        {
            const std::lock_guard lock(vertex_stripe_of(this).m);
            subs_.clear();
        }
        return detached;
    }

    /**
     * @brief Store this vertex's `:acl`: the raw TLV bytes (served back verbatim by an
     *        `:acl` read) plus the same bytes parsed into typed ACEs (what evaluation
     *        walks). Storing replaces; empty ⇒ no restrictions.
     */
    void set_acl(std::span<const std::byte> raw, std::vector<ace_t> aces) {
        vertex_ext_t& e = ensure_ext();
        const std::lock_guard lock(vertex_stripe_of(this).m);
        e.acl.assign(raw.begin(), raw.end());
        e.aces = std::move(aces);
        // Lock-free bearing flag (#361 §3): the graph's nearest-bearing-ancestor walk
        // reads it without touching any stripe. Publish under the lock, before the
        // dirty flag, same ordering discipline as the ACE list itself.
        set_flag(flag_t::OWN_ACES, !e.aces.empty());
        // Publish-then-mark (ADR-0050 cache protocol): the new ACEs are visible
        // under m_ BEFORE the generation bump and dirty flag are raised, so a rebuild
        // that observes the flag always reads the new list (or leaves the flag set for
        // the next one). Bump the generation ahead of the flag so a rebuild in flight
        // over the OLD list detects the write at publish and declines to clear.
        e.acl_gen.fetch_add(1, std::memory_order_release);
        e.acl_cache_dirty.store(true, std::memory_order_release);
    }

    /** @brief A copy of the stored raw `:acl` TLV bytes (empty ⇒ no `:acl` set). */
    [[nodiscard]] std::vector<std::byte> acl_bytes() {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        return e != nullptr ? e->acl : std::vector<std::byte>{};
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
     * @brief Mark this vertex's cached effective-ACE merge stale (ADR-0050).
     *
     * Raised by the graph on every `:acl` write for the WRITTEN vertex's whole
     * subtree (subtree-precise invalidation via the ADR-0057 child links —
     * wiring-frequency); @ref set_acl raises it for the written vertex itself.
     * The next @ref with_effective_aces on a marked vertex rebuilds lazily.
     * @note Lock-free (a release store) — callable under the graph's map lock
     *       during the subtree walk without touching any vertex mutex.
     */
    void mark_acl_cache_dirty() noexcept {
        // No extension block ⇒ no cached merge exists to invalidate; a block created
        // later starts dirty, so a concurrent first-gated-op cannot miss this mark
        // (its rebuild reads ancestor ACEs already published before this walk).
        if (vertex_ext_t* e = ext_.load(std::memory_order_acquire)) {
            // Bump the generation ahead of the flag (release), so a lazy rebuild racing
            // this mark from an ancestor :acl write detects the change at publish.
            e->acl_gen.fetch_add(1, std::memory_order_release);
            e->acl_cache_dirty.store(true, std::memory_order_release);
        }
    }

    /**
     * @brief Evaluate against this vertex's cached effective-ACE merge, rebuilding
     *        it first iff it is stale — the ADR-0050 cached-merge verb.
     *
     * When the dirty flag is raised the generation is SNAPSHOTTED, this vertex's own
     * parsed ACEs are SNAPSHOTTED, and @p rebuild runs with the stripe lock RELEASED
     * (#361 §2): the graph's rebuild walks the immutable parent chain taking each
     * ancestor's @ref with_aces — one stripe lock at a time, never nested — so an
     * ancestor sharing this vertex's stripe cannot self-deadlock, and no cross-stripe
     * ordering exists at all. The merge is then stored under a re-acquired lock, the
     * flag is lowered only if the generation is unchanged, and @p eval runs over the
     * cached list.
     *
     * Race resolution (rebuild vs concurrent `:acl` write): the writer publishes ACEs
     * and BUMPS the generation BEFORE raising the flag (@ref set_acl / graph subtree
     * mark). The flag is lowered AFTER the fresh merge is published and only when the
     * generation is unchanged (#425): so `dirty == false` under the lock always means a
     * published merge — a concurrent reader that still sees the flag raised rebuilds
     * rather than evaluating a not-yet-populated (empty, open-by-default) cache, which
     * would transiently allow a denied caller. A write landing during the unlocked
     * rebuild window advances the generation, so the rebuilder declines to clear and the
     * possibly-stale merge is rebuilt on the NEXT check; a stale-forever cache is
     * impossible. Concurrent rebuilds may interleave; each stores a valid merge of some
     * recent state, and the flag/generation protocol converges the cache.
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
        vertex_ext_t& e = ensure_ext();  // gated eval caches its merge here (fresh ⇒ dirty)
        std::unique_lock lock(vertex_stripe_of(this).m);
        // Generation-gated rebuild (#425). `acl_gen` is bumped — ahead of the dirty flag,
        // lock-free — by every invalidator of THIS vertex's merge: its own `set_acl`, the
        // placeholder revert, and the subtree mark an ancestor `:acl` write fans out
        // (`mark_acl_cache_dirty`). A rebuild snapshots the generation BEFORE the UNLOCKED
        // ancestor walk (#361 §2 releases the stripe lock so an ancestor sharing this stripe
        // cannot self-deadlock) and, back under the lock, verifies it is UNCHANGED. That one
        // guard gates BOTH the publish and the clear:
        //   - unchanged ⇒ no `:acl` write touched this merge across the walk, so `merged` is a
        //     clean, current snapshot: publish it, then lower the flag. `dirty == false` under
        //     the lock therefore ALWAYS means eff_aces holds a CURRENT published merge — closing
        //     the original #425 window where a loser read the empty (open-by-default) cache
        //     after the winner cleared the flag (a transient fail-open).
        //   - changed ⇒ a write landed during the walk, so `merged` may be stale or torn:
        //     DISCARD it and retry with the new generation. Publishing it would let a slow
        //     rebuilder CLOBBER a fresh merge a faster one already published, and — since a
        //     mismatched clear is a no-op, not a re-raise — leave `dirty == false` over a stale
        //     merge: a PERSISTENT fail-open. Never publishing or clearing across a generation
        //     change also means no `:acl` mark is ever lost (no stale-forever cache).
        // A reader that finds the flag already clear takes the fast path — evaluate the current
        // cached merge, no rebuild — and a rebuilder whose flag a peer clears mid-retry likewise
        // falls through to that fresh cache.
        while (e.acl_cache_dirty.load(std::memory_order_acquire)) {
            const std::uint32_t gen = e.acl_gen.load(std::memory_order_acquire);
            const std::vector<ace_t> own = e.aces;  // snapshot; rebuild runs unlocked
            lock.unlock();
            std::vector<ace_t> merged = rebuild(static_cast<const std::vector<ace_t>&>(own));
            lock.lock();
            if (e.acl_gen.load(std::memory_order_acquire) != gen)
                continue;  // an :acl write raced the walk — drop the stale merge, rebuild
            e.eff_aces = std::move(merged);
            e.acl_cache_dirty.store(false, std::memory_order_release);
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

    // -- settings & propagation policy --------------------------------------------------

    /** @brief A consistent copy of the QoS settings (taken under the vertex lock;
     *         `kDefaultSettings` when no extension block). */
    [[nodiscard]] settings_t settings_snapshot() {
        const std::lock_guard lock(vertex_stripe_of(this).m);
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        return e != nullptr ? e->settings : kDefaultSettings;
    }

    /**
     * @brief Mutate the QoS settings under the vertex lock: @p f receives `settings_t&`;
     *        its return value is passed through (the `:settings.<field>` write surface —
     *        concurrent single-field writes both land, no read-modify-write clobber).
     *        Allocates the extension block on a default-QoS vertex's first write.
     */
    template <typename F>
    auto update_settings(F&& f) -> decltype(f(std::declval<settings_t&>())) {
        vertex_ext_t& e = ensure_ext();
        const std::lock_guard lock(vertex_stripe_of(this).m);
        return f(e.settings);
    }

    /** @brief How this vertex participates in an ANCESTOR's propagate sweep (RFC-0008 §C). */
    [[nodiscard]] delivery_mode_t delivery_mode() const noexcept { return delivery_mode_; }
    /** @brief Set the propagation policy — wiring-time, via `graph_t::set_delivery_mode`
     *         (which also maintains the sweep's UNCONDITIONAL membership). */
    void set_delivery_mode(delivery_mode_t mode) noexcept { delivery_mode_ = mode; }

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
    /** @brief Adjust the own active-slot count by @p delta (subscribe/unsubscribe). */
    void bump_own_subs(std::int32_t delta) noexcept {
        own_subs_.fetch_add(static_cast<std::uint32_t>(delta), std::memory_order_relaxed);
    }
    /** @brief The active subscriber slots on strict ancestors — the one relaxed load the
     *         write hot path pays before deciding whether to walk ancestors at all. */
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
    };

    /** @brief Set or clear @p f. An RMW, because the two bits have two different writers. */
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
     * @brief The @ref store LKV allocation (control block + rope), NOTHROW: `nullptr` on
     *        OOM instead of the bad_alloc that abort()s under the MCU profile's
     *        `-fno-exceptions` (#477, the engine-task storm crash class).
     *
     * Host profile (exceptions on): catch — zero cost on the hot success path, no probe
     * race. MCU profile: the mem_heap.hpp probe-then-commit discipline; the probe covers
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

    // The dispatch view of one slot; call with m_ held. Owning copies of the byte/string
    // fields (the slot may be cleared while dispatch runs outside the lock); the route
    // copy is a refcount clone (ADR-0041 §2 — keeps it alive across an unsubscribe).
    //
    // Single named return (NRVO), filled in place: this runs once per active edge on
    // every fan-out (snapshot_edges), so it is a dispatch hot path. The earlier
    // two-branch double-return brace-initialized the empty cold members and defeated
    // NRVO, costing ~7 ns/edge — ~+7 µs/publish at fan-out 1024 (the #385
    // subscriber-cold-split regression). The cold `remote` fields keep their
    // default-member-init values (empty link/route/caller, non-compact) when local.
    [[nodiscard]] edge_view_t edge_view_of(const subscriber_t& s) const {
        edge_view_t e;
        e.callback = s.callback;
        e.callback_ctx = s.callback_ctx;
        e.target_key = s.target_key;
        if (s.remote != nullptr) {
            e.link = s.remote->link;
            e.return_route = s.remote->return_route;
            e.delivery_compact = s.remote->delivery_compact;
            e.caller = s.remote->caller;
        }
        return e;
    }

    /**
     * @brief The NOTHROW twin of `edge_view_of` for the writer-thread fan-out snapshot
     *        (#477): fill @p out with the slot's dispatch view, soft-failing instead of
     *        throwing when an owning copy (target key / link / caller) cannot allocate.
     *
     * A local callback edge copies nothing that allocates (empty key, no cold half, the
     * route is a refcount clone), so the hot small-fan-out path never reaches a probe.
     * `edge_view_of` stays for the admission-time latch (control plane). Call with the
     * stripe lock held.
     * @retval false An owning copy failed (OOM) — drop this edge's delivery; @p out is
     *         partially filled and must be discarded.
     */
    [[nodiscard]] bool try_edge_view_of(const subscriber_t& s, edge_view_t& out) const noexcept {
        out.callback = s.callback;
        out.callback_ctx = s.callback_ctx;
        out.target_key = s.target_key;  // refcount clone — nothrow, and no longer a malloc
        if (s.remote != nullptr) {
            if (!tr::detail::try_assign(out.link, s.remote->link) ||
                !tr::detail::try_assign(out.caller, s.remote->caller))
                return false;
            out.return_route = s.remote->return_route;  // refcount clone — nothrow
            out.delivery_compact = s.remote->delivery_compact;
        }
        return true;
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
     * lock-free readers (@ref settings / @ref handlers) stay valid forever.
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
     * @brief Install a registration's identity (constructor + @ref fill): allocate the
     *        extension block iff this identity needs one — STREAM role (history ring),
     *        any user handler, or non-default QoS — and store the cold members there.
     *        A plain default leaf allocates nothing (#361 §1).
     */
    void adopt_identity(role_t role, const settings_t& settings, handlers_t handlers) {
        const bool has_handlers = handlers.on_read || handlers.on_write || handlers.on_children ||
                                  handlers.on_app_field_write;
        if (role != role_t::STREAM && !has_handlers && settings == kDefaultSettings &&
            ext_.load(std::memory_order_acquire) == nullptr)
            return;
        vertex_ext_t& e = ensure_ext();
        e.settings = settings;
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

    path_key_t name_;  // own canonical NAME record (one segment; empty at the root) — the
                       // full key is rendered on demand by walking parent_ (ADR-0057);
                       // immutable once the node is linked (lock-free parent walks)

    // The stored value is a rope (ADR-0053 §6): a contiguous scalar is a single-link
    // rope (small-buffer inline, no extra alloc), a chunked stream keeps its links.
    /** @brief The last-known value, held through the slot policy this target bound
     *         (`tr::graph::lkv_slot_t` in `config.hpp`; ADR-0069 §1). The default binding is
     *         `sp_atomic_slot_t` — today's `std::atomic<std::shared_ptr<const rope_t>>`,
     *         which is lock-free by CONTRACT and spin-locked in practice. `lkv_slot.hpp`
     *         documents that caveat and the contract any replacement must satisfy. Do not
     *         read "lock-free" here as "no serializing operation". */
    lkv_slot_t lkv_{};
    std::vector<subscriber_t> subs_;  // fan-out edges; guarded by m_
    // The lazily-allocated cold half (#361 §1): handlers, STREAM ring, the ACL state +
    // ADR-0050 effective-merge cache, non-default settings, and the stream drain cursor.
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
    role_t role_;  // behavioral role (byte-wide enum)
    // How this vertex participates in an ANCESTOR's propagate sweep (RFC-0008 §C).
    // Set at wiring time via graph_t::set_delivery_mode (the "configure before frames
    // flow" contract, like the QoS settings); read on the assign path. Default IF_NEWER.
    delivery_mode_t delivery_mode_ = delivery_mode_t::IF_NEWER;
    // Two lock-free predicates, packed into ONE byte so the flag group stays exactly four
    // bytes wide and `sizeof(vertex_t)` stays at the 112 the #361 diet measured — the size
    // gate's own failure message says to put a new member behind vertex_ext_t rather than
    // inline it, and a bit costs less than either. Written under a lock (a different one
    // per bit), read lock-free off hot paths, so the writes are RMWs and compose.
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
     * atomic. Placed here rather than in @ref vertex_ext_t deliberately: the ext block is
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
};

}  // namespace tr::graph
