/**
 * @file
 * @brief #795 — the terminus FWD{REPLY}'s egress-construction segments (the reply HEAD and, on a
 *        mint, the trailing `PATH_REF`) draw from the router's injected `egress` backend, not the
 *        global heap — and a refusal degrades by value, never an abort.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The reply head was the last reply-egress byte source a bounded node could not bound: it was
 * hard-wired to `view::heap_alloc`'s global heap (`op_resolve_walk.hpp` `assemble`, two
 * `heap_alloc` sites — the head and the RFC-0024 mint), reached on EVERY reply and sized by the
 * peer's swapped route bytes. ADR-0074 gives it its own injection, kept separate from `flat`
 * (the flatten seam, sized against payload bytes) so a slab deployment sized for flattens is not
 * silently re-scoped.
 *
 * @section instrument The instrument is checked before the guard is
 *
 * "Draws from the seam" is non-vacuous only against the old code: with the head back on
 * `view::heap_alloc` the injected `egress` backend is asked for NOTHING and `served() == 0`. So
 * every armed case asserts `served() > 0` (or the exact mint size) FIRST — re-point `assemble`'s
 * two allocations at `view::heap_alloc` and the first check of every case here reddens.
 *
 * The span (arena) tier is the case that matters for the MCU class: a synchronous CAN/UART child
 * delivers a contiguous SPAN, and for a READ the arena tier borrows every span — so the reply
 * HEAD is the ONLY allocation the whole resolve makes. `served() == 1` on that path is therefore
 * exactly the head, and the global-new comparison isolates it cleanly.
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
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_pool.hpp"
#include "libtracer/op_resolve.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

/** @brief Global-new call counter, live only while @ref g_arm is set. */
std::size_t g_allocs = 0;
bool g_arm = false;

/** @brief The counted allocation itself — malloc-backed so `operator delete` can free it. */
void* counted(std::size_t n) {
    if (g_arm) ++g_allocs;
    return std::malloc(n == 0 ? 1 : n);
}

/**
 * @brief The aligned counted allocation — `aligned_alloc` for a genuinely OVER-aligned request
 *        (which `free` accepts), `malloc` for a fundamental one.
 */
void* counted_aligned(std::size_t n, std::size_t align) {
    if (align <= alignof(std::max_align_t)) return counted(n);
    if (g_arm) ++g_allocs;
    const std::size_t rounded = ((n == 0 ? 1 : n) + align - 1) / align * align;
    return std::aligned_alloc(align, rounded);
}

}  // namespace

// Every allocating and deallocating form is replaced (the `terminus_flatten_backend_test`
// precedent): the heap backend allocates through `::operator new(bytes, std::nothrow)`, so a
// hole in the set would make the head INVISIBLE to the counter, and a missing delete form is an
// `alloc-dealloc-mismatch` under ASan.
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

/** @brief The minted `PATH_REF` reply trailer's byte length (one element): 4-byte header + 8. */
constexpr std::size_t kMintBytes = tr::wire::path_ref_wire_bytes(1);

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/**
 * @brief A `mem_backend_t` that serves from an upstream backend until its serve budget runs out,
 *        then refuses — the heap-exhaustion stand-in, with the exhaustion point movable.
 *
 * Delegation, not a private allocator (the `terminus_flatten_backend_test` precedent): a served
 * segment is the upstream's own, so it reclaims exactly as an un-injected resolver's would. The
 * ONLY difference between serving and refusing is the `nullptr`.
 */
class arming_backend_t final : public tr::mem::mem_backend_t {
   public:
    explicit arming_backend_t(tr::mem::mem_backend_t& upstream = tr::mem::heap_backend()) noexcept
        : mem_backend_t("test_egress"), up_(upstream) {}

    [[nodiscard]] tr::view::segment_t* alloc(
        std::size_t size, tr::mem::alloc_hint_t hint = tr::mem::alloc_hint_t::NONE) override {
        if (refuse_first_ > 0) {  // refuse the leading N draws, then serve
            --refuse_first_;
            ++refusals_;
            return nullptr;
        }
        if (budget_ == 0) {
            ++refusals_;
            return nullptr;
        }
        if (budget_ > 0) --budget_;
        tr::view::segment_t* const seg = up_.alloc(size, hint);
        if (seg != nullptr) {
            ++served_;
            sizes_.push_back(size);
        }
        return seg;
    }
    void destroy(tr::view::segment_t* seg) noexcept override { up_.destroy(seg); }
    [[nodiscard]] std::size_t alignment() const noexcept override { return up_.alignment(); }
    [[nodiscard]] std::size_t max_segment_size() const noexcept override {
        return up_.max_segment_size();
    }

