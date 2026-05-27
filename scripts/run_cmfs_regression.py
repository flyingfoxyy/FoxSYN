#!/usr/bin/env python3
"""Test cmfs on MCNC benchmarks: map -> hpart -> cpr -> cmfs, report hop gain."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

CMFS_RESULT_RE = re.compile(
    r"cmfs:.*?(\d+)\s+attempts,\s+(\d+)\s+removals\s+\((\d+)\s+timeouts\)"
)
CMFS_ARRIVAL_RE = re.compile(
    r"cmfs:\s+arrival\s+([0-9.]+)\s+->\s+([0-9.]+)\s+\(improvement\s+([0-9.-]+)\)"
)
HOP_RE = re.compile(r"\bhop =\s*(\d+)")
ARR_RE = re.compile(r"\barr\s*=\s*([0-9.]+)")


@dataclass
class Result:
    case: str
    nodes: str = "-"
    hop_before: str = "-"
    hop_after: str = "-"
    arr_before: str = "-"
    arr_after: str = "-"
    attempts: str = "-"
    removals: str = "-"
    timeouts: str = "-"
    gain: str = "-"
    sec: str = "-"
    status: str = "FAIL"


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def run_case(foxsyn: Path, workdir: Path, case: Path, parts: int,
             cmfs_args: str, timeout: int) -> Result:
    name = case.stem
    rel = case.relative_to(workdir).as_posix()

    command = (
        f"read {rel}; st; if -K 6; "
        f"hpart -N {parts}; cpr; ps; "
        f"cmfs {cmfs_args}; ps"
    )

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
        return Result(case=name, sec=f"{time.perf_counter()-start:.1f}", status="TIMEOUT")

    elapsed = time.perf_counter() - start
    output = strip_ansi(proc.stdout + proc.stderr)
    result = Result(case=name, sec=f"{elapsed:.1f}")

    # Parse ps outputs (there are two: before cmfs and after cmfs)
    ps_lines = [l for l in output.splitlines() if "nd =" in l or "pdb" in l]

    # Extract hop from pdb lines
    hop_values = [m.group(1) for m in HOP_RE.finditer(output)]
    arr_values = [m.group(1) for m in ARR_RE.finditer(output)]

    if len(hop_values) >= 2:
        result.hop_before = hop_values[-2]
        result.hop_after = hop_values[-1]
    elif len(hop_values) == 1:
        result.hop_before = hop_values[0]
        result.hop_after = hop_values[0]

    if len(arr_values) >= 2:
        result.arr_before = arr_values[-2]
        result.arr_after = arr_values[-1]

    # Parse cmfs summary
    m = CMFS_RESULT_RE.search(output)
    if m:
        result.attempts = m.group(1)
        result.removals = m.group(2)
        result.timeouts = m.group(3)

    m = CMFS_ARRIVAL_RE.search(output)
    if m:
        result.arr_before = m.group(1)
        result.arr_after = m.group(2)
        result.gain = m.group(3)

    # Extract node count from first ps line
    nd_match = re.search(r"\bnd =\s*(\d+)", output)
    if nd_match:
        result.nodes = nd_match.group(1)

    if proc.returncode == 0:
        result.status = "OK"

    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Test cmfs on benchmarks")
    parser.add_argument("--foxsyn", type=Path, default=Path("./FoxSYN"))
    parser.add_argument("--cases-root", type=Path, default=Path("./SimpleCircuits"))
    parser.add_argument("-N", "--parts", type=int, default=4)
    parser.add_argument("--cmfs-args", default="-v -W 4 -M 500")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("-j", "--jobs", type=int,
                        default=max(1, min(os.cpu_count() or 4, 8)))
    parser.add_argument("--match", default="")
    args = parser.parse_args()

    workdir = Path.cwd()
    foxsyn = args.foxsyn if args.foxsyn.is_absolute() else (workdir / args.foxsyn).resolve()
    cases_root = args.cases_root if args.cases_root.is_absolute() else (workdir / args.cases_root).resolve()

    if not foxsyn.is_file():
        print(f"error: FoxSYN not found: {foxsyn}", file=sys.stderr)
        return 1

    cases = sorted(p for p in cases_root.rglob("*") if p.suffix in {".v", ".blif"} and p.is_file())
    # Skip pre-mapped files
    cases = [c for c in cases if "mapped" not in c.stem]
    if args.match:
        cases = [c for c in cases if args.match in c.as_posix()]

    if not cases:
        print("error: no cases found", file=sys.stderr)
        return 1

    print(f"# cmfs regression: parts={args.parts}, cmfs_args='{args.cmfs_args}', cases={len(cases)}")
    print(f"{'Case':<16} {'Nodes':>6} {'HopBef':>7} {'HopAft':>7} "
          f"{'ArrBef':>9} {'ArrAft':>9} {'Gain':>9} "
          f"{'Att':>4} {'Rem':>4} {'T/O':>4} {'Time':>6} {'Status':<6}")
    print("-" * 100)

    results: list[Result] = []
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(run_case, foxsyn, workdir, c, args.parts, args.cmfs_args, args.timeout): c
            for c in cases
        }
        for future in as_completed(futures):
            r = future.result()
            results.append(r)
            print(f"{r.case:<16} {r.nodes:>6} {r.hop_before:>7} {r.hop_after:>7} "
                  f"{r.arr_before:>9} {r.arr_after:>9} {r.gain:>9} "
                  f"{r.attempts:>4} {r.removals:>4} {r.timeouts:>4} {r.sec:>5}s {r.status:<6}")
            sys.stdout.flush()

    # Summary
    print("-" * 100)
    gains = [r for r in results if r.gain != "-" and float(r.gain) > 0]
    print(f"\nCases with gain: {len(gains)}/{len(results)}")
    if gains:
        gains.sort(key=lambda r: float(r.gain), reverse=True)
        for r in gains:
            print(f"  {r.case}: arrival improvement = {r.gain}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
