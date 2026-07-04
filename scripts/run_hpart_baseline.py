#!/usr/bin/env python3
"""
hpart baseline harness.

hmetis has internal randomness, so re-running hpart on the same input can
change cut/hop numbers between runs. To get a baseline that later hpart
changes can be compared against, this script does two things per case:

  1. Runs hpart once and freezes the resulting partition to a `.part` file
     via `--save-part` (so future regressions can `--load-part` the exact
     same partition and isolate hpart-algorithm changes from hmetis noise).
  2. Records the resulting cut/hop/area/edge metrics into a JSON baseline
     file for later comparison.

Usage:
  # Capture baseline (freezes partitions + saves metrics JSON):
  python3 scripts/run_hpart_baseline.py --save-baseline regression/hpart_baseline.json

  # Later, after changing hpart's algorithm, load the same frozen partitions
  # and compare metrics against the baseline:
  python3 scripts/run_hpart_baseline.py --compare regression/hpart_baseline.json
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import asdict, dataclass
from pathlib import Path

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
HPART_SUMMARY_RE = re.compile(
    r"tool = (?P<tool>\S+), parts = (?P<parts>\d+), cut size = (?P<cut>\d+)"
)


@dataclass
class Result:
    case: str
    parts: int
    cut: int = -1
    hop: int = -1
    area: int = -1
    edge: int = -1
    sec: float = -1.0
    status: str = "FAIL"


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def case_key(cases_root: Path, case: Path) -> str:
    rel = case.relative_to(cases_root).with_suffix("")
    return rel.as_posix().replace("/", "__")


def run_one(foxsyn: Path, workdir: Path, case: Path, key: str, parts: int,
            base: str, tool: str, timeout: int,
            part_file: Path | None, load: bool) -> Result:
    rel_case = case.relative_to(workdir).as_posix()
    hpart_cmd = f"hpart -T {tool} -N {parts}"
    if part_file is not None:
        hpart_cmd += f" {'--load-part' if load else '--save-part'} {part_file}"
    command = f"read {rel_case}; st; {base}; {hpart_cmd}; ps"

    r = Result(case=key, parts=parts)
    start = time.perf_counter()
    try:
        proc = subprocess.run(
            [str(foxsyn), "-c", command],
            cwd=workdir, capture_output=True, text=True,
            timeout=timeout, check=False,
        )
    except subprocess.TimeoutExpired:
        r.sec = time.perf_counter() - start
        r.status = "TIMEOUT"
        return r

    r.sec = time.perf_counter() - start
    out = strip_ansi(proc.stdout + proc.stderr)

    m = HPART_SUMMARY_RE.search(out)
    if m:
        r.cut = int(m.group("cut"))

    pdb_lines = [l for l in out.splitlines() if l.lstrip().startswith("pdb")]
    if pdb_lines:
        hop_m = re.search(r"\bhop =\s*(\d+)", pdb_lines[-1])
        if hop_m:
            r.hop = int(hop_m.group(1))

    stat_lines = [l for l in out.splitlines() if " nd =" in l]
    if stat_lines:
        area_m = re.search(r"\bnd =\s*(\d+)", stat_lines[-1])
        edge_m = re.search(r"\bedge =\s*(\d+)", stat_lines[-1])
        if area_m:
            r.area = int(area_m.group(1))
        if edge_m:
            r.edge = int(edge_m.group(1))

    if proc.returncode == 0 and m:
        r.status = "OK"
    return r


def pct(new: float, old: float) -> str:
    if old <= 0:
        return "  n/a"
    delta = (new - old) / old * 100.0
    sign = "+" if delta >= 0 else ""
    return f"{sign}{delta:.1f}%"


def compare(baseline: list[dict], current: list[Result]) -> int:
    base_map = {(b["case"], b["parts"]): b for b in baseline}
    regressions = 0
    cols = (f"{'Case':<24} {'N':>3} {'Cut_b':>6} {'Cut_n':>6} {'Cut%':>7}  "
            f"{'Hop_b':>5} {'Hop_n':>5} {'Hop%':>7}  {'Status'}")
    print(cols)
    print("-" * len(cols))
    for r in sorted(current, key=lambda x: (x.case, x.parts)):
        b = base_map.get((r.case, r.parts))
        if b is None:
            print(f"{r.case:<24} {r.parts:>3} {'(new)':>6} {r.cut:>6}        "
                  f"{'':>5} {r.hop:>5}        {r.status} NEW")
            continue
        reg = ""
        if r.cut > b["cut"] * 1.001:
            reg += " CUT_UP"
            regressions += 1
        if r.hop > b["hop"] * 1.001:
            reg += " HOP_UP"
            regressions += 1
        print(f"{r.case:<24} {r.parts:>3} {b['cut']:>6} {r.cut:>6} {pct(r.cut, b['cut']):>7}  "
              f"{b['hop']:>5} {r.hop:>5} {pct(r.hop, b['hop']):>7}  {r.status}{reg}")
    print("-" * len(cols))
    return regressions


def main() -> int:
    parser = argparse.ArgumentParser(description="hpart baseline harness")
    parser.add_argument("--foxsyn", type=Path, default=Path("./FoxSYN"))
    parser.add_argument("--cases-root", type=Path, default=Path("./SimpleCircuits"))
    parser.add_argument("--parts-list", type=int, nargs="+", default=[4, 16])
    parser.add_argument("--base", default="if -K 6",
                         help="Commands run between `st` and `hpart`.")
    parser.add_argument("-T", "--tool", default="hmetis",
                         choices=("hmetis", "shmetis", "kmetis"))
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("-j", "--jobs", type=int,
                         default=max(1, min(os.cpu_count() or 4, 8)))
    parser.add_argument("--match", default="")
    parser.add_argument("--parts-dir", type=Path, default=Path("regression/parts"),
                         help="Directory to save/load frozen .part files (not tracked by git).")
    parser.add_argument("--save-baseline", type=Path, default=None,
                         help="Freeze partitions under --parts-dir and save metrics JSON here.")
    parser.add_argument("--compare", type=Path, default=None,
                         help="Load frozen partitions from --parts-dir and compare metrics "
                              "against this baseline JSON.")
    args = parser.parse_args()

    if args.save_baseline is None and args.compare is None:
        print("error: pass either --save-baseline or --compare", file=sys.stderr)
        return 1

    workdir = Path.cwd()
    foxsyn = args.foxsyn if args.foxsyn.is_absolute() else (workdir / args.foxsyn).resolve()
    cases_root = args.cases_root if args.cases_root.is_absolute() else (workdir / args.cases_root).resolve()
    parts_dir = args.parts_dir if args.parts_dir.is_absolute() else (workdir / args.parts_dir).resolve()

    if not foxsyn.is_file():
        print(f"error: FoxSYN not found: {foxsyn}", file=sys.stderr)
        return 1
    if not cases_root.is_dir():
        print(f"error: cases root not found: {cases_root}", file=sys.stderr)
        return 1

    load = args.compare is not None
    cases = sorted(p for p in cases_root.rglob("*") if p.suffix in {".v", ".blif"} and p.is_file())
    cases = [c for c in cases if "mapped" not in c.stem]
    if args.match:
        cases = [c for c in cases if args.match in c.as_posix()]
    if not cases:
        print("error: no testcase matched", file=sys.stderr)
        return 1

    parts_list = sorted(dict.fromkeys(args.parts_list))
    tasks = [(c, case_key(cases_root, c), n) for c in cases for n in parts_list]

    mode = "loading" if load else "saving"
    print(f"# hpart baseline: tool={args.tool}, base=`{args.base}`, "
          f"parts={','.join(map(str, parts_list))}, cases={len(cases)}, "
          f"partitions {mode} under {parts_dir}")

    results: list[Result] = []
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {}
        for case, key, n in tasks:
            part_file = parts_dir / f"N{n}" / f"{key}.part"
            if not load:
                part_file.parent.mkdir(parents=True, exist_ok=True)
            futures[executor.submit(run_one, foxsyn, workdir, case, key, n,
                                     args.base, args.tool, args.timeout,
                                     part_file, load)] = (key, n)
        done = 0
        for future in as_completed(futures):
            r = future.result()
            results.append(r)
            done += 1
            print(f"[{done:>3}/{len(tasks)}] {r.case:<24} N={r.parts:<3} "
                  f"-> {r.status} cut={r.cut} hop={r.hop} area={r.area} edge={r.edge} "
                  f"time={r.sec:.2f}s", file=sys.stderr, flush=True)

    fails = [r for r in results if r.status != "OK"]
    print(f"\nsummary: ok={len(results) - len(fails)}/{len(results)}")
    if fails:
        print("failed cases: " + ", ".join(f"{r.case}(N={r.parts})" for r in fails))

    if args.save_baseline:
        data = [asdict(r) for r in results]
        args.save_baseline.write_text(json.dumps(data, indent=2))
        print(f"baseline saved to {args.save_baseline} ({len(data)} records)")
        print(f"partitions frozen under {parts_dir}")

    if args.compare:
        if not args.compare.exists():
            print(f"error: baseline file not found: {args.compare}", file=sys.stderr)
            return 1
        baseline = json.loads(args.compare.read_text())
        print(f"\n=== comparison against {args.compare} ===\n")
        regressions = compare(baseline, results)
        if regressions:
            print(f"\n*** {regressions} regression(s) detected ***")
            return 1
        print("\nno regressions.")

    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
