/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ADR-0044 — stateless transport-peer enumeration + transparent per-peer FWD over
 * the CAN bus binding, proven over the in-memory fake link (no kernel CAN):
 *
 *     client --loopback--> transit T (node 1) --CAN bus--> peer P (node 5)
 *                                             \--(same bus)-- bystander Q (node 7)
 *
 * Assertions:
 *   - P and Q announce at join (hello advertise); a read of the transit's
 *     /net/can0:children[] synthesizes exactly {n5, n7} — from live traffic, with
 *     NO vertex created for any peer (the transit graph holds only /net + /net/can0);
 *   - the same listing is reachable remotely: FWD{READ, dst=/net/can0, :children[]}
 *     from the client returns the peer POINTs;
 *   - FWD{READ, dst=/n5/a/b} forwards through T DIRECTED to P (bystander Q, on the
 *     same broadcast bus, delivers nothing), resolves at P's terminus, and the
 *     REPLY routes back per-peer (P answers over `n1`, T forwards to `cli`) —
 *     byte-exact stored VALUE at the client, zero per-request state anywhere;
 *   - a silent peer expires from the enumeration after `peer_ttl` (design (b) of
 *     ADR-0044's implementation note: passive last-heard table);
 *   - the peer table grows one entry per DISTINCT announcing node (structurally
 *     bounded by the 13-bit id space) and re-announcing is idempotent.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_can.hpp"
#include "libtracer/view_can.hpp"

namespace {

using namespace std::chrono_literals;
namespace can = tr::net::can;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

constexpr auto kBudget = 5000ms;

// ---------------------------------------------------------------------------
// In-memory fake CAN bus + link (the transport_can_test.cpp test seam).
// ---------------------------------------------------------------------------

class fake_link_t;

class fake_can_bus_t {
   public:
    void attach(fake_link_t* l) {
        const std::lock_guard lock(m_);
        links_.push_back(l);
    }
    void detach(fake_link_t* l) {
        const std::lock_guard lock(m_);
        for (auto it = links_.begin(); it != links_.end(); ++it) {
            if (*it == l) {
                links_.erase(it);
                break;
            }
        }
    }
    void broadcast(fake_link_t* from, const tr::net::can_frame_data_t& f);

   private:
    std::mutex m_;
    std::vector<fake_link_t*> links_;
};

class fake_link_t : public tr::net::can_link_t {
   public:
    explicit fake_link_t(fake_can_bus_t& bus) : bus_(bus) {
        bus_.attach(this);
        worker_ = std::thread([this] { run(); });
    }
    ~fake_link_t() override {
        bus_.detach(this);
        {
            const std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    fake_link_t(const fake_link_t&) = delete;
    fake_link_t& operator=(const fake_link_t&) = delete;

    void write_raw(const tr::net::can_frame_data_t& f) override { bus_.broadcast(this, f); }
    void on_receive(rx_fn_t rx) override {
        const std::lock_guard lock(m_);
        rx_ = std::move(rx);
    }
    void enqueue(const tr::net::can_frame_data_t& f) {
        const std::lock_guard lock(m_);
        q_.push_back(f);
        cv_.notify_one();
    }

   private:
    void run() {
        std::unique_lock lock(m_);
        while (true) {
            cv_.wait(lock, [this] { return stop_ || !q_.empty(); });
            if (stop_ && q_.empty()) return;
            tr::net::can_frame_data_t f = q_.front();
            q_.pop_front();
            rx_fn_t rx = rx_;
            lock.unlock();
            if (rx) rx(f);
            lock.lock();
        }
    }

    fake_can_bus_t& bus_;
    rx_fn_t rx_;
    std::deque<tr::net::can_frame_data_t> q_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread worker_;
};

void fake_can_bus_t::broadcast(fake_link_t* from, const tr::net::can_frame_data_t& f) {
    const std::lock_guard lock(m_);
    for (auto* l : links_) {
        if (l != from) l->enqueue(f);
    }
}

// ---------------------------------------------------------------------------
// Wire builders (the fwd_multihop_test.cpp idiom, canonical emit helpers).
// ---------------------------------------------------------------------------

std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> p(4);
    tr::detail::store_le<std::uint32_t>(p, v);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}

// A FIELD selector for the whole-array ":children[]" (RFC-0004 §C: NAME +
// index_mode VALUE u8 = ELEMENT with no index — the append/whole-array form).
std::vector<std::byte> b_field_children() {
    std::vector<std::byte> body;
    tr::wire::emit_name(body, "children");
    const std::byte mode{1};  // index_mode ELEMENT, no index => "[]"
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&mode, 1));
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}

std::vector<std::byte> b_fwd(fwd_op_t op, const std::vector<std::byte>& dst,
                             const std::vector<std::byte>& src,
                             const std::vector<std::byte>& field = {}) {
    std::vector<std::byte> body;
    const std::byte ob{static_cast<std::uint8_t>(op)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&ob, 1));
    body.insert(body.end(), dst.begin(), dst.end());
    body.insert(body.end(), field.begin(), field.end());
    body.insert(body.end(), src.begin(), src.end());
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

// SPEC{ type, name } with no config — the provide_link-staged connection form.
view_t conn_spec(std::string_view type, std::string_view name) {
    std::vector<std::byte> body;
    tr::wire::emit_name(body, "type");
    tr::wire::emit_name(body, type);
    tr::wire::emit_name(body, "name");
    tr::wire::emit_name(body, name);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);
    return owned(out);
}

