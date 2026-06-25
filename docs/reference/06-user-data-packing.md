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

```c
// Publisher
bool led_on = true;
uint8_t b = led_on ? 1 : 0;
tlv_t *tlv = tlv_new_value(&b, sizeof b);
tracer_write("/dashboard/led", tlv);
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

```c
struct dashboard_state {
    bool led_on;        // single byte
    uint8_t  brightness;
    // ...
};

extern struct dashboard_state g_dash;

// Construct a view directly over g_dash.led_on. Because the segment
// owner is g_dash itself (or a static segment descriptor for it),
// no memcpy happens. The view is a (pointer, length=1) pair into
// the live struct.
tlv_t *tlv = tlv_view_into(&g_dash.led_on, sizeof g_dash.led_on,
                           &g_dash_segment);
tracer_write("/dashboard/led", tlv);
```

The published TLV reads the byte directly from `g_dash.led_on` at every fanout. If `g_dash` is updated between publish and subscriber-consume, the subscriber sees the new value. (Whether this is desired or a bug is application-dependent — usually you want a snapshot, in which case the explicit-copy form above is correct.)

For a **multi-byte scalar** (u32, f64) the same pattern applies; the protocol does not care about scalar size.

---

## GPIO register (memory-mapped I/O as a vertex)

A hardware register is just memory at a fixed address. Wrap it in a view and it becomes a libtracer vertex with **zero-copy reads**.

```c
#define GPIOA_IDR_ADDR  0x40020010   // STM32F4 GPIOA input data register

// Construct a static segment descriptor for the MMIO region. The segment's
// `destroy` callback is a no-op (MMIO is not allocated; you can't free it).
static segment_t gpio_segment = {
    .refcount = 1,                   // permanently held
    .destroy  = noop_destroy,
    .base     = (void *)GPIOA_IDR_ADDR,
    .size     = sizeof(uint32_t),
};

// Expose GPIOA's input data register as a vertex.
// Every read produces a TLV whose view points at the live register;
// no memory copy occurs.
tracer_register_view_vertex("/gpio/A/IDR",
                            &gpio_segment,
                            /*offset=*/0,
                            /*length=*/sizeof(uint32_t));
```

### Subscriber side

```c
tlv_t *t = tracer_read("/gpio/A/IDR");
uint32_t idr_value;
memcpy(&idr_value, tlv_payload(t), sizeof idr_value);  // first copy at the boundary
tlv_release(t);
```

The TLV's payload pointer points directly at `0x40020010`. The subscriber chooses whether to copy into local memory (for a stable snapshot) or operate on the view directly.

### Write side (single-copy, register-mapped)

```c
uint32_t bit = 1u << 5;  // set pin PA5
tlv_t *tlv = tlv_new_value(&bit, sizeof bit);
tracer_write("/gpio/A/BSRR", tlv);
```

The vertex registered for `/gpio/A/BSRR` (Bit Set/Reset Register at `0x40020018`) has a write-handler that copies the incoming TLV's payload bytes into the register. **One copy** — from the TLV view into the register. This is the single-copy write semantic: the TLV is a view (no copies on the way in), but landing it in the register requires one write to `*(volatile uint32_t *)0x40020018`.

### Why this matters

A single API substrate (`tracer_read` / `tracer_write`) covers:

- Logical software-defined endpoints (sensor readings, control state).
- Hardware-defined endpoints (GPIO registers, peripheral SFRs).
- Bridged remote endpoints (a register on another MCU, accessible via CAN bridge).

To a subscriber, all three look identical. Tooling like `tracer-top` enumerates the entire address space — software and hardware — through one walk.

---

## Structured record with named fields

For self-describing data, define a user-range structured TLV (`opt.PL=1`) with NAME + value children. Pick a type code in `0x80–0xFF` and document its layout for your project.

```c
struct imu_sample {
    uint64_t ts_ns;
    float    accel_x, accel_y, accel_z;
    float    gyro_x, gyro_y, gyro_z;
};

