# Reference 07 — Host Graph Embedded in the Larger System

> **Status**: stub. New section — no single canonical plan doc covers it end-to-end. Synthesizes [../plans/04-graph-and-endpoint-api.md](../plans/04-graph-and-endpoint-api.md) (graph model) and [../plans/05-modules-transport-and-discovery.md](../plans/05-modules-transport-and-discovery.md) (bridges/discovery).

## What goes here when filled

The load-bearing insight: **each host sees its slice of the network as a DAG; the global topology can be any graph, including cycles.** Conforming implementations must handle this without livelocks or duplicate delivery.

- **Per-host view**:
  - A host's local graph is a DAG: own vertices + bridge proxies (one proxy vertex per remote vertex this host has bound to).
  - Subscribers read the local DAG; the bridge layer is invisible from the API surface.
  - Bridge proxies are first-class vertices — they have schemas, settings, liveness state, just like local vertices.
- **Global topology**:
  - The union of all host DAGs is the global graph.
  - This global graph can have any shape: trees, meshes, rings, arbitrary cycles.
  - Two hosts may bridge the same remote vertex via different transport modules (CAN + IP); this is supported, not an error.
- **Cycle handling**:
  - Each emitted TLV carries an originating-peer-id and timestamp (in the `ROUTER` TLV when bridged).
  - On re-bridge, a host MUST drop a TLV it has already seen (matched by `(origin, ts)` in a small recent-set).
  - Recent-set size is implementation-defined; recommended bound is the deepest expected route fanout × longest expected delivery window.
- **Bridge identity**:
  - Each bridge module instance has a peer-id (UUIDv4 or device-derived).
  - Discovery modules announce `(peer-id, transport, address)` tuples; bridges consume these to decide what to bind.
- **"Every host is a router"**:
  - There is no architectural distinction between leaf and router. Any host with two transport modules loaded can bridge between them.
  - A WAN router (future module) is a host that runs only bridges + discovery — no application vertices. This is convention, not a separate node type.
- **Embedding examples**:
  - RC car (1 host, 1 transport, no bridges) → DAG = entire view.
  - Robot with CAN bus + WiFi → 2-transport host bridging CAN devices onto IP; CAN devices appear as proxy vertices to the WiFi-side subscribers.
  - Fleet of robots with central monitor → star topology; monitor host runs subscribers on `/**`.
  - Mesh of robots with no central node → arbitrary-cycle topology; dedup rule prevents storms.
- **What this means for application code**:
  - Application reads / writes paths. The path resolves locally (proxy or own vertex). Topology is invisible.
  - Failure of a remote bridge surfaces as a liveness event on the proxy vertex — same API as a local vertex going down.