// The peer names inside a members POINT (POINT{ POINT{NAME}... }) view/TLV.
std::set<std::string> member_names(const tlv_t& point) {
    std::set<std::string> names;
    for (const tlv_t& m : point.children) {
        if (m.type == type_t::POINT && !m.children.empty() && m.children[0].type == type_t::NAME) {
            names.emplace(tr::detail::as_string_view(m.children[0].payload));
        }
    }
    return names;
}

std::set<std::string> enumerate_local(graph_t& g, const char* path) {
    const auto r = g.read(*path_t::parse(path));
    if (!r) return {};
    const auto dec = tr::wire::view_as_tlv(r->only());
    if (!dec || dec->type != type_t::POINT) return {};
    return member_names(*dec);
}

// Poll until `pred` holds (the enumeration is fed by live announce traffic on
// worker threads) — deadline-bounded, no fixed sleeps on the success path.
template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

// A bounded reply mailbox for the raw loopback client.
struct mailbox_t {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::vector<std::byte>> q;

    void push(std::vector<std::byte> v) {
        {
            const std::lock_guard lock(m);
            q.push_back(std::move(v));
        }
        cv.notify_all();
    }
    std::optional<std::vector<std::byte>> wait(std::chrono::milliseconds budget) {
        std::unique_lock lock(m);
        if (!cv.wait_for(lock, budget, [&] { return !q.empty(); })) return std::nullopt;
        std::vector<std::byte> v = std::move(q.front());
        q.erase(q.begin());
        return v;
    }
};

std::uint8_t value_u8(const tlv_t& v) { return tr::detail::load_le<std::uint8_t>(v.payload); }

// ---------------------------------------------------------------------------
// The end-to-end topology test.
// ---------------------------------------------------------------------------

