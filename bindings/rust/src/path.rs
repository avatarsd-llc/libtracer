// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/*!
 * @brief PATH string ⇄ TLV conversion + validation (Unit 2).
 *
 * Mirrors the address-portion parsing of `core/src/path.cpp` (`path_t::parse`):
 * a path is rooted at `/`, split on `/`, each segment validated (1..64 bytes,
 * no reserved character `/ : . [ ] * ?`), at most [`MAX_SEGMENTS`] segments and
 * [`MAX_PATH_BYTES`] total encoded body bytes. Vector-pinned: `path-sensor-temp`.
 *
 * Since RFC-0018 a `PATH` body is `opt.PL = 0` and holds packed `[u8 len][utf8]` segment
 * RECORDS rather than `NAME` child TLVs, so everything here reads and writes the body
 * directly. `NAME` itself is untouched — it still spells SETTINGS keys and `:schema`
 * labels; the RFC removes it from `PATH` bodies only.
 *
 * The two accumulative bounds (segment count, total encoded body) sit at the
 * construction/admission tier — [`split_path`] (count) and `tlv_builders::path`
 * (bytes) on the way in from a string, and [`admit_path_tlv`] (both) on the way
 * in from decoded bytes — never at decode
 * ([`tlv_to_path`]), per `docs/reference/05-protocol-tlvs.md` §"Enforcement of the PATH
 * constraints" and RFC-0023 §5.6.
 */

use alloc::string::String;
use alloc::vec::Vec;

use crate::tlv_builders::{
    packed_segments, path as build_path, validate_segment, BuildError, MAX_PATH_BYTES, MAX_SEGMENTS,
};
use crate::{type_code, Tlv};

/**
 * @brief Split a rooted path string (`"/sensor/temp"`) into its validated segments
 * (`["sensor", "temp"]`). The root `"/"` yields an empty slice (the graph root).
 *
 * The string must be rooted at `/`; trailing slashes are stripped (the root is
 * preserved). Each segment is validated per [`validate_segment`].
 *
 * # Errors
 * [`BuildError::NotRooted`] if not `/`-rooted, [`BuildError::SegmentLength`] /
 * [`BuildError::ReservedChar`] on an invalid segment, or
 * [`BuildError::TooManySegments`] beyond [`MAX_SEGMENTS`].
 */
pub fn split_path(text: &str) -> Result<Vec<&str>, BuildError> {
    if !text.starts_with('/') {
        return Err(BuildError::NotRooted);
    }
    // Strip trailing slashes but keep the root "/".
    let mut addr = text;
    while addr.len() > 1 && addr.ends_with('/') {
        addr = &addr[..addr.len() - 1];
    }
    if addr == "/" {
        return Ok(Vec::new()); // graph root: zero segments
    }
    let mut segments = Vec::new();
    // Skip the leading '/', then split on '/'.
    for seg in addr[1..].split('/') {
        if seg.is_empty() {
            return Err(BuildError::SegmentLength); // "//" or empty segment
        }
        validate_segment(seg)?;
        segments.push(seg);
        if segments.len() > MAX_SEGMENTS {
            return Err(BuildError::TooManySegments);
        }
    }
    Ok(segments)
}

/**
 * @brief Convert a rooted path string into a PATH TLV (`type=0x06`, `PL=0`, packed body).
 * Vector-pinned: `path-sensor-temp` (`"/sensor/temp"`).
 *
 * # Errors
 * Any error from [`split_path`] or from building the [`Tlv`] (including
 * [`BuildError::EmptyPath`] for the root, or [`BuildError::PathTooLong`]).
 */
pub fn path_to_tlv(text: &str) -> Result<Tlv, BuildError> {
    let segments = split_path(text)?;
    build_path(&segments)
}

/**
 * @brief Convert a PATH TLV back into its rooted string form (`"/sensor/temp"`). A PATH
 * with an empty body renders as the root `"/"`.
 *
 * This is the **decode tier**: it renders, it does not admit. The segment-count and
 * total-length bounds are enforced by [`admit_path_tlv`] (and, on the encode side, by
 * [`split_path`] / [`crate::tlv_builders::path`]) — see that function for why.
 *
 * Rendering IS canonical / key context — the caller wants a spelling it can look an address
 * up by — so the RFC-0018 §5.4 `len == 0` escape is refused here rather than skipped.
 *
 * # Errors
 * [`BuildError::TypeMismatch`] if the TLV is not a PATH or carries a structured body,
 * [`BuildError::InvalidUtf8`] on a non-UTF-8 segment, or
 * [`BuildError::SegmentLength`] / [`BuildError::ReservedChar`] on ragged framing, an escape
 * record, or a segment that could not be rendered back into a re-splittable rooted string.
 */
