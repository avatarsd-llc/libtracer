// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/*!
 * @brief Typed TLV builders and child-lookup accessors (Unit 1).
 *
 * This is the base of the typed protocol tier that sits on top of the raw wire
 * codec in [`crate`]. It produces the EXACT wire bytes the shared conformance
 * vectors pin (`tests/conformance/vectors/v1/`) — nothing here invents wire
 * structure; every builder returns a [`Tlv`] node that [`crate::encode`] renders
 * to bytes byte-for-byte with the C++ core (`core/`) and the TypeScript client
 * (`bindings/typescript`). See `docs/reference/05-protocol-tlvs.md`.
 */

use alloc::vec::Vec;

use crate::{type_code, Opt, Timestamp, Tlv, Trailer};

/**
 * @brief Maximum bytes in a single path/name segment (reference/03 §path syntax,
 * `core` `kMaxSegmentBytes`).
 */
pub const MAX_SEGMENT_BYTES: usize = 64;
/** @brief Maximum number of segments in a path (`core` `kMaxSegments`). */
pub const MAX_SEGMENTS: usize = 32;
/**
 * @brief Maximum total encoded NAME-child bytes in a PATH payload (`core`
 * `kMaxPathBytes`). Mirrors `core/src/path.cpp`, which caps the accumulated
 * NAME-TLV payload (4 bytes overhead + segment bytes each).
 */
pub const MAX_PATH_BYTES: usize = 1024;

/**
 * @brief A typed-tier build/parse failure. Kept SEPARATE from the codec-fault
 * [`crate::Error`]: these are semantic (path/schema) rejections, not wire
 * framing faults. Each maps to a registered protocol error concept (see
 * `crate::error_registry`).
 */
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BuildError {
    /** @brief A segment was empty or exceeded [`MAX_SEGMENT_BYTES`] (`tr::path::invalid`). */
    SegmentLength,
    /** @brief A segment held a reserved character `/ : . [ ] * ?` (`tr::path::invalid`). */
    ReservedChar,
    /** @brief A path had zero segments where at least one is required (`tr::path::invalid`). */
    EmptyPath,
    /** @brief A path exceeded [`MAX_SEGMENTS`] segments (`tr::path::invalid`). */
    TooManySegments,
    /** @brief A path's encoded NAME payload exceeded [`MAX_PATH_BYTES`] (`tr::path::invalid`). */
    PathTooLong,
    /** @brief A path string was not rooted at `/` (`tr::path::invalid`). */
    NotRooted,
    /** @brief A `:field` selector was empty or malformed (`tr::path::invalid`). */
    InvalidField,
    /**
     * @brief A structured TLV did not match its expected shape / a child had the wrong
     * type code (`tr::schema::type_mismatch`).
     */
    TypeMismatch,
    /**
     * @brief Bytes that should be UTF-8 (a NAME/DESCRIPTION payload) were not
     * (`tr::path::invalid`).
     */
    InvalidUtf8,
}

const RESERVED_CHARS: &[u8] = b"/:.[]*?";

/**
 * @brief Validate a single path/name segment against the addressing rules: 1..64 UTF-8
 * bytes, no reserved character `/ : . [ ] * ?`.
 *
 * # Errors
 * [`BuildError::SegmentLength`] or [`BuildError::ReservedChar`] on violation.
 */
pub fn validate_segment(segment: &str) -> Result<(), BuildError> {
    let bytes = segment.as_bytes();
    if bytes.is_empty() || bytes.len() > MAX_SEGMENT_BYTES {
        return Err(BuildError::SegmentLength);
    }
    if bytes.iter().any(|b| RESERVED_CHARS.contains(b)) {
        return Err(BuildError::ReservedChar);
    }
    Ok(())
}

/* --------------------------------------------------------------- opt help --- */

impl Opt {
    /**
     * @brief A structured option byte: only `PL=1` (bit 6) set, all else clear — the
     * `0x40` opt every structured container (PATH, SUBSCRIBER, FIELD, FWD, …)
     * carries at rest.
     */
    pub const fn structured() -> Opt {
        Opt {
            pl: true,
            ts: false,
            cr: false,
            ll: false,
            cw: false,
            tf: false,
        }
    }
}

