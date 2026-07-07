# Reference 06 — User Data Packing into the Graph

> **Status**: draft, v1, 2026-05-03. How application-level data of any size — from a single boolean to a streaming GB/s feed — gets put into the graph and delivered with appropriate copy semantics.
> **Audience**: anyone designing the data layout for their application.
> **See also**: [02-graph-model.md](02-graph-model.md) §read-vs-write copy semantics; [03-addressing.md](03-addressing.md) §address-shift slicing; [05-protocol-tlvs.md](05-protocol-tlvs.md) §VALUE.

---

## The packing rules

The protocol does not impose a serialization layer. The user picks the packing that fits their data; the protocol just moves TLVs.

| Data shape | Recommended TLV form |
| ---- | ---- |
| Single scalar (bool, u8, u16, u32, u64, i32, f32, f64) | One `VALUE` TLV with raw little-endian bytes; or a user-range type code (`0x80..0xFF`) with implicit length |
| Fixed-shape struct (e.g., `{u32, u32, f32, f32}` IMU sample) | One `VALUE` TLV with packed-struct bytes; sender and receiver agree on the layout out-of-band |
| Self-describing record with named fields | User-range structured TLV (PL=1) with `NAME` + value-TLV children |
| Variable-length collection | User-range structured TLV (PL=1) with homogeneous children |
| Memory-mapped hardware register | Single `VALUE` TLV whose payload view points directly into MMIO space (zero-copy on read) |
| Large payload (anything where a single TLV is too big to ship) | Address-shift slicing across `ep[0..N]` with shared timestamp |
| Multiple coherent streams (camera + LIDAR) | Separate vertices, common timestamp domain; subscriber joins by timestamp |

The key insight: **the graph imposes no shape**. An endpoint is a name attached to a memory view. The protocol does not preordain payload shape, sample layout, or chunking strategy.

---

## Single boolean (or any single byte)

The minimal endpoint. A 1-byte payload, optionally with the timestamp prefix.

```cpp
// Publisher — see the graph module (../modules/graph.md) and view module (../modules/views.md).
tr::graph::graph_t g;
tr::graph::vertex_t* led =
    *g.register_vertex(tr::graph::path_t("/dashboard/led"), tr::graph::role_t::STORED_VALUE);

// Build a fresh single-byte VALUE view.
bool led_on = true;
tr::view::segment_ptr_t seg = tr::view::heap_alloc(1);
seg->bytes[0] = static_cast<std::byte>(led_on ? 1 : 0);
tr::view::view_t value = tr::view::view_t::over(std::move(seg));

g.write(led, value);
```

On the wire (no trailer, default `LL=0` u16 length):

```
01 00 01 00 01
^  ^  ^^^^^ ^
|  |  len=1  payload byte (0x01 = true)
|  opt = 0  (no flags)
type = 0x01 VALUE
```

**5 bytes total.** With CRC-16 trailer (`opt.CR=1, opt.CW=1`), 7 bytes. With CRC-32, 9 bytes. With absolute TS + CRC-32 (`opt.TS=1, opt.CR=1`), 17 bytes.

Header overhead is **4 bytes** in the default case (`LL=0`, payload ≤ 64 KiB), or 6 bytes when `LL=1`. Trailer overhead is paid per-TLV only when the corresponding `opt` flags are set, and adds 0 / 2 / 4 / 6 / 8 / 10 / 12 / 14 / 16 bytes depending on which combinations of `TS`, `CR`, `TF`, `CW` are selected. See [01-data-format.md](01-data-format.md) §frame size summary for the full table.

Per-message overhead is the same whether the payload is 1 byte or 1 MiB; the cost amortizes immediately past the smallest payloads.

### Casting trick for true zero-copy on the read side

If the publisher's source data is already a `bool` somewhere in memory (e.g., a struct field, a stack local with stable address), the TLV's view can point directly to it instead of being copied:

