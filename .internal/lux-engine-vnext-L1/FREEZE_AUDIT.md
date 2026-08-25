# vNext L1 World Data Path — Re-audit Candidate

## Current conclusion

The World/Journal/Schedule/Hierarchy correctness work remains accepted, as do
the generator, TypeInfo and exact binary serialization foundations. LXWC v1
is retired. The second-hardening implementation now qualifies lexical LXWC v2
residency transactions, exact change publication, linear membership-ledger
unload, explicit runtime budgets and storage-driven Snapshot.

Current status:

- `Foundation Qualified`
- `L1 Architecture Accepted`
- `Correctness Hardened`
- `Performance Contract Passed`
- `Public API Freeze Candidate`
- `Independent Re-audit Required`
- `Domain Migration Blocked`

The former v1 Writer's quadratic path was not optimized or frozen; the entire
Writer/image contract was deleted. Batch unload is not implemented as an
Entity-by-all-World-storages scan. Normal load/unload publishes the exact delta
without history loss; allocation failure alone falls back to one pin-safe
history-loss epoch.

## Qualification evidence

The following gates pass:

- immutable structurally validated LXWC v2 image;
- lexical multi-section load/unload with all-or-nothing canonical staging;
- `INACTIVE/STAGED/ACTIVE` instance lifetime and wrong-World rejection;
- exact remove/destroy/create/add observation order and linear unload;
- one erased call and one typed storage lookup per loaded column;
- caller-provided validation/decode budgets and adaptive record-count Schedule
  scratch without Foundation capacity defaults;
- `O(N + M + R + B)` runtime load with natural 1M completion;
- storage-driven `O(N + M_copy)` Snapshot with zero hot-path `has()` calls;
- fresh configure/build/install, clean install-surface gate and three independent
  consumers (`core+schedule`, Object affinity, `world_section`);
- Windows Debug/RelWithDebInfo/Hardened, Android PLAYER and second no-work
  builds;
- raw CSV, allocation/high-water statistics, scaling exponent, structural
  counters and raw EnTT relative baselines.

Raw CSV, medians, p95, allocation counts and structural counters are in
`.internal/benchmarks/20260825-l1-world-data-path-v2`. Acceptance policy is in
`.internal/qualification/l1-world-data-path.toml`; the external evaluator reports
`PASS: 2 relative, 1 scaling, 4 structural rules`. Previous CSVs remain
historical regression inputs only.

Validation matrix:

- Windows RelWithDebInfo: `79/79` CTest;
- Windows Debug: `66/66` CTest;
- Windows Hardened Contracts: `79/79` CTest;
- Android arm64 PLAYER: full compile;
- all four configured trees: second Ninja invocation reports no work.

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
