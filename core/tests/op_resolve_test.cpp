/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0004 / ADR-0035 — op_resolver_t host tests, over the ADR-0041 terminus
 * arena. A node arena-decodes a FWD, resolves it against a LOCAL vertex, applies
 * READ / WRITE / AWAIT (+ a FIELD :field selector), and builds the FWD{REPLY} as
 * a zero-copy rope. The load-bearing check (like graph_test's "read is a clone")
 * is that the reply payload SHARES the vertex's stored segment — proven via
 * use_count + segment-pointer identity on the rope links, before any flatten.
 * The AWAIT cases exercise the waiter/condvar path under TSan. New ADR-0041
 * cases: a CRC-carrying WRITE stores trailer-LESS bytes (§4), and a
 * non-canonical dst PATH still resolves via the path_key re-emit fallback (§3).
 * Input FWDs are built via the codec (round-trip-safe), replies decoded back.
 */

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory_resource>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::op_resolver_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::wire::opt_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

// --- wire builders (canonical bytes via the production emit helpers) ---------
std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::detail::emit_name(out, s);
    return out;
}
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) {
        const std::vector<std::byte> n = b_name(s);
        body.insert(body.end(), n.begin(), n.end());
    }
    std::vector<std::byte> out;
    tr::detail::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}
std::vector<std::byte> b_value(std::span<const std::byte> p) {
    std::vector<std::byte> out;
    tr::detail::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}
std::vector<std::byte> b_value(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> p;
    for (std::uint8_t b : bytes) p.push_back(std::byte{b});
    return b_value(p);
}
std::vector<std::byte> b_subscriber(std::initializer_list<std::string_view> target) {
    std::vector<std::byte> out;
    tr::detail::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, b_path(target));
    return out;
}
void append(std::vector<std::byte>& dst, const std::vector<std::byte>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

// Assemble a FWD frame (RFC-0004 §B child order). `selector`/`payload` optional.
std::vector<std::byte> b_fwd(fwd_op_t op, const std::vector<std::byte>& dst,
                             const std::vector<std::byte>& src,
                             const std::vector<std::byte>& selector = {},
                             const std::vector<std::byte>& payload = {}) {
    std::vector<std::byte> body;
    append(body, b_value({static_cast<std::uint8_t>(op)}));
    append(body, dst);
    if (!selector.empty()) append(body, selector);
    append(body, src);
    if (!payload.empty()) append(body, payload);
    std::vector<std::byte> out;
    tr::detail::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

// Arena-decode + resolve (ADR-0041): mirrors fwd_router_t's terminus wiring —
// decode_into from the (default) resource, resolve over the arena.
tr::graph::result_t<tr::view::rope_t> resolve_bytes(op_resolver_t& resolver,
                                                    std::span<const std::byte> fwd,
                                                    std::string_view inbound_link = {}) {
    const auto arena = tr::wire::decode_into(fwd, *std::pmr::get_default_resource());
    if (!arena) return std::unexpected(tr::graph::status_t::INVALID_PATH);
    return resolver.resolve(*arena, inbound_link);
}

// A view_t over a fresh owned heap segment holding `bytes` (graph_test idiom).
tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

// A decoded reply: the flattened backing view kept alongside the tlv whose spans
// borrow it (the tlv must not outlive the view).
struct decoded_reply_t {
    tr::view::view_t flat;
    tlv_t tlv;
};

// Decode a reply rope (flatten then decode — the one allowed copy, at the consumer).
decoded_reply_t decode_reply(const tr::view::rope_t& reply) {
    tr::view::view_t flat = reply.flatten();
    const auto dec = tr::wire::decode(flat.bytes());
    return decoded_reply_t{std::move(flat), dec ? *dec : tlv_t{}};
}

std::uint8_t value_u8(const tlv_t& v) { return tr::detail::load_le<std::uint8_t>(v.payload); }

// ---------------------------------------------------------------------------
void test_read_zero_copy() {
    std::printf("READ a STORED_VALUE -> reply payload shares the stored segment (zero-copy):\n");
    graph_t g;
    op_resolver_t resolver(g);
    const auto path = path_t::parse("/sensor/temp");
    tr::graph::vertex_t* v = *g.register_vertex(*path, role_t::STORED_VALUE);

    const std::vector<std::byte> val = b_value({0xD2, 0x04, 0x00, 0x00});  // VALUE u32=1234
    (void)g.write(v, make_value(val));

    // Reference handle on the stored segment (LKV + this = use_count 2).
    const auto stored = g.read(v);
    check(stored.has_value() && stored->owner.use_count() == 2, "stored LKV use_count == 2");

    const auto fwd = b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"reply-ep"}));
    auto reply = resolve_bytes(resolver, fwd);
    check(reply.has_value(), "resolve READ produced a reply");

    const auto& links = reply->links();
    check(links.size() == 2, "reply rope = fresh head + 1 roped payload link");
    const tr::view::view_t& payload_link = links.back();
    check(payload_link.owner.get() == stored->owner.get(),
          "reply payload link SHARES the stored segment (segment-pointer identity)");
    check(stored->owner.use_count() == 3,
          "stored segment refcount bumped by the reply rope (LKV+ref+reply == 3), no copy");

    const auto dr = decode_reply(*reply);
    const tlv_t& r = dr.tlv;
    check(r.type == type_t::FWD && r.children.size() == 5, "reply decodes to a 5-child FWD");
    check(value_u8(r.children[0]) == static_cast<std::uint8_t>(fwd_op_t::REPLY), "op == REPLY");
    check(r.children[3].type == type_t::VALUE &&
              value_u8(r.children[3]) == static_cast<std::uint8_t>(reply_kind_t::RESULT),
          "kind == RESULT");
    // reply dst == request src (/reply-ep); reply src == responder ep (/sensor/temp).
    check(tr::wire::equal(r.children[1], *tr::wire::decode(b_path({"reply-ep"}))),
          "reply dst == request src (/reply-ep)");
    check(tr::wire::equal(r.children[2], *tr::wire::decode(b_path({"sensor", "temp"}))),
          "reply src == responder endpoint (/sensor/temp)");
    check(r.children[4].type == type_t::VALUE && r.children[4].payload.size() == 4 &&
              tr::detail::load_le<std::uint32_t>(r.children[4].payload) == 1234,
          "reply payload decodes to the written VALUE u32=1234");
}

