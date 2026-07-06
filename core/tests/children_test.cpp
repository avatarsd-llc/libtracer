/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * #82 — in-band vertex creation via a `:children[]` SPEC write (ADR-0017, ADR-0021;
 * docs/reference/05 §0x0E). A SPEC{ type, name, config? } appended to a parent's
 * `:children[]` instantiates a child of a device-catalog type. Covers: create +
 * resolve the child, the built-in `stored_value` type, an unknown type =>
 * SCHEMA_NOT_FOUND, a duplicate name => PATH_IN_USE, a non-SPEC value =>
 * TYPE_MISMATCH, and a custom-registered factory (the #83 transport-vertex seam).
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

// A view_t over fresh owned bytes.
view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

// Build a SPEC{ NAME "type" <type>, NAME "name" <name> } as an owned VALUE view.
view_t spec(std::string_view type, std::string_view name) {
    std::vector<std::byte> body;
    tr::wire::emit_name(body, "type");
    tr::wire::emit_name(body, type);
    tr::wire::emit_name(body, "name");
    tr::wire::emit_name(body, name);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);
    return owned(out);
}

// A bare VALUE TLV (not a SPEC) — for the TYPE_MISMATCH case.
view_t bare_value() {
    std::vector<std::byte> out;
    const std::byte b{0x2A};
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return owned(out);
}

void test_create_and_resolve() {
    std::printf("Create a child via :children[] SPEC and resolve it:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);

    const auto w = g.write(path_t("/dev:children[]"), spec("stored_value", "temp"));
    check(w.has_value(), "SPEC{stored_value, temp} write accepted");

    // The child is now a first-class vertex at /dev/temp — resolvable and writable.
    vertex_t* child = g.find(path_t::parse("/dev/temp")->key());
    check(child != nullptr, "child /dev/temp resolves in the vertex map");
    const auto val = bare_value();
    const auto cw = g.write(path_t("/dev/temp"), val);
    check(cw.has_value(), "the created child accepts an ordinary value write");
    const auto cr = g.read(path_t("/dev/temp"));
    check(cr.has_value() && cr->only().bytes().size() == val.bytes().size() &&
              std::memcmp(cr->only().bytes().data(), val.bytes().data(), val.bytes().size()) == 0,
          "and reads back the identical VALUE bytes (last-writer-wins)");
}

void test_unknown_type() {
    std::printf("Unknown catalog type => SCHEMA_NOT_FOUND:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    const auto w = g.write(path_t("/dev:children[]"), spec("no_such_type", "x"));
    check(!w.has_value() && w.error() == status_t::SCHEMA_NOT_FOUND,
          "unknown type is the ENOTTY of creation");
    check(g.find(path_t::parse("/dev/x")->key()) == nullptr, "no child was created");
}

void test_duplicate_name() {
    std::printf("Duplicate child name => PATH_IN_USE:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    check(g.write(path_t("/dev:children[]"), spec("stored_value", "dup")).has_value(),
          "first create succeeds");
    const auto again = g.write(path_t("/dev:children[]"), spec("stored_value", "dup"));
    check(!again.has_value() && again.error() == status_t::PATH_IN_USE,
          "second create with the same name is rejected");
}

void test_non_spec_value() {
    std::printf("A non-SPEC :children[] value => TYPE_MISMATCH:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    const auto w = g.write(path_t("/dev:children[]"), bare_value());
    check(!w.has_value() && w.error() == status_t::TYPE_MISMATCH,
          "a bare VALUE is not a creation spec");
}

void test_custom_factory() {
    std::printf("A device-registered custom type (the #83 transport-vertex seam):\n");
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);

    // A device adds its own catalog type — here a STREAM-role vertex, standing in for
    // a controller / transport connection. The graph composes the key; the factory
    // only chooses the role (and could register several port sub-vertices).
    bool factory_ran = false;
    g.register_child_type(
        "streamy", [&factory_ran](graph_t& gg, std::vector<std::byte> key, const tr::wire::tlv_t*) {
            factory_ran = true;
            return gg.register_vertex_key(std::move(key), role_t::STREAM);
        });

    const auto w = g.write(path_t("/dev:children[]"), spec("streamy", "s"));
    check(w.has_value(), "SPEC of a custom-registered type is accepted");
    check(factory_ran, "the device factory ran");
    check(g.find(path_t::parse("/dev/s")->key()) != nullptr, "the custom child resolves");
}

}  // namespace

int main() {
    test_create_and_resolve();
    test_unknown_type();
    test_duplicate_name();
    test_non_spec_value();
    test_custom_factory();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
