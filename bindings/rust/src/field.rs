// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

//! `:field` selector — FIELD TLV (`0x10`) build/parse (Unit 5).
//!
//! The control-plane `:field` selector (RFC-0004 §C, ADR-0035): a structured TLV
//! whose children are per-level `NAME [+ VALUE index] [+ VALUE index_mode]`
//! triples. Mirrors `bindings/typescript/packages/client/src/fwd.ts` `fieldTlv` /
//! `parseField` and the step grammar of `core/src/path.cpp`. Vector-pinned:
//! `field-append`, `field-indexed`, `field-nested`.

use alloc::string::{String, ToString};
use alloc::vec::Vec;

use crate::tlv_builders::{name, value_u32, value_u8, BuildError};
use crate::{type_code, Opt, Tlv};

/// Max levels in a `:field` selector (`core` `kMaxFieldDepth`).
pub const MAX_FIELD_DEPTH: usize = 8;

/// The index mode of one `:field` level, matching the FIELD wire `index_mode`
/// VALUE (RFC-0004 §C).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FieldMode {
    /// No brackets — a plain scalar field (`index_mode` absent / 0).
    Scalar,
    /// `[N]` element or `[]` append (`index_mode=1`); the index is carried
    /// separately when present.
    Element,
    /// `[*]` wildcard (`index_mode=2`) — subscriber-path targets only.
    Wildcard,
}

/// One level of a `:field` selector chain.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FieldLevel {
    /// The level's field name (e.g. `"subscribers"`, `"settings"`).
    pub name: String,
    /// The `[N]` index when this is an element selector; `None` for `[]` append,
    /// scalar, or wildcard.
    pub index: Option<u32>,
    /// The level's mode.
    pub mode: FieldMode,
}

impl FieldLevel {
    /// A scalar level (`name`, no brackets).
    pub fn scalar(name: &str) -> FieldLevel {
        FieldLevel {
            name: name.to_string(),
            index: None,
            mode: FieldMode::Scalar,
        }
    }

    /// An indexed element level (`name[index]`).
    pub fn indexed(name: &str, index: u32) -> FieldLevel {
        FieldLevel {
            name: name.to_string(),
            index: Some(index),
            mode: FieldMode::Element,
        }
    }

    /// An append element level (`name[]`).
    pub fn append(name: &str) -> FieldLevel {
        FieldLevel {
            name: name.to_string(),
            index: None,
            mode: FieldMode::Element,
        }
    }

    /// A wildcard level (`name[*]`).
    pub fn wildcard(name: &str) -> FieldLevel {
        FieldLevel {
            name: name.to_string(),
            index: None,
            mode: FieldMode::Wildcard,
        }
    }
}

/// Parse a `:field` selector string into its levels. Accepts an optional leading
/// `:`; levels split on `.`; each level may carry a trailing `[N]`, `[]`
/// (append), or `[*]` (wildcard).
///
/// `":subscribers[]"` → one append level; `":subscribers[3]"` → one indexed
/// level; `":settings.deadline_ns"` → two scalar levels.
///
/// # Errors
/// [`BuildError::InvalidField`] on an empty selector or a malformed level,
/// [`BuildError::TooManySegments`] beyond [`MAX_FIELD_DEPTH`], or a per-name
/// segment error.
pub fn parse_field(field: &str) -> Result<Vec<FieldLevel>, BuildError> {
    let body = field.strip_prefix(':').unwrap_or(field);
    if body.is_empty() {
        return Err(BuildError::InvalidField);
    }
    let mut levels = Vec::new();
    for part in body.split('.') {
        levels.push(parse_level(part)?);
        if levels.len() > MAX_FIELD_DEPTH {
            return Err(BuildError::TooManySegments);
        }
    }
    Ok(levels)
}

