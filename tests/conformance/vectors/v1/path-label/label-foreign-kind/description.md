# path-label/label-foreign-kind

An escape record at an **unassigned** kind, sitting exactly where a label could have sat:

```
06 00 0C 00                       PATH, opt = 0x00, body 12 bytes
   06 73 65 6E 73 6F 72              record: len 6, "sensor"
   00 17 02 AA BB                    escape, kind 0x17, len 2 -- somebody else's element
```

This is the second of the ruled element's two structural clauses — **an element is read by its
`kind`, never by its position**
([RFC-0027](../../../../../docs/spec/rfcs/0027-label-switched-path-compression.md) §5.1, applying
RFC-0024 amendment 2's ruling to path elements: a child identified by position *"breaks the moment
any future RFC adds a second trailing child"*). `0x17` is in RFC-0018's unassigned escape-kind
range; only `0x16` is a path label.

## What a core must do

| role | required behaviour |
| --- | --- |
| a forwarder that does not implement `0x17` | step over the record by `3 + len` and keep walking; relay the frame. It MUST NOT drop a frame it is only relaying, and MUST NOT read `AA BB` as a label of any width |
| a hop that implements `0x16` | the same. This record is not its element; "the second escape in the body" is not a label |
| any key context | refuse — **any** escape makes a `PATH` inadmissible as a `path_lookup_key` (amendment 5), whatever its kind |

Self-delimiting framing is what makes the first row possible at all, and it is the property both
RFCs selected for: *the node least likely to implement a kind is exactly the node that must still
step over it* (RFC-0018 §8). That is the NARROW argument for the ruled 7-byte spelling over the
rejected 5-byte tag welded into the name-segment grammar — a non-minting MCU skips by length using
the walker it already ships, with no new code and no new reject path, and pays 3 bytes per element
to not have to know the mechanism exists.

## Why it is a separate vector from `label-wrong-length`

Kind and length are **two** clauses. A core that checks the length but not the kind would read
`AA BB` — or worse, a future kind's 4-byte payload — as a path label and dereference it against
its own table, which is a mis-delivery rather than an error. A core that checks the kind but not
the length is `path-label/label-wrong-length`'s failure. Merging them into one vector lets either
core pass (RFC-0024 §9.4's lesson), so they stay apart.

## Why this is an `input.bin` case and not a `reject.bin` one

A packed `PATH` body is `opt.PL = 0`, opaque to the codec: there is nothing here for a codec to
refuse, and every core must round-trip these bytes. Carriage of an unimplemented kind is the
*required* behaviour, not a tolerated one. See `path/path-escape-in-key-context`.
