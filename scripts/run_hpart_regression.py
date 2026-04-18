#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path


ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
HPART_SUMMARY_RE = re.compile(
    r"tool = (?P<tool>\S+), parts = (?P<parts>\d+), cut size = (?P<cut>\d+)"
)
IF_VERBOSE_RE = re.compile(
    r"^[A-Z]:\s+Del =\s*(?P<delay>[0-9.]+)\.\s+Ar =\s*(?P<area>[0-9.]+)\.\s+"
    r"Edge =\s*(?P<edge>[0-9.]+)\.\s+Cut =\s*(?P<cut>[0-9.]+)\.\s+T =\s*(?P<time>[0-9.]+) sec$",
    re.MULTILINE,
)
ERROR_RE = re.compile(r"^Error:\s*(?P<message>.+)$", re.MULTILINE)


@dataclass(frozen=True)
class RunSpec:
    flow: str
    command: str
    parts: int


@dataclass(frozen=True)
class Job:
    case: Path
    rel_case: Path
    display_case: str
    spec: RunSpec


@dataclass
class JobResult:
    case: str
    flow: str
    parts: int
    cut: str = "-"
    area: str = "-"
    edge: str = "-"
    hop: str = "-"
    tool: str = "-"
    sec: str = "-"
    status: str = "FAIL"
    note: str = "-"

    @property
    def row_label(self) -> str:
        return f"{self.flow} N={self.parts:>2}"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run base/sota flows with hpart on SimpleCircuits and print compact result tables."
    )
    parser.add_argument(
        "--foxsyn",
        type=Path,
        default=Path("./FoxSYN"),
        help="FoxSYN executable path, resolved from the current working directory.",
    )
    parser.add_argument(
        "--cases-root",
        type=Path,
        default=Path("./SimpleCircuits"),
        help="Root directory containing regression cases.",
    )
    parser.add_argument(
        "--parts-list",
        type=int,
        nargs="+",
        default=[4, 16],
        help="Partition counts to compare. Default: 4 16.",
    )
    parser.add_argument(
        "--base",
        default="if -K 6 -v",
        help="Command sequence inserted between `st` and `hpart` for the base flow.",
    )
    parser.add_argument(
        "--sota",
        default="if -K 6 -v",
        help="Command sequence inserted between `st` and `hpart` for the sota flow.",
    )
    parser.add_argument(
        "-T",
        "--tool",
        default="hmetis",
        choices=("hmetis", "shmetis", "kmetis"),
        help="Partitioner passed to hpart.",
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=max(1, min((os.cpu_count() or 4), 8)),
        help="Maximum concurrent jobs. Default: min(cpu_count, 8).",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=600,
        help="Per-job timeout in seconds.",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Only run the first N cases after sorting. 0 means all.",
    )
    parser.add_argument(
        "--match",
        default="",
        help="Only run cases whose relative path contains this substring.",
    )
    parser.add_argument(
        "--cases-per-table",
        type=int,
        default=4,
        help="How many testcases to show in each table. Default: 4.",
    )
    return parser.parse_args()


def trim_note(note: str, width: int = 52) -> str:
    if len(note) <= width:
        return note
    return note[: width - 3] + "..."


def format_num(text: str) -> str:
    try:
        value = float(text)
    except ValueError:
        return text
    if value.is_integer():
        return str(int(value))
    return f"{value:.2f}".rstrip("0").rstrip(".")


def resolve_path(path: Path, base: Path) -> Path:
    return path if path.is_absolute() else (base / path).resolve()


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def find_cases(cases_root: Path, match: str, limit: int) -> list[Path]:
    cases = sorted(
        path for path in cases_root.rglob("*") if path.suffix in {".v", ".blif"} and path.is_file()
    )
    if match:
        cases = [path for path in cases if match in path.as_posix()]
    if limit > 0:
        cases = cases[:limit]
    return cases


def make_display_case(cases_root: Path, case_path: Path) -> str:
    rel = case_path.relative_to(cases_root).as_posix()
    suffix = case_path.suffix
    return rel[: -len(suffix)] if suffix else rel


