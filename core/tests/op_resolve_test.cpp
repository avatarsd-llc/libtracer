/**
 * @file
 * @brief RFC-0004 / ADR-0035 — op_resolver_t host tests, over the ADR-0041 terminus arena.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A node arena-decodes a FWD, resolves it against a LOCAL vertex, applies
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

#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

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

using tr::testing::check;
using tr::testing::make_value;

// --- wire builders (canonical bytes via the production emit helpers) ---------
std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, s);
    return out;
}
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) {
        const std::vector<std::byte> n = b_name(s);
        body.insert(body.end(), n.begin(), n.end());
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}
std::vector<std::byte> b_value(std::span<const std::byte> p) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}
std::vector<std::byte> b_value(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> p;
    for (std::uint8_t b : bytes) p.push_back(std::byte{b});
    return b_value(p);
}
std::vector<std::byte> b_subscriber(std::initializer_list<std::string_view> target) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, b_path(target));
    return out;
}
void append(std::vector<std::byte>& dst, const std::vector<std::byte>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

using tr::testing::b_fwd;

/**
 * @brief Arena-decode + resolve (ADR-0041): mirrors fwd_router_t's terminus wiring — decode_into
 *        from the (default) resource, resolve over the arena.
 */
tr::graph::result_t<tr::view::rope_t> resolve_bytes(op_resolver_t& resolver,
                                                    std::span<const std::byte> fwd,
                                                    std::string_view inbound_link = {}) {
    const auto arena = tr::wire::decode_into(fwd, tr::mem::heap_source());
    if (!arena) return std::unexpected(tr::graph::status_t::INVALID_PATH);
    return resolver.resolve(*arena, inbound_link);
}

/**
 * @brief A decoded reply: the flattened backing view kept alongside the tlv whose spans borrow it
 *        (the tlv must not outlive the view).
 */
struct decoded_reply_t {
    tr::view::view_t flat;
    tlv_t tlv;
};

/** @brief Decode a reply rope (flatten then decode — the one allowed copy, at the consumer). */
decoded_reply_t decode_reply(const tr::view::rope_t& reply) {
    tr::view::view_t flat = reply.flatten();
    const auto dec = tr::wire::decode(flat.bytes());
    return decoded_reply_t{std::move(flat), dec ? *dec : tlv_t{}};
}

std::uint8_t value_u8(const tlv_t& v) { return tr::detail::load_le<std::uint8_t>(v.payload); }

/**
 * @brief The registered u16 error code of a STATUS{ ERROR{ VALUE u16 LE } } payload (RFC-0002 §C) —
 *        0 when the shape doesn't match.
 */
std::uint16_t status_error_code(const tlv_t& status) {
    if (status.type != type_t::STATUS || status.children.size() != 1) return 0;
    const tlv_t& err = status.children[0];
    if (err.type != type_t::ERROR || !err.opt.pl || err.children.empty()) return 0;
    const tlv_t& id = err.children[0];
    if (id.type != type_t::VALUE || id.payload.size() != 2) return 0;
    return tr::detail::load_le<std::uint16_t>(id.payload);
}

// ---------------------------------------------------------------------------
void test_read_zero_copy() {
    std::printf("READ a STORED_VALUE -> reply payload shares the stored segment (zero-copy):\n");
    graph_t g;
    op_resolver_t resolver(g);
    const auto path = path_t::parse("/sensor/temp");
    tr::graph::vertex_handle_t v = g.register_vertex(*path, role_t::STORED_VALUE);

    const std::vector<std::byte> val = b_value({0xD2, 0x04, 0x00, 0x00});  // VALUE u32=1234
    (void)g.write(v, make_value(val));

    // A read now REFERENCES the published rope instead of cloning it, so holding this result
    // does not move the segment's refcount: the LKV's own reference is the only one. (It read
    // `== 2` while a read copied the rope out.)
    const auto stored = g.read(v);
    check(stored.has_value() && (*stored)->only().owner.use_count() == 1,
          "holding a read result does not bump the stored segment");

    const auto fwd = b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"reply-ep"}));
    auto reply = resolve_bytes(resolver, fwd);
    check(reply.has_value(), "resolve READ produced a reply");

    const auto& links = reply->links();
    check(links.size() == 2, "reply rope = fresh head + 1 roped payload link");
    const tr::view::view_t& payload_link = links.back();
    check(payload_link.owner.get() == (*stored)->only().owner.get(),
          "reply payload link SHARES the stored segment (segment-pointer identity)");
    // The property this test exists for is unchanged and is the one above: the reply's payload
    // link IS the stored segment, not a copy of it. What moved is the arithmetic — the reply
    // rope adds the only additional reference, because the read no longer adds one of its own.
    check((*stored)->only().owner.use_count() == 2,
          "stored segment refcount bumped by the reply rope alone (LKV+reply == 2), no copy");

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
    tr::graph::vertex_handle_t v = g.register_vertex(*path, role_t::STORED_VALUE);

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
    check(rd.has_value() && (*rd)->only().bytes().size() == 5 /*VALUE 01 00 01 00 2A*/,
          "vertex LKV updated by the WRITE");
    const auto inner = tr::wire::decode((*rd)->only());
    check(inner && inner->type == type_t::VALUE && value_u8(*inner) == 0x2A,
          "stored value decodes to the written byte 0x2A");
}

