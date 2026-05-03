# Conformance test vectors

Cross-implementation test data. **Every implementation of libtracer — the reference impl, the Rust binding, third-party reimplementations — runs the same vectors.** Passing them is the operational definition of compatibility.

## Layout

```text
tests/conformance/
├── vectors/
│   ├── v1/              Vectors tagged for spec version 1
│   │   ├── encode/      Input → expected wire bytes
│   │   ├── decode/      Wire bytes → expected structure
│   │   └── roundtrip/   Encode-then-decode equality
│   └── v2/              (when v2 lands)
└── README.md
```

Vectors are language-agnostic — typically JSON or hex, with a small driver in each implementation that consumes them.

## Adding vectors

A new vector is appropriate when:

- A spec change (new RFC) requires verifying behavior across implementations.
- A bug surfaced ambiguity that the spec did not pin down — fix the spec first, then add a vector that future implementations would have caught.

Vectors for a published spec version are append-only. Removing or changing a vector for a frozen version is a spec change.
