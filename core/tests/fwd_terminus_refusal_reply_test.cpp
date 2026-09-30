/**
 * @file
 * @brief #1612 — a request the terminus cannot serve for want of MEMORY is answered with an
 *        addressed `STATUS{BACKPRESSURE}`, built on the stack from the request's own bytes,
 *        through no allocator at all.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The three resource arms of `fwd_router_t`'s terminus — the rx source refusing the decode
 * arena, the egress backend refusing the reply head, the rx source refusing the reply's iov
 * table — used to count a drop and return. The requester saw only its own timeout, which made a
 * memory condition indistinguishable from a lost frame, an unreachable node or a wedged link
 * (#1612 cost an evening of exactly that misattribution). Reference 04 §"Exhaustion is a
 * value" now states the rule: *the reply must not need the memory that just refused*.
 *
 * @section shape What a case here has to prove
 *
 * - the INSTRUMENT: the seam was asked and refused, so the case is not vacuous;
 * - the REPLY: exactly one frame back, `FWD{REPLY}` with `kind=ERROR`, the registered
 *   `tr::flow::backpressure` code, and the requester's own `src` echoed as the reply's `dst` —
 *   an unaddressed error is one the requester cannot correlate;
 * - the COUNT: the arm's own counter still moves, because the reply does not un-drop the
 *   operation (STYLE.md §Introspection — `dropped` is work lost, and it was);
 * - the BUDGET: on the arm the issue was filed about, the whole receive — refusal, reply build
 *   and send — reaches the global allocator ZERO times. The test link records into a fixed
 *   array for that reason: a `std::vector` recorder would be the only allocation and would
 *   hide a regression behind its own noise.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/error.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

/** @brief Global-new call counter, live only while @ref g_arm is set. */
std::size_t g_allocs = 0;
bool g_arm = false;

/** @brief The counted allocation itself — malloc-backed so `operator delete` can free it. */
void* counted(std::size_t n) {
    if (g_arm) ++g_allocs;
    return std::malloc(n == 0 ? 1 : n);
}

/** @brief The aligned counted allocation (`aligned_alloc` only for an over-aligned request). */
void* counted_aligned(std::size_t n, std::size_t align) {
    if (align <= alignof(std::max_align_t)) return counted(n);
    if (g_arm) ++g_allocs;
    const std::size_t rounded = ((n == 0 ? 1 : n) + align - 1) / align * align;
    return std::aligned_alloc(align, rounded);
}

}  // namespace

