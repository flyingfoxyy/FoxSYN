# `hpart` Notes

## Purpose

`hpart` partitions the current ABC network with an external hypergraph partitioner
(`hmetis`, `shmetis`, or `kmetis`) and writes the partition result back into ABC
physical partition data (`Pdb`).

After `hpart` succeeds, the network stores:

- per-object `part_id`
- per-object `fCutNet`
- network-level partition stats:
  - number of partitions
  - cut size
  - hop number

These stats are then visible through `ps` / `print_stats`.

## Command Line

`hpart` is registered as a FoxSYN command:

```text
hpart [-T hmetis|shmetis|kmetis] [-N num] [-v]
```

Options:

- `-T name`
  - partitioner name
  - valid values: `hmetis`, `shmetis`, `kmetis`
  - default: `hmetis`
- `-N num`
  - number of partitions
  - valid range: `2 <= num <= 255`
  - default: `4`
- `-v`
  - print the external partitioner command and keep its stdout/stderr visible

Example:

```text
read regression/SimpleCircuits/mcnc/alu4.v
hpart -N 4 -v
ps
```

## External Tool Requirements

`hpart` searches the selected partitioner in `PATH`.

- If the executable is not found, the command fails.
- A temporary directory is created under `/tmp` with the pattern
  `foxsyn_hpart_XXXXXX`.
- The generated hypergraph file is named `network.hgr`.
- The partition file is read from `network.hgr.part.<N>`.
- Temporary files are deleted automatically when the command returns.

For `hmetis` and `shmetis`, the command currently uses:

```text
<tool> <graph_file> <N> 2 10 1 1 0 0 0
```

For `kmetis`, the command currently uses:

```text
<tool> <graph_file> <N>
```

If the partitioner exits with a non-zero status but still produces a valid
partition file, `hpart` keeps going and uses that file.

## Effective Scope

`hpart` is currently intended for combinational networks without latch objects.

The command will fail immediately if:

- there is no current network
- the requested partition count is invalid
- the network contains any latch
- no executable partitioner is found
- the generated hypergraph has no vertices or no edges
- the partition file cannot be read back
- cut size or hop number computation fails

## Hypergraph Construction

`hpart` converts the current network into a hypergraph before calling the
external partitioner.

### Hypergraph Vertices

The following ABC objects are treated as hypergraph vertices:

- `PI`
- internal `NODE`
- `CONST1`

The following objects are not hypergraph vertices:

- `NET`
- `BI`
- `BO`

Latches are rejected before partitioning, so they do not appear in a successful
`hpart` run.

### Hypergraph Edges

Each hyperedge is built from one carrier vertex and all reachable sink vertices.

Carrier objects:

- `PI`
- internal `NODE`
- `CONST1`

Traversal rules:

- start from each carrier's fanouts
- recursively traverse only interconnect objects:
  - `NET`
  - `BI`
  - `BO`
- stop when another hypergraph vertex is reached

An edge is emitted only when it has at least two pins:

- the carrier vertex itself
- at least one reachable sink vertex

`PO` is ignored during hypergraph construction.

## Writeback Flow

After the partition file is read:

1. clear all existing `part_id`
2. create one `Pdb` object sized by `Abc_NtkObjNumMax()`
3. assign the new `part_id` to each hypergraph vertex
4. traverse the network once to recompute `fCutNet`
5. traverse the network once to compute `hop number`
6. derive `cut size` from the marked `fCutNet`
7. store `{num_parts, cut_size, hop_num}` into `Pdb`
8. print a summary

The logical network itself is not rewritten. `hpart` only updates physical
partition metadata.

## Partition Statistics

`hpart` relies on the base ABC APIs below:

- `Abc_NtkComputeCutSize()`
- `Abc_NtkComputeHopNum()`
- `Abc_NtkSetPartStats()`
- `Abc_NtkGetPartStats()`

These APIs are declared in `src/abc/src/base/abc/abc.h`.

### Participating Objects

The following objects participate in `cut size` and `hop number` computation:

- `PI`
- internal `NODE`
- `CONST1`

The following objects do not participate:

- `PO`
- `NET`
- `BI`
- `BO`
- latch

### `fCutNet`

`fCutNet` is maintained on fanin objects.

A fanin object is marked as cut when:

- the current object has a valid `part_id`
- one of its fanins also has a valid `part_id`
- the fanin partition differs from the current object partition

This is checked directly on explicit fanin edges. There is no recursive sink
collection in the current implementation. `PO` is skipped, so a `PO` boundary
does not mark its driver as cut.

### Cut Size

`cut size` is the number of participating `PI/NODE/CONST1` objects whose
`fCutNet` bit is set after the direct fanin scan.

### Hop Number

`hop number` is computed with one forward dynamic-programming pass over the
network.

Each object stores one local hop level, initialized to `0`.

For every participating object:

- inspect all fanins with valid `part_id`
- compute:
  - `fanin_hop + 1` if fanin partition differs from current object partition
  - `fanin_hop + 0` otherwise
- the current object hop level is the maximum candidate over all fanins
- the network hop number is the maximum hop level seen over all objects

Example:

- `a -> b -> c`
- `a`, `b` are in `part0`
- `c` is in `part1`

Then:

- `hop(a) = 0`
- `hop(b) = 0`
- `hop(c) = 1`

### Error Conditions

The current implementation does not treat missing `part_id` as an error.
Objects without a valid `part_id` are skipped by `cut size` and `hop number`
computation.

## Summary Output

After a successful partition, `hpart` prints:

```text
tool = <tool>, parts = <N>, cut size = <cut>, hop num = <hop>
PART 0 -> ...
PART 1 -> ...
...
```

Later, `ps` prints the same partition stats through `Pdb`, for example:

```text
pdb : part =  4  cut =   68  hop =    6  pavg = 31.5  pmin = 28  pmax = 35
```

Where:

- `part` is the partition count
- `cut` is the cut size
- `hop` is the hop number
- `pavg` is average number of assigned objects per partition
- `pmin` is the minimum partition size
- `pmax` is the maximum partition size

## Current Limitations

- latches are not supported
- `PO` does not participate in hypergraph construction, `cut size`, or `hop`
- the command depends on an external partitioner executable instead of an
  in-process partitioning library

## Related Files

- `src/main.cpp`
  - command-line parsing and command registration
- `src/hpart/hpart.cpp`
  - hypergraph construction, external tool invocation, result writeback
- `src/abc/src/base/abc/abcPdb.cpp`
  - `part_id`, `fCutNet`, cut size, hop number, and stored partition stats
- `docs/abc_pdb.md`
  - lower-level notes about `Pdb`
