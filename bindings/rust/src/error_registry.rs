// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

//! Protocol ERROR registry + STATUS (Unit 3).
//!
//! The frozen RFC-0002 `tr::<concept>::<error>` registry — the 15-entry
//! code ⇄ path table mirrored from `core/include/libtracer/error.hpp` — plus
//! ERROR-TLV (`0x08`) parse/encode in both identity forms (registered u16 code /
//! string `tr::…` path) and STATUS-TLV (`0x09`) helpers.
//!
//! This registry is deliberately SEPARATE from the codec-fault [`crate::Error`]:
//! that enum names wire *framing* faults for the fuzz harness; this one is the
//! protocol *semantic* error model that rides inside STATUS/FWD replies.
//! Vector-pinned: `error-registered-code`, `error-registered-detail`,
//! `error-string-form`, `empty-status-ok`.

use alloc::string::{String, ToString};
use alloc::vec::Vec;

use crate::tlv_builders::{value_u16, BuildError};
use crate::{type_code, Opt, Tlv};

/// A registered protocol error code (RFC-0002 §D) — the u16 wire identity of a
/// built-in `tr::…` error path, carried LE in the ERROR TLV's first-child VALUE.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u16)]
pub enum ErrCode {
    /// `tr::frame::truncated`
    FrameTruncated = 0x0001,
    /// `tr::frame::invalid`
    FrameInvalid = 0x0002,
    /// `tr::frame::crc_fail`
    FrameCrcFail = 0x0003,
    /// `tr::tlv::nesting_too_deep`
    TlvNestingTooDeep = 0x0010,
    /// `tr::path::not_found`
    PathNotFound = 0x0020,
    /// `tr::path::invalid`
    PathInvalid = 0x0021,
    /// `tr::path::in_use`
    PathInUse = 0x0022,
    /// `tr::schema::type_mismatch`
    SchemaTypeMismatch = 0x0030,
    /// `tr::schema::not_found`
    SchemaNotFound = 0x0031,
    /// `tr::flow::backpressure`
    FlowBackpressure = 0x0040,
    /// `tr::flow::timeout`
    FlowTimeout = 0x0041,
    /// `tr::flow::address_shift_gap`
    FlowAddressShiftGap = 0x0042,
    /// `tr::access::denied`
    AccessDenied = 0x0050,
    /// `tr::transport::down`
    TransportDown = 0x0060,
    /// `tr::version::mismatch`
    VersionMismatch = 0x0070,
}

/// Registry-side severity of a protocol error (RFC-0002 §D) — advisory metadata
/// that never travels on the wire.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Severity {
    /// Expected-in-operation outcome (e.g. a missing path).
    Warn,
    /// A genuine protocol-level failure.
    Error,
    /// The peer relationship itself is unsound.
    Critical,
}

/// Registry-side disposition of a protocol error (RFC-0002 §D) — what the caller
/// should do next; never travels on the wire.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Disposition {
    /// Retry may succeed (congestion, timing, lossy link).
    Transient,
    /// Don't retry this request as-is.
    Permanent,
    /// Tear down the peer relationship.
    Fatal,
}

