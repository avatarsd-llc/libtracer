# A container that fails by value (L0 substrate)

A bounded seam is only bounded if the containers above it can report a refusal.
`tr::mem::block_array_t<T>` is that container: the same four-word footprint as a
`std::pmr::vector`, drawing from an injected [`block_source_t`](mem-block-source.md), with two
differences that carry the whole point.

1. **Growth returns `false` instead of throwing.** `std::pmr::vector::push_back` on an exhausted
   resource throws, and on ESP-IDF that reaches the link-wrapped `__cxa_throw` → `abort()` stub —
   a peer-reachable reboot when the container sits on the RX decode path.
2. **Relocation is a `memcpy`.** `T` must be trivially copyable and trivially destructible, so
   growth needs no move loop and the vacated block needs no destruction.

## What to notice

- **A refused push leaves the array unchanged**, which is what lets every caller treat
  exhaustion as a clean reject rather than as a half-applied operation. The example asserts both
  the size and the surviving element after the refusal.
- **`push_slot()` is not a convenience.** Claiming one uninitialized slot and filling it in place
  removes the temporary entirely; building a 48-byte element on the stack and copying it in
  writes it field-by-field and reads it back as wide loads, and the resulting store-forwarding
  stall made the first working migration of the terminus decode **45 % slower while executing
  fewer instructions** (IPC 5.03 → 2.55). Hot paths use `push_slot`.
- **The two `static_assert`s are the seam's edge, and they are where a migration stops being
  mechanical.** An element type holding a `std::string` or a `std::vector` is rejected. The fix
  is not an owning-but-nothrow element — it is to make the public descriptor **non-owning**
  (`std::string_view`, `std::span<const std::byte>`) and let the store copy the bytes into its
  own blocks ([reference 09](../reference/09-memory-substrate.md) §migrating a STORE). Where the
  element type genuinely cannot be inverted, the `std::pmr`
  [adapter](mem-source-resource.md) is the documented escape — with its own, weaker, guarantee.
- **The array binds its source at construction.** A `block_array_t` **member** takes its store in
  its owner's constructor and keeps it for life, so a `set_…_source` setter *cannot* re-seat one.
  A brace-initialised default (`mem::block_array_t<std::byte> buf_{mem::heap_source()};`) is a
  hardcoded store wearing a member-initialiser, and it is invisible to every injection point the
  type otherwise offers.
- **`block_array_t` runs no destructors, and has no `erase`.** That is the price of trivially
  copyable elements: a store built on it frees its byte blocks by hand through one helper, and
  erases by swapping the last element down.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/mem_block_array.cpp
:language: cpp
:linenos:
```

See also: [backends](../modules/backends.md) ·
[memory substrate reference](../reference/09-memory-substrate.md) ·
[the failable block seam](mem-block-source.md) ·
[the `std::pmr` adapter](mem-source-resource.md).