```cpp
struct dashboard_state {
    bool led_on;        // single byte
    std::uint8_t brightness;
    // ...
};

extern dashboard_state g_dash;

// Borrow the live byte directly — no memcpy, no ownership transfer.
// The view is a (pointer, length=1) span into the live struct.
tr::view::view_t value = tr::view::view_t::over(
    tr::view::borrow(std::span<std::byte>{
        reinterpret_cast<std::byte*>(&g_dash.led_on), sizeof g_dash.led_on}));
g.write(led, value);
```

The published TLV reads the byte directly from `g_dash.led_on` at every fanout. If `g_dash` is updated between publish and subscriber-consume, the subscriber sees the new value. (Whether this is desired or a bug is application-dependent — usually you want a snapshot, in which case the explicit-copy form above is correct.)

For a **multi-byte scalar** (u32, f64) the same pattern applies; the protocol does not care about scalar size.

---

## GPIO register (memory-mapped I/O as a vertex)

A hardware register is just memory at a fixed address. Wrap it in a view and it becomes a libtracer vertex with **zero-copy reads**.

```cpp
constexpr std::uintptr_t GPIOA_IDR_ADDR = 0x40020010;   // STM32F4 GPIOA input data register

tr::graph::graph_t g;

// Borrow the MMIO region directly — no allocation, no ownership, nothing to free.
// borrow() takes a std::span<std::byte>; for a register at a fixed address, point
// a std::byte* at that address. The view is a live window onto the register.
auto* idr = reinterpret_cast<std::byte*>(GPIOA_IDR_ADDR);
tr::view::view_t idr_view =
    tr::view::view_t::over(tr::view::borrow(std::span<std::byte>{idr, sizeof(std::uint32_t)}));

// Expose GPIOA's input data register as a vertex whose stored view points at the
// live register; every read observes the register with no memory copy.
tr::graph::vertex_t* v_idr =
    *g.register_vertex(tr::graph::path_t("/gpio/A/IDR"), tr::graph::role_t::STORED_VALUE);
g.write(v_idr, idr_view);
```

### Subscriber side

```cpp
auto r = g.read(v_idr);                         // r is std::expected<rope_t, …>
std::span<const std::byte> bytes = r->only().bytes();
std::uint32_t idr_value;
std::memcpy(&idr_value, bytes.data(), sizeof idr_value);   // first copy at the boundary
```

The TLV's payload pointer points directly at `0x40020010`. The subscriber chooses whether to copy into local memory (for a stable snapshot) or operate on the view directly.

### Write side (single-copy, register-mapped)

```cpp
// Standard fresh-VALUE helper.
tr::view::view_t value_u32(std::uint32_t x) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(4);
    for (int i = 0; i < 4; ++i)
        seg->bytes[i] = static_cast<std::byte>((x >> (8 * i)) & 0xFF);
    return tr::view::view_t::over(std::move(seg));
}

tr::graph::vertex_t* v_bsrr =
    *g.register_vertex(tr::graph::path_t("/gpio/A/BSRR"), tr::graph::role_t::HANDLER);
g.write(v_bsrr, value_u32(1u << 5));   // set pin PA5
```

The vertex registered for `/gpio/A/BSRR` (Bit Set/Reset Register at `0x40020018`) has a write-handler that copies the incoming TLV's payload bytes into the register. **One copy** — from the TLV view into the register. This is the single-copy write semantic: the TLV is a view (no copies on the way in), but landing it in the register requires one write to `*(volatile uint32_t *)0x40020018`.

### Why this matters

A single API substrate (`g.read` / `g.write`, see the [graph module](../modules/graph.md)) covers:

- Logical software-defined endpoints (sensor readings, control state).
- Hardware-defined endpoints (GPIO registers, peripheral SFRs).
- Remote endpoints (a register on another MCU, reached by its source route over CAN).

To a subscriber, all three look identical. Tooling like `tracer-top` enumerates the entire address space — software and hardware — through one walk.

---

## Structured record with named fields

For self-describing data, define a user-range structured TLV (`opt.PL=1`) with NAME + value children. Pick a type code in `0x80–0xFF` and document its layout for your project.

The reference impl models a structured node as a parent (`POINT`) TLV with `opt.PL=1` carrying `NAME` + `VALUE` children; the encoder emits it under a user-range type code (`0x80+`). See the [wire module](../modules/frame-codec.md).

