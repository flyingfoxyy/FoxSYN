# CSR Optimization Enhancement — Claude Handoff

This file is the continuation context for the CSR optimization work. Read it together with:

- Design: `docs/superpowers/specs/2026-07-11-csr-optimization-enhancement-design.md`
- Implementation plan: `docs/superpowers/plans/2026-07-11-csr-optimization-enhancement.md`
- Current CSR documentation: `docs/csr.md`
- Repository instructions: `AGENTS.md` and `/home/longfei/.codex/RTK.md`

## 1. Current repository state

- Main checkout: `/home/longfei/FoxSYN`, branch `decompose2`
- Implementation worktree: `/home/longfei/FoxSYN/.worktrees/csr-enhancement`
- Implementation branch: `csr-enhancement`
- Current HEAD: `c3b393a56630e49c2c4ba3a7c47a023a12d90eec`
- Worktree was clean when this handoff was written.
- Prefix shell commands with `rtk`.
- The user explicitly canceled further spec/code-quality reviews. The requested workflow is currently: implement first, test optimization effect, then decide whether to polish.

The old preserved baseline path `/tmp/FoxSYN-csr-baseline-799702c` no longer exists in the current environment. If Task 12 needs binary-to-binary baseline comparison, rebuild commit `799702c` in a separate temporary checkout/worktree or consume a supplied baseline-results JSON.

## 2. Approved hard constraints and defaults

The stable profile is:

- Hop must not increase: `hop_after <= hop_before`.
- Cumulative positive node growth is at most 2% of CSR entry nodes.
- Deletions never refund the growth budget.
- Cut-net is at most 150% of the CSR entry cut-net.
- Balance remains a hard constraint.
- Default trajectory count is 1; `-T 1..3` enables deterministic alternatives.
- Default runtime target is no more than 5x the old CSR runtime.
- CSR remains internally self-contained; do not orchestrate `cpr`, `cmfs`, or `rewrite` commands around it.

Important ownership rules:

- Capture PDB `num_parts` and `balance_pct` before duplicating the entry network.
- Keep the frame's original network installed while trajectory duplicates run.
- Install only the selected winner with `Abc_FrameReplaceCurrentNetwork`.
- Destroy MFS state before deleting or replacing a network snapshot.
- Phase 1 and Phase 2 share one non-refunding growth budget.

## 3. Completed work: Tasks 1–11

### Task 1 — CSR internal state and tests

Commits:

- `d4ab2db` — `Add CSR optimization state tests`
- `aa9dcd5` — `Harden CSR entry limits`

Implemented:

- `src/csr/csr_internal.hpp`
- `src/csr/csr_state.cpp`
- `src/test_csr.cpp` and CMake test target
- Entry metrics and frozen limits
- PDB scalar capture/restoration
- Overflow-safe percentage limits with `INT_MAX` saturation

### Task 2 — Shared growth and search budgets

Commit:

- `7305304` — `Add CSR search and growth budgets`

Implemented:

- `GrowthTracker` with non-refunding deletion semantics
- `SearchBudget` hard counters
- `OptimizationState`
- Unit tests for exact budget exhaustion

### Task 3 — Frame-level transactional trajectories

Commit:

- `f57d4d7` — `Make CSR trajectory execution transactional`

Implemented:

- `Config::num_trajectories = 1`
- Public `ApplyCsr(Abc_Frame_t *, ...)`
- Original frame network remains installed while duplicate trajectories execute
- Frozen limits and one shared per-trajectory optimization state
- Phase snapshots, rollback/audit plumbing, transactional optional balance repair
- Winner installation through the frame API

### Task 4 — Deterministic candidates and `-T`

Commits:

- `a94ae61` — `Make CSR candidate ordering deterministic`
- `862e616` — same subject; a delayed agent temporarily reverted part of the change
- `d489f74` — `Restore CSR deterministic CLI wiring`

Final state is correct despite the noisy history:

- `TrajectoryPolicy::{GainFirst, BoundaryConcentration, ScarcityFirst}`
- Internal `CutCandidate` and total comparator `(-weight, node_id, iFanin)`
- `csr -T NUM`, valid range 1–3, default 1
- CLI error strings and usage text are tested

### Task 5 — Bounded Phase 0 compound relocation

Commit:

- `c95b135` — `Add CSR compound relocation search`

Implemented:

- `RelocationStep` / `RelocationSequence`
- Deterministic beam search with seed limit 64, beam width 8, depth 3
- Reversible part-ID logs and exact balance/hop/cut-net checks
- Transactional application and rollback
- Exact cut-net gates on existing single move and swap paths
- Compound relocation runs after move/swap convergence
- Verbose `phase0 compound=<count>` reporting

