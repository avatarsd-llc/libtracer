#!/usr/bin/env bash
# Model-check the shipped QSBR header with GenMC (#2120): core/tests/genmc/qsbr_mc.cpp under
# each fence flavour's memory models. Exits non-zero on any violation.
#
# usage: tools/genmc_qsbr.sh            (GENMC=<path to genmc>, default `genmc` on PATH)
#
# FENCED is checked under RC11 and IMM. COMPILER (kSingleCore) is checked under SC only: its
# correctness argument is one hart's, whose preempted executions are SC interleavings.
# MEMBARRIER is not covered: GenMC does not model the syscall's IPI.
set -euo pipefail
GENMC=${GENMC:-genmc}
root=$(cd "$(dirname "$0")/.." && pwd)
src=$root/core/tests/genmc/qsbr_mc.cpp
scenarios=("SCENARIO_HORIZON BRACKETS=1" "SCENARIO_HORIZON BRACKETS=2" "SCENARIO_REUSE BRACKETS=1"
           "SCENARIO_RETIRE BRACKETS=1" "SCENARIO_RETIRE BRACKETS=2")
fail=0
run() {  # preset model scenario brackets
    local out
    out=$("$GENMC" --disable-estimation "--$2" -- -std=c++20 "-I$root/core/tests/genmc/$1" \
        "-I$root/core/include" "-D$3" "-D$4" "$src" 2>&1) || true
    local n
    n=$(grep -m1 'complete executions' <<<"$out" | grep -o '[0-9]*$' || true)
    if grep -q 'No errors were detected' <<<"$out"; then
        printf '%-8s %-5s %-17s %-10s OK (%s executions)\n' "$1" "$2" "$3" "$4" "${n:-?}"
    else
        printf '%-8s %-5s %-17s %-10s VIOLATION\n' "$1" "$2" "$3" "$4"
        grep -m1 -E '^Error' <<<"$out" || true
        printf '%s\n' "$out" | tail -n 40
        fail=1
    fi
}
for s in "${scenarios[@]}"; do
    read -r sc br <<<"$s"
    run fenced rc11 "$sc" "$br"
    run fenced imm "$sc" "$br"
    run compiler sc "$sc" "$br"
done
exit $fail