void publish_imu(const struct imu_sample *s) {
    // type=0x80 USER_IMU_RECORD, opt.PL=1
    tlv_t *rec = tlv_new_structured(USER_IMU_RECORD);
    tlv_append_child(rec, tlv_new_name("ts_ns"));
    tlv_append_child(rec, tlv_new_value(&s->ts_ns, sizeof s->ts_ns));
    tlv_append_child(rec, tlv_new_name("accel"));
    tlv_append_child(rec, tlv_new_value(&s->accel_x, 3 * sizeof(float)));
    tlv_append_child(rec, tlv_new_name("gyro"));
    tlv_append_child(rec, tlv_new_value(&s->gyro_x, 3 * sizeof(float)));
    tracer_write("/imu", rec);
}
```

A subscriber walks the children iteratively (per [01-data-format.md](01-data-format.md) §iterative parsing) and extracts fields by NAME match. This is **self-describing on the wire**: if the IMU record gains a `mag` field later, old subscribers ignore it; new subscribers read it.

For a **fixed-shape** struct where schema evolution doesn't matter and bytes are precious, pack the whole struct as one VALUE TLV instead:

```c
tlv_t *tlv = tlv_new_value(s, sizeof *s);
tracer_write("/imu", tlv);
```

The trade-off is wire-format-versus-self-description, identical to the choice between Cap'n Proto (fixed schema) and JSON (named fields).

---

## Streaming a high-speed ADC (1 GB/s)

The publisher slices the stream across enumerated child endpoints with shared timestamps. Each slice is independently routable; the receiver assembles or processes-as-stream per its QoS.

### Publisher

```c
#define SLICE_SIZE  (4 * 1024)         // 4 KiB per slice, fits one MTU on most LANs
#define SLICES_PER_SECOND  (1000UL * 1000 * 1000 / SLICE_SIZE)  // ~244000

void on_dma_complete(const uint8_t *adc_buf, size_t buf_len, uint64_t ts) {
    size_t n_slices = buf_len / SLICE_SIZE;
    for (size_t i = 0; i < n_slices; i++) {
        // Construct a view directly into the DMA buffer — no memcpy.
        // The DMA buffer's segment refcount tracks who holds views.
        tlv_t *tlv = tlv_view_into_with_ts(
            adc_buf + i * SLICE_SIZE,
            SLICE_SIZE,
            &dma_buf_segment,
            ts);

        char path[64];
        snprintf(path, sizeof path, "/adc/raw[%zu]", i);
        tracer_write(path, tlv);
    }
}
```

Each `tracer_write` is a view-clone (refcount bump on the DMA segment) and a router dispatch. **No byte copies happen between the DMA buffer and the network's egress.** The only copy is in the transport layer when bytes leave the host (`send` system call into kernel buffer); for SHM or RDMA transports, even that copy disappears.

### Subscriber (process-as-stream)

```c
// Subscribe with assemble=false (default) — receive each slice as it arrives.
tlv_t *sub = tlv_new_subscriber("/local/dsp-pipeline", default_settings());
tracer_write("/adc/raw[*]:subscribers[]", sub);

// In the dsp-pipeline handler:
void on_adc_slice(const tlv_t *t, void *ctx) {
    const uint8_t *bytes = tlv_payload(t);
    size_t        len   = tlv_payload_len(t);
    uint64_t      ts    = tlv_timestamp(t);
    process_adc_slice(bytes, len, ts);   // FIR filter, FFT, whatever
}
```

The subscriber processes 4 KiB at a time, never holding more than one slice's worth of memory. Throughput is bounded by the DSP pipeline + transport, not by buffer allocation.

### Subscriber (assemble for batch processing)

```c
tlv_t *settings = tlv_new_settings_list({
    {"address_shift.assemble", true},
    {"address_shift.expected_count", n_slices_per_second / 10},  // 100ms batches
    {"deadline_ns", 200 * 1000 * 1000},                          // 200ms safety
});
tlv_t *sub = tlv_new_subscriber("/local/batch-handler", settings);
tracer_write("/adc/raw[*]:subscribers[]", sub);
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

```c
void on_frame(const uint8_t *frame, size_t frame_len, uint64_t ts_ns) {
    size_t slice_size = 64 * 1024;
    size_t n = (frame_len + slice_size - 1) / slice_size;
    for (size_t i = 0; i < n; i++) {
        tlv_t *t = tlv_view_into_with_ts(frame + i * slice_size,
                                          MIN(slice_size, frame_len - i * slice_size),
                                          &camera_segment, ts_ns);
        char p[48];
        snprintf(p, sizeof p, "/camera/frame[%zu]", i);
        tracer_write(p, t);
    }
}
```

### Publisher: LIDAR

