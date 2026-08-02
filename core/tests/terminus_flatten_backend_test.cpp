/**
 * @file
 * @brief #766 — the TERMINUS resolver's rope-tier flattens draw from the router's injected
 *        `mem_backend_t` too, and a refusal is answered by value rather than by reading a
 *        short span.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The sibling of `fwd_flatten_backend_test.cpp`, which pins the router's OWN four
 * `materialize()` sites (#730). This file pins the half that injection did not reach: the
 * two flattens one call BELOW `fwd_router_t::resolve_terminus_rope` —
 * `view_node::ensure_cache` (the per-node contiguous span every `wire()`/`body()` read of a
 * multi-link TLV materializes) and `view_node::own_wire` (the ADR-0053 ⑤ ownership flatten).
 * Both took `rope_t::materialize`'s DEFAULT global-heap backend, so a node that pointed
 * every documented seam at its own slab still allocated globally the moment a peer sent a
 * FRAGMENTED terminus request — peer-drivable, and an `abort()` under `-fno-exceptions`.
 *
 * @section instrument The instrument is checked before the guard is
 *
 * A flatten only happens on a MULTI-link rope: `materialize()` returns a single-link rope's
 * one link zero-copy and never touches the backend. So every armed case asserts
 * `refusals() > 0` FIRST, and the served case asserts `served() > 0` — with the sites back
 * on the global heap that count is ZERO and the case reports itself vacuous instead of
 * green. That is the ablation: re-point `ensure_cache`/`own_wire` at the default backend and
 * the first check of every case here reddens.
 *
 * @section heap What the global-new counter adds
 *
 * "Draws from the seam" and "does not draw from the global heap" are different claims, and
 * the refusal cases only prove the first. So the served case runs the SAME request twice —
 * once with `flat` on a static-slab `pool_t`, once on the default heap backend — with a
 * counting global `operator new` armed across the delivery, and compares. The pool arm must
 * make strictly fewer global allocations, by at least the number of flattens the seam
 * served: those bytes came out of the slab.
 *
 * @section answer The refusal contract
 *
 * A refused flatten leaves a node's `wire()`/`body()` EMPTY, and an empty span read as an op
 * byte, a lookup key or a field name silently changes the answer. The walk therefore carries
 * a sticky per-resolve flag (`spans_intact()`) and answers:
 *
 *   - a refusal on the reply's OWN route bytes ⇒ `BACKPRESSURE` on the error side, which the
 *     router drops — there is no trustworthy address to answer to, and a reply built on a
 *     short `src` would be a truncated route on the wire;
 *   - a refusal anywhere else before dispatch ⇒ an ADDRESSED `kind=ERROR STATUS{BACKPRESSURE}`
 *     reply, the same answer an OOM'd reply assembly gives (the client falls back on the same
 *     link rather than presuming the node dead).
 *
 * Never a `kind=RESULT`, never a truncated frame, never a mutation — which is what the
 * refusal SWEEP below asserts: it walks the refusal point across every flatten the request
 * makes and requires each outcome to be one of those two.
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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/mem_pool.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

/** @brief Global-new call/byte counters, live only while @ref g_arm is set. */
std::size_t g_allocs = 0;
std::size_t g_bytes = 0;
bool g_arm = false;

/** @brief The counted allocation itself — malloc-backed so `operator delete` can free it. */
void* counted(std::size_t n) {
    if (g_arm) {
        ++g_allocs;
        g_bytes += n;
    }
    return std::malloc(n == 0 ? 1 : n);
}

/**
 * @brief The over-aligned counted allocation — `aligned_alloc`, whose result `free` accepts.
 *
 * Forwarding an over-aligned request to plain `malloc` would hand back memory aligned only to
 * `max_align_t`; a `std::pmr` control block asking for 16- or 32-byte alignment would then be
 * under-aligned. The size is rounded up to a multiple of the alignment, which `aligned_alloc`
 * requires.
 */
void* counted_aligned(std::size_t n, std::size_t align) {
    if (g_arm) {
        ++g_allocs;
        g_bytes += n;
    }
    if (align < alignof(std::max_align_t)) align = alignof(std::max_align_t);
    const std::size_t rounded = ((n == 0 ? 1 : n) + align - 1) / align * align;
    return std::aligned_alloc(align, rounded);
}

}  // namespace