void test_await() {
    std::printf("AWAIT -> next write; and a timeout -> ERROR(TIMEOUT):\n");
    graph_t g;
    op_resolver_t resolver(g);
    const auto path = path_t::parse("/sensor/temp");
    tr::graph::vertex_handle_t v = g.register_vertex(*path, role_t::STORED_VALUE);

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
    check(status_error_code(rto.children[4]) == 0x0041 /*tr::flow::timeout*/,
          "ERROR payload == STATUS{ ERROR{ VALUE u16=0x0041 tr::flow::timeout } }");
}

/**
 * @brief #585: an AWAIT carrying a FIELD selector answers SCHEMA_NOT_FOUND instead of
 *        silently awaiting the whole vertex.
 *
 * Pre-fix, the selector was decoded, validated, and then DISCARDED, so `await <v>:<anything>`
 * was byte-identical to `await <v>` — a peer asking to be woken on one facet got the vertex,
 * or `tr::flow::timeout`, which it cannot tell from a quiet link.
 *
 * Verified against a build with the guard deleted: exactly TWO assertions flip — the two
 * status-CODE checks, which see 0x0041 `tr::flow::timeout` there instead of 0x0031. Read those
 * two. The `-> ERROR` checks beside them pass on the broken build too (a timeout is also an
 * ERROR reply), and the field-less case is an explicit CONTROL that must pass both ways,
 * pinning that ordinary await is untouched.
 */
void test_await_field_selector_is_enotty() {
    std::printf("AWAIT + :field -> ERROR(SCHEMA_NOT_FOUND), never a silent vertex await:\n");
    graph_t g;
    op_resolver_t resolver(g);
    const auto path = path_t::parse("/sensor/temp");
    (void)g.register_vertex(*path, role_t::STORED_VALUE);

    std::vector<std::byte> tbuf(8);
    tr::detail::store_le<std::uint64_t>(tbuf, 1'000'000ull);  // 1ms, so the control is quick

    const auto await_with = [&](const std::vector<std::byte>& sel) {
        const auto f = b_fwd(fwd_op_t::AWAIT, b_path({"sensor", "temp"}), b_path({"reply-ep"}), sel,
                             b_value(tbuf));
        auto reply = resolve_bytes(resolver, f);
        check(reply.has_value(), "resolve AWAIT returned");
        return decode_reply(*reply);
    };
    const auto field_named = [](const char* name) {
        std::vector<std::byte> out;
        tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, b_name(name));
        return out;
    };

    // A field that does not exist for ANY verb.
    {
        const auto d = await_with(field_named("zzz_no_such_field"));
        const tlv_t& r = d.tlv;
        check(value_u8(r.children[3]) == static_cast<std::uint8_t>(reply_kind_t::ERROR),
              "AWAIT :zzz_no_such_field -> ERROR");
        check(status_error_code(r.children[4]) == 0x0031 /*tr::schema::not_found*/,
              "... and the code is SCHEMA_NOT_FOUND, not flow::timeout");
    }

    // A field that DOES exist for read and write. Still ENOTTY: the field is real, the
    // await surface is not (RFC-0010 §C). This is what separates "unknown field" from
    // "await has no field surface" — the fix is the second, not the first.
    {
        const auto d = await_with(field_named("subscribers"));
        const tlv_t& r = d.tlv;
        check(value_u8(r.children[3]) == static_cast<std::uint8_t>(reply_kind_t::ERROR),
              "AWAIT :subscribers -> ERROR (the field is real; the await is not)");
        check(status_error_code(r.children[4]) == 0x0031 /*tr::schema::not_found*/,
              "... SCHEMA_NOT_FOUND, though READ/WRITE serve :subscribers fine");
    }

    // CONTROL — passes with or without the fix. Ordinary await must still time out.
    {
        const auto d = await_with({});
        const tlv_t& r = d.tlv;
        check(status_error_code(r.children[4]) == 0x0041 /*tr::flow::timeout*/,
              "control: a field-less AWAIT still answers flow::timeout");
    }
}

