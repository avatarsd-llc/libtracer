/**
 * @file
 * @brief #927 — `tr::net::config_reader_t`: the PAIR-CONSUMING SETTINGS walk every
 *        transport config parser shares.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The walk used to advance one child at a time, so the NAME child that is the
 * *value* of one pair was also tested as the *key* of the next position. Combined
 * with the ignore-unknown-keys forward-compat rule, any pair whose string value
 * textually equalled a known key (`link_hint = "addr"`) bound the FOLLOWING child
 * as that key's value — and last-match-wins then overrode a legitimate earlier
 * occurrence. This pins the fix and, just as importantly, pins what did NOT change:
 *
 *   - the hijack vectors (an unknown forward-compat key, and a plain value equal to
 *     another queried key) no longer bind anything;
 *   - the same vector on the `cert` + `key` shape, which is where the defect mattered
 *     most and where it lived longest: `parse_wt_config` kept a hand-rolled every-offset
 *     copy of this walk, so a forward-compat `hint = "key"` pair re-pointed a node's
 *     PRIVATE KEY at an attacker-named file. Both quic and webtransport run this reader
 *     now, so the walk pinned here is the one they execute;
 *   - forward-compat TOLERANCE survives — unknown pairs are still skipped, now as
 *     WHOLE pairs. This is the deliberate opposite of the ACL parse ruling (#906):
 *     config tolerates what it does not understand because a newer peer legitimately
 *     sends more; an ACL rejects it because a dropped attribute widens access;
 *   - representative well-formed tcp / quic / udp+ws / can configs parse exactly as
 *     they did before, including nested-SETTINGS module namespacing in a value slot;
 *   - repeat-key last-wins, wrong-type-ignored and empty-VALUE-ignored are unchanged.
 */

#include "libtracer/config_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/tlv_emit.hpp"

namespace {

using tr::net::config_reader_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;

/** @brief Report one assertion; a failure sets the process exit status. */
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

/** @brief True when @p got holds exactly @p want. */
bool is(const std::optional<std::string_view>& got, std::string_view want) {
    return got.has_value() && *got == want;
}

/** @brief Append a `NAME` child — a key, or a string value; the wire shape is one. */
void name_child(std::vector<std::byte>& out, std::string_view s) {
    tr::wire::emit_tlv(
        out, type_t::NAME, opt_t{},
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size()));
}

/** @brief Append a `VALUE` child carrying @p v little-endian in exactly `sizeof(T)` bytes. */
template <class T>
void value_child(std::vector<std::byte>& out, T v) {
    std::vector<std::byte> body;
    tr::detail::append_le<T>(body, v);
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, body);
}

/** @brief Append a child of @p type carrying @p body verbatim (a nested SETTINGS, a
 *         stray non-NAME in a key slot, an empty `VALUE`, …). */
void raw_child(std::vector<std::byte>& out, type_t type, opt_t opt,
               std::span<const std::byte> body) {
    tr::wire::emit_tlv(out, type, opt, body);
}

/**
 * @brief A decoded SETTINGS TLV plus the bytes it borrows.
 *
 * Heap-held so the decoded TLV's spans stay valid for the reader's whole lifetime.
 */
struct config_blob_t {
    std::vector<std::byte> bytes; /**< @brief The encoded SETTINGS TLV. */
    tr::wire::tlv_t tlv;          /**< @brief The decode of `bytes`. */
};

/** @brief Wrap @p children as `SETTINGS(PL=1){…}` and decode it. */
std::unique_ptr<config_blob_t> settings(const std::vector<std::byte>& children) {
    auto blob = std::make_unique<config_blob_t>();
    tr::wire::emit_tlv(blob->bytes, type_t::SETTINGS, opt_t{.pl = true}, children);
    auto dec = tr::wire::decode(blob->bytes);
    if (!dec) {
        check(false, "fixture SETTINGS failed to decode");
        return blob;
    }
    blob->tlv = std::move(*dec);
    return blob;
}

/**
 * @brief The issue's vector: an unknown forward-compat key whose string VALUE spells
 *        a known setting name.
 *
 * `SETTINGS{ NAME "addr" NAME "192.0.2.7", NAME "link_hint" NAME "addr",
 *            NAME "10.0.0.9" NAME "up" }` — a newer peer's `link_hint = "addr"` plus a
 * per-address pair it also does not share. Under the every-offset scan the "addr"
 * sitting in `link_hint`'s VALUE slot matched as a key, bound the next child
 * ("10.0.0.9") as its value, and last-match-wins destroyed the real `addr`.
 */
