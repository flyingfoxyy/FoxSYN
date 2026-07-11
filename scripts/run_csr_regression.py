#!/usr/bin/env python3
"""Test csr on benchmarks: map -> hpart -> csr, report cut-edge/area/hop deltas."""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

# csr: cut-edges 667 -> 487 (after phase0=610, after phase1=500, after phase2=487)
CSR_CUTEDGE_RE = re.compile(
    r"csr:\s+cut-edges\s+(\d+)\s+->\s+(\d+)\s+\((?:after phase0=\d+,\s+)?"
    r"after phase1=(\d+),\s+after phase2=(\d+)\)"
)
# csr: phase0 33 moves / 0 swaps / 2 compound; phase1 2372 attempts / 82 successes; phase2 5 replications
CSR_COUNTS_RE = re.compile(
    r"csr:\s+phase0\s+(\d+)\s+moves\s+/\s+(\d+)\s+swaps\s+/\s+(\d+)\s+compound;\s+"
    r"phase1\s+(\d+)\s+attempts\s+/\s+(\d+)\s+successes;\s+phase2\s+(\d+)\s+replications"
)
# csr: trajectory 1 cut-edge=455 cut-net=78 hop=7 nodes=293 sec=0.89 valid=1
CSR_TRAJECTORY_RE = re.compile(
    r"csr:\s+trajectory\s+(\d+)\s+cut-edge=(\d+)\s+cut-net=(\d+)\s+hop=(\d+)\s+"
    r"nodes=(\d+)\s+sec=([\d.]+)\s+valid=(\d)"
)
# csr: selected trajectory 2
CSR_SELECTED_RE = re.compile(r"csr:\s+selected trajectory\s+(\d+)")
ND_RE = re.compile(r"\bnd =\s*(\d+)")
HOP_RE = re.compile(r"\bhop =\s*(\d+)")
CUTNET_RE = re.compile(r"\bcut-net =\s*(\d+)")
# "Networks are equivalent" / "Networks are NOT EQUIVALENT"
CEC_RE = re.compile(r"Networks are (NOT )?EQUIVALENT", re.IGNORECASE)

# csr's own -T flag postdates the frozen baseline binary (commit 799702c);
# strip it so the same csr_args string can be replayed against that baseline.
BASELINE_STRIP_RE = re.compile(r"-T\s+\d+")

SUMMARY_FIELDS = (
    "cut_before", "cut_after", "cut_after_p1",
    "cutnet_before", "cutnet_after", "hop_before", "hop_after",
    "nodes_before", "nodes_after",
    "phase0_moves", "phase0_swaps", "phase0_compound",
    "phase1_attempts", "phase1_successes", "phase2_replications",
    "selected_trajectory",
)