void test_write() {
    std::printf("WRITE a VALUE -> reply kind=RESULT, vertex LKV updated:\n");
    graph_t g;
    op_resolver_t resolver(g);
    const auto path = path_t::parse("/sensor/temp");
    tr::graph::vertex_t* v = *g.register_vertex(*path, role_t::STORED_VALUE);

    const auto fwd = b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"reply-ep"}), {},
                           b_value({0x2A}));
    auto reply = resolve_bytes(resolver, fwd);
    check(reply.has_value(), "resolve WRITE produced a reply");
    const auto dr = decode_reply(*reply);
    const tlv_t& r = dr.tlv;
    check(r.type == type_t::FWD && r.children.size() == 4, "WRITE reply = 4 children (no payload)");
    check(value_u8(r.children[3]) == static_cast<std::uint8_t>(reply_kind_t::RESULT),
          "WRITE reply kind == RESULT");
    const auto rd = g.read(v);
    check(rd.has_value() && rd->bytes().size() == 5 /*VALUE 01 00 01 00 2A*/,
          "vertex LKV updated by the WRITE");
    const auto inner = tr::wire::view_as_tlv(*rd);
    check(inner && inner->type == type_t::VALUE && value_u8(*inner) == 0x2A,
          "stored value decodes to the written byte 0x2A");
}

