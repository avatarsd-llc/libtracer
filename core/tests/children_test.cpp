/**
 * @file
 * @brief #82 — in-band vertex creation via a `:children[]` SPEC write (ADR-0017, ADR-0021;
 *        docs/reference/05 §0x0E).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A SPEC{ type, name, config? } appended to a parent's
 * `:children[]` instantiates a child of a device-catalog type. Covers: create +
 * resolve the child, the built-in `stored_value` type, an unknown type =>
 * SCHEMA_NOT_FOUND, a duplicate name => PATH_IN_USE, a non-SPEC value =>
 * TYPE_MISMATCH, and a custom-registered factory (the #83 transport-vertex seam).
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/config_reader.hpp"
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

/** @brief A view_t over fresh owned bytes. */
view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/** @brief Build a SPEC{ NAME "type" <type>, NAME "name" <name> } as an owned VALUE view. */
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

/**
 * @brief `SPEC{ type, name, <forward-compat pair whose VALUE spells @p hijacked_key>,
 *        <@p follower and its value> }` — the #927 pair-consuming vector on the creation spec.
 *
 * The creation spec is the same positional `(NAME key, value)` grammar as the transport
 * SETTINGS, and `create_child` walked it at every offset too. So a pair an older node is
 * meant to skip — `hint = "name"` from a newer peer — put the string `"name"` in a VALUE
 * slot, where the scan re-read it as a key and bound the FOLLOWING child as the child's
 * name. The vertex was then created at an address the sender never asked for, and the same
 * shape re-binds `type` (wrong factory) or `config` (a different SETTINGS reaches the
 * transport module — the `cert`/`key` blob among them).
 */
view_t spec_hijack(std::string_view type, std::string_view name, std::string_view hijacked_key,
                   std::string_view follower) {
    std::vector<std::byte> body;
    tr::wire::emit_name(body, "type");
    tr::wire::emit_name(body, type);
    tr::wire::emit_name(body, "name");
    tr::wire::emit_name(body, name);
    tr::wire::emit_name(body, "hint");
    tr::wire::emit_name(body, hijacked_key);  // a VALUE that spells a known key
    tr::wire::emit_name(body, follower);      // the child the old scan bound to it
    tr::wire::emit_name(body, "ignored");
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);
    return owned(out);
}

/**
 * @brief `SPEC{ NAME "type" VALUE <type>, NAME "name" VALUE <name> }` — the two field
 *        values spelled as VALUE (`0x01`) instead of NAME (`0x02`) (#877).
 *
 * A structurally valid TLV that decodes and re-encodes to itself, so a codec-only
 * conformance harness cannot tell it from the sanctioned spelling. The terminus can:
 * it matches each `(NAME key, value)` pair on the value's TYPE, so both fields are
 * skipped and the create is refused. This is the shape a binding emitted for months
 * while round-tripping its own bytes green.
 */
view_t spec_value_typed(std::string_view type, std::string_view name) {
    const auto emit_value_str = [](std::vector<std::byte>& out, std::string_view s) {
        tr::wire::emit_tlv(
            out, type_t::VALUE, opt_t{},
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size()));
    };
    std::vector<std::byte> body;
    tr::wire::emit_name(body, "type");
    emit_value_str(body, type);
    tr::wire::emit_name(body, "name");
    emit_value_str(body, name);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);
    return owned(out);
}

/** @brief The raw bytes of a conformance vector's `input.bin`. */
std::vector<std::byte> vector_bytes(std::string_view case_dir) {
    const std::filesystem::path p =
        std::filesystem::path{LIBTRACER_VECTORS_DIR} / case_dir / "input.bin";
    std::ifstream f(p, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    return out;
}

/** @brief A bare VALUE TLV (not a SPEC) — for the TYPE_MISMATCH case. */
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
    check(g.find(path_t::parse("/dev/temp")->key()).has_value(),
          "child /dev/temp resolves in the vertex map");
    const auto val = bare_value();
    const auto cw = g.write(path_t("/dev/temp"), val);
    check(cw.has_value(), "the created child accepts an ordinary value write");
    const auto cr = g.read(path_t("/dev/temp"));
    check(
        cr.has_value() && (*cr)->only().bytes().size() == val.bytes().size() &&
            std::memcmp((*cr)->only().bytes().data(), val.bytes().data(), val.bytes().size()) == 0,
        "and reads back the identical VALUE bytes (last-writer-wins)");
}