void test_subscribers_field() {
    std::printf(":subscribers[] — WRITE a SUBSCRIBER, then READ the array (rope of slot views):\n");
    graph_t g;
    op_resolver_t resolver(g);
    const auto path = path_t::parse("/sensor/temp");
    tr::graph::vertex_handle_t v = g.register_vertex(*path, role_t::STORED_VALUE);

    // FIELD :subscribers[] (append): NAME "subscribers", VALUE u8 index_mode=ELEMENT.
    std::vector<std::byte> field_append;
    {
        std::vector<std::byte> body = b_name("subscribers");
        append(body, b_value({0x01}));  // index_mode=ELEMENT, no index => "[]"
        tr::wire::emit_tlv(field_append, type_t::FIELD, opt_t{.pl = true}, body);
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
    tr::graph::vertex_handle_t v = g.register_vertex(*path, role_t::STORED_VALUE);

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
    check(rd.has_value() && (*rd)->only().bytes().size() == 6,
          "stored LKV excludes the trailer (6 bytes, not 10)");
    const auto inner = tr::wire::decode((*rd)->only());
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
    (void)g.write(*g.find(path->key()), make_value(val));

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

/**
 * @brief ADR-0042 §3 — the owner-declared `pin_payload_ratio` referenced WRITE store: a view-
 *        delivered frame's big trailer-less payload stores as a SUBVIEW of the frame (segment-
 *        pointer identity, zero copy, frame pinned); the default (0), a small payload, a trailered
 *        payload, and a span-delivered frame all keep the ADR-0041 one-copy trailer-sliced store.
 */
void test_pin_payload_ratio_store() {
    std::printf("pin_payload_ratio — referenced vs copied WRITE store (ADR-0042 §3):\n");
    graph_t g;
    op_resolver_t resolver(g);
    const auto path = path_t::parse("/sensor/blob");
    tr::graph::vertex_handle_t v = g.register_vertex(*path, role_t::STORED_VALUE);

    // A 32-byte payload => a 36-byte trailer-less VALUE TLV on the wire.
    std::vector<std::byte> big(32);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::byte>(i);
    const std::vector<std::byte> big_tlv = b_value(big);
    const auto fwd_big =
        b_fwd(fwd_op_t::WRITE, b_path({"sensor", "blob"}), b_path({"reply-ep"}), {}, big_tlv);

    // The frame as an OWNING view (what a view-delivering transport hands up).
    tr::view::view_t frame = make_value(fwd_big);

    // Default threshold 0 => referencing DISABLED: stored segment differs (copied).
    {
        const auto arena = tr::wire::decode_into(frame.bytes(), tr::mem::heap_source());
        auto reply = resolver.resolve(*arena, {}, &frame);
        check(reply.has_value(), "WRITE with default threshold produced a reply");
        const auto rd = g.read(v);
        check(rd.has_value() && (*rd)->only().owner.get() != frame.owner.get(),
              "default pin_payload_ratio=0 => stored bytes are a COPY (segment differs)");
        check(rd.has_value() && (*rd)->only().bytes().size() == big_tlv.size() &&
                  std::memcmp((*rd)->only().bytes().data(), big_tlv.data(), big_tlv.size()) == 0,
              "copied store holds the payload TLV bytes");
    }

    // Opt in through the OWNER-side declaration. RFC-0022 §3.B withdrew the
    // `:settings.store_ref_min_bytes` write surface — the copy/pin trade is a deployment
    // call, not remotely-writable QoS — so this is now the only door, and a peer's write
    // of the old name answers SCHEMA_NOT_FOUND.
    check(!g.write(path_t("/sensor/blob:settings.store_ref_min_bytes"),
                   make_value(b_value({0x08, 0x00, 0x00, 0x00})))
               .has_value(),
          "the removed `:settings.store_ref_min_bytes` write surface refuses");
    g.set_pin_payload_ratio(v, 8);
    check(g.pin_payload_ratio(v) == 8, "the owner-side declaration takes the threshold (8)");

    // Big trailer-less payload >= threshold => the stored view IS the frame segment.
    {
        const auto arena = tr::wire::decode_into(frame.bytes(), tr::mem::heap_source());
        auto reply = resolver.resolve(*arena, {}, &frame);
        check(reply.has_value(), "WRITE over threshold produced a reply");
        const auto rd = g.read(v);
        check(rd.has_value() && (*rd)->only().owner.get() == frame.owner.get(),
              "referenced store: stored segment IS the frame segment (pointer identity)");
        check(rd.has_value() && (*rd)->only().bytes().size() == big_tlv.size() &&
                  std::memcmp((*rd)->only().bytes().data(), big_tlv.data(), big_tlv.size()) == 0,
              "referenced subview covers exactly the payload TLV within the frame");
    }

    // Release the frame: the store's refcount pin keeps the bytes alive.
    frame = tr::view::view_t{};
    {
        const auto rd = g.read(v);
        check(rd.has_value() && (*rd)->only().bytes().size() == big_tlv.size() &&
                  std::memcmp((*rd)->only().bytes().data(), big_tlv.data(), big_tlv.size()) == 0,
              "referenced store survives the frame view's release (segment pinned)");
    }

    // A CRC-trailered payload over the threshold falls back to the trailer-sliced copy.
    {
        tlv_t val;
        val.type = type_t::VALUE;
        val.opt.cr = true;
        std::vector<std::byte> pb(32);
        val.payload = pb;
        const std::vector<std::byte> val_bytes = tr::wire::encode(val);
        const auto fwd_crc =
            b_fwd(fwd_op_t::WRITE, b_path({"sensor", "blob"}), b_path({"reply-ep"}), {}, val_bytes);
        tr::view::view_t crc_frame = make_value(fwd_crc);
        const auto arena = tr::wire::decode_into(crc_frame.bytes(), tr::mem::heap_source());
        auto reply = resolver.resolve(*arena, {}, &crc_frame);
        check(reply.has_value(), "trailered WRITE over threshold produced a reply");
        const auto rd = g.read(v);
        check(rd.has_value() && (*rd)->only().owner.get() != crc_frame.owner.get(),
              "trailered payload => falls back to the one-copy store (no reference)");
        const auto inner = tr::wire::decode((*rd)->only());
        check(inner.has_value() && !inner->opt.cr && !inner->trailer.has_value(),
              "trailered fallback stays trailer-sliced at rest (ADR-0041 §4)");
    }

    // A small payload (< threshold) keeps the copy.
    {
        const auto fwd_small = b_fwd(fwd_op_t::WRITE, b_path({"sensor", "blob"}),
                                     b_path({"reply-ep"}), {}, b_value({0x2A}));
        tr::view::view_t small_frame = make_value(fwd_small);
        const auto arena = tr::wire::decode_into(small_frame.bytes(), tr::mem::heap_source());
        auto reply = resolver.resolve(*arena, {}, &small_frame);
        check(reply.has_value(), "small WRITE produced a reply");
        const auto rd = g.read(v);
        check(rd.has_value() && (*rd)->only().owner.get() != small_frame.owner.get(),
              "payload under the threshold => copied (a small copy beats pinning)");
    }
}

/**
 * @brief TSan case: concurrent writers replace the LKV while a REFERENCED frame is pinned by a
 *        reader's clone — the frame segment's refcount drop races across threads and the pinned
 *        bytes must stay intact.
 */
void test_store_ref_concurrent() {
    std::printf("referenced store under concurrent writes (TSan — pinned frame stays valid):\n");
    graph_t g;
    op_resolver_t resolver(g);
    const auto path = path_t::parse("/sensor/blob");
    tr::graph::vertex_handle_t v = g.register_vertex(*path, role_t::STORED_VALUE);
    g.set_pin_payload_ratio(v, 8);

    std::vector<std::byte> big(64);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::byte>(0xA0 + i);
    const std::vector<std::byte> big_tlv = b_value(big);
    const auto fwd =
        b_fwd(fwd_op_t::WRITE, b_path({"sensor", "blob"}), b_path({"reply-ep"}), {}, big_tlv);
    tr::view::view_t frame = make_value(fwd);
    const auto arena = tr::wire::decode_into(frame.bytes(), tr::mem::heap_source());
    auto reply = resolver.resolve(*arena, {}, &frame);
    check(reply.has_value(), "referenced WRITE produced a reply");

    // Pin the referenced store via a reader clone, then drop the frame view.
    const auto pinned = g.read(v);
    check(pinned.has_value() && (*pinned)->only().owner.get() == frame.owner.get(),
          "reader clone pins the frame segment");
    frame = tr::view::view_t{};

    // Concurrent writers churn the LKV (each replacement drops a segment ref) while
    // readers clone whatever is current. The pinned view must stay byte-stable.
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&g, v, t] {
            for (int i = 0; i < 200; ++i) {
                std::vector<std::byte> p(16, static_cast<std::byte>(t * 16 + (i & 0xF)));
                (void)g.write(v, make_value(b_value(p)));
                const auto r = g.read(v);
                (void)r;
            }
        });
    }
    for (auto& w : workers) w.join();

    check((*pinned)->only().bytes().size() == big_tlv.size() &&
              std::memcmp((*pinned)->only().bytes().data(), big_tlv.data(), big_tlv.size()) == 0,
          "pinned referenced view is byte-stable across 800 concurrent writes");
    const auto rd = g.read(v);
    check(rd.has_value(), "vertex still reads cleanly after the churn");
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
    check(status_error_code(wr.children[4]) == 0x0021 /*tr::path::invalid*/,
          "ERROR payload == STATUS{ ERROR{ VALUE u16=0x0021 tr::path::invalid } }");

    // dst not local: an unregistered path => NOT_FOUND.
    const auto nfwd = b_fwd(fwd_op_t::READ, b_path({"nope", "missing"}), b_path({"reply-ep"}));
    auto nreply = resolve_bytes(resolver, nfwd);
    const auto dnr = decode_reply(*nreply);
    const tlv_t& nr = dnr.tlv;
    check(value_u8(nr.children[3]) == static_cast<std::uint8_t>(reply_kind_t::ERROR),
          "non-local dst => kind=ERROR");
    check(status_error_code(nr.children[4]) == 0x0020 /*tr::path::not_found*/,
          "ERROR payload == STATUS{ ERROR{ VALUE u16=0x0020 tr::path::not_found } }");
}

