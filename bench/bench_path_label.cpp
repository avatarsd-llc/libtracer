/**
 * @file
 * @brief RFC-0027 §12.4 clauses 1 and 2 — what a PATH LABEL saves per hop, as a SLOPE over hop
 *        count, and what it does (and does not) do to the terminus residual.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0027 §12.4 makes this measurement a **condition of acceptance** of the implementation, and
 * it names the instrument as precisely as it names the question: *"a single-hop point measurement
 * is the wrong instrument for a per-hop claim"* — the correction RFC-0024 §8.4 had to make on
 * itself. So this bench sweeps HOP COUNT and reports a regression slope, and it never quotes a
 * one-hop delta as a per-hop figure.
 *
 * @section arms The two arms, and why they are comparable
 *
 * A chain of `H + 1` nodes, each a real @ref tr::net::fwd_router_t over its own
 * @ref tr::graph::graph_t, linked by in-process transports that hand bytes straight to the next
 * node's receiver. Every hop does the real routing work; only the socket is absent.
 *
 *  - `plabel-string` — `FWD{op=READ}` whose `dst` is the full canonical address: three segments
 *    per hop (`net/<module>/<name>`, the shape RFC-0023 prices) and then the leaf.
 *  - `plabel-label` — the **identical frame** but for its `dst`, which spells each hop's whole
 *    mount run as that hop's minted 7-byte path-label element (§5.3.3, amendment 6).
 *
 * `bench_hop_chain` paid for the rule this file obeys: its first published delta was ~40 % reply
 * leg because one arm sent a `WRITE` that generated a `RESULT` and the other sent a `COMPACT`
 * that generated nothing, and a delivery assertion cannot see that. Here **both arms send the
 * same opcode to the same vertex over the same graphs, and both nodes' routers carry a mint
 * table**, so the only difference in the whole system is how the destination is spelled. The
 * frames each arm emits are counted **per direction** and a mismatch **voids the run**
 * (non-zero exit), never a footnote.
 *
 * @section labels Where the labels come from
 *
 * Nothing is hand-spelled. The chain is warmed with the STRING arm, which is precisely §6.2's
 * trigger: each hop mints on the reply it relays, and prepends its own label to that reply's
 * `src` (§6.1 erratum 2). The reply that reaches node 0's application therefore carries
 * `[L0][L1]…[L(H-1)]` at the head of its `src` — and that prefix, with the leaf appended, IS the
 * labelled `dst` of the next request. So the label arm's address is read off the wire the
 * implementation produced, which also makes this bench an end-to-end check that §6.1 point 4
 * ("the first reply returns to the original sender with fully-minted `src`") actually holds.
 *
 * @section residual §12.4 clause 2 — the axis that decides, and it is now answerable
 *
 * Clause 2 asks for §3.2's depth table re-run in the label spelling: the TERMINUS residual against
 * address depth, which §3.3 nominates as the axis that decides, because the byte column of §3.3's
 * own table ties `PATH_REF`. This file could not answer it for two cars running, and the reason
 * was always in the implementation rather than in the instrument. Both halves have now landed:
 *
 *  - the terminus **mints** for the residual it resolved (§6.1 point 3, #1357), so the warm reply
 *    carries `H + 1` labels rather than `H` — the terminus's own label at the tail of the run;
 *  - the terminus **dereferences** one (§7.2, #1363), so a `dst` that is nothing but that run is
 *    an address this chain can actually carry rather than an `H + 1`-th `NOT_FOUND`.
 *
 * So the `presid-*` sweep is now a real A/B on the depth axis: the string arm spells the residual
 * as `depth` packed segments and the label arm spells it as **one 7-byte element, at every
 * depth**. Both arms still send the same opcode to the same vertex over the same graphs with the
 * same tables, and the per-direction frame census still voids a point that is not like-for-like.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/path_element.hpp"
#include "libtracer/path_label.hpp"
#include "libtracer/path_label_table.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/transport.hpp"

namespace {

using tr::graph::graph_t;
using tr::net::fwd_router_t;
using tr::net::path_label_table_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Hop counts swept, so the per-hop claim is read off a SLOPE and never off a point. */
constexpr std::array<std::size_t, 4> kHopCounts{1, 2, 4, 8};

