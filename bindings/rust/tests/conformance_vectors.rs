// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
/*!
 * @brief Unit 10 — in-crate conformance tests over the SHARED vectors
 * (tests/conformance/vectors/v1/). The `conformance` example already checks
 * encode(decode(input)) == input for every vector (round-trip only); this suite
 * closes that gap by loading BOTH input.bin AND expected.json and asserting the
 * DECODED STRUCTURE matches — the typed-tier builders/parsers (Units 1-6) must
 * reproduce and interpret every vector byte-for-byte, exactly like the C++ core
 * and the TypeScript client.
 *
 * Tests link std (file I/O). The library crate stays #![no_std]. Zero runtime
 * deps are preserved: the tiny JSON reader below extracts the `hex` /
 * `total_bytes` fields without pulling in serde.
 */

use std::fs;
use std::path::PathBuf;

use libtracer::error_registry::ErrorId;
use libtracer::field::FieldMode;
use libtracer::fwd::{fwd_kind, fwd_op, FieldSel};
use libtracer::structured::{self, Ace};
use libtracer::{
    decode, decode_fwd, encode, encode_field, encode_fwd, error_code, parse_error, parse_field_tlv,
    reply_error_code, status_ok, status_with_errors, subscriber, value, value_opts, value_u32,
    BuildError, ErrCode, FwdRequest, ValueOptions,
};

/* ----------------------------------------------------------------- helpers --- */

fn vectors_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../tests/conformance/vectors/v1")
}

/** @brief Read a vector's raw input bytes and its expected.json text. */
fn load(name: &str) -> (Vec<u8>, String) {
    let dir = vectors_dir().join(name);
    let bin =
        fs::read(dir.join("input.bin")).unwrap_or_else(|e| panic!("read {name}/input.bin: {e}"));
    let json = fs::read_to_string(dir.join("expected.json"))
        .unwrap_or_else(|e| panic!("read {name}/expected.json: {e}"));
    (bin, json)
}

fn from_hex(s: &str) -> Vec<u8> {
    let b = s.as_bytes();
    assert!(b.len() % 2 == 0, "odd hex length");
    let n = |c: u8| match c {
        b'0'..=b'9' => c - b'0',
        b'a'..=b'f' => c - b'a' + 10,
        b'A'..=b'F' => c - b'A' + 10,
        _ => panic!("bad hex digit {c}"),
    };
    b.chunks(2).map(|p| (n(p[0]) << 4) | n(p[1])).collect()
}

/**
 * @brief Extract a JSON string value by top-level-ish key (`"key": "value"`). The
 * vector `hex` fields are pure hex with no escapes, so a scan-to-quote suffices.
 */
fn json_str(text: &str, key: &str) -> String {
    let needle = format!("\"{key}\"");
    let start = text.find(&needle).unwrap_or_else(|| panic!("no key {key}"));
    let after = &text[start + needle.len()..];
    let colon = after.find(':').unwrap();
    let q1 = after[colon..].find('"').unwrap() + colon + 1;
    let q2 = after[q1..].find('"').unwrap() + q1;
    after[q1..q2].to_string()
}

/** @brief Extract a JSON unsigned-integer value by key (`"key": 123`). */
fn json_uint(text: &str, key: &str) -> u64 {
    let needle = format!("\"{key}\"");
    let start = text.find(&needle).unwrap_or_else(|| panic!("no key {key}"));
    let after = &text[start + needle.len()..];
    let colon = after.find(':').unwrap();
    after[colon + 1..]
        .trim_start()
        .chars()
        .take_while(|c| c.is_ascii_digit())
        .collect::<String>()
        .parse()
        .unwrap()
}

/**
 * @brief Assert a vector's `hex` == input.bin, `total_bytes` == len, and that the codec
 * round-trips it (the shared invariant every vector satisfies).
 */
