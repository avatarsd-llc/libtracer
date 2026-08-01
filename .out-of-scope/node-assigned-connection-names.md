# Node-Assigned Connection Names (optional SPEC name)

libtracer does not make the creator endpoint's connection `name` optional with a
node-assigned fallback for creator-initiated connections.

## Why this is out of scope

The proposal ([#622](https://github.com/avatarsd-llc/libtracer/issues/622)) was to let a
SPEC creation omit `name`, with the node appending to the next free registry slot and
returning the assigned segment. Two facts decided it (ADR-0073 §5):

1. **It adds no capability.** An application that wants container-like nameless append
   names its connections `"0"`, `"1"`, `"2"` today — zero protocol change. The registry
   underneath is already a stable slot table, so a creator-chosen numeric name is exactly
   as stable as an assigned one. The amendment was about who mints the segment, not about
   a new power.

2. **It costs retry idempotency, the one thing the creator-chosen name buys.** The creator
   endpoint exists for remote orchestration over links that drop. With a supplied name, a
   retried create answers `PATH_IN_USE` — the client learns the first attempt succeeded,
   and there is one connection. With an omitted name, the retry appends **again**: two
   connections, one orphaned, and nothing tells the client. A duplicate-resource bug
   reachable by ordinary packet loss is a bad trade for zero new capability, under the
   project's ordering (minimalism above unification).

The adjacent legitimate need — a session that has **no creator** to name it (a bus
listener accepting an inbound peer) — is served by the node-assigned `p<slot>` fallback of
ADR-0073 §2, where the idempotency objection cannot arise because there is no retry: the
peer dialed us.

The third-party orchestration flow also *depends* on the creator choosing the name: an
orchestrator that creates a link under a name it chose can compose the producer-rooted
subscription target offline, without reading back a reply that a lossy link might drop
(ADR-0073 §Consequences, #491).

## Revival condition

A real consumer demonstrating a need for node-assigned names **with** retry safety. The
correct shape then is a client-supplied idempotency token separate from the name (retry
with the same token returns the same assigned segment instead of appending). That is
strictly more machinery than "name it yourself" — which is why the status quo wins until
the need is demonstrated, and why the naive optional-name version stays rejected even
then.

## Prior requests

- [#622](https://github.com/avatarsd-llc/libtracer/issues/622) — "RFC-0014 amendment: make the SPEC's connection name optional (node assigns the slot and returns it)"
