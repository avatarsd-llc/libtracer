// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

//! libtracer core wire codec — a from-scratch, native Rust port of the C++
//! reference codec in `core/src/frame.cpp` and the TypeScript core in
//! `bindings/typescript/src/codec.mjs`. It must match both byte-for-byte;
//! correctness is gated by the SHARED conformance vectors under
//! `tests/conformance/vectors/v1/` and the cross-core differential fuzzer
//! (`tests/conformance/diff_fuzz.py`).
//!
//! The wire format is documented in `docs/reference/01-data-format.md` and pinned
//! normatively by `docs/spec/v1.md`:
//!
//!   header (4 or 6 bytes) + payload + optional trailer (timestamp then CRC).
//!
//! This is a third, fully independent implementation (no FFI over C++,
//! per ADR-0028) — it compiles `#![no_std]` (it only needs `alloc` for the owned
//! TLV tree) so it can target WASM and bare-metal MCUs.

#![no_std]
// Documentation coverage is a build gate, mirroring the C++ Doxygen WARN_AS_ERROR
// gate (core/Doxyfile / docs.yml): every public item MUST carry a rustdoc comment.
#![deny(missing_docs)]

extern crate alloc;

use alloc::vec::Vec;

// ---------------------------------------------------------------- typed tier ---
// The typed protocol layer built on top of the raw wire codec below. Each module
// produces / parses the exact bytes the shared conformance vectors pin, matching
// the C++ core and the TypeScript client byte-for-byte. See each module's docs.

pub mod error_registry;
pub mod field;
pub mod path;
pub mod tlv_builders;

pub use error_registry::{
    error_code, error_raw_code, error_string, parse_error, status_errors, status_ok,
    status_with_errors, Disposition, ErrCode, ErrorId, ParsedError, Severity,
};
pub use field::{
    encode_field, field_tlv, parse_field, parse_field_tlv, FieldLevel, FieldMode, MAX_FIELD_DEPTH,
};
pub use tlv_builders::{
    name, subscriber, validate_segment, value, value_opts, value_u16, value_u32, value_u64,
    value_u8, BuildError, ValueOptions, MAX_PATH_BYTES, MAX_SEGMENTS, MAX_SEGMENT_BYTES,
};

/// Core type-code registry (0x01-0x10). 0x05 is retired (was LIST, ADR-0003).
/// The codec treats every nonzero type generically, so these constants are a
/// convenience, not an exhaustive set.
pub mod type_code {
    /// Opaque scalar value.
    pub const VALUE: u8 = 0x01;
    /// UTF-8 name segment.
    pub const NAME: u8 = 0x02;
    /// Human-readable description.
    pub const DESCRIPTION: u8 = 0x03;
    /// Subscriber registration.
    pub const SUBSCRIBER: u8 = 0x04;
    /// Structured path (sequence of NAME children).
    pub const PATH: u8 = 0x06;
    /// Point in a path/graph.
    pub const POINT: u8 = 0x07;
    /// Error report.
    pub const ERROR: u8 = 0x08;
    /// Status report.
    pub const STATUS: u8 = 0x09;
    /// Access-control list.
    pub const ACL: u8 = 0x0a;
    /// QoS settings.
    pub const SETTINGS: u8 = 0x0b;
    /// Time.
    pub const TIME: u8 = 0x0c;
    /// Router-wrapped frame.
    pub const ROUTER: u8 = 0x0d;
    /// In-band vertex-creation spec (structured; opt.PL=1).
    pub const SPEC: u8 = 0x0e;
    /// Remote-operation forward frame (structured; RFC-0004 §B / ADR-0035).
    pub const FWD: u8 = 0x0f;
    /// Control-plane `:field` selector (structured; RFC-0004 §C / ADR-0035).
    pub const FIELD: u8 = 0x10;
}

/// The iterative-parser depth cap (docs/reference/01 §iterative parsing). A frame
/// nested deeper than this is rejected rather than overflowing the call stack.
pub const MAX_DEPTH: usize = 32;

/// Reserved bits 7 and 0; a set reserved bit makes a frame invalid.
const RESERVED_MASK: u8 = 0b1000_0001;

/// A decode failure. Names mirror the C++ `tr::wire::error_t` enum and the TS
/// `ERROR` codes so the conformance harness can emit identical `ERR:<reason>` text.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    /// Ran out of bytes (header/payload/trailer truncated).
    FrameTruncated,
    /// Reserved bit set, type code 0x00, or trailing bytes after the root TLV.
    FrameInvalid,
    /// Trailer CRC did not match the recomputed value.
    FrameCrcFail,
    /// Nesting exceeded [`MAX_DEPTH`].
    TlvNestingTooDeep,
}

