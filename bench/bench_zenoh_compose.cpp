/**
 * @file
 * @brief The Zenoh arm of the composition-throughput comparison — TWO PROCESSES over real
 *        loopback UDP, K puts per K values, deliveries counted by the SUBSCRIBER.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Zenoh has no composite send, so the same K values that libtracer ships in one datagram are
 * K separate `put`s here. That is the comparison, and it is only fair if this side is a REAL
 * publisher talking to a REAL subscriber in another process: the subscriber listens on a
 * configured UDP endpoint, the publisher connects to it, and multicast scouting is disabled so
 * the pair talks over that socket and nothing else. The withdrawn comparison's Zenoh side had
 * no peer at all and never reached the wire — run_compose.sh's syscall audit exists so that
 * cannot recur silently.
 *
 * Same record framing, same subscriber-side counter and same RESULT rows as the libtracer arm
 * (bench_compose.hpp), so "a value" means the same thing on both sides.
 *
 *   bench_zenoh_compose sub <port> <K> <value_bytes> <window_ms> <run_id> <min_msgs> [audit]
 *   bench_zenoh_compose pub <port> <K> <value_bytes> <groups> <latency_groups> <run_id>
 *
 * `run_id` is the driver's per-point nonce and both roles require it; `min_msgs` is the fewest
 * throughput datagrams the subscriber will publish a rate from (bench_compose.hpp).
 */
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "bench_compose.hpp"
#include "zenoh.hxx"

using namespace std::chrono_literals;
using bench::compose::phase_t;
using zenoh::Bytes;
using zenoh::Config;
using zenoh::KeyExpr;
using zenoh::Sample;
using zenoh::Session;

namespace {

/** @brief Peer-mode config pinned to ONE loopback UDP endpoint, scouting off. */
Config make_config(bool listen, std::uint16_t port) {
    Config c = Config::create_default();
    const std::string ep = "[\"udp/127.0.0.1:" + std::to_string(port) + "\"]";
    c.insert_json5(listen ? "listen/endpoints" : "connect/endpoints", ep);
    c.insert_json5("mode", "\"peer\"");
    c.insert_json5("scouting/multicast/enabled", "false");
    return c;
}

/** @brief The delivery-COUNTING subscriber process. */
int run_sub(int argc, char** argv) {
    const auto port = static_cast<std::uint16_t>(std::strtoul(argv[2], nullptr, 10));
    const auto width = static_cast<std::size_t>(std::strtoul(argv[3], nullptr, 10));
    const auto value_bytes = static_cast<std::size_t>(std::strtoul(argv[4], nullptr, 10));
    const auto window_ms = static_cast<std::uint64_t>(std::strtoull(argv[5], nullptr, 10));
    const auto run_id = static_cast<std::uint32_t>(std::strtoul(argv[6], nullptr, 10));
    const auto min_msgs = static_cast<std::uint64_t>(std::strtoull(argv[7], nullptr, 10));
    const bool audit = argc > 8 && std::string_view(argv[8]) == "audit";
    if (!bench::compose::value_bytes_ok(value_bytes)) return 2;

    bench::compose::sub_counter_t counter("zenoh", "compose-udp", width, value_bytes, run_id,
                                          min_msgs);
    {
        Session session = Session::open(make_config(true, port));
        {
            auto sub = session.declare_subscriber(
                KeyExpr("bench/compose"),
                [&](const Sample& s) {
                    const std::vector<std::uint8_t> v = s.get_payload().as_vector();
                    (void)counter.on_message(std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(v.data()), v.size()));
                },
                zenoh::closures::none);

            std::printf("SUB_READY\n");
            std::fflush(stdout);

            const auto deadline = bench::Clock::now() + std::chrono::milliseconds(window_ms);
            while (!counter.done() && bench::Clock::now() < deadline)
                std::this_thread::sleep_for(2ms);
            // A DRAIN for a straggling datagram, not a barrier — see below.
            std::this_thread::sleep_for(50ms);
        }
        // Teardown, THEN read. The subscriber is undeclared by its destructor and the session
        // is closed explicitly; zenoh-c documents z_close as dropping the closure callbacks of
        // entities not already undeclared. Reading the counter while the callback could still
        // run would be a data race no sleep length fixes. The libtracer arm gets a stronger
        // guarantee for the same shape — ~udp_transport_t JOINS its recv thread — and this arm
        // rests on zenoh's undeclare/close contract, which is the strongest ordering its API
        // offers here.
        session.close();
    }
    return audit ? counter.finish_audit() : counter.finish();
}

/**
 * @brief Wrap @p buf as a payload WITHOUT copying it — the fairness fix, and why it is needed.
 *
 * `Bytes(const std::vector<std::uint8_t>&)` (bytes.hxx) selects `z_bytes_copy_from_buf`, whose
 * own documentation reads "converts a data from buffer into `z_owned_bytes_t` **by copying**".
 * Used inside the blast loop that is one staging copy of every payload byte PER PUT, charged to
 * this arm and to no other: libtracer's arm hands the transport `std::span`s and its `sendmsg`
 * gathers them in place, so the copy is harness overhead on one side of a comparison, not an
 * engine difference. `#568` is open because the withdrawn bench flattered libtracer; a harness
 * that quietly taxes the other arm is the same defect with the sign reversed.
 *
 * `z_bytes_from_buf` is the aliasing twin — "`this_` will take ownership of the buffer …
 * [deleter] can be NULL if `data` is located in static memory and does not require a drop"
 * (zenoh_commons.h) — so with a NULL deleter it takes the buffer as it stands and owes it no
 * drop. That the two entry points differ exactly in copying is the vendored API's own
 * documented contract; this harness relies on it, it did not measure it. The C entry point is
 * used rather than the C++ `Bytes(uint8_t*, size_t, Deleter)` overload because that overload
 * has no NULL-deleter form: it always heap-allocates a `Droppable` to hold the deleter
 * (detail/closures.hxx `into_context`).
 *
 * @warning LIFETIME. The buffer must outlive every payload built over it AND the session that
 * may still reference it. The caller therefore declares its staging buffers BEFORE the
 * `Session`, so the session is destroyed first. Constructing an empty `Bytes` and overwriting
 * it is the same delegation the vendored `Bytes` constructors use (`Bytes(...) : Bytes() { ...
 * }`), not a trick of ours.
 *
 * This does NOT make the loop allocation-free — see bench_compose.hpp @ref compose_alloc. It
 * removes the HARNESS's copy; whatever zenoh allocates internally per put is zenoh's own cost
 * and this harness makes no claim about it.
 */