/// The 15-entry registry table: `(code enum, u16 code, tr:: path, severity,
/// disposition)`, in code order. Single source of truth for the lookups below.
const REGISTRY: [(ErrCode, u16, &str, Severity, Disposition); 15] = [
    (ErrCode::FrameTruncated, 0x0001, "tr::frame::truncated", Severity::Error, Disposition::Transient),
    (ErrCode::FrameInvalid, 0x0002, "tr::frame::invalid", Severity::Error, Disposition::Permanent),
    (ErrCode::FrameCrcFail, 0x0003, "tr::frame::crc_fail", Severity::Error, Disposition::Transient),
    (ErrCode::TlvNestingTooDeep, 0x0010, "tr::tlv::nesting_too_deep", Severity::Error, Disposition::Permanent),
    (ErrCode::PathNotFound, 0x0020, "tr::path::not_found", Severity::Warn, Disposition::Permanent),
    (ErrCode::PathInvalid, 0x0021, "tr::path::invalid", Severity::Warn, Disposition::Permanent),
    (ErrCode::PathInUse, 0x0022, "tr::path::in_use", Severity::Warn, Disposition::Permanent),
    (ErrCode::SchemaTypeMismatch, 0x0030, "tr::schema::type_mismatch", Severity::Error, Disposition::Permanent),
    (ErrCode::SchemaNotFound, 0x0031, "tr::schema::not_found", Severity::Warn, Disposition::Permanent),
    (ErrCode::FlowBackpressure, 0x0040, "tr::flow::backpressure", Severity::Warn, Disposition::Transient),
    (ErrCode::FlowTimeout, 0x0041, "tr::flow::timeout", Severity::Warn, Disposition::Transient),
    (ErrCode::FlowAddressShiftGap, 0x0042, "tr::flow::address_shift_gap", Severity::Error, Disposition::Permanent),
    (ErrCode::AccessDenied, 0x0050, "tr::access::denied", Severity::Error, Disposition::Permanent),
    (ErrCode::TransportDown, 0x0060, "tr::transport::down", Severity::Error, Disposition::Transient),
    (ErrCode::VersionMismatch, 0x0070, "tr::version::mismatch", Severity::Critical, Disposition::Fatal),
];

impl ErrCode {
    /// The u16 wire code of this error (the ERROR TLV's VALUE payload, LE).
    pub const fn code(self) -> u16 {
        self as u16
    }

    /// The registered code for a raw u16, or `None` if unregistered.
    pub fn from_code(code: u16) -> Option<ErrCode> {
        REGISTRY.iter().find(|e| e.1 == code).map(|e| e.0)
    }

    /// The registered code for a `tr::…` path, or `None` if unregistered.
    pub fn from_path(path: &str) -> Option<ErrCode> {
        REGISTRY.iter().find(|e| e.2 == path).map(|e| e.0)
    }

    /// The canonical `tr::<concept>::<error>` path (the string-form identity).
    pub fn path(self) -> &'static str {
        REGISTRY[self.index()].2
    }

    /// The registry severity of this error.
    pub fn severity(self) -> Severity {
        REGISTRY[self.index()].3
    }

    /// The registry disposition of this error.
    pub fn disposition(self) -> Disposition {
        REGISTRY[self.index()].4
    }

    fn index(self) -> usize {
        // Small linear scan (15 entries) — the registry is order-stable.
        REGISTRY.iter().position(|e| e.0 == self).unwrap_or(0)
    }
}

/// The two identity forms of an ERROR TLV's first child (RFC-0002 §C).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ErrorId {
    /// A registered code (`VALUE` first child, u16 LE). Carries the raw u16 so an
    /// unrecognized-but-numeric code round-trips; use [`ErrCode::from_code`] to
    /// resolve it to a known entry.
    Code(u16),
    /// A string-form identity (`NAME` first child, UTF-8 `tr::…` path) — used by
    /// third-party extensions outside the frozen registry.
    Path(String),
}

/// A decoded ERROR TLV: its identity plus any optional human `DESCRIPTION` detail.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ParsedError {
    /// The error identity (registered code or string path).
    pub id: ErrorId,
    /// The optional human-readable `DESCRIPTION` (`0x03`) detail, if present.
    pub description: Option<String>,
}

impl ParsedError {
    /// The resolved registry code, when the identity is a registered numeric code
    /// (or a `tr::…` path that maps into the registry).
    pub fn err_code(&self) -> Option<ErrCode> {
        match &self.id {
            ErrorId::Code(c) => ErrCode::from_code(*c),
            ErrorId::Path(p) => ErrCode::from_path(p),
        }
    }
}

/// Build an ERROR TLV (`type=0x08`, `PL=1`) in registered-code form:
/// `ERROR{ VALUE u16=code [, DESCRIPTION detail] }`. Vector-pinned:
/// `error-registered-code`, `error-registered-detail`.
pub fn error_code(code: ErrCode, description: Option<&str>) -> Tlv {
    error_with_id_value(value_u16(code.code()), description)
}

