<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

Copy this file to docs/spec/rfcs/NNNN-short-title.md (NNNN = the RFC number,
sequential and zero-padded; record the tracking issue in the table below).
See GOVERNANCE.md §"Spec changes" for the process:
open an `rfc`-labelled issue first, then PR the document; it stays open ≥14 days.
-->

# RFC NNNN — {Short title}

| Field | Value |
| ---- | ---- |
| **RFC** | NNNN |
| **Title** | {short title} |
| **Status** | draft \| in-comment \| accepted \| rejected \| superseded |
| **Author(s)** | {name} |
| **Created** | YYYY-MM-DD |
| **Comment window closes** | YYYY-MM-DD (≥ 14 days after opening) |
| **Tracking issue** | #NNNN |
| **Target spec version** | v1 (or v2 if backwards-incompatible) |

## Summary

One paragraph: what is proposed and why.

## Motivation

What problem does this solve? What is currently impossible, ambiguous, or contradictory? Cite the specific docs/sections (and any prior analysis or ADRs).

## Proposed change

The concrete change. For wire-format changes, be byte-precise. For normative changes, give the exact MUST/SHOULD wording. List every file the accepted RFC will edit.

## Compatibility

- Does this break protocol-v1 implementations? (If yes, it requires v2 per GOVERNANCE.)
- Does it require new or changed conformance vectors? (List them.)
- How should existing implementations / deployed devices migrate?

## Alternatives considered

What was rejected, and why. (Reference ADRs where a decision is already recorded.)

## Discussion

Per [GOVERNANCE.md](../../../GOVERNANCE.md), the tracking issue stays open at least 14 days for implementer feedback before this document is merged. Record sustained objections and their resolution here.
