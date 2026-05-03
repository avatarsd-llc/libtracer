# core — Reference C/C++ implementation

The reference implementation of the libtracer protocol. Header-first, no exceptions, no RTTI. Suitable for ESP32, STM32, and bare-metal targets.

## Layout

```text
core/
├── include/libtracer/    Public headers — users write #include <libtracer/...>
├── src/                  Implementation (.cpp)
├── tests/                Unit tests (host-side)
└── CMakeLists.txt
```

The directory name `include/libtracer/` is part of the public API: include paths in user code (`#include <libtracer/tracer.hpp>`) depend on it. Do not rename.

## Building

```sh
cmake -B build
cmake --build build
ctest --test-dir build
```

## Compatibility

Targets C++17. No standard-library allocations on hot paths. No exceptions. No RTTI. See [docs/reference/](../docs/reference/) for design constraints.
