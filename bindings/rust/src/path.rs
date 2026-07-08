// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/*!
 * @brief PATH string ⇄ TLV conversion + validation (Unit 2).
 *
 * Mirrors the address-portion parsing of `core/src/path.cpp` (`path_t::parse`):
 * a path is rooted at `/`, split on `/`, each segment validated (1..64 bytes,
 * no reserved character `/ : . [ ] * ?`), at most [`MAX_SEGMENTS`] segments and
 * [`MAX_PATH_BYTES`] total encoded NAME payload. Vector-pinned: `path-sensor-temp`.
 */

use alloc::string::String;
use alloc::vec::Vec;

use crate::tlv_builders::{
    path as build_path, validate_segment, BuildError, MAX_PATH_BYTES, MAX_SEGMENTS,
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
 * @brief Convert a rooted path string into a PATH TLV (`type=0x06`, `PL=1`).
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
 * with no NAME children renders as the root `"/"`.
 *
 * # Errors
 * [`BuildError::TypeMismatch`] if the TLV is not a PATH or a child is not a
 * NAME, [`BuildError::InvalidUtf8`] on a non-UTF-8 segment, or a segment-length
 * / total-length violation.
 */
pub fn tlv_to_path(tlv: &Tlv) -> Result<String, BuildError> {
    if tlv.type_code != type_code::PATH {
        return Err(BuildError::TypeMismatch);
    }
    if tlv.children.is_empty() {
        return Ok(String::from("/"));
    }
    if tlv.children.len() > MAX_SEGMENTS {
        return Err(BuildError::TooManySegments);
    }
    let mut out = String::new();
    let mut payload_bytes = 0usize;
    for child in &tlv.children {
        if child.type_code != type_code::NAME {
            return Err(BuildError::TypeMismatch);
        }
        let seg = child.payload_str()?;
        validate_segment(seg)?;
        payload_bytes += 4 + child.payload.len();
        if payload_bytes > MAX_PATH_BYTES {
            return Err(BuildError::PathTooLong);
        }
        out.push('/');
        out.push_str(seg);
    }
    Ok(out)
}
