// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/*!
 * @brief One concept: reading a node's `:stats` counters. One READ names one seam and
 * answers one SETTINGS block of `NAME`/u64 pairs (RFC-0010 Amendment 1).
 *
 * Run: `cargo run --example fwd_read_stats` (from `bindings/rust/`).
 */

use libtracer::fwd::{fwd_kind, fwd_op};
use libtracer::structured::{settings_get, settings_typed, SettingValue};
use libtracer::{decode_fwd, encode, encode_fwd_bytes, FieldSel, FwdRequest};

fn main() {
    // READ <any vertex>:stats.graph.delivery — the seam is node-scoped.
    let mut req = FwdRequest::new(fwd_op::READ, &["sensor", "temp"], &["client"]);
    req.field = Some(FieldSel::Str(":stats.graph.delivery"));
    let frame = encode_fwd_bytes(&req).unwrap();
    println!("READ :stats.graph.delivery ({} bytes)", frame.len());

    // What a fresh node answers: four u64 LE counters, all zero. (Built here so the
    // example runs without a peer; the `settings/stats-seam-block` vector pins it.)
    let zero = 0u64.to_le_bytes();
    let names = ["no_target", "denied", "out_of_memory", "fan_out_truncated"];
    let pairs: Vec<_> = names
        .iter()
        .map(|n| (*n, SettingValue::Value(&zero)))
        .collect();
    let block = settings_typed(&pairs).unwrap();
    assert_eq!(encode(&block).len(), 113);

    let mut reply = FwdRequest::new(fwd_op::REPLY, &["client"], &["sensor", "temp"]);
    reply.kind = Some(fwd_kind::RESULT);
    reply.payload = Some(block);
    let answer = decode_fwd(&encode_fwd_bytes(&reply).unwrap()).unwrap();

    // A reader looks counters up by NAME and ignores any it does not know.
    let settings = answer.payload.expect("a RESULT carries the block");
    for name in names {
        let raw = settings_get(&settings, name)
            .unwrap()
            .expect("counter present");
        let count = u64::from_le_bytes(raw.try_into().expect("fixed-width u64"));
        println!("{name:>18} = {count}");
        assert_eq!(count, 0);
    }
    assert_eq!(settings_get(&settings, "not_a_counter").unwrap(), None);
}
