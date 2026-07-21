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

#include "libtracer/mem_pool.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::view::rope_t;
using tr::view::view_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

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

/** @brief Carve @p slab (caller-owned) into a pool of @p slot-byte slots. */
struct scratch_pool_t {
    std::vector<std::byte> slab;
    tr::mem::pool_t pool;
    explicit scratch_pool_t(std::size_t slot, std::size_t slots = 16)
        : slab(slots * (sizeof(tr::view::segment_t) + slot + 64)), pool(slab, slot) {}
};

}  // namespace

/** @brief Run the ADR-0060 value-backend seam probes. */
int main() {
    std::printf("graph_t value_backend_ seam (ADR-0060):\n");

    // The field-write value: an 8-byte deadline_ns knob (5000 LE), as graph_test.
    const std::array<std::byte, 8> le{std::byte{0x88}, std::byte{0x13}};  // 5000
    const std::vector<std::byte> tlv = value_tlv_bytes(le);
    const auto fp = path_t::parse("/s/temp:settings.deadline_ns");
    check(fp.has_value() && !fp->field().steps.empty(), "field path parses");

    // ROUTING + BEHAVIOUR: a pool with room accepts the multi-link field write and
    // reads it back exactly — the flatten drew from the injected pool.
    {
        scratch_pool_t sp(/*slot=*/64);
        graph_t g(std::pmr::get_default_resource(), &sp.pool);
        auto v = g.register_vertex(path_t("/s/temp"), role_t::STORED_VALUE);
        const auto w = g.write(v, fp->field(), multilink(tlv));
        check(w.has_value(), "multi-link field write through a pool-backed graph succeeds");
        check(g.settings(v).deadline_ns == 5000, "value read back byte-exact (deadline_ns=5000)");
        // A plain single-link value write never materializes — the seam is untouched
        // and the ordinary store path is unaffected.
        const std::array<std::byte, 3> pv_bytes{std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};
        const auto pv = g.write(v, rope_t{view_t::over(tr::view::borrow_const(pv_bytes))});
        check(pv.has_value(), "ordinary single-link value write is unaffected by the pool seam");
    }

    // BACKPRESSURE (§3): a pool whose slot cannot hold the value makes the flatten
    // return nullptr → the write rejects with BACKPRESSURE (not TYPE_MISMATCH, not a
    // heap fallback). The SAME value on the default heap accepts — proving the seam is
    // actually consulted, not ignored.
    {
        scratch_pool_t tiny(/*slot=*/4);  // < the ~10-byte flattened TLV
        graph_t g(std::pmr::get_default_resource(), &tiny.pool);
        auto v = g.register_vertex(path_t("/s/temp"), role_t::STORED_VALUE);
        const auto w = g.write(v, fp->field(), multilink(tlv));
        check(!w.has_value() && w.error() == status_t::BACKPRESSURE,
              "an undersized pool BACKPRESSUREs the write (no heap fallback, no TYPE_MISMATCH)");
        check(g.settings(v).deadline_ns != 5000, "the rejected write landed nothing");
    }
    {
        graph_t heap;  // default heap backend
        auto v = heap.register_vertex(path_t("/s/temp"), role_t::STORED_VALUE);
        const auto w = heap.write(v, fp->field(), multilink(tlv));
        check(w.has_value() && heap.settings(v).deadline_ns == 5000,
              "the identical write on the default-heap graph accepts (contrast: seam is live)");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
