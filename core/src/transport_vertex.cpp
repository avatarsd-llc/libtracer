/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/transport_vertex.hpp"

#include <cassert>
#include <cstring>
#include <memory>
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
using wire::tlv_t;
using wire::type_t;

namespace {

/**
 * @brief Parse the optional SPEC `config` SETTINGS (the shared config_reader_t walk): NAME "addr"
 *        NAME <utf8>, NAME "kind" NAME <utf8>, NAME "port" VALUE u16, NAME "keepalive" VALUE u32.
 *
 * There is no `role` key. The role is POSITIONAL — it IS the module (RFC-0014 §1/§3) — and
 * the only key that ever carried it was the superseded `:children[]` spelling's override,
 * retired with that door at S7. A `role` pair on the wire is now an unknown pair: ignored,
 * like every other unknown pair, never obeyed.
 *
 * ONLY the universal
 * keys land here (ADR-0043 §5 leanness): kind-private pairs (e.g. quic's `cert`/`key`)
 * are the kind's factory's business — it parses them from the raw config TLV it
 * receives. Unknown pairs are ignored (forward-compat).
 */
void parse_config(const tlv_t* config, conn_settings_t& s) {
    const config_reader_t cfg(config);
    if (const auto v = cfg.name("addr")) s.addr = std::string(*v);
    if (const auto v = cfg.name("kind")) s.kind = std::string(*v);
    if (const auto v = cfg.u16("port")) {
        s.port = *v;
        s.port_set = true;
    }
    if (const auto v = cfg.u32("keepalive")) s.keepalive_ms = *v;
    if (const auto v = cfg.u32("max_frame")) s.max_frame = *v;
    if (const auto v = cfg.u32("backoff")) s.backoff_ms = *v;
    if (const auto v = cfg.u32("connect_timeout")) s.connect_timeout_ms = *v;
}

/**
 * @brief Re-encode a decoded TLV to owned wire bytes — the S5 engine's copy of the SPEC's
 *        `config` SETTINGS, taken because the decoded tlv_t BORROWS the write's rope,
 *        which is gone by the first re-dial. Trailers are not re-emitted (a config
 *        SETTINGS never carries one; the reader would ignore it anyway).
 */
void reemit_tlv(std::vector<std::byte>& out, const tlv_t& tlv) {
    if (tlv.opt.pl) {
        std::vector<std::byte> body;
        for (const tlv_t& child : tlv.children) reemit_tlv(body, child);
        wire::emit_tlv(out, tlv.type, tlv.opt, body);
        return;
    }
    wire::emit_tlv(out, tlv.type, tlv.opt, tlv.payload);
}

/** @brief A 1-byte link-liveness VALUE TLV (link_state_t) as an owned view. */
[[nodiscard]] view_t link_state_value(link_state_t state) {
    std::vector<std::byte> out;
    const std::byte b{static_cast<std::uint8_t>(state)};
    wire::emit_tlv(out, type_t::VALUE, wire::opt_t{}, std::span<const std::byte>(&b, 1));
    return view::over_bytes(out).value_or(
        view_t{});  // empty view on alloc failure (caller-checked)
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

void transport_vertex_t::ctl_txn_t::unroute(std::string name) { unroute_ = std::move(name); }

void transport_vertex_t::ctl_txn_t::stop_engine(self_heal_link_t* engine) { stop_ = engine; }

void transport_vertex_t::ctl_txn_t::destroy_link(std::unique_ptr<transport_t> link) {
    destroy_ = std::move(link);
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
        (void)owner_.router_.remove_child(unroute_);
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
    if (publish_) {
        const vertex_handle_t vertex = *publish_;
        publish_.reset();
        // The fan-out: `write` delivers to this connection's subscribers, and a
        // routing-plane subscriber drives acquire_link/release_link straight back here.
        out = owner_.graph_.write(vertex, link_state_value(publish_state_));
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
                                       std::string net_root, mem::mem_backend_t* rx_backend,
                                       slim_net_t, mem::block_source_t* egress_src)
    : graph_(graph),
      router_(router),
      net_root_(std::move(net_root)),
      rx_backend_(rx_backend),
      // The nullptr guard the FULL ctor used to hold, MOVED here (#873): both ctors reach
      // this one line, so a null argument still means the process heap and behaviour is
      // bit-identical for every existing caller. Before this parameter a SLIM node's
      // `egress_source()` answered the process heap unconditionally — it could not be told
      // otherwise, so the accessor lied about that node's store.
      egress_src_(egress_src != nullptr ? egress_src : &mem::heap_source()) {
    // Register the `<net_root>` grouping vertex if it isn't already. It is the ENUMERATION
    // root (`/net:children[]` lists this plane's modules) and nothing more: RFC-0014 S7
    // retired the `client`/`listener` CREATION registrations that used to hang off it, so a
    // `write /net:children[] += SPEC{type = "client"|"listener", …}` now answers
    // `SCHEMA_NOT_FOUND` like any other unregistered catalog type. The ONE wire creation door
    // for a connection is the per-module creator endpoint `<net_root>/<module>/conn`
    // (`register_module` mints it; RFC-0014 §1/§2).
    if (!graph_.find(path_t::parse(net_root_)->key())) {
        (void)graph_.register_vertex(*path_t::parse(net_root_), graph::role_t::STORED_VALUE);
    }
    // The `quic` kind is NOT a builtin: it lives in the separate libtracer_quic
    // module (ADR-0043), which extends this catalog through register_transport_type
    // (quic_transport_factory) — this file never learns about msquic (open/closed).
    // A slim node likewise registers whatever factories it wants after construction.
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
                                       std::string net_root, mem::mem_backend_t* rx_backend,
                                       mem::block_source_t* egress_src)
    : transport_vertex_t(graph, router, std::move(net_root), rx_backend, slim_net, egress_src) {
    // The ADR-0079 net-plane egress store (#873 family 1) is now set by the delegated ctor —
    // the nullptr guard lives there, so a SLIM node gets the same treatment this one does
    // and `egress_source()` answers for both. All that is left here is the builtin catalog.
    register_builtin_transports(*this, rx_backend_, egress_src_);
}

void transport_vertex_t::register_transport_type(std::string kind, transport_factory_t factory) {
    // Default traits: eager construction, exactly as every pre-S5 registration behaved.
    register_transport_type(std::move(kind), std::move(factory), transport_kind_traits_t{});
}

void transport_vertex_t::register_transport_type(std::string kind, transport_factory_t factory,
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
    transport_types_.insert_or_assign(std::move(kind), kind_entry_t{std::move(factory), traits});
}

result_t<void> transport_vertex_t::register_module(std::string module, std::string kind,
                                                   conn_role_t role) {
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
    if (auto minted = mint_module_locked(module); !minted) return minted;
    if (!declared) modules_.push_back({std::move(module), std::move(kind), role});
    return {};
}

result_t<std::string> transport_vertex_t::module_for(std::string_view kind,
                                                     conn_role_t role) const {
    // The PUBLIC entry locks (#881); a creation running inside its own locked section —
    // `endpoint_create_locked` and the `*_locked` family below it, since S7 retired the
    // `:children[]` door — already holds `ctl_m_`, a plain NON-RECURSIVE std::mutex
    // (ADR-0063 erratum 1), so such a caller reaches the body directly. The fix is a split
    // precisely because a lock added in place would self-deadlock there.
    const ctl_txn_t txn(*this);  // ADR-0063 §3 control-plane serialization
    return module_for_locked(kind, role);
}

/** @brief `module_for`'s body, for a caller that already holds `ctl_m_`. */
result_t<std::string> transport_vertex_t::module_for_locked(std::string_view kind,
                                                            conn_role_t role) const {
    for (const module_decl_t& d : modules_) {
        if (d.kind == kind && d.role == role) return d.module;
    }
    // Declared-only (ADR-0073 §4): no derived "<kind>-client"/"<kind>-server" fallback —
    // an undeclared (kind, role) is an unsupported catalog entry, the same convention as
    // an unknown SPEC `type`. The application mints every module segment.
    return std::unexpected(status_t::SCHEMA_NOT_FOUND);
}

/** @brief The declaration an endpoint write resolves its (kind, role) through. */
result_t<transport_vertex_t::module_decl_t> transport_vertex_t::declaration_for_locked(
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
    return *found;
}

result_t<void> transport_vertex_t::mint_module_locked(const std::string& module) {
    // The `<net_root>/<module>` grouping vertex. graph_.find IS the dedupe — the same rule
    // `make_connection_locked`'s lazy mint follows, and for the same reason: a separate
    // seen-set would be a second source of truth for something the graph already knows.
    std::vector<std::byte> mod_key;
    (void)wire::emit_path_segment(mod_key, std::string_view(net_root_).substr(1));
    (void)wire::emit_path_segment(mod_key, module);
    if (!graph_.find(mod_key)) {
        auto mod = graph_.register_vertex_key(mod_key, graph::role_t::STORED_VALUE, {});
        if (!mod) return std::unexpected(mod.error());
    }

    std::vector<std::byte> endpoint_key = mod_key;
    (void)wire::emit_path_segment(endpoint_key, kConnEndpointName);
    if (graph_.find(endpoint_key)) return {};  // a second kind under the same module

    // `role_t::HANDLER` is what makes the endpoint WRITE-ONLY AND VALUELESS (RFC-0014 §2):
    // the graph runs `on_write` and stores no last-known-value, so the write is EXECUTED,
    // never assigned. No `on_read` is installed either — a whole-vertex read answers
    // NOT_FOUND, and the endpoint's one readable facet is `:schema`, which the graph's own
    // field door already serves as the RFC-0014 Amendment 3 catalog envelope:
    // `POINT{NAME "conn", SETTINGS{…}}`, with an EMPTY `SETTINGS` for a module that declares
    // no catalog. That empty answer is conforming, not a stub — the probe of §6 asks whether
    // the endpoint EXISTS, and `SCHEMA_NOT_FOUND` would answer a question nobody asked.
    //
    // The lambda captures the module by VALUE. The path is the module, so the dispatch never
    // re-derives it from a payload the peer wrote — a creator cannot address one module's
    // endpoint and have the connection mount under another.
    graph::handlers_t handlers;
    handlers.on_write = [this, module](const view::rope_t& value,
                                       const graph::write_ctx_t&) -> result_t<void> {
        return endpoint_write(module, value);
    };
    // RFC-0014 §5, discharged by the Amendment 2 general contract: the two control payloads
    // this endpoint accepts demand DIFFERENT rights, so the endpoint declares them rather
    // than having `graph_t` learn a transport concept. `SPEC` (create) demands `CREATE`, so
    // the create right is delegable on the endpoint's own ACL without any right on the parent
    // transport; `NAME` (remove) demands `WRITE`, per RFC-0009 §A.2's reserved-and-unused
    // `DELETE`. A peer may hold either without the other. Anything else written here takes the
    // default `WRITE` and is refused by `endpoint_write` on its shape (§2), not by the gate.
    handlers.payload_rights = {
        graph::payload_right_t{wire::type_t::SPEC, graph::acl_right_t::CREATE},
        graph::payload_right_t{wire::type_t::NAME, graph::acl_right_t::WRITE},
    };
    auto endpoint = graph_.register_vertex_key(std::move(endpoint_key), graph::role_t::HANDLER,
                                               std::move(handlers));
    if (!endpoint) return std::unexpected(endpoint.error());
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

result_t<void> transport_vertex_t::endpoint_write(const std::string& module,
                                                  const view::rope_t& value) {
    // A DEVICE-link payload is permanently un-parsable on the CPU (ADR-0024), so it is a
    // malformed control write rather than a transient one — the same classification
    // `graph_t::write`'s field surface makes.
    if (!value.all_host()) return std::unexpected(status_t::TYPE_MISMATCH);
    mem::mem_backend_t& backend = rx_backend_ != nullptr ? *rx_backend_ : mem::heap_backend();
    // Single-link (every ordinary control write) is zero-copy; a rope that straddled links
    // pays one flatten, and an exhausted backend surfaces as TRANSIENT backpressure rather
    // than being read back as a truncated — and therefore "malformed" — SPEC (#917).
    const auto flat = value.try_materialize(backend);
    if (!flat) return std::unexpected(status_t::BACKPRESSURE);
    const auto payload = wire::decode(*flat);
    if (!payload) return std::unexpected(status_t::TYPE_MISMATCH);

    // ONE critical section for the whole DECISION: parse, declaration lookup, socket
    // construction and routing all happen under `ctl_m_`, so two peers writing the same
    // endpoint cannot interleave into a half-built connection. Head of the declared lock
    // order (this -> fwd_router_t -> graph_t -> the vertex stripe), and the graph holds none
    // of its own locks across `on_write`, so nothing here descends against that order.
    // What the decision does NOT do is fan out or join: those are phase 2's (S6, #492).
    ctl_txn_t txn(*this, ctl_scope_t::OPERATION);  // ADR-0063 §3 serialization
    switch (payload->type) {
        case type_t::SPEC: {
            const result_t<void> made = endpoint_create_locked(txn, module, *payload);
            // A creation's BIRTH-liveness publish is not the creation's verdict — the
            // connection exists either way — so its status is dropped here exactly as it
            // was dropped at the `(void) set_link_state_locked` site it moved from.
            (void)txn.discharge();
            return made;
        }
        case type_t::NAME: {
            const result_t<void> removed =
                endpoint_remove_locked(txn, module, detail::as_string_view(payload->payload));
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

result_t<void> transport_vertex_t::endpoint_create_locked(ctl_txn_t& txn, const std::string& module,
                                                          const tlv_t& spec) {
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

    const tlv_t* config = pairs.settings("config");
    conn_settings_t settings;
    parse_config(config, settings);

    const auto declared = declaration_for_locked(module, settings.kind);
    if (!declared) return std::unexpected(declared.error());
    // The role is POSITIONAL — it IS the module (RFC-0014 §1/§3) — so the declaration SETS it.
    // Nothing on the wire can say otherwise: the `role` config pair died with the superseded
    // `:children[]` spelling it belonged to (S7), and `parse_config` no longer reads one, so a
    // creator cannot mount a LISTEN socket under a module whose path promises DIAL.
    settings.role = declared->role;
    // A kind-less SPEC is the staged-link spelling; the module's declared kind is the kind
    // this endpoint constructs, so recording it keeps `settings_of` honest about what the
    // connection is. `make_connection_locked` still prefers a `provide_link` staging over
    // the factory, so filling this in does not change WHICH link is used.
    if (settings.kind.empty()) settings.kind = declared->kind;

    const auto made =
        make_connection_locked(txn, module, std::string(name), config, std::move(settings));
    if (!made) return std::unexpected(made.error());
    // The endpoint is valueless: the handle the creation produced is the connection's, not
    // this vertex's, and it is deliberately not published anywhere the write can return it.
    return {};
}

result_t<void> transport_vertex_t::endpoint_remove_locked(ctl_txn_t& txn, const std::string& module,
                                                          std::string_view name) {
    // An empty NAME names nothing and is not an "absent" connection — it is a malformed
    // control payload, so it is refused rather than swallowed as a no-op success.
    if (name.empty()) return std::unexpected(status_t::TYPE_MISMATCH);
    // Remove-side reserve (RFC-0014 §2/§3): the endpoint cannot self-destruct. Refused
    // before the lookup, so it never reaches `retire()`.
    if (name == kConnEndpointName) return std::unexpected(status_t::PERMISSION_DENIED);

    std::string qualified(std::string_view(net_root_).substr(1));
    qualified.push_back('/');
    qualified += module;
    qualified.push_back('/');
    qualified += name;
    // An unresolvable name — never created, or already removed — is a NO-OP SUCCESS at this
    // layer (RFC-0014 §2). `retire()`'s own idempotence only covers an already-resolved
    // handle, so the endpoint owns this leg: a retried remove after a dropped reply must
    // answer the same as the first one, or teardown is not retry-safe either.
    if (!conns_.contains(qualified)) return {};
    return remove_connection_locked(txn, qualified);
}

bool transport_vertex_t::is_structural(wire::key_view_t key) const {
    if (key.empty()) return false;  // the graph root is nobody's structural vertex
    // The net root is emitted as ONE NAME segment everywhere in this file
    // (`make_connection_locked` composes the mount key the same way), so the whole
    // predicate is two segment compares
    // over the key bytes — no key is materialised and nothing is allocated.
    const std::string_view root = std::string_view(net_root_).substr(1);
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
    std::string under(leaf);
    under.push_back('/');
    for (const auto& [staged_key, staged] : pending_links_) {
        (void)staged;
        if (staged_key.starts_with(under)) return true;  // key is `<module>/<name>`
    }
    std::string qualified_under(root);
    qualified_under.push_back('/');
    qualified_under += under;
    for (const auto& [qualified, conn] : conns_) {
        (void)conn;
        if (qualified.starts_with(qualified_under)) return true;  // `<root>/<module>/<name>`
    }
    return false;
}

void transport_vertex_t::provide_link(std::string module, std::string name, transport_t& link) {
    const ctl_txn_t txn(*this, ctl_scope_t::OPERATION);  // ADR-0063 §3 serialization
    std::string key = std::move(module);
    key.push_back('/');
    key += name;
    pending_links_.insert_or_assign(std::move(key), &link);
}

result_t<vertex_handle_t> transport_vertex_t::make_connection_locked(ctl_txn_t& txn,
                                                                     const std::string& module,
                                                                     const std::string& name,
                                                                     const tlv_t* config,
                                                                     conn_settings_t settings) {
    // With the module resolved, the staging is a DIRECT lookup: `pending_links_` is keyed by
    // exactly this string (see `provide_link`). No scan, and no way to reach a key whose
    // module half is not the one this connection mounts under.
    std::string staged_key = module;
    staged_key.push_back('/');
    staged_key += name;

    // The routing key IS the mount path (ADR-0061): `net/<module>/<name>`. Keeping the root
    // segment in the key means the registry's precomputed run is exactly the prefix a hop
    // prepends to `src`, so the forward path needs no per-hop assembly.
    std::string qualified(std::string_view(net_root_).substr(1));
    qualified.push_back('/');
    qualified += module;
    qualified.push_back('/');
    qualified += name;
    if (conns_.contains(qualified)) return std::unexpected(status_t::PATH_IN_USE);

    // The #373 first-level shadow guard is GONE, and can be: it existed because a connection
    // NAME was the first `dst` segment, so a link sharing a name with a first-level vertex
    // black-holed every `/name/...` read onto the transport. A routable connection is now
    // addressed `/net/<module>/<name>`, so a first-level vertex cannot shadow one — and
    // keeping the guard would instead wrongly reject a connection merely named after an
    // unrelated local vertex.

    // Compose the mount key: `<net_root>/<module>/<name>`, replacing the flat key the
    // graph's `:children[]` machinery used to hand the retired door (the endpoint never had one).
    std::vector<std::byte> mount_key;
    for (std::string_view seg : {std::string_view(net_root_).substr(1), std::string_view(module),
                                 std::string_view(name)}) {
        (void)wire::emit_path_segment(mount_key, seg);
    }

    // The `/net/<module>` structural vertex, created lazily on first use. graph_.find IS the
    // dedupe — a separate seen-set would be a second source of truth (and another container
    // instantiation) for something the graph already knows.
    {
        std::vector<std::byte> mod_key;
        (void)wire::emit_path_segment(mod_key, std::string_view(net_root_).substr(1));
        (void)wire::emit_path_segment(mod_key, module);
        if (!graph_.find(mod_key)) {
            (void)graph_.register_vertex_key(mod_key, graph::role_t::STORED_VALUE, {});
        }
    }

    // Resolve the connection's link. Precedence, WITHIN the module resolved above: a
    // provide_link-staged transport wins (the test/manual seam); otherwise the config `kind`
    // selects a factory and the real socket is CONSTRUCTED here and owned by the connection.
    // A staging under a DIFFERENT module is a different connection and is not considered.
    transport_t* link = nullptr;
    std::unique_ptr<transport_t> owned;
    self_heal_link_t* engine = nullptr;
    const auto pl = pending_links_.find(staged_key);
    if (pl != pending_links_.end()) {
        link = pl->second;
    } else if (!settings.kind.empty()) {
        const auto factory = transport_types_.find(settings.kind);
        // An unregistered kind is an unsupported catalog entry — same convention as an
        // unknown SPEC `type` (SCHEMA_NOT_FOUND, the ENOTTY of creation).
        if (factory == transport_types_.end()) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        // The one mint site of the S5 engine, behind the #1470 module gate. On a
        // `kSelfHealLinks = false` build the whole arm is DISCARDED — `self_heal_link_t` is
        // never named, so the linker never pulls its TU in — and no kind can reach it
        // anyway, because `register_transport_type` refuses to catalogue a `self_heal_dial`
        // kind on such a build. The eager arm below then serves every registered kind.
        if constexpr (kSelfHealLinks) {
            if (factory->second.traits.self_heal_dial && settings.role == conn_role_t::DIAL) {
                // The RFC-0014 §4 S5 engine (#492): creation constructs NO socket — the vertex
                // is minted DORMANT and the engine dials on demand / self-heals under its own
                // worker. The factory therefore does not run here, so the universal DIAL keys
                // it would have validated are gated NOW (the same predicate `dial_or_listen`
                // applies): creation must refuse a misconfigured SPEC at the write, never
                // defer it to a first dial that answers success today and failure later.
                // Kind-PRIVATE config stays the factory's to refuse, at dial time — the §2
                // catalog validation proper is S3's. Its ENVELOPE is ruled (RFC-0014
                // Amendment 3): the catalog is the `SETTINGS` of the endpoint's ordinary
                // `:schema` record, empty until a module declares one, so what is still open
                // here is the module-side declaration and the validation it would license,
                // never the reply shape.
                if (settings.addr.empty() || settings.port == 0)
                    return std::unexpected(status_t::TYPE_MISMATCH);
                // The engine owns a byte COPY of the raw config: the decoded TLV borrows the
                // write's rope, which is gone by the first re-dial.
                std::vector<std::byte> raw;
                if (config != nullptr) reemit_tlv(raw, *config);
                auto heal = std::make_unique<self_heal_link_t>(
                    factory->second.factory, settings, std::move(raw),
                    factory->second.traits.delivers_ropes);
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
            auto built = factory->second.factory(settings, config);
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
    // emission and the `std::function` it captures into — is never compiled.
    if (bus_link_t* const bus = bus_of(*link)) {
        handlers.on_children = [bus]() -> result_t<view_t> {
            std::vector<std::byte> members;
            bus->enumerate_peers([&members](std::string_view peer) {
                std::vector<std::byte> body;
                wire::emit_name(body, peer);
                wire::emit_tlv(members, type_t::POINT, wire::opt_t{.pl = true}, body);
            });
            std::vector<std::byte> out;
            wire::emit_tlv(out, type_t::POINT, wire::opt_t{.pl = true}, members);
            const auto res = view::over_bytes(out);
            if (!res) return std::unexpected(status_t::BACKPRESSURE);
            return *res;
        };
    }

    // Register the identity vertex at the composed /net/<name> key (graph owns addressing).
    // On failure the just-constructed socket (if any) is torn down by `owned`'s destructor.
    result_t<vertex_handle_t> v = graph_.register_vertex_key(
        std::move(mount_key), graph::role_t::STORED_VALUE, std::move(handlers));
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
            engine->set_liveness_publisher(
                [g, vh](link_state_t s) { (void)g->write(vh, link_state_value(s)); });
        }
    }

    const bool constructed = owned != nullptr && engine == nullptr;
    const conn_role_t effective_role = settings.role;
    const auto inserted = conns_.insert_or_assign(qualified, conn_t{.vertex = *v,
                                                                    .settings = std::move(settings),
                                                                    .owned = std::move(owned),
                                                                    .engine = engine});

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
    if (!router_.add_child(qualified, *link)) {
        (void)graph_.retire(*v);
        // The config-constructed socket's destructor JOINS its receive thread, so it is
        // handed to phase 2 like every other join (S6, #492) instead of running here under
        // `ctl_m_`; the map entry itself goes now, so nothing observes a half-built
        // connection once the lock drops.
        txn.destroy_link(std::move(inserted.first->second.owned));
        conns_.erase(inserted.first);
        return std::unexpected(status_t::BACKPRESSURE);
    }
    // The staged link is CONSUMED only once the connection is fully wired. Erasing it before
    // the registry call would make the rollback above lossy: the caller's provide_link
    // staging would be gone, so a retry once the pressure clears would no longer find its
    // link and would fail NOT_FOUND instead of succeeding.
    if (pl != pending_links_.end()) pending_links_.erase(pl);
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
            txn, qualified,
            effective_role == conn_role_t::LISTEN ? link_state_t::LISTENING : link_state_t::UP);
    // An engine-managed connection is born RESTING (RFC-0014 §4: vertex exists, no
    // socket, refcount 0) — the one initial publish the engine's worker does not own.
    // COLLECTED here, before any op can reach the link, and written by phase 2 once
    // `ctl_m_` is down: a birth publish fans out like any other (S6, #492).
    if (engine != nullptr) (void)set_link_state_locked(txn, qualified, link_state_t::DORMANT);
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
    const auto it = conns_.find(name);
    if (it == conns_.end()) return std::unexpected(status_t::NOT_FOUND);
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
    txn.unroute(std::string(name));
    txn.stop_engine(it->second.engine);
    txn.retire(it->second.vertex);
    txn.destroy_link(std::move(it->second.owned));
    // The map entry goes NOW, under the lock, while the identity vertex is still registered
    // — so a same-name creation racing this teardown is refused `PATH_IN_USE` by
    // `register_vertex_key` until phase 2's retire lands, and by then phase 2's un-route has
    // landed too. There is no window in which two connections own one routing NAME.
    conns_.erase(it);
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
    const auto it = conns_.find(name);
    if (it == conns_.end()) return std::unexpected(status_t::NOT_FOUND);
    // Resolution is all that happens under the lock. The write itself bumps write_seq_ and
    // DELIVERS to subscribers (RFC-0008 §D) — so await(/net/<name>) fires and a subscribe
    // streams the transition — and a routing-plane subscriber of this very connection's
    // liveness drives `acquire_link`/`release_link` (RFC-0014 §4's standing-binding seam),
    // straight back into `ctl_m_`. Publishing from here re-entered a non-recursive mutex on
    // its own thread; the collected write runs in phase 2 with the lock down (S6, #492).
    txn.publish(it->second.vertex, state);
    return {};
}

result_t<void> transport_vertex_t::acquire_link(std::string_view name) {
    const ctl_txn_t txn(*this);  // ADR-0063 §3 control-plane serialization
    const auto it = conns_.find(name);
    if (it == conns_.end()) return std::unexpected(status_t::NOT_FOUND);
    // Lock order: ctl_m_ → the engine's own mutex; the engine never takes ctl_m_ back, and
    // neither `acquire` nor `release` joins a thread or dispatches — they flip the refcount
    // and kick the worker — so they stay in phase 1 where the `conns_` lookup already is.
    // A connection without an engine answers success as a no-op (see the header: a
    // LISTEN ignores refcount per RFC-0014 §4, and a manual link's liveness is manual).
    if constexpr (kSelfHealLinks)  // #1470 module gate
        if (it->second.engine != nullptr) it->second.engine->acquire();
    return {};
}

result_t<void> transport_vertex_t::release_link(std::string_view name) {
    const ctl_txn_t txn(*this);  // ADR-0063 §3 control-plane serialization
    const auto it = conns_.find(name);
    if (it == conns_.end()) return std::unexpected(status_t::NOT_FOUND);
    if constexpr (kSelfHealLinks)  // #1470 module gate
        if (it->second.engine != nullptr) it->second.engine->release();
    return {};
}

const conn_settings_t* transport_vertex_t::settings_of(std::string_view name) const {
    // ADR-0063 §3 — readers of conns_ race the insert's rebalance
    const ctl_txn_t txn(*this);
    const auto it = conns_.find(name);
    return it == conns_.end() ? nullptr : &it->second.settings;
}

transport_t* transport_vertex_t::link_of(std::string_view name) const {
    // ADR-0063 §3 — readers of conns_ race the insert's rebalance
    const ctl_txn_t txn(*this);
    const auto it = conns_.find(name);
    return it == conns_.end() ? nullptr : it->second.owned.get();
}

}  // namespace tr::net
