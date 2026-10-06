# SuperMap: FPGA Technology Mapping with AGDMap

**Status**: Production-ready for area mode; delay mode functional with known QoR gaps  
**Last Updated**: 2026-10-06  
**Total Development**: Phase 0–10 complete (10 phases, 5 optimization stages)

---

## Overview

SuperMap is an FPGA technology mapper that implements:
- **Plain LUT mapping**: Standard K-feasible cut enumeration + area/delay flow optimization
- **AGDMap integration**: And-gate decomposition for wide-gate FPGA architectures (configurable via `-g`)

This mapper was ported from the original `agdmap` reference implementation with significant enhancements:
- Iterative area/delay passes with backward flow reselection
- Multi-pass convergence with fanout re-estimation
- Persistent cost tracking and dominance pruning
- Complemented primary output handling

---

## Current Status

### ✅ Completed Features

**Core Functionality (Phase 0-4)**
- K-feasible cut enumeration with dominance pruning
- Area-flow and edge-flow cost model
- AGDMap bin-packing decomposition (area mode)
- Basic graph export to ABC network
- Cut merging and truth table computation

**Delay Mode (Phase 5-7)**
- Delay-oriented cut selection and arrival time tracking
- Delay-mode AGDMap decomposition (multi-level bin trees)
- Iterative delay/area passes (3 delay + 2 area rounds)
- Backward flow area recovery for delay mode

**Quality & Correctness (Phase 6, 8, 10)**
- Regression test framework (15 designs × 4 modes)
- Complemented PO export fix (Phase 8: no inverter injection)
- `-D -g` CEC correctness fix (Phase 10: bin overflow + truth-table expansion)
- All 60 test cases CEC-verified equivalent

**Plain-Path Optimization (Stage A-E)**
- Stage A: Persistent area cost tracking across passes
- Stage B: `BackwardFlowPlain()` area recovery for plain path
- Stage C: Per-cutsize cut cap (retain more size-2 cuts)
- Stage D: Within-pass convergence (iterate until stable)
- Stage E: Delay-mode cut pruning + schedule alignment

---

## QoR Summary

### Area Mode (geo mean vs original agdmap)

| Metric | SuperMap (smap -a) | Original agdmap | Δ |
|---|---|---|---|
| **P4 LUT** | **0.8456** | 0.9651 | **-12.4%** ✅ |
| Edge | 0.9048 | 1.0168 | -11.0% |

**Stage A-E impact** (plain path only, no `-g`):
- Stage 0 → Stage E: Area 1.0519 → 1.0205 (-3.0%), Delay 1.1347 → 1.0951 (-3.5%)

### Delay Mode (geo mean vs reference)

| Mode | Delay Lev | vs agdmap | vs ABC `if -K 6` |
|---|---|---|---|
| `smap -D -g` | **0.9514** | +4.3% behind | — |
| `smap -D` (plain) | 0.9911 | — | — |

**Known gap**: Plain delay mode (`smap -D`) has +11-22% area overhead vs `if -K 6` due to missing area recovery passes (documented in Phase 9).

---

## Architecture

### Command-Line Interface

```bash
smap [-a|-D] [-g] [-W] [-d dotfile]
  -a      Area-oriented mapping (default)
  -D      Delay-oriented mapping
  -g      Enable AGDMap wide-gate decomposition
  -W      Diagnostic: widen delay-mode cut retention pools
  -d FILE Export graph structure to DOT format
```

### Key Files

| File | LOC | Purpose |
|---|---|---|
| `map.cpp` | 1438 | Mapper main flow: cut enum, iterative passes, cost selection |
| `map.hpp` | 1120 | `mapper` class: config, state, AGDMap interface |
| `agdmap.cpp` | 627 | Bin-packing decomposition, mapping solution tree builder |
| `graph.cpp` | 303 | Graph export to ABC, complemented PO handling |
| `cut.hpp` | 476 | Cut storage, kCut truth tables, cut merging |
| `prune.hpp` | 159 | Dominance pruning, area/delay filtering |

### Core Data Structures

```cpp
struct Cut {
    uint size;              // Number of inputs
    uint leaves[MAX_SIZE];  // Fanin node IDs
    uint fid;               // Function ID (truth table)
    // AGDMap cuts carry decomposition tree (linked Cut chain)
};

struct Bin {
    uint leaves[6];    // Real cut leaves (post-union)
    uint8 cuts[16];    // Sub-cut indices in decomposition
    uint8 ibins[6];    // Fanin bin connections
    uint root;         // Root node ID or virtual offset
};

class mapper {
    std::vector<std::vector<Cut*>> _cuts;  // Per-node cut sets
    std::vector<Cut*> _best_cuts;          // Selected solution cuts
    std::vector<float> _area, _edge;       // Flow costs
    std::vector<Time> _arrival, _required; // Timing
};
```

---

## Remaining Work

### High Priority

**1. Phase 9: Delay-Mode Area Recovery** (design complete, not implemented)
- **Problem**: `smap -D` has +11-22% area vs `if -K 6` (e.g., alu4: 219 vs 198 LUTs)
- **Root cause**: No backward area recovery after delay optimization
- **Solution**: Three-step coordinated approach (must be implemented together):
  1. Ideal-depth target calculation (per-node optimal level)
  2. Hard-gate filter (reject cuts that exceed slack budget)
  3. Forward-topological MFFC exact area pass
