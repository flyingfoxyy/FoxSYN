# csr Phase 0 Relocation — Implementation Plan

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax for tracking. Implement task-by-task, build + run after each.

**Goal:** Add a hop-preserving node-relocation pass (Phase 0) to `csr` that reduces cross-partition cut-edges by relabeling node partitions, before the existing resub/replicate phases.

**Architecture:** A new `run_phase0_relocate` runs before Phase 1. It greedily moves each node to the neighbor partition that minimizes its incident cut-edges, gated by (1) strict cut-edge decrease, (2) exact global hop recompute must not worsen, (3) a per-partition balance cap. Pure `part_id` relabeling — zero area, zero logic change.

**Tech Stack:** C++ (C++23), vendored ABC, existing `csr`/`cpr` modules. Build via `make release`. No unit-test framework — verification is build + FoxSYN runs on frozen-partition benchmarks + `cec`.

## Global Constraints

- **Hop hard constraint:** every accepted move keeps global `Abc_NtkComputeHopNum(pNtk)` ≤ the value at Phase 0 entry. Reject (roll back) any move that raises it.
- **Balance cap:** no partition may exceed `compute_balance_max_allowed(sz, balance_pct)`; `balance_pct` from `-B`, else pdb's, else 2.
- **Strict decrease:** only apply moves with cut-edge `delta < 0`, re-verified against current partitions at apply time.
- **Default on:** relocation runs by default; `-L` disables it. With `-L`, results must be byte-identical to current `main`.
- **Cut-edge definition (must match `ComputeCutEdgeCount`):** a cut-edge is a `(consumer=NODE, driver=part_stat vertex)` fanin pair with differing valid partitions. PO fanouts never count.
- Match surrounding style: 4-space indent, braces on own line, `snake_case` locals, `PascalCase` types.

---

### Task 1: Fix `run_csr_regression.py` ps-output regex

The `ps` output changed (`786a8ef`): `cut =` became `cut-net =` / `cut-edge =`. The regression script's `CUTNET_RE = \bcut =` no longer matches. Fix before using the script to measure Phase 0.

**Files:**
- Modify: `scripts/run_csr_regression.py:29`

- [ ] **Step 1: Update the regex**

Change line 29 from:

```python
CUTNET_RE = re.compile(r"\bcut =\s*(\d+)")
```

to:

```python
CUTNET_RE = re.compile(r"\bcut-net =\s*(\d+)")
```

- [ ] **Step 2: Verify it matches the new ps output**

Run:

```bash
cd /home/longfei/FoxSYN/regression && python3 - <<'EOF'
import re
CUTNET_RE = re.compile(r"\bcut-net =\s*(\d+)")
line = "pdb : part =  4  hop =    8  cut-net =   56  cut-edge =  667  arr =1614.00"
print(CUTNET_RE.findall(line))
EOF
```

Expected: `['56']`

- [ ] **Step 3: Commit**

```bash
cd /home/longfei/FoxSYN
git add scripts/run_csr_regression.py
git commit -m "run_csr_regression: match new ps cut-net label"
```

---

### Task 2: Add `do_relocate` config flag + `-L` CLI plumbing (empty phase stub)

Add the config field, CLI flag, usage text, and an empty `run_phase0_relocate` wired into `ApplyCsr` before Phase 1. This locks the CLI surface; the algorithm arrives in Task 3. With the empty stub, behavior is unchanged (relocation is a no-op), so `-L` and default must both reproduce current numbers.

**Files:**
- Modify: `src/csr/csr.hpp:19-20` (add field)
- Modify: `src/csr/csr.cpp` (add stub before `ApplyCsr`; call it; extend final printf)
- Modify: `src/main.cpp` (flag parse + usage)

**Interfaces:**
- Produces: `void fox::csr::run_phase0_relocate(Abc_Ntk_t *pNtk, const Config &cfg, int &total_moves)` — Task 3 fills the body.
- Produces: `Config::do_relocate` (bool, default `true`).

- [ ] **Step 1: Add the config field**

In `src/csr/csr.hpp`, after the `do_balance_repair` line (currently line 19), add:

```cpp
    bool do_balance_repair    = false; // -b: run cpr-style enforce_balance after phase1/2 (off by default)
    bool do_relocate          = true;  // -L disables: phase 0 hop-preserving node relocation
```

(The first line above already exists — insert only the `do_relocate` line after it.)

- [ ] **Step 2: Add the empty phase-0 stub in csr.cpp**

