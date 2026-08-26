# The configuration space

> **Scope:** the build configuration of the C++23 reference implementation under
> [`core/`](https://github.com/avatarsd-llc/libtracer/tree/main/core/) — which knobs exist, what each costs on a given target, and
> which constants are not knobs. **Not the standard:** the [spec](../../spec/v1.md) constrains
> bytes on the wire, not build systems, and a second implementer needs none of this. Every
> number here is a measurement with its target and flags named. Standard-level companions:
> [`../../reference/10-module-catalog.md`](../../reference/10-module-catalog.md) for module
> responsibilities, [`../../reference/12-deployment-profiles.md`](../../reference/12-deployment-profiles.md)
> for which combinations serve which deployments.

## Framing

A libtracer node is not one artifact with runtime switches. It is **a chosen set of modules,
sized for one target**, and both choices are made at build time. That is a deliberate
consequence of serving two targets that share no sensible middle: a single-core
microcontroller with tens of kilobytes of RAM, and a many-core host. A runtime flag would
force both to carry the other's cost.

The configuration space therefore has exactly three kinds of axis, and no fourth:

| axis kind | what it selects | mechanism |
| --- | --- | --- |
| **module set** | which translation units compile at all | CMake `option()` / Kconfig — a dropped module leaves neither code nor a symbol. One module, `kBusLinks`, has no TU of its own and is stated in the config header instead — see *The one module that is not a TU list*, below; one, `kSelfHealLinks`, needs both a switch and a member — see *The module that is BOTH*, below |
| **buffer sizes** | how big the fixed tables are | `inline constexpr` in one hand-written header |
| **policy types** | which implementation of a named seam is bound | `using` alias in the same header |

All three are **plain C++ or build-system state — never a preprocessor feature macro**
([ADR-0068 — build configuration is plain C++](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0068-build-configuration-is-plain-cpp-config-header.md)).
A knob is a constant or an alias, so a wrong value is a compile error in the build that set it
rather than a silent behavioural fork between translation units.

The sizes and policies are members of **one named type**, `default_config_t`
(`core/include/libtracer/config.hpp:84`), bound once by `using config_t = default_config_t;`
(`:601`). An application declares its own by inheriting and overriding what differs (`:68-78`):

```cpp
struct my_node_config_t : tr::graph::default_config_t {
    static constexpr std::size_t kCacheLineBytes = 0;   // single-core: no false sharing
};
using config_t = my_node_config_t;
```

Inheriting means a knob added later does not break the preset — it inherits the new default
rather than failing to compile. The rest of the library names the derived spellings re-exported
below the traits type (`:646-669`), each of which is exactly its traits member, so introducing
`config_t` moved no call site.

It is **bound once, not threaded as a template parameter**, and
[ADR-0070 — configuration is a named traits type](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0070-configuration-is-a-named-traits-type.md)
records why with measurements: a parameter produces byte-identical machine code, so it buys no
latency; its one unique capability — two configurations in one binary — would fork the
process-global stripe and hazard tables and so cost the RAM the configuration exists to save;
and an app-declared traits type cannot reach the library's out-of-line translation units anyway.

## The delivery mechanism

One header, `<libtracer/config.hpp>`, carries every size and policy. It is **ordinary
hand-written C++** and the only place a default is spelled — a consumer with no build-system
participation at all (a raw `-Icore/include` compile, a vendored source drop, the Cortex-M0
footprint gate) gets stock settings and builds.

A target that wants non-default values supplies an **override fragment**,
`libtracer/config_override.hpp`, earlier on the include path. `config.hpp` picks it up
automatically and uses the `config_t` it binds. One file per build is what makes a size
agreement impossible to break: a bare `-D` set on some TUs and not others would be an ODR
violation the linker is not obliged to notice.

The fragment **inherits** `default_config_t` and states only what differs:

```cpp
// libtracer/config_override.hpp
namespace tr::graph {
struct esp_config_t : default_config_t {
    static constexpr std::size_t kCacheLineBytes = 0;
};
using config_t = esp_config_t;
}  // namespace tr::graph
```

That inheritance is why there is no drift gate any more: a knob added to `config.hpp` reaches
every override with its new default, because no override restates the knobs it does not change.
Earlier revisions generated the whole header from a CMake template and byte-compared a default
render against the checked-in copy — a gate that existed only because the defaults were spelled
twice ([ADR-0068 §Erratum 1](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0068-build-configuration-is-plain-cpp-config-header.md)).

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

| CMake option | module | ESP-IDF counterpart | what dropping it removes |
| --- | --- | --- | --- |
| `LIBTRACER_NET_PLANE` | FWD routing plane (`op_resolve`, `route_handle`, `fwd_router`, `transport_vertex`) | none — the component has no counterpart | inter-node forwarding; a pure in-process graph still works |
| `LIBTRACER_TRANSPORT_TCP` | `tcp_transport_t` | `CONFIG_LIBTRACER_TRANSPORT_TCP` | — |
| `LIBTRACER_TRANSPORT_UDP` | `udp_transport_t` | `CONFIG_LIBTRACER_TRANSPORT_UDP` | — |
| `LIBTRACER_TRANSPORT_WS` | `transport_ws_*` | `CONFIG_LIBTRACER_TRANSPORT_WS` — selects a *different implementation* on chips (see below) | — |
| `LIBTRACER_TRANSPORT_CAN` | `transport_can` + its platform link | `CONFIG_LIBTRACER_TRANSPORT_CAN` | — |
| `LIBTRACER_WITH_QUIC` | `libtracer_quic` (needs msquic) | none | off by default |

`LIBTRACER_WITH_CUDA` is **gone** (#1381): the GPU backend is a tier module with its own CMake
project, `backends/cuda/`, so "do I want it" is answered by whether you configure that directory
and link `libtracer_cuda` — not by a core option, and not by a compile definition that used to
reach every consumer TU of a CUDA-enabled build. It plugs into core through
`tr::mem::register_device_backend`, the memory layer's mirror of `register_transport_type`.

The always-compiled core is the L2/L3 wire codec, the L0/L1 substrate, `path`, and the L4
graph runtime. See [10-module-catalog.md](../../reference/10-module-catalog.md) for what each module is
responsible for and [12-deployment-profiles.md](../../reference/12-deployment-profiles.md) for the profiles
these combinations serve.

The socket transports sharing one dependency (`posix_endpoint`) is handled by the build, not
by the modules: it compiles when *any* of them is on.

**One option, two implementations.** `LIBTRACER_TRANSPORT_WS` names the WebSocket *module*,
not a fixed pair of types, and on ESP-IDF the target decides which pair it builds. A chip
target gets the IDF-native links (`httpd_ws_link_t`, `esp_ws_client_link_t`) and does **not**
compile `transport_ws_*` at all, so no `ws` entry is registered in the built-in catalog there
and a `kind=ws` SPEC with no staged link answers `SCHEMA_NOT_FOUND`; the `linux` target and
every non-IDF build get the portable pair. This is still selection-by-which-TU-compiles — the
same rule as `socketcan_link.cpp` versus its stub — and it is a correctness split, not a size
one: the portable pair's gather egress asks `sendmsg` for `MSG_NOSIGNAL`, which lwIP rejects
with `EOPNOTSUPP`, so on silicon it silently drops every data frame. See
[the ESP-IDF integration README](https://github.com/avatarsd-llc/libtracer/blob/main/integrations/esp-idf/README.md).

### The one module that is not a TU list — `kBusLinks`

Every module above is selected by which sources compile. The **bus** module — ADR-0044's
peer-named addressing tier: a mount's bus shape, per-peer resolution, in-band peer enumeration,
the peer-named receiver and the two peer-lifecycle notifiers — cannot be, and is the deliberate
exception ([#375](https://github.com/avatarsd-llc/libtracer/issues/375)). Whether a `tcp` or `ws`
*listener* is peer-named is a **wiring-time** choice made inside a TU that a bus-less target still
compiles for its point-to-point half, so there is no file to leave out. It is therefore stated as
a configuration member, `kBusLinks`, and reached through one gate, `tr::net::bus_of`, so the
closure is a compile-time fold at every consumer rather than a run-time branch. Measured on rv32
(`-Os -fno-exceptions -fno-rtti`, GCC 15.2, per-TU `.text`), binding it `false` removes 1,400 B
from `fwd_router.cpp` and 678 B from `transport_vertex.cpp` — 2,078 B of flash, 0 B of `.bss`, because
the tier is code and per-instance state rather than a static table.

Closing it is a **refusal, never a silent downgrade**. A `peer_named=true` listener SPEC is
answered `TYPE_MISMATCH` (permanent — no retry grows a build a bus facet), a directly-constructed
peer-named listener reports `ok() == false`, and compiling `LIBTRACER_TRANSPORT_CAN` — a bus by
construction — is a `static_assert`. Serving such a configuration as flat would be worse than
either: the listener's own per-frame tier select would keep delivering peer-named into a sink the
router never installed.

### The module that is BOTH — `kSelfHealLinks`

The RFC-0014 §4 S5 link-liveness engine is the third shape, and the one this space did not have
before [#1470](https://github.com/avatarsd-llc/libtracer/issues/1470): it *does* have a TU of its
own (`core/src/self_heal_link.cpp`), so it is dropped by a build switch like every other module —
and it *also* needs a configuration member, because the one place it is minted sits inside
`transport_vertex.cpp`, a TU every net-plane node compiles. Without the member, dropping the TU
is a link error rather than a trim.

So it carries both, and they are held in step from opposite ends. `LIBTRACER_SELF_HEAL_LINKS`
(CMake) / `CONFIG_LIBTRACER_SELF_HEAL_LINKS` (Kconfig) decides whether the TU compiles;
`kSelfHealLinks` decides whether anything reaches it, folding the mint site and the four calls
into the engine away at compile time so the linker never pulls the archive member back in. The
ESP-IDF component drives both from the one Kconfig symbol, so they cannot disagree there; a core
build pairs them by hand, and `self_heal_link.cpp` opens with a `static_assert(kSelfHealLinks, …)`
that catches the mismatch in the direction a link error would not explain. The saving is the
reporter's C6 measurement: **4,336 B** of reachable `self_heal_link_t` symbols, **0 B** of
`.dram0.bss`.

Closing it is a **refusal, never a silent downgrade**, on the same principle as the bus tier:
`register_transport_type` does not catalogue a `self_heal_dial` kind on such a build (so a `SPEC`
naming it answers `SCHEMA_NOT_FOUND`, and a debug build asserts at the registration), rather than
registering it with the trait cleared — which would bring the connection up eagerly, with no
redial and no liveness publishing, and say nothing. Today the whole question is latent: **no
in-tree kind sets `self_heal_dial`**, so the engine is unreachable on every stock target and the
knob is a pure saving until [#1548](https://github.com/avatarsd-llc/libtracer/issues/1548) flips
the built-in point-to-point kinds onto it.

### The required-modules footprint ceiling

The minimum-feature module set — `frame`, `tlv_arena`, `backend_set`, `mem_pool`, `mem_source`,
`rope`, `path` — targets **≤ 16 KiB of stripped flash** on
`arm-none-eabi-g++ -std=c++23 -Os -fno-exceptions -fno-rtti -mcpu=cortex-m0` with
`--specs=nano.specs` (`tools/cortexm0_footprint.py:84` for the module list, `tools/cortexm0_footprint.py:151-167` for the
compile flags, `tools/cortexm0_footprint.py:172` for the link spec). One committed sentinel
measures against it: `tools/cortexm0_footprint.py`, driven by `.github/workflows/footprint-cortexm0.yml`
over the `core/tests/footprint/sentinel_node.cpp` fixture. A second tool, `tools/esp_size_gate.py`,
*reports* the component's flash and static-RAM contribution to the esp32c3/c6 **full-node** image
(all socket transports plus CAN — far more than the required modules) without setting a ceiling on
either: that image is one deployment's composition, not a budget the library owes, and a ceiling
there would constrain how thin a client this library can serve. Its numbers are published to the
job step summary and a `footprint-<target>` artifact, and reviewed run to run.

The Cortex-M0 sentinel runs in warn mode because the measured node is **20,937 B, about 4.5 KiB
over the 16 KiB budget** (re-measured 2026-07-27; `.github/workflows/footprint-cortexm0.yml:13-20`).
**The overage is not attributed to a module.** An attribution of roughly 2.7 KiB to `std::pmr`
soft-float reaching the image through the arena decoder's `std::pmr::memory_resource&` seam does
not survive the removal of that seam — the decoder takes a `tr::mem::block_source_t&`
([ADR-0065 — failable allocation gets its own seam](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0065-failable-allocation-gets-its-own-seam-block-source.md))
— and the residue has not been re-measured against that seam. The gate is a referee for
the compile-time doctrine
([ADR-0047 — build-time closed module sets](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0047-build-time-closed-module-sets-compile-time-seams.md) §5):
templating techniques are admissible exactly as far as it stays green, because it catches
type-erasure bloat and template-instantiation bloat alike.

## The sized and bound axes

Every knob is overridden the same way — as a member of the fragment's traits type. The column
below names the member; a knob the fragment does not state keeps the default beside it.

The **ESP-IDF column is that component's fragment**: it derives six knobs from its Kconfig
values and chip facts and writes them into a generated `libtracer/config_override.hpp`, listed
ahead of `core/include` on the component's include path. Everything the column calls *inherited*
is a knob the fragment does not state at all (#1244).

| knob | kind | default | ESP-IDF |
| --- | --- | --- | --- |
| `kVertexLockStripes` (`config.hpp:98`) | count | 16 | menuconfig `CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES` (`integrations/esp-idf/libtracer/CMakeLists.txt:296`) |
| `kCacheLineBytes` (`:122`) | padding width | 64 | derived from `CONFIG_FREERTOS_UNICORE`, not exposed (`integrations/esp-idf/libtracer/CMakeLists.txt:314`) |
| `kHazardReaderSlots` (`:149`) | count | 64 | inherited — the refcount slot never builds the domain |
| `kEdgePinSlots` (`:162`) | count | 32 | set to 8 (`integrations/esp-idf/libtracer/CMakeLists.txt:309`) |
| `kMaxVertexBytes64` / `kMaxVertexBytes32` (`:198` / `:216`) | RAM ratchet | 96 / 72 | the preset — deliberately not overridable |
| `kPinPayloadRatio` (`:262`) | ratio | 0 — the `kPinNever` sentinel | the preset |
| `acl_policy_t` (`:271`) | policy type | `allow_only_policy_t` | inherited — the full policy is not selectable |
| `lkv_slot_t` (`:287`) | policy type | `sp_atomic_slot_t` | inherited — the hazard slot is not selectable |
| `kSpinWaitSafe` (`:653`) | target fact | `true` | derived from `IDF_TARGET` — `false` on every chip, `true` on `linux` (`integrations/esp-idf/libtracer/CMakeLists.txt:330`) |
| `kWeaklyOrdered` (`:446`) | target fact | `true` | inherited — every ESP chip is weakly ordered, which is the default |
| `kBusLinks` (`:610`) | module presence | `true` | inherited — the component builds the full peer-named tier |
| `kSelfHealLinks` (`:541`) | module presence | `true` | menuconfig `CONFIG_LIBTRACER_SELF_HEAL_LINKS` (`integrations/esp-idf/libtracer/CMakeLists.txt:341`) |
| `kSelfHealWorkerStackBytes` (`:568`) | size | `0` — the platform default | menuconfig `CONFIG_LIBTRACER_SELF_HEAL_WORKER_STACK` (`integrations/esp-idf/libtracer/CMakeLists.txt:347`) |

Two CMake variables survive for one transition release, `-DLIBTRACER_ACL_FULL` and
`-DLIBTRACER_LKV_SLOT`; `core/CMakeLists.txt` writes a fragment on their behalf. The five other
cache variables this table used to list were deleted with the template (#1142).

Each is documented at its declaration with what it costs and when to move it; that header is
the reference, not this table. What matters here is the shape: **thirteen knobs, all named, all
finite.** Three are counts (`kVertexLockStripes`, `kHazardReaderSlots`, `kEdgePinSlots`), one is a
padding width, one is a per-target RAM ceiling, one is a ratio, one is a thread stack size
(`kSelfHealWorkerStackBytes`), two are type bindings, two are
target facts rather than preferences, and two — `kBusLinks`, below, and `kSelfHealLinks` (the
RFC-0014 S5 link-liveness engine, #1470) — state whether a *module* is present at all. `kSpinWaitSafe` says whether a task on this target may spin
for a lock another task holds, and the guard in `mem_pool.hpp` reads it to refuse
`synchronized_pool_t<spin_sync_t>` where the answer is no (#1158). `kWeaklyOrdered` says whether
the target's memory model may reorder a later relaxed load ahead of an earlier `seq_cst` store,
and the guard in `vertex.hpp` reads it to refuse a `kDeliverySkipOrder` weaker than `seq_cst`
(#1143) — the compile-time half of the ordering question whose evidence half is the
`ubuntu-24.04-arm` CI leg (#1140). Both default to the value that is safe to inherit in silence,
which for the ordering fact is the STRICT one: asserting on a TSO host costs nothing, while a
target that wrongly claims TSO disarms the check on the one class it exists for.

Two of the thirteen carry no build-system variable at all. `kMaxVertexBytes64` / `kMaxVertexBytes32`
and `kPinPayloadRatio` are preset members: an application moves them by declaring its own traits
type, not by passing `-D`. `kPinPayloadRatio` is the pin/copy amplification ratio `K` — a
trailer-less written value is stored as a subview of the inbound frame, rather than copied out,
exactly when `payload_bytes * K >= segment_bytes`. Both branches are correct, so `K` selects which
correct branch is cheaper rather than imposing a limit: pinning holds the whole owning RX
**segment** for the value's lifetime, and `K` bounds that waste at `(K-1)×` the payload where an
absolute byte threshold bounded it not at all. `segment_bytes` is the segment's **allocated**
size, not the delivered view's length, because a datagram transport receiving into a fixed-size
segment and delivering a subview of it makes those differ by orders of magnitude. The shipped
value is the reserved sentinel `kPinNever` (0) — never pin, on every target. It is also the one
knob with a per-vertex override (`graph_t::set_pin_payload_ratio`), which exists for an owner that
knows one vertex's traffic differs from the target's default.

The ESP-IDF component exposes exactly five options — `LIBTRACER_TRANSPORT_{UDP,TCP,WS,CAN}` and
`LIBTRACER_VERTEX_LOCK_STRIPES` — so an integrator reaching for the full ACL policy, the hazard
slot, or the net-plane switch through menuconfig will not find them. Those choices are made by
editing the component's render site or by building libtracer's sources from a CMake consumer.

### Where the bytes are

The lock-stripe table is the only global mutable buffer libtracer links into a node. Its cost
divides into a part reserved at link time and a part that really is lazy. The table's cost is
commonly described as lazy in full; only the platform primitive behind each handle is:

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
is no second core for two stripes on one line to contend over.

The same knob governs the hazard domain's cells and retire lists, where at 64 slots it is worth
a further 6.5 KB. Measured on rv32 at N = 64, same instrument (`-Os`, real `core/src/graph.cpp`,
GCC 15.2):

| `kCacheLineBytes` | hazard registry `.bss` | TU `.bss` + `.sbss` |
| ---: | ---: | ---: |
| 64 | 8,384 B | 11,649 B |
| 0 | 1,828 B | 4,197 B |

The TU column is not the registry plus the stripes: binding `hazard_slot_t` also pulls in
roughly 2 KB of libstdc++ `atomic::wait` back-end (`__waiter_pool_base`) `.bss` beyond the
registry itself.

`kCacheLineBytes` is an optimization axis and never a correctness one: 0 on a multi-core target
costs control-plane throughput under concurrent verb traffic and changes nothing observable.
The ESP-IDF component therefore *derives* it from `CONFIG_FREERTOS_UNICORE` rather than exposing
it — a unicore build has no second core by construction, so the right value is not a question
an integrator should be asked.

### The LKV slot contract

`lkv_slot_t` is the one knob whose value is a **name the integrator supplies**, so it is the one
knob with a contract attached. The declaration instructs that the named type must satisfy the
policy contract in `lkv_slot.hpp` (`config.hpp:287`, and the instruction itself at `:284-285`) —
a header that is absent from `core/Doxyfile`'s `INPUT` list, so the generated API site does not
serve the page that instruction points at. The contract, stated here, is three operations over
`value_ptr_t = std::shared_ptr<const view::rope_t>`:

| operation | signature | rule |
| --- | --- | --- |
| publish | `[[nodiscard]] bool store(value_ptr_t, std::memory_order = seq_cst)` | Sequentially consistent by default: `vertex_t::store` relies on this sharing one total order with the `write_seq_` bump and the waiter count, which is what makes the waiterless publish unable to lose a wakeup. **`false` means nothing was published** and the previous value still stands — the caller soft-fails; it never reports the write as taken. A slot that reclaims lazily has to allocate to publish, which is why the return type exists at all. |
| drop | `void clear(std::memory_order)` | Cannot fail, and says so in the return type: a clear releases resources rather than acquiring any. Only `revert_to_placeholder` calls it, with `release`. |
| read | `value_ptr_t load() const` | Returns an **owning** handle. |

Owning is not negotiable. The composed branch read `graph_t::read_subtree_folded`
(`core/include/libtracer/graph.hpp:1733`) stashes one LKV per node into a vector that outlives
the map lock and spans three passes, so **N values are held simultaneously**. A reclamation
scheme that can protect only one value per reader at a time — hazard pointers, as classically
stated — therefore cannot hand back a pinned pointer; it must promote the pin to a counted
reference before releasing it, and that promotion is a read-modify-write on the one cache line
every reader shares. That promotion, not the scheme, is what the measured win is net of; the
numbers and their conditions are in
[`../concurrency/00-scaling-and-serialization.md`](../concurrency/00-scaling-and-serialization.md),
which is where they belong rather than repeated here.

The default binding, `sp_atomic_slot_t`, is **lock-free by contract and spin-locked in
practice**: `std::atomic<std::shared_ptr<T>>::is_lock_free()` returns 0 on libstdc++, so both
load and store take its internal pointer-lock bit (`core/include/libtracer/lkv_slot.hpp:101-103`).
Its reclamation is the refcount, so there is no scheme to implement and no registry to size —
which is why a raw `-I` consumer and the stock ESP-IDF component build what they would have
built without the policy seam
([ADR-0069 — the LKV slot is a compile-time policy](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0069-lkv-slot-is-a-compile-time-policy-hazard-reclamation.md)).

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
- **`sizeof(vertex_t)` is gated in the header, not in a test.** The ceilings are `config_t`
  members and the assertions sit in `vertex.hpp` beside the type they constrain
  (`core/include/libtracer/vertex.hpp:3086,3091`), so every build on every target checks its
  own binding, for free. A test-resident gate covers only the configurations CI actually
  builds: one, in practice, and never the 32-bit arm, because no CI leg cross-compiles that
  test while the ESP-IDF legs compile `vertex_t` itself on every change. That distinction has
  teeth here — both arms are **ratchets pinned to the measured size, so neither has headroom by
  construction** (`config.hpp:216`): 96 B on 64-bit, 72 B on rv32, and the next added member
  is a build failure on both. They were ceilings held above the measurement until 2026-08-10,
  which is why 16 B reclaimed on the 64-bit arm and 8 B on the 32-bit one went unnoticed — a
  ceiling answers "did you regress past a fixed point", never "did this get leaner". The stripe carries a companion
  assertion of a different kind: `alignof(vertex_stripe_t) == kStripeAlign` (`vertex_stripe.hpp:77`),
  which catches an `alignas` that asked for less than the payload's natural alignment and was
  therefore ignored — silently, by GCC, per `[dcl.align]/5`.
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
  injected or per-target configuration, never a magic number
  ([RFC-0006 — resource-bounded nesting depth](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0006-resource-bounded-nesting-depth.md)).
  Field depth is the worked example: resource-keyed rather than configurable, precisely so it
  cannot diverge between peers.

## Adding an axis

A knob is added as a `default_config_t` member in `config.hpp`, with its default as an ordinary
C++ literal. That is the whole procedure: there is one copy, so nothing has to be regenerated
and nothing can drift.

Nothing else has to be touched. Every override fragment inherits `default_config_t`, so an
existing override — the ESP-IDF component's, an application's — picks up the new knob at its
new default without an edit. Deliberately, a knob does **not** get a CMake cache variable: it is
set in the fragment, as C++. (`-DLIBTRACER_ACL_FULL` and `-DLIBTRACER_LKV_SLOT` survive for one
transition release, with `core/CMakeLists.txt` writing a fragment on their behalf.)

Put the knob in `default_config_t` even when it states an L0 fact — `kSpinWaitSafe` is a
`tr::mem` concept whose spelling is derived from `config_t`. A knob outside the one named type
cannot be reached by a fragment at all, which is exactly how that one ended up stranded in the
build system.
