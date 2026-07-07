// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// Mermaid syntax gate for the docs.
//
// sphinxcontrib-mermaid embeds every ```mermaid / ```{mermaid} block into the page
// verbatim and lets mermaid.js render it in the READER'S BROWSER. The Sphinx build
// never parses the diagram, so a diagram with a syntax error still builds and deploys
// green — it only fails when someone opens the page, showing an "Syntax error in text"
// box instead of the diagram. This script closes that gap: it extracts every mermaid
// block under docs/ and parses each with the SAME mermaid version the site loads
// (pinned in package.json to conf.py's mermaid_version, 11.12.1), failing CI on any
// block the browser would reject.
//
// Run from anywhere: `node tools/mermaid-lint/check-mermaid.mjs` (paths resolve against
// the repo root, computed from this file's location). Exit 0 = all clean, 1 = at least
// one block failed to parse.

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join, resolve, relative } from 'node:path';
import { createRequire } from 'node:module';
import { JSDOM } from 'jsdom';

// mermaid.parse() runs its bundled DOMPurify, which only wires up its real API (addHook,
// sanitize) when a DOM is present. Under bare node it degrades to a stub that throws
// "DOMPurify.addHook is not a function" on any diagram with an HTML label (`<br/>`), which
// would look like a syntax error but is not. A jsdom window makes the sanitizer real, so
// parse reports only genuine grammar errors. jsdom is imported statically (it needs no
// DOM); mermaid is imported dynamically BELOW so these globals exist before it loads.
const dom = new JSDOM('<!DOCTYPE html><body></body>');
globalThis.window = dom.window;
globalThis.document = dom.window.document;

const require = createRequire(import.meta.url);
const mermaidVersion = require('mermaid/package.json').version;
const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');

/** Recursively collect *.md files under `dir`. */
function markdownFilesUnder(dir) {
    const out = [];
    for (const entry of readdirSync(dir)) {
        const p = join(dir, entry);
        const st = statSync(p);
        if (st.isDirectory()) out.push(...markdownFilesUnder(p));
        else if (entry.endsWith('.md')) out.push(p);
    }
    return out;
}

// Every doc that can carry a diagram: all Markdown under docs/, plus top-level *.md.
const files = [
    ...markdownFilesUnder(join(repoRoot, 'docs')),
    ...readdirSync(repoRoot)
        .filter((e) => e.endsWith('.md'))
        .map((e) => join(repoRoot, e)),
].sort();

// A fence opens a mermaid block if it is `\`\`\`mermaid` or `\`\`\`{mermaid}` (MyST
// directive form), tolerating leading indent, 3+ fence chars (` or ~), and trailing
// directive args. NOTE: no `\b` after `{mermaid}` — `}` is not a word boundary, so the
// obvious `mermaid\b` regex silently skips every directive-form block.
const openRe = /^([ \t]*)([`~]{3,})\s*(\{mermaid\}|mermaid)(\s.*)?$/i;
// A MyST directive option line inside the block, e.g. `:caption: ...` — not diagram text.
const optionRe = /^[ \t]*:[\w][\w-]*:/;

/** Extract every mermaid block as {file, line, body}. */
function extractBlocks(file) {
    const lines = readFileSync(file, 'utf8').split('\n');
    const blocks = [];
    for (let i = 0; i < lines.length; i++) {
        const m = openRe.exec(lines[i]);
        if (!m) continue;
        const fenceChar = m[2][0];
        const closeRe = new RegExp(`^[ \\t]*\\${fenceChar}{3,}\\s*$`);
        const start = i;
        const body = [];
        for (i++; i < lines.length && !closeRe.test(lines[i]); i++) body.push(lines[i]);
        // Drop leading blank / directive-option lines so the graph header is line 1.
        let j = 0;
        while (j < body.length && (body[j].trim() === '' || optionRe.test(body[j]))) j++;
        blocks.push({ file, line: start + 1, body: body.slice(j).join('\n').trim() });
    }
    return blocks;
}

const mermaid = (await import('mermaid')).default;
mermaid.initialize({ startOnLoad: false });

let total = 0;
const failures = [];
for (const file of files) {
    for (const block of extractBlocks(file)) {
        total++;
        const where = `${relative(repoRoot, block.file)}:${block.line}`;
        try {
            await mermaid.parse(block.body);
        } catch (err) {
            const msg = (err && err.message ? err.message : String(err))
                .split('\n')
                .slice(0, 3)
                .join('\n      ');
            failures.push({ where, msg });
        }
    }
}

if (failures.length === 0) {
    console.log(`mermaid-lint: all ${total} diagram(s) parse clean (mermaid ${mermaidVersion}).`);
    process.exit(0);
}

console.error(`mermaid-lint: ${failures.length} of ${total} diagram(s) FAILED to parse:\n`);
for (const f of failures) console.error(`  ✗ ${f.where}\n      ${f.msg}\n`);
console.error('These render as a "Syntax error in text" box on the docs site. Fix the source above.');
process.exit(1);