void test_enumeration_and_forwarding() {
    std::printf("ADR-0044 end to end (enumerate + FWD through, directed):\n");

    fake_can_bus_t bus;

    // ----- transit node T (CAN node 1): graph + router + /net/can0. ----------
    // Declaration = reverse teardown order (the RAII idiom of fwd_multihop_test):
    // the CAN transports are declared LAST so their receive threads stop first —
    // no frame can be forwarded into a half-destroyed loopback/mailbox.
    graph_t graph_t_;
    fwd_router_t router_t(graph_t_);
    transport_vertex_t net_t(graph_t_, router_t);

    // The client link: a raw loopback endpoint on the far side.
    mailbox_t inbox;
    tr::net::loopback_channel_t channel;
    router_t.add_child("cli", channel.a());
    channel.b().set_receiver([&](std::span<const std::byte> f) {
        inbox.push(std::vector<std::byte>(f.begin(), f.end()));
    });

    tr::net::transport_can tcan_t(std::make_unique<fake_link_t>(bus),
                                  {0, 1, tr::view::can_frame_mode_t::CLASSIC, "transit"});
    net_t.provide_link("can0", tcan_t);
    const auto made = graph_t_.write(path_t("/net:children[]"), conn_spec("client", "can0"));
    check(made.has_value(), "the CAN connection vertex /net/can0 is SPEC-created");

    // ----- peer P (CAN node 5): terminus with /a/b holding a known VALUE. ----
    graph_t graph_p;
    const std::uint32_t kStored = 0xCAFEF00Du;
    tr::graph::vertex_t* vp = *graph_p.register_vertex(path_t("/a/b"), role_t::STORED_VALUE);
    (void)graph_p.write(vp, owned(b_value_u32(kStored)));
    fwd_router_t router_p(graph_p);
    tr::net::transport_can tcan_p(std::make_unique<fake_link_t>(bus),
                                  {0, 5, tr::view::can_frame_mode_t::CLASSIC, "boardB"});
    router_p.add_child("can0", tcan_p);

    // ----- bystander Q (CAN node 7): same bus, must never deliver n5 traffic. -
    std::atomic<int> q_deliveries{0};
    tr::net::transport_can tcan_q(std::make_unique<fake_link_t>(bus),
                                  {0, 7, tr::view::can_frame_mode_t::CLASSIC, "boardC"});
    tcan_q.set_receiver([&](std::span<const std::byte>) { ++q_deliveries; });

    // ----- 1) local enumeration: /net/can0:children[] == {n5, n7}. ------------
    const bool heard = wait_until(
        [&] {
            return enumerate_local(graph_t_, "/net/can0:children[]") ==
                   std::set<std::string>{"n5", "n7"};
        },
        kBudget);
    check(heard, "reading /net/can0:children[] synthesizes exactly {n5, n7} (join hellos)");

    // No vertex was created for any peer — the transit graph holds only what the
    // test created (ADR-0044 §1: peers never mutate any node's graph).
    check(graph_t_.find(path_t::parse("/n5")->key()) == nullptr &&
              graph_t_.find(path_t::parse("/net/can0/n5")->key()) == nullptr,
          "no vertex exists for peer n5 anywhere on the transit node");
    check(enumerate_local(graph_t_, "/net:children[]") == std::set<std::string>{"can0"},
          "/net:children[] (generic member read) still lists only the connection");

    // ----- 2) remote enumeration: FWD{READ /net/can0 :children[]} from the client.
    channel.b().send(
        b_fwd(fwd_op_t::READ, b_path({"net", "can0"}), b_path({"reply-ep"}), b_field_children()));
    const auto r_enum = inbox.wait(kBudget);
    check(r_enum.has_value(), "client received a REPLY for the remote :children[] READ");
    if (r_enum) {
        const auto dec = tr::wire::decode(*r_enum);
        check(dec && dec->type == type_t::FWD && dec->children.size() == 5 &&
                  value_u8(dec->children[3]) == static_cast<std::uint8_t>(reply_kind_t::RESULT) &&
                  dec->children[4].type == type_t::POINT &&
                  member_names(dec->children[4]) == std::set<std::string>{"n5", "n7"},
              "the REPLY carries POINT{ POINT{NAME n5}, POINT{NAME n7} }");
    }

    // ----- 3) FWD READ through: /n5/a/b resolves at P, reply routes back. -----
    channel.b().send(b_fwd(fwd_op_t::READ, b_path({"n5", "a", "b"}), b_path({"reply-ep"})));
    const auto r_read = inbox.wait(kBudget);
    check(r_read.has_value(), "client received a REPLY for the forwarded READ via peer n5");
    if (r_read) {
        const auto dec = tr::wire::decode(*r_read);
        check(dec && dec->type == type_t::FWD && dec->children.size() == 5,
              "REPLY decodes as FWD (op,dst,src,kind,value)");
        if (dec && dec->children.size() == 5) {
            const tlv_t& r = *dec;
            check(value_u8(r.children[3]) == static_cast<std::uint8_t>(reply_kind_t::RESULT),
                  "kind == RESULT");
            check(tr::wire::equal(r.children[1], *tr::wire::decode(b_path({"reply-ep"}))),
                  "REPLY dst fully consumed to /reply-ep at the client");
            check(r.children[4].type == type_t::VALUE && r.children[4].payload.size() == 4 &&
                      tr::detail::load_le<std::uint32_t>(r.children[4].payload) == kStored,
                  "REPLY payload == P's stored VALUE (byte-exact through the bus)");
        }
    }
    check(q_deliveries.load() == 0,
          "bystander Q delivered NOTHING (directed groups skip non-addressed peers)");

    // ----- 4) FWD WRITE through: the peer's LKV updates, RESULT acks. ---------
    const std::uint32_t kWritten = 0x0BADF00Du;
    {
        std::vector<std::byte> body;
        const std::byte ob{static_cast<std::uint8_t>(fwd_op_t::WRITE)};
        tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&ob, 1));
        const std::vector<std::byte> dst = b_path({"n5", "a", "b"});
        const std::vector<std::byte> src = b_path({"reply-ep"});
        const std::vector<std::byte> payload = b_value_u32(kWritten);
        body.insert(body.end(), dst.begin(), dst.end());
        body.insert(body.end(), src.begin(), src.end());
        body.insert(body.end(), payload.begin(), payload.end());
        std::vector<std::byte> out;
        tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
        channel.b().send(out);
    }
    const auto r_write = inbox.wait(kBudget);
    check(r_write.has_value(), "client received a REPLY for the forwarded WRITE via n5");
    if (r_write) {
        const auto dec = tr::wire::decode(*r_write);
        check(dec && dec->children.size() == 4 &&
                  value_u8(dec->children[3]) == static_cast<std::uint8_t>(reply_kind_t::RESULT),
              "WRITE REPLY kind == RESULT (no payload)");
    }
    const auto stored = graph_p.read(vp);
    std::optional<tlv_t> inner;
    if (stored) {
        if (auto t = tr::wire::view_as_tlv(stored->only())) inner = std::move(*t);
    }
    check(inner && inner->type == type_t::VALUE && inner->payload.size() == 4 &&
              tr::detail::load_le<std::uint32_t>(inner->payload) == kWritten,
          "P's /a/b LKV updated to the forwarded value (byte-exact)");

    // Statelessness of the transit: still no peer vertices, no label state.
    check(graph_t_.find(path_t::parse("/n5")->key()) == nullptr,
          "transit graph still untouched after forwarded READ+WRITE");
}

