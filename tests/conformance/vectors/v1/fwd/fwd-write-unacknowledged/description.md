# fwd/fwd-write-unacknowledged

FWD{ op=WRITE, dst=/sensor/temp, src=<zero-length PATH>, payload=VALUE u32=1234 }: the
unacknowledged write.

```
0f402100010001000106000c000673656e736f720474656d700600000001000400d2040000
```

```
0F 40 21 00                          FWD, PL = 1, length 33
   01 00 01 00 01                       VALUE op = WRITE
   06 00 0C 00 06 "sensor" 04 "temp"    PATH dst = /sensor/temp
   06 00 00 00                          PATH src, zero-length: no reply requested
   01 00 04 00 D2 04 00 00              VALUE u32 1234
```

37 bytes total.

## The rule this pins

annex 05 §Reserved range, RFC-0004 Amendment 2: a request whose `src` is a zero-length `PATH`
carries no return route, and the terminus emits no frame for it. A `WRITE` is applied and answered
with silence, whether it succeeds, is refused or is denied by the ACL. The `src` child is empty
and never omitted. The child run is positional, and a `WRITE` payload may itself be a `PATH`, so
an omitted `src` would be ambiguous.

This is the push-ingest form: the producer is the client, and a subscription would invert who
initiates. A producer's delivery leg uses the same empty `src`.

## Bound by

C++ `core/tests/empty_src_unacked_test.cpp`, `test_conformance_vectors`, which feeds these bytes
to a router: the value is applied and zero frames go out. The same frame with a one-segment `src`
is the positive control and draws exactly one reply.
