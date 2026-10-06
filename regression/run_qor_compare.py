#!/usr/bin/env python3
import concurrent.futures
import csv
import math
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CASE_ROOT = ROOT / "regression" / "SimpleCircuits"
AGD_ABC = Path("/home/longfei/agdmap/abc")
SUPERMAP = Path(os.environ.get("FOXSYN_BIN", ROOT / "release" / "FoxSYN"))
OUT_MD = ROOT / "regression" / "qor_compare_results.md"
OUT_CSV = ROOT / "regression" / "qor_compare_results.csv"

SETS = ["EPFL", "mcnc", "opencores", "vtr"]
WORKERS = int(os.environ.get("QOR_WORKERS", "4"))
TIMEOUT_SEC = int(os.environ.get("QOR_TIMEOUT_SEC", "180"))
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
PS_RE = re.compile(r"i/o\s*=\s*\d+/\s*\d+.*?\bnd\s*=\s*(\d+)\s+edge\s*=\s*(\d+).*?\blev\s*=\s*(\d+)")


def load_cases():
    cases = []
    for set_name in SETS:
        with (CASE_ROOT / set_name / "list").open(encoding="utf-8") as f:
            for raw in f:
                item = raw.strip()
                if not item or item.startswith("#"):
                    continue
                cases.append((set_name, item, CASE_ROOT / set_name / item))
    return cases


def clean(text):
    return ANSI_RE.sub("", text)


def run_abc(binary, cmd, log_path):
    start = time.time()
    try:
        proc = subprocess.run(
            ["rtk", str(binary), "-c", cmd],
            cwd=str(ROOT),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=TIMEOUT_SEC,
        )
    except subprocess.TimeoutExpired as exc:
        log_path.write_text(clean(exc.stdout or ""), encoding="utf-8", errors="replace")
        return {"status": "timeout", "sec": time.time() - start}

    out = clean(proc.stdout)
    log_path.write_text(out, encoding="utf-8", errors="replace")
    match = None
    for m in PS_RE.finditer(out):
        match = m
    if proc.returncode:
        return {"status": f"exit_{proc.returncode}", "sec": time.time() - start}
    if not match:
        return {"status": "parse_fail", "sec": time.time() - start}
    return {
        "status": "ok",
        "sec": time.time() - start,
        "nd": int(match.group(1)),
        "edge": int(match.group(2)),
        "lev": int(match.group(3)),
    }


def compare_rows(rows, lhs, rhs, metric):
    vals = []
    lhs_sum = rhs_sum = 0
    w = t = l = 0
    for r in rows:
        a = int(r[f"{lhs}_{metric}"])
        b = int(r[f"{rhs}_{metric}"])
        vals.append(a / b)
        lhs_sum += a
        rhs_sum += b
        if a < b:
            w += 1
        elif a == b:
            t += 1
        else:
            l += 1
    gmean = math.exp(sum(math.log(x) for x in vals) / len(vals))
    total = lhs_sum / rhs_sum
    return gmean, total, f"{w}/{t}/{l}"


def fmt(x):
    return f"{x:.4f}"


