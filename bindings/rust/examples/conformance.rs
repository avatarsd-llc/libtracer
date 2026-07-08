// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
/*!
 * @brief Conformance harness for the shared vectors under tests/conformance/vectors/v1/.
 * Mirrors core/tests/conformance_runner.cpp and
 * bindings/typescript/packages/core/conformance/harness.mjs byte-for-byte, so the polyglot
 * driver (tests/conformance/run-all.py) and the differential fuzzer
 * (tests/conformance/diff_fuzz.py) can treat all three cores identically.
 *
 * Two modes:
 *   <vectors-dir> (default / --tap): for every vector directory containing an
 *       input.bin, check encode(decode(input.bin)) == input.bin (byte-for-byte);
 *       for every directory containing a reject.bin (negative case), check that
 *       decode(reject.bin) FAILS with the error named by expected.json's "reject"
 *       field. Emit TAP version 13 to stdout. Exit 0 iff every vector is `ok`.
 *   --roundtrip: read one hex frame per stdin line; for each, print
 *       encode(decode(hex)) as hex, or `ERR:<reason>` on a decode failure. Exactly
 *       one output line per input line. This feeds diff_fuzz.py.
 *
 * Needs std (it does file/stdin I/O); the library crate stays #![no_std].
 */

use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use libtracer::{decode, encode};

/** @brief Decode a hex string to bytes, or `None` on odd length / non-hex digit. */
fn from_hex(s: &str) -> Option<Vec<u8>> {
    if s.len() % 2 != 0 {
        return None;
    }
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(s.len() / 2);
    let nibble = |c: u8| -> Option<u8> {
        match c {
            b'0'..=b'9' => Some(c - b'0'),
            b'a'..=b'f' => Some(c - b'a' + 10),
            b'A'..=b'F' => Some(c - b'A' + 10),
            _ => None,
        }
    };
    let mut i = 0;
    while i < bytes.len() {
        let hi = nibble(bytes[i])?;
        let lo = nibble(bytes[i + 1])?;
        out.push((hi << 4) | lo);
        i += 2;
    }
    Some(out)
}

/** @brief Lowercase hex encoding of `bytes`. */
fn to_hex(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(bytes.len() * 2);
    for &b in bytes {
        s.push(HEX[(b >> 4) as usize] as char);
        s.push(HEX[(b & 0x0f) as usize] as char);
    }
    s
}

/**
 * @brief `--roundtrip`: differential-fuzz batch mode. One re-encoded hex line (or
 * `ERR:<reason>`) per stdin line. Mirrors harness.mjs `runRoundtrip`.
 */
fn run_roundtrip() -> ExitCode {
    let mut text = String::new();
    if std::io::stdin().read_to_string(&mut text).is_err() {
        // Non-UTF-8 stdin is not a valid hex stream; nothing to emit.
        return ExitCode::SUCCESS;
    }
    let mut lines: Vec<&str> = text.split('\n').collect();
    // A trailing newline yields a final empty element — drop only that one.
    if lines.last() == Some(&"") {
        lines.pop();
    }
    let mut out: Vec<String> = Vec::with_capacity(lines.len());
    for raw in lines {
        let line = raw.trim_end_matches(['\r', '\n']);
        if line.is_empty() {
            out.push("ERR:EMPTY_LINE".to_string());
            continue;
        }
        match from_hex(line) {
            None => out.push("ERR:BAD_HEX".to_string()),
            Some(bytes) => match decode(&bytes) {
                Ok(tlv) => out.push(to_hex(&encode(&tlv))),
                Err(e) => out.push(format!("ERR:{}", e.name())),
            },
        }
    }
    print!("{}\n", out.join("\n"));
    ExitCode::SUCCESS
}

/**
 * @brief Extract the expected decode-error name from a case's expected.json —
 * the value of its top-level `"reject"` field (scan-to-quote, mirroring
 * tests/conformance_vectors.rs `json_str`; the field is machine-written ASCII).
 */
fn reject_expectation(case_dir: &Path) -> Option<String> {
    let text = fs::read_to_string(case_dir.join("expected.json")).ok()?;
    let key = text.find("\"reject\"")?;
    let colon = key + text[key..].find(':')?;
    let q1 = colon + 1 + text[colon + 1..].find('"')?;
    let q2 = q1 + 1 + text[q1 + 1..].find('"')?;
    Some(text[q1 + 1..q2].to_string())
}