The unit fixture reduces cut-edge by 2 in two moves. Its hop changes from 2 to 0, so the test correctly checks non-increase rather than equality; hop improvement is legal under the approved constraint.

### Task 6 — Exact incremental HopState

Commit:

- `2db5ef7` — `Add exact incremental CSR hop state`

Implemented:

- New `src/csr/csr_hop.cpp`
- Full initialization using `Abc_NtkDfs`
- Topology-ranked min-priority propagation
- Increase, decrease, transaction, rollback, and full-vector verification
- `VerifyAgainstFull` independently recomputes every object's arrival

### Task 7 — Aggregated Phase 2 candidates

Commit:

- `c213803` — `Aggregate CSR replication candidates`

Implemented:

- Candidate key `(driver_id, target_part)`
- Saved outgoing edges, added boundary edges, node cost, predicted cut-net delta
- Deterministic ratio ordering using integer cross multiplication
- Phase 2 consumes one aggregate candidate instead of repeated edge candidates

### Task 8 — Replication clusters

Commit:

- `1c51fa8` — `Add CSR replication cluster search`

Implemented:

- `ReplicationCluster`
- Depth <=2, node count <=3, at most 16 evaluated clusters per driver-target
- Original-topological duplicate construction
- Explicit inverse fanout-patch log
- Reverse-topological duplicate deletion on rejection
- Exact cut-edge/cut-net/hop/balance checks
- Growth budget consumed only after all other checks pass
- HopState reinitialization after temporary object creation/deletion because ABC object IDs do not shrink after deletion
- Phase 2 uses cluster search; one-node clusters preserve single-node behavior

Test note: the exact cluster fixture needs an internal test-only balance percentage of 150. With the production maximum 99, that artificial fixture's P1 size would grow from 3 to 5 while the computed cap is 4. Production still uses the configured hard balance limit.

ASan was not run: the current top-level Makefile has no `asan` target even though `AGENTS.md` documents one.

### Task 9 — Divisor metadata and hypothetical cut-net

Commit:

- `9b09019` — `Rank CSR divisors by partition benefit`

Implemented:

- `DivisorInfo` and total ordering by coverage, cut-edge delta, cut-net delta, hop, ID
- `ComputeHypotheticalCutNetDelta`
- Exact affected-driver union accounting; cached cut-net flags are not trusted

Known gap versus the full plan:

- The metadata type/helper is implemented and tested, but the existing MFS divisor enumeration is not yet fully refactored to retain the best 64 metadata entries and cap actual divisor-set SAT evaluations at 32 per node.

### Task 10 — Multi-plan Phase 1 core path

Commit:

- `b0441ac` — `Search multiple CSR resub plans`

Implemented:

- `ResubPlan`, selector, external-divisor rule, and plan legality helper
- `Phase1Stats`
- A consumer-level joint replacement fast path:
  - detects an existing equivalent divisor with the same SOP and fanin set
  - replaces two crossing fanins with one legal external divisor
  - performs exact metrics/audit and rollback
- The joint fixture ends with one fanin, cut-edge 1, hop 1
- Existing MFS Phase 1 remains as the fallback after this fast path

Important known gap versus the full Task 10 plan:

- This is not yet the complete MFS node-level multi-plan engine.
- It does not yet generate all pure-removal, pair-removal, zero/one/two-divisor SAT plans per consumer.
- It does not yet retain four SAT-successful plans or enforce every per-node SAT counter inside that new engine.
- It does not yet implement the sequential cut-net-cap test described in the plan.

The user asked to get a runnable optimization first, so this core path was deliberately committed before the full refactor.

### Task 11 — Multi-trajectory ownership and reporting

Commit:

- `c3b393a` — `Add deterministic CSR multi-trajectory search`

Implemented:

- `TakeBestTrajectory` with injectable delete function
- Lexicographic winner selection and exactly-once loser deletion
- Winner PDB metadata restoration before installation
- One summary line per trajectory with cut-edge, cut-net, hop, nodes, seconds, and validity
- `csr: selected trajectory N`
- Failed trajectories are deleted independently; an empty result set leaves the entry frame network installed

Known gap to check before claiming strict Task 11 completion:

- Trajectory policy mapping is currently used by compound relocation through `state.trajectory_id`.
- Audit whether BoundaryConcentration and ScarcityFirst influence every intended Phase 1/2 search location, not only compound relocation/candidate ordering.
- Fresh `SearchBudget` placement per node or driver-target should be audited when the full Task 10 engine is completed.

