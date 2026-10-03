// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/*!
 * @brief One concept: subscribing is a write. A consumer writes a SUBSCRIBER naming its
 * own endpoint into the producer's `:subscribers[]`, and each later delivery arrives as
 * an ordinary FWD{WRITE} addressed to that endpoint (RFC-0004 §C/§D).
 *
 * Run: `cargo run --example fwd_subscribe` (from `bindings/rust/`).
 */

use libtracer::fwd::{fwd_dst_path, fwd_op};
use libtracer::structured::subscriber_target_path;
use libtracer::{decode_fwd, encode_fwd_bytes, subscriber, value_u32, FieldSel, FwdRequest};

fn main() {
    // Consumer -> producer: WRITE /sensor/temp:subscribers[] <- SUBSCRIBER{target=/client}.
    let mut req = FwdRequest::new(fwd_op::WRITE, &["sensor", "temp"], &["client"]);
    req.field = Some(FieldSel::Str(":subscribers[]"));
    req.payload = Some(subscriber(&["client"]).unwrap());
    let frame = encode_fwd_bytes(&req).unwrap();
    println!("subscribe ({} bytes): {:02x?}", frame.len(), frame);

    // The producer sees a field-write whose payload names where to deliver.
    let seen = decode_fwd(&frame).unwrap();
    assert!(seen.field.is_some(), "a subscribe is addressed to a field");
    let target = subscriber_target_path(seen.payload.as_ref().unwrap()).unwrap();
    assert_eq!(target.as_deref(), Some("/client"));
    println!("producer will deliver to {}", target.unwrap());

    // Producer -> consumer: a delivery is a WRITE of the new VALUE to that target.
    let mut push = FwdRequest::new(fwd_op::WRITE, &["client"], &["sensor", "temp"]);
    push.payload = Some(value_u32(21_750));
    let delivery = decode_fwd(&encode_fwd_bytes(&push).unwrap()).unwrap();
    assert_eq!(delivery.op, fwd_op::WRITE);
    assert_eq!(fwd_dst_path(&delivery).unwrap(), "/client");
    assert_eq!(delivery.payload.unwrap().payload_uint(), 21_750);
    println!("delivery: WRITE /client <- 21750");
}
