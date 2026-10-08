# fwd/reply-error-bare-ignored

`FWD{ op=REPLY, dst=/net/downlink/a/net/downlink/cli/reply-ep, src=/sensor/temp, kind=ERROR, ERROR{ VALUE u16=0x0020 tr::path::not_found } }`.
This is the [`fwd/fwd-reply-error`](../fwd-reply-error/description.md) frame with its STATUS
wrapper removed, so the payload is a **bare ERROR**.

## The point this frame depicts

A `kind=ERROR` reply has one error spelling: a `STATUS` carrying the `ERROR`
([RFC-0004](../../../../../../docs/spec/rfcs/0004-remote-operation-addressing.md) §B). Reference/05
§`0x08` once also listed "inline reply payload in implementations that opt to skip the STATUS
wrapper". No emitter ever wrote that form, and no reader ever read it. The RFC-0004 erratum of
2026-10-08 ([#1983](https://github.com/avatarsd-llc/libtracer/issues/1983)) removed it from the text.

This vector pins what a reader does with the bare form:

> A reader MUST NOT surface a bare `ERROR` payload as the reply's error. It reads no registered
> code and no `tr::` path from it, the same answer it gives for a `STATUS` that carries no `ERROR`.

A bare `ERROR` stays a well-formed TLV, and RFC-0002 §C still admits one outside a reply, for
protocol-stack reporting where there is no request to answer. Only the reply-payload spelling is
gone.

## Byte breakdown

`0x0F FWD`, `opt.PL=1`, `length=0x0051` (81), 85 bytes total. Offsets 4–74 are
`fwd/fwd-reply-error` verbatim (see its table). The difference is the missing 4-byte STATUS header
at offset 75, which also shrinks the outer FWD length by 4.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | `0F 40 51 00` | FWD, PL=1, body length 81 |
| 4 | `01 00 01 00 03` | VALUE u8 `0x03`, `op = REPLY` |
| 9 | `06 00 29 00` … | PATH `dst` = `/net/downlink/a/net/downlink/cli/reply-ep`, PL=0, body 41 |
| 54 | `06 00 0C 00` … | PATH `src` = `/sensor/temp`, PL=0, body 12 |
| 70 | `01 00 01 00 01` | VALUE u8 `0x01`, `kind = ERROR` |
| 75 | `08 40 06 00` | **ERROR at top level**, PL=1, body length 6: no STATUS wrapper |
| 79 | `01 00 02 00 20 00` | VALUE u16 LE `0x0020`, `tr::path::not_found` |

## What this vector does and does not gate

Per [HARNESS.md](../../../../HARNESS.md), a vector gates the **codec** only: the contract is
`encode(decode(input.bin)) == input.bin`, which every core satisfies. The reader rule above is a
**behavioural** claim, bound in each core's own suite against these bytes:

- C++ `core/tests/path_label_origin_test.cpp`: `fall_back_on_label_refusal` does not treat it as
  the `NOT_FOUND` refusal that drops a cached label.
- Rust `bindings/rust/tests/conformance_vectors.rs`: `reply_error_code` answers 0 and
  `reply_error_path` answers `None`.
- TypeScript `bindings/typescript/packages/client/test/vectors.test.mjs`: `replyErrorCode`
  answers 0 and `replyErrorPath` answers `null`.

This vector is replaced when RFC-0030 (stage 4,
[#1942](https://github.com/avatarsd-llc/libtracer/issues/1942)) retires `REPLY`: its reply write
carries a top-level `ERROR` (RFC-0030 §8.6), and that is a different frame.