pub fn tlv_to_path(tlv: &Tlv) -> Result<String, BuildError> {
    if tlv.type_code != type_code::PATH || tlv.opt.pl || !tlv.children.is_empty() {
        return Err(BuildError::TypeMismatch);
    }
    if tlv.payload.is_empty() {
        return Ok(String::from("/"));
    }
    // The two ACCUMULATIVE bounds — segment count and total encoded body bytes — are NOT
    // checked here. `reference/05` §"Enforcement of the PATH constraints" puts them where an
    // address is CONSTRUCTED or ADMITTED ([`admit_path_tlv`]), not at decode: "the codec does
    // not enforce these constraints, and is not expected to". This is the decode side (an
    // already-parsed `&Tlv`, reached from `fwd_dst_path` / `fwd_src_path` /
    // `subscriber_target_path`), and an accumulated `src` return route grows past both bounds
    // while remaining legal at every byte-reachable depth (RFC-0019 §10(b), RFC-0023 §5.6).
    //
    // The per-segment rules stay: they are not accumulative (every legally constructed PATH
    // satisfies them segment-by-segment, so accumulation can never push a route past them),
    // and they are what makes the rendered string re-splittable — a segment holding `/`, or an
    // empty one, would render an address that does not round-trip through `split_path`.
    let mut out = String::new();
    for record in packed_segments(&tlv.payload)? {
        let seg = core::str::from_utf8(record).map_err(|_| BuildError::InvalidUtf8)?;
        validate_segment(seg)?;
        out.push('/');
        out.push_str(seg);
    }
    Ok(out)
}

/**
 * @brief Admit a decoded PATH TLV as an *address* — the construction/admission-tier gate
 * that `docs/reference/05-protocol-tlvs.md` §"Enforcement of the PATH constraints" names
 * as the home of the length and segment-count limits (RFC-0023 §5.6).
 *
 * Call this where a foreign PATH stops being bytes and becomes an address a resolver will
 * spell: before a vertex lookup, before storing a `SUBSCRIBER` target, before accepting a
 * `FWD` `dst` as this node's own address. Do **not** call it on a `src` return route being
 * forwarded on: that route accumulates a hop per forward and is legal at any byte-reachable
 * depth, which is precisely why the bound does not live in [`tlv_to_path`].
 *
 * The predicate is the encode-time MUST of [`crate::tlv_builders::path`], evaluated over an
 * already-decoded tree: `(records ≤ `[`MAX_SEGMENTS`]`) ∧ (each record a valid literal
 * segment) ∧ (body ≤ `[`MAX_PATH_BYTES`]`)`. A PATH with an empty body is the graph root and
 * is admitted. The RFC-0018 §5.4 escape is refused — admission is exactly the canonical/key
 * door the escape is not admissible at.
 *
 * # Errors
 * [`BuildError::TypeMismatch`] if the TLV is not a PATH or carries a structured body,
 * [`BuildError::TooManySegments`] beyond [`MAX_SEGMENTS`], [`BuildError::PathTooLong`]
 * beyond [`MAX_PATH_BYTES`], [`BuildError::InvalidUtf8`] on a non-UTF-8 segment, or
 * [`BuildError::SegmentLength`] / [`BuildError::ReservedChar`] on ragged framing, an escape
 * record, or an invalid segment.
 */
pub fn admit_path_tlv(tlv: &Tlv) -> Result<(), BuildError> {
    if tlv.type_code != type_code::PATH || tlv.opt.pl || !tlv.children.is_empty() {
        return Err(BuildError::TypeMismatch);
    }
    // The body IS the encoded path (RFC-0018), so the byte cap is one length check rather
    // than an accumulation over children.
    if tlv.payload.len() > MAX_PATH_BYTES {
        return Err(BuildError::PathTooLong);
    }
    let records = packed_segments(&tlv.payload)?;
    if records.len() > MAX_SEGMENTS {
        return Err(BuildError::TooManySegments);
    }
    for record in records {
        let seg = core::str::from_utf8(record).map_err(|_| BuildError::InvalidUtf8)?;
        validate_segment(seg)?;
    }
    Ok(())
}
