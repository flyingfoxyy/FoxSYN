#!/usr/bin/env python3
"""Compare csr gain with and without a pdecomp -K 2 pre-pass, holding the
partition fixed (via hpart --save-part/--load-part) so the comparison
isolates node-granularity effects from hMetis run-to-run randomness.

For each case:
  1. read; st; if -K 6; hpart -N <parts> --save-part <file>   (one hMetis run, frozen)
  2. baseline:  --load-part <file>; csr -v
  3. pdecomp:   --load-part <file>; pdecomp -K 2; csr -v

csr itself has no RNG given a fixed partition (verified: repeated runs on
the same --load-part produce byte-identical output), so each branch only
needs one run per case.
"""

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

CSR_CUTEDGE_RE = re.compile(
    r"csr:\s+cut-edges\s+(\d+)\s+->\s+(\d+)\s+\((?:after phase0=\d+,\s+)?"
    r"after phase1=(\d+),\s+after phase2=(\d+)\)"
)
PDECOMP_OK_RE = re.compile(r"pdecomp:\s+decomposed to K<=(\d+)\s+\(hop=(\d+),\s+cut-edge=(\d+)\s+unchanged\)")
PDECOMP_FAIL_RE = re.compile(r"pdecomp:\s+partition invariant violated")
CEC_RE = re.compile(r"Networks are (NOT )?EQUIVALENT", re.IGNORECASE)


