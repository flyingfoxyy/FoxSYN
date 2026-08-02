#!/usr/bin/env python3
"""Sweep csr4 (Phase 0 read-only measurement) across benchmarks, tabulate
recoverable-wire percentages. No CEC, no baseline, no partition freezing --
csr4 Phase 0 is read-only, so this is strictly simpler than run_csr_regression.py."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

# csr4: dir 0->1: 99 boundary LUTs, 15 groups, 0 wide-skipped, sum-bkill=5 recoverable=1 encoders=1
CSR4_DIR_RE = re.compile(
    r"csr4:\s+dir\s+(\d+)->(\d+):\s+(\d+)\s+boundary LUTs,\s+(\d+)\s+groups,\s+"
    r"(\d+)\s+wide-skipped,\s+sum-bkill=(\d+)\s+recoverable=(\d+)\s+encoders=(\d+)"
)
# csr4: TOTAL recoverable wires (detected-floor, cut-function ODC only) = 1 / 48 crossing (2.1%)
CSR4_TOTAL_RE = re.compile(
    r"csr4:\s+TOTAL recoverable wires \(detected-floor, cut-function ODC only\)\s+=\s+"
    r"(\d+)\s+/\s+(\d+)\s+crossing\s+\(([\d.]+)%\)"
)


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


@dataclass
class Result:
    case: str = "-"
    boundary_0to1: str = "-"
    boundary_1to0: str = "-"
    sum_bkill: str = "-"
    recoverable: str = "-"
    crossing: str = "-"
    pct: str = "-"
    sec: str = "-"
    status: str = "FAIL"
    dirs: list = field(default_factory=list)


def parse_output(text: str, case: str = "-") -> Result:
    """Pure function: parse FoxSYN stdout/stderr into a Result. Never launches FoxSYN."""
    text = strip_ansi(text)
    result = Result(case=case)

    dirs = CSR4_DIR_RE.findall(text)
    result.dirs = dirs
    bkill_total = 0
    for d in dirs:
        src, dst, boundary, groups, skipped, bkill, recoverable, encoders = d
        bkill_total += int(bkill)
        if src == "0" and dst == "1":
            result.boundary_0to1 = boundary
        elif src == "1" and dst == "0":
            result.boundary_1to0 = boundary
    if dirs:
        result.sum_bkill = str(bkill_total)

    m = CSR4_TOTAL_RE.search(text)
    if m:
        result.recoverable = m.group(1)
        result.crossing = m.group(2)
        result.pct = m.group(3)

    return result


# ---------------------------------------------------------------------
# --self-test: representative output fed through parse_output without
# launching FoxSYN.
# ---------------------------------------------------------------------

SELF_TEST_OUTPUT = (
    "csr4: dir 1->0: 14 boundary LUTs, 3 groups, 0 wide-skipped, sum-bkill=4 recoverable=0 encoders=0\n"
    "csr4: dir 0->1: 99 boundary LUTs, 15 groups, 0 wide-skipped, sum-bkill=5 recoverable=1 encoders=1\n"
    "csr4: TOTAL recoverable wires (detected-floor, cut-function ODC only) = 1 / 48 crossing (2.1%)\n"
    "csr4: cost 1 encoder LUT(s); NOTE lower bound -- fixed cut boundaries, capped grouping, "
    "K-infeasible groups all round down.\n"
)


def run_self_test() -> int:
    ok = True
    parsed = parse_output(SELF_TEST_OUTPUT, case="selftest")

    checks = [
        ("boundary_1to0", parsed.boundary_1to0, "14"),
        ("boundary_0to1", parsed.boundary_0to1, "99"),
        ("sum_bkill", parsed.sum_bkill, "9"),
        ("recoverable", parsed.recoverable, "1"),
        ("crossing", parsed.crossing, "48"),
        ("pct", parsed.pct, "2.1"),
    ]
    for name, actual, expected in checks:
        if actual != expected:
            print(f"self-test FAILED: {name}: expected {expected!r}, got {actual!r}",
                  file=sys.stderr)
            ok = False

    if ok:
        print("self-test OK")
        return 0
    return 1


# ---------------------------------------------------------------------
# Live FoxSYN execution
# ---------------------------------------------------------------------

def build_command(rel: str, parts: int) -> str:
    return f"read {rel}; st; if -K 6; hpart -N {parts}; csr4; quit"


def run_foxsyn(foxsyn: Path, workdir: Path, command: str, timeout: int):
    """Run one FoxSYN invocation, returning (elapsed_sec, output_text, returncode) or None on timeout."""
    start = time.perf_counter()
    try:
        proc = subprocess.run(
            [str(foxsyn), "-c", command],
            cwd=workdir,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return None
    elapsed = time.perf_counter() - start
    return elapsed, proc.stdout + proc.stderr, proc.returncode


def case_key(case: Path) -> str:
    """Unique case identifier: several corpus subdirs share a bare stem, so key
    on the file's own parent dir name (see run_csr_regression.py's case_key)."""
    return f"{case.parent.name}_{case.stem}"


def run_case(foxsyn: Path, workdir: Path, case: Path, parts: int, timeout: int) -> Result:
    name = case_key(case)
    rel = case.relative_to(workdir).as_posix()
    command = build_command(rel, parts)

    outcome = run_foxsyn(foxsyn, workdir, command, timeout)
    if outcome is None:
        return Result(case=name, status="TIMEOUT")

    elapsed, output, returncode = outcome
    result = parse_output(output, case=name)
    result.sec = f"{elapsed:.2f}"
    if returncode == 0 and result.crossing != "-":
        result.status = "OK"
    else:
        result.status = "FAIL"
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Sweep csr4 Phase 0 measurement across benchmarks")
    parser.add_argument("--self-test", action="store_true",
                        help="Run parse_output against representative output and exit")
    parser.add_argument("--foxsyn", type=Path, default=Path("./FoxSYN"))
    parser.add_argument("--cases-root", type=Path, default=Path("./SimpleCircuits"))
    parser.add_argument("-N", "--parts", type=int, default=2,
                        help="csr4 v1 only supports N=2 partitions")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("-j", "--jobs", type=int,
                        default=max(1, min(os.cpu_count() or 4, 8)))
    parser.add_argument("--match", default="")
    parser.add_argument("--exclude", default="",
                        help="Comma-separated substrings; cases matching any are skipped")
    parser.add_argument("--csv", type=Path, default=None,
                        help="Write the result table to this CSV file")
    args = parser.parse_args()

    if args.self_test:
        return run_self_test()

    workdir = Path.cwd()
    foxsyn = args.foxsyn if args.foxsyn.is_absolute() else (workdir / args.foxsyn).resolve()
    cases_root = args.cases_root if args.cases_root.is_absolute() else (workdir / args.cases_root).resolve()

    if not foxsyn.is_file():
        print(f"error: FoxSYN not found: {foxsyn}", file=sys.stderr)
        return 1

    cases = sorted(p for p in cases_root.rglob("*") if p.suffix in {".v", ".blif"} and p.is_file())
    cases = [c for c in cases if "mapped" not in c.stem]
    if args.match:
        cases = [c for c in cases if args.match in c.as_posix()]
    if args.exclude:
        excludes = [e for e in args.exclude.split(",") if e]
        cases = [c for c in cases if not any(e in c.as_posix() for e in excludes)]

    if not cases:
        print("error: no cases found", file=sys.stderr)
        return 1

    print(f"# csr4 measurement sweep: parts={args.parts}, cases={len(cases)}")
    print(f"{'Case':<16} {'Bnd0->1':>8} {'Bnd1->0':>8} {'SumBkill':>8} {'Recov':>6} "
          f"{'Pct':>6} {'Time':>6} {'Status':<8}")
    print("-" * 80)

    results: list[Result] = []
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(run_case, foxsyn, workdir, c, args.parts, args.timeout): c
            for c in cases
        }
        for future in as_completed(futures):
            r = future.result()
            results.append(r)
            print(f"{r.case:<16} {r.boundary_0to1:>8} {r.boundary_1to0:>8} {r.sum_bkill:>8} "
                  f"{r.recoverable:>6} {r.pct:>6} {r.sec:>5}s {r.status:<8}")
            sys.stdout.flush()

    print("-" * 80)

    # Sort by pct descending for the summary table (more useful than case name
    # for answering "where is the water" at a glance); ties/non-numeric sink last.
    def pct_key(r: Result) -> float:
        try:
            return float(r.pct)
        except ValueError:
            return -1.0

    ok = [r for r in results if r.status == "OK"]
    ok_sorted = sorted(ok, key=pct_key, reverse=True)
    print("\nSorted by recoverable %% (descending):")
    for r in ok_sorted:
        print(f"  {r.case:<16} recoverable={r.recoverable:>4} / crossing={r.crossing:>4}  ({r.pct}%)")

    failed = [r for r in results if r.status == "FAIL"]
    timedout = [r for r in results if r.status == "TIMEOUT"]

    total_recoverable = sum(int(r.recoverable) for r in ok if r.recoverable != "-")
    total_crossing = sum(int(r.crossing) for r in ok if r.crossing != "-")
    total_pct = 100.0 * total_recoverable / total_crossing if total_crossing > 0 else 0.0

    print(f"\nAggregate across OK cases: {total_recoverable} / {total_crossing} crossing "
          f"({total_pct:.1f}%)")
    print(f"Case status: OK={len(ok)} FAIL={len(failed)} TIMEOUT={len(timedout)} "
          f"(total={len(results)})")
    if failed:
        print("!!! FAILED: " + ", ".join(r.case for r in failed))
    if timedout:
        print("!!! TIMEOUT: " + ", ".join(r.case for r in timedout))

    if args.csv:
        import csv
        with open(args.csv, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["case", "boundary_0to1", "boundary_1to0", "sum_bkill",
                             "recoverable", "crossing", "pct", "sec", "status"])
            for r in sorted(results, key=lambda r: r.case):
                writer.writerow([r.case, r.boundary_0to1, r.boundary_1to0, r.sum_bkill,
                                 r.recoverable, r.crossing, r.pct, r.sec, r.status])
        print(f"\nwrote CSV: {args.csv}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
