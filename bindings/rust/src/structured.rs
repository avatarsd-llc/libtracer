// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/*!
 * @brief Structured-TLV typed accessors + builders (Unit 6).
 *
 * Typed read/build for the core structured containers: POINT (`0x07`), SETTINGS
 * (`0x0B`), ACL (`0x0A`, NFSv4-style ACEs), SUBSCRIBER (`0x04`), SPEC (`0x0E`),
 * plus a generic NAME-tagged field reader that also covers the ROUTER (`0x0D`)
 * envelope. Byte layouts per reference/05. Vector-pinned: `settings-reliability`,
 * `subscriber-path`, `router-wrapped`, `acl-aces`.
 */

use alloc::string::{String, ToString};
use alloc::vec::Vec;

use crate::path::tlv_to_path;
use crate::tlv_builders::{name, value, value_u16, value_u64, value_u8, BuildError};
use crate::{type_code, Opt, Tlv};

fn named_value(key: &str, val: Tlv) -> Result<[Tlv; 2], BuildError> {
    Ok([name(key)?, val])
}

/* -------------------------------------------------------------- NAME-tagged --- */

/**
 * @brief A NAME-tagged field: a NAME key paired with the TLV that follows it. The
 * generic shape of SETTINGS, SPEC, an ACE's fields, and the ROUTER envelope.
 */
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NamedField {
    /** @brief The field key (the NAME child's UTF-8 payload). */
    pub key: String,
    /** @brief The value TLV that follows the NAME key. */
    pub value: Tlv,
}

/**
 * @brief Read the NAME-tagged fields of any structured TLV: each `NAME` child is paired
 * with the immediately-following child as its value. A trailing `NAME` with no
 * following value is skipped. Covers `router-wrapped`, SETTINGS, SPEC.
 *
 * # Errors
 * [`BuildError::InvalidUtf8`] on a non-UTF-8 NAME key.
 */
pub fn named_fields(tlv: &Tlv) -> Result<Vec<NamedField>, BuildError> {
    let mut out = Vec::new();
    let ch = &tlv.children;
    let mut i = 0;
    while i < ch.len() {
        if ch[i].type_code == type_code::NAME && i + 1 < ch.len() {
            out.push(NamedField {
                key: ch[i].payload_str()?.to_string(),
                value: ch[i + 1].clone(),
            });
            i += 2;
            continue;
        }
        i += 1;
    }
    Ok(out)
}

/**
 * @brief The value TLV of the first NAME-tagged field with the given key, or `None`.
 *
 * # Errors
 * [`BuildError::InvalidUtf8`] on a non-UTF-8 NAME key while scanning.
 */
pub fn named_field(tlv: &Tlv, key: &str) -> Result<Option<Tlv>, BuildError> {
    Ok(named_fields(tlv)?
        .into_iter()
        .find(|f| f.key == key)
        .map(|f| f.value))
}

/* ---------------------------------------------------------------- SETTINGS --- */

/**
 * @brief Build a SETTINGS TLV (`type=0x0B`, `PL=1`) from `NAME`→opaque-VALUE pairs.
 * Vector-pinned: `settings-reliability` (`SETTINGS{ NAME "reliability", VALUE u8=1 }`).
 *
 * # Errors
 * A per-key segment error from [`name`].
 */
pub fn settings(pairs: &[(&str, &[u8])]) -> Result<Tlv, BuildError> {
    let mut children = Vec::with_capacity(pairs.len() * 2);
    for (key, val) in pairs {
        let [k, v] = named_value(key, value(val))?;
        children.push(k);
        children.push(v);
    }
    Ok(Tlv {
        type_code: type_code::SETTINGS,
        opt: Opt::structured(),
        payload: Vec::new(),
        children,
        trailer: None,
    })
}

/**
 * @brief The opaque value bytes of a SETTINGS key, or `None`.
 *
 * # Errors
 * [`BuildError::TypeMismatch`] if the TLV is not a SETTINGS, or a non-UTF-8 key.
 */
pub fn settings_get(tlv: &Tlv, key: &str) -> Result<Option<Vec<u8>>, BuildError> {
    if tlv.type_code != type_code::SETTINGS {
        return Err(BuildError::TypeMismatch);
    }
    Ok(named_field(tlv, key)?.map(|v| v.payload))
}

/* -------------------------------------------------------------- SUBSCRIBER --- */

/**
 * @brief The target PATH TLV of a SUBSCRIBER (`0x04`), or `None`.
 *
 * # Errors
 * [`BuildError::TypeMismatch`] if the TLV is not a SUBSCRIBER.
 */