/** @brief Terminus residual depths swept on a fixed one-hop route (the §3.2 depth axis). */
constexpr std::array<std::size_t, 5> kResidualDepths{1, 2, 4, 8, 12};

/**
 * @brief Registry widths swept — children per hop, the route's own child plus decoys.
 *
 * §15 clause 2 is worded *"at realistic registry widths"*, and it is the clause this bench can
 * fire. A label deref is a bounds check, a slot load and a generation compare, so it is flat in
 * the registry's size; a mount descent folds a digest chain and compares candidate mount slots,
 * so it is not. A two-child node is the NARROW end of the spectrum and the least favourable
 * width for the label; 64 is the WIDE end `bench_forward_demux` already sweeps. Measuring only
 * one of them would answer the clause for one deployment shape and call it the answer.
 */
constexpr std::array<std::size_t, 3> kRegistryWidths{1, 8, 64};

constexpr double kDefaultBudgetSeconds = 1.0;
constexpr double kPlateau = 0.05;

/** @brief Per-point time budget; `LIBTRACER_BENCH_SECONDS` shortens a local run. */
[[nodiscard]] double budget_seconds() {
    const char* const env = std::getenv("LIBTRACER_BENCH_SECONDS");
    if (env == nullptr) return kDefaultBudgetSeconds;
    const double v = std::strtod(env, nullptr);
    return v > 0.0 ? v : kDefaultBudgetSeconds;
}

/** @brief Grow the batch until the per-op cost stops falling — the clock, not the code. */
template <typename Op>
[[nodiscard]] std::size_t calibrate_batch(Op&& op) {
    double prev = 0.0;
    for (std::size_t batch = 1; batch <= (1U << 18); batch *= 2) {
        const std::uint64_t a = bench::now_ns();
        for (std::size_t i = 0; i < batch; ++i) op();
        const double per = static_cast<double>(bench::now_ns() - a) / static_cast<double>(batch);
        if (prev > 0.0 && per > prev * (1.0 - kPlateau)) return batch;
        prev = per;
    }
    return 1U << 18;
}

/** @brief One end of an in-process link: `send` hands the bytes to the peer end's receiver. */
struct wire_link_t : transport_t {
    wire_link_t* peer = nullptr;
    std::size_t bytes = 0;  /**< @brief Bytes this end has carried. */
    std::size_t frames = 0; /**< @brief Frames this end has carried. */
    /** @brief The last frame this end was handed — the mint evidence the label arm reads. */
    std::vector<std::byte> last;

    /** @brief Push bytes INTO this node, as an inbound frame from the peer. */
    void inject(std::span<const std::byte> f) { rx_.deliver_borrowed(f); }

    void send(std::span<const std::byte> f) override {
        bytes += f.size();
        ++frames;
        last.assign(f.begin(), f.end());
        if (peer != nullptr) peer->inject(f);
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        scratch_.clear();
        for (const auto& s : iov) scratch_.insert(scratch_.end(), s.begin(), s.end());
        bytes += scratch_.size();
        ++frames;
        last = scratch_;
        if (peer != nullptr) peer->inject(scratch_);
    }

   private:
    std::vector<std::byte> scratch_;
};