void test_await() {
    std::printf("AWAIT -> next write; and a timeout -> ERROR(TIMEOUT):\n");
    graph_t g;
    op_resolver_t resolver(g);
    const auto path = path_t::parse("/sensor/temp");
    tr::graph::vertex_t* v = *g.register_vertex(*path, role_t::STORED_VALUE);

    // 5s await; a writer thread supplies the next value after a short delay.
    std::vector<std::byte> tobuf(8);
    tr::detail::store_le<std::uint64_t>(tobuf, 5'000'000'000ull);  // 5s
    const auto fwd = b_fwd(fwd_op_t::AWAIT, b_path({"sensor", "temp"}), b_path({"reply-ep"}), {},
                           b_value(tobuf));
    std::thread writer([&] {
        std::this_thread::sleep_for(40ms);
        (void)g.write(v, make_value(b_value({0x7B})));  // VALUE u8=123
    });
    auto reply = resolve_bytes(resolver, fwd);
    writer.join();
    check(reply.has_value(), "resolve AWAIT returned");
    const auto dr = decode_reply(*reply);
    const tlv_t& r = dr.tlv;
    check(value_u8(r.children[3]) == static_cast<std::uint8_t>(reply_kind_t::RESULT),
          "AWAIT reply kind == RESULT");
    check(r.children.size() == 5 && r.children[4].type == type_t::VALUE &&
              value_u8(r.children[4]) == 0x7B,
          "AWAIT reply payload == the next write (VALUE u8=123)");

    // Timeout path: 1ms deadline, no writer.
    std::vector<std::byte> tbuf(8);
    tr::detail::store_le<std::uint64_t>(tbuf, 1'000'000ull);  // 1ms
    const auto fwd_to =
        b_fwd(fwd_op_t::AWAIT, b_path({"sensor", "temp"}), b_path({"reply-ep"}), {}, b_value(tbuf));
    auto reply_to = resolve_bytes(resolver, fwd_to);
    const auto drto = decode_reply(*reply_to);
    const tlv_t& rto = drto.tlv;
    check(value_u8(rto.children[3]) == static_cast<std::uint8_t>(reply_kind_t::ERROR),
          "AWAIT timeout reply kind == ERROR");
    check(rto.children[4].type == type_t::STATUS && rto.children[4].children.size() == 1 &&
              rto.children[4].children[0].type == type_t::ERROR &&
              value_u8(rto.children[4].children[0]) == 0x08 /*TIMEOUT*/,
          "ERROR payload == STATUS{ ERROR(TIMEOUT=0x08) }");
}

void test_subscribers_field() {
    std::printf(":subscribers[] — WRITE a SUBSCRIBER, then READ the array (rope of slot views):\n");
    graph_t g;
    op_resolver_t resolver(g);
    const auto path = path_t::parse("/sensor/temp");
    tr::graph::vertex_t* v = *g.register_vertex(*path, role_t::STORED_VALUE);

    // FIELD :subscribers[] (append): NAME "subscribers", VALUE u8 index_mode=ELEMENT.
    std::vector<std::byte> field_append;
    {
        std::vector<std::byte> body = b_name("subscribers");
        append(body, b_value({0x01}));  // index_mode=ELEMENT, no index => "[]"
        tr::detail::emit_tlv(field_append, type_t::FIELD, opt_t{.pl = true}, body);
    }
    // Two subscribes (distinct targets) to assert slot order.
    for (const char* tgt : {"sub-a", "sub-b"}) {
        const auto wfwd = b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"reply-ep"}),
                                field_append, b_subscriber({tgt}));
        auto wr = resolve_bytes(resolver, wfwd);
        const auto dwr = decode_reply(*wr);
        const tlv_t& r = dwr.tlv;
        check(value_u8(r.children[3]) == static_cast<std::uint8_t>(reply_kind_t::RESULT),
              std::string("subscribe ") + tgt + " => RESULT");
    }

    // Reference handles on the stored slot segments (slot order).
    const auto refsubs = g.read_subscribers(v);
    check(refsubs.has_value() && refsubs->size() == 2, "two populated subscriber slots");

    const auto rfwd =
        b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"reply-ep"}), field_append);
    auto reply = resolve_bytes(resolver, rfwd);
    check(reply.has_value(), "resolve READ :subscribers[] produced a reply");

    // Rope: [head (incl. POINT wrapper header)] + [slot0 view] + [slot1 view].
    const auto& links = reply->links();
    check(links.size() == 3, "reply rope = head + N=2 roped slot links");
    check(links[1].owner.get() == (*refsubs)[0].owner.get() &&
              links[2].owner.get() == (*refsubs)[1].owner.get(),
          "slot links SHARE the stored SUBSCRIBER segments in slot order (zero-copy)");

    const auto dr = decode_reply(*reply);
    const tlv_t& r = dr.tlv;
    check(value_u8(r.children[3]) == static_cast<std::uint8_t>(reply_kind_t::RESULT),
          ":subscribers[] read kind == RESULT");
    const tlv_t& wrapper = r.children[4];
    check(wrapper.type == type_t::POINT && wrapper.opt.pl && wrapper.children.size() == 2,
          "payload is a PL=1 wrapper with 2 SUBSCRIBER children");
    check(wrapper.children[0].type == type_t::SUBSCRIBER &&
              wrapper.children[1].type == type_t::SUBSCRIBER,
          "wrapper children are SUBSCRIBER TLVs");
    check(tr::wire::equal(wrapper.children[0], *tr::wire::decode(b_subscriber({"sub-a"}))) &&
              tr::wire::equal(wrapper.children[1], *tr::wire::decode(b_subscriber({"sub-b"}))),
          "slot order preserved: sub-a then sub-b");
}

