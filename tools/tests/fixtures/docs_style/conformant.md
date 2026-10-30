# Batching

## In one paragraph

The application composes many samples into one BATCH record and pushes it as one
write. The graph never waits, paces or times; count, window and clock are the
application's.

## The picture

```{mermaid}
accTitle: One batch, one write
accDescr: The application composes a BATCH record and writes it to a vertex.
flowchart LR
  app([application]) -- compose --> rec[BATCH record]
  rec -- write --> v(vertex)
```

## How it works

The record carries a TIME base, optional offsets and the samples.

```text
A line inside a code block is never linted, however long it runs and whatever it says: MUST route handle.
```

## Cost

### Latency

One write per batch; not benched.

| target | wire bytes | allocations | note that a table row is never wrapped, so it is never measured |
| --- | --- | --- | --- |
| NARROW | 4 + n | 0 | |

## Where the rules live

**Normative source:** [v1 §3](../spec/v1.md#3-wire-format) and
[the BATCH registry entry](../reference/05-protocol-tlvs.md#batch).

**Exercised by:** the vector `batch-time-base`, the test `batch_compose_test`.

## History

Added by the batching amendment.
