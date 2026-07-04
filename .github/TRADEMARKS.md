# Trademark Policy

The name **libtracer** and any associated logos are trademarks of **avatarsd LLC**. The Apache 2.0 license covers the source code; **it does not grant trademark rights.**

This policy exists to keep the name meaningful: when a user sees "libtracer-compatible" on a product, it should mean something verifiable.

## What you may do without permission

- **Refer to the project by name** in documentation, blog posts, talks, comparisons, academic work, and similar uses.
- **State that your software uses libtracer** if it does (e.g., "built with libtracer", "uses the libtracer reference implementation").
- **State that your implementation is libtracer-compatible** if it passes the conformance vectors in [tests/conformance/](../tests/conformance/) for a specific spec version. Be specific about which version: "libtracer v1 compatible" is good; "libtracer compatible" without a version is ambiguous.
- **Distribute unmodified copies** of the libtracer reference implementation under its existing name.
- **Distribute modified versions** of the reference implementation under your own name. You may say "based on libtracer" or "fork of libtracer"; do not present a modified version as the canonical libtracer.

## What requires permission

- Using "libtracer" as part of a **product name, company name, or domain name** (e.g., "libtracer-cloud.com", "LibtracerPro"). Likely to confuse users about the source — ask first.
- Using libtracer **logos** in marketing material for a commercial product or service.
- Issuing **certifications** that imply official libtracer endorsement.

## Compatibility badges

Implementations and products that pass the conformance suite may use the badge:

> **libtracer v1 compatible**

The badge is informational, not a certification. The maintainers do not currently run a paid certification program. If you want to advertise compatibility:

1. Run the conformance vectors and pass them.
2. State which spec version you target.
3. If asked to demonstrate, point to your CI output or test results.

False claims of compatibility (badge displayed without passing vectors) are a misuse of the trademark.

## Contact

For trademark questions, open an issue tagged `trademark` or contact avatarsd LLC via the maintainers listed in [GOVERNANCE.md §Roles](GOVERNANCE.md).

## Inspiration

This policy is loosely modeled on the [Mozilla](https://www.mozilla.org/en-US/foundation/trademarks/policy/), [Rust Foundation](https://foundation.rust-lang.org/policies/logo-policy-and-media-guide/), and [Linux Foundation](https://www.linuxfoundation.org/legal/trademarks) trademark policies.