- **Verification**: alu4/priority/C5315 area ↓, Delay Lev geo ≥ 0.9514, 4-mode CEC pass
- **Caution**: Two prior attempts failed (5.6b degraded area/edge; hard-gate alone regressed Lev 3.2%)

**2. Regression Script Coverage Gap**
- **Problem**: `run_qor_compare.py` only tests `-g` modes; Stage A-E improvements (plain path) not validated
- **Impact**: Cannot detect plain-path regressions in CI
- **Fix**: Add `-a` and `-D` columns to regression table

### Medium Priority

**3. G1: Area-Flow Formula Alignment**
- Current: `Σ A/f + 1` (area per fanout + LUT)
-論文: `Σ (A-1)/f + 1` (amortize per-node cost)
- **Expected gain**: 3-5% area improvement (low risk, local change)

**4. Wide-Gate Path Optimization**
- Stage A-E only applied to plain path; `-g` modes still use Phase 5-7 baseline
- **Unknown**: Whether Stage B/D/E patterns transfer to AGDMap decomposition
- **Blocker**: Need evaluation; may require decomposition-aware backward flow

### Low Priority / Design Debt

**5. Two-Dimensional Dominance Pruning**
- Current: area-flow pruning OR delay pruning (mode-dependent)
- Ideal: Full `(area, delay)` Pareto frontier
- **Note**: Non-blocking; current heuristic already competitive

**6. `-W` Flag Promotion**
- Currently diagnostic-only; widens cut retention in delay mode
- **Decision needed**: Default behavior vs. opt-in flag

**7. State Snapshot for Safe Experimentation**
- Phase 5.6c audit: current snapshot insufficient for rollback (only stores best cuts + stats)
- **Impact**: Cannot safely A/B test alternate decomposition strategies (5.6a/5.6e proved risky)
- **Blocker for**: Delay-tree replacement, seed pass variants

---

## Known Limitations

1. **No multi-output LUT support**: Each LUT has exactly one output (standard academic assumption)
2. **No retiming/register optimization**: Pure combinational mapping
3. **No placement awareness**: Does not consider wire delay or congestion
4. **AGDMap limited to And-gates**: Cannot decompose Mux/XOR directly (must go through AIG)

---

## Verification

### CEC Coverage (60 test cases)

| Mode | Designs | Status |
|---|---|---|
| `-a` | 15 (MCNC + VTR) | ✅ All equivalent |
| `-D` | 15 | ✅ All equivalent |
| `-a -g` | 15 | ✅ All equivalent |
| `-D -g` | 15 | ✅ All equivalent (Phase 10 fix) |

### QoR Regression Suite

Located in `regression/`:
- `SimpleCircuits/mcnc/`: 10 MCNC benchmarks (alu4, apex2, C5315, C7552, des, ex5p, i10, k2, misex3, priority)
- `SimpleCircuits/vtr/`: 5 VTR designs (LU8PEEng, LU32PEEng, sha, diffeq, fpu)
- Scripts: `run_qor_compare.py`, `run_supper_phase6_smoke.sh`

**Smoke test**:
```bash
cd regression && ./run_supper_phase6_smoke.sh
# Expected: 12/12 [OK], 0 failures
```

---

## Development History

| Phase | Date | Commits | Focus |
|---|---|---|---|
| 0 | 2024-01 | e635cf3 | Initial port from agdmap reference |
| 1-4 | 2024-02 | — | Area-mode core functionality |
| 5.1-5.7 | 2024-03 | — | Delay-mode implementation + iterative passes |
| 6.0-6.6 | 2024-04 | — | Bug fixes, regression framework, snapshot |
| 7.1-7.2 | 2024-05 | — | Delay-tree refinement (5.6a/5.6e rejected) |
| Stage A | 2024-06 | 221cdc9 | Persistent area cost |
| Stage B | 2024-06 | 221cdc9 | BackwardFlowPlain area recovery |
| Stage C | 2024-06 | 221cdc9 | Per-cutsize cap tuning |
| Stage D | 2024-06 | 221cdc9 | Within-pass convergence |
| Stage E | 2024-06 | 44fb574 | Delay pruning + schedule alignment |
| 8 | 2024-08 | faac7f8 | Complemented PO export fix |
| 9 | 2024-09 | (design) | Delay area recovery (not implemented) |
| 10 | 2026-09-23 | f518035 | `-D -g` CEC fix (bin overflow + truth expansion) |

---

## References

- Original AGDMap paper: Cong & Hwang, "Simultaneous Depth and Area Minimization" (1995)
- ABC mapper: `if` command in Berkeley ABC
- Project documentation: `FoxSYN-supermap/.claude/work_state.md` (detailed phase log)

---

## Contact & Notes

This mapper is production-ready for **area-oriented mapping** (`smap -a`). Delay mode works correctly but has known area overhead (Phase 9 gap). For critical delay-area tradeoff applications, consider implementing Phase 9.3 before deployment.

The AGDMap integration (`-g` flag) is most beneficial for wide-gate architectures (AND-gate primitives with >6 inputs). For standard 6-LUT FPGAs, plain mode (`smap -a` / `smap -D`) often suffices.
