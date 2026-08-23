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

using wire::tlv_t;

self_heal_link_t::self_heal_link_t(transport_vertex_t::transport_factory_t factory,
                                   conn_settings_t settings, std::vector<std::byte> raw_config,
                                   bool inner_delivers_ropes)
    : factory_(std::move(factory)),
      // Defaults resolved ONCE, at bind time, so the factory and every wait in this file
      // read the same effective values (RFC-0014 §4: config overrides the engine's own
      // defaults; 0/absent means "the engine's default", never "no bound").
      settings_([&] {
          conn_settings_t s = std::move(settings);
          if (s.backoff_ms == 0) s.backoff_ms = kDefaultBackoffMs;
          if (s.connect_timeout_ms == 0) s.connect_timeout_ms = kDefaultConnectTimeoutMs;
          return s;
      }()),
      raw_config_(std::move(raw_config)),
      inner_ropes_(inner_delivers_ropes) {}

self_heal_link_t::~self_heal_link_t() { stop(); }

void self_heal_link_t::set_liveness_publisher(liveness_publish_fn_t fn) {
    const std::lock_guard l(m_);
    publish_ = std::move(fn);
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
    std::shared_ptr<sock_t> s;
    {
        const std::lock_guard l(m_);
        s = inner_;
    }
    transport_drop_stats_t stats = s != nullptr ? s->link->drop_stats() : transport_drop_stats_t{};
    stats.dropped_tx += engine_dropped_tx_.load(std::memory_order_relaxed);
    return stats;
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
        corpses_.push_back(std::move(inner_));
        live_gen_ = 0;
        state_ = link_state_t::DORMANT;
        publish_pending_ = true;
        ensure_worker_locked();
    }
    cv_.notify_all();
}

void self_heal_link_t::stop() {
    std::thread w;
    {
        const std::lock_guard l(m_);
        stop_ = true;
        w = std::move(worker_);
        cv_.notify_all();
    }
    if (w.joinable()) w.join();
    // Tear the sockets down with `m_` RELEASED: each destruction joins a receive thread
    // that may itself be blocked in on_socket_down waiting for `m_`.
    std::vector<std::shared_ptr<sock_t>> dead;
    std::shared_ptr<sock_t> last;
    {
        const std::lock_guard l(m_);
        dead.swap(corpses_);
        last = std::move(inner_);
        live_gen_ = 0;
        state_ = link_state_t::DORMANT;  // terminal resting value; nothing publishes it
    }
    dead.clear();
    last.reset();
}

void self_heal_link_t::send(std::span<const std::byte> frame) {
    if (const std::shared_ptr<sock_t> s = ready_socket()) {
        s->link->send(frame);
        return;
    }
    engine_dropped_tx_.fetch_add(1, std::memory_order_relaxed);
}

