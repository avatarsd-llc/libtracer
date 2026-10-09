/**
 * @file
 * @brief The Noise link harness's ZENOH arms (#2065): a Zenoh session pair over plain UDP, and
 *        over Zenoh's secured links, TLS (over TCP) and QUIC, with the same rows as
 *        `bench_noise_link`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Every row comes from `link_harness.hpp`, the same driver the libtracer arms run through, so
 * the two engines are measured by one piece of code: one warm session pair per arm (the
 * server session listens, the client session connects, peer mode, multicast scouting off, as
 * `bench_zenoh_net` configures it), request/reply and streaming at 64 B, 1 KiB, 4 KiB, 16 KiB
 * and 65507 B, session setup on its own row, and the RAM the first pair costs the process. A
 * request is a `put` on `bench/link/req` from the client's declared publisher; the server's
 * subscriber echoes it with a `put` on `bench/link/rep` from its own declared publisher.
 *
 *  - **`udp`** — plain Zenoh over UDP, the counterpart of libtracer's plain `udp` arm.
 *  - **`tls`** / **`quic`** — Zenoh's secured links, the counterpart of libtracer's Noise arm.
 *    Zenoh has no Noise link: these are DIFFERENT SECURITY PROTOCOLS WITH THE SAME GOAL,
 *    AUTHENTICATED ENCRYPTION. TLS 1.3 authenticates the server with a certificate and runs over
 *    a reliable byte stream; the Noise binding authenticates both ends with a PSK over
 *    datagrams. Their `-setup` rows are the handshake cost and their `-rr`/`-stream` rows the
 *    per-frame cost, kept apart for exactly that reason. The run makes a throwaway CA and a
 *    server certificate (EC P-256, 127.0.0.1) with the `openssl` command at start-up and deletes
 *    them at exit; without the command, both arms print why and are skipped.
 *
 * Every `put` uses Zenoh's defaults (congestion control included); the streaming row's `NOTE`
 * line reports what was lost. Built only when zenoh-c is vendored (`bench/fetch_zenoh.sh`).
 * Diagnostic, not a gate.
 *
 *     bench_zenoh_link                 # every arm
 *     bench_zenoh_link --arm=tls       # one arm
 *     bench_zenoh_link --quick         # a fifth of the time budget (a smoke run)
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "bench_common.hpp"
#include "link_harness.hpp"
#include "zenoh.hxx"

namespace {

using bench::link::link_pair_t;
using bench::link::rx_state_t;

/**
 * @brief A port nothing on loopback holds right now, for the next pair's listener: the kernel
 *        picks one for a probe socket, which is closed again. Every pair takes a fresh port,
 *        so no setup waits on one a torn-down pair left in TIME_WAIT.
 */
[[nodiscard]] std::uint16_t free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(a);
    std::uint16_t port = 0;
    if (fd >= 0 && ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 &&
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) == 0)
        port = ntohs(a.sin_port);
    if (fd >= 0) ::close(fd);
    return port;
}

/** @brief Where the throwaway TLS material lives; empty if it could not be made. */
std::string g_tls_dir;

/**
 * @brief Make a CA and a CA-signed server certificate for 127.0.0.1 with the `openssl` command.
 *        rustls refuses a self-signed CA certificate as the server's own, hence two.
 * @return The directory, or empty when the command is missing or failed.
 */
[[nodiscard]] std::string make_tls_material() {
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) /
                     ("bench_zenoh_link_tls_" + std::to_string(getpid()));
    std::filesystem::create_directories(dir, ec);
    if (ec) return {};
    const std::string d = dir.string();
    const std::string cmd =
        "cd '" + d +
        "' && openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes "
        "-keyout ca.key -out ca.pem -days 2 -subj /CN=bench-ca >/dev/null 2>&1 && "
        "openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout key.pem "
        "-out srv.csr -subj /CN=localhost >/dev/null 2>&1 && "
        "printf 'subjectAltName=IP:127.0.0.1,DNS:localhost\\nbasicConstraints=CA:FALSE\\n' > "
        "ext.cnf && openssl x509 -req -in srv.csr -CA ca.pem -CAkey ca.key -CAcreateserial "
        "-out cert.pem -days 2 -extfile ext.cnf >/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0) {
        std::filesystem::remove_all(dir, ec);
        return {};
    }
    return d;
}

