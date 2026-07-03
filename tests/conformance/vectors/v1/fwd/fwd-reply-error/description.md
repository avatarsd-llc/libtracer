# fwd/fwd-reply-error

FWD{ op=REPLY, dst=/via_board/via_net/reply-ep, src=/sensor, kind=ERROR, STATUS{ ERROR{ VALUE u16=0x0020 tr::path::not_found } } }

The ERROR is structured (opt.PL=1) per RFC-0002 §C: its first child is a VALUE
carrying the u16 LE registered code `0x0020` (`tr::path::not_found`).

```
0f404e00010001000306402400020009007669615f626f617264020007007669615f6e6574020008007265706c792d657006400a000200060073656e736f72010001000109400a0008400600010002002000
```
