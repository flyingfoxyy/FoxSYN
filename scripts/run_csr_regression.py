#!/usr/bin/env python3
"""Test csr on benchmarks: map -> hpart -> csr, report cut-edge/area/hop deltas."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
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
ND_RE = re.compile(r"\bnd =\s*(\d+)")
HOP_RE = re.compile(r"\bhop =\s*(\d+)")
CUTNET_RE = re.compile(r"\bcut =\s*(\d+)")
# "Networks are equivalent" / "Networks are NOT EQUIVALENT"
CEC_RE = re.compile(r"Networks are (NOT )?EQUIVALENT", re.IGNORECASE)


@dataclass
class Result:
    case: str
    nodes_before: str = "-"
    nodes_after: str = "-"
    hop_before: str = "-"
    hop_after: str = "-"
    cutnet_before: str = "-"
    cutnet_after: str = "-"
    cut_before: str = "-"
    cut_after_p1: str = "-"
    cut_after: str = "-"
    phase1_attempts: str = "-"
    phase1_successes: str = "-"
    phase2_replications: str = "-"
    gain: str = "-"
    cec: str = "-"
    sec: str = "-"
    status: str = "FAIL"


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def run_case(foxsyn: Path, workdir: Path, case: Path, parts: int,
             csr_args: str, timeout: int, runs: int, do_cec: bool,
             parts_dir: Path | None, save_parts: bool) -> Result:
    best: Result | None = None

    for _ in range(runs):
        r = run_case_once(foxsyn, workdir, case, parts, csr_args, timeout,
                          do_cec, parts_dir=parts_dir, save_parts=save_parts)
        if best is None:
            best = r
        elif r.status == "OK" and r.gain != "-":
            if best.gain == "-" or int(r.gain) > int(best.gain):
                best = r

    return best  # type: ignore


def run_case_once(foxsyn: Path, workdir: Path, case: Path, parts: int,
                  csr_args: str, timeout: int, do_cec: bool,
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

    before_blif = after_blif = None
    cec_segment = ""
    if do_cec:
        before_fd, before_path = tempfile.mkstemp(suffix=".blif", prefix=f"csr_{name}_before_")
        after_fd, after_path = tempfile.mkstemp(suffix=".blif", prefix=f"csr_{name}_after_")
        os.close(before_fd)
        os.close(after_fd)
        before_blif, after_blif = Path(before_path), Path(after_path)
        cec_segment = f"; write {before_blif}"
        cec_tail = f"; write {after_blif}; cec {before_blif} {after_blif}"
    else:
        cec_tail = ""

    command = (
        f"read {rel}; st; if -K 6; {hpart_cmd}{cec_segment}; ps; "
        f"csr {csr_args}; ps{cec_tail}"
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
    finally:
        if before_blif is not None:
            before_blif.unlink(missing_ok=True)
        if after_blif is not None:
            after_blif.unlink(missing_ok=True)

    elapsed = time.perf_counter() - start
    output = strip_ansi(proc.stdout + proc.stderr)
    result = Result(case=name, sec=f"{elapsed:.1f}")

    nd_values = ND_RE.findall(output)
    if len(nd_values) >= 1:
        result.nodes_before = nd_values[0]
    if len(nd_values) >= 2:
        result.nodes_after = nd_values[1]

    hop_values = HOP_RE.findall(output)
    if len(hop_values) >= 1:
        result.hop_before = hop_values[0]
    if len(hop_values) >= 2:
        result.hop_after = hop_values[1]

    cutnet_values = CUTNET_RE.findall(output)
    if len(cutnet_values) >= 1:
        result.cutnet_before = cutnet_values[0]
    if len(cutnet_values) >= 2:
        result.cutnet_after = cutnet_values[1]

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

    if do_cec:
        m = CEC_RE.search(output)
        if m:
            result.cec = "NOT_EQ" if m.group(1) else "EQ"
        else:
            result.cec = "N/A"

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
    parser.add_argument("--cec", action="store_true",
                        help="Write BLIF before/after csr and run cec for functional equivalence")
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

    print(f"# csr regression: parts={args.parts}, csr_args='{args.csr_args}', "
          f"cec={'on' if args.cec else 'off'}, cases={len(cases)}")
    if parts_dir:
        mode = "saving" if save_parts else "loading"
        print(f"# partition {mode}: {parts_dir}")
    print(f"{'Case':<16} {'NdBef':>6} {'NdAft':>6} {'HopBef':>6} {'HopAft':>6} "
          f"{'CutEdgeB':>8} {'CutEdgeA':>8} {'Gain':>6} {'CEC':>7} {'Time':>6} {'Status':<8}")
    print("-" * 110)

    results: list[Result] = []
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(run_case, foxsyn, workdir, c, args.parts, args.csr_args,
                            args.timeout, args.runs, args.cec, parts_dir, save_parts): c
            for c in cases
        }
        for future in as_completed(futures):
            r = future.result()
            results.append(r)
            print(f"{r.case:<16} {r.nodes_before:>6} {r.nodes_after:>6} "
                  f"{r.hop_before:>6} {r.hop_after:>6} "
                  f"{r.cut_before:>8} {r.cut_after:>8} {r.gain:>6} {r.cec:>7} "
                  f"{r.sec:>5}s {r.status:<8}")
            sys.stdout.flush()

    print("-" * 110)
    ok = [r for r in results if r.status == "OK"]
    gains = [r for r in ok if r.gain != "-" and int(r.gain) > 0]
    losses = [r for r in ok if r.gain != "-" and int(r.gain) < 0]
    crashes = [r for r in results if r.status != "OK"]
    print(f"\nCases with cut-edge gain: {len(gains)}/{len(results)}  "
          f"(OK={len(ok)}, negative-gain={len(losses)}, crash/fail={len(crashes)})")

    if gains:
        gains.sort(key=lambda r: int(r.gain), reverse=True)
        total_before = sum(int(r.cut_before) for r in gains if r.cut_before != "-")
        total_after = sum(int(r.cut_after) for r in gains if r.cut_after != "-")
        print(f"\nTotal cut-edges across gaining cases: {total_before} -> {total_after}")

    # Area / hop deltas across all OK cases (not just gaining ones)
    area_deltas = [(r.case, int(r.nodes_after) - int(r.nodes_before))
                    for r in ok if r.nodes_before != "-" and r.nodes_after != "-"]
    hop_deltas = [(r.case, int(r.hop_after) - int(r.hop_before))
                   for r in ok if r.hop_before != "-" and r.hop_after != "-"]
    area_grown = [d for _, d in area_deltas if d > 0]
    area_total_before = sum(int(r.nodes_before) for r in ok if r.nodes_before != "-")
    area_total_after = sum(int(r.nodes_after) for r in ok if r.nodes_after != "-")
    hop_total_before = sum(int(r.hop_before) for r in ok if r.hop_before != "-")
    hop_total_after = sum(int(r.hop_after) for r in ok if r.hop_after != "-")
    print(f"\nArea (node count) across OK cases: {area_total_before} -> {area_total_after} "
          f"({len(area_grown)}/{len(ok)} cases grew)")
    print(f"Hop count across OK cases: {hop_total_before} -> {hop_total_after}")

    if args.cec:
        eq = [r for r in ok if r.cec == "EQ"]
        not_eq = [r for r in ok if r.cec == "NOT_EQ"]
        na = [r for r in ok if r.cec == "N/A"]
        print(f"\ncec: EQ={len(eq)}  NOT_EQ={len(not_eq)}  N/A={len(na)}")
        if not_eq:
            print("!!! FUNCTIONAL MISMATCH: " + ", ".join(r.case for r in not_eq))

    if crashes:
        print(f"\ncrash/fail cases: {', '.join(r.case for r in crashes)}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
