/**
 * @file
 * @brief #2484 — `ws_framing_t` over `frame_ingress_t::read_with`: core's RFC 6455 receive
 *        rules framing a raw server stream that arrives one single-buffer read at a time,
 *        the way the ESP-IDF client link reads `esp_transport_read`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * One stream of server frames is cut into reads of every size from 1 byte up, so every
 * header and body boundary lands at every offset of a read. Each cut must hand up the same
 * messages: an unfragmented BINARY whole, a fragmented one reassembled across a PING (which
 * gets its PONG), a TEXT ignored. Then the closing answers: a CLOSE is handed to the end and
 * closes the stream, an over-cap header closes it counted, and the shared upgrade check
 * (`ws::upgrade_accepted`) accepts the RFC 6455 §1.3 answer and nothing else.
 */

#include "libtracer/ws_framing.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/frame_ingress.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/ws.hpp"

namespace {

using op_t = tr::net::ws::opcode_t;

int g_failures = 0;
/** @brief Record one assertion. */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief The end under test: records what the rules asked of it. */
struct end_t {
    /** @brief An end with receive cap @p cap, reassembling from the heap. */
    explicit end_t(std::size_t cap = 1024) : cap_bytes(cap), assembler(tr::mem::heap_source()) {}

    std::size_t cap_bytes;                      /**< @brief The receive cap. */
    tr::net::ws_rx_frame_t frame;               /**< @brief The frame in progress. */
    tr::net::ws_assembler_t assembler;          /**< @brief Fragment reassembly. */
    std::vector<std::vector<std::byte>> got;    /**< @brief Messages handed up. */
    std::vector<std::vector<std::byte>> pongs;  /**< @brief PING payloads answered. */
    std::vector<std::vector<std::byte>> closes; /**< @brief CLOSE payloads answered. */
    int malformed_count = 0;                    /**< @brief Rule breaches counted. */
    int dropped_count = 0;                      /**< @brief Refusals counted. */

    [[nodiscard]] std::size_t cap() const { return cap_bytes; }
    void malformed() { ++malformed_count; }
    void dropped() { ++dropped_count; }
    void pong(std::span<const std::byte> b) { pongs.emplace_back(b.begin(), b.end()); }
    void closing(std::span<const std::byte> b) { closes.emplace_back(b.begin(), b.end()); }
    void hand_up(tr::view::rope_t m) {
        const tr::view::view_t flat = m.materialize();
        got.emplace_back(flat.bytes().begin(), flat.bytes().end());
    }
};

/** @brief @p n bytes of @p seed, @p seed + 1, ... */
std::vector<std::byte> pattern(std::size_t n, unsigned seed) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::byte>((seed + i) & 0xFF);
    return v;
}

/** @brief Append one unmasked server frame to @p out. */
void put(std::vector<std::byte>& out, op_t op, bool fin, const std::vector<std::byte>& payload) {
    out.push_back(static_cast<std::byte>((fin ? 0x80U : 0U) | static_cast<unsigned>(op)));
    const std::size_t n = payload.size();
    if (n < 126) {
        out.push_back(static_cast<std::byte>(n));
    } else {
        out.push_back(std::byte{126});
        out.push_back(static_cast<std::byte>(n >> 8));
        out.push_back(static_cast<std::byte>(n & 0xFF));
    }
    out.insert(out.end(), payload.begin(), payload.end());
}

/**
 * @brief Frame @p wire through read_with, at most @p step bytes per read.
 * @return Whether the stream stayed open to its end.
 */
bool frame_all(end_t& end, const std::vector<std::byte>& wire, std::size_t step) {
    const tr::net::ws_framing_t<end_t> framing{end};
    tr::net::frame_ingress_t<tr::net::ws::kMaxClientFrameHeader, 0> ingress;
    std::size_t at = 0;
    while (at < wire.size()) {
        const bool open = ingress.read_with(
            [&](std::span<std::byte> into) {
                const std::size_t n = std::min({into.size(), step, wire.size() - at});
                std::copy_n(wire.begin() + static_cast<std::ptrdiff_t>(at), n, into.begin());
                at += n;
                return static_cast<int>(n);
            },
            tr::mem::heap_backend(), framing);
        if (!open) return false;
    }
    return !ingress.mid_frame();
}

}  // namespace

