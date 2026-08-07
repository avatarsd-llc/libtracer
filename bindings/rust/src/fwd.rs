// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/*!
 * @brief FWD remote-operation envelope — FWD TLV (`0x0F`) build/parse (Unit 4).
 *
 * The source-routed remote-operation frame (RFC-0004 §B/§D, ADR-0035). Mirrors
 * `bindings/typescript/packages/client/src/fwd.ts` and the terminus resolver of
 * `core/src/op_resolve.cpp`. Child order (RFC-0004 §B): `op`, `dst` (PATH),
 * `FIELD?` selector, `src` (PATH), then per-op:
 *   - REPLY: `kind` (VALUE u8), optional payload
 *   - WRITE: optional payload
 *   - AWAIT: optional `await_timeout` (VALUE u64 ns)
 *   - READ : nothing
 *
 * Vector-pinned: all nine vectors under `fwd/`.
 */

use alloc::string::String;
use alloc::vec::Vec;

use crate::error_registry::ErrCode;
use crate::field::{field_tlv, parse_field, FieldLevel};
use crate::path::path_to_tlv;
use crate::tlv_builders::{path as build_path, value_u64, value_u8, BuildError};
use crate::{type_code, Opt, Tlv};

/** @brief FWD `op` discriminant (RFC-0004 §B — the first child, a VALUE u8). */
pub mod fwd_op {
    /** @brief Read a vertex's last-known value. */
    pub const READ: u8 = 0;
    /** @brief Write a value / field (subscribe is a `:subscribers[]` field-write). */
    pub const WRITE: u8 = 1;
    /** @brief Await the next write, up to an optional timeout. */
    pub const AWAIT: u8 = 2;
    /** @brief A reply routed back along the accumulated `src`. */
    pub const REPLY: u8 = 3;

    /**
     * @brief The opcode field of the `op` byte — bits 5-0 (RFC-0024 §7.5).
     *
     * Bits 7-6 are FLAGS. A forwarder MUST mask before switching (RFC-0024 §9.3): an
     * unrecognised flag then degrades to the plain opcode instead of an unknown-opcode
     * reject, which is what makes a flag additive at all.
     */
    pub const OPCODE_MASK: u8 = 0x3F;

    /**
     * @brief `op` bit 7 — the bound-path MINT REQUEST (RFC-0024 §7.5).
     *
     * Set by an origin asking each host on the route to answer with its own vertex ref. It
     * costs ZERO request bytes: there is no free TLV `opt` bit, and a dedicated presence
     * child would spend a 4-byte header to express one. The request is a hint, never an
     * obligation — a host that will not mint answers the ordinary reply.
     */
    pub const FLAG_MINT_REQUEST: u8 = 0x80;
}

/** @brief FWD{REPLY} `kind` discriminant (RFC-0004 §D — a VALUE u8 after `src`). */
pub mod fwd_kind {
    /** @brief A successful result (payload is the RESULT value). */
    pub const RESULT: u8 = 0;
    /** @brief A failure (payload is `STATUS{ ERROR }`). */
    pub const ERROR: u8 = 1;
}

/**
 * @brief The registered wire ERROR code of a `tr::…` path, or 0 when unregistered.
 * The FWD error-path table is [`ErrCode`] itself (RFC-0002 §D).
 */
pub fn fwd_error_code_for_path(path: &str) -> u16 {
    ErrCode::from_path(path).map(ErrCode::code).unwrap_or(0)
}

/** @brief The canonical `tr::…` path of a registered wire ERROR code, or `None`. */
pub fn fwd_error_path(code: u16) -> Option<&'static str> {
    ErrCode::from_code(code).map(ErrCode::path)
}

/* -------------------------------------------------------------- FWD request --- */

/** @brief A `:field` selector for a FWD request: a raw string (parsed) or pre-built levels. */
pub enum FieldSel<'a> {
    /** @brief A `:field` selector string (see [`parse_field`]). */
    Str(&'a str),
    /** @brief Pre-parsed selector levels. */
    Levels(Vec<FieldLevel>),
}