def parse_stats_line(output: str) -> tuple[str, str, str]:
    clean = strip_ansi(output)
    lines = [line.rstrip() for line in clean.splitlines()]
    stats_lines = [
        line for line in lines
        if ":" in line and not line.startswith("ABC command line:") and not line.lstrip().startswith("pdb")
    ]
    pdb_lines = [line for line in lines if line.lstrip().startswith("pdb")]
    area = "-"
    edge = "-"
    hop = "-"
    if stats_lines:
        stats_line = stats_lines[-1]
        area_match = re.search(r"\barea =\s*([0-9.]+)", stats_line)
        if area_match is None:
            area_match = re.search(r"\bnd =\s*([0-9.]+)", stats_line)
        if area_match is None:
            area_match = re.search(r"\band =\s*([0-9.]+)", stats_line)
        edge_match = re.search(r"\bedge =\s*([0-9.]+)", stats_line)
        hop_match = re.search(r"\bhop =\s*([0-9.]+)", stats_line)
        if area_match:
            area = format_num(area_match.group(1))
        if edge_match:
            edge = format_num(edge_match.group(1))
        if hop_match:
            hop = format_num(hop_match.group(1))
    if hop == "-" and pdb_lines:
        hop_match = re.search(r"\bhop =\s*([0-9.]+)", pdb_lines[-1])
        if hop_match:
            hop = format_num(hop_match.group(1))
    if area == "-" or edge == "-":
        verbose_matches = list(IF_VERBOSE_RE.finditer(clean))
        if verbose_matches:
            last = verbose_matches[-1]
            if area == "-":
                area = format_num(last.group("area"))
            if edge == "-":
                edge = format_num(last.group("edge"))
    if area == "-" or edge == "-":
        for stats_line in reversed(stats_lines):
            if area == "-":
                area_match = re.search(r"\bnd =\s*([0-9.]+)", stats_line)
                if area_match is None:
                    area_match = re.search(r"\band =\s*([0-9.]+)", stats_line)
                if area_match:
                    area = format_num(area_match.group(1))
            if edge == "-":
                edge_match = re.search(r"\bedge =\s*([0-9.]+)", stats_line)
                if edge_match:
                    edge = format_num(edge_match.group(1))
            if area != "-" and edge != "-":
                break
    if hop == "-":
        hop_match = re.search(r"\bhop =\s*([0-9.]+)", clean)
        if hop_match:
            hop = format_num(hop_match.group(1))
    return area, edge, hop


