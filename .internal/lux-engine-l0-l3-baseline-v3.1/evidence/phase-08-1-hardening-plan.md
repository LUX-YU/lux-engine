# Phase 8.1 P0 Hardening Contract

- `loadWorldPartition(source, partition, max_bytes, stop)` is the only load seam; no compatibility overload.
- `max_bytes` constrains range reads, stored/decoded chunks, table pages and assembled partition data.
- Every range submit accounts the checked request size through `SubmitOptions.accounted_bytes`.
- `WorldPartitionData` and object views retain BundleId + Generation; materializers reject mismatches before mutation.
- Root format version, chunk count and file size must match every opened sidecar header.
- Deferred IO, decode cancellation, Scene/requester destruction, multi-volume and multi-extent paths are P0 tests.
- No `WorldLoadLimits`, cache manager, Scene scope, residency or streaming policy type is authorized.
