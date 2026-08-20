/**
 * @file
 * @brief The WebTransport peer, driven from ANOTHER PROCESS — the instrument #1182 was
 *        missing.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `transport_webtransport.cpp`'s peer-driven allocations run on msquic worker threads, and
 * both failure-injection seams this tree owns are aimed at a PROCESS, not at a side of the
 * pair:
 *
 *  - `transport_alloc_softfail_test.cpp`'s `operator new` override is `thread_local` to the
 *    arming thread, so a worker never sees it;
 *  - `tr::detail::probe_fail_hook` IS process-wide and `try_reserve` consults it — but with
 *    the peer in the same process it also refuses the PEER's opens, so no stream ever reaches
 *    the server and the assertion measures the client (measured on #1108's branch).
 *
 * Running the peer here, behind a `fork`+`exec`, is what separates them: the hook armed in the
 * server's process reaches only the server's workers. This binary owns no libtracer transport
 * at all — it is `raw_wt_client_t` (raw_wt_client.hpp, shared verbatim with
 * `webtransport_test.cpp`) plus a line protocol, so the gated peer and the peer every other
 * classifier vector uses cannot drift apart.
 *
 * ## The protocol
 *
 * One command per stdin line, exactly one response line per command, flushed. Synchronous by
 * construction: the parent never has to disentangle an async notification from a reply, and
 * every step it takes is ordered after an acknowledged one — which is what lets the test
 * synchronise on observable state instead of sleeping.
 *
 *     CONNECT <port>            -> CONNECT ok | CONNECT fail
 *     SESSION <authority> [path]-> SESSION <tag> ok | SESSION fail
 *     OPEN                      -> OPEN <tag> ok | OPEN <tag> fail
 *     WRITE <tag> <hex...>      -> WRITE ok | WRITE fail
 *     ISABORT <tag>             -> ISABORT 0 | ISABORT 1
 *     WAITABORT <tag> <ms>      -> WAITABORT 0 | WAITABORT 1
 *     WAITSHUT <ms>             -> WAITSHUT 0 | WAITSHUT 1
 *     CLOSE <tag>               -> CLOSE ok
 *     QUIT                      -> QUIT ok   (then exit 0)
 *
 * `SESSION`'s optional second word is the extended CONNECT `:path`; it defaults to the root.
 * A long one is what lets a vector aim at the server's PEER-SIZED `:path` copy (#934).
 * `WAITSHUT` is the CONNECTION-scoped counterpart of `WAITABORT`: it answers whether the
 * server tore the whole connection down, which is what the count-then-close refusal of an
 * unaffordable extended CONNECT looks like from the peer (#934).
 *
 * A closed stdin (the parent died) ends the loop, so no driver can outlive its test.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "raw_wt_client.hpp"

namespace {

using tr::testing::wt::connect_frame;
using tr::testing::wt::raw_wt_client_t;

/** @brief The client, constructed on CONNECT so a failed QUIC handshake is a reportable
 *         answer rather than a dead process. */
std::unique_ptr<raw_wt_client_t> g_cli;

/** @brief Streams by tag — the index IS the tag the parent quotes back. */
std::vector<raw_wt_client_t::wt_stream_t*> g_streams;

/** @brief The stream @p tag names, or null when the parent quoted a tag that does not
 *         exist (a harness bug, answered rather than crashed on). */
raw_wt_client_t::wt_stream_t* by_tag(std::size_t tag) {
    return tag < g_streams.size() ? g_streams[tag] : nullptr;
}

/** @brief Decode an even-length hex string into bytes; empty on a malformed digit. */
std::vector<std::uint8_t> from_hex(std::string_view hex) {
    std::vector<std::uint8_t> out;
    if (hex.size() % 2 != 0) return out;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nib(hex[i]);
        const int lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

/** @brief Emit one response line and flush it — the parent blocks on this line. */
void reply(const std::string& line) {
    std::cout << line << '\n';
    std::cout.flush();
}

/** @brief Open one stream, register it under the next tag, and answer with that tag. */
void do_open(bool uni, bool immediate = false) {
    if (!g_cli) {
        reply("OPEN 0 fail");
        return;
    }
    raw_wt_client_t::wt_stream_t* const s = g_cli->open_stream(uni, immediate);
    const std::size_t tag = g_streams.size();
    g_streams.push_back(s);
    std::ostringstream os;
    os << "OPEN " << tag << (s->h != nullptr ? " ok" : " fail");
    reply(os.str());
}

}  // namespace

