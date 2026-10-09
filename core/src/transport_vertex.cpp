/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/transport_vertex.hpp"

#include <array>
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <span>
#include <thread>
#include <utility>

#include "libtracer/builtin_transports.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/config_reader.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/key_view.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/packed_path.hpp"
#include "libtracer/path.hpp"
#include "libtracer/self_heal_link.hpp"
#include "libtracer/tlv_emit.hpp"

namespace tr::net {

using graph::path_t;
using graph::result_t;
using graph::status_t;
using graph::vertex_handle_t;
using view::view_t;
using wire::tlv_node_t;
using wire::type_t;

namespace {

/**
 * @brief Parse the optional SPEC `config` SETTINGS (the shared config_reader_t walk): NAME "addr"
 *        NAME <utf8>, NAME "kind" NAME <utf8>, NAME "port" VALUE u16, NAME "max_frame" /
 *        "backoff" / "connect_timeout" VALUE u32.
 *
 * There is no `role` key. The role is POSITIONAL — it IS the module (RFC-0014 §1/§3) — and
 * the only key that ever carried it was the superseded `:children[]` spelling's override,
 * retired with that door at S7. A `role` pair on the wire is now an unknown pair: ignored,
 * like every other unknown pair, never obeyed.
 *
 * ONLY the universal
 * keys land here (ADR-0043 §5 leanness): kind-private pairs (e.g. quic's `tls`/`insecure`)
 * are the kind's factory's business — it parses them from the raw config TLV it
 * receives. Unknown pairs are ignored (forward-compat). `keepalive` is one of them since
 * #1666: it had no consumer, so it is accepted and dropped rather than stored.
 */
void parse_config(const tlv_node_t* config, conn_settings_t& s) {
    const wire::config_reader_t cfg(config);
    if (const auto v = cfg.name("addr")) s.addr = *v;
    if (const auto v = cfg.name("kind")) s.kind = *v;
    if (const auto v = cfg.u16("port")) {
        s.port = *v;
        s.port_set = true;
    }
    if (const auto v = cfg.u32("max_frame")) s.max_frame = *v;
    if (const auto v = cfg.u32("backoff")) s.backoff_ms = *v;
    if (const auto v = cfg.u32("connect_timeout")) s.connect_timeout_ms = *v;
}

/** @brief @p s's bytes as a byte span (no copy). */
[[nodiscard]] std::span<const std::byte> text_bytes(std::string_view s) noexcept {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

/**
 * @brief Append one TLV (`<type> <opt> <length> <body>`) to @p out — `wire::emit_tlv`'s bytes,
 *        written into a failable store (#1780).
 *
 * The same rule `emit_tlv` applies: trailer bits cleared, the length widened to u32 for a body
 * past 0xFFFF; the header itself is `wire::store_header`'s, so the layout has one spelling.
 * @retval false The store refused — @p out may hold a partial record; the caller drops it.
 */
[[nodiscard]] bool put_tlv(mem::bytes_t& out, type_t type, wire::opt_t opt,
                           std::span<const std::byte> body) noexcept {
    opt = opt.without_trailer();
    if (body.size() > 0xFFFFu) opt.ll = true;
    std::array<std::byte, 6> head{};
    const std::size_t n = wire::header_bytes(opt);
    wire::store_header(std::span<std::byte>(head).first(n), type, opt, body.size());
    return out.append(head.data(), n) && out.append(body.data(), body.size());
}

/**
 * @brief Append one packed path segment record `[u8 len][bytes]` per entry of @p segs —
 *        `wire::emit_path_segment`'s bytes, into a failable store.
 * @retval false A segment is empty or longer than a record can say, or the store refused.
 */
[[nodiscard]] bool put_segments(mem::bytes_t& out,
                                std::initializer_list<std::string_view> segs) noexcept {
    for (const std::string_view seg : segs) {
        if (seg.empty() || seg.size() > wire::kPackedSegMaxBytes) return false;
        const auto len = static_cast<std::byte>(seg.size());
        if (!out.append(&len, 1) || !out.append(text_bytes(seg).data(), seg.size())) return false;
    }
    return true;
}

/** @brief @p key less its last packed record, which spells @p last (the parent's key). */
[[nodiscard]] std::span<const std::byte> parent_key(const mem::bytes_t& key,
                                                    std::string_view last) noexcept {
    return mem::as_span(key).first(key.size() - 1 - last.size());
}

/**
 * @brief Replace @p out with @p parts joined by `/` — the qualified connection key
 *        `net/<module>/<name>` and the staging key `<module>/<name>`.
 * @retval false The store refused.
 */
[[nodiscard]] bool join_into(mem::string_t& out, std::initializer_list<std::string_view> parts) {
    out.clear();
    for (const std::string_view part : parts) {
        if (!out.empty() && !out.append("/")) return false;
        if (!out.append(part)) return false;
    }
    return true;
}

/** @brief True when @p key is `<dir>/...` — @p dir followed by a segment separator. */
[[nodiscard]] bool under_dir(std::string_view key, std::string_view dir) noexcept {
    return key.size() > dir.size() && key.starts_with(dir) && key[dir.size()] == '/';
}

/**
 * @brief Re-encode a validated node to owned wire bytes — the connection's copy of the SPEC's
 *        `config` SETTINGS, taken because the node BORROWS the write's rope, which is gone
 *        once the write returns (and long gone by the liveness engine's first re-dial).
 *        Trailers are not re-emitted (a config SETTINGS never carries one; the reader would
 *        ignore it anyway).
 * @retval false The store refused.
 */
[[nodiscard]] bool put_node(mem::bytes_t& out, const tlv_node_t& tlv) noexcept {
    if (!tlv.opt().pl) return put_tlv(out, tlv.type(), tlv.opt(), tlv.payload());
    mem::bytes_t body(out.source());
    for (const tlv_node_t child : tlv.children())
        if (!put_node(body, child)) return false;
    return put_tlv(out, tlv.type(), tlv.opt(), mem::as_span(body));
}

/**
 * @brief Encode a module's creation catalog as the content of its endpoint's `:schema`
 *        `SETTINGS` (RFC-0014 Amendment 3) — one RFC-0013 §B per-key record per key.
 *
 * `NAME <key> SETTINGS{ NAME "dtype" NAME <tag>, [NAME "required" VALUE 01], <descriptor> }`:
 * the `dtype` and `required` members are PROJECTED from the declaration the endpoint
 * validates against, so the advertised catalog cannot contradict the refusal, and the
 * module's descriptor bytes follow verbatim. Runs once, when the endpoint is minted; the
 * result is handed to the graph, which keeps its own copy, so nothing here outlives the call.
 * @retval false The store refused.
 */
[[nodiscard]] bool encode_catalog(mem::bytes_t& out, conn_catalog_t catalog) noexcept {
    for (const conn_key_t& key : catalog.keys()) {
        mem::bytes_t record(out.source());
        const std::byte yes{1};
        if (!put_tlv(record, type_t::NAME, {}, text_bytes("dtype")) ||
            !put_tlv(record, type_t::NAME, {}, text_bytes(to_string(key.dtype))))
            return false;
        if (key.required && (!put_tlv(record, type_t::NAME, {}, text_bytes("required")) ||
                             !put_tlv(record, type_t::VALUE, {}, std::span(&yes, 1))))
            return false;
        if (!record.append(key.descriptor.data(), key.descriptor.size()) ||
            !put_tlv(out, type_t::NAME, {}, text_bytes(key.name)) ||
            !put_tlv(out, type_t::SETTINGS, wire::opt_t{.pl = true}, mem::as_span(record)))
            return false;
    }
    return true;
}

/**
 * @brief Does a `SPEC`'s @p config conform to the module's declared @p catalog (RFC-0014 §2)?
 *
 * Read through the SAME pair walk every factory reads its keys through, so "conforms" means
 * exactly "the factory will see this key": a catalogued key that appears anywhere in the
 * config must have a well-formed reading in its declared shape — the right value type and,
 * for a `VALUE`, the exact width — and a `required` key must appear. A catalogued key the
 * walk would silently read as absent (a `NAME` where a `u16` belongs, a 4-byte `port`) is
 * the very silent default the catalog exists to turn into a refusal. Keys the catalog does
 * not name are not looked at: they stay forward-compatible unknown pairs (RFC-0014
 * Amendment 4), and an empty catalog therefore accepts every config.
 */
[[nodiscard]] bool conforms(conn_catalog_t catalog, const tlv_node_t* config) noexcept {
    const wire::config_reader_t cfg(config);
    for (const conn_key_t& key : catalog.keys()) {
        if (!cfg.has(key.name)) {
            if (key.required) return false;
            continue;
        }
        bool readable = false;
        switch (key.dtype) {
            case conn_dtype_t::UTF8:
                readable = cfg.name(key.name).has_value();
                break;
            case conn_dtype_t::BOOL:
            case conn_dtype_t::U8:
                readable = cfg.u8(key.name).has_value();
                break;
            case conn_dtype_t::U16:
                readable = cfg.u16(key.name).has_value();
                break;
            case conn_dtype_t::U32:
                readable = cfg.u32(key.name).has_value();
                break;
        }
        if (!readable) return false;
    }
    return true;
}

/** @brief A 1-byte link-liveness VALUE TLV (link_state_t) as a view over a segment from
 *         @p backend, the graph's value backend (#2052); empty when it refused (caller-checked). */
[[nodiscard]] view_t link_state_value(link_state_t state, mem::mem_backend_t& backend) {
    view::segment_ptr_t seg = view::segment_alloc(backend, 5);  // a 4-byte header, one state byte
    if (!seg) return view_t{};
    wire::store_header(seg->bytes.first(4), type_t::VALUE, wire::opt_t{}, 1);
    seg->bytes[4] = static_cast<std::byte>(state);
    return view_t::over(std::move(seg));
}

}  // namespace

// ---------------------------------------------------------------------------------------
// The RFC-0014 S6 two-phase control-plane seam (#492). See transport_vertex.hpp's ctl_m_
// doc for the invariant this type exists to make unskippable.
// ---------------------------------------------------------------------------------------

transport_vertex_t::ctl_txn_t::ctl_txn_t(const transport_vertex_t& owner, ctl_scope_t scope)
    : owner_(owner),
      ops_lock_(owner.ops_m_, std::defer_lock),
      lock_(owner.ctl_m_, std::defer_lock) {
    // Both checks run BEFORE their locks, or the diagnosis would be the hang it exists to
    // replace: a thread that reaches here already holding one has come back round through a
    // graph or router callback, and both are plain non-recursive std::mutexes.
    assert(!owner_.ctl_held_by_this_thread() &&
           "transport_vertex_t::ctl_m_ re-entered: a graph/router callback reached back into "
           "the control plane. The work that fans out or joins belongs in ctl_txn_t phase 2.");
    if (scope == ctl_scope_t::OPERATION) {
        assert(!owner_.ops_held_by_this_thread() &&
               "transport_vertex_t control-plane operation re-entered from inside its own "
               "discharge: a liveness subscriber (or another graph/router callback) is "
               "mutating the control plane. Only the LOOKUP doors are re-entrant.");
        ops_lock_.lock();
        owner_.ops_owner_.store(detail::this_thread_id(), std::memory_order_relaxed);
    }
    lock_.lock();
    owner_.ctl_owner_.store(detail::this_thread_id(), std::memory_order_relaxed);
}

transport_vertex_t::ctl_txn_t::~ctl_txn_t() {
    // The backstop: an early return out of any phase-1 decision still releases the lock and
    // still discharges. Nothing collected here is failable in a way a destructor could act
    // on, so the status goes; every explicit caller that wants it calls discharge() itself.
    (void)discharge();
    // `ops_m_` outlives phase 2 by exactly this much: the whole operation, decision and
    // discharge, is one serialized step (see the member's doc). Stamp cleared first, for
    // the reason release_lock() gives.
    if (ops_lock_.owns_lock()) {
        owner_.ops_owner_.store(detail::unowned_thread_id(), std::memory_order_relaxed);
        ops_lock_.unlock();
    }
}

void transport_vertex_t::ctl_txn_t::release_lock() {
    if (!lock_.owns_lock()) return;
    // Clear the stamp BEFORE unlocking: between the two, another thread could take the
    // mutex and stamp itself, and a store made after that would overwrite the new owner.
    owner_.ctl_owner_.store(detail::unowned_thread_id(), std::memory_order_relaxed);
    lock_.unlock();
}

void transport_vertex_t::ctl_txn_t::publish(vertex_handle_t vertex, link_state_t state) {
    publish_ = vertex;
    publish_state_ = state;
}

void transport_vertex_t::ctl_txn_t::unroute(mem::string_t name) { unroute_ = std::move(name); }

void transport_vertex_t::ctl_txn_t::stop_engine(self_heal_link_t* engine) { stop_ = engine; }

void transport_vertex_t::ctl_txn_t::destroy_link(transport_ptr_t link, mem::bytes_t config,
                                                 mem::block_ptr_t<detail_bus::listing_t> listing) {
    destroy_ = std::move(link);
    destroy_config_ = std::move(config);
    destroy_listing_ = std::move(listing);
}

void transport_vertex_t::ctl_txn_t::retire(vertex_handle_t vertex) { retire_ = vertex; }

result_t<void> transport_vertex_t::ctl_txn_t::discharge() {
    release_lock();
    result_t<void> out{};
    // Teardown order, unchanged from when these lines ran under the lock (#494): un-route
    // FIRST so the NAME stops resolving before anything is destroyed, then stop the engine
    // so no liveness write can land on a vertex that is about to retire, then retire, then
    // destroy the socket.
    if (!unroute_.empty()) {
        (void)owner_.router_.remove_child(unroute_.view());
        unroute_.clear();
    }
    // `if constexpr` on the module gate (#1470), here and at every other call INTO the
    // engine: with `kSelfHealLinks = false` nothing mints one, so `stop_` is provably null —
    // and discarding the call is what stops the linker pulling `self_heal_link.cpp`'s 4.3 KB
    // back into an image that can never reach it. A null CHECK would not: the reference is
    // what costs, not the branch.
    if constexpr (kSelfHealLinks) {
        if (stop_ != nullptr) {
            self_heal_link_t* const engine = stop_;
            stop_ = nullptr;
            engine->stop();  // JOINS the worker — the reason this is not under `ctl_m_`
        }
    }
    if (retire_) {
        const vertex_handle_t vertex = *retire_;
        retire_.reset();
        out = owner_.graph_.retire(vertex);
    }
    destroy_.reset();  // JOINS the receive thread — same reason
    // The config the socket (or its engine's settings) viewed goes only after the socket.
    destroy_config_ = mem::bytes_t(mem::null_source());
    destroy_listing_.reset();  // the retired vertex's `:children[]` hook no longer names it
    if (publish_) {
        const vertex_handle_t vertex = *publish_;
        publish_.reset();
        // The fan-out: `write` delivers to this connection's subscribers, and a
        // routing-plane subscriber drives acquire_link/release_link straight back here.
        out = owner_.graph_.write(vertex,
                                  link_state_value(publish_state_, owner_.graph_.value_backend()));
    }
    return out;
}

bool transport_vertex_t::ctl_held_by_this_thread() const noexcept {
    return ctl_owner_.load(std::memory_order_relaxed) == detail::this_thread_id();
}

bool transport_vertex_t::ops_held_by_this_thread() const noexcept {
    return ops_owner_.load(std::memory_order_relaxed) == detail::this_thread_id();
}

// SLIM target ctor (@ref slim_net_t): member init + the graph-side catalog wiring,
// but NO built-in factory registration. This TU-locus deliberately does NOT name
// register_builtin_transports, so a consumer that only ever calls THIS ctor lets the
// linker garbage-collect the udp/tcp/ws factories (and the transport TUs nothing else
// references). The full ctor below delegates here and adds the builtins.
transport_vertex_t::transport_vertex_t(graph::graph_t& graph, fwd_router_t& router,
                                       std::string_view net_root, mem::mem_backend_t* rx_backend,
                                       slim_net_t, mem::block_source_t* egress_src)
    : graph_(graph),
      router_(router),
      rx_backend_(rx_backend),
      // The nullptr guard the FULL ctor used to hold, MOVED here (#873): both ctors reach
      // this one line, so a null argument means the default, the process net sub-pool (#1777),
      // exactly as an omitted argument does. Before this parameter a SLIM node's
      // `egress_source()` answered the process heap unconditionally — it could not be told
      // otherwise, so the accessor lied about that node's store.
      egress_src_(egress_src != nullptr ? egress_src : &mem::net_source()),
      net_root_(*egress_src_),
      pending_links_(*egress_src_),
      conns_(*egress_src_),
      transport_types_(*egress_src_),
      modules_(*egress_src_),
      endpoints_(*egress_src_) {
    if (!net_root_.assign(net_root))
        mem::exhausted_at_init(*egress_src_, "transport_vertex_t: the net root");
    // Register the `<net_root>` grouping vertex if it isn't already. It is the ENUMERATION
    // root (`/net:children[]` lists this plane's modules) and nothing more: RFC-0014 S7
    // retired the `client`/`listener` CREATION registrations that used to hang off it, so a
    // `write /net:children[] += SPEC{type = "client"|"listener", …}` now answers
    // `SCHEMA_NOT_FOUND` like any other unregistered catalog type. The ONE wire creation door
    // for a connection is the per-module creator endpoint `<net_root>/<module>/conn`
    // (`register_module` mints it; RFC-0014 §1/§2).
    if (!graph_.find(path_t::parse(net_root_.view())->key())) {
        (void)graph_.register_vertex(*path_t::parse(net_root_.view()), graph::role_t::STORED_VALUE);
    }
    // The `quic` kind is NOT a builtin: it lives in the separate libtracer_quic
    // module (ADR-0043), which extends this catalog through register_transport_type
    // (quic_transport_factory) — this file never learns about msquic (open/closed).
    // A slim node likewise registers whatever factories it wants after construction.
    //
    // RFC-0014 §4's routing-plane caller (#1816): every remote subscription edge routed
    // through a connection holds that connection's link through the refcount seam, and
    // nothing else does — no timer, no clock, no per-subscription liveness. Installed only
    // where an engine can exist to hold (#1470): on a `kSelfHealLinks = false` build every
    // acquire would be the documented no-op, so the graph is left with nothing to call.
    if constexpr (kSelfHealLinks) {
        graph::graph_hooks_t hooks = graph_.hooks();
        hooks.link_hold = {&transport_vertex_t::link_hold_thunk, this};
        graph_.set_hooks(hooks);
    }
}

transport_vertex_t::~transport_vertex_t() {
    // The creator endpoints first. Each context is cut loose from this object under its own
    // lock, after every call already inside `endpoint_write` has returned; a call that comes
    // in afterwards finds no plane and answers NOT_FOUND. Only then is the endpoint retired,
    // so `<net_root>/<module>/conn` stops resolving, and its context handed to the graph to
    // free once no reader can still reach it (`graph_t::park_release`). If the park is
    // refused, the context is left to the graph's lifetime rather than freed under a reader.
    for (endpoint_ctx_t* const e : endpoints_) {
        {
            std::unique_lock lock(e->m);
            e->self = nullptr;
            e->cv.wait(lock, [e] { return e->calls == 0; });
        }
        (void)graph_.retire(*e->vertex);
        (void)graph_.park_release({e, [](void* c) {
                                       auto* const ctx = static_cast<endpoint_ctx_t*>(c);
                                       mem::drop_in(*ctx->src, ctx);
                                   }});
    }
    // Before `conns_` destructs: destroying an owned socket can fire its departure eviction,
    // which gives back each evicted edge's hold through this seam. Cleared only if it is
    // still this plane's, so a plane wired later onto the same graph keeps its own.
    if constexpr (kSelfHealLinks) {
        graph::graph_hooks_t hooks = graph_.hooks();
        if (hooks.link_hold.ctx != this) return;
        hooks.link_hold = {};
        graph_.set_hooks(hooks);
    }
}

void transport_vertex_t::link_hold_thunk(void* ctx, std::string_view link, bool held) {
    auto* const self = static_cast<transport_vertex_t*>(ctx);
    // Phase 1 only (a `LOOKUP` transaction), so this is safe from inside a fan-out and from
    // a teardown's phase 2. NOT_FOUND — a bus peer, a provided child, a connection whose
    // removal is evicting this very edge — has no count to move.
    (void)(held ? self->acquire_link(link) : self->release_link(link));
}

// FULL (default) ctor: the slim wiring PLUS the built-in transport-factory catalog
// entries (config `kind` selectors) this build compiled in — udp / tcp / ws, each
// from its own TU gated by a per-module CMake option (register_builtin_transports is
// the full-node or CMake-generated dispatcher; see builtin_transports.hpp). Keeping the
// concrete transport references out of this file lets a build DROP a transport without
// a dangling reference — module selection is by compiled TU, no preprocessor macros.
// Delegating to the slim ctor keeps the catalog wiring in one place; the extra call
// here is the ONLY reference to register_builtin_transports, so it (and the builtins)
// stay linked exactly when this ctor is reachable — @ref slim_net_t sheds them.
transport_vertex_t::transport_vertex_t(graph::graph_t& graph, fwd_router_t& router,
                                       std::string_view net_root, mem::mem_backend_t* rx_backend,
                                       mem::block_source_t* egress_src)
    : transport_vertex_t(graph, router, net_root, rx_backend, slim_net, egress_src) {
    // The ADR-0079 net-plane egress store (#873 family 1) is now set by the delegated ctor —
    // the nullptr guard lives there, so a SLIM node gets the same treatment this one does
    // and `egress_source()` answers for both. All that is left here is the builtin catalog.
    register_builtin_transports(*this, rx_backend_, egress_src_);
}

void transport_vertex_t::register_transport_type(std::string_view kind,
                                                 transport_factory_t factory) {
    // Default traits: eager construction, exactly as every pre-S5 registration behaved.
    register_transport_type(kind, factory, transport_kind_traits_t{});
}

void transport_vertex_t::register_transport_type(std::string_view kind, transport_factory_t factory,
                                                 transport_kind_traits_t traits) {
    // The #1470 module gate: on a build that closed the RFC-0014 §4 S5 engine out, a kind
    // that asks for an engine-managed DIAL is REFUSED, never quietly downgraded. Registering
    // it with the trait cleared would be the worst answer available — the connection would
    // come up eagerly, with no redial and no liveness publishing, and nothing would say so.
    // Refusing means the kind is not catalogued at all, so a `SPEC` naming it answers
    // SCHEMA_NOT_FOUND, exactly as any unregistered kind does; the assert names the cause on
    // a debug build, where a setup-time programming error should stop the program.
    // Discarded entirely at the default binding — this costs a stock build nothing.
    if constexpr (!kSelfHealLinks) {
        if (traits.self_heal_dial) {
            assert(!traits.self_heal_dial &&
                   "register_transport_type: a self_heal_dial kind on a kSelfHealLinks=false "
                   "build — the liveness engine is not in this image (#1470)");
            return;
        }
    }
    const ctl_txn_t txn(*this, ctl_scope_t::OPERATION);  // ADR-0063 §3 serialization
    if (kind_entry_t* const row = transport_types_.find(kind)) {
        *row = kind_entry_t{factory, traits};  // replace: the row's key is already held
        return;
    }
    // A setup call: a store too small for the catalog is a sizing bug (ADR-0056, ADR-0083 Q7).
    mem::string_t key(*egress_src_);
    if (!key.assign(kind) ||
        transport_types_.try_emplace(std::move(key), kind_entry_t{factory, traits}).value ==
            nullptr)
        mem::exhausted_at_init(*egress_src_, "transport_vertex_t::register_transport_type");
}

result_t<void> transport_vertex_t::register_module(std::string_view module, std::string_view kind,
                                                   conn_role_t role, conn_catalog_t catalog) {
    // Registration is a minting boundary (ADR-0073 §1): the ONE shared segment-validity
    // predicate gates the name here, exactly as path_t::parse gates the local string tier.
    if (!graph::valid_segment(module)) return std::unexpected(status_t::INVALID_PATH);
    const ctl_txn_t txn(*this, ctl_scope_t::OPERATION);  // ADR-0063 §3 serialization
    // A declaration is KEYED on (kind, role) — `module_for` resolves through that pair — so a
    // second declaration of a pair already declared under a DIFFERENT module used to
    // overwrite the first one's name in place. That is a SILENT RENAME: the first module's
    // `/net/<module>/conn` endpoint stayed minted and answering, while every subsequent
    // `module_for` sent connections of that pair to the second name. Two live doors, one of
    // them unreachable through the resolver, and nothing said so. It is now a loud refusal by
    // value, on the seam's own convention: the pair is already taken, which is what
    // `PATH_IN_USE` says everywhere else in this file (the reserved-name collision, the
    // duplicate connection name).
    //
    // Refused BEFORE the mint below, so a rejected declaration leaves nothing behind — not
    // even the second name's grouping vertex.
    bool declared = false;
    for (const module_decl_t& d : modules_) {
        if (d.kind != kind || d.role != role) continue;
        // Identical re-declaration stays idempotent-OK: setup code that declares the same
        // (module, kind, role) triple twice — a module serving one pair, registered from two
        // places — is not making a contradictory claim, so it keeps succeeding.
        if (d.module != module) return std::unexpected(status_t::PATH_IN_USE);
        declared = true;
        break;
    }
    // "Adding a module adds its creator endpoint and catalog" (RFC-0014 §1). Minted BEFORE
    // the declaration is recorded, so a refusal leaves nothing half-declared: a module whose
    // endpoint could not be registered would advertise a (kind, role) the wire has no door to.
    // Idempotent, so the re-declaration path above runs it again and mints nothing.
    if (auto minted = mint_module_locked(module, catalog); !minted) return minted;
    if (declared) return {};
    module_decl_t decl{mem::string_t(*egress_src_), mem::string_t(*egress_src_), role};
    if (!decl.module.assign(module) || !decl.kind.assign(kind) ||
        modules_.emplace_back(std::move(decl)) == nullptr)
        return std::unexpected(status_t::BACKPRESSURE);
    return {};
}

result_t<std::string_view> transport_vertex_t::module_for(std::string_view kind,
                                                          conn_role_t role) const {
    // The public entry locks (#881) and IS the body: since S7 retired the `:children[]`
    // creation door, no caller resolves a module from inside its own locked section — a
    // creation goes through `declaration_for_locked` — so the #881 entry-locks/`_locked`-body
    // split this used to carry had one caller, this wrapper, and was folded (#1602). `ctl_m_`
    // is a plain NON-RECURSIVE std::mutex (ADR-0063 erratum 1): a future internal caller that
    // already holds it must re-split rather than call this, which would self-deadlock.
    const ctl_txn_t txn(*this);  // ADR-0063 §3 control-plane serialization
    for (const module_decl_t& d : modules_) {
        if (d.kind == kind && d.role == role) return d.module.view();
    }
    // Declared-only (ADR-0073 §4): no derived "<kind>-client"/"<kind>-server" fallback —
    // an undeclared (kind, role) is an unsupported catalog entry, the same convention as
    // an unknown SPEC `type`. The application mints every module segment.
    return std::unexpected(status_t::SCHEMA_NOT_FOUND);
}

/** @brief The declaration an endpoint write resolves its (kind, role) through. */
result_t<const transport_vertex_t::module_decl_t*> transport_vertex_t::declaration_for_locked(
    std::string_view module, std::string_view kind) const {
    const module_decl_t* found = nullptr;
    std::size_t hits = 0;
    for (const module_decl_t& d : modules_) {
        if (d.module != module) continue;
        // A SPEC that DID name a kind must name one this module constructs. Silently
        // creating the module's own kind instead would mount a connection the creator did
        // not ask for — the kind is data the creator supplied, so a mismatch is refused.
        if (!kind.empty() && d.kind != kind) continue;
        ++hits;
        found = &d;
    }
    // Not a (module, kind) this plane declares: the unsupported-catalog-entry convention,
    // the same answer an unknown SPEC `type` and an unregistered transport kind give.
    if (hits == 0) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    // One module declared for two kinds, and a kind-less SPEC: genuinely ambiguous, so it is
    // refused rather than resolved by declaration order — the same ruling (and the same
    // status) the retired `:children[]` spelling gave two stagings sharing a leaf NAME.
    if (hits > 1) return std::unexpected(status_t::TYPE_MISMATCH);
    return found;
}

result_t<void> transport_vertex_t::mint_module_locked(std::string_view module,
                                                      conn_catalog_t catalog) {
    // The `<net_root>/<module>` grouping vertex. graph_.find IS the dedupe — the same rule
    // `make_connection_locked`'s lazy mint follows, and for the same reason: a separate
    // seen-set would be a second source of truth for something the graph already knows.
    // The endpoint's key is built once; the module's is its prefix.
    mem::bytes_t endpoint_key(*egress_src_);
    if (!put_segments(endpoint_key, {net_root_.view().substr(1), module, kConnEndpointName}))
        return std::unexpected(status_t::BACKPRESSURE);
    const std::span<const std::byte> mod_key = parent_key(endpoint_key, kConnEndpointName);
    if (!graph_.find(mod_key)) {
        auto mod = graph_.register_vertex_key(mod_key, graph::role_t::STORED_VALUE, {});
        if (!mod) return std::unexpected(mod.error());
    }
    if (graph_.find(mem::as_span(endpoint_key))) {
        // A second declaration under the same module (a second kind, or the same triple
        // again). The catalog is the ENDPOINT's — there is one `:schema` to serve it from —
        // so it was fixed when the endpoint was minted. Naming no catalog makes no claim;
        // naming that same table is idempotent; naming any OTHER one would leave a
        // declaration whose keys the endpoint neither advertises nor enforces, so it is
        // refused by value, on the "already taken" convention `register_module` uses.
        if (catalog.empty()) return {};
        for (const auto& e : endpoints_) {
            if (e->module == module)
                return e->catalog.same_table(catalog) ? result_t<void>{}
                                                      : std::unexpected(status_t::PATH_IN_USE);
        }
        // An endpoint this plane did not mint (another plane over the same graph and root).
        // It serves no catalog of ours, so a catalog cannot be attached to it here.
        return std::unexpected(status_t::PATH_IN_USE);
    }

    // `role_t::HANDLER` is what makes the endpoint WRITE-ONLY AND VALUELESS (RFC-0014 §2):
    // the graph runs `on_write` and stores no last-known-value, so the write is EXECUTED,
    // never assigned. No `on_read` is installed either — a whole-vertex read answers
    // NOT_FOUND, and the endpoint's one readable facet is `:schema`, which the graph's own
    // field door already serves as the RFC-0014 Amendment 3 catalog envelope:
    // `POINT{NAME "conn", SETTINGS{…}}`, with an EMPTY `SETTINGS` for a module that declares
    // no catalog. That empty answer is conforming, not a stub — the probe of §6 asks whether
    // the endpoint EXISTS, and `SCHEMA_NOT_FOUND` would answer a question nobody asked. A
    // module that DOES declare one has it encoded here, once, and handed to the graph with
    // the registration below: the graph owns the frame, the module what is inside it.
    //
    // The seam's context holds the module by VALUE. The path is the module, so the dispatch
    // never re-derives it from a payload the peer wrote — a creator cannot address one module's
    // endpoint and have the connection mount under another. The context is a hook's `ctx`
    // (RFC-0028 D10), so it must outlive every call the graph can still make through the
    // seam — which, for a write that loaded it just before the retire, is past this object's
    // own lifetime. So it is drawn from the GRAPH's table source, one block per minted module
    // that never moves, and the graph frees it (`endpoint_ctx_t` has the whole story).
    mem::block_source_t& ctx_src = graph_.table_source();
    mem::string_t ctx_module(ctx_src);
    if (!ctx_module.assign(module)) return std::unexpected(status_t::BACKPRESSURE);
    endpoint_ctx_t* const ctx =
        mem::make_in<endpoint_ctx_t>(ctx_src, this, std::move(ctx_module), catalog, &ctx_src);
    if (ctx == nullptr || !endpoints_.push_back(ctx)) {
        mem::drop_in(ctx_src, ctx);
        return std::unexpected(status_t::BACKPRESSURE);
    }
    graph::handlers_t handlers;
    // Every call is counted in and out under the context's own lock, and a call that finds
    // the plane gone answers as the absent path the endpoint now is: see `~transport_vertex_t`.
    handlers.on_write = {
        [](void* c, const graph::value_t& value, const graph::write_ctx_t&) -> result_t<void> {
            auto* e = static_cast<endpoint_ctx_t*>(c);
            std::unique_lock lock(e->m);
            transport_vertex_t* const self = e->self;
            if (self == nullptr) return std::unexpected(status_t::NOT_FOUND);
            ++e->calls;
            lock.unlock();
            const result_t<void> out = self->endpoint_write(e->module, e->catalog, value);
            lock.lock();
            // Signalled under the lock, so the destructor cannot see the count fall and let
            // the plane go before this call is done with the context's lock.
            --e->calls;
            e->cv.notify_all();
            return out;
        },
        ctx};
    // RFC-0014 §5, discharged by the Amendment 2 general contract: the two control payloads
    // this endpoint accepts demand DIFFERENT rights, so the endpoint declares them rather
    // than having `graph_t` learn a transport concept. `SPEC` (create) demands `CREATE`, so
    // the create right is delegable on the endpoint's own ACL without any right on the parent
    // transport; `NAME` (remove) demands `WRITE`, per RFC-0009 §A.2's reserved-and-unused
    // `DELETE`. A peer may hold either without the other. Anything else written here takes the
    // default `WRITE` and is refused by `endpoint_write` on its shape (§2), not by the gate.
    static constexpr graph::payload_right_t kRights[] = {
        graph::payload_right_t{wire::type_t::SPEC, graph::acl_right_t::CREATE},
        graph::payload_right_t{wire::type_t::NAME, graph::acl_right_t::WRITE},
    };
    // Empty for a catalog-less module, which then costs the graph nothing beyond the rows.
    // A catalog that cannot be encoded is refused like a registration that cannot be made.
    mem::bytes_t encoded(*egress_src_);
    auto endpoint =
        encode_catalog(encoded, catalog)
            ? graph_.register_vertex_key(mem::as_span(endpoint_key), graph::role_t::HANDLER,
                                         handlers, {}, kRights, mem::as_span(encoded))
            : std::unexpected(status_t::BACKPRESSURE);
    if (!endpoint) {
        // Nothing may answer for a module whose endpoint does not exist: the context pushed
        // above would otherwise let a retried declaration's catalog check find it. No seam
        // was published, so nothing but this frame can reach it.
        endpoints_.pop_back();
        mem::drop_in(ctx_src, ctx);
        return std::unexpected(endpoint.error());
    }
    ctx->vertex = *endpoint;
    // RFC-0014 §3 (S4): `conn` is HIDDEN from `<net_root>/<module>:children[]`, which returns
    // the module's member CONNECTIONS. The endpoint is the control that creates them, not one
    // of them, so a peer walking the listing as a topology of links would descend into a
    // vertex with no peer behind it. It stays registered and addressable — §6 makes `read
    // <module>/conn:schema` the sanctioned creatability probe, so being unlisted is exactly
    // why it must still resolve. Hiding cannot fail on a vertex just registered, and the
    // status is discarded for that reason: a failure would mean the endpoint vanished between
    // two calls under `ctl_m_`, which the graph's insert-only discipline (ADR-0057) excludes.
    (void)graph_.hide_from_enumeration(*endpoint);
    return {};
}

result_t<void> transport_vertex_t::endpoint_write(std::string_view module, conn_catalog_t catalog,
                                                  const graph::value_t& value) {
    // A DEVICE-link payload is permanently un-parsable on the CPU (ADR-0024), so it is a
    // malformed control write rather than a transient one — the same classification
    // `graph_t::write`'s field surface makes.
    if (!value.all_host()) return std::unexpected(status_t::TYPE_MISMATCH);
    mem::mem_backend_t& backend = rx_backend_ != nullptr ? *rx_backend_ : mem::net_backend();
    // Single-link (every ordinary control write) is zero-copy; a rope that straddled links
    // pays one flatten, and an exhausted backend surfaces as TRANSIENT backpressure rather
    // than being read back as a truncated — and therefore "malformed" — SPEC (#917).
    const auto flat = value.try_materialize(backend);
    if (!flat) return std::unexpected(status_t::BACKPRESSURE);
    const auto payload = wire::tlv_node_t::over(*flat);
    if (!payload) return std::unexpected(status_t::TYPE_MISMATCH);

    // ONE critical section for the whole DECISION: parse, declaration lookup, socket
    // construction and routing all happen under `ctl_m_`, so two peers writing the same
    // endpoint cannot interleave into a half-built connection. Head of the declared lock
    // order (this -> fwd_router_t -> graph_t -> the vertex stripe), and the graph holds none
    // of its own locks across `on_write`, so nothing here descends against that order.
    // What the decision does NOT do is fan out or join: those are phase 2's (S6, #492).
    ctl_txn_t txn(*this, ctl_scope_t::OPERATION);  // ADR-0063 §3 serialization
    switch (payload->type()) {
        case type_t::SPEC: {
            const result_t<void> made = endpoint_create_locked(txn, module, catalog, *payload);
            // A creation's BIRTH-liveness publish is not the creation's verdict — the
            // connection exists either way — so its status is dropped here exactly as it
            // was dropped at the `(void) set_link_state_locked` site it moved from.
            (void)txn.discharge();
            return made;
        }
        case type_t::NAME: {
            const result_t<void> removed =
                endpoint_remove_locked(txn, module, detail::as_string_view(payload->payload()));
            if (!removed) return removed;
            // A removal's verdict IS the retire's, which phase 2 performs.
            return txn.discharge();
        }
        default:
            // Any other payload — a VALUE, an empty write, a structured TLV that is neither
            // control type. The endpoint NEVER falls through to an ordinary assign
            // (RFC-0014 §2); a creator that meant to write data wrote to the wrong vertex.
            return std::unexpected(status_t::TYPE_MISMATCH);
    }
}

result_t<void> transport_vertex_t::endpoint_create_locked(ctl_txn_t& txn, std::string_view module,
                                                          conn_catalog_t catalog,
                                                          const tlv_node_t& spec) {
    // SPEC{ NAME "name" NAME <seg>, NAME "config" SETTINGS{ pairs }? } — no `type` and no
    // `role`: the module in the path already says both (RFC-0014 §1). Read through the ONE
    // pair-consuming walk, exactly as `graph_t::create_child` reads the `:children[]` SPEC.
    // The marker below exempts THIS reader from the connection-config page gate: it walks the
    // SPEC ENVELOPE (`name`, `config`), not a connection config, so its keys are no kind's and
    // belong on no row of docs/modules/connection-config.md. `graph_t::create_child` reads the
    // other door's envelope through the same type and is excluded there for the same reason.
    const wire::config_reader_t pairs(&spec);  // config-keys: not-connection-config
    const std::string_view name = pairs.name("name").value_or(std::string_view{});
    // `name` is REQUIRED and stays required (ADR-0073 §5, #622): an omitted name with a
    // node-assigned fallback would cost retry idempotency — a create retried over a link
    // that dropped its reply would append a SECOND connection instead of answering
    // PATH_IN_USE. The `p<slot>` fallback exists only for creator-LESS inbound peers, where
    // there is no retry because the peer dialed us.
    if (name.empty()) return std::unexpected(status_t::TYPE_MISMATCH);
    // The wire minting boundary runs THE shared segment predicate (ADR-0073 §1) — the same
    // one `graph_t::create_child` runs on the other door — so a name that enters the graph
    // here is expressible in the addressing grammar. Without it the endpoint could mint an
    // enumerable-but-unaddressable connection — the failure the retired `:children[]` door was
    // structurally incapable of, because `graph_t::create_child` ran the predicate for it.
    if (!graph::valid_segment(name)) return std::unexpected(status_t::INVALID_PATH);
    // The reserved name, refused BEFORE anything is built (RFC-0014 §3 create-side).
    // Falling through would answer PATH_IN_USE anyway — `register_vertex_key` collides with
    // the endpoint's own key — but only after the kind's factory had already dialed or bound
    // a socket, which is the side effect a refusal must not have.
    // §2 used to say `tr::schema::type_mismatch` here, contradicting §3; the RFC now says
    // `tr::path::in_use` in both clauses (#492 S7), leaving `type_mismatch` for the empty
    // name and the SPEC envelope's schema-shape violations. So this early return is a
    // side-effect guard only — it answers exactly what falling through would.
    if (name == kConnEndpointName) return std::unexpected(status_t::PATH_IN_USE);

    const std::optional<tlv_node_t> settings_node = pairs.settings("config");
    const tlv_node_t* config = settings_node ? &*settings_node : nullptr;
    // The module's declared catalog (RFC-0014 §2: the device validates "the `config` against
    // its `conn:schema` catalog"). A config that omits a required key, or carries a
    // catalogued key in another shape, is MALFORMED — §2's `tr::schema::type_mismatch` —
    // and is refused here, before a declaration is resolved or a socket built.
    // `SCHEMA_NOT_FOUND` is not this answer: §Compatibility and Amendment 3 reserve it for
    // "endpoint present, config TYPE unknown", the unregistered-kind refusal below.
    if (!conforms(catalog, config)) return std::unexpected(status_t::TYPE_MISMATCH);
    // The connection's own copy of its config (#1780): `config` borrows the write's rope,
    // which is gone once the write returns, and the settings below VIEW their text keys —
    // so they are parsed out of the copy, which the connection keeps for its whole life.
    mem::bytes_t config_bytes(*egress_src_);
    if (config != nullptr && !put_node(config_bytes, *config))
        return std::unexpected(status_t::BACKPRESSURE);
    std::optional<tlv_node_t> owned_node;
    if (config != nullptr) {
        auto node = tlv_node_t::over(mem::as_span(config_bytes));
        if (!node) return std::unexpected(status_t::TYPE_MISMATCH);
        owned_node = *node;
    }
    config = owned_node ? &*owned_node : nullptr;
    conn_settings_t settings;
    parse_config(config, settings);

    const auto declared = declaration_for_locked(module, settings.kind);
    if (!declared) return std::unexpected(declared.error());
    // The role is POSITIONAL — it IS the module (RFC-0014 §1/§3) — so the declaration SETS it.
    // Nothing on the wire can say otherwise: the `role` config pair died with the superseded
    // `:children[]` spelling it belonged to (S7), and `parse_config` no longer reads one, so a
    // creator cannot mount a LISTEN socket under a module whose path promises DIAL.
    settings.role = (*declared)->role;
    // A kind-less SPEC is the staged-link spelling; the module's declared kind is the kind
    // this endpoint constructs, so recording it keeps `settings_of` honest about what the
    // connection is. `make_connection_locked` still prefers a `provide_link` staging over
    // the factory, so filling this in does not change WHICH link is used.
    if (settings.kind.empty()) settings.kind = (*declared)->kind.view();

    const auto made =
        make_connection_locked(txn, module, name, config, std::move(config_bytes), settings);
    if (!made) return std::unexpected(made.error());
    // The endpoint is valueless: the handle the creation produced is the connection's, not
    // this vertex's, and it is deliberately not published anywhere the write can return it.
    return {};
}

result_t<void> transport_vertex_t::endpoint_remove_locked(ctl_txn_t& txn, std::string_view module,
                                                          std::string_view name) {
    // An empty NAME names nothing and is not an "absent" connection — it is a malformed
    // control payload, so it is refused rather than swallowed as a no-op success.
    if (name.empty()) return std::unexpected(status_t::TYPE_MISMATCH);
    // Remove-side reserve (RFC-0014 §2/§3): the endpoint cannot self-destruct. Refused
    // before the lookup, so it never reaches `retire()`.
    if (name == kConnEndpointName) return std::unexpected(status_t::PERMISSION_DENIED);

    mem::string_t qualified(*egress_src_);
    if (!join_into(qualified, {net_root_.view().substr(1), module, name}))
        return std::unexpected(status_t::BACKPRESSURE);
    // An unresolvable name — never created, or already removed — is a NO-OP SUCCESS at this
    // layer (RFC-0014 §2). `retire()`'s own idempotence only covers an already-resolved
    // handle, so the endpoint owns this leg: a retried remove after a dropped reply must
    // answer the same as the first one, or teardown is not retry-safe either.
    if (!conns_.contains(qualified.view())) return {};
    return remove_connection_locked(txn, qualified.view());
}

bool transport_vertex_t::is_structural(wire::key_view_t key) const {
    if (key.empty()) return false;  // the graph root is nobody's structural vertex
    // The net root is emitted as ONE NAME segment everywhere in this file
    // (`make_connection_locked` composes the mount key the same way), so the whole
    // predicate is two segment compares
    // over the key bytes — no key is materialised and nothing is allocated.
    const std::string_view root = net_root_.view().substr(1);
    const wire::key_view_t parent = key.parent();
    const std::string_view leaf = detail::as_string_view(key.last_segment());
    // `<net_root>` itself: the enumeration root the ctor registers.
    if (parent.empty()) return leaf == root;
    // `<net_root>/<module>`: exactly two segments, the first the root. Anything deeper is a
    // connection (`<net_root>/<module>/<name>`) or below one — the peer's mounted graph.
    if (!parent.parent().empty()) return false;
    if (detail::as_string_view(parent.last_segment()) != root) return false;
    const ctl_txn_t txn(*this);  // ADR-0063 §3 control-plane serialization
    for (const module_decl_t& d : modules_) {
        if (d.module == leaf) return true;
    }
    // A module can also be known to this plane without ever having been DECLARED: since S7
    // the wire door needs a declaration, but `provide_link` — the public test/manual seam —
    // takes the module as data and declares nothing, so a staging is a module name `modules_`
    // has never seen. So the staged set and the live connections are the other two places a
    // module name of this plane can be read back from.
    // No fourth container is added for this (commit `221ed983` deleted exactly that state):
    // these are the ones the class already keeps for creation and teardown.
    for (const auto& staged : pending_links_)
        if (under_dir(staged.key, leaf)) return true;  // key is `<module>/<name>`
    for (const auto& conn : conns_) {
        const std::string_view qualified = conn.key;  // `<root>/<module>/<name>`
        if (under_dir(qualified, root) && under_dir(qualified.substr(root.size() + 1), leaf))
            return true;
    }
    return false;
}

void transport_vertex_t::provide_link(std::string_view module, std::string_view name,
                                      transport_t& link) {
    const ctl_txn_t txn(*this, ctl_scope_t::OPERATION);  // ADR-0063 §3 serialization
    // A setup call: a store too small for the staging is a sizing bug (ADR-0056, ADR-0083 Q7).
    mem::string_t key(*egress_src_);
    if (!join_into(key, {module, name}))
        mem::exhausted_at_init(*egress_src_, "transport_vertex_t::provide_link");
    if (transport_t** const staged = pending_links_.find(key.view())) {
        *staged = &link;
        return;
    }
    if (pending_links_.try_emplace(std::move(key), &link).value == nullptr)
        mem::exhausted_at_init(*egress_src_, "transport_vertex_t::provide_link");
}

namespace {
/**
 * @brief A BUS connection vertex's `on_children` hook (RFC-0028 D10 `{fn, ctx}`, @p c the
 *        connection's @ref detail_bus::listing_t): the bus's currently-audible peers as a
 *        POINT of POINT{NAME <peer>} members, built on every read.
 *
 * Drawn as the graph's own `:children[]` door draws (#2052): the scratch for one read from the
 * graph's table source, the answer copied into a view from the graph's value backend, so a
 * graph given its own source takes nothing from the default root here. `ok` collects every
 * refusal: one refused record anywhere refuses the read as BACKPRESSURE.
 */
result_t<view_t> bus_children(void* c) {
    const auto& listing = *static_cast<const detail_bus::listing_t*>(c);
    mem::bytes_t members(listing.graph->table_source());
    bool ok = true;
    listing.bus->enumerate_peers([&](std::string_view peer) {
        mem::bytes_t body(members.source());
        ok &= put_tlv(body, type_t::NAME, wire::opt_t{}, text_bytes(peer)) &&
              put_tlv(members, type_t::POINT, wire::opt_t{.pl = true}, mem::as_span(body));
    });
    mem::bytes_t out(members.source());
    if (!ok || !put_tlv(out, type_t::POINT, wire::opt_t{.pl = true}, mem::as_span(members)))
        return std::unexpected(status_t::BACKPRESSURE);
    // `out` is non-empty by construction; `nullopt` is exactly an alloc failure.
    const std::span<const std::byte> bytes = mem::as_span(out);
    view::segment_t* const seg =
        listing.graph->value_backend().alloc(bytes.size(), mem::alloc_hint_t::NONE);
    if (seg == nullptr) return std::unexpected(status_t::BACKPRESSURE);
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t{view::segment_ptr_t::adopt(seg), 0, bytes.size()};
}
}  // namespace

result_t<vertex_handle_t> transport_vertex_t::make_connection_locked(
    ctl_txn_t& txn, std::string_view module, std::string_view name, const tlv_node_t* config,
    mem::bytes_t config_bytes, conn_settings_t settings) {
    const std::string_view root = net_root_.view().substr(1);
    // The routing key IS the mount path (ADR-0061): `net/<module>/<name>`. Keeping the root
    // segment in the key means the registry's precomputed run is exactly the prefix a hop
    // prepends to `src`, so the forward path needs no per-hop assembly.
    mem::string_t qualified(*egress_src_);
    // The mount key `<net_root>/<module>/<name>` is built here too, so one refusal answers
    // both; the `/net/<module>` vertex's key is its prefix.
    mem::bytes_t mount_key(*egress_src_);
    if (!join_into(qualified, {root, module, name}) ||
        !put_segments(mount_key, {root, module, name}))
        return std::unexpected(status_t::BACKPRESSURE);
    // The key's bytes do not move when the string is moved into the table below, so this view
    // names the connection for the rest of the call.
    const std::string_view qv = qualified.view();
    // With the module resolved, the staging is a DIRECT lookup: `pending_links_` is keyed by
    // `<module>/<name>` — exactly the qualified key past its root (see `provide_link`). No
    // scan, and no way to reach a key whose module half is not the one this connection
    // mounts under.
    const std::string_view staged_key = qv.substr(root.size() + 1);
    if (conns_.contains(qv)) return std::unexpected(status_t::PATH_IN_USE);

    // The #373 first-level shadow guard is GONE, and can be: it existed because a connection
    // NAME was the first `dst` segment, so a link sharing a name with a first-level vertex
    // black-holed every `/name/...` read onto the transport. A routable connection is now
    // addressed `/net/<module>/<name>`, so a first-level vertex cannot shadow one — and
    // keeping the guard would instead wrongly reject a connection merely named after an
    // unrelated local vertex.

    // Compose the mount key: `<net_root>/<module>/<name>`, replacing the flat key the
    // graph's `:children[]` machinery used to hand the retired door (the endpoint never had one).
    // The `/net/<module>` structural vertex, created lazily on first use. The registration IS
    // the dedupe: a second one answers PATH_IN_USE and changes nothing, so its result is
    // dropped — a separate seen-set, or a `find` first, would be a second source of truth for
    // something the graph already knows. Its key is the mount key's first two records, so it
    // is read off that key's prefix rather than built twice.
    (void)graph_.register_vertex_key(parent_key(mount_key, name), graph::role_t::STORED_VALUE, {});

    // Resolve the connection's link. Precedence, WITHIN the module resolved above: a
    // provide_link-staged transport wins (the test/manual seam); otherwise the config `kind`
    // selects a factory and the real socket is CONSTRUCTED here and owned by the connection.
    // A staging under a DIFFERENT module is a different connection and is not considered.
    transport_t* link = nullptr;
    transport_ptr_t owned;
    self_heal_link_t* engine = nullptr;
    transport_t* const* const pl = pending_links_.find(staged_key);
    if (pl != nullptr) {
        link = *pl;
    } else if (!settings.kind.empty()) {
        const kind_entry_t* const factory = transport_types_.find(settings.kind);
        // An unregistered kind is an unsupported catalog entry — same convention as an
        // unknown SPEC `type` (SCHEMA_NOT_FOUND, the ENOTTY of creation).
        if (factory == nullptr) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        // The one mint site of the S5 engine, behind the #1470 module gate. On a
        // `kSelfHealLinks = false` build the whole arm is DISCARDED — `self_heal_link_t` is
        // never named, so the linker never pulls its TU in — and no kind can reach it
        // anyway, because `register_transport_type` refuses to catalogue a `self_heal_dial`
        // kind on such a build. The eager arm below then serves every registered kind.
        if constexpr (kSelfHealLinks) {
            if (factory->traits.self_heal_dial && settings.role == conn_role_t::DIAL) {
                // The RFC-0014 §4 S5 engine (#492): creation constructs NO socket — the vertex
                // is minted DORMANT and the engine dials on demand / self-heals under its own
                // worker. The factory therefore does not run here, so the universal DIAL keys
                // it would have validated are gated NOW (the same predicate `dial_or_listen`
                // applies): creation must refuse a misconfigured SPEC at the write, never
                // defer it to a first dial that answers success today and failure later.
                // Kind-PRIVATE config that the module's declared catalog describes was
                // already checked at the write (`conforms`, RFC-0014 §2 / Amendment 3); what
                // a catalog cannot express — a value's range, a key's meaning — stays the
                // factory's to refuse, at dial time.
                if (settings.addr.empty() || settings.port == 0)
                    return std::unexpected(status_t::TYPE_MISMATCH);
                // The engine re-reads the raw config on every dial from the connection's own
                // copy, which outlives it (`ctl_txn_t::destroy_link` drops the copy after the
                // engine); its settings' views point into the same copy.
                mem::poly_ptr_t<self_heal_link_t> heal = mem::make_poly<self_heal_link_t>(
                    *egress_src_, factory->factory, settings, mem::as_span(config_bytes),
                    factory->traits.delivers_ropes, *egress_src_);
                if (!heal) return std::unexpected(status_t::BACKPRESSURE);
                engine = heal.get();
                owned = std::move(heal);
                link = owned.get();
            }
        }
        // The eager arm — every kind on a stock build, and EVERY kind on a build that closed
        // the engine out. Keyed on `link` rather than written as the `else` of the arm above,
        // because an `else` attached to a discarded `if constexpr` body would be discarded
        // with it and a closed-out build would construct nothing at all.
        if (link == nullptr) {
            // The raw config TLV rides along so the kind's factory can parse its own
            // kind-private keys (ADR-0043 §5 leanness: they never land in settings).
            auto built = factory->factory(settings, config, *egress_src_);
            if (!built) return std::unexpected(built.error());
            owned = std::move(*built);
            link = owned.get();
        }
    } else {
        // Neither a staged link nor a construction kind — nothing can carry the bytes.
        // Same missing-required-`kind` refusal as the module-resolution gate above (#1062).
        return std::unexpected(status_t::TYPE_MISMATCH);
    }

    // A BUS link (ADR-0044) serves its currently-audible peers as this vertex's
    // synthesized `:children[]` — a POINT of POINT{NAME <peer>} members built on
    // every read from the transport's live-traffic table. NO vertex is ever
    // created for a peer. Every listed name is also a routable next-hop segment
    // (the registry's peer fallback): CAN's names always were legal segments, and
    // the ws/tcp bus servers now name accepted sessions `p<slot>` (#426, ADR-0073
    // §2) instead of the unaddressable `<ip>:<port>`, so the ADR-0044 promise holds
    // per the enumerable⇒addressable invariant (ADR-0073 §1). Routing a `dst`
    // through a bus link's own connection NAME remains the sharp edge:
    // `transport_ws_server::send` BROADCASTS, so one request draws one reply per
    // peer — rejecting that hop is #741. See reference/14 §Forwarding. Kind-neutral in the one
    // sense that matters here: any transport whose
    // bus() is non-null gets this wiring; point-to-point links keep the plain
    // vertex. The captured facet lives exactly as long as the link (the class's
    // documented lifetime contract — the graph must not outlive this object).
    graph::handlers_t handlers;
    // Asked through `bus_of` (#375 deliverable 3): on a target that closed the bus module out
    // every connection vertex is the plain one, and the synthesis below — with the TLV
    // emission it performs — is never compiled. The hook's `ctx` pairs the bus facet, which
    // lives exactly as long as the link, with the graph whose sources the listing draws
    // (#2052). The hook owns nothing (RFC-0028 D10): the connection owns the pair, and phase 2
    // frees it after the retire, next to the link.
    mem::block_ptr_t<detail_bus::listing_t> listing;
    if (bus_link_t* const bus = bus_of(*link)) {
        listing = mem::make_block<detail_bus::listing_t>(*egress_src_, bus, &graph_);
        if (!listing) return std::unexpected(status_t::BACKPRESSURE);
        handlers.on_children = {&bus_children, listing.get()};
    }

    // Register the identity vertex at the composed /net/<name> key (graph owns addressing).
    // On failure the just-constructed socket (if any) is torn down by `owned`'s destructor.
    result_t<vertex_handle_t> v =
        graph_.register_vertex_key(mem::as_span(mount_key), graph::role_t::STORED_VALUE, handlers);
    if (!v) return v;  // PATH_IN_USE on a duplicate connection name

    // The engine is the sole writer of this connection's DIAL transitions (RFC-0014 §4):
    // its publisher writes the vertex VALUE directly — deliberately NOT through
    // set_link_state, whose ctl_m_ the engine's worker must never take (the teardown
    // path joins that worker while HOLDING ctl_m_). remove_connection stops the engine
    // before the vertex retires, so no publish lands on a retired vertex.
    if constexpr (kSelfHealLinks) {  // #1470 module gate — see `ctl_txn_t::discharge`
        if (engine != nullptr) {
            graph::graph_t* const g = &graph_;
            const vertex_handle_t vh = *v;
            engine->set_liveness_publisher([g, vh](link_state_t s) {
                (void)g->write(vh, link_state_value(s, g->value_backend()));
            });
        }
    }

    const bool constructed = owned && engine == nullptr;
    const conn_role_t effective_role = settings.role;
    conn_t made{.vertex = *v,
                .config = std::move(config_bytes),
                .settings = settings,
                .owned = std::move(owned),
                .engine = engine,
                .listing = std::move(listing)};
    conn_t* const conn = conns_.try_emplace(std::move(qualified), std::move(made)).value;
    if (conn == nullptr) {
        // The table could not grow: the rollback a refused `add_child` takes below, minus the
        // table entry. A refused emplace moves nothing out of its arguments, so `made` still
        // owns the socket, which goes to phase 2 like every other join.
        (void)graph_.retire(*v);
        txn.destroy_link(std::move(made.owned), std::move(made.config), std::move(made.listing));
        return std::unexpected(status_t::BACKPRESSURE);
    }

    // Wire the link into the router's child_registry_t — the single owner of the
    // NAME→link demux table (Brick 3a). The `/net/<name>` NAME is exactly the router
    // child name a `dst` routes through.
    //
    // Its `bool` is the WHOLE creation's verdict (#930), not advice. `add_child` answers
    // false when the registry could not grow, and it is the only place that can tell anyone
    // so — the refusal is TOTAL there, so nothing is registered and no receiver is wired.
    // Discarding it published the vertex anyway: a connection reporting UP liveness that no
    // `dst` resolves, no inbound frame lands on, and no `remove_child` can take down. Peer-
    // drivable on a bounded node, by creating connections until the slab exhausts.
    //
    // Roll the creation back in the reverse order it was built — the same order
    // `remove_connection` uses, and for the same reason: retire the identity vertex FIRST so
    // the address stops resolving (and its `:children[]` seam stops naming the link) before
    // erasing the `conns_` entry, which destroys the config-constructed socket. There is
    // nothing to un-route: the registry holds no entry to remove. BACKPRESSURE is the
    // exhausted-resource status the rest of the failable seam answers with (ADR-0065), so a
    // wiring refusal surfaces as an error the peer can retry rather than as a live-looking
    // dead connection.
    //
    // The connection's catalog `(kind, role)` rides along (#1650): the router interns it once,
    // so every write this link carries tells the target's admission filter which kind of
    // session it came from. The pair is the module's own declaration, held nowhere new.
    // `qv` views the table key, a `string_t`, which keeps a terminator: its `data()` is the C
    // string the router's (owning) name parameter is built from.
    if (!router_.add_child(qv.data(), *link, nullptr,
                           link_kind_t{.kind = conn->settings.kind, .role = effective_role})) {
        (void)graph_.retire(*v);
        // The config-constructed socket's destructor JOINS its receive thread, so it is
        // handed to phase 2 like every other join (S6, #492) instead of running here under
        // `ctl_m_`; the map entry itself goes now, so nothing observes a half-built
        // connection once the lock drops.
        txn.destroy_link(std::move(conn->owned), std::move(conn->config), std::move(conn->listing));
        conns_.erase(qv);
        return std::unexpected(status_t::BACKPRESSURE);
    }
    // The staged link is CONSUMED only once the connection is fully wired. Erasing it before
    // the registry call would make the rollback above lossy: the caller's provide_link
    // staging would be gone, so a retry once the pressure clears would no longer find its
    // link and would fail NOT_FOUND instead of succeeding. (Unconditional: with nothing
    // staged under this key, the erase finds nothing.)
    (void)pending_links_.erase(staged_key);
    // LAST WIRING STEP (#1025): the link may now deliver. Everything an inbound frame needs
    // is in place — the registry entry is published and `add_child` has installed the
    // receiver and the down-notifier — so this is the first instant at which a decoded frame
    // has somewhere to land. A transport that started its receive thread in its own
    // constructor takes the base's no-op default; one that can defer it (the built-in `ws`
    // DIAL, which the factory constructs with `defer_recv`) spawns it here, which is what
    // stops a server's push-on-connect message being decoded into an empty sink and dropped.
    // Unconditional by design: the owner should not have to know which kinds defer.
    link->start_receiving();
    // A config-constructed socket is live once built: publish its liveness so an awaiter
    // on /net/<name> sees the bring-up. A DIAL socket is `UP`; a LISTEN socket that bound
    // is `LISTENING` (a bind failure returns an error from the factory above, so a
    // constructed LISTEN is always bound). Provided links report via set_link_state.
    //
    // The role read here is the EFFECTIVE one — the same field the factory was handed. Since
    // S7 that is always the endpoint module's declared role and nothing else can move it; the
    // read is kept honest anyway, because publishing anything but what the factory acted on is
    // how a `client`'s `UP` once landed over a socket the factory had just BOUND as a listener.
    if (constructed)
        (void)set_link_state_locked(
            txn, qv,
            effective_role == conn_role_t::LISTEN ? link_state_t::LISTENING : link_state_t::UP);
    // An engine-managed connection is born RESTING (RFC-0014 §4: vertex exists, no
    // socket, refcount 0) — the one initial publish the engine's worker does not own.
    // COLLECTED here, before any op can reach the link, and written by phase 2 once
    // `ctl_m_` is down: a birth publish fans out like any other (S6, #492).
    if (engine != nullptr) (void)set_link_state_locked(txn, qv, link_state_t::DORMANT);
    return v;
}

result_t<void> transport_vertex_t::remove_connection(std::string_view name) {
    // The PUBLIC entry opens the transaction (#881); the RFC-0014 `NAME`-write dispatch
    // already has one open — `ctl_m_` is a plain, NON-RECURSIVE std::mutex — so it calls
    // the body directly with its own.
    ctl_txn_t txn(*this, ctl_scope_t::OPERATION);  // ADR-0063 §3 serialization
    const result_t<void> removed = remove_connection_locked(txn, name);
    if (!removed) return removed;
    return txn.discharge();
}

/** @brief `remove_connection`'s body, for a caller that already holds `ctl_m_`. */
result_t<void> transport_vertex_t::remove_connection_locked(ctl_txn_t& txn, std::string_view name) {
    const std::size_t at = conns_.lower_bound(name);
    if (at == conns_.size() || conns_.at(at).key != name)
        return std::unexpected(status_t::NOT_FOUND);
    auto& entry = conns_.at(at);
    // Everything below is COLLECTED, in the order #494 fixed and phase 2 replays:
    //
    //  1. Un-route BEFORE anything is destroyed: after this the NAME resolves to nothing,
    //     so the socket can go without a forward ever reaching freed memory.
    //  2. Stop the S5 engine BEFORE the vertex retires: its worker publishes liveness by
    //     writing that vertex. `stop()` JOINS the worker, and the worker may be inside a
    //     publish whose fan-out reaches a subscriber that calls back in here (S6's own
    //     wiring does exactly that) — which is why it must not run under `ctl_m_`.
    //  3. Retire the identity vertex (RFC-0009 §B.6): /net/<name> re-virginizes, so a later
    //     connection may take the same name — which is exactly the tombstone the registry
    //     reuses. Retiring an already-retired or unregistered vertex is a no-op, so a
    //     half-built connection tears down cleanly too.
    //  4. Destroy the owned socket, which joins its recv thread — the second join, and the
    //     second reason phase 2 exists. A provided link is borrowed and left untouched.
    //
    // #576: step 3 is the peer-driven append site of the value-seam park — but only for a
    // BUS link. The identity vertex bears a value seam iff it was given one at creation, and
    // that happens only when `link->bus() != nullptr` (CAN; a tcp/ws server wired
    // `peer_named = true`). Tearing down a point-to-point connection — every dial link, UDP,
    // loopback, a default-wired server — parks NOTHING, so a default deployment never needs
    // a collect() point. A bus node parks one ~96 B value_handlers_t per teardown, which is
    // the case #576 exists for. We do NOT collect here even then: this runs on whatever
    // thread the teardown arrived on, which is precisely the free location graph_t::collect()
    // exists to take out of the library's hands. The embedder calls collect() where it knows
    // no reader holds a seam.
    // The routing name is handed over with the entry's own key: the entry goes below, the
    // un-route happens in phase 2, and nothing is copied to bridge the two.
    txn.unroute(std::move(entry.key));
    txn.stop_engine(entry.value.engine);
    txn.retire(entry.value.vertex);
    txn.destroy_link(std::move(entry.value.owned), std::move(entry.value.config),
                     std::move(entry.value.listing));
    // The map entry goes NOW, under the lock, while the identity vertex is still registered
    // — so a same-name creation racing this teardown is refused `PATH_IN_USE` by
    // `register_vertex_key` until phase 2's retire lands, and by then phase 2's un-route has
    // landed too. There is no window in which two connections own one routing NAME.
    conns_.erase_at(at);
    return {};
}

result_t<void> transport_vertex_t::set_link_state(std::string_view name, link_state_t state) {
    // The PUBLIC entry opens the transaction (#881). This is the liveness door a TRANSPORT
    // thread knocks on for a provided link, while create/remove is wire-driven on a receive
    // thread — so the unguarded find here walked `conns_` mid-rebalance and could return a
    // node `remove_connection` was erasing. `make_connection_locked` collects creation liveness on
    // its own transaction via `set_link_state_locked`, which is why the fix is a split:
    // `ctl_m_` is non-recursive, so it cannot re-enter through this wrapper. `OPERATION`
    // scope: the resolution and the write it defers are ONE step against a concurrent
    // teardown of the same connection, which is what stops the write landing on a vertex
    // mid-retire (see `ops_m_`).
    ctl_txn_t txn(*this, ctl_scope_t::OPERATION);  // ADR-0063 §3 serialization
    const result_t<void> resolved = set_link_state_locked(txn, name, state);
    if (!resolved) return resolved;
    // The verdict IS the write's, and the write is phase 2's.
    return txn.discharge();
}

/** @brief `set_link_state`'s body, for a caller that already holds `ctl_m_`. */
result_t<void> transport_vertex_t::set_link_state_locked(ctl_txn_t& txn, std::string_view name,
                                                         link_state_t state) {
    conn_t* const it = conns_.find(name);
    if (it == nullptr) return std::unexpected(status_t::NOT_FOUND);
    // Resolution is all that happens under the lock. The write itself bumps write_seq_ and
    // DELIVERS to subscribers (RFC-0008 §D) — so await(/net/<name>) fires and a subscribe
    // streams the transition — and a routing-plane subscriber of this very connection's
    // liveness drives `acquire_link`/`release_link` (RFC-0014 §4's standing-binding seam),
    // straight back into `ctl_m_`. Publishing from here re-entered a non-recursive mutex on
    // its own thread; the collected write runs in phase 2 with the lock down (S6, #492).
    txn.publish(it->vertex, state);
    return {};
}

result_t<void> transport_vertex_t::acquire_link(std::string_view name) {
    const ctl_txn_t txn(*this);  // ADR-0063 §3 control-plane serialization
    conn_t* const it = conns_.find(name);
    if (it == nullptr) return std::unexpected(status_t::NOT_FOUND);
    // Lock order: ctl_m_ → the engine's own mutex; the engine never takes ctl_m_ back, and
    // neither `acquire` nor `release` joins a thread or dispatches — they flip the refcount
    // and kick the worker — so they stay in phase 1 where the `conns_` lookup already is.
    // A connection without an engine answers success as a no-op (see the header: a
    // LISTEN ignores refcount per RFC-0014 §4, and a manual link's liveness is manual).
    if constexpr (kSelfHealLinks)  // #1470 module gate
        if (it->engine != nullptr) it->engine->acquire();
    return {};
}

result_t<void> transport_vertex_t::release_link(std::string_view name) {
    const ctl_txn_t txn(*this);  // ADR-0063 §3 control-plane serialization
    conn_t* const it = conns_.find(name);
    if (it == nullptr) return std::unexpected(status_t::NOT_FOUND);
    if constexpr (kSelfHealLinks)  // #1470 module gate
        if (it->engine != nullptr) it->engine->release();
    return {};
}

const conn_settings_t* transport_vertex_t::settings_of(std::string_view name) const {
    // ADR-0063 §3 — readers of conns_ race the insert's rebalance
    const ctl_txn_t txn(*this);
    const conn_t* const it = conns_.find(name);
    return it == nullptr ? nullptr : &it->settings;
}

transport_t* transport_vertex_t::link_of(std::string_view name) const {
    // ADR-0063 §3 — readers of conns_ race the insert's rebalance
    const ctl_txn_t txn(*this);
    const conn_t* const it = conns_.find(name);
    return it == nullptr ? nullptr : it->owned.get();
}

}  // namespace tr::net
