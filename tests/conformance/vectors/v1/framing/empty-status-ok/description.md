# framing/empty-status-ok

The smallest valid libtracer TLV and the unsubscribe sentinel. type=0x09 STATUS, opt=0x00 (no flags; LL=0 means u16 length), length=0, no payload, no trailer. An empty STATUS means OK. 4 bytes total.

```
09 00 00 00
```
