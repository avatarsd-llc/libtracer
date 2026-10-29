# Batching

## In one paragraph

A batch is many samples sent as one value. The application holds the samples, chooses their
base time and offsets from its own clock, and composes them into one BATCH record. Its write
of that record is the flush: the graph never waits, counts, paces or times anything, and never
learns that the value is a batch. The record has one layout and two choices on it. The samples
are either framed, one `VALUE` each, or packed into one block. The record is either standalone,
headed `0x80`, or folded into a branch write, headed `VALUE`. It is pushed like any other
value: by `propagate`, or by `propagate` FOLD as one branch-write frame for a subtree. One
model, not three features.

## The picture

```{mermaid}
:caption: "Legend: purple = application, sky blue = record, green = vertex, blue = frame."
flowchart TB
    accTitle: One batch, two ways to push it
    accDescr: The application composes one BATCH record. Standalone, it is assigned to vertex /b and propagated as is. Folded, it is assigned to /s/adc0 and propagate FOLD on /s carries it in one branch-write frame. Both reach the subscribing application.
    A([application]):::app -->|compose| R["BATCH record"]:::record
    R --> C{carriage?}
    C -->|"standalone 0x80: assign"| B("vertex /b"):::vertex
    C -->|"folded 0x01: assign"| S("vertex /s/adc0"):::vertex
    B -->|propagate| U([subscriber]):::app
    S -->|"propagate /s FOLD"| F[/"branch-write frame for /s"/]:::frame
    F --> U
```

## When you need it

Use a batch when the cost of framing each sample dominates: an ADC sweep, a PWM timeline, a
fixed-rate capture. One sample per frame pays a TLV header, a routing walk and a ring entry per
sample. A batch pays them once per batch. If samples arrive slowly enough that each one is worth
its own delivery, write them one at a time and skip this page.

