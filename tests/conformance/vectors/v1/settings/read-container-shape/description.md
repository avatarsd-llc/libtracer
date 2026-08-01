# settings/read-container-shape

[RFC-0022](../../../../../../docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
§5.5 — the bare `:settings` read **keeps its container and loses its knobs**. The bytes
below are what a vertex declaring one `rw` app field `kp` (holding `VALUE u32 = 7`) serves:

```
SETTINGS (PL=1) {
  NAME "app"
  SETTINGS (PL=1) {
    NAME "kp"
    VALUE u32 = 7                    ; the stored field TLV, verbatim
  }
}
```

**Both halves of the vector** (§4):

- a vertex that DOES declare app fields serves `SETTINGS{ NAME "app" SETTINGS{…} }` — the
  bytes above;
- a vertex that declares NONE serves the **empty** container `0B 40 00 00`, which is
  honest rather than absent.

The reserved `app` subkey and the single-traversal renderer contract of RFC-0010 §A.4 both
survive; what is gone is the flat core-namespace knob enumeration that used to precede the
`app` record. `:settings.app` and `:settings.app.<name…>` are unchanged.

The read now enumerates exactly what the WRITE gate accepts — which is nothing in the core
namespace, honestly, rather than seven names of which four were never honoured. A write to
`:settings.<any knob name>` answers `ERROR{tr::schema::not_found}` (`0x0031`); see
`settings/removed-knob`.

```
0b401900020003006170700b400e00020002006b700100040007000000
```
