/**
 * @file
 * @brief RFC-0005 §C follow-on — the composed SUBTREE-SNAPSHOT read: a plain READ of a
 *        vertex with ≥ 1 registered child serves the folded POINT tree of its registered
 *        subtree's landed LKVs.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `snapshot(target) = POINT{ [stored TLV of target]?, child_node* }`,
 * `child_node(c) = POINT{ NAME(c), [stored TLV of c]?, child_node(grandchild)* }` — each
 * node's value is the vertex's stored TLV VERBATIM (opaque bytes; a non-VALUE TLV such as
 * a STATUS composes as-is). The battery (modeled on folded_children_test):
 * a seeded DIFFERENTIAL over randomized tree shapes (snapshot map == tracked writes ==
 * per-leaf reads, order-independent), the branch-write ROUND-TRIP, ACL subtree PRUNING
 * (incl. allow-under-denied-ancestor, set-equivalent to the gated enumerate+read loop),
 * leaf/handler REGRESSION (byte-identical to the pre-snapshot read), the names-only
 * topology tree, and the ZERO-COPY link-structure accounting (owned headers + borrowed
 * names + refcount-cloned LKV links — never one flattened buffer).
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::acl_right_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::subject_token_t;
using tr::graph::vertex_handle_t;
using tr::view::rope_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief A view_t over a fresh, owned heap segment holding `bytes`. */
view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/** @brief A view_t over an owned heap segment holding the given literal bytes. */
view_t make_value(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> v;
    v.reserve(bytes.size());
    for (const std::uint8_t b : bytes) v.push_back(std::byte{b});
    return make_value(v);
}

/** @brief A VALUE TLV over raw payload bytes. */
std::vector<std::byte> value_tlv(std::span<const std::byte> payload) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, payload);
    return out;
}

/** @brief A VALUE TLV over literal payload bytes. */
std::vector<std::byte> value_tlv(std::initializer_list<std::uint8_t> payload) {
    std::vector<std::byte> body;
    for (const std::uint8_t b : payload) body.push_back(std::byte{b});
    return value_tlv(body);
}

/** @brief A POINT TLV: NAME `name` first, then the pre-encoded `children` bytes. */
std::vector<std::byte> point_tlv(std::string_view name, std::span<const std::byte> children) {
    std::vector<std::byte> body;
    tr::wire::emit_name(body, name);
    body.insert(body.end(), children.begin(), children.end());
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::POINT, opt_t{.pl = true}, body);
    return out;
}

