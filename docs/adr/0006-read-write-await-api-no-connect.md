# The API is read / write / await + a field-write control surface — no connect/disconnect/subscribe

The entire data API is **three calls — `read`, `write`, `await`** — plus refcount management. Every control surface (subscriptions, QoS, ACLs, liveness) is a **field-write** to a `:`-addressed vertex field: subscribing *is* writing a SUBSCRIBER TLV into a `:subscribers[]` slot. There is **no** `connect` / `disconnect` / `subscribe` primitive.

## Considered options

- **Explicit subscribe/connect verbs** as in DDS, Zenoh, MQTT. Rejected: libtracer folds all control into one uniform addressing model under the `:` separator, trading named-verb discoverability for radical API minimalism and a single mental model.

## Consequences

- The pre-spec C++ in `core/` ships the exact inverse (`connect`/`disconnect` on `point_i`, and no `await` anywhere) and is discarded under [the rebuild decision](0001-extract-reference-implementation-from-strawberry-fw.md).
- Bindings and integrations target `read`/`write`/`await` + field-write; `await`'s placement (per-vertex vs free `tracer_await()`) is the one open detail for the rewrite.
