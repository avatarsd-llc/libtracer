// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

//! FWD remote-operation envelope — FWD TLV (`0x0F`) build/parse (Unit 4).
//!
//! The source-routed remote-operation frame (RFC-0004 §B/§D, ADR-0035). Mirrors
//! `bindings/typescript/packages/client/src/fwd.ts` and the terminus resolver of
//! `core/src/op_resolve.cpp`. Child order (RFC-0004 §B): `op`, `dst` (PATH),
//! `FIELD?` selector, `src` (PATH), then per-op:
//!   - REPLY: `kind` (VALUE u8), optional payload
//!   - WRITE: optional payload
//!   - AWAIT: optional `await_timeout` (VALUE u64 ns)
//!   - READ : nothing
//!
//! Vector-pinned: all nine `fwd/*` vectors.

use alloc::string::String;
use alloc::vec::Vec;

use crate::error_registry::ErrCode;
use crate::field::{field_tlv, parse_field, FieldLevel};
use crate::path::path_to_tlv;
use crate::tlv_builders::{path as build_path, value_u64, value_u8, BuildError};
use crate::{type_code, Opt, Tlv};

/// FWD `op` discriminant (RFC-0004 §B — the first child, a VALUE u8).
pub mod fwd_op {
    /// Read a vertex's last-known value.
    pub const READ: u8 = 0;
    /// Write a value / field (subscribe is a `:subscribers[]` field-write).
    pub const WRITE: u8 = 1;
    /// Await the next write, up to an optional timeout.
    pub const AWAIT: u8 = 2;
    /// A reply routed back along the accumulated `src`.
    pub const REPLY: u8 = 3;
}

/// FWD{REPLY} `kind` discriminant (RFC-0004 §D — a VALUE u8 after `src`).
pub mod fwd_kind {
    /// A successful result (payload is the RESULT value).
    pub const RESULT: u8 = 0;
    /// A failure (payload is `STATUS{ ERROR }`).
    pub const ERROR: u8 = 1;
}

/// The registered wire ERROR code of a `tr::…` path, or 0 when unregistered.
/// The FWD error-path table is [`ErrCode`] itself (RFC-0002 §D).
pub fn fwd_error_code_for_path(path: &str) -> u16 {
    ErrCode::from_path(path).map(ErrCode::code).unwrap_or(0)
}

/// The canonical `tr::…` path of a registered wire ERROR code, or `None`.
pub fn fwd_error_path(code: u16) -> Option<&'static str> {
    ErrCode::from_code(code).map(ErrCode::path)
}

/* -------------------------------------------------------------- FWD request --- */

/// A `:field` selector for a FWD request: a raw string (parsed) or pre-built levels.
pub enum FieldSel<'a> {
    /// A `:field` selector string (see [`parse_field`]).
    Str(&'a str),
    /// Pre-parsed selector levels.
    Levels(Vec<FieldLevel>),
}

/// A request to build a FWD frame. `op` selects the trailing-child layout.
pub struct FwdRequest<'a> {
    /// The operation (a [`fwd_op`] value).
    pub op: u8,
    /// Forward route: the unresolved destination path segments (the PATH `dst`).
    pub dst: &'a [&'a str],
    /// Accumulated return route: the originator's reply-endpoint segments (`src`).
    pub src: &'a [&'a str],
    /// Optional `:field` selector.
    pub field: Option<FieldSel<'a>>,
    /// Optional payload node (a VALUE to WRITE, a SUBSCRIBER to subscribe, the
    /// RESULT/STATUS of a REPLY). Embedded verbatim.
    pub payload: Option<Tlv>,
    /// REPLY only — the reply [`fwd_kind`] (defaults to RESULT).
    pub kind: Option<u8>,
    /// AWAIT only — the `await_timeout` in ns (absent ⇒ the responder default).
    pub await_timeout_ns: Option<u64>,
}

impl<'a> FwdRequest<'a> {
    /// A minimal request: `op`, `dst`, `src`; all optional fields cleared.
    pub fn new(op: u8, dst: &'a [&'a str], src: &'a [&'a str]) -> FwdRequest<'a> {
        FwdRequest {
            op,
            dst,
            src,
            field: None,
            payload: None,
            kind: None,
            await_timeout_ns: None,
        }
    }
}