In `src/csr/csr.cpp`, immediately **after** `resolve_num_parts` (ends ~line 823) and **before** `bool ApplyCsr`, insert:

```cpp
// ---------------------------------------------------------------------
// Phase 0: hop-preserving node relocation. Greedily moves each node to the
// neighbor partition minimizing its incident cut-edges. Pure part_id
// relabel (zero area, zero logic). Gated by strict cut-edge decrease, an
// exact global-hop-non-worsening check, and a per-partition balance cap.
// ---------------------------------------------------------------------
void run_phase0_relocate(Abc_Ntk_t *pNtk, const Config &cfg, int &total_moves)
{
    (void)pNtk; (void)cfg; (void)total_moves; // filled in Task 3
}
```

- [ ] **Step 3: Wire it into ApplyCsr before Phase 1**

In `src/csr/csr.cpp` `ApplyCsr`, find (currently ~line 847):

```cpp
    int total_attempts = 0, total_successes = 0;
    run_phase1_resub(pNtk, cfg, total_attempts, total_successes);
    int after_phase1 = ComputeCutEdgeCount(pNtk);
```

Replace with:

```cpp
    int total_moves = 0;
    if (cfg.do_relocate)
        run_phase0_relocate(pNtk, cfg, total_moves);
    int after_phase0 = ComputeCutEdgeCount(pNtk);

    int total_attempts = 0, total_successes = 0;
    run_phase1_resub(pNtk, cfg, total_attempts, total_successes);
    int after_phase1 = ComputeCutEdgeCount(pNtk);
```

- [ ] **Step 4: Extend the summary printf**

In `src/csr/csr.cpp` `ApplyCsr`, find (currently ~line 879):

```cpp
    printf("csr: cut-edges %d -> %d (after phase1=%d, after phase2=%d)\n",
           initial_cutedges, final_cutedges, after_phase1, after_phase2);
    printf("csr: phase1 %d attempts / %d successes; phase2 %d replications\n",
           total_attempts, total_successes, total_replications);
```

Replace with:

```cpp
    printf("csr: cut-edges %d -> %d (after phase0=%d, after phase1=%d, after phase2=%d)\n",
           initial_cutedges, final_cutedges, after_phase0, after_phase1, after_phase2);
    printf("csr: phase0 %d moves; phase1 %d attempts / %d successes; phase2 %d replications\n",
           total_moves, total_attempts, total_successes, total_replications);
```

- [ ] **Step 5: Add `-L` flag parsing in main.cpp**

In `src/main.cpp` `Csr_Command`, find the `case 'b':` block:

```cpp
        case 'b':
            cfg.do_balance_repair ^= 1;
            break;
```

Add after it:

```cpp
        case 'L':
            cfg.do_relocate ^= 1;
            break;
```

- [ ] **Step 6: Update the usage string in main.cpp**

In `src/main.cpp` `Csr_Command` usage block, change the synopsis line:

```cpp
    Abc_Print(-2, "usage: csr [-R num] [-S num] [-X num] [-G num] [-B num] [-bv]\n");
```

to:

```cpp
    Abc_Print(-2, "usage: csr [-R num] [-S num] [-X num] [-G num] [-B num] [-bLv]\n");
```

And add a line after the `-b` usage line:

```cpp
    Abc_Print(-2, "\t-b      : run cpr-style balance repair after phase1/2 [default = %s]\n", cfg.do_balance_repair ? "on" : "off");
    Abc_Print(-2, "\t-L      : disable phase 0 hop-preserving relocation [default = on]\n");
```

(The `-b` line already exists — insert only the `-L` line after it.)

- [ ] **Step 7: Build**

Run:

```bash
cd /home/longfei/FoxSYN && make release -j16 2>&1 | tail -3
```

Expected: ends with `Built target FoxSYN`, exit 0.

- [ ] **Step 8: Verify CLI surface + no behavior change**

Run:

```bash
cd /home/longfei/FoxSYN/regression
./FoxSYN -c "csr -h" 2>&1 | grep -- "-L"
./FoxSYN -c "read SimpleCircuits/mcnc/alu4.v; st; if -K 6; hpart -N 4 --load-part parts/N4/mcnc__alu4.part; csr -v" 2>&1 | grep "csr: cut-edges"
```

Expected: the `-L` usage line prints; and `csr: cut-edges 667 -> 549 (after phase0=667, after phase1=573, after phase2=549)` (phase0=667 == initial, since stub is a no-op).

