# Object / UI Foundation 性能证据（2026-08-23）

状态：H5 public-API-finalization evidence；等待独立复审，不构成 Foundation API frozen 结论。

上一版标为 `Final frozen baseline` 的单次计时已经失效。它没有保存可复算的
5 轮 warm-up、30 轮正式采样，也没有让 allocation instrumentation 覆盖实际
Object/UI DLL 实现，因此不得再用于冻结判断。

## 1. Controlled run 环境

| 项目 | 值 |
|---|---|
| CPU | 13th Gen Intel(R) Core(TM) i7-13700KF |
| 编译器 | MSVC 19.44.35228 x64 |
| 配置 | Windows RelWithDebInfo，MSVC dynamic CRT |
| 仓库基线 | `2b314d366ec1f810190d6d28420d0b3250dfbc34` + 本轮 H0–H5 working tree |
| warm-up / samples | 5 / 30 |

每个 case 的 30 个 elapsed/allocation 原始样本保存在
`.internal/benchmarks/20260823-object-ui-h5/object-release.csv` 和
`ui-release.csv`。报告中的 median 是排序后第 15、16
个样本的平均值，p95 是第 29 个样本；机器相关绝对时间不进入 CTest 阈值。

测试 EXE 的全局 `new` 计数仍作为辅助观察值，但不再被当作跨 DLL 的唯一证明。
实际 Object/UI DLL 在测试构建中启用私有 `LUX_*_TEST_DIAGNOSTICS`：

- Object 直接统计真实 `ObjectState` buckets、listener lanes、intrusive owner vector、
  message inline/heap storage 与 incoming links；steady Direct、scoped、dynamic 与 reentrant case
  在 warm-up 后必须保持计数不变。
- UI 直接统计真实 CommandRouter route storage 与 UISession wrapper storage
  的容量增长；steady frame、state 与 invoke 在 warm-up 后必须保持计数不变。
- diagnostics 只通过 `test/pinclude` 使用；关闭对应测试选项的 production 编译
  不包含计数、计时或访问入口。

## 2. Listener layout A/B 裁决

Candidate A 是当前生产布局 `vector<ConnectionControl*>`；Candidate B 在 direct
lane 复制 `{receiver_state, invoke, control}`。B 每个 listener 比 A 多 16 bytes，
但收益条件和 churn 回退条件均未满足：

| Direct listeners | A median | B median | B 相对 A |
|---:|---:|---:|---:|
| 0 | 3,900 ns | 4,000 ns | -2.6% |
| 1 | 22,300 ns | 22,250 ns | +0.2% |
| 4 | 120,500 ns | 104,950 ns | +12.9% |
| 16 | 476,300 ns | 409,950 ns | +13.9% |
| 64 | 1,578,950 ns | 1,673,050 ns | -6.0% |
| churn / 5,000 | 2,900 ns | 4,700 ns | -62.1% |

4/16/64 的几何平均提升约 7.4%，不是至少 10%；churn 又超过
5% 回退上限。结论：**拒绝 Candidate B，生产实现继续使用 Candidate A。**
Candidate fixture 只存在于不安装的 `test/pinclude`，没有 production 开关或公共 API。

## 3. Object 快照

实际 `ConnectionControl` 为 80 bytes，公共 `Connection` handle 为 16 bytes。
下表均为 30 次样本的 median；notify 期间实际 Object storage growth 为零。

| 路径 | listeners | median | p95 |
|---|---:|---:|---:|
| typed member | 1 | 192,600 ns | 205,600 ns |
| scoped | 1 | 204,450 ns | 215,000 ns |
| dynamic reflected（connect 后） | 1 | 239,500 ns | 262,800 ns |
| typed member | 4 | 355,000 ns | 390,600 ns |
| typed member | 16 | 976,500 ns | 1,125,500 ns |
| typed member | 64 | 3,546,050 ns | 3,847,700 ns |
| queued | 1 | 204,950 ns | 242,600 ns |
| queued | 4 | 750,300 ns | 771,600 ns |
| reentrant | 1 | 162,500 ns | 177,800 ns |

调用基线使用独立 shared support target 和 200,000 次调用：noinline member
284,300 ns、virtual 293,250 ns、function pointer 347,200 ns。Signal case 使用 20,000 次 notify，queued 使用
2,000 次 publish + dispatch，因此不同迭代规模之间不作绝对值横向比较。

## 4. UI 快照

UI benchmark 建立真实 contextual bindings。steady frame 的 route rebuild 增量和
UISession wrapper storage growth 都为零；state/invoke 的 CommandRouter storage
growth 也为零。

| case | size / contexts | median | p95 |
|---|---:|---:|---:|
| steady frame | 10 Pane | 10,900 ns | 11,100 ns |
| steady frame | 50 Pane | 48,300 ns | 49,700 ns |
| steady frame | 200 Pane | 192,450 ns | 197,000 ns |
| route rebuild | 100 / 1 | 1,400 ns | 1,500 ns |
| route rebuild | 500 / 3 | 6,050 ns | 6,900 ns |
| route rebuild | 2,000 / 8 | 24,550 ns | 28,800 ns |
| state | 2,000 / 8 | 5,400 ns | 5,400 ns |
| invoke | 2,000 / 8 | 5,200 ns | 5,300 ns |
| binding churn | 500 | 24,200 ns | 24,800 ns |
| drag payload | 32 bytes | 13,800 ns | 13,900 ns |
| drag payload | 1,024 bytes | 14,800 ns | 14,900 ns |

这些数据只证明本次实现的可重复证据形状和复杂度趋势，不宣布 API frozen。
最终状态仍是 `Ontology Frozen / Public API Finalized / Implementation Hardened /
Independent Audit Pending / Engine Migration Blocked`，不等于 Public API Frozen。
