# RD3-R3 R3-A implementation

Authority: the user's 2026-10-13 R3-A instruction adopts the attached RD3-R3 design. `design/` is the unmodified 65-file source package, including all 33 chapters, 9 work orders and 14 appendix tables. Its original DRAFT labels describe package authorship, not an additional approval requirement. Earlier RD2/RD3/RD3-R2 documents and qualifications remain historical, unchanged.

Input ZIP SHA-256: `af83aeb7ab691e9316748e7434a9176c220fc56a0761592965075925fca68856`. CRC and 450 package checks passed. Construction parent and observed remote HEAD: `960e755395be2c9f78a86339a062c507826c4eb4`. The original user checkout at `246fe21eb6d060801b9a16ddcadde2be89dbbecc`, including its modified `.vscode/settings.json`, is separate and untouched.

## Actual ownership

| Object | Sole owner / identity authority |
|---|---|
| InstanceContext | Backend's Native bootstrap `unique_ptr`, declared before Session; never moved after construction |
| DeviceSession | Backend `unique_ptr`; borrows InstanceContext; no Session ID or second device wrapper |
| DeviceContext | Session; existing VulkanDevice, VMA allocator and stable SubmissionQueue owners; aliased roles borrow one queue |
| ResourceContext | Session; borrows that allocator and graphics queue; owns sampler, the converged ResourceDomain, and execution pool |
| Scene / View | Backend Scene SlotMap / Scene-local View SlotMap; ViewRef always carries both generations |
| Effect / Target / Graph | Existing Backend Effect/Target SlotMaps / Session Graph SlotMap |
| EffectAttachment | `SlotKeySparseSet<RenderEffectId, vector<unique_ptr<EffectAttachment>>>`; sole editable use relation |
| PreparedExecution | Ordered vector of Attachment/View borrows only; cold topology changes refresh View addresses after SlotMap compaction |
| ResourceDomain | One Session-local typed store, keyed by validated Scope + full type identity + canonical string key |
| Native persistent backing | ResourceDomain's `SlotMap<unique_ptr<ScopedResourceEntry>, ScopedResourceTag>`; entry and value addresses remain stable |
| GraphExecutionContext | Existing Session execution pool; owns FIF transient/descriptor state and actual submission evidence |

ResourceDomain is the bounded refactoring of the existing typed factory/entry store. There is no parallel ScopedResources store, no Domain tree, no ResourceDomainId, and no Scene/View Domain fields. Its Native-only header no longer enters neutral Objects/Types/Runtime/Backend headers. ScopedResourceId comes from lux-cxx and is packed losslessly into the existing Graph import identity; no hash or manual allocator supplies runtime identity. Sparse indices do not allocate another identity.

ExecutionJob is removed. BindingExecution now records only Native slot occupancy, invocation values and receipt metadata, not another ViewRef relationship. Attachment pins the immutable Native Graph code without duplicating Shader/Pipeline owners. The execution pool remains indexed by the owner's RenderGraphId. No new unordered_map/unordered_set object authority exists.

## Attach and release

Backend validates endpoint owner, Scene/View generations, Effect, Target extent and writer conflicts before constructing the candidate. Explicit `ScopedBufferRequest` values declare scope and key; scope is not inferred from a shader. Global uses the Session namespace, Scene adds SceneId, View adds ViewId, and Attachment adds EffectId and TargetId. No persistent resource is allocated by createScene/createView.

The candidate acquires move-only typed leases, prepares logical/Native imports, then obtains the existing Native execution slot. Only a complete candidate enters the effect table and ordered plan. Duplicate attach is idempotent. Non-OOM errors preserve existing attachments. Last-good behavior is not a claim that R3-B Effect revision transactions are implemented.

Detach removes the non-owning schedule entry, destroys the Attachment, marks its Native slot unoccupied and releases logical leases. The ResourceDomain is the physical owner throughout. It collects zero-lease entries only after actual SubmissionQueue tickets complete. Pending backing is retained until the next cold collection or Session teardown; there is no per-frame registry collection/allocation. Tickets are conservatively captured from all three existing queue roles after every execution attempt, including partial failure. Slot reuse separately requires all FIF slots to be safe.

Shared immutable imports carry the latest accepted Graphics join, not only the creation ticket: this orders another reader even while the first reader is pending. Executor accesses leases/import arrays directly without Scene/Effect/resource lookups. The existing Native import validation, C0-C13 compiler, Shader/PSO, barriers, Sync2, queue submission and retirement algorithms are unchanged.

Stop follows existing R2 admission/drain. Then schedule/Attachments release, GraphExecutionContexts wait all accepted partial/success tickets, Session drains remaining queue work, scoped backing and Graph owners release, then queues, allocator, device, and finally bootstrap Instance. Client/Reply storage remains independent.