def run_job(foxsyn: Path, workdir: Path, tool: str, timeout: int, job: Job) -> JobResult:
    command = (
        f"read {job.rel_case.as_posix()}; st; {job.spec.command}; "
        f"hpart -T {tool} -N {job.spec.parts}; ps"
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
        return JobResult(
            case=job.display_case,
            flow=job.spec.flow,
            parts=job.spec.parts,
            sec=f"{time.perf_counter() - start:.2f}",
            note=f"timeout after {timeout}s",
        )

    output = proc.stdout + proc.stderr
    result = JobResult(
        case=job.display_case,
        flow=job.spec.flow,
        parts=job.spec.parts,
        sec=f"{time.perf_counter() - start:.2f}",
        note=f"exit={proc.returncode}" if proc.returncode else "-",
    )

    summary = HPART_SUMMARY_RE.search(output)
    if summary:
        result.tool = summary.group("tool")
        result.cut = summary.group("cut")

    result.area, result.edge, result.hop = parse_stats_line(output)

    error = ERROR_RE.search(strip_ansi(output))
    if error:
        result.note = trim_note(error.group("message").strip())
    elif proc.returncode != 0:
        result.note = trim_note(result.note)

    result.status = "OK" if proc.returncode == 0 and summary else "FAIL"
    return result


def print_progress(done: int, total: int, result: JobResult) -> None:
    print(
        f"[{done:>3}/{total}] {result.row_label:<10} {result.case}"
        f" -> {result.status} cut={result.cut} area={result.area} edge={result.edge} hop={result.hop} time={result.sec}s",
        file=sys.stderr,
        flush=True,
    )


def chunked(values: list[str], size: int) -> list[list[str]]:
    return [values[index : index + size] for index in range(0, len(values), size)]


def build_tables(
    case_order: list[str],
    results: dict[tuple[str, str, int], JobResult],
    specs: list[RunSpec],
    cases_per_table: int,
) -> list[str]:
    area_width = 10
    edge_width = 11
    hop_width = 5
    cut_width = 8
    setting_width = max(len("setting"), max(len(f"{spec.flow} N={spec.parts:>2}") for spec in specs))

    def format_metric_row(area: str, edge: str, hop: str, cut: str) -> str:
        return f"{area:>{area_width}} {edge:>{edge_width}} {hop:>{hop_width}} {cut:>{cut_width}}"

    metrics_width = len(format_metric_row("Area", "Edge", "Hop", "Cutsize"))
    terminal_width = shutil.get_terminal_size((160, 24)).columns
    max_cases_fit = cases_per_table
    while max_cases_fit > 1:
        table_width = setting_width + 3 + max_cases_fit * metrics_width + (max_cases_fit - 1) * 4
        if table_width <= terminal_width:
            break
        max_cases_fit -= 1
    cases_per_table = max(1, min(cases_per_table, max_cases_fit))

    tables: list[str] = []
    for case_chunk in chunked(case_order, cases_per_table):
        header_top = f"{'setting':<{setting_width}} | " + " || ".join(
            f"{case_name:^{metrics_width}}" for case_name in case_chunk
        )
        header_bottom = f"{'':<{setting_width}} | " + " || ".join(
            format_metric_row("Area", "Edge", "Hop", "Cutsize") for _ in case_chunk
        )
        separator = f"{'-' * setting_width}-+-" + "-||-".join(
            "-" * metrics_width for _ in case_chunk
        )

        lines = [header_top, header_bottom, separator]
        for spec in specs:
            row_cells: list[str] = []
            for case_name in case_chunk:
                result = results.get((case_name, spec.flow, spec.parts))
                if result is None:
                    row_cells.append(format_metric_row("-", "-", "-", "-"))
                elif result.status != "OK":
                    row_cells.append(format_metric_row("ERR", "ERR", "ERR", "ERR"))
                else:
                    row_cells.append(format_metric_row(result.area, result.edge, result.hop, result.cut))
            lines.append(f"{f'{spec.flow} N={spec.parts:>2}':<{setting_width}} | " + " || ".join(row_cells))

        border = "-" * max(len(line) for line in lines)
        tables.append("\n".join([border, *lines, border]))
    return tables


def main() -> int:
    args = parse_args()
    workdir = Path.cwd()
    foxsyn = resolve_path(args.foxsyn, workdir)
    cases_root = resolve_path(args.cases_root, workdir)
    parts_list = sorted(dict.fromkeys(args.parts_list))

    if not foxsyn.is_file():
        print(f"error: FoxSYN not found: {foxsyn}", file=sys.stderr)
        return 1
    if not cases_root.is_dir():
        print(f"error: cases root not found: {cases_root}", file=sys.stderr)
        return 1
    if args.cases_per_table <= 0:
        print("error: --cases-per-table must be positive", file=sys.stderr)
        return 1

    cases = find_cases(cases_root, args.match, args.limit)
    if not cases:
        print("error: no testcase matched", file=sys.stderr)
        return 1

    specs: list[RunSpec] = []
    for flow, command in (("base", args.base), ("sota", args.sota)):
        for parts in parts_list:
            specs.append(RunSpec(flow=flow, command=command, parts=parts))

    jobs: list[Job] = []
    case_order: list[str] = []
    for case in cases:
        rel_case = case.relative_to(workdir)
        display_case = make_display_case(cases_root, case)
        case_order.append(display_case)
        for spec in specs:
            jobs.append(
                Job(
                    case=case,
                    rel_case=rel_case,
                    display_case=display_case,
                    spec=spec,
                )
            )

    total_jobs = len(jobs)
    results: dict[tuple[str, str, int], JobResult] = {}
    done = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        future_map = {
            executor.submit(run_job, foxsyn, workdir, args.tool, args.timeout, job): job
            for job in jobs
        }
        for future in as_completed(future_map):
            result = future.result()
            results[(result.case, result.flow, result.parts)] = result
            done += 1
            print_progress(done, total_jobs, result)

    print(
        f"# hpart regression: tool={args.tool}, base=`{args.base}`, sota=`{args.sota}`, "
        f"parts={','.join(str(parts) for parts in parts_list)}, cases={len(cases)}"
    )
    print()
    for table in build_tables(case_order, results, specs, args.cases_per_table):
        print(table)
        print()

    summary_items: list[str] = []
    for spec in specs:
        ok_count = sum(
            1
            for result in results.values()
            if result.flow == spec.flow and result.parts == spec.parts and result.status == "OK"
        )
        summary_items.append(f"{spec.flow} N={spec.parts:>2}: ok={ok_count}/{len(cases)}")
    print("summary: " + ", ".join(summary_items))
    return 0 if all(result.status == "OK" for result in results.values()) else 2


if __name__ == "__main__":
    sys.exit(main())
