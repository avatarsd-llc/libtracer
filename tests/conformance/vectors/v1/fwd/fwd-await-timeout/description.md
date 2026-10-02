# fwd/fwd-await-timeout

FWD{ op=AWAIT, dst=/sensor/temp, src=/reply-ep, await_timeout=1e9 ns }

`await_timeout` is the requester's own deadline (1 s). Per RFC-0004 Amendment 3 the terminus
MAY treat it as a hint and is not required to answer `TIMEOUT`. The vector pins the encoding
only; the bytes are unchanged by the amendment.

```
0f4037000100010002064012000200060073656e736f720200040074656d7006400c00020008007265706c792d65700100080000ca9a3b00000000
```
