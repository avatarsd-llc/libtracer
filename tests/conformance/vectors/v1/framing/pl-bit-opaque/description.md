# framing/pl-bit-opaque

The PL-bit polarity pair, opaque side. type=0x07 POINT, opt=0x00, length=12. The 12 payload bytes are **byte-identical** to `framing/pl-bit-structured`'s (which decodes them as two child TLVs); here opt.PL=0 so they MUST stay one opaque byte string. Only bit 6 of `opt` differs between the two frames — see the sibling's description for the mask-typo rationale.

```
07 00 0C 00 01 00 02 00 AB CD 02 00 02 00 6F 6B
```

16 bytes total.
