# Canonical L1 World + Simulation qualification

This record qualifies production commit
`4475a7493d674144c362c3fb7b53fc4f10a8ed99`. The evidence commit that adds
this file contains no production-code changes relative to that commit.

## Build and contract matrix

- Windows RelWithDebInfo: full `all -j 4 -k 0`, second build no-work, CTest
  81/81.
- Windows Debug: full `all -j 4 -k 0`, second build no-work, CTest 86/86.
- Windows RelWithDebInfo with Object/UI hardened contracts: full
  `all -j 4 -k 0`, second build no-work, CTest 86/86.
- Android arm64-v8a PLAYER, Android API 33, NDK 30.0.15729638 / Clang 21:
  full 386/386 build, second build no-work, fresh install.
- Fresh Windows RelWithDebInfo install: installed-architecture validation and
  the ten independent core-task, world, asset, Simulation, System, ECS, and
  snapshot consumers passed; each consumer's second build was no-work.

The builds used a detached clean worktree at the tested commit. User-owned
working-tree changes under `.gitignore`, `.internal`, `$stage/`,
`CodecByteIO.hpp`, `TaskGraphBuilder.hpp`, and the three noted World files were
not present in the qualified source tree and were not modified or committed.

## Exact-SHA benchmark policy

The schema-v4 CSV rows identify the tested commit, MSVC 19.44.35228,
RelWithDebInfo, Windows, and x86-64. Evaluation used
`../../l0_l1_taskgraph_qualification.toml` and
`../../evaluate_l1_qualification.py` with `--expected-commit` set to the full
SHA above.

Evaluator result:

```text
PASS: 0 relative, 3 scaling, 5 structural rules
```

Selected medians:

| Metric | Size | Median ns | Allocations | Lane binds | Appends | Per-record lookups |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| task_graph_execute_none | 16 | 5,800 | 0 | 0 | 0 | 0 |
| task_graph_execute_none | 1,024 | 336,200 | 0 | 0 | 0 | 0 |
| world_change_batch_record_1 | 100,000 | 327,100 | 0 | 1 | 100,000 | 0 |
| world_change_batch_record_1 | 1,000,000 | 2,969,600 | 0 | 1 | 1,000,000 | 0 |
| world_change_batch_record_32 | 100,000 | 9,916,100 | 0 | 32 | 3,200,000 | 0 |
| world_change_batch_record_32 | 1,000,000 | 96,442,700 | 0 | 32 | 32,000,000 | 0 |

Raw qualification CSVs remain outside the repository. Their SHA-256 digests
are recorded so the exact inputs can be audited without turning the source
tree into an evidence store:

```text
8DFF3D1DEEE684B9FE2E19707A56FCC328A481B96752C5A3324F485529B4B455  canonical-l1-task-graph-16-qualification-4475a749.csv
B51CE6233DDDBDB481FC34A309A01ABF2D1F2F0344FE808E70D3D21195690687  canonical-l1-task-graph-1024-qualification-4475a749.csv
06904DA9304415C6C973916B90EED24741BE8523D9E51240307B7A44CA44CBB7  canonical-l1-change-batch-100k-qualification-4475a749.csv
8B1722BE965E79210E4B8868700191D7B31C5041D6ACA2B7868491980FC5DDFC  canonical-l1-change-batch-1m-qualification-4475a749.csv
```

The World diagnostic harness was also run at 10,000 objects, four data records
per object, and 16 partitions. Its external CSV digest is:

```text
14D72A4327D3D1973847F6C8CB3A9E8F88928E4BEAF54DDE27E82C1EAEFE9579  canonical-l1-world-diagnostic-4475a749.csv
```

This record is a re-audit candidate. It does not constitute independent public
API freeze acceptance and does not authorize L2 Process or L3 Scene work.
