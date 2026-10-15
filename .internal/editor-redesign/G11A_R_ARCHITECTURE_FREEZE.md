# G11A-R R0 — source audit and convergence decisions

Status: construction freeze; not implementation or verification qualification.
Input: `d5b4ff0ad792dd8e2d9ada7c26bda3c86fc33959`.
Isolated source: `E:/RenderV2Rebuild/G11AR/source`; protected user source is a different worktree.
The v9 document SHA256 is `b5f4dbc6cf1787b15ed20088b50e64dd01b7d2aee0e5812aa5b4f707ca316cec`;
the implementation contract SHA256 is `becf8cad2f862f9411a726220c23b12b2866558b5c5920ff2a10b56a3457babe`.
Fresh protection/dependency snapshot: `E:/RenderV2Rebuild/G11AR/evidence/preflight.json`.

## Source findings and replacement obligations

| Current source/state | Concrete issue | Required final location / deletion |
|---|---|---|
| `features/Resources.hpp`: TypeId, ABI, Scope | Correct neutral identity, but no creation/capability contract | Move the existing declarations into one neutral resource component; features consumes it, no aliases |
| `runtime/TextureFeature.cpp` and zero-pass definition | Storage commands installed as a fabricated Feature | Storage registration contributes the same existing Operation descriptors/handlers to the one Operation SlotMap; delete Texture Feature registration |
| `DeviceSession::textures_` / `scoped_` | Separate instance owners and unrelated scope resolution | One ResourceRegistry owns native instances, keyed by domain/type/semantic key; scoped buffers become a normal typed native storage |
| `ExecutionState.hpp::ScopedBuffer` | Scope encoded with Scene/View and a Context address | Registry domain keys express Root/Scene/View/Attachment; prepared Context borrows pin the actual instance |
| `TextureVersion` | Public physical backing mistaken for a second resource model | Private Texture backing record; business SlotKey and revision remain unchanged |
| `TextureUpload` | Texture layout and publication mixed with shared staging/submit state machine | Texture builds native copy descriptions; one native Transfer work owns chunk/prepare/ticket progression for Buffer and Image |
| `GpuCompletion::upload`, `texture_ready`, `commitUpload` | Runtime explicitly names Texture work | One erased bounded work contract in the existing DeferredCompletion slots; no second request owner |
| `PassProgram::texture`, `ResourcePlan::texture` | Asset ID stored in immutable compiled code | Neutral binding contract plus separate actual arguments resolved at Attach; Graph code contains no asset values |
| Compiler name/ABI2 checks; executor `textures_` | New Buffer consumer would require business branches in both | Reflected native layout and cold storage binding resolver; prepared descriptor/buffer/push data in Context |
| `VulkanDevice::queue_`, `queue_family_`, `queues_[0]` | Same physical queue state twice | Keep role array only |
| `graph/src/Range.hpp` | Shared private algorithm in source directory | `graph/pinclude/lux/engine/render/graph/detail/Range.hpp`, PRIVATE include path |

The source audit confirms the following algorithms must survive relocation: Texture row/block validation,
region clone-before-patch, one-page CPU preparation, prior-ticket preservation on failed submit, queue-family
release/acquire with timeline wait, canonical color conversion, S0 move-only allocation permits, and R5
pipeline/bind/rendering-scope lowering. `compileGraph` consumes the existing analysis, never reruns it.

## Ownership and publication

```text
RenderServer::Impl
  owns frozen registration metadata and code pins, sole Operation SlotMap, original Deferred slots
  owns InstanceContext, then DeviceSession (destroyed first)
    DeviceSession owns Device / VMA allocation ledger / physical Queue owners / Retirement
    DeviceSession owns one ResourceRegistry
      domain SlotMap: identity + parent + scope + closing state; no GPU objects
      instance entries: domain + full type/ABI + semantic key + config + one native owner
  Scene owns View; their domain identities follow their actual lifecycle
  Effect owns immutable CompiledGraph and four-field ExecutionGroups
    Context owns transient allocations and prepared binding borrows, including Attachment domain
```

Domain keys are owner-local SlotKeys. Public/untrusted access must carry the existing Session proof or a
checked registry-issued domain qualification; generation is never duplicated or used as GPU completion.
Root does not belong to a Client. `findVisible` is ancestor read-only lookup, not write authorization.
Local `getOrCreate` compares complete type, ABI, scope and creation config before reuse. Factory failure
does not publish an entry. Attach retains newly created entries as candidates until all resources and
bindings succeed; failure releases candidates and preserves the previous Context/Group.

Closing a domain first prevents new uses. Contexts and accepted work release their borrows before the
instance can be disposed. Prepared references cover future submissions; actual queue tickets cover past
submissions. Neither replaces the other. Transient images/buffers stay outside persistent Registry.

## Neutral interface and factory boundary

Use one `render_resource` component, consumed by Feature, Native and Runtime. Core/Graph do not acquire
Protocol or Vulkan dependencies. Move existing Storage Type/ABI/Scope declarations; do not duplicate them.
Its descriptor describes full stable identity, ABI, allowed scopes, config contract and required/optional
capabilities. Config and Ops remain professional typed contracts. Native factories are installed explicitly
in the same cold registration entry, selected by API; the built-in Texture factory installs automatically.
The directory contains metadata only. Registry entries alone own actual native Storage instances.

