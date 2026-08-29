/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The RFC-0010 application-property field tables of an L4 vertex (ADR-0058): the
 * owner-declared remote-access class, one field's descriptor record, the view-shaped
 * declaration slot, the borrowed-install argument, the per-vertex descriptor table and
 * the lazily-allocated extension group that carries it together with its apply seam.
 *
 * Extracted from `vertex.hpp` (#868), which had fused this concern with the vertex core,
 * the subscription machinery and the lock-stripe table into one 3421-line hub every
 * net-plane TU pulled. Nothing here knows what a vertex IS: these are the storage classes
 * ADR-0058 names, and `vertex_ext_t` owns one group of them.
 */
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/view.hpp"

/**
 * @file
 * @brief `tr::graph` application-property field tables (RFC-0010 §A.2/§B, ADR-0058).
 */

namespace tr::graph {

// L1 types this layer consumes (upward dependency on tr::view, docs/adr/0016 §2).
using view::view_t;

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
 * The value and descriptor bytes are OPAQUE to the runtime (stored and served verbatim —
 * the last store-verbatim control surface, now that `:acl` re-encodes from its parsed
 * projection, #907): dtype/range validation is the owner's, in its apply seam
 * (@ref handlers_t::on_app_field_write) — the runtime
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
 * with the vertex's value seam — so it lives here, not in
 * @ref value_handlers_t. A vertex with no app fields and no apply seam keeps this group
 * null and pays neither the table nor the ~32 B `std::function`. Allocated on the first of
 * either `set_app_fields*` (the table) or an `on_app_field_write` at registration; guarded
 * by the vertex mutex, insert-only (never freed before the vertex).
 */
struct app_field_group_t {
    app_field_table_t table; /**< @brief The view-slot descriptor table + lazy value store. */
    /** @brief The owner apply seam (RFC-0010 §A.3): fires after a declared field write
     *         stored its bytes, OUTSIDE the vertex lock. Unset ⇒ bytes just store. Never fires
     *         for a write the field ADMISSION seam refused — that seam lives in the value-seam
     *         block (`tr::graph::value_handlers_t::on_app_field_admit`), NOT here: this group is
     *         carried by every app-field-bearing vertex, and a filter almost none of them
     *         install may not cost all of them a `std::function`. */
    std::function<void(std::string_view name, const view_t& value)> on_app_field_write;
};

}  // namespace tr::graph
