# fwd/fwd-await-timeout

FWD{ op=AWAIT, dst=/sensor/temp, src=/reply-ep, await_timeout=1e9 ns }

`await_timeout` is the requester's own deadline (1 s). Per RFC-0004 Amendment 3 the terminus
MAY treat it as a hint and is not required to answer `TIMEOUT`. The vector pins the encoding
only; the bytes are unchanged by the amendment.

```
0f402e00010001000206000c000673656e736f720474656d7006000900087265706c792d65700100080000ca9a3b00000000
```
