# Author graph and logical analysis

## Ownership

`RenderGraphBuilder` owns its candidate value arrays. `finish()` validates local
structure and transfers them into a self-contained `RenderGraphDefinition` with
const accessors. Names, ranges and uses are owned, never borrowed from callers.
`analyze()` returns one independent `GraphAnalysis`; it owns derived values and
does not retain pointers into the Definition. There is no second editable graph.

`GraphPassId` and `GraphResourceId` are owner-local indices, checked against array
size, not runtime SlotKeys. They do **not** detect numerically equal IDs from a
different graph. Callers must carry the owning Definition; there is no global ID
allocator, provenance promise, or hidden name registry. Declare resources before
uses; pass dependencies can be added after both passes exist.

Resource versions are explicit author values. Version 0 means initial content;
positive versions denote writes, not pass declaration order. Different writers
may fill disjoint ranges of one version; overlapping writers of that version are
ambiguous. Ordered positive versions establish physical WAW and WAR constraints
only where ranges overlap. Reads name their version; READ_WRITE/attachment LOAD
also name an earlier input version. A skipped conditional write must preserve a
declared earlier version of the same range. Missing fallback is an error, even
when the reader is conditional; analysis does not prove predicate implication.

Imported/previous-history resources explicitly declare initially available
ranges. Previous history is read-only and separate from current history. An
unavailable previous-history read may explicitly select a compatible fallback
resource/version; the analysis records the actual selected binding. This is a
logical validity contract, not GPU readiness or a runtime predicate executor.

Exports and side-effect passes root the data/explicit-dependency closure. Dead
semantic branches can be culled; malformed local IDs/ranges are always rejected.
Live cycles, incomplete reads, ambiguous producers and unavailable fallback fail
with static RenderError IDs and numeric pass/resource/detail arguments. Lifetime
positions refer to the final culled order; export observation is position N.
Logical disjoint lifetimes alone never authorize Native memory aliasing.

All positive versions of a resource describe the same physical identity. A later
compiler must union its version/range lifetimes when assigning backing; it must
not independently alias a version merely because that version's readers ended.
Versions do not implicitly inherit unwritten ranges from earlier versions: either
write the complete requested range (possibly with disjoint writers), name the
earlier version for that range, or declare an in-place READ_WRITE/LOAD. Numeric
versions express author intent; gaps are allowed and never allocate sparse arrays.

Resource-to-use indexing and stable Kahn ordering determine the live execution order.
Hazards use explicit versions and exact overlapping ranges. Cycles are rejected;
duplicate names and unknown IDs are errors. Culling and conditional availability
belong to this single analysis. Native derives its barriers and recording plan
from the resulting Definition/Analysis rather than building another logical graph.

## Public entry points

`declareResource(GraphResource)`, `addPass(GraphPass)`, `dependsOn(before, after)`
and `exportOutput(GraphOutput)` return `RenderResult`. All reject invalid local
indices/ranges immediately; `finish() &&` also rejects duplicate complete keys
using a sorted index. Repeated identical dependency edges are canonicalized.
An empty graph or graph with no observable roots analyzes to an empty plan.
Explicit dependency cycles fail even if unobservable; unreachable semantic
read/conditional errors may be culled. There is no automatic latest-version
guessing, forward-name resolution, predicate implication engine or runtime cache.

Error arguments are `{pass, resource, detail}`; `UINT32_MAX` means no relevant
pass/resource. Ambiguity detail names the other writer; undefined-read detail
names the requested version. A cycle reports an actual cycle member, not an
innocent downstream node left over by Kahn. Errors are deterministic for the same
definition. Static descriptors are available for the host's existing error sink.

Build through `cmake/render-v2-bootstrap` with the sibling lux-cxx SDK:

```text
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> --output-on-failure -j 1 -R "^render\."
```

Analysis is cold compilation work, not per-frame execution. Dense overlapping-version
graphs may have quadratically many real hazards; no near-linear bound is claimed.
Logical history declarations do not implement runtime history rotation or GPU completion.

## Current public dependency

Graph depends on Core and its neutral resource description contracts. GraphPassId/GraphResourceId are declared in Core.
Program.hpp supplies the neutral PassProgram compiler input and imports cooked rdesc::SpirvArtifact from resource/description;
it contains no concrete Storage arguments. Native compilation
consumes the one successful Definition/Analysis directly; Feature selection is a separate upper layer.