## 4. Verification completed at HEAD

The following passed at `c3b393a`:

```bash
rtk make release
rtk ./release/test_csr
rtk ./release/test_hop
rtk ./release/test_cpr
rtk git diff --check
```

The frozen partition fixture expected by the plan is absent:

```text
regression/parts_n4_flat/alu4.part
```

An earlier fallback attempt to run hmetis exited with status `40704`. Do not treat a fresh hmetis partition as deterministic baseline evidence.

## 5. Task 12 TODO — regression, acceptance, and documentation

Task 12 has not been implemented. Continue from here.

### 5.1 Regression parser and self-test

Modify `scripts/run_csr_regression.py`:

- Refactor parsing into `parse_output(text: str) -> Result`.
- Add `--self-test` using representative output.
- Parse at least:
  - cut-edge before/after
  - cut-net before/after
  - hop before/after
  - nodes before/after
  - selected trajectory
  - trajectory summary lines
  - phase counts, including the new `phase0 ... / ... compound` format
- Add fields:
  - `selected_trajectory`
  - `constraints`
  - `deterministic`
  - `baseline_cut_after`
  - `runtime_ratio`
- The self-test must assert:

```python
assert parsed.cutnet_before == "56"
assert parsed.cutnet_after == "79"
assert parsed.selected_trajectory == "1"
assert parsed.constraints == "OK"
assert parsed.deterministic is True
```

### 5.2 Constraint enforcement

Mark `constraints=OK` only if all parsed values exist and:

```python
hop_after <= hop_before
nodes_after * 100 <= nodes_before * 102
cutnet_after * 100 <= cutnet_before * 150
cut_after <= cut_before
```

Add exact-repeat support. Repeated runs on a frozen partition must match the optimization summary and selected trajectory byte-for-byte (ignore elapsed seconds).

Suggested CLI from the plan:

```text
--exact-repeats 3
--baseline-foxsyn PATH
--baseline-results JSON
--write-results JSON
```

### 5.3 Baseline and runtime reporting

- Rebuild the old baseline at commit `799702c` in an isolated temporary checkout because the old `/tmp` binary is gone.
- Compare default `-T 1` against the old CSR.
- Report runtime ratio and flag ratios above 5x.
- Do not compare runs that use different random hmetis partitions.

### 5.4 Acceptance runs

Preferred path:

1. Obtain or regenerate frozen `.part` files once.
2. Run T1 three exact repeats.
3. Run T3 three exact repeats.
4. Run CEC before/after CSR.
5. Run the old baseline on the same frozen partitions.

At minimum run:

```bash
rtk make release
rtk ./release/test_csr
rtk ./release/test_hop
rtk ./release/test_cpr
rtk python3 scripts/run_csr_regression.py --self-test
```

If frozen partitions remain unavailable, clearly report that benchmark effect and runtime acceptance are blocked; do not present random hmetis runs as deterministic comparisons.

### 5.5 Documentation

Update `docs/csr.md`, which is currently stale. It still describes the old two-phase/single-trajectory/hop-slack implementation.

Document:

- Frame-based ownership and transaction model
- Entry hard limits
- `-T 1..3`
- Phase 0 move/swap/compound search
- Exact incremental HopState
- Aggregated replication candidates and clusters
- Shared non-refunding growth budget
- Joint Phase 1 replacement fast path and the remaining full-MFS gap
- Per-trajectory and selected-trajectory output
- Regression constraint checks and actual measured results
- The missing Makefile `asan` target if it remains unresolved

Do not retain old experimental numbers as “current results” unless they are rerun on this branch with frozen partitions.

## 6. Recommended continuation order

1. Implement `parse_output` and `--self-test` first.
2. Update summary regexes for the new output.
3. Add constraints and exact-repeat comparison.
4. Decide whether strict completion requires closing the Task 9/10/11 gaps before benchmarking.
5. Obtain frozen partitions or create a deterministic partition-generation path.
6. Rebuild baseline commit `799702c` separately.
7. Run T1/T3/CEC/runtime acceptance.
8. Update `docs/csr.md` only with measured results.

## 7. Safety notes

- Work only in `/home/longfei/FoxSYN/.worktrees/csr-enhancement` unless deliberately creating an isolated baseline checkout.
- Preserve unrelated user changes.
- Do not delete the frame entry network while trajectories are running.
- Do not refund growth after deletion or rollback.
- Do not weaken hop, cut-net, node-growth, or balance constraints to make an artificial test pass. Test-only fixtures may explicitly widen their captured limits when the fixture itself is incompatible with production balance math.
