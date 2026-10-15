/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief #2114: the reference/06 self-describing record round-trips as ONE value.
 *
 * reference/06 §"Structured record with named fields" shows an IMU sample packed as named
 * `NAME`/`VALUE` children. The page used to build that record under `POINT` (`0x07`), which the
 * core reads as a branch write and refuses with `tr::schema::type_mismatch`; the record shape the
 * page's own table names is a user-range type code with `opt.PL=1`, which the graph stores
 * whole. The code from `struct imu_sample` to `publish` below is the page's example, the same
 * apart from comments, so a change that breaks the snippet breaks this test. `encode_imu` takes
 * the type code as a parameter only so the control below can reuse it.
 *
 * Three cases: the record writes, reads back byte-identical, and reaches a subscriber as one
 * delivery carrying the whole record; and, as the control that pins why the page changed, the
 * same children under `POINT` are still refused.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;

// ---- reference/06 §Structured record with named fields: the example, verbatim ----

struct imu_sample {
    std::uint64_t ts_ns;
    float accel_x, accel_y, accel_z;
    float gyro_x, gyro_y, gyro_z;
};

// The project's record type: a user-range code, documented for the project.
// 0x80 is BATCH (reference/05 §User range), so a project record starts at 0x81.
constexpr auto imu_record_t = static_cast<tr::wire::type_t>(0x81);

// TLV builder helpers.
tr::wire::tlv_t name_tlv(std::string_view s) {
    tr::wire::tlv_t t;
    t.type = tr::wire::type_t::NAME;
    t.payload = {reinterpret_cast<const std::byte*>(s.data()), s.size()};
    return t;
}
tr::wire::tlv_t value_tlv(std::span<const std::byte> b) {
    tr::wire::tlv_t t;
    t.type = tr::wire::type_t::VALUE;
    t.payload = b;
    return t;
}

/** @brief The page's record encoder, with the record's type code as a parameter. */
std::vector<std::byte> encode_imu(const imu_sample& s, tr::wire::type_t type) {
    auto bytes = [](const auto& x) {
        return std::span<const std::byte>{reinterpret_cast<const std::byte*>(&x), sizeof x};
    };

    // The record: a user-range type with opt.PL=1, self-describing NAME/VALUE children.
    tr::wire::tlv_t rec;
    rec.type = type;
    rec.opt = tr::wire::opt_t{.pl = true};
    rec.children = {
        name_tlv("ts_ns"),
        value_tlv(bytes(s.ts_ns)),
        name_tlv("accel"),
        value_tlv({reinterpret_cast<const std::byte*>(&s.accel_x), 3 * sizeof(float)}),
        name_tlv("gyro"),
        value_tlv({reinterpret_cast<const std::byte*>(&s.gyro_x), 3 * sizeof(float)}),
    };
    return tr::wire::encode(rec);
}

/** @brief Publish @p wire as one value at @p imu, as the page does. */
tr::graph::result_t<void> publish(tr::graph::graph_t& g, tr::graph::vertex_handle_t imu,
                                  const std::vector<std::byte>& wire) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(wire.size());
    std::memcpy(seg->bytes.data(), wire.data(), wire.size());
    return g.write(imu, tr::view::view_t::over(std::move(seg)));
}

/** @brief True when @p got holds exactly the bytes of @p want. */
bool same_bytes(std::span<const std::byte> got, const std::vector<std::byte>& want) {
    return got.size() == want.size() && std::memcmp(got.data(), want.data(), want.size()) == 0;
}

/** @brief Subscriber state: deliveries seen, and the bytes of the last one. */
struct seen_t {
    int count = 0;
    std::vector<std::byte> last;
};

}  // namespace

int main() {
    std::printf("reference/06 structured record (#2114):\n");
    tr::graph::graph_t g;
    const tr::graph::vertex_handle_t imu =
        g.register_vertex(tr::graph::path_t("/imu"), tr::graph::role_t::STORED_VALUE);

    seen_t seen;
    const auto on_imu = [](void* c, const tr::graph::value_t& v) {
        auto& s = *static_cast<seen_t*>(c);
        ++s.count;
        const std::span<const std::byte> b = v.only().bytes();
        s.last.assign(b.begin(), b.end());
    };
    const auto sub = g.subscribe(tr::graph::path_t("/imu"), on_imu, &seen);
    check(sub.has_value(), "a subscriber attaches to /imu");

    const imu_sample s{.ts_ns = 123456789ull,
                       .accel_x = 0.5f,
                       .accel_y = -1.0f,
                       .accel_z = 9.81f,
                       .gyro_x = 0.01f,
                       .gyro_y = 0.02f,
                       .gyro_z = 0.03f};
    const std::vector<std::byte> record = encode_imu(s, imu_record_t);

    check(publish(g, imu, record).has_value(), "the user-range PL=1 record writes");

    const auto back = g.read(imu);
    check(back.has_value() && same_bytes((*back)->only().bytes(), record),
          "it reads back byte-identical, as one value");

    check(seen.count == 1, "the subscriber gets exactly one delivery");
    check(same_bytes(seen.last, record), "and that delivery is the whole record");

    // The record is self-describing: a reader finds a field by NAME match.
    const auto node = tr::wire::tlv_node_t::over(record);
    bool found_gyro = false;
    if (node) {
        bool next_is_gyro = false;
        for (const auto& c : node->children()) {
            if (next_is_gyro) {
                found_gyro = c.type() == tr::wire::type_t::VALUE &&
                             c.payload().size() == 3 * sizeof(float) &&
                             std::memcmp(c.payload().data(), &s.gyro_x, 3 * sizeof(float)) == 0;
                break;
            }
            const std::span<const std::byte> n = c.payload();
            next_is_gyro = c.type() == tr::wire::type_t::NAME && n.size() == 4 &&
                           std::memcmp(n.data(), "gyro", 4) == 0;
        }
    }
    check(found_gyro, "the gyro field is found by NAME in the stored record");

    // CONTROL: the shape the page used to show. POINT is a branch write (reference/05 §0x07),
    // and NAME/VALUE pairs are not a branch-write node, so the core refuses it.
    const auto point = publish(g, imu, encode_imu(s, tr::wire::type_t::POINT));
    check(!point.has_value() && point.error() == tr::graph::status_t::TYPE_MISMATCH,
          "control: the same children under POINT answer TYPE_MISMATCH");
    check(seen.count == 1, "control: the refused POINT write delivers nothing");

    return tr::testing::summary("structured_record");
}
