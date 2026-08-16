# fwd/fwd-label-terminus-deref

The labelled request a **terminus dereferences to one of its own vertices** ([RFC-0027](../../../../../docs/spec/rfcs/0027-label-switched-path-compression.md) §7.2). The other end of [`fwd/fwd-label-terminus-reply`](../fwd-label-terminus-reply/description.md): those bytes are the label a terminus **minted** for the residual it resolved, and these are the same label **presented back**.

```
0F 40 1D 00                       FWD, opt=0x40 (PL=1), length=29
   01 00 01 00 00                 VALUE op = READ
   06 00 07 00                     PATH dst, length=7        ← the whole address
      00 16 04 00 00 01 00        element 0: PATH LABEL, index=0, generation=1
   06 00 09 00 …                  PATH src = /reply-ep      (PL=0, packed records)
```

**33 bytes, against the 38 the string spelling costs** for the same operation on the same two-segment residual. That difference is the whole of what §12.4 axis 2 measures — the axis §3.3 nominates as *"the one that decides"*, because the byte column of §3.3's own table ties `PATH_REF`.

## There is no name beside the label, and that is not an omission

A label **replaces** the bytes of the part it stands for (§6.1). The `dst` here is one 7-byte element and nothing else, so a host that cannot validate it has nothing to fall back to: it answers `tr::path::not_found` and delivers nothing ([`fwd/fwd-label-terminus-stale`](../fwd-label-terminus-stale/description.md)), with no re-resolution, no nearest match and no fall-through to a canonical walk. Falling through would mean reading a peer-supplied slot index as UTF-8, which is exactly the guessing §7.2 forbids.

## A hop and a terminus are told apart by the TABLE, not by these bytes

A label aliases the minting host's own reference to something — the identical `(index, generation)` pair RFC-0024's bound spelling carries. If that reference names one of the host's **connection vertices**, the frame is forwarded over the link it names ([`fwd/fwd-label-stale`](../fwd-label-stale/description.md)'s live case). If it names an **ordinary vertex**, the host is the terminus and applies the operation there. Nothing in the frame distinguishes the two, and nothing should: a label stands for a resolution the minting host already made.

## What a labelled operation is still subject to

Everything the string spelling is subject to. §8.2 requires the ACL to be evaluated at the dereferenced vertex, *for that operation's own right, exactly as the string form does* — **a generation match authorizes nothing**. The reference implementation satisfies that by reuse rather than by restatement: the deref hands a vertex to the same resolver the canonical address reaches, and the gate is `graph_t::read` / `write` / `await`'s own `acl_allows`, in the same place, with the same subject. A revoked right therefore takes effect on the very next operation over an already-minted label; there is no snapshot to go stale.

At a terminus a denial is spelled `tr::access::denied`, not `tr::path::not_found` — and that is the same anti-enumeration rule a hop's `not_found` serves (§8.1: a labelled probe yields what a string probe yields), read against what the string probe yields *here*.

## No PATH_REF rides the answer to this

§11.2's second clause: a host SHOULD NOT bind a `PATH_REF` over a path whose elements are already labelled. So an origin that sets `op` bit 7 (RFC-0024's mint request) on a labelled `dst` is answered with the ordinary reply and no `PATH_REF` — one address, one compression. The operation itself is unaffected, which is what makes the refusal free (§6.3).

## What this vector gates

The codec, and only the codec — the harness routes nothing ([HARNESS.md](../../../../HARNESS.md) §"The execution model has ONE forwarder"). That a terminus actually *dereferences* these bytes to the vertex the label aliases, that the answer is byte-identical to the string spelling's, that a denied labelled operation reaches the identical ACL verdict, and that no second label or `PATH_REF` is minted onto the reply are **behavioural** claims, bound by `core/tests/path_label_terminus_test.cpp` against the production `fwd_router_t` + `op_resolver_t` wiring.
