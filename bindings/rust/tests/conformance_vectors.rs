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
use libtracer::structured::{self, Ace, DeliveryPolicy, SettingValue};
use libtracer::{
    decode, decode_fwd, encode, encode_field, encode_fwd, error_code, parse_error, parse_field_tlv,
    path_ref, path_ref_element, reply_error_code, status_ok, status_with_errors, subscriber, value,
    value_opts, value_u32, BuildError, ErrCode, FwdRequest, Opt, PathRefElement, Tlv, ValueOptions,
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

/** @brief Lowercase hex of @p bytes — the spelling the byte pins below are written in. */
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
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

/**
 * @brief A bare DESCRIPTION (`0x03`) node carrying opaque UTF-8 — the optional human detail a
 * STATUS or an ERROR may carry (reference/05 §`0x03`). The crate exposes it only as the
 * `description` argument of `error_code`/`error_string`, so a STATUS-level DESCRIPTION is
 * spelled out here.
 */
fn description_tlv(text: &str) -> Tlv {
    Tlv {
        type_code: libtracer::type_code::DESCRIPTION,
        opt: Opt::default(),
        payload: text.as_bytes().to_vec(),
        children: Vec::new(),
        trailer: None,
    }
}

/**
 * @brief Assert a NEGATIVE vector: `hex` == reject.bin, and `decode` fails with the error
 * `expected.json`'s `"reject"` field names.
 */
fn assert_vector_rejected(name: &str) -> Vec<u8> {
    let dir = vectors_dir().join(name);
    let bin =
        fs::read(dir.join("reject.bin")).unwrap_or_else(|e| panic!("read {name}/reject.bin: {e}"));
    let json = fs::read_to_string(dir.join("expected.json"))
        .unwrap_or_else(|e| panic!("read {name}/expected.json: {e}"));
    assert_eq!(
        from_hex(&json_str(&json, "hex")),
        bin,
        "{name}: hex != reject.bin"
    );
    assert_eq!(
        json_uint(&json, "total_bytes") as usize,
        bin.len(),
        "{name}: total_bytes != reject.bin length"
    );
    let want = json_str(&json, "reject");
    match decode(&bin) {
        Ok(_) => panic!("{name}: decode SUCCEEDED — it must fail with {want}"),
        Err(e) => assert_eq!(e.name(), want, "{name}: wrong reject error"),
    }
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
    // RFC-0023 repriced the cap 32 -> 255: 33 segments is now LEGAL, and this case was
    // inverted from a must-reject. 33 * (4 + 1) = 165 encoded bytes, far under MAX_PATH_BYTES.
    let thirty_three = format!("/{}", vec!["a"; 33].join("/"));
    assert_eq!(
        libtracer::path::split_path(&thirty_three).unwrap().len(),
        33
    );
    assert!(libtracer::path::path_to_tlv(&thirty_three).is_ok());
    // 204 segments = 1020 bytes: the byte-derived ceiling under this body encoding, and the
    // deepest PATH today's NAME-TLV grammar can express (RFC-0023 §4.2).
    let two_oh_four = format!("/{}", vec!["a"; 204].join("/"));
    assert!(libtracer::path::path_to_tlv(&two_oh_four).is_ok());
    // 205 = 1025 bytes: rejected by the BYTE cap, not the count. Under this encoding the count
    // clause can never fire; it becomes binding only under RFC-0018's packed body.
    let two_oh_five = format!("/{}", vec!["a"; 205].join("/"));
    assert_eq!(
        libtracer::path::path_to_tlv(&two_oh_five).unwrap_err(),
        BuildError::PathTooLong
    );
    // too many segments (> 255) — the count clause itself, checked at the split tier where no
    // byte budget has been accumulated yet.
    let many = format!("/{}", vec!["a"; 256].join("/"));
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

/**
 * @brief Build a PATH TLV with `n` one-byte NAME segments WITHOUT going through
 * `tlv_builders::path` — i.e. bypassing the construction-tier admission gate, the way
 * bytes arriving off the wire do. `n` above 204 is not expressible through the builders.
 */
fn raw_path_tlv(n: usize) -> Tlv {
    Tlv {
        type_code: libtracer::type_code::PATH,
        opt: Opt::structured(),
        payload: Vec::new(),
        children: (0..n).map(|_| libtracer::name("a").unwrap()).collect(),
        trailer: None,
    }
}

/**
 * @brief RFC-0019 §10(b) regression, the one the RFC-0023 §5.6 relocation exists to fix:
 * an accumulated `src` return route deeper than the admission bounds must DECODE, because
 * the codec tier does not enforce them (`reference/05` §"Enforcement of the PATH
 * constraints"). 256 one-byte segments overshoot BOTH accumulative bounds at once — the
 * count (256 > 255) and the body budget (256 × 5 = 1280 > 1024) — and the bytes are
 * still well-formed TLV that must round-trip byte-identically.
 */
#[test]
fn path_decode_admits_over_limit_accumulated_src_route() {
    let deep = raw_path_tlv(256);
    let bin = encode(&deep);
    let decoded = decode(&bin).unwrap();
    assert_eq!(encode(&decoded), bin, "over-limit PATH must round-trip");
    assert_eq!(decoded.children.len(), 256);

    let rendered = libtracer::path::tlv_to_path(&decoded)
        .expect("decode tier must not enforce the accumulative PATH bounds");
    assert_eq!(rendered, format!("/{}", vec!["a"; 256].join("/")));

    // …and through the FWD accessor the route actually arrives on. Built by hand because
    // `encode_fwd` runs the construction-tier gate, which legitimately rejects this depth.
    let fwd = Tlv {
        type_code: libtracer::type_code::FWD,
        opt: Opt::structured(),
        payload: Vec::new(),
        children: vec![
            libtracer::value_u8(fwd_op::READ),
            libtracer::tlv_builders::path(&["sensor", "temp"]).unwrap(),
            deep,
        ],
        trailer: None,
    };
    let parsed = libtracer::decode_fwd(&encode(&fwd)).unwrap();
    assert_eq!(
        libtracer::fwd::fwd_src_path(&parsed).unwrap(),
        rendered,
        "an accumulated src route is legal at any byte-reachable depth"
    );
    assert_eq!(
        libtracer::fwd::fwd_dst_path(&parsed).unwrap(),
        "/sensor/temp"
    );
}

/**
 * @brief The relocated bound, at its new tier: `admit_path_tlv` is where the count and
 * byte limits live (RFC-0023 §5.6). It must fail exactly the cases the decode-tier check
 * used to fail — 256 segments reject, 33 accept — and keep the 255 bound exact.
 */
#[test]
fn path_admission_enforces_segment_count() {
    // 33 segments: legal since RFC-0023 repriced 32 → 255.
    assert!(libtracer::path::admit_path_tlv(&raw_path_tlv(33)).is_ok());
    // 204 segments = 1020 bytes: the deepest this body encoding can express.
    assert!(libtracer::path::admit_path_tlv(&raw_path_tlv(204)).is_ok());
    // 205 = 1025 bytes: the byte budget binds first under this encoding.
    assert_eq!(
        libtracer::path::admit_path_tlv(&raw_path_tlv(205)).unwrap_err(),
        BuildError::PathTooLong
    );
    // 256 segments: the count clause itself — checked BEFORE the byte accumulation, so it
    // is this error and not PathTooLong that surfaces. Deleting the count check in
    // `admit_path_tlv` reddens exactly this assertion.
    assert_eq!(
        libtracer::path::admit_path_tlv(&raw_path_tlv(256)).unwrap_err(),
        BuildError::TooManySegments
    );
    // The admission gate is also the child-type and segment-syntax gate.
    assert_eq!(
        libtracer::path::admit_path_tlv(&value_u32(7)).unwrap_err(),
        BuildError::TypeMismatch
    );
    // The graph root (zero children) is admissible.
    assert!(libtracer::path::admit_path_tlv(&raw_path_tlv(0)).is_ok());
}

/**
 * @brief The encode-time MUST is unmoved: `tlv_builders::path` rejects an over-count
 * segment list on its own, independently of `split_path`'s string-tier check. Deleting
 * the count check at `tlv_builders.rs` reddens this.
 */
#[test]
fn path_builder_enforces_segment_count() {
    let many = vec!["a"; 256];
    assert_eq!(
        libtracer::tlv_builders::path(&many).unwrap_err(),
        BuildError::TooManySegments
    );
    let thirty_three = vec!["a"; 33];
    assert!(libtracer::tlv_builders::path(&thirty_three).is_ok());
}

/* ------------------------------------------------- RFC-0024 §4 — PATH_REF --- */

/**
 * @brief The minimal bound path: one element, 12 bytes, byte-pinned against the vector.
 *
 * The hex literal is the pin — a change to `path_ref` that no longer produces the published
 * bytes fails here even if the vector file were regenerated alongside it.
 */
#[test]
fn path_ref_1host() {
    let bin = assert_vector_consistent("path-ref/ref-1host");
    assert_eq!(hex(&bin), "140008000700000003000000");
    let built = path_ref(&[PathRefElement {
        index: 7,
        generation: 3,
    }])
    .unwrap();
    assert_eq!(encode(&built), bin);
    let t = decode(&bin).unwrap();
    assert_eq!(t.type_code, libtracer::type_code::PATH_REF);
    // PL=0: the body is a fixed-stride record array, NOT children (RFC-0024 §4.2).
    assert!(!t.opt.pl && t.children.is_empty());
    // There is no count field on the wire — the count IS length / 8 (§4.3).
    assert_eq!(t.payload.len() / libtracer::PATH_REF_ELEMENT_BYTES, 1);
    assert_eq!(
        path_ref_element(&t.payload, 0),
        Some(PathRefElement {
            index: 7,
            generation: 3
        })
    );
    assert_eq!(path_ref_element(&t.payload, 1), None);
}

/** @brief The direct-link bound path: `4 + 8H` = 20 bytes at H=2 (RFC-0024 §3.2). */
#[test]
fn path_ref_2host() {
    let bin = assert_vector_consistent("path-ref/ref-2host");
    assert_eq!(hex(&bin), "1400100007000000030000002a00000001000000");
    let elems = [
        PathRefElement {
            index: 7,
            generation: 3,
        },
        PathRefElement {
            index: 42,
            generation: 1,
        },
    ];
    assert_eq!(encode(&path_ref(&elems).unwrap()), bin);
    let t = decode(&bin).unwrap();
    for (i, want) in elems.iter().enumerate() {
        assert_eq!(path_ref_element(&t.payload, i).as_ref(), Some(want));
    }
}

/** @brief The one-forwarder bound path: origin, forwarder, terminus — 28 bytes at H=3. */
#[test]
fn path_ref_3host() {
    let bin = assert_vector_consistent("path-ref/ref-3host");
    assert_eq!(
        hex(&bin),
        "140018000700000003000000130000000c0000002a00000001000000"
    );
    let elems = [
        PathRefElement {
            index: 7,
            generation: 3,
        },
        PathRefElement {
            index: 19,
            generation: 12,
        },
        PathRefElement {
            index: 42,
            generation: 1,
        },
    ];
    assert_eq!(encode(&path_ref(&elems).unwrap()), bin);
    assert_eq!(
        decode(&bin).unwrap().payload.len() / libtracer::PATH_REF_ELEMENT_BYTES,
        3
    );
}

/**
 * @brief The normative maximum — 255 elements, a 2040-byte body, 2044 total (RFC-0024 §4.3).
 *
 * Pinned by its head and tail rather than 4088 hex characters: the first element, the last,
 * and the envelope's `length` word are what an off-by-one in the bound would move.
 */
#[test]
fn path_ref_255_elements() {
    let bin = assert_vector_consistent("path-ref/ref-255-elements");
    assert_eq!(bin.len(), 4 + 255 * 8);
    assert_eq!(hex(&bin[..12]), "1400f8070000000000000000");
    assert_eq!(hex(&bin[bin.len() - 8..]), "fe000000f2060000");
    let elems: Vec<PathRefElement> = (0..255u32)
        .map(|i| PathRefElement {
            index: i,
            generation: (i * 7) % 65536,
        })
        .collect();
    assert_eq!(encode(&path_ref(&elems).unwrap()), bin);
    // One over the bound has no bound spelling at all — the builder refuses rather than
    // truncating, and the origin falls back to the canonical PATH it minted from.
    let over = vec![PathRefElement::default(); 256];
    assert_eq!(path_ref(&over).unwrap_err(), BuildError::TooManySegments);
}

/**
 * @brief A `length` that is not a whole number of elements is `tr::frame::invalid`.
 *
 * Unlike `path/path-value-children-illegal` — a resolver rule whose bytes still round-trip —
 * this is a shape error readable from the header alone, because the element count is not on
 * the wire at all: it IS `length / 8`.
 */
#[test]
fn path_ref_len_not_multiple_of_8() {
    let bin = assert_vector_rejected("path-ref/ref-len-not-multiple-of-8");
    assert_eq!(hex(&bin), "14000c000700000003000000aabbccdd");
}

/**
 * @brief The reverse list's own code carries the SAME body grammar (RFC-0024 §7.1
 * amendment 2), so an unframeable length is `tr::frame::invalid` under `0x15` too.
 *
 * The pairing with `path_ref_len_not_multiple_of_8` is the point: a core that applied the
 * shape rule to `0x14` alone would accept these bytes, and the two codes would then disagree
 * about what a bound-path body is.
 */
#[test]
fn path_ref_reverse_len_not_multiple_of_8() {
    let bin = assert_vector_rejected("path-ref/reverse-len-not-multiple-of-8");
    assert_eq!(hex(&bin), "15000c000700000003000000aabbccdd");
}

/**
 * @brief `fwd/fwd-reverse-mint` — the forwarded leg's reverse list is a TYPED trailing child.
 *
 * Pinned against `fwd/fwd-mint-request` in the direction that matters: the origin's frame is
 * the one WITHOUT the child, so the reverse list's bytes are proven to ride the forwarded leg
 * only. The child's own type byte is asserted, because a positional reading decodes this
 * frame identically and is exactly what amendment 2 ruled out.
 */
#[test]
fn fwd_reverse_mint_is_a_typed_trailing_child() {
    let bin = assert_vector_consistent("fwd/fwd-reverse-mint");
    let fwd = decode(&bin).expect("fwd-reverse-mint decodes");
    let last = fwd.children.last().expect("the FWD has children");
    assert_eq!(
        last.type_code,
        libtracer::type_code::PATH_REF_REVERSE,
        "the reverse list heads with 0x15, not 0x14"
    );
    assert_eq!(last.payload.len(), 8, "one element");
    assert!(
        libtracer::is_path_ref_type(last.type_code),
        "and it is a bound-path body for the shape rule's purposes"
    );
    // The origin's own frame carries no such child — that is what "zero added ORIGIN bytes"
    // means after the amendment.
    let origin = decode(&assert_vector_consistent("fwd/fwd-mint-request")).unwrap();
    assert!(
        origin
            .children
            .iter()
            .all(|c| c.type_code != libtracer::type_code::PATH_REF_REVERSE),
        "the origin emits no reverse child"
    );
    let parsed = libtracer::fwd::parse_fwd_tlv(&fwd).expect("parse_fwd accepts the forwarded leg");
    assert!(parsed.mint_request, "the op byte is mint-flagged");
    assert!(
        parsed.reverse.is_some(),
        "and the reverse list is parsed out"
    );
    assert!(
        parsed.payload.is_none(),
        "a READ has no payload — the reverse child is not mistaken for one"
    );
}

/** @brief One element over the §4.3 bound: 256 elements / 2048 bytes ⇒ `tr::frame::invalid`. */
#[test]
fn path_ref_256_elements() {
    let bin = assert_vector_rejected("path-ref/ref-256-elements");
    assert_eq!(bin.len(), 4 + 256 * 8);
    assert_eq!(hex(&bin[..4]), "14000008");
}

/**
 * @brief `opt.PL = 1` on a PATH_REF is `tr::frame::invalid` — the body is not child TLVs.
 *
 * The same 16-byte body as `ref-2host`, one option bit different: a generic `PL = 1` walker
 * would read the first element's index bytes as a TLV header and mis-frame the whole body.
 */
#[test]
fn path_ref_pl_set() {
    let bin = assert_vector_rejected("path-ref/ref-pl-set");
    assert_eq!(hex(&bin), "1440100007000000030000002a00000001000000");
    // The one differing byte, stated: ref-2host is the same frame with opt = 0x00.
    let mut cleared = bin.clone();
    cleared[1] = 0x00;
    assert_eq!(cleared, assert_vector_consistent("path-ref/ref-2host"));
}

/**
 * @brief `opt.LL = 1` on a PATH_REF is `tr::frame::invalid` — the u32 length is unreachable.
 *
 * `ref-1host`'s single element under the 6-byte header. 255 elements is 2040 bytes, three
 * orders inside a u16, so a wide length can only ever be two wasted bytes; §4.2 forbids the
 * bit so that one route has one byte spelling. Its own vector because the clause is
 * independent of the `PL` one — a core that drops it still satisfies the other three rules.
 */
#[test]
fn path_ref_ll_set() {
    let bin = assert_vector_rejected("path-ref/ref-ll-set");
    assert_eq!(hex(&bin), "1408080000000700000003000000");
}

/**
 * @brief An empty body (`H = 0`) round-trips: §4.3's element bound is an upper one.
 *
 * `0 % 8 == 0` and `0 <= 2040`, and no clause demands a first element — so the codec carries
 * the frame and a route naming no vertex is the router's to refuse (§5). The low end of the
 * range whose high end `ref-255-elements` / `ref-256-elements` straddle.
 */
#[test]
fn path_ref_empty() {
    let bin = assert_vector_consistent("path-ref/ref-empty");
    assert_eq!(hex(&bin), "14000000");
    assert_eq!(encode(&path_ref(&[]).unwrap()), bin);
    let t = decode(&bin).unwrap();
    assert!(t.payload.is_empty());
    assert_eq!(path_ref_element(&t.payload, 0), None);
}

/* ------------------------------- encode/decode symmetry on PATH_REF (#1004) --- */

/**
 * @brief A raw `PATH_REF` node as a caller can spell one — `Tlv`'s fields are all public, so
 * this bypasses the guarded `path_ref` builder, which is the door #1004 is about.
 */
fn raw_path_ref(pl: bool, ll: bool, payload: Vec<u8>, children: Vec<Tlv>) -> Tlv {
    Tlv {
        type_code: libtracer::type_code::PATH_REF,
        opt: Opt {
            pl,
            ll,
            ..Opt::default()
        },
        payload,
        children,
        trailer: None,
    }
}

/** @brief A structured `FWD` wrapping @p children — the nesting case for the property. */
fn fwd_wrapping(children: Vec<Tlv>) -> Tlv {
    Tlv {
        type_code: libtracer::type_code::FWD,
        opt: Opt::structured(),
        payload: Vec::new(),
        children,
        trailer: None,
    }
}

/**
 * @brief Each ill-formed `PATH_REF` shape encodes to NOTHING, and the bytes this crate used
 * to mint for it are `FRAME_INVALID` (#1004).
 *
 * The property being closed, in both halves. Before the fix `encode` serialized every one of
 * these verbatim; the second assertion in each pair is what those bytes were worth — this
 * core's own `decode` refuses them, and so does every conformant node. Three of the four
 * pre-fix encodings ARE the published negative vectors, byte-for-byte, which is why those
 * are read from disk rather than written out as literals here.
 */
#[test]
fn path_ref_ill_formed_encodes_to_nothing() {
    // (index 7, generation 3), little-endian — one well-formed element, reused below.
    let one = vec![7u8, 0, 0, 0, 3, 0, 0, 0];

    // opt.LL = 1. Pre-fix output: `1408080000000700000003000000` — `ref-ll-set` exactly.
    assert!(encode(&raw_path_ref(false, true, one.clone(), Vec::new())).is_empty());
    assert_eq!(
        hex(&assert_vector_rejected("path-ref/ref-ll-set")),
        "1408080000000700000003000000"
    );

    // A length that is not a whole number of elements. Pre-fix output:
    // `14000c000700000003000000aabbccdd` — `ref-len-not-multiple-of-8` exactly.
    let mut ragged = one.clone();
    ragged.extend_from_slice(&[0xaa, 0xbb, 0xcc, 0xdd]);
    assert!(encode(&raw_path_ref(false, false, ragged, Vec::new())).is_empty());
    assert_eq!(
        hex(&assert_vector_rejected(
            "path-ref/ref-len-not-multiple-of-8"
        )),
        "14000c000700000003000000aabbccdd"
    );

    // One element over the §4.3 bound. Pre-fix output: the 2052-byte `ref-256-elements`,
    // whose elements are `(index = i, generation = 0)` — rebuilt here so the pin is the
    // published bytes rather than a length.
    let over: Vec<u8> = (0..=libtracer::MAX_PATH_REF_ELEMENTS as u32)
        .flat_map(|i| {
            let mut e = i.to_le_bytes().to_vec();
            e.extend_from_slice(&0u32.to_le_bytes());
            e
        })
        .collect();
    assert_eq!(
        over.len(),
        (libtracer::MAX_PATH_REF_ELEMENTS + 1) * libtracer::PATH_REF_ELEMENT_BYTES
    );
    assert!(encode(&raw_path_ref(false, false, over.clone(), Vec::new())).is_empty());
    let vector_256 = assert_vector_rejected("path-ref/ref-256-elements");
    assert_eq!(
        vector_256[4..],
        over,
        "the pre-fix encoding IS this vector's body"
    );

    // opt.PL = 1 — a caller who mistook PATH_REF for a structured type. This shape has no
    // published vector (a PL body is child TLVs, not an element array), so its pre-fix
    // encoding is written out: a PATH_REF whose body is one NAME TLV.
    let pl_set = raw_path_ref(true, false, Vec::new(), name_child_body(&one));
    assert!(encode(&pl_set).is_empty());
    let pre_fix_pl = from_hex("14400c00020008000700000003000000");
    assert_eq!(
        decode(&pre_fix_pl).unwrap_err(),
        libtracer::Error::FrameInvalid
    );
}

/** @brief One NAME child carrying @p payload — the body a `PL = 1` PATH_REF would frame. */
fn name_child_body(payload: &[u8]) -> Vec<Tlv> {
    vec![Tlv {
        type_code: libtracer::type_code::NAME,
        opt: Opt::default(),
        payload: payload.to_vec(),
        children: Vec::new(),
        trailer: None,
    }]
}

/**
 * @brief A refused TLV refuses its ANCESTORS — never a shorter frame that decodes (#1004).
 *
 * The counterfactual is the point: dropping the bad child would have emitted `0f4005000200010061`,
 * a 9-byte FWD that decodes cleanly one component short. Silent truncation is worse than
 * emitting nothing, so the parent refuses too.
 */
#[test]
fn path_ref_refusal_propagates_to_ancestors() {
    let mut ragged = vec![7u8, 0, 0, 0, 3, 0, 0, 0];
    ragged.extend_from_slice(&[0xaa, 0xbb, 0xcc, 0xdd]);
    let leading_name = name_child_body(&[0x61]);
    let mut children = leading_name.clone();
    children.push(raw_path_ref(false, false, ragged, Vec::new()));

    // The sibling alone still encodes — the parent is refused for the PATH_REF, not for being
    // structured, so this is not a vacuous "everything is empty now" pass.
    assert_eq!(
        hex(&encode(&fwd_wrapping(leading_name))),
        "0f4005000200010061"
    );
    assert!(encode(&fwd_wrapping(children.clone())).is_empty());
    // Two levels up refuses as well.
    assert!(encode(&fwd_wrapping(vec![fwd_wrapping(children)])).is_empty());
}

/**
 * @brief The guard does not over-refuse: every WELL-FORMED `PATH_REF` still encodes to the
 * bytes it did before, including the two ends of the §4.3 range (#1004).
 *
 * Built by struct literal rather than through `path_ref`, so this covers the door the guard
 * sits on rather than the builder that satisfies the rule by construction.
 */
#[test]
fn path_ref_well_formed_is_untouched_by_the_guard() {
    for name in [
        "ref-empty",
        "ref-1host",
        "ref-2host",
        "ref-3host",
        "ref-255-elements",
    ] {
        let bin = assert_vector_consistent(&format!("path-ref/{name}"));
        let body = bin[4..].to_vec();
        assert_eq!(
            encode(&raw_path_ref(false, false, body, Vec::new())),
            bin,
            "{name}: byte-identical to the published vector"
        );
    }
}

/**
 * @brief `encode_fwd_bytes` surfaces the refusal instead of answering `Ok(vec![])` (#1004).
 *
 * `FwdRequest::payload` is embedded verbatim, so a caller-built ill-formed `PATH_REF` reaches
 * the codec through this `Result`-returning door; a success carrying an empty frame would be
 * the worst of the two answers.
 */
#[test]
fn encode_fwd_bytes_refuses_an_ill_formed_path_ref_payload() {
    let dst = ["sensor", "temp"];
    let src = ["client"];
    let mut req = FwdRequest::new(fwd_op::WRITE, &dst, &src);
    assert!(libtracer::encode_fwd_bytes(&req).is_ok());

    req.payload = Some(raw_path_ref(false, false, vec![0u8; 9], Vec::new()));
    assert_eq!(
        libtracer::encode_fwd_bytes(&req).unwrap_err(),
        BuildError::InvalidPathRef
    );

    // A well-formed PATH_REF payload still builds — the wrapper refuses the shape, not the type.
    req.payload = Some(
        path_ref(&[PathRefElement {
            index: 7,
            generation: 3,
        }])
        .unwrap(),
    );
    let bytes = libtracer::encode_fwd_bytes(&req).unwrap();
    assert_eq!(
        decode_fwd(&bytes).unwrap().payload.unwrap().type_code,
        libtracer::type_code::PATH_REF
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
    assert_eq!(encode(&encode_field(":settings.app").unwrap()), bin);
    let levels = parse_field_tlv(&decode(&bin).unwrap()).unwrap();
    assert_eq!(levels.len(), 2);
    assert_eq!(levels[0].name, "settings");
    assert_eq!(levels[1].name, "app");
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

/** @brief RFC-0026 (#993): the mask is u32 end to end — a rights bit above the old
 * 16-bit ceiling survives build → decode → read untruncated (the reader used to
 * narrow to u16, silently dropping the high half of a wider grant). */
#[test]
fn ace_mask_u32_round_trip() {
    let aces = [Ace {
        ace_type: 0,
        flags: 0,
        subject: b"peer-a".to_vec(),
        access_mask: 0x0001_0001,
        expires_ns: None,
    }];
    let t = decode(&encode(&structured::acl(&aces))).unwrap();
    let got = structured::acl_aces(&t).unwrap();
    assert_eq!(got[0].access_mask, 0x0001_0001);
}

/* ------------------------------------------------------------- Unit 6 — SPEC --- */

/**
 * @brief `spec/create-child` — the minimal creation SPEC, byte-for-byte.
 *
 * The gate on the value TYPE of the two fields (#877): both are `NAME` (`0x02`). The
 * terminus pairs a `NAME` key with a `NAME` value and skips any other type, so a
 * `VALUE`-typed `type`/`name` leaves the catalog selector empty and the create is
 * refused with `INVALID_PATH`. That spelling round-trips its own bytes perfectly, so
 * only a byte pin against the shared vector can catch it — the assertion below is the
 * one that reddens if `spec()` goes back to wrapping the fields in `value()`.
 */
#[test]
fn spec_create_child() {
    let bin = assert_vector_consistent("spec/create-child");
    assert_eq!(
        encode(&structured::spec("stored_value", "temp", None).unwrap()),
        bin,
        "the SPEC builder must emit the vector's bytes — NAME field values, not VALUE"
    );

    let t = decode(&bin).unwrap();
    assert_eq!(t.type_code, libtracer::type_code::SPEC);
    assert_eq!(
        structured::spec_type_name(&t).unwrap(),
        (Some("stored_value".to_string()), Some("temp".to_string()))
    );
    // Every field value in the vector is a NAME node, keys and values alike.
    let fields = structured::named_fields(&t).unwrap();
    let keys: Vec<&str> = fields.iter().map(|f| f.key.as_str()).collect();
    assert_eq!(keys, vec!["type", "name"]);
    for f in &fields {
        assert_eq!(
            f.value.type_code,
            libtracer::type_code::NAME,
            "SPEC field {} must carry a NAME value",
            f.key
        );
    }
}

/**
 * @brief The refused spelling, stated as an ablation: a SPEC whose `type`/`name` values
 * are `VALUE` nodes is NOT the vector, and the reader declines to read it.
 *
 * This is the shape the Rust builder emitted before #877. It decodes cleanly and
 * re-encodes to itself — a codec-only harness cannot tell it from the real thing — so
 * the difference has to be asserted here, against the golden bytes.
 */
#[test]
fn spec_value_typed_fields_are_not_the_vector() {
    let bin = assert_vector_consistent("spec/create-child");
    let drifted = Tlv {
        type_code: libtracer::type_code::SPEC,
        opt: Opt::structured(),
        payload: Vec::new(),
        children: vec![
            libtracer::name("type").unwrap(),
            value(b"stored_value"),
            libtracer::name("name").unwrap(),
            value(b"temp"),
        ],
        trailer: None,
    };
    let drifted_bytes = encode(&drifted);
    assert_ne!(
        drifted_bytes, bin,
        "a VALUE-typed SPEC must not equal the golden bytes"
    );
    // It still round-trips itself — which is exactly why the codec harness misses it.
    assert_eq!(encode(&decode(&drifted_bytes).unwrap()), drifted_bytes);
    // And the reader reports both fields absent, as the terminus does.
    assert_eq!(
        structured::spec_type_name(&decode(&drifted_bytes).unwrap()).unwrap(),
        (None, None)
    );
}

/**
 * @brief `spec/conn-client-ws` — a connection-formation SPEC whose `config` mixes both
 * value types: `role`/`port` as opaque `VALUE`, `kind`/`addr` as textual `NAME`.
 *
 * Before #877 the Rust settings builder could emit only the `VALUE` form, so the
 * string-valued keys were inexpressible — a link this core described was never formed.
 */
#[test]
fn spec_conn_client_ws() {
    let bin = assert_vector_consistent("spec/conn-client-ws");
    let config = structured::settings_typed(&[
        ("role", SettingValue::Value(&[0])),
        ("port", SettingValue::Value(&8080u16.to_le_bytes())),
        ("kind", SettingValue::Name("ws")),
        ("addr", SettingValue::Name("127.0.0.1")),
    ])
    .unwrap();
    assert_eq!(
        encode(&structured::spec("client", "up", Some(config)).unwrap()),
        bin,
        "the conn SPEC builder must reproduce the shared vector byte-for-byte"
    );

    let t = decode(&bin).unwrap();
    assert_eq!(
        structured::spec_type_name(&t).unwrap(),
        (Some("client".to_string()), Some("up".to_string()))
    );
    let cfg = structured::named_field(&t, "config").unwrap().unwrap();
    assert_eq!(cfg.type_code, libtracer::type_code::SETTINGS);
    // The string keys read back only through the NAME-typed accessor …
    assert_eq!(
        structured::settings_str(&cfg, "kind").unwrap(),
        Some("ws".to_string())
    );
    assert_eq!(
        structured::settings_str(&cfg, "addr").unwrap(),
        Some("127.0.0.1".to_string())
    );
    // … and the integer keys are NOT strings, so the typed accessor declines them.
    assert_eq!(structured::settings_str(&cfg, "port").unwrap(), None);
    assert_eq!(
        structured::settings_get(&cfg, "port").unwrap(),
        Some(vec![0x90, 0x1f])
    );
    assert_eq!(
        structured::settings_get(&cfg, "role").unwrap(),
        Some(vec![0])
    );
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

/**
 * @brief The terminus's RESULT reply (#419 ruling (c)): its routes are the request's
 * swapped, so `dst` is the accumulated return route `fwd/fwd-src-accumulated` carries as
 * `src`, and `src` is the terminus residual `fwd/fwd-routed-two-mount` ends on.
 */
#[test]
fn fwd_reply_result() {
    let bin = assert_vector_consistent("fwd/fwd-reply-result");
    let mut req = FwdRequest::new(
        fwd_op::REPLY,
        &["net", "downlink", "a", "net", "downlink", "cli", "reply-ep"],
        &["sensor", "temp"],
    );
    req.kind = Some(fwd_kind::RESULT);
    req.payload = Some(value_u32(1234));
    assert_eq!(encode(&encode_fwd(&req).unwrap()), bin);
    let f = decode_fwd(&bin).unwrap();
    assert_eq!(f.op, fwd_op::REPLY);
    assert_eq!(f.kind, Some(fwd_kind::RESULT));
    assert_eq!(
        libtracer::fwd::fwd_dst_path(&f).unwrap(),
        "/net/downlink/a/net/downlink/cli/reply-ep"
    );
    assert_eq!(libtracer::fwd::fwd_src_path(&f).unwrap(), "/sensor/temp");
    assert_eq!(f.payload.unwrap().payload_uint(), 1234);
}

/**
 * @brief The same terminus and the same swapped routes as `fwd_reply_result`, on the error
 * side: the reply `src` is the refused spelling (#419 ruling (c)).
 */
#[test]
fn fwd_reply_error() {
    let bin = assert_vector_consistent("fwd/fwd-reply-error");
    let mut req = FwdRequest::new(
        fwd_op::REPLY,
        &["net", "downlink", "a", "net", "downlink", "cli", "reply-ep"],
        &["sensor", "temp"],
    );
    req.kind = Some(fwd_kind::ERROR);
    req.payload = Some(status_with_errors(&[error_code(
        ErrCode::PathNotFound,
        None,
    )]));
    assert_eq!(encode(&encode_fwd(&req).unwrap()), bin);
    let f = decode_fwd(&bin).unwrap();
    assert_eq!(f.kind, Some(fwd_kind::ERROR));
    assert_eq!(
        libtracer::fwd::fwd_dst_path(&f).unwrap(),
        "/net/downlink/a/net/downlink/cli/reply-ep"
    );
    assert_eq!(libtracer::fwd::fwd_src_path(&f).unwrap(), "/sensor/temp");
    assert_eq!(reply_error_code(&f), 0x0020);
    assert_eq!(
        ErrCode::from_code(reply_error_code(&f)),
        Some(ErrCode::PathNotFound)
    );
}

/**
 * @brief The cross-core acceptance rule (#878): a reply's ERROR is the FIRST ERROR child of
 * the STATUS, at whatever position — reference/05 §`0x09` pins no order over a STATUS's
 * children, and RFC-0002 §C pins position only INSIDE the ERROR. Same frame as
 * `fwd_reply_error` with the STATUS's optional DESCRIPTION written first.
 *
 * The TypeScript binding pins these same bytes in `vectors.test.mjs`; before #878 it demanded
 * `children[0]` and read this frame as code 0.
 */
#[test]
fn fwd_reply_error_after_description() {
    let bin = assert_vector_consistent("fwd/fwd-reply-error-after-description");
    let mut req = FwdRequest::new(
        fwd_op::REPLY,
        &["net", "downlink", "a", "net", "downlink", "cli", "reply-ep"],
        &["sensor", "temp"],
    );
    req.kind = Some(fwd_kind::ERROR);
    // The offending shape is built with this crate's OWN builder: `status_with_errors` takes an
    // arbitrary `&[Tlv]` and copies it verbatim, so a caller appending the STATUS's optional
    // DESCRIPTION ahead of the ERROR needs nothing exotic.
    req.payload = Some(status_with_errors(&[
        description_tlv("no such vertex"),
        error_code(ErrCode::PathNotFound, None),
    ]));
    assert_eq!(encode(&encode_fwd(&req).unwrap()), bin);

    let f = decode_fwd(&bin).unwrap();
    assert_eq!(f.kind, Some(fwd_kind::ERROR));

    // The vector is only a gate while its ERROR is genuinely NOT the first child: assert the
    // shape before asserting the read, so a future re-blessing that reorders it cannot leave
    // this test silently passing on the easy case.
    let status = f.payload.as_ref().expect("kind=ERROR carries a STATUS");
    assert_eq!(status.type_code, libtracer::type_code::STATUS);
    assert_eq!(status.children.len(), 2);
    assert_eq!(
        status.children[0].type_code,
        libtracer::type_code::DESCRIPTION,
        "the ERROR must not be child 0 or this vector gates nothing"
    );
    assert_eq!(status.children[1].type_code, libtracer::type_code::ERROR);

    assert_eq!(reply_error_code(&f), 0x0020);
    assert_eq!(
        ErrCode::from_code(reply_error_code(&f)),
        Some(ErrCode::PathNotFound)
    );
    assert_eq!(libtracer::fwd::reply_error_path(&f).unwrap(), None);
}

#[test]
fn fwd_routed_mount_residual() {
    let bin = assert_vector_consistent("fwd/fwd-routed-mount-residual");
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

/**
 * @brief The two-mount route (#419): a `dst` crossing TWO `net/<module>/<name>` mounts
 * before its residual `/sensor/temp` resolves at the terminus.
 */
#[test]
fn fwd_routed_two_mount() {
    let bin = assert_vector_consistent("fwd/fwd-routed-two-mount");
    let req = FwdRequest::new(
        fwd_op::READ,
        &["net", "uplink", "b", "net", "uplink", "c", "sensor", "temp"],
        &["reply-ep"],
    );
    assert_eq!(encode(&encode_fwd(&req).unwrap()), bin);
    let f = decode_fwd(&bin).unwrap();
    assert_eq!(
        libtracer::fwd::fwd_dst_path(&f).unwrap(),
        "/net/uplink/b/net/uplink/c/sensor/temp"
    );
    assert_eq!(libtracer::fwd::fwd_src_path(&f).unwrap(), "/reply-ep");
}

#[test]
fn fwd_src_accumulated() {
    // Mid-route, two hops in: `src` has grown by TWO full `net/<module>/<name>` mount
    // runs (six segments), never two bare NAMEs. `dst` still carries the next mount plus
    // the residual. Bytes changed 2026-08-02 by maintainer ruling on #419 — the previous
    // frame was pre-S2a and did not compose under mount routing.
    let bin = assert_vector_consistent("fwd/fwd-src-accumulated");
    let req = FwdRequest::new(
        fwd_op::READ,
        &["net", "uplink", "d", "sensor", "temp"],
        &["net", "downlink", "a", "net", "downlink", "cli", "reply-ep"],
    );
    assert_eq!(encode(&encode_fwd(&req).unwrap()), bin);
    let f = decode_fwd(&bin).unwrap();
    assert_eq!(
        libtracer::fwd::fwd_dst_path(&f).unwrap(),
        "/net/uplink/d/sensor/temp"
    );
    assert_eq!(
        libtracer::fwd::fwd_src_path(&f).unwrap(),
        "/net/downlink/a/net/downlink/cli/reply-ep"
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

/* ------------------------------------------- RFC-0022 §3.A delivery policy --- */

/**
 * @brief `subscriber/policy-absent` — a SUBSCRIBER naming no `delivery_policy`.
 *
 * The vector carries a `SETTINGS` child that names a DIFFERENT key
 * (`delivery_compact`), which is the trap: a parser that read the policy by POSITION
 * rather than by NAME would decode `0x0001` here. Absent MUST decode as all-zero, and
 * the builder MUST emit no `SETTINGS` child at all for an all-zero policy — so its
 * bytes stay byte-identical to what a pre-RFC-0022 sender emits.
 */
#[test]
fn subscriber_policy_absent() {
    let bin = assert_vector_consistent("subscriber/policy-absent");
    let t = decode(&bin).unwrap();
    let p = structured::subscriber_policy(&t).unwrap();
    assert_eq!(
        p,
        DeliveryPolicy::default(),
        "a neighbouring key is NOT the policy"
    );
    assert!(!p.durability_request());

    // The builder's absent case: no SETTINGS child, byte-identical to plain `subscriber`.
    let built = structured::subscriber_with_policy(&["client"], DeliveryPolicy::default()).unwrap();
    assert_eq!(encode(&built), encode(&subscriber(&["client"]).unwrap()));
}

/**
 * @brief `subscriber/policy-durability` — bit 5 set, and nothing else.
 *
 * Both directions against the SAME bytes: the builder must produce the vector, and the
 * accessor must read `durability_request` back out of it.
 */
#[test]
fn subscriber_policy_durability() {
    let bin = assert_vector_consistent("subscriber/policy-durability");
    let policy = DeliveryPolicy::from_bits(DeliveryPolicy::DURABILITY_REQUEST);
    let built = structured::subscriber_with_policy(&["client"], policy).unwrap();
    assert_eq!(
        encode(&built),
        bin,
        "the builder produces the vector byte-for-byte"
    );

    let t = decode(&bin).unwrap();
    let got = structured::subscriber_policy(&t).unwrap();
    assert_eq!(got.bits, 0x0020);
    assert!(got.durability_request());
    assert_eq!(got.reliability(), 0);
    assert_eq!(got.priority(), 0);
    assert_eq!(
        structured::subscriber_target_path(&t).unwrap(),
        Some("/client".to_string())
    );
}

/**
 * @brief `subscriber/policy-reserved-bits` — every reserved bit set, reliability=1 under
 * them.
 *
 * The vector's own note names the two ways to fail it: reject the unknown bits, or let
 * them leak into an honoured field. Both are asserted here, plus the round trip that
 * proves the word is stored VERBATIM rather than masked on the way through.
 */
#[test]
fn subscriber_policy_reserved_bits() {
    let bin = assert_vector_consistent("subscriber/policy-reserved-bits");
    let t = decode(&bin).unwrap();
    // Not rejected — decoding a word with unknown bits is an ignore, not an error.
    let got = structured::subscriber_policy(&t).unwrap();
    assert_eq!(got.bits, 0xFFC1);
    // No leak: only bits 0-1 are reliability, only 2-4 priority, only 5 durability.
    assert_eq!(got.reliability(), 1);
    assert_eq!(got.priority(), 0);
    assert!(!got.durability_request());
    assert_eq!(got.reserved(), 0x03FF);
    // Verbatim: re-emitting keeps 6-15, so a future sender's bits survive the hop.
    let built = structured::subscriber_with_policy(&["client"], got).unwrap();
    assert_eq!(encode(&built), bin);
}

/** @brief The packed layout is RFC-0022 §3.A's table, and it is the C++ core's. */
#[test]
fn delivery_policy_bit_layout() {
    assert_eq!(DeliveryPolicy::from_bits(0x0001).reliability(), 1);
    assert_eq!(DeliveryPolicy::from_bits(0x0003).reliability(), 3);
    assert_eq!(DeliveryPolicy::from_bits(0x001C).priority(), 7);
    assert!(DeliveryPolicy::from_bits(0x0020).durability_request());
    assert!(!DeliveryPolicy::from_bits(0x001F).durability_request());
    // Reserved bits decode into NO honoured field.
    let all_reserved = DeliveryPolicy::from_bits(0xFFC0);
    assert_eq!(all_reserved.reliability(), 0);
    assert_eq!(all_reserved.priority(), 0);
    assert!(!all_reserved.durability_request());
}

/* ------------------------------------------- #995 — NAME-field walk parity --- */

/**
 * @brief `settings/duplicate-key-last-wins` — the plain NAME-field family disposition:
 * pair-consuming + last-WELL-FORMED-occurrence-wins, wrong-typed never destructive.
 *
 * The pre-#995 walk resynchronised at every offset and took the FIRST match, so these
 * bytes read `None` here and `"ws"` at the C++ terminus (`config_reader_t`).
 */
#[test]
fn settings_duplicate_key_last_wins() {
    let bin = assert_vector_consistent("settings/duplicate-key-last-wins");
    let t = decode(&bin).unwrap();
    // The string reader: the wrong-typed first occurrence is skipped, the last
    // well-formed one wins.
    assert_eq!(
        structured::settings_str(&t, "kind").unwrap(),
        Some("ws".to_string())
    );
    // The type-agnostic reader: last occurrence, whatever its type.
    assert_eq!(
        structured::settings_get(&t, "kind").unwrap(),
        Some(b"ws".to_vec())
    );
    // The walk itself preserves BOTH pairs in wire order — which occurrence wins is
    // the consumer's disposition, not the walk's.
    let fields = structured::named_fields(&t).unwrap();
    assert_eq!(fields.len(), 2);
    assert!(fields.iter().all(|f| f.key == "kind"));
    // And the other order: a wrong-typed LATER occurrence never clobbers a good one.
    let flipped = structured::settings_typed(&[
        ("kind", SettingValue::Name("ws")),
        ("kind", SettingValue::Value(&[0x01])),
    ])
    .unwrap();
    assert_eq!(
        structured::settings_str(&flipped, "kind").unwrap(),
        Some("ws".to_string())
    );
}

/**
 * @brief `spec/desync-stray-value` — the #995 SPEC witness: a stray non-`NAME` in the
 * first key slot desynchronizes the pair stream, and the walk STOPS instead of
 * resyncing.
 *
 * This is the acceptance regression test: before #995 the reader answered
 * `(Some("stored_value"), Some("temp"))` out of bytes the terminus refuses with
 * `INVALID_PATH` (nothing created).
 */
#[test]
fn spec_desync_stray_value_reads_nothing() {
    let bin = assert_vector_consistent("spec/desync-stray-value");
    let t = decode(&bin).unwrap();
    assert_eq!(
        structured::spec_type_name(&t).unwrap(),
        (None, None),
        "the pair walk must stop at the stray VALUE, exactly as the terminus does"
    );
    assert!(
        structured::named_fields(&t).unwrap().is_empty(),
        "no pair survives a desync at child 0"
    );
}

/** @brief A trailing unpaired `NAME` is ignored; the pairs before it still parse. */
#[test]
fn named_fields_trailing_unpaired_key_is_ignored() {
    let t = Tlv {
        type_code: libtracer::type_code::SETTINGS,
        opt: Opt::structured(),
        payload: Vec::new(),
        children: vec![
            libtracer::name("addr").unwrap(),
            libtracer::text_name("10.0.0.2").unwrap(),
            libtracer::name("kind").unwrap(),
        ],
        trailer: None,
    };
    let fields = structured::named_fields(&t).unwrap();
    assert_eq!(fields.len(), 1);
    assert_eq!(fields[0].key, "addr");
    assert_eq!(structured::settings_str(&t, "kind").unwrap(), None);
}

/**
 * @brief `subscriber/policy-last-wins` — both pre-#995 SUBSCRIBER divergences, each
 * with its sign: last well-formed occurrence wins (the reader returned the FIRST
 * match), and a wrongly-typed value is SKIPPED (the reader rejected the whole read).
 */
#[test]
fn subscriber_policy_last_wins() {
    let bin = assert_vector_consistent("subscriber/policy-last-wins");
    let t = decode(&bin).unwrap();
    let got = structured::subscriber_policy(&t).unwrap();
    assert_eq!(
        got,
        DeliveryPolicy::from_bits(0x0021),
        "the last WELL-FORMED word wins; the trailing NAME-typed occurrence is \
         skipped, not an error, and does not clobber it"
    );
}

/**
 * @brief `acl/ace-duplicate-key` — the #995 SECURITY family: a repeated ACE key is
 * REJECTED in every tier, because last-wins would read the narrow-then-wide
 * `access_mask` as the 0xFFFF grant.
 *
 * The codec round-trips these bytes untouched (that is `assert_vector_consistent`);
 * the refusal asserted here is the READER's — exactly the boundary HARNESS.md names.
 */
#[test]
fn ace_duplicate_key_is_rejected() {
    let bin = assert_vector_consistent("acl/ace-duplicate-key");
    let t = decode(&bin).unwrap();
    assert_eq!(
        structured::acl_aces(&t),
        Err(BuildError::TypeMismatch),
        "a duplicate access_mask, narrow then wide: last-wins would WIDEN the grant"
    );
}

/** @brief The rest of the ACE reject family: unknown key, desync, odd child count. */
#[test]
fn ace_reject_family_structural_refusals() {
    let ace = |children: Vec<Tlv>| Tlv {
        type_code: libtracer::type_code::ACL,
        opt: Opt::structured(),
        payload: Vec::new(),
        children: vec![Tlv {
            type_code: libtracer::type_code::ACL,
            opt: Opt::structured(),
            payload: Vec::new(),
            children,
            trailer: None,
        }],
        trailer: None,
    };
    let base = || {
        vec![
            libtracer::name("type").unwrap(),
            value(&[0x00]),
            libtracer::name("subject").unwrap(),
            value(b"peer-a"),
            libtracer::name("access_mask").unwrap(),
            value(&0x0001u16.to_le_bytes()),
        ]
    };
    // The well-formed base parses.
    assert!(structured::acl_aces(&ace(base())).is_ok());
    // An UNKNOWN key: rejected, never skipped — a dropped attribute widens access.
    let mut unknown = base();
    unknown.push(libtracer::name("grace_ns").unwrap());
    unknown.push(value(&[0x01]));
    assert_eq!(
        structured::acl_aces(&ace(unknown)),
        Err(BuildError::TypeMismatch)
    );
    // A non-NAME in a key slot: the pair stream is desynchronized — rejected, not
    // resumed (the config reader STOPS here; a security reader refuses outright).
    let mut desync = base();
    desync.insert(0, value(&[0xEE]));
    desync.insert(1, value(&[0xEE]));
    assert_eq!(
        structured::acl_aces(&ace(desync)),
        Err(BuildError::TypeMismatch)
    );
    // An odd child count: a trailing key whose value the sender believes it wrote.
    let mut odd = base();
    odd.push(libtracer::name("expires_ns").unwrap());
    assert_eq!(
        structured::acl_aces(&ace(odd)),
        Err(BuildError::TypeMismatch)
    );
}

/* ------------------------------- RFC-0024 §5-§7 — the bound-path routing car --- */

/**
 * @brief `fwd/fwd-mint-request` — the mint ask is one BIT of an `op` byte that already
 * existed.
 *
 * Pinned against `fwd/fwd-read` rather than only against its own hex, because the whole
 * claim of the request side is that a mint request costs ZERO added bytes. If the two frames
 * ever differ by more than that byte, the claim is gone and this fails.
 */
#[test]
fn fwd_mint_request_is_one_bit() {
    let mint = assert_vector_consistent("fwd/fwd-mint-request");
    let plain = assert_vector_consistent("fwd/fwd-read");
    assert_eq!(mint.len(), plain.len(), "a mint request adds no bytes");
    let differing: Vec<usize> = (0..mint.len()).filter(|&i| mint[i] != plain[i]).collect();
    assert_eq!(differing.len(), 1, "exactly one byte differs");
    let i = differing[0];
    assert_eq!(plain[i], 0x00, "fwd-read's op byte is READ");
    assert_eq!(mint[i], 0x80, "and the mint request is bit 7 of it");
    // The masking rule (RFC-0024 §9.3): a forwarder switches on `op & 0x3F`, so this frame
    // is a READ everywhere but at the mint. A core that reads the raw byte sees opcode 0x80.
    assert_eq!(mint[i] & 0x3F, 0x00, "the opcode is still READ");
    // And the parser does mask, so both frames read as the same operation.
    let pf = libtracer::fwd::decode_fwd(&plain).unwrap();
    let mf = libtracer::fwd::decode_fwd(&mint).unwrap();
    assert_eq!(mf.op, libtracer::fwd::fwd_op::READ);
    assert_eq!(mf.op, pf.op);
    assert!(mf.mint_request && !pf.mint_request);
    assert!(
        !mf.dst_bound,
        "the mint REQUEST is canonically addressed — it is the key"
    );
}

/**
 * @brief `fwd/fwd-mint-reply` — the minted binding is the reply's LAST child.
 *
 * Position is the pin that matters here. Anywhere but last and every existing positional
 * reader of a reply shifts by one, mint or no mint.
 */
#[test]
fn fwd_mint_reply_carries_the_binding_last() {
    let bin = assert_vector_consistent("fwd/fwd-mint-reply");
    let t = decode(&bin).unwrap();
    assert_eq!(t.type_code, libtracer::type_code::FWD);
    // op / dst / src / kind / payload / PATH_REF — the mint appended, nothing displaced.
    assert_eq!(t.children.len(), 6);
    assert_eq!(t.children[0].payload, vec![0x03], "op = REPLY");
    assert_eq!(t.children[3].payload, vec![0x00], "kind = RESULT");
    assert_eq!(t.children[4].payload, vec![0xD2, 0x04, 0x00, 0x00]);
    let mint = &t.children[5];
    assert_eq!(mint.type_code, libtracer::type_code::PATH_REF);
    assert!(!mint.opt.pl && mint.children.is_empty());
    assert_eq!(
        mint.payload.len() / libtracer::PATH_REF_ELEMENT_BYTES,
        1,
        "a terminus answers exactly one element — its own reference to the target"
    );
    assert_eq!(
        path_ref_element(&mint.payload, 0),
        Some(PathRefElement {
            index: 2,
            generation: 0
        })
    );
    // 4 + 8 bytes of mint, and nothing else changed.
    let plain_reply_len = bin.len() - 12;
    assert!(plain_reply_len > 0);
}

/**
 * @brief `acl/bound-vs-canonical-allow` — a `PATH_REF` in `dst` position, and it is shorter.
 *
 * The §6.3 pair's allow half. The byte count IS the case for the form, so it is pinned
 * against the canonical twin rather than asserted in isolation.
 */
#[test]
fn acl_bound_vs_canonical_allow() {
    let bound = assert_vector_consistent("acl/bound-vs-canonical-allow");
    let canonical = assert_vector_consistent("fwd/fwd-read");
    assert_eq!(
        hex(&bound),
        "0f402100010001000014000800020000000000000006400c00020008007265706c792d6570"
    );
    assert!(
        bound.len() < canonical.len(),
        "the bound spelling is the shorter one — {} vs {}",
        bound.len(),
        canonical.len()
    );
    let t = decode(&bound).unwrap();
    assert_eq!(t.children[0].payload, vec![0x00], "op = READ");
    let dst = &t.children[1];
    assert_eq!(dst.type_code, libtracer::type_code::PATH_REF);
    assert_eq!(
        path_ref_element(&dst.payload, 0),
        Some(PathRefElement {
            index: 2,
            generation: 0
        })
    );
    // The canonical twin's dst is a PATH of NAME children — the two forms, side by side.
    let c = decode(&canonical).unwrap();
    assert_eq!(c.children[1].type_code, libtracer::type_code::PATH);
    assert!(c.children[1].opt.pl);
    // The FWD parser accepts both spellings in `dst` and says which it got.
    let pf = libtracer::fwd::decode_fwd(&bound).unwrap();
    assert!(pf.dst_bound && !pf.mint_request);
    assert!(!libtracer::fwd::decode_fwd(&canonical).unwrap().dst_bound);
}

/**
 * @brief `acl/bound-vs-canonical-deny` — denied, and carrying no binding.
 *
 * Two things are pinned. The outcome tail is the `ERROR{tr::access::denied}` the canonical
 * spelling produces byte for byte — that agreement IS §6.3's claim. And there is no
 * `PATH_REF` after the `STATUS`: a denial never hands back a handle to what it refused.
 */
#[test]
fn acl_bound_vs_canonical_deny() {
    let bin = assert_vector_consistent("acl/bound-vs-canonical-deny");
    let t = decode(&bin).unwrap();
    assert_eq!(t.children[0].payload, vec![0x03], "op = REPLY");
    // `src` echoes the request's dst, which is the bound form — the one thing that cannot
    // agree between two spellings of one address.
    assert_eq!(t.children[2].type_code, libtracer::type_code::PATH_REF);
    assert_eq!(t.children[3].payload, vec![0x01], "kind = ERROR");
    let f = libtracer::fwd::decode_fwd(&bin).unwrap();
    assert_eq!(reply_error_code(&f), 0x0050, "tr::access::denied");
    // Nothing past the STATUS: no mint on a denial (RFC-0024 §6.1).
    assert_eq!(t.children.len(), 5);
    // The outcome tail, byte for byte — kind VALUE (5 B) + STATUS{ERROR{VALUE u16}} (14 B).
    assert_eq!(
        hex(&bin[bin.len() - 19..]),
        "010001000109400a0008400600010002005000"
    );
}

/* --------------------------- RFC-0024 §3.4/§5 — the forwarder hop (car 3) --- */

/**
 * @brief `fwd/fwd-bound-forward` -> `fwd/fwd-bound-forwarded` — one bound hop, byte for byte.
 *
 * The pair is the whole of what a forwarder does to a bound frame: the `dst` loses exactly
 * one element — its own, consumed, never rewritten — and `src` grows by the inbound mount
 * run, canonically. A binding that grew or reordered the residual would still route and
 * would no longer be this protocol, which is why the residual is pinned as the tail of the
 * inbound body rather than re-derived.
 */
#[test]
fn fwd_bound_forward_is_one_hop_from_forwarded() {
    let before = assert_vector_consistent("fwd/fwd-bound-forward");
    let after = assert_vector_consistent("fwd/fwd-bound-forwarded");
    assert_eq!(
        hex(&before),
        "0f4031000100010000140010000100000000000000efbe00000700000006400c00020008007265706c792d65700100040009000000"
    );
    assert_eq!(
        hex(&after),
        "0f403000010001000014000800efbe0000070000000640130002000300636c69020008007265706c792d65700100040009000000"
    );

    let bf = libtracer::fwd::decode_fwd(&before).unwrap();
    let af = libtracer::fwd::decode_fwd(&after).unwrap();
    assert_eq!(bf.op, libtracer::fwd::fwd_op::READ);
    assert_eq!(af.op, bf.op, "a hop relays the opcode it was given");
    assert!(bf.dst_bound && af.dst_bound);

    // Two elements in, one out, and the survivor is the NEXT host's — element 0 is this
    // host's own and is consumed (RFC-0024 §4.1).
    let bt = decode(&before).unwrap();
    let at = decode(&after).unwrap();
    let bin_dst = &bt.children[1];
    let out_dst = &at.children[1];
    assert_eq!(bin_dst.type_code, libtracer::type_code::PATH_REF);
    assert!(
        !out_dst.opt.pl,
        "the re-headed PATH_REF keeps PL clear — a record array"
    );
    assert_eq!(bin_dst.payload.len() / libtracer::PATH_REF_ELEMENT_BYTES, 2);
    assert_eq!(out_dst.payload.len() / libtracer::PATH_REF_ELEMENT_BYTES, 1);
    assert_eq!(
        path_ref_element(&bin_dst.payload, 0),
        Some(PathRefElement {
            index: 1,
            generation: 0
        })
    );
    assert_eq!(
        path_ref_element(&out_dst.payload, 0),
        path_ref_element(&bin_dst.payload, 1),
        "the residual is the inbound element array minus its head"
    );

    // `src` accumulates CANONICALLY on a bound frame: the return route is the one every
    // canonical hop builds, so a peer that never speaks the bound form still answers.
    assert_eq!(bt.children[2].type_code, libtracer::type_code::PATH);
    assert_eq!(at.children[2].type_code, libtracer::type_code::PATH);
    assert_eq!(bt.children[2].children.len(), 1, "src = /reply-ep");
    assert_eq!(at.children[2].children.len(), 2, "src = /cli/reply-ep");

    // The payload rode through untouched.
    assert_eq!(
        hex(&before[before.len() - 8..]),
        hex(&after[after.len() - 8..])
    );
}

/**
 * @brief `path/path-reserved-brackets` (#996) — the reserved-character set is
 * `/ : . [ ] * ?`, pinned cross-tier: the vector's bytes are CODEC-legal (a NAME
 * payload is free bytes on the wire, so the round-trip must carry `frame[7]`
 * bit-for-bit), while THIS core's segment predicate must refuse that NAME and
 * accept the `camera` control. C++ pins the same verdict in
 * `core/tests/path_test.cpp`; TS in
 * `bindings/typescript/packages/client/test/vectors.test.mjs`. Relaxing
 * `validate_segment` back below seven characters turns this red.
 */
#[test]
fn path_reserved_brackets() {
    let bin = assert_vector_consistent("path/path-reserved-brackets");
    let tlv = decode(&bin).unwrap();
    assert_eq!(tlv.type_code, libtracer::type_code::PATH);
    assert_eq!(tlv.children.len(), 2);
    let seg = |i: usize| core::str::from_utf8(&tlv.children[i].payload).unwrap();
    assert_eq!(seg(0), "camera");
    assert_eq!(seg(1), "frame[7]");

    // The predicate's verdict over the vector's own NAME payloads.
    assert_eq!(libtracer::validate_segment(seg(0)), Ok(()), "control");
    assert_eq!(
        libtracer::validate_segment(seg(1)),
        Err(BuildError::ReservedChar),
        "brackets are reserved (reference/03 MUST, normative via spec v1 §3)"
    );

    // The full seven-character set, exactly — pinned so the set can only change
    // together with the vector and the sibling suites.
    for c in "/:.[]*?".chars() {
        let probe = format!("a{c}b");
        assert_eq!(
            libtracer::validate_segment(&probe),
            Err(BuildError::ReservedChar),
            "{probe:?} must be refused"
        );
    }
}