/**
 * @brief #929: a `TRANSPORT_DOWN` status leaves this seam as `tr::transport::down` (0x0060).
 *
 * `error_code(status_t)` is the L4→wire cast, and the two enums either side of it are separate
 * registries, so the map is hand-written. `-Werror=switch` proves an arm EXISTS for every
 * status; only a wire read proves the arm is the RIGHT one. Before #929 no `status_t` member
 * could reach `err_t::TRANSPORT_DOWN` at all — the transport factories spent `NOT_FOUND` on a
 * link that did not come up, which goes out as `tr::path::not_found` (0x0020), PERMANENT.
 *
 * A HANDLER vertex whose `on_read` refuses is the shortest path from an arbitrary status to
 * the ERROR reply's bytes: the seam's error propagates verbatim (`graph.cpp`'s HANDLER arm),
 * so this asserts the mapping, not the transport.
 */
void test_transport_down_reaches_the_wire() {
    std::printf("#929: status_t::TRANSPORT_DOWN goes out as tr::transport::down (0x0060):\n");
    graph_t g;
    op_resolver_t resolver(g);

    tr::graph::handlers_t down;
    down.on_read = []() -> tr::graph::result_t<tr::view::rope_t> {
        return std::unexpected(status_t::TRANSPORT_DOWN);
    };
    (void)g.register_vertex(*path_t::parse("/net/link"), role_t::HANDLER, std::move(down));

    const auto fwd = b_fwd(fwd_op_t::READ, b_path({"net", "link"}), b_path({"reply-ep"}));
    auto reply = resolve_bytes(resolver, fwd);
    check(reply.has_value(), "a refusing HANDLER still produces an addressed reply");
    const auto dec = decode_reply(*reply);
    const tlv_t& r = dec.tlv;
    check(value_u8(r.children[3]) == static_cast<std::uint8_t>(reply_kind_t::ERROR),
          "a TRANSPORT_DOWN status => kind=ERROR");
    check(status_error_code(r.children[4]) == 0x0060 /*tr::transport::down*/,
          "ERROR payload == STATUS{ ERROR{ VALUE u16=0x0060 tr::transport::down } }");
    check(status_error_code(r.children[4]) != 0x0020 /*tr::path::not_found*/,
          "it is NOT tr::path::not_found — the code whose disposition says stop retrying");
}