impl Error {
    /// The stable wire name used by the conformance harness (`ERR:<name>`).
    pub const fn name(self) -> &'static str {
        match self {
            Error::FrameTruncated => "FRAME_TRUNCATED",
            Error::FrameInvalid => "FRAME_INVALID",
            Error::FrameCrcFail => "FRAME_CRC_FAIL",
            Error::TlvNestingTooDeep => "TLV_NESTING_TOO_DEEP",
        }
    }
}

/// The 1-byte `opt` field, unpacked. Bits MSB->LSB: R | PL | TS | CR | LL | CW | TF | R.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Opt {
    /// bit 6: payload is structured (children, not opaque bytes).
    pub pl: bool,
    /// bit 5: trailer carries a timestamp.
    pub ts: bool,
    /// bit 4: trailer carries a CRC.
    pub cr: bool,
    /// bit 3: length width (false = u16, true = u32).
    pub ll: bool,
    /// bit 2: CRC width (false = CRC-32C, true = CRC-16-CCITT).
    pub cw: bool,
    /// bit 1: timestamp form (false = absolute u64, true = relative i32).
    pub tf: bool,
}

impl Opt {
    /// Unpack the raw `opt` byte (ignores reserved bits, which are checked separately).
    pub const fn decode(b: u8) -> Opt {
        Opt {
            pl: b & 0x40 != 0,
            ts: b & 0x20 != 0,
            cr: b & 0x10 != 0,
            ll: b & 0x08 != 0,
            cw: b & 0x04 != 0,
            tf: b & 0x02 != 0,
        }
    }

    /// Pack back into the raw `opt` byte (reserved bits always zero).
    pub const fn encode(self) -> u8 {
        (if self.pl { 0x40 } else { 0 })
            | (if self.ts { 0x20 } else { 0 })
            | (if self.cr { 0x10 } else { 0 })
            | (if self.ll { 0x08 } else { 0 })
            | (if self.cw { 0x04 } else { 0 })
            | (if self.tf { 0x02 } else { 0 })
    }
}

/// A decoded wire timestamp. Absolute form is a u64 ns count since the Unix epoch;
/// relative form is a signed i32 ns offset. `value` holds the bit pattern as an
/// `i64` (absolute = the u64 reinterpreted) to round-trip the full range exactly.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Timestamp {
    /// false = absolute u64 ns; true = relative i32 ns.
    pub relative: bool,
    /// ns; for the absolute form this is the u64 value reinterpreted as i64.
    pub value: i64,
}

/// The trailer CRC width.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CrcWidth {
    /// CRC-32C (Castagnoli).
    Crc32c,
    /// CRC-16-CCITT (FALSE).
    Crc16Ccitt,
}

/// A decoded CRC. 16-bit values are zero-extended into `value`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Crc {
    /// Which CRC the trailer carried.
    pub width: CrcWidth,
    /// The CRC value (16-bit values zero-extended).
    pub value: u32,
}

/// A decoded trailer: an optional timestamp followed by an optional CRC.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Trailer {
    /// Present iff opt.TS.
    pub ts: Option<Timestamp>,
    /// Present iff opt.CR.
    pub crc: Option<Crc>,
}

/// A decoded TLV. For opaque TLVs (opt.PL=0) `payload` holds the bytes and
/// `children` is empty; for structured TLVs (opt.PL=1) `children` holds the parsed
/// sub-TLVs and `payload` is empty.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Tlv {
    /// A [`type_code`] value (or a user/unknown nonzero code).
    pub type_code: u8,
    /// The unpacked option bits.
    pub opt: Opt,
    /// Opaque payload bytes (empty when `opt.pl`).
    pub payload: Vec<u8>,
    /// Parsed sub-TLVs (empty unless `opt.pl`).
    pub children: Vec<Tlv>,
    /// Optional trailer (present iff `opt.ts || opt.cr`).
    pub trailer: Option<Trailer>,
}

/* ------------------------------------------------------------------ crc --- */

/// CRC-32C (Castagnoli): reflected poly 0x82F63B78, init/xor 0xFFFFFFFF.
pub fn crc32c(data: &[u8]) -> u32 {
    let mut c: u32 = 0xFFFF_FFFF;
    for &b in data {
        let mut x = (c ^ b as u32) & 0xFF;
        for _ in 0..8 {
            x = if x & 1 != 0 {
                0x82F6_3B78 ^ (x >> 1)
            } else {
                x >> 1
            };
        }
        c = x ^ (c >> 8);
    }
    c ^ 0xFFFF_FFFF
}

