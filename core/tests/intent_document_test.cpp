/**
 * @file
 * @brief RFC-0010 Amendment 5 (#2107): the borrowed intent document a vertex serves on
 *        `:schema`, as `NAME "doc"` → `VALUE` at the `POINT` level.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Every case reads through the graph's own read interface (`graph_t::read` on
 * `<v>:schema`): the four document × table combinations, the two conformance vectors
 * byte-exact, one span shared by two vertices, the `:settings` exclusion, and a replace or
 * clear racing a `:schema` read (a TSan target: the tsan lane runs the whole suite).
 */

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::app_access_t;
using tr::graph::app_field_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;

using bytes_t = std::vector<std::byte>;

/** @brief The document the vector `schema/owner-intent-document` carries. */
constexpr std::string_view kDocAlone =
    "contract: \"https://vendor.example/contracts/thermo/1.0\"\n"
    "help: \"Room temperature\"\n"
    "type: \"f32\"\n"
    "unit: \"Cel\"\n";
/** @brief The document the vector `schema/owner-intent-document-app` carries. */
constexpr std::string_view kDocFields =
    "contract: \"https://vendor.example/contracts/thermo/1.0\"\n"
    "help: \"Room temperature controller\"\n"
    "fields:\n"
    "  kp:\n"
    "    type: \"f32\"\n"
    "    help: \"Proportional gain\"\n";

/** @brief The raw bytes of a conformance vector's `input.bin`. */
bytes_t vector_bytes(std::string_view case_dir) {
    const std::filesystem::path p =
        std::filesystem::path{LIBTRACER_VECTORS_DIR} / case_dir / "input.bin";
    std::ifstream f(p, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    bytes_t out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    return out;
}

/** @brief The flattened bytes of a read of @p path; empty when the read failed. */
bytes_t read_bytes(const graph_t& g, std::string_view path) {
    const auto r = g.read(path_t(path));
    if (!r) return {};
    const tr::view::view_t flat = (*r)->rope().flatten();
    const std::span<const std::byte> b = flat.bytes();
    return bytes_t(b.begin(), b.end());
}

/** @brief The `:schema` record built by hand: `POINT{ NAME <name>, SETTINGS{} [, doc] [, app] }`,
 *         where @p app is the owner part's `SETTINGS` body (empty ⇒ no owner part). */
bytes_t expected_schema(std::string_view name, std::string_view doc, const bytes_t& app) {
    bytes_t body;
    tr::wire::emit_name(body, name);
    tr::wire::emit_tlv(body, type_t::SETTINGS, opt_t{.pl = true}, bytes_t{});
    if (!doc.empty()) {
        tr::wire::emit_name(body, "doc");
        tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::as_bytes(std::span(doc)));
    }
    if (!app.empty()) {
        tr::wire::emit_name(body, "app");
        tr::wire::emit_tlv(body, type_t::SETTINGS, opt_t{.pl = true}, app);
    }
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::POINT, opt_t{.pl = true}, body);
    return out;
}

/** @brief The owner part's body for one field `kp`, access `rw`, empty descriptor bytes. */
bytes_t kp_owner_part() {
    bytes_t rec;
    tr::wire::emit_name(rec, "access");
    tr::wire::emit_tlv(rec, type_t::VALUE, opt_t{},
                       std::as_bytes(std::span(std::string_view("rw"))));
    bytes_t app;
    tr::wire::emit_name(app, "kp");
    tr::wire::emit_tlv(app, type_t::SETTINGS, opt_t{.pl = true}, rec);
    return app;
}

/** @brief Install the one-field `kp` table on @p v. */
bool install_kp(graph_t& g, vertex_handle_t v) {
    std::vector<app_field_t> table;
    table.push_back(app_field_t{.name = "kp", .access = app_access_t::RW});
    return g.set_policy(v, {.app_fields = std::move(table)}).has_value();
}