Bytes alias_bytes(std::vector<std::uint8_t>& buf) {
    Bytes b;
    ::z_bytes_from_buf(zenoh::interop::as_owned_c_ptr(b), buf.data(), buf.size(), nullptr, nullptr);
    return b;
}

/** @brief The publisher process — K puts per group, because there is no composite send. */
int run_pub(int argc, char** argv) {
    (void)argc;
    const auto port = static_cast<std::uint16_t>(std::strtoul(argv[2], nullptr, 10));
    const auto width = static_cast<std::size_t>(std::strtoul(argv[3], nullptr, 10));
    const auto value_bytes = static_cast<std::size_t>(std::strtoul(argv[4], nullptr, 10));
    const auto groups = static_cast<std::uint64_t>(std::strtoull(argv[5], nullptr, 10));
    const auto lat_groups = static_cast<std::uint64_t>(std::strtoull(argv[6], nullptr, 10));
    const auto run_id = static_cast<std::uint32_t>(std::strtoul(argv[7], nullptr, 10));
    if (!bench::compose::value_bytes_ok(value_bytes)) return 2;

    // Built once, exactly as on the libtracer arm — the records and their `uint8_t` staging
    // buffers both. DECLARED BEFORE THE SESSION so they outlive it: the payloads below alias
    // these buffers instead of copying them (see alias_bytes), and destruction runs in reverse.
    const auto stage = [](const std::vector<std::vector<std::byte>>& recs) {
        std::vector<std::vector<std::uint8_t>> out;
        out.reserve(recs.size());
        for (const std::vector<std::byte>& r : recs)
            out.emplace_back(reinterpret_cast<const std::uint8_t*>(r.data()),
                             reinterpret_cast<const std::uint8_t*>(r.data()) + r.size());
        return out;
    };
    std::vector<std::vector<std::uint8_t>> lat_u8 =
        stage(bench::compose::make_group(width, value_bytes, phase_t::LATENCY, run_id));
    std::vector<std::vector<std::uint8_t>> thru_u8 =
        stage(bench::compose::make_group(width, value_bytes, phase_t::THROUGHPUT, run_id));
    std::vector<std::vector<std::uint8_t>> eop_u8 =
        stage(bench::compose::make_group(width, value_bytes, phase_t::END_OF_POINT, run_id));

    std::uint64_t a = 0, b = 0;
    Session session = Session::open(make_config(false, port));
    {
        auto pub = session.declare_publisher(KeyExpr("bench/compose"));
        std::this_thread::sleep_for(700ms);  // let the UDP session establish

        for (std::uint64_t i = 0; i < lat_groups; ++i) {
            bench::compose::put_le(std::as_writable_bytes(std::span<std::uint8_t>(lat_u8[0])),
                                   bench::compose::kOffTs, bench::now_ns(), 8);
            for (std::size_t j = 0; j < width; ++j) pub.put(alias_bytes(lat_u8[j]));
            const auto until =
                bench::Clock::now() + std::chrono::nanoseconds(bench::compose::kPaceNs);
            while (bench::Clock::now() < until) {
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(bench::compose::kDrainMs));

        a = bench::now_ns();
        for (std::uint64_t i = 0; i < groups; ++i) {
            for (std::size_t j = 0; j < width; ++j) pub.put(alias_bytes(thru_u8[j]));
            if ((i & 63) == 0) std::this_thread::yield();
        }
        b = bench::now_ns();

        std::this_thread::sleep_for(200ms);
        for (int i = 0; i < 8; ++i) {
            pub.put(alias_bytes(eop_u8[0]));
            std::this_thread::sleep_for(20ms);
        }
    }
    // Close while the staging buffers are still alive. The payloads ALIAS them (alias_bytes),
    // so the session must be finished with them before they go. Declaration order already gives
    // that — the session is declared after the buffers and so destroyed before them — and this
    // makes it explicit instead of resting on when the last session reference happens to drop.
    session.close();

    const double span = static_cast<double>(b - a) / 1e9;
    std::printf("PUB_SENT\tzenoh\t%zu\t%llu\t%llu\t%.0f\n", width,
                static_cast<unsigned long long>(groups),
                static_cast<unsigned long long>(groups * width),
                span > 0 ? static_cast<double>(groups) / span : 0.0);
    std::fflush(stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    zenoh::init_log_from_env_or("error");
    if (argc >= 8 && std::string_view(argv[1]) == "sub") return run_sub(argc, argv);
    if (argc >= 8 && std::string_view(argv[1]) == "pub") return run_pub(argc, argv);
    std::fprintf(stderr,
                 "usage: bench_zenoh_compose sub <port> <K> <value_bytes> <window_ms> <run_id> "
                 "<min_msgs> [audit]\n"
                 "       bench_zenoh_compose pub <port> <K> <value_bytes> <groups> "
                 "<latency_groups> <run_id>\n");
    return 2;
}
