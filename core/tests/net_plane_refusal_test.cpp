/**
 * @file
 * @brief The net plane's own state on the seam (#1779): every growth site refuses by value,
 *        registers nothing, and gives every block back.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Since #1779 the router's registration state — the registry slot's text block, the receiver
 * ctx, a bus mount's token cache, an interned link kind — and the tx-handoff ring draw from an
 * injected `block_source_t` instead of `std::string` / `std::vector` / `std::deque` /
 * `std::make_unique` on the global heap. This drives each of them to a refusal with a source
 * that refuses its Nth request, sweeping N until the call succeeds, and pins three things:
 *
 * 1. a refused `add_child` returns false and leaves NOTHING behind — no live registry entry,
 *    no receiver installed on the link;
 * 2. every refusal point is reached (the sweep refuses at least once per allocation the
 *    successful call makes), so no growth site escapes the seam;
 * 3. the source balances to zero once the router is gone — what a refusal left half-built is
 *    returned, not leaked.
 *
 * @section ablation What goes RED if a guard is ablated
 *
 * Drop the `ctx_p == nullptr` arm in `add_child` and the sweep dereferences a null ctx. Drop
 * `drop_tokens()` on the refused-registry arm and (3) goes red on the bus sweep. Return the
 * old `std::make_unique` cache and the bus sweep's refusal count falls short of its
 * allocation count, failing (2).
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/tracer.hpp"
#include "libtracer/tx_handoff.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;

/** @brief A heap-backed source that refuses request number @p nth (0: never), and counts. */
class refuse_nth_source_t final : public tr::mem::block_source_t {
   public:
    /** @brief Refuse request number @p nth; 0 never refuses. */
    explicit refuse_nth_source_t(int nth) noexcept
        : tr::mem::block_source_t("refuse-nth"), nth_(nth) {}
    /** @brief Serve from the heap, except the Nth request. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (++calls_ == nth_) {
            ++refused_;
            return nullptr;
        }
        void* const p = tr::mem::heap_source().try_alloc(bytes, align);
        if (p != nullptr) ++blocks_out_;
        return p;
    }
    /** @brief Return to the heap and settle the account. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        --blocks_out_;
        tr::mem::heap_source().release(p, bytes, align);
    }
    int calls_ = 0;       /**< @brief Requests seen. */
    int refused_ = 0;     /**< @brief Requests refused. */
    long blocks_out_ = 0; /**< @brief Blocks handed out and not returned. */

   private:
    int nth_; /**< @brief The request to refuse. */
};

/** @brief A link that says whether a receiver was wired onto it. */
struct wired_link_t : tr::net::transport_t {
    void send(std::span<const std::byte>) override {}
    /** @brief True ⇔ some inbound sink has been installed on this link. */
    [[nodiscard]] bool wired() const noexcept { return rx_.has_any(); }
};

/** @brief The smallest BUS link: a transport that also answers `bus()`, and says whether its
 *         peer-named receiver was wired. */
struct fake_bus_t final : tr::net::transport_t, tr::net::bus_link_t {
    void send(std::span<const std::byte>) override {}
    tr::net::bus_link_t* bus() override { return this; }
    tr::net::transport_t* peer_link(std::string_view) override { return nullptr; }
    void enumerate_peers(const tr::net::bus_link_t::peer_visitor_t&) const override {}
    [[nodiscard]] std::string_view peer_name(tr::net::peer_handle_t,
                                             std::span<char>) const override {
        return {};
    }
    /** @brief True ⇔ the peer-named inbound sink has been installed on this link. */
    [[nodiscard]] bool wired() const noexcept { return peer_rx_.has_any(); }
};

/**
 * @brief Sweep the label source's refusal over one `add_child` of @p link until it succeeds.
 *
 * @return How many refusals the sweep met before the first success — the number of
 *         allocations a fresh registration makes on the label plane.
 */
template <class Link>
int sweep_add_child(const char* what, tr::net::link_kind_t kind) {
    std::printf("%s\n", what);
    int refusals = 0;
    for (int nth = 1; nth < 32; ++nth) {
        refuse_nth_source_t src(nth);
        bool added = false;
        {
            tr::graph::graph_t graph{tr::mem::heap_source()};
            tr::net::fwd_router_t router{graph, {.label_src = &src}};
            Link link;
            added = router.add_child("net/ws/a", link, nullptr, kind);
            if (src.refused_ == 0) {
                check(added, "with nothing refused, the registration succeeds");
                check(link.wired() && router.registry().live_size() == 1,
                      "…and is wired and registered — so the refusals above are not vacuous");
            } else {
                ++refusals;
                check(!added, "a refused allocation is add_child's false");
                check(!link.wired(), "…with NO receiver installed on the link");
                check(router.registry().live_size() == 0, "…and nothing registered");
                // The refusal is not sticky: the next attempt on a healthy source succeeds and
                // reuses whatever the refused one kept (a slot, a ctx, a kind record).
                check(router.add_child("net/ws/a", link, nullptr, kind),
                      "…and the same registration succeeds once the source serves again");
            }
        }
        check(src.blocks_out_ == 0, "every block goes back once the router is gone");
        if (src.refused_ == 0) break;
    }
    std::printf("  %d allocation(s) on the label plane, each refused once\n", refusals);
    return refusals;
}