/**
 * @brief Build an [`Opt`] from explicit flags, mirroring the TypeScript `opt()` helper.
 * A convenience over the struct literal for the typed builders.
 *
 * Bits, MSB→LSB in the wire byte: `PL | TS | CR | LL | CW | TF`.
 */
pub const fn opt(pl: bool, ts: bool, cr: bool, ll: bool, cw: bool, tf: bool) -> Opt {
    Opt {
        pl,
        ts,
        cr,
        ll,
        cw,
        tf,
    }
}

/* ------------------------------------------------------------- value nodes --- */

/**
 * @brief Option selectors for [`value_opts`]. Mirror the wire opt/trailer fields the
 * codec pins (vectors `value-ll-u32`, `value-crc32c`, `value-crc16`,
 * `value-ts-abs`).
 */
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ValueOptions {
    /** @brief Use the 32-bit length field (`opt.LL=1`, 6-byte header). Default: 16-bit. */
    pub long_length: bool,
    /** @brief Emit a CRC trailer over the payload (+ timestamp bytes). Default: none. */
    pub crc: bool,
    /** @brief With [`ValueOptions::crc`], use CRC-16-CCITT (`opt.CW=1`) instead of CRC-32C. */
    pub crc16: bool,
    /** @brief Absolute u64 wire timestamp (ns since epoch) → `opt.TS=1, TF=0`. */
    pub timestamp_ns: Option<u64>,
    /**
     * @brief Relative i32 wire timestamp (ns) → `opt.TS=1, TF=1`. Ignored if
     * [`ValueOptions::timestamp_ns`] is set.
     */
    pub timestamp_rel_ns: Option<i32>,
}

/**
 * @brief Build a plain opaque VALUE TLV (`type=0x01`, no trailer). Vector-pinned:
 * `value-bool-true`.
 */
pub fn value(payload: &[u8]) -> Tlv {
    value_opts(payload, &ValueOptions::default())
}

/**
 * @brief Build a VALUE TLV (`type=0x01`) with length-width / CRC / wire-timestamp
 * selectors. Vector-pinned: `value-ll-u32`, `value-crc32c`, `value-crc16`,
 * `value-ts-abs`.
 */
pub fn value_opts(payload: &[u8], opts: &ValueOptions) -> Tlv {
    let has_abs = opts.timestamp_ns.is_some();
    let has_rel = !has_abs && opts.timestamp_rel_ns.is_some();
    let has_ts = has_abs || has_rel;

    let trailer = if has_ts {
        let ts = if has_abs {
            Timestamp {
                relative: false,
                value: opts.timestamp_ns.unwrap() as i64,
            }
        } else {
            Timestamp {
                relative: true,
                value: opts.timestamp_rel_ns.unwrap() as i64,
            }
        };
        Some(Trailer {
            ts: Some(ts),
            crc: None,
        })
    } else {
        None
    };

    Tlv {
        type_code: type_code::VALUE,
        opt: opt(
            false,
            has_ts,
            opts.crc,
            opts.long_length,
            opts.crc16,
            has_rel,
        ),
        payload: payload.to_vec(),
        children: Vec::new(),
        trailer,
    }
}

/** @brief A 1-byte VALUE TLV node (e.g. a FWD `op`/`kind` discriminant, an ACE flag). */
pub fn value_u8(v: u8) -> Tlv {
    value(&[v])
}

/** @brief A little-endian u16 VALUE TLV node (e.g. an ACL `access_mask`, an ERROR code). */
pub fn value_u16(v: u16) -> Tlv {
    value(&v.to_le_bytes())
}

/** @brief A little-endian u32 VALUE TLV node (e.g. a FIELD `[N]` index). */
pub fn value_u32(v: u32) -> Tlv {
    value(&v.to_le_bytes())
}

/** @brief A little-endian u64 VALUE TLV node (e.g. an AWAIT timeout, an ACE `expires_ns`). */
pub fn value_u64(v: u64) -> Tlv {
    value(&v.to_le_bytes())
}

/* -------------------------------------------------------------- name/path --- */

