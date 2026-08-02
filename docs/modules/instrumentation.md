# instrumentation — the reachability counters

```{admonition} In one paragraph
:class: tip
One optional, compile-time-gated instrument ships in the public headers: three
counters that record which branch a vertex store actually took — the pinned
subview, the one-copy path, or a pin the reader had to refuse. It exists because
a measurement of "is pinning a win?" is worthless unless the arm that intends to
pin *reached the pin branch*. Without its build definition the counters do not
exist and the hooks compile to nothing, so the binaries the library ships are
unchanged.
```

## What it does

Pinning needs an owning, view-delivered frame **and** a trailer-less option byte.
A benchmark that satisfies neither reports a clean "no regression" — against code
that never ran. The instrument closes that hole: the decision site ticks a
counter per branch, and every published measurement carries the counts, so a
reader can see that the arm under test was reachable at all before reading its
number.

Three counters, and the third is the interesting one:

| Counter | Meaning |
|---|---|
| pin hits | the store took the pinned-subview branch |
| copy hits | the store took the one-copy branch |
| pin refused | the ratio predicate said *pin*, but the reader could not — a borrowed, span-delivered frame with no owning segment to subview |

"Refused" is separated from "copy" because the two mean opposite things: a copy
there is the transport being definitionally unable to pin, not the predicate
declining.

The counters are **thread-local plain integers, deliberately not atomics**. As
atomics they were a fifth of the effect the benchmark was trying to resolve — one
relaxed read-modify-write on a shared cache line, on the hot store path, made the
same-binary control arm measurably slower than untouched code. The instrument was
measuring itself. The price of the fix is that a counter must be read on the
thread that ticked it.

## Pitfalls

- **It is off unless a build asks for it.** The hooks are macros that expand to
  nothing by default; do not write code whose behaviour depends on them.
- **Read on the ticking thread.** A different thread reads its own zeros, not a
  total.
- **Reset between interleaved arms.** Counters accumulate for the life of the
  thread.

## API reference

```{doxygendefine} LIBTRACER_TICK_PIN
:project: libtracer
```

```{doxygendefine} LIBTRACER_TICK_COPY
:project: libtracer
```

```{doxygendefine} LIBTRACER_TICK_PIN_REFUSED
:project: libtracer
```

See: [graph](graph.md) (the store path being measured),
[config](config.md) (the ratio knob the predicate reads),
[performance & conformance](../performance.md).
