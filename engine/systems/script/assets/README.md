# Script asset access

ScriptAssetAccess adapts engine assets to the existing script completion and result-handle protocol.
It borrows an AssetRepository for typed loads and a captured AssetVfsView for raw byte reads.
The repository owner outlives accepted typed operations and all returned script results.

Typed reads use AssetRepository::load<T> and retain AssetRef<Asset> in the existing result slot.
withAsset<T> takes a temporary reference before invoking the callback, so reentrant script release
cannot invalidate that borrow. The callback must not retain the reference. There is no separate
HeldAsset wrapper or copied shared_ptr ownership of decoded assets.

Raw reads call VFS.open on ExecutionRuntime's blocking IO scheduler. They retain the source view and
their byte result through completion. They do not pass through an OperationPort or run blocking IO
on the CPU computation pool.

ScriptAssetScope still provides the script-specific slot identity, explicit result release,
per-instance admission limits and completion backpressure. Those are script ABI responsibilities,
not a second asset cache. Revoke invalidates script handles and cancels that instance's wait;
it does not cancel a repository load needed by another consumer.

The script's retained-result reservation must cover the repository's decode limits for typed requests.
Raw requests retain their own byte bound. Queries never perform IO or decoding.

AssetRepository must be closed only after script consumers settle and release results. A script
operation's code pin covers that operation; it does not authorize unloading a codec while the
repository still has requests or references using it. Dynamic codec unloading is outside this contract.