void test_unknown_type() {
    std::printf("Unknown catalog type => SCHEMA_NOT_FOUND:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    const auto w = g.write(path_t("/dev:children[]"), spec("no_such_type", "x"));
    check(!w.has_value() && w.error() == status_t::SCHEMA_NOT_FOUND,
          "unknown type is the ENOTTY of creation");
    check(!g.find(path_t::parse("/dev/x")->key()).has_value(), "no child was created");
}

/**
 * @brief `:children` is addressed WHOLE — `[]` and nothing else creates (#581).
 *
 * `append` was the sole predicate, so `:children[].bogus` (any tail, any depth) created the
 * child exactly as the sanctioned `:children[]` does and answered RESULT, with the tail
 * provably inert — two different tails produced identical replies and identical graph
 * state, and nothing was ever created AT the tail. The READ of the byte-identical selector
 * already answered SCHEMA_NOT_FOUND, so the halves disagreed. On `/net` this spelling built
 * a live connection vertex and wired it into the router.
 */
void test_children_addressed_whole() {
    std::printf(":children is addressed whole — a trailing step creates nothing:\n");
    for (const char* p : {"/dev:children[].bogus", "/dev:children[].a.b.c",
                          "/dev:children[3].bogus", "/dev:children.bogus"}) {
        graph_t g;
        (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
        const auto fp = path_t::parse(p);
        check(fp.has_value(), "the multi-step :children path parses (the branch is reachable)");
        if (!fp) continue;
        const auto w = g.write(*fp, spec("stored_value", "x"));
        check(!w.has_value() && w.error() == status_t::SCHEMA_NOT_FOUND,
              "a non-whole :children write names nothing: SCHEMA_NOT_FOUND");
        check(!g.find(path_t::parse("/dev/x")->key()).has_value(),
              "... and NO child was created (the tail must not be discarded)");
    }

    // The one legal shape is untouched — this gate must not use `plain_step`, since a
    // sanctioned create is exactly `append == true`.
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    check(g.write(path_t("/dev:children[]"), spec("stored_value", "x")).has_value(),
          ":children[] (append) still creates");
    check(g.find(path_t::parse("/dev/x")->key()).has_value(), "... and the child exists");
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
    check(g.find(path_t::parse("/dev/s")->key()).has_value(), "the custom child resolves");
}

}  // namespace

/**
 * @brief #688 / ADR-0073 §1 — the wire creation boundary runs THE segment predicate.
 *
 * A peer-supplied child name arrives as raw `NAME` payload bytes, bypassing
 * `path_t::parse` entirely — so before the fix, `SPEC{NAME "name" "a/b"}` registered a
 * vertex no conforming client could ever address (enumerable but unaddressable), and a
 * `/` inside one NAME broke the injectivity of the address→vertex map. Each rejection
 * case here failed before the fix (the write succeeded); the positive control pins that
 * the predicate does not over-reject.
 */
void test_wire_name_predicate() {
    std::printf("A peer-supplied child name must pass the segment predicate (#688):\n");
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);

    // One case per reserved character (reference/03 §Reserved characters).
    for (const std::string_view bad : {"a/b", "a:b", "a.b", "a*b", "a?b"}) {
        const auto w = g.write(path_t("/dev:children[]"), spec("stored_value", bad));
        char label[64];
        std::snprintf(label, sizeof label, "name \"%.*s\" => INVALID_PATH, nothing created",
                      static_cast<int>(bad.size()), bad.data());
        check(!w.has_value() && w.error() == status_t::INVALID_PATH, label);
    }

    // Over kMaxSegmentBytes: same rejection the local parser gives the same bytes.
    const std::string long_name(tr::graph::kMaxSegmentBytes + 1, 'x');
    const auto wl = g.write(path_t("/dev:children[]"), spec("stored_value", long_name));
    check(!wl.has_value() && wl.error() == status_t::INVALID_PATH,
          "an over-kMaxSegmentBytes name => INVALID_PATH");

    // And none of the rejects left residue: the parent still has no children at all.
    check(!g.find(path_t::parse("/dev/a")->key()).has_value(), "no partial registration");

    // Positive control: a legal name still creates, resolves, and is addressable
    // end-to-end through the SAME string grammar that rejects the cases above.
    const auto ok = g.write(path_t("/dev:children[]"), spec("stored_value", "ok-name"));
    check(ok.has_value(), "a legal name still creates");
    check(g.find(path_t::parse("/dev/ok-name")->key()).has_value(),
          "and the created child is addressable via path_t::parse");
}

