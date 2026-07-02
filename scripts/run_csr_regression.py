#!/usr/bin/env python3
"""Test csr on benchmarks: map -> hpart -> csr, report cut-edge gain."""

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

# csr: cut-edges 683 -> 495 (after phase1=585, after phase2=495)
CSR_CUTEDGE_RE = re.compile(
    r"csr:\s+cut-edges\s+(\d+)\s+->\s+(\d+)\s+\(after phase1=(\d+),\s+after phase2=(\d+)\)"
)
# csr: phase1 2924 attempts / 96 successes; phase2 5 replications
CSR_COUNTS_RE = re.compile(
    r"csr:\s+phase1\s+(\d+)\s+attempts\s+/\s+(\d+)\s+successes;\s+phase2\s+(\d+)\s+replications"
)
CUT_RE = re.compile(r"\bcut =\s*(\d+)")


@dataclass
class Result:
    case: str
    nodes: str = "-"
    cut_before: str = "-"
    cut_after_p1: str = "-"
    cut_after: str = "-"
    phase1_attempts: str = "-"
    phase1_successes: str = "-"
    phase2_replications: str = "-"
    gain: str = "-"
    sec: str = "-"
    status: str = "FAIL"


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def run_case(foxsyn: Path, workdir: Path, case: Path, parts: int,
             csr_args: str, timeout: int, runs: int = 1,
             parts_dir: Path | None = None, save_parts: bool = False) -> Result:
    best: Result | None = None

    for _ in range(runs):
        r = run_case_once(foxsyn, workdir, case, parts, csr_args, timeout,
                          parts_dir=parts_dir, save_parts=save_parts)
        if best is None:
            best = r
        elif r.status == "OK" and r.gain != "-":
            if best.gain == "-" or int(r.gain) > int(best.gain):
                best = r

    return best  # type: ignore


def run_case_once(foxsyn: Path, workdir: Path, case: Path, parts: int,
                  csr_args: str, timeout: int,
                  parts_dir: Path | None = None, save_parts: bool = False) -> Result:
    name = case.stem
    rel = case.relative_to(workdir).as_posix()

    hpart_cmd = f"hpart -N {parts}"
    if parts_dir is not None:
        part_file = parts_dir / f"{name}.part"
        if save_parts:
            hpart_cmd += f" --save-part {part_file}"
        elif part_file.exists():
            hpart_cmd += f" --load-part {part_file}"

    command = (
        f"read {rel}; st; if -K 6; "
        f"{hpart_cmd}; ps; "
        f"csr {csr_args}; ps"
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

    cut_values = [m.group(1) for m in CUT_RE.finditer(output)]
    if cut_values:
        result.cut_before = cut_values[0]

    m = CSR_CUTEDGE_RE.search(output)
    if m:
        result.cut_before = m.group(1)
        result.cut_after = m.group(2)
        result.cut_after_p1 = m.group(3)
        result.gain = str(int(m.group(1)) - int(m.group(2)))

    m = CSR_COUNTS_RE.search(output)
    if m:
        result.phase1_attempts = m.group(1)
        result.phase1_successes = m.group(2)
        result.phase2_replications = m.group(3)

    nd_match = re.search(r"\bnd =\s*(\d+)", output)
    if nd_match:
        result.nodes = nd_match.group(1)

    if proc.returncode == 0:
        result.status = "OK"

    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Test csr on benchmarks")
    parser.add_argument("--foxsyn", type=Path, default=Path("./FoxSYN"))
    parser.add_argument("--cases-root", type=Path, default=Path("./SimpleCircuits"))
    parser.add_argument("-N", "--parts", type=int, default=4)
    parser.add_argument("--csr-args", default="-v")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("-j", "--jobs", type=int,
                        default=max(1, min(os.cpu_count() or 4, 8)))
    parser.add_argument("--match", default="")
    parser.add_argument("--runs", type=int, default=1,
                        help="Run each case N times, report best (mitigates hmetis randomness)")
    parser.add_argument("--save-parts-dir", type=Path, default=None,
                        help="Save hmetis partition results to this directory for reproducible runs")
    parser.add_argument("--load-parts-dir", type=Path, default=None,
                        help="Load partition results from this directory instead of running hmetis")
    args = parser.parse_args()

    workdir = Path.cwd()
    foxsyn = args.foxsyn if args.foxsyn.is_absolute() else (workdir / args.foxsyn).resolve()
    cases_root = args.cases_root if args.cases_root.is_absolute() else (workdir / args.cases_root).resolve()

    if not foxsyn.is_file():
        print(f"error: FoxSYN not found: {foxsyn}", file=sys.stderr)
        return 1

    parts_dir: Path | None = None
    save_parts = False
    if args.save_parts_dir is not None:
        parts_dir = args.save_parts_dir if args.save_parts_dir.is_absolute() else (workdir / args.save_parts_dir).resolve()
        parts_dir.mkdir(parents=True, exist_ok=True)
        save_parts = True
    elif args.load_parts_dir is not None:
        parts_dir = args.load_parts_dir if args.load_parts_dir.is_absolute() else (workdir / args.load_parts_dir).resolve()
        save_parts = False

    cases = sorted(p for p in cases_root.rglob("*") if p.suffix in {".v", ".blif"} and p.is_file())
    cases = [c for c in cases if "mapped" not in c.stem]
    if args.match:
        cases = [c for c in cases if args.match in c.as_posix()]

    if not cases:
        print("error: no cases found", file=sys.stderr)
        return 1

    print(f"# csr regression: parts={args.parts}, csr_args='{args.csr_args}', cases={len(cases)}")
    if parts_dir:
        mode = "saving" if save_parts else "loading"
        print(f"# partition {mode}: {parts_dir}")
    print(f"{'Case':<16} {'Nodes':>6} {'CutBef':>7} {'CutP1':>7} {'CutAft':>7} "
          f"{'Gain':>6} {'P1Att':>6} {'P1Succ':>6} {'P2Rep':>6} {'Time':>6} {'Status':<8}")
    print("-" * 100)

    results: list[Result] = []
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(run_case, foxsyn, workdir, c, args.parts, args.csr_args,
                            args.timeout, args.runs, parts_dir, save_parts): c
            for c in cases
        }
        for future in as_completed(futures):
            r = future.result()
            results.append(r)
            print(f"{r.case:<16} {r.nodes:>6} {r.cut_before:>7} {r.cut_after_p1:>7} {r.cut_after:>7} "
                  f"{r.gain:>6} {r.phase1_attempts:>6} {r.phase1_successes:>6} "
                  f"{r.phase2_replications:>6} {r.sec:>5}s {r.status:<8}")
            sys.stdout.flush()

    print("-" * 100)
    ok = [r for r in results if r.status == "OK"]
    gains = [r for r in ok if r.gain != "-" and int(r.gain) > 0]
    crashes = [r for r in results if r.status != "OK"]
    print(f"\nCases with cut-edge gain: {len(gains)}/{len(results)}  "
          f"(OK={len(ok)}, crash/fail={len(crashes)})")
    if gains:
        gains.sort(key=lambda r: int(r.gain), reverse=True)
        total_before = sum(int(r.cut_before) for r in gains if r.cut_before != "-")
        total_after = sum(int(r.cut_after) for r in gains if r.cut_after != "-")
        for r in gains:
            print(f"  {r.case}: cut-edges {r.cut_before} -> {r.cut_after} (gain={r.gain})")
        print(f"\nTotal cut-edges across gaining cases: {total_before} -> {total_after}")

    if crashes:
        print(f"\ncrash/fail cases: {', '.join(r.case for r in crashes)}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