/** @brief The four document × table cases, each read through `graph_t::read`. */
void test_four_cases() {
    std::printf("the four document x table cases:\n");
    // Neither: today's record, and the vector that pins it, byte for byte.
    {
        graph_t g;
        (void)g.register_vertex(path_t("/temp"), role_t::STORED_VALUE);
        check(read_bytes(g, "/temp:schema") == vector_bytes("settings/schema-enumerates-nothing"),
              "neither: byte-identical to settings/schema-enumerates-nothing (today)");
    }
    // Table only: today's two-part record, unchanged.
    {
        graph_t g;
        const vertex_handle_t v = g.register_vertex(path_t("/temp"), role_t::STORED_VALUE);
        std::vector<app_field_t> table;
        bytes_t desc;
        tr::wire::emit_name(desc, "dtype");
        tr::wire::emit_name(desc, "f32");
        table.push_back(app_field_t{.name = "kp", .access = app_access_t::RW, .descriptor = desc});
        (void)g.set_policy(v, {.app_fields = std::move(table)});
        check(read_bytes(g, "/temp:schema") == vector_bytes("tlv-types/point-schema-app"),
              "table only: byte-identical to tlv-types/point-schema-app (today)");
    }
    // Document only: the `doc` pair, and no owner part.
    {
        graph_t g;
        const vertex_handle_t v = g.register_vertex(path_t("/temp"), role_t::STORED_VALUE);
        check(g.set_intent_document(v, kDocAlone).has_value(), "document only: install succeeds");
        check(read_bytes(g, "/temp:schema") == expected_schema("temp", kDocAlone, {}),
              "document only: POINT{ NAME, SETTINGS{}, NAME \"doc\" VALUE <doc> }, no `app`");
        g.clear_intent_document(v);
        check(read_bytes(g, "/temp:schema") == vector_bytes("settings/schema-enumerates-nothing"),
              "document only, cleared: back to today's bytes");
    }
    // Both: the document, then the unchanged owner part; the order holds whichever lands first.
    {
        graph_t g;
        const vertex_handle_t v = g.register_vertex(path_t("/temp"), role_t::STORED_VALUE);
        (void)g.set_intent_document(v, kDocFields);
        (void)install_kp(g, v);
        const bytes_t want = expected_schema("temp", kDocFields, kp_owner_part());
        check(read_bytes(g, "/temp:schema") == want,
              "document plus table: doc pair before the owner part");
        g.clear_intent_document(v);
        check(read_bytes(g, "/temp:schema") == expected_schema("temp", {}, kp_owner_part()),
              "document plus table, document cleared: the owner part stays, unchanged");
        (void)g.set_intent_document(v, kDocAlone);
        check(read_bytes(g, "/temp:schema") == expected_schema("temp", kDocAlone, kp_owner_part()),
              "a second install replaces the document");
        (void)g.set_policy(v, {.app_fields = tr::graph::app_fields_decl_t{}});
        check(read_bytes(g, "/temp:schema") == expected_schema("temp", kDocAlone, {}),
              "the table uninstalled: the document stays, the owner part goes");
    }
}

/** @brief The two Amendment 5 vectors, byte-exact against `read_schema`. */
void test_conformance_vectors() {
    std::printf("the schema/owner-intent-document vectors:\n");
    {
        graph_t g;
        const vertex_handle_t v = g.register_vertex(path_t("/temp"), role_t::STORED_VALUE);
        (void)g.set_intent_document(v, kDocAlone);
        check(read_bytes(g, "/temp:schema") == vector_bytes("schema/owner-intent-document"),
              "schema/owner-intent-document == read_schema's bytes, exactly");
    }
    {
        graph_t g;
        const vertex_handle_t v = g.register_vertex(path_t("/temp"), role_t::STORED_VALUE);
        (void)install_kp(g, v);
        (void)g.set_intent_document(v, kDocFields);
        check(read_bytes(g, "/temp:schema") == vector_bytes("schema/owner-intent-document-app"),
              "schema/owner-intent-document-app == read_schema's bytes, exactly");
    }
}

/** @brief Many vertices may install the same borrowed bytes; each holds only its view. */
void test_shared_span() {
    std::printf("one span shared by two vertices:\n");
    graph_t g;
    const vertex_handle_t a = g.register_vertex(path_t("/a"), role_t::STORED_VALUE);
    const vertex_handle_t b = g.register_vertex(path_t("/b"), role_t::STORED_VALUE);
    (void)g.set_intent_document(a, kDocAlone);
    (void)g.set_intent_document(b, kDocAlone);
    check(read_bytes(g, "/a:schema") == expected_schema("a", kDocAlone, {}) &&
              read_bytes(g, "/b:schema") == expected_schema("b", kDocAlone, {}),
          "both vertices serve the one span");
    g.clear_intent_document(a);
    check(read_bytes(g, "/a:schema") == expected_schema("a", {}, {}),
          "clearing one: its schema is today's again");
    check(read_bytes(g, "/b:schema") == expected_schema("b", kDocAlone, {}),
          "... and the other still serves the document, intact");
}