@dataclass
class Result:
    case: str = "-"
    nodes_before: str = "-"
    nodes_after: str = "-"
    hop_before: str = "-"
    hop_after: str = "-"
    cutnet_before: str = "-"
    cutnet_after: str = "-"
    cut_before: str = "-"
    cut_after_p1: str = "-"
    cut_after: str = "-"
    phase0_moves: str = "-"
    phase0_swaps: str = "-"
    phase0_compound: str = "-"
    phase1_attempts: str = "-"
    phase1_successes: str = "-"
    phase2_replications: str = "-"
    selected_trajectory: str = "-"
    trajectories: list = field(default_factory=list)
    gain: str = "-"
    constraints: str = "-"
    deterministic: bool = False
    baseline_cut_after: str = "-"
    runtime_ratio: str = "-"
    cec: str = "-"
    sec: str = "-"
    status: str = "FAIL"


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def _to_int(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def ceil_percentage_limit(count: int, percentage: int) -> int:
    """Mirrors csr_state.cpp's ComputePercentageLimit(count, percentage, round_up=true):
    node_limit and cutnet_limit are ceil(entry * pct / 100), not floor. Using a
    floor-equivalent check here (entry*pct <= after*100) rejects legal results
    whenever entry*pct isn't a clean multiple of 100 -- e.g. an odd entry cut-net
    count times 150 always leaves a remainder of 50, so *every* odd-cutnet case
    would be flagged as a violation despite passing the implementation's own
    (already-tested) hard constraint.
    """
    return -(-(count * percentage) // 100)


def compute_constraints(result: Result) -> str:
    """OK only if all four comparisons parsed and every hard constraint holds:
    hop_after <= hop_before; nodes_after <= ceil(nodes_before*1.02);
    cutnet_after <= ceil(cutnet_before*1.50); cut_after <= cut_before.
    """
    hop_before, hop_after = _to_int(result.hop_before), _to_int(result.hop_after)
    nodes_before, nodes_after = _to_int(result.nodes_before), _to_int(result.nodes_after)
    cutnet_before, cutnet_after = _to_int(result.cutnet_before), _to_int(result.cutnet_after)
    cut_before, cut_after = _to_int(result.cut_before), _to_int(result.cut_after)

    values = (hop_before, hop_after, nodes_before, nodes_after,
              cutnet_before, cutnet_after, cut_before, cut_after)
    if any(v is None for v in values):
        return "-"

    ok = (
        hop_after <= hop_before
        and nodes_after <= ceil_percentage_limit(nodes_before, 102)
        and cutnet_after <= ceil_percentage_limit(cutnet_before, 150)
        and cut_after <= cut_before
    )
    return "OK" if ok else "FAIL"


def parse_output(text: str, case: str = "-") -> Result:
    """Pure function: parse FoxSYN stdout/stderr into a Result. Never launches FoxSYN."""
    text = strip_ansi(text)
    result = Result(case=case)

    nd_values = ND_RE.findall(text)
    if len(nd_values) >= 1:
        result.nodes_before = nd_values[0]
    if len(nd_values) >= 2:
        result.nodes_after = nd_values[1]

    hop_values = HOP_RE.findall(text)
    if len(hop_values) >= 1:
        result.hop_before = hop_values[0]
    if len(hop_values) >= 2:
        result.hop_after = hop_values[1]

    cutnet_values = CUTNET_RE.findall(text)
    if len(cutnet_values) >= 1:
        result.cutnet_before = cutnet_values[0]
    if len(cutnet_values) >= 2:
        result.cutnet_after = cutnet_values[1]

    m = CSR_CUTEDGE_RE.search(text)
    if m:
        result.cut_before = m.group(1)
        result.cut_after = m.group(2)
        result.cut_after_p1 = m.group(3)
        result.gain = str(int(m.group(1)) - int(m.group(2)))

    m = CSR_COUNTS_RE.search(text)
    if m:
        (result.phase0_moves, result.phase0_swaps, result.phase0_compound,
         result.phase1_attempts, result.phase1_successes,
         result.phase2_replications) = m.groups()

    result.trajectories = CSR_TRAJECTORY_RE.findall(text)

    m = CSR_SELECTED_RE.search(text)
    if m:
        result.selected_trajectory = m.group(1)

    cec_m = CEC_RE.search(text)
    if cec_m:
        result.cec = "NOT_EQ" if cec_m.group(1) else "EQ"

    result.constraints = compute_constraints(result)
    return result


def summaries_match(a: Result, b: Result) -> bool:
    """Byte-for-byte comparison of the optimization summary, ignoring elapsed seconds."""
    if any(getattr(a, f) != getattr(b, f) for f in SUMMARY_FIELDS):
        return False
    if len(a.trajectories) != len(b.trajectories):
        return False
    for ta, tb in zip(a.trajectories, b.trajectories):
        # trajectory tuple: (id, cut_edge, cut_net, hop, nodes, sec, valid)
        if ta[:5] != tb[:5] or ta[6] != tb[6]:
            return False
    return True


# ---------------------------------------------------------------------
# --self-test: representative output fed through parse_output without
# launching FoxSYN.
# ---------------------------------------------------------------------

SELF_TEST_OUTPUT = (
    "\x1b[1;37malu4                          :\x1b[0m i/o =   14/    8  lat =    0  nd =   288  edge =   1327  aig  =  1304  lev = 15\n"
    "\x1b[1;36mpdb                           :\x1b[0m part =\x1b[1;32m  4\x1b[0m  hop =\x1b[1;33m    7\x1b[0m  cut-net =\x1b[1;33m   56\x1b[0m  cut-edge =\x1b[1;33m  630\x1b[0m  arr =\x1b[1;33m1412.00\x1b[0m\n"
    "csr: trajectory 0 initial cut-edges = 630\n"
    "csr: phase0 round  0  moves= 24  cut-edges=571  hop=7/7\n"
    "csr: phase0 compound=2\n"
    "csr: phase1 round  4  candidates=465  fixed=  0  cut-edges=465\n"
    "csr: phase2 node budget exhausted (5/5), stopping\n"
    "csr: cut-edges 630 -> 455 (after phase0=547, after phase1=465, after phase2=455)\n"
    "csr: phase0 33 moves / 0 swaps / 2 compound; phase1 2372 attempts / 82 successes; phase2 5 replications\n"
    "csr: trajectory 0 cut-edge=455 cut-net=79 hop=7 nodes=293 sec=1.04 valid=1\n"
    "csr: selected trajectory 1\n"
    "\x1b[1;37malu4                          :\x1b[0m i/o =   14/    8  lat =    0  nd =   293  edge =   1300  aig  =  1204  lev = 15\n"
    "\x1b[1;36mpdb                           :\x1b[0m part =\x1b[1;32m  4\x1b[0m  hop =\x1b[1;33m    7\x1b[0m  cut-net =\x1b[1;33m   79\x1b[0m  cut-edge =\x1b[1;33m  455\x1b[0m  arr =\x1b[1;33m1415.00\x1b[0m"
)


def run_self_test() -> int:
    ok = True
    parsed = parse_output(SELF_TEST_OUTPUT, case="selftest")
    parsed_repeat = parse_output(SELF_TEST_OUTPUT, case="selftest")
    parsed.deterministic = summaries_match(parsed, parsed_repeat)

    checks = [
        ("cutnet_before", parsed.cutnet_before, "56"),
        ("cutnet_after", parsed.cutnet_after, "79"),
        ("selected_trajectory", parsed.selected_trajectory, "1"),
        ("constraints", parsed.constraints, "OK"),
        ("deterministic", parsed.deterministic, True),
    ]
    for name, actual, expected in checks:
        if actual != expected:
            print(f"self-test FAILED: {name}: expected {expected!r}, got {actual!r}",
                  file=sys.stderr)
            ok = False

    assert parsed.cutnet_before == "56"
    assert parsed.cutnet_after == "79"
    assert parsed.selected_trajectory == "1"
    assert parsed.constraints == "OK"
    assert parsed.deterministic is True

    # Determinism must trip on a genuine mismatch, not always report True.
    diverged = parse_output(SELF_TEST_OUTPUT.replace("cut-edge=455", "cut-edge=456"),
                            case="selftest")
    if summaries_match(parsed, diverged):
        print("self-test FAILED: summaries_match did not detect a real mismatch",
              file=sys.stderr)
        ok = False

    if ok:
        print("self-test OK")
        return 0
    return 1


# ---------------------------------------------------------------------
# Live FoxSYN execution
# ---------------------------------------------------------------------

def build_command(rel: str, parts: int, hpart_cmd: str, csr_args: str,
                  cec_segment: str, cec_tail: str) -> str:
    return (
        f"read {rel}; st; if -K 6; {hpart_cmd}{cec_segment}; ps; "
        f"csr {csr_args}; ps{cec_tail}"
    )


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
    """Unique case identifier: several corpus subdirs share a bare stem
    (e.g. EPFL/i2c.v and opencores/i2c.blif), so the stem alone collides
    both in printed reports and in --save-parts-dir/--load-parts-dir
    filenames. Keying on the file's own parent dir name is stable across
    differently-scoped --cases-root invocations (unlike keying on a path
    relative to --cases-root), since a frozen-partition directory is
    meant to be reused across scoped and full-corpus runs alike.
    """
    return f"{case.parent.name}_{case.stem}"


def run_case_once(foxsyn: Path, workdir: Path, case: Path, parts: int,
                  csr_args: str, timeout: int, do_cec: bool,
                  parts_dir: Path | None = None, save_parts: bool = False) -> Result:
    name = case_key(case)
    rel = case.relative_to(workdir).as_posix()

    hpart_cmd = f"hpart -N {parts}"
    if parts_dir is not None:
        part_file = parts_dir / f"{name}.part"
        if save_parts:
            hpart_cmd += f" --save-part {part_file}"
        elif part_file.exists():
            hpart_cmd += f" --load-part {part_file}"

    before_blif = after_blif = None
    cec_segment = cec_tail = ""
    if do_cec:
        before_fd, before_path = tempfile.mkstemp(suffix=".blif", prefix=f"csr_{name}_before_")
        after_fd, after_path = tempfile.mkstemp(suffix=".blif", prefix=f"csr_{name}_after_")
        os.close(before_fd)
        os.close(after_fd)
        before_blif, after_blif = Path(before_path), Path(after_path)
        cec_segment = f"; write {before_blif}"
        cec_tail = f"; write {after_blif}; cec {before_blif} {after_blif}"

    command = build_command(rel, parts, hpart_cmd, csr_args, cec_segment, cec_tail)

    try:
        outcome = run_foxsyn(foxsyn, workdir, command, timeout)
    finally:
        if before_blif is not None:
            before_blif.unlink(missing_ok=True)
        if after_blif is not None:
            after_blif.unlink(missing_ok=True)

    if outcome is None:
        return Result(case=name, status="TIMEOUT")

    elapsed, output, returncode = outcome
    result = parse_output(output, case=name)
    result.sec = f"{elapsed:.2f}"
    if returncode == 0:
        result.status = "OK"
    return result


def run_baseline_once(baseline_foxsyn: Path, workdir: Path, case: Path, parts: int,
                      csr_args: str, timeout: int, parts_dir: Path | None) -> Result | None:
    """Run the pre-enhancement CSR binary; strips -T (unknown to that build)."""
    baseline_args = BASELINE_STRIP_RE.sub("", csr_args).strip()
    result = run_case_once(baseline_foxsyn, workdir, case, parts, baseline_args,
                           timeout, do_cec=False, parts_dir=parts_dir, save_parts=False)
    return result if result.status == "OK" else None


def run_case(foxsyn: Path, workdir: Path, case: Path, parts: int,
             csr_args: str, timeout: int, runs: int, do_cec: bool,
             parts_dir: Path | None, save_parts: bool,
             exact_repeats: int, baseline_foxsyn: Path | None,
             baseline_results: dict) -> Result:
    best: Result | None = None
    all_runs: list[Result] = []

    repeat_count = max(runs, exact_repeats, 1)
    for _ in range(repeat_count):
        r = run_case_once(foxsyn, workdir, case, parts, csr_args, timeout,
                          do_cec, parts_dir=parts_dir, save_parts=save_parts)
        all_runs.append(r)
        if best is None:
            best = r
        elif r.status == "OK" and r.gain != "-":
            if best.gain == "-" or int(r.gain) > int(best.gain):
                best = r

    assert best is not None
    ok_runs = [r for r in all_runs if r.status == "OK"]
    if exact_repeats > 1:
        # Determinism check must compare every repeat, never cherry-pick the best.
        best.deterministic = (
            len(ok_runs) == len(all_runs)
            and all(summaries_match(ok_runs[0], r) for r in ok_runs[1:])
        )

    name = case_key(case)
    if baseline_results:
        entry = baseline_results.get(name)
        if entry:
            best.baseline_cut_after = str(entry.get("cut_after", "-"))
            baseline_sec = entry.get("sec")
            if baseline_sec and best.status == "OK" and best.sec != "-":
                best.runtime_ratio = f"{float(best.sec) / float(baseline_sec):.2f}"
    elif baseline_foxsyn is not None and best.status == "OK":
        baseline_result = run_baseline_once(baseline_foxsyn, workdir, case, parts,
                                            csr_args, timeout, parts_dir)
        if baseline_result is not None:
            best.baseline_cut_after = baseline_result.cut_after
            if baseline_result.sec != "-" and best.sec != "-" and float(baseline_result.sec) > 0:
                best.runtime_ratio = f"{float(best.sec) / float(baseline_result.sec):.2f}"

    return best


def main() -> int:
    parser = argparse.ArgumentParser(description="Test csr on benchmarks")
    parser.add_argument("--self-test", action="store_true",
                        help="Run parse_output against representative output and exit")
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
    parser.add_argument("--exact-repeats", type=int, default=1,
                        help="Run each case N times on a frozen partition and require "
                             "byte-for-byte identical summaries (ignoring elapsed seconds); "
                             "never selects the best run for the determinism verdict")
    parser.add_argument("--cec", action="store_true",
                        help="Write BLIF before/after csr and run cec for functional equivalence")
    parser.add_argument("--save-parts-dir", type=Path, default=None,
                        help="Save hmetis partition results to this directory for reproducible runs")
    parser.add_argument("--load-parts-dir", type=Path, default=None,
                        help="Load partition results from this directory instead of running hmetis")
    parser.add_argument("--baseline-foxsyn", type=Path, default=None,
                        help="Path to the pre-enhancement CSR binary; runs it on the same "
                             "frozen partition for cut-edge/runtime comparison")
    parser.add_argument("--baseline-results", type=Path, default=None,
                        help="JSON file of {case: {cut_after, sec}} from a prior "
                             "--write-results run; used instead of re-running a baseline binary")
    parser.add_argument("--write-results", type=Path, default=None,
                        help="Write per-case {cut_after, sec} results to this JSON file")
    args = parser.parse_args()

    if args.self_test:
        return run_self_test()

    workdir = Path.cwd()
    foxsyn = args.foxsyn if args.foxsyn.is_absolute() else (workdir / args.foxsyn).resolve()
    cases_root = args.cases_root if args.cases_root.is_absolute() else (workdir / args.cases_root).resolve()

    if not foxsyn.is_file():
        print(f"error: FoxSYN not found: {foxsyn}", file=sys.stderr)
        return 1

    if args.baseline_foxsyn is not None and not args.baseline_foxsyn.is_file():
        print(f"error: baseline FoxSYN not found: {args.baseline_foxsyn}", file=sys.stderr)
        return 1

    baseline_results: dict = {}
    if args.baseline_results is not None:
        if not args.baseline_results.is_file():
            print(f"error: baseline results file not found: {args.baseline_results}", file=sys.stderr)
            return 1
        baseline_results = json.loads(args.baseline_results.read_text())

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
          f"cec={'on' if args.cec else 'off'}, exact_repeats={args.exact_repeats}, cases={len(cases)}")
    if parts_dir:
        mode = "saving" if save_parts else "loading"
        print(f"# partition {mode}: {parts_dir}")
    if args.baseline_foxsyn:
        print(f"# baseline: {args.baseline_foxsyn}")
    if baseline_results:
        print(f"# baseline results loaded: {args.baseline_results} ({len(baseline_results)} cases)")
    print(f"{'Case':<16} {'NdBef':>6} {'NdAft':>6} {'HopBef':>6} {'HopAft':>6} "
          f"{'CutEdgeB':>8} {'CutEdgeA':>8} {'Gain':>6} {'Cons':>5} {'Det':>5} "
          f"{'BaseA':>6} {'RtRatio':>7} {'CEC':>7} {'Time':>6} {'Status':<8}")
    print("-" * 130)

    results: list[Result] = []
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(run_case, foxsyn, workdir, c, args.parts, args.csr_args,
                            args.timeout, args.runs, args.cec, parts_dir, save_parts,
                            args.exact_repeats, args.baseline_foxsyn, baseline_results): c
            for c in cases
        }
        for future in as_completed(futures):
            r = future.result()
            results.append(r)
            det = "-" if args.exact_repeats <= 1 else ("Y" if r.deterministic else "N")
            print(f"{r.case:<16} {r.nodes_before:>6} {r.nodes_after:>6} "
                  f"{r.hop_before:>6} {r.hop_after:>6} "
                  f"{r.cut_before:>8} {r.cut_after:>8} {r.gain:>6} {r.constraints:>5} {det:>5} "
                  f"{r.baseline_cut_after:>6} {r.runtime_ratio:>7} {r.cec:>7} "
                  f"{r.sec:>5}s {r.status:<8}")
            sys.stdout.flush()

    print("-" * 130)
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
    area_total_before = sum(int(r.nodes_before) for r in ok if r.nodes_before != "-")
    area_total_after = sum(int(r.nodes_after) for r in ok if r.nodes_after != "-")
    area_grown = [r for r in ok if r.nodes_before != "-" and r.nodes_after != "-"
                  and int(r.nodes_after) > int(r.nodes_before)]
    hop_total_before = sum(int(r.hop_before) for r in ok if r.hop_before != "-")
    hop_total_after = sum(int(r.hop_after) for r in ok if r.hop_after != "-")
    print(f"\nArea (node count) across OK cases: {area_total_before} -> {area_total_after} "
          f"({len(area_grown)}/{len(ok)} cases grew)")
    print(f"Hop count across OK cases: {hop_total_before} -> {hop_total_after}")

    violations = [r for r in ok if r.constraints == "FAIL"]
    print(f"\nConstraint violations: {len(violations)}/{len(ok)}")
    if violations:
        print("!!! CONSTRAINT VIOLATIONS: " + ", ".join(r.case for r in violations))

    if args.exact_repeats > 1:
        nondeterministic = [r for r in ok if not r.deterministic]
        print(f"Non-deterministic cases ({args.exact_repeats} repeats): {len(nondeterministic)}/{len(ok)}")
        if nondeterministic:
            print("!!! NON-DETERMINISTIC: " + ", ".join(r.case for r in nondeterministic))

    if args.cec:
        eq = [r for r in ok if r.cec == "EQ"]
        not_eq = [r for r in ok if r.cec == "NOT_EQ"]
        na = [r for r in ok if r.cec == "N/A"]
        print(f"\ncec: EQ={len(eq)}  NOT_EQ={len(not_eq)}  N/A={len(na)}")
        if not_eq:
            print("!!! FUNCTIONAL MISMATCH: " + ", ".join(r.case for r in not_eq))

    ratios = [(r.case, float(r.runtime_ratio)) for r in ok if r.runtime_ratio != "-"]
    if ratios:
        geomean = math.exp(sum(math.log(v) for _, v in ratios) / len(ratios))
        over_5x = [case for case, v in ratios if v > 5.0]
        print(f"\nRuntime ratio (new/baseline) geometric mean: {geomean:.2f}x "
              f"over {len(ratios)} case(s)")
        if over_5x:
            print("!!! RUNTIME RATIO > 5x: " + ", ".join(over_5x))

    if crashes:
        print(f"\ncrash/fail cases: {', '.join(r.case for r in crashes)}")

    if args.write_results:
        payload = {
            r.case: {"cut_after": r.cut_after, "sec": r.sec}
            for r in ok
        }
        args.write_results.write_text(json.dumps(payload, indent=2, sort_keys=True))
        print(f"\nwrote results: {args.write_results}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