    /** @brief Refuse every subsequent allocation. */
    void arm() noexcept { budget_ = 0; }
    /** @brief Serve without limit. */
    void disarm() noexcept { budget_ = -1; }
    /** @brief Refuse the first @p k allocations, then serve — isolates the reply-head refusal. */
    void refuse_first(int k) noexcept { refuse_first_ = k; }
    /** @brief How many allocations were REFUSED — the refusal instrument. */
    [[nodiscard]] int refusals() const noexcept { return refusals_; }
    /** @brief How many were SERVED — the "the seam is consulted at all" instrument. */
    [[nodiscard]] int served() const noexcept { return served_; }
    /** @brief Was an allocation of EXACTLY @p n bytes served (the site-specific instrument)? */
    [[nodiscard]] bool served_size(std::size_t n) const noexcept {
        return std::find(sizes_.begin(), sizes_.end(), n) != sizes_.end();
    }
    /** @brief The largest size served — the reply head is the largest egress draw a reply makes. */
    [[nodiscard]] std::size_t max_served_size() const noexcept {
        return sizes_.empty() ? 0 : *std::max_element(sizes_.begin(), sizes_.end());
    }

   private:
    tr::mem::mem_backend_t& up_;
    int budget_ = -1;       // <0 ⇒ unlimited
    int refuse_first_ = 0;  // leading draws to refuse before the budget applies
    int refusals_ = 0;
    int served_ = 0;
    std::vector<std::size_t> sizes_{};
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

// --- wire builders (the terminus_flatten_backend_test shapes) ------------------------

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

using tr::testing::b_fwd;
using tr::testing::b_fwd_mint;

/**
 * @brief A rope over @p bytes split into @p links links — the multi-link shape that makes the
 *        terminus resolve run on the ROPE tier.
 *
 * The links are heap segments allocated OUTSIDE any measured window, so they never appear in the
 * counts the cases compare.
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

/** @brief What one reply frame IS — the facts every case below asserts on. */
struct reply_facts_t {
    bool is_fwd_reply = false;   /**< @brief An `FWD` whose op child is `REPLY`. */
    bool kind_error = false;     /**< @brief Its `kind` child is `ERROR` (vs `RESULT`). */
    bool has_path_ref = false;   /**< @brief A `PATH_REF` child is present (the mint). */
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
    for (const tlv_t& c : dec->children)
        if (c.type == type_t::PATH_REF) f.has_path_ref = true;
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
 *        `tr::flow::backpressure`) — read off the public registry so the assertion is about the
 *        WIRE, not the resolver's internal status map.
 */
[[nodiscard]] std::uint16_t backpressure_code() {
    return std::to_underlying(tr::wire::err_t::FLOW_BACKPRESSURE);
}

/** @brief The fixture every case shares: `/sensor/temp`, behind a link. */
struct node_t {
    graph_t g;
    vertex_handle_t temp = g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    rec_link_t in;
};

/** @brief A span-delivered (arena tier) terminus READ of `/sensor/temp`. */
std::vector<std::byte> read_frame() {
    return b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"origin"}));
}

// --- (a) the span (arena) tier reply head is the injected egress backend's ------------

/**
 * @brief A span-delivered terminus READ builds its reply HEAD from the injected egress backend,
 *        and those bytes leave the global heap.
 *
 * The arena tier borrows every span on a READ, so the reply head is the ONLY allocation the
 * resolve makes: `served() == 1` is exactly the head. Two instruments — `served() > 0` (the
 * seam is consulted; ZERO on the pre-#795 code) and the strict global-new comparison (the head's
 * bytes came out of the slab, not `malloc`).
 */