fn assert_vector_consistent(name: &str) -> Vec<u8> {
    let (bin, json) = load(name);
    assert_eq!(
        from_hex(&json_str(&json, "hex")),
        bin,
        "{name}: hex != input.bin"
    );
    assert_eq!(
        json_uint(&json, "total_bytes") as usize,
        bin.len(),
        "{name}: total_bytes != input.bin length"
    );
    let tlv = decode(&bin).unwrap_or_else(|e| panic!("{name}: decode failed: {:?}", e));
    assert_eq!(encode(&tlv), bin, "{name}: round-trip differs");
    bin
}

/* --------------------------------------------------- all-vectors structural --- */

/** @brief Every vector: hex-field consistency + codec round-trip, over the whole corpus. */
#[test]
fn all_vectors_hex_and_roundtrip() {
    let mut count = 0;
    let root = vectors_dir();
    let mut stack = vec![root.clone()];
    while let Some(dir) = stack.pop() {
        for entry in fs::read_dir(&dir).unwrap() {
            let p = entry.unwrap().path();
            if p.is_dir() {
                stack.push(p);
            } else if p.file_name().map(|n| n == "input.bin").unwrap_or(false) {
                let rel = p
                    .parent()
                    .unwrap()
                    .strip_prefix(&root)
                    .unwrap()
                    .to_string_lossy()
                    .replace('\\', "/");
                assert_vector_consistent(&rel);
                count += 1;
            }
        }
    }
    assert!(count >= 28, "expected >= 28 vectors, found {count}");
}

/* ------------------------------------------------------------ Unit 1 — VALUE --- */

#[test]
fn value_bool_true() {
    let bin = assert_vector_consistent("tlv-types/value-bool-true");
    assert_eq!(encode(&value(&[0x01])), bin);
    let t = decode(&bin).unwrap();
    assert_eq!(t.type_code, libtracer::type_code::VALUE);
    assert_eq!(t.payload, vec![0x01]);
}

#[test]
fn value_ll_u32() {
    let bin = assert_vector_consistent("tlv-types/value-ll-u32");
    let opts = ValueOptions {
        long_length: true,
        ..Default::default()
    };
    assert_eq!(encode(&value_opts(&[0xAA, 0xBB, 0xCC], &opts)), bin);
    assert!(decode(&bin).unwrap().opt.ll);
}

#[test]
fn value_crc32c() {
    let bin = assert_vector_consistent("crc/value-crc32c");
    let opts = ValueOptions {
        crc: true,
        ..Default::default()
    };
    assert_eq!(
        encode(&value_opts(&[0xAA, 0xBB, 0xCC, 0xDD, 0xEE], &opts)),
        bin
    );
}

#[test]
fn value_crc16() {
    let bin = assert_vector_consistent("crc/value-crc16");
    let opts = ValueOptions {
        crc: true,
        crc16: true,
        ..Default::default()
    };
    assert_eq!(encode(&value_opts(&[0xAA, 0xBB, 0xCC], &opts)), bin);
}

#[test]
fn value_ts_abs() {
    let bin = assert_vector_consistent("tlv-types/value-ts-abs");
    let opts = ValueOptions {
        timestamp_ns: Some(0x0102_0304_0506_0708),
        ..Default::default()
    };
    assert_eq!(encode(&value_opts(&[0xAA, 0xBB, 0xCC], &opts)), bin);
    let t = decode(&bin).unwrap();
    let ts = t.trailer.unwrap().ts.unwrap();
    assert!(!ts.relative);
    assert_eq!(ts.value, 0x0102_0304_0506_0708);
}

/* --------------------------------------------------------------- Unit 2 — PATH --- */

#[test]
fn path_sensor_temp() {
    let bin = assert_vector_consistent("path/path-sensor-temp");
    // build: string -> TLV -> bytes reproduces the vector
    let tlv = libtracer::path::path_to_tlv("/sensor/temp").unwrap();
    assert_eq!(encode(&tlv), bin);
    // parse: TLV -> string
    let decoded = decode(&bin).unwrap();
    assert_eq!(
        libtracer::path::tlv_to_path(&decoded).unwrap(),
        "/sensor/temp"
    );
    assert_eq!(decoded.children.len(), 2);
    // segments builder equivalence
    assert_eq!(
        encode(&libtracer::tlv_builders::path(&["sensor", "temp"]).unwrap()),
        bin
    );
}

