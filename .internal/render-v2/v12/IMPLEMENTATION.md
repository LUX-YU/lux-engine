# Render V2 v12 implementation

Baseline: `78bcd220815f845466d85d31f994901e4db224ba` (production `b427f9d5ac99bcec42a154e6a865585c058a3365`).
Implementation checkout: `E:/RenderV2Rebuild/V12/source`. Evidence root: `E:/RenderV2Rebuild/V12/evidence`.

## N0 — baseline and migration contracts

The v12 decisions supersede conflicting historical proposals. Preserve the single Graph/Analysis,
typed IPC object ownership and failed-request recovery, CommandId/RequestId generations, DeviceSession,
Transfer, physical Queue Tickets and monotonic multi-queue LastUse. No IPC queue algorithm change.

Protection includes the original checkout's settings and Graph.hpp, and the Layer checkout's newly
modified Storage.hpp. Neither checkout is the implementation destination. Frozen Legacy sources,
all pre-existing reports/checkers and dependency checkouts remain read-only. Only new v12 records
are mutable. Production changes are limited to Render, the Execution migration and actual consumers,
their build/install/tests, existing shader/material toolchain seams required for N5/N6, and the new example.
An IPC wake notification, if actually needed, requires evidence in N7 and must not change the queue algorithm.

| Stage | Existing authority → resulting authority | Consumers / gate |
|---|---|---|
| N1 | process_execution → core_execution, same runtime and algorithms | Engine context, asset/world/object loading, scene composition/script, Editor, Render fixtures and installed consumers |
| N2 | single-binding CommandEntry → Command routing table + unique Handler object table | Client discovery/select; FeatureId; one object serving multiple member bindings |
| N3 | GPU/Readback/CPU variant → generic Pending source owner + professional operation | Upload/readback/compile; reserve before effects, source recovery, callback tail/code pin safety |
| N4 | Domain-owning Registry → actual Global/Scene/View/Attach StorageSets + borrowed index | Attach, qualified StorageId handles, Texture and Buffer extensions, last-use retirement |
| N5 | encoded in-process config/fixed demo ABI → typed immutable config + unique final LayoutPlan | live Graph lowering, actual shader parameters and resource bindings, optional readback |
| N6 | color-only fixture → actual Mesh/Material/Instance and Camera | two views/effects, indexed/indirect, batch transforms, numerical GPU oracle |
| N7 | fixed wait / indistinguishable skip → measured bounded wait and fairness | separate performance commit; coverage map, fresh installed SDK, independent verification |

N2's final gate cannot retain gpu_completion_ just to avoid the N3 dependency: migrate the minimum
generic Reply seam first when needed. No second production path or compatibility forwarding header.
After defer moves an SBO request, reborrow from Pending; the original entry reference is invalid.
Before logical Storage destruction, withdraw future executions and unlink their Storage borrows;
submitted work retains physical backing through actual Queue requirements. Destructor retirement
metadata is acquired before publication, never allocated by a fallible destructor path.

## Layout sources and reuse boundary

The baseline LglslEmitter::emitPassGlsl injects provisional positions. N5 reuses its PassShaderContract
path and the frozen RenderGraphCompiler.LayoutAllocator / LayoutPlan / SPIR-V relocation algorithms
as source references, without linking or editing Legacy. Final shader locations, pipeline layouts and
descriptor writes must all derive from one resulting plan. Shared descriptor table shape cannot be
inferred from the subset reflected by a single shader. Source blob/SHA inventory is in N0 evidence.
Existing material toolchain and resource-description Mesh/Material contracts must be consumed rather
than replaced with another author graph or shader parser.

## Qualification and evidence

The prior 102-test matrix is historical evidence, not a new run. N0 records its names/invariants and
baseline raw performance artifacts; Native and Alias are separately measured. N1–N6 preserve wait
policy for comparison. Only N7 changes fairness/waiting, in a separate commit. Historical performance
PARTIAL and Root TOOLCHAIN BLOCKED are not rewritten. New measurements cannot close unproven causality.

Each stage has a buildable implementation checkpoint and separate validation record. Final verification
uses an independent no-alternates clean clone of exact I; V only adds evidence/reports. Release, Debug,
ASan, real GPU validation, install relocation, public headers and actual affected consumer closures
must be covered. Test count may change only with explicit invariant-to-replacement mapping.

Package discrepancy: appendix S04 names Code_Atlas_v1, absent from the package; Member_Function_Audit
is present. Actual source inventories supply the missing reference; no claim that the absent file was read.

Current status: N0 PASS; N1–N7 NOT_STARTED. N0 verified 719 blobs, three user files and clean dependency SHAs; a present probe passed and an absent probe failed as required. A/A ran the same historical b427 binaries four times each for Native, Alias and Command (12 successful runs), preserving all samples. This is a measurement baseline, not a performance improvement claim. No new functional or performance PASS is claimed.