// Every allocating form, not just the throwing one: libtracer's heap backend allocates
// through `::operator new(bytes, std::nothrow)` (mem_heap.hpp), so overriding only
// `operator new(size)` would make the flatten INVISIBLE to this counter — exactly the
// undercount `bench_compact_delivery` was corrected for.
//
// And every DEALLOCATING form, for a reason ASan reports as `alloc-dealloc-mismatch`: a
// replacement set with a hole leaves that one form to the sanitizer's own operator, which is
// then handed a pointer this file `malloc`ed. The sized+aligned `operator delete` is the hole
// libstdc++'s `std::pmr::memory_resource::deallocate` walks into on every pmr control block.
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
using tr::graph::vertex_handle_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/**
 * @brief A `mem_backend_t` that serves from an upstream backend until its serve budget runs
 *        out, then refuses — the heap-exhaustion stand-in, with the exhaustion point
 *        movable.
 *
 * Delegation, not a private allocator (the `fwd_flatten_backend_test` precedent): a served
 * segment is the upstream's own, so it reclaims exactly as an un-injected resolver's would.
 * The ONLY difference between serving and refusing is the `nullptr`, which is the variable
 * under test. `refuse_after(k)` serves the first `k` allocations of the window and refuses
 * from the `k+1`-th — that is what lets the sweep move the refusal across EVERY flatten one
 * request makes, instead of only the first.
 */
class arming_backend_t final : public tr::mem::mem_backend_t {
   public:
    explicit arming_backend_t(tr::mem::mem_backend_t& upstream = tr::mem::heap_backend()) noexcept
        : mem_backend_t("test_arming"), up_(upstream) {}

    [[nodiscard]] tr::view::segment_t* alloc(
        std::size_t size, tr::mem::alloc_hint_t hint = tr::mem::alloc_hint_t::NONE) override {
        if (budget_ == 0) {
            ++refusals_;
            return nullptr;
        }
        if (budget_ > 0) --budget_;
        tr::view::segment_t* const seg = up_.alloc(size, hint);
        if (seg != nullptr) ++served_;
        return seg;
    }
    void destroy(tr::view::segment_t* seg) noexcept override { up_.destroy(seg); }
    [[nodiscard]] std::size_t alignment() const noexcept override { return up_.alignment(); }
    [[nodiscard]] std::size_t max_segment_size() const noexcept override {
        return up_.max_segment_size();
    }

    /** @brief Refuse every subsequent allocation. */
    void arm() noexcept { budget_ = 0; }
    /** @brief Serve without limit (the positive control's precondition). */
    void disarm() noexcept { budget_ = -1; }
    /** @brief Serve @p k more allocations, then refuse — the sweep's knob. */
    void refuse_after(int k) noexcept { budget_ = k; }
    /** @brief How many allocations were REFUSED — the instrument check. */
    [[nodiscard]] int refusals() const noexcept { return refusals_; }
    /** @brief How many were SERVED — the "the seam is consulted at all" check. */
    [[nodiscard]] int served() const noexcept { return served_; }
    void reset_counts() noexcept {
        refusals_ = 0;
        served_ = 0;
    }

   private:
    tr::mem::mem_backend_t& up_;
    int budget_ = -1;  // <0 ⇒ unlimited
    int refusals_ = 0;
    int served_ = 0;
};

/** @brief A link that records what the router sends back, and can push ropes upward. */
class rec_link_t : public transport_t {
   public:
    explicit rec_link_t(bool ropes = false) : ropes_(ropes) {}
    void send(std::span<const std::byte> frame) override {
        sent.emplace_back(frame.begin(), frame.end());
    }
    [[nodiscard]] bool delivers_ropes() const override { return ropes_; }
    void inject(tr::view::rope_t frame) { rx_.deliver_rope(std::move(frame)); }
    std::vector<std::vector<std::byte>> sent;

   private:
    bool ropes_ = false;
};

// --- wire builders (the fwd_flatten_backend_test shapes) -----------------------------

/** @brief Append @p src to @p dst. */
void append(std::vector<std::byte>& dst, const std::vector<std::byte>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

/** @brief A `PATH` TLV over the given `/`-segments. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

/** @brief An opaque `VALUE` TLV holding a little-endian `u32`. */
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::array<std::byte, 4> raw{};
    tr::detail::store_le(std::span<std::byte>(raw), v, 4);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, raw);
    return out;
}