```cpp
struct imu_sample {
    std::uint64_t ts_ns;
    float accel_x, accel_y, accel_z;
    float gyro_x, gyro_y, gyro_z;
};

// TLV builder helpers.
tr::wire::tlv_t name_tlv(std::string_view s) {
    tr::wire::tlv_t t;
    t.type = tr::wire::type_t::NAME;
    t.payload = {reinterpret_cast<const std::byte*>(s.data()), s.size()};
    return t;
}
tr::wire::tlv_t value_tlv(std::span<const std::byte> b) {
    tr::wire::tlv_t t;
    t.type = tr::wire::type_t::VALUE;
    t.payload = b;
    return t;
}

void publish_imu(const imu_sample& s, tr::graph::graph_t& g, tr::graph::vertex_t* imu) {
    auto bytes = [](const auto& x) {
        return std::span<const std::byte>{reinterpret_cast<const std::byte*>(&x), sizeof x};
    };

    // Parent record: POINT + opt.PL=1, self-describing NAME/VALUE children.
    tr::wire::tlv_t rec;
    rec.type = tr::wire::type_t::POINT;
    rec.opt  = tr::wire::opt_t{.pl = true};
    rec.children = {
        name_tlv("ts_ns"), value_tlv(bytes(s.ts_ns)),
        name_tlv("accel"), value_tlv({reinterpret_cast<const std::byte*>(&s.accel_x), 3 * sizeof(float)}),
        name_tlv("gyro"),  value_tlv({reinterpret_cast<const std::byte*>(&s.gyro_x), 3 * sizeof(float)}),
    };

    // Encode to wire bytes, then publish as a view over those bytes.
    std::vector<std::byte> wire = tr::wire::encode(rec);
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(wire.size());
    std::memcpy(seg->bytes.data(), wire.data(), wire.size());
    g.write(imu, tr::view::view_t::over(std::move(seg)));
}
```

A subscriber walks the children iteratively (per [01-data-format.md](01-data-format.md) §iterative parsing) and extracts fields by NAME match. This is **self-describing on the wire**: if the IMU record gains a `mag` field later, old subscribers ignore it; new subscribers read it.

For a **fixed-shape** struct where schema evolution doesn't matter and bytes are precious, pack the whole struct as one VALUE TLV instead:

```cpp
// One opaque VALUE holding the packed struct.
tr::wire::tlv_t v = value_tlv({reinterpret_cast<const std::byte*>(&s), sizeof s});
std::vector<std::byte> wire = tr::wire::encode(v);
tr::view::segment_ptr_t seg = tr::view::heap_alloc(wire.size());
std::memcpy(seg->bytes.data(), wire.data(), wire.size());
g.write(imu, tr::view::view_t::over(std::move(seg)));
```

The trade-off is wire-format-versus-self-description, identical to the choice between Cap'n Proto (fixed schema) and JSON (named fields).

---

## Streaming a high-speed ADC (1 GB/s)

The publisher slices the stream across enumerated child endpoints with shared timestamps. Each slice is independently routable; the receiver assembles or processes-as-stream per its QoS.

### Publisher

```cpp
constexpr std::size_t SLICE_SIZE = 4 * 1024;   // 4 KiB per slice, fits one MTU on most LANs

// Slice vertices registered once at init (see §static path handles in 03-addressing).
extern tr::graph::vertex_t* adc_raw[];         // adc_raw[i] == /adc/raw[i]

void on_dma_complete(std::byte* adc_buf, std::size_t buf_len,
                     tr::graph::graph_t& g) {
    std::size_t n_slices = buf_len / SLICE_SIZE;
    for (std::size_t i = 0; i < n_slices; ++i) {
        // Borrow directly into the DMA buffer — no memcpy, no ownership transfer.
        tr::view::view_t slice = tr::view::view_t::over(
            tr::view::borrow(std::span<std::byte>{adc_buf + i * SLICE_SIZE, SLICE_SIZE}));
        g.write(adc_raw[i], slice);
    }
}
```

