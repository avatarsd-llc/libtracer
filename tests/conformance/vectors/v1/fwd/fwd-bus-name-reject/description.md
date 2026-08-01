# fwd/fwd-bus-name-reject

FWD{ op=READ, dst=/net/ws-server/srv/sensor, src=/reply-ep } — a `dst` routed THROUGH a multi-peer (bus) link's own connection NAME, with a residual below it that names no current peer.

Valid, round-trip-safe at the **codec** layer (the 3-core conformance machine checks `encode(decode(input))==input`, which holds). At the **routing** layer (RFC-0020, amending RFC-0004 §B per ADR-0073 §3) it MUST be **rejected, never broadcast**: a single directed `kind=ERROR` reply `STATUS=ERROR(tr::path::invalid=0x0021)` along the accumulated `src`. Only the link's peer names route below the mount; a `dst` naming the mount exactly still addresses the connection vertex locally. The C++ `mount_routing_test` asserts the rejection and both positive controls.

```
0f403e00010001000006402500020003006e65740200090077732d736572766572020003007372760200060073656e736f7206400c00020008007265706c792d6570
```
