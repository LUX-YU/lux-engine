# vNext L1 v2 基准记录（2026-08-24）

## 环境与方法

- Windows 11 10.0.26200，Intel Core i7-13700KF。
- MSVC RelWithDebInfo，CMake 4.1.2，Ninja 1.11.1。
- 每项 5 次 warmup、30 次正式采样；p95 使用 nearest-rank；v2 原始样本和
  median/p95 行见 `l1-v2-benchmark.csv`。
- raw EnTT 与 `World::query<Read<...>>` 分别覆盖 100k 和 1m entities，使用
  相同数据与 20 次遍历。
- Schedule 每个样本执行 1,000 tick，覆盖 1/16/64/256/1024 systems；commands
  每个样本执行 100 tick。
- hierarchy no-change 使用 1m entities，并由 test-only detail counter 断言访问
  node 数为 0；transform leaf-dirty 使用 100k-entity World，断言只访问 1 个
  node。
- Snapshot 与 LXWS 覆盖 10k、100k、1m entities；LXWS 使用四种真实
  archetype/component 组合与 Transform2D/Transform3D codec columns，不再用
  空 component selection 得出结论。

## 验收摘要

| 指标 | 规模 | median | p95 | 结果 |
|---|---:|---:|---:|---|
| raw EnTT / World ReadQuery | 100k × 20 | 1004.7 / 911.0 µs | 1117.4 / 1094.5 µs | World -9.33% / -2.05%，通过约 5% overhead 上限 |
| raw EnTT / World ReadQuery | 1m × 20 | 10.5124 / 9.7197 ms | 11.6132 / 10.5599 ms | World -7.54% / -9.07%，通过 |
| Schedule run | 1/16/64/256/1024 systems × 1000 tick | 20.8/157.0/632.2/3055.7/12011.7 µs | 22.3/165.4/655.7/3102.7/12647.7 µs | steady run 观测零分配 |
| pure + Object affinity lane | 2 systems × 1000 tick | 32.5 µs | 32.5 µs | 通用 affinity protocol，观测零分配 |
| commands | 1/100/10k × 100 tick | 3.5/135.1/12593.1 µs | 3.6/137.5/13148.5 µs | arena warmup 后观测零分配 |
| hierarchy no-change | 1m | 0 ns | 100 ns | visited nodes = 0，零分配 |
| transform leaf dirty | 100k | 800 ns | 800 ns | visited nodes = subtree size = 1，零分配 |
| Snapshot capture | 10k/100k/1m | 0.344/4.026/88.789 ms | 0.351/4.135/93.884 ms | journal baseline 不引入二次扫描 |
| Snapshot instantiate | 10k/100k/1m | 0.335/3.479/97.090 ms | 0.352/4.387/126.035 ms | fresh baseline |
| Snapshot restore | 10k/100k/1m | 0.337/3.379/94.916 ms | 0.349/3.567/109.389 ms | 保留 WorldConfig、复用 journal blocks |
| LXWS encode | 10k/100k/1m | 2.557/30.971/305.950 ms | 5.306/34.980/355.059 ms | 真实多 archetype/component mix |
| LXWS decode | 10k/100k/1m | 2.436/32.653/418.042 ms | 3.276/37.732/447.204 ms | explicit LE + codec columns |

CSV 的 `allocations` 是基准可执行文件覆盖到的全局 allocation 观测；Windows
DLL 内部分配不一定被该重载截获。因此 Schedule/command 零分配的权威验收还由
内部 allocation-event 计数测试完成。Snapshot 的 allocation 数是 cold operation
的预期值，不作为 steady-state 零分配指标。

旧 `l1-benchmark.csv` 与 `l1-benchmark-rerun.csv` 仅保留为 v1 历史诊断证据；
L1 v2 Freeze Candidate 以 `l1-v2-benchmark.csv` 为准。