/** @brief An opaque `VALUE` TLV holding one byte. */
std::vector<std::byte> b_value_u8(std::uint8_t v) {
    const std::byte b{v};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}

/** @brief A `FWD` frame with the RFC-0004 §B child order. */
std::vector<std::byte> b_fwd(fwd_op_t op, const std::vector<std::byte>& dst,
                             const std::vector<std::byte>& src,
                             const std::vector<std::byte>& payload = {}) {
    std::vector<std::byte> body;
    append(body, b_value_u8(static_cast<std::uint8_t>(op)));
    append(body, dst);
    append(body, src);
    if (!payload.empty()) append(body, payload);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

/**
 * @brief A rope over @p bytes split into @p links links — the multi-link shape is what makes
 *        the terminus resolver FLATTEN instead of adopting its one link zero-copy.
 *
 * The links are heap segments allocated OUTSIDE any measured window (the transport's own
 * receive buffers stand in for them on a real link), so they never appear in the counts the
 * cases compare.
 */
tr::view::rope_t as_rope(std::span<const std::byte> bytes, std::size_t links) {
    tr::view::rope_t r;
    if (bytes.empty() || links == 0) return r;
    const std::size_t step = (bytes.size() + links - 1) / links;
    for (std::size_t off = 0; off < bytes.size(); off += step) {
        const std::size_t n = std::min(step, bytes.size() - off);
        tr::view::segment_ptr_t seg = tr::view::heap_alloc(n);
        std::memcpy(seg->bytes.data(), bytes.data() + off, n);
        r.append(tr::view::view_t::over(std::move(seg)));
    }
    return r;
}

// --- reply readers -------------------------------------------------------------------

/** @brief What one reply frame IS — the three facts every case below asserts on. */
struct reply_facts_t {
    bool is_fwd_reply = false;   /**< @brief An `FWD` whose op child is `REPLY`. */
    bool kind_error = false;     /**< @brief Its `kind` child is `ERROR` (vs `RESULT`). */
    std::uint16_t code = 0;      /**< @brief The `STATUS{ERROR{VALUE u16}}` registered code. */
    std::size_t route_bytes = 0; /**< @brief Bytes in the reply's `dst` PATH — 0 ⇒ truncated. */
};

/** @brief Read @p frame as an `FWD{REPLY}` — an unparseable frame answers all-false. */
reply_facts_t read_reply(std::span<const std::byte> frame) {
    reply_facts_t f;
    const auto dec = tr::wire::decode(frame);
    if (!dec || dec->type != type_t::FWD || dec->children.size() < 4) return f;
    const tlv_t& op = dec->children[0];
    if (op.type != type_t::VALUE || op.payload.size() != 1) return f;
    if (static_cast<fwd_op_t>(std::to_integer<std::uint8_t>(op.payload[0])) != fwd_op_t::REPLY)
        return f;
    f.is_fwd_reply = true;
    if (dec->children[1].type == type_t::PATH) f.route_bytes = dec->children[1].children.size();
    const tlv_t& kind = dec->children[3];
    if (kind.type != type_t::VALUE || kind.payload.size() != 1) return f;
    f.kind_error = static_cast<reply_kind_t>(std::to_integer<std::uint8_t>(kind.payload[0])) ==
                   reply_kind_t::ERROR;
    if (!f.kind_error || dec->children.size() < 5) return f;
    const tlv_t& status = dec->children[4];
    if (status.type != type_t::STATUS || status.children.size() != 1) return f;
    const tlv_t& err = status.children[0];
    if (err.type != type_t::ERROR || err.children.empty()) return f;
    const tlv_t& id = err.children[0];
    if (id.type == type_t::VALUE && id.payload.size() == 2)
        f.code = tr::detail::load_le<std::uint16_t>(id.payload);
    return f;
}

/**
 * @brief The registered code an addressed `BACKPRESSURE` refusal carries (RFC-0002 §D:
 *        `tr::flow::backpressure`) — read off the public registry, not off the resolver's
 *        internal `status_t` → `err_t` map, so the assertion is about the WIRE.
 */
[[nodiscard]] std::uint16_t backpressure_code() {
    return std::to_underlying(tr::wire::err_t::FLOW_BACKPRESSURE);
}

/** @brief The `u32` a vertex currently holds, or `nullopt` if it holds nothing usable. */
std::optional<std::uint32_t> stored_u32(const graph_t& g, vertex_handle_t v) {
    const auto ref = g.read(v);
    if (!ref || !*ref) return std::nullopt;
    if ((*ref)->total_length() == 0 || (*ref)->link_count() != 1) return std::nullopt;
    const auto tlv = tr::wire::decode((*ref)->only());
    if (!tlv || tlv->payload.size() != 4) return std::nullopt;
    return tr::detail::load_le<std::uint32_t>(tlv->payload);
}

/** @brief The fixture every case shares: `/sensor/temp` behind a rope-delivering link. */
struct node_t {
    graph_t g;
    vertex_handle_t temp = g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    rec_link_t in{/*ropes=*/true};
};

/** @brief The 4-link fragmented terminus READ the #766 measurement used. */
std::vector<std::byte> read_frame() {
    return b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"origin"}));
}

