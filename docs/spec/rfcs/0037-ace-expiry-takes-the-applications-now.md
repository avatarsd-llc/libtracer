<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0037 — ACE expiry is judged against the application's `now`

<!-- status: accepted -->

| Field | Value |
| ---- | ---- |
| **RFC** | 0037 |
| **Title** | ACE expiry is judged against the application's `now` |
| **Status** | **accepted** (2026-10-25). The maintainer ruled the direction on 2026-10-24 (Q2: "ACE expiry takes the app's `now` through the subject-lookup hook") and approved it as an amendment. Comment window waived by default and not invoked. |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-10-25 |
| **Comment window** | Waived by default while the project is solo-maintained ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"). Invoke it explicitly if outside input is wanted. `docs/implementations.md` lists no registered implementation, so the waiver's revert trigger has not fired. |
| **Instrument** | **Amendment.** [reference/05](../../reference/05-protocol-tlvs.md) §`0x0A` is a normative annex. It says an operation is allowed iff "some non-expired ACE" grants it, and it names no reference clock; the shipped behaviour read the node's system clock. This RFC names the reference clock and pins what a leased ACE does when there is none. A peer can observe both, because a lease is honoured or refused at a different moment, so this is outside the erratum instrument. No wire byte changes. |
| **Tracking issue** | [#2308](https://github.com/avatarsd-llc/libtracer/issues/2308) |
| **Target spec version** | v1 itself. `docs/spec/v1.md` still reads "(DRAFT)". |
| **Amends** | reference/05 §`0x0A` (the `expires_ns` field and the enforcement paragraph); the [ADR-0050](../../adr/0050-acl-pure-policy-cached-effective-ace-merge.md) §2 note that `expires_ns` "is evaluated by the policy against `now`". |

---

## 1. Summary

An ACE may carry `expires_ns`, an absolute instant after which it no longer applies. Until now
the reference node compared it against its own system clock, read inside the ACL gate. That
breaks the standing rule that the library reads no clock, and it leaves the spec silent on which
clock a lease is judged by.

This RFC makes the reference clock the **application's**:

1. `expires_ns` is nanoseconds on the clock of the node's application. The epoch is the
   application's choice and must match the clock it writes `expires_ns` against.
2. At check time the node compares it against the `now` the application supplies. An ACE is
   non-expired iff `expires_ns` is absent or `0`, or `now < expires_ns`.
3. **No `now`, no lease (fail closed).** A node whose application supplies no `now` cannot
   prove a lease live or lapsed. An ALLOW ACE with a non-zero `expires_ns` then grants nothing,
   and a DENY ACE with one still refuses. A standing ACE (`expires_ns` absent or `0`) is
   unaffected.
4. The node reads no clock to judge expiry.

## 2. Motivation

- **The library reads no clock.** A deadline is driven by the application through a seam that
  takes `now` from the application's own clock (the standing design rule). The ACL gate was one
  of the remaining clock reads; [#1665](https://github.com/avatarsd-llc/libtracer/issues/1665)
  and [#1685](https://github.com/avatarsd-llc/libtracer/issues/1685) had already narrowed it to
  the leased case.
- **A node without a wall clock.** An MCU often has no calibrated wall clock at boot, so the
  system clock reads the epoch, or something close to it. Every lease then looked live for
  decades. Only the application knows whether its clock is trustworthy yet.
- **One clock per deployment.** The owner writes `:acl`, so the owner already chooses the clock
  `expires_ns` is written against. Judging it against the same clock is the only reading under
  which a lease means what its writer meant.

## 3. Specification

### 3.1 reference/05 §`0x0A`

The ACE layout line for `expires_ns` gains its unit:

```
    NAME "expires_ns"   VALUE <u64>          ; optional; ns on the application's clock, 0 = never
```

and a paragraph follows the enforcement paragraph:

> **Expiry is judged against the application's `now`** (RFC-0037). An ACE's `expires_ns` is an
> absolute instant in nanoseconds on the clock of the node's application; the epoch is the
> application's choice and MUST match the clock that writes `expires_ns`. At check time the node
> compares it against the `now` its application supplies: the ACE is non-expired iff
> `expires_ns` is absent or `0`, or `now < expires_ns`. A node whose application supplies no
> `now` MUST fail every lease closed: an ALLOW ACE with a non-zero `expires_ns` grants nothing,
> and a DENY ACE with one still refuses. An ACE with `expires_ns` absent or `0` is unaffected. An
> expired ACE still closes an otherwise-open vertex, as before. No wire byte changes.

### 3.2 Why a leased DENY still refuses without a `now`

Under the full policy (ordered, first match per bit) a list may put a leased DENY in front of a
standing ALLOW for the same subject. Skipping the DENY for want of a clock would grant what a
live clock refuses. So without a `now` a lease is treated as unprovable in the direction that
refuses: an ALLOW does not grant and a DENY still applies. The ALLOW-only MCU profile stores no
DENY, so there it reduces to "a leased grant gives nothing".

### 3.3 What does not change

- The wire. `expires_ns` keeps its u64 layout, its optionality and its strict parsing (an empty
  or over-wide payload is still refused, so an absent value never reads as "never expires").
- Expiry is `expires_ns <= now`, not `<`: at the deadline the grant is already gone.
- Only the merge is cached, never a verdict (ADR-0050), so expiry still needs no invalidation.

## 4. Reference implementation

- `tr::graph::subject_lookup_fn_t` gains a last parameter, `std::uint64_t& now_ns`. The gate
  sets it to `tr::graph::kAceNowUnknown` before the call, and the application overwrites it with
  its `now`. The ADR-0047 `{fn, ctx}` shape stays, and nothing is allocated.
- `graph_t::acl_allows` uses that value only when the effective ACL it evaluates holds a lease.
  `now_ns()` and its `system_clock::now()` are deleted from `graph.cpp`.
- `detail_acl::ace_applies` implements §3.2: at `kAceNowUnknown` a lease reads as lapsed, except
  that a leased DENY still applies.
- The public API break is recorded in `core/CHANGELOG.md`.

Pinned by `core/tests/acl_clock_read_test.cpp`. A fake application clock drives a lease through
the real hook. The test also covers the no-`now` rule on the graph and on the full policy, and
it counts zero `system_clock::now()` calls across every gate.

## 5. Conformance

No conformance vector changes. The `acl/acl-aces` vector pins the ACE wire shape, which is
unchanged, and no vector evaluates a lease against a clock.
