# field/field-nested

FIELD{ NAME "settings", NAME "app" } — :settings.app (two scalar levels)

The reserved `app` subkey (RFC-0010 §A.1) is what a two-level `:settings.<x>` field addresses
now: RFC-0022 §3.B deleted the flat core-namespace knobs, so the second level is either `app`
or it resolves to nothing.

```
104013000200080073657474696e677302000300617070
```