@dataclass
class Result:
    case: str
    cut_before: str = "-"
    cut_after: str = "-"
    gain_pct: str = "-"
    cec: str = "-"
    sec: str = "-"
    status: str = "FAIL"
    note: str = ""


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def run_branch(foxsyn: Path, workdir: Path, case: Path, part_file: Path,
                use_pdecomp: bool, timeout: int, do_cec: bool) -> Result:
    name = case.stem
    rel = case.relative_to(workdir).as_posix()

    before_blif = after_blif = None
    cec_segment = ""
    cec_tail = ""
    if do_cec:
        before_fd, before_path = tempfile.mkstemp(suffix=".blif", prefix=f"pdc_{name}_before_")
        after_fd, after_path = tempfile.mkstemp(suffix=".blif", prefix=f"pdc_{name}_after_")
        os.close(before_fd)
        os.close(after_fd)
        before_blif, after_blif = Path(before_path), Path(after_path)
        cec_segment = f"; write {before_blif}"
        cec_tail = f"; write {after_blif}; cec {before_blif} {after_blif}"

    pdecomp_segment = "; pdecomp -K 2" if use_pdecomp else ""
    command = (
        f"read {rel}; st; if -K 6; hpart -N 4 --load-part {part_file}{pdecomp_segment}"
        f"{cec_segment}; csr -v{cec_tail}"
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

    if use_pdecomp:
        if PDECOMP_FAIL_RE.search(output):
            result.status = "PDECOMP_FAIL"
            result.note = "invariant violated"
            return result
        if not PDECOMP_OK_RE.search(output):
            result.status = "PDECOMP_MISSING"
            result.note = "no pdecomp success marker found"
            return result

    m = CSR_CUTEDGE_RE.search(output)
    if m:
        result.cut_before = m.group(1)
        result.cut_after = m.group(2)
        before_i, after_i = int(m.group(1)), int(m.group(2))
        if before_i > 0:
            result.gain_pct = f"{(before_i - after_i) / before_i * 100:.2f}"

    if do_cec:
        m = CEC_RE.search(output)
        if m:
            result.cec = "NOT_EQ" if m.group(1) else "EQ"
        else:
            result.cec = "N/A"

    if proc.returncode == 0 and m:
        result.status = "OK"
    elif result.status == "FAIL":
        result.note = "no cut-edge line found" if proc.returncode == 0 else f"exit {proc.returncode}"

    return result


def run_case(foxsyn: Path, workdir: Path, case: Path, parts_dir: Path,
             timeout: int, do_cec: bool) -> tuple[Result, Result]:
    name = case.stem
    part_file = parts_dir / f"{name}.part"

    if not part_file.exists():
        save_cmd = (
            f"read {case.relative_to(workdir).as_posix()}; st; if -K 6; "
            f"hpart -N 4 --save-part {part_file}"
        )
        subprocess.run([str(foxsyn), "-c", save_cmd], cwd=workdir,
                        capture_output=True, text=True, timeout=timeout, check=False)

    if not part_file.exists():
        fail = Result(case=name, status="FAIL", note="could not create partition file")
        return fail, fail

    baseline = run_branch(foxsyn, workdir, case, part_file, False, timeout, do_cec)
    pdecomp = run_branch(foxsyn, workdir, case, part_file, True, timeout, do_cec)
    return baseline, pdecomp


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare csr with/without pdecomp -K 2 pre-pass")
    parser.add_argument("--foxsyn", type=Path, default=Path("./FoxSYN"))
    parser.add_argument("--cases-root", type=Path, default=Path("./SimpleCircuits/EPFL"))
    parser.add_argument("--parts-dir", type=Path, default=Path("./parts_pdecomp_cmp"))
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("-j", "--jobs", type=int, default=max(1, min(os.cpu_count() or 4, 8)))
    parser.add_argument("--match", default="")
    parser.add_argument("--cec", action="store_true")
    args = parser.parse_args()

    workdir = Path.cwd()
    foxsyn = args.foxsyn if args.foxsyn.is_absolute() else (workdir / args.foxsyn).resolve()
    cases_root = args.cases_root if args.cases_root.is_absolute() else (workdir / args.cases_root).resolve()
    parts_dir = args.parts_dir if args.parts_dir.is_absolute() else (workdir / args.parts_dir).resolve()
    parts_dir.mkdir(parents=True, exist_ok=True)

    if not foxsyn.is_file():
        print(f"error: FoxSYN not found: {foxsyn}", file=sys.stderr)
        return 1

    cases = sorted(p for p in cases_root.rglob("*.v") if "mapped" not in p.stem)
    if args.match:
        cases = [c for c in cases if args.match in c.as_posix()]
    if not cases:
        print("error: no cases found", file=sys.stderr)
        return 1

    print(f"# pdecomp+csr comparison: cases={len(cases)}, partition frozen via --save-part/--load-part")
    print(f"{'Case':<14} {'Base%':>8} {'Pdec%':>8} {'Delta':>8} {'PdecStatus':<14} {'CEC(b/p)':>10}")
    print("-" * 70)

    rows: list[tuple[str, Result, Result]] = []
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(run_case, foxsyn, workdir, c, parts_dir, args.timeout, args.cec): c
            for c in cases
        }
        for future in as_completed(futures):
            baseline, pdecomp = future.result()
            rows.append((baseline.case, baseline, pdecomp))

    rows.sort(key=lambda r: r[0])
    for name, base, pdec in rows:
        delta = "-"
        if base.gain_pct != "-" and pdec.gain_pct != "-":
            delta = f"{float(pdec.gain_pct) - float(base.gain_pct):+.2f}"
        cec_str = f"{base.cec}/{pdec.cec}" if args.cec else "-"
        print(f"{name:<14} {base.gain_pct:>7}% {pdec.gain_pct:>7}% {delta:>8} "
              f"{pdec.status:<14} {cec_str:>10}")

    print("-" * 70)
    valid = [(n, b, p) for n, b, p in rows if b.gain_pct != "-" and p.gain_pct != "-"]
    if valid:
        avg_base = sum(float(b.gain_pct) for _, b, _ in valid) / len(valid)
        avg_pdec = sum(float(p.gain_pct) for _, _, p in valid) / len(valid)
        print(f"\nAverage cut-edge reduction: baseline={avg_base:.2f}%  "
              f"pdecomp+csr={avg_pdec:.2f}%  delta={avg_pdec-avg_base:+.2f}pp  (n={len(valid)})")

    fails = [(n, p) for n, _, p in rows if p.status not in ("OK",)]
    if fails:
        print(f"\npdecomp-side issues: " + ", ".join(f"{n}({p.status})" for n, p in fails))

    return 0


if __name__ == "__main__":
    sys.exit(main())