`getOrCreate<T>` returns `T::Access`, pairing a checked native pointer with the matching `T::Ops`.
There is no cast from neutral `T` to a native implementation and no virtual `IStorage` hierarchy.
An external extension has this shape (the tests must compile and execute the actual final API):

```cpp
struct TestBufferStorage {
    static const StorageDescriptor description;
    struct Config;
    struct Ops;
    class Access; // borrowed native pointer + exactly matching Ops
};
// One explicit install contributes its factory, then the existing Server registration path:
// server.registerStorage<TestBufferStorage>();
// server.registerFeature<TestBufferFeature>();
```

Factory/config/schema/handler code pins outlive callbacks, instances, compiled bindings and queued work.
Freeze precedes device creation. Hardware capability negotiation is separate from Host executor presence.
Optional unavailable storage is not advertised as executable; required unavailable capabilities fail at the
defined cold boundary. No registration creates physical Texture backing. A second API is a contract-only
test factory, not a second production backend.

## Transfer, completion and retirement

Keep one fixed staging-page pool and one chunk progression algorithm. A professional storage supplies
validated Buffer/Image copy regions and final visibility; it does not own another submission loop. CPU
preparation uses the host's bounded executor and touches its assigned slice only. Owner-thread advance
performs bounded flush/record/submit/poll. Fixed Deferred/Reply admission precedes all accepted GPU work.

The original ReceivedRequest remains the source owner in one Deferred slot. Generic work owns native
candidates and source borrows until safe completion; publication callback commits the professional record.
Failures retain any preceding accepted ticket. Stop closes admission, joins callbacks, drains actual GPU
work, disposes native work, settles requests, resets Responder, then destroys Session and Instance.
Requester close/abandon does not cancel GPU work or free its input early.

Retirement accepts composite cleanup with queue-aware completion requirements. Unrelated queue serials
are never compared. Already established submission dependencies can prove coverage; otherwise all actual
queue conditions remain. Collection uses cached queue frontiers and bounded scans, not repeated per-item
polling or restarting a dense scan after every erase. Descriptor slots stay occupied until future references
and past GPU uses are both finished. A minimal indirect-reference fixture proves this without introducing
production Material or Instance storage.

## Shader, GAPI and visibility

Cooked Shader/Pass Program values move to neutral headers with no forwarding shim. Existing SPIRV-Tools
and SPIRV-Cross remain the only validation/reflection path. ResourcePlan records shape/type/ABI/scope and
binding requirements, while Attach supplies asset values. The native resolver prepares direct buffer/image,
descriptor and push-constant data; execute does no name/schema/type lookup or feature dispatch.
Keep linear BT.2020/D65, SRGB hardware EOTF once, legacy LINEAR=linear709, DATA unchanged, alpha unchanged.

Current gapi Image/View/Sampler/Descriptor constructors invoke `VK_FUNC_INVOKE`: on errors that macro prints
and asserts (except DeviceLost); it is not a substitute for RenderResult. Reuse only narrow VkResult-returning
primitives, adding a narrowly consumed primitive where needed. Do not replace VMA ownership or the existing
Native fault hooks. gapi's lightweight handle does not own a device and requires explicit release.

Toolset `c104eb6...` implements BUILD_TIME_EXPORT and BUILD_TIME_SHARED as build PUBLIC; only the former
is exported for install. PRIVATE is actual target-private. Range is PRIVATE, not SHARED. Installed consumers
must not receive source-only native include paths to pass neutral header checks.

## Complexity and verification budget

One new neutral component; one Registry/domain table; one factory metadata record; one generic transfer
work; one erased deferred work contract; prepared native binding data. These replace the owners and special
branches listed above. No new thread, Manager, Registry hierarchy, public resource version/lease tree, graph,
operation table or pending-request list. Final report must count actual added/deleted types and states.

Baseline test-name manifests (72 each, not just a count): `evidence/baseline-{release,debug,asan}-tests.json`.
Baseline performance summary copied from exact G11A verification to `evidence/baseline-performance-summary.json`.
Historical actual GPU topology is alias 0:0, same-family 0:2, dedicated transfer 1:0; re-probe during verification.
New tests must cover F-01 through F-16, especially external installed TestBuffer through the real Graph path,
failed Attach rollback, domain/session/generation, factory ABI/config/capability, indirect references, and
cross-queue retirement. Keep all original 72 names and their behavioral assertions, adapting obsolete APIs.

Measure 1/16/64 MiB uploads, exact source/staging/resident/descriptor bytes, three queue forms, complete
request encode/handler/GPU-ready/reply consumption, four-view cadence and Engine new=0. Keep raw tails and
failed attempts. GPU timestamp intervals and CPU wait/sleep intervals are distinct. G11A's old
`completion_consume_decode_ns` column measured decode only and cannot be mislabeled as full consumption.

Final qualification requires exact final I, independent no-alternates clone, Release/Debug/ASan all twice,
all CTests, actual GPU Validation/SyncValidation, header/install/relocation consumers, 719 blob protection,
three SDK prefix backup/sync, and safe fast-forward I/V push. Root TOOLCHAIN BLOCKED and IPC historical
PARTIAL are separate facts. G11B–G13 are not started. This freeze itself proves none of those final gates.
