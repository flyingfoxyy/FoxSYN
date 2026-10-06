#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
FOXSYN_BIN=${FOXSYN_BIN:-"$ROOT/release/FoxSYN"}

ADDER="$ROOT/regression/SimpleCircuits/EPFL/adder.v"
ALU4="$ROOT/regression/SimpleCircuits/mcnc/alu4.v"
C7552="$ROOT/regression/SimpleCircuits/mcnc/C7552.v"
I10_AIG="$ROOT/../agdmap/i10.aig"

TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/foxsyn-phase6-smoke.XXXXXX")
trap 'rm -rf "$TMP_DIR"' EXIT

LAST_LOG=

require_file() {
    local label=$1
    local path=$2

    if [[ ! -f "$path" ]]; then
        echo "[FAIL] missing $label: $path" >&2
        exit 1
    fi
}

log_name() {
    local label=$1
    echo "$TMP_DIR/${label//[^A-Za-z0-9_.-]/_}.log"
}

run_foxsyn() {
    local label=$1
    local abc_cmd=$2
    local log

    log=$(log_name "$label")
    if ! "$FOXSYN_BIN" -c "$abc_cmd" >"$log" 2>&1; then
        echo "[FAIL] $label" >&2
        tail -n 80 "$log" >&2 || true
        exit 1
    fi

    LAST_LOG=$log
}

require_grep() {
    local label=$1
    local pattern=$2
    local log=$3

    if ! grep -q "$pattern" "$log"; then
        echo "[FAIL] $label: missing '$pattern'" >&2
        tail -n 80 "$log" >&2 || true
        exit 1
    fi
}

run_cec_case() {
    local label=$1
    local input=$2
    local smap_flags=$3
    local mapped_aig

    mapped_aig="$TMP_DIR/${label//[^A-Za-z0-9_.-]/_}.aig"
    run_foxsyn "$label" \
        "read $input; st; smap $smap_flags; strash; write_aiger $mapped_aig; cec -n $input $mapped_aig"
    require_grep "$label" "Networks are equivalent" "$LAST_LOG"
    echo "[OK] $label"
}

run_smoke_case() {
    local label=$1
    local input=$2
    local smap_flags=$3

    run_foxsyn "$label" "read $input; st; smap $smap_flags; ps"
    require_grep "$label" "Delay" "$LAST_LOG"
    echo "[OK] $label"
}

require_file "FoxSYN binary" "$FOXSYN_BIN"
require_file "adder benchmark" "$ADDER"
require_file "alu4 benchmark" "$ALU4"
require_file "C7552 benchmark" "$C7552"
require_file "i10 AIG benchmark" "$I10_AIG"

echo "[INFO] FoxSYN: $FOXSYN_BIN"

run_cec_case "adder smap CEC" "$ADDER" ""
run_cec_case "alu4 smap -g CEC" "$ALU4" "-g"
run_cec_case "alu4 smap -D -g CEC" "$ALU4" "-D -g"
run_cec_case "i10.aig smap -g CEC" "$I10_AIG" "-g"
run_cec_case "i10.aig smap -D -g CEC" "$I10_AIG" "-D -g"

run_smoke_case "C7552 smap -D -g smoke" "$C7552" "-D -g"
run_smoke_case "C7552 smap -D -g -W smoke" "$C7552" "-D -g -W"

echo "[OK] Phase 6 supper smoke regression passed"
