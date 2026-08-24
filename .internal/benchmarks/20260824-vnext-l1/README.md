# vNext L1 v2.1 基准记录（2026-08-24）

## 环境与方法

- Windows 11 10.0.26200，Intel Core i7-13700KF。
- MSVC RelWithDebInfo，CMake 4.1.2，Ninja 1.11.1。
- 每项 5 次 warmup、30 次正式采样；p95 使用 nearest-rank；v2.1 原始样本和
  median/p95 行见 `l1-v2.1-benchmark.csv`。
- raw EnTT 与 `World::query<Read<...>>` 分别覆盖 100k 和 1m entities，使用
  相同数据与 20 次遍历。
- Schedule 每个样本执行 1,000 tick，覆盖 1/16/64/256/1024 systems；commands
  每个样本执行 100 tick。
- hierarchy 覆盖 1m real balanced Parent graph initial/no-change、100k deep/star、
  star reparent 与 journal overflow full resync；结构计数断言每条 relation 只进入
  固定次数，real no-change visited nodes 为 0。
- transform 覆盖 100k star root、100k deep propagation 与 1m peak/100k active
  sparse churn；CSV 单列报告 dense cache retained bytes。
- Snapshot 与 LXWS 覆盖 10k、100k、1m entities；LXWS 同时测 World build、
  encode、decode 与 materialize，使用四种真实
  archetype/component 组合与 Transform2D/Transform3D codec columns，不再用
  空 component selection 得出结论。

## 验收摘要

| 指标 | 规模 | median | p95 | 结果 |
|---|---:|---:|---:|---|
| raw EnTT / World ReadQuery | 100k × 20 | 970.3 / 835.4 µs | 1038.9 / 909.3 µs | World -13.90% / -12.48%，通过约 5% overhead 上限 |
| raw EnTT / World ReadQuery | 1m × 20 | 10.6046 / 9.5965 ms | 11.5547 / 10.1339 ms | World -9.51% / -12.30%，通过 |
| Schedule run | 1/16/64/256/1024 systems × 1000 tick | 22.4/181.0/659.5/3199.6/12482.2 µs | 23.2/185.5/673.0/3293.5/12778.4 µs | steady run 观测零分配 |
| pure + Object affinity lane | 2 systems × 1000 tick | 29.0 µs | 31.7 µs | 通用 affinity protocol，观测零分配 |
| commands | 1/100/10k × 100 tick | 3.4/123.5/26276.7 µs | 3.5/126.3/34102.5 µs | arena warmup 后观测零分配 |
| WorldEdit/Schedule WriteQuery | 1m | 8.975/15.623 ms | 9.398/16.140 ms | shared bounded scratch；warmup 后观测零分配 |
| hierarchy balanced initial / no-change | 1m real Parent | 51.096 ms / 100 ns | 56.409 ms / 100 ns | initial O(N)，no-change visited=0 |
| hierarchy deep/star initial | 100k | 4.584/4.279 ms | 5.463/5.545 ms | relation fixed-visit gate |
| hierarchy star reparent / resync | 100k | 0.3 µs / 4.434 ms | 0.5 µs / 5.899 ms | O(1) sibling append；resync O(N) |
| transform star/deep propagation | 100k | 16.112/13.455 ms | 19.731/15.008 ms | visited = subtree size，零分配 |
| transform sparse high-water | 1m peak / 100k active | 1.6 µs | 2.0 µs | dense retained bytes = 32,400,024 |
| Snapshot capture | 10k/100k/1m | 0.460/5.387/48.108 ms | 1.329/7.358/51.651 ms | suppressing cold edit |
| Snapshot instantiate | 10k/100k/1m | 0.454/4.926/47.245 ms | 0.852/6.734/50.809 ms | zero construction journal writes |
| Snapshot restore | 10k/100k/1m | 0.451/5.311/47.254 ms | 1.372/6.591/51.647 ms | 保留 WorldConfig、复用 journal blocks |
| LXWS World build | 10k/100k/1m | 10.795/133.732/1498.140 ms | 12.644/145.766/1797.465 ms | codec traversal + archetype construction |
| LXWS encode/decode | 1m | 277.687/300.200 ms | 294.298/338.138 ms | explicit LE + codec columns |
| LXWS materialize | 10k/100k/1m | 3.232/35.155/328.938 ms | 4.225/41.844/392.523 ms | zero construction journal writes |

CSV 的 `allocations` 是基准可执行文件覆盖到的全局 allocation 观测；Windows
DLL 内部分配不一定被该重载截获。因此 Schedule/command 零分配的权威验收还由
内部 allocation-event 计数测试完成。Snapshot 的 allocation 数是 cold operation
的预期值，不作为 steady-state 零分配指标。

旧 `l1-benchmark.csv` 与 `l1-benchmark-rerun.csv` 仅保留为 v1 历史诊断证据；
`l1-v2-benchmark.csv` 是 v2 历史证据；L1 v2.1 independent re-audit 以
`l1-v2.1-benchmark.csv` 为准。