void test_write_creates_remote() {
    std::printf("write-creates over FWD (RFC-0005): a remote data WRITE creates the path:\n");
    graph_t g;
    op_resolver_t resolver(g);

    // A remote DATA write to a nonexistent path creates it, mkdir-p style.
    const auto fwd = b_fwd(fwd_op_t::WRITE, b_path({"fresh", "leaf"}), b_path({"reply-ep"}), {},
                           b_value({0x5A}));
    auto reply = resolve_bytes(resolver, fwd);
    check(reply.has_value(), "WRITE to an unregistered path resolves");
    const auto dec = decode_reply(*reply);
    check(value_u8(dec.tlv.children[3]) == static_cast<std::uint8_t>(reply_kind_t::RESULT),
          "write-create replies kind=RESULT (not NOT_FOUND)");
    check(g.read(path_t("/fresh/leaf")).has_value(), "the created vertex serves the written value");
    check(g.find(path_t::parse("/fresh")->key()).has_value(),
          "the intermediate level was created too (mkdir-p)");

    // A remote FIELD write to a nonexistent path still does NOT create — there is
    // no vertex whose control surface it could address.
    std::vector<std::byte> field_body;
    tr::wire::emit_name(field_body, "settings");
    tr::wire::emit_name(field_body, "priority");
    std::vector<std::byte> field_sel;
    tr::wire::emit_tlv(field_sel, type_t::FIELD, opt_t{.pl = true}, field_body);
    const auto ffwd = b_fwd(fwd_op_t::WRITE, b_path({"other", "leaf"}), b_path({"reply-ep"}),
                            field_sel, b_value({1}));
    auto freply = resolve_bytes(resolver, ffwd);
    const auto fdec = decode_reply(*freply);
    check(value_u8(fdec.tlv.children[3]) == static_cast<std::uint8_t>(reply_kind_t::ERROR),
          "field write to an unregistered path => kind=ERROR");
    check(status_error_code(fdec.tlv.children[4]) == 0x0020 /*tr::path::not_found*/,
          "field write keeps tr::path::not_found (no vertex to control)");
    check(!g.find(path_t::parse("/other/leaf")->key()).has_value(), "field write created nothing");
}

}  // namespace

