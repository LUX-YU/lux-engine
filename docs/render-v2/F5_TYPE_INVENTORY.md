# F5 type responsibility inventory

This inventories implemented types, not the RD2 target DataSet/Feature API. All
factories use existing RenderResult; ordinary heap OOM is fatal. No new registry,
virtual backend, reflection parser or persistent compiler object is introduced.

| Type | Role / unique responsibility | Owner, thread, failure / simpler form |
|---|---|---|
| RuntimePathTag / RuntimePathId | Value: local cold path index | StrongId primitive; no registry implementation |
| RuntimeAddress<Local>, RuntimeScene/View/Target/Path | Value: endpoint owner plus existing local identity | No owned resource; rejects foreign equal numeric keys; one small template avoids four identical structs |
| RuntimeTargetRect / RuntimeOutputPlacement | Value: explicit opaque output rectangle and order | Target owns copied placement; optional rectangle means full target, not an invalid zero rectangle |
| RuntimeCapacity / RuntimeNativeOptions / RuntimeOptions | Cold configuration values | Validated once; no independent mutable state authority; FIF and invocation capacity remain distinct |
| RuntimeProgress | Observational counters | Backend publishes atomic fields; snapshot is observational, not a scheduling transaction |
| RuntimeReply | Owned bounded reply bytes and source frame | R2 completion arena; no borrowed caller output or per-request allocation |
| RuntimeScenePublication / RuntimeViewPublication | Fixed F5 typed PROGRAM values and blob reference | Existing luxop traits; immutable retained packet, owner-thread producer; not F7 DataSchema |
| ERuntimeControl / RuntimeControl / RuntimeReadback | Fixed typed CONTROL values | Existing R2 request/reply, numeric argument validation; bounded CPU output |
| ERuntimeState | Terminal Runtime state value | Atomic observation; FAILED cannot resume or recreate a device internally |
| RuntimeRequest | Move-only RAII reception obligation | R2 inbox pin; poll consumes, destructor abandons; safe after Runtime destruction; no private heap state |
| ERuntimeFrameValue / RuntimeFrameField | Cold scalar-source binding value | Generated field index, not another schema; verifies type, offset and F2 invocation-input declaration; resource ABI members rejected |
| RuntimePathSource | Owning cold input transaction | Existing LogicalGraphPlan/programs/commands/values consumed together; initial borrow scope explicit |
| RenderRuntimeBuilder / Assembly | Short-lived builder | Complete native and transport assembly; finish transfers once and starts the real thread; no public init/shutdown |
| RenderRuntime / Impl | Unique running owner | Owns transport, backend and OS thread; bounded drains, wake/deadline, diagnostics, Stop/Drain; destructor joins |
| VulkanBackend / RuntimeBackendState | Unique native and Scene/View/Target authority | Cold assembly then only backend thread; actual R4 RAII owners and Core SlotMap; not a second GPU registry |
| HostHandle | Private move-only OS owner | Closes event/timer/thread HANDLE; moved-from destruction only; factory reports Win32 failure |
| RuntimeRoutes | Bound typed route values | One-time registration; no hot name lookup |
| ScenePassValue / RuntimeSceneState | Fixed F5 persistent Scene values | Scene owns the accepted values; View invocation copies bounded bytes, no second Scene authority |
| RuntimeViewState | View membership and independent dynamic values | Camera/history epoch/path/target bindings and receipts; removal cannot destroy Native slots prematurely |
| RuntimeTargetLayer / RuntimeTargetState | Target membership, shape and output order | Owns layer references and one optional WSI owner; removal/resize explicit; no Renderer per Target |
| RuntimePathState | Cold registered logical/native path | Immutable code, fixed commands, defaults and generated frame-field links; no per-frame compilation |
| RuntimeNativeVariant | Physical extent specialization owner | Shared exact shader/program owners, bounded invocation pool, trace and target backing; retired on real queue proof |
| TargetBacking / InvocationTargets | Actual native target backing and bound import list | Unique Image/ImageViews; destruction order keeps views before image; states are native completion evidence |
| VRuntimeAsset / RuntimeAssetBacking | Single native Buffer or Image owner and its state | Backend-owned; borrowed by multiple Scenes or one View history; CPU revision never replaces GPU Ready |
| PresentationLayer | Synchronous borrowed output invocation | Bounded per-frame scratch; carries exact retained receipt and native state, never outlives backing owner |
| PresentationTarget / Backing | One real Surface and output lifecycle owner | Window is host borrow; backend thread only; explicit capability and native failure |
| SwapchainBacking | Private native swapchain candidate owner | Acquired images, semaphores, acquire/present fences and readback buffers; cold rollback/teardown waits actual proofs |

Native Plan/Invocation types are inventoried in F5_IMPLEMENTATION.md. Stateless
`executeRuntimeFrame`, publication/control application and variant construction are
free algorithms; there is no FrameScheduler/TimeManager/FeatureManager object.

Future F6 responsibility: freeze a public Feature API target with no reverse
Runtime/Vulkan dependency and declare plugin codegen at plugin build/install time.
F5 assembly and fixed consumers are not that SDK. Future F7 responsibility: replace
fixed F5 payload assembly with the actual Scope-owned DataSet/Storage and asset
ready/receipt data chain. F5 shared native backing tests do not close F7 obligations.

`NativeBufferBoundary` is a cold structural value `{GraphResourceId, byte_offset}`.
It refines the existing compiler's imported Buffer cells for several paths sharing
backing; it is not a handle, GPU owner or independent hazard algorithm. Native code
copies it, and physical specialization preserves it. Failure is InvalidArgument for
non-imported/non-buffer/out-of-range boundaries; state capacity is checked by Runtime.
The profiling wrapper/query pool is test-only and absent from the production link map.
