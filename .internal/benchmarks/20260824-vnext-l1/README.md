# vNext L1 基准记录（2026-08-24）

## 环境与方法

- Windows 11 10.0.26200，Intel Core i7-13700KF。
- MSVC RelWithDebInfo，CMake 4.1.2，Ninja 1.11.1。
- 每项 5 次 warmup、30 次正式采样；原始数据见 `l1-benchmark.csv`。
- raw EnTT 与 `World::view` 使用相同的 100,000 实体和 20 次遍历，并按样本交替执行先后顺序，降低缓存与频率漂移偏差。
- Schedule 每个样本执行 1,000 tick；commands 每个样本执行 100 tick。
- Snapshot 与 LXWS 覆盖 10k、100k、1m entities。

## 验收摘要

| 指标 | 规模 | median | p95 | 结果 |
|---|---:|---:|---:|---|
| raw EnTT view | 100k × 20 | 989.2 µs | 1.1491 ms | 基线 |
| `World::view` | 100k × 20 | 988.0 µs | 1.1365 ms | 相对基线 -0.12%，通过约 5% 目标 |
| Schedule run | 1/16/64/256 systems × 1000 tick | 15.3/141.2/593.6/3126.4 µs | 15.5/143.2/622.1/3314.7 µs | steady run 观测零分配 |
| commands | 1/100/10k × 100 tick | 3.3/135.2/13826.7 µs | 3.3/142.5/16941.7 µs | arena warmup 后观测零分配 |
| Snapshot capture | 10k/100k/1m | 0.241/2.907/30.460 ms | 0.259/4.558/36.523 ms | 线性扩展 |
| Snapshot instantiate | 10k/100k/1m | 0.239/2.765/41.283 ms | 0.241/4.630/48.423 ms | 线性扩展 |
| Snapshot restore | 10k/100k/1m | 0.241/3.104/37.237 ms | 0.305/4.887/45.044 ms | 线性扩展 |
| LXWS encode | 10k/100k/1m | 0.288/4.112/43.450 ms | 0.295/5.697/48.195 ms | 线性扩展 |
| LXWS decode | 10k/100k/1m | 0.301/3.258/28.812 ms | 0.321/4.666/34.878 ms | 线性扩展 |

CSV 的 `allocations` 是基准可执行文件覆盖到的全局 allocation 观测；Windows DLL 内部分配不一定被该重载截获。因此 command 零分配的权威验收同时由 `ecs_schedule_contract_test` 读取 command arena 自身的 allocation-event 计数完成，而不是只依赖 CSV。

`l1-benchmark-rerun.csv` 是采用固定分组次序时留下的诊断样本，仅用于说明运行次序噪声；冻结基线以成对交替采样的 `l1-benchmark.csv` 为准。
