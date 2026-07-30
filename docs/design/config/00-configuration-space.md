# The configuration space

> **Status:** design (descriptive record of shipped behaviour). **Not normative** — the
> [spec](../../spec/v1.md) constrains bytes on the wire, not build systems, and a second
> implementer needs none of this. Every number is a measurement with its target and flags
> named. Standard-level companions:
> [`../../reference/10-module-catalog.md`](../../reference/10-module-catalog.md) for module
> responsibilities, [`../../reference/12-deployment-profiles.md`](../../reference/12-deployment-profiles.md)
> for which combinations serve which deployments.

This document answers one question the reference suite deliberately cannot: **which knobs
exist, what does each cost on my target, and which constants must I leave alone.**

## Framing

A libtracer node is not one artifact with runtime switches. It is **a chosen set of modules,
sized for one target**, and both choices are made at build time. That is a deliberate
consequence of serving two targets that share no sensible middle: a single-core
microcontroller with tens of kilobytes of RAM, and a many-core host. A runtime flag would
force both to carry the other's cost.

The configuration space therefore has exactly three kinds of axis, and no fourth:

| axis kind | what it selects | mechanism |
| --- | --- | --- |
| **module set** | which translation units compile at all | CMake `option()` / Kconfig — a dropped module leaves neither code nor a symbol |
| **buffer sizes** | how big the fixed tables are | `inline constexpr` in one generated header |
| **policy types** | which implementation of a named seam is bound | `using` alias in the same header |

All three are **plain C++ or build-system state — never a preprocessor feature macro**
(ADR-0068). A knob is a constant or an alias, so a wrong value is a compile error in the
build that set it rather than a silent behavioural fork between translation units.

The sizes and policies are members of **one named type**, `default_config_t`, bound once by
`using config_t = …` (ADR-0070). An application declares its own by inheriting and overriding
what differs:

```cpp
struct my_node_config_t : tr::graph::default_config_t {
    static constexpr std::size_t kCacheLineBytes = 0;   // single-core: no false sharing
};
using config_t = my_node_config_t;
```

Inheriting means a knob added later does not break the preset — it inherits the new default
rather than failing to compile.

It is **bound once, not threaded as a template parameter**, and ADR-0070 records why with
measurements: a parameter produces byte-identical machine code, so it buys no latency; its one
unique capability — two configurations in one binary — would fork the process-global stripe and
hazard tables and so cost the RAM the configuration exists to save; and an app-declared traits
type cannot reach the library's out-of-line translation units anyway.

## The delivery mechanism

One header, `<libtracer/config.hpp>`, carries every size and policy. Two renderings of a
single template (`core/include/libtracer/config.hpp.in`) exist:

- **generated** — CMake (or the ESP-IDF component) renders it into the build tree with the
  target's values and puts that directory *first* on the public include path, so it shadows
  the checked-in copy for every TU of that build. One file per build is what makes a size
  agreement impossible to break: a bare `-D` set on some TUs and not others would be an ODR
  violation the linker is not obliged to notice.
- **checked in** — the default rendering, held byte-identical to the template by a
  configure-time drift gate. A consumer with no build-system participation at all (a raw
  `-Icore/include` compile, a vendored source drop, the Cortex-M0 footprint gate) gets stock
  settings and builds.

The consequence worth stating plainly: **the values are chosen by whoever builds libtracer's
sources.** For every real integration path that is the application's own build — the ESP-IDF
component, PlatformIO, a `FetchContent`/`add_subdirectory` CMake consumer — so an application
sets its own values. A consumer linking a *prebuilt* libtracer archive gets the values that
archive was built with; changing them means rebuilding it.

## The module set

Selection is by **which sources compile**, not by conditional compilation inside them. A
disabled transport contributes no TU and no registration call, so nothing in the image
references it. Every option defaults ON — modularity is opt-*out*, so the default build is a
full node.

| option | module | what dropping it removes |
| --- | --- | --- |
| `LIBTRACER_NET_PLANE` | FWD routing plane (`op_resolve`, `route_handle`, `fwd_router`, `transport_vertex`) | inter-node forwarding; a pure in-process graph still works |
| `LIBTRACER_TRANSPORT_TCP` | `tcp_transport_t` | — |
| `LIBTRACER_TRANSPORT_UDP` | `udp_transport_t` | — |
| `LIBTRACER_TRANSPORT_WS` | `transport_ws_*` | — |
| `LIBTRACER_TRANSPORT_CAN` | `transport_can` + its platform link | — |
| `LIBTRACER_WITH_QUIC` | `libtracer_quic` (needs msquic) | off by default |
| `LIBTRACER_WITH_CUDA` | `mem_cuda` GPU backend (needs the CUDA toolkit) | off by default |

The always-compiled core is the L2/L3 wire codec, the L0/L1 substrate, `path`, and the L4
graph runtime. See [10-module-catalog.md](../../reference/10-module-catalog.md) for what each module is
responsible for and [12-deployment-profiles.md](../../reference/12-deployment-profiles.md) for the profiles
these combinations serve.

Two of the three socket transports sharing one dependency (`posix_endpoint`) is handled by
the build, not by the modules: it compiles when *any* of them is on.

## The sized and bound axes

