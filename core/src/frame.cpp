// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

#include "libtracer/frame.hpp"

#include <algorithm>

#include "libtracer/crc.hpp"

namespace tracer {
namespace {

constexpr std::uint8_t u8(std::byte b) noexcept { return std::to_integer<std::uint8_t>(b); }

std::uint64_t read_le(std::span<const std::byte> b, std::size_t off, std::size_t n) noexcept {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        v |= static_cast<std::uint64_t>(u8(b[off + i])) << (8 * i);
    }
    return v;
}

void write_le(std::vector<std::byte>& out, std::uint64_t v, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
    }
}

struct Decoded {
    Tlv tlv;
    std::size_t consumed;
};

std::expected<Decoded, Error> decode_at(std::span<const std::byte> buf, std::size_t depth) {
    if (depth >= kMaxDepth) return std::unexpected(Error::TlvNestingTooDeep);
    if (buf.size() < 4) return std::unexpected(Error::FrameTruncated);

    const std::uint8_t type_b = u8(buf[0]);
    const std::uint8_t opt_b = u8(buf[1]);
    if (type_b == 0x00) return std::unexpected(Error::FrameInvalid);
    if (Opt::reserved_set(opt_b)) return std::unexpected(Error::FrameInvalid);

    const Opt opt = Opt::decode(opt_b);
    const std::size_t header = opt.ll ? 6u : 4u;
    if (buf.size() < header) return std::unexpected(Error::FrameTruncated);

    const std::uint64_t length = read_le(buf, 2, opt.ll ? 4u : 2u);
    const std::size_t ts_size = opt.ts ? (opt.tf ? 4u : 8u) : 0u;
    const std::size_t crc_size = opt.cr ? (opt.cw ? 2u : 4u) : 0u;
    const std::size_t total = header + length + ts_size + crc_size;
    if (buf.size() < total) return std::unexpected(Error::FrameTruncated);

    Tlv tlv;
    tlv.type = static_cast<Type>(type_b);
    tlv.opt = opt;
    const std::span<const std::byte> payload = buf.subspan(header, length);

    if (opt.ts || opt.cr) {
        Trailer trailer;
        if (opt.ts) {
            Timestamp t;
            t.relative = opt.tf;
            if (opt.tf) {
                t.value = static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(read_le(buf, header + length, 4)));
            } else {
                t.value = static_cast<std::int64_t>(read_le(buf, header + length, 8));
            }
            trailer.ts = t;
        }
        if (opt.cr) {
            const std::span<const std::byte> ts_bytes = buf.subspan(header + length, ts_size);
            const std::size_t crc_off = header + length + ts_size;

            std::vector<std::byte> covered;
            covered.reserve(payload.size() + ts_bytes.size());
            covered.insert(covered.end(), payload.begin(), payload.end());
            covered.insert(covered.end(), ts_bytes.begin(), ts_bytes.end());

            Crc c;
            if (opt.cw) {
                c.width = Crc::Width::Crc16Ccitt;
                c.value = static_cast<std::uint32_t>(read_le(buf, crc_off, 2));
                if (crc::crc16_ccitt(covered) != static_cast<std::uint16_t>(c.value)) {
                    return std::unexpected(Error::FrameCrcFail);
                }
            } else {
                c.width = Crc::Width::Crc32c;
                c.value = static_cast<std::uint32_t>(read_le(buf, crc_off, 4));
                if (crc::crc32c(covered) != c.value) {
                    return std::unexpected(Error::FrameCrcFail);
                }
            }
            trailer.crc = c;
        }
        tlv.trailer = trailer;
    }

    if (opt.pl) {
        std::size_t pos = 0;
        while (pos < payload.size()) {
            auto child = decode_at(payload.subspan(pos), depth + 1);
            if (!child) return std::unexpected(child.error());
            pos += child->consumed;
            tlv.children.push_back(std::move(child->tlv));
        }
        if (pos != payload.size()) return std::unexpected(Error::FrameInvalid);
    } else {
        tlv.payload = payload;
    }

    return Decoded{std::move(tlv), total};
}

}  // namespace

std::expected<Tlv, Error> decode(std::span<const std::byte> input) {
    auto r = decode_at(input, 0);
    if (!r) return std::unexpected(r.error());
    if (r->consumed != input.size()) return std::unexpected(Error::FrameInvalid);  // trailing bytes
    return std::move(r->tlv);
}

std::vector<std::byte> encode(const Tlv& tlv) {
    std::vector<std::byte> body;
    if (tlv.opt.pl) {
        for (const Tlv& child : tlv.children) {
            const std::vector<std::byte> cb = encode(child);
            body.insert(body.end(), cb.begin(), cb.end());
        }
    } else {
        body.assign(tlv.payload.begin(), tlv.payload.end());
    }

    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(tlv.type)));
    out.push_back(static_cast<std::byte>(tlv.opt.encode()));
    write_le(out, body.size(), tlv.opt.ll ? 4u : 2u);
    out.insert(out.end(), body.begin(), body.end());

    std::vector<std::byte> ts_bytes;
    if (tlv.opt.ts) {
        const Timestamp t = (tlv.trailer && tlv.trailer->ts)
                                ? *tlv.trailer->ts
                                : Timestamp{.relative = tlv.opt.tf, .value = 0};
        if (tlv.opt.tf) {
            write_le(ts_bytes, static_cast<std::uint32_t>(static_cast<std::int32_t>(t.value)), 4);
        } else {
            write_le(ts_bytes, static_cast<std::uint64_t>(t.value), 8);
        }
        out.insert(out.end(), ts_bytes.begin(), ts_bytes.end());
    }
    if (tlv.opt.cr) {
        std::vector<std::byte> covered;
        covered.reserve(body.size() + ts_bytes.size());
        covered.insert(covered.end(), body.begin(), body.end());
        covered.insert(covered.end(), ts_bytes.begin(), ts_bytes.end());
        if (tlv.opt.cw) {
            write_le(out, crc::crc16_ccitt(covered), 2);
        } else {
            write_le(out, crc::crc32c(covered), 4);
        }
    }
    return out;
}

bool equal(const Tlv& a, const Tlv& b) noexcept {
    if (a.type != b.type || a.opt != b.opt || a.trailer != b.trailer) return false;
    if (!std::ranges::equal(a.payload, b.payload)) return false;
    if (a.children.size() != b.children.size()) return false;
    for (std::size_t i = 0; i < a.children.size(); ++i) {
        if (!equal(a.children[i], b.children[i])) return false;
    }
    return true;
}

}  // namespace tracer