/** @brief The document is schema, not a value: no `:settings` read carries it. */
void test_not_a_setting() {
    std::printf("the document is not a setting:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    const bytes_t settings_before = read_bytes(g, "/s:settings");
    (void)g.set_intent_document(v, kDocAlone);
    check(read_bytes(g, "/s:settings") == settings_before,
          "`:settings` is the empty container, with or without a document");
    const auto app = g.read(path_t("/s:settings.app"));
    check(!app && app.error() == status_t::SCHEMA_NOT_FOUND,
          "`:settings.app` stays closed: a document alone installs no table");
    (void)install_kp(g, v);
    bytes_t seven;
    tr::wire::emit_tlv(seven, type_t::VALUE, opt_t{}, bytes_t{std::byte{7}});
    check(g.write(path_t("/s:settings.app.kp"), tr::testing::make_value(seven)).has_value(),
          "the declared field takes a write");
    const bytes_t with_doc = read_bytes(g, "/s:settings");
    g.clear_intent_document(v);
    check(!with_doc.empty() && read_bytes(g, "/s:settings") == with_doc,
          "`:settings` with a table is the same bytes with or without a document");
}

/** @brief A replace or a clear racing `:schema` reads: every read is one whole record. */
void test_replace_and_clear_race_a_read() {
    std::printf("replace and clear racing a :schema read:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/r"), role_t::STORED_VALUE);
    const bytes_t none = expected_schema("r", {}, {});
    const bytes_t one = expected_schema("r", kDocAlone, {});
    const bytes_t two = expected_schema("r", kDocFields, {});
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const bytes_t got = read_bytes(g, "/r:schema");
            if (got != none && got != one && got != two) torn.fetch_add(1);
        }
    });
    for (int i = 0; i < 2000; ++i) {
        (void)g.set_intent_document(v, (i & 1) != 0 ? kDocAlone : kDocFields);
        if (i % 3 == 0) g.clear_intent_document(v);
    }
    stop.store(true, std::memory_order_release);
    reader.join();
    check(torn.load() == 0, "no read saw a torn or mixed record");
}

/**
 * @brief The borrowed bytes' lifetime against a racing `:schema` read: the owner installs a
 *        mutable buffer, clears it, then overwrites it, while another thread reads.
 *
 * The contract is that once a clear returns, no read touches the old bytes. A read that copied
 * them outside the vertex mutex would race the overwrite, which TSan reports; with the copy
 * under the mutex the clear orders every earlier read before the write. Each read must also
 * be one whole record: no document, or the buffer as it stood while installed.
 */
void test_clear_ends_the_borrow() {
    std::printf("a clear ends the borrow: the owner may overwrite the bytes at once:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/l"), role_t::STORED_VALUE);
    std::string buf(kDocAlone);
    std::string other(kDocAlone);
    other[0] = 'C';
    const bytes_t none = expected_schema("l", {}, {});
    const bytes_t a = expected_schema("l", kDocAlone, {});
    const bytes_t b = expected_schema("l", other, {});
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const bytes_t got = read_bytes(g, "/l:schema");
            if (got != none && got != a && got != b) torn.fetch_add(1);
        }
    });
    for (int i = 0; i < 2000; ++i) {
        (void)g.set_intent_document(v, buf);
        g.clear_intent_document(v);
        buf[0] = buf[0] == 'c' ? 'C' : 'c';  // the owner reuses its buffer once cleared
    }
    stop.store(true, std::memory_order_release);
    reader.join();
    check(torn.load() == 0, "every read is no document or the buffer as installed");
}

}  // namespace

int main() {
    test_four_cases();
    test_conformance_vectors();
    test_shared_span();
    test_not_a_setting();
    test_replace_and_clear_race_a_read();
    test_clear_ends_the_borrow();
    return tr::testing::summary("intent_document");
}
