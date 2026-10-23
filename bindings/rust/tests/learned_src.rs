// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
/*!
 * @brief #2245 — the codec parses a learned chain (RFC-0029 §6.2 / §7.1).
 *
 * A PAIR is an escape record inside a `PATH` body: `00 16 08` + u32 index + u32 generation,
 * 11 bytes. A reply's `src` and a forwarded subscribe's `0x15` reverse list both carry them,
 * so neither body is a multiple of 8 any more and neither may be refused by the retired
 * RFC-0024 fixed-stride rule, which now applies to `0x14` alone.
 */
use libtracer::fwd::{fwd_kind, fwd_op};
use libtracer::path::path_to_tlv;
use libtracer::{decode, decode_fwd, encode, is_path_ref_type, type_code, value_u8, Opt, Tlv};

/** @brief One PAIR record: escape kind 0x16, 8-byte body (u32 index, u32 generation) LE. */
fn pair(index: u32, gen: u32) -> Vec<u8> {
    let mut out = vec![0x00, 0x16, 0x08];
    out.extend_from_slice(&index.to_le_bytes());
    out.extend_from_slice(&gen.to_le_bytes());
    out
}

fn opaque(type_code: u8, payload: Vec<u8>) -> Tlv {
    Tlv {
        type_code,
        opt: Opt::default(),
        payload,
        ..Tlv::default()
    }
}

fn fwd(children: Vec<Tlv>) -> Vec<u8> {
    encode(&Tlv {
        type_code: type_code::FWD,
        opt: Opt {
            pl: true,
            ..Opt::default()
        },
        children,
        ..Tlv::default()
    })
}

#[test]
fn only_0x14_keeps_the_fixed_stride_rule() {
    assert!(is_path_ref_type(type_code::PATH_REF));
    assert!(!is_path_ref_type(type_code::PATH_REF_REVERSE));
}

#[test]
fn a_reply_whose_src_is_a_learned_chain_parses() {
    let mut chain = pair(7, 1);
    chain.extend(pair(3, 2)); // the target's PAIR, then one relaying hop's
    let bytes = fwd(vec![
        value_u8(fwd_op::REPLY),
        path_to_tlv("/client/q1").unwrap(),
        opaque(type_code::PATH, chain.clone()),
        value_u8(fwd_kind::RESULT),
    ]);
    assert!(!bytes.is_empty());
    let parsed = decode_fwd(&bytes).expect("a PAIR src is a PATH like any other");
    assert_eq!(parsed.op, fwd_op::REPLY);
    assert_eq!(parsed.src.payload, chain);
}

#[test]
fn a_forwarded_subscribe_carries_its_reverse_list_with_no_flag() {
    let mut rev = pair(5, 9); // 11 B: not a multiple of 8
    rev.push(2);
    rev.extend_from_slice(b"up"); // a hop without a PAIR adds its NAME run
    let bytes = fwd(vec![
        value_u8(fwd_op::WRITE),
        path_to_tlv("/sensor").unwrap(),
        path_to_tlv("/client/sub1").unwrap(),
        value_u8(0x2a),
        opaque(type_code::PATH_REF_REVERSE, rev.clone()),
    ]);
    assert!(
        decode(&bytes).is_ok(),
        "an 11-byte-stride 0x15 body decodes"
    );
    let parsed = decode_fwd(&bytes).unwrap();
    assert_eq!(parsed.op, fwd_op::WRITE, "a bare WRITE: bit 7 is never set");
    assert_eq!(parsed.reverse.map(|r| r.payload), Some(rev));
    assert!(parsed.payload.is_some());
}

#[test]
fn a_retired_path_ref_dst_is_refused() {
    let bytes = fwd(vec![
        value_u8(fwd_op::READ),
        opaque(type_code::PATH_REF, vec![0; 8]),
        path_to_tlv("/client/q1").unwrap(),
    ]);
    assert!(decode_fwd(&bytes).is_err());
}
