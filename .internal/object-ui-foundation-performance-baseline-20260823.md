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

该 fixture 尚未提供 noinline direct、virtual、function pointer、dynamic Signal、
median/p95、cycles/op 和 bytes/connection，因此不能用于宣称“接近直接调用”。

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
