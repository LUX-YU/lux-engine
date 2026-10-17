# N5 Native layout and binding implementation freeze

This is the current construction design, not a completed gate. Existing G4 analysis/order, Effect-owned
CompiledGraph, DeviceSession allocations, Queue tickets and Context retirement remain unique.

1. Cooked Shader metadata names source descriptors. PassProgram maps those shader names to actual G4
   resource keys in cold compilation. StorageDescriptor loses set/binding entirely. A single Native
   LayoutPlan resolves source locations to final shared-table/private locations and supplies relocation,
   VkPipelineLayout creation and descriptor writes. No Runtime lookup is added to recording.
2. The existing Device descriptor array has one frozen complete Native shape. TextureStorage requests
   slots and provides ImageView/Sampler values; it cannot choose positions. Reflection subsets do not
   define this shared table. Private buffer bindings are packed, not assigned one set per resource.
3. Extract controlled decoration relocation from the audited Toolchain/Legacy algorithms. Preserve
   original-word collection, bounds checks, unsupported-decoration rejection and actual SPIRV-Tools /
   SPIRV-Cross reconciliation. No new language parser or Engine production dependency. Existing lglsl
   Pass emitter is used only by asset-generation/test tooling; inputs/tool/artifact hashes are retained.
4. ExecutionContext owns private descriptor sets alongside its existing physical execution resources.
   Reserve two descriptor banks at Attach. This is sufficient because execute() permits at most one
   submitted incomplete execution per Context: before a second submit, the first is observed complete.
   Owner-side replacement writes the bank not referenced by that incomplete submission; further updates
   before the next submit may rewrite the unpublished/free bank. A bank never changes while GPU uses it.
   Both banks retire with the Context's already-reserved retirement entry, not another map/retire queue.
5. PreparedStorageBinding subscriptions notify the actual Context on Owner-thread publication. Rebinding
   updates Native handles, descriptor selection, barriers, parameter sources and LastUse together. All
   maps, indices, arrays and descriptor capacity are prepared cold; no allocation/name lookup per draw.
   Withdrawal disconnects subscriptions while Storage is alive, before releasing leases/Context storage.
6. Explicit graph readback is already independent of drawing. The new layout work must retain its real
   Copy/Host barrier and Ticket completion, and retain color-only zero-readback allocation behavior.

Required counterexamples: provisional collisions across shader stages/includes; full shared table vs
reflection subset; missing/wrong name/shape; private buffer packing; two logical sampled resources;
replacement while old bank is in flight, repeated replacement before new submit, exact old backing
retirement; R5 same/different shader scope counts; no hot allocation. Multi-parameter and expanded ABI
work remains part of N5 and must replace current float4/push16 restrictions before final N5 acceptance.

Sources and frozen hashes: algorithm-sources-corrected.json; actual additional controlled relocation
source engine/toolchain/shader/src/SpirvRelocation.cpp. Never link or modify render_legacy.
