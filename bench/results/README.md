# Raw bench samples

Machine-produced `SAMPLE` lines and summaries kept beside the harness that made them, so a
claim in a PR body or an ADR can be re-derived rather than believed (the derive-don't-assert
rule). Each file names the harness, the arms and the machine axis it swept.

| file | harness | what |
| --- | --- | --- |
| `seam_guard_ab_2026-08-01_*` | `bench_seam_guard` via `run_seam_ab.sh` | ADR-0072 §4's gate, lever by lever: `main` / the first domain implementation / minus the announce counter / minus the asymmetric barrier / final. Interleaved, median of 15, `taskset -c 2`, Release `-O2`. |
| `seam_guard_main_vs_final_cpu2_samples.txt` | same | `main` vs final only, 21 reps, core 2 — the tighter two-arm estimate. |
| `seam_guard_main_vs_final_cpu6_samples.txt` | same | the same pair on core 6, to show the residual is not one core's artifact. |
| `seam_guard_erratum2_main_vs_final_cpu2_*` | same | the ADR-0072 **erratum 2** head vs `main`, 21 reps, core 2, interleaved. The headline two-arm estimate: the `plain` control moved −0.22 ns, so the read/write deltas are the change and not the machine. |
| `seam_guard_erratum2_announce_bisect_cpu2_*` | same | **where the residual goes.** Three arms — `main`, this head, and this head with the two `guard.protect()` calls replaced by a plain `slot->load(acquire)` (`noannounce`, an ablation tree, so it differs from `final` in ONE line each on the read and write paths). The control is flat across all three (13.39 / 13.39 / 13.16), and the ablation lands on `main` (+0.13 read, −0.40 write), which is what closes the "find the last 1.5 ns on the write leg" bisect: there is nothing else to find, the residual IS the announce pair. |
| `reclaim_domain_bss_census.txt` | `size` / `riscv32-esp-elf-size` on a probe binary and on the real TUs | the ADR-0072 erratum 2 RAM half: host and rv32 `.bss` for `main`, the erratum-1 head and this one at both knob settings — plus the starvation-bound ablation (peak live blocks and µs/retire, 65 long-lived readers vs 20 000 retires) against each library. |
| `mem_reg_escape_after.txt` | `bench_forward_heap` | the perf gate's `mem:reg_escape` row after the intrusive retire record shrank to one word — 352 B/vertex, i.e. back on `main`'s number. The three-word form read 368 B and failed the gate. |

**Reading the 5-arm lever run with the right uncertainty.** In `seam_guard_ab_2026-08-01_*`
the `plain` control spans 12.37…13.68 ns across the five arms — a 1.31 ns drift, which is
LARGER than the residual those arms were used to attribute. So per-lever deltas from that run
carry roughly ±1.3 ns of arm-to-arm uncertainty, and only the levers whose effect is several
times that (the gate counter, the asymmetric barrier) are safely attributed from it; the
per-thread-claim row is not. The two-arm and bisect runs above were re-run for exactly this
reason and their controls ARE flat, which is why the headline claim rests on them.

These are a snapshot of one machine (`AMD Ryzen AI 9 HX PRO 375`, Linux, `membarrier`
`PRIVATE_EXPEDITED` available). They are evidence for a decision, not a perf-history series
— the long-running series lives on `gh-pages` and is machine-maintained.
