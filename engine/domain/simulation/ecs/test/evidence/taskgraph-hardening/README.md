# L0/L1 TaskGraph hardening evidence

This directory contains durable Windows RelWithDebInfo qualification evidence
for the completion-driven L0 TaskGraph and uniquely-owned L1 System execution
contracts. It is evidence for an independent re-audit candidate, not evidence
of L2 worker-backend or parallel execution performance.

## Run contract

- Engine baseline: `641e3aaf`, followed by the scoped TaskGraph hardening
  commits on `codex/object-ui-foundation`.
- Compiler: MSVC 19.44 (Visual Studio 2022 17.14.35).
- Benchmark mode: 5 warmups and 30 retained samples per metric.
- Graph/System sizes: 1, 16, 64, 256, 1024, and 4096.
- Write sizes: 100,000 and 1,000,000 entities with 1, 4, and 16 write
  columns.
- Thresholds are defined in `../../l0_l1_taskgraph_qualification.toml`, not in
  Foundation C++.

The evaluator command was:

```text
py -3 engine/simulation/ecs/test/evaluate_l1_qualification.py
  --policy engine/simulation/ecs/test/l0_l1_taskgraph_qualification.toml
  --input "engine/simulation/ecs/test/evidence/taskgraph-hardening/relwithdebinfo/*.csv"
```

Result:

```text
PASS: 1 relative, 10 scaling, 14 structural rules
```

## Selected median observations

| Metric | Size | Median | Allocations | Lane binds | Journal binds | Appends | Per-record lookups |
|---|---:|---:|---:|---:|---:|---:|---:|
| `task_graph_execute_none` | 1,024 | 41,700 ns | 0 | 0 | 0 | 0 | 0 |
| `task_graph_execute_chain` | 1,024 | 41,200 ns | 0 | 0 | 0 | 0 | 0 |
| `task_graph_execute_diamond` | 1,024 | 43,300 ns | 0 | 0 | 0 | 0 | 0 |
| `system_execute_mixed_affinity` | 1,024 | 55,300 ns | 0 | 0 | 0 | 0 | 0 |
| `world_change_log_write_16` | 1,000,000 | 146,471,600 ns | 0 | 16 | 16 | 16,000,000 | 0 |
| `system_write_query_16` | 1,000,000 | 211,289,200 ns | 0 | 16 | 16 | 16,000,000 | 0 |

The raw CSV files are the source of truth. Timings describe this machine and
configuration only; structural counters and externally evaluated scaling and
relative rules are the portable qualification claims.

## Status boundary

```text
L0 TaskGraph DAG/Completion Contract    Re-audit Candidate
L1 System Ownership/Compiler Contract   Re-audit Candidate
L1 Reference Execution Correctness      Qualified
L2 Worker/Owner Backend                  Not Implemented
Parallel Execution Performance          Not Qualified
WorldSection Residency                   Re-audit Required
Public API Freeze                        Blocked
Domain Migration                         Blocked
Independent Re-audit                     Required
```