// --- the fix: the terminus flattens are the injected seam's -------------------------

/**
 * @brief A fragmented terminus READ flattens through the INJECTED backend, and those bytes
 *        leave the global heap.
 *
 * Two claims, two instruments. `served() > 0` is the seam claim: with `ensure_cache` /
 * `own_wire` back on `rope_t::materialize`'s default the count is ZERO — the #766
 * measurement's headline (a 4-link `FWD{READ}` consulted the injection zero times). The
 * global-new comparison is the RAM claim: the same request, once with `flat` on a
 * static-slab `pool_t` and once on the heap backend, must cost strictly fewer global
 * allocations in the pool arm — the flatten's bytes came out of the slab.
 */
void test_terminus_read_draws_from_the_injected_seam() {
    std::printf("a fragmented terminus READ flattens through the injected backend:\n");
    const std::vector<std::byte> frame = read_frame();

    // Arm 1 — the seam on a static slab. No global allocation can serve it.
    static std::array<std::byte, 64 * 1024> slab{};
    tr::mem::pool_t pool(slab, 2048);
    arming_backend_t seam(pool);
    std::size_t pool_allocs = 0;
    {
        node_t n;
        (void)n.g.write(n.temp, tr::view::rope_t(*tr::view::over_bytes(b_value_u32(0x2A2A2A2Au))));
        fwd_router_t router(n.g, std::pmr::get_default_resource(), &tr::mem::heap_source(), &seam);
        router.add_child("in", n.in);
        tr::view::rope_t rope = as_rope(frame, 4);  // built OUTSIDE the window
        g_allocs = 0;
        g_bytes = 0;
        g_arm = true;
        n.in.inject(std::move(rope));
        g_arm = false;
        pool_allocs = g_allocs;
        check(seam.served() > 0,
              "instrument: the injected backend was CONSULTED and served the terminus flattens");
        check(n.in.sent.size() == 1, "and the request was answered");
        if (n.in.sent.size() == 1) {
            const reply_facts_t f = read_reply(n.in.sent[0]);
            check(f.is_fwd_reply && !f.kind_error, "with a kind=RESULT reply (nothing degraded)");
        }
    }

    // Arm 2 — the same request with the seam left on the global heap (the pre-#766 draw).
    std::size_t heap_allocs = 0;
    {
        node_t n;
        (void)n.g.write(n.temp, tr::view::rope_t(*tr::view::over_bytes(b_value_u32(0x2A2A2A2Au))));
        fwd_router_t router(n.g);  // defaults: flat = heap_backend()
        router.add_child("in", n.in);
        tr::view::rope_t rope = as_rope(frame, 4);
        g_allocs = 0;
        g_bytes = 0;
        g_arm = true;
        n.in.inject(std::move(rope));
        g_arm = false;
        heap_allocs = g_allocs;
        check(n.in.sent.size() == 1, "the un-injected default answers the same request");
    }

    std::printf("    global new: %zu calls with the seam on a slab, %zu on the heap (%d served)\n",
                pool_allocs, heap_allocs, seam.served());
    check(pool_allocs + static_cast<std::size_t>(seam.served()) <= heap_allocs,
          "and every flatten the seam served is a global allocation that did NOT happen");
}

// --- the refusal: answered by value, never read short ------------------------------

/**
 * @brief A refusing seam under a fragmented terminus READ answers the refusal — no reply
 *        built on a short span, and no `kind=RESULT`.
 *
 * With the sites on the global heap this injection cannot fail at all: the refusal count is
 * zero and a full RESULT reply goes out, so both checks below redden on the ablation.
 */
