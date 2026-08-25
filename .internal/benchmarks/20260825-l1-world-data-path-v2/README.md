# L1 World Data Path v2 Historical Evidence

Date: 2026-08-25

Build: Windows RelWithDebInfo, MSVC 19.44

Mode: 5 warmups + 30 recorded samples per metric

The CSV files in this directory are the durable raw and summary evidence. The
acceptance thresholds are intentionally outside the C++ implementation in
`.internal/qualification/l1-world-data-path.toml`.

Evaluation command:

```text
python engine/ecs/test/evaluate_l1_qualification.py \
  --policy .internal/qualification/l1-world-data-path.toml \
  --input .internal/benchmarks/20260825-l1-world-data-path-v2/*.csv
```

Historical evaluator result:

```text
PASS: 2 relative, 1 scaling, 4 structural rules
```

The second independent audit rejected this evidence as a freeze qualification.
It does not cover the per-row residency ledger's column-count complexity or
memory, deferred image ownership, Schedule multi-write lane lookup, or a truly
dense 1M Parent/Transform resident World. The measurements remain useful for
comparison, but `Correctness Hardened`, `Performance Contract Passed` and
`Public API Freeze Candidate` are withdrawn until the WDP3 matrix passes.

## Representative results

| Metric | Size | Median | p95 | Relevant evidence |
|---|---:|---:|---:|---|
| Raw EnTT read | 1M | 3.738 ms | 3.963 ms | 0 allocations |
| World ReadQuery | 1M | 3.632 ms | 4.384 ms | median ratio 0.972 |
| WorldEdit WriteQuery | 1M | 9.181 ms | 9.394 ms | 0 allocations |
| Schedule WriteQuery | 1M | 12.367 ms | 12.842 ms | 1M-record high water, 0 overflow |
| Hierarchy balanced initial sync | 1M | 23.645 ms | 25.789 ms | linear rebuild counters |
| Hierarchy real no-change | 1M | 0.0001 ms | 0.0001 ms | 0 visited nodes |
| Transform large-root dirty | 1M | 68.847 ms | 69.789 ms | 0 measured allocations |
| Transform deep propagation | 1M | 123.216 ms | 130.348 ms | 0 measured allocations |
| Snapshot capture | 1M | 16.507 ms | 17.283 ms | one storage dispatch/lookup |
| Snapshot instantiate | 1M | 16.790 ms | 17.533 ms | construction changes suppressed |
| Snapshot restore | 1M | 16.479 ms | 17.125 ms | destination policy retained |
| Raw EnTT create + fixed insert | 1M | 37.972 ms | 39.686 ms | relative baseline |
| World predecoded fixed insert | 1M | 37.181 ms | 39.422 ms | median ratio 0.979 |
| LXWC dense fixed-3 full load | 100K | 35.347 ms | 36.908 ms | 3 dispatches/lookups |
| LXWC dense fixed-3 full load | 1M | 551.551 ms | 565.164 ms | scaling exponent 1.193 |
| Live 10K section reconcile in 1M World | 1M resident | 12.818 ms | 13.622 ms | 29,999 visits, 0 history loss |

The sparse/high-churn Transform probe retained 32,400,024 bytes at a 1M
entity-slot high-water mark with 100K live entities. This is recorded for the
independent re-audit; v2.1 intentionally retains dense stamps pending evidence
that a sparse representation is superior.

## Scope of the performance statement

This evidence qualifies structural invariants, scaling, and relative baselines;
it does not freeze absolute millisecond thresholds. In particular:

- erased dispatch and storage lookup counts equal non-empty columns;
- normal live residency changes do not mark journal history loss;
- the 10K streamed delta reconciles without scanning the 1M resident World;
- the predecoded range-insert seam is compared with raw EnTT at the same size;
- the 100K-to-1M dense fixed workload is evaluated by a scaling exponent.

Product capacity and memory budgets remain caller-supplied policy. LXWC widths,
header sizes, and format versions remain representation constants.