def render_table(title, headers, rows):
    cols = [len(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            cols[i] = max(cols[i], len(cell))
    def line(items):
        return "| " + " | ".join(item.ljust(cols[i]) for i, item in enumerate(items)) + " |"
    out = [title, line(headers), "|-" + "-|-".join("-" * cols[i] for i in range(len(headers))) + "-|"]
    for row in rows:
        out.append(line(row))
    return "\n".join(out)


def main():
    if not AGD_ABC.exists():
        raise SystemExit(f"missing {AGD_ABC}")
    if not SUPERMAP.exists():
        raise SystemExit(f"missing {SUPERMAP}")

    cases = load_cases()
    print(f"[INFO] cases={len(cases)} workers={WORKERS} timeout={TIMEOUT_SEC}s", flush=True)

    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=WORKERS) as pool:
        futs = {pool.submit(run_case, case): case for case in cases}
        for i, fut in enumerate(concurrent.futures.as_completed(futs), 1):
            row = fut.result()
            rows.append(row)
            statuses = " ".join(row[f"{k}_status"] for k in ["if_area", "agd_area", "super_area", "if_delay", "agd_delay", "super_delay"])
            print(f"[{i:03d}/{len(cases):03d}] {row['set']}/{row['case']} {statuses}", flush=True)

    rows.sort(key=lambda r: (r["set"], r["case"]))
    fieldnames = ["set", "case", "path"]
    for label in ["if_area", "agd_area", "super_area", "if_delay", "agd_delay", "super_delay"]:
        fieldnames += [f"{label}_status", f"{label}_nd", f"{label}_edge", f"{label}_lev", f"{label}_sec"]
    with OUT_CSV.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

    area_ok = [r for r in rows if all(r[f"{k}_status"] == "ok" for k in ["if_area", "agd_area", "super_area"])]
    delay_ok = [r for r in rows if all(r[f"{k}_status"] == "ok" for k in ["if_delay", "agd_delay", "super_delay"])]

    agd_area_nd = compare_rows(area_ok, "agd_area", "if_area", "nd")
    super_area_nd = compare_rows(area_ok, "super_area", "if_area", "nd")
    agd_delay_nd = compare_rows(delay_ok, "agd_delay", "if_delay", "nd")
    super_delay_nd = compare_rows(delay_ok, "super_delay", "if_delay", "nd")
    agd_delay_lev = compare_rows(delay_ok, "agd_delay", "if_delay", "lev")
    super_delay_lev = compare_rows(delay_ok, "super_delay", "if_delay", "lev")

    area_rows = [
        ["if", "if -K 6 -a -v", "1.0000", "1.0000", "-"],
        ["agdmap", "agdmap -v", fmt(agd_area_nd[0]), fmt(agd_area_nd[1]), agd_area_nd[2]],
        ["supermap", "smap -a -g", fmt(super_area_nd[0]), fmt(super_area_nd[1]), super_area_nd[2]],
    ]

    delay_rows = [
        ["if", "if -K 6 -v", "1.0000", "1.0000", "-", "1.0000", "1.0000", "-"],
        ["agdmap", "agdmap -d -v", fmt(agd_delay_nd[0]), fmt(agd_delay_nd[1]), agd_delay_nd[2], fmt(agd_delay_lev[0]), fmt(agd_delay_lev[1]), agd_delay_lev[2]],
        ["supermap", "smap -D -g", fmt(super_delay_nd[0]), fmt(super_delay_nd[1]), super_delay_nd[2], fmt(super_delay_lev[0]), fmt(super_delay_lev[1]), super_delay_lev[2]],
    ]

    md = []
    md.append("# QoR Compare")
    md.append("")
    md.append(f"- cases listed: {len(rows)}")
    md.append(f"- area mode usable cases: {len(area_ok)}")
    md.append(f"- delay mode usable cases: {len(delay_ok)}")
    md.append(f"- supermap delay failures: {len(rows) - len(delay_ok)}")
    md.append("")
    md.append(render_table("## Area Mode", ["Mapper", "Cmd", "LUT geo", "LUT total", "W/T/L"], area_rows))
    md.append("")
    md.append(render_table("## Delay Mode", ["Mapper", "Cmd", "LUT geo", "LUT total", "W/T/L", "Lev geo", "Lev total", "W/T/L"], delay_rows))
    md.append("")
    if len(delay_ok) != len(rows):
        md.append("### Delay Failures")
        for r in rows:
            if r["if_delay_status"] == "ok" and r["agd_delay_status"] == "ok" and r["super_delay_status"] != "ok":
                md.append(f"- {r['set']}/{r['case']}: supermap={r['super_delay_status']}")

    OUT_MD.write_text("\n".join(md) + "\n", encoding="utf-8")
    print(f"[INFO] wrote {OUT_MD}")
    print(f"[INFO] wrote {OUT_CSV}")


def run_case(case):
    set_name, name, path = case
    stem = f"{set_name}_{Path(name).stem}"
    cmds = {
        "if_area": (AGD_ABC, f"read {path}; st; if -K 6 -a -v; ps"),
        "agd_area": (AGD_ABC, f"read {path}; st; agdmap -v; ps"),
        "super_area": (SUPERMAP, f"read {path}; st; smap -a -g; ps"),
        "if_delay": (AGD_ABC, f"read {path}; st; if -K 6 -v; ps"),
        "agd_delay": (AGD_ABC, f"read {path}; st; agdmap -d -v; ps"),
        "super_delay": (SUPERMAP, f"read {path}; st; smap -D -g; ps"),
    }
    row = {"set": set_name, "case": name, "path": str(path)}
    for label, (binary, cmd) in cmds.items():
        res = run_abc(binary, cmd, ROOT / "regression" / f".qor_{stem}.{label}.log")
        row[f"{label}_status"] = res["status"]
        row[f"{label}_sec"] = f"{res['sec']:.3f}"
        row[f"{label}_nd"] = str(res.get("nd", ""))
        row[f"{label}_edge"] = str(res.get("edge", ""))
        row[f"{label}_lev"] = str(res.get("lev", ""))
    return row


if __name__ == "__main__":
    main()