/// CRC-16-CCITT (FALSE): poly 0x1021, MSB-first, init 0xFFFF, no final xor.
pub fn crc16_ccitt(data: &[u8]) -> u16 {
    let mut c: u16 = 0xFFFF;
    for &b in data {
        let idx = (((c >> 8) as u8) ^ b) as u16;
        let mut t = idx << 8;
        for _ in 0..8 {
            t = if t & 0x8000 != 0 {
                (t << 1) ^ 0x1021
            } else {
                t << 1
            };
        }
        c = t ^ (c << 8);
    }
    c
}

/* --------------------------------------------------------------- endian --- */

/// Read `n` (<= 8) little-endian bytes at `off` as an unsigned `u64`.
fn read_le(b: &[u8], off: usize, n: usize) -> u64 {
    let mut v: u64 = 0;
    let mut i = 0;
    while i < n {
        v |= (b[off + i] as u64) << (8 * i);
        i += 1;
    }
    v
}

/// Append `n` (<= 8) little-endian bytes of unsigned `v`.
fn write_le(out: &mut Vec<u8>, v: u64, n: usize) {
    let mut i = 0;
    while i < n {
        out.push(((v >> (8 * i)) & 0xFF) as u8);
        i += 1;
    }
}

/* --------------------------------------------------------------- decode --- */

/// One TLV parsed in isolation — NO recursion into children. Mirrors `parse_one`
/// in `frame.cpp`. For a structured TLV (`opt.pl`) the returned slice is the PL
/// payload region, left for [`decode`] to walk iteratively; for an opaque TLV the
/// bytes are copied into `tlv.payload` and the slice is empty. The returned
/// `usize` is the full encoded size (header + length + trailer).
fn parse_one(buf: &[u8]) -> Result<(Tlv, usize, &[u8]), Error> {
    if buf.len() < 4 {
        return Err(Error::FrameTruncated);
    }
    let type_b = buf[0];
    let opt_b = buf[1];
    if type_b == 0x00 {
        return Err(Error::FrameInvalid);
    }
    if opt_b & RESERVED_MASK != 0 {
        return Err(Error::FrameInvalid);
    }

    let opt = Opt::decode(opt_b);
    let header = if opt.ll { 6 } else { 4 };
    if buf.len() < header {
        return Err(Error::FrameTruncated);
    }

    let length = read_le(buf, 2, if opt.ll { 4 } else { 2 });
    let ts_size = if opt.ts {
        if opt.tf {
            4u64
        } else {
            8u64
        }
    } else {
        0
    };
    let crc_size = if opt.cr {
        if opt.cw {
            2u64
        } else {
            4u64
        }
    } else {
        0
    };
    // u64 math: `length` is at most a u32, so this sum can never overflow u64 and
    // an over-long length on a short buffer fails the bounds check below rather
    // than wrapping into an out-of-bounds slice.
    let total = header as u64 + length + ts_size + crc_size;
    if (buf.len() as u64) < total {
        return Err(Error::FrameTruncated);
    }
    let length = length as usize;
    let ts_size = ts_size as usize;
    let total = total as usize;

    let mut tlv = Tlv {
        type_code: type_b,
        opt,
        ..Tlv::default()
    };

    let payload = &buf[header..header + length];

    if opt.ts || opt.cr {
        let mut trailer = Trailer::default();
        if opt.ts {
            let value = if opt.tf {
                (read_le(buf, header + length, 4) as u32) as i32 as i64
            } else {
                read_le(buf, header + length, 8) as i64
            };
            trailer.ts = Some(Timestamp {
                relative: opt.tf,
                value,
            });
        }
        if opt.cr {
            // The CRC covers the payload bytes followed by the timestamp bytes
            // (NOT the header); they are contiguous in `buf`.
            let covered = &buf[header..header + length + ts_size];
            let crc_off = header + length + ts_size;
            if opt.cw {
                let value = read_le(buf, crc_off, 2) as u16;
                if crc16_ccitt(covered) != value {
                    return Err(Error::FrameCrcFail);
                }
                trailer.crc = Some(Crc {
                    width: CrcWidth::Crc16Ccitt,
                    value: value as u32,
                });
            } else {
                let value = read_le(buf, crc_off, 4) as u32;
                if crc32c(covered) != value {
                    return Err(Error::FrameCrcFail);
                }
                trailer.crc = Some(Crc {
                    width: CrcWidth::Crc32c,
                    value,
                });
            }
        }
        tlv.trailer = Some(trailer);
    }

    let children: &[u8] = if opt.pl {
        payload // walked iteratively by decode(), not here
    } else {
        tlv.payload.extend_from_slice(payload);
        &[]
    };
    Ok((tlv, total, children))
}

