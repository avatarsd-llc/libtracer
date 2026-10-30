# Phrases to avoid in the docs

A short, hand-picked list of phrases the docs style lint (`tools/check_docs_style.py`,
rule `avoid-phrase`) reports, each with the glossary term to use instead.
[CONTEXT.md](CONTEXT.md) is the glossary and holds the full _Avoid_ notes behind each one;
this list keeps only the phrases that are wrong wherever they appear, so a hit is worth
reading. A page that discusses a rejected design by name keeps its hit in the baseline.

Matching ignores case and skips code blocks and code spans. One bullet per phrase, in the
form `- "phrase" → use instead`.

- "the libtracer protocol" → Tracer (the protocol); libtracer is the implementation
- "8-byte header" → the 4-byte TLV header (6 with `opt.LL=1`)
- "XOR-16" → CRC-16-CCITT (`opt.CW=1`) or CRC-32C
- "kMaxDepth" → receiver-resource-bounded nesting depth
- "route handle" → path element or chain delivery
- "COMPACT plane" → chain delivery
- "bound path" → path element (a PAIR)
- "vertex ref" → path element (a PAIR)
- "path label" → path element
- "reply correlation-id" → path-as-route (replies retrace the link)
- "grouping vertex" → structural vertex
- "per-field subscription" → field promotion to a `/` child vertex
- "feature negotiation" → no capability negotiation; receivers accept every variant