/** @brief Concatenate pre-encoded TLV byte runs. */
std::vector<std::byte> cat(std::initializer_list<std::vector<std::byte>> parts) {
    std::vector<std::byte> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

/** @brief Byte-compare a view against expected bytes. */
bool same_bytes(const view_t& v, std::span<const std::byte> expect) {
    const std::span<const std::byte> got = v.bytes();
    return got.size() == expect.size() &&
           (got.empty() || std::memcmp(got.data(), expect.data(), got.size()) == 0);
}

/**
 * @brief The decoded form of one snapshot: `topology` = every node path present in the
 *        POINT tree (the read target itself is "" — paths are target-relative,
 *        "/name/name…" below it); `values` = the nodes that carry stored bytes, verbatim.
 */
struct snap_map_t {
    std::set<std::string> topology;
    std::map<std::string, std::vector<std::byte>> values;
};

/**
 * @brief Parse one flattened snapshot POINT tree into a path→bytes map (the
 *        order-independent comparison form). Returns nullopt on any framing error.
 *
 * Node grammar: a POINT whose body is `[NAME]` (absent at the root — the target's
 * identity is the addressed vertex) then any run of non-POINT TLV records (the node's own
 * stored bytes, opaque and verbatim) and POINT child nodes.
 */
std::optional<snap_map_t> parse_snapshot(std::span<const std::byte> bytes) {
    snap_map_t out;
    /** @brief One TLV record: its type, body window, and end offset — or nullopt when ragged. */
    struct rec_t {
        type_t type;
        std::size_t body_off;
        std::size_t body_len;
        std::size_t end;
    };
    const auto record_at = [&bytes](std::size_t p) -> std::optional<rec_t> {
        if (p + 4 > bytes.size()) return std::nullopt;
        const auto type = static_cast<type_t>(std::to_integer<std::uint8_t>(bytes[p]));
        const std::uint8_t opt = std::to_integer<std::uint8_t>(bytes[p + 1]);
        const bool ll = opt_t::decode(opt).ll;  // typed unpack — no raw bit masks
        std::size_t len = 0;
        const std::size_t lw = ll ? 4 : 2;
        if (p + 2 + lw > bytes.size()) return std::nullopt;
        for (std::size_t i = 0; i < lw; ++i)
            len |= static_cast<std::size_t>(std::to_integer<std::uint8_t>(bytes[p + 2 + i]))
                   << (8 * i);
        if (p + 2 + lw + len > bytes.size()) return std::nullopt;
        return rec_t{type, p + 2 + lw, len, p + 2 + lw + len};
    };
    /** @brief One open node region: its body window cursor and the node's path. */
    struct open_t {
        std::size_t cursor;
        std::size_t end;
        std::string path;
        bool named;  // the root needs no NAME; a child must lead with one
    };
    const auto root = record_at(0);
    if (!root || root->type != type_t::POINT || root->end != bytes.size()) return std::nullopt;
    out.topology.insert("");
    std::vector<open_t> stack;
    stack.push_back(open_t{root->body_off, root->body_off + root->body_len, "", true});
    while (!stack.empty()) {
        open_t& top = stack.back();
        if (top.cursor >= top.end) {
            if (top.cursor != top.end) return std::nullopt;
            stack.pop_back();
            continue;
        }
        const auto r = record_at(top.cursor);
        if (!r || r->end > top.end) return std::nullopt;
        if (!top.named) {
            if (r->type != type_t::NAME) return std::nullopt;
            top.path += '/';
            for (std::size_t i = 0; i < r->body_len; ++i)
                top.path +=
                    static_cast<char>(std::to_integer<std::uint8_t>(bytes[r->body_off + i]));
            top.named = true;
            out.topology.insert(top.path);
            top.cursor = r->end;
            continue;
        }
        if (r->type == type_t::POINT) {
            const std::size_t body_off = r->body_off;
            const std::size_t body_end = r->end;
            top.cursor = r->end;  // `top` may be invalidated by the push below
            const std::string parent_path = top.path;
            stack.push_back(open_t{body_off, body_end, parent_path, false});
        } else {
            // Own stored bytes, verbatim (a run of records accumulates — opaque).
            auto& v = out.values[top.path];
            v.insert(v.end(), bytes.begin() + static_cast<std::ptrdiff_t>(top.cursor),
                     bytes.begin() + static_cast<std::ptrdiff_t>(r->end));
            top.cursor = r->end;
        }
    }
    return out;
}

/** @brief Read `target` and parse the snapshot, or nullopt on read/parse failure. */
std::optional<snap_map_t> read_snapshot_map(graph_t& g, const path_t& target,
                                            std::string_view caller = {}) {
    const auto h = g.find(target.key());
    if (!h) return std::nullopt;
    const auto r = g.read(*h, caller);
    if (!r) return std::nullopt;
    const view_t flat = r->flatten();
    return parse_snapshot(flat.bytes());
}

// --- 1. DIFFERENTIAL: randomized tree shapes ---------------------------------

/** @brief One randomized tree: registered relative paths + the value bytes written. */
struct gen_tree_t {
    std::vector<std::string> paths;  // relative ("/a", "/a/b", …), pre-order
    std::map<std::string, std::vector<std::byte>> written;
};

/** @brief Generate and register a random tree under `root`; write random VALUE TLVs. */
gen_tree_t build_random_tree(graph_t& g, const std::string& root, std::mt19937& rng) {
    gen_tree_t t;
    std::uniform_int_distribution<int> fan(0, 3);
    std::uniform_int_distribution<int> len(0, 5);
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<int> pct(0, 99);
    (void)g.register_vertex(path_t(root), role_t::STORED_VALUE);
    /** @brief One frontier entry: a registered relative path and its remaining depth. */
    struct work_t {
        std::string rel;
        int depth;
    };
    std::vector<work_t> stack{{std::string{}, 4}};
    while (!stack.empty()) {
        const work_t w = stack.back();
        stack.pop_back();
        const int n = w.depth > 0 ? fan(rng) : 0;
        for (int i = 0; i < n; ++i) {
            std::string rel = w.rel + "/" + static_cast<char>('a' + i);
            (void)g.register_vertex(path_t(root + rel), role_t::STORED_VALUE);
            t.paths.push_back(rel);
            if (pct(rng) < 60) {  // ~60% value presence
                std::vector<std::byte> payload(static_cast<std::size_t>(len(rng)));
                for (std::byte& b : payload) b = static_cast<std::byte>(byte(rng));
                const std::vector<std::byte> tlv = value_tlv(payload);
                if (g.write(path_t(root + rel), make_value(tlv)).has_value()) t.written[rel] = tlv;
            }
            stack.push_back(work_t{rel, w.depth - 1});
        }
    }
    // The target itself may hold its own value too (the root's leading value child).
    if (pct(rng) < 60) {
        const std::vector<std::byte> tlv = value_tlv({0x77});
        if (g.write(path_t(root), make_value(tlv)).has_value()) t.written[""] = tlv;
    }
    return t;
}

void test_differential_random_trees() {
    std::printf("DIFFERENTIAL — seeded randomized tree shapes:\n");
    std::mt19937 rng(0xC0FFEEu);  // seeded — reproducible shapes
    int bad_topology = 0;
    int bad_values = 0;
    int bad_leaves = 0;
    int leafless = 0;
    const int kShapes = 24;
    for (int s = 0; s < kShapes; ++s) {
        graph_t g;
        const std::string root = "/r";
        const gen_tree_t t = build_random_tree(g, root, rng);
        if (t.paths.empty()) {
            ++leafless;  // a childless shape — the leaf-read regression covers that path
            continue;
        }
        const auto snap = read_snapshot_map(g, path_t(root));
        if (!snap) {
            ++bad_topology;
            continue;
        }
        // Topology: every registered node of the subtree, exactly once.
        std::set<std::string> expect_topo{""};
        for (const std::string& p : t.paths) expect_topo.insert(p);
        if (snap->topology != expect_topo) ++bad_topology;
        // Values: the tracked writes, verbatim, order-independent.
        if (snap->values != t.written) ++bad_values;
        // Per-leaf differential: a leaf's snapshot bytes == its individual read().
        for (const std::string& p : t.paths) {
            const bool is_leaf = expect_topo.find(p + "/a") == expect_topo.end();
            if (!is_leaf) continue;
            const auto r = g.read(path_t(root + p));
            const auto it = snap->values.find(p);
            if (r.has_value() != (it != snap->values.end())) {
                ++bad_leaves;
                continue;
            }
            if (r && !same_bytes(r->flatten(), it->second)) ++bad_leaves;
        }
    }
    check(leafless < kShapes, "the seed produced non-trivial tree shapes");
    check(bad_topology == 0, "snapshot topology == registered subtree, every shape");
    check(bad_values == 0, "snapshot value map == tracked writes, every shape");
    check(bad_leaves == 0, "per-leaf read() matches the snapshot map, every shape");
}

// --- 2. ROUND-TRIP: branch-write then read back ------------------------------

void test_branch_write_round_trip() {
    std::printf("ROUND-TRIP — branch-write a POINT tree, read the parent back:\n");
    graph_t g;
    vertex_handle_t w = g.register_vertex(path_t("/w"), role_t::STORED_VALUE);
    // POINT{ NAME w, VALUE 07, POINT{ NAME t, VALUE AA BB }, POINT{ NAME u, POINT{ NAME
    // d, VALUE CC } } } — RFC-0005 decomposition creates /w/t, /w/u, /w/u/d.
    const std::vector<std::byte> branch =
        point_tlv("w", cat({value_tlv({0x07}), point_tlv("t", value_tlv({0xAA, 0xBB})),
                            point_tlv("u", point_tlv("d", value_tlv({0xCC})))}));
    check(g.write(w, make_value(branch)).has_value(), "branch write at /w succeeds");

    const auto snap = read_snapshot_map(g, path_t("/w"));
    check(snap.has_value(), "read /w parses as a snapshot POINT tree");
    if (!snap) return;
    const std::set<std::string> topo{"", "/t", "/u", "/u/d"};
    check(snap->topology == topo, "topology round-trips (every decomposed landing site)");
    const std::map<std::string, std::vector<std::byte>> values{
        {"", value_tlv({0x07})},
        {"/t", value_tlv({0xAA, 0xBB})},
        {"/u/d", value_tlv({0xCC})},
    };
    check(snap->values == values, "per-node stored TLVs round-trip verbatim (map-compare)");
}

// --- 3. ACL PRUNE ------------------------------------------------------------

void test_acl_prune() {
    std::printf("ACL PRUNE — a READ-denied mid node prunes its subtree, siblings stay:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/p/mid"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/p/mid/inner"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/p/sib"), role_t::STORED_VALUE);
    (void)g.write(path_t("/p/mid"), make_value(value_tlv({0x01})));
    (void)g.write(path_t("/p/mid/inner"), make_value(value_tlv({0x02})));
    (void)g.write(path_t("/p/sib"), make_value(value_tlv({0x03})));

    // Enforcement on: every caller resolves to a subject.
    g.set_subject_resolver([](std::string_view) -> std::optional<subject_token_t> {
        return subject_token_t{std::byte{'u'}};
    });
    std::vector<std::byte> everyone;
    for (const char c : std::string_view("EVERYONE@")) everyone.push_back(std::byte(c));
    // Deny READ on /p/mid (a WRITE-only ACE closes READ; no INHERIT — the denial is
    // structural pruning, not inherited policy)…
    const std::vector<tr::graph::ace_t> deny_aces{
        {.subject = everyone, .access_mask = static_cast<std::uint32_t>(acl_right_t::WRITE)}};
    const std::vector<std::byte> deny_read = tr::graph::encode_acl(deny_aces);
    check(g.write(path_t("/p/mid:acl"), make_value(deny_read)).has_value(),
          "install the READ-denying ACL on /p/mid");
    // …while /p/mid/inner explicitly GRANTS READ (the allow-under-denied-ancestor case).
    const std::vector<tr::graph::ace_t> grant_aces{
        {.subject = everyone, .access_mask = static_cast<std::uint32_t>(acl_right_t::READ)}};
    const std::vector<std::byte> grant_read = tr::graph::encode_acl(grant_aces);
    check(g.write(path_t("/p/mid/inner:acl"), make_value(grant_read)).has_value(),
          "grant READ on /p/mid/inner under the denied ancestor");

    check(g.read(path_t("/p/mid/inner")).has_value(),
          "the allowed grandchild IS directly readable (its own effective ACL grants READ)");

    const auto snap = read_snapshot_map(g, path_t("/p"), "peer");
    check(snap.has_value(), "snapshot read of /p succeeds for the gated caller");
    if (!snap) return;
    check(snap->topology.count("/sib") == 1, "the denied node's sibling is present");
    check(snap->topology.count("/mid") == 0, "the READ-denied mid node is absent");
    check(snap->topology.count("/mid/inner") == 0,
          "the allowed-under-denied-ancestor node is pruned WITH the subtree (structural)");
    check(snap->values.count("/sib") == 1 && snap->values.count("/mid") == 0,
          "no denied bytes leak into the value map");

    // Set-equivalence with the gated enumerate+read loop: what the snapshot serves is
    // exactly what this caller could gather by walking `:children` and reading, with the
    // same gates applied — the denied mid node stops BOTH walks identically.
    std::set<std::string> walked{""};
    std::vector<std::string> frontier{""};
    while (!frontier.empty()) {
        const std::string at = frontier.back();
        frontier.pop_back();
        const auto members = g.read(path_t("/p" + at + ":children"));
        if (!members) continue;  // READ denied here => cannot enumerate below
        const view_t flat = members->flatten();
        const auto listing = parse_snapshot(flat.bytes());  // POINT{ POINT{NAME}… } parses too
        if (!listing) continue;
        for (const std::string& child : listing->topology) {
            if (child.empty()) continue;
            const std::string rel = at + child;
            const auto ch = g.find(path_t::parse("/p" + rel)->key());
            if (!ch) continue;
            const auto r = g.read(*ch, "peer");
            if (!r.has_value() && r.error() == status_t::PERMISSION_DENIED)
                continue;  // the same PRUNE gate
            walked.insert(rel);
            frontier.push_back(rel);
        }
    }
    check(walked == snap->topology,
          "snapshot topology is set-equivalent to the gated enumerate+read loop");
}