/// An open structured node whose children are still being parsed. `pos` is the
/// cursor within `payload`; `total` advances the parent's cursor when it closes.
struct Open<'a> {
    node: Tlv,
    payload: &'a [u8],
    pos: usize,
    total: usize,
}

/// Decode exactly one TLV that fills `input` into a TLV tree. Nesting is parsed
/// ITERATIVELY with an explicit stack (recursion is forbidden so a maliciously
/// deep frame cannot overflow a small MCU call stack) and capped at [`MAX_DEPTH`].
/// Trailing bytes after the root make the frame [`Error::FrameInvalid`].
pub fn decode(input: &[u8]) -> Result<Tlv, Error> {
    let (root_node, root_total, root_children) = parse_one(input)?;
    if root_total != input.len() {
        return Err(Error::FrameInvalid); // trailing bytes
    }
    if !root_node.opt.pl {
        return Ok(root_node); // opaque root: done
    }

    let mut stack: Vec<Open> = Vec::new();
    stack.push(Open {
        node: root_node,
        payload: root_children,
        pos: 0,
        total: root_total,
    });

    loop {
        let top = stack.last().unwrap();
        if top.pos == top.payload.len() {
            // Node complete — pop and graft onto its parent (or return if root).
            let done = stack.pop().unwrap();
            match stack.last_mut() {
                None => return Ok(done.node),
                Some(parent) => {
                    parent.pos += done.total;
                    parent.node.children.push(done.node);
                }
            }
            continue;
        }
        // A child of `top` sits at depth == stack.len(); reject at the cap.
        if stack.len() >= MAX_DEPTH {
            return Err(Error::TlvNestingTooDeep);
        }
        // Copy out the slice/cursor before mutating the stack (the returned refs
        // borrow `input`, not the stack).
        let (payload, pos) = {
            let top = stack.last().unwrap();
            (top.payload, top.pos)
        };
        let (child_node, child_total, child_children) = parse_one(&payload[pos..])?;
        if child_node.opt.pl {
            stack.push(Open {
                node: child_node,
                payload: child_children,
                pos: 0,
                total: child_total,
            });
        } else {
            let parent = stack.last_mut().unwrap();
            parent.pos += child_total;
            parent.node.children.push(child_node);
        }
    }
}

/* --------------------------------------------------------------- encode --- */

/// Encode a TLV tree back to wire bytes, recomputing the trailer CRC from the body
/// (+ timestamp bytes) when `opt.cr` is set. Mirrors `encode` in `frame.cpp`.
pub fn encode(tlv: &Tlv) -> Vec<u8> {
    let mut body: Vec<u8> = Vec::new();
    if tlv.opt.pl {
        for child in &tlv.children {
            body.extend_from_slice(&encode(child));
        }
    } else {
        body.extend_from_slice(&tlv.payload);
    }

    let mut out: Vec<u8> = Vec::new();
    out.push(tlv.type_code);
    out.push(tlv.opt.encode());
    write_le(&mut out, body.len() as u64, if tlv.opt.ll { 4 } else { 2 });
    out.extend_from_slice(&body);

    let mut ts_bytes: Vec<u8> = Vec::new();
    if tlv.opt.ts {
        let t = match &tlv.trailer {
            Some(tr) if tr.ts.is_some() => tr.ts.unwrap(),
            _ => Timestamp {
                relative: tlv.opt.tf,
                value: 0,
            },
        };
        if tlv.opt.tf {
            write_le(&mut ts_bytes, (t.value as i32 as u32) as u64, 4);
        } else {
            write_le(&mut ts_bytes, t.value as u64, 8);
        }
        out.extend_from_slice(&ts_bytes);
    }
    if tlv.opt.cr {
        let mut covered = body;
        covered.extend_from_slice(&ts_bytes);
        if tlv.opt.cw {
            write_le(&mut out, crc16_ccitt(&covered) as u64, 2);
        } else {
            write_le(&mut out, crc32c(&covered) as u64, 4);
        }
    }
    out
}
