// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
/*!
 * @brief Rust codec perf bench — the `lang` axis (Rust core) of the ranged cross-core
 * perf matrix (ADR-0032, issue #96). It measures decode+encode roundtrip latency
 * and throughput over the SHARED conformance vectors under
 * tests/conformance/vectors/v1/ (the "vector data" workload — real structured
 * TLVs, not synthetic scalars), so a C++-vs-TS-vs-Rust surface can be rendered
 * over the SAME inputs.
 *
 * Output: one machine-parseable RESULT line per vector, in the SAME
 * tab-separated column contract as the C++ bench (bench/bench_common.hpp) and the
 * TS bench (bindings/typescript/packages/core/bench/perf.mjs):
 *
 *   RESULT \t system \t mode \t size \t fanout \t endpoints \t pub_s \t
 *          deliv_s \t mb_s \t p50ns \t p99ns \t meanns
 *
 * with system="rust-core", mode="codec", fanout=1, endpoints=1. pub_s and deliv_s
 * are roundtrips/sec (one roundtrip == one decode + one encode); mb_s is
 * bytes/sec/1e6; p50/p99/mean are per-roundtrip nanoseconds. A human-readable
 * per-vector ns/op footer follows on stderr for readability.
 *
 * Run headlessly with the release profile (representative numbers):
 *   cargo run --release --example perf -- tests/conformance/vectors/v1
 * The vectors dir is taken from argv, else defaults relative to CARGO_MANIFEST_DIR
 * (mirrors examples/conformance.rs).
 */

use std::hint::black_box;
use std::path::{Path, PathBuf};
use std::time::Instant;

use libtracer::{decode, encode};

/** @brief Target per-vector throughput-phase wall time, so each ns/op is stable. */
const TARGET_NS: u128 = 100_000_000; // 100 ms throughput phase
const LATENCY_SAMPLES: usize = 20_000; // individual timed roundtrips for percentiles
const WARMUP: usize = 5_000;
const PROBE_N: usize = 2_000;

/**
 * @brief One decode+encode roundtrip on the given bytes (the unit of work). The result
 * is fed through `black_box` so the optimizer cannot elide the work.
 */
fn roundtrip(input: &[u8]) {
    let tlv = decode(input).expect("conformance vector must decode");
    black_box(encode(black_box(&tlv)));
}

struct Measure {
    pub_s: f64,
    mb_s: f64,
    p50: u64,
    p99: u64,
    mean: u64,
}

/**
 * @brief Measure a single vector: a tight throughput loop (>= TARGET_NS) for the rate,
 * then a batch of individually-timed roundtrips for the latency percentiles.
 */
fn measure(input: &[u8]) -> Measure {
    // Warm up branch predictors / caches before any timing.
    for _ in 0..WARMUP {
        roundtrip(input);
    }

    // Calibrate iteration count from a short probe so the timed loop ~= TARGET_NS.
    let t0 = Instant::now();
    for _ in 0..PROBE_N {
        roundtrip(input);
    }
    let probe = t0.elapsed().as_nanos().max(1);
    let per_op = (probe / PROBE_N as u128).max(1);
    let mut iters = (TARGET_NS / per_op) as usize;
    if iters < PROBE_N {
        iters = PROBE_N;
    }
    if iters > 50_000_000 {
        iters = 50_000_000;
    }

    // Throughput phase: one tight loop, measure total wall time.
    let t0 = Instant::now();
    for _ in 0..iters {
        roundtrip(input);
    }
    let secs = t0.elapsed().as_nanos() as f64 / 1e9;
    let pub_s = iters as f64 / secs;
    let mb_s = (iters as f64 * input.len() as f64) / secs / 1e6;

    // Latency phase: time individual roundtrips for the percentiles.
    let mut samples = vec![0u64; LATENCY_SAMPLES];
    for s in samples.iter_mut() {
        let start = Instant::now();
        roundtrip(input);
        *s = start.elapsed().as_nanos() as u64;
    }
    samples.sort_unstable();
    let at = |p: f64| -> u64 {
        let idx = ((p * samples.len() as f64) as usize).min(samples.len() - 1);
        samples[idx]
    };
    let sum: u128 = samples.iter().map(|&v| v as u128).sum();
    let mean = (sum / samples.len() as u128) as u64;

    Measure {
        pub_s,
        mb_s,
        p50: at(0.5),
        p99: at(0.99),
        mean,
    }
}

/** @brief Recursively collect (relative-posix-path, absolute-path) for every input.bin. */
fn find_inputs(root: &Path, dir: &Path, out: &mut Vec<(String, PathBuf)>) {
    let mut entries: Vec<PathBuf> = match std::fs::read_dir(dir) {
        Ok(rd) => rd.filter_map(|e| e.ok().map(|e| e.path())).collect(),
        Err(_) => return,
    };
    entries.sort();
    for p in entries {
        if p.is_dir() {
            find_inputs(root, &p, out);
        } else if p.file_name().map(|n| n == "input.bin").unwrap_or(false) {
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

/** @brief Default vectors dir relative to CARGO_MANIFEST_DIR (mirrors conformance.rs). */
fn default_vectors_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/conformance/vectors/v1")
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let vectors_dir = args
        .iter()
        .find(|a| !a.starts_with("--"))
        .map(PathBuf::from)
        .unwrap_or_else(default_vectors_dir);

    // Sort by POSIX relative path so the row order matches the C++/TS harnesses.
    let mut cases: Vec<(String, PathBuf)> = Vec::new();
    find_inputs(&vectors_dir, &vectors_dir, &mut cases);
    cases.sort_by(|a, b| a.0.cmp(&b.0));

    if cases.is_empty() {
        eprintln!("no input.bin vectors found under {}", vectors_dir.display());
        std::process::exit(2);
    }

    let mut rows: Vec<(String, usize, Measure)> = Vec::with_capacity(cases.len());
    for (rel, abs) in &cases {
        let input = std::fs::read(abs).expect("read input.bin");
        let m = measure(&input);
        // RESULT line — identical column contract to bench_common.hpp emit() /
        // perf.mjs: system="rust-core", mode="codec", fanout=1, endpoints=1.
        // pub_s == deliv_s (roundtrips/sec); mb_s = bytes/sec/1e6.
        println!(
            "RESULT\trust-core\tcodec\t{}\t1\t1\t{:.0}\t{:.0}\t{:.1}\t{}\t{}\t{}",
            input.len(),
            m.pub_s,
            m.pub_s,
            m.mb_s,
            m.p50,
            m.p99,
            m.mean,
        );
        rows.push((rel.clone(), input.len(), m));
    }

    // Human-readable footer (per-vector ns/op), to stderr so RESULT stdout stays
    // clean for collate.py-style tooling.
    eprintln!(
        "\n# rust-core codec bench — {} vectors ({})",
        rows.len(),
        vectors_dir.display()
    );
    eprintln!(
        "# {:<34}{:>7}{:>12}{:>9}{:>9}{:>10}",
        "vector", "size", "ns/op", "p50", "p99", "M op/s"
    );
    for (rel, size, m) in &rows {
        eprintln!(
            "# {:<34}{:>7}{:>12}{:>9}{:>9}{:>10}",
            rel,
            format!("{}B", size),
            m.mean,
            m.p50,
            m.p99,
            format!("{:.2}", m.pub_s / 1e6),
        );
    }
}