void test_terminus_read_refusal_is_answered() {
    std::printf("a fragmented terminus READ under a refusing seam answers the refusal:\n");
    node_t n;
    (void)n.g.write(n.temp, tr::view::rope_t(*tr::view::over_bytes(b_value_u32(0x33333333u))));
    arming_backend_t seam;
    fwd_router_t router(n.g, std::pmr::get_default_resource(), &tr::mem::heap_source(), &seam);
    router.add_child("in", n.in);

    seam.arm();
    n.in.inject(as_rope(read_frame(), 4));
    check(seam.refusals() > 0,
          "instrument: the injected backend was ASKED and refused (a flatten really happened)");
    check(n.in.sent.size() <= 1, "at most one frame is answered");
    if (n.in.sent.empty()) {
        check(true, "the refusal is answered by value — the request is dropped, never aborted");
    } else {
        const reply_facts_t f = read_reply(n.in.sent[0]);
        check(f.is_fwd_reply && f.kind_error && f.code == backpressure_code(),
              "the refusal is answered as an addressed kind=ERROR STATUS{BACKPRESSURE}");
        check(f.route_bytes > 0, "and its return route is INTACT — never a truncated reply");
    }

    // The positive control: the identical request is served once memory returns, so the
    // answer above was the exhaustion and not a frame that never reached the terminus.
    seam.disarm();
    n.in.sent.clear();
    n.in.inject(as_rope(read_frame(), 4));
    check(n.in.sent.size() == 1, "the same request is answered once memory returns");
    if (n.in.sent.size() == 1) {
        const reply_facts_t f = read_reply(n.in.sent[0]);
        check(f.is_fwd_reply && !f.kind_error, "with the kind=RESULT reply it always gave");
    }
}

/**
 * @brief A refusing seam under a fragmented terminus WRITE stores NOTHING — the vertex keeps
 *        its previous value.
 *
 * The positive observable: a drop is invisible, a preserved value is not. Pre-#766 this
 * WRITE lands under the same injection (the ownership flatten drew from the global heap),
 * so the assertion fails loudly against the un-seamed code.
 */
void test_terminus_write_refusal_stores_nothing() {
    std::printf("a fragmented terminus WRITE under a refusing seam stores nothing:\n");
    node_t n;
    constexpr std::uint32_t kFirst = 0x44444444u;
    constexpr std::uint32_t kSecond = 0x55555555u;
    (void)n.g.write(n.temp, tr::view::rope_t(*tr::view::over_bytes(b_value_u32(kFirst))));
    arming_backend_t seam;
    fwd_router_t router(n.g, std::pmr::get_default_resource(), &tr::mem::heap_source(), &seam);
    router.add_child("in", n.in);

    const std::vector<std::byte> write = b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}),
                                               b_path({"origin"}), b_value_u32(kSecond));

    seam.arm();
    n.in.inject(as_rope(write, 4));
    check(seam.refusals() > 0, "instrument: the injected backend was ASKED and refused");
    check(stored_u32(n.g, n.temp) == kFirst,
          "the vertex still holds the PREVIOUS value — the refused flatten was not stored");
    if (!n.in.sent.empty()) {
        const reply_facts_t f = read_reply(n.in.sent[0]);
        check(f.is_fwd_reply && f.kind_error && f.code == backpressure_code(),
              "and any answer is an addressed BACKPRESSURE, never a RESULT ack");
    }

    // The positive control: the same WRITE lands once memory returns.
    seam.disarm();
    n.in.sent.clear();
    n.in.inject(as_rope(write, 4));
    check(stored_u32(n.g, n.temp) == kSecond, "and the next write lands once memory returns");
}

/**
 * @brief The refusal SWEEP: move the exhaustion point across EVERY flatten one request
 *        makes, and require each outcome to be one of the two documented answers.
 *
 * One armed case only ever proves the FIRST flatten's guard. A resolve reads spans at
 * several points — the reply's own route bytes, the op discriminant, the dst lookup key —
 * and each is a separate opportunity to read a short span and answer wrongly. The sweep
 * refuses at k = 0, 1, 2, … flattens and asserts the invariant that has to hold at every k:
 * the answer is either nothing (a drop) or an addressed `kind=ERROR BACKPRESSURE` with an
 * intact return route. A `kind=RESULT` at any k means a span was read short and believed;
 * a truncated route means the reply was built on one.
 */
