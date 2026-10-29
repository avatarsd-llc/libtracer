# One batch, four spellings (L2/L3 + L4 graph)

The application composes a batch: it chooses the samples, their base time and offsets from
its own clock, and when the batch is full. The graph has no clock, no window and no counter,
and it never learns that a value is a batch. This example spells one batch four ways and
pushes it both ways a graph can.

## What to notice

- **Two choices on one record.** The samples are either framed, one `VALUE` child each, or
  packed into one `VALUE` block of `n × width` bytes when the stream's descriptor declares a
  `width`. The carriage is either standalone, headed by the BATCH type `0x80`, or folded,
  headed `VALUE` (`0x01`) for the seat a `propagate` FOLD branch-write node admits. The body
  bytes are the same in both carriages: only the first byte changes.
- **Composition references the samples.** `compose_batch` allocates one small head segment
  and ropes the application's own sample buffers on behind it, so the framed record is the
  head link plus one link per sample and no sample byte is copied.
- **The bytes are checked.** The framed records and the folded branch-write frame are compared
  byte for byte with the conformance vectors `stream/batch-composed-standalone`,
  `stream/batch-composed-folded` and `stream/fold-carries-composed-batch`. The packed records
  are compared with bytes written out by hand from the registry entry.
- **Pushing is the ordinary write path.** Compose, swap the value in with `assign`, call
  `propagate`. There is no batch-specific operation.

The rules for the record are in [the BATCH registry entry](../reference/05-protocol-tlvs.md)
and [reference 22](../reference/22-backpressure-and-sizing.md) on sizing.

## Source

```{literalinclude} /core/examples/graph_batch.cpp
:language: cpp
:linenos:
```

See also: [graph module](../modules/graph.md) ·
[user data packing](../reference/06-user-data-packing.md).
