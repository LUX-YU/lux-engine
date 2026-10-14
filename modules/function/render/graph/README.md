# G4: author graph and logical analysis

Baseline: `4d0d6378558174622d8a9a7d6aca63fb1d608284`, fetched on 2026-10-14.
Authority: Draft 7.1, Implementation Plan v5 and G4 command dated 2026-10-14.

## Ownership and stage boundary (G4-0)

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
when the reader is conditional (G4 does not prove predicate implication).

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

Allowed: `graph/**`, Render component registration and required Graph build/tests.
Read-only: Core/Data/Protocol/IPC, Legacy (719 blobs), historical reports and user
settings. Forbidden: Feature/Shader/Native/Client/Server implementations or stubs.
Graph links Core only; no IPC/Protocol/GPU dependency. Root product closure is
separate from the bootstrap qualification. GPU tests are N/A in G4.

## Legacy algorithm review

`DependencyAnalyzer.cpp` supplies the useful resource-to-use indexing and stable
Kahn approach. G4 replaces its declaration-order last-writer inference, whole
resource hazards, hash maps and original-pass-index lifetimes with explicit
versions, exact overlapping ranges and final live positions. It rejects cycles
instead of appending remaining nodes. Legacy `RGBuilder`'s silent first-name-wins
imports and unresolved forward references are not retained: duplicate names and
unknown IDs are errors. Compiler culling/conditional availability are folded into
the single analysis; Native barrier/binding/render-pass/submission programs remain
read-only migration references for G7, not a second G4 compiler.

## Public entry points and verification

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

Seven durable Graph behavior groups cover authoring, byte/image ranges, versions,
topology/culling, conditional/history, lifetimes and independent discrete oracles.
The last group checks byte coverage and aspect/mip/layer cell lifetimes; it does
not call internal range helpers to compute its expected answers. Graph SDK tests
install into a new directory, move it, compile each public header and execute an
external `find_package(... COMPONENTS render_graph)` consumer. That consumer
asserts no Protocol/Data/IPC target was imported.

Build through `cmake/render-v2-bootstrap` using the verified sibling lux-cxx SDK:

```text
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> --output-on-failure -j 1 -R "^render\."
<build>/render/graph/test/render_graph_cost.exe > <external-evidence>/cost.csv
py -3 modules/function/render/graph/test/check_g4.py --clean --build <build>
```

`render_graph_cost` measures only `analyze()` on pre-authored independent and
chained buffer graphs (P=R=32/128/512), with three discarded warmups and 40 samples
each. It reports elapsed ns, successful allocation calls and cumulative allocated
bytes including temporaries and returned results, excluding builder construction,
output destruction and printing. This is cold compilation work, not per-frame
execution, GPU throughput or a zero-allocation claim. Dense overlapping-version
graphs may have quadratically many real hazards; no near-linear bound is claimed.

G5's next input is this immutable Definition/Analysis plus real Feature resource
requirements. Scope identities, Storage ABI, Shader contracts, Native ResourcePlan,
barriers, history rotation and GPU completion remain NOT_IMPLEMENTED here. Their
absence is a stage boundary, not a successful GPU/renderer qualification.
