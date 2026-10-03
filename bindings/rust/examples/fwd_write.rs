// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/*!
 * @brief One concept: a remote write is one FWD frame — `op`, the `dst` route, the
 * `src` reply route, then the VALUE (RFC-0004 §B).
 *
 * Run: `cargo run --example fwd_write` (from `bindings/rust/`).
 */

use libtracer::fwd::{fwd_dst_path, fwd_op};
use libtracer::{decode_fwd, encode_fwd_bytes, value_u32, FwdRequest};

fn main() {
    // Write 21500 (a temperature in milli-degrees) to /sensor/temp on a remote node.
    let mut req = FwdRequest::new(fwd_op::WRITE, &["sensor", "temp"], &["client"]);
    req.payload = Some(value_u32(21_500));
    let frame = encode_fwd_bytes(&req).expect("a valid request");
    println!("FWD{{WRITE}} ({} bytes): {:02x?}", frame.len(), frame);

    // The receiving node reads the same four children back, positionally.
    let parsed = decode_fwd(&frame).expect("a well-formed FWD");
    assert_eq!(parsed.op, fwd_op::WRITE);
    let dst = fwd_dst_path(&parsed).unwrap();
    assert_eq!(dst, "/sensor/temp");
    let value = parsed.payload.as_ref().expect("a WRITE carries its value");
    assert_eq!(value.payload_uint(), 21_500);
    println!("dst {dst} <- {}", value.payload_uint());

    // The frame carries no correlation id: the reply finds its way home along `src`.
    assert_eq!(parsed.src.payload, [6, b'c', b'l', b'i', b'e', b'n', b't']);
}