/**
 * @brief Build a NAME TLV (`type=0x02`) for one path segment after validating it
 * (1..64 UTF-8 bytes, no reserved characters).
 *
 * # Errors
 * [`BuildError::SegmentLength`] / [`BuildError::ReservedChar`] on an invalid
 * segment.
 */
pub fn name(segment: &str) -> Result<Tlv, BuildError> {
    validate_segment(segment)?;
    Ok(Tlv {
        type_code: type_code::NAME,
        opt: Opt::default(),
        payload: segment.as_bytes().to_vec(),
        children: Vec::new(),
        trailer: None,
    })
}

/**
 * @brief Build a PATH TLV (`type=0x06`, `PL=1`) from segments — one validated NAME
 * child per segment. Vector-pinned: `path-sensor-temp`; normative byte layout
 * reference/05 §0x06.
 *
 * # Errors
 * [`BuildError::EmptyPath`], [`BuildError::TooManySegments`],
 * [`BuildError::PathTooLong`], or a per-segment error.
 */
pub fn path(segments: &[&str]) -> Result<Tlv, BuildError> {
    if segments.is_empty() {
        return Err(BuildError::EmptyPath);
    }
    if segments.len() > MAX_SEGMENTS {
        return Err(BuildError::TooManySegments);
    }
    let mut children = Vec::with_capacity(segments.len());
    let mut payload_bytes = 0usize;
    for seg in segments {
        let child = name(seg)?;
        // Each NAME costs 4 (header) + segment bytes; mirror core's kMaxPathBytes
        // check against the accumulated payload size.
        payload_bytes += 4 + child.payload.len();
        if payload_bytes > MAX_PATH_BYTES {
            return Err(BuildError::PathTooLong);
        }
        children.push(child);
    }
    Ok(Tlv {
        type_code: type_code::PATH,
        opt: Opt::structured(),
        payload: Vec::new(),
        children,
        trailer: None,
    })
}

/**
 * @brief Build a SUBSCRIBER TLV (`type=0x04`, `PL=1`) wrapping a target PATH — the
 * payload of a subscribe-write (reference/05 §0x04). Vector-pinned:
 * `subscriber-path` = `SUBSCRIBER{ PATH{ NAME "sensor", NAME "temp" } }`.
 *
 * # Errors
 * Any error from building the target [`path`].
 */
pub fn subscriber(target_path: &[&str]) -> Result<Tlv, BuildError> {
    Ok(Tlv {
        type_code: type_code::SUBSCRIBER,
        opt: Opt::structured(),
        payload: Vec::new(),
        children: alloc::vec![path(target_path)?],
        trailer: None,
    })
}

/* --------------------------------------------------------------- accessors --- */

impl Tlv {
    /** @brief The first child with the given `type_code`, or `None`. */
    pub fn first_child(&self, type_code: u8) -> Option<&Tlv> {
        self.children.iter().find(|c| c.type_code == type_code)
    }

    /** @brief The child at `index`, or `None` if out of range. */
    pub fn child_at(&self, index: usize) -> Option<&Tlv> {
        self.children.get(index)
    }

    /** @brief Iterate the children with the given `type_code`. */
    pub fn children_of_type(&self, type_code: u8) -> impl Iterator<Item = &Tlv> {
        self.children
            .iter()
            .filter(move |c| c.type_code == type_code)
    }

    /**
     * @brief The opaque payload interpreted as UTF-8 (for NAME / DESCRIPTION).
     *
     * # Errors
     * [`BuildError::InvalidUtf8`] if the payload is not valid UTF-8.
     */
    pub fn payload_str(&self) -> Result<&str, BuildError> {
        core::str::from_utf8(&self.payload).map_err(|_| BuildError::InvalidUtf8)
    }

    /**
     * @brief The opaque payload interpreted as a little-endian unsigned integer (up to
     * 8 bytes; extra leading zero-extension). Returns 0 for an empty payload.
     */
    pub fn payload_uint(&self) -> u64 {
        let mut v = 0u64;
        for (i, &b) in self.payload.iter().take(8).enumerate() {
            v |= (b as u64) << (8 * i);
        }
        v
    }
}