/** @brief One Zenoh session's config: listen or connect on @p proto at @p port. */
[[nodiscard]] zenoh::Config make_config(const char* proto, bool listen, std::uint16_t port) {
    zenoh::Config c = zenoh::Config::create_default();
    c.insert_json5(listen ? "listen/endpoints" : "connect/endpoints",
                   "[\"" + std::string(proto) + "/127.0.0.1:" + std::to_string(port) + "\"]");
    c.insert_json5("mode", "\"peer\"");
    c.insert_json5("scouting/multicast/enabled", "false");
    if (std::string_view(proto) != "udp") {
        c.insert_json5("transport/link/tls/root_ca_certificate", "\"" + g_tls_dir + "/ca.pem\"");
        if (listen) {
            c.insert_json5("transport/link/tls/listen_private_key",
                           "\"" + g_tls_dir + "/key.pem\"");
            c.insert_json5("transport/link/tls/listen_certificate",
                           "\"" + g_tls_dir + "/cert.pem\"");
        }
    }
    return c;
}

/** @brief A Zenoh session pair wired to the harness: the client puts requests, the server
 *         echoes them. Members are declared so the subscribers die before their sessions. */
class zenoh_pair_t final : public link_pair_t {
   public:
    /** @brief Open both sessions over @p proto and declare the four entities. */
    zenoh_pair_t(rx_state_t& rx, const char* proto) {
        const std::uint16_t port = free_port();
        server_.emplace(zenoh::Session::open(make_config(proto, true, port)));
        rep_.emplace(server_->declare_publisher(zenoh::KeyExpr("bench/link/rep")));
        req_sub_.emplace(server_->declare_subscriber(
            zenoh::KeyExpr("bench/link/req"),
            [this, &rx](const zenoh::Sample& s) {
                auto v = s.get_payload().as_vector();
                if (rx.on_server(std::as_bytes(std::span(v))))
                    rep_->put(zenoh::Bytes(std::move(v)));
            },
            zenoh::closures::none));
        client_.emplace(zenoh::Session::open(make_config(proto, false, port)));
        req_.emplace(client_->declare_publisher(zenoh::KeyExpr("bench/link/req")));
        rep_sub_.emplace(client_->declare_subscriber(
            zenoh::KeyExpr("bench/link/rep"),
            [&rx](const zenoh::Sample& s) {
                const auto v = s.get_payload().as_vector();
                rx.on_client(std::as_bytes(std::span(v)));
            },
            zenoh::closures::none));
    }

    ~zenoh_pair_t() override {
        // The client end first, so the server never echoes into a closed session.
        rep_sub_.reset();
        req_.reset();
        client_.reset();
        req_sub_.reset();
        rep_.reset();
        server_.reset();
    }

    void send(std::span<const std::byte> f) override {
        const auto* b = reinterpret_cast<const std::uint8_t*>(f.data());
        req_->put(zenoh::Bytes(std::vector<std::uint8_t>(b, b + f.size())));
    }

   private:
    std::optional<zenoh::Session> server_;
    std::optional<zenoh::Session> client_;
    std::optional<zenoh::Publisher> rep_;
    std::optional<zenoh::Publisher> req_;
    std::optional<zenoh::Subscriber<void>> req_sub_;
    std::optional<zenoh::Subscriber<void>> rep_sub_;
};

/** @brief A pair factory for @p proto; null (and a reason on stderr) if it did not open. */
template <const char* Proto>
[[nodiscard]] std::unique_ptr<link_pair_t> make_pair(rx_state_t& rx) {
    try {
        return std::make_unique<zenoh_pair_t>(rx, Proto);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "zenoh %s pair: %s\n", Proto, e.what());
        return nullptr;
    }
}

constexpr char kUdp[] = "udp";
constexpr char kTls[] = "tls";
constexpr char kQuic[] = "quic";

}  // namespace

int main(int argc, char** argv) {
    bench::link::options_t o;
    if (!bench::link::parse_options(argc, argv, o)) return 2;
    zenoh::init_log_from_env_or("error");
    std::printf("# bench_zenoh_link: zenoh link arms (#2065), one warm session pair per arm\n");
    std::printf(
        "# secured arms (tls, quic) vs libtracer noise: different security protocols, same goal: "
        "authenticated encryption\n");
    bench::emit_clock_floor();
    g_tls_dir = make_tls_material();
    std::vector<bench::link::arm_t> arms = {
        {"zenoh", "udp", bench::link::kDatagramBound, make_pair<kUdp>}};
    if (g_tls_dir.empty()) {
        std::printf(
            "# arms zenoh/tls, zenoh/quic: skipped (no openssl command for the "
            "throwaway certificate)\n");
    } else {
        arms.push_back({"zenoh", "tls", bench::link::kDatagramBound, make_pair<kTls>});
        arms.push_back({"zenoh", "quic", bench::link::kDatagramBound, make_pair<kQuic>});
    }
    bool ok = true;
    for (const auto& a : arms) ok &= bench::link::run_arm(a, o);
    if (!g_tls_dir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(g_tls_dir, ec);
    }
    return ok ? 0 : 1;
}