void self_heal_link_t::send(std::span<const std::span<const std::byte>> iov) {
    if (const std::shared_ptr<sock_t> s = ready_socket()) {
        s->link->send(iov);
        return;
    }
    engine_dropped_tx_.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<self_heal_link_t::sock_t> self_heal_link_t::ready_socket() {
    std::unique_lock l(m_);
    if (stop_) return nullptr;
    if (state_ == link_state_t::UP && inner_ != nullptr) return inner_;
    // Ops on a down/RECONNECTING link fail fast with link-down (§4) — the self-heal loop
    // is already driving toward UP; blocking here would stall the forward path on a dead
    // peer for up to backoff × forever.
    if (state_ == link_state_t::RECONNECTING) return nullptr;
    // DORMANT / DIALING: any op auto-wakes the link and waits for ONE connect attempt,
    // bounded by connect_timeout (§4's sanctioned stall-on-dial) — except the worker's
    // own publish fan-out, which must not block on the worker (a subscriber of this
    // link's own liveness routed through this link would otherwise self-deadlock).
    if (detail::this_thread_id() == worker_id_) return nullptr;
    if (state_ == link_state_t::DORMANT) {
        wake_requested_ = true;
        ensure_worker_locked();
        cv_.notify_all();
    }
    const std::uint64_t before = attempt_seq_;
    const bool concluded =
        cv_.wait_for(l, std::chrono::milliseconds(settings_.connect_timeout_ms), [&] {
            return stop_ || (state_ == link_state_t::UP && inner_ != nullptr) ||
                   attempt_seq_ > before;
        });
    if (!concluded || stop_) return nullptr;  // deadline: the attempt is still in flight
    if (state_ == link_state_t::UP && inner_ != nullptr) return inner_;
    return nullptr;  // the one attempt concluded not-up: link-down
}

void self_heal_link_t::ensure_worker_locked() {
    if (stop_ || worker_.joinable()) return;
    worker_ = std::thread([this] { worker_main(); });
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
    std::vector<std::shared_ptr<sock_t>> dead;
    dead.swap(corpses_);
    l.unlock();
    dead.clear();  // joins each dead socket's receive thread — hence outside `m_`
    l.lock();
}

void self_heal_link_t::wire_socket(sock_t& sock) {
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
    sock.link->set_down_notifier(
        [](void* c) {
            auto* const s = static_cast<sock_t*>(c);
            s->self->on_socket_down(*s);
        },
        &sock);
    sock.link->start_receiving();
}

void self_heal_link_t::on_socket_down(sock_t& sock) {
    const std::lock_guard l(m_);
    if (stop_) return;
    // A corpse's late report (its recv thread firing after a heal already replaced it)
    // must not kill the healthy successor: only the LIVE generation's loss transitions.
    if (sock.gen != live_gen_ || inner_ == nullptr || state_ != link_state_t::UP) return;
    corpses_.push_back(std::move(inner_));
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
    // Decode the stored config bytes fresh per attempt (the tlv borrows raw_config_,
    // which outlives it); the factory re-parses its kind-private keys from it exactly as
    // it would at an eager creation.
    const tlv_t* cfg_ptr = nullptr;
    std::optional<tlv_t> cfg;
    if (!raw_config_.empty()) {
        if (auto decoded = wire::decode(raw_config_)) {
            cfg.emplace(std::move(*decoded));
            cfg_ptr = &*cfg;
        }
    }
    auto built = factory_(settings_, cfg_ptr);
    std::shared_ptr<sock_t> sock;
    if (built) {
        sock = std::make_shared<sock_t>();
        sock->link = std::move(*built);
        sock->self = this;
        sock->gen = gen_ctr_.fetch_add(1, std::memory_order_relaxed) + 1;
        // Sinks + notifier + start, BEFORE the socket is adopted: a kind whose receive
        // thread starts in its constructor is already draining, and the sinks route into
        // this engine's rx_, which the router wired at add_child — frames have somewhere
        // to land from the first instant (#1025).
        wire_socket(*sock);
    }
    l.lock();
    ++attempt_seq_;
    if (stop_) {
        // Torn down concurrently: the just-built socket is a corpse (destroyed by stop()
        // outside m_), never adopted.
        if (sock != nullptr) corpses_.push_back(std::move(sock));
        return false;
    }
    if (sock == nullptr) return false;
    inner_ = std::move(sock);
    live_gen_ = inner_->gen;
    return true;
}

void self_heal_link_t::worker_main() {
    std::unique_lock l(m_);
    worker_id_ = detail::this_thread_id();
    for (;;) {
        cv_.wait(l, [&] {
            return stop_ || !corpses_.empty() || publish_pending_ || wake_requested_ ||
                   state_ == link_state_t::RECONNECTING;
        });
        if (stop_) break;
        if (!corpses_.empty()) {
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
            cv_.notify_all();
            continue;
        }
        // state_ == RECONNECTING: one self-heal tick. Runs iff refcount > 0 (§4);
        // retries FOREVER — no attempt cap, no terminal failure state.
        if (refs_ == 0) {
            state_ = link_state_t::DORMANT;
            publish_unlocked(l, link_state_t::DORMANT);
            cv_.notify_all();
            continue;
        }
        const bool up = attempt_locked(l);
        if (stop_) break;
        if (up) {
            state_ = link_state_t::UP;
            publish_unlocked(l, link_state_t::UP);
            cv_.notify_all();
            continue;
        }
        cv_.notify_all();  // op-waiters observe the concluded attempt (fail-fast)
        // The backoff wait between attempts — interruptible by stop and by the last
        // standing release (which dormants at the top of the next iteration).
        cv_.wait_for(l, std::chrono::milliseconds(settings_.backoff_ms),
                     [&] { return stop_ || refs_ == 0; });
    }
    worker_id_ = {};
}

}  // namespace tr::net