```c
void on_scan(const uint8_t *scan, size_t scan_len, uint64_t ts_ns) {
    // Scan fits in one slice.
    tlv_t *t = tlv_view_into_with_ts(scan, scan_len, &lidar_segment, ts_ns);
    tracer_write("/lidar/scan[0]", t);
}
```

### Subscriber: temporal join

```c
// Subscribe to both streams.
tlv_t *cam_sub   = tlv_new_subscriber("/local/fusion/cam",   default_settings());
tlv_t *lidar_sub = tlv_new_subscriber("/local/fusion/lidar", default_settings());
tracer_write("/camera/frame[*]:subscribers[]", cam_sub);
tracer_write("/lidar/scan[*]:subscribers[]",   lidar_sub);

// In the fusion handler:
static frame_buffer_t  pending_frame;   // map: ts_ns → assembled frame
static scan_buffer_t   pending_scan;

void on_camera_slice(const tlv_t *t, void *_) {
    uint64_t ts = tlv_timestamp(t);
    frame_buffer_add_slice(&pending_frame, ts, t);   // tlv_acquire to keep view alive
    try_emit_pair(ts);
}

void on_lidar_scan(const tlv_t *t, void *_) {
    uint64_t ts = tlv_timestamp(t);
    scan_buffer_add(&pending_scan, ts, t);
    try_emit_pair(ts);
}

void try_emit_pair(uint64_t ts) {
    // Allow ±5 ms slack in matching (PTP-synced clocks, sensor exposure window)
    uint64_t slack = 5 * 1000 * 1000;
    auto frame = frame_buffer_take_complete_near(&pending_frame, ts, slack);
    auto scan  = scan_buffer_take_near(&pending_scan, ts, slack);
    if (frame && scan) {
        run_fusion(frame, scan);
        tlv_release(frame);
        tlv_release(scan);
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

```c
// On the authoritative host:
static int32_t target_rpm = 3000;
tracer_register_view_vertex("/control/target_rpm",
                            &local_segment,
                            offsetof(/* the local var */ ...),
                            sizeof(int32_t));
```

### Other hosts subscribe with `transient_local` durability

```c
// On a consumer host:
tlv_t *settings = tlv_new_settings_list({
    {"durability", DURABILITY_TRANSIENT_LOCAL},
    {"history_keep_last", 1},
});
tlv_t *sub = tlv_new_subscriber("/local/cached/target_rpm", settings);
tracer_write("/control/target_rpm:subscribers[]", sub);

// Anytime the consumer wants the latest value:
tlv_t *t = tracer_read("/local/cached/target_rpm");
int32_t rpm;
memcpy(&rpm, tlv_payload(t), sizeof rpm);
tlv_release(t);
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

```c
tlv_t *a = ...; // SETTINGS {NAME "x", VALUE 1}
tlv_t *b = ...; // SETTINGS {NAME "y", VALUE 2}
tlv_t *merged = tlv_struct_concat(a, b);
// merged is SETTINGS {NAME "x", VALUE 1, NAME "y", VALUE 2}
// No bytes copied. 'merged' holds views into a's and b's segments.
```

### Split: cut a structured TLV at child index K

```c
tlv_t *whole = ...; // structured TLV with K1+K2 children
tlv_t *first, *rest;
tlv_struct_split(whole, K1, &first, &rest);
// first holds first K1 children; rest holds the remaining K2.
// Both share whole's underlying segment via refcount.
```

### Mix: insert a child at position K

```c
tlv_struct_insert(whole, K, new_child);
// View tree updated; insertion point's children shifted in the view-array.
// No bytes copied unless serialization is requested.
```

### Serialize

```c
size_t  n = tlv_serialized_size(merged);
uint8_t *out = malloc(n);
tlv_serialize_to(merged, out, n);
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

The examples earlier in this document use `tracer_write("/path/string", tlv)` for clarity. On hosted platforms (Linux laptops, ESP32 with PSRAM and a relaxed code budget) this is fine. On a 16 KB Cortex-M0+ flashing telemetry from an ISR, it is unacceptable: `snprintf` alone is 2–6 KB of code, the parser walks the path string each call, and the segment allocator runs from interrupt context.

The MCU-friendly variant is to **encode the PATH TLV at build time** and pass a handle to the writer. Three orders of magnitude less per-write cost, and `snprintf` is no longer linked.

### Recipe — single sensor, build-time path

```c
#include "tracer.h"