pub fn subscriber_target(tlv: &Tlv) -> Result<Option<&Tlv>, BuildError> {
    if tlv.type_code != type_code::SUBSCRIBER {
        return Err(BuildError::TypeMismatch);
    }
    Ok(tlv.first_child(type_code::PATH))
}

/**
 * @brief The target path of a SUBSCRIBER in string form (`"/sensor/temp"`), or `None`
 * when the subscriber has no target PATH (the unsubscribe sentinel).
 *
 * # Errors
 * [`BuildError::TypeMismatch`] if the TLV is not a SUBSCRIBER, or a PATH-decode
 * error.
 */
pub fn subscriber_target_path(tlv: &Tlv) -> Result<Option<String>, BuildError> {
    match subscriber_target(tlv)? {
        Some(p) => Ok(Some(tlv_to_path(p)?)),
        None => Ok(None),
    }
}

/* -------------------------------------------------------------------- POINT --- */

/**
 * @brief Build a POINT TLV (`type=0x07`, `PL=1`): a leading `NAME`, an optional own
 * `VALUE`, then any extra children (DESCRIPTION / SETTINGS / SUBSCRIBER / nested
 * POINT) in caller order (reference/05 §0x07).
 *
 * # Errors
 * A segment error from building the leading [`name`].
 */
pub fn point(
    vertex_name: &str,
    own_value: Option<&[u8]>,
    extra: &[Tlv],
) -> Result<Tlv, BuildError> {
    let mut children = Vec::new();
    children.push(name(vertex_name)?);
    if let Some(v) = own_value {
        children.push(value(v));
    }
    children.extend_from_slice(extra);
    Ok(Tlv {
        type_code: type_code::POINT,
        opt: Opt::structured(),
        payload: Vec::new(),
        children,
        trailer: None,
    })
}

/**
 * @brief The vertex name of a POINT (its first NAME child), or `None`.
 *
 * # Errors
 * [`BuildError::TypeMismatch`] if the TLV is not a POINT, or a non-UTF-8 name.
 */
pub fn point_name(tlv: &Tlv) -> Result<Option<String>, BuildError> {
    if tlv.type_code != type_code::POINT {
        return Err(BuildError::TypeMismatch);
    }
    match tlv.first_child(type_code::NAME) {
        Some(n) => Ok(Some(n.payload_str()?.to_string())),
        None => Ok(None),
    }
}

/**
 * @brief The own VALUE payload of a POINT (its first VALUE child), or `None`.
 *
 * # Errors
 * [`BuildError::TypeMismatch`] if the TLV is not a POINT.
 */
pub fn point_value(tlv: &Tlv) -> Result<Option<Vec<u8>>, BuildError> {
    if tlv.type_code != type_code::POINT {
        return Err(BuildError::TypeMismatch);
    }
    Ok(tlv.first_child(type_code::VALUE).map(|v| v.payload.clone()))
}

/**
 * @brief The nested child POINTs of a POINT (recursive vertex structure).
 *
 * # Errors
 * [`BuildError::TypeMismatch`] if the TLV is not a POINT.
 */
pub fn point_children(tlv: &Tlv) -> Result<Vec<&Tlv>, BuildError> {
    if tlv.type_code != type_code::POINT {
        return Err(BuildError::TypeMismatch);
    }
    Ok(tlv.children_of_type(type_code::POINT).collect())
}

/* ---------------------------------------------------------------------- ACL --- */

/**
 * @brief One NFSv4-style access-control entry (ADR-0020, reference/05 §0x0A). The wire
 * form is an inner `ACL` TLV of NAME-tagged fields.
 */
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Ace {
    /** @brief `type`: 0 = ALLOW, 1 = DENY. */
    pub ace_type: u8,
    /** @brief `flags` bitfield (INHERIT=0x1, INHERIT_ONLY=0x2, NO_PROPAGATE=0x4, GROUP=0x8). */
    pub flags: u8,
    /** @brief `subject` token bytes (e.g. `"peer-a"`, or special `"EVERYONE@"` / `"OWNER@"`). */
    pub subject: Vec<u8>,
    /** @brief `access_mask` u16 bitfield (READ=0x01, WRITE=0x02, SUBSCRIBE=0x04, …). */
    pub access_mask: u16,
    /** @brief Optional `expires_ns` u64 (absent ⇒ never expires). */
    pub expires_ns: Option<u64>,
}

