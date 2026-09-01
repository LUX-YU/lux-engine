# Pre-L5 Foundation Hardening Plan

Status: **Complete**

Qualified implementation: `fe46e7ecf47c38d4d57131101fdcfc37e3327115`

Baseline: `e26faaac12409e4cdd9b5dd33d6caf912413df89`

Date: 2026-09-01

## Objective

Close four source-level P1 defects before L5 work begins:

1. malformed public MaterialGraph input must fail closed before lowering;
2. the installed Material compiler must be relocatable and independent of the original source/build trees;
3. shared Shader reflection/LGLSL support must have Toolchain Shader ownership rather than crossing a sibling
   package's `pinclude` boundary;
4. GraphKit compound edit, undo and redo transactions must be atomic.

The same closure corrects `node_graph_editor` to the EDITOR layer, marks the stale Script/FlowForge freeze document
as historical, and creates the unfinished-work index promised by AGENTS.md.

## Frozen decisions

- Material's node vocabulary is closed to the built-in final node types; no RTTI extension seam is added.
- GraphKit uses a preallocated inverse-operation journal, not whole-document snapshots.
- `.internal/UNFINISHED-WORK.md` is an index of active Held ADRs and reopening evidence, not a duplicate design spec.
- L1-L3 remain closed. L5 panels, Asset residency, Product streaming, Script capability injection and GPU CI remain
  outside this implementation.