| knob | kind | default | set by |
| --- | --- | --- | --- |
| `kVertexLockStripes` | count | 16 | `-DLIBTRACER_VERTEX_LOCK_STRIPES`; ESP-IDF menuconfig |
| `kCacheLineBytes` | padding width | 64 | `-DLIBTRACER_CACHE_LINE_BYTES`; ESP-IDF derives it |
| `kHazardReaderSlots` | count | 64 | `-DLIBTRACER_HAZARD_READER_SLOTS` |
| `kMaxVertexBytes64` / `kMaxVertexBytes32` | RAM ceiling | 120 / 80 | the preset — deliberately not a CMake variable |
| `acl_policy_t` | policy type | `allow_only_policy_t` | `-DLIBTRACER_ACL_FULL=ON` |
| `lkv_slot_t` | policy type | `sp_atomic_slot_t` | `-DLIBTRACER_LKV_SLOT=<type>` |

Each is documented at its declaration with what it costs and when to move it; that header is
the reference, not this table. What matters here is the shape: **five knobs, all named, all
finite.** Two are counts, one is a width, two are type bindings.

### Where the bytes are

The lock-stripe table is the only global mutable buffer libtracer links into a node. Its cost
divides into a part reserved at link time and a part that really is lazy — a distinction the
header used to get wrong:

- **Static, not lazy.** `kVertexLockStripes` stripe structs plus the same number of condvar
  handles, in `.bss`, present whether or not a graph is ever constructed.
- **Lazy.** The platform primitive *behind* each handle. On FreeRTOS a stripe's mutex costs
  roughly 90 B of heap on that stripe's first lock, so a stripe no vertex has hashed onto
  costs its struct and no heap.

Most of the struct is false-sharing padding, which is what `kCacheLineBytes` governs.
Measured on rv32 (`-Os -fno-exceptions -fno-rtti`, GCC 15.2, compiling the real
`core/src/graph.cpp`), 16 stripes:

| `kCacheLineBytes` | stripe table `.bss` | TU `.bss` + `.sbss` |
| ---: | ---: | ---: |
| 64 | 1,024 B | 1,200 B |
| 0 | 128 B | 304 B |

**896 B of a single-core node's static RAM, spent against a hazard it does not have** — there
is no second core for two stripes on one line to contend over. The same knob governs the
hazard domain's cells and retire lists, where at 64 slots it is worth a further 6.5 KB.

This is an optimization axis and never a correctness one: 0 on a multi-core target costs
control-plane throughput under concurrent verb traffic and changes nothing observable. The
ESP-IDF component therefore *derives* it from `CONFIG_FREERTOS_UNICORE` rather than exposing
it — a unicore build has no second core by construction, so the right value is not a question
an integrator should be asked.

### Per-target nuances

Four differences that surprise people, each a property of the target rather than a choice:

- **The stripe table is a different object on a microcontroller.** libstdc++ makes
  `std::mutex`'s constructor `constexpr` only where its gthreads port supports static
  initialization. ESP-IDF's does not, so the host gets an `inline constinit` table indexed
  with no init-guard check, while the MCU gets a guarded function-local static — one
  predicted branch per control-plane verb. Prose describing "the `constinit` stripe table" is
  describing the host only.
- **The hazard domain costs nothing until it is bound.** Its storage lives inside an `inline`
  function whose static is never named under the default slot, so the default binding emits
  no registry at all. Binding `hazard_slot_t` also pulls in roughly 2 KB of libstdc++
  `atomic::wait` back-end `.bss` beyond the registry itself.
- **`sizeof(vertex_t)` is gated in the header**, not in a test — so every build on every target
  checks its own binding, for free. The ceilings are `config_t` members. This matters more than
  it sounds: while the gate lived in a test it covered exactly one configuration and **never the
  32-bit arm at all**, because no CI leg cross-compiled that test — and rv32 sits *exactly* on
  its 80 B ceiling with zero headroom. The stripe's footprint is gated too: a member that pushes
  a padded stripe one byte past the line would silently double the table.
- **A single-core target's constraint is RAM; a many-core host's is the read path.** The two
  policy-type knobs exist because of that split: see
  [15-concurrency-and-scaling.md](../../reference/15-concurrency-and-scaling.md) for which shapes scale and
  [`../concurrency/`](../concurrency/README.md) for this implementation's
  measured costs.

## What is deliberately not configurable

Some constants look exactly like sizing knobs and are not:

- **The address bounds** — maximum segment length, maximum path length, maximum segment
  count, maximum field depth ([03-addressing.md](../../reference/03-addressing.md)). These are normative and
  incorporated by the spec: making them per-target would let one node accept a path another
  must reject, which is an interoperability failure dressed as a RAM saving. A node that
  wants a smaller bound is asking for a *profile*, and that is a spec question.
- **Anything derived from an injected resource.** Capacities that depend on how much memory
  the application handed the node come from that resource, not from a constant — bounds are
  injected or per-target configuration, never a magic number (RFC-0006). Field depth is the
  worked example: resource-keyed rather than configurable, precisely so it cannot diverge
  between peers.

## Adding an axis

A knob is added to `config.hpp.in` — the template is the source of truth — and its default
rendering is regenerated rather than hand-edited; the drift gate fails the configure step if
the two disagree. Every render site must then set it. There are two (`core/CMakeLists.txt`
and the ESP-IDF component), and **the drift gate covers only the first**: a template variable
the component does not set renders as an empty substitution, which is a compile error in the
integrator's tree rather than a configure error in this one. Set it in both.