impl Ace {
    /**
     * @brief Build the inner ACL TLV (one ACE): NAME-tagged `type`, `flags`, `subject`,
     * `access_mask`, and optional `expires_ns`, in that order.
     */
    fn to_tlv(&self) -> Tlv {
        let mut children = Vec::new();
        push_named(&mut children, "type", value_u8(self.ace_type));
        push_named(&mut children, "flags", value_u8(self.flags));
        push_named(&mut children, "subject", value(&self.subject));
        push_named(&mut children, "access_mask", value_u16(self.access_mask));
        if let Some(exp) = self.expires_ns {
            push_named(&mut children, "expires_ns", value_u64(exp));
        }
        Tlv {
            type_code: type_code::ACL,
            opt: Opt::structured(),
            payload: Vec::new(),
            children,
            trailer: None,
        }
    }
}

fn push_named(children: &mut Vec<Tlv>, key: &str, val: Tlv) {
    // Keys here are fixed ASCII literals (never reserved chars), so name() cannot
    // fail; expect() documents the invariant.
    children.push(name(key).expect("ACE field key is a valid segment"));
    children.push(val);
}

/**
 * @brief Build an ACL TLV (`type=0x0A`, `PL=1`): the outer ACE collection whose children
 * are inner ACL ACEs. Vector-pinned: `acl-aces`.
 */
pub fn acl(aces: &[Ace]) -> Tlv {
    Tlv {
        type_code: type_code::ACL,
        opt: Opt::structured(),
        payload: Vec::new(),
        children: aces.iter().map(Ace::to_tlv).collect(),
        trailer: None,
    }
}

/**
 * @brief Parse an ACL TLV into its ACEs (each inner ACL a NAME-tagged ACE record).
 *
 * # Errors
 * [`BuildError::TypeMismatch`] if the TLV is not an ACL or an ACE lacks the
 * required `type` / `subject` / `access_mask` fields.
 */
pub fn acl_aces(tlv: &Tlv) -> Result<Vec<Ace>, BuildError> {
    if tlv.type_code != type_code::ACL {
        return Err(BuildError::TypeMismatch);
    }
    let mut out = Vec::new();
    for inner in tlv.children_of_type(type_code::ACL) {
        let fields = named_fields(inner)?;
        let get = |k: &str| fields.iter().find(|f| f.key == k).map(|f| &f.value);
        let ace_type = get("type").ok_or(BuildError::TypeMismatch)?.payload_uint() as u8;
        let flags = get("flags").map(|v| v.payload_uint() as u8).unwrap_or(0);
        let subject = get("subject")
            .ok_or(BuildError::TypeMismatch)?
            .payload
            .clone();
        let access_mask = get("access_mask")
            .ok_or(BuildError::TypeMismatch)?
            .payload_uint() as u16;
        let expires_ns = get("expires_ns").map(|v| v.payload_uint());
        out.push(Ace {
            ace_type,
            flags,
            subject,
            access_mask,
            expires_ns,
        });
    }
    Ok(out)
}

/* --------------------------------------------------------------------- SPEC --- */

/**
 * @brief Build a SPEC TLV (`type=0x0E`, `PL=1`) for in-band vertex creation
 * (reference/05 §0x0E): NAME-tagged `type` (catalog selector) and `name` (the
 * new child's path component), plus an optional `config` SETTINGS child.
 *
 * # Errors
 * A segment error from a NAME key.
 */
pub fn spec(type_sel: &str, child_name: &str, config: Option<Tlv>) -> Result<Tlv, BuildError> {
    let mut children = Vec::new();
    let [k1, v1] = named_value("type", value(type_sel.as_bytes()))?;
    children.push(k1);
    children.push(v1);
    let [k2, v2] = named_value("name", value(child_name.as_bytes()))?;
    children.push(k2);
    children.push(v2);
    if let Some(cfg) = config {
        children.push(name("config")?);
        children.push(cfg);
    }
    Ok(Tlv {
        type_code: type_code::SPEC,
        opt: Opt::structured(),
        payload: Vec::new(),
        children,
        trailer: None,
    })
}

/**
 * @brief The `type` (catalog selector) and `name` (child component) of a SPEC, as a
 * pair of UTF-8 strings; either is `None` when its field is absent.
 *
 * # Errors
 * [`BuildError::TypeMismatch`] if the TLV is not a SPEC, or a non-UTF-8 field.
 */
pub fn spec_type_name(tlv: &Tlv) -> Result<(Option<String>, Option<String>), BuildError> {
    if tlv.type_code != type_code::SPEC {
        return Err(BuildError::TypeMismatch);
    }
    let type_sel = named_field(tlv, "type")?
        .map(|v| v.payload_str().map(|s| s.to_string()))
        .transpose()?;
    let child_name = named_field(tlv, "name")?
        .map(|v| v.payload_str().map(|s| s.to_string()))
        .transpose()?;
    Ok((type_sel, child_name))
}