void test_write_trailer_sliced() {
    std::printf("WRITE with a CRC trailer -> stored bytes are trailer-LESS (ADR-0041 §4):\n");
    graph_t g;
    op_resolver_t resolver(g);
    const auto path = path_t::parse("/sensor/temp");
    tr::graph::vertex_t* v = *g.register_vertex(*path, role_t::STORED_VALUE);

    // A VALUE carrying a CRC-32C trailer, as a foreign producer might send it.
    tlv_t val;
    val.type = type_t::VALUE;
    val.opt.cr = true;
    const std::array<std::byte, 2> pb{std::byte{0xBE}, std::byte{0xEF}};
    val.payload = pb;
    const std::vector<std::byte> val_bytes = tr::wire::encode(val);
    check(val_bytes.size() == 4 + 2 + 4, "input VALUE carries a 4-byte CRC trailer");

    const auto fwd =
        b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"reply-ep"}), {}, val_bytes);
    auto reply = resolve_bytes(resolver, fwd);
    check(reply.has_value(), "resolve WRITE(trailered VALUE) produced a reply");
    const auto dr = decode_reply(*reply);
    check(value_u8(dr.tlv.children[3]) == static_cast<std::uint8_t>(reply_kind_t::RESULT),
          "trailered WRITE reply kind == RESULT");

    // Stored-at-rest: header + body only (6 bytes), opt trailer bits cleared,
    // and the stored TLV is self-consistent (decodes with no trailer).
    const auto rd = g.read(v);
    check(rd.has_value() && rd->bytes().size() == 6,
          "stored LKV excludes the trailer (6 bytes, not 10)");
    const auto inner = tr::wire::view_as_tlv(*rd);
    check(inner.has_value() && inner->type == type_t::VALUE && !inner->opt.cr &&
              !inner->trailer.has_value() && inner->payload.size() == 2,
          "stored value decodes trailer-less with the CR bit cleared");
}

