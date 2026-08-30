# ADR: CPU Typed Asset Lifecycle Gate

- Status: Needs more evidence
- Date: 2026-08-30
- Input: `process_asset` typed load Sender

## Evidence

`process_asset` now proves a narrow typed workflow:

```text
AssetId -> OperationPort<ReadAssetImage> -> owned AssetBlob
        -> TAssetSerDeser<ConcreteAsset> -> shared_ptr<const ConcreteAsset>
```

Texture, Material and Model cover synchronous completion, deferred completion, admission rejection, storage failure,
identity/type/limit validation, cancellation and retained `SharedBytes` ownership. The target contains no Provider
implementation, cache, runtime codec catalog or Asset ownership service.

## Decision

No CPU Asset lifecycle API is approved. A typed Sender does not establish the owner or semantics of in-flight
deduplication, invalidation generation, retry/backoff, strong/weak retention, cross-Scene sharing, memory budget or the
relationship between CPU payload and renderer-owned GPU objects.

The existing Asset Residency Barrier A remains in force. A future proposal must provide a real Product workflow with
failure/retry, cancellation, generation replacement and an independent second domain before requesting a public type
budget. No Manager, Store, Lease, Ref, Context, Services or cache type is created by this review.