void test_out_of_range_index_mode() {
    std::printf(
        "FIELD index_mode byte outside {0,1,2} -> INVALID_PATH (no silent index drop, #437):\n");
    graph_t g;
    op_resolver_t resolver(g);
    const auto path = path_t::parse("/sensor/temp");
    (void)g.register_vertex(*path, role_t::STORED_VALUE);

    // A FIELD selector: NAME "app-x", VALUE index=5 (u32), VALUE index_mode=3 (u8). Mode 3 is
    // outside {SCALAR=0, ELEMENT=1, WILDCARD=2}; before #437 the selector_to_field switch had no
    // default, so it fell through and silently DROPPED the decoded index (resolving as a plain
    // non-indexed scalar) rather than rejecting the malformed selector.
    std::vector<std::byte> fbody;
    for (const auto& part : {b_name("app-x"), b_value({5, 0, 0, 0}), b_value({3})})
        fbody.insert(fbody.end(), part.begin(), part.end());
    std::vector<std::byte> field;
    tr::wire::emit_tlv(field, type_t::FIELD, opt_t{.pl = true}, fbody);

    const auto fwd = b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"reply-ep"}), field);
    auto reply = resolve_bytes(resolver, fwd);
    check(reply.has_value(), "malformed-mode resolve produced a reply (soft error, not a crash)");
    const auto dr = decode_reply(*reply);
    const tlv_t& r = dr.tlv;
    check(value_u8(r.children[3]) == static_cast<std::uint8_t>(reply_kind_t::ERROR),
          "index_mode=3 => kind=ERROR");
    check(status_error_code(r.children[4]) == 0x0021 /*tr::path::invalid*/,
          "ERROR payload == INVALID_PATH (0x0021) — the index is rejected, not dropped");
}

// ---------------------------------------------------------------------------
// The external subscription observer (graph_t::set_subscription_observer).
// ---------------------------------------------------------------------------

/** @brief `/a/b` spelled back from a canonical key, so an event is asserted on readable text. */
std::string spell(tr::wire::key_view_t key) {
    std::string out;
    const std::span<const std::byte> b = key.bytes();
    std::size_t i = 0;
    while (i + 4 <= b.size()) {
        const std::size_t len = tr::detail::load_le<std::uint16_t>(b.subspan(i + 2, 2));
        if (i + 4 + len > b.size()) break;
        out.push_back('/');
        out.append(reinterpret_cast<const char*>(b.data()) + i + 4, len);
        i += 4 + len;
    }
    return out;
}

/** @brief One observed event, flattened to owned strings (the event's views are call-scoped). */
struct seen_event_t {
    tr::graph::sub_event_t::kind_t kind;
    std::string producer;
    std::string target;
    std::string link;
    std::size_t slot;
};

