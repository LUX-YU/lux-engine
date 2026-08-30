# Engine Asset Residency：P0 Direction + DESIGN HOLD（v3.1）

> 状态：**DESIGN HOLD — DO NOT IMPLEMENT GENERIC DEMAND WIRING BEFORE BARRIER A**。
>
> 本文保留已经冻结的 ownership 方向，但故意不提供可施工的 generic demand API。
>
> 原因：当前缺少真实 Mesh/Material/Texture workload 下 demand identity、lease lifetime、failure/retry、invalidation 的证据。若现在交给实施 LLM，会高度诱导出 DemandTracker/Bridge/Registry/Manager。

---

## 1. 已冻结的 ownership

```text
CPU decoded Asset residency/lifetime
    = engine runtime shared / Host-level concern

GPU residency/lifetime
    = Render-owned

World root WorldDescription
    = Asset

World sidecar volumes
    = NOT Assets
```

Scene core 不拥有 CPU AssetResidency。

System 不直接拥有 Render GPU handle lifetime policy。

---

## 2. 当前禁止新增 public production types

在 Design Barrier A 前：

```text
NO NEW PUBLIC TYPES IN THIS AREA
```

尤其禁止：

```text
AssetResidency           # 若当前 repo 无正式类型，不在本轮先造
AssetLease               # 不提前冻结
AssetDemandKey
ResourceDemandKey
DemandTracker
AssetRuntimeBridge
ResidencyConnection
ResidencyRegistry
AssetResidencyManager
AssetResidencyContext
ResourceDemand
```

如果 repo 现有低层 Asset loading/cache 类型已存在，可以继续作为 Probe 的 concrete dependency；不要改名包装成上述 framework。

---

## 3. 为什么暂缓 generic demand protocol

当前未回答且必须由 Probe A 证明的问题：

```text
System发“需要 Asset X”时：
    这是 edge-triggered event 还是 level state？

同一 System重复发 required=true：
    是重复 acquire 还是幂等？

required=false：
    精确释放哪个需求？

多个 Entity共同需要同一 Asset：
    demand identity在哪里？

Asset尚未 resident时：
    interest token 是否已经存在？

load失败后：
    retry/cooldown由谁决定？

asset generation变化：
    stale completion/old payload如何处理？
```

没有这些答案，任何 `DemandKey/Tracker` 都是在替产品猜 policy。

---

## 4. Probe A 前允许做什么

Spatial3D probe 若需要 Mesh/Material/Texture：

```text
使用现有 Asset load/decode primitives
使用现有 Render upload Sender
使用 concrete probe-local state记录当前需要的 asset
```

允许 probe-local：

```text
unordered_map<AssetId, ...>
small concrete component/state
```

前提：不对外宣称 generic engine AssetResidency API。

Probe 的目标是收集：

```text
who issues demand
how long demand lives
how duplicates behave
what completion target is
how release happens
```

---

## 5. Barrier A 决策标准

只有真实 Probe A 至少跑通：

```text
World materialization
-> Mesh/Material reference components
-> async Asset load
-> CPU decoded payload
-> Render upload
-> GPU handle ready
-> asset no longer needed
```

之后才能冻结 generic residency surface。

优先目标仍然是**最小类型数**。

如果一个 class + move-only token 可以解决，不得造：

```text
Manager + Request + Key + Connection + Bridge
```

五件套。

---

## 6. 已冻结的 future invariants

即使 Barrier A 之后，以下仍必须成立：

```text
lease/reference count == 0 means reclaimable, not necessarily immediate destroy
CPU decoded cache policy != GPU residency policy
Render GPU lifecycle stays inside Render domain
System/Scene must not hold backend GPU allocation objects
World sidecars never enter AssetResidency
Process remains asset-policy blind
```

---

## 7. 不属于本阶段的问题

后续 P1：

```text
LRU / cache retention
memory-pressure eviction
CPU decoded byte budget
texture mip/streaming budget
retry/cooldown
hot reload/invalidation
```

---

## 8. LLM instruction

如果在 Phase 7–10 实施过程中发现“需要一个 generic AssetResidency 才能继续”：

```text
DO NOT CREATE IT.
Implement the concrete probe flow with existing primitives.
Record the exact repeated lifecycle pattern.
Return to Design Barrier A.
```

这是 intentional design barrier，不是缺失实现。

---

## 9. 2026-08-30 Barrier A evidence review

真实 preloaded Spatial3D slice 已证明：两个 World object 可共享同一组 GPU Mesh/Material/Texture handle；释放第一个
instance 后第二个继续渲染；最终释放后 slot 可回收，Texture generation 从 1 变为 2。

但该 slice 使用 procedural/preloaded CPU payload，尚未证明：

```text
AssetId -> cooked provider -> CPU decode
async duplicate completion
failure / retry / cancellation
World generation replacement
cross-Scene / second-domain ownership
```

结论固定为 `needs more evidence`。本轮不批准 generic demand/residency surface，Phase G Full 3D Streaming 继续被
Barrier A 阻断；所有禁用类型与 future invariants 保持不变。
