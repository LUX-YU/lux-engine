# vNext L1 World Data Path — Qualification Pending

## Current conclusion

The World/Journal/Schedule/Hierarchy correctness work remains accepted, as do
the generator, TypeInfo and exact binary serialization foundations. The LXWC
v1 persistence surface and its performance conclusion are withdrawn.

Current status:

- `L1 Architecture Accepted`
- `World Data Path Rework Required`
- `Performance Contract Rejected`
- `Public API Freeze Blocked`
- `Domain Migration Blocked`

The committed v1 `WorldSectionWriter::build()` performs a growing-vector
duplicate scan. At one million selected entities it requires approximately
500 billion comparisons per build, so the previous monolithic qualification
benchmark cannot complete reasonably. This is an algorithmic failure, not a
deadlock. The v1 Writer is not being optimized and frozen; it is retired by the
LXWC v2 cooked-runtime data-path split.

## Qualification required

Freeze-candidate status requires all of the following new evidence:

- immutable structurally validated LXWC v2 image;
- bulk range load/unload into an existing World with transactional rollback;
- one erased call and one typed storage lookup per loaded column;
- `O(N + M + R + B)` runtime load with natural 1M completion;
- storage-driven `O(N + M_copy)` Snapshot with zero hot-path `has()` calls;
- fresh install, independent consumers and retired-surface gates;
- Windows Debug/RelWithDebInfo/Hardened, Android PLAYER and second no-work
  builds;
- raw CSV, allocation/high-water statistics and raw EnTT relative baselines.

Previous v2.1 CSVs remain historical regression inputs only. They do not prove
the new WorldSection contract.

After all gates pass, the maximum state is:

```text
Foundation Qualified
L1 Architecture Accepted
Correctness Hardened
Performance Contract Passed
Public API Freeze Candidate
Independent Re-audit Required
Domain Migration Blocked
```
