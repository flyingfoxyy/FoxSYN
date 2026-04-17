# ABC Pdb Notes

## Purpose

`Pdb` stores per-object physical partition information for `Abc_Ntk_t`. It is used to attach a `part_id` location to objects in an ABC network without changing the original logical meaning of the netlist.

## Data Model

- `part_id` is a type alias of `uint8_t`.
- `0xFF` is the only invalid value and is exposed as `ABC_PART_ID_NONE`.
- `0` is a valid partition ID.
- `Pdb` stores data as `vector<part_id>`, indexed by `Abc_Obj_t::Id`.
- `Abc_Ntk_t::pPdb` is `NULL` by default. This means the network currently has no physical information.
- `pPdb` is allocated lazily on the first write of a valid `part_id`.

## Object State

- `Abc_Obj_t::fCutNet` is a one-bit flag.
- `fCutNet = 1` means:
  the object has a valid `part_id`, and at least one fanout also has a valid `part_id`, and the two partition IDs are different.
- If the object itself has no valid `part_id`, `fCutNet` is `0`.
- If a fanout has no valid `part_id`, that fanout is ignored when evaluating `fCutNet`.

## API Semantics

- `Abc_NtkGetPartId()` / `Abc_ObjGetPartId()` return `ABC_PART_ID_NONE` when no physical information is present.
- `Abc_NtkSetPartId()` / `Abc_ObjSetPartId()` write a valid partition ID and update affected `fCutNet` state.
- Writing `ABC_PART_ID_NONE` is treated as clearing the object partition.
- `Abc_NtkClearPartId()` / `Abc_ObjClearPartId()` clear one object’s partition.
- `Abc_NtkClearPartIds()` removes all physical information, frees `pPdb`, sets `pPdb = NULL`, and clears all `fCutNet`.
- `Abc_NtkUpdateCutNets()` recomputes `fCutNet` for the whole network.

## Update Rules

- Incremental topology edits such as add/delete/patch fanin update the related `fCutNet`.
- Network duplication copies `part_id` and then recomputes `fCutNet` after the new topology is fully connected.