Which vertex role holds the batch, and which carriage it uses, pair up. The table is in
[reference 22 §3.5](../reference/22-backpressure-and-sizing.md#35-high-rate-acquisition--recipe-c-compose--swap--push):
a stored vertex holds the newest window, a STREAM vertex holds the last few windows, one ring
entry per batch.

## How it works

**Compose.** The application calls `tr::wire::compose_batch` with its sample views, the base
time, and the offsets when the stream is non-uniform. It gets back a rope: one small head
segment it owns (the record header, the `TIME` base child, the offset run) with the sample
buffers linked on behind it, never copied. For packed samples, `tr::wire::encode_packed_batch`
writes the whole record into a span the caller supplies.

**Two choices on one record.**

| Choice | Option A | Option B | Who decides |
| --- | --- | --- | --- |
| samples | framed: one `VALUE` child per sample | packed: one `VALUE` block of `n × width` bytes | the stream's descriptor (`width`) |
| carriage | standalone: header type `0x80` | folded: header type `VALUE` (`0x01`) | how the application pushes it |
| sample time | uniform: base plus `n × dt_ns`, no bytes per sample | non-uniform: a packed `i32` offset per sample | the descriptor (`dt_ns`) |

Every body byte is the same in both carriages; only the first byte differs. A folded batch sits
in the one structured `VALUE` a branch-write node admits, so the graph never sees a
user-range type at all.

**Swap and push.** `graph_t::assign` swaps the composed value in through the ordinary atomic
last-value publish. `graph_t::propagate(v)` delivers it. `propagate(v, emission_mode_t::FOLD)`
sweeps the subtree under `v` and delivers one branch-write frame instead of one write per
vertex. There is no batch-specific write path and no batch role.

**Read it back.** A subscriber of a standalone batch reads the record with
`tr::wire::read_batch`, or `tr::wire::read_packed_batch` with the descriptor's `width`, and
gets a `batch_view_t`: the sample count, each sample, and each sample's time from the base and
its offset. The descriptor, not the record, says which shape the bytes are.

```{admonition} Open question: reading a folded batch
:class: warning

How a FOLD subscriber knows that a branch-write node's `VALUE` is a batch, and reads it back,
waits on a maintainer ruling (the FOLD-seat reader question raised with #2377, the compiled
batch example). Until it is ruled, this page shows only the standalone read-back.
```

## Worked example

These bytes are read from the conformance vectors when the site builds. The compiled example
[One batch, four spellings](../examples/graph-batch.md) composes the same batch and checks its
output against these vectors byte for byte, under ctest as `example_graph_batch`. Open a row to
see its header fields.

The batch: base `1700000000000000000` ns, offsets `[0, 1500, -250]` ns (a non-uniform stream),
three `u16` samples, framed, standalone.

```{hex-walk} stream/batch-composed-standalone
:caption: One framed batch, standalone.
u64: TIME base, the sample time of frame 0 in ns
i32[]: offsets of each sample from the base, in ns
u16: sample 0
u16: sample 1
u16: sample 2
```

The folded carriage, `stream/batch-composed-folded`, is these 50 bytes with the first one
changed to `01`. Pushed by `propagate(/s, FOLD)` from `/s/adc0`, it arrives inside one
branch-write frame:

```{hex-walk} stream/fold-carries-composed-batch
:caption: The same batch, folded and swept from /s.
str: the root's leading NAME
str: the child vertex's NAME
u64: TIME base, the sample time of frame 0 in ns
i32[]: offsets of each sample from the base, in ns
u16: sample 0
u16: sample 1
u16: sample 2
```

Packed, the three samples become one 6-byte block and the record shrinks from 50 bytes to 42:
the block saves the 4-byte header of every sample but the first. The example prints those bytes
and checks them; `stream/batch-packed-roundtrip` is the packed vector.

## Cost

### Wire bytes

A record is a 4-byte header, a 12-byte `TIME` base, a 4-byte header plus 4 bytes
per sample of offsets on a non-uniform stream, then the samples: `4 + width` bytes each framed,
or one 4-byte header plus `n × width` packed. Above 64 KiB of body the record header grows to 6
bytes (`opt.LL`). For `u16` samples:

| samples | sample bytes | framed, uniform | packed, uniform | framed, non-uniform | packed, non-uniform |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 32 | 64 B | 208 B | 84 B | 340 B | 216 B |
| 512 | 1 KiB | 3,088 B | 1,044 B | 5,140 B | 3,096 B |
| 2,048 | 4 KiB | 12,304 B | 4,116 B | 20,500 B | 12,312 B |
| 8,192 | 16 KiB | 49,168 B | 16,404 B | 81,942 B | 49,176 B |

Packed and uniform is the one shape whose overhead stays flat (20 bytes) as the batch grows.

### Allocations

`compose_batch` allocates one head segment per composition, about 196 bytes,
flat from 64 B to 64 KiB per sample; the copying spelling spends 459 KB at the top of that
ladder (`batch_compose_alloc_test`, figures from
[reference 22 §3.5](../reference/22-backpressure-and-sizing.md#35-high-rate-acquisition--recipe-c-compose--swap--push)).
`encode_packed_batch` and both readers allocate nothing. Delivery is the ordinary write path:
one ring entry per batch at the receiving vertex, not one per sample.

### Latency

One delivery per batch. A sample waits until the application decides the batch is
full, and that wait is the application's own choice.
Not benched: no bench isolates batching yet; a compose-and-propagate row in
`bench_compose_net` would cover it.

### Flash and RAM per target

| Target | Graph RAM | Flash |
| --- | --- | --- |
| NARROW | none: no role, no field, no counter | the batch header's functions, linked only if the app calls them |
| MID | none | same |
| WIDE | none | same |

Flash is not measured: the size gates measure the core without the batch functions, and a
per-feature row in `tools/esp_size_gate.py` would measure them.

## What the library does not do

- It does not decide when a batch is full. Count and window are `if` statements in the
  application's loop; the retired `batch_count` and `batch_window_ns` keys are read by nothing.
- It reads no clock and runs no timer. The base time is the application's number.
- It does not pace or play samples out. Playout is application policy (#2366, retire the
  reference playout helper).
- It keeps no buffer per subscriber. The queue, if any, is the receiving vertex's own ring.
- It does not interpret the record. A receiver that has never heard of BATCH moves it unchanged.

## Pitfalls

- **Symptom:** `propagate(v, FOLD)` is refused. **Cause:** `v` is a STREAM vertex, whose ring
  holds samples with no single foldable value. **Fix:** compose the batch onto a stored vertex
  the sweep visits.
- **Symptom:** the receiving ring refuses writes at a rate it handled before batching.
  **Cause:** the ring was sized for samples, and one entry is now a whole batch. **Fix:** size the
  ring in bytes against the batch, per
  [reference 22 §3.5](../reference/22-backpressure-and-sizing.md#35-high-rate-acquisition--recipe-c-compose--swap--push).
- **Symptom:** a consumer delivers none of a packed batch's samples. **Cause:** the block's length
  is not a multiple of the descriptor's `width`, or the offset count is not the sample count, so
  the batch is malformed. **Fix:** publish the descriptor before the first packed batch, and do
  not mix packed and framed batches on one vertex.
- **Symptom:** sample times read back shifted. **Cause:** the descriptor's `dt_ns` and the record
  disagree about uniform or non-uniform. **Fix:** set `dt_ns` to `0` whenever offsets are sent.

## Where the rules live

**Normative source:** [v1 §3](../spec/v1.md#3-wire-format), the record in
[reference 05 §User range](../reference/05-protocol-tlvs.md#user-range-0x80--0xff), the
branch-write node in
[reference 05 §Branch write](../reference/05-protocol-tlvs.md#branch-write--decomposition), and
the header in [reference 01 §Frame layout](../reference/01-data-format.md#frame-layout).

**Exercised by:** `batch-composed-standalone`, `batch-composed-folded`,
`fold-carries-composed-batch`, `batch-packed-roundtrip`, `batch-packed-malformed`,
`batch-time-roundtrip`, `batch_test`, `batch_compose_alloc_test`.

## History

:::{dropdown} How batching got here
- [RFC-0025](../spec/rfcs/0025-stream-class-values.md) Amendment 1 (2026-08-21) moved sample
  time into a payload `TIME` base with derived or offset per-sample times.
- Amendment 3 (2026-08-21) made a batch the wire form of the `assign` and `propagate` flush,
  added the FOLD emission mode, and assigned BATCH the user-range type `0x80`.
- Amendment 4 (2026-08-23) made batching user-orchestrated: the application composes, its
  write is the flush, and `batch_count` and `batch_window_ns` were retired. An erratum the
  same day scoped `0x80` to a standalone flush and seated a folded batch in `VALUE`.
- Amendment 5 (2026-10-18) added the packed sample block for fixed-width samples.
- #2366 (retire the reference playout helper) moves playout wholly to the application.
:::

Next concept: transports (#2379, the transports pilot). Go deeper:
[reference 22 §3.5](../reference/22-backpressure-and-sizing.md#35-high-rate-acquisition--recipe-c-compose--swap--push),
then [reference 05 §User range](../reference/05-protocol-tlvs.md#user-range-0x80--0xff), then
[v1 §3](../spec/v1.md#3-wire-format).
