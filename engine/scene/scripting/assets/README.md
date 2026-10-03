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
scopes. The provider's host-boundary destruction revokes all scopes and joins its own TaskScope;
it must not be destroyed from a callback running inside that same scope.

The native completion records a small `ScriptAssetReadOutcome` through the original ScriptAbility
completion. Expected storage/codec failures are values, not script faults. The original structured
failure remains available as the scope's most recent diagnostic. A stale completion drops its
record. A backpressured completion keeps the original outcome, payload and completion in that
same bounded record. `deliverCompletions()` retries a fixed maintenance batch without resubmitting
IO. A completion accepted before coroutine resumption remains covered by scope revocation.

Typed payloads retain their code owner until after the final asset deleter. `withAsset<T>` takes
a temporary owning read reference before invoking a nonthrowing callback, so reentrant release
cannot destroy its current value or code. The callback cannot retain the borrowed reference.

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

The native tests exercise actual Process, asset codec and C++ ScriptSystem backend lifetimes.
They do not establish Lua projection, PLAYER composition or GPU qualification.
