# ESP-IDF: one link, and its `max_frame`

A link is created the production way: the app declares a `udp-server` module on its
`transport_vertex_t`, then writes a connection SPEC to the module's creator endpoint,
`/net/udp-server/conn`. Left on the default sources, the link draws its receive blocks from the
arena's **net sub-pool** at its inbound frame cap. That makes `max_frame` a sizing decision on an
MCU, not a tuning knob.

## What to notice

- **The cap decides the draw.** A UDP link's receive buffers are one byte past the smaller of
  its SPEC's `max_frame` and its rx backend's slot. The default rx backend has no slot bound, so
  without `max_frame` the cap is the datagram limit, 64 KiB, twice the default arena: those draws
  are refused and every datagram is dropped, counted in the link's `dropped_rx`. The app sets
  `max_frame` to 1 KiB and reads `tr::mem::net_source()`'s census before and after the link comes
  up; the difference is what the link drew.
- **Nothing is injected.** `transport_vertex_t net(g, router)` takes its receive backend and
  egress source from the net sub-pool by default. A node that wants its links on a separate,
  fixed pool passes its own, and that pool's slot then bounds the buffers too (the `full_node`
  example does this for its listener).
- **Only the UDP transport is compiled.** `sdkconfig.defaults` turns TCP and WebSocket off.
- **lwIP is started first on a chip.** `esp_netif_init()` brings up the TCP/IP task the link's
  socket needs. The `linux` target has host sockets and skips it.
- **Board-only, named and not run:** a peer reaches the link only after the board joins a network
  (Wi-Fi or Ethernet bring-up, which is the application's). From a host on that network, dial
  `kind=udp`, `addr=<board IP>`, `port=47301`.

## Source

```{literalinclude} /integrations/esp-idf/examples/concepts/one_link/main/app_main.cpp
:language: cpp
:linenos:
```

## Build and run

```console
$ cd integrations/esp-idf/examples/concepts/one_link
$ idf.py set-target esp32c6 build
$ idf.py flash monitor        # board-only
```

CI builds it for `esp32c6`, and also builds and runs it on the ESP-IDF `linux` target, where the
link binds a host UDP port. See also: [a datagram already has boundaries](net-udp-datagram.md) ·
[DIAL and LISTEN are two constructors](net-dial-and-listen.md).
