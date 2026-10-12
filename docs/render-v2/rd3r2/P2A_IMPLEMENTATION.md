# RD3-R2 P2-A — DeviceSession, ResourceDomain and Effect bindings

Base: `98002bb03bb8ae0c6ed156fcd7e847082b6a7148` (RD3-P1 V).
Scope: the user's 2026-10-12 P2-A authorization. No P2-B/P3/P4/P5 implementation or historical qualification rewrite.

## Ownership and actual lifetime

| Object | Sole owner | Borrows / release order |
|---|---|---|
| DeviceSession | Backend `unique_ptr`, fixed address | No worker; Backend stops, drains and joins its existing worker first |
| InstanceContext | DeviceSession | R4 VulkanInstance owns Instance/validation/debug; physical-device discovery returns borrowed native handles |
| DeviceContext | DeviceSession | R4 VulkanDevice, VMA and unique SubmissionQueue owners; aliased queue roles are pointers only; enabled features remain facts of VulkanDevice |
| ResourceContext | DeviceSession | Borrows DeviceContext allocator; owns sampler and Native execution pools, including their original descriptor/staging/backing RAII |
| Native compiled Graph | Session Graph SlotMap | RenderGraphId allocated here; immutable code/Shader/PSO retain P1 sharing and full equality |
| Native execution | ResourceContext sparse index keyed by GraphId | Each binding has independent invocation data, revision, FIF slots, descriptor state and submission receipts |
| ResourceDomain | Session `SlotMap<unique_ptr<ResourceDomain>>` | Root/Scene/View domains are stable across growth and dense compaction; parent lookup is read-only |
| Scene / View | Backend Scene SlotMap / Scene View SlotMap | ViewRef is the complete SceneId + local ViewId; DomainId is a different tag |
| Effect / Target | Backend SlotMaps | Neither owns View or the other object |
| EffectBinding | Backend `SlotKeySparseSet<RenderEffectId, vector<EffectBinding>>` | Exact Scene/View/Target triples; the only editable relationship authority |
| ExecutionJob | Backend derived ordered vector | Borrowed effect/binding/GraphId and execution-pool offset; updated only on attach/detach |

Backend no longer owns Instance, Device, allocator, queue owners, domains, graphs or Native execution pools directly.
`View.effect`, `View.graph`, `View.target`, `Target.view` and `GraphViewGroup` are removed.
Native command pools remain owned by the existing SubmissionQueue RAII. Moving or duplicating them into a
second global pool would break the queue's lifetime contract; ResourceContext consumes these queues through
the unchanged Native graph implementation.

Shutdown releases execution contexts after their actual tickets, drains remaining accepted queue work (including
direct Native transfer work), then destroys child domains before parents, sampler/resources, compiled Graphs,
queues, allocator, device and instance. `lastSubmitted()` only exposes an actual queue-issued ticket;
no queue/submission/Sync2/retirement algorithm is replaced. Domain removal waits accepted queue work and rejects
parents with children. Direct typed `erase<T>()` requires an externally serialized GPU-safe point;
`DeviceContext::drain()` supplies that cold point. Raw resource borrows must not outlive erase/Domain/Session.

## Public contracts

```cpp
auto created = client.createView(scene); // creates only Scene-owned View and its Domain
// Decode the reply to RuntimeView, then separately:
auto attached = client.attachEffect(effect, view, target);
RuntimeBinding binding{effect, view, target};
auto graph = client.graph(binding);
auto published = client.publishBindingPass(binding, pass, revision, scalars);
auto output = client.readbackTarget(binding, offset, bytes);
auto detached = client.detachEffect(effect, view, target);
```

All three endpoint owners must agree and match the receiving Runtime. Server lookup verifies every generation.
Duplicate attach is idempotent, with no additional Native backing or job. View/Target/Effect removal returns Busy
while any binding references it; Scene removal requires no remaining Views. A single View can simultaneously
serve several Effect/Target pairs. Multiple writers to one Target are explicitly rejected until P3 composition.
Runtime control and parameter protocol names are bumped to v2; there is no ambiguous legacy View-only graph API.

Scene work reuse selects only a real producer from this frame. A binding which borrowed Scene work cannot become
a second-level donor. Distinct bindings of the same View may share legitimate Scene work; their View output stays
independent. Native rejects illegal reuse exactly as before; no relaxation of its synchronization checks was made.

```cpp
auto& domain = session.getResourceDomain(domainId); // trusted ID only
auto factory = session.resources.bufferFactory(256, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
auto buffer = domain.getOrCreate<vulkan::Buffer>(factory);
```

One local resource per type is implemented. Lookup compares lux-cxx compiler-local type hash **and full name**;
neither is an object ID or sparse index, and neither is persisted or exposed as plugin ABI. The compact type-erased
entry owns a typed unique_ptr, so returned addresses remain stable. No unordered runtime identity table exists.
Factories return exactly `RenderResult<unique_ptr<T>>` and provide `isCompatible(const T&)` to validate configuration
without constructing on a hit. A mismatch returns `kDefinitionMismatch`. Native failure leaves the registry unchanged.
The eager unique_ptr overload adopts an empty type slot; an occupied slot is an explicit conflict, with unused
candidate RAII destruction. `tryGet<T>()` walks the read-only parent chain; creation/erase affect only the local domain.
Ordinary heap OOM remains fatal under the repository policy; recoverable Native OOM is an expected error.

## Verification boundary

Production tests extend the existing RD3 Runtime and fault executables; public ResourceDomain and DeviceSession
headers join the independent C++20 header compilation targets. Tests cover lazy factories for three types,
Native Buffer OOM/retry, config conflicts/adoption, parent shadowing, domain generations/address stability and
child-before-parent destruction after real `vkCmdFillBuffer` work. Existing P1 GPU oracle, stop/late replies,
multi-queue partial submit, owner counters and zero warm allocations remain exercised.

`checks/verify_p2a.py` checks an exact clean I in an independent clone, all 719 frozen Legacy blobs, unchanged
historical evidence, unchanged Native/Graph algorithm translation units, allowed change paths and checkout bytes.
Actual independent results belong to the separate report-only Verification V, not this implementation note.

Not implemented: full Effect-owned logical Graph/Feature factories and atomic hot updates (P2-B), Target
composition/WSI/resize/history (P3), specialized Legacy storage/import leases/async replacement (P4), plugin SDK,
CodePin and Engine/Editor integration (P5), or L/H algorithms. Old unrelated p95/4K footprint/product protocols
are NOT_RUN, and their previous results remain immutable. P2-B's minimal next step is moving the fixed logical
definition into an Effect-owned Graph/Feature composition and publishing its compiled candidate across bindings.
