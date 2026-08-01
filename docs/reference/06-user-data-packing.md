# Reference 06 — User data packing into the graph

> **Topic**: how application data of any size — from a single boolean to a streaming GB/s feed — is put into the graph and delivered with the intended copy semantics.
> **Audience**: anyone designing the data layout for an application on top of libtracer.
> **See also**: [02-graph-model.md](02-graph-model.md) §read-vs-write copy semantics; [03-addressing.md](03-addressing.md) §address-shift slicing; [05-protocol-tlvs.md](05-protocol-tlvs.md) §VALUE.

---

## The packing rules

The protocol does not impose a serialization layer. The application picks the packing that fits its data; the protocol moves TLVs.

| Data shape | Recommended TLV form |
| ---- | ---- |
| Single scalar (bool, u8, u16, u32, u64, i32, f32, f64) | One `VALUE` TLV with raw little-endian bytes; or a user-range type code (`0x80..0xFF`) with implicit length |
| Fixed-shape struct (e.g., `{u32, u32, f32, f32}` IMU sample) | One `VALUE` TLV with packed-struct bytes; sender and receiver agree on the layout out-of-band |
| Self-describing record with named fields | User-range structured TLV (PL=1) with `NAME` + value-TLV children |
| Variable-length collection | User-range structured TLV (PL=1) with homogeneous children |
| Memory-mapped hardware register | Single `VALUE` TLV whose payload view points directly into MMIO space (zero-copy on read) |
| Large payload (anything where a single TLV is too big to ship) | Address-shift slicing across `ep[0..N]` with shared timestamp |
| Multiple coherent streams (camera + LIDAR) | Separate vertices, common timestamp domain; subscriber joins by timestamp |

The governing property: **the graph imposes no shape**. An endpoint is a name attached to a memory view. The protocol does not preordain payload shape, sample layout, or chunking strategy.

---

## Single boolean (or any single byte)

The minimal endpoint. A 1-byte payload, optionally with the timestamp prefix.

```cpp
// Publisher — see the graph module (../modules/graph.md) and view module (../modules/views.md).
tr::graph::graph_t g;
tr::graph::vertex_handle_t led =
    g.register_vertex(tr::graph::path_t("/dashboard/led"), tr::graph::role_t::STORED_VALUE);

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

Header overhead is **4 bytes** in the default case (`LL=0`, payload ≤ 64 KiB), or 6 bytes when `LL=1`. Trailer overhead is paid per-TLV only when the corresponding `opt` flags are set, and adds 0, 2, 4, 6, 8, 10 or 12 bytes — the timestamp contributes 0 (off), 4 (`TF=1`, relative i32) or 8 (`TF=0`, absolute u64), the checksum 0 (off), 2 (`CW=1`, CRC-16) or 4 (`CW=0`, CRC-32). Header plus trailer therefore runs 4–16 bytes per TLV at `LL=0` and 6–18 at `LL=1`. See [01-data-format.md](01-data-format.md) §frame size summary for the enumerated table.

Per-message overhead is the same whether the payload is 1 byte or 1 MiB; the cost amortizes immediately past the smallest payloads.

### Zero-copy on the read side

If the publisher's source data is already a `bool` somewhere in memory (a struct field, a static with a stable address), the TLV's view can point directly at it instead of being copied:

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

The published TLV reads the byte directly from `g_dash.led_on` at every fanout. If `g_dash` is updated between publish and subscriber-consume, the subscriber sees the new value. Whether that is the intent or a defect is application-dependent; where a snapshot is wanted, the explicit-copy form above is the correct one.

For a **multi-byte scalar** (u32, f64) the same pattern applies; the protocol does not care about scalar size.

---

## GPIO register as a memory-mapped vertex

A hardware register is memory at a fixed address. Wrapped in a view it becomes a libtracer vertex with **zero-copy reads**.

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
tr::graph::vertex_handle_t v_idr =
    g.register_vertex(tr::graph::path_t("/gpio/A/IDR"), tr::graph::role_t::STORED_VALUE);
g.write(v_idr, idr_view);
```

### Subscriber side

A read of a published value hands back a **reference to** that value, not a copy of it. In this binding the result type is `std::expected<value_ref_t, status_t>`: the outer `expected` carries the status, and the `value_ref_t` inside it dereferences to the stored rope.

