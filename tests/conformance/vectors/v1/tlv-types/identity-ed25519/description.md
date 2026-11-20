# tlv-types/identity-ed25519

The node-identity record that `read <vertex>:identity` serves (annex 05 §The node-identity
record, RFC-0011): the complete ed25519 form, 60 bytes. The key is the recognisable run
`00 01 … 1F`, not a real public key.

```
0b403800020004006b696e640100010001020003006b657901002000000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
```

```
0B 40 38 00                  SETTINGS, PL = 1, length 56
   02 00 04 00 "kind"           NAME "kind"
   01 00 01 00 01               VALUE u8 = 0x01 (ed25519)
   02 00 03 00 "key"            NAME "key"
   01 00 20 00 00 01 … 1F       VALUE, the 32-byte key
```

## The rule this pins

The two required members come in that fixed order. `kind` is from the identity-kind registry,
and kind `0x01` requires exactly 32 key bytes. A node holding a keypair serves these bytes,
byte-identical, at every vertex of its graph and on every transport. The read is exempt from the
READ gate. A keyless node answers `ERROR{tr::schema::not_found}`, never an empty record.

## Bound by

C++ `core/tests/identity_test.cpp`: `test_record_is_byte_exact` compares the served record with
these bytes. The other tests in that file cover the node-scoped, pre-auth and keyless rules.