void test_unknown_key_value_cannot_hijack() {
    std::printf("unknown forward-compat key whose value spells a known key:\n");
    std::vector<std::byte> ch;
    name_child(ch, "addr");
    name_child(ch, "192.0.2.7");
    name_child(ch, "link_hint");
    name_child(ch, "addr");
    name_child(ch, "10.0.0.9");
    name_child(ch, "up");
    const auto blob = settings(ch);
    const config_reader_t cfg(&blob->tlv);

    const std::optional<std::string_view> addr = cfg.name("addr");
    std::printf("  (observed addr = %.*s)\n", static_cast<int>(addr.value_or("<absent>").size()),
                addr.value_or("<absent>").data());
    check(is(cfg.name("addr"), "192.0.2.7"), "the legitimate earlier `addr` survives");
    check(!is(cfg.name("addr"), "10.0.0.9"), "`addr` is NOT hijacked to the following child");
    check(is(cfg.name("link_hint"), "addr"),
          "the unknown pair is still a well-formed pair (its value reads as a value)");
    check(is(cfg.name("10.0.0.9"), "up"), "the third pair keeps its own key");
}

/**
 * @brief The verifier's vector: no unknown key needed — any string value equal to
 *        another queried key mis-bound.
 *
 * `SETTINGS{ NAME "kind" NAME "addr", NAME "port" VALUE u16 9000 }`: `kind = "addr"` is
 * a perfectly ordinary setting, yet the old scan read its value as a key and bound
 * `addr` to the string "port".
 */
void test_known_key_value_cannot_hijack() {
    std::printf("a string value equal to another queried key:\n");
    std::vector<std::byte> ch;
    name_child(ch, "kind");
    name_child(ch, "addr");
    name_child(ch, "port");
    value_child<std::uint16_t>(ch, 9000);
    const auto blob = settings(ch);
    const config_reader_t cfg(&blob->tlv);

    const std::optional<std::string_view> addr = cfg.name("addr");
    std::printf("  (observed addr = %.*s)\n", static_cast<int>(addr.value_or("<absent>").size()),
                addr.value_or("<absent>").data());
    check(is(cfg.name("kind"), "addr"), "`kind` keeps its own string value");
    check(!cfg.name("addr").has_value(), "`addr` was never set, so it reads as absent");
    check(cfg.u16("port") == std::optional<std::uint16_t>{9000}, "`port` is unaffected");
}

/**
 * @brief Forward-compat tolerance is PRESERVED — unknown pairs are skipped as whole
 *        pairs, whatever their value TLV is (integer, string, nested SETTINGS).
 */
void test_unknown_pairs_are_still_tolerated() {
    std::printf("unknown pairs are skipped, known keys around them still parse:\n");
    std::vector<std::byte> nested;  // a module namespace: NAME "send_buf_kb" VALUE u32
    name_child(nested, "send_buf_kb");
    value_child<std::uint32_t>(nested, 64);

    std::vector<std::byte> ch;
    name_child(ch, "future_u32");
    value_child<std::uint32_t>(ch, 7);
    name_child(ch, "addr");
    name_child(ch, "10.0.0.1");
    name_child(ch, "future_string");
    name_child(ch, "whatever");
    name_child(ch, "port");
    value_child<std::uint16_t>(ch, 8080);
    name_child(ch, "transport_tcp");
    raw_child(ch, type_t::SETTINGS, opt_t{.pl = true}, nested);
    name_child(ch, "keepalive");
    value_child<std::uint32_t>(ch, 30000);
    const auto blob = settings(ch);
    const config_reader_t cfg(&blob->tlv);

    check(is(cfg.name("addr"), "10.0.0.1"), "a known key after an unknown integer pair");
    check(cfg.u16("port") == std::optional<std::uint16_t>{8080},
          "a known key after an unknown string pair");
    check(cfg.u32("keepalive") == std::optional<std::uint32_t>{30000},
          "a known key after an unknown NESTED-SETTINGS pair (module namespacing)");
    check(!cfg.name("future_u32").has_value() && !cfg.u32("future_string").has_value(),
          "an unknown key still answers by TYPE — no accidental cross-type read");
    check(!cfg.name("send_buf_kb").has_value(),
          "the walk does not descend into a nested SETTINGS value");
}