- [ ] **Step 9: Commit**

```bash
cd /home/longfei/FoxSYN
git add src/csr/csr.hpp src/csr/csr.cpp src/main.cpp
git commit -m "csr: add -L flag and phase 0 relocation stub"
```

---

### Task 3: Implement the relocation algorithm

Fill in the helpers and `run_phase0_relocate` body. This is the core: incident cut-edge counting, best-target selection, the FM-style multi-round loop with balance + hop gates.

**Files:**
- Modify: `src/csr/csr.cpp` (add two static helpers + fill `run_phase0_relocate`)

**Interfaces:**
- Consumes: `is_part_stat_vertex` (csr.cpp:30), `ComputeCutEdgeCount` (csr.cpp:38), `resolve_num_parts` (csr.cpp:807), `fox::cpr::partition_sizes` / `fox::cpr::compute_balance_max_allowed` (cpr.hpp:26-27), `Abc_NtkComputeHopNum` (abc.h:564).
- Produces: `static int node_incident_cross(Abc_Obj_t *pNode, part_id as_part)`, `static part_id best_relocate_target(Abc_Obj_t *pNode, int &best_delta)`.

- [ ] **Step 1: Add the two static helpers before `run_phase0_relocate`**

In `src/csr/csr.cpp`, immediately **before** the `run_phase0_relocate` comment block added in Task 2, insert:

```cpp
// Count pNode's incident cross-partition cut-edges assuming it lives in
// `as_part`. Fanin edges: pNode is the consumer (a node), so any part_stat
// driver in a different partition is a cut-edge. Fanout edges: pNode is the
// driver, so only NODE consumers in a different partition count (matches
// ComputeCutEdgeCount's Abc_NtkForEachNode outer loop; PO fanouts never
// count). Since only pNode's part changes on a move, this equals the exact
// global cut-edge contribution of pNode.
static int node_incident_cross(Abc_Obj_t *pNode, part_id as_part)
{
    int cross = 0;
    Abc_Obj_t *pObj;
    int k;
    Abc_ObjForEachFanin(pNode, pObj, k)
    {
        if (!is_part_stat_vertex(pObj))
            continue;
        part_id fp = Abc_ObjGetPartId(pObj);
        if (fp != ABC_PART_ID_NONE && fp != as_part)
            cross++;
    }
    Abc_ObjForEachFanout(pNode, pObj, k)
    {
        if (!Abc_ObjIsNode(pObj))
            continue;
        part_id fp = Abc_ObjGetPartId(pObj);
        if (fp != ABC_PART_ID_NONE && fp != as_part)
            cross++;
    }
    return cross;
}

// Pick the neighbor partition (a partition some fanin/fanout lives in) that
// minimizes pNode's incident cross-edges. Returns the current partition with
// best_delta=0 if no neighbor partition is strictly better. best_delta is the
// exact global cut-edge change of the returned move (<0 means improvement).
static part_id best_relocate_target(Abc_Obj_t *pNode, int &best_delta)
{
    part_id cur = Abc_ObjGetPartId(pNode);
    int cur_cross = node_incident_cross(pNode, cur);
    part_id best = cur;
    int best_cross = cur_cross;

    auto consider = [&](part_id P) {
        if (P == ABC_PART_ID_NONE || P == cur)
            return;
        int c = node_incident_cross(pNode, P);
        if (c < best_cross)
        {
            best_cross = c;
            best = P;
        }
    };

    Abc_Obj_t *pObj;
    int k;
    Abc_ObjForEachFanin(pNode, pObj, k)
        consider(Abc_ObjGetPartId(pObj));
    Abc_ObjForEachFanout(pNode, pObj, k)
        consider(Abc_ObjGetPartId(pObj));

    best_delta = best_cross - cur_cross;
    return best;
}
```

- [ ] **Step 2: Replace the stub body of `run_phase0_relocate`**

Replace the entire stub function body (the `(void)...` line) so the function reads:

```cpp
void run_phase0_relocate(Abc_Ntk_t *pNtk, const Config &cfg, int &total_moves)
{
    int num_parts = resolve_num_parts(pNtk);
    int balance_pct = cfg.balance_pct;
    if (balance_pct < 0)
        balance_pct = pNtk->pPdb->balance_pct();
    if (balance_pct < 0)
        balance_pct = 2;

    std::vector<int> sz;
    fox::cpr::partition_sizes(pNtk, num_parts, sz);
    int max_allowed = fox::cpr::compute_balance_max_allowed(sz, balance_pct);

    const int baseline_hop = Abc_NtkComputeHopNum(pNtk);
    int best_cutedges = ComputeCutEdgeCount(pNtk);
    int stall_count = 0;

    struct Move {
        int node_id;
        part_id target;
        int delta;
    };

    for (int round = 0; round < cfg.max_rounds; ++round)
    {
        std::vector<Move> moves;
        Abc_Obj_t *pObj;
        int i;
        Abc_NtkForEachNode(pNtk, pObj, i)
        {
            if (Abc_ObjGetPartId(pObj) == ABC_PART_ID_NONE)
                continue;
            int delta;
            part_id tgt = best_relocate_target(pObj, delta);
            if (delta < 0)
                moves.push_back({pObj->Id, tgt, delta});
        }
        if (moves.empty())
            break;

        std::sort(moves.begin(), moves.end(),
                  [](const Move &a, const Move &b) { return a.delta < b.delta; });

        int round_moved = 0;
        for (const auto &mv : moves)
        {
            Abc_Obj_t *pNode = Abc_NtkObj(pNtk, mv.node_id);
            if (!pNode || !Abc_ObjIsNode(pNode))
                continue;
            part_id cur = Abc_ObjGetPartId(pNode);
            if (cur == ABC_PART_ID_NONE)
                continue;

            // Re-verify best target against current (possibly changed)
            // neighbor partitions -- earlier moves this round may have made
            // the precomputed delta stale.
            int delta;
            part_id tgt = best_relocate_target(pNode, delta);
            if (delta >= 0 || tgt == cur)
                continue;

            // Balance cap.
            if (sz[tgt] + 1 > max_allowed)
                continue;

            // Apply tentatively, then exact global hop check.
            Abc_ObjSetPartId(pNode, tgt);
            int new_hop = Abc_NtkComputeHopNum(pNtk);
            if (new_hop > baseline_hop)
            {
                Abc_ObjSetPartId(pNode, cur); // roll back
                continue;
            }

            sz[cur] -= 1;
            sz[tgt] += 1;
            round_moved++;
        }

        total_moves += round_moved;

        int new_cutedges = ComputeCutEdgeCount(pNtk);
        if (cfg.verbose)
            printf("csr: phase0 round %2d  moves=%3d  cut-edges=%d  hop=%d\n",
                   round, round_moved, new_cutedges, baseline_hop);

        if (new_cutedges < best_cutedges)
        {
            best_cutedges = new_cutedges;
            stall_count = 0;
        }
        else if (++stall_count >= cfg.stall_limit)
        {
            break;
        }

        if (round_moved == 0)
            break;
    }
}
```

- [ ] **Step 3: Build**

Run:

```bash
cd /home/longfei/FoxSYN && make release -j16 2>&1 | tail -3
```

Expected: `Built target FoxSYN`, exit 0.

- [ ] **Step 4: Verify phase 0 reduces cut-edge with hop unchanged on alu4**

Run:

```bash
cd /home/longfei/FoxSYN/regression
./FoxSYN -c "read SimpleCircuits/mcnc/alu4.v; st; if -K 6; hpart -N 4 --load-part parts/N4/mcnc__alu4.part; ps; csr -v" 2>&1 | grep -E "csr: phase0|csr: cut-edges|pdb.*hop"
```

Expected: `after phase0` < 667 (initial); phase 0 verbose rounds show non-increasing `hop=8` (equal to the pre-csr hop from the `ps` line); `csr: cut-edges 667 -> <final>` with final ≤ 549 (never worse than the pre-relocation result).

- [ ] **Step 5: Verify `-L` reproduces the old (no-relocation) result**

Run:

```bash
cd /home/longfei/FoxSYN/regression
./FoxSYN -c "read SimpleCircuits/mcnc/alu4.v; st; if -K 6; hpart -N 4 --load-part parts/N4/mcnc__alu4.part; csr -L -v" 2>&1 | grep "csr: cut-edges"
```

Expected: `csr: cut-edges 667 -> 549 (after phase0=667, after phase1=573, after phase2=549)` — identical to current `main` (relocation disabled ⇒ phase0=initial).

- [ ] **Step 6: Verify functional equivalence (cec) on the relocated result**

Run:

```bash
cd /home/longfei/FoxSYN/regression
./FoxSYN -c "read SimpleCircuits/mcnc/alu4.v; st; if -K 6; hpart -N 4 --load-part parts/N4/mcnc__alu4.part; write /tmp/csr_reloc_before.blif; csr -v; write /tmp/csr_reloc_after.blif; cec /tmp/csr_reloc_before.blif /tmp/csr_reloc_after.blif" 2>&1 | grep -i "equivalent"
```

Expected: `Networks are equivalent`.

- [ ] **Step 7: Commit**

```bash
cd /home/longfei/FoxSYN
git add src/csr/csr.cpp
git commit -m "csr: implement phase 0 hop-preserving node relocation"
```

---

### Task 4: Full frozen-partition regression + acceptance verification

Measure Phase 0 across all 90 SimpleCircuits cases at N=4 using the frozen partitions, comparing relocation-on vs `-L` (off), and verify the acceptance criteria (cec EQ, hop not worse, extra cut-edge gain).

**Files:**
- None modified (measurement only). Uses `scripts/run_csr_regression.py`, `regression/parts/N4/`.

- [ ] **Step 1: Run relocation-off baseline (the current two-phase result)**

Run:

```bash
cd /home/longfei/FoxSYN/regression
python3 ../scripts/run_csr_regression.py --foxsyn ./FoxSYN --cases-root SimpleCircuits \
  -N 4 --csr-args "-v -L" --load-parts-dir parts/N4 --cec -j 8 --timeout 120 \
  > /tmp/csr_reloc_off.log 2>&1
tail -12 /tmp/csr_reloc_off.log
```

Expected: completes; note the "Cases with cut-edge gain" and total cut-edge line.

Note: `run_csr_regression.py`'s `--load-parts-dir` expects files named `<case>.part`, but the frozen dir uses `<group>__<case>.part`. If the loader misses files it falls back to a fresh `hpart` (non-frozen). If the run shows non-deterministic cut-edge across repeat runs, add a `--parts-name-map` shim or copy/symlink the `N4/*.part` files to bare `<case>.part` names first:

```bash
cd /home/longfei/FoxSYN/regression && mkdir -p parts_n4_flat && \
  for f in parts/N4/*.part; do b=$(basename "$f"); ln -sf "../$f" "parts_n4_flat/${b##*__}"; done
```

Then use `--load-parts-dir parts_n4_flat`.

- [ ] **Step 2: Run relocation-on**

Run:

```bash
cd /home/longfei/FoxSYN/regression
python3 ../scripts/run_csr_regression.py --foxsyn ./FoxSYN --cases-root SimpleCircuits \
  -N 4 --csr-args "-v" --load-parts-dir parts_n4_flat --cec -j 8 --timeout 120 \
  > /tmp/csr_reloc_on.log 2>&1
tail -12 /tmp/csr_reloc_on.log
```

Expected: completes; total cut-edge across gaining cases ≤ the relocation-off total (Phase 0 only helps or is neutral).

- [ ] **Step 3: Verify acceptance criteria**

Run:

```bash
cd /home/longfei/FoxSYN/regression
echo "=== cec must be all EQ (0 NOT_EQ) ==="
grep -c "NOT_EQ" /tmp/csr_reloc_on.log || echo "0 NOT_EQ"
echo "=== hop must not worsen: HopAft <= HopBef per OK case ==="
grep -E "^\S" /tmp/csr_reloc_on.log | awk 'NF>=6 && $4 ~ /^[0-9]+$/ && $5 ~ /^[0-9]+$/ && $5 > $4 {print "HOP WORSENED:", $0}'
```

Expected: `0 NOT_EQ` (or count 0); no "HOP WORSENED" lines. (Column layout: `Case Ndbef Ndaft HopBef HopAft ...` — adjust `$4/$5` if the header shows different positions; confirm against the printed header.)

- [ ] **Step 4: Summarize the delta**

Run:

```bash
cd /home/longfei/FoxSYN/regression
echo "=== OFF ==="; grep -E "Cases with cut-edge gain|Total cut-edges" /tmp/csr_reloc_off.log
echo "=== ON  ==="; grep -E "Cases with cut-edge gain|Total cut-edges" /tmp/csr_reloc_on.log
```

Expected: ON shows total cut-edges ≤ OFF; document the improvement. No commit (measurement task) unless a helper script/symlink was added — if `parts_n4_flat` symlinks were created, they are gitignored under `regression/*`, so nothing to commit.

---

### Task 5: Document Phase 0 in docs/csr.md