// --- 4. REGRESSION: leaf and handler-target reads ----------------------------

void test_leaf_and_handler_regression() {
    std::printf("REGRESSION — leaf and handler-target reads byte-identical to before:\n");
    graph_t g;
    vertex_handle_t leaf = g.register_vertex(path_t("/leaf"), role_t::STORED_VALUE);
    const std::vector<std::byte> val = value_tlv({0x2A});
    check(g.write(leaf, make_value(val)).has_value(), "write the leaf value");
    const auto r = g.read(leaf);
    check(r.has_value() && r->link_count() == 1 && same_bytes(r->only(), val),
          "leaf read serves the stored TLV byte-identically (single-link, zero copy)");

    vertex_handle_t empty = g.register_vertex(path_t("/empty"), role_t::STORED_VALUE);
    const auto e = g.read(empty);
    check(!e.has_value() && e.error() == status_t::NOT_FOUND,
          "value-less childless leaf stays NOT_FOUND");

    // A HANDLER target's on_read keeps precedence over the snapshot even with children.
    tr::graph::handlers_t h;
    h.on_read = [] { return rope_t{make_value({0x2A})}; };
    vertex_handle_t hv = g.register_vertex(path_t("/hnd"), role_t::HANDLER, std::move(h));
    (void)g.register_vertex(path_t("/hnd/child"), role_t::STORED_VALUE);
    (void)g.write(path_t("/hnd/child"), make_value(value_tlv({0x11})));
    const auto hr = g.read(hv);
    check(hr.has_value() && hr->link_count() == 1 &&
              std::to_integer<int>(hr->only().bytes()[0]) == 0x2A,
          "handler-target read serves on_read, not the snapshot (precedence kept)");

    // A HANDLER without on_read stays NOT_FOUND — the hook sits AFTER the handler seam.
    vertex_handle_t hn = g.register_vertex(path_t("/hnd2"), role_t::HANDLER);
    (void)g.register_vertex(path_t("/hnd2/child"), role_t::STORED_VALUE);
    const auto hnr = g.read(hn);
    check(!hnr.has_value() && hnr.error() == status_t::NOT_FOUND,
          "on_read-less handler target stays NOT_FOUND even with children");
}

