// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/*!
 * @brief One concept: a VALUE frame with a CRC trailer, and what `decode` says when a
 * byte flips in transit.
 *
 * Run: `cargo run --example value_crc` (from `bindings/rust/`).
 */

use libtracer::{decode, encode, value_opts, CrcWidth, Error, ValueOptions};

fn main() {
    // A 4-byte reading, with the opt-in CRC-32C trailer (opt.CR = 1).
    let reading = 21_500u32.to_le_bytes();
    let opts = ValueOptions {
        crc: true,
        ..ValueOptions::default()
    };
    let frame = encode(&value_opts(&reading, &opts));

    // 4-byte header + 4 payload bytes + 4 CRC bytes.
    println!("frame ({} bytes): {:02x?}", frame.len(), frame);
    assert_eq!(frame.len(), 4 + 4 + 4);

    // Decoding checks the trailer and hands back the payload unchanged.
    let tlv = decode(&frame).expect("an intact frame decodes");
    assert_eq!(tlv.payload, reading);
    let crc = tlv
        .trailer
        .and_then(|t| t.crc)
        .expect("the trailer carries the CRC");
    assert_eq!(crc.width, CrcWidth::Crc32c);
    println!(
        "decoded payload {:02x?}, crc32c {:#010x}",
        tlv.payload, crc.value
    );

    // Flip one payload bit: the frame still parses, but the CRC no longer matches.
    let mut damaged = frame.clone();
    damaged[4] ^= 0x01;
    assert_eq!(decode(&damaged), Err(Error::FrameCrcFail));
    println!("one flipped bit -> {}", Error::FrameCrcFail.name());

    // Without the trailer the same damage is invisible to the codec.
    let mut bare = encode(&value_opts(&reading, &ValueOptions::default()));
    bare[4] ^= 0x01;
    assert!(decode(&bare).is_ok());
    println!("same flip without a CRC -> decodes silently");
}