// ---------------------------------------------------------------------------
// Expiry: a silent peer leaves the enumeration after peer_ttl.
// ---------------------------------------------------------------------------

void test_peer_expiry() {
    std::printf("ADR-0044 expiry (silent peer leaves the listing):\n");

    fake_can_bus_t bus;
    tr::net::transport_can observer(
        std::make_unique<fake_link_t>(bus),
        {0, 1, tr::view::can_frame_mode_t::CLASSIC, "obs", std::chrono::milliseconds(150)});

    std::optional<tr::net::transport_can> ghost;
    ghost.emplace(
        std::make_unique<fake_link_t>(bus),
        tr::net::transport_can_config_t{0, 9, tr::view::can_frame_mode_t::CLASSIC, "ghost"});

    const auto names = [&] {
        std::set<std::string> out;
        observer.enumerate_peers([&](std::string_view p) { out.emplace(p); });
        return out;
    };
    check(wait_until([&] { return names() == std::set<std::string>{"n9"}; }, kBudget),
          "the announcing peer n9 is enumerated");
    ghost.reset();  // silence (a rebooted/unplugged board)
    check(wait_until([&] { return names().empty(); }, kBudget),
          "after peer_ttl of silence the peer expires from the enumeration");
}

// ---------------------------------------------------------------------------
// Per-distinct-node growth: many announcers all enumerate; re-announce is idempotent.
// ---------------------------------------------------------------------------

void test_peer_table_growth() {
    std::printf("ADR-0044 peer table (per-distinct-node growth, idempotent refresh):\n");

    fake_can_bus_t bus;
    tr::net::transport_can observer(std::make_unique<fake_link_t>(bus),
                                    {0, 1, tr::view::can_frame_mode_t::CLASSIC, "obs"});

    // A raw injector announces hellos from many distinct nodes — TWICE, so a
    // re-announce refreshes its entry instead of growing the table.
    fake_link_t injector(bus);
    const std::uint16_t first = 100;
    const std::uint16_t last = 120;  // 21 nodes
    const auto flood = [&] {
        for (std::uint16_t node = first; node <= last; ++node) {
            can::advertise_t hello;
            hello.can_id = can::encode_can_id({0, node, tr::net::kCanControlEndpoint});
            hello.slice_count = 0;  // presence only
            const std::vector<std::byte> bytes = can::encode_advertise(hello);
            std::size_t off = 0;
            while (off < bytes.size()) {
                const std::size_t n =
                    std::min<std::size_t>(tr::view::kCanClassicMaxData, bytes.size() - off);
                tr::net::can_frame_data_t f;
                f.id = can::encode_can_id({0, node, tr::net::kCanControlEndpoint});
                f.fd = false;
                f.len = static_cast<std::uint8_t>(n);
                std::memcpy(f.data.data(), bytes.data() + off, n);
                injector.write_raw(f);
                off += n;
            }
        }
    };
    flood();
    flood();

    const auto names = [&] {
        std::set<std::string> out;
        observer.enumerate_peers([&](std::string_view p) { out.emplace(p); });
        return out;
    };
    std::set<std::string> expect;
    for (std::uint16_t node = first; node <= last; ++node)
        expect.insert("n" + std::to_string(node));
    check(wait_until([&] { return names() == expect; }, kBudget),
          "all 21 distinct announcers are enumerated (no artificial capacity)");
    check(names().size() == expect.size(),
          "re-announcing grew nothing — one entry per distinct node id");
}

}  // namespace

int main() {
    test_enumeration_and_forwarding();
    test_peer_expiry();
    test_peer_table_growth();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
