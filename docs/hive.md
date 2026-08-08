# `hive` Notes

## Purpose

`hive` is a read-only command that discovers convex, narrow-boundary, dense-interior regions in a mapped logic netlist. It finds Top-N such regions and prints a report; it performs no netlist changes and does not write any Pdb.

## Command Line

`hive` is registered as a FoxSYN command:

```text
hive [-N num] [-M num] [-I num] [-O num] [-K num] [-S num] [-v]
```

Options:

- `-N num` — number of regions to report; valid range `1 <= num`; default `20`
- `-M num` — maximum region node count; valid `1 <= num`; default `64`
- `-I num` — maximum input count `Imax`; valid `1 <= num`; default `32`
- `-O num` — maximum output count `Omax`; valid `1 <= num`; default `8`
- `-K num` — LUT width (for LB calculation only); valid `2 <= num <= 16`; default `6`
- `-S num` — seed count limit, `0` = all nodes; valid `0 <= num`; default `2000`
- `-v` — verbose mode (print member lists and boundary ids); off by default

Parameters are parsed in the style of other ABC-derived commands; invalid values cause immediate failure and usage message.

## Metrics

`N` is the number of internal nodes in the region (all nodes, including constants, are counted).

`in` is the number of distinct external drivers touching the region.

`out` is the number of nodes inside the region that have at least one external fanout.

`Q` is the density metric `N / (in + out)`.

`LB` is the lower bound on the number of LUTs needed to implement the region under the structural-pin-preserving, LUT-only model.

`gap` is `N - LB`.

`rank` is the strictly increasing topological rank assigned in the CombGraph snapshot.

**M1-M3 caveat**: LB is only a lower bound under a structural-pin-preserving, LUT-only model, and gap is NOT a lower bound for functional resynthesis. See docs/hive-design.md §2.5 for the full argument and counterexample.

## Report Format

The command prints:

```
hive: 4213 nodes, K=6, 2000 seeds
  #  root      N   in  out     Q    LB   gap  lvl
  0  n0873    38    9    2   3.45    2    36    5..11
  1  n1245    47   11    3   3.36    3    44   12..19
  ...
hive: 20 regions, 612 distinct nodes (14.5% of netlist), Q in [1.82, 3.45]
```

When `-v` is given, each region includes its member node id list and `in`/`out` object id lists. A final line states the model caveat for `gap`.

## Preconditions and read-only guarantees

- Current network must exist and satisfy `Abc_NtkIsLogic(pNtk)`.
- No `Pdb` is required or consumed.
- Never mutates network structure or `pData`; never writes `Pdb`.
- Allowed side effects are limited to internal caches (travId increments and paired `vFanouts.nSize` adjustments inside `Abc_NodeMffcLabel`).
- If preconditions fail, the command prints a reason and returns failure.
- Empty network (no internal nodes) succeeds but reports zero regions.

## Related Files

- `src/hive/hive.hpp`
- `src/hive/hive_graph.{hpp,cpp}`
- `src/hive/region.{hpp,cpp}`
- `src/hive/convex.{hpp,cpp}`
- `src/hive/hive_internal.hpp`
- `src/hive/hive.cpp`
- `src/test_hive.cpp`
- `docs/hive-design.md`