void test_span_tier_reply_head_draws_from_egress() {
    std::printf("a span-delivered terminus READ builds its reply head from the egress backend:\n");
    const std::vector<std::byte> frame = read_frame();

    static std::array<std::byte, 64 * 1024> slab{};
    tr::mem::pool_t pool(slab, 2048);
    arming_backend_t egress(pool);
    std::size_t pool_allocs = 0;
    std::size_t head_size = 0;
    {
        node_t n;
        (void)n.g.write(n.temp, tr::view::rope_t(*tr::view::over_bytes(b_value_u32(0x2A2A2A2Au))));
        // egress is the 6th ctor arg; flat/rx keep their heap defaults so the ONLY injected
        // backend under test here is the reply-egress one.
        fwd_router_t router(n.g, std::pmr::get_default_resource(), &tr::mem::heap_source(),
                            &tr::mem::heap_backend(), 0, &egress);
        router.add_child("in", n.in);
        g_allocs = 0;
        g_arm = true;
        router.on_frame("in", frame);  // the SPAN (arena) tier — no rope, no frame_view
        g_arm = false;
        pool_allocs = g_allocs;
        head_size = egress.max_served_size();

        check(egress.served() > 0,
              "instrument: the egress backend was CONSULTED for the reply head (zero pre-#795)");
        check(n.in.sent.size() == 1, "and the request was answered");
        if (n.in.sent.size() == 1) {
            const reply_facts_t f = read_reply(n.in.sent[0]);
            check(f.is_fwd_reply && !f.kind_error, "with a kind=RESULT reply (nothing degraded)");
        }
    }

    std::size_t heap_allocs = 0;
    {
        node_t n;
        (void)n.g.write(n.temp, tr::view::rope_t(*tr::view::over_bytes(b_value_u32(0x2A2A2A2Au))));
        fwd_router_t router(n.g);  // defaults: egress = heap_backend()
        router.add_child("in", n.in);
        g_allocs = 0;
        g_arm = true;
        router.on_frame("in", frame);
        g_arm = false;
        heap_allocs = g_allocs;
        check(n.in.sent.size() == 1, "the un-injected default answers the same request");
    }

    std::printf(
        "    global new: %zu with egress on a slab, %zu on the heap (head %zu B, %d served)\n",
        pool_allocs, heap_allocs, head_size, egress.served());
    check(
        head_size > kMintBytes,
        "instrument: the head is the largest egress draw — bigger than a mint, so it is the head");
    check(pool_allocs < heap_allocs,
          "and the reply head is STRICTLY fewer global allocations on the slab — it left the heap");
}

// --- (c) the RFC-0024 mint site draws from egress too --------------------------------

/**
 * @brief A mint-requesting terminus READ draws BOTH its reply head and its trailing `PATH_REF`
 *        from the egress backend — the second `assemble` allocation (`op_resolve_walk.hpp` :589).
 *
 * `served_size(kMintBytes)` is the site-specific instrument: the mint is a fixed 12-byte segment
 * and nothing else on this path asks for that exact size (the head is always larger). Put the
 * mint back on `view::heap_alloc` and this check alone reddens.
 */
void test_mint_site_draws_from_egress() {
    std::printf("a mint-requesting terminus READ draws head AND mint from the egress backend:\n");
    node_t n;
    (void)n.g.write(n.temp, tr::view::rope_t(*tr::view::over_bytes(b_value_u32(0x5A5A5A5Au))));
    arming_backend_t egress;
    fwd_router_t router(n.g, std::pmr::get_default_resource(), &tr::mem::heap_source(),
                        &tr::mem::heap_backend(), 0, &egress);
    router.add_child("in", n.in);

    router.on_frame("in",
                    b_fwd_mint(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"origin"})));

    check(egress.served() >= 2, "instrument: the egress backend served at least the head and mint");
    check(egress.served_size(kMintBytes),
          "instrument: the egress backend served the exact 12-byte mint segment (:589)");
    check(n.in.sent.size() == 1, "the mint request is answered");
    if (n.in.sent.size() == 1) {
        const reply_facts_t f = read_reply(n.in.sent[0]);
        check(f.is_fwd_reply && !f.kind_error, "with a kind=RESULT reply");
        check(f.has_path_ref, "carrying the minted PATH_REF trailer (the mint really happened)");
    }
}

// --- (b) exhaustion is answered by value, never an abort -----------------------------

/**
 * @brief A fully-refusing egress backend under a terminus READ answers BY VALUE — the resolve
 *        does not abort, and no `kind=RESULT` reply is built on bytes it could not allocate.
 *
 * When egress refuses the RESULT head the rope is empty; `or_backpressure` then tries the error
 * head, which the same refusing backend also declines — so the whole reply cannot be built and
 * the router DROPS it. A drop is the by-value answer here (never an abort). The reachability of
 * the addressed BACKPRESSURE reply — where the error head CAN be built — is the next case.
 */
void test_egress_refusal_is_answered_by_value() {
    std::printf("a terminus READ under a fully-refusing egress backend is answered by value:\n");
    node_t n;
    (void)n.g.write(n.temp, tr::view::rope_t(*tr::view::over_bytes(b_value_u32(0x33333333u))));
    arming_backend_t egress;
    fwd_router_t router(n.g, std::pmr::get_default_resource(), &tr::mem::heap_source(),
                        &tr::mem::heap_backend(), 0, &egress);
    router.add_child("in", n.in);

    egress.arm();
    router.on_frame("in", read_frame());
    check(egress.refusals() > 0,
          "instrument: the egress backend was ASKED and refused (a head alloc really happened)");
    check(n.in.sent.empty(),
          "the reply cannot be built at all, so it is DROPPED — by value, never an abort");

    // The positive control: the same request is served once memory returns, so the drop above
    // was the exhaustion and not a frame that never reached the terminus.
    egress.disarm();
    n.in.sent.clear();
    router.on_frame("in", read_frame());
    check(n.in.sent.size() == 1, "the same request is answered once memory returns");
    if (n.in.sent.size() == 1) {
        const reply_facts_t f = read_reply(n.in.sent[0]);
        check(f.is_fwd_reply && !f.kind_error, "with the kind=RESULT reply it always gave");
    }
}

