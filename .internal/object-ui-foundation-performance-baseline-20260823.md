# Object / UI Foundation 性能复审基线（2026-08-23）

状态：F0 baseline；只用于同机、同配置的相对比较。

## 当前 Object benchmark

RelWithDebInfo 的现有测试输出（迭代次数分别由现有 fixture 固定）：

| 路径 | listeners | elapsed |
|---|---:|---:|
| typed Direct | 0 | 350000 ns / 20000 |
| typed Direct | 1 | 590500 ns / 20000 |
| typed Direct | 4 | 730700 ns / 20000 |
| typed Direct | 16 | 1210000 ns / 20000 |
| typed Direct | 64 | 3213000 ns / 20000 |
| Queued | 1 | 253700 ns / 2000 |
| Queued | 4 | 935700 ns / 2000 |
| churn | — | 810400 ns / 5000 |

F0 时的 fixture 尚未提供 noinline direct、virtual、function pointer、dynamic
Signal、median/p95、cycles/op 和 bytes/connection，因此该表只保留为历史基线。

## 当前 UI benchmark

| Pane 数 | elapsed |
|---:|---:|
| 10 | 93600 ns / 10 frames |
| 50 | 422300 ns / 10 frames |
| 200 | 1663500 ns / 10 frames |

现有 Command 测试主要测 O(1) query，没有记录 route rebuild count、rebuild elapsed
或 wrapper allocation；本轮以 unchanged frame `rebuild_count == 0` 作为硬语义目标。

机器相关绝对时间不进入 CI 阈值。Listener Candidate B 只按 ADR/实施计划中的
相对收益、回退和空间阈值裁决。

## O13 listener layout 裁决

同一 RelWithDebInfo 进程使用 5 轮 warm-up、30 轮采样。Candidate B 每个 direct
listener 比 Candidate A 多 24 bytes，空间条件满足，但热路径没有达到收益条件：

| listeners | A median | B median | B 相对 A |
|---:|---:|---:|---:|
| 1 | 286400 ns | 285500 ns | +0.3% |
| 4 | 712400 ns | 795900 ns | -11.7% |
| 16 | 2516200 ns | 2881600 ns | -14.5% |
| 64 | 9725200 ns | 11505600 ns | -18.3% |

结论：**拒绝 Candidate B**。生产实现继续使用
`vector<ConnectionControl*>`；实验源码、开关和临时 target 均未保留。

## O15 最终 Object 快照

当前 fixture 覆盖 noinline member、virtual、function pointer、typed member、
scoped Signal、dynamic reflected Signal、queued、churn 和 0/1/4/16/64 listener。
本次同机快照的 20,000 次单 listener 路径为：

| 路径 | elapsed | notify allocations |
|---|---:|---:|
| typed member Signal | 353200 ns | 0 |
| scoped Signal | 302000 ns | 0 |
| dynamic reflected Signal（connect 后） | 343100 ns | 0 |

直接基线使用 200,000 次调用：noinline member 299500 ns、virtual 294500 ns、
function pointer 291300 ns。不同迭代规模只用于各自路径的稳定采样，不将绝对值
设为 CI 阈值。`Connection` handle 保持 16 bytes。

## U13 最终 UI 快照

- steady 10/50/200 Pane 的 10 帧路径分别约 0.15/0.70/2.76 ms，且 route
  rebuild count 保持不变、wrapper allocation 计数为零；
- 100/500/2000 bindings 下，50/200 次 state query 均不随 binding 总量线性增长；
- 32-byte drag payload 使用 inline wrapper storage，1024-byte payload 才进入 heap
  fallback；两条路径均通过 borrowed-view decode 测试。
