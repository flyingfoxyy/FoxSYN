#!/usr/bin/env python3
"""Sweep csr3 (Phase 0 read-only measurement) across benchmarks, tabulate
recoverable-wire percentages. No CEC, no baseline, no partition freezing --
csr3 Phase 0 is read-only, so this is strictly simpler than run_csr_regression.py."""

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

# csr3: dir 0->1: 24 crossing signals, 7 groups (2 sim-pruned), sum-k=24 recoverable=7
CSR3_DIR_RE = re.compile(
    r"csr3:\s+dir\s+(\d+)->(\d+):\s+(\d+)\s+crossing signals,\s+(\d+)\s+groups\s+"
    r"\((\d+)\s+sim-pruned\),\s+sum-k=(\d+)\s+recoverable=(\d+)"
)
# csr3: TOTAL recoverable wires (detected-floor, combinational SDC only) = 7 / 35 crossing (20.0%)
CSR3_TOTAL_RE = re.compile(
    r"csr3:\s+TOTAL recoverable wires \(detected-floor, combinational SDC only\)\s+=\s+"
    r"(\d+)\s+/\s+(\d+)\s+crossing\s+\(([\d.]+)%\)"
)


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


@dataclass
class Result:
    case: str = "-"
    cross_0to1: str = "-"
    cross_1to0: str = "-"
    sum_k: str = "-"
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

    dirs = CSR3_DIR_RE.findall(text)
    result.dirs = dirs
    for d in dirs:
        src, dst, crossing, groups, pruned, sum_k, recoverable = d
        if src == "0" and dst == "1":
            result.cross_0to1 = crossing
        elif src == "1" and dst == "0":
            result.cross_1to0 = crossing

    m = CSR3_TOTAL_RE.search(text)
    if m:
        result.recoverable = m.group(1)
        result.crossing = m.group(2)
        result.pct = m.group(3)
        result.sum_k = m.group(2)  # sum-k across both directions equals total crossing

    return result


# ---------------------------------------------------------------------
# --self-test: representative output fed through parse_output without
# launching FoxSYN.
# ---------------------------------------------------------------------

SELF_TEST_OUTPUT = (
    "csr3: dir 0->1: 24 crossing signals, 4 groups (3 sim-pruned), sum-k=24 recoverable=7\n"
    "csr3: dir 1->0: 11 crossing signals, 5 groups (5 sim-pruned), sum-k=11 recoverable=0\n"
    "csr3: TOTAL recoverable wires (detected-floor, combinational SDC only) = 7 / 35 crossing (20.0%)\n"
    "csr3: NOTE this is a lower bound; reachability/ODC water (e.g. one-hot buses) is NOT measured.\n"
)


def run_self_test() -> int:
    ok = True
    parsed = parse_output(SELF_TEST_OUTPUT, case="selftest")

    checks = [
        ("cross_0to1", parsed.cross_0to1, "24"),
        ("cross_1to0", parsed.cross_1to0, "11"),
        ("recoverable", parsed.recoverable, "7"),
        ("crossing", parsed.crossing, "35"),
        ("pct", parsed.pct, "20.0"),
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
    return f"read {rel}; st; if -K 6; hpart -N {parts}; csr3; quit"


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
    parser = argparse.ArgumentParser(description="Sweep csr3 Phase 0 measurement across benchmarks")
    parser.add_argument("--self-test", action="store_true",
                        help="Run parse_output against representative output and exit")
    parser.add_argument("--foxsyn", type=Path, default=Path("./FoxSYN"))
    parser.add_argument("--cases-root", type=Path, default=Path("./SimpleCircuits"))
    parser.add_argument("-N", "--parts", type=int, default=2,
                        help="csr3 v1 only supports N=2 partitions")
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

    print(f"# csr3 measurement sweep: parts={args.parts}, cases={len(cases)}")
    print(f"{'Case':<16} {'Cross0->1':>9} {'Cross1->0':>9} {'SumK':>6} {'Recov':>6} "
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
            print(f"{r.case:<16} {r.cross_0to1:>9} {r.cross_1to0:>9} {r.sum_k:>6} "
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
            writer.writerow(["case", "cross_0to1", "cross_1to0", "sum_k",
                             "recoverable", "crossing", "pct", "sec", "status"])
            for r in sorted(results, key=lambda r: r.case):
                writer.writerow([r.case, r.cross_0to1, r.cross_1to0, r.sum_k,
                                 r.recoverable, r.crossing, r.pct, r.sec, r.status])
        print(f"\nwrote CSV: {args.csv}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
