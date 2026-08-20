# `backends/cuda` — the GPU device backend

`mem_cuda` is an L0 `tr::mem::mem_backend_t` over CUDA device memory
([ADR-0024](../../docs/adr/0024-mem-cuda-gpu-backend-heterogeneous-rope.md)). Its segments are
`DEVICE`-space: the bytes are a device pointer the CPU **must not** dereference, so such a
segment backs only an opaque `VALUE` payload while the TLV header and trailer stay in a `HOST`
segment — the heterogeneous host+device rope.

## Why it is a tier, not part of core

CUDA is a **vendor** name, and core carries none. This directory is a standalone CMake project
that *consumes* core (the same shape [`bench/`](../../bench/CMakeLists.txt) uses), so the
dependency arrow points up the tiers only: core's build never mentions `backends/`, and
`grep -rniE 'cuda|nvidia' core/src core/include` is empty.

The module plugs in through **`tr::mem::register_device_backend(backend, hook)`** — the L0
mirror of `tr::net::transport_vertex_t::register_transport_type`. `tr::mem::transfer` routes a
`DEVICE`-space segment to whatever hook its own backend registered, and answers `false` for a
backend nobody registered. Adding a second vendor (ROCm, an NPU, dmabuf) means adding a sibling
directory here, not an enumerator to core.

`register_cuda_backend()` is this module's `quic_transport_factory()`: the value the composition
root hands core. `cuda_backend()` calls it on first use, so a program that only ever calls
`tr::view::cuda_alloc` cannot end up with an unregistered backend.

## Build

Needs the CUDA toolkit. The implementation uses the CUDA **runtime** API only (no kernels), so
a host C++ compiler plus `libcudart` is enough — no `nvcc`.

```sh
cmake -S backends/cuda -B build/cuda -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/cuda -j
ctest --test-dir build/cuda --output-on-failure   # needs a real GPU
```

Link `libtracer_cuda` (it carries `libtracer` and `CUDA::cudart` as PUBLIC usage requirements)
and include `libtracer/mem_cuda.hpp` — the include spelling is unchanged from when the header
lived in core.

## Test

`tests/cuda_test.cpp` needs a real device, so **CI never builds this tier**.
[`tools/test-cuda.sh`](../../tools/test-cuda.sh) runs it in an `nvidia/cuda:*-devel` container
with the local GPU attached via CDI. It checks `cuda_alloc`, `space == DEVICE`, the idempotent
registration, the H2D→D2H round-trip through `tr::mem::transfer` (which is the proof the
registry routes on real hardware) and the heterogeneous rope's refusal to `flatten()`.

The GPU-free half of the same seam — does `tr::mem::transfer` route a *registered* device
backend to its hook, refuse an unregistered one, replace on re-registration and refuse to
overflow — is pinned in `core/tests/substrate_test.cpp` and runs on every pull request.