/** @brief `ADDED /sensor/temp -> /ui/panel via link-a [0]` — the whole event in one line. */
std::string render(const seen_event_t& e) {
    std::string out = e.kind == tr::graph::sub_event_t::kind_t::ADDED ? "ADDED " : "REMOVED ";
    out += e.producer;
    out += " -> ";
    out += e.target.empty() ? "(none)" : e.target;
    out += " via ";
    out += e.link;
    out += " [" + std::to_string(e.slot) + "]";
    return out;
}

/** @brief FIELD `:subscribers[]` (append) — NAME + index_mode=ELEMENT, no index. */
std::vector<std::byte> b_field_subs_append() {
    std::vector<std::byte> out;
    std::vector<std::byte> body = b_name("subscribers");
    append(body, b_value({0x01}));
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}

/** @brief FIELD `:subscribers[N]` — NAME + index u32 + index_mode=ELEMENT (RFC-0004 §C). */
std::vector<std::byte> b_field_subs_at(std::uint32_t n) {
    std::vector<std::byte> out;
    std::vector<std::byte> body = b_name("subscribers");
    append(body, b_value({static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(n >> 8),
                          static_cast<std::uint8_t>(n >> 16), static_cast<std::uint8_t>(n >> 24)}));
    append(body, b_value({0x01}));
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}

/** @brief The RFC-0009 §D.1 eviction sentinel — an empty STATUS (`09 00 00 00`). */
std::vector<std::byte> b_evict_sentinel() {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::STATUS, opt_t{}, {});
    return out;
}