fn parse_level(part: &str) -> Result<FieldLevel, BuildError> {
    match part.find('[') {
        None => {
            if part.is_empty() {
                return Err(BuildError::InvalidField);
            }
            Ok(FieldLevel::scalar(part))
        }
        Some(br) => {
            if !part.ends_with(']') {
                return Err(BuildError::InvalidField);
            }
            let name = &part[..br];
            if name.is_empty() {
                return Err(BuildError::InvalidField);
            }
            let inner = &part[br + 1..part.len() - 1];
            if inner == "*" {
                Ok(FieldLevel::wildcard(name))
            } else if inner.is_empty() {
                Ok(FieldLevel::append(name))
            } else {
                let index: u32 = inner.parse().map_err(|_| BuildError::InvalidField)?;
                Ok(FieldLevel::indexed(name, index))
            }
        }
    }
}

/// Build a FIELD TLV node (`type=0x10`, `PL=1`) from its levels. Each level emits
/// a `NAME`, then (for `Element` with an index) a u32 index VALUE, then the
/// `index_mode` VALUE (`Element`=1, `Wildcard`=2; `Scalar` emits neither).
///
/// # Errors
/// [`BuildError::InvalidField`] on zero levels, [`BuildError::TooManySegments`]
/// beyond [`MAX_FIELD_DEPTH`], or a per-name segment error.
pub fn field_tlv(levels: &[FieldLevel]) -> Result<Tlv, BuildError> {
    if levels.is_empty() {
        return Err(BuildError::InvalidField);
    }
    if levels.len() > MAX_FIELD_DEPTH {
        return Err(BuildError::TooManySegments);
    }
    let mut children = Vec::new();
    for level in levels {
        children.push(name(&level.name)?);
        match level.mode {
            FieldMode::Wildcard => children.push(value_u8(2)),
            FieldMode::Element => {
                if let Some(index) = level.index {
                    children.push(value_u32(index));
                }
                children.push(value_u8(1));
            }
            FieldMode::Scalar => {}
        }
    }
    Ok(Tlv {
        type_code: type_code::FIELD,
        opt: Opt::structured(),
        payload: Vec::new(),
        children,
        trailer: None,
    })
}

/// Build a FIELD TLV from a `:field` selector string. Vector-pinned:
/// `field-append`, `field-indexed`, `field-nested`.
///
/// # Errors
/// Any error from [`parse_field`] or [`field_tlv`].
pub fn encode_field(field: &str) -> Result<Tlv, BuildError> {
    field_tlv(&parse_field(field)?)
}

/// Parse a FIELD TLV back into its levels (the inverse of [`field_tlv`]).
///
/// # Errors
/// [`BuildError::TypeMismatch`] if the TLV is not a FIELD or a child sequence is
/// malformed, or a non-UTF-8 name.
pub fn parse_field_tlv(tlv: &Tlv) -> Result<Vec<FieldLevel>, BuildError> {
    if tlv.type_code != type_code::FIELD {
        return Err(BuildError::TypeMismatch);
    }
    let mut levels = Vec::new();
    let mut i = 0;
    let ch = &tlv.children;
    while i < ch.len() {
        if ch[i].type_code != type_code::NAME {
            return Err(BuildError::TypeMismatch);
        }
        let name = ch[i].payload_str()?.to_string();
        i += 1;
        // Optional index VALUE (u32) then optional index_mode VALUE (u8).
        let mut index: Option<u32> = None;
        let mut mode = FieldMode::Scalar;
        // An index (>1 byte VALUE) precedes the 1-byte index_mode.
        if i < ch.len() && ch[i].type_code == type_code::VALUE && ch[i].payload.len() >= 2 {
            index = Some(ch[i].payload_uint() as u32);
            i += 1;
        }
        if i < ch.len() && ch[i].type_code == type_code::VALUE && ch[i].payload.len() == 1 {
            mode = match ch[i].payload[0] {
                2 => FieldMode::Wildcard,
                1 => FieldMode::Element,
                _ => FieldMode::Scalar,
            };
            i += 1;
        }
        levels.push(FieldLevel { name, index, mode });
    }
    Ok(levels)
}