fn field_node(sel: &FieldSel) -> Result<Tlv, BuildError> {
    match sel {
        FieldSel::Str(s) => field_tlv(&parse_field(s)?),
        FieldSel::Levels(l) => field_tlv(l),
    }
}

/// Build a FWD TLV node (`type=0x0F`, `PL=1`) — the remote-operation envelope.
/// Children are assembled in the normative order for the request's `op`.
/// Vector-pinned: `fwd-read`, `fwd-write-value`, `fwd-await-timeout`,
/// `fwd-write-subscriber-field`, `fwd-reply-result`, `fwd-reply-error`,
/// `fwd-routed-multihop`, `fwd-src-accumulated`, `fwd-wildcard-reject`.
///
/// # Errors
/// Any error from building the `dst`/`src` PATHs, the FIELD selector, or an
/// invalid `op`.
pub fn encode_fwd(req: &FwdRequest) -> Result<Tlv, BuildError> {
    let mut children = alloc::vec![value_u8(req.op), build_path(req.dst)?];
    if let Some(sel) = &req.field {
        children.push(field_node(sel)?);
    }
    children.push(build_path(req.src)?);

    if req.op == fwd_op::REPLY {
        children.push(value_u8(req.kind.unwrap_or(fwd_kind::RESULT)));
        if let Some(p) = &req.payload {
            children.push(p.clone());
        }
    } else if req.op == fwd_op::WRITE {
        if let Some(p) = &req.payload {
            children.push(p.clone());
        }
    } else if req.op == fwd_op::AWAIT {
        if let Some(ns) = req.await_timeout_ns {
            children.push(value_u64(ns));
        }
    }

    Ok(Tlv {
        type_code: type_code::FWD,
        opt: Opt::structured(),
        payload: Vec::new(),
        children,
        trailer: None,
    })
}

/// Convenience: build a FWD frame directly to encoded bytes.
///
/// # Errors
/// Any error from [`encode_fwd`].
pub fn encode_fwd_bytes(req: &FwdRequest) -> Result<Vec<u8>, BuildError> {
    Ok(crate::encode(&encode_fwd(req)?))
}

/* --------------------------------------------------------------- FWD parse --- */

/// A parsed FWD frame, its children read positionally (RFC-0004 §B order).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ParsedFwd {
    /// The operation (a [`fwd_op`] value).
    pub op: u8,
    /// The `dst` PATH child.
    pub dst: Tlv,
    /// The optional FIELD selector child.
    pub field: Option<Tlv>,
    /// The `src` PATH child.
    pub src: Tlv,
    /// The reply [`fwd_kind`] (REPLY frames only).
    pub kind: Option<u8>,
    /// The payload TLV child (WRITE payload / REPLY result), if present.
    pub payload: Option<Tlv>,
    /// The AWAIT `await_timeout` ns, if present.
    pub await_timeout_ns: Option<u64>,
}

fn u8_of(t: &Tlv) -> u8 {
    t.payload.first().copied().unwrap_or(0)
}

/// Parse an already-decoded FWD [`Tlv`] positionally (RFC-0004 §B).
///
/// # Errors
/// [`BuildError::TypeMismatch`] if the TLV is not a FWD or is missing a required
/// `op` / `dst` / `src` child.
pub fn parse_fwd_tlv(fwd: &Tlv) -> Result<ParsedFwd, BuildError> {
    if fwd.type_code != type_code::FWD {
        return Err(BuildError::TypeMismatch);
    }
    let ch = &fwd.children;
    let mut i = 0;
    if ch.get(i).map(|c| c.type_code) != Some(type_code::VALUE) {
        return Err(BuildError::TypeMismatch);
    }
    let op = u8_of(&ch[i]);
    i += 1;
    if ch.get(i).map(|c| c.type_code) != Some(type_code::PATH) {
        return Err(BuildError::TypeMismatch);
    }
    let dst = ch[i].clone();
    i += 1;
    let mut field = None;
    if ch.get(i).map(|c| c.type_code) == Some(type_code::FIELD) {
        field = Some(ch[i].clone());
        i += 1;
    }
    if ch.get(i).map(|c| c.type_code) != Some(type_code::PATH) {
        return Err(BuildError::TypeMismatch);
    }
    let src = ch[i].clone();
    i += 1;

    let mut kind = None;
    let mut payload = None;
    let mut await_timeout_ns = None;
    if op == fwd_op::REPLY {
        if ch.get(i).map(|c| c.type_code) == Some(type_code::VALUE) {
            kind = Some(u8_of(&ch[i]));
            i += 1;
        }
        if i < ch.len() {
            payload = Some(ch[i].clone());
        }
    } else if op == fwd_op::WRITE {
        if i < ch.len() {
            payload = Some(ch[i].clone());
        }
    } else if op == fwd_op::AWAIT {
        if let Some(c) = ch.get(i) {
            if c.type_code == type_code::VALUE && c.payload.len() >= 8 {
                await_timeout_ns = Some(c.payload_uint());
            }
        }
    }

    Ok(ParsedFwd {
        op,
        dst,
        field,
        src,
        kind,
        payload,
        await_timeout_ns,
    })
}

