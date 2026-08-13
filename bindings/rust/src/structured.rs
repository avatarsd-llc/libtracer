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
use crate::tlv_builders::{
    name, subscriber, text_name, value, value_u16, value_u32, value_u64, value_u8, BuildError,
};
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
 * @brief Read the NAME-tagged fields of any structured TLV as positional `(NAME key,
 * value)` PAIRS — the C++ `wire::config_reader_t` mechanics (#927), mirrored here so
 * both cores parse one grammar (#995).
 *
 * The walk is **pair-consuming**: it steps two children at a time, so a value child is
 * never re-read as the next position's key. A child that is not a `NAME` where a key
 * belongs desynchronizes the pair stream and the walk STOPS there rather than guessing
 * a resync — the every-offset scan this replaced made the grammar ambiguous (a pair
 * whose value textually equalled a known key bound the following child as that key's
 * value). A trailing unpaired `NAME` is ignored. Duplicate keys are preserved in wire
 * order: which occurrence wins — or whether a repeat is rejected outright — is each
 * consumer's disposition, not the walk's (#995: plain NAME-field readers are last-wins;
 * security readers reject).
 *
 * Covers SETTINGS, SPEC, and the `router-wrapped` envelope. The ROUTER (`0x0D`) code
 * is a reserved codepoint with no implemented mechanism in any core; when one lands,
 * its reader joins the REJECT family (the #995 ruling), but until then this walk only
 * enumerates its pairs.
 *
 * # Errors
 * [`BuildError::InvalidUtf8`] on a non-UTF-8 NAME key.
 */
pub fn named_fields(tlv: &Tlv) -> Result<Vec<NamedField>, BuildError> {
    let mut out = Vec::new();
    let ch = &tlv.children;
    let mut i = 0;
    while i + 1 < ch.len() {
        if ch[i].type_code != type_code::NAME {
            break;
        }
        out.push(NamedField {
            key: ch[i].payload_str()?.to_string(),
            value: ch[i + 1].clone(),
        });
        i += 2;
    }
    Ok(out)
}

/**
 * @brief The value TLV of the LAST NAME-tagged field with the given key, or `None` —
 * the plain NAME-field family disposition (#995): pair-consuming + last-occurrence-wins,
 * matching the C++ termini of this grammar (`config_reader_t`, `create_child`,
 * `parse_subscriber_tlv`).
 *
 * Type-agnostic: "last" here means the last PAIR carrying the key. A typed reader
 * ([`settings_str`], [`spec_type_name`]) additionally requires its value TYPE, so a
 * wrong-typed later occurrence never clobbers a well-formed earlier one there.
 *
 * # Errors
 * [`BuildError::InvalidUtf8`] on a non-UTF-8 NAME key while scanning.
 */
pub fn named_field(tlv: &Tlv, key: &str) -> Result<Option<Tlv>, BuildError> {
    Ok(named_fields(tlv)?
        .into_iter()
        .rev()
        .find(|f| f.key == key)
        .map(|f| f.value))
}

/* ---------------------------------------------------------------- SETTINGS --- */

/**
 * @brief The value a SETTINGS key carries — and, inseparably, the TLV **type** that
 * spells it. The two forms are not interchangeable: a reader looks a key up BY type,
 * so a string written as [`SettingValue::Value`] is invisible where a string is
 * expected, and vice versa. Vector-pinned: `spec/conn-client-ws` carries both in one
 * record (`role`/`port` opaque, `kind`/`addr` textual).
 */
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SettingValue<'a> {
    /** @brief An opaque `VALUE` child (`0x01`): integers (little-endian), flags, blobs. */
    Value(&'a [u8]),
    /** @brief A UTF-8 `NAME` child (`0x02`): the form a string-valued key MUST take. */
    Name(&'a str),
}

/** @brief The SETTINGS container around an already-built run of key/value children. */
fn settings_of(children: Vec<Tlv>) -> Tlv {
    Tlv {
        type_code: type_code::SETTINGS,
        opt: Opt::structured(),
        payload: Vec::new(),
        children,
        trailer: None,
    }
}

/**
 * @brief Build a SETTINGS TLV (`type=0x0B`, `PL=1`) from `NAME` key → typed value pairs,
 * choosing the value TLV type per key. Vector-pinned: `spec/conn-client-ws`.
 *
 * # Errors
 * A per-key or per-value segment error from [`name`] / [`text_name`].
 */
pub fn settings_typed(pairs: &[(&str, SettingValue<'_>)]) -> Result<Tlv, BuildError> {
    let mut children = Vec::with_capacity(pairs.len() * 2);
    for (key, val) in pairs {
        let node = match val {
            SettingValue::Value(bytes) => value(bytes),
            SettingValue::Name(text) => text_name(text)?,
        };
        let [k, v] = named_value(key, node)?;
        children.push(k);
        children.push(v);
    }
    Ok(settings_of(children))
}

/**
 * @brief Build a SETTINGS TLV (`type=0x0B`, `PL=1`) from `NAME`→opaque-VALUE pairs — the
 * all-[`SettingValue::Value`] case of [`settings_typed`]. Vector-pinned:
 * `settings-reliability` (`SETTINGS{ NAME "reliability", VALUE u8=1 }`).
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
    Ok(settings_of(children))
}

/**
 * @brief The opaque value bytes of a SETTINGS key, or `None`. On a repeated key the
 * LAST occurrence wins ([`named_field`]'s #995 family disposition).
 *
 * @note Type-agnostic by design — it hands back whatever payload the value child holds.
 *       Use [`settings_str`] for a key a reader looks up as a string, so the value TYPE
 *       is checked and not merely the bytes.
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

/**
 * @brief The string value of a SETTINGS key — present only when the value child is a
 * `NAME` (`0x02`), the one form a string-valued key is read back from. A key whose
 * value is an opaque `VALUE` reads as `None` here, matching the terminus's **type**
 * check (`config_reader_t`).
 *
 * Within the matching pairs the LAST **well-formed** (`NAME`-typed) occurrence wins,
 * and a wrong-typed occurrence never clobbers a well-formed one — `SETTINGS{ NAME
 * "kind" VALUE …, NAME "kind" NAME "ws" }` reads `"ws"`, exactly as at the terminus
 * (#995; the shared `settings/duplicate-key-last-wins` vector pins both cores).
 *
 * # Errors
 * [`BuildError::TypeMismatch`] if the TLV is not a SETTINGS; [`BuildError::InvalidUtf8`]
 * on a non-UTF-8 key or value.
 */
pub fn settings_str(tlv: &Tlv, key: &str) -> Result<Option<String>, BuildError> {
    if tlv.type_code != type_code::SETTINGS {
        return Err(BuildError::TypeMismatch);
    }
    for f in named_fields(tlv)?.into_iter().rev() {
        if f.key == key && f.value.type_code == type_code::NAME {
            return Ok(Some(f.value.payload_str()?.to_string()));
        }
    }
    Ok(None)
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

/** @brief The SETTINGS key the packed delivery policy travels under (RFC-0022 §3.A). */
pub const DELIVERY_POLICY_KEY: &str = "delivery_policy";

/**
 * @brief One subscription's DELIVERY policy — the packed 16-bit word RFC-0022 §3.A
 * carries in a SUBSCRIBER's `SETTINGS{ NAME "delivery_policy" VALUE u16 }` child.
 *
 * Delivery policy describes one **producer→subscriber relationship**, not the producer:
 * before RFC-0022 these lived on the vertex, where one setting decided the behaviour of
 * every subscriber at once. The bit layout mirrors the C++ core's `delivery_policy_t`
 * exactly, because the same bytes cross between them:
 *
 * | bits | field |
 * | --- | --- |
 * | 0–1 | `reliability` (0 = best-effort, 1 = reliable) |
 * | 2–4 | `priority` (0–7, 0 = default) |
 * | 5 | `durability_request` — deliver the producer's latched last value on join |
 * | 6–15 | reserved |
 *
 * The reserved bits are **round-tripped verbatim and never interpreted**: §3.A says a
 * sender MUST write 0 and a receiver MUST *ignore* them, which is an ignore, not a
 * reject. So [`bits`](Self::bits) keeps whatever arrived, while every accessor masks —
 * a future sender's bits survive a hop through this binding instead of being refused or
 * silently normalised away.
 */
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct DeliveryPolicy {
    /** @brief The packed word, VERBATIM — reserved bits included. */
    pub bits: u16,
}

impl DeliveryPolicy {
    /** @brief Bit 5: ask the producer to deliver its latched last value once, on join. */
    pub const DURABILITY_REQUEST: u16 = 0x0020;

    /** @brief Wrap a packed word as it arrived, keeping every reserved bit. */
    #[must_use]
    pub const fn from_bits(bits: u16) -> Self {
        Self { bits }
    }

    /** @brief Bits 0–1 — the reliability class. */
    #[must_use]
    pub const fn reliability(self) -> u8 {
        (self.bits & 0x0003) as u8
    }

    /** @brief Bits 2–4 — the priority class (0–7). */
    #[must_use]
    pub const fn priority(self) -> u8 {
        ((self.bits >> 2) & 0x0007) as u8
    }

    /** @brief Bit 5 — `durability_request`. */
    #[must_use]
    pub const fn durability_request(self) -> bool {
        (self.bits & Self::DURABILITY_REQUEST) != 0
    }

    /** @brief Bits 6–15 — the reserved field, as it arrived. Never interpreted; exposed
     * so a caller can assert it survived a round trip. */
    #[must_use]
    pub const fn reserved(self) -> u16 {
        self.bits >> 6
    }

    /** @brief True when no honoured bit is set AND nothing is reserved — the "absent"
     * case, which emits no `SETTINGS` child at all. */
    #[must_use]
    pub const fn is_default(self) -> bool {
        self.bits == 0
    }
}

/**
 * @brief The delivery policy a SUBSCRIBER carries, or [`DeliveryPolicy::default`] when it
 * names none — absent ⇒ all-zero ⇒ today's behaviour (RFC-0022 §3.A).
 *
 * A `SETTINGS` child that names a DIFFERENT key (e.g. `delivery_compact`) is not the
 * policy: the word must be read by name, never by position. Returning the default for
 * such a record is what the `subscriber/policy-absent` vector pins.
 *
 * The walk is the plain NAME-field family's (#995), exactly the C++
 * `parse_subscriber_tlv` → `config_reader_t` rule: the LAST **well-formed** occurrence
 * (a 2-byte `VALUE`) wins, and an ill-formed one — wrong TLV type or wrong width — is
 * SKIPPED rather than rejected, so it neither errors out the read nor clobbers a
 * well-formed earlier word. The shared `subscriber/policy-last-wins` vector pins both
 * cores.
 *
 * # Errors
 * [`BuildError::TypeMismatch`] if the TLV is not a SUBSCRIBER.
 */
pub fn subscriber_policy(tlv: &Tlv) -> Result<DeliveryPolicy, BuildError> {
    if tlv.type_code != type_code::SUBSCRIBER {
        return Err(BuildError::TypeMismatch);
    }
    let mut policy = DeliveryPolicy::default();
    for child in tlv.children_of_type(type_code::SETTINGS) {
        for f in named_fields(child)? {
            if f.key != DELIVERY_POLICY_KEY {
                continue;
            }
            if f.value.type_code != type_code::VALUE || f.value.payload.len() != 2 {
                continue;
            }
            policy = DeliveryPolicy::from_bits(u16::from_le_bytes([
                f.value.payload[0],
                f.value.payload[1],
            ]));
        }
    }
    Ok(policy)
}

/**
 * @brief Build a SUBSCRIBER TLV carrying a target PATH and this subscription's packed
 * delivery policy (RFC-0022 §3.A) — the payload of a subscribe-write.
 *
 * An all-zero policy emits **no** `SETTINGS` child, so the bytes are byte-identical to
 * [`crate::subscriber`]'s and to what a pre-RFC-0022 sender emits (the
 * `subscriber/policy-absent` vector). Reserved bits in `policy` are emitted verbatim.
 *
 * # Errors
 * Any error from building the target path.
 */
pub fn subscriber_with_policy(
    target_path: &[&str],
    policy: DeliveryPolicy,
) -> Result<Tlv, BuildError> {
    let mut tlv = subscriber(target_path)?;
    if !policy.is_default() {
        tlv.children.push(Tlv {
            type_code: type_code::SETTINGS,
            opt: Opt::structured(),
            payload: Vec::new(),
            children: alloc::vec![name(DELIVERY_POLICY_KEY)?, value_u16(policy.bits)],
            trailer: None,
        });
    }
    Ok(tlv)
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
    /** @brief `subject` token bytes (e.g. `"peer-a"`, or the one special subject `"EVERYONE@"`). */
    pub subject: Vec<u8>,
    /** @brief `access_mask` u32 bitfield (READ=0x01, WRITE=0x02, SUBSCRIBE=0x04, …) —
     * canonical wire width per RFC-0026. */
    pub access_mask: u32,
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
        push_named(&mut children, "access_mask", value_u32(self.access_mask));
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
 * STRICT — the security-reader family of the #995 ruling, matching the C++
 * `graph::parse_acl` (#906): an ACL is a security document, so a shape the builder
 * never emits is REJECTED rather than read leniently, because leniency here does not
 * lose a field — it inverts or widens a grant. Per ACE, `TypeMismatch` on: an odd
 * child count (an unpaired key or value), a non-`NAME` child in a key slot (a
 * desynchronized pair stream), an UNKNOWN key (a silently dropped attribute would
 * widen access), a REPEATED key (last-wins would let `access_mask` narrow-then-wide
 * widen the grant — the shared `acl/ace-duplicate-key` vector pins the refusal in
 * both cores), a numeric field whose value is not a `VALUE`, an empty `subject`, and
 * a missing required `type` / `subject` / `access_mask`.
 *
 * What is deliberately NOT mirrored here: the terminus's policy-tier checks (DENY
 * acceptance, the flag-bit subset, per-field width caps) — those gate what a graph
 * STORES, not what the codec reads; this crate has no policy to evaluate. The
 * `access_mask` width is the canonical u32 of RFC-0026 (#993), read without
 * narrowing.
 *
 * # Errors
 * [`BuildError::TypeMismatch`] per the reject rules above;
 * [`BuildError::InvalidUtf8`] on a non-UTF-8 key.
 */
pub fn acl_aces(tlv: &Tlv) -> Result<Vec<Ace>, BuildError> {
    if tlv.type_code != type_code::ACL {
        return Err(BuildError::TypeMismatch);
    }
    let mut out = Vec::new();
    for inner in tlv.children_of_type(type_code::ACL) {
        let ch = &inner.children;
        if ch.len() % 2 != 0 {
            return Err(BuildError::TypeMismatch);
        }
        let mut ace_type: Option<u8> = None;
        let mut flags: Option<u8> = None;
        let mut subject: Option<Vec<u8>> = None;
        let mut access_mask: Option<u32> = None;
        let mut expires_ns: Option<u64> = None;
        let mut i = 0;
        while i + 1 < ch.len() {
            if ch[i].type_code != type_code::NAME {
                return Err(BuildError::TypeMismatch);
            }
            let val = &ch[i + 1];
            // Every arm rejects a repeat (dup .is_some()) — the security disposition.
            match ch[i].payload_str()? {
                "type" => {
                    if ace_type.is_some() || val.type_code != type_code::VALUE {
                        return Err(BuildError::TypeMismatch);
                    }
                    ace_type = Some(val.payload_uint() as u8);
                }
                "flags" => {
                    if flags.is_some() || val.type_code != type_code::VALUE {
                        return Err(BuildError::TypeMismatch);
                    }
                    flags = Some(val.payload_uint() as u8);
                }
                "subject" => {
                    // Opaque bytes (VALUE recommended; NAME for the "EVERYONE@"
                    // spelling) — any non-empty payload, like the terminus.
                    if subject.is_some() || val.payload.is_empty() {
                        return Err(BuildError::TypeMismatch);
                    }
                    subject = Some(val.payload.clone());
                }
                "access_mask" => {
                    if access_mask.is_some() || val.type_code != type_code::VALUE {
                        return Err(BuildError::TypeMismatch);
                    }
                    // The full canonical u32 (RFC-0026) — narrowing here would silently
                    // drop the high rights bits a wider mask grants.
                    access_mask = Some(val.payload_uint() as u32);
                }
                "expires_ns" => {
                    if expires_ns.is_some() || val.type_code != type_code::VALUE {
                        return Err(BuildError::TypeMismatch);
                    }
                    expires_ns = Some(val.payload_uint());
                }
                _ => return Err(BuildError::TypeMismatch),
            }
            i += 2;
        }
        let (Some(ace_type), Some(subject), Some(access_mask)) = (ace_type, subject, access_mask)
        else {
            return Err(BuildError::TypeMismatch);
        };
        out.push(Ace {
            ace_type,
            flags: flags.unwrap_or(0),
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
 * Vector-pinned: `spec/create-child`, `spec/conn-client-ws`.
 *
 * Both field VALUES are `NAME` (`0x02`) nodes, not `VALUE` (`0x01`): the terminus
 * matches each `(NAME key, value)` pair on the value's TYPE and skips any other,
 * so a `VALUE`-typed `type`/`name` leaves the selector empty and the create is
 * refused outright with `INVALID_PATH`. The two spellings are not interchangeable
 * — and a core that emits the wrong one round-trips its own bytes green, which is
 * why the vectors above exist.
 *
 * The two fields are checked differently because the terminus checks them
 * differently: `name` becomes a path component, so it must satisfy the addressing
 * grammar ([`name`]), while `type` is a catalog selector that is never addressed
 * and only has to be non-empty ([`text_name`]).
 *
 * @note [`name`] and the C++ `valid_segment` it mirrors agree on the full
 *       seven-character reserved set `/ : . [ ] * ?` (#996 closed the drift where
 *       C++ admitted `[` / `]`), pinned cross-tier by the
 *       `path/path-reserved-brackets` conformance vector. So a `child_name` this
 *       builder refuses is one every conforming terminus refuses too.
 *
 * # Errors
 * A segment error from a NAME key, from a `type` outside the 64-byte budget, or
 * from a `child_name` that is not a legal address segment.
 */
pub fn spec(type_sel: &str, child_name: &str, config: Option<Tlv>) -> Result<Tlv, BuildError> {
    let mut children = Vec::new();
    let [k1, v1] = named_value("type", text_name(type_sel)?)?;
    children.push(k1);
    children.push(v1);
    let [k2, v2] = named_value("name", name(child_name)?)?;
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
 * pair of UTF-8 strings; either is `None` when its field is absent — or present
 * with a value child that is not a `NAME`, which the terminus likewise treats as
 * absent. Reading the type as well as the bytes is what stops this reader from
 * accepting the `VALUE`-typed spelling the terminus refuses.
 *
 * The walk is [`named_fields`]' pair-consuming one (#995), the same rule
 * `create_child` runs: a SPEC whose children begin with a stray non-`NAME` (the
 * shared `spec/desync-stray-value` vector) reads BOTH fields absent here, exactly as
 * the terminus — which refuses the create with `INVALID_PATH` — never sees them. On
 * a repeated key the last `NAME`-typed occurrence wins, and a wrong-typed occurrence
 * never clobbers a well-formed one.
 *
 * # Errors
 * [`BuildError::TypeMismatch`] if the TLV is not a SPEC, or a non-UTF-8 field.
 */
pub fn spec_type_name(tlv: &Tlv) -> Result<(Option<String>, Option<String>), BuildError> {
    if tlv.type_code != type_code::SPEC {
        return Err(BuildError::TypeMismatch);
    }
    let named_str = |key: &str| -> Result<Option<String>, BuildError> {
        for f in named_fields(tlv)?.into_iter().rev() {
            if f.key == key && f.value.type_code == type_code::NAME {
                return Ok(Some(f.value.payload_str()?.to_string()));
            }
        }
        Ok(None)
    };
    Ok((named_str("type")?, named_str("name")?))
}