#[test]
fn path_split_and_root() {
    assert_eq!(
        libtracer::path::split_path("/sensor/temp").unwrap(),
        vec!["sensor", "temp"]
    );
    assert!(libtracer::path::split_path("/").unwrap().is_empty());
    assert_eq!(
        libtracer::path::split_path("/a/b/").unwrap(),
        vec!["a", "b"]
    );
}

#[test]
fn path_rejects_reserved_and_overlong() {
    // reserved characters (/ : . [ ] * ?)
    for bad in ["/a.b", "/a:b", "/a[b", "/a]b", "/a*b", "/a?b"] {
        assert_eq!(
            libtracer::path::path_to_tlv(bad).unwrap_err(),
            BuildError::ReservedChar,
            "expected reserved-char rejection for {bad}"
        );
    }
    // over-length single segment (> 64 bytes)
    let long = format!("/{}", "x".repeat(65));
    assert_eq!(
        libtracer::path::path_to_tlv(&long).unwrap_err(),
        BuildError::SegmentLength
    );
    // not rooted
    assert_eq!(
        libtracer::path::path_to_tlv("sensor/temp").unwrap_err(),
        BuildError::NotRooted
    );
    // too many segments (> 32)
    let many = format!("/{}", vec!["a"; 33].join("/"));
    assert_eq!(
        libtracer::path::split_path(&many).unwrap_err(),
        BuildError::TooManySegments
    );
    // empty segment
    assert_eq!(
        libtracer::path::split_path("/a//b").unwrap_err(),
        BuildError::SegmentLength
    );
}

/* -------------------------------------------------- Unit 3 — ERROR + STATUS --- */

#[test]
fn error_registered_code() {
    let bin = assert_vector_consistent("errors/error-registered-code");
    assert_eq!(encode(&error_code(ErrCode::PathNotFound, None)), bin);
    let parsed = parse_error(&decode(&bin).unwrap()).unwrap();
    assert_eq!(parsed.id, ErrorId::Code(0x0020));
    assert_eq!(parsed.err_code(), Some(ErrCode::PathNotFound));
    assert_eq!(parsed.description, None);
    assert_eq!(ErrCode::PathNotFound.path(), "tr::path::not_found");
}

#[test]
fn error_registered_detail() {
    let bin = assert_vector_consistent("errors/error-registered-detail");
    assert_eq!(
        encode(&error_code(ErrCode::FlowTimeout, Some("deadline exceeded"))),
        bin
    );
    let parsed = parse_error(&decode(&bin).unwrap()).unwrap();
    assert_eq!(parsed.err_code(), Some(ErrCode::FlowTimeout));
    assert_eq!(parsed.description.as_deref(), Some("deadline exceeded"));
}

#[test]
fn error_string_form() {
    let bin = assert_vector_consistent("errors/error-string-form");
    assert_eq!(
        encode(&libtracer::error_string("tr::acme::widget::jammed", None)),
        bin
    );
    let parsed = parse_error(&decode(&bin).unwrap()).unwrap();
    assert_eq!(parsed.id, ErrorId::Path("tr::acme::widget::jammed".into()));
    assert_eq!(parsed.err_code(), None); // unregistered third-party path
}

#[test]
fn empty_status_ok() {
    let bin = assert_vector_consistent("framing/empty-status-ok");
    assert_eq!(encode(&status_ok()), bin);
    let t = decode(&bin).unwrap();
    assert_eq!(t.type_code, libtracer::type_code::STATUS);
    assert!(!t.opt.pl);
    assert!(t.children.is_empty());
}