Update the canonical csr doc to describe the new phase, its gates, and its place in the pipeline.

**Files:**
- Modify: `docs/csr.md` (算法流程 section: add Phase 0; 命令接口: add `-L`)

- [ ] **Step 1: Add the Phase 0 pipeline entry**

In `docs/csr.md`, in the `## 算法流程` code block, change the pipeline pseudocode so relocation runs first. Find:

```
ApplyCsr(pNtk, cfg):
    initial_cutedges = ComputeCutEdgeCount(pNtk)

    Phase 1: run_phase1_resub      -- 优先尝试不增加节点的 SAT resub
```

Replace with:

```
ApplyCsr(pNtk, cfg):
    initial_cutedges = ComputeCutEdgeCount(pNtk)

    Phase 0: run_phase0_relocate   -- 保 hop 的节点迁移(纯重标记,零面积),默认开启
    after_phase0 = ComputeCutEdgeCount(pNtk)

    Phase 1: run_phase1_resub      -- 优先尝试不增加节点的 SAT resub
```

- [ ] **Step 2: Add a Phase 0 subsection**

In `docs/csr.md`, immediately before the `### Phase 1：partition-match resub` heading, insert:

```markdown
### Phase 0：保 hop 的节点迁移(relocation)

Phase 1/2 都在改消费者或驱动者的逻辑,却不处理"节点本身应该待在别的分区"这类结构性跨分区边。Phase 0 补上这个杠杆:对每个节点,计算迁到某个**邻居分区**(fanin/fanout 出现过的分区)后自身关联跨分区边(fanin 边 + NODE fanout 边,PO 不计,与 `ComputeCutEdgeCount` 同约定)的变化 `delta`,贪心地应用 `delta < 0` 的迁移。纯 `part_id` 重标记,零面积、零逻辑改动。

三重门槛,缺一不可:

1. **cut-edge 严格下降**:只接受 `delta < 0`,且在真正应用前用当前分区重算 `delta`(FM 式多轮里前面的迁移会让预算过期)。
2. **hop 精确不变差**:每次试探性迁移后重算全网 `Abc_NtkComputeHopNum`,超过 Phase 0 进入时的基线就回滚。区别于 Phase 2 的静态 slack 快照——迁移会同时翻转节点 fanin 边和所有 fanout 边的跨分区状态,影响更广,精确重算最稳(运行时间不敏感,可承受)。
3. **平衡上限**:复用 cpr 的 `compute_balance_max_allowed`,分区节点数达上限就拒绝迁入,从机制上杜绝"全挪到一个分区"。

Phase 0 放最前:先用零成本迁移收紧边界,还可能给 Phase 1 创造出原本没有的"同分区 divisor"。`-L` 关闭(默认开启)。
```

- [ ] **Step 3: Add `-L` to the command interface**

In `docs/csr.md`, in the `## 命令接口` usage block, update the synopsis and add the `-L` line to match `main.cpp`:

Change `usage: csr [-R num] [-S num] [-X num] [-G num] [-B num] [-bv]` to `[-bLv]`, and after the `-b` option line add:

```
    -L      : disable phase 0 hop-preserving relocation [default = on]
```

- [ ] **Step 4: Commit**

```bash
cd /home/longfei/FoxSYN
git add docs/csr.md
git commit -m "docs: document csr phase 0 relocation"
```

---

## Self-Review

**Spec coverage:** Phase 0 placement (Task 2/3), delta definition matching `ComputeCutEdgeCount` (Task 3 helper comments), exact global hop gate (Task 3 step 2), balance cap via cpr primitives (Task 3 step 2), FM multi-round with apply-time re-verification (Task 3 step 2), `-L` default-on flag (Task 2), ps-regex prerequisite fix (Task 1), regression/cec verification (Task 4), docs (Task 5). All spec sections mapped.

**Placeholder scan:** No TBD/TODO. The one conditional in Task 4 (part-file name mismatch shim) is a concrete fallback with exact commands, not a placeholder.

**Type consistency:** `run_phase0_relocate(Abc_Ntk_t*, const Config&, int&)` consistent across Task 2 stub and Task 3 body and the ApplyCsr call site. `node_incident_cross(Abc_Obj_t*, part_id)->int` and `best_relocate_target(Abc_Obj_t*, int&)->part_id` consistent between helper definitions and their uses in the loop. `do_relocate` bool consistent (hpp field, `-L` toggle, ApplyCsr guard).