void test_subscription_observer() {
    std::printf("subscription observer — EXTERNAL :subscribers[] mutations only:\n");
    graph_t g;
    op_resolver_t resolver(g);
    std::vector<seen_event_t> seen;
    g.set_subscription_observer([&seen](const tr::graph::sub_event_t& e) {
        seen.push_back(
            seen_event_t{e.kind, spell(e.producer), spell(e.target), std::string(e.link), e.slot});
    });

    tr::graph::vertex_handle_t v =
        g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    (void)g.register_vertex(*path_t::parse("/ui/panel"), role_t::STORED_VALUE);

    // --- silence: the local doors ------------------------------------------------------
    (void)g.subscribe(*path_t::parse("/sensor/temp"), *path_t::parse("/ui/panel"));
    check(seen.empty(), "a DIRECT graph_t::subscribe(src, target) does NOT fire the observer");
    auto sink = [](const tr::view::rope_t&) {};
    const auto sub = g.subscribe(*path_t::parse("/sensor/temp"), sink);
    check(seen.empty(), "the callback-form subscribe sugar does NOT fire it either");
    if (sub)
        check(g.unsubscribe(*sub).has_value() && seen.empty(),
              "the local unsubscribe() does NOT fire it");

    // An op through the RESOLVER but with an EMPTY inbound link is a LOCAL op by definition
    // (the ADR-0018 caller context) — still silent.
    {
        const auto fwd = b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"reply-ep"}),
                               b_field_subs_append(), b_subscriber({"ui", "panel"}));
        (void)resolve_bytes(resolver, fwd);
    }
    check(seen.empty(), "a resolver WRITE with an EMPTY inbound_link is local — still silent");

    // --- the external append ------------------------------------------------------------
    {
        const auto fwd = b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"reply-ep"}),
                               b_field_subs_append(), b_subscriber({"ui", "panel"}));
        auto reply = resolve_bytes(resolver, fwd, "link-a");
        const auto d = decode_reply(*reply);
        check(value_u8(d.tlv.children[3]) == static_cast<std::uint8_t>(reply_kind_t::RESULT),
              "the external subscribe resolved to RESULT");
    }
    check(seen.size() == 1, "an external :subscribers[] append fires exactly once");
    const std::string added = seen.empty() ? std::string{} : render(seen.back());
    // A wire append binds a REMOTE subscriber (op_resolve_walk's remote_sub branch =>
    // subscribe_wire), which DROPS target_key — the event still reports the PATH the peer
    // sent, because that is what the record says.
    check(added == "ADDED /sensor/temp -> /ui/panel via link-a [2]",
          std::string("... ADDED, with producer / target / link / slot (got '") + added + "')");
    check(g.own_subs(v) == 3, "the external append really landed as a third slot");

    // A SUBSCRIBER carrying no PATH at all: the bare remote subscriber => empty target.
    {
        std::vector<std::byte> bare;
        tr::wire::emit_tlv(bare, type_t::SUBSCRIBER, opt_t{.pl = true}, {});
        const auto fwd = b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"reply-ep"}),
                               b_field_subs_append(), bare);
        (void)resolve_bytes(resolver, fwd, "link-b");
    }
    check(render(seen.back()) == "ADDED /sensor/temp -> (none) via link-b [3]",
          "a PATH-less SUBSCRIBER reports an EMPTY target (the bare remote-subscriber case)");

    // --- the external CLEAR (RFC-0009 §D.1 eviction sentinel) ---------------------------
    {
        const auto fwd = b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"reply-ep"}),
                               b_field_subs_at(2), b_evict_sentinel());
        auto reply = resolve_bytes(resolver, fwd, "link-a");
        const auto d = decode_reply(*reply);
        check(value_u8(d.tlv.children[3]) == static_cast<std::uint8_t>(reply_kind_t::RESULT),
              "the external :subscribers[2] clear resolved to RESULT");
    }
    check(render(seen.back()) == "REMOVED /sensor/temp -> /ui/panel via link-a [2]",
          "the external clear fires REMOVED, naming the DEPARTING edge's target");

    // Clearing an already-empty slot changed nothing and must not be reported.
    const std::size_t before_noop = seen.size();
    {
        const auto fwd = b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"reply-ep"}),
                               b_field_subs_at(2), b_evict_sentinel());
        (void)resolve_bytes(resolver, fwd, "link-a");
    }
    check(seen.size() == before_noop, "re-clearing an empty slot fires nothing");

    // --- the external REPLACE (§D.1) is two events, in causal order ---------------------
    const std::size_t before_replace = seen.size();
    {
        const auto fwd = b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"reply-ep"}),
                               b_field_subs_at(0), b_subscriber({"ui", "other"}));
        (void)resolve_bytes(resolver, fwd, "link-c");
    }
    check(seen.size() == before_replace + 2, "a replace of a LIVE slot fires two events");
    check(seen.size() >= 2 &&
              render(seen[seen.size() - 2]) == "REMOVED /sensor/temp -> /ui/panel via link-c [0]" &&
              render(seen.back()) == "ADDED /sensor/temp -> /ui/other via link-c [0]",
          "... REMOVED (displaced) then ADDED (incoming), same slot");

    // Filling the slot cleared above is an ADD and nothing else.
    const std::size_t before_refill = seen.size();
    {
        const auto fwd = b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"reply-ep"}),
                               b_field_subs_at(2), b_subscriber({"ui", "panel"}));
        (void)resolve_bytes(resolver, fwd, "link-c");
    }
    check(seen.size() == before_refill + 1 &&
              render(seen.back()) == "ADDED /sensor/temp -> /ui/panel via link-c [2]",
          "a replace that FILLS an empty slot is one ADDED, no REMOVED");

    // --- a REFUSED external subscribe reports nothing -----------------------------------
    const std::size_t before_refused = seen.size();
    {
        const auto fwd = b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"reply-ep"}),
                               b_field_subs_at(9999), b_subscriber({"ui", "panel"}));
        auto reply = resolve_bytes(resolver, fwd, "link-c");
        const auto d = decode_reply(*reply);
        check(value_u8(d.tlv.children[3]) == static_cast<std::uint8_t>(reply_kind_t::ERROR),
              "a :subscribers[9999] replace is refused (INVALID_PATH)");
    }
    check(seen.size() == before_refused, "a REFUSED external subscribe fires nothing");

    // --- link teardown is documented as SILENT ------------------------------------------
    const std::size_t before_evict = seen.size();
    // `link-b`'s slot is the one an APPEND made — that is the door (subscribe_wire) that stores
    // the edge's link NAME; a `[N]` replace stores only the gate context, so its edge carries no
    // link and eviction never matches it (pre-existing, unrelated to this observer).
    const std::size_t evicted = g.evict_link_edges("link-b");
    check(evicted > 0 && seen.size() == before_evict,
          "evict_link_edges (link teardown) is SILENT by design — the app's link-down is the "
          "removal signal for the whole link");
}

int main() {
    test_read_zero_copy();
    test_write();
    test_await();
    test_await_field_selector_is_enotty();
    test_subscribers_field();
    test_write_trailer_sliced();
    test_pin_payload_ratio_store();
    test_store_ref_concurrent();
    test_non_canonical_dst();
    test_wildcard_and_not_local();
    test_out_of_range_index_mode();
    test_transport_down_reaches_the_wire();
    test_write_creates_remote();
    test_subscription_observer();
    return tr::testing::summary("op_resolve");
}