#[test]
fn error_registry_is_complete_and_bidirectional() {
    // All 15 registered codes resolve both ways.
    let codes = [
        0x0001u16, 0x0002, 0x0003, 0x0010, 0x0020, 0x0021, 0x0022, 0x0030, 0x0031, 0x0040, 0x0041,
        0x0042, 0x0050, 0x0060, 0x0070,
    ];
    for c in codes {
        let e = ErrCode::from_code(c).unwrap_or_else(|| panic!("no ErrCode for {c:#06x}"));
        assert_eq!(e.code(), c);
        assert_eq!(ErrCode::from_path(e.path()), Some(e));
        assert!(e.path().starts_with("tr::"));
    }
}

/* --------------------------------------------------------------- Unit 5 — FIELD --- */

#[test]
fn field_append() {
    let bin = assert_vector_consistent("field/field-append");
    assert_eq!(encode(&encode_field(":subscribers[]").unwrap()), bin);
    let levels = parse_field_tlv(&decode(&bin).unwrap()).unwrap();
    assert_eq!(levels.len(), 1);
    assert_eq!(levels[0].name, "subscribers");
    assert_eq!(levels[0].mode, FieldMode::Element);
    assert_eq!(levels[0].index, None);
}

#[test]
fn field_indexed() {
    let bin = assert_vector_consistent("field/field-indexed");
    assert_eq!(encode(&encode_field(":subscribers[3]").unwrap()), bin);
    let levels = parse_field_tlv(&decode(&bin).unwrap()).unwrap();
    assert_eq!(levels.len(), 1);
    assert_eq!(levels[0].index, Some(3));
    assert_eq!(levels[0].mode, FieldMode::Element);
}

#[test]
fn field_nested() {
    let bin = assert_vector_consistent("field/field-nested");
    assert_eq!(encode(&encode_field(":settings.deadline_ns").unwrap()), bin);
    let levels = parse_field_tlv(&decode(&bin).unwrap()).unwrap();
    assert_eq!(levels.len(), 2);
    assert_eq!(levels[0].name, "settings");
    assert_eq!(levels[1].name, "deadline_ns");
    assert_eq!(levels[0].mode, FieldMode::Scalar);
    assert_eq!(levels[1].mode, FieldMode::Scalar);
}

/* ----------------------------------------------------- Unit 6 — structured --- */

#[test]
fn settings_reliability() {
    let bin = assert_vector_consistent("tlv-types/settings-reliability");
    assert_eq!(
        encode(&structured::settings(&[("reliability", &[1])]).unwrap()),
        bin
    );
    let t = decode(&bin).unwrap();
    assert_eq!(
        structured::settings_get(&t, "reliability").unwrap(),
        Some(vec![1u8])
    );
}

#[test]
fn subscriber_path() {
    let bin = assert_vector_consistent("tlv-types/subscriber-path");
    assert_eq!(encode(&subscriber(&["sensor", "temp"]).unwrap()), bin);
    let t = decode(&bin).unwrap();
    assert_eq!(
        structured::subscriber_target_path(&t).unwrap(),
        Some("/sensor/temp".to_string())
    );
}

#[test]
fn router_wrapped_named_fields() {
    let bin = assert_vector_consistent("tlv-types/router-wrapped");
    let t = decode(&bin).unwrap();
    let fields = structured::named_fields(&t).unwrap();
    let keys: Vec<&str> = fields.iter().map(|f| f.key.as_str()).collect();
    assert_eq!(
        keys,
        vec!["origin_peer_id", "origin_timestamp", "hop_count", "data"]
    );
    // the wrapped "data" VALUE is 0xABCD
    let data = structured::named_field(&t, "data").unwrap().unwrap();
    assert_eq!(data.payload, vec![0xAB, 0xCD]);
}

