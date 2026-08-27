/**
 * @file
 * @brief ADR-0060 — the write-path value byte-buffer seam on `graph_t`: the copy-store
 *        flatten of a branch/field write draws its owned `segment` from the injected
 *        `value_backend_` (a `mem_backend_t`), not the default heap.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Probes the three guarantees the ADR commits to:
 *   - ROUTING: a multi-link field write through a pool-backed graph flattens into
 *     the injected pool (proven by contrast — a pool too small to hold the value
 *     BACKPRESSUREs, where the default heap accepts the identical write);
 *   - BEHAVIOUR: a pool-backed graph reads back byte-exact and its ordinary
 *     single-link value writes (which never materialize) are unaffected;
 *   - BACKPRESSURE (§3): pool exhaustion / oversize surfaces as `BACKPRESSURE`, not
 *     a silent heap fallback and not a spurious `TYPE_MISMATCH`.
 */

#include <array>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/mem_source.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::view::rope_t;
using tr::view::view_t;

using tr::testing::check;

/** @brief The raw bytes of a VALUE TLV carrying @p payload (the field-write shape). */
std::vector<std::byte> value_tlv_bytes(std::span<const std::byte> payload) {
    const tr::wire::tlv_t t{.type = tr::wire::type_t::VALUE, .payload = payload};
    return tr::wire::encode(t);
}

/**
 * @brief A MULTI-link rope over @p bytes, split across two borrowed segments.
 *
 * A single-link rope would `materialize()` zero-copy and never touch `value_backend_`;
 * two links force the flatten this seam routes. @p bytes must outlive the write (the
 * links are borrowed — the write copies before returning).
 */
rope_t multilink(std::span<const std::byte> bytes) {
    const std::size_t mid = bytes.size() / 2;
    rope_t r{view_t::over(tr::view::borrow_const(bytes.first(mid)))};
    r.append(view_t::over(tr::view::borrow_const(bytes.subspan(mid))));
    return r;
}

/**
 * @brief A pass-through source that refuses a draw of EXACTLY @p refuse_bytes and counts what
 *        it served — the ADR-0060 seam instrument, re-aimed at the one injected source.
 *
 * @par Why a size predicate and not a budget (#873 phase 1)
 * The old instrument was a `tr::mem::pool_t` injected as `value_backend`, so "a slot too small
 * to hold the value" isolated the flatten by construction. `graph_t` now takes ONE source and
 * builds the backend over it, so a source that simply refuses past a budget would starve the
 * vertex registration and the per-write LKV control block long before the flatten — proving
 * nothing about the seam under test.
 *
 * Refusing exactly the flattened TLV's byte count isolates the same allocation the undersized
 * pool used to: it is the ONE draw `value.materialize(*value_backend_)` makes, and no other
 * channel in this fixture asks for that size. `served_exactly()` is the positive half — the
 * routing instrument that reddens if the flatten stops using the seam at all.
 */
class value_probe_source_t final : public tr::mem::block_source_t {
   public:
    /** @brief Refuse draws of exactly @p refuse_bytes; 0 refuses nothing. */
    explicit value_probe_source_t(std::size_t refuse_bytes = 0) noexcept
        : block_source_t("value_probe"), refuse_(refuse_bytes) {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (refuse_ != 0 && bytes == refuse_) {
            ++refused_;
            return nullptr;
        }
        void* const p = tr::mem::heap_source().try_alloc(bytes, align);
        if (p != nullptr && bytes == watch_) ++served_watched_;
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::heap_source().release(p, bytes, align);
    }

    /** @brief Count draws of exactly @p n bytes from now on (the routing instrument). */
    void watch(std::size_t n) noexcept { watch_ = n; }
    /** @brief How many watched-size draws were served. */
    [[nodiscard]] std::size_t served_watched() const noexcept { return served_watched_; }
    /** @brief How many draws were refused. */
    [[nodiscard]] std::size_t refused() const noexcept { return refused_; }

   private:
    std::size_t refuse_;
    std::size_t watch_ = 0;
    std::size_t served_watched_ = 0;
    std::size_t refused_ = 0;
};

}  // namespace