/** @brief The universal transport keys (`transport_vertex::parse_config`) round-trip. */
void test_universal_keys_unchanged() {
    std::printf("the universal transport keys parse as before:\n");
    std::vector<std::byte> ch;
    name_child(ch, "kind");
    name_child(ch, "udp");
    name_child(ch, "addr");
    name_child(ch, "203.0.113.4");
    name_child(ch, "port");
    value_child<std::uint16_t>(ch, 7000);
    name_child(ch, "role");
    value_child<std::uint8_t>(ch, 1);
    name_child(ch, "keepalive");
    value_child<std::uint32_t>(ch, 15000);
    name_child(ch, "max_frame");
    value_child<std::uint32_t>(ch, 65536);
    name_child(ch, "backoff");
    value_child<std::uint32_t>(ch, 250);
    name_child(ch, "connect_timeout");
    value_child<std::uint32_t>(ch, 3000);
    const auto blob = settings(ch);
    const config_reader_t cfg(&blob->tlv);

    check(is(cfg.name("kind"), "udp") && is(cfg.name("addr"), "203.0.113.4"),
          "kind / addr (string values)");
    check(cfg.u16("port") == std::optional<std::uint16_t>{7000} &&
              cfg.u8("role") == std::optional<std::uint8_t>{1},
          "port / role (u16, u8)");
    check(cfg.u32("keepalive") == std::optional<std::uint32_t>{15000} &&
              cfg.u32("max_frame") == std::optional<std::uint32_t>{65536} &&
              cfg.u32("backoff") == std::optional<std::uint32_t>{250} &&
              cfg.u32("connect_timeout") == std::optional<std::uint32_t>{3000},
          "keepalive / max_frame / backoff / connect_timeout (u32)");
}

/** @brief The kind-private key sets (tcp+ws listener, quic, can) parse as before. */
void test_kind_private_keys_unchanged() {
    std::printf("the kind-private key sets parse as before:\n");
    {
        std::vector<std::byte> ch;  // tcp / ws listener
        name_child(ch, "role");
        value_child<std::uint8_t>(ch, 1);
        name_child(ch, "port");
        value_child<std::uint16_t>(ch, 7100);
        name_child(ch, "kind");
        name_child(ch, "tcp");
        name_child(ch, "peer_named");
        value_child<std::uint8_t>(ch, 1);
        name_child(ch, "max_peers");
        value_child<std::uint32_t>(ch, 4);
        const auto blob = settings(ch);
        const config_reader_t cfg(&blob->tlv);
        check(cfg.flag("peer_named") == std::optional<bool>{true} &&
                  cfg.u32("max_peers") == std::optional<std::uint32_t>{4} &&
                  is(cfg.name("kind"), "tcp"),
              "tcp/ws listener: peer_named / max_peers / kind");
    }
    {
        std::vector<std::byte> ch;  // quic
        name_child(ch, "cert");
        name_child(ch, "/etc/node/cert.pem");
        name_child(ch, "key");
        name_child(ch, "/etc/node/key.pem");
        const auto blob = settings(ch);
        const config_reader_t cfg(&blob->tlv);
        check(
            is(cfg.name("cert"), "/etc/node/cert.pem") && is(cfg.name("key"), "/etc/node/key.pem"),
            "quic: cert / key — two adjacent string pairs, the shape most at risk");
    }
    {
        std::vector<std::byte> ch;  // can
        name_child(ch, "ifname");
        name_child(ch, "can0");
        name_child(ch, "node");
        value_child<std::uint16_t>(ch, 3);
        name_child(ch, "version");
        value_child<std::uint8_t>(ch, 1);
        name_child(ch, "fd");
        value_child<std::uint8_t>(ch, 1);
        name_child(ch, "peer_ttl_ms");
        value_child<std::uint32_t>(ch, 5000);
        const auto blob = settings(ch);
        const config_reader_t cfg(&blob->tlv);
        check(is(cfg.name("ifname"), "can0") &&
                  cfg.u16("node") == std::optional<std::uint16_t>{3} &&
                  cfg.u8("version") == std::optional<std::uint8_t>{1} &&
                  cfg.flag("fd") == std::optional<bool>{true} &&
                  cfg.u32("peer_ttl_ms") == std::optional<std::uint32_t>{5000},
              "can: ifname / node / version / fd / peer_ttl_ms");
    }
}

/** @brief Repeat-key, wrong-type and empty-`VALUE` semantics are untouched. */
void test_repeat_and_illformed_semantics_unchanged() {
    std::printf("repeat / wrong-type / empty-VALUE semantics are unchanged:\n");
    {
        std::vector<std::byte> ch;  // last well-formed occurrence wins
        name_child(ch, "addr");
        name_child(ch, "first");
        name_child(ch, "addr");
        name_child(ch, "second");
        const auto blob = settings(ch);
        const config_reader_t cfg(&blob->tlv);
        check(is(cfg.name("addr"), "second"), "a repeated key resolves to the LAST occurrence");
    }
    {
        std::vector<std::byte> ch;  // a wrong-typed repeat does not clobber a good one
        name_child(ch, "port");
        value_child<std::uint16_t>(ch, 6000);
        name_child(ch, "port");
        name_child(ch, "not-an-integer");
        const auto blob = settings(ch);
        const config_reader_t cfg(&blob->tlv);
        check(cfg.u16("port") == std::optional<std::uint16_t>{6000},
              "a wrong-typed later occurrence is ignored, not destructive");
    }
    {
        std::vector<std::byte> ch;  // an empty VALUE payload is not a value
        name_child(ch, "port");
        raw_child(ch, type_t::VALUE, opt_t{}, {});
        const auto blob = settings(ch);
        const config_reader_t cfg(&blob->tlv);
        check(!cfg.u16("port").has_value(), "an empty VALUE payload is ignored");
    }
    {
        std::vector<std::byte> ch;  // a trailing unpaired key has no value
        name_child(ch, "addr");
        name_child(ch, "10.0.0.2");
        name_child(ch, "kind");
        const auto blob = settings(ch);
        const config_reader_t cfg(&blob->tlv);
        check(is(cfg.name("addr"), "10.0.0.2") && !cfg.name("kind").has_value(),
              "a trailing unpaired key is ignored, the pairs before it still parse");
    }
    {
        const config_reader_t none(nullptr);
        std::vector<std::byte> ch;
        const auto blob = settings(ch);
        const config_reader_t empty(&blob->tlv);
        check(!none.name("addr").has_value() && !none.u16("port").has_value() &&
                  !empty.name("addr").has_value(),
              "a null config and an empty SETTINGS answer nullopt");
    }
}