```cpp
auto r = g.read(v_idr);                    // std::expected<value_ref_t, status_t>
if (!r) return;                            // r.error() is a status_t
std::span<const std::byte> bytes = (*r)->only().bytes();   // (*r) is the reference; -> reaches the rope
std::uint32_t idr_value;
std::memcpy(&idr_value, bytes.data(), sizeof idr_value);   // first copy at the boundary
```

The rule behind the type: *a read of a published value returns a reference to it; a read that composes a new value returns the value.* A folded or materialized subtree read has no published object to reference, so it hands back the composed value instead.

Holding the reference keeps the value alive. Where an implementation draws values from an injected allocator, an outstanding reference **pins** that allocation, so a reference held across many writes holds a value the allocator cannot reclaim.

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

tr::graph::vertex_handle_t v_bsrr =
    g.register_vertex(tr::graph::path_t("/gpio/A/BSRR"), tr::graph::role_t::HANDLER);
g.write(v_bsrr, value_u32(1u << 5));   // set pin PA5
```

The vertex registered for `/gpio/A/BSRR` (Bit Set/Reset Register at `0x40020018`) has a write-handler that copies the incoming TLV's payload bytes into the register. **One copy** — from the TLV view into the register. This is the single-copy write semantic: the TLV is a view (no copies on the way in), but landing it in the register requires one write to `*(volatile uint32_t *)0x40020018`.

### One substrate for software and hardware endpoints

A single API substrate (`read` / `write`, see the [graph module](../modules/graph.md)) covers:

- Logical software-defined endpoints (sensor readings, control state).
- Hardware-defined endpoints (GPIO registers, peripheral SFRs).
- Remote endpoints (a register on another MCU, reached by its source route over CAN).

To a subscriber, all three look identical. Tooling such as `tracer-top` enumerates the entire address space — software and hardware — through one walk.

---

## Structured record with named fields

For self-describing data, define a user-range structured TLV (`opt.PL=1`) with NAME + value children. Pick a type code in `0x80–0xFF` and document its layout for the project.

A structured node is a parent (`POINT`) TLV with `opt.PL=1` carrying `NAME` + `VALUE` children; the encoder emits it under a user-range type code (`0x80+`). See the [wire module](../modules/frame-codec.md).

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

void publish_imu(const imu_sample& s, tr::graph::graph_t& g, tr::graph::vertex_handle_t imu) {
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

A subscriber walks the children iteratively (per [01-data-format.md](01-data-format.md) §iterative parsing) and extracts fields by NAME match. This is **self-describing on the wire**: if the IMU record gains a `mag` field, subscribers built against the older layout ignore it and subscribers built against the newer one read it.

For a **fixed-shape** struct where schema evolution does not matter and bytes are precious, pack the whole struct as one VALUE TLV instead:

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
extern std::vector<tr::graph::vertex_handle_t> adc_raw;         // adc_raw[i] == /adc/raw[i]

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

Each `write` is a view-clone (a refcount bump on the DMA segment's backend) and a router dispatch. **No byte copies happen between the DMA buffer and the network's egress.** The only copy is in the transport layer when bytes leave the host (`send` system call into kernel buffer); for shared-memory or RDMA transports, even that copy disappears.

### Subscriber (process-as-stream)

Register the subscription by writing a SUBSCRIBER record to the parent's `:subscribers[]` field — a subtree subscription observes every indexed child (see [03-addressing.md](03-addressing.md) §subtree subscriptions and the [graph module](../modules/graph.md)):

```cpp
// Subscribe with assemble=false (default) — receive each slice as it arrives.
// The SUBSCRIBER value names the local handler path /local/dsp-pipeline.
g.write(tr::graph::path_t("/adc/raw:subscribers[]"), subscriber_value);   // subtree: every /adc/raw[i]