/**
 * @brief A forward-compat pair in the creation SPEC cannot re-bind `name` or `type` (#927).
 *
 * `create_child`'s walk is now pair-consuming, so an unknown key is skipped together with
 * its value and no value child is ever tested as the next position's key.
 */
void test_spec_pair_scan_cannot_be_hijacked() {
    std::printf("a forward-compat SPEC pair cannot re-bind name or type (#927):\n");
    {
        graph_t g;
        (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
        const auto w =
            g.write(path_t("/dev:children[]"), spec_hijack("stored_value", "good", "name", "evil"));
        check(w.has_value(), "SPEC{stored_value, good, hint=\"name\", evil} is accepted");
        check(g.find(path_t::parse("/dev/good")->key()).has_value(),
              "the child is created at the name the sender ASKED for");
        check(!g.find(path_t::parse("/dev/evil")->key()).has_value(),
              "and NOT at the attacker-chosen following child");
    }
    {
        // The same shape against `type`: "no_such_type" is unregistered, so a hijack
        // would turn an accepted create into SCHEMA_NOT_FOUND (and, where a second type
        // IS registered, would silently build the wrong kind of vertex).
        graph_t g;
        (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
        const auto w = g.write(path_t("/dev:children[]"),
                               spec_hijack("stored_value", "temp", "type", "no_such_type"));
        check(w.has_value() && g.find(path_t::parse("/dev/temp")->key()).has_value(),
              "`type` keeps the sender's selector, so the create still succeeds");
    }
}

/**
 * @brief The shared `spec/` conformance vectors, fed to the REAL creation door (#877).
 *
 * The vectors are the cross-core contract for what a creation SPEC looks like — the same
 * bytes the Rust and TypeScript builders are pinned against. Two things are asserted, and
 * they are different claims:
 *
 * 1. **The bytes are golden.** The C++ emitter, run over the same inputs, reproduces the
 *    vector byte-for-byte — so a vector that drifts from the reference encoder fails here
 *    rather than sitting on disk describing a protocol nobody implements.
 * 2. **The bytes are ACCEPTED.** Each vector is written through `:children[]` and creates
 *    the vertex it names. That is the half a codec harness structurally cannot check: a
 *    round-trip is satisfied by any well-formed TLV, including one every terminus refuses.
 *
 * The `conn-client-ws` case additionally carries a `config` whose values are mixed by TYPE
 * (`role`/`port` opaque VALUEs, `kind`/`addr` textual NAMEs), and is read back through the
 * production `config_reader_t` — the typed read a transport factory performs.
 */
void test_conformance_vectors() {
    std::printf("the shared spec/ vectors are the bytes we emit AND the bytes we accept:\n");

    // --- spec/create-child -------------------------------------------------------
    {
        const std::vector<std::byte> vec = vector_bytes("spec/create-child");
        const view_t built = spec("stored_value", "temp");
        check(vec.size() == built.bytes().size() &&
                  std::memcmp(vec.data(), built.bytes().data(), vec.size()) == 0,
              "spec/create-child == what the C++ emitter builds (byte-exact)");

        graph_t g;
        (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
        const auto w = g.write(path_t("/dev:children[]"), owned(vec));
        check(w.has_value(), "the vector's bytes are ACCEPTED by :children[]");
        check(g.find(path_t::parse("/dev/temp")->key()).has_value(),
              "... and created /dev/temp, the vertex the vector names");
    }

    // --- spec/conn-client-ws -----------------------------------------------------
    {
        const std::vector<std::byte> vec = vector_bytes("spec/conn-client-ws");

        graph_t g;
        (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);

        // Stand in for a transport factory: capture the `config` SETTINGS the creation
        // door hands over, and read it with the production reader.
        bool saw_config = false;
        std::optional<std::uint8_t> role;
        std::optional<std::uint16_t> port;
        std::string kind;
        std::string addr;
        g.register_child_type(
            "client", [&](graph_t& gg, std::vector<std::byte> key, const tr::wire::tlv_t* config) {
                saw_config = config != nullptr;
                const tr::net::config_reader_t cfg(config);
                role = cfg.u8("role");
                port = cfg.u16("port");
                if (const auto k = cfg.name("kind")) kind.assign(*k);
                if (const auto a = cfg.name("addr")) addr.assign(*a);
                return gg.register_vertex_key(std::move(key), role_t::STREAM);
            });

        const auto w = g.write(path_t("/dev:children[]"), owned(vec));
        check(w.has_value(), "spec/conn-client-ws is ACCEPTED by :children[]");
        check(g.find(path_t::parse("/dev/up")->key()).has_value(), "... and created /dev/up");
        check(saw_config, "the vector's config SETTINGS reached the factory");
        check(role == 0 && port == 8080, "config_reader_t reads role=DIAL(0) and port=8080");
        check(kind == "ws" && addr == "127.0.0.1",
              "... and reads the STRING keys kind/addr, which only a NAME value can carry");
    }
}

/**
 * @brief A SPEC whose `type`/`name` values are VALUE-typed creates nothing (#877).
 *
 * The ablation behind the `spec/create-child` vector. These bytes differ from the vector
 * only in the two value TYPE codes, decode cleanly, and round-trip to themselves — so the
 * conformance harness scores them `ok` and can never object. The terminus is where the
 * difference is observable, and it is total: both fields are skipped, the catalog selector
 * is empty, and the answer is INVALID_PATH with nothing created. A binding that emits this
 * spelling cannot create a vertex anywhere, which is why the vector's value types are
 * asserted rather than assumed.
 */
void test_value_typed_spec_is_refused() {
    std::printf("a VALUE-typed SPEC field is not a lenient spelling — it creates nothing:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);

    const auto w = g.write(path_t("/dev:children[]"), spec_value_typed("stored_value", "temp"));
    check(!w.has_value() && w.error() == status_t::INVALID_PATH,
          "SPEC{ NAME \"type\" VALUE …, NAME \"name\" VALUE … } => INVALID_PATH");
    check(!g.find(path_t::parse("/dev/temp")->key()).has_value(), "nothing was created");

    // Positive control on the SAME two inputs: only the value type differs, and the
    // sanctioned spelling still creates. The rejection is about the type, nothing else.
    check(g.write(path_t("/dev:children[]"), spec("stored_value", "temp")).has_value(),
          "the NAME-typed spelling of the identical fields still creates");
    check(g.find(path_t::parse("/dev/temp")->key()).has_value(), "... and /dev/temp exists");
}

int main() {
    test_create_and_resolve();
    test_unknown_type();
    test_children_addressed_whole();
    test_duplicate_name();
    test_non_spec_value();
    test_custom_factory();
    test_wire_name_predicate();
    test_spec_pair_scan_cannot_be_hijacked();
    test_conformance_vectors();
    test_value_typed_spec_is_refused();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
