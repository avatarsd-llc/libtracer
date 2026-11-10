/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0014 §4 S5 — the link-liveness engine's body (#492). See self_heal_link.hpp for
 * the state machine; this file is the concurrency: one worker thread per engine that is
 * the sole dialer and the sole liveness publisher, a corpse list that keeps socket
 * destruction (a recv-thread join) off the notifier thread, and a generation stamp that
 * keeps a stale corpse's late down-report from killing a healed successor. The worker
 * waits on no timer: an event or the application's `now` releases each attempt (#2304).
 */

#include "libtracer/self_heal_link.hpp"

#include <optional>
#include <utility>

#include "libtracer/frame.hpp"
#include "libtracer/sync.hpp"
#include "libtracer/wait.hpp"

namespace tr::net {

// The module gate (#1470), the same shape `transport_can.cpp` states for `kBusLinks`: this
// TU IS the link-liveness engine, so a build that bound the module out and then compiled it
// anyway has its two selectors disagreeing. Say so here rather than shipping 4.3 KB of
// unreachable flash. `false` is the default since v0.17.0 (#1670), so the likelier mistake is
// now an opt-in that set only the TU switch: the message names both halves.
static_assert(kSelfHealLinks,
              "self_heal_link.cpp is compiled on a build whose config binds "
              "kSelfHealLinks = false (the default since v0.17.0). To opt in, bind "
              "`static constexpr bool kSelfHealLinks = true;` in libtracer/config_override.hpp; "
              "otherwise turn LIBTRACER_SELF_HEAL_LINKS (CMake) OFF so the TU is dropped too. "
              "The ESP-IDF CONFIG_LIBTRACER_SELF_HEAL_LINKS sets both.");

using wire::tlv_node_t;

self_heal_link_t::self_heal_link_t(transport_factory_t factory, conn_settings_t settings,
                                   std::span<const std::byte> raw_config, bool inner_delivers_ropes,
                                   mem::block_source_t& src, mem::block_source_t& stack)
    : factory_(factory),
      // Defaults resolved ONCE, at bind time, so the factory and every wait in this file
      // read the same effective values (RFC-0014 §4: config overrides the engine's own
      // defaults; 0/absent means "the engine's default", never "no bound").
      settings_([&] {
          conn_settings_t s = settings;
          if (s.backoff_ms == 0) s.backoff_ms = kDefaultBackoffMs;
          if (s.connect_timeout_ms == 0) s.connect_timeout_ms = kDefaultConnectTimeoutMs;
          return s;
      }()),
      raw_config_(raw_config),
      inner_ropes_(inner_delivers_ropes),
      src_(src),
      stack_(stack) {}

self_heal_link_t::~self_heal_link_t() { stop(); }

void self_heal_link_t::shut_down() { stop(); }

void self_heal_link_t::set_liveness_publisher(liveness_publish_fn_t fn) {
    const std::lock_guard l(m_);
    publish_ = fn;
}

void self_heal_link_t::born() {
    const std::lock_guard l(m_);
    born_ = true;
    // A demand that came before the birth write was answered and recorded; it dials now.
    if (attempt_due_) ensure_worker_locked();
}

void self_heal_link_t::sock_ref_t::reset() noexcept {
    sock_t* const s = s_;
    s_ = nullptr;
    if (s == nullptr || s->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    mem::drop_in(*s->src, s);  // destroys the socket first: joins its receive thread
}

void self_heal_link_t::park_locked(sock_ref_t dead) noexcept {
    sock_t* const s = dead.detach();
    if (s == nullptr) return;
    s->next_corpse = corpses_;
    corpses_ = s;
}

void self_heal_link_t::drop_corpses(sock_t* head) noexcept {
    while (head != nullptr) {
        sock_t* const next = head->next_corpse;
        // Shut down BEFORE the list lets go: a send in flight may hold the socket past the
        // engine, and the join here is what stops its receive thread reaching the engine
        // (#2288). The last hold, wherever it drops, then frees through `sock_t::src`.
        head->link->shut_down();
        sock_ref_t{head}.reset();  // the list's hold; the last one destroys the socket
        head = next;
    }
}

link_state_t self_heal_link_t::state() const {
    const std::lock_guard l(m_);
    return state_;
}

bool self_heal_link_t::link_up() const noexcept {
    const std::lock_guard l(m_);
    return state_ == link_state_t::UP;
}

transport_drop_stats_t self_heal_link_t::drop_stats() const noexcept {
    sock_ref_t s;
    {
        const std::lock_guard l(m_);
        s = inner_;
    }
    transport_drop_stats_t stats = s ? s->link->drop_stats() : transport_drop_stats_t{};
    const transport_drop_stats_t engine = engine_drops_.snapshot();
    stats.dropped_rx += engine.dropped_rx;
    stats.dropped_tx += engine.dropped_tx;
    return stats;
}

std::uint64_t self_heal_link_t::sweep(std::uint64_t now, std::uint64_t deadline) {
    sweep_t sw{.now = now, .deadline = deadline};
    sock_ref_t s;
    {
        const std::lock_guard l(m_);
        clocked_ = true;  // from here on the app's `now`, not traffic, spaces the retries
        s = inner_;
        // A heal waits for `backoff` on the app's clock (#2304). The first call that finds the
        // wait stamps it; one at or past the stamp releases the attempt and stamps the next, so
        // a pending reconnect always answers a time strictly after `now`, even while the worker
        // has not yet taken an attempt released earlier (or could not be started: retried here).
        if (state_ == link_state_t::RECONNECTING && refs_ > 0) {
            if (retry_at_ == 0) {
                retry_at_ = now + settings_.backoff_ms;
            } else if (now >= retry_at_) {
                retry_at_ = now + settings_.backoff_ms;
                attempt_due_ = true;
                ensure_worker_locked();
                cv_.notify_all();
            }
            sw.due(retry_at_);
        }
    }
    if (s) sw.due(s->link->sweep(now, deadline));
    return sw.next;
}

void self_heal_link_t::acquire() {
    const std::lock_guard l(m_);
    if (stop_) return;
    ++refs_;
    // A standing binding demands reachability: kick the link toward UP without blocking the
    // caller (the §4 "bring it up and wait" verb is `await` on the vertex).
    demand_locked();
}

void self_heal_link_t::release() {
    const std::lock_guard l(m_);
    if (refs_ == 0) return;  // unbalanced release: ignored, never underflows
    if (--refs_ != 0) return;
    // §4: refcount → 0 → close socket, go dormant, stop retrying. An UP socket is parked
    // for the worker to reap and the transition published by the worker (sole publisher);
    // a RECONNECTING loop observes refs_ == 0 at its next gate and dormants itself.
    if (state_ == link_state_t::UP) {
        park_locked(std::move(inner_));
        live_gen_ = 0;
        state_ = link_state_t::DORMANT;
        attempt_due_ = false;  // a retry `sweep` released before the heal came up is stale
        publish_pending_ = true;
        ensure_worker_locked();
    }
    cv_.notify_all();
}

void self_heal_link_t::stop() {
    // On a thread that runs link callbacks nothing here may wait (`on_callback_thread`): the
    // worker could be reaping a socket whose callback is this one, and a socket's own thread
    // could be the one asking. The stop is still made; the destructor, which the graph's
    // collect() runs outside every callback, joins and frees what is left.
    const bool defer = on_callback_thread();
    bool started = false;
    {
        const std::lock_guard l(m_);
        stop_ = true;
        // Claim the worker under the lock and clear the flag, so a second `stop()` (the
        // destructor after an explicit one) joins nothing rather than joining twice.
        started = worker_started_ && !defer;
        if (!defer) worker_started_ = false;
        cv_.notify_all();
    }
    if (started) worker_.join();
    // Tear the sockets down with `m_` RELEASED: each destruction joins a receive thread
    // that may itself be blocked in on_socket_down waiting for `m_`.
    // The live socket joins the corpses, so every socket leaves the same way: shut down here,
    // while the engine is alive, whoever holds it last.
    sock_t* dead = nullptr;
    {
        const std::lock_guard l(m_);
        park_locked(std::move(inner_));
        dead = std::exchange(corpses_, nullptr);
        live_gen_ = 0;
        state_ = link_state_t::DORMANT;  // terminal resting value; nothing publishes it
    }
    if (!defer) {
        drop_corpses(dead);
        return;
    }
    // Deferred: shut down (which waits for nothing here), and kept for the destructor.
    for (sock_t* s = dead; s != nullptr; s = s->next_corpse) s->link->shut_down();
    const std::lock_guard l(m_);
    sock_t** tail = &dead;
    while (*tail != nullptr) tail = &(*tail)->next_corpse;
    *tail = corpses_;  // a socket the worker parked meanwhile
    corpses_ = dead;
}

void self_heal_link_t::send(std::span<const std::byte> frame) {
    const std::span<const std::byte> one[1] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void self_heal_link_t::send(std::span<const std::span<const std::byte>> iov) {
    if (const sock_ref_t s = ready_socket()) {
        s->link->send(iov);
        return;
    }
    engine_drops_.count_dropped_tx();
}

void self_heal_link_t::send(std::span<const std::span<const std::byte>> head,
                            const graph::value_t& value, const graph::delivery_lane_t& lane) {
    // The whole delivery, unchanged (#2238): the inner link picks the vertex's ring from the
    // lane and keeps the value by reference. The base lowering would flatten it onto the
    // inner link's control lane.
    if (const sock_ref_t s = ready_socket()) {
        s->link->send(head, value, lane);
        return;
    }
    engine_drops_.count_dropped_tx();
}

self_heal_link_t::sock_ref_t self_heal_link_t::ready_socket() {
    const std::lock_guard l(m_);
    if (stop_) return {};
    if (state_ == link_state_t::UP && inner_) return inner_;
    // Every other state is link-down, answered at once (RFC-0014 Amendment 5): the op never
    // waits, its frame is the caller's counted drop, and the link is kicked toward UP on the
    // worker. A caller that must bring the link up and wait awaits `up` on the connection
    // vertex. The worker's own publish fan-out never kicks: a subscriber of this link's
    // liveness routed through this link would otherwise re-dial the link it just put to rest,
    // or retry it in a loop.
    if (detail::this_thread_id() != worker_id_) demand_locked();
    return {};
}

void self_heal_link_t::demand_locked() {
    // A DORMANT link dials on demand (Amendment 5). A RECONNECTING one retries on the event only
    // while no app clock drives it (#2304): once `sweep` has run, its `now` spaces the attempts
    // by `backoff`, and traffic must not bypass that. DIALING is already driving toward UP.
    if (state_ != link_state_t::DORMANT && (state_ != link_state_t::RECONNECTING || clocked_))
        return;
    attempt_due_ = true;
    retry_at_ = 0;  // an attempt released off the app's clock: the next wait is stamped afresh
    ensure_worker_locked();
    cv_.notify_all();
}

void self_heal_link_t::worker_entry(void* self) {
    static_cast<self_heal_link_t*>(self)->worker_main();
}

void self_heal_link_t::ensure_worker_locked() {
    if (stop_ || !born_ || worker_started_) return;
    // The #1470 stack knob (0 = the platform default; every host build). On ESP-IDF the
    // stack is drawn with its control block from the engine's thread-stack store (#2288,
    // `link_memory_t::stack`, default `mem::stack_source()`), never from `src_`, the sockets'
    // store, which on an MCU is the shared value arena. A
    // refused spawn, from the source or the platform, leaves the engine unworkered (counted
    // in that source's census) and the next transition tries again. Never a throw: a
    // `std::thread` spawn failure is a `std::abort()` under -fno-exceptions.
    worker_started_ = worker_.start(&self_heal_link_t::worker_entry, this,
                                    kSelfHealWorkerStackBytes, "tr_heal", stack_);
}

void self_heal_link_t::publish_unlocked(std::unique_lock<tr::mutex_t>& l, link_state_t s) {
    // Snapshot the sink and publish with `m_` RELEASED: the write fans out to
    // subscribers under graph locks, and a subscriber edge routed through THIS link
    // re-enters `send` (which takes `m_`).
    const liveness_publish_fn_t fn = publish_;
    l.unlock();
    if (fn) fn(s);
    l.lock();
}

void self_heal_link_t::reap_locked(std::unique_lock<tr::mutex_t>& l) {
    sock_t* const dead = std::exchange(corpses_, nullptr);
    l.unlock();
    drop_corpses(dead);  // joins each dead socket's receive thread — hence outside `m_`
    l.lock();
}

bool self_heal_link_t::wire_socket(sock_t& sock) {
    // The engine's own sinks forward into `rx_` — whatever receiver the router installed
    // on the ENGINE at add_child dispatches from there, so the registry wiring is done
    // once and every healed socket inherits it. Down-notifier before start_receiving,
    // same bring-up discipline as make_connection (#1025).
    if (sock.link->delivers_ropes()) {
        sock.link->set_rope_receiver(
            [](void* c, view::rope_t frame) {
                auto* const s = static_cast<sock_t*>(c);
                s->self->engine_drops_.count_dropped_rx(
                    static_cast<std::size_t>(!s->self->rx_.deliver_rope(std::move(frame))));
            },
            &sock);
    } else {
        sock.link->set_receiver(
            [](void* c, std::span<const std::byte> frame) {
                auto* const s = static_cast<sock_t*>(c);
                s->self->rx_.deliver_borrowed(frame);
            },
            &sock);
    }
    const bool went_down = sock.link->set_down_notifier(
        [](void* c) {
            auto* const s = static_cast<sock_t*>(c);
            s->self->on_socket_down(*s);
        },
        &sock);
    // A receive that could not start (#2288: its stack refused) is a socket dead on arrival:
    // the same failed attempt as a went-down, retried on the engine's backoff.
    return !sock.link->start_receiving() || went_down;
}

void self_heal_link_t::on_socket_down(sock_t& sock) {
    const std::lock_guard l(m_);
    if (stop_) return;
    // A corpse's late report (its recv thread firing after a heal already replaced it)
    // must not kill the healthy successor: only the LIVE generation's loss transitions.
    if (sock.gen != live_gen_ || !inner_ || state_ != link_state_t::UP) return;
    park_locked(std::move(inner_));
    live_gen_ = 0;
    // Loss while in use → self-heal (refcount > 0); loss with nothing bound → dormant,
    // no background retry (§4). State flips HERE so ops fail fast immediately; the
    // publish is the worker's (sole publisher — this runs on the socket's own thread). The
    // loss is the event that releases the heal's first attempt; later ones wait for `backoff`
    // on the app's clock, or for the next event when no clock drives the link (#2304).
    state_ = refs_ > 0 ? link_state_t::RECONNECTING : link_state_t::DORMANT;
    attempt_due_ = refs_ > 0;
    retry_at_ = 0;
    publish_pending_ = true;
    ensure_worker_locked();
    cv_.notify_all();
}

bool self_heal_link_t::attempt_locked(std::unique_lock<tr::mutex_t>& l) {
    l.unlock();
    // Validate the stored config bytes fresh per attempt and read them in place (the node
    // borrows raw_config_, which outlives it); the factory re-parses its kind-private keys
    // from it exactly as it would at an eager creation.
    std::optional<tlv_node_t> cfg;
    if (!raw_config_.empty()) {
        if (auto node = tlv_node_t::over(raw_config_)) cfg = *node;
    }
    const tlv_node_t* cfg_ptr = cfg ? &*cfg : nullptr;
    auto built = factory_(settings_, cfg_ptr, src_);
    sock_ref_t sock;
    bool went_down = false;  // the socket died before its notifier was in: a failed attempt
    // A refused record is a failed attempt, as a refused socket is: the link it would have
    // named is destroyed with `built` and the engine retries on its backoff.
    if (built) sock = sock_ref_t(mem::make_in<sock_t>(src_));
    if (sock) {
        sock->link = std::move(*built);
        sock->self = this;
        sock->src = &src_;
        sock->gen = gen_ctr_.fetch_add(1, std::memory_order_relaxed) + 1;
        // Sinks + notifier + start, BEFORE the socket is adopted: a kind whose receive
        // thread starts in its constructor is already draining, and the sinks route into
        // this engine's rx_, which the router wired at add_child — frames have somewhere
        // to land from the first instant (#1025).
        went_down = wire_socket(*sock.get());
    }
    l.lock();
    if (stop_ || went_down) {
        // Torn down concurrently, or already dead: the just-built socket is a corpse
        // (destroyed by stop() or the reaper outside m_), never adopted.
        park_locked(std::move(sock));
        return false;
    }
    if (!sock) return false;
    inner_ = std::move(sock);
    live_gen_ = inner_->gen;
    return true;
}

void self_heal_link_t::worker_main() {
    std::unique_lock l(m_);
    worker_id_ = detail::this_thread_id();
    for (;;) {
        // No timeout: every wake is an event, so a dormant or backing-off link costs no
        // wakeup at all (#2304). An attempt is released by a demand (`demand_locked`), by a
        // loss, or by the app's `now` passing `retry_at_` (`sweep`).
        cv_.wait(l, [&] {
            return stop_ || corpses_ != nullptr || publish_pending_ || attempt_due_ ||
                   (state_ == link_state_t::RECONNECTING && refs_ == 0);
        });
        if (stop_) break;
        if (corpses_ != nullptr) {
            reap_locked(l);
            continue;  // re-evaluate: state may have moved while unlocked
        }
        if (publish_pending_) {
            publish_pending_ = false;
            publish_unlocked(l, state_);
            continue;
        }
        if (state_ == link_state_t::RECONNECTING && refs_ == 0) {
            // The last standing release stops the heal (§4): dormant, no further attempt.
            attempt_due_ = false;
            state_ = link_state_t::DORMANT;
            publish_unlocked(l, link_state_t::DORMANT);
            continue;
        }
        attempt_due_ = false;
        if (state_ == link_state_t::DORMANT) {
            // dormant → dialing: the demand dial (first-ever or resumed), §4.
            state_ = link_state_t::DIALING;
            publish_unlocked(l, link_state_t::DIALING);
            if (stop_) break;
        } else if (state_ != link_state_t::RECONNECTING) {
            continue;  // a stale release: the link came up meanwhile
        }
        const bool up = attempt_locked(l);
        if (stop_) break;
        // A failed attempt heals only under a standing binding, forever (§4: no attempt cap,
        // no terminal state); a lone one-shot's failed dial re-dormants with NO retry.
        state_ = up          ? link_state_t::UP
                 : refs_ > 0 ? link_state_t::RECONNECTING
                             : link_state_t::DORMANT;
        publish_unlocked(l, state_);
    }
    worker_id_ = {};
}

}  // namespace tr::net
