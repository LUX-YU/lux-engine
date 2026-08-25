# vNext L1 World Data Path — Re-audit Candidate

## Current conclusion

The World/Journal/Schedule/Hierarchy correctness work remains accepted, as do
the generator, TypeInfo and exact binary serialization foundations. LXWC v1
is retired. Immutable LXWC v2 load/unload and storage-driven Snapshot now pass
their correctness, installation and 1M performance contracts.

Current status:

- `Foundation Qualified`
- `L1 Architecture Accepted`
- `Correctness Hardened`
- `Performance Contract Passed`
- `Public API Freeze Candidate`
- `Independent Re-audit Required`
- `Domain Migration Blocked`

The former v1 Writer's quadratic path was not optimized or frozen; the entire
Writer/image contract was deleted. A separate 1M deep Transform qualification
found and removed an O(N^2) dirty-root reduction. The corrected path completes
naturally and is covered by the recorded qualification matrix.

## Qualification evidence

The following gates pass:

- immutable structurally validated LXWC v2 image;
- bulk range load/unload into an existing World with transactional rollback;
- one erased call and one typed storage lookup per loaded column;
- `O(N + M + R + B)` runtime load with natural 1M completion;
- storage-driven `O(N + M_copy)` Snapshot with zero hot-path `has()` calls;
- fresh install, independent consumers and retired-surface gates;
- Windows Debug/RelWithDebInfo/Hardened, Android PLAYER and second no-work
  builds;
- raw CSV, allocation/high-water statistics and raw EnTT relative baselines.

Raw CSV, medians, p95, allocation counts and structural counters are in
`.internal/benchmarks/20260825-l1-world-data-path`. Previous v2.1 CSVs remain
historical regression inputs only.

The maximum state remains:

```text
Foundation Qualified
L1 Architecture Accepted
Correctness Hardened
Performance Contract Passed
Public API Freeze Candidate
Independent Re-audit Required
Domain Migration Blocked
```