#[test]
fn acl_aces() {
    let bin = assert_vector_consistent("acl/acl-aces");
    let t = decode(&bin).unwrap();
    let aces = structured::acl_aces(&t).unwrap();
    assert_eq!(aces.len(), 2);
    // ACE1: ALLOW, INHERIT, peer-a, READ|WRITE, no expiry
    assert_eq!(aces[0].ace_type, 0);
    assert_eq!(aces[0].flags, 0x1);
    assert_eq!(aces[0].subject, b"peer-a");
    assert_eq!(aces[0].access_mask, 0x0003);
    assert_eq!(aces[0].expires_ns, None);
    // ACE2: ALLOW, flags 0, EVERYONE@, READ, expires 0x0102030405060708
    assert_eq!(aces[1].subject, b"EVERYONE@");
    assert_eq!(aces[1].access_mask, 0x0001);
    assert_eq!(aces[1].expires_ns, Some(0x0102_0304_0506_0708));
    // round-trip through the builder reproduces the exact bytes
    assert_eq!(encode(&structured::acl(&aces)), bin);
}

#[test]
fn ace_builder_matches_manual() {
    let aces = [
        Ace {
            ace_type: 0,
            flags: 0x1,
            subject: b"peer-a".to_vec(),
            access_mask: 0x0003,
            expires_ns: None,
        },
        Ace {
            ace_type: 0,
            flags: 0,
            subject: b"EVERYONE@".to_vec(),
            access_mask: 0x0001,
            expires_ns: Some(0x0102_0304_0506_0708),
        },
    ];
    let bin = assert_vector_consistent("acl/acl-aces");
    assert_eq!(encode(&structured::acl(&aces)), bin);
}

/* --------------------------------------------------------------- Unit 4 — FWD --- */

#[test]
fn fwd_read() {
    let bin = assert_vector_consistent("fwd/fwd-read");
    let req = FwdRequest::new(fwd_op::READ, &["sensor", "temp"], &["reply-ep"]);
    assert_eq!(encode(&encode_fwd(&req).unwrap()), bin);
    let f = decode_fwd(&bin).unwrap();
    assert_eq!(f.op, fwd_op::READ);
    assert_eq!(libtracer::fwd::fwd_dst_path(&f).unwrap(), "/sensor/temp");
    assert_eq!(libtracer::fwd::fwd_src_path(&f).unwrap(), "/reply-ep");
    assert!(f.field.is_none());
    assert!(f.payload.is_none());
}

#[test]
fn fwd_write_value() {
    let bin = assert_vector_consistent("fwd/fwd-write-value");
    let mut req = FwdRequest::new(fwd_op::WRITE, &["sensor", "temp"], &["reply-ep"]);
    req.payload = Some(value_u32(1234));
    assert_eq!(encode(&encode_fwd(&req).unwrap()), bin);
    let f = decode_fwd(&bin).unwrap();
    assert_eq!(f.op, fwd_op::WRITE);
    let p = f.payload.unwrap();
    assert_eq!(p.type_code, libtracer::type_code::VALUE);
    assert_eq!(p.payload_uint(), 1234);
}

#[test]
fn fwd_await_timeout() {
    let bin = assert_vector_consistent("fwd/fwd-await-timeout");
    let mut req = FwdRequest::new(fwd_op::AWAIT, &["sensor", "temp"], &["reply-ep"]);
    req.await_timeout_ns = Some(1_000_000_000);
    assert_eq!(encode(&encode_fwd(&req).unwrap()), bin);
    let f = decode_fwd(&bin).unwrap();
    assert_eq!(f.op, fwd_op::AWAIT);
    assert_eq!(f.await_timeout_ns, Some(1_000_000_000));
}

#[test]
fn fwd_write_subscriber_field() {
    let bin = assert_vector_consistent("fwd/fwd-write-subscriber-field");
    let mut req = FwdRequest::new(fwd_op::WRITE, &["sensor", "temp"], &["reply-ep"]);
    req.field = Some(FieldSel::Str(":subscribers[]"));
    req.payload = Some(subscriber(&["reply-ep"]).unwrap());
    assert_eq!(encode(&encode_fwd(&req).unwrap()), bin);
    let f = decode_fwd(&bin).unwrap();
    assert_eq!(f.op, fwd_op::WRITE);
    assert!(f.field.is_some());
    assert_eq!(
        f.payload.unwrap().type_code,
        libtracer::type_code::SUBSCRIBER
    );
}

