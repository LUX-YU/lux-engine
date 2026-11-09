# Asset identity, data and prepared uses

`AssetRepository` uses the initialized process ExecutionRuntime and an AssetVfs view. Asset identity,
format codecs and providers stay in modules/resource/asset; no Render type is part of this module.

```cpp
auto asset = repository.acquire<lux::asset::MeshAsset>("/Assets/models/box.mesh");
if (!asset)
{
    co_return lux::cxx::unexpected(asset.error());
}
auto data = co_await repository.read(*asset);
if (!data)
{
    co_return lux::cxx::unexpected(data.error());
}
const lux::rdesc::Mesh& mesh = **data;
```

AssetRef counts use of an identity. AssetDataRef pins a particular immutable CPU snapshot or typed prepared result. Returning a
data borrow does not return the asset reference; the retention policy may reclaim unused CPU data while an
asset is still in use. `usage(id)` reports identity references, CPU data readers and prepared-use readers separately.
A missing usage record is not an asset-directory lookup; use VFS to enumerate assets.

The repository owns polymorphic codecs supplied by the resource module. Optional application codecs are
passed at construction and their defining code must remain loaded through repository destruction. No per-
consumer registration is required. Simultaneous reads share one decode. Cancelling one reader does not
cancel another reader's shared operation. IO uses the blocking scheduler, codecs use the CPU scheduler,
and adoption returns to Main. Acquire, observation and policy operations are Main-thread APIs.

DirectoryAssetProvider indexes cooked `.luxasset` headers without loading their full data. `write` encodes
borrowed data and replaces an existing record through the selected provider. Read-only providers return
UNSUPPORTED; uncertain publication stays an error and invalidates the cache. Old data references survive
replacement. Raw source import and automatic GPU replacement are not implemented by this module.

`watch(watcher, period)` registers one timer-driven task in the existing repository task group. FileWatcher
keeps its synchronous drain API. Directory refresh runs on blocking IO, then invalidates affected cached
records on Main. The watcher must outlive that task. No polling thread or additional scope is created.

The application first stops and retires consumers, returning AssetRef and AssetDataRef values. It then
destroys the repository at an outer Main-thread boundary. Its destructor stops its own TaskScope, waits for
accepted work and directly awaited operations, then releases cached uses in dependency order. It must not
run inside an Execution callback. External references must already have been returned; unresolved references
are a contract failure, not an unbounded wait or forced reference-count reset.
Runtime dispatch, FileWatcher and VFS/providers remain alive until this barrier completes. Shared reads already
accepted finish their IO/decode without cancellation; stopping one waiter never shortens another waiter's borrow.
EngineContext does not repeat the repository's shutdown coroutine. Ordinary consumers have no public
request/settle/retire protocol to sequence.

## Prepared uses

`registerUse<Definition>(provider, dependencies...)` binds a cold preparation operation. Registration performs
no IO or upload. Mesh/Texture definitions use direct upload; only definitions with actual composition
implement a single static `prepare` function. `use<Definition>(asset, provider)` returns `AssetDataRef<Definition::Output>`; it does not
create another Asset identity. Definition owns the domain-specific composition, including any dependent
uses. The repository owns the single shared preparation root and immutable result for a source revision.

Providers and captured dependencies have stable addresses and outlive repository retirement. Consumers may
cancel independently. A submitted release cannot be revived: a new use waits for its terminal result first.
A failed release preserves its result and dependencies and is reported to subsequent use/retirement.
`forEachUse` visits purpose type, provider address, reader count, preparation state and residency on Main.
Counts describe borrows and dependencies, not entities.

The default policy schedules release after the last reader. A custom AssetRetentionPolicy selects unused
results by purpose and provider; `collect()` reevaluates only candidate groups, not the whole asset directory.
Closing ignores retention and releases derived results before waiting for their dependent references.
Reference destructors only decrement counters and notify the existing CompletionWork. They never issue IPC.
`AssetDataRef<void>` retains an internal dependency without unchecked casts; `originalAsset()` only returns
a source Asset for actual CPU snapshots.


## Reference header boundary

AssetRef and AssetDataRef contain only a pointer to a stable repository record. Their headers use opaque
retain/release and data-access functions; they do not expose Task, split, RunLoop or completion records.
The concrete records and shared operation state live in AssetRepository.cpp. The asynchronous repository
API still intentionally exposes Sender/Task types. No additional identity table or reference allocation
is introduced. Last-reference publication keeps the existing atomic ordering and Main notification.

The registerUse template binds typed provider operations through configureUse/useDependencies/adoptUseData;
it does not write preparing/releasing or the shared operation. Failed release retains the same result and
dependencies. The narrow function-call boundary is not a claim of zero overhead or measured speedup.