/**
 * @brief A non-NAME child where a key belongs stops the walk.
 *
 * The pairing is unknowable past that point, and resynchronizing by trying every
 * offset is precisely the defect this fix removes — so the walk stops instead of
 * guessing. The pairs before the desync still parse.
 */
void test_desync_stops_the_walk() {
    std::printf("a non-NAME child in a key slot desynchronizes the pair stream:\n");
    std::vector<std::byte> ch;
    name_child(ch, "addr");
    name_child(ch, "10.0.0.3");
    value_child<std::uint8_t>(ch, 9);  // stray: a value where a key belongs
    name_child(ch, "port");
    value_child<std::uint16_t>(ch, 9100);
    const auto blob = settings(ch);
    const config_reader_t cfg(&blob->tlv);

    check(is(cfg.name("addr"), "10.0.0.3"), "the pair before the desync still parses");
    check(!cfg.u16("port").has_value(), "nothing after the desync is bound (no resync guessing)");
}

/**
 * @brief The security-sensitive shape: the quic / webtransport `cert` + `key` pair.
 *
 * `parse_wt_config` (`core/src/transport_webtransport.cpp`) kept a hand-rolled
 * every-offset copy of this walk after the other five parsers were migrated, so the
 * defect survived on the one shape where it names a **private key file**. The vector
 * is the issue's own, retargeted at `key`:
 *
 * `SETTINGS{ NAME "cert" NAME "/etc/node/cert.pem", NAME "key" NAME "/etc/node/key.pem",
 *            NAME "hint" NAME "key", NAME "/tmp/attacker-key.pem" NAME "pem" }`
 *
 * A newer peer's forward-compat `hint = "key"` put the string `"key"` in a VALUE slot;
 * the every-offset scan re-read it as a key, bound the FOLLOWING child as the private-key
 * path, and last-match-wins overrode the legitimate `/etc/node/key.pem`. Observed under
 * the defect: `key = /tmp/attacker-key.pem`. Both transports now run this reader, so the
 * walk under test is the one they execute.
 */
void test_cert_key_pair_cannot_be_hijacked() {
    std::printf("the cert/key shape (quic + webtransport) cannot be hijacked:\n");
    std::vector<std::byte> ch;
    name_child(ch, "cert");
    name_child(ch, "/etc/node/cert.pem");
    name_child(ch, "key");
    name_child(ch, "/etc/node/key.pem");
    name_child(ch, "hint");
    name_child(ch, "key");
    name_child(ch, "/tmp/attacker-key.pem");
    name_child(ch, "pem");
    const auto blob = settings(ch);
    const config_reader_t cfg(&blob->tlv);

    const std::optional<std::string_view> k = cfg.name("key");
    std::printf("  (observed key = %.*s)\n", static_cast<int>(k.value_or("<absent>").size()),
                k.value_or("<absent>").data());
    check(is(cfg.name("key"), "/etc/node/key.pem"), "the legitimate private-key path survives");
    check(!is(cfg.name("key"), "/tmp/attacker-key.pem"),
          "`key` is NOT hijacked to the attacker-controlled following child");
    check(is(cfg.name("cert"), "/etc/node/cert.pem"), "`cert` is unaffected");
    check(is(cfg.name("hint"), "key"), "the unknown pair still reads as an ordinary pair");
}

}  // namespace

int main() {
    std::printf("config_reader_t — the pair-consuming SETTINGS walk (#927)\n\n");
    test_unknown_key_value_cannot_hijack();
    test_known_key_value_cannot_hijack();
    test_cert_key_pair_cannot_be_hijacked();
    test_unknown_pairs_are_still_tolerated();
    test_universal_keys_unchanged();
    test_kind_private_keys_unchanged();
    test_repeat_and_illformed_semantics_unchanged();
    test_desync_stops_the_walk();
    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
