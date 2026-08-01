# Raw bench samples

Machine-produced `SAMPLE` lines and summaries kept beside the harness that made them, so a
claim in a PR body or an ADR can be re-derived rather than believed (the derive-don't-assert
rule). Each file names the harness, the arms and the machine axis it swept.

| file | harness | what |
| --- | --- | --- |
| `seam_guard_ab_2026-08-01_*` | `bench_seam_guard` via `run_seam_ab.sh` | ADR-0072 §4's gate, lever by lever: `main` / the first domain implementation / minus the announce counter / minus the asymmetric barrier / final. Interleaved, median of 15, `taskset -c 2`, Release `-O2`. |
| `seam_guard_main_vs_final_cpu2_samples.txt` | same | `main` vs final only, 21 reps, core 2 — the tighter two-arm estimate. |
| `seam_guard_main_vs_final_cpu6_samples.txt` | same | the same pair on core 6, to show the residual is not one core's artifact. |
| `mem_reg_escape_after.txt` | `bench_forward_heap` | the perf gate's `mem:reg_escape` row after the intrusive retire record shrank to one word — 352 B/vertex, i.e. back on `main`'s number. The three-word form read 368 B and failed the gate. |

These are a snapshot of one machine (`AMD Ryzen AI 9 HX PRO 375`, Linux, `membarrier`
`PRIVATE_EXPEDITED` available). They are evidence for a decision, not a perf-history series
— the long-running series lives on `gh-pages` and is machine-maintained.