/// Build an ERROR TLV in raw registered-code form from an arbitrary u16 (for a
/// code not in the frozen registry).
pub fn error_raw_code(code: u16, description: Option<&str>) -> Tlv {
    error_with_id_value(value_u16(code), description)
}

/// Build an ERROR TLV (`type=0x08`, `PL=1`) in string form:
/// `ERROR{ NAME tr::… [, DESCRIPTION detail] }`. Vector-pinned:
/// `error-string-form`.
pub fn error_string(path: &str, description: Option<&str>) -> Tlv {
    let id = Tlv {
        type_code: type_code::NAME,
        opt: Opt::default(),
        payload: path.as_bytes().to_vec(),
        children: Vec::new(),
        trailer: None,
    };
    error_with_id_value(id, description)
}

fn error_with_id_value(id: Tlv, description: Option<&str>) -> Tlv {
    let mut children = alloc::vec![id];
    if let Some(text) = description {
        children.push(Tlv {
            type_code: type_code::DESCRIPTION,
            opt: Opt::default(),
            payload: text.as_bytes().to_vec(),
            children: Vec::new(),
            trailer: None,
        });
    }
    Tlv {
        type_code: type_code::ERROR,
        opt: Opt::structured(),
        payload: Vec::new(),
        children,
        trailer: None,
    }
}

/// Parse an ERROR TLV into its identity and optional description.
///
/// # Errors
/// [`BuildError::TypeMismatch`] if the TLV is not an ERROR or its first child is
/// neither a registered-code VALUE (≥2 bytes) nor a string NAME.
pub fn parse_error(tlv: &Tlv) -> Result<ParsedError, BuildError> {
    if tlv.type_code != type_code::ERROR {
        return Err(BuildError::TypeMismatch);
    }
    let first = tlv.children.first().ok_or(BuildError::TypeMismatch)?;
    let id = match first.type_code {
        type_code::VALUE => {
            if first.payload.len() < 2 {
                return Err(BuildError::TypeMismatch);
            }
            ErrorId::Code(first.payload[0] as u16 | ((first.payload[1] as u16) << 8))
        }
        type_code::NAME => ErrorId::Path(first.payload_str()?.to_string()),
        _ => return Err(BuildError::TypeMismatch),
    };
    let description = tlv
        .children
        .iter()
        .find(|c| c.type_code == type_code::DESCRIPTION)
        .map(|c| c.payload_str().map(|s| s.to_string()))
        .transpose()?;
    Ok(ParsedError { id, description })
}

/* ------------------------------------------------------------------ status --- */

/// Build the empty STATUS=OK TLV (`09 00 00 00`) — `opt.PL=0`, zero length. The
/// smallest valid frame and the implicit OK reply. Vector-pinned: `empty-status-ok`.
pub fn status_ok() -> Tlv {
    Tlv {
        type_code: type_code::STATUS,
        opt: Opt::default(),
        payload: Vec::new(),
        children: Vec::new(),
        trailer: None,
    }
}

/// Build a non-empty STATUS TLV (`type=0x09`, `PL=1`) carrying one or more ERROR
/// children (and optional DESCRIPTION siblings appended by the caller).
pub fn status_with_errors(errors: &[Tlv]) -> Tlv {
    Tlv {
        type_code: type_code::STATUS,
        opt: Opt::structured(),
        payload: Vec::new(),
        children: errors.to_vec(),
        trailer: None,
    }
}

/// The ERROR children of a STATUS TLV (empty for STATUS=OK).
///
/// # Errors
/// [`BuildError::TypeMismatch`] if the TLV is not a STATUS.
pub fn status_errors(tlv: &Tlv) -> Result<Vec<ParsedError>, BuildError> {
    if tlv.type_code != type_code::STATUS {
        return Err(BuildError::TypeMismatch);
    }
    tlv.children
        .iter()
        .filter(|c| c.type_code == type_code::ERROR)
        .map(parse_error)
        .collect()
}