/** @brief A tx-handoff ring the source refuses is a ring of NO slots — drop-and-count. */
void test_tx_handoff_refused_ring() {
    std::printf("tx_handoff_t over a refusing source\n");
    refuse_nth_source_t src(1);
    {
        tr::net::tx_handoff_t q(4, src, 64);
        check(q.capacity() == 0, "the refused ring has no slots");
        const auto fill = [](tr::mem::block_array_t<std::byte>& buf) -> std::size_t {
            const std::byte b{1};
            return buf.append(&b, 1) ? 1 : 0;
        };
        check(q.admit(fill) == tr::net::tx_handoff_t::admit_t::WRITE,
              "the first sender still becomes the writer");
        check(q.admit(fill) == tr::net::tx_handoff_t::admit_t::REFUSED,
              "a record that meets the write in flight is refused, not queued");
        check(q.refused() == 1, "…and counted");
        check(!q.next(), "the writer finds nothing queued");
    }
    check(src.blocks_out_ == 0, "nothing was left out");

    refuse_nth_source_t open(0);
    {
        tr::net::tx_handoff_t q(4, open, 64);
        check(q.capacity() == 4, "an open source gives the ring all its slots");
    }
    check(open.blocks_out_ == 0, "the ring and its slots go back on destruction");
}

/** @brief An anchor id that cannot be one key record comes back EMPTY, never truncated. */
void test_session_anchor_id_bound() {
    std::printf("session_anchor_id is bounded by one key record\n");
    const auto id = tr::net::fwd_router_t::session_anchor_id("net/ws", "p0");
    check(id.view() == ":net/ws/p0", "an ordinary id spells `:<mount>/<peer>`");
    const std::string_view mount(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    check(tr::net::fwd_router_t::session_anchor_id(mount, "p0").view().empty(),
          "an id past one segment record is empty — the router anchors nothing for it");
}

/** @brief The fixed-buffer label record is the bytes the growing-buffer form emits. */
void test_path_label_record_matches_emit() {
    std::printf("path_label_record == emit_path_label\n");
    const tr::wire::path_label_t label{.index = 0x0102u, .generation = 7};
    std::vector<std::byte> grown;
    const bool emitted = tr::wire::emit_path_label(grown, label);
    const auto rec = tr::wire::path_label_record(label);
    check(emitted && rec.has_value() && grown.size() == rec->size() &&
              std::equal(grown.begin(), grown.end(), rec->begin()),
          "the two spellings of a label element are byte-identical");
    check(!tr::wire::path_label_record(tr::wire::path_label_t{}).has_value(),
          "the reserved zero label has no record");
}

/** @brief The span form of a segment record is the bytes the growing-buffer form emits. */
void test_store_path_segment_matches_emit() {
    std::printf("store_path_segment == emit_path_segment\n");
    std::vector<std::byte> grown;
    const bool emitted = tr::wire::emit_path_segment(grown, std::string_view("uplink"));
    std::array<std::byte, 16> buf{};
    const std::size_t measured = tr::wire::store_path_segment({}, "uplink");
    const std::size_t stored = tr::wire::store_path_segment(buf, "uplink");
    check(emitted && measured == grown.size() && stored == grown.size() &&
              std::equal(grown.begin(), grown.end(), buf.begin()),
          "measuring, storing and emitting a segment record agree byte for byte");
    check(tr::wire::store_path_segment(buf, "") == 0, "an empty segment has no record");
    check(tr::wire::store_path_segment(std::span<std::byte>(buf).first(3), "uplink") == 0,
          "a buffer too small stores nothing");
}

}  // namespace

int main() {
    check(sweep_add_child<wired_link_t>("add_child (point-to-point) under a refusing label "
                                        "source",
                                        {}) >= 3,
          "the registry chunk, the slot text and the receiver ctx are all on the seam");
    check(sweep_add_child<wired_link_t>("add_child with a link kind", {.kind = "ws", .role = {}}) >=
              4,
          "…and so is the interned link-kind record");
    if constexpr (tr::net::kBusLinks) {
        check(sweep_add_child<fake_bus_t>("add_child (bus mount) under a refusing label source",
                                          {}) >= 4,
              "…and so is a bus mount's per-peer token cache");
    }
    test_tx_handoff_refused_ring();
    test_session_anchor_id_bound();
    test_path_label_record_matches_emit();
    test_store_path_segment_matches_emit();
    return tr::testing::summary("net_plane_refusal");
}
