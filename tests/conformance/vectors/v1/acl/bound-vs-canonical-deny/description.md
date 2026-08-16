# acl/bound-vs-canonical-deny

The **deny** half of RFC-0024 §6.3's mandated pair: what
[`acl/bound-vs-canonical-allow`](../bound-vs-canonical-allow/description.md)'s frame answers
when the caller is not allowed.

```
0F 40 31 00                       FWD, opt=0x40 (PL=1), length=49
   01 00 01 00 03                 VALUE op = REPLY
   06 00 09 00 …                  PATH dst = /reply-ep      (PL=0, packed records)
   14 00 08 00                    PATH_REF src                ← the request's dst, echoed
      02 00 00 00 00 00 00 00     element 0: index=2, generation=0
   01 00 01 00 01                 VALUE kind = ERROR
   09 40 0A 00                    STATUS
      08 40 06 00                 ERROR
         01 00 02 00 50 00        VALUE u16 = 0x0050  tr::access::denied
```

## What must agree, and the one thing that cannot

The **outcome** is byte-identical between the two spellings:

```
01 00 01 00 01  09 40 0A 00  08 40 06 00  01 00 02 00 50 00
kind = ERROR    STATUS{ ERROR{ VALUE u16 = 0x0050 } }
```

The canonical spelling answers exactly those bytes. The two whole frames differ in one
place and only one: the reply's `src` echoes the **request's `dst`**, and the request's
`dst` is precisely the thing the two spellings disagree about. A `PATH_REF` there is
uninformative to the origin — an element minted by *this* node means nothing on the origin
— but it is what the request named, and the reply's job is to say what was asked.

## The two properties this depicts

**A generation match authorizes nothing.** The element in `src` dereferenced cleanly: index
2 is in range and generation 0 matches. The operation was still denied, because
authorization is evaluated per operation at the dereferenced vertex, exactly as the
canonical form evaluates it at the resolved one (RFC-0024 §6.2). A bound path is an
**address, never a capability** — it reaches nothing its holder could not reach by name.

**A denied operation mints nothing.** There is no `PATH_REF` after the `STATUS`, and there
would be none even had the request set the mint flag. Denial happens before any vref is
produced, so probing the bound form yields *exists + denied* and never *exists + here is a
handle to it* (RFC-0024 §6.1). A bound path cannot be used to discover a namespace its
holder cannot already walk.

## What this vector gates

The codec, and only the codec — including that a `PATH_REF` decodes in `src` position, not
only in `dst`. The two behavioural claims above are bound by
`core/tests/bound_path_test.cpp` — `test_mint_denied_by_acl` (which also feeds a granted
caller's binding to a denied one) and `test_revocation_is_immediate`.
