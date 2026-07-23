/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0004 / ADR-0035 slice 4 — the route-handle: ws delivery-compaction. The ws
 * (full-TLV) counterpart of transport_can's `identity↔path` map (#55/ADR-0030).
 *
 * Taken literally, "a delivery *is* a FWD WRITE" (RFC-0004 §D) makes every streamed
 * sample re-carry its full return route — ~16x overhead on a small high-rate
 * sample (RFC-0004 §E.1). The fix is header-elision generalized: a per-link LABEL
 * that aliases an established delivery route. A label is meaningful only on the
 * link it was bound for; each forwarding hop SWAPS it (MPLS-style), exactly as a
 * CAN-ID is re-resolved against each bus. Binding is advertise-driven: the upstream
 * advertises `label ↔ route` in-band when a compact-flagged flow starts; each hop
 * learns `label → (downstream link, out-label)` and re-advertises downstream with
 * its OWN label. Re-advertise on (re)connect is the self-heal (ADR-0030); a
 * delivery bearing an unknown/stale label is dropped with a NACK that prompts a
 * re-advertise — never a crash.
 *
 * `route_handle_t` is the per-node label store: a small `label ↔ binding` map kept
 * per link, scoped to the flows explicitly flagged `delivery_compact`. A
 * cold/one-shot/non-compact flow allocates NO entry here, preserving the slice-3
 * stateless-forwarder property. The orchestration (advertise propagation + COMPACT
 * swap) lives in fwd_router_t, which owns one route_handle_t.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tr::net {

/**
 * @brief One learned per-link label binding — what an inbound label means here.
 *
 * Either a forwarding swap (rewrite to @ref out_label and re-emit over @ref
 * down_link) or a local terminus (resolve @ref local_route and apply the write).
 */
struct handle_binding_t {
    bool terminus = false;     /**< @brief true ⇒ deliver locally; false ⇒ forward + swap. */
    std::string down_link;     /**< @brief Forward: this node's NAME for the downstream link. */
    std::uint16_t out_label{}; /**< @brief Forward: label to stamp on the downstream COMPACT. */
    std::vector<std::byte>
        local_route; /**< @brief Terminus: the local dst PATH TLV bytes to resolve + write. */
};

/**
 * @brief Per-connection `label ↔ route` tables for ws delivery-compaction (RFC-0004 §E.1).
 *
 * The label state lives PER LINK (ADR-0038 §3 / ADR-0039): each connection owns
 * its own small tables — an INGRESS table (a label arriving on the link → its
 * @ref handle_binding_t), an EGRESS table (a label this node advertised over the
 * link → the route it aliases, retained so a NACK can re-advertise), and a
 * monotonic label allocator — drawn from the injected memory resource and guarded
 * by the LINK'S OWN mutex, so label traffic on one connection never contends with
 * another. The only cross-link lock is a `shared_mutex` over the link registry,
 * taken exclusively only when a link's tables are first CREATED (never on clear —
 * which empties an existing entry under the per-link mutex, leaving the registry
 * insert-only so a handed-out table reference can never dangle). State exists only for flows
 * that opted into compaction, so @ref ingress_count on a node forwarding only
 * one-shot/cold traffic is zero.
 */
class route_handle_t {
   public:
    /**
     * @brief Draw all label state from @p mr (ADR-0039 §1).
     *
     * A bounded node passes a pool resource over its slab and the label tables
     * live entirely in host-chosen memory; the default is the standard heap.
     * @p mr must outlive this object.
     */
    explicit route_handle_t(std::pmr::memory_resource* mr = std::pmr::get_default_resource())
        : mr_(mr), links_(mr) {}

    route_handle_t(const route_handle_t&) = delete;
    route_handle_t& operator=(const route_handle_t&) = delete;

    /**
     * @brief Record an ingress binding: a @p label arriving on @p in_link means @p binding.
     * @param in_link This node's NAME for the link the ADVERTISE/COMPACT arrives on.
     * @param label   The label as seen on that inbound link.
     * @param binding Its meaning (forward-swap or local terminus).
     */
    void bind_ingress(std::string_view in_link, std::uint16_t label, handle_binding_t binding);

    /**
     * @brief Look up what a @p label arriving on @p in_link means (nullopt ⇒ stale/unknown).
     * @param in_link This node's NAME for the inbound link.
     * @param label   The inbound label.
     * @return The learned binding, or `std::nullopt` if no binding exists (drop + NACK).
     */
    [[nodiscard]] std::optional<handle_binding_t> lookup_ingress(std::string_view in_link,
                                                                 std::uint16_t label) const;

    /**
     * @brief Remember the @p route advertised over @p out_link under @p label.
     *
     * Lets this node re-advertise the binding on a HANDLE_NACK (or reconnect) without
     * re-deriving the route. Idempotent — re-recording the same key replaces it.
     * @param out_link This node's NAME for the downstream link the ADVERTISE went out on.
     * @param label    The label this node assigned for that downstream flow.
     * @param route    The (possibly stripped) dst PATH TLV bytes the label aliases.
     */
    void record_egress(std::string_view out_link, std::uint16_t label,
                       std::vector<std::byte> route);

    /**
     * @brief Find this link's label for @p route, or allocate + record a fresh one (#136).
     *
     * The producer-origin lazy-advertise primitive (RFC-0004 §E.1, Q5): the first compact
     * delivery on a `(out_link, route)` flow has no binding, so a new label is allocated,
     * recorded as egress, and returned with `fresh == true` (the caller must send the
     * ADVERTISE once); subsequent deliveries find the same label and return `fresh ==
     * false` (send only the COMPACT). @ref clear_link drops the binding so a post-reconnect
     * delivery re-advertises — the self-heal, with no transport "up" event. Distinct from
     * @ref alloc_label + @ref record_egress (which always mint a new label, used by the
     * forwarding-hop swap).
     *
     * @param out_link This node's NAME for the downstream link.
     * @param route    A complete PATH TLV's bytes — the delivery route the label aliases.
     * @return `{label, fresh}` — the (reused or new) label, and whether it was just created.
     */
    [[nodiscard]] std::pair<std::uint16_t, bool> ensure_egress(std::string_view out_link,
                                                               std::span<const std::byte> route);

    /**
     * @brief The route this node advertised over @p out_link under @p label (for re-advertise).
     * @param out_link This node's NAME for the downstream link.
     * @param label    The downstream label.
     * @return The retained route PATH bytes, or `std::nullopt` if unknown.
     */
    [[nodiscard]] std::optional<std::vector<std::byte>> egress_route(std::string_view out_link,
                                                                     std::uint16_t label) const;

    /**
     * @brief Allocate a fresh, per-link, monotonic label (≥1; 0 is reserved "none").
     * @param link This node's NAME for the link the label is scoped to.
     * @return A label unique among this link's currently allocated labels.
     */
    [[nodiscard]] std::uint16_t alloc_label(std::string_view link);

    /**
     * @brief Drop ALL state (ingress, egress, allocator) for @p link — the self-heal hook.
     *
     * A transport calls this on (re)connect/disconnect of @p link so a subsequent
     * re-advertise rebinds from a clean slate; a delivery on a now-cleared label is
     * stale and is NACK'd rather than misrouted.
     * @param link This node's NAME for the link whose state to forget.
     */
    void clear_link(std::string_view link);

    /** @brief Count of live ingress bindings (tests assert a non-compact flow holds 0). */
    [[nodiscard]] std::size_t ingress_count() const;

    /** @brief Count of live egress (advertised) bindings. */
    [[nodiscard]] std::size_t egress_count() const;

   private:
    // One connection's label state (ADR-0038 §3): flat pmr entry arrays (a link
    // carries FEW compact flows, so a linear label scan beats a node-based map —
    // no per-entry allocation, cache-linear) + the link's own mutex. Non-movable
    // (the mutex), constructed in place in the node-based registry map below.
    struct ingress_entry_t {
        std::uint16_t label = 0;
        handle_binding_t binding;
    };
    struct egress_entry_t {
        std::uint16_t label = 0;
        std::pmr::vector<std::byte> route;
    };
    struct link_tables_t {
        explicit link_tables_t(std::pmr::memory_resource* mr) : ingress(mr), egress(mr) {}
        std::mutex m;
        std::pmr::vector<ingress_entry_t> ingress;
        std::pmr::vector<egress_entry_t> egress;
        std::uint16_t next_label = 1;  // 0 is reserved "none"
    };

    /** @brief The link's tables, created on first use (exclusive registry lock). */
    [[nodiscard]] link_tables_t& tables(std::string_view link);
    /** @brief The link's tables if they exist (shared registry lock), else nullptr. */
    [[nodiscard]] link_tables_t* find_tables(std::string_view link) const;

    std::pmr::memory_resource* mr_;
    mutable std::shared_mutex links_m_;  // registry only: create/clear, never per delivery
    std::pmr::map<std::pmr::string, link_tables_t, std::less<>> links_;
};

/**
 * @brief Encode an ADVERTISE frame: `ADVERTISE{ VALUE label(u16), PATH route }`.
 * @param label      The per-link label being bound (this hop's outbound label).
 * @param route_path A complete PATH TLV's bytes — the dst route the label aliases.
 * @return The framed ADVERTISE TLV bytes, ready for transport_t::send.
 */
[[nodiscard]] std::vector<std::byte> encode_advertise(std::uint16_t label,
                                                      std::span<const std::byte> route_path);

/**
 * @brief Encode a COMPACT delivery: `COMPACT{ VALUE label(u16), <payload TLV> }`.
 * @param label   The per-link label naming the established route (no route bytes ride).
 * @param payload A complete payload TLV's bytes (the delivered VALUE).
 * @return The framed COMPACT TLV bytes, ready for transport_t::send.
 */
[[nodiscard]] std::vector<std::byte> encode_compact(std::uint16_t label,
                                                    std::span<const std::byte> payload);

/**
 * @brief NOTHROW `encode_advertise` — build the frame into @p out, soft-failing on OOM
 *        instead of a bad_alloc `abort()` under `-fno-exceptions` (#477).
 *
 * For the writer-thread delivery path (a compact flow's first delivery re-advertises); the
 * throwing form stays for setup-time callers. A dropped ADVERTISE self-heals: the peer's
 * HANDLE_NACK on the unknown label prompts a re-advertise (RFC-0004 §E.1).
 * @retval false The frame buffer could not be allocated — @p out is left empty.
 */
[[nodiscard]] bool try_encode_advertise(std::vector<std::byte>& out, std::uint16_t label,
                                        std::span<const std::byte> route_path) noexcept;

/**
 * @brief NOTHROW `encode_compact` — build the frame into @p out, soft-failing on OOM
 *        instead of a bad_alloc `abort()` under `-fno-exceptions` (#477).
 *
 * For the writer-thread per-delivery egress: on OOM the delivery drops (a subscriber
 * missing one value under heap exhaustion is valid delivery behavior), never an abort.
 * @retval false The frame buffer could not be allocated — @p out is left empty.
 */
[[nodiscard]] bool try_encode_compact(std::vector<std::byte>& out, std::uint16_t label,
                                      std::span<const std::byte> payload) noexcept;

/**
 * @brief Encode a HANDLE_NACK: `HANDLE_NACK{ VALUE label(u16) }` (stale-label signal).
 * @param label The unknown/stale label that prompted the NACK.
 * @return The framed HANDLE_NACK TLV bytes, sent back over the inbound link.
 */
[[nodiscard]] std::vector<std::byte> encode_handle_nack(std::uint16_t label);

}  // namespace tr::net
