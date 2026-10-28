/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0014 §4 S5 — the link-liveness engine's body (#492). See self_heal_link.hpp for
 * the state machine; this file is the concurrency: one worker thread per engine that is
 * the sole dialer and the sole liveness publisher, a corpse list that keeps socket
 * destruction (a recv-thread join) off the notifier thread, and a generation stamp that
 * keeps a stale corpse's late down-report from killing a healed successor.
 */

#include "libtracer/self_heal_link.hpp"

#include <chrono>
#include <optional>
#include <utility>

#include "libtracer/frame.hpp"

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
                                   mem::block_source_t& src)
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
      src_(src) {}

self_heal_link_t::~self_heal_link_t() { stop(); }

void self_heal_link_t::shut_down() { stop(); }

void self_heal_link_t::set_liveness_publisher(liveness_publish_fn_t fn) {
    const std::lock_guard l(m_);
    publish_ = fn;
}

void self_heal_link_t::sock_ref_t::reset() noexcept {
    sock_t* const s = s_;
    s_ = nullptr;
    if (s == nullptr || s->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    mem::block_source_t& src = s->self->src_;
    mem::drop_in(src, s);  // destroys the socket first: joins its receive thread
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
    stats.dropped_tx += engine_dropped_tx_.load(std::memory_order_relaxed);
    return stats;
}

std::size_t self_heal_link_t::close_frozen_peers(std::uint64_t now, std::uint64_t deadline) {
    sock_ref_t s;
    {
        const std::lock_guard l(m_);
        s = inner_;
    }
    return s ? s->link->close_frozen_peers(now, deadline) : 0;
}

void self_heal_link_t::acquire() {
    const std::lock_guard l(m_);
    if (stop_) return;
    ++refs_;
    // A standing binding demands reachability: kick a dormant link toward UP without
    // blocking the caller (the §4 "bring it up and wait" verb is `await` on the vertex).
    if (state_ == link_state_t::DORMANT) {
        wake_requested_ = true;
        ensure_worker_locked();
        cv_.notify_all();
    }
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
        publish_pending_ = true;
        ensure_worker_locked();
    }
    cv_.notify_all();
}

void self_heal_link_t::stop() {
    bool started = false;
    {
        const std::lock_guard l(m_);
        stop_ = true;
        // Claim the worker under the lock and clear the flag, so a second `stop()` (the
        // destructor after an explicit one) joins nothing rather than joining twice.
        started = worker_started_;
        worker_started_ = false;
        cv_.notify_all();
    }
    if (started) worker_.join();
    // Tear the sockets down with `m_` RELEASED: each destruction joins a receive thread
    // that may itself be blocked in on_socket_down waiting for `m_`.
    sock_t* dead = nullptr;
    sock_ref_t last;
    {
        const std::lock_guard l(m_);
        dead = std::exchange(corpses_, nullptr);
        last = std::move(inner_);
        live_gen_ = 0;
        state_ = link_state_t::DORMANT;  // terminal resting value; nothing publishes it
    }
    drop_corpses(dead);
    last.reset();
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
    engine_dropped_tx_.fetch_add(1, std::memory_order_relaxed);
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
    engine_dropped_tx_.fetch_add(1, std::memory_order_relaxed);
}

self_heal_link_t::sock_ref_t self_heal_link_t::ready_socket() {
    const std::lock_guard l(m_);
    if (stop_) return {};
    if (state_ == link_state_t::UP && inner_) return inner_;
    // Every other state is link-down, answered at once (RFC-0014 Amendment 5): the op never
    // waits, its frame is the caller's counted drop, and a DORMANT link is kicked toward UP on
    // the worker. DIALING and RECONNECTING are already driving there. A caller that must
    // bring the link up and wait awaits `up` on the connection vertex. The worker's own
    // publish fan-out never kicks: a subscriber of this link's liveness routed through this
    // link would otherwise re-dial the link it just put to rest.
    if (state_ == link_state_t::DORMANT && detail::this_thread_id() != worker_id_) {
        wake_requested_ = true;
        ensure_worker_locked();
        cv_.notify_all();
    }
    return {};
}

void self_heal_link_t::worker_entry(void* self) {
    static_cast<self_heal_link_t*>(self)->worker_main();
}

void self_heal_link_t::ensure_worker_locked() {
    if (stop_ || worker_started_) return;
    // The #1470 stack knob (0 = the platform default; every host build). On ESP-IDF the
    // stack is drawn from the link's own source with its control block (#2288); a refused
    // spawn, from the source or the platform, simply leaves the engine unworkered and the
    // next transition tries again. Never a throw: a `std::thread` spawn failure is a
    // `std::abort()` under -fno-exceptions.
    worker_started_ = worker_.start(&self_heal_link_t::worker_entry, this,
                                    kSelfHealWorkerStackBytes, "tr_heal", src_);
}

void self_heal_link_t::publish_unlocked(std::unique_lock<std::mutex>& l, link_state_t s) {
    // Snapshot the sink and publish with `m_` RELEASED: the write fans out to
    // subscribers under graph locks, and a subscriber edge routed through THIS link
    // re-enters `send` (which takes `m_`).
    const liveness_publish_fn_t fn = publish_;
    l.unlock();
    if (fn) fn(s);
    l.lock();
}

void self_heal_link_t::reap_locked(std::unique_lock<std::mutex>& l) {
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
                s->self->rx_.deliver_rope(std::move(frame));
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
    sock.link->start_receiving();
    return went_down;
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
    // publish is the worker's (sole publisher — this runs on the socket's own thread).
    state_ = refs_ > 0 ? link_state_t::RECONNECTING : link_state_t::DORMANT;
    publish_pending_ = true;
    ensure_worker_locked();
    cv_.notify_all();
}

bool self_heal_link_t::attempt_locked(std::unique_lock<std::mutex>& l) {
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
        cv_.wait(l, [&] {
            return stop_ || corpses_ != nullptr || publish_pending_ || wake_requested_ ||
                   state_ == link_state_t::RECONNECTING;
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
        if (wake_requested_) {
            wake_requested_ = false;
            if (state_ != link_state_t::DORMANT) continue;  // stale kick
            // dormant → dialing: the demand dial (first-ever or resumed), §4.
            state_ = link_state_t::DIALING;
            publish_unlocked(l, link_state_t::DIALING);
            if (stop_) break;
            const bool up = attempt_locked(l);
            if (stop_) break;
            if (up) {
                state_ = link_state_t::UP;
                publish_unlocked(l, link_state_t::UP);
            } else {
                // A lone one-shot's failed dial re-dormants with NO background retry;
                // only a standing binding (refs > 0) triggers self-heal (§4).
                state_ = refs_ > 0 ? link_state_t::RECONNECTING : link_state_t::DORMANT;
                publish_unlocked(l, state_);
            }
            continue;
        }
        // state_ == RECONNECTING: one self-heal tick. Runs iff refcount > 0 (§4);
        // retries FOREVER — no attempt cap, no terminal failure state.
        if (refs_ == 0) {
            state_ = link_state_t::DORMANT;
            publish_unlocked(l, link_state_t::DORMANT);
            continue;
        }
        const bool up = attempt_locked(l);
        if (stop_) break;
        if (up) {
            state_ = link_state_t::UP;
            publish_unlocked(l, link_state_t::UP);
            continue;
        }
        // The backoff wait between attempts — interruptible by stop and by the last
        // standing release (which dormants at the top of the next iteration).
        cv_.wait_for(l, std::chrono::milliseconds(settings_.backoff_ms),
                     [&] { return stop_ || refs_ == 0; });
    }
    worker_id_ = {};
}

}  // namespace tr::net