Each `g.write` is a view-clone (a refcount bump on the DMA segment's backend) and a router dispatch. **No byte copies happen between the DMA buffer and the network's egress.** The only copy is in the transport layer when bytes leave the host (`send` system call into kernel buffer); for SHM or RDMA transports, even that copy disappears.

### Subscriber (process-as-stream)

Register the subscription by writing a SUBSCRIBER record to the parent's `:subscribers[]` field — a subtree subscription observes every indexed child (see [03-addressing.md](03-addressing.md) §subtree subscriptions and the [graph module](../modules/graph.md)):

```cpp
// Subscribe with assemble=false (default) — receive each slice as it arrives.
// The SUBSCRIBER value names the local handler path /local/dsp-pipeline.
g.write(tr::graph::path_t("/adc/raw:subscribers[]"), subscriber_value);   // subtree: every /adc/raw[i]

// In the dsp-pipeline handler, the delivered view borrows the producer's bytes.
void on_adc_slice(const tr::view::view_t& delivered) {
    auto t = tr::wire::view_as_tlv(delivered);        // zero-copy decode
    std::span<const std::byte> bytes = t->payload;    // spans borrow the buffer
    process_adc_slice(bytes);                          // FIR filter, FFT, whatever
}
```

The subscriber processes 4 KiB at a time, never holding more than one slice's worth of memory. Throughput is bounded by the DSP pipeline + transport, not by buffer allocation.

### Subscriber (assemble for batch processing)

Set the assembly QoS atomically on the subscriber's `:settings` object, then register the subscription. The `address_shift.*` / `deadline_ns` fields are the v1 QoS design (see [03-addressing.md](03-addressing.md) §subscriber assembly policies):

```cpp
// Atomic whole-object settings write (SETTINGS 0x0B): assemble=true,
// expected_count for 100 ms batches, deadline_ns=200 ms safety window.
g.write(tr::graph::path_t("/local/batch-handler:settings"), assemble_settings_value);

// Register the subtree subscription: observes every /adc/raw[i].
g.write(tr::graph::path_t("/adc/raw:subscribers[]"), subscriber_value);
```

The router buffers slices per timestamp group; once the group is complete (or deadline expires), it delivers one assembled TLV. This is the right shape for batch DSP that needs N-sample windows.

### Transport choice for 1 GB/s

| Transport | Realistic max | When to use |
| ---- | ---- | ---- |
| `transport_shm` | 5–20 GB/s intra-host | Producer and consumer on same Linux box |
| `transport_iceoryx2` (future) | 5–20 GB/s intra-host | Same as SHM but with safety-cert backbone |
| `transport_rdma` (future) | 10–100 Gb/s inter-host | InfiniBand / RoCE LAN with HPC NICs |
| `transport_tcp` | 10 Gb/s realistic | LAN with regular NICs |
| `transport_can` | ~1 Mb/s | Don't even try for GB/s ADCs |

libtracer is a **control plane** that negotiates the data plane in the GB/s case: the SHM segment, the RDMA queue pair, the iceoryx2 service. The TLV ownership-transfer semantic is what makes the handoff zero-copy at the libtracer layer; the underlying transport then delivers without further library involvement.

---

## High-speed camera + LIDAR with synchronization

Two independent streams, common timestamp domain, subscriber joins by timestamp.

### Architecture

```
/camera/frame[0..N]          ← 30 fps, 4 MiB per frame
/lidar/scan[0..M]            ← 10 Hz, 100 KiB per scan
/sensor/clock                 ← PTP-synced clock vertex (optional)
```

Each producer publishes to its own vertex with its own slicing. **Both producers use the same wall-clock-ns timestamp** (typically PTP-synced via the host's hardware clock).

### Publisher: camera

```cpp
extern tr::graph::vertex_t* camera_frame[];   // camera_frame[i] == /camera/frame[i]

void on_frame(std::byte* frame, std::size_t frame_len, std::uint64_t /*ts_ns*/,
              tr::graph::graph_t& g) {
    std::size_t slice_size = 64 * 1024;
    std::size_t n = (frame_len + slice_size - 1) / slice_size;
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t len = std::min(slice_size, frame_len - i * slice_size);
        tr::view::view_t slice = tr::view::view_t::over(
            tr::view::borrow(std::span<std::byte>{frame + i * slice_size, len}));
        g.write(camera_frame[i], slice);   // all slices share the frame's ts_ns domain
    }
}
```

### Publisher: LIDAR

```cpp
void on_scan(std::byte* scan, std::size_t scan_len, std::uint64_t /*ts_ns*/,
             tr::graph::graph_t& g, tr::graph::vertex_t* lidar_scan0) {
    // Scan fits in one slice — borrow it directly.
    tr::view::view_t t = tr::view::view_t::over(
        tr::view::borrow(std::span<std::byte>{scan, scan_len}));
    g.write(lidar_scan0, t);   // lidar_scan0 == /lidar/scan[0]
}
```

### Subscriber: temporal join

```cpp
// Subscribe to both streams: a SUBSCRIBER record naming the local fusion handler,
// written to each parent's :subscribers[] field (subtree subscription).
g.write(tr::graph::path_t("/camera/frame:subscribers[]"), cam_subscriber_value);   // every /camera/frame[i]
g.write(tr::graph::path_t("/lidar/scan:subscribers[]"),   lidar_subscriber_value); // every /lidar/scan[i]

// In the fusion handler. The delivered view borrows the producer's bytes; a
// copy of the view_t (refcounted) keeps the segment alive while it sits in a buffer.
static frame_buffer_t pending_frame;   // map: ts_ns → assembled frame
static scan_buffer_t  pending_scan;

void on_camera_slice(const tr::view::view_t& delivered) {
    std::uint64_t ts = slice_timestamp(delivered);   // app reads the agreed timestamp
    frame_buffer_add_slice(&pending_frame, ts, delivered);   // stores a view_t copy
    try_emit_pair(ts);
}

void on_lidar_scan(const tr::view::view_t& delivered) {
    std::uint64_t ts = slice_timestamp(delivered);
    scan_buffer_add(&pending_scan, ts, delivered);
    try_emit_pair(ts);
}

void try_emit_pair(std::uint64_t ts) {
    // Allow ±5 ms slack in matching (PTP-synced clocks, sensor exposure window)
    std::uint64_t slack = 5 * 1000 * 1000;
    auto frame = frame_buffer_take_complete_near(&pending_frame, ts, slack);
    auto scan  = scan_buffer_take_near(&pending_scan, ts, slack);
    if (frame && scan) {
        run_fusion(*frame, *scan);   // views drop when the locals go out of scope
    }
}
```

The subscriber owns the temporal-join policy (window size, slack tolerance, dropping behavior on missing partner). The protocol just delivers timestamped TLVs; **synchronization is the application's responsibility**, libtracer makes the timestamps comparable.

### Coherency note

For sub-microsecond synchronization (e.g., precise stereo-LIDAR fusion), use PTP-synced hardware clocks (STM32F7+, ESP32-S3 with HW PTP, Linux with PHC). For ~ms accuracy (most robotics), NTP-sync is sufficient. The protocol carries u64 nanoseconds and trusts the publisher's clock; clock sync is a host-level concern.

---

## "Synchronize the value of a variable" pattern

A common need: a configuration or state variable that lives in one process and should be reflected on every other interested process. libtracer's read/write/subscribe primitives handle this directly, no separate "shared variable" type:

### Define the variable as a vertex

```cpp
// On the authoritative host: expose the variable as a STORED_VALUE vertex.
static std::int32_t target_rpm = 3000;

tr::graph::vertex_t* v_rpm =
    *g.register_vertex(tr::graph::path_t("/control/target_rpm"), tr::graph::role_t::STORED_VALUE);

// Borrow the live variable's bytes (zero-copy) and store the view.
g.write(v_rpm, tr::view::view_t::over(tr::view::borrow(
    std::span<std::byte>{reinterpret_cast<std::byte*>(&target_rpm), sizeof target_rpm})));
```

### Other hosts subscribe with `transient_local` durability

```cpp
// On a consumer host: set durability=transient_local + history_keep_last=1 on the
// cached subscriber's :settings, then register the subtree subscription.
g.write(tr::graph::path_t("/local/cached/target_rpm:settings"), durability_settings_value);
g.write(tr::graph::path_t("/control/target_rpm:subscribers[]"), subscriber_value);

// Anytime the consumer wants the latest value:
tr::graph::vertex_t* cached =
    *g.register_vertex(tr::graph::path_t("/local/cached/target_rpm"), tr::graph::role_t::STORED_VALUE);
auto r = g.read(cached);
std::int32_t rpm;
std::memcpy(&rpm, r->only().bytes().data(), sizeof rpm);
```

The combination of:

- **Read of the local cached vertex** = always returns the last-known-value, no network round-trip.
- **Subscription with `transient_local`** = late joiners get the current value, not just future updates.
- **Write to `/control/target_rpm`** = updates the authoritative vertex, fans out to all subscribers, all caches converge.

…gives the "shared variable" semantic without any extra protocol surface. Updates are eventually consistent; ordering within a single subscription is preserved; concurrent writes from multiple authoritative hosts are last-write-wins by timestamp (no CRDT, no consensus — see [04-communication-flows.md](04-communication-flows.md) §network partition).

---

## Worked example progression

The same vertex/edge primitives across **eight orders of magnitude** of payload rate:

| Application | Payload | Rate | Wire bytes / sec | TLV form |
| ---- | ---- | ---- | ---- | ---- |
| Single boolean (LED on/off) | 1 byte | 1 Hz | 8 B/s | 1 VALUE TLV |
| RC control input | 5 bytes | 100 Hz | 1.2 KB/s | 1 VALUE TLV |
| GPIO register | 4 bytes | poll | n/a (read-only) | view into MMIO |
| IMU sample | 28 bytes | 1 kHz | 35 KB/s | user-range record (PL=1) or packed VALUE |
| 1 KB sensor record | 1 KiB | 1 kHz | 1 MB/s | user-range record (PL=1) of named fields |
| 4K camera stream | 8 MiB | 30 Hz | 240 MB/s | address-shift `frame[0..N]` |
| Lidar + camera fusion | varies | 10 Hz | 250 MB/s | two vertex trees, ts-join |
| 1 GS/s ADC | 4 KiB slices | 244 kHz | 1 GB/s | address-shift `raw[0..N]` |
| Continuous shared variable | 4 bytes | 1 Hz | 32 B/s | VALUE + transient_local sub |
| 100 GB/s data plane | varies | varies | 100 GB/s | libtracer = control plane only; data plane via RDMA / SHM |

There is **no fundamental change** to the API, the wire format, or the addressing scheme across this range. What changes is:

- **Slice size** (chosen by the publisher to match transport MTU and processing granularity).
- **Transport module loaded** (UART for RC, TCP for IMU, SHM for ADC, RDMA for HPC).
- **QoS settings** (best-effort for high-rate, reliable for control, transient-local for shared state).

The protocol's job is to be invariant under these knobs; the user's job is to choose the knobs.

---

## Same-substrate operations: mix, split, concat

A structured TLV (any type with `opt.PL=1`) can be manipulated structurally without touching bytes. These operations are useful in routers, transforms, and any code that aggregates / disassembles structured TLVs.

### Concat: merge two structured TLVs of the same type

A structured `tr::wire::tlv_t` carries its members in a `children` vector, so these operations are expressed directly on that vector (see the [wire module](../modules/frame-codec.md)); the leaf `payload` spans keep borrowing their original buffers — no bytes are copied until you re-encode.

```cpp
tr::wire::tlv_t a = /* … */;   // SETTINGS {NAME "x", VALUE 1}
tr::wire::tlv_t b = /* … */;   // SETTINGS {NAME "y", VALUE 2}

tr::wire::tlv_t merged = a;    // same type/opt
merged.children.insert(merged.children.end(), b.children.begin(), b.children.end());
// merged is SETTINGS {NAME "x", VALUE 1, NAME "y", VALUE 2}
// The child payload spans still borrow a's and b's buffers.
```

### Split: cut a structured TLV at child index K

```cpp
tr::wire::tlv_t whole = /* … */;   // structured TLV with K1+K2 children

tr::wire::tlv_t first = whole, rest = whole;
first.children.assign(whole.children.begin(), whole.children.begin() + K1);
rest.children.assign(whole.children.begin() + K1, whole.children.end());
// first holds the first K1 children; rest holds the remaining K2.
// Both keep borrowing whole's child payload buffers.
```

### Mix: insert a child at position K

```cpp
whole.children.insert(whole.children.begin() + K, new_child);
// The children vector is updated; entries after K shift by one.
// No bytes copied unless you re-encode.
```

### Serialize

```cpp
std::vector<std::byte> out = tr::wire::encode(merged);
// 'out' contains the canonical wire bytes for merged.
// The proof obligation from [02-graph-model.md] guarantees this is identical
// to the bytes that would result from constructing the same logical container
// from scratch.
```

The proof obligation is the contract that makes mix/split/concat **safe to compose freely** — no operation can produce a TLV whose serialization differs from its logical content.

---

## What this section is NOT

- A serialization framework. libtracer doesn't replace Cap'n Proto / FlatBuffers / Protobuf for **typed**, **schema-evolving**, **reflection-rich** payloads. If you need that, embed those formats inside VALUE TLVs.
- A compression layer. libtracer doesn't compress; if your payload benefits from compression, do it in the publisher and document the encoding (NAME field "encoding" = "zstd-3" inside a structured (`PL=1`) TLV).
- A type system. libtracer's "TLV type" is a transport routing concern, not a data type. The user-range `0x80..0xFF` is for *protocol* tagging, not for substituting a real schema language.

---

## MCU-friendly publishing (zero-alloc, no `snprintf`)

> **Normative reference**: [../spec/v1.md](../spec/v1.md) §3.1.
> **See also**: [03-addressing.md](03-addressing.md) §static path handles; [04-communication-flows.md](04-communication-flows.md) §the static-path write flow.

The examples earlier in this document write by handle after registering a `path_t("/path/string")` (see the [graph module](../modules/graph.md)). On hosted platforms (Linux laptops, ESP32 with PSRAM and a relaxed code budget) this is fine. On a 16 KB Cortex-M0+ flashing telemetry from an ISR, it is unacceptable: `snprintf` alone is 2–6 KB of code, the parser walks the path string each call, and the segment allocator runs from interrupt context.

The MCU-friendly variant is to **encode the PATH TLV at build time** and pass a handle to the writer. Three orders of magnitude less per-write cost, and `snprintf` is no longer linked.

### Recipe — single sensor, build-time path

```cpp
// Parse-once handle: the PATH TLV is encoded a single time at registration and
// its bytes live for the node's lifetime. The path_t("...") ctor validates the
// literal (ADR-0054). A binding may additionally expose a consteval PATH encoder
// that emits the same bytes into .rodata.
tr::graph::graph_t g;
tr::graph::vertex_t* temp =
    *g.register_vertex(tr::graph::path_t("/sensor/temp"), tr::graph::role_t::STORED_VALUE);

void tim2_irq_handler() {            // hard-real-time ISR
    float t = read_thermistor_adc();

    // Fresh f32 VALUE view.
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(4);
    std::uint32_t bits;
    std::memcpy(&bits, &t, 4);
    for (int i = 0; i < 4; ++i)
        seg->bytes[i] = static_cast<std::byte>((bits >> (8 * i)) & 0xFF);

    // Single load + dispatch. ~0.4 µs at 100 MHz.
    g.write(temp, tr::view::view_t::over(std::move(seg)));

    TIM2->SR &= ~TIM_SR_UIF;
}
```

What the macro emits, verbatim, into `.rodata`:

```
06 40 12 00                                ← outer PATH TLV: type=0x06, opt=PL=1 (0x40), length=18
   02 00 06 00 73 65 6E 73 6F 72           ← NAME "sensor"
   02 00 04 00 74 65 6D 70                 ← NAME "temp"
```

22 bytes of flash. Zero RAM. Zero per-write allocation.

### Recipe — N indexed slots, init-time registration

When the path includes a runtime-derived index (an address-shift slice number, a peer-id), encode once at init and reuse the handle:

```cpp
constexpr std::size_t N_SLICES = 64;

// File scope — vertex handles are filled in at init.
static tr::graph::vertex_t* slice_vtx[N_SLICES];

// Called once from main() before the DMA / capture loop starts.
void publisher_init(tr::graph::graph_t& g) {
    for (std::size_t i = 0; i < N_SLICES; ++i) {
        // String work is ALLOWED here — init runs once and amortizes across the
        // program lifetime. path_t::parse returns std::expected; deref on success.
        auto p = tr::graph::path_t::parse("/adc/raw[" + std::to_string(i) + "]");
        slice_vtx[i] = *g.register_vertex(*p, tr::graph::role_t::STREAM);
        // Each register_vertex encodes exactly one long-lived PATH TLV.
    }
}

// DMA-half-complete ISR — has to be fast and ISR-safe.
void dma_half_complete_irq(std::byte* bytes, std::size_t len, tr::graph::graph_t& g) {
    std::size_t slice = (current_offset / SLICE_SIZE) % N_SLICES;
    tr::view::view_t v = tr::view::view_t::over(
        tr::view::borrow(std::span<std::byte>{bytes, len}));
    g.write(slice_vtx[slice], v);    // pointer load + dispatch; no string ops
}
```

The trade: a one-time RAM cost of ~1.6 KB (64 PATH TLVs averaging ~25 bytes each + bookkeeping) buys ISR-safe publishing of 64 distinct slot paths.

### Recipe — indexed slot paths (and a non-implemented optimization)

The reference core writes each slice by the handle of its real indexed path `/adc/raw[i]`:

```cpp
extern tr::graph::vertex_t* adc_raw[];   // adc_raw[i] == /adc/raw[i], registered at init

void dma_half_complete_irq(std::byte* bytes, std::size_t S, std::size_t n_slices_in_buf,
                           tr::graph::graph_t& g) {
    for (std::size_t i = 0; i < n_slices_in_buf; ++i) {
        tr::view::view_t v = tr::view::view_t::over(
            tr::view::borrow(std::span<std::byte>{bytes + i * S, S}));
        g.write(adc_raw[i], v);
    }
}
```

For very large N (e.g., 4096 slices) where individual registration would burn RAM, a **single-PATH-plus-index** form — encoding the base `/adc/raw` once and supplying `i` at write time, expanding `[i]` into the dispatch key without allocating — would help. This is a **permitted-but-not-implemented** optimization (non-normative): the reference core has no separate indexed-handle write. It would be semantically equivalent to the real write to `/adc/raw[i]` shown above, and from the subscriber's perspective the wire bytes are identical.

### What this buys, concretely

For a representative Cortex-M4 RC-car build (1 transport, no GUI, 32 KB flash budget):

| Variant | Flash overhead | RAM overhead | Per-write cost | ISR-safe? |
| ---- | ---- | ---- | ---- | ---- |
| String-form `path_t::parse(...)` on the hot path | +5 KB (`snprintf` etc.) | small heap alloc per write | 1–10 µs | No |
| Parse-once `path_t("...")` literal | +bytes of the path | none | ~0.4 µs | Yes |
| Init-registered handle | +bytes of the path | one PATH TLV per path | ~0.4 µs | Yes |
| Indexed slot paths (per-`[i]` handle) | +bytes of each path | one PATH TLV per slot | ~0.5 µs | Yes |

### When the string form is still the right answer

- Configuration tools / CLIs (`tracer-top`, REPLs) where path is a runtime user input — the user typed a string; parse it.
- Glue code on hosted platforms where the publisher runs at human-speed (a few writes per second from a worker thread). The string form is more readable; the cost is unmeasurable.
- Tests, where the verbosity is welcome and code size is irrelevant.

The string-form entry point is implementation-defined and OPTIONAL ([../spec/v1.md](../spec/v1.md) §3.1.4). A bare-metal build MAY omit it entirely; an ESP32 build with the IDE-companion module loaded will include it.

### Don't conflate this with serialization

The static-path optimization is purely about **the address of a vertex**, not about the value being written. The value TLV (`VALUE`, a user-range record, an MMIO view) is constructed per-write and follows the rules earlier in this document. What changed is only that the path side of `(path, value)` no longer requires runtime string work.