// In the dsp-pipeline handler, the delivered view borrows the producer's bytes.
void on_adc_slice(const tr::view::view_t& delivered) {
    auto t = tr::wire::decode(delivered);             // zero-copy decode
    std::span<const std::byte> bytes = t->payload;    // spans borrow the buffer
    process_adc_slice(bytes);                          // FIR filter, FFT, whatever
}
```

The subscriber processes 4 KiB at a time, never holding more than one slice's worth of memory. Throughput is bounded by the DSP pipeline and the transport, not by buffer allocation.

### Subscriber (assemble for batch processing)

Set the assembly QoS on the subscriber, then register the subscription. The
`address_shift.*` fields are the v1 QoS **design**, not implemented — see
[03-addressing.md](03-addressing.md) §subscriber assembly policies, whose deadline is a
module-namespaced magnitude of its own (the core `deadline_ns` knob it used to borrow was removed
as inert by [RFC-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md) §3.E):

```cpp
// Per-knob settings writes: assemble=true, expected_count for 100 ms batches,
// address_shift.deadline_ns = a 200 ms safety window. (Design; unimplemented.)
g.write(tr::graph::path_t("/local/batch-handler:settings.address_shift.assemble"), on_value);

// Register the subtree subscription: observes every /adc/raw[i].
g.write(tr::graph::path_t("/adc/raw:subscribers[]"), subscriber_value);
```

The router buffers slices per timestamp group; once the group is complete (or the deadline expires), it delivers one assembled TLV. This is the shape that suits batch DSP needing N-sample windows.

### Transport choice at 1 GB/s

The ceilings below are properties of the medium and its stack, not measurements of libtracer. They bound what any protocol on that medium can carry; libtracer's own per-operation costs are a separate question, treated in [15-concurrency-and-scaling.md](15-concurrency-and-scaling.md).

| Transport class | Medium ceiling | When to use |
| ---- | ---- | ---- |
| Shared memory | 5–20 GB/s intra-host | Producer and consumer on the same host |
| Zero-copy IPC with a safety-certified backbone | 5–20 GB/s intra-host | Same as shared memory, where certification is required |
| RDMA (InfiniBand / RoCE) | 10–100 Gb/s inter-host | HPC NICs on a LAN |
| TCP | ~10 Gb/s | LAN with ordinary NICs |
| CAN | ~1 Mb/s | Not viable for GB/s ADCs |

At these rates libtracer is a **control plane** that negotiates the data plane: the shared-memory segment, the RDMA queue pair, the IPC service. The TLV ownership-transfer semantic is what makes the handoff zero-copy at the libtracer layer; the underlying transport then delivers without further library involvement.

---

## High-speed camera and LIDAR with synchronization

Two independent streams, common timestamp domain, subscriber joins by timestamp.

### Architecture

```
/camera/frame[0..N]          ← 30 fps, 4 MiB per frame
/lidar/scan[0..M]            ← 10 Hz, 100 KiB per scan
/sensor/clock                 ← PTP-synced clock vertex (optional)
```

Each producer publishes to its own vertex with its own slicing. **Both producers use the same wall-clock-ns timestamp**, typically PTP-synced via the host's hardware clock.

### Publisher: camera

```cpp
extern std::vector<tr::graph::vertex_handle_t> camera_frame;   // camera_frame[i] == /camera/frame[i]

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
             tr::graph::graph_t& g, tr::graph::vertex_handle_t lidar_scan0) {
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

The subscriber owns the temporal-join policy: window size, slack tolerance, and what happens when a partner is missing. The protocol delivers timestamped TLVs; **synchronization is the application's responsibility**, and libtracer's contribution is making the timestamps comparable.

### Clock coherency

For sub-microsecond synchronization (precise stereo-LIDAR fusion, for instance), use PTP-synced hardware clocks — STM32F7 and later, ESP32-S3 with hardware PTP, Linux with a PHC. For ~ms accuracy, which covers most robotics, NTP sync is sufficient. The protocol carries u64 nanoseconds and trusts the publisher's clock; clock sync is a host-level concern.

---

## Shared-variable pattern

A configuration or state variable lives in one process and should be reflected in every other interested process. The read / write / subscribe primitives cover this directly; there is no separate "shared variable" type.

### Define the variable as a vertex

```cpp
// On the authoritative host: expose the variable as a STORED_VALUE vertex.
static std::int32_t target_rpm = 3000;

tr::graph::vertex_handle_t v_rpm =
    g.register_vertex(tr::graph::path_t("/control/target_rpm"), tr::graph::role_t::STORED_VALUE);

// Borrow the live variable's bytes (zero-copy) and store the view.
g.write(v_rpm, tr::view::view_t::over(tr::view::borrow(
    std::span<std::byte>{reinterpret_cast<std::byte*>(&target_rpm), sizeof target_rpm})));
```

### Other hosts subscribe requesting durability

```cpp
// On a consumer host: the subscription REQUESTS the producer's latched last value
// (RFC-0022 §3.A bit 5) — durability is a property of THIS subscription, not of the
// producer, so a sibling subscriber that does not ask is unaffected. The producer's own
// `:settings` carries only storage policy.
g.write(tr::graph::path_t("/control/target_rpm:subscribers[]"), durable_subscriber_value);

// Whenever the consumer wants the latest value:
tr::graph::vertex_handle_t cached =
    g.register_vertex(tr::graph::path_t("/local/cached/target_rpm"), tr::graph::role_t::STORED_VALUE);
auto r = g.read(cached);                   // std::expected<value_ref_t, status_t>
if (!r) return;
std::int32_t rpm;
std::memcpy(&rpm, (*r)->only().bytes().data(), sizeof rpm);
```

The combination of:

- **Read of the local cached vertex** — always returns the last-known-value, no network round-trip.
- **A subscription that sets `durability_request`** (RFC-0022 §3.A) — late joiners get the current value, not only future updates. It is the SUBSCRIBER's request, so a sibling subscription that does not ask is unaffected.
- **Write to `/control/target_rpm`** — updates the authoritative vertex, fans out to all subscribers, all caches converge.

…gives the shared-variable semantic without extra protocol surface. Updates are eventually consistent; ordering within a single subscription is preserved; concurrent writes from multiple authoritative hosts are last-write-wins by timestamp — no CRDT, no consensus (see [04-communication-flows.md](04-communication-flows.md) §network partition).

---

## Dynamic range of the worked examples

The same vertex/edge primitives cover **eight orders of magnitude** of payload rate. The wire-bytes column is an order-of-magnitude figure that includes per-TLV header and trailer.

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
| Continuous shared variable | 4 bytes | 1 Hz | 32 B/s | VALUE + a durability-requesting sub |
| 100 GB/s data plane | varies | varies | 100 GB/s | libtracer = control plane only; data plane via RDMA / shared memory |

Across this range there is **no fundamental change** to the API, the wire format, or the addressing scheme. What changes is:

- **Slice size** — chosen by the publisher to match transport MTU and processing granularity.
- **Transport module loaded** — UART for RC, TCP for IMU, shared memory for ADC, RDMA for HPC.
- **QoS** — best-effort for high-rate, reliable for control, `durability_request` for shared state: all three are the SUBSCRIPTION's (RFC-0022 §3.A), not the producer's.

The protocol's job is to be invariant under these knobs; the application's job is to choose them.

---

## Same-substrate operations: mix, split, concat

A structured TLV (any type with `opt.PL=1`) can be manipulated structurally without touching bytes. These operations serve routers, transforms, and any code that aggregates or disassembles structured TLVs.

### Concat: merge two structured TLVs of the same type

A structured `tr::wire::tlv_t` carries its members in a `children` vector, so these operations are expressed directly on that vector (see the [wire module](../modules/frame-codec.md)); the leaf `payload` spans keep borrowing their original buffers — no bytes are copied until re-encoding.

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
// No bytes copied unless the TLV is re-encoded.
```

### Serialize

```cpp
std::vector<std::byte> out = tr::wire::encode(merged);
// 'out' holds the canonical wire bytes for merged. The spec-level proof
// obligation (02-graph-model.md, §spec-level proof obligation) guarantees these
// are the same bytes that constructing the identical logical container from
// scratch would produce.
```

That proof obligation is the contract that makes mix/split/concat **safe to compose freely**: no operation can produce a TLV whose serialization differs from its logical content.

---

## Non-goals

- **Not a serialization framework.** libtracer does not replace Cap'n Proto, FlatBuffers or Protobuf for **typed**, **schema-evolving**, **reflection-rich** payloads. Where those are needed, embed them inside VALUE TLVs.
- **Not a compression layer.** libtracer does not compress. Where a payload benefits from compression, compress it in the publisher and document the encoding — a NAME field `"encoding"` = `"zstd-3"` inside a structured (`PL=1`) TLV.
- **Not a type system.** A TLV type is a transport routing concern, not a data type. The user range `0x80..0xFF` is for *protocol* tagging, not a substitute for a schema language.

---

## MCU-friendly publishing (zero-alloc, no `snprintf`)

> **Normative reference**: [../spec/v1.md](../spec/v1.md) §3.1.
> **See also**: [03-addressing.md](03-addressing.md) §static path handles; [04-communication-flows.md](04-communication-flows.md) §the static-path write flow.

The examples earlier in this document write by handle after registering a `path_t("/path/string")` (see the [graph module](../modules/graph.md)). On hosted platforms — Linux laptops, an ESP32 with PSRAM and a relaxed code budget — that is fine. On a 16 KB Cortex-M0+ flashing telemetry from an ISR it is not: `snprintf` alone costs **2–6 KB of code on Cortex-M, depending on the libc**, the parser walks the path string on every call, and the segment allocator runs from interrupt context.

The MCU-friendly variant **encodes the PATH TLV at build time** and passes a handle to the writer. Per-write cost is ~0.4 µs rather than the string form's 1–10 µs, and `snprintf` is not linked at all.

### Recipe — single sensor, build-time path

```cpp
// Parse-once handle: the PATH TLV is encoded a single time at registration and
// its bytes live for the node's lifetime. The path_t("...") ctor validates the
// literal (ADR-0054). A binding may additionally expose a consteval PATH encoder
// that emits the same bytes into .rodata.
tr::graph::graph_t g;
tr::graph::vertex_handle_t temp =
    g.register_vertex(tr::graph::path_t("/sensor/temp"), tr::graph::role_t::STORED_VALUE);

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

The encoded PATH TLV, verbatim, as it sits in `.rodata`:

```
06 40 12 00                                ← outer PATH TLV: type=0x06, opt=PL=1 (0x40), length=18
   02 00 06 00 73 65 6E 73 6F 72           ← NAME "sensor"
   02 00 04 00 74 65 6D 70                 ← NAME "temp"
```

22 bytes of flash. Zero RAM. Zero per-write allocation.

The parse-once constructor and its validation contract are recorded in [ADR-0054, `path_t` parse-once constructor](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0054-path-t-parse-once-constructor.md).

### Recipe — N indexed slots, init-time registration

Where the path includes a runtime-derived index — an address-shift slice number, a peer id — encode once at init and reuse the handle:

```cpp
constexpr std::size_t N_SLICES = 64;

// File scope — vertex handles are filled in at init.
static std::vector<tr::graph::vertex_handle_t> slice_vtx;

// Called once from main() before the DMA / capture loop starts.
void publisher_init(tr::graph::graph_t& g) {
    slice_vtx.reserve(N_SLICES);
    for (std::size_t i = 0; i < N_SLICES; ++i) {
        // String work is ALLOWED here — init runs once and amortizes across the
        // program lifetime. path_t::parse returns std::expected; deref on success.
        auto p = tr::graph::path_t::parse("/adc/raw[" + std::to_string(i) + "]");
        slice_vtx.push_back(g.register_vertex(*p, tr::graph::role_t::STREAM));
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

The trade: a one-time RAM cost of ~1.6 KB — 64 PATH TLVs averaging ~25 bytes each, plus bookkeeping — buys ISR-safe publishing of 64 distinct slot paths.

### Recipe — indexed slot paths

Each slice is written by the handle of its real indexed path `/adc/raw[i]`:

```cpp
extern std::vector<tr::graph::vertex_handle_t> adc_raw;   // adc_raw[i] == /adc/raw[i], registered at init

void dma_half_complete_irq(std::byte* bytes, std::size_t S, std::size_t n_slices_in_buf,
                           tr::graph::graph_t& g) {
    for (std::size_t i = 0; i < n_slices_in_buf; ++i) {
        tr::view::view_t v = tr::view::view_t::over(
            tr::view::borrow(std::span<std::byte>{bytes + i * S, S}));
        g.write(adc_raw[i], v);
    }
}
```

For very large N — 4096 slices, say — where individual registration would burn RAM, a **single-PATH-plus-index** form would help: encode the base `/adc/raw` once and supply `i` at write time, expanding `[i]` into the dispatch key without allocating. This is **permitted but not required**, and non-normative: no separate indexed-handle write exists in the reference core. It is semantically equivalent to the real write to `/adc/raw[i]` shown above, and from the subscriber's perspective the wire bytes are identical.

### Cost of each addressing mode

For a representative Cortex-M4 build with one transport, no GUI and a 32 KB flash budget. Per-write costs are the same ballpark figures as [04-communication-flows.md](04-communication-flows.md) §performance envelope, quoted at 100 MHz.

| Variant | Flash overhead | RAM overhead | Per-write cost | ISR-safe |
| ---- | ---- | ---- | ---- | ---- |
| String-form `path_t::parse(...)` on the hot path | `snprintf` at 2–6 KB depending on libc, plus the path parser | small heap alloc per write | 1–10 µs | No |
| Parse-once `path_t("...")` literal | the path's own bytes | none | ~0.4 µs | Yes |
| Init-registered handle | the path's own bytes | one PATH TLV per path | ~0.4 µs | Yes |
| Indexed slot paths (per-`[i]` handle) | the bytes of each path | one PATH TLV per slot | ~0.4 µs — an indexed slot path is an init-registered handle, and the dispatch is identical | Yes |

### Where the string form is the right answer

- Configuration tools and CLIs (`tracer-top`, REPLs) where the path is runtime user input — a string was typed, so parse it.
- Glue code on hosted platforms where the publisher runs at human speed, a few writes per second from a worker thread. The string form is more readable and the cost does not register.
- Tests, where verbosity is welcome and code size is irrelevant.

The string-form entry point is implementation-defined and OPTIONAL ([../spec/v1.md](../spec/v1.md) §3.1.4). A bare-metal build MAY omit it entirely; a build carrying an interactive-tooling module will include it.

### Path packing is not value packing

The static-path optimization concerns **the address of a vertex**, not the value written to it. The value TLV — a `VALUE`, a user-range record, an MMIO view — is constructed per write and follows the rules earlier in this document. What the optimization changes is only that the path side of `(path, value)` no longer requires runtime string work.

---

## Pitfalls

| Rule | Failure mode |
| ---- | ---- |
| A borrowed view is a live window, not a snapshot. | A publisher that borrows a mutable variable and then mutates it before fanout completes delivers the *new* bytes to subscribers that were sent the *old* value's notification. An implementation wanting snapshot semantics must copy into a fresh segment at write time. |
| A read returns a reference to the published value, not a copy. | Code that stores the read result long-term keeps that value alive. Where values come from an injected allocator, a long-lived reference pins an allocation the allocator cannot reclaim, and the working set grows with the number of outstanding references rather than with the graph. |
| A composed read (folded or materialized subtree) has no published object to reference. | An implementation that assumes every read hands back a reference into existing storage will get the ownership of a composed subtree read wrong — that result is a value the caller owns. |
| Per-TLV overhead is fixed, not proportional. | Sizing a stream by payload alone under-counts. At 1-byte payloads the 4–16 bytes of header and trailer dominate; batching into one structured TLV, or accepting the overhead, is a deliberate choice rather than a default. |
| An MMIO-backed vertex is read at read time, not at write time. | A subscriber that caches the view rather than the bytes reads the register again later and observes a different value than the one that triggered its notification. |
| Address-shift slices carry no reassembly metadata. | An implementation expecting the wire format to identify slice order or completeness will find nothing there. Grouping is by the shared timestamp the publisher stamps, and reassembly is subscriber policy (assemble QoS), not a wire feature. |
| The user type range `0x80..0xFF` carries no protocol meaning. | Two applications that assign different records to the same user type code and meet on one bus cannot be told apart by any protocol-level check. The code space is per-deployment and needs project-level registration. |
| `snprintf` on a publishing hot path is a code-size and latency decision, not a style one. | Linking it costs 2–6 KB of Cortex-M code depending on the libc, and the string form is not ISR-safe. A build that only ever publishes from an ISR should not link it at all. |
