// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/*!
 * @brief One concept: an address is a PATH TLV whose body is packed segment records,
 * one length byte then the segment's bytes (RFC-0018).
 *
 * Run: `cargo run --example path_tlv` (from `bindings/rust/`).
 */

use libtracer::path::{path_to_tlv, split_path, tlv_to_path};
use libtracer::{decode, encode, type_code, BuildError};

fn main() {
    let tlv = path_to_tlv("/sensor/temp").expect("a rooted, valid path");
    assert_eq!(tlv.type_code, type_code::PATH);

    // The body: [6]"sensor" [4]"temp". No separators, no terminator.
    let mut body = vec![6u8];
    body.extend_from_slice(b"sensor");
    body.push(4);
    body.extend_from_slice(b"temp");
    assert_eq!(tlv.payload, body);
    println!(
        "PATH body ({} bytes): {:02x?}",
        tlv.payload.len(),
        tlv.payload
    );

    // On the wire it is an ordinary opaque TLV: 4-byte header + body.
    let frame = encode(&tlv);
    assert_eq!(frame.len(), 4 + body.len());

    // Decoding and rendering gives back the one canonical spelling.
    let back = tlv_to_path(&decode(&frame).unwrap()).unwrap();
    assert_eq!(back, "/sensor/temp");
    println!("round trip: {back}");

    // A trailing slash is the same address; a relative one is no address at all.
    assert_eq!(split_path("/sensor/temp/").unwrap(), ["sensor", "temp"]);
    assert_eq!(path_to_tlv("sensor/temp"), Err(BuildError::NotRooted));
    println!("\"sensor/temp\" -> NotRooted");
}
