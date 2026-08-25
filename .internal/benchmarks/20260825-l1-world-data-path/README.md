# L1 World data-path qualification — 2026-08-25

This directory contains the raw and summary CSV emitted by the reworked
`ecs_l1_benchmark`. Every group used 5 warmups and 30 samples at a requested
size of 1,000,000. The executable published the complete CSV through a
temporary file after every completed metric, so a later failure could not
erase earlier evidence.

## Result

- 1M World read query median: 3.849 ms versus raw EnTT 4.217 ms; both paths
  allocate zero times in the measured loop.
- 3 dense fixed-32B WorldSection columns: 20.253 ms at 100K and 217.293 ms at
  1M, a 10.73x increase and below the 15x scaling gate.
- 1M predecoded fixed World range create/insert: 36.502 ms versus raw EnTT
  33.097 ms, 10.29% overhead and below the 25% gate.
- Loaded-column erased dispatch and typed-storage lookup counts are exactly
  the non-empty column counts for the 3, 16 and 64 column cases.
- 1M Snapshot capture median: 18.873 ms, with exactly one COPY-storage thunk
  and one typed-storage lookup. No entity-level `has()` path exists.
- 1M balanced Hierarchy initial synchronization visits 999,999 relations;
  the next unchanged frame visits zero. Star reparent visits one node.
- 1M Transform root/deep propagation visits exactly 1,000,000 nodes with zero
  measured allocations. The sparse high-water leaf case visits one node and
  records 32,400,024 retained dense-cache bytes.

## Correctness issue found by qualification

The first 1M deep Transform diagnostic exposed a real quadratic path rather
than a deadlock. Initial resync marked every node, while `collectRoots()`
walked the marked parent chain for every candidate, producing
`1 + 2 + ... + N` work. The run was stopped after the already-completed root
metric had been durably published. Root selection now admits a candidate only
when its immediate parent is not marked, which is equivalent and linear.
After the fix, 1M deep propagation completed with a 107.439 ms median and
118.450 ms p95.

## Files

- `query-1m-qualification.csv`
- `hierarchy-1m-qualification.csv`
- `transform-1m-qualification.csv`
- `snapshot-1m-qualification.csv`
- `world-section-1m-qualification.csv`

These results qualify the storage-driven L1 paths on the recorded Windows
RelWithDebInfo environment. They do not mark L1 Frozen; independent re-audit
is still required.