// --- 5. Non-VALUE stored TLV composes verbatim -------------------------------

void test_non_value_tlv_verbatim() {
    std::printf("non-VALUE stored TLV (STATUS) composes verbatim:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/n"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/n/st"), role_t::STORED_VALUE);
    // An empty STATUS=OK TLV (the 4-byte `09 00 00 00` seed frame) — stored bytes are
    // opaque to the graph; the snapshot must not re-type or re-wrap them.
    std::vector<std::byte> status;
    tr::wire::emit_tlv(status, type_t::STATUS, opt_t{}, {});
    check(g.write(path_t("/n/st"), make_value(status)).has_value(), "store the STATUS TLV");
    const auto snap = read_snapshot_map(g, path_t("/n"));
    check(snap.has_value(), "snapshot read of /n succeeds");
    if (!snap) return;
    const auto it = snap->values.find("/st");
    check(it != snap->values.end() && it->second == status,
          "the STATUS TLV appears verbatim as the node's stored bytes");
}

// --- 6. Names-only branch -> topology-only POINT tree ------------------------

void test_names_only_topology() {
    std::printf("names-only branch — no descendant values -> topology-only POINT tree:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/t"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/t/a"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/t/b"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/t/b/c"), role_t::STORED_VALUE);
    const auto h = g.find(path_t::parse("/t")->key());
    const auto r = g.read(*h);
    check(r.has_value(), "read of the value-less branch succeeds (not NOT_FOUND)");
    if (!r) return;
    // Exact expected bytes: POINT{ POINT{NAME a}, POINT{NAME b, POINT{NAME c}} }.
    std::vector<std::byte> a_node;
    {
        std::vector<std::byte> body;
        tr::wire::emit_name(body, "a");
        tr::wire::emit_tlv(a_node, type_t::POINT, opt_t{.pl = true}, body);
    }
    const std::vector<std::byte> b_node = point_tlv("b", point_tlv("c", {}));
    std::vector<std::byte> expect;
    tr::wire::emit_tlv(expect, type_t::POINT, opt_t{.pl = true}, cat({a_node, b_node}));
    check(same_bytes(r->flatten(), expect), "topology-only snapshot bytes are exact");
}

// --- 7. ZERO-COPY accounting --------------------------------------------------

void test_zero_copy_link_structure() {
    std::printf("ZERO-COPY — per-node owned header + borrowed name + cloned LKV links:\n");
    graph_t g;
    vertex_handle_t z = g.register_vertex(path_t("/z"), role_t::STORED_VALUE);
    vertex_handle_t c1 = g.register_vertex(path_t("/z/c1"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/z/c2"), role_t::STORED_VALUE);
    const std::vector<std::byte> zv = value_tlv({0x01});
    const std::vector<std::byte> cv = value_tlv({0x02, 0x03});
    check(g.write(z, make_value(zv)).has_value(), "write /z's own value");
    check(g.write(c1, make_value(cv)).has_value(), "write /z/c1's value");

    const auto r = g.read(z);
    check(r.has_value(), "snapshot read of /z succeeds");
    if (!r) return;
    // Root: header + 1 LKV link. c1: header + borrowed name + 1 LKV link. c2 (value-less):
    // header + borrowed name. NEVER one flattened buffer.
    check(r->link_count() == 7, "7 links: (hdr+lkv) + (hdr+name+lkv) + (hdr+name)");
    // The LKV links are refcount CLONES of the stored segments — the same bytes a leaf
    // read serves, at the same addresses (no byte copy anywhere).
    const auto leaf = g.read(c1);
    check(leaf.has_value() && leaf->link_count() == 1, "leaf read of /z/c1 is single-link");
    if (leaf) {
        bool shared = false;
        for (const view_t& l : r->links())
            if (l.bytes().data() == leaf->only().bytes().data() &&
                l.bytes().size() == leaf->only().bytes().size())
                shared = true;
        check(shared, "the snapshot's c1 LKV link aliases the stored segment (refcount clone)");
    }
    // And the flattened whole still parses to the right map.
    const auto snap = parse_snapshot(r->flatten().bytes());
    check(snap.has_value() && snap->values.size() == 2 && snap->topology.size() == 3,
          "the scatter-gather rope flattens to the same composed tree");
}

}  // namespace

int main() {
    std::printf("libtracer subtree-snapshot read tests (RFC-0005 §C follow-on):\n");
    test_differential_random_trees();
    test_branch_write_round_trip();
    test_acl_prune();
    test_leaf_and_handler_regression();
    test_non_value_tlv_verbatim();
    test_names_only_topology();
    test_zero_copy_link_structure();
    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