/** @brief A request to build a FWD frame. `op` selects the trailing-child layout. */
pub struct FwdRequest<'a> {
    /** @brief The operation (a [`fwd_op`] value). */
    pub op: u8,
    /** @brief Forward route: the unresolved destination path segments (the PATH `dst`). */
    pub dst: &'a [&'a str],
    /** @brief Accumulated return route: the originator's reply-endpoint segments (`src`). */
    pub src: &'a [&'a str],
    /** @brief Optional `:field` selector. */
    pub field: Option<FieldSel<'a>>,
    /**
     * @brief Optional payload node (a VALUE to WRITE, a SUBSCRIBER to subscribe, the
     * RESULT/STATUS of a REPLY). Embedded verbatim.
     */
    pub payload: Option<Tlv>,
    /** @brief REPLY only — the reply [`fwd_kind`] (defaults to RESULT). */
    pub kind: Option<u8>,
    /** @brief AWAIT only — the `await_timeout` in ns (absent ⇒ the responder default). */
    pub await_timeout_ns: Option<u64>,
}

impl<'a> FwdRequest<'a> {
    /** @brief A minimal request: `op`, `dst`, `src`; all optional fields cleared. */
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

/**
 * @brief Build a FWD TLV node (`type=0x0F`, `PL=1`) — the remote-operation envelope.
 * Children are assembled in the normative order for the request's `op`.
 * Vector-pinned: `fwd-read`, `fwd-write-value`, `fwd-await-timeout`,
 * `fwd-write-subscriber-field`, `fwd-reply-result`, `fwd-reply-error`,
 * `fwd-routed-mount-residual`, `fwd-routed-two-mount`, `fwd-src-accumulated`,
 * `fwd-wildcard-reject`.
 *
 * # Errors
 * Any error from building the `dst`/`src` PATHs, the FIELD selector, or an
 * invalid `op`.
 */
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

/**
 * @brief Convenience: build a FWD frame directly to encoded bytes.
 *
 * # Errors
 * Any error from [`encode_fwd`].
 */
pub fn encode_fwd_bytes(req: &FwdRequest) -> Result<Vec<u8>, BuildError> {
    Ok(crate::encode(&encode_fwd(req)?))
}

/* --------------------------------------------------------------- FWD parse --- */

/** @brief A parsed FWD frame, its children read positionally (RFC-0004 §B order). */
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ParsedFwd {
    /** @brief The operation (a [`fwd_op`] value). */
    pub op: u8,
    /** @brief The `dst` route child — a `PATH`, or a `PATH_REF` when [`Self::dst_bound`]. */
    pub dst: Tlv,
    /** @brief The optional FIELD selector child. */
    pub field: Option<Tlv>,
    /** @brief The `src` route child — a `PATH`, or a `PATH_REF` on a reply to a bound op. */
    pub src: Tlv,
    /** @brief The reply [`fwd_kind`] (REPLY frames only). */
    pub kind: Option<u8>,
    /** @brief The payload TLV child (WRITE payload / REPLY result), if present. */
    pub payload: Option<Tlv>,
    /** @brief The AWAIT `await_timeout` ns, if present. */
    pub await_timeout_ns: Option<u64>,
    /** @brief `op` bit 7 was set — the origin asked for a bound-path mint (RFC-0024 §7.5). */
    pub mint_request: bool,
    /** @brief `dst` is a `PATH_REF` (`0x14`), not a canonical `PATH` (RFC-0024 §4). */
    pub dst_bound: bool,
}

fn u8_of(t: &Tlv) -> u8 {
    t.payload.first().copied().unwrap_or(0)
}

/**
 * @brief Parse an already-decoded FWD [`Tlv`] positionally (RFC-0004 §B).
 *
 * # Errors
 * [`BuildError::TypeMismatch`] if the TLV is not a FWD or is missing a required
 * `op` / `dst` / `src` child.
 */
pub fn parse_fwd_tlv(fwd: &Tlv) -> Result<ParsedFwd, BuildError> {
    if fwd.type_code != type_code::FWD {
        return Err(BuildError::TypeMismatch);
    }
    let ch = &fwd.children;
    let mut i = 0;
    if ch.get(i).map(|c| c.type_code) != Some(type_code::VALUE) {
        return Err(BuildError::TypeMismatch);
    }
    // The opcode is `op & 0x3F`; bits 7-6 are flags (RFC-0024 §7.5, normative in §9.3).
    // Switching on the raw byte would make a mint-flagged READ an unknown opcode here.
    let op_byte = u8_of(&ch[i]);
    let op = op_byte & fwd_op::OPCODE_MASK;
    let mint_request = op_byte & fwd_op::FLAG_MINT_REQUEST != 0;
    i += 1;
    // Two address forms (RFC-0024 §4): a canonical PATH, or a bound PATH_REF whose body
    // shape the grammar has already checked. What an element MEANS is node-scoped, so a
    // binding core carries one and never interprets it.
    let dst_bound = ch.get(i).map(|c| c.type_code) == Some(type_code::PATH_REF);
    if !dst_bound && ch.get(i).map(|c| c.type_code) != Some(type_code::PATH) {
        return Err(BuildError::TypeMismatch);
    }
    let dst = ch[i].clone();
    i += 1;
    let mut field = None;
    if ch.get(i).map(|c| c.type_code) == Some(type_code::FIELD) {
        field = Some(ch[i].clone());
        i += 1;
    }
    // `src` takes either form too: a reply to a bound operation echoes the request's `dst`.
    let src_kind = ch.get(i).map(|c| c.type_code);
    if src_kind != Some(type_code::PATH) && src_kind != Some(type_code::PATH_REF) {
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
        mint_request,
        dst_bound,
    })
}