/**
 * @brief The addressed-BACKPRESSURE degrade: when the RESULT reply's head is refused but the
 *        smaller error head CAN be allocated, the terminus answers an addressed
 *        `kind=ERROR STATUS{BACKPRESSURE}` — built from the egress backend.
 *
 * This is the reply path's must-not-drop guarantee (`or_backpressure`): a reply whose RESULT
 * assembly failed on OOM is answered by value on the SAME link, never a silent drop. The
 * exhaustion is targeted at the FIRST egress draw only — the RESULT head — so the head alloc
 * refuses (empty rope), `or_backpressure` runs, and the error head is the SECOND egress draw,
 * which is served. `served() > 0` proves that error head came from the injection (the global
 * heap on the pre-#795 code); the addressed BACKPRESSURE with an intact route proves the
 * degrade answered by value.
 */
void test_saturated_reply_degrades_to_addressed_backpressure() {
    std::printf("a refused RESULT head degrades to an addressed BACKPRESSURE built from egress:\n");
    node_t n;
    (void)n.g.write(n.temp, tr::view::rope_t(*tr::view::over_bytes(b_value_u32(0x44444444u))));
    arming_backend_t egress;
    fwd_router_t router(n.g, std::pmr::get_default_resource(), &tr::mem::heap_source(),
                        &tr::mem::heap_backend(), 0, &egress);
    router.add_child("in", n.in);

    // Refuse ONLY the RESULT head (the first egress draw): the rope is empty, `or_backpressure`
    // fires, and the error head — the second draw — is served. That is the saturated-snapshot
    // degrade `or_backpressure` exists for, answered by value on the same link.
    egress.refuse_first(1);
    router.on_frame("in", read_frame());

    check(egress.refusals() > 0, "instrument: the RESULT head draw was refused");
    check(egress.served() > 0,
          "instrument: and the egress backend served the degraded reply's error head");
    check(n.in.sent.size() == 1, "the saturated reply is answered, not dropped");
    if (n.in.sent.size() == 1) {
        const reply_facts_t f = read_reply(n.in.sent[0]);
        check(f.is_fwd_reply && f.kind_error && f.code == backpressure_code(),
              "as an addressed kind=ERROR STATUS{BACKPRESSURE}");
        check(f.route_bytes > 0, "with an INTACT return route — never a truncated reply");
    }
}

// --- (d) the default path is unchanged ----------------------------------------------

/**
 * @brief The defaulted egress parameter keeps the terminus behaviour it had — both tiers answer
 *        exactly as before, and the two are byte-identical (the ADR-0053 differential oracle).
 *
 * The RAM rule for this change: the default is still `heap_backend()`, so a caller that injects
 * nothing pays nothing and sees no behavioural difference.
 */
void test_default_egress_unchanged() {
    std::printf("an un-injected node answers both tiers exactly as before:\n");
    node_t n;
    (void)n.g.write(n.temp, tr::view::rope_t(*tr::view::over_bytes(b_value_u32(0x77777777u))));
    fwd_router_t router(n.g);  // no egress argument at all
    router.add_child("in", n.in);

    const std::vector<std::byte> frame = read_frame();
    router.on_frame("in", frame);  // the SPAN (arena) tier
    check(n.in.sent.size() == 1, "the span-tier terminus answers");
    const std::vector<std::byte> span_reply =
        n.in.sent.empty() ? std::vector<std::byte>{} : n.in.sent[0];
    n.in.sent.clear();

    rec_link_t rope_in{/*ropes=*/true};
    router.add_child("rin", rope_in);
    rope_in.inject(as_rope(b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"origin"})),
                           4));  // the ROPE tier, multi-link
    check(rope_in.sent.size() == 1, "the rope-tier terminus answers");
    if (!span_reply.empty() && rope_in.sent.size() == 1)
        check(span_reply == rope_in.sent[0],
              "and the two tiers are byte-identical (the ADR-0053 differential oracle holds)");
}

}  // namespace

int main() {
    std::printf("terminus reply egress backend seam (#795, ADR-0074)\n\n");

    test_span_tier_reply_head_draws_from_egress();
    std::printf("\n");
    test_mint_site_draws_from_egress();
    std::printf("\n");
    test_egress_refusal_is_answered_by_value();
    std::printf("\n");
    test_saturated_reply_degrades_to_addressed_backpressure();
    std::printf("\n");
    test_default_egress_unchanged();

    std::printf("\n%s\n", g_failures == 0 ? "all checks passed" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