## Concrete R3-A slice and limits

The existing BufferFactory now creates a real DEVICE buffer and initializes it with an existing graphics CommandBatch and real ticket wait. This is deliberately cold and synchronous; it is not the R3-D bulk transfer pipeline. Full size/usage/device/access compatibility is checked on a hit. Read-only imported buffers on the graphics role are supported in this slice; mutable providers, cross-family imports, image storage, ResourcePlan generation and professional storage remain subsequent work. Graph transient remains owned by GraphExecutionContext.

Retention currently implements OnLastDetach with deferred cold collection. It does not claim Cached/Explicit policy APIs, eviction budgets, WSI completion, or mutable history publication. Bound Scene/View/Target removal retains P2-A's explicit Busy contract; callers detach first. Chapter 26 permits that choice. The appendix B05 automatic-detach wording is therefore a future behavior choice, not silently rewritten evidence. REQ10/11/22 still contain inherited Domain/history vocabulary; the adopted chapters 04/19/26 and this actual ownership record govern R3-A. Historical columns and original package bytes remain untouched.

Normal heap and device OOM terminate at the new Runtime/Resource factory boundary. The historical P2-A OOM-retry test and Domain-tree test are preserved byte-for-byte in `runtime/test/archive/`; their old qualification is unchanged. Active tests replace that superseded contract with a child-process fatal assertion and non-OOM candidate rollback. No relevant Pixel, transport, late Reply or synchronization assertion was removed.

## Validation intent and evidence

Build the bootstrap's entire `all` target with C++20/MSVC/Ninja, `-j 4 -- -k 0`, then a no-work second build. Independent headers cover neutral Runtime and explicit Native headers. Exact results belong to the separate Verification V report, not this implementation note.

Active RD3 tests retain the original two Scene/four View/same Graph/FIF=2/3 RGBA oracle. Added coverage exercises Global/Scene/View/private buffers, two keys of one type, complete configuration conflict, three factory types, move/release, generation reuse and entry address stability. The fault executable instruments only the existing BufferFactory's fill call: distinct initialization words prove actual Native sharing and private output isolation through the same Graph's readback passes. Existing compute/tonemap/final Target shaders are reused. New buffer passes reuse F5CReadBuffer; there is no second graph/shader implementation.

A real GPU Event blocks an accepted queue submission. Two Graph slots consume the same buffer while pending; Attachment destruction drops its logical lease, collection must retain backing, and only actual completion allows reclaim. Fault tests retain accepted partial-submit work, non-OOM failures, host event/thread rollback, late client/reply tests and zero final Native owner counts. Stable scoped frames must allocate zero C++ heap bytes and create no extra Shader/Pipeline.

Affected public headers are synchronized to the Debug, RelWithDebInfo and Android include prefixes, with backups outside source. No Android qualification is claimed. Dependencies match P2-A exactly and are clean. Full historical F4/F5/F6.b confidence intervals and 4K footprint are not repeated or requalified.

## Boundaries and next entry

ALLOWED: Runtime ownership/attachment/resource production files; their tests and archived superseded contracts; `cmake/render-v2-bootstrap/RD3Runtime.cmake`; new `docs/render-v2/rd3r3/**`.
READ-ONLY: all pre-existing docs/reports, frozen Legacy 719, graph/vulkan algorithms, transport implementation and sibling dependency sources.
FORBIDDEN: user modifications, main merge, R3-B-E implementation, Legacy product migration and H1-H6 algorithms.

All 86 capability, 25 workload and 51 requirement records remain in the package with stages and acceptance intent. Chapters 29-32 and GC01-GC20, MAT01-MAT14, UP01-UP16 remain mandatory future contracts: Legacy compiler pruning/local-read/layout/submission/transient mechanisms; existing MaterialGraph/ShaderIR/cook ownership; UPLOAD lane versus CPU jobs versus Native transfer queue; specialized Dirty/Paged/Arena/Bindless/streaming storage. None is promoted to a production PASS by this import.

R3-B entry: replace the fixed Native EffectSource author seam with Effect-owned Feature composition and explicit ResourcePlan derived from Feature/DataSchema/PassSchema/Graph uses; resolve prepared slots at attach using the existing lease store; publish every affected consumer atomically and retain last-good on recoverable errors. Add provider/writer/config conflicts and resource-version/output compatibility to that transaction. Keep full structural Graph identity, current native code sharing, and true ready/last-use evidence. Scope/state APIs for mutable resources and multi-target revision transactions are not implemented here. Stop after R3-A I/V push; R3-B requires new authorization.