/** @brief A `PATH` TLV over @p segs, packed-record body (RFC-0018). */
[[nodiscard]] std::vector<std::byte> path_tlv(const std::vector<std::string_view>& segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief A `PATH` TLV whose body is @p prefix verbatim followed by @p segs — the labelled
 *         spelling, whose leading elements are the label records the hops themselves minted. */
[[nodiscard]] std::vector<std::byte> path_tlv_prefixed(std::span<const std::byte> prefix,
                                                       const std::vector<std::string_view>& segs) {
    std::vector<std::byte> body(prefix.begin(), prefix.end());
    for (const std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief One little-endian `u32` as a `VALUE` TLV, appended to @p out. */
void emit_value(std::vector<std::byte>& out, std::uint32_t v) {
    std::array<std::byte, 4> p{};
    for (std::size_t i = 0; i < 4; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
}

/** @brief `FWD{op, dst, src, VALUE}` — the frame both arms carry, differing only in @p dst. */
[[nodiscard]] std::vector<std::byte> build_fwd(std::uint8_t op, std::span<const std::byte> dst,
                                               std::span<const std::byte> src, std::uint32_t v) {
    std::vector<std::byte> body;
    const std::byte opb{op};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&opb, 1));
    body.insert(body.end(), dst.begin(), dst.end());
    body.insert(body.end(), src.begin(), src.end());
    emit_value(body, v);
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

/** @brief The `src` PATH body of an emitted FWD frame — where §6.1 erratum 2's mint lands. */
[[nodiscard]] std::optional<std::vector<std::byte>> src_body_of(std::span<const std::byte> frame) {
    const auto dec = tr::wire::decode(frame);
    if (!dec) return std::nullopt;
    bool seen_dst = false;
    for (const tr::wire::tlv_t& c : dec->children) {
        if (c.type != type_t::PATH) continue;
        if (!seen_dst) {
            seen_dst = true;
            continue;
        }
        return std::vector<std::byte>(c.payload.begin(), c.payload.end());
    }
    return std::nullopt;
}

/** @brief The leading run of LABEL elements of @p body, as raw bytes — nothing else. */
[[nodiscard]] std::vector<std::byte> leading_labels(std::span<const std::byte> body,
                                                    std::size_t& count) {
    count = 0;
    std::size_t off = 0;
    while (off < body.size()) {
        const tr::wire::path_element_t el = tr::wire::path_element_at(body.subspan(off), 0);
        if (el.bytes == 0 || el.kind != tr::wire::path_element_kind_t::LABEL) break;
        off += el.bytes;
        ++count;
    }
    return std::vector<std::byte>(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(off));
}

/** @brief The name node @p i uses for its downstream link toward node @p i+1. */
[[nodiscard]] std::string down_name(std::size_t i) {
    return "net/ws-client/n" + std::to_string(i + 1);
}
/** @brief The name node @p i uses for its upstream link back toward node @p i-1. */
[[nodiscard]] std::string up_name(std::size_t i) {
    return "net/ws-server/n" + std::to_string(i - 1);
}

/**
 * @brief `hops + 1` graphs wired into a line, every router carrying a mint table.
 *
 * Both arms run over THIS topology, so the mint tables are present in both: the arms differ in
 * the `dst` spelling and in nothing else, which is the condition `bench_hop_chain`'s §guard
 * section says an A/B on this surface has to meet.
 */
struct chain_t {
    std::size_t hops;
    std::vector<std::unique_ptr<graph_t>> graphs;
    std::vector<std::unique_ptr<fwd_router_t>> routers;
    std::vector<std::unique_ptr<path_label_table_t>> tables;
    std::vector<wire_link_t> down; /**< @brief `down[i]` is node i's end toward i+1. */
    std::vector<wire_link_t> up;   /**< @brief `up[i]` is node i's end toward i-1. */
    wire_link_t origin;            /**< @brief The link node 0's application injects through. */
    std::vector<std::string> leaf; /**< @brief The terminus residual segments, owned. */
    /** @brief Decoy children widening every hop's registry — never on the route. */
    std::vector<std::unique_ptr<wire_link_t>> decoys;

    /**
     * @brief Build a chain of @p h hops whose terminus vertex sits @p depth segments deep, each
     *        hop carrying @p width children (the route's own plus `width - 1` decoys).
     */
    chain_t(std::size_t h, std::size_t depth, std::size_t width) : hops(h), down(h + 1), up(h + 1) {
        const std::size_t nodes = hops + 1;
        for (std::size_t i = 0; i < nodes; ++i) graphs.push_back(std::make_unique<graph_t>());
        for (std::size_t i = 0; i < nodes; ++i) {
            routers.push_back(std::make_unique<fwd_router_t>(*graphs[i]));
            tables.push_back(std::make_unique<path_label_table_t>(&tr::mem::heap_source(), 64, 16));
            routers[i]->configure_path_labels(tables[i].get());
        }
        // The terminus residual, `depth` segments deep, registered ancestor-first.
        std::string acc;
        for (std::size_t d = 0; d < depth; ++d) {
            leaf.push_back("s" + std::to_string(d));
            acc += "/" + leaf.back();
            (void)graphs[nodes - 1]->register_vertex(*tr::graph::path_t::parse(acc),
                                                     tr::graph::role_t::STORED_VALUE);
        }
        // The addressed vertex holds a value, and that is load-bearing rather than tidy: §8.1
        // mints only on SUCCESS, so a READ of an empty vertex takes `apply_op`'s error arm, the
        // terminus mints nothing, and the reply comes home one label short of point 4. A bench
        // that seeded no value would measure a chain whose last part is permanently a string and
        // report it as the label spelling's cost.
        {
            static const std::array<std::byte, 4> kSeed{std::byte{0xD2}, std::byte{0x04},
                                                        std::byte{0x00}, std::byte{0x00}};
            std::vector<std::byte> value;
            tr::wire::emit_tlv(value, type_t::VALUE, opt_t{}, std::span<const std::byte>(kSeed));
            tr::view::segment_ptr_t seg = tr::view::heap_alloc(value.size());
            std::memcpy(seg->bytes.data(), value.data(), value.size());
            (void)graphs[nodes - 1]->write(*tr::graph::path_t::parse(acc),
                                           tr::view::view_t::over(std::move(seg)));
        }
        // The connection vertices the children's mount keys join on — what a label
        // dereferences. Registered BEFORE `add_child`, because the join is made at
        // registration time and a child added first acquires no `conn_slot` to mint against.
        for (std::size_t i = 0; i + 1 < nodes; ++i) {
            (void)graphs[i]->register_vertex(*tr::graph::path_t::parse("/net"),
                                             tr::graph::role_t::STORED_VALUE);
            (void)graphs[i]->register_vertex(*tr::graph::path_t::parse("/net/ws-client"),
                                             tr::graph::role_t::STORED_VALUE);
            (void)graphs[i]->register_vertex(*tr::graph::path_t::parse("/" + down_name(i)),
                                             tr::graph::role_t::STORED_VALUE);
        }
        for (std::size_t i = 0; i + 1 < nodes; ++i) {
            down[i].peer = &up[i + 1];
            up[i + 1].peer = &down[i];
            (void)routers[i]->add_child(down_name(i), down[i]);
            (void)routers[i + 1]->add_child(up_name(i + 1), up[i + 1]);
        }
        // The decoys, added LAST so the route's own child is never the newest entry — a
        // registry that answers fastest for whatever was registered most recently would make
        // the width axis measure insertion order instead of width.
        for (std::size_t i = 0; i < nodes; ++i) {
            for (std::size_t d = 0; d + 1 < width; ++d) {
                const std::string name =
                    "net/ws-client/d" + std::to_string(i) + "_" + std::to_string(d);
                (void)graphs[i]->register_vertex(*tr::graph::path_t::parse("/" + name),
                                                 tr::graph::role_t::STORED_VALUE);
                decoys.push_back(std::make_unique<wire_link_t>());
                (void)routers[i]->add_child(name, *decoys.back());
            }
        }
        (void)routers[0]->add_child("net/ws-server/app", origin);
    }

    /** @brief The full canonical address of the terminus vertex, seen from node 0. */
    [[nodiscard]] std::vector<std::string_view> full_route() const {
        static std::vector<std::string> owned;
        owned.clear();
        for (std::size_t i = 0; i < hops; ++i) owned.push_back(down_name(i));
        std::vector<std::string_view> segs;
        for (const std::string& n : owned) {
            std::size_t a = 0;
            while (a <= n.size()) {
                const std::size_t e = n.find('/', a);
                const std::size_t end = e == std::string::npos ? n.size() : e;
                segs.emplace_back(n.data() + a, end - a);
                if (e == std::string::npos) break;
                a = e + 1;
            }
        }
        for (const std::string& s : leaf) segs.emplace_back(s);
        return segs;
    }

    /** @brief Just the terminus residual — what the label arm still spells as strings. */
    [[nodiscard]] std::vector<std::string_view> residual() const {
        std::vector<std::string_view> segs;
        for (const std::string& s : leaf) segs.emplace_back(s);
        return segs;
    }

    [[nodiscard]] std::size_t forward_frames() const {
        std::size_t n = 0;
        for (const wire_link_t& l : down) n += l.frames;
        return n;
    }
    [[nodiscard]] std::size_t reverse_frames() const {
        std::size_t n = origin.frames;
        for (const wire_link_t& l : up) n += l.frames;
        return n;
    }
    [[nodiscard]] std::size_t wire_bytes() const {
        std::size_t n = 0;
        for (const wire_link_t& l : down) n += l.bytes;
        return n;
    }
    void reset_counters() {
        for (wire_link_t& l : down) {
            l.bytes = 0;
            l.frames = 0;
        }
        for (wire_link_t& l : up) {
            l.bytes = 0;
            l.frames = 0;
        }
        origin.bytes = 0;
        origin.frames = 0;
    }
    /** @brief Every hop's counted labelled resolution, summed — the label arm's liveness proof. */
    [[nodiscard]] std::size_t label_resolves() const {
        std::size_t n = 0;
        for (const auto& r : routers) n += r->label_resolves();
        return n;
    }
    /** @brief Every hop's counted §7.2 refusal, summed — must stay zero on a warm route. */
    [[nodiscard]] std::size_t label_not_found() const {
        std::size_t n = 0;
        for (const auto& r : routers) n += r->label_not_found();
        return n;
    }
};

/** @brief One measured arm. */
struct arm_t {
    bench::Latency::Summary lat{};
    std::size_t fwd_frames = 0;
    std::size_t rev_frames = 0;
    std::size_t wire_bytes = 0;
    std::size_t first_frame = 0;
    bool ok = false;
};

/** @brief Time @p op over a calibrated batch for the point budget, and summarize. */
template <typename Op>
[[nodiscard]] bench::Latency::Summary time_op(Op&& op) {
    for (int i = 0; i < 64; ++i) op();  // warm; the first execution is never measured
    const std::size_t batch = calibrate_batch(op);
    bench::Latency lat;
    const auto deadline = static_cast<std::uint64_t>(budget_seconds() * 1e9);
    const std::uint64_t t0 = bench::now_ns();
    std::uint64_t total = 0;
    while (total < deadline) {
        const std::uint64_t a = bench::now_ns();
        for (std::size_t i = 0; i < batch; ++i) op();
        lat.add((bench::now_ns() - a) / batch);
        total = bench::now_ns() - t0;
    }
    return lat.summarize();
}

/**
 * @brief Run both spellings over one chain shape and emit two RESULT rows.
 *
 * @param hops   Hops in the chain — the slope axis of §12.4 clause 1.
 * @param depth  Terminus residual depth — the axis of §12.4 clause 2, held at 1 for the slope.
 * @param width  Children per hop — §15 clause 2's "realistic registry widths".
 * @param tag_s  Mode string for the string arm; @p tag_l for the label arm.
 * @return false when the arms did not emit the same traffic, or the label arm never resolved a
 *         label — either voids the point, and the caller must fail the run rather than print it.
 */
[[nodiscard]] bool run_point(std::size_t hops, std::size_t depth, std::size_t width,
                             const char* tag_s, const char* tag_l) {
    chain_t c(hops, depth, width);
    const std::vector<std::byte> dst_s = path_tlv(c.full_route());
    const std::vector<std::byte> src = path_tlv({"reply"});
    std::uint32_t v = 0;

    // Warm the STRING arm once: §6.2's trigger fires, every hop mints, and the reply carries the
    // minted prefix home. This is where the label arm's address comes from — nothing is spelled
    // by hand, so the arm measures the implementation's own output.
    const auto op_s = [&] { c.origin.inject(build_fwd(0x00, dst_s, src, ++v)); };
    op_s();
    std::size_t minted = 0;
    std::vector<std::byte> prefix;
    if (const std::optional<std::vector<std::byte>> body = src_body_of(c.origin.last))
        prefix = leading_labels(*body, minted);
    // `hops + 1`, not `hops`: each forwarding hop contributes its own local part (§6.1 point 2)
    // and the TERMINUS contributes the residual it resolved (point 3). Point 4's "fully-minted
    // src" is exactly this count, and asserting it here is what makes the label arm's address
    // the implementation's own output rather than a hand-spelling of what it ought to be.
    if (minted != hops + 1) {
        std::printf(
            "VOID hops=%zu depth=%zu — the reply carried %zu minted labels, not %zu"
            " (origin frames=%zu last=%zuB src_body=%zuB)\n",
            hops, depth, minted, hops + 1, c.origin.frames, c.origin.last.size(),
            src_body_of(c.origin.last) ? src_body_of(c.origin.last)->size() : 0);
        return false;
    }
    // NOTHING is appended. The terminus's label REPLACED the residual's string bytes (§6.1), so
    // the whole labelled address is the run the reply came home with — at depth 12 exactly as at
    // depth 1, which is the shape clause 2 exists to price.
    const std::vector<std::byte> dst_l = path_tlv_prefixed(prefix, {});
    const auto op_l = [&] { c.origin.inject(build_fwd(0x00, dst_l, src, ++v)); };

    // Traffic census, per direction, per arm — `bench_hop_chain`'s §guard rule.
    arm_t s;
    arm_t l;
    s.first_frame = build_fwd(0x00, dst_s, src, 1).size();
    l.first_frame = build_fwd(0x00, dst_l, src, 1).size();
    c.reset_counters();
    op_s();
    s.fwd_frames = c.forward_frames();
    s.rev_frames = c.reverse_frames();
    s.wire_bytes = c.wire_bytes();
    const std::size_t resolves_before = c.label_resolves();
    c.reset_counters();
    op_l();
    l.fwd_frames = c.forward_frames();
    l.rev_frames = c.reverse_frames();
    l.wire_bytes = c.wire_bytes();
    const std::size_t resolved = c.label_resolves() - resolves_before;

    // `hops + 1` again, and for the matching reason: `label_resolves_` counts a hop's deref to a
    // LINK and a terminus's deref to a local VERTEX with one counter, because both are "a label
    // stood for a resolution and the resolution was taken".
    if (resolved != hops + 1 || c.label_not_found() != 0) {
        std::printf(
            "VOID hops=%zu depth=%zu — the label arm resolved %zu labels (want %zu),"
            " refusals=%zu. It is measuring a DROP, not a delivery.\n",
            hops, depth, resolved, hops + 1, c.label_not_found());
        return false;
    }
    if (s.fwd_frames != l.fwd_frames || s.rev_frames != l.rev_frames) {
        std::printf(
            "VOID hops=%zu depth=%zu — NOT LIKE-FOR-LIKE: string %zu fwd / %zu rev,"
            " label %zu fwd / %zu rev\n",
            hops, depth, s.fwd_frames, s.rev_frames, l.fwd_frames, l.rev_frames);
        return false;
    }

    // Interleaved within the point, so a slow window in the machine is shared by both arms.
    s.lat = time_op(op_s);
    l.lat = time_op(op_l);
    const bench::Latency::Summary s2 = time_op(op_s);
    const bench::Latency::Summary l2 = time_op(op_l);
    if (s2.p50 < s.lat.p50) s.lat = s2;
    if (l2.p50 < l.lat.p50) l.lat = l2;

    // `fanout` carries the hop count and `endpoints` the registry width — the two axes a slope
    // is read over — so `perf_gate.py`'s (mode,size,fan,ep) tuple stays unambiguous. The
    // residual depth rides the mode string, because a point is keyed on four fields and there
    // is no fifth. `size_bytes` is the frame the ORIGINATOR emits, which differs between the
    // arms by construction and is the byte column §3.3 prices.
    bench::emit("libtracer", tag_s, s.first_frame, hops, width, 0.0, 0.0, 0.0, s.lat);
    bench::emit_tail("libtracer", tag_s, s.first_frame, hops, width, s.lat);
    bench::emit("libtracer", tag_l, l.first_frame, hops, width, 0.0, 0.0, 0.0, l.lat);
    bench::emit_tail("libtracer", tag_l, l.first_frame, hops, width, l.lat);
    std::printf(
        "NOTE hops=%zu depth=%zu width=%zu string_p50=%llu label_p50=%llu delta=%lld"
        " frame_B string=%zu label=%zu wire_B string=%zu label=%zu resolves=%zu\n",
        hops, depth, width, static_cast<unsigned long long>(s.lat.p50),
        static_cast<unsigned long long>(l.lat.p50),
        static_cast<long long>(s.lat.p50) - static_cast<long long>(l.lat.p50), s.first_frame,
        l.first_frame, s.wire_bytes, l.wire_bytes, resolved);
    std::fflush(stdout);
    return true;
}

}  // namespace

int main() {
    std::printf("# bench_path_label — RFC-0027 §12.4 clauses 1 and 2\n");
    std::printf(
        "# clause 1: per-hop saving as a SLOPE over hop count, at three registry widths\n"
        "# (depth held at 1). `endpoints` carries the width; `fanout` carries the hops.\n");
    bool ok = true;
    for (const std::size_t w : kRegistryWidths)
        for (const std::size_t h : kHopCounts)
            ok = run_point(h, 1, w, "plabel-string", "plabel-label") && ok;

    std::printf(
        "# clause 2: the TERMINUS RESIDUAL against depth, one hop — THE AXIS THAT DECIDES.\n"
        "# The string arm spells the residual as `depth` packed segments; the label arm\n"
        "# spells it as ONE 7-byte element at every depth, because the terminus minted for\n"
        "# the residual it resolved (§6.1 point 3) and dereferences that label on the way\n"
        "# back in (§7.2). Read the label arm's slope against depth: flat is the claim.\n");
    // The depth rides the MODE STRING here, and it has to since the label spelling landed: a
    // point is keyed on (mode, size, fan, ep), the depth used to ride `size_bytes` because the
    // residual's string bytes were in the frame — and the label arm's frame is now 45 B at every
    // depth, so all five points would collapse onto one key and be pooled as repeats of each
    // other. A slope read off pooled points is not a slope.
    for (const std::size_t d : kResidualDepths) {
        const std::string tag_s = "presid-string-d" + std::to_string(d);
        const std::string tag_l = "presid-label-d" + std::to_string(d);
        ok = run_point(1, d, 8, tag_s.c_str(), tag_l.c_str()) && ok;
    }

    if (!ok) {
        std::printf(
            "\nFAILED: at least one point was voided above. Its numbers are not results.\n");
        return 1;
    }
    return 0;
}