void test_non_canonical_dst() {
    std::printf("non-canonical dst PATH (LL-widened NAME) -> path_key fallback resolves:\n");
    graph_t g;
    op_resolver_t resolver(g);
    const auto path = path_t::parse("/sensor/temp");
    (void)g.register_vertex(*path, role_t::STORED_VALUE);
    const std::vector<std::byte> val = b_value({0x2A});
    (void)g.write(g.find(path->key()), make_value(val));

    // A dst PATH whose NAMEs use a widened (LL) length — legal wire, but NOT
    // byte-identical to the canonical vertex key, so the span-aliased lookup
    // (ADR-0041 §3) must fall back to the re-emit and still find the vertex.
    tlv_t dst;
    dst.type = type_t::PATH;
    dst.opt.pl = true;
    for (const std::string_view seg : {std::string_view("sensor"), std::string_view("temp")}) {
        tlv_t n;
        n.type = type_t::NAME;
        n.opt.ll = true;  // widened length ⇒ non-canonical header
        n.payload =
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(seg.data()), seg.size());
        dst.children.push_back(n);
    }
    const auto fwd = b_fwd(fwd_op_t::READ, tr::wire::encode(dst), b_path({"reply-ep"}));
    auto reply = resolve_bytes(resolver, fwd);
    check(reply.has_value(), "resolve READ with LL-widened dst produced a reply");
    const auto dr = decode_reply(*reply);
    check(value_u8(dr.tlv.children[3]) == static_cast<std::uint8_t>(reply_kind_t::RESULT),
          "non-canonical dst resolves via the re-emit fallback (kind == RESULT)");
    check(dr.tlv.children.size() == 5 && value_u8(dr.tlv.children[4]) == 0x2A,
          "reply payload is the stored value (0x2A)");
}

std::vector<std::byte> read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    return out;
}

void test_wildcard_and_not_local() {
    std::printf("[*] on a data target -> INVALID_PATH; dst not local -> NOT_FOUND:\n");
    graph_t g;
    op_resolver_t resolver(g);
    const auto path = path_t::parse("/sensor/temp");
    (void)g.register_vertex(*path, role_t::STORED_VALUE);

    // The fwd-wildcard-reject conformance vector: a valid FWD whose FIELD carries a
    // [*] (index_mode=WILDCARD) on a non-subscriber path => must reject INVALID_PATH.
    const std::filesystem::path vroot{LIBTRACER_VECTORS_DIR};
    const auto wild = read_file(vroot / "fwd" / "fwd-wildcard-reject" / "input.bin");
    const auto wdec = tr::wire::decode(wild);
    check(wdec.has_value(), "fwd-wildcard-reject vector decodes (codec round-trip-safe)");
    check(tr::wire::encode(*wdec) == wild, "vector re-encodes byte-exactly (3-core machine green)");
    auto wreply = resolve_bytes(resolver, wild);
    check(wreply.has_value(), "wildcard resolve produced a reply (not a hard error)");
    const auto dwild = decode_reply(*wreply);
    const tlv_t& wr = dwild.tlv;
    check(value_u8(wr.children[3]) == static_cast<std::uint8_t>(reply_kind_t::ERROR),
          "[*] on a data target => kind=ERROR");
    check(wr.children[4].type == type_t::STATUS && wr.children[4].children.size() == 1 &&
              value_u8(wr.children[4].children[0]) == 0x03 /*INVALID_PATH*/,
          "ERROR payload == STATUS{ ERROR(INVALID_PATH=0x03) }");

    // dst not local: an unregistered path => NOT_FOUND.
    const auto nfwd = b_fwd(fwd_op_t::READ, b_path({"nope", "missing"}), b_path({"reply-ep"}));
    auto nreply = resolve_bytes(resolver, nfwd);
    const auto dnr = decode_reply(*nreply);
    const tlv_t& nr = dnr.tlv;
    check(value_u8(nr.children[3]) == static_cast<std::uint8_t>(reply_kind_t::ERROR),
          "non-local dst => kind=ERROR");
    check(nr.children[4].type == type_t::STATUS && nr.children[4].children.size() == 1 &&
              value_u8(nr.children[4].children[0]) == 0x01 /*NOT_FOUND*/,
          "ERROR payload == STATUS{ ERROR(NOT_FOUND=0x01) }");
}

}  // namespace

int main() {
    test_read_zero_copy();
    test_write();
    test_await();
    test_subscribers_field();
    test_write_trailer_sliced();
    test_non_canonical_dst();
    test_wildcard_and_not_local();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