// Every allocating and deallocating form is replaced (the `terminus_egress_backend_test`
// precedent): a hole in the set would make an allocation INVISIBLE to the counter.
void* operator new(std::size_t n) {
    void* const p = counted(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) {
    void* const p = counted(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new(std::size_t n, std::align_val_t a) {
    void* const p = counted_aligned(n, static_cast<std::size_t>(a));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned(n, static_cast<std::size_t>(a));
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::router_stats_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::b_fwd;
using tr::testing::b_path;
using tr::testing::check;

/** @brief `tr::flow::backpressure` (RFC-0002 §C), the registered code the reply must carry. */
constexpr std::uint16_t kBackpressureCode = std::to_underlying(tr::wire::err_t::FLOW_BACKPRESSURE);

/**
 * @brief A `block_source_t` that serves from the heap until armed, then refuses.
 *
 * Delegation, not a private allocator (the `router_drop_stats_test` twin): a served block IS
 * the heap source's, and the ONLY difference between armed and un-armed is the `nullptr`.
 */
class arming_source_t final : public tr::mem::block_source_t {
   public:
    arming_source_t() noexcept : block_source_t("test_arming_src") {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (armed_) {
            ++refusals_;
            return nullptr;
        }
        return tr::mem::heap_source().try_alloc(bytes, align);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::heap_source().release(p, bytes, align);
    }

    /** @brief Refuse every subsequent draw. */
    void arm() noexcept { armed_ = true; }
    /** @brief Serve again. */
    void disarm() noexcept { armed_ = false; }
    /** @brief How many draws were REFUSED — the instrument check. */
    [[nodiscard]] int refusals() const noexcept { return refusals_; }

   private:
    bool armed_ = false;
    int refusals_ = 0;
};

/** @brief A `mem_backend_t` that serves from the heap until armed, then refuses. */
class arming_backend_t final : public tr::mem::mem_backend_t {
   public:
    arming_backend_t() noexcept : mem_backend_t("test_arming") {}

    [[nodiscard]] tr::view::segment_t* alloc(
        std::size_t size, tr::mem::alloc_hint_t hint = tr::mem::alloc_hint_t::NONE) override {
        if (armed_) {
            ++refusals_;
            return nullptr;
        }
        return tr::mem::heap_backend().alloc(size, hint);
    }
    void destroy(tr::view::segment_t* seg) noexcept override {
        tr::mem::heap_backend().destroy(seg);
    }
    /** @brief Refuse every subsequent allocation. */
    void arm() noexcept { armed_ = true; }
    /** @brief Serve again. */
    void disarm() noexcept { armed_ = false; }
    /** @brief How many allocations were REFUSED. */
    [[nodiscard]] int refusals() const noexcept { return refusals_; }

   private:
    bool armed_ = false;
    int refusals_ = 0;
};

/**
 * @brief A link that records the LAST frame the router sent into a fixed array — no heap, so
 *        the recorder cannot be the allocation the zero-alloc case counts.
 */
class fixed_link_t : public transport_t {
   public:
    explicit fixed_link_t(bool ropes = false) : ropes_(ropes) {}
    void send(std::span<const std::byte> frame) override {
        ++count;
        len = std::min(frame.size(), last.size());
        std::memcpy(last.data(), frame.data(), len);
    }
    [[nodiscard]] bool delivers_ropes() const override { return ropes_; }
    /** @brief Push a rope up as a receive (the rope-tier ingress). */
    void inject(tr::view::rope_t frame) { rx_.deliver_rope(std::move(frame)); }
    /** @brief The last recorded frame. */
    [[nodiscard]] std::span<const std::byte> frame() const {
        return std::span<const std::byte>(last.data(), len);
    }
    std::size_t count = 0;             /**< @brief Frames the router handed down. */
    std::array<std::byte, 512> last{}; /**< @brief The last frame's bytes. */
    std::size_t len = 0;               /**< @brief Its length. */

   private:
    bool ropes_ = false;
};

// --- wire builders -----------------------------------------------------------------

/** @brief Append @p src to @p dst. */
void append(std::vector<std::byte>& dst, std::span<const std::byte> src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

/** @brief A `NAME` TLV. */
std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, s);
    return out;
}

/** @brief An opaque `VALUE` TLV holding one byte. */
std::vector<std::byte> b_value_u8(std::uint8_t v) {
    const std::byte b{v};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}

/** @brief `FIELD{ NAME "subscribers", VALUE u8 index_mode=ELEMENT }` — the `:subscribers[]`
 *         append selector, which sits BETWEEN `dst` and `src` on the wire. */
std::vector<std::byte> b_field_subscribers_append() {
    std::vector<std::byte> body;
    append(body, b_name("subscribers"));
    append(body, b_value_u8(1));
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}

/** @brief `SUBSCRIBER{ PATH target }`. */
std::vector<std::byte> b_subscriber(std::span<const std::byte> target) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, target);
    return out;
}

/** @brief The SUBSCRIBE the issue was filed about: a `:subscribers[]` WRITE with a selector. */
std::vector<std::byte> b_subscribe(std::span<const std::byte> dst, std::span<const std::byte> src) {
    return b_fwd(fwd_op_t::WRITE, dst, src, b_field_subscribers_append(), b_subscriber(src));
}

/** @brief A rope over @p bytes split into @p links links. */
tr::view::rope_t as_rope(std::span<const std::byte> bytes, std::size_t links) {
    tr::view::rope_t r;
    const std::size_t step = (bytes.size() + links - 1) / links;
    for (std::size_t off = 0; off < bytes.size(); off += step) {
        const std::size_t n = std::min(step, bytes.size() - off);
        tr::view::segment_ptr_t seg = tr::view::heap_alloc(n);
        std::memcpy(seg->bytes.data(), bytes.data() + off, n);
        r.append(tr::view::view_t::over(std::move(seg)));
    }
    return r;
}

/** @brief @p frame with an absolute (TF=0) wire-time stamp @p ns on its outer trailer. */
std::vector<std::byte> stamped(std::vector<std::byte> frame, std::int64_t ns) {
    opt_t opt = opt_t::decode(std::to_integer<std::uint8_t>(frame[1]));
    opt.ts = true;
    frame[1] = static_cast<std::byte>(opt.encode());
    tr::wire::emit_trailer_ts(frame, /*relative=*/false, ns);
    return frame;
}

// --- the reply shape --------------------------------------------------------------------

/**
 * @brief Is @p reply the addressed `FWD{REPLY, dst=req.src, src=req.dst, kind=ERROR,
 *        STATUS{ERROR{code}}}` for a request that carried @p req_dst / @p req_src?
 *
 * Decoded with the owning codec — this is the test's reader, not the router's — and compared
 * route-for-route: the reply's `dst` must be the request's `src` BYTE FOR BYTE, because that is
 * what lets the requester correlate the answer with the operation it timed out on.
 */
bool is_backpressure_reply(std::span<const std::byte> reply, std::span<const std::byte> req_dst,
                           std::span<const std::byte> req_src) {
    const auto dec = tr::wire::decode(reply);
    if (!dec || dec->type != type_t::FWD || dec->children.size() != 5) return false;
    const auto& c = dec->children;
    if (c[0].type != type_t::VALUE || c[0].payload.size() != 1 ||
        std::to_integer<std::uint8_t>(c[0].payload[0]) != std::to_underlying(fwd_op_t::REPLY))
        return false;
    if (!std::ranges::equal(tr::wire::encode(c[1]), req_src)) return false;
    if (!std::ranges::equal(tr::wire::encode(c[2]), req_dst)) return false;
    if (c[3].type != type_t::VALUE || c[3].payload.size() != 1 ||
        std::to_integer<std::uint8_t>(c[3].payload[0]) != std::to_underlying(reply_kind_t::ERROR))
        return false;
    if (c[4].type != type_t::STATUS || c[4].children.size() != 1) return false;
    const auto& err = c[4].children[0];
    if (err.type != type_t::ERROR || err.children.size() != 1) return false;
    const auto& code = err.children[0];
    return code.type == type_t::VALUE && code.payload.size() == 2 &&
           tr::detail::load_le<std::uint16_t>(code.payload) == kBackpressureCode;
}

// --- arena: the arm the issue was filed about ----------------------------------------

/**
 * @brief A SUBSCRIBE whose decode the rx source refuses is answered with `BACKPRESSURE`
 *        addressed to the requester's own `src`, the arm still counts, and the whole receive
 *        allocates nothing.
 */
void test_arena_refusal_is_answered_without_allocating() {
    std::printf("an rx-source refusal at the terminus answers STATUS{BACKPRESSURE}, zero-alloc:\n");
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    arming_source_t rx;
    fwd_router_t router(g, {.rx = &rx});
    fixed_link_t client;
    (void)router.add_child("client", client);

    const std::vector<std::byte> dst = b_path({"sensor", "temp"});
    const std::vector<std::byte> src = b_path({"client", "back"});
    const std::vector<std::byte> subscribe = b_subscribe(dst, src);

    // Control FIRST, un-armed: the SUBSCRIBE resolves and is answered with a RESULT, so the
    // armed run below measures the refusal and not a frame that never resolved.
    router.on_frame("client", subscribe);
    check(client.count == 1, "control: the well-formed SUBSCRIBE is answered");
    check(!is_backpressure_reply(client.frame(), dst, src),
          "control: ...and not with BACKPRESSURE");
    const router_stats_t before = router.drop_stats();

    rx.arm();
    g_allocs = 0;
    g_arm = true;
    router.on_frame("client", subscribe);
    g_arm = false;
    const router_stats_t after = router.drop_stats();

    check(rx.refusals() > 0, "instrument: the rx source was ASKED and refused");
    check(after.arena_dropped == before.arena_dropped + 1, "the arm still counts arena_dropped");
    check(client.count == 2, "exactly one frame went back to the requester");
    check(is_backpressure_reply(client.frame(), dst, src),
          "and it is FWD{REPLY, dst=<the request's src>, kind=ERROR, STATUS{BACKPRESSURE}}");
    std::printf("  global allocations during the refused receive: %zu\n", g_allocs);
    check(g_allocs == 0, "the reply path reached the global allocator ZERO times");

    // The refusal leaves no residue: served again, the same frame resolves again.
    rx.disarm();
    router.on_frame("client", subscribe);
    check(client.count == 3 && !is_backpressure_reply(client.frame(), dst, src),
          "the SUBSCRIBE is served once the source recovers");
}

/**
 * @brief The acceptance shape verbatim: a bounded `pool_source_t` as rx, exhausted, then a
 *        SUBSCRIBE — the client receives `STATUS{BACKPRESSURE}` with its own `src` echoed.
 *
 * The slab is too small for a single decode arena, so exhaustion is immediate — what the
 * issue reached after a `:children[]` walk this test reaches on the first draw.
 */
void test_bounded_pool_source_exhaustion_is_answered() {
    std::printf("a bounded pool_source_t rx that cannot fund the decode still answers:\n");
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    alignas(std::max_align_t) std::array<std::byte, 32> slab{};
    std::array<tr::mem::size_class_t, 4> classes{};
    tr::mem::pool_source_t<> rx{std::span<std::byte>(slab),
                                std::span<tr::mem::size_class_t>(classes)};
    fwd_router_t router(g, {.rx = &rx});
    fixed_link_t client;
    (void)router.add_child("client", client);

    const std::vector<std::byte> dst = b_path({"sensor", "temp"});
    const std::vector<std::byte> src = b_path({"client"});
    router.on_frame("client", b_subscribe(dst, src));

    check(rx.refused() > 0, "instrument: the pool refused (it is smaller than one arena)");
    check(client.count == 1, "one frame went back");
    check(is_backpressure_reply(client.frame(), dst, src),
          "and it is the addressed STATUS{BACKPRESSURE}, src echoed");
}

// --- the other two arms, and the rope tier ---------------------------------------------

/**
 * @brief The reply-head arm: an egress backend that refuses the reply head (and therefore the
 *        resolver's own `or_backpressure` head too) is answered from the stack instead.
 */
void test_egress_refusal_is_answered() {
    std::printf("an egress-backend refusal of the reply head answers from the stack:\n");
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    arming_backend_t egress;
    fwd_router_t router(g, {.egress = &egress});
    fixed_link_t client;
    (void)router.add_child("client", client);

    const std::vector<std::byte> dst = b_path({"sensor", "temp"});
    const std::vector<std::byte> src = b_path({"client"});
    const std::vector<std::byte> read = b_fwd(fwd_op_t::READ, dst, src);

    router.on_frame("client", read);
    check(client.count == 1 && !is_backpressure_reply(client.frame(), dst, src),
          "control: the READ is answered with a RESULT");

    const router_stats_t before = router.drop_stats();
    egress.arm();
    router.on_frame("client", read);
    check(egress.refusals() > 0, "instrument: the egress backend was ASKED and refused");
    check(router.drop_stats().assemble_dropped == before.assemble_dropped + 1,
          "assemble_dropped still counts the lost RESULT");
    check(client.count == 2 && is_backpressure_reply(client.frame(), dst, src),
          "and the requester gets the addressed BACKPRESSURE");
}

/**
 * @brief The rope tier is a second code path (the frame is a lazy `tlv_view_t`, not an arena),
 *        and its refusal arms answer through the rope cursor — the request split across links
 *        so the echoed routes are gathered from more than one span.
 */
void test_rope_tier_refusal_is_answered() {
    std::printf("the rope-tier terminus answers its refusal arms too:\n");
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    arming_source_t rx;
    fwd_router_t router(g, {.rx = &rx});
    fixed_link_t client(/*ropes=*/true);
    (void)router.add_child("client", client);

    const std::vector<std::byte> dst = b_path({"sensor", "temp"});
    const std::vector<std::byte> src = b_path({"client", "way", "home"});
    const std::vector<std::byte> read = b_fwd(fwd_op_t::READ, dst, src);

    client.inject(as_rope(read, 5));
    check(client.count == 1 && !is_backpressure_reply(client.frame(), dst, src),
          "control: the roped READ is answered with a RESULT");

    const router_stats_t before = router.drop_stats();
    rx.arm();
    client.inject(as_rope(read, 5));
    const router_stats_t after = router.drop_stats();
    check(rx.refusals() > 0, "instrument: the rx source was ASKED and refused");
    check(after.reply_iov_dropped + after.assemble_dropped ==
              before.reply_iov_dropped + before.assemble_dropped + 1,
          "the refused arm still counts");
    check(client.count == 2 && is_backpressure_reply(client.frame(), dst, src),
          "and the requester gets BACKPRESSURE with its 3-segment src echoed across links");
}

// --- the echo, and the frame with nowhere to reply to ---------------------------------

/** @brief A stamped request's TF=0 stamp rides the refusal reply verbatim (#1109 for #1612). */
void test_refusal_echoes_the_wire_time_stamp() {
    std::printf("a stamped request gets its stamp back on the refusal:\n");
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    arming_source_t rx;
    fwd_router_t router(g, {.rx = &rx});
    fixed_link_t client;
    (void)router.add_child("client", client);

    const std::vector<std::byte> dst = b_path({"sensor", "temp"});
    const std::vector<std::byte> src = b_path({"client"});
    constexpr std::int64_t kStamp = 0x0102030405060708;
    const std::vector<std::byte> read = stamped(b_fwd(fwd_op_t::READ, dst, src), kStamp);

    rx.arm();
    router.on_frame("client", read);
    check(client.count == 1 && is_backpressure_reply(client.frame(), dst, src),
          "the stamped request is answered with BACKPRESSURE");
    const auto dec = tr::wire::decode(client.frame());
    check(dec && dec->opt.ts && !dec->opt.tf && dec->trailer && dec->trailer->ts &&
              !dec->trailer->ts->relative && dec->trailer->ts->value == kStamp,
          "and the reply's outer trailer carries the request's absolute stamp verbatim");
}

/**
 * @brief A refused frame whose `src` cannot be located has nowhere to reply to: it stays a
 *        counted drop and NOTHING goes on the wire — never a headerless or mis-addressed frame.
 */
void test_frame_without_src_is_not_answered() {
    std::printf("a refused frame with no locatable src is dropped, not mis-answered:\n");
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    arming_source_t rx;
    fwd_router_t router(g, {.rx = &rx});
    fixed_link_t client;
    (void)router.add_child("client", client);

    // op + dst and nothing else: a FWD the grammar accepts, with no return route.
    std::vector<std::byte> body;
    append(body, tr::testing::fwd_op_child(static_cast<std::uint8_t>(fwd_op_t::READ)));
    append(body, b_path({"sensor", "temp"}));
    const std::vector<std::byte> no_src = tr::testing::fwd_envelope(body);

    const router_stats_t before = router.drop_stats();
    rx.arm();
    router.on_frame("client", no_src);
    check(rx.refusals() > 0, "instrument: the rx source refused the decode");
    check(router.drop_stats().arena_dropped == before.arena_dropped + 1, "the drop is counted");
    check(client.count == 0, "and nothing was sent — there is no src to address");
}

}  // namespace

int main() {
    std::printf("terminus refusal → addressed, allocation-free STATUS{BACKPRESSURE} (#1612)\n\n");

    test_arena_refusal_is_answered_without_allocating();
    std::printf("\n");
    test_bounded_pool_source_exhaustion_is_answered();
    std::printf("\n");
    test_egress_refusal_is_answered();
    std::printf("\n");
    test_rope_tier_refusal_is_answered();
    std::printf("\n");
    test_refusal_echoes_the_wire_time_stamp();
    std::printf("\n");
    test_frame_without_src_is_not_answered();

    return tr::testing::summary("fwd_terminus_refusal_reply");
}