int main() {
    // Unbuffered stdin reads plus explicit flushes on stdout: the protocol's whole value is
    // that a reply means the command really ran.
    std::ios::sync_with_stdio(false);

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream is(line);
        std::string verb;
        is >> verb;

        if (verb == "CONNECT") {
            unsigned port = 0;
            is >> port;
            g_cli = std::make_unique<raw_wt_client_t>(static_cast<std::uint16_t>(port));
            reply(g_cli->ok ? "CONNECT ok" : "CONNECT fail");
        } else if (verb == "SESSION") {
            std::string authority;
            std::string path;
            is >> authority >> path;
            if (path.empty()) path = "/";
            if (!g_cli || !g_cli->ok) {
                reply("SESSION fail");
                continue;
            }
            // The extended CONNECT that establishes the session. The SERVER decides when it
            // is up; the parent waits on `session_up()`, never on this reply.
            raw_wt_client_t::wt_stream_t* const s = g_cli->open_bidi();
            const std::size_t tag = g_streams.size();
            g_streams.push_back(s);
            if (s->h == nullptr) {
                reply("SESSION fail");
                continue;
            }
            g_cli->write(s, connect_frame(authority, path));
            std::ostringstream os;
            os << "SESSION " << tag << " ok";
            reply(os.str());
        } else if (verb == "OPEN") {
            do_open(true);
        } else if (verb == "OPENI") {
            // IMMEDIATE: the stream is announced to the server with NO payload, so it
            // provokes stream adoption alone (#1182 guard 1) — nothing follows it into the
            // handshake accumulator.
            do_open(true, true);
        } else if (verb == "OPENBIDI") {
            do_open(false);
        } else if (verb == "WRITE") {
            std::size_t tag = 0;
            std::string hex;
            is >> tag >> hex;
            raw_wt_client_t::wt_stream_t* const s = by_tag(tag);
            const std::vector<std::uint8_t> bytes = from_hex(hex);
            if (s == nullptr || !g_cli || bytes.empty()) {
                reply("WRITE fail");
                continue;
            }
            g_cli->write(s, bytes);
            reply("WRITE ok");
        } else if (verb == "ISABORT") {
            std::size_t tag = 0;
            is >> tag;
            raw_wt_client_t::wt_stream_t* const s = by_tag(tag);
            const bool a = s != nullptr && s->aborted.load(std::memory_order_relaxed);
            reply(a ? "ISABORT 1" : "ISABORT 0");
        } else if (verb == "WAITABORT") {
            std::size_t tag = 0;
            unsigned ms = 0;
            is >> tag >> ms;
            raw_wt_client_t::wt_stream_t* const s = by_tag(tag);
            const bool a =
                s != nullptr && raw_wt_client_t::wait_aborted(s, std::chrono::milliseconds(ms));
            reply(a ? "WAITABORT 1" : "WAITABORT 0");
        } else if (verb == "WAITSHUT") {
            unsigned ms = 0;
            is >> ms;
            const bool s = g_cli && g_cli->wait_shutdown(std::chrono::milliseconds(ms));
            reply(s ? "WAITSHUT 1" : "WAITSHUT 0");
        } else if (verb == "CLOSE") {
            std::size_t tag = 0;
            is >> tag;
            if (raw_wt_client_t::wt_stream_t* const s = by_tag(tag); s != nullptr && g_cli)
                g_cli->close_stream(s);
            reply("CLOSE ok");
        } else if (verb == "QUIT") {
            reply("QUIT ok");
            break;
        } else if (!verb.empty()) {
            reply("ERR unknown");
        }
    }
    // Tear the client down before returning: its destructor closes every handle and blocks
    // until msquic's callbacks drain, which is what makes the server see a clean departure.
    g_cli.reset();
    return 0;
}