/**
 * @brief One negative case: decode(reject.bin) MUST fail with exactly the
 * expected error. Returns `(ok, diagnostic)`.
 */
fn check_reject(reject_bin: &Path) -> (bool, String) {
    let Some(want) = reject_bin.parent().and_then(reject_expectation) else {
        return (
            false,
            "reject.bin without a \"reject\" expectation in expected.json".to_string(),
        );
    };
    match fs::read(reject_bin) {
        Err(e) => (false, e.to_string()),
        Ok(bytes) => match decode(&bytes) {
            Ok(_) => (false, format!("decode succeeded, expected {}", want)),
            Err(e) if e.name() == want => (true, String::new()),
            Err(e) => (
                false,
                format!("decode failed with {}, expected {}", e.name(), want),
            ),
        },
    }
}

/**
 * @brief Recursively collect (relative-posix-path, absolute-path) for every
 * input.bin and reject.bin.
 */
fn find_inputs(root: &Path, dir: &Path, out: &mut Vec<(String, PathBuf)>) {
    let mut entries: Vec<PathBuf> = match fs::read_dir(dir) {
        Ok(rd) => rd.filter_map(|e| e.ok().map(|e| e.path())).collect(),
        Err(_) => return,
    };
    entries.sort();
    for p in entries {
        if p.is_dir() {
            find_inputs(root, &p, out);
        } else if p
            .file_name()
            .map(|n| n == "input.bin" || n == "reject.bin")
            .unwrap_or(false)
        {
            let rel = p
                .parent()
                .unwrap()
                .strip_prefix(root)
                .unwrap()
                .components()
                .map(|c| c.as_os_str().to_string_lossy())
                .collect::<Vec<_>>()
                .join("/");
            out.push((rel, p));
        }
    }
}

/** @brief Default / `--tap`: emit TAP version 13 for the round-trip of every vector. */
fn run_tap(vectors_dir: &Path) -> ExitCode {
    let mut cases: Vec<(String, PathBuf)> = Vec::new();
    find_inputs(vectors_dir, vectors_dir, &mut cases);
    // Sort by POSIX relative path so keys match the C++/TS harnesses.
    cases.sort_by(|a, b| a.0.cmp(&b.0));

    let mut lines: Vec<String> = Vec::new();
    lines.push("TAP version 13".to_string());
    lines.push(format!("1..{}", cases.len()));
    let mut n = 0;
    let mut fails = 0;
    for (rel, abs) in &cases {
        n += 1;
        let mut ok = false;
        let mut diag = String::new();
        if abs.file_name().map(|f| f == "reject.bin").unwrap_or(false) {
            (ok, diag) = check_reject(abs);
        } else {
            match fs::read(abs) {
                Ok(input) => match decode(&input) {
                    Ok(tlv) => {
                        ok = encode(&tlv) == input;
                        if !ok {
                            diag = "round-trip differs from input.bin".to_string();
                        }
                    }
                    Err(e) => diag = e.name().to_string(),
                },
                Err(e) => diag = e.to_string(),
            }
        }
        lines.push(format!(
            "{} {} - {}",
            if ok { "ok" } else { "not ok" },
            n,
            rel
        ));
        if !ok {
            lines.push(format!("# {}", diag));
            fails += 1;
        }
    }

    print!("{}\n", lines.join("\n"));
    if fails == 0 {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}

/**
 * @brief Locate the vectors dir from argv; fall back to the repo tree relative to
 * CARGO_MANIFEST_DIR so the example is runnable standalone (`cargo run`).
 */
fn default_vectors_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/conformance/vectors/v1")
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.iter().any(|a| a == "--roundtrip") {
        return run_roundtrip();
    }
    // First non-flag argument is the vectors dir (the driver appends it). `--tap`
    // is accepted and ignored for parity with the C++ harness contract.
    let dir = args
        .iter()
        .find(|a| !a.starts_with("--"))
        .map(PathBuf::from)
        .unwrap_or_else(default_vectors_dir);
    run_tap(&dir)
}