void test_terminus_refusal_sweep() {
    std::printf("moving the refusal across every flatten keeps the answer sound:\n");
    const std::vector<std::byte> frame = read_frame();

    // How many flattens does this request make when nothing refuses? That is the sweep's
    // upper bound — and its own instrument: zero means the seam is not consulted at all.
    int total = 0;
    {
        node_t n;
        arming_backend_t seam;
        fwd_router_t router(n.g, std::pmr::get_default_resource(), &tr::mem::heap_source(), &seam);
        router.add_child("in", n.in);
        (void)n.g.write(n.temp, tr::view::rope_t(*tr::view::over_bytes(b_value_u32(0x66666666u))));
        n.in.inject(as_rope(frame, 4));
        total = seam.served();
    }
    check(total > 0, "instrument: the un-refused request DOES flatten through the seam");

    int drops = 0;
    int addressed = 0;
    bool sound = true;
    for (int k = 0; k < total; ++k) {
        node_t n;
        arming_backend_t seam;
        fwd_router_t router(n.g, std::pmr::get_default_resource(), &tr::mem::heap_source(), &seam);
        router.add_child("in", n.in);
        (void)n.g.write(n.temp, tr::view::rope_t(*tr::view::over_bytes(b_value_u32(0x66666666u))));
        seam.refuse_after(k);
        n.in.inject(as_rope(frame, 4));
        if (seam.refusals() == 0) continue;  // this k never reached a flatten
        if (n.in.sent.empty()) {
            ++drops;
            continue;
        }
        const reply_facts_t f = read_reply(n.in.sent[0]);
        const bool ok =
            f.is_fwd_reply && f.kind_error && f.code == backpressure_code() && f.route_bytes > 0;
        if (ok) {
            ++addressed;
        } else {
            sound = false;
            std::printf("    refusal after %d served: unsound answer (reply=%d error=%d code=%u)\n",
                        k, static_cast<int>(f.is_fwd_reply), static_cast<int>(f.kind_error),
                        static_cast<unsigned>(f.code));
        }
    }
    std::printf("    %d refusal points: %d dropped, %d answered addressed BACKPRESSURE\n", total,
                drops, addressed);
    check(sound,
          "every refusal point answers a drop or an addressed BACKPRESSURE — never a RESULT");
    check(drops + addressed > 0, "instrument: the sweep actually hit refusals");
}

// --- the default path: byte-identical, no regression -------------------------------

/**
 * @brief The defaulted parameter keeps the terminus behaviour it had — the span (arena) tier
 *        and an un-injected rope terminus both answer exactly as before.
 *
 * The RAM rule for this change: the default is still `heap_backend()`, so a caller that
 * injects nothing pays nothing and sees no behavioural difference.
 */
void test_default_backend_unchanged() {
    std::printf("an un-injected node answers both tiers exactly as before:\n");
    node_t n;
    (void)n.g.write(n.temp, tr::view::rope_t(*tr::view::over_bytes(b_value_u32(0x77777777u))));
    fwd_router_t router(n.g);  // no backend argument at all
    router.add_child("in", n.in);

    const std::vector<std::byte> frame = read_frame();
    router.on_frame("in", frame);  // the SPAN (arena) tier
    check(n.in.sent.size() == 1, "the span-tier terminus answers");
    const std::vector<std::byte> span_reply =
        n.in.sent.empty() ? std::vector<std::byte>{} : n.in.sent[0];
    n.in.sent.clear();
    n.in.inject(as_rope(frame, 4));  // the ROPE tier, multi-link
    check(n.in.sent.size() == 1, "the rope-tier terminus answers");
    if (!span_reply.empty() && n.in.sent.size() == 1)
        check(span_reply == n.in.sent[0],
              "and the two tiers are byte-identical (the ADR-0053 differential oracle holds)");
}

}  // namespace

int main() {
    std::printf("terminus rope-flatten backend seam (#766)\n\n");

    test_terminus_read_draws_from_the_injected_seam();
    std::printf("\n");
    test_terminus_read_refusal_is_answered();
    std::printf("\n");
    test_terminus_write_refusal_stores_nothing();
    std::printf("\n");
    test_terminus_refusal_sweep();
    std::printf("\n");
    test_default_backend_unchanged();

    std::printf("\n%s\n", g_failures == 0 ? "all checks passed" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