int main() {
    std::printf("ws_framing_t over frame_ingress_t::read_with (#2484):\n");
    const std::vector<std::byte> whole = pattern(300, 1);
    const std::vector<std::byte> a = pattern(10, 0x40);
    const std::vector<std::byte> b = pattern(7, 0x80);
    const std::vector<std::byte> ping = pattern(5, 0xC0);
    std::vector<std::byte> wire;
    put(wire, op_t::BINARY, true, whole);
    put(wire, op_t::TEXT, true, pattern(20, 9));
    put(wire, op_t::BINARY, false, a);
    put(wire, op_t::PING, true, ping);
    put(wire, op_t::CONT, true, b);
    put(wire, op_t::BINARY, true, {});
    std::vector<std::byte> ab = a;
    ab.insert(ab.end(), b.begin(), b.end());

    bool every_cut = true;
    for (std::size_t step = 1; step <= wire.size(); ++step) {
        end_t end;
        const bool open = frame_all(end, wire, step);
        every_cut = every_cut && open && end.got.size() == 3 && end.got[0] == whole &&
                    end.got[1] == ab && end.got[2].empty() && end.pongs.size() == 1 &&
                    end.pongs[0] == ping && end.closes.empty() && end.malformed_count == 0 &&
                    end.dropped_count == 0;
    }
    check(every_cut,
          "every read size hands up the same messages: whole, reassembled across a PING, "
          "TEXT ignored, the PING answered");

    {
        end_t end;
        std::vector<std::byte> closing;
        put(closing, op_t::CLOSE, true, pattern(2, 3));
        put(closing, op_t::BINARY, true, pattern(4, 4));
        check(!frame_all(end, closing, 3), "a CLOSE closes the stream");
        check(end.closes.size() == 1 && end.closes[0] == pattern(2, 3),
              "after handing its payload to the end's closing()");
        check(end.got.empty(), "and nothing behind it is handed up");
    }
    {
        end_t end(64);
        std::vector<std::byte> over;
        put(over, op_t::BINARY, true, pattern(65, 5));
        check(!frame_all(end, over, 4), "a header announcing more than the cap closes the stream");
        check(end.malformed_count == 1 && end.got.empty(), "counted, nothing handed up");
    }
    {
        using tr::net::ws::upgrade_accepted;
        constexpr std::string_view kKey = "dGhlIHNhbXBsZSBub25jZQ==";  // RFC 6455 §1.3
        check(upgrade_accepted("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                               "Connection: Upgrade\r\nsec-WEBSOCKET-accept:  "
                               "s3pPLMBiTxaQ9kYGzzhZRbK+xOo= \r\n\r\n",
                               kKey),
              "upgrade_accepted: the RFC 6455 §1.3 answer, any header-name case, trimmed");
        check(!upgrade_accepted("HTTP/1.1 101 Switching Protocols\r\n"
                                "Sec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n",
                                kKey),
              "a wrong accept is refused");
        check(!upgrade_accepted("HTTP/1.1 403 Forbidden\r\n"
                                "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n",
                                kKey),
              "so is any status but 101");
        check(!upgrade_accepted("HTTP/1.1 101 Switching Protocols\r\n\r\n", kKey),
              "and an answer with no accept at all");
        // #2515: the status line's code, and both RFC 6455 §4.1 headers, case-insensitive.
        constexpr std::string_view kGood = "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n";
        const auto answer = [&](std::string_view status, std::string_view lines) {
            return upgrade_accepted(
                std::string(status) + "\r\n" + std::string(lines) + std::string(kGood) + "\r\n",
                kKey);
        };
        check(answer("HTTP/1.1 101 Switching Protocols",
                     "UPGRADE: WebSocket\r\nconnection: keep-alive, UPGRADE\r\n"),
              "both headers match case-insensitively, Upgrade as one token of a list");
        check(answer("HTTP/1.1 101", "Upgrade: websocket\r\nConnection: Upgrade\r\n"),
              "a status line with no reason phrase is still a 101");
        check(!answer("HTTP/1.1 403 Forbidden",
                      "X-Note: 101\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"),
              "a 101 in a header value is not the status code");
        check(!answer("HTTP/1.1 400 Bad 101", "Upgrade: websocket\r\nConnection: Upgrade\r\n"),
              "nor is one in the reason phrase");
        check(!answer("HTTP/1.1 1010 Odd", "Upgrade: websocket\r\nConnection: Upgrade\r\n"),
              "nor a code that only starts with 101");
        check(!answer("HTTP/1.1 101 Switching Protocols", "Connection: Upgrade\r\n"),
              "an answer with no Upgrade header is refused");
        check(
            !answer("HTTP/1.1 101 Switching Protocols", "Upgrade: h2c\r\nConnection: Upgrade\r\n"),
            "so is an Upgrade to anything but websocket");
        check(!answer("HTTP/1.1 101 Switching Protocols", "Upgrade: websocket\r\n"),
              "and one with no Connection header");
        check(!answer("HTTP/1.1 101 Switching Protocols",
                      "Upgrade: websocket\r\nConnection: keep-alive, upgraded\r\n"),
              "or a Connection list without the Upgrade token");
    }

    if (g_failures != 0) {
        std::printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
