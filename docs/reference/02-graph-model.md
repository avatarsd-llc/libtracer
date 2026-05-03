# Reference 02 — Graph Model

> **Status**: stub. Canonical content lives in [../plans/04-graph-and-endpoint-api.md](../plans/04-graph-and-endpoint-api.md) until promoted.

## What goes here when filled

- Definitions: vertex (endpoint), edge (subscription/binding), path (ordered names), schema (writable-field map per vertex).
- Naming rules: UTF-8, case-sensitive, `/`-separated, length cap, reserved characters.
- Wildcard matching: `*` (one segment), `**` (zero or more segments), at-least-one-match semantics.
- Schema discipline: core writable fields per vertex (`subscribers[]`, `settings.*`, `liveness.*`, `acl`, `schema`, `description`), module-namespaced extension fields.
- Address scopes: a path resolves first within the host-local graph, then via bridges; collision rules.
- Cross-walk: how libtracer's path model maps to ROS topics, DDS DataReader/Writer, MQTT topic trees, Zenoh keyspace.
