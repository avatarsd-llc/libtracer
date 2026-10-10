#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Reproduce RFC-0033 Appendix A with a third-party Noise implementation (RFC-0033 §15 Q6).

The handshake runs in `noiseprotocol` (https://github.com/plizonczyk/noiseprotocol), a Noise
implementation this project did not write, from the inputs in transcript.json alone. Every
value the transcript records is then compared with what that library produced:

- the symmetric state (ck, h, k) after each token, read from the library's own SymmetricState
  by wrapping its methods (they are observed, not reimplemented);
- the ephemeral public keys, the ee Diffie-Hellman input and both handshake messages;
- the handshake hash and the two transport keys after Split();
- the three transport datagrams, sealed by the library's CipherState at the transcript's
  explicit nonces (Noise §11.4) and framed as RFC-0033 §5.3 (type byte, u64 LE nonce).

Run (the result is recorded next to this script, in independent_check_noiseprotocol.txt):

    python3 -m venv /tmp/noise-venv && /tmp/noise-venv/bin/pip install noiseprotocol==0.3.1
    /tmp/noise-venv/bin/python -I independent_check_noiseprotocol.py [transcript.json]

Exit status 0 when every value matches, 1 otherwise.
"""

import importlib.metadata
import json
import platform
import struct
import sys
from pathlib import Path

from noise.connection import Keypair, NoiseConnection
from noise.state import SymmetricState

HERE = Path(__file__).resolve().parent


def traced():
    """Wrap SymmetricState's operations so each top-level call records the state after it."""
    log = []
    depth = [0]

    def wrap(name):
        inner = getattr(SymmetricState, name)

        def call(self, arg):
            if depth[0] == 0 and not log_owner(self, log):
                log.append({"who": self, "op": "start", "ck": self.ck, "h": self.h, "k": key(self)})
            depth[0] += 1
            try:
                out = inner(self, arg)
            finally:
                depth[0] -= 1
            if depth[0] == 0:
                log.append({"who": self, "op": name, "arg": bytes(arg), "ck": self.ck,
                            "h": self.h, "k": key(self)})
            return out

        setattr(SymmetricState, name, call)

    for n in ("mix_hash", "mix_key", "mix_key_and_hash", "encrypt_and_hash", "decrypt_and_hash"):
        wrap(n)
    return log


def log_owner(state, log):
    return any(e["who"] is state for e in log)


def key(state):
    k = getattr(state.cipher_state, "k", None)
    return bytes(k) if isinstance(k, (bytes, bytearray)) else None


def side(initiator, inputs):
    c = NoiseConnection.from_name(b"Noise_NNpsk0_25519_ChaChaPoly_SHA256")
    c.set_as_initiator() if initiator else c.set_as_responder()
    c.set_psks(psk=bytes.fromhex(inputs["psk"]))
    c.set_prologue(bytes.fromhex(inputs["prologue"]))
    e = inputs["initiator_ephemeral_private" if initiator else "responder_ephemeral_private"]
    c.set_keypair_from_private_bytes(Keypair.EPHEMERAL, bytes.fromhex(e))
    c.start_handshake()
    return c


def datagram(cipher, nonce, plaintext):
    cipher.set_nonce(nonce)
    return b"\x04" + struct.pack("<Q", nonce) + cipher.encrypt_with_ad(b"", plaintext)


def opened(cipher, data):
    """Open one datagram at its own nonce; None when the library refuses it."""
    nonce = struct.unpack("<Q", data[1:9])[0]
    cipher.set_nonce(nonce)
    try:
        return cipher.decrypt_with_ad(b"", data[9:])
    except Exception:  # noqa: BLE001 (any refusal is a failed check, reported below)
        return None


def main(argv):
    path = Path(argv[1]) if len(argv) > 1 else HERE / "transcript.json"
    t = json.loads(path.read_text())
    inp, der, steps, out = t["inputs"], t["derived"], t["steps"], t["outputs"]
    log = traced()
    results = []

    def check(name, got, want_hex):
        ok = got is not None and bytes(got).hex() == want_hex
        results.append(ok)
        print(f"{'ok' if ok else 'NOT OK'}  {name}")
        if not ok:
            print(f"    want {want_hex}\n    got  {bytes(got).hex() if got is not None else None}")

    i, r = side(True, inp), side(False, inp)
    states = {True: i.noise_protocol.handshake_state.symmetric_state,
              False: r.noise_protocol.handshake_state.symmetric_state}
    p1 = bytes.fromhex(inp["msg1_payload"])
    check("msg1 payload = flags || counter (u64 LE)", bytes.fromhex(inp["msg1_flags"])
          + struct.pack("<Q", int(inp["msg1_counter_decimal"])), inp["msg1_payload"])
    m1 = b"\x01" + bytes(i.write_message(p1))
    got_p1 = r.read_message(m1[1:])
    m2 = b"\x02" + bytes(r.write_message(bytes.fromhex(inp["msg2_payload"])))
    got_p2 = i.read_message(m2[1:])

    check("msg1 (58 B)", m1, out["msg1"])
    check("msg2 (50 B)", m2, out["msg2"])
    check("responder reads msg1's payload", got_p1, inp["msg1_payload"])
    check("initiator reads msg2's payload", got_p2, inp["msg2_payload"])
    check("initiator e.pub", m1[1:33], der["initiator_ephemeral_public"])
    check("responder e.pub", m2[1:33], der["responder_ephemeral_public"])

    # The symmetric-state trace, per side, in token order (Appendix A.3).
    # Both sides run the same token sequence, so both traces carry the same step names.
    names = ["initialize", "prologue", "psk", "initiator_e_h", "initiator_e", "msg1_payload",
             "responder_e_h", "responder_e", "ee", "msg2_payload"]
    for who in (True, False):
        trace = [e for e in log if e["who"] is states[who]]
        label = "initiator" if who else "responder"
        if len(trace) != len(names):
            results.append(False)
            print(f"NOT OK  {label} trace has {len(trace)} steps, want {len(names)}")
            continue
        for step, e in zip(names, trace):
            if step.endswith("_e_h"):  # MixHash(e.pub) inside the e token: h only
                check(f"{label} {step[:-2]} MixHash h", e["h"], steps[step[:-2] + "_h"])
                continue
            for field in ("ck", "h", "k"):
                k = f"{step}_{field}"
                if k in steps and not (step.endswith("_e") and field == "h"):
                    check(f"{label} {k}", e[field], steps[k])
        ee = [e for e in trace if e["op"] == "mix_key"][2]
        check(f"{label} DH(e, re) input to MixKey", ee["arg"], der["dh_ee"])

    check("handshake hash (initiator)", i.get_handshake_hash(), out["handshake_hash"])
    check("handshake hash (responder)", r.get_handshake_hash(), out["handshake_hash"])
    ie, idd = i.noise_protocol.cipher_state_encrypt, i.noise_protocol.cipher_state_decrypt
    re_, rd = r.noise_protocol.cipher_state_encrypt, r.noise_protocol.cipher_state_decrypt
    check("k_i2r (initiator send)", ie.k, out["k_i2r"])
    check("k_i2r (responder receive)", rd.k, out["k_i2r"])
    check("k_r2i (responder send)", re_.k, out["k_r2i"])
    check("k_r2i (initiator receive)", idd.k, out["k_r2i"])

    conf = datagram(ie, int(inp["confirmation_nonce_decimal"]), b"")
    ping = datagram(ie, int(inp["frame_i2r_nonce_decimal"]), bytes.fromhex(inp["frame_i2r"]))
    pang = datagram(re_, int(inp["frame_r2i_nonce_decimal"]), bytes.fromhex(inp["frame_r2i"]))
    check("confirmation datagram", conf, out["confirmation"])
    check("frame i->r datagram", ping, out["datagram_i2r"])
    check("frame r->i datagram", pang, out["datagram_r2i"])
    check("responder opens the confirmation", opened(rd, bytes.fromhex(out["confirmation"])), "")
    check("responder opens frame i->r", opened(rd, bytes.fromhex(out["datagram_i2r"])),
          inp["frame_i2r"])
    check("initiator opens frame r->i", opened(idd, bytes.fromhex(out["datagram_r2i"])),
          inp["frame_r2i"])

    versions = ", ".join(f"{p} {importlib.metadata.version(p)}"
                         for p in ("noiseprotocol", "cryptography"))
    print(f"\n{sum(results)}/{len(results)} values reproduced by {versions}, "
          f"Python {platform.python_version()}")
    print("INDEPENDENT CHECK: PASS" if all(results) else "INDEPENDENT CHECK: FAIL")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
