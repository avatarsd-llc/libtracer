# The zero-heap profile

## In one paragraph

The zero-heap profile is the promise that an MCU build of the library reaches no heap: every
allocation the core makes goes through the one injected block source, and the default behind it
is the static arena. The promise is checked at link time, not trusted. `tools/check_no_heap.py`
reads the MCU `libtracer.a` and lists every function that reaches `malloc`, `free`, `operator
new` or `delete`, the ESP-IDF and FreeRTOS heaps, or a call that allocates out of sight, such as
starting a thread. On the Cortex-M0 build the list is empty. On the ESP32-C6 component it is a
ratchet: the functions that still reach a heap are pinned, and the pins may only shrink.

## The picture

```{mermaid}
:caption: "Legend: purple = application, grey dashed = not the library."
flowchart LR
    accTitle: How the no-heap check reads an archive
    accDescr: The MCU build produces libtracer.a. The check reads its symbols and relocations with objdump and follows calls within the archive to a fixpoint, so a function that only calls an allocating helper counts too. Each function that reaches a heap entry point is compared with the per-target baseline. A function the baseline does not pin, or a pinned one that grew, fails CI.
    B["MCU build"] -->|"libtracer.a"| O[objdump -r -t]:::notlib
    O -->|"symbols, relocations"| F["call closure, to a fixpoint"]
    F -->|"sites per function"| K{"pinned?"}
    K -->|"new or grown"| X["CI fails"]
    K -->|"pinned, same or smaller"| P["CI passes"]
    P -->|"repin lowers"| A([maintainer]):::app
```

## When you need it

When a node must run without a heap, or prove it does: a bare-metal target, an RTOS configured
without dynamic allocation, a certification argument. And when CI fails the no-heap step on a
change of yours.

## How it works

**What counts as the heap.** The C allocator and newlib's reentrant forms, every variant of
`operator new` and `operator delete`, `heap_caps_*` and `pvPortMalloc`, and calls that
allocate without naming any of them: `std::thread`, `pthread_create`, the FreeRTOS creators that
are not `...Static`, and `std::pmr::new_delete_resource`. A class with a virtual destructor
counts, because every object that emits its vtable references `operator delete`; that is why the
block source's destructor is not virtual.

**The closure.** A function reaches the heap when it names an entry point, or calls a function
in the archive that does, iterated to a fixpoint. A new function that only calls an allocating
helper is a site of its own.

**The ratchet.** `tools/no_heap_baseline.json` pins, per target, each function that reaches a
heap and through what. A function it does not list fails, and so does a pinned one that gained
a path or a count. A pin that is gone also fails until `--repin` lowers it, so the baseline never
claims more than the truth; `--repin` never adds one.

**The two targets.** The Cortex-M0 footprint node, built with `arm-none-eabi-g++`, pins no site.
The ESP32-C6 `full_node` component pins 13 objects: the TCP, UDP, WebSocket and TWAI links and
transports, the loopback, the transport vertex, and two of the graph's own objects. Those pins
may only shrink.

**Leaving the profile.** `kSlabPool = false` with `kArenaBytes = 0`, or
`CONFIG_LIBTRACER_DEFAULT_ROOT_HEAP` on ESP-IDF, makes the platform heap the default root. That
saves the arena's static RAM, and the check no longer holds for that image. Choose it only when
every graph, router, link and transport vertex is given its own source.

## Cost

### Wire bytes

None. The profile changes where bytes come from, never what is sent.

### Allocations

None from a heap on the Cortex-M0 build. A default that was not redirected draws from the
[arena](arena.md); an injected source draws from wherever the application placed it.

### Latency

The check runs at build time and adds nothing at run time.

### Flash and RAM per target

| Target | Check | RAM the profile reserves |
| --- | --- | --- |
| NARROW | Cortex-M0: zero sites, checked in CI | the arena, 2,048 B on the footprint node |
| MID | ESP32-C6: a ratchet of 13 pinned objects | the arena; about 5.2 KB saved by the heap root at a 4 KiB arena |
| WIDE | not run: a host links a heap by design | none |

## What the library does not do

- It does not see allocation inside code outside the archive that is not on its list, such as
  newlib stdio; the closure stops at the archive's edge.
- It does not check a heap-rooted image.
- It does not let `--repin` add a pin. A new site is fixed or argued in review.

## Pitfalls

- **Symptom:** the no-heap step fails on a change that calls no `new`. **Cause:** it calls a
  helper that allocates, or adds a class with a virtual destructor. **Fix:** draw through a block
  source, and keep destructors non-virtual where no one deletes through a base.
- **Symptom:** the step fails after a site was removed. **Cause:** the pin is now stale. **Fix:**
  run the check with `--repin` and commit the lowered baseline.
- **Symptom:** a heap-rooted node's memory grows without bound. **Cause:** a default was left
  in place, and it now draws from the heap. **Fix:** inject a source into it, or keep the arena.

## Where the rules live

**Normative source:** [v1 §1](../../spec/v1.md#1-scope) leaves memory to the implementation,
and [v1 §3.1](../../spec/v1.md#31-static-path-handle-conformance) asks only that addressing
a vertex on the hot path not allocate. The zero-heap profile itself is not normative.

**Exercised by:** `test_check_no_heap`, `graph_oom_softfail_test`, `transport_alloc_softfail_test`.

## History

:::{dropdown} How the profile got here
- [ADR-0083](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0083-one-allocation-seam.md)
  (2026-10-03), Decision 1, put every core allocation through one injected seam and added the
  link check on the MCU archive.
:::

Next concept: [Batching](../batching.md). Go deeper:
[failable allocation and backpressure](../../design/allocation-and-backpressure.md#the-rule),
then [reference 09 §One injection at the graph](../../reference/09-memory-substrate.md#one-injection-at-the-graph),
then the [backends module](../../modules/backends.md).
