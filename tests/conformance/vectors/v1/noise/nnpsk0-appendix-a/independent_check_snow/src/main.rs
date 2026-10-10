// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
/*!
 * Reproduce RFC-0033 Appendix A with `snow`, a third-party Noise implementation in Rust
 * (RFC-0033 §15 Q6), over its default resolver: curve25519-dalek, the RustCrypto
 * `chacha20poly1305` and `sha2`. None of those is the library the Appendix A generator used
 * (Python `cryptography`, which is OpenSSL), so this check is independent in the Noise logic
 * and in the primitives.
 *
 * From transcript.json's inputs alone it runs both sides, then compares both handshake
 * messages, the ephemeral public keys, both payloads, the handshake hash, the two transport
 * keys (`dangerously_get_raw_split`) and the three transport datagrams, sealed at the
 * transcript's explicit nonces (Noise §11.4) and framed as RFC-0033 §5.3. snow does not
 * expose its symmetric state between tokens, so the per-token ck/h/k rows are checked by
 * `independent_check_noiseprotocol.py` and by the C++ conformance runner.
 *
 * Run (the result is recorded in ../independent_check_snow.txt):
 *
 *     cargo run --release --manifest-path independent_check_snow/Cargo.toml [transcript.json]
 */

use std::path::PathBuf;

/** Hex-decode a transcript field. */
fn unhex(s: &str) -> Vec<u8> {
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
}

/** Hex-encode bytes. */
fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

/** Build one side with Appendix A's PSK, prologue and fixed ephemeral key. */
fn side(t: &serde_json::Value, initiator: bool) -> snow::HandshakeState {
    let inp = &t["inputs"];
    let psk = unhex(inp["psk"].as_str().unwrap());
    let prologue = unhex(inp["prologue"].as_str().unwrap());
    let e = unhex(
        inp[if initiator { "initiator_ephemeral_private" } else { "responder_ephemeral_private" }]
            .as_str()
            .unwrap(),
    );
    let b = snow::Builder::new(t["protocol_name"].as_str().unwrap().parse().unwrap())
        .psk(0, &psk)
        .prologue(&prologue)
        .fixed_ephemeral_key_for_testing_only(&e);
    if initiator { b.build_initiator().unwrap() } else { b.build_responder().unwrap() }
}

/** Seal one transport datagram: type 0x04, the u64 LE nonce, then the Noise message. */
fn datagram(s: &snow::StatelessTransportState, nonce: u64, pt: &[u8]) -> Vec<u8> {
    let mut out = vec![0u8; 9 + pt.len() + 16];
    out[0] = 0x04;
    out[1..9].copy_from_slice(&nonce.to_le_bytes());
    let n = s.write_message(nonce, pt, &mut out[9..]).unwrap();
    out.truncate(9 + n);
    out
}

/** Open one datagram at its own nonce; None when snow refuses it. */
fn opened(s: &snow::StatelessTransportState, d: &[u8]) -> Option<Vec<u8>> {
    let nonce = u64::from_le_bytes(d[1..9].try_into().unwrap());
    let mut out = vec![0u8; d.len()];
    let n = s.read_message(nonce, &d[9..], &mut out).ok()?;
    out.truncate(n);
    Some(out)
}