/// Decode a complete FWD frame's bytes into its parsed children.
///
/// # Errors
/// [`BuildError::TypeMismatch`] if the frame is not a decodable FWD (a codec
/// fault surfaces as `TypeMismatch`; use [`crate::decode`] directly for the
/// underlying [`crate::Error`]).
pub fn decode_fwd(bytes: &[u8]) -> Result<ParsedFwd, BuildError> {
    let tlv = crate::decode(bytes).map_err(|_| BuildError::TypeMismatch)?;
    parse_fwd_tlv(&tlv)
}

/// The `dst` route of a parsed FWD in string form.
///
/// # Errors
/// A PATH-decode error.
pub fn fwd_dst_path(fwd: &ParsedFwd) -> Result<String, BuildError> {
    crate::path::tlv_to_path(&fwd.dst)
}

/// The `src` (accumulated return route) of a parsed FWD in string form.
///
/// # Errors
/// A PATH-decode error.
pub fn fwd_src_path(fwd: &ParsedFwd) -> Result<String, BuildError> {
    crate::path::tlv_to_path(&fwd.src)
}

/* ------------------------------------------------------------- reply errors --- */

/// The ERROR TLV of a `kind=ERROR` reply's `STATUS{ ERROR }` payload, or `None`.
fn reply_error_tlv(reply: &ParsedFwd) -> Option<&Tlv> {
    let status = reply.payload.as_ref()?;
    if status.type_code == type_code::STATUS {
        return status.first_child(type_code::ERROR);
    }
    None
}

/// The registered wire ERROR code of a `kind=ERROR` reply's
/// `STATUS{ ERROR{ VALUE u16 } }` payload (RFC-0002), or 0 when absent or when
/// the ERROR carries the string-form identity (see [`reply_error_path`]).
pub fn reply_error_code(reply: &ParsedFwd) -> u16 {
    if let Some(err) = reply_error_tlv(reply) {
        if let Some(id) = err.children.first() {
            if id.type_code == type_code::VALUE && id.payload.len() >= 2 {
                return id.payload[0] as u16 | ((id.payload[1] as u16) << 8);
            }
        }
    }
    0
}

/// The string-form `tr::…` identity of a `kind=ERROR` reply's
/// `STATUS{ ERROR{ NAME } }` payload (RFC-0002), or `None` when the reply carries
/// no ERROR or a registered-code identity (see [`reply_error_code`]).
///
/// # Errors
/// [`BuildError::InvalidUtf8`] if the NAME identity is not valid UTF-8.
pub fn reply_error_path(reply: &ParsedFwd) -> Result<Option<String>, BuildError> {
    if let Some(err) = reply_error_tlv(reply) {
        if let Some(id) = err.children.first() {
            if id.type_code == type_code::NAME {
                return Ok(Some(id.payload_str()?.into()));
            }
        }
    }
    Ok(None)
}

/// Convenience: build a `dst`/`src` PATH from a rooted string (for callers that
/// have string paths rather than segment slices).
///
/// # Errors
/// A path validation error.
pub fn path_tlv_from_str(text: &str) -> Result<Tlv, BuildError> {
    path_to_tlv(text)
}
