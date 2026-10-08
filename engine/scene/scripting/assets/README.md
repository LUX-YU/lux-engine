# Script asset capability storage

`scene_script_assets` is an engine-only native provider. It borrows a captured `AssetReadPort` and
the application's `ExecutionRuntime`. It has no Editor, Lua, renderer, project directory or compiler
dependency. Its one compiled `ScopeIdSource` is the reason for a shared library boundary: loading a
second consumer DLL must not create an independently numbered result domain.

`ScriptAssetAccess` owns one existing `TaskScope` and bounded weak instance records. A
`ScriptAssetScope` owns a bounded `SlotMap` of requests/results for one prepared native script
instance. Preparation and revocation attach to the original `ScriptSystem` mount lifecycle through
`ScriptApiInstanceBinding`. The binding is noncopyable; moving transfers its sole revocation duty.
Mount retirement revokes first, destroys the backend, destroys bindings, and finally releases code
associated with the script artifact. Preparation failures use that same cleanup path.

All scope and access calls run on their construction thread. Workers only own captured byte inputs,
codec inputs and task results. Raw reading uses `portSender`; typed reading uses the existing
`loadAsset<T>`/`TAssetSerDeser<T>` pipeline. Neither path enters a VM or modifies a Registry.

## Result and completion lifetime

A result token contains a native scope identity and a generation-checked slot. It is an alias,
not a reference count. Explicit release invalidates every copy. Peer instances, recycled slots and
revoked scopes reject old tokens. Scope destruction does not wait for IO and does not stop peer
scopes. Provider destruction closes admission, requests cancellation and revokes its scopes without
joining or collecting completions. Accepted tasks own the captured inputs, completion state and code
needed to finish through the original ExecutionRuntime. The runtime outlives this accepted work;
its explicit shutdown remains the synchronization boundary.

Completion, wake and result-cleanup callbacks may remove the provider or scope. Synchronous calls
retain only the private storage needed until that invocation returns. Revocation is immediate;
these temporary references do not admit new work or keep the public provider alive. An externally
held revoked scope retains neither result budget nor the provider's read endpoint.

The native completion records a small `ScriptAssetReadOutcome` through the original ScriptAbility
completion. Expected storage/codec failures are values, not script faults. The original structured
failure remains available as the scope's most recent diagnostic. A stale completion drops its
record. A backpressured completion keeps the original outcome, payload and completion in that
same bounded record. `deliverCompletions()` retries a fixed maintenance batch without resubmitting
IO. A completion accepted before coroutine resumption remains covered by scope revocation.

Typed payloads retain their code owner until after the final asset deleter. `withAsset<T>` takes
a temporary owning read reference before invoking a nonthrowing callback, so reentrant release
cannot destroy its current value or code. The callback cannot retain the borrowed reference.
The shared control block for that payload is allocated by the native library, not by the caller's
template instantiation in a plugin. Its final code release can therefore unload a plugin only after
the plugin value deleter has returned; no plugin control-block function remains on the return path.

## Bounds

Admission reserves one result slot and the configured maximum image bytes; typed admission also
reserves the configured maximum decoded bytes. This conservative logical charge lasts through
backpressure until explicit release, failure delivery or revocation, even for shared storage.
Per-request image bounds travel through the original read port to VFS/Pak before payload allocation.
Codec bounds remain the original codec contract. Provider index caches, task infrastructure and
temporary codec allocations are separate from retained-result accounting; this is not an RSS cap.

`describeAsset` and `copyAssetBytes` never perform IO. Raw images have no inferred asset type.
Typed results have a known type, but no promised raw-image size (`has_image == false`). Byte chunks
are bounded to 256 bytes with checked range arithmetic. Typed access never reinterprets a raw image.

## Composition and supported script access

`AssetAbility` exposes asynchronous raw image reads, bounded byte queries and explicit release.
`SkeletonAbility` is an independent example of typed CPU decoding and bone queries; it uses the
same instance scope. Neither capability compiles assets, traverses project directories, opens
native paths or controls a renderer. Importing a native capability does not load Lua or Editor.

The optional `scene_script_assets_lua` target registers the two generated projections and bounded
opaque value codecs. Full IDs never pass through floating-point Lua numbers. The original
ScriptAbility invocation, awaitable, LuaBoundary and Delay remain responsible for suspension and
resumption. Userdata transports trivial values only, never an asset, owner, callback or component
address. Expected read/decode failures are inspectable outcomes and do not fault a coroutine.

`ScriptRuntimeHost` explicitly borrows backend/component descriptors and artifact/world resolvers,
and owns a captured read port. Those borrowed objects and their code must outlive every installed
instance. An empty port grants no asset capability. Independent typed capabilities are provided by
factory functions, without changing a host asset enum or the generic VM. `ScriptRuntimeSystem`
constructs the native provider, receives completions in maintenance while paused, and runs script
rules only through the original Simulation graph. Shutdown retires bindings before joining tasks.

Editor Run can retain this same host through `RunEnvironment::scripts`; the shared host does not
extend the lifetime of its borrowed spans or resolver contexts. The composition owner supplies
those lifetimes. Run reads its frozen source and the engine capability; it never borrows the live
author model or an Editor project service. No automatic Editor-specific script discovery is added.

`cmake/installed-consumers/editor-ec2` qualifies native reads without Lua, generated Lua execution,
and SDK-only generation/packaging of the same asset script into an actual Scene. Tests separately
cover native capacities, transport errors, foreign IDs, stop timing, and a dynamically loaded
nontrivial codec/deleter. These tests are not GPU or desktop-input qualification.