// PATH TLV is generated at compile time and lives in .rodata / flash.
// The macro validates the literal at preprocessor time; an invalid
// path fails the build, not the runtime.
static const tracer_path_t TEMP_PATH = TRACER_PATH("/sensor/temp");

void tim2_irq_handler(void) {       // hard-real-time ISR
    float t = read_thermistor_adc();

    // Inline value TLV; no heap touch.
    uint8_t buf[12];
    tlv_t value = tlv_inline_value_f32(buf, sizeof buf, t);

    // Single load + dispatch. ~0.4 µs at 100 MHz.
    tracer_write(&TEMP_PATH, &value);

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

```c
#define N_SLICES  64

// File scope — handles are filled in at init.
static tracer_path_handle_t h_slice[N_SLICES];

// Called once from main() before the DMA / capture loop starts.
void publisher_init(void) {
    for (size_t i = 0; i < N_SLICES; i++) {
        char buf[40];
        // sprintf is ALLOWED here — init runs once, code-size of one
        // sprintf call amortizes across the program lifetime.
        snprintf(buf, sizeof buf, "/adc/raw[%zu]", i);
        h_slice[i] = tracer_path_register(buf);
        // h_slice[i] now points at a heap-allocated PATH TLV that
        // will not be freed for the rest of the node's life.
    }
}

// DMA-half-complete ISR — has to be fast and ISR-safe.
void dma_half_complete_irq(const uint8_t *bytes, size_t len, uint64_t ts_ns) {
    size_t slice = (current_offset / SLICE_SIZE) % N_SLICES;
    tlv_t v = tlv_view_into_with_ts(bytes, len, &dma_segment, ts_ns);
    tracer_write(h_slice[slice], &v);    // pointer load + dispatch; no string ops
}
```

The trade: a one-time RAM cost of ~1.6 KB (64 PATH TLVs averaging ~25 bytes each + bookkeeping) buys ISR-safe publishing of 64 distinct slot paths.

### Recipe — indexed-handle helper (when registering N is too costly)

For very large N (e.g., 4096 slices) where individual registration burns RAM, the indexed-handle form encodes the **base path** once and supplies the index at write time. The implementation expands `[i]` into the dispatch key on the fly without allocating:

```c
// One PATH TLV for the base name.
static const tracer_path_t ADC_RAW = TRACER_PATH("/adc/raw");

void dma_half_complete_irq(...) {
    for (size_t i = 0; i < n_slices_in_buf; i++) {
        tlv_t v = tlv_view_into_with_ts(bytes + i*S, S, &dma_segment, ts_ns);
        tracer_write_indexed(&ADC_RAW, /*index=*/i, &v);
    }
}
```

The router treats `tracer_write_indexed(&ADC_RAW, i, v)` as semantically equivalent to a write to `/adc/raw[i]`. From the subscriber's perspective the wire bytes are identical.

### What this buys, concretely

For a representative Cortex-M4 RC-car build (1 transport, no GUI, 32 KB flash budget):

| Variant | Flash overhead | RAM overhead | Per-write cost | ISR-safe? |
| ---- | ---- | ---- | ---- | ---- |
| String-form `tracer_write_str(...)` | +5 KB (`snprintf` etc.) | small heap alloc per write | 1–10 µs | No |
| Build-time `TRACER_PATH(...)` literal | +bytes of the path | none | ~0.4 µs | Yes |
| Init-registered handle | +bytes of the path | one PATH TLV per path | ~0.4 µs | Yes |
| Indexed-handle on a base path | +bytes of the base path | none | ~0.5 µs | Yes |

### When the string form is still the right answer

- Configuration tools / CLIs (`tracer-top`, REPLs) where path is a runtime user input — the user typed a string; parse it.
- Glue code on hosted platforms where the publisher runs at human-speed (a few writes per second from a worker thread). The string form is more readable; the cost is unmeasurable.
- Tests, where the verbosity is welcome and code size is irrelevant.

The string-form entry point is implementation-defined and OPTIONAL ([../spec/v1.md](../spec/v1.md) §3.1.4). A bare-metal build MAY omit it entirely; an ESP32 build with the IDE-companion module loaded will include it.

### Don't conflate this with serialization

The static-path optimization is purely about **the address of a vertex**, not about the value being written. The value TLV (`VALUE`, a user-range record, an MMIO view) is constructed per-write and follows the rules earlier in this document. What changed is only that the path side of `(path, value)` no longer requires runtime string work.