/**
 * @brief Decode a complete FWD frame's bytes into its parsed children.
 *
 * # Errors
 * [`BuildError::TypeMismatch`] if the frame is not a decodable FWD (a codec
 * fault surfaces as `TypeMismatch`; use [`crate::decode`] directly for the
 * underlying [`crate::Error`]).
 */
pub fn decode_fwd(bytes: &[u8]) -> Result<ParsedFwd, BuildError> {
    let tlv = crate::decode(bytes).map_err(|_| BuildError::TypeMismatch)?;
    parse_fwd_tlv(&tlv)
}

/**
 * @brief The `dst` route of a parsed FWD in string form.
 *
 * # Errors
 * A PATH-decode error.
 */
pub fn fwd_dst_path(fwd: &ParsedFwd) -> Result<String, BuildError> {
    crate::path::tlv_to_path(&fwd.dst)
}

/**
 * @brief The `src` (accumulated return route) of a parsed FWD in string form.
 *
 * # Errors
 * A PATH-decode error.
 */
pub fn fwd_src_path(fwd: &ParsedFwd) -> Result<String, BuildError> {
    crate::path::tlv_to_path(&fwd.src)
}

/* ------------------------------------------------------------- reply errors --- */

/**
 * @brief The ERROR TLV of a `kind=ERROR` reply's `STATUS{ ERROR }` payload, or `None`.
 *
 * The **first ERROR child of the STATUS, at whatever position** — never `children[0]`.
 * reference/05 §`0x09` gives a STATUS "one or more ERROR TLVs and optional DESCRIPTION
 * text" and pins no order over them; RFC-0002 §C pins position only one level down,
 * INSIDE the ERROR ("its first child is the identity"). A reader that demands position 0
 * refuses a documented STATUS shape and answers code 0, which is what it also answers for
 * a STATUS carrying no ERROR at all.
 *
 * Vector-pinned by `fwd/fwd-reply-error-after-description` (the DESCRIPTION written
 * first), which the TypeScript binding pins against the same bytes so the two cores
 * cannot drift apart on it again (#878).
 */
fn reply_error_tlv(reply: &ParsedFwd) -> Option<&Tlv> {
    let status = reply.payload.as_ref()?;
    if status.type_code == type_code::STATUS {
        return status.first_child(type_code::ERROR);
    }
    None
}

/**
 * @brief The registered wire ERROR code of a `kind=ERROR` reply's
 * `STATUS{ ERROR{ VALUE u16 } }` payload (RFC-0002), or 0 when absent or when
 * the ERROR carries the string-form identity (see [`reply_error_path`]).
 */
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

/**
 * @brief The string-form `tr::…` identity of a `kind=ERROR` reply's
 * `STATUS{ ERROR{ NAME } }` payload (RFC-0002), or `None` when the reply carries
 * no ERROR or a registered-code identity (see [`reply_error_code`]).
 *
 * # Errors
 * [`BuildError::InvalidUtf8`] if the NAME identity is not valid UTF-8.
 */
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

/**
 * @brief Convenience: build a `dst`/`src` PATH from a rooted string (for callers that
 * have string paths rather than segment slices).
 *
 * # Errors
 * A path validation error.
 */
pub fn path_tlv_from_str(text: &str) -> Result<Tlv, BuildError> {
    path_to_tlv(text)
}