fn main() {
    let path = std::env::args().nth(1).map(PathBuf::from).unwrap_or_else(|| {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../transcript.json")
    });
    let t: serde_json::Value =
        serde_json::from_str(&std::fs::read_to_string(&path).unwrap()).unwrap();
    let (inp, der, out) = (&t["inputs"], &t["derived"], &t["outputs"]);
    let field = |v: &serde_json::Value, k: &str| v[k].as_str().unwrap().to_string();
    let nonce = |k: &str| field(inp, k).parse::<u64>().unwrap();
    let mut results: Vec<bool> = Vec::new();
    let mut check = |name: &str, got: Option<&[u8]>, want: &str| {
        let ok = got.map(hex).as_deref() == Some(want);
        println!("{}  {name}", if ok { "ok" } else { "NOT OK" });
        if !ok {
            println!("    want {want}\n    got  {:?}", got.map(hex));
        }
        results.push(ok);
    };

    let (mut i, mut r) = (side(&t, true), side(&t, false));
    let mut buf = [0u8; 128];
    let mut pay = [0u8; 128];

    let n = i.write_message(&unhex(&field(inp, "msg1_payload")), &mut buf).unwrap();
    let m1 = [&[0x01u8][..], &buf[..n]].concat();
    let p1 = r.read_message(&m1[1..], &mut pay).ok().map(|n| pay[..n].to_vec());
    check("responder reads msg1's payload", p1.as_deref(), &field(inp, "msg1_payload"));
    let n = r.write_message(&unhex(&field(inp, "msg2_payload")), &mut buf).unwrap();
    let m2 = [&[0x02u8][..], &buf[..n]].concat();
    let p2 = i.read_message(&m2[1..], &mut pay).ok().map(|n| pay[..n].to_vec());
    check("initiator reads msg2's payload", p2.as_deref(), &field(inp, "msg2_payload"));

    check("msg1 (58 B)", Some(&m1), &field(out, "msg1"));
    check("msg2 (50 B)", Some(&m2), &field(out, "msg2"));
    check("initiator e.pub", Some(&m1[1..33]), &field(der, "initiator_ephemeral_public"));
    check("responder e.pub", Some(&m2[1..33]), &field(der, "responder_ephemeral_public"));
    check("handshake hash (initiator)", Some(i.get_handshake_hash()), &field(out, "handshake_hash"));
    check("handshake hash (responder)", Some(r.get_handshake_hash()), &field(out, "handshake_hash"));
    let (ik1, ik2) = i.dangerously_get_raw_split();
    let (rk1, rk2) = r.dangerously_get_raw_split();
    check("k_i2r (initiator)", Some(&ik1), &field(out, "k_i2r"));
    check("k_i2r (responder)", Some(&rk1), &field(out, "k_i2r"));
    check("k_r2i (initiator)", Some(&ik2), &field(out, "k_r2i"));
    check("k_r2i (responder)", Some(&rk2), &field(out, "k_r2i"));

    let (it, rt) = (
        i.into_stateless_transport_mode().unwrap(),
        r.into_stateless_transport_mode().unwrap(),
    );
    let conf = datagram(&it, nonce("confirmation_nonce_decimal"), &[]);
    let ping = datagram(&it, nonce("frame_i2r_nonce_decimal"), &unhex(&field(inp, "frame_i2r")));
    let pang = datagram(&rt, nonce("frame_r2i_nonce_decimal"), &unhex(&field(inp, "frame_r2i")));
    check("confirmation datagram", Some(&conf), &field(out, "confirmation"));
    check("frame i->r datagram", Some(&ping), &field(out, "datagram_i2r"));
    check("frame r->i datagram", Some(&pang), &field(out, "datagram_r2i"));
    let o = opened(&rt, &unhex(&field(out, "confirmation")));
    check("responder opens the confirmation", o.as_deref(), "");
    let o = opened(&rt, &unhex(&field(out, "datagram_i2r")));
    check("responder opens frame i->r", o.as_deref(), &field(inp, "frame_i2r"));
    let o = opened(&it, &unhex(&field(out, "datagram_r2i")));
    check("initiator opens frame r->i", o.as_deref(), &field(inp, "frame_r2i"));

    let passed = results.iter().filter(|x| **x).count();
    println!("\n{passed}/{} values reproduced by snow 0.9.6 (default resolver)", results.len());
    let ok = passed == results.len();
    println!("INDEPENDENT CHECK: {}", if ok { "PASS" } else { "FAIL" });
    std::process::exit(if ok { 0 } else { 1 });
}