/** @brief Run the ADR-0060 value-backend seam probes. */
int main() {
    std::printf("graph_t value seam over the injected source (ADR-0060, #873 phase 1):\n");

    // The field-write value: a 4-byte VALUE (5000 LE) written to an owner-declared app
    // field. Any field write flattens a multi-link value at the ONE ADR-0060 seam site
    // (`graph.cpp`'s `value.materialize(*value_backend_)`), so `settings.app.kp` exercises
    // exactly what `settings.history_keep_last` used to, before RFC-0022 deleted the flat
    // knob namespace.
    const std::array<std::byte, 4> le{std::byte{0x88}, std::byte{0x13}};  // 5000
    const std::vector<std::byte> tlv = value_tlv_bytes(le);
    const auto fp = path_t::parse("/s/temp:settings.app.kp");
    check(fp.has_value() && !fp->field().steps.empty(), "field path parses");

    /** @brief Register `/s/temp` with one declared `rw` app field `kp` — the write target. */
    const auto with_field = [&](graph_t& g) {
        const auto v = g.register_vertex(path_t("/s/temp"), role_t::STORED_VALUE);
        std::vector<tr::graph::app_field_t> table;
        table.push_back(
            tr::graph::app_field_t{.name = "kp", .access = tr::graph::app_access_t::RW});
        g.set_app_fields(v, std::move(table));
        return v;
    };

    /** @brief The bytes `:settings.app.kp` serves back, or empty when it holds none. */
    const auto stored_bytes = [&](graph_t& g, tr::graph::vertex_handle_t v) {
        const auto r = g.read(v, fp->field());
        if (!r) return std::vector<std::byte>{};
        const tr::view::view_t flat = r->flatten();
        const std::span<const std::byte> b = flat.bytes();
        return std::vector<std::byte>(b.begin(), b.end());
    };

    // ROUTING + BEHAVIOUR: a pool with room accepts the multi-link field write and
    // reads it back exactly — the flatten drew from the injected pool.
    {
        value_probe_source_t src;
        src.watch(tlv.size());
        graph_t g(&src);
        const auto v = with_field(g);
        const auto w = g.write(v, fp->field(), multilink(tlv));
        check(w.has_value(), "multi-link field write through a source-backed graph succeeds");
        check(src.served_watched() == 1,
              "the flatten drew EXACTLY the TLV's bytes from the injected source (routing)");
        check(stored_bytes(g, v) == tlv, "value read back byte-exact (the flattened VALUE TLV)");
        // A plain single-link value write never materializes — the seam is untouched
        // and the ordinary store path is unaffected.
        const std::array<std::byte, 3> pv_bytes{std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};
        const auto pv = g.write(v, rope_t{view_t::over(tr::view::borrow_const(pv_bytes))});
        check(pv.has_value(), "ordinary single-link value write is unaffected by the seam");
        check(src.served_watched() == 1, "...and never reaches the flatten (no second draw)");
    }

    // BACKPRESSURE (§3): a pool whose slot cannot hold the value makes the flatten
    // return nullptr → the write rejects with BACKPRESSURE (not TYPE_MISMATCH, not a
    // heap fallback). The SAME value on the default heap accepts — proving the seam is
    // actually consulted, not ignored.
    {
        value_probe_source_t refusing(tlv.size());  // refuses the flatten, serves all else
        graph_t g(&refusing);
        const auto v = with_field(g);
        const auto w = g.write(v, fp->field(), multilink(tlv));
        check(refusing.refused() > 0, "the refusing source WAS consulted by the flatten");
        check(!w.has_value() && w.error() == status_t::BACKPRESSURE,
              "a refused flatten BACKPRESSUREs the write (no heap fallback, no TYPE_MISMATCH)");
        check(stored_bytes(g, v).empty(), "the rejected write landed nothing");
    }
    {
        graph_t heap;  // the process-default source
        const auto v = with_field(heap);
        const auto w = heap.write(v, fp->field(), multilink(tlv));
        check(w.has_value() && stored_bytes(heap, v) == tlv,
              "the identical write on the default-source graph accepts (contrast: seam is live)");
    }

    return tr::testing::summary("graph_value_backend");
}
