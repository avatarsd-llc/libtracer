/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Per-endpoint :acl STRUCTURAL STORAGE (#81-A, ADR-0018/0020). The graph stores
 * the raw ACL TLV bytes opaquely and serves them back byte-for-byte; it does NOT
 * parse the NFSv4 ACE children and does NOT enforce them (enforcement is the
 * deferred security_acl module). These checks pin storage + round-trip + the
 * type-mismatch rejection — nothing about gating.
 */

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
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

tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

// A minimal structured ACL TLV (opt.PL=1) carrying one child ACE. We do not assert
// anything about the child's semantics — storage is opaque.
std::vector<std::byte> make_acl_bytes() {
    const std::vector<std::byte> subject = {std::byte{'O'}, std::byte{'W'}, std::byte{'N'},
                                            std::byte{'E'}, std::byte{'R'}, std::byte{'@'}};
    tr::wire::tlv_t name{.type = type_t::NAME, .payload = subject};
    tr::wire::tlv_t ace{.type = type_t::ACL, .opt = opt_t{.pl = true}};
    ace.children.push_back(name);
    tr::wire::tlv_t acl{.type = type_t::ACL, .opt = opt_t{.pl = true}};
    acl.children.push_back(ace);
    return tr::wire::encode(acl);
}

}  // namespace

int main() {
    graph_t g;
    const auto path = path_t::parse("/x");
    (void)g.register_vertex(*path, role_t::STORED_VALUE);

    std::printf(":acl storage + round-trip:\n");

    // No ACL set yet => read returns NOT_FOUND.
    {
        const auto r = g.read(*path_t::parse("/x:acl"));
        check(!r.has_value() && r.error() == status_t::NOT_FOUND, "unset :acl reads NOT_FOUND");
    }

    // Write a valid ACL TLV, read it back, assert byte-equality.
    const std::vector<std::byte> acl = make_acl_bytes();
    {
        const auto w = g.write(*path_t::parse("/x:acl"), make_value(acl));
        check(w.has_value(), "writing a valid ACL TLV to :acl succeeds");

        const auto r = g.read(*path_t::parse("/x:acl"));
        const bool eq = r.has_value() && r->bytes().size() == acl.size() &&
                        std::equal(acl.begin(), acl.end(), r->bytes().begin());
        check(eq, "read :acl returns the stored bytes verbatim");
    }

    // A non-ACL TLV (e.g. a VALUE) is rejected with TYPE_MISMATCH; storage is unchanged.
    {
        tr::wire::tlv_t value{.type = type_t::VALUE,
                              .payload = std::span<const std::byte>(acl).first(0)};
        const std::vector<std::byte> not_acl = tr::wire::encode(value);
        const auto w = g.write(*path_t::parse("/x:acl"), make_value(not_acl));
        check(!w.has_value() && w.error() == status_t::TYPE_MISMATCH,
              "writing a non-ACL TLV to :acl returns TYPE_MISMATCH");

        const auto r = g.read(*path_t::parse("/x:acl"));
        const bool unchanged = r.has_value() && r->bytes().size() == acl.size() &&
                               std::equal(acl.begin(), acl.end(), r->bytes().begin());
        check(unchanged, "rejected write leaves the stored ACL unchanged");
    }

    std::printf(g_failures == 0 ? "\nACL: PASS\n" : "\nACL: FAIL (%d)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
