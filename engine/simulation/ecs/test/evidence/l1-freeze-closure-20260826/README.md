# L1 freeze closure qualification

This record qualifies exact production and test commit
`d6754945c9dbb8d581f6b46fd46e93e69be58247`. The evidence commit that adds
this file contains no production-code changes relative to that commit.

## Build and contract matrix

- Windows RelWithDebInfo: full `all -j 4 -k 0`, CTest 82/82, and second
  build no-work.
- Windows Debug: full `all -j 4 -k 0`, CTest 69/69, second build no-work,
  and install-prefix synchronization.
- Windows RelWithDebInfo with hardened ECS contracts: full
  `all -j 4 -k 0`, CTest 82/82, and second build no-work.
- Android arm64-v8a PLAYER, Android API 33, RelWithDebInfo: full build,
  second build no-work, and install-prefix synchronization.
- Fresh Windows RelWithDebInfo tree: full 415-target build, CTest 82/82,
  second build no-work, clean install, and installed-architecture validation.
- The ten repository installed consumers for Task, World, World asset,
  Simulation description/asset/core/system/ECS, and snapshot configured and
  built against the fresh prefix; every second build was no-work.
- Three additional repository-external consumers for serialization, resource
  asset, and Hierarchy/Transform configured, built, ran successfully, and had
  no work on their second builds.

The qualified source tree excluded user-owned working-tree changes under
`.gitignore`, `.internal`, `$stage/`, `CodecByteIO.hpp`,
`TaskGraphBuilder.hpp`, `EcsCommands.hpp`, and the three noted World files.
Those changes were not staged or included in the qualification commits.

## Exact-SHA benchmark policy

The schema-v4 CSV rows identify the full tested commit, MSVC
19.44.35228.0, RelWithDebInfo, Windows, and x86-64. Evaluation used
`../../l0_l1_taskgraph_qualification.toml` and
`../../evaluate_l1_qualification.py` with `--expected-commit` set to the
exact SHA above.

Evaluator result:

```text
PASS: 0 relative, 8 scaling, 9 structural rules
```

Selected medians:

| Metric | Size | Median ns | Allocations | Structural observation |
| --- | ---: | ---: | ---: | --- |
| `task_graph_execute_none` | 16 | 77,000 | 0 | prepared execution |
| `task_graph_execute_none` | 1,024 | 340,000 | 0 | prepared execution |
| `simulation_step_execute` | 1 | 128,500 | 0 | command safe point included |
| `command_batch_record` | 100,000 | 1,068,400 | 0 | bounded command/payload storage |
| `command_batch_record` | 1,000,000 | 10,310,500 | 0 | bounded command/payload storage |
| `world_change_batch_record_1` | 1,000,000 | 2,713,300 | 0 | 1 bind, 1M appends, 0 per-record lookups |
| `world_change_batch_record_32` | 1,000,000 | 130,020,700 | 0 | 32 binds, 32M appends, 0 per-record lookups |
| `hierarchy_delta_apply` | 16 | 700 | 0 | transient prepared batches |
| `hierarchy_delta_apply` | 1,024 | 28,200 | 0 | transient prepared batches |
| `ecs_snapshot` | 100,000 | 1,658,900 | 128 | cold structural capture |
| `ecs_snapshot` | 1,000,000 | 16,590,300 | 1,227 | cold structural capture |
| `world_description_build` | 10,000 | 4,009,400 | diagnostic | canonical cold build |
| `world_description_build` | 1,000,000 | 469,878,800 | diagnostic | canonical cold build |
| `world_description_lookup` | 1,000,000 | 139,018,200 | diagnostic | binary lookup |
| `world_partition_freeze` | 100,000 / 4,096 partitions | 45,875,800 | diagnostic | exact-cover freeze |

Raw qualification CSVs remain outside the repository at
`C:\Users\ChenHui\.codex\backups\lux-engine-l1-freeze-68201e05\qualification-d6754945`.
Their SHA-256 digests are recorded below so the exact evaluator inputs remain
auditable without making the source repository an evidence store:

```text
F9A46A53FB206379FE96EA56AD4C4C838BC0E298940F0DA89A51CF192DCDE386  changes-100k.csv
5E1527EF676FBFD800D914BBA798DA65ED1DD3491405D693870F8DCAF08AFE26  changes-1m.csv
FF9F48349B5AB011CD8A6CA2130B1F0FBCEEF979C4CD49FF8B1D4FB92ADADD35  commands-100k.csv
17EBC3AC907A68C77BBDB443C0072A33F0846C2AE59BCF9B6D025F2B956CD17A  commands-1m.csv
C6A4B44D3316C1122761AF6BA55EAF4F31F6325310311223B2D33A426D2FF81B  hierarchy-1024.csv
9525410E45417F0B23222C149FBCA45058547D16C07BCDD706A9A53778DE53BA  hierarchy-16.csv
F8F4B5657C283269E07E4ABCB5C6C1363C8D309B74F7FE5B6F6A70FD43B79B48  journal-readers-8.csv
7BC04BAED110454AD284B806F9932E5E4CD87C20181ED6EA963CB3F3E6993472  simulation-step.csv
6204BF2A65F3D88AF4879915727ABBFBA8B1D123212F08C22DF53C8F7D7FD14C  snapshot-100k.csv
EA7DF8B00A39971112BA66D47C761D9460D13F570A3C063AADFC1DD17E540674  snapshot-1m.csv
41B76C9E88C7AD24F3376116B0B551E2B479E4143E4B9791C6F9EAD8C8657D87  task-1024.csv
5B51AE0A1F1F1502E503399232CA429D9C7A9CB1C87829097E17623457F959B5  task-16.csv
D136762CE2C594A190DF50A92451790B2B57F6AAFF166EC0C9C4617488A86681  world-100k-p16.csv
2B187782703568A7D5A9A9CE4A90BB29C873CE68CBFEF322A3C0607B0058CA9D  world-100k-p256.csv
F199AC870D41368D2A388F7CA24E55C5EFB06F464A332321852A0F897BE52BDF  world-100k-p4096.csv
7AB92D4B270138FBF2B230A7286902F40589F2C5E13EEC4B4E5221A0DF97CC66  world-10k-p16.csv
3EFD3A469A435A59754F3558E59D1D4051E69A55CB41989E0A1FF4ECAE4D85F7  world-1m-p16.csv
```

## Status boundary

```text
L0 Generic Mechanisms                 Qualified
L1 World Architecture                 Freeze Candidate
L1 Simulation Architecture            Freeze Candidate
L1 Failure Semantics                  Hardened
L1 Hot-path Capacity Contract         Qualified
Journal Concurrency Contract          Qualified
Hierarchy Delta Contract              Qualified
L2 Process                            Awaiting Independent Acceptance
Public API Freeze                     Independent Acceptance Required
```

This record is an exact-SHA re-audit candidate. It does not constitute
independent public API freeze acceptance and does not itself authorize formal
L2 Process implementation.
