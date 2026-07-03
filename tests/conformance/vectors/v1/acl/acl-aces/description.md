# acl-aces

An `0x0A` ACL TLV carrying two NFSv4-style ACEs ([ADR-0020](../../../../../../docs/adr/0020-acl-nfsv4-style-aces-with-inheritance.md), [reference 05 §0x0A](../../../../../../docs/reference/05-protocol-tlvs.md)) — the recursion is deliberate: the outer ACL is the ACE collection, each inner ACL is one ACE with NAME-tagged fields.

- **ACE 1**: `type=ALLOW(0)`, `flags=INHERIT(0x1)`, `subject="peer-a"`, `access_mask=0x0003` (`READ|WRITE`) — an inheriting grant covering the subtree.
- **ACE 2**: `type=ALLOW(0)`, `flags=0`, `subject="EVERYONE@"` (special subject), `access_mask=0x0001` (`READ`), `expires_ns=0x0102030405060708` — a time-bounded everyone-read on this vertex only.

Core-subset-conformant (#81): ALLOW-only, single `INHERIT` flag — a reference node accepts this `:acl` write and enforces it.
