#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Render the Tracer Internet-Draft and check it, failing on any problem.
#
#   kramdown-rfc  Markdown -> RFCXML v3
#   xml2rfc       RFCXML   -> text + HTML
#   idnits        text     -> nits report
#
# Fails on any kramdown-rfc or xml2rfc warning or error, and on any idnits
# error (**), flaw (~~) or warning (==). One idnits warning is expected and
# allowed: "Couldn't figure out when the document was first submitted". idnits
# looks the draft name up in the Datatracker, and a draft that has never been
# posted has no record there. The warning goes away once -00 is posted.
#
# Usage: build.sh [out-dir]   (default: _build next to this script)
# Needs on PATH: kramdown-rfc, xml2rfc, idnits (v2).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
src="$here/draft-avatarsd-tracer.md"
out="${1:-$here/_build}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
docname="$(sed -n 's/^docname: *//p' "$src")"
xml="$out/$docname.xml"
txt="$out/$docname.txt"

# kramdown-rfc caches fetched references; keep the cache out of the source tree.
export KRAMDOWN_REFCACHEDIR="${KRAMDOWN_REFCACHEDIR:-$out/refcache}"
mkdir -p "$KRAMDOWN_REFCACHEDIR"

fail=0

echo "== kramdown-rfc"
kramdown-rfc "$src" >"$xml" 2>"$out/kramdown-rfc.log" || fail=1
cat "$out/kramdown-rfc.log"
# Reference fetches are progress messages, not diagnostics.
if grep -v -e ': fetching from ' "$out/kramdown-rfc.log" | grep -q .; then fail=1; fi
[ "$fail" -eq 0 ] || { echo "kramdown-rfc reported problems" >&2; exit 1; }

echo "== xml2rfc"
xml2rfc --text --html --path "$out" "$xml" >"$out/xml2rfc.log" 2>&1 || fail=1
cat "$out/xml2rfc.log"
if grep -q -i -e 'warning' -e 'error' "$out/xml2rfc.log"; then fail=1; fi
[ "$fail" -eq 0 ] || { echo "xml2rfc reported problems" >&2; exit 1; }

echo "== idnits"
idnits --verbose "$txt" >"$out/idnits.log" 2>&1 || fail=1
cat "$out/idnits.log"
grep -q 'Summary:' "$out/idnits.log" || { echo "idnits produced no summary" >&2; exit 1; }
# Each finding starts with its marker; the allowed warning is dropped by text.
if grep -E '^ *(\*\*|~~|==) ' "$out/idnits.log" \
	| grep -v "Couldn't figure out when the document was first submitted" | grep -q .; then
	echo "idnits reported errors, flaws or warnings" >&2
	exit 1
fi
[ "$fail" -eq 0 ] || { echo "idnits failed" >&2; exit 1; }

echo "OK: $xml, $txt and the HTML rendering are in $out"
