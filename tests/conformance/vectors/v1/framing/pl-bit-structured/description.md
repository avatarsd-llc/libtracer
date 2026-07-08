# framing/pl-bit-structured

The PL-bit polarity pair, structured side. type=0x07 POINT, opt=0x40 (PL=1 only), length=12. The payload is two concatenated child TLVs: VALUE `01 00 02 00 AB CD` and NAME `02 00 02 00 6F 6B` ("ok"). A conforming decoder MUST walk into the children.

The sibling vector `framing/pl-bit-opaque` carries the **byte-identical** 12-byte payload with opt=0x00 — only bit 6 differs between the two frames, so a core whose PL mask is shifted to any other bit position decodes one of the pair wrongly (wrong structure, or a bogus trailer/length read) and fails the round-trip or cross-core agreement.

```
07 40 0C 00 01 00 02 00 AB CD 02 00 02 00 6F 6B
```

16 bytes total.