#[test]
fn fwd_reply_result() {
    let bin = assert_vector_consistent("fwd/fwd-reply-result");
    let mut req = FwdRequest::new(
        fwd_op::REPLY,
        &["via_board", "via_net", "reply-ep"],
        &["sensor"],
    );
    req.kind = Some(fwd_kind::RESULT);
    req.payload = Some(value_u32(1234));
    assert_eq!(encode(&encode_fwd(&req).unwrap()), bin);
    let f = decode_fwd(&bin).unwrap();
    assert_eq!(f.op, fwd_op::REPLY);
    assert_eq!(f.kind, Some(fwd_kind::RESULT));
    assert_eq!(f.payload.unwrap().payload_uint(), 1234);
}

#[test]
fn fwd_reply_error() {
    let bin = assert_vector_consistent("fwd/fwd-reply-error");
    let mut req = FwdRequest::new(
        fwd_op::REPLY,
        &["via_board", "via_net", "reply-ep"],
        &["sensor"],
    );
    req.kind = Some(fwd_kind::ERROR);
    req.payload = Some(status_with_errors(&[error_code(
        ErrCode::PathNotFound,
        None,
    )]));
    assert_eq!(encode(&encode_fwd(&req).unwrap()), bin);
    let f = decode_fwd(&bin).unwrap();
    assert_eq!(f.kind, Some(fwd_kind::ERROR));
    assert_eq!(reply_error_code(&f), 0x0020);
    assert_eq!(
        ErrCode::from_code(reply_error_code(&f)),
        Some(ErrCode::PathNotFound)
    );
}

#[test]
fn fwd_routed_multihop() {
    let bin = assert_vector_consistent("fwd/fwd-routed-multihop");
    let req = FwdRequest::new(
        fwd_op::READ,
        &["net", "board", "can0", "ow", "sensor"],
        &["reply-ep"],
    );
    assert_eq!(encode(&encode_fwd(&req).unwrap()), bin);
    let f = decode_fwd(&bin).unwrap();
    assert_eq!(
        libtracer::fwd::fwd_dst_path(&f).unwrap(),
        "/net/board/can0/ow/sensor"
    );
}

#[test]
fn fwd_src_accumulated() {
    let bin = assert_vector_consistent("fwd/fwd-src-accumulated");
    let req = FwdRequest::new(
        fwd_op::READ,
        &["can0", "ow", "sensor"],
        &["via_board", "via_net", "reply-ep"],
    );
    assert_eq!(encode(&encode_fwd(&req).unwrap()), bin);
    let f = decode_fwd(&bin).unwrap();
    assert_eq!(libtracer::fwd::fwd_dst_path(&f).unwrap(), "/can0/ow/sensor");
    assert_eq!(
        libtracer::fwd::fwd_src_path(&f).unwrap(),
        "/via_board/via_net/reply-ep"
    );
}

#[test]
fn fwd_wildcard_reject() {
    // At the CODEC layer this is a valid, round-trip-safe frame (resolution-layer
    // rejection is a separate concern). The FIELD carries a WILDCARD level.
    let bin = assert_vector_consistent("fwd/fwd-wildcard-reject");
    let mut req = FwdRequest::new(fwd_op::READ, &["sensor", "temp"], &["reply-ep"]);
    req.field = Some(FieldSel::Str(":data[*]"));
    assert_eq!(encode(&encode_fwd(&req).unwrap()), bin);
    let f = decode_fwd(&bin).unwrap();
    let levels = parse_field_tlv(&f.field.unwrap()).unwrap();
    assert_eq!(levels[0].mode, FieldMode::Wildcard);
}
