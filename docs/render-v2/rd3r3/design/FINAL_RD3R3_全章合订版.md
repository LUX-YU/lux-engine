# RD3-R3 · 完整架构章节合订版

> 本文件机械生成，分章 00–32 是唯一编辑源。源文件位于本包根目录；本合订版不是额外一份设计权威。


---

## Source: 00_架构总纲.md

# RD3-R3 — Render V2 统一架构总纲

> **规范性质：2026-10-13 设计候选，供审查与后续按阶段实施；不是已经完成的 C++ API。** 当前实际远端锚点：`codex/render-v2` HEAD `960e755395be2c9f78a86339a062c507826c4eb4`，P2-A 实现 I `483d20ed900b8c226a740704f3c888498b1dae11`，验证 V `960e755...`，报告为限定范围 PASS。P2-B/P3/P4/P5 尚无本方案资格和授权。原历史报告、719 个冻结 Legacy 源文件不修改。本包仅是全套新设计与实施规划；不宣称新版本已构建或通过 GPU 测试。

## 0.1 总原则

**RenderEffect = RenderFeatures + 它拥有的逻辑 RenderGraph；RenderBackend 唯一维护 EffectId → (SceneId,ViewId,TargetId) 使用关系；编译器输出 GraphIR + 不变 ResourcePlan；attach 准备资源引用和 Native Bindings；一张完整 Native Graph 执行产生 Target 的最终画面。** Scene 拥有 View，但二者不反向持有 Effect/Graph/Target。Engine/ECS/Editor/Plugin 仅通过平台中立的 RenderRuntime 协议向 Server 发布类型化数据。

**单 RenderBackend、单 DeviceSession、初始化时选一块 GPU。** Vulkan 的 `InstanceContext` 在 Session 外由 Native bootstrap 持有，Session 借用它并拥有 `DeviceContext` 和 `ResourceContext`。不建立全局 Vulkan 设备单例或多 Session 调度器；后端中立的 Server 通过唯一 Native 实现接缝接入 Vulkan；未来 D3D12 替换的是 Native 实现，不是 Engine 的 RenderSystem。

**资源 Scope 不等于资源 Owner 或生命周期。** Global、Scene、View、Attachment-private、Graph transient 分别描述共享与存储范围；真正使用权由成功 attach 后的 RAII 绑定记录持有；共享资源的 Native backing 只有一个 Owner，CPU 引用结束后仍须由真实 GPU ticket / WSI / 插件 CodePin 保留。场景析构触发撤销 attach，不能直接调用 `vkDestroyBuffer`。

## 0.2 权威所有权轮廓

```text
Application / Engine / ECS / Editor / Plugin
    │  data projections → typed PROGRAM / UPLOAD; CONTROL
    ▼
RenderRuntime [neutral client, no Vk/DXGI/Native ownership]
    │ existing bounded R2 transport & reply/diagnostics
    ▼
RenderBackend [neutral server object authority, one frame driver]
    ├─ SlotMap<Scene> → each Scene owns SlotMap<View>
    ├─ SlotMap<RenderEffect> → {Features, logical RenderGraph}
    ├─ SlotMap<Target> → target description, not editable bindings
    ├─ SlotKeySparseSet<EffectId, attachments> [SOLE editable relationship]
    │    └─ attachment: (SceneId,ViewId,TargetId), prepared leases/bindings
    ├─ cold-derived target execution order / composition [NOT second authority]
    └─ Native session port [one selected implementation]
        └─ Vulkan native ownership
             ├─ InstanceContext [native bootstrap-owned; outlives session]
             └─ DeviceSession [exactly one]
                 ├─ DeviceContext [VkDevice/VMA/queues/enabled caps]
                 ├─ ResourceContext [device resource/transfer services]
                 ├─ Scoped Resource Storage [device-local, scope-keyed]
                 ├─ immutable compiled Graph programs & target variants
                 ├─ per-attachment Native executions/FIF/history/import state
                 └─ Native Target / GPU retirement
```

Compiler: `Feature resource demands + DataSchema/PassSchema + RenderGraph` → `RenderGraphIR + ResourcePlan` → Native `C9–C13` candidate. Compiler's scope labels and binding-slot names are structural, never contain concrete Scene/View IDs in Shader/PSO code identity. Attach resolves actual scope IDs and resources once. Stable frame reads prepared slots/versions without a Registry/type-name/hash lookup on every Pass.

## 0.3 明确修正 RD3-R2 的旧假设

| RD3-R2 旧写法 | RD3-R3 唯一权威 |
|---|---|
| DeviceSession 拥有 InstanceContext | Native bootstrap owns InstanceContext; DeviceSession borrows it. Device/ResourceContext owned by Session |
| Scene/View DomainId 是资源存活的主要依据 | ScopeKey 只决定共享位置；attach 的 RAII 使用权决定动态需求；GPU 完成决定 Native retirement |
| 公共 ResourceDomain 树必须是新 API 的中心 | 不强制保留公开 ResourceDomain 类；可迁移 P2-A 既有 Domain 存储作为 Session 内部 ScopedResources 实现，不再另建并行 Registry |
| Feature::create(DeviceSession&) 立即分配所有资源 | 纯 Feature 只作者态声明；编译阶段生成 ResourcePlan；attach 解析后才按需求惰性分配。可选设备专属准备留在 Native 侧 |
| EffectBindings + ExecutionJob + GraphViewGroup 多份长期索引 | 仅 EffectId → attachment 关系可编辑；调度由冷态派生，`ExecutionJob` 非必需长期类型 |
| RenderGraphCompiler 通过 Shader 猜资源 Scope | Scope/Key/Provider/写权由 Feature/DataSchema 显式声明，Compiler 验证与合并；Shader 只证明真实 Native access |
| 一个 View 持一个 History | 按真实用途选择 View-scope 或 Attachment-private history；不同 Effect/Target 不可误共享 |
| Engine 每个 Feature 编写一个 Proxy | 按组件/事件/插件注册 DataProjection，typed 数据和 Wire 共享 Schema，不给核心 RenderSystem 加业务 switch |
| OOM 失败重试是新合同 | **新设计不承诺 OOM 恢复**；Heap/device 内存耗尽按 Fatal 策略处理。非 OOM 结构/能力/业务错误保留 last-good；历史 OOM 测试结论保持历史事实 |

## 0.4 直接阅读路径

01 设计哲学 → 02 关系/RAII → 03 身份/Schema → **04 Scope/资源生命期** → **05 编译 ResourcePlan/attach** → 06 Feature → 07 Graph → 08 Shader ABI → 09/21 Vulkan Context/Session → 10 Target/Executor → **24 ECS DataProjection** → **25 Client/Backend 端口** → 11 插件 SDK → 12/23 Legacy 业务与存储 → 13/27 高级效果 → 14 代码规范 → 15 示例 → 16/17 性能验收 → 18 阶段计划 → 19 决策 → 20 迁移 → 22 Resource API → 26 反例与生命周期 → 28 目录/CMake → appendices/work_orders。

**其他章若与本章冲突视为文档错误，不能自由选择实现；验证脚本应拒绝矛盾。** `FINAL_RD3R3_全章合订版.md` 由各章机械生成，分章文件为唯一编辑来源。


### 本次 Legacy 图编译/材质图/大资源上传专项补充

架构权威补充为第 **29–32** 章：Legacy Compiler 的静态编译优化/正确性负例、MaterialGraph 的独立作者图与 Shader Cook/资产链接、R2 `UPLOAD` Lane→Native dedicated/aliased Transfer Queue 的真实路径、Transient pool 与专业 Storage 的双重性能模型。旧第 07/09/23 章的概述不得解读为已经迁移完这些路径；R3-C/R3-D/L1/L2 需逐项证实。特别强调：当前 P2-A 的 `pollUpload` **仅用于关闭排空**，没有主动处理大 Mesh/Texture 业务上传。

---

## Source: 01_设计哲学与成熟引擎对照.md

# 01 — 设计哲学：少对象，明确事实，保留 GPU 性能

## 1.1 工程取舍

优先单一事实源、现代 C++20、强类型/`lux-cxx` 生成式 ID、冷态计划与热态执行分离。一个长期类必须有独立的状态和寿命，不能为了对齐旧模块名或缩短文件而生成 `Manager/Registry/Context/Proxy/Adapter` 队列。`ResourcePlan`、`ScopeKey`、`Attachment` 可以先是编译结果值、复合身份、Backend 私有 move-only 值；不要因有名词就产生一个独立公共管理器。硬件能力由真实 supported/enabled 区分，未启用时显式 fallback 或错误。

## 1.2 与成熟引擎的关系

RenderGraph/Pass/ResourceUse/Producer/History/Transient lifetime 应支持 Deferred、Clustered、GPU-driven、Temporal、Compute、Copy、multi-queue、资源 Residency 等组合；不在业务层新建一套 `ShadowRenderer`、`PointCloudRenderer`、`UIFrameLoop`。面向 UE 类效果**不表示已经实现 Nanite/Lumen/TSR/VSM**：第 13 与 27 章逐项列出必须补的 shader/native/storage/primitives 与真实质量 oracle。设备功能缺失时由真实 Capability Contract 处理，而不是默默切换到功能不同的算法并声称同等结果。

## 1.3 尊重旧代码的算法价值，不尊重错误结构

Legacy `TGpuBuffer` 的 Dirty epoch、HostMirror/Non-mirror、并发上传回退；`TPagedGpuStream` 的稀疏更新合并；稳定 record pages；Mesh TLSF + chained arena；Bindless slot/generation/mip residency；GPU Instance streams；PointCloud chunk pool；Transfer ring 和 deferred retirement 都须逐项审核并尽可能保留。旧 `RenderContext` 大型 service hub、每 Feature 虚基类/Scene 接口、用 `unordered_map` 做高频身份查找、FrameSerial 充 GPU 完成凭据，均不自动继承。是否替换某优化算法，以同规模 GPU 输出/CPU 时间/VRAM/分配和错误路径证据判断。

## 1.4 原始性能与新性能资格

旧 F4/F5/F6.b `PARTIAL` 资格保留而不作为每次结构变更必须跑的 p95/CI 门禁；新框架先验证最小实机纵向路径与热点复杂度。等业务迁移稳定后，按新可比工作负载对比 Legacy，并记录历史失效测试为何不适用。GPU 同步、错误读写、生命周期不因架构速度优先而免测。

## 1.5 四种事实不能互换

1. 逻辑身份（EffectId/SceneId/ViewRef/TargetId）≠ 结构性编译身份（Shader/Graph Layout）≠ Native 资源身份（VkBuffer/VMA allocation）≠ 业务数据源身份（ECS Entity/asset）。
2. 静态资源需求 ≠ 已分配资源；Usage Lease ≠ Resource owner；CPU logical release ≠ GPU complete。
3. 设备无关数据 Schema ≠ Wire Encoding ≠ Shader-facing GPU ABI。一个 ECS Component 不必能直接 `memcpy` 到 GPU。
4. Backend 独立 ≠ 为每个潜在图形 API 提前创建虚基类大全；仅在 Native Provider 边界需要真实实现替换合同。

---

## Source: 02_模块分层与所有权.md

# 02 — 唯一所有权、Scope 与使用关系

## 2.1 唯一可编辑关系

`Scene` 在 Backend SlotMap 内，且唯一拥有其 `SlotMap<View>`；`ViewRef={SceneId,ViewId}` 为完整身份。`RenderEffect` 独立于 Scene/View/Target，持作者 Feature 配置与一份逻辑 RenderGraph。`Target` 保存 format/extent/surface/offscreen/layer 的中立输出合同，但不持另一张独立可编辑的 Effect/Scene 使用表。

Backend 唯一的 Effect 关联权威是 `SlotKeySparseSet<RenderEffectId, vector<Attachment>>`。每条 Attachment 的身份是 `(EffectId,SceneId,ViewId,TargetId)`，可带 placement、instance key；不分配第二个 BindingId，除非实际异步 Client 对解绑后的回执需要不可歧义的版本化身份，此时先评估 scope/attachment epoch 而不是自造另一套 allocator。Backend 对 Target 的完整执行顺序只保存**由这张表冷态派生的计划**；Native GraphId 分组只是执行优化，不能更改层次依赖。

## 2.2 三种生命周期

- Scene/View **逻辑生命周期**：删除 Scene 先撤销全部 View、对应 Attachment，再使生成式 ID 失效。Scene/View 并非 GPU Buffer 真 Owner。
- Attachment **使用权生命周期**：成功 attach 创建 Backend-owned move-only record，持 ResourceLeases + Native Binding/Context 引用。重复 attach 不重复持有资源；detach 释放使用权。被多个 Attachment 同范围引用的 Storage 可共享；最后一个 release 后按 retention policy 进入可回收/缓存状态。
- Native **物理生命周期**：唯一 Native RAII owner 持实际 GPU backing；Cache/ResourceLease release 后，还须直到 accepted submit/transfer/WSI/CodePin 证明安全退休，不能用 FrameSerial/Effect revision 替代真实 Ticket。

## 2.3 设备层与用户层

RenderBackend 从 Native Provider 接收单个 NativeSession，公开 API 与 Engine 不含 Vk/DirectX；Vulkan bootstrap 持有 `InstanceContext`（多物理 GPU 发现），比唯一 DeviceSession 寿命长。DeviceSession 持 DeviceContext/ResourceContext、GPU ScopeStorage、Native compiled code/contexts/targets。编译后的 Graph Code 没有 SceneId/ViewRef/GPU backing 直接所有权；Context 只借真实资源、拥有本次 Descriptor/slots/tickets/temporary backings。

## 2.4 资源作用域不是容器类型

`Global`：DeviceSession 内共享 Mesh/Texture/Material 等；`Scene`：SceneId 维度的 Instance/Light/Scene Cache；`View`：ViewRef 维度的 Camera 等共享视图数据；`Attachment`：相同 View 不同 Effect/Target 的历史/私有缓存；`Transient`：纯 Graph lifetime/aliasing。作用域以 `ScopeKey` 表达，具体 Storage 对象由 Session ResourceContext 的唯一内部 ScopedResources 存储；不需要对每种 Scope 派生 ResourceDomain 子类，也不需要 Scene 本地的第二份 Registry。

P2-A 已有 `ResourceDomainId`/`ResourceDomain`/`SlotMap<unique_ptr<...>>` 是可复用的内部实现候选，但不是最终外部 API 的永久约束。迁移阶段可建立 ScopeKey→旧 DomainId 的一个冷态映射，完成生命周期重构后删除过渡映射；**禁止**同时长期保持 Domain、ScopeResources、EffectBinding 三份资源所有权权威。

## 2.5 拒绝的模型

`View.effect/graph/target`、`Target.view`、`Effect.scene/view`、Scene feature registry、强制每 Feature 一个 Proxy、另设 ResourceDomainManager、长期 ExecutionJob 注册表、设备级所有资源的统一类型管理器、每 Scene/Feature 复制 Shader/PSO、Engine NativeDevice 访问、跨 DLL 传 STL owning ABI。一个 CPU 借用从 SlotMap 的 dense value 取得后不能越过 erase/compact；必要时使用 `StableSlotMap` 或唯一 `unique_ptr` 稳定地址。

## 2.6 可控释放顺序

阻断新请求 → owner-thread 停止可编辑关系 → 撤销 attachment/让资源使用权退场 → 清理派生执行计划 → 收齐真实 Native pending tickets → 释放 Native attachment contexts/transients → 清理 scoped persistent resources（子 scope 先于父）→ 释放 Native Graph/Shader/PSO → 队列/allocator/device → InstanceContext。跨 Plugin 的析构代码需 CodePin 覆盖全部生命周期。内存不足按 fatal policy；结构/ABI/不支持/重复 writer 等可恢复候选失败必须保留 last-good。

---

## Source: 03_数据描述与类型身份.md

# 03 — 分离的身份、DataSchema、PassSchema 与 Provider

## 3.1 对象 ID

外部运行时对象使用 `lux-cxx::SlotKey<Tag>`，由唯一 `SlotMap/StableSlotMap` 分配，不以 Hash 创建 ID。`ViewRef` 包含 `SceneId+ViewId`；跨 Runtime 协议额外携带 R2 owner identity。`RenderGraphId` 是本次唯一 Native Session 中编译程序的生成式身份，不是逻辑 Graph 名称；不存到 View；在无多 Session 的本期不新增 DeviceSessionId。`ResourceKey`（资源名）与 `ResourceScope`（共享范围）构成 ResourcePlan 的逻辑槽身份，不充当生成式资源 ID。

Shader/Schema 名称使用稳定 canonical-name TypeId + 原始字符串 collision check，不把 64-bit Hash 直接当 sparse array index；ScopeId 是受 backend 验证的 Session root、SceneId、ViewRef 或具体 attachment 关系，不等于 ECS Entity。

## 3.2 三种 schema

| 合同 | 作用 | 禁止 |
|---|---|---|
| `DataSchema<T>` | 中立语义、版本、可选 CPU/GPU/Wire representation、source authority/可见 scope | 直接把任意 ECS struct 视为 GPU/Wire ABI |
| `PassSchema<P>` | GPU passes 的 typed resource uses、访问范围、Stages、producer version、push/descriptor shapes | 只凭 Feature 名称生成假的 GPU barrier |
| `ProjectionSchema<P>` / 显式 ProjectionDescriptor | ECS/Editor/外部数据如何转换到某 neutral DataSchema、观察哪些变化、如何提交/背压 | RenderBackend include ECS Component 或新业务 switch |

复用既有 `lux-cxx` meta generator/Shader Toolchain、SPIR-V reflection/当前 F6.a/b Full OwnerShape：**只有一个字段语义来源**。CPU-only 非平凡类型不要求 trivially-copyable；GPU-only 不强制 CPU 镜像；Wire 是独立版本化编码，禁止 across DLL owning STL。

## 3.3 编译时结构与 attach 实例

Compiler 按 Feature + DataSchema 解析 `ResourceRequirement{Type, Key, Scope, Access, ABI, optional/required, provider, config}`；Shader/PassSchema 决定真实 GPU Use、RAW/WAW/WAR 与同步。具体的 SceneId、ViewRef、Target 实际尺寸、GPU Buffer handle 只在 attach Native Binding 阶段出现，不得混入共享 PSO/Shader layout key。

多个同类型资源例如 `Light.main`/`Light.preview` 必须不同 semantic key；没有默认 key 时仅当唯一合法含义经 contract 说明才允许，否则拒绝 Ambiguous。不同 Writer 需要确定仲裁或数据分区，不以 Feature attach 顺序隐式覆盖。

## 3.4 数据生产者与 GPU Store 的分工

Engine ECS 持 CPU World/Asset 业务 authority；Editor/Grid/外部传感器/插件也可通过自己的 Producer 发布中立数据，进入 PROGRAM/UPLOAD。Server 对 typed payload 校验 Owner/Version/Schema，并更新相关存储的必要片段或其 GPU-owned 队列。Feature 只声明数据需求/输出，不为数据源自动实例化第二份 ECS。

---

## Source: 04_作用域资源与RAII生命周期.md

# 04 — ScopeResources、Attach RAII 与唯一物理 Owner

## 4.1 设计裁决

本章**取代旧“独立 ResourceDomain 类必然承担完整生命周期”的前提**。`Global/Scene/View/Attachment/Transient` 是作用域策略，不是五种 Registry 类型。唯一 DeviceSession 的 ResourceContext 内可以用一个私有 `ScopedResources` 及稳定的 `ScopeRecord` 持久存储；ResourceDomainId/ResourceDomain 仅作为已完成 P2-A 代码的有界迁移实现，最终不得成为第二份所有权表。上层 Scene/View 不持 Native 资源或显式查找其他 GPU Scope 的 Registry。

```cpp
// 目标语义；不预设公开 C++ class、返回类型、exact member shape
struct ScopeKey {
    ResourceScope scope; // Global / Scene / View / Attachment
    // typed owner key: root / SceneId / ViewRef / effect-binding tuple
};
struct ResourceSlotKey {
    RenderDataTypeId type;
    ResourceSemanticKey semantic;
};
// 一个 native storage 身份由 (device session, ScopeKey, ResourceSlotKey, ABI/config)
// 决定；具体 GPU backing 的唯一 owning RAII 对象位于 ScopedResources
```

`ScopeKey` 必须通过 Backend 已验证的 attach 关系构造；不要让外部消息直接拼凑未验证 SceneId/ViewId 形成可读任意 GPU 数据的 scope。`Global` 根作用域是单 DeviceSession 的资源库；`Scene` 按 SceneId；`View` 按完整 ViewRef；`Attachment` 按 Effect + 三元组 + 实际输出兼容配置身份（处理不同分辨率/History）。Graph transient 不在 ScopedResources 中。

## 4.2 attach 与 lease

`attachEffect(E, S, V, T)` 先校验所有身份、Target、Effect 编译版与 Scope，获取已经编译的 ResourcePlan，再为 plan 的每个持久 Slot 选择 ScopeKey、检查兼容性、复用或惰性创建。成功构建 `PreparedBindings`、ExecutionContext 和 ResourceLease 后才把完整 Attachment 插入 Backend 的唯一权威 Effect 关系表。重复 attach 的请求必须是幂等/明确 `AlreadyExists`，不会重复拿 lease 或重复分配 Native slot。结构/ABI/Provider/权限不匹配时旧关系仍保持不变；**OOM 是致命错误，不设计 retry 或 OOM rollback 协议**。

```text
compile Effect resource declaration → immutable ResourcePlan
  → validate Attachment identity, target signature, capability & writer
  → resolve each required ScopeKey + ResourceSlotKey
  → acquire shared lease or construct specialized storage once
  → prepare native resource binding, history, descriptor & slot
  → publish one complete Backend Attachment
Detach/Scene-remove/Effect-retire → drop leases → storage may be cached/released
  → Native backing waits accepted Submit/Transfer/WSI CodePin completion before destroy
```

**资源真 Owner 不在 Attachment 中**，Attachment 只持拥有期限受控的 Lease；真实资源只位于 Session ScopedResources/Native backing，所有消费者拥有各自引用。Session 先拒绝新的 attach 再开始 teardown。Scene 删除先通过 Backend 撤销全部含 Scene View 的 Attachment，禁止 Scene destructor 直接释放 GPU 资源。

## 4.3 ResourceLease 最小语义

`ResourceLease` 是表示使用权的抽象名，优先以 move-only 的内部 `ResourceRef`（稳定 SlotKey/指向 StableSlot 的 lease）实现，不要求每个资源一个 refcount heap control block。Acquire/Release 只在唯一 Backend owner-thread 上完成；最后一个 logical holder 释放后依 `Retention {OnLastDetach, Cached, Explicit}` 选择回收时机；Native retire 拥有真实 Ticket 保护，即使 CPU use count 已归零也不能销毁仍被 GPU 使用的 Buffer 或 descriptor slot。已提交的编译程序、实际 Imported backing 或 plugin callback 的 code pin 也必须活过最后使用者。

`OnLastDetach` 适合某 Attachment 的独占 History；`Cached` 适合代价昂贵但近期可能再用的 Root Mesh/Texture；`Explicit` 适合独立预加载的资产。**资源不必因没有 View 而绝对不可预加载**：Asset service 可以持自己的显式 Lease，但不能绕过 ScopeKey 与共享 Owner。

## 4.4 多种 Scope 示例

| 资源 | 默认 Scope | 判定依据 |
|---|---|---|
| Mesh geometry/Texture assets/Material catalog | Global | 跨 Scene/Effect 使用相同资产, GPU index/arena 统一 |
| Instance/Scene Light/Scene spatial data | Scene | 一个 World 的实例/灯光和基准坐标独立 |
| 通用 Camera GPU data | View | 同一 View 的所有兼容 Effect 可以读取相同当前相机 |
| TAA/TSR History、针对不同 render scale 的 HZB | Attachment | 即使相同 View，不同 Effect/Target 也不能无条件复用 |
| GBuffer/Lighting temporary/color resolve scratch | Transient | 由 Graph 生命周期/alias plan 分配，且 GPU 使用完可重用 |

这些只是默认建议；具体业务 `ResourceRequirement` 可选择更精确 Scope，并由 Compiler/attach 验证。两个 Effect 在同一 Scene 消费同一 `Light.main` 可共享 GPU Storage，但只能根据该 Scene 的正确版本与访问范围合法共享 GPU 计算工作。

## 4.5 新资源访问 API 的位置

`getOrCreate<T>(factory)` / `tryGet<T>()` / `getLocal<T>()` 等统一 camelCase API 仍存在，但作为 Session 的 ScopedResources 冷态接口。**Feature 作者、Engine/ECS、Executor 热态不需要显式调用** `session.getResourceDomain(id)`。兼容性定义为 Type/Key/Config/GPU ABI 完全匹配；需要不同配置应使用明确新 ResourceKey 或受控 Replace transaction。`getOrCreate<T>(std::move(owner))` 只表示接管完整 Native owner，并且已有条目时必须定义拒绝或不消耗候选的清晰语义，不能偷偷覆盖。

## 4.6 删除与失败的实际顺序

先撤销关系 → 冷态撤销 prepared binding → 释放 CPU lease → Native 按 queue ticket/WSI 退休 → DeviceSession 关闭时按真实 accepted last-use 完成 → ScopeRecords 子/父销毁 → Session Context/Device/Instance。失败的未发布 attach 候选无外部可见关系；它的已准备资源可按 cache 策略安全保留或释放，不允许让已有 attachment 指向一半重建的 descriptor。Graph-native import alias/使用范围仍交由当前 R4 Sync2/compiler 的唯一正确实现。

---

## Source: 05_ResourcePlan与Attach编译事务.md

# 05 — Compiler 派生 ResourcePlan，attach 一次绑定，Executor 直接运行

## 5.1 编译器可以确定什么

RenderGraphCompiler 可以从 Feature 的显式 `requirements`、中立 `DataSchema`、Shader `PassSchema`、Graph `ResourceUse`、输出端口及目标能力生成**结构性** `ResourcePlan`，包括资源类型、Scope、Key、required/optional、Shader-facing ABI、访问权限、Owner/Writers、格式/usage、容量规则/默认配置、Fallback、Retention/History Policy 与 Binding Slot。

**不能自动从 Shader 推出 Scene/Global/View Scope 或 ECS 数据来源。** Shader 只能确定真实的 Buffer/Image 范围与 read/write/stage；Scope/Provider/authority 由 Feature/Schema 明确声明。具体 Scene 数量、实际 vkBuffer 地址、View 的 Camera、当前 revision、Target 尺寸在 attach 后才知道；相对尺寸和动态配置在 attach 时完成合法 Native 细化，不反过来更改已冻结 GPU ABI。

```cpp
// 规范示意：由 Feature authored demand + PassSchema lowered；并非已实现公开 API
struct ResourceRequirement {
    ResourceSemanticKey slot;
    RenderDataTypeId type;
    ResourceScope scope;        // GLOBAL / SCENE / VIEW / ATTACHMENT / TRANSIENT
    AccessContract access;     // actual GPU read/write/stages/range
    CreationContract creation; // required/optional/config/capability/fallback
    AbiIdentity abi;
};
struct ResourcePlan {
    std::vector<ResourceRequirement> requirements; // canonical, deduplicated
    std::vector<BindingRecipe> bindings;            // pass-field → plan-slot
};
```

`BindingRecipe` 引用编译生成的稳定数字 Slot，不包含每帧 string lookup。实例版 `PreparedBindings` 存储 Native Buffer/ImageView/Descriptor/Ticket 借用和版本状态；背后的 ResourceLease 由 Attachment 持有。热态 Executor 只消耗 Graph + Context + PreparedBindings，不调用 `ResourceDomain::tryGet<T>()` 或重复编译 ResourcePlan。

## 5.2 Compiler 与 attach 的时序

1. `RenderEffect` 持 Feature collection + 逻辑 Graph，Feature 构图/资源需求是作者阶段。
2. C0–C8：Canonicalize/Resolve/producer+version/hazard/topo/lifetime/scope proof → immutable GraphIR 和 ResourcePlan；资源需求冲突、ABI/Writer ambiguity、Scope 泄漏 cold-fail。
3. Native C9–C13：Shader+Layout、physical resource/alias plan、command recipes、Queue/Sync2、assembly；可根据 Device caps/Target signature 创建不可变代码变体。
4. Attach：给 ResourcePlan 每个外部资源槽确定 ScopeKey 与实际 Owner，复用/创建 Storage、取得 use lease、检查 readiness、绑定 Descriptor/Native Context；仅最后发布 Effect triple。
5. 热帧：只更新受影响版本、上传 Dirty、必要 Descriptor patch，直接执行计划好的 slots；编译结构和资源发现不在每帧重复。

Graph 的 transient 自 C7 Lifetime/C10 planning 管理；Persistent History 必须标注具体 Attachment/View 作用域，显式 epoch/fallback，不能混入 ephemeral alias。共享编译代码不意味着 Scene 的 GPU 内容可共享。多 View sharing scene Pass 保留原 Scope proof/side-effect 和真实 GPU donor Ticket。

## 5.3 Attach 更新不是隐式资源重建

Effect 修改含 Feature/Graph 结构时：为所有受影响的 `EffectId → Binding triples` 和其关联 Target 完整输出闭包准备候选 Graph/ResourcePlan/native contexts、验证资源配置，冷态一次安全发布；任何非 OOM 结构失败保持全部 last-good。起始版本可以在唯一 Backend owner-thread 内串行准备/提交，相关 CONTROL 不允许插入。将来跨帧异步时必须额外验证 Effect revision、Attachment 集合 epoch、Target epoch、Resource generation、Device capability revision 和 CodePin。

Hot 参数更新、ECS 类型数据 delta、资源版本/Descriptor 变化不自动重编 Graph；仅拓扑/Shader ABI/显式 Native specialization 才新 code key。不要因 `EffectId` 相同或 Graph 哈希相同就认为 Native Code/Writer Scope compatible。

## 5.4 编译期优化机会

- 资源需求去重：相同 `Type+Scope+Key+ABI+Config` 才合并；多个 Producer/Writer 冲突拒绝。
- 显式 Binding Slot：无热态全局 TypeId/map/string 查找；已验证 Native descriptor recipes 内可跳过与当前完整绑定结构完全相同的冗余 bind。
- Transient alias：依据 Resource Use 的精确 mip/layer/buffer byte-ranges 与 GPU overlap 证明；不能仅通过 scope 相同或不相同作别名优化。
- 上传合并：Scene/Global 状态按数据版本 Dirty coalescing 一次发布；不同 View 真正相同的 Scene Pass 也必须验证同一 Frame 输入与 side effects。
- Queue sync：C12 根据真实 RAW/WAR/WAW 和 imported ready tickets 做最小合法 Sync2 barrier、Queue ownership，绝不能因“资源已附加”就省掉动态 hazards。

## 5.5 典型反例

同一 View 被同一 Effect 输出到 4K Target 和 1080p Target：Graph 代码可能按 extent specialization 不同，History 绝不复用同一 Attachment slot。两个 Scene A/B 共用 Root MeshStorage，但 Scene Instance/Light 必须不同。只有相同 RenderGraphResourceId 不足以跨两份 Graph 复用 transient Buffer。一个 Plugin Feature 只声明 Light 数据而没有任何 GPU 读写时，不能因此插入无意义的 barrier。

---

## Source: 06_RenderEffect与Feature组合.md

# 06 — RenderEffect 是 Graph + Feature 集合；Feature 编写与资源准备分离

## 6.1 语义不再混同

`Shader` 实现 GPU 计算与着色；`RenderFeature` 定义可组合的 GPU Pass、所需中立数据和可选 CPU 预处理逻辑；`RenderGraph` 定义实际 GPU 工作的资源、生产者、依赖、条件与输出；`RenderEffect` 是逻辑 Graph 和 Feature 集合的单一组合/编辑及版本拥有者。它不含 Scene/View/Target/Native Session 的长期反向引用。Native Graph 在唯一 DeviceSession 编译代码容器中，当前 `RenderGraphId` 指 Native 编译身份。

```cpp
// 概念性 class shape；不主张新增 FeatureCollectionManager
class RenderEffect {
    // small FeatureEntry collection (author configuration + code pin)
    // one logical RenderGraphDefinition / IR ownership
    // immutable candidate revision + shared hot parameter schema
};
```

不得把 Feature 当 GPU 数据的默认 Owner，也不再强制每个 Renderer 功能都必须有 Feature。例如旧 `LightFeature` 如果仅为资源提供者，改为独立 Scene `LightStorage` + Engine projection；真正阴影和 lighting Pass 才有对应 Feature。`StandardMeshStackFeature` 资源提供功能与 GPU-driven draw algorithm 明确拆开。

## 6.2 两类 Feature 的创建和状态

**纯作者 Feature**：可以是简单结构/自由函数，`static describe(config, requirements)` 和 `build(config,builder)`；不需要 DeviceSession，也不要求 `IRenderFeature` 继承。Shader variations 与 GPU ABI 的结构依赖可编译为版本化 Graph。

**确有设备相关行为的 Feature**：设备相关准备不要绑定到长期、跨 Scene 的 RenderEffect author；由 Native 侧在 compile/attach 阶段通过当前 `DeviceContext`/`ResourceContext` 或确实必要的 `DeviceSession&` 创建其运行期实现。其 Lifetime 由 DeviceSession/Attachment 持有，资源需求仍由统一 ResourcePlan 表达。允许插件提供可选 `prepareNative` ops，但是有真正消费者时才新增，Host 必须管理 CodePin；不要每个 Feature 默认启动 `Worker`、Pool、Registry。

```cpp
// 作者态示意
struct GridFeature {
    static void describe(const GridOptions&, ResourceRequirements&);
    static RenderResult<void> build(const GridOptions&, RenderGraphBuilder&) noexcept;
};
// native prepare 是可选 hook，不在 Native record 路径通过 Feature this 执行
```

如果一个 Effect 创建成功却从未 attach，默认不应预分配 Scene/View/Attachment GPU Storage。Session 设备级 Shader Code 可能在创建 Effect 时冷编译，但业务存储遵循 attach/preload lease；Effect 创建时不能捕获“某 Scene 的 LightStorage”。Feature 可以有 Effect-scope CPU 参数，实际 GPU runtime backing 必须有明确 Session/Attachment owner。

## 6.3 资源与数据需求

`Feature.requires<T>(scope,key,access,config)` 产生逻辑请求；`PassSchema` 保留真实 GPU 读写范围。`DataSchema` 可 CPU-only/GPU-only/external，GPU-only 不能要求虚构 CPU mirror；Renderer 不凭类型 T 自动生成 ECS Proxy，也不能按第一个注册的类型实例猜数据源。多个 Feature 共享相同 Storage 时由 Compiler 合并相容需求，attach 增加 lease；不相容则 cold error。

## 6.4 无状态 ≠ 无行为

少数 Feature 需要在帧开始准备 View jitter、CSM cascade、LOD budget 或反馈优先级。优先由明确的 typed Data Producer/Native preparation 上传参数或编译成 GPU Pass，不让编译 Graph 的 `[this]` Callback 在 GPU record 热态访问正在变化的 Feature。确实需要 CPU per-frame 程序时，用生命周期稳定的固定 Ops/准备表，在特定事件版本变化时执行，不能引入每 Feature 额外 FrameLoop。

## 6.5 C++20 作者与插件

C++20 Concepts 用于本地 Author `describe/build` 签名验证；可选 hook 通过 `if constexpr` 生成固定 Function Ops，不为所有 Feature 强制虚接口。跨插件 DLL 使用版本化 extern C 表和 Host-owned 值元数据，绝不跨边界传 owning STL/RTTI/异常；在 GPU Native program/callback/destructor 仍在途时保留插件 CodePin。Installed SDK consumer 需能不修改 Runtime 核心就新增 Feature+DataSchema+Projection。

## 6.6 Effect 编辑事务

Effect 的逻辑身份在结构修改时保持稳定；候选 Feature 配置和逻辑 Graph 编译成功后，影响所有 attach 及同 Target 组合后续闭包；一次发布或全部 last-good。调整 exposure/grid opacity 等 hot values 只更新数据，不重建结构。Graph native cache 按完整 structural/ABI/caps/target signature 复用，**没有第二个独立拥有逻辑 RenderGraph 的 GraphCache**。

---

## Source: 07_RenderGraph推导与编译.md

# 07 — 一份逻辑图、ResourcePlan 与 Native C0–C13

## 7.1 逻辑图由 RenderEffect 拥有

`RenderEffect` 中的 `RenderGraphDefinition` 与 Features 是唯一作者态权威；`RenderGraphIR` 是降低后的 immutable 编译值，不需要全局逻辑 GraphManager/第二 GraphCache。Target 混合多个 Effect 时构造的组合 IR 是**编译候选派生值**，不再创造另一份可供用户编辑的逻辑 Graph authority。

## 7.2 固定阶段

逻辑 C0–C8：Canonicalize → Resolve data producers/versions/semantics → interface validation → RAW/WAW/WAR/subresource hazards → topological sort → reachability → lifetime → Scope sharing proof → immutable IR + ResourcePlan。Native C9–C13：shader/layout/PSO → physical resource plan and aliasing → Native command/descriptor recipes → Sync2/Queue mapping → compiled executable assembly。每一阶段可以是自由函数/实现 TU，不引入动态 CompilerPassRegistry、第二 Shader parser 或每 Feature 自定义第二套 hazard analyzer。

## 7.3 Graph Native 复用与作用域

Native Program 归 Session，本 Session 的 `RenderGraphId` 由 lux-cxx SlotMap 分配。Cache 可先用有界 vector 全结构比较，包含 Device enabled caps、Shader code+ABI、Owner layout、pipeline、Target format/samples/extent specialization/operations；hash 只作加速提示，碰撞须全文比较。SceneId、ViewRef、Effect 热参数、实际 VkImage/VkBuffer 值不进入 Native Code 键。同构不同 Effect 可选共用 Program，实施不以所有效果必须 dedupe 为基础。

## 7.4 完整 Target 图

一次 `execute(finalGraph, preparedInputs, target)` 表示当前 Target 画面的**完整 GPU 图**：Shadow、HZB、Depth、GBuffer、Lighting、Post、Tonemap、Overlay、明确 Composite/Resolve、导出。Target 同时由 E1/E2 的多个 View 使用时，Backend 按唯一 EffectAttachment 集合与其 placement 排列，并由 Compiler 在临时组合 IR 中 remap Graph resource/pass/producer namespace，声明最终 Composite。不能在 Graph 执行后隐藏第二套 GPU `PresentationRenderer` 或无法被图追踪的 Tonemap；真正 WSI acquire/present 只属于最终展示操作，非隐藏渲染阶段。

跨 Target 使用完成画面要以显式 output import + Ready Ticket + Native state 表达，不得直接偷看另一个 Graph 中间 transient。多 Effect 更新的受影响闭包以最终 Target 图为单位计算，串行候选发布整体成功或保持所有消费者旧有效图。

## 7.5 高级 Native 的可扩展性

Graph 必须表达 buffer range 与 image mip/layer/3D/cube/array/MSAA、history current/previous、conditional fallback、async compute/transfer、GPU feedback、indirect/indirect count（按真实需求扩 primitive）、local read/rendering load/store、Readback、multi-view/fallback。`Capability Supported` 与 `Enabled` 不能混淆；未实现 RayQuery/AS/mesh dispatch 不得编个枚举就宣称可支持 UE 级效果。

## 7.6 编译优化的前提

Alias 只对生命周期不交叠且 memory requirements 相容的资源；Sync2 只按真实访问/Queue/alias/last-use 证明最小合法 barrier；Descriptor bind 只在同一有效命令缓冲依据完整布局/版本/资源相同跳过；Scene Pass 复用只在同一 Scene、输入/副作用可证且 donor 已被真实 Native Ticket 保留时发生。无任何优化可以改变实际 pixel/numeric oracle、GPU hazard 或 last-good 生命周期。


## 7.7 Legacy RenderGraphCompiler 必要细节（规范索引：第 29 章）

不得只以 C0–C13 阶段名替代真实功能验收。Legacy 有 `ResolveForwardReferences/UsageUnion/DeclaredExternalConsume` 的先后依赖；`eliminateStarvedPasses` **早于** `eliminateDeadPasses`；`LiveAccessIndex` 仅计算活 Pass 且由 Attachment Load/Store 与条件链共用；`conditionChain`、动态 LocalRead boundary、跨 Queue 提交模板、第一/后续 View Barrier、预编译 descriptor bind masks、MeshBucket/ExecutionProgram/动态 Patch 等真实机制。新 R4 Typed Native Recipes 可提供等价行为而不复制旧 bytecode；须以第 29 章逐项反例验收，不可“只实现拓扑排序”就称完整 Compiler。

Graph 瞬态物理池不能与长期 `ScopedResources` 混用：VMA transient/lazy image pool、兼容 `TransientResourceKey`、真实 last-use/recompile reuse/resize candidate 的价值参见第 32 章。材质作者 `material::MaterialGraph` 不属于 RenderGraph 的 Pass 图，详见第 30 章。

---

## Source: 08_Shader与布局自动生成.md

# 08 — Shader ABI、数据类型与直接 Native Binding

## 8.1 统一的描述来源

沿用现有 Schema/Generated PassParams、Shader reflection/SPIR-V patch、F6.a/b full structural OwnerShape 算法；不写第二个 parser 或手工固定 `SCENE/FEATURE/PASS` 三个 Descriptor Set。`DataSchema<T>` 描述长期中立数据类型，`PassSchema<P>` 描述某一 Pass 的真实 GPU 读写，`ResourceRequirement` 描述共享与存储范围，`NativeLayout` 根据全部使用需求确定 Descriptor/Pipeline Layout。CPU/Wire/GPU representations 可不同，由既有 meta/toolchain 显式生成、验证并版本化。

结构代码 Identity 包含 Shader bytecode/entrypoint/ABI、full OwnerShape、pass layout、GPU enabled caps、Target compatibility；运行时 SceneId、ViewRef、Effect hot params、ResourceLease/vk handles 不在代码 Identity 中。真实 Bindings 在 attach 冷态解析成 indexed recipe，更新数据时只在 backing/descriptor revision 改变时 patch。若 Native descriptor compatibility 不符应候选失败；不能通过魔法 `first registered light` 解决。

## 8.2 Shader 资源与复杂业务

Mesh/Material/Texture/Light/PointCloud 等 Storage 允许持不同底层 native Buffer/Image/Descriptor 实例，但对 Graph Builder 统一提供 `ResourceBindingDesc`（Buffer/Image/Subresource/ImageView/Access/ABI/Ready）；专业 Storage 的内部 `updateTransform`, `uploadChunk`, `replaceMip` 等仍应按领域独立，不强制为每种业务提供同一个上传虚函数。

图形 Native 实现用 Vulkan SPIR-V；未来 D3D12 从中立 Shader variant/Schema 生成并验证 DXIL/Root Signature/PSO。不能把 SPIR-V 字节直接当 DXIL，或要求 Engine/Projection 直接调用 `vkCmd*`。

## 8.3 优化与负例

固定 topology record/submit 预构建 Pipeline、Descriptor layout、Native bindings；同一 CommandBuffer 上只有完整 bind state 相等才能跳过 descriptor/PSO bind。Shader resource subrange、Queue family、layout、ready barrier 仍经唯一 Native compiler/Executor 的真实同步状态证明。编译期 Scope 合并不得错误地把两个 Scene 的相同 Type Buffer 合为同一个 VkBuffer；仅共用实际 Global ResourceKey 才允许 GPU backing 共享。

## 8.4 插件合同

插件提供的 Schema canonical name + version + FieldABI/Hash 必须由 Host 复制；不能留下一份指向插件二进制临时内存的 span/string_view。Native callback/Shader specialization/destructor 使用第三方代码时，CodePin 覆盖直到最后 CPU/GPU use。Unregistered plugin type 的旧 ABI 引用不得复活。Installed SDK 的用户代码不得 include Vulkan private headers 才能描述 Shader/Pass/ResourcePlan。


## 8.5 MaterialGraph 输出与 Native Shader 身份（规范索引：第 30 章）

现有 `modules/function/material/**` 已有注册 Node/Pin/Payload+ShaderIR，`engine/toolchain/material/**` 已有 ShaderIR→GLSL→SPIR-V 与 Cooker，而 Legacy `GraphMaterialData`/`MaterialResources` 已有 GPU param/texture + graph variant bucket。**必须将这三层贯通，不得将 MaterialGraph 直接当 Effect 的 RenderGraph，也不得为不同材质参数重复编译同一个 PSO**。材质 Node 插件与 RenderFeature 插件有不同 code pin/ABI 作用，Node Type 与 Render Runtime ID 不混同。Shader 结构修改需重编候选，参数值更改只更新 GPU records；Alpha/DoubleSided、shader code、Owner/Layout 为 exact Native compatibility。Shader toolchain 的 Vulkan-only SPIR-V 编译路径尚非 DXIL；未实现功能见第 30 章。

---

## Source: 09_NativeCompiler与Executor.md

# 09 — Native Compiler、Context、Executor 的职责边界

## 9.1 只有一个执行流水线

`RenderBackend` 拥有合法目标/Effect attachments 与一个 FrameLoop；Native Compiler 消费 GraphIR/ResourcePlan + selected Native Session + Target signature，产生唯一 DeviceSession 中的不可变 `vulkan::Graph`。`GraphExecutionContext` 持 per-attachment/Target variant 的 transient、descriptor instances、FIF slots、history import states、GPU accepted tickets；`Executor` 是录制/提交的算法，不能拥有另一份 Scene/View/Effect 权威。

```text
Neutral Effect / GraphIR + ResourcePlan + Target output declaration
 → Backend Native Port (one implementation selected at bootstrap)
 → Vulkan C9 Shader/Layout, C10 Resources, C11 Recipes, C12 Queue/Sync, C13 Assembly
 → immutable Graph code (per selected DeviceSession)
 → attach: PreparedBindings & RAII resource leases
 → Executor executes native passes in a valid order
 → final Target output / Present / explicit Readback
```

## 9.2 访问最窄的 Context

`InstanceContext` 归 Native bootstrap；仅用于 Instance/Validation/Adapter enumeration；`DeviceContext` 含 VkDevice/VMA/Queue/enabled caps；`ResourceContext` 提供需要的 native pools/staging/transfer service。创建 `MeshStorage` 仅要 ResourceContext 时不应依赖整个 RenderBackend；编译逻辑 IR 不应看到 Vulkan Context。DeviceSession 是设备会话持有者，不成为 UniversalManager；必要 Native Service 只在真实消费者出现时添加。

## 9.3 当前 P2-A 事实与待整改

P2-A 已把 Native owner 移进 `DeviceSession.hpp/.cpp`，但仍让 `InstanceContext` 成员在 Session 内，`DeviceSession` 位于 `render/runtime` 并由公共 assembly 接缝暴露 Vulkan 类型，旧 `ExecutionJob` 仍留在 `Objects.hpp`，这**不是最新目标模型**。后续只迁相关 Owner，不重写已确认正确的 R4 RAII、C0–C13、Sync2/Submit/LastUse、Queue/address stable 算法；先做小型真实 native consumer，再删除旧重复入口。

## 9.4 资源和同步

Native Storage 实际 backing 与 transient backing 分别由 ScopeResources 或 GraphExecutionContext 唯一拥有；`PreparedBindings` 是已保证寿命的直接引用/descriptor，不能再次声明 Buffer Owner。资源 Ready/LastUse 与 Queue Ticket 是 Native owner 的真实事实；CPU Lease 释放不可让已提交的 VkBuffer 立即析构。任意 Partial Submit Failure 无法构造完整 Join Ticket 时，保留所有已接受 work 到安全 teardown，而不是虚构成功凭据。

## 9.5 执行效率

资源 ID/Type/Key 到 backing 的解析发生在 attach/rebind；CommandBuffer Record 跳过多余 Pipeline/Descriptor bind，不能跳过真实 Alias/Queue hazard。Native `Graph` 跨多 View 使用同一 PSO/Shader，不复制代码；slot 数按实际附加并发增长，在冷态分配、热态固定分配量为零；同一 GraphId 可在非相邻的合法 Target 工作段执行，不做会破坏 Target 依赖的全局排序。

## 9.6 Native Provider / D3D12

Client、Engine/Editor、neutral backend、Feature authorship/GraphIR 不包含 `vulkan/vulkan.h` 或 DirectX headers。Native Provider 只需要在启动/编译/资源/执行/目标边界提供真实具体实现，不创建第二个 RenderBackend、第二个调度器或 Native Feature Proxy。D3D12 对相同中立接口独立实现 Device/PSO/DXIL/RootSignature/Resources/Barrier/Swapchain，与 Vulkan Shader Output 不混用。详细边界见第 25 章。


## 9.7 已有 Native 快路机制与真实 UPLOAD Ready

Legacy Native Compiler 还包括: Dynamic local-read merged rendering scopes、Load/Store/Conditional 证明、reused bind recipes、Kernel compiled execution program、cross-queue submit templates、VMA lazy/transient pool 与完整 Resize Candidate，必须按第 29/32 章纳入 R3-C 的真实验收。当前 R4 C0–C13 已具备的部分优先复用，禁止把 Legacy 的错误 split-barrier/全局固定 DS 号恢复。

Mesh/Texture 的 CPU Payload 通过 R2 UPLOAD（单独的 byte budget）进入 Native Transfer Service。Dedicated Transfer Queue 与 aliased Graphics Queue 使用不同提交路线，但均由唯一 R4 SubmissionQueue/Ticket 给出 GPU ready 事实。PreparedBindings 只有在真实 readyTicket、Queue family、layout 与 memory barrier 完整证明后，才能供 Graph 使用；不能因为 Upload Reply 已提交就提前声明资源可读。精确方案第 31 章。

---

## Source: 10_SceneViewTarget与执行分组.md

# 10 — Scene/View/Target 与 Effect Attachment 执行关系

## 10.1 中立对象

Scene 真正拥有 View（每 Scene 局部生成式 ViewId）；View 保存 Camera 与 view-only logical facts，不存 EffectId、GraphId 或 TargetId。Target 保存 offscreen/surface、颜色深度接口、format/samples/extent/placement rules，但**不保存第二张可编辑的 EffectBinding 列表**。Backend 唯一持 `EffectId → [(SceneId,ViewId,TargetId,placement,...)]`，并以生成式 ID 校验三元组与 Runtime owner。

## 10.2 attach 与派生计划

`createView(scene)` 不隐式创建特定 Effect、Native Graph 或针对某个 View 的全量 GPU 资源。`attachEffect(effect,view,target,placement)` 在本帧合法 owner-thread 完成编译需求检查、ScopeKey 资源租约和 PreparedBindings，最后发布关联。`detachEffect()` 只移除指定关系；删除 Scene/View/Target 时按引用关系撤销/拒绝、GPU 退休，不扫描/写入反向字段修补状态。

**不引入长期 ExecutionJob/GraphViewGroup 两套可编辑类型。** 如果实际 Record 性能需要有序数组，Backend 可冷态构造值型 `PreparedSchedule` 或直接按 Attachments 组织有序记录，唯一可编辑使用关系仍是 Effect Binding。相同 GraphId 可以在合法序列中的多个非连续位置出现，不能为减少 PSO binds 改变 Layer/Target 的 producer-consumer 顺序。

## 10.3 多 Effect → 一个 Target

两个 Effect 渲染同一 Target：Compiler 生成本 Target 的完整合成候选，Effect 的子逻辑图先经过 namespace remap，按 explicit layer/blend/dependency 合并 Pass 与最后 Composite/Resolve。这个组合 IR 不成为另一份用户可编辑 RenderEffect/逻辑 Graph；Native 编译产物由 Session 内统一 code 容器持有。Effect 更新影响该 Target 的全体组合消费者，必须按闭包一次安全发布而非半切换。

## 10.4 视图与历史状态

View Camera 可以是 View scope 数据，供不同 Effect 读取同一视角。但 Temporal/TSR/HZB 等历史若依赖 Effect/Target/Render Scale/Shader History ABI，必须用 Attachment-private 作用域或显式相容性证明，不仅通过 ViewId 推断可复用。Resize、camera cut、jitter/DSR/Effect ABI changes 必须改变 epoch 或正确 fallback。Full final output 以及 imported another Target completed image 必须声明 GPU ready/access state。

## 10.5 本期限定

一个 RenderBackend/一个 DeviceSession/一个 GPU；支持多 Scene、多 View、跨 Effect/Target 关联，但 P2-A 当前真实 GPU 业务只覆盖固定 offscreen readback；WSI、resize、complete multi-effect Target、temporal histories 与 Legacy UI 仍待后续 P3/L stage，不能宣称已具备。独立帧节奏 10Hz ECS + 144Hz Render 的 R2 合同继续保留。

---

## Source: 11_插件SDK与扩展边界.md

# 11 — 可安装 SDK、插件 Feature、DataProjection 与 CodePin

## 11.1 一条扩展路径，不改核心业务 switch

插件可贡献：neutral DataSchema/PassSchema；RenderFeature 作者配置与构建 Ops；Engine-side DataProjection（观察自己的 ECS Component 或 Editor 数据）；可选 Native storage Factory/特殊 Shader/DeviceCaps fallback；版本化 Operation/Widget 数据。宿主将这些注册为完整的、冷态验证的结构合同，新的 Feature 和新数据类型无需修改 Renderer 的 `switch(FeatureType)` 或硬编码 Proxy 表。

## 11.2 DeviceSession 不会泄漏到 Engine

Feature build 在 neutral graph/requirements 上操作，不持 Vulkan Device；在真正设备专属 Native Prepare 或资源 Factory 时才由已选 Native Backend 取得窄的 `DeviceContext`/`ResourceContext`，必要时可借 DeviceSession，但不绑定 Scene A 的 Storage 指针到可跨场景的 Effect author。Engine plugin 的 Projection 只依赖中立 RenderRuntime/Schema/Transport，不能 include Vk/WSI/Native private headers。

## 11.3 ABI 纪律

跨 DLL 的 C ABI 必须使用版本化结构 size/capabilities、Host-owned metadata、opaque handles、callback table，不跨 ABI transfer owning STL/RTTI/exceptions。为任何回调、注册 Schema、Native shader kernel、资源析构代码设置真实 CodePin，直到所有调用及对应 GPU/CPU 生命周期结束才允许卸载。Host 会验证 data type canonical name/version/Field ABI，冲突返回错误而不是注册顺序优先。

## 11.4 Installed 插件用例

安装后独立 consumer 新增自有 `GridComponent`、`GridData`、`GridFeature`、`GridProjection`（或 Gaussian 数据类型），无需修改 render backend 源码；真实 ECS 变化触发 typed wire publication、ScopeStorage 更新、attach GPU output readback。插件卸载前创建仍在用的 Effect/Attachment/Native Code/Resource Destruction，卸载后不存在可调用的 DLL 代码悬挂。外部编译与运行要实际测试，不以 Header-only 示例冒充 SDK PASS。

## 11.5 数据发布有权威

同一 DataSchema 允许不同 Producer 类型（ECS、Editor UI、外部 robot source），但**同一具体 scoped data key**默认一个 writer；多个 writer 需显式 partition/merge/priority transaction，不能默默采用最后到达的包。Project/Scene 生命周期变化会撤销 Producer 关联和相关 Attachment，而不是再注册一套永久监听器。

---

## Source: 12_Legacy功能完整迁移.md

# 12 — Legacy 86 项合同及完整产品迁移

## 12.1 范围一项不减

冻结来源 `a669409a289a6fa4092f21176397795b1cdb7f3e` 的 719 个 Legacy source 保持原样。`appendix/CapabilityMatrix.csv` 精确保留 F01–F36 36 项业务、I01–I44 44 项设施/新增合同、H01–H06 6 项高级方向；`GoldenWorkloads.csv` W01–W25 与 `RequirementsTrace.csv` REQ01–REQ51 的历史 wording/source/oracle 作为来源字段不删除。新列给出 RD3-R3 的 owner/stage/test；并不表示未来功能已 PASS。

## 12.2 业务里程碑

| Stage | 交付闭环（真实 native 输出/数据） | 主要能力 |
|---|---|---|
| L1 | MeshStack、Forward GPU-driven、Deferred GBuffer、StandardMaterial、DepthPrepass、Spatial Cull、GPU Skinning、RenderCluster/Picking、Camera Projection | F01–F03, F06–F07, F11–F14 |
| L2 | Clustered Lighting、Scene Light provider、PCF/EVSM/CSM/Mesh Shadow、HZB、Tonemap/SSAO/LinearDepth/Fog/Highlight/Skybox | F04–F05, F08–F10, F15–F20 |
| L3 | Grid2D/3D、Gizmo lines/triangles、Canvas2D、Terrain/Water/Trajectory、StreamingFeedback、PointCloud Simple/GPU-driven/LOD/SoftSplat/Transient、GPU Point/Trajectory storage | F21–F36 |
| Product | Editor/Player/Toolchain/installed SDK、完整 WSI/Offscreen/multiple Views/Targets、所有 F/I 实际 consumer | F01–F36 + I01–I44 |
| H1–H6 | 虚拟几何、动态 GI、TSR、VSM、复杂 BSDF/体积、真正 3DGS；分别做质量、稳定性、可比性能资格 | H01–H06 |

Legacy PointCloud 的软 splat ≠ Full 3DGS；`Graph` 能表达一个 pass ≠ 已实现高级质量算法。不能把 Grid/Canvas/Trajectory 全部降为 MeshInstance，也不能把 GPU-only dynamic data 迁回 CPU 每帧全量重建。

## 12.3 每类迁移最小证据

检查 frozen Legacy 的 Shader ABI/GPU layout、内存分配和 Dirty 策略、增量上传、Scope/Producer、RenderGraph Pass/SideEffect、Ready/LastUse、Plugin Pin、性能负载。先写一个新的具体 Storage Consumer，沿用已证明的算法；再由 Source→Projection→Schema→ResourcePlan→Attach→Executor→Target 做真实 Pixel/Numeric Oracle。保留旧报告中的 regression/partial 样本，不用删除失败历史为新版本通过。

## 12.4 不强制复刻 Legacy 的所有类

旧 `RenderContext` 总服务对象、Scene Feature 安装、Feature `onFrameBegin` 虚生命周期、业务 Proxy/RenderSyncStage、`ResourceRegistry` 哈希身份表不自动继承。旧 `GPUBuffer`、PagedGpuStream、Arena、Bindless、PointCloud Chunk、Transfer/Staging、Deferred retirement 等算法保留并与新 Context/ScopedResources/Attachment Ticket/Output 声明对接。第 23 章和 LegacyGpuAlgorithms.csv 为逐类迁移最低要求。


## 12.5 不能遗漏的 RenderGraphCompiler / MaterialGraph / 专用 Upload 迁移

第 29 章的 Graph compiler 快路和同步正确性负例、第 30 章现有中立材质 Node Graph/Shader Cooker + Legacy GraphFamilyGPU、第 31 章大体积 UPLOAD/专用 Native Transfer Queue、第 32 章 Transient Physical Pool 现在都是正式业务迁移输入。把它们列入 R3-B/R3-C/R3-D/R3-E/L1–L3 的跨阶段验收；不是仅等到 H5 才考虑材质图，也不是仅靠旧 R2 `UPLOAD` Lane 存在就算完成 Mesh/Texture 大上传。

---

## Source: 13_先进渲染兼容性.md

# 13 — 高级渲染 H1–H6 架构能力，禁止“预支持”冒充算法

**目标是允许未来实现类似 UE 的真实效果，不是要求立即复制 Unreal 的 Renderer，也不是提前实现每个高级算法。** Native Graph 具备 Typed Image/Buffer/Pass/Producer/History、GPU feedback、conditional fallback、Queue/Sync2、import/export、virtual page/backing 与 Shader capability 协商接缝；实际消费者出现时再增加 Mesh/Task/RayQuery/RTAS/IndirectCount 等 Native primitives。所有 H 阶段必须有独立质量与性能参考测试。

| Stage | 业务算法与必要数据 | Graph/Storage/Native 机制 | 明确失败/退化判据 |
|---|---|---|---|
| H1 虚拟几何（Nanite-like） | Cluster hierarchy/HLOD/mesh page metadata、GPU LOD selection、HZB cull、反馈与 streaming | Global compressed geometry pages + Scene Instance, GPU page request/readback/ready, indexed/indirect (Mesh Shader optional) | 缺 page 有 parent coarse fallback、无 stale mesh addresses/VRAM overflow；不能把普通 LOD 叫 Nanite |
| H2 动态 GI/反射（Lumen-like） | Screen trace + scene radiance/probe/SDF cache、temporal denoise、可选 hardware RT/RayQuery | 3D/Cube resources、Gpu scene caches、multi-pass compute、history、DeviceCaps/RT AS 或纯 software fallback | 动态几何/光源下有效 GI/reflections 与时域稳定；不能仅 SSR/AO 冒充 Lumen |
| H3 TSR-like | Motion vectors、jitter、depth/reactive mask、history/cut invalidation、dynamic render scale | Attachment-private/history ping-pong 与 graph explicit reprojection/accumulation, 目标相对尺寸、output temporal validity | Ghosting/flicker/disocclusion/upsample reference；纯 upscale blend 不是 TSR |
| H4 虚拟阴影（VSM-like） | Shadow atlas/page table、feedback/page request、clipmap/LOD/dirty invalidation、cache | Global/Scene shadow page storage、Texture subresources、Graph shadow pass and GPU feedback/transfer | 遮挡/动态几何和 page 缺失 fallback，页超额不产生错误图像 |
| H5 BSDF/体积 | Material families/advanced BRDF、3D volume density/light、multi-owner shader | GPU full binding ABI、3D images、bindless tiers/fallback、Compute/Graphics integration | 不支持能力明确 fallback/拒绝；energy/quality comparison 与 volume slicing |
| H6 Full 3DGS/Mixed | Gaussian mean/shape/SH、GPU projection/frustum cull、tile binning/sort、alpha compositing/transmittance、mesh depth/geometry mix | 专业 chunk/LOD storage、GPU indirect/sort/raster stages、streaming/history/target compose | 与参考 splat output 数值/视觉对比；SoftSplat 点云不等于完整 3DGS |

## 13.1 底层合同如何支撑 H

- ResourcePlan 明确 GPU-only/Scene/Global/Attachment resource，Native Backing 可按实际 GPU Budget 管理 page、atlas、demand-driven residency；不为复杂算法造第二数据所有权域。
- Feature 集合可提交多个 compute+graphics passes，但 Shader stage/Native queue feature 支持必须真实启用；GPU indirect args and buffer subranges 由 Graph Use/hazard analyzer 确认。
- 对时间算法 history 正确失效与 target-size specialization 必需；复杂 shader flags/layout/Target format 独立编译 Native variant，绝不在不同 VkDevice 上共享 Vulkan pipeline。
- 时间帧 budget、gpu page allocation/dirty、native submission/present/CodePin 均纳入实际 GPU ticket 生命周期。

## 13.2 参考资料边界

可参考 Epic 官方 Nanite/Lumen/VSM/TSR 技术文档了解目标机制；**该参考不构成 LuxEngine 功能已实现的证明**。参考网址记录在 `REFERENCES_外部机制参考.md`。未来 H 阶段指定固定版本/场景/图像参考，再评估 UE-style 质量目标；不要直接假定某个 UE 内部实现细节是 Vulkan 必需 API。


## MaterialGraph / GPU Streaming / Graph Compiler 的实际前置条件

类似 UE 的复杂材质系统需要独立 MaterialGraph Node/Pin/ShaderIR/AssetCook 与 Shader Variant/PSO/参数表，不应把 MaterialGraph Node 转为每个 RenderGraph Pass；复杂 BSDF/材质层、Alpha/Depth/Shadow Pass 需要真实变体、稳定 Shader ABI/Node Code Pin。Nanite-like/H1、VSM/H4、3DGS/H6 需要**异步大页/Chunk UPLOAD、页驻留/反馈、Queue family transfer、readyTicket、增量替换和 in-flight retire**，不能把它们放在 PROGRAM scalar lane。Lumen-like/H2、TSR/H3 必须有条件链、Graph 完整时域版本和合规 Pipeline/Barrier、Transient/Lazy Memory 池。详细合同见第 29–32 章。

---

## Source: 14_Cpp20类型和编码纪律.md

# 14 — C++20 代码准则与 API 风格

## 14.1 命名与所有权

所有函数/方法使用 lowerCamelCase：`getOrCreate`, `tryGet`, `getLocal`, `attachEffect`, `prepareBindings`, `createResource`, `retireWhenComplete`。禁止 `get_or_create` 及盲目的旧 snake_case facade。类语义要稳定；`ResourcePlan/ScopeKey/EffectAttachment` 首选值、move-only owner、自由算法，确有资源所有权时才用 class。不要建立 SceneManager/ViewManager/DeviceSessionManager/GraphCacheManager/FeatureLifecycleManager/UniversalResourceStore 或无限 `Impl` 链。下层 `namespace vulkan` 已明确用途时可以用 `Graph/Compiler/Executor` 短名，不重复 `VulkanVulkanGraph`。

## 14.2 身份和容器

运行时 Scene/Target/Effect/CompiledGraph 等唯一 ID 由 `lux-cxx` SlotMap 或 StableSlotMap 分配；每 Scene ViewId 为局部 SlotKey，完整 ViewRef 必有 SceneId。其他引用索引用 SlotKeySparseSet 或连续小 vector，不重新分配同一 ID；不得使用 `std::unordered_map` 做核心对象身份存储、手写 ID allocator、把 hash 作为 runtime ID。Shader/Type canonical hash 作为结构身份可存在，但需保留完整 canonical name 与版本 Collision 验证，禁止把 64 位 hash 直接索引 SparseSet。Gpu Shader 物理 slot/descriptor index 与 C++ 生成式对象 ID 并非同一个语义，不能为了统一容器破坏 StableIndex ABI。

## 14.3 Concepts 的适当使用

```cpp
// 目标草案：Concept 验证工厂形状，不要求所有领域 Storage 继承虚基类
template<class Factory, class T>
concept ResourceFactoryFor = requires(Factory f, const T& existing) {
    { f() } -> std::same_as<RenderResult<std::unique_ptr<T>>>;
    { f.isCompatible(existing) } -> std::same_as<bool>;
};

template<class Feature, class Config>
concept GraphAuthor = requires(const Config& cfg, ResourceRequirements& needs,
                               RenderGraphBuilder& graph) {
    { Feature::describe(cfg, needs) };
    { Feature::build(cfg, graph) } -> std::same_as<RenderResult<void>>;
};
```

`create` 可以使用更窄的 `DeviceContext&`/`ResourceContext&`，不必一律 `DeviceSession&`；Concept 只保证本地签名，Plugin C ABI 仍用版本化固定 ops。不要强制 `MeshStorage`、`TextureStorage`、`LightStorage` 在内容更新 API 上统一成 `update(span<byte>)`；它们只统一 Scope 创建、查找、Lease、BindingProvider 合同。

## 14.4 Error 策略：不使用异常/RTTI，OOM fatal

项目新代码不以 C++ exceptions 或 RTTI 管理运行时状态，不编写 `catch(std::bad_alloc)` 作为功能性恢复路径。Native/Host Heap/device OOM 视作致命资源耗尽，不设计 `try again` / swap to other GPU / 减配恢复流程；可用已有 Fatal helper 记录后终止。**历史 P2-A 曾对 Native OOM 做可重试实验是历史证据，不等于新目标 API 继续承诺 OOM 恢复。** 无效 ID、Schema/Scope mismatch、缺失能力、重复 writer、Budget admission、Control backpressure、编译结构候选失败等非致命合同错误继续使用 `RenderResult`/既有标准化错误；旧有效 Native code/context 在普通可恢复失败时保持 last-good。

不允许给任意 `std::vector`/`std::span` 函数机械打 `noexcept` 并假装所有内存分配都可恢复；遵守项目 Fatal/OOM 及 move-only/noexcept 边界。绝不编写 Runtime 的无界 waiter/`waitIdle` 热态补丁掩盖 Sync2/Queue hazard。

## 14.5 资源所有权和 ABI

CPU-visible ResourceLease 不持裸不稳定 SlotMap value 指针越过 erase/compaction；稳定地址可 `SlotMap<unique_ptr<T>>` 或 StableSlotMap。Gpu RAII owner 只出现一次，Descriptor/Native Buffer/Image handle 借用必须由 lease/ticket 保活；CodePin 覆盖插件资源析构器。跨 DLL 不传 owning STL/RTTI exception；Wire/Shader ABI 与 C++ struct 要显式版本和布局验证。

## 14.6 禁止的补偿式设计

Feature attach 不能自动启动第二个 Worker/FrameLoop；Graph Compiler 不允许新建第二套 Data Hazard Analyzer；Engine 不允许对具体 RenderFeature 执行业务 switch 或新增一个 Proxy 派生类才能投影 ECS 数据；Stable topology FrameRecord 不允许每帧字符串、RTTI/typeid、hash lookup/heap allocation；Native Provider 不能返回 VkDevice 给 Engine 用户。

---

## Source: 15_端到端API示例.md

# 15 — 端到端目标 API：有意区分用户、后端与资源作者

> 下面均为目标伪码；**不代表 P2-A HEAD 已有这些签名或完整功能**。真正实现时优先遵循现有 R2 message/reply，无阻塞本机 C++ direct call 偷穿 Engine 边界。

## 15.1 Engine/Editor 只依赖 Client

```cpp
// 由 Application/Native Bootstrap 完成选 GPU 和 Vulkan/D3D12 Native Provider 装配
// Engine ECS / Editor 只获得 neutral RenderRuntime& client
RuntimeScene scene = await(client.createScene());
RuntimeView view = await(client.createView(scene));
RuntimeTarget target = await(client.createTarget(targetDescription));
RuntimeEffect effect = await(client.createEffect(deferredEffectDescription));
await(client.attachEffect(effect, view, target, placement));
// View 本身不存 effectId/graphId/targetId；Effect 不存 sceneId/viewId
```

上层在应用启动时选择 Native Provider/设备；Engine Rendering、ECS/Project UI 不调用 `backend.assemblyDevice()`、不包含 Vulkan 头。`await` 仅表示使用现有 request/reply 机制取结果，不是新增异步线程 API。

## 15.2 Effect 通过 Feature 形成 Plan

```cpp
struct GridSettings { float spacing{}, opacity{}; };
struct GridFeature {
    static void describe(const GridSettings&, ResourceRequirements& out);
    static RenderResult<void> build(const GridSettings&, RenderGraphBuilder&) noexcept;
};
// ResourcePlan 在 compile 阶段形成；此时没有某 Scene 的 VkBuffer
// 具体 GridData 可以来自 ECS/Editor 插件，Plan 只持 neutral type/key/scope
```

## 15.3 attach 内部使用统一资源 API

```cpp
// Native owner-thread 示例：由已验证的 ScopeKey + ResourceSlotKey 获得 scoped resource
// 具体 public facade 不需要向 Feature/Engine 暴露
ScopeKey scope = resolveScope(attachment, requirement);
auto& scopeResources = session.scopedResources(scope);
auto storage = scopeResources.getOrCreate<MeshStorage>(resourceKey, lazyMeshFactory);
if (!storage) return cxx::unexpected(storage.error());
// acquireLease(*storage) 与 PreparedBindings 一起组装成 Backend-owned Attachment
// executor 不会重复调用 session.scopedResources()/getOrCreate()
```

`getOrCreate<T>(factory)` 只能在缺失时调用昂贵 factory；同 T key 既存但不兼容要 error，不可 silently rewrite；`getOrCreate<T>(std::move(owned))` 表示明确接管完整 Native owner 的另一种形式。这里用 ScopeKey，而不是根据 SceneId 猜 DomainId。

## 15.4 同一个 View 多个 Effect / 私有 History

```cpp
await(client.attachEffect(gridEffect, view, editorTarget));
await(client.attachEffect(tsrEffect, view, gameTarget));
// Camera facts 可在同 View scope；TSR history 按 (effect,scene,view,target,signature) 私有
await(client.detachEffect(tsrEffect, view, gameTarget));
// gridEffect 的 attachment 与该 View 仍有效
```

## 15.5 ECS 与插件 DataProjection

```cpp
struct GridProjection {
    static constexpr auto observations = observe<GridComponent>();
    static RenderResult<void> project(
        const GridComponent& component, RenderDataWriter<GridData>& out) noexcept;
};
// Engine/Editor 插件注册这个 projection，并且只负责中立 typed payload；
// 不知 Vulkan Native Buffer/Graph/Binding 的实际布局。
// `RenderDataWriter` 为目标语义示例：Wire/Owner/backpressure 应复用现有 R2 管线。
```

## 15.6 一 Target 多 Effect 的完整输出

```text
Effect A logical graph + Effect B logical graph + Target layers
    → compiler-derived composite graph (no second editable logical owner)
    → Native Graph variant for full Target final output
    → PreparedBindings/Resources/ExecutionContext from all participating attachments
    → ONE native output operation; WSI present follows its valid GPU completion
```

## 15.7 设备作用域

```text
Vulkan bootstrap:
  owns InstanceContext; enumerates physical GPUs; selects one
  constructs DeviceSession borrowing InstanceContext (one selected logical device)
  adopts native session through neutral backend port
Server RenderBackend:
  owns scenes/effects/targets/attachment triples, never gives Engine a VkDevice
```

---

## Source: 16_性能模型与可比实验.md

# 16 — 性能纪律：少热态查找，保留成熟 GPU 存储

## 16.1 新模型热态预算

完成 attach 后，执行态资源槽是预验证的连续 Native Binding 数组，C++ FrameRecord 不重新查 ScopedResources、遍历 TypeRegistry、创建 Descriptor Layout/PSO 或重新编译 Effect。冷态允许有界编译/候选/资源扩容；Session GPU backing 并发容量按实际 attached Work × FIF 配置，而非启动上限 `maxViews×FIF` 给每个变体预分配。固定 topology 首方 C++ heap allocations=0 是目标，需要用实际执行测试检验。

## 16.2 Legacy GPU Storage 的继承门禁

| 存储 | 最有价值的基线指标 | 不能靠简化结构牺牲 |
|---|---|---|
| Mesh/VBO/IBO | arena fragmentation、段数/VRAM、分配与释放时间、GPU data correctness | VMA TLSF/ChainedSegment、StableRecordPages |
| Instance | 0/0.1/1/10/100% dirty、上传字节/次数、dense active slots、多个 View cull | 分流 Dirty、coalesced staging，稳定物理 slot |
| Texture/Bindless | mip residency bytes、wanted feedback、区域更新字节、slot reuse latency | mip feedback，GPU-safe generation slots，region batch |
| Material/Light/Skinning | per-FIF descriptor patch、GPU data revision, native output oracle | 类型化 update+abi，不强制重复传输 |
| PointCloud | chunk upload/delete/reset、octree LOD、GPU alias/VRAM、命令顺序 | chunk pooling、single ordered commands、false invalidation 不覆盖新写 |
| Transfer | ring capacity/overflow、copy coalescing、queue wait/submit、barrier correctness | Staging ring/queue Ticket、no duplicate Transfer owner |

记录 Raw conditions: Windows GPU/driver/caps、device memory budgets、FIF、target resolution、Shader variants、冷/热条件、P50/P95/CI/VRAM，保证与 Legacy 比较的是相同行为、相同输入规模、相同 GPU 输出。只读历史 p95 regression 数据，不要求每个架构阶段重跑旧 5% CI。

## 16.3 编译与执行优化证明

Compiler ResourcePlan 去重与静态 scope slot 减少 hot type lookups；Compiler hazard/alignment 提高允许 alias/overlap 利用率；Engine Projection O(changed) 减少每帧完整 ECS 扫描；Graph grouping 仅合法相邻工作排序。所有“提升”在真实消费者存在前只是设计假说；禁止声称没有测过的提升已达成。

## 16.4 产品阶段

L1–L3/产品收口时重新建立新的基线并与 Legacy stable workloads 比较，特别是 1080p/4K、多 Scene/View/Target、多大 Buffer、真实 GPU Validation/SyncVal、Plugin、场景切换和 VRAM 峰值；历史退化无法用降低 workload/删除测试获得 PASS。

---

## Source: 17_完整验收与正确性矩阵.md

# 17 — 以影响为单位的验收，而非机械 QA 膨胀

## 17.1 硬门禁

每阶段：检查远端 HEAD/受保护文件/用户工作区、强 ID/唯一 owner、结构性错误回滚 last-good、真实 GPU 数值/像素与 Native SyncValidation（相关时）、Stop/Drain/已接受 submit 真实退休；I 含生产/测试变更，独立 clean clone V 只新增验收报告并 STOP。普通内存 OOM 和 Native OOM 不做恢复资格；非 OOM 业务错误/DeviceCaps 不合法仍需要清晰 RenderResult。历史 Frozen Legacy 719 源文件绝不改写。

## 17.2 最小阶段表

| Gate | 本阶段必须真的可运行 | 不用机械重做 |
|---|---|---|
| R3-A | 仅一份 Attachment authority + native Session 生命周期/Scope/lease 接缝，两个 Scene/四 View/共享编译图；真实 GPU in-flight detach 和非 OOM rollback | 旧全部 F4/F5/F6.b perf CI 或全量 36 F |
| R3-B | Effect-owned Graph/Features → Compiler ResourcePlan → attach native binding → fixed hot execution；源/ScopeKey/ABI 冲突负例 | 完整高级算法、所有专业 Storage |
| R3-C | 单 Target 多 Effect 的完整 Graph、WSI/offscreen、History/cut/resize、Queue/sync/fallback | 尚未开发的 UE 风格算法质量 |
| R3-D | 真正 Root Mesh/Texture、Scene Instance/Light、View/Attachment storage，Legacy 优化数据布局与 GPU outputs/Dirty benchmark | 不相关旧微基准反复重复测量 |
| R3-E | ECS/Editor plugin DataProjection → R2 Protocol → GPU data → native pass，背压/退出、installed SDK、CodePin | 其他插件类型的理论编译 |
| L1–L3 & Product | F01–36 + I01–44 的全部真实 GPU/产品消费者和 W01–25 对应 oracle | 不把小三角机制测试算业务全部 PASS |
| H1–H6 | 各先进算法独立质量/性能/兼容 fallback/GPU reference | “Graph 可表示”不算算法 PASS |

## 17.3 不能漏掉的 12 类反例

1. 同一个 View 同时属于 E1/T1 与 E2/T2，Attachment-private History 不串。
2. 两 Effect 同 Scene 共用 Scene LightStorage，先 detach E1 不释放 E2 使用的 Buffer。
3. Root MeshStorage 被两个 Scene 使用；其中一个 Scene 删除不销毁 Root 资源。
4. Effect attach 后出现 ABI/schema conflict，旧 Attachment/Native Graph 和 Target 保留完整 last-good。
5. Shader 只有 StorageBuffer 声明但 Feature 没写 Scope/Writer，Compiler 拒绝，而不是猜 Global。
6. 未使用的 Effect 创建不分配 Scene/View GPU backing；显式 Asset preload 合法。
7. 删除 Scene 连带撤销所有 View/Attachment，但旧 Native GPU Submit 按真实 tickets 完成后才释放。
8. GPU partial submit 后失败不得假装 Full join；已接受的工作在 Native teardown 时安全等待或 Device Lost 终态。
9. Target 两 Effect 混合，最终像素来自一个明确的 Graph Composite；不会隐式第二个 renderer。
10. ECS Grid Component 更新、销毁、补发时，Projection 通过协议保持 revision 和 backpressure，不漏取消/删除。
11. Plugin 新增中立 data+Feature+Projection、不修改核心 RenderSystem switch，卸载时 CodePin 合法。
12. Native Vulkan ↔ Native D3D12 未来接缝在 neutral Client/Schema/IR 不带 Vk；当前只验证 Vulkan 原实现，不宣称 D3D12 实装。

## 17.4 持续状态记录

必须明确 `PASS/PARTIAL/FAIL/NOT_RUN/NOT_IMPLEMENTED`，附 code SHA/source, I/V, GPU/driver, exact actual tests, old vs new oracle, performance context。原 RD2/RD3/P1/P2-A 历史资格与新设计决策分别记录；不能把已通过的旧 P2-A Native OOM retry 变成新“允诺支持 OOM 恢复”，也不能改写历史失败记录。

---

## Source: 18_阶段计划与交付门禁.md

# 18 — RD3-R3 从 P2-A PASS 继续推进的分期方案

> **本设计包不自动授权任何生产阶段。** 当前 HEAD `960e755...` 的 P2-A 是已实施、经实施方独立 clean clone 有限验收的代码；本章提出下一阶段的目标工作单。用户批准后，一次只执行一张，不回滚历史 R0/R1/R2/F0–F6.b 或 RD3 P1/P2-A Git 证据。

## 18.1 建议的顺序

```text
已实现 RD3-P1 → 已实现 RD3-R2 P2-A (报 PASS，范围有限)
  → R3-A [Attachment 使用权 / ScopedResources、InstanceContext 所有权与接口收敛]
  → R3-B [Effect-owned Features/Logical Graph + Compiler ResourcePlan + attach binder]
  → R3-C [Native final Target, multi-Effect composition, WSI/history/resize]
  → R3-D [Legacy 高价值 GPU Storage 逐业务纵向迁移 + Dirty/VRAM]
  → R3-E [Engine/Editor/ECS DataProjection、Plugin SDK、native backend port]
  → L1/L2/L3 [全部 Legacy 业务实际迁移] → Product [完整产品资格]
  → H1–H6 [独立 UE-like 高级渲染算法和质量资格]
```

### R3-A：不继续增加第二个资源生命周期架构

核心交付是统一的 Effect 使用关系中的 move-only attachment 生命周期、内部 Session ScopeResources/Lease 与冷态准备执行接缝；修正 InstanceContext owner 为 Native bootstrap（真正 outlive Session）；消除永久 ExecutionJob 第二权威；保留 P2-A Scene/View/Effect/Target 与 Native Graph 共享/actual in-flight safety。先拿一个真实 Buffer 证明 Root/Scene/View/Attachment scope acquire/release/retain，再扩大。无需完成全 Feature SDK/所有 GPU 资源。

### R3-B：Effect/Compiler 的 ResourcePlan

Effect-owned logical Graph + concrete Feature authoring；Compiler 给每个 GPU 资源需求生成可验证的 ResourcePlan，attach 后绑定 Session Storage/Native resource slots，结构更新对全部受影响输出目标整体 last-good。1–2 个真实 Feature（如 Grid/Tonemap/Light）完成端到端；不能将 Shader 声明当成 Scope 来源。

### R3-C：Target/History/Native

单 Target 不同 Effect 的完整 Graph 输出、真正 Offscreen/Surface/WSI、resize/camera cut/history、多个 Target 之间显式 dependency、true Queue Ticket，非最后的隐藏 Tonemap/Composite。此阶段也完成 native/compiler 固定阶段和后端中立边界的必要迁移，不要求 D3D12 实现。

### R3-D：专业 GPU Storage

先 Mesh/Instance/Texture/Material/Light 的高价值路径，之后 PointCloud/Canvas/Terrain/Water/Trajectory 等由 L1–L3 实际消费者推动。每类用已有 Legacy 真实 source/layout/Dirty/arena/bindless 实现作为可比基准，保留核心优化并按新 Session/Scope/Lease/Binding API 改名与重构。不得无消费者新写一套 UniversalGpuStorage。

### R3-E：Engine DataProjection 与插件

Engine 自有 ECS/Asset/Editor 数据通过注册型 Projection + neutral DataSchema + R2 PROGRAM/UPLOAD 投影至 RenderBackend，不改核心业务 switch；Plugin native optional hooks 与 CodePin/Installed SDK；Provider 装配与 Native Session 接缝无 Vulkan leakage。只需一个真实 Grid + Mesh + 插件 Generic Data 集成证明，业务范围留后续 L 阶段。

## 18.2 工作单纪律

每张 work order 明确 ALLOWED / READ-ONLY / FORBIDDEN 与真实模块依赖影响；不强迫每次跑全量旧 perf，按安全/GPU/实际变化做最小充分测试；I/V 独立，V 只增加报告，Push then STOP，用户再授权下一阶段。不以行数或 3,000 个机械检查替代真实源码所有权审查。

## 18.3 原历史能力要求

`F01–F36/I01–I44/H01–H06`、`W01–W25`、`REQ01–REQ51` 只保留/扩展，不能删除。历史 performance PARTIAL 作为证据继续存档；即使新架构未完成 Legacy 业务，最多只能标阶段 PASS（限定），不能误标产品/高级功能 PASS。

---

## Source: 19_RD3R3统一架构裁决.md

# 19 — RD3-R3 唯一裁决表（本包内部统一规范，待用户批准）

| Decision | 设计终态 | 明确拒绝 |
|---|---|---|
| R3-D01 | `RenderBackend` 唯一 Scene/View/Effect/Target 关系与单 FrameLoop | 第二 renderer、第二事件总线、Scene 默认 Renderer |
| R3-D02 | Scene 持局部 View；View/Effect/Target 不相互持可编辑反向身份 | View.effect/graph/target、Effect.scene/view、Target.view |
| R3-D03 | Backend 唯一 EffectId → SceneId/ViewId/TargetId Attachment 使用关系，attach RAII | EffectBindings + ExecutionJob + GraphViewGroup 多表同时权威 |
| R3-D04 | Effect 拥有逻辑 Graph 与 Feature Author，Shader 实施 GPU 算法 | Effect=Shader/独立逻辑 GraphCache、Feature 默认 GPU Owner |
| R3-D05 | Feature/DataSchema 显式声明 Scope/Key/Provider/ABI；Compiler 派生 ResourcePlan | Shader 读取 SSBO 即推断是 Global/Scene |
| R3-D06 | attach 校验 + lazily acquire scoped resource leases + Native PreparedBindings 才发布 | Scene 创建时强制全部 Resource Storage 分配 |
| R3-D07 | Scope 选 Global/Scene/View/Attachment，Graph transient 由 GraphContext 管 | 必须有四个 manager、ViewId 独断 temporal history |
| R3-D08 | Session ScopedResources 为唯一持久 GPU backing owner；Attachment 持使用权 | ResourceDomain 与 ScopeStore 两份 Native 资源权威 |
| R3-D09 | ResourceDomainId/ResourceDomain 作为 P2-A 过渡实现允许，非冻结公共中心 | 为了维持旧名额外再设 ScopeManager/Adapter 链 |
| R3-D10 | `getOrCreate<T>()` 只 cold 本地创建/命中、Config compatibility；父 scope 只读 | `get_or_create`、热态 per-pass Registry 查找、隐式覆盖父资源 |
| R3-D11 | 资源唯一 Native RAII，CPU Lease release 后须真实 ticket/WSI/pin 再销毁 | CPU Scene 析构立刻 vkDestroy，FrameSerial 代替 GPU completion |
| R3-D12 | 一个 DeviceSession 初始化选择 GPU，不做多 GPU 同帧；没有 DeviceSessionId | 预制 sessions registry/multi GPU dispatch |
| R3-D13 | Native bootstrap 拥有 InstanceContext，且 outlives Session | DeviceSession 默认 own InstanceContext / global singleton VkDevice |
| R3-D14 | DeviceSession 拥 DeviceContext、ResourceContext、Native Graph/contexts/storage；仅一层 Native Port | 巨型 VulkanBackend、第二个 Graph executor、Native 头泄漏 Engine |
| R3-D15 | Graph C0–C8/C9–C13 固定已有算法、Shader ABI/Sync2/Queue 真证据 | 第二 parser/hazard/pass scheduler、任意 compiler pass manager |
| R3-D16 | Native compiled Graph 不可变可共享，View/Scene 数据在 PreparedBindings 分离 | 每 View duplicate Shader/PSO、hash/EffectId 冒充 GraphId |
| R3-D17 | 执行时使用 indexed bindings；热态稳定零无谓 allocation/lookup/recompile | 每 pass getResourceDomain/type hash lookup |
| R3-D18 | Multi-effect Target 派生单次完整 final Graph，不创建第二可编辑图 | Hidden postprocess/compositor Native graph outside declaration |
| R3-D19 | Effect/Target 结构更新闭包候选整体发布，普通错误 last-good | 部分消费者提前切换；Effect revision 当 GPU Ticket |
| R3-D20 | Engine/Editor/ECS 经中立 Client/R2 Protocol；数据生产源以 DataProjection 注册 | Engine include Vulkan、核心 RenderSystem 业务 switch/每 Feature Proxy 虚基类 |
| R3-D21 | DataSchema、PassSchema、Projection 同源/版本/类型权威，Wire、GPU ABI 各自验证 | 任意 ECS struct memcpy GPU/wire、插件跨 DLL STL/RTTI |
| R3-D22 | 插件 CodePin 覆盖 callbacks/Native code/resource destructor 最后使用 | GPU 在途时卸载 dll/unowned callback |
| R3-D23 | lux-cxx SlotMap/StableSlotMap/SlotKeySparseSet for runtime identity | unordered_map 对象 ID 权威、独立 Handle allocator |
| R3-D24 | Legacy 成熟 GPU 存储算法价值保留，新接口/作用域/生命周期重设计 | 无依据 GenericGpuStorage 取代 Paged/Arena/Bindless/Dirty |
| R3-D25 | 当前 OOM=致命终止；非 OOM 可恢复结构/支持性错误使用 RenderResult | 继续将旧 Native OOM retry 当成新公共 API 保证 |
| R3-D26 | 保留 UE-like H1–H6 能力扩展接缝，实际算法各独立证明 | 以 API 能表达就标记算法/产品 PASS |
| R3-D27 | 一个 I + independent V + STOP；保留历史 docs/perf/legacy 719 | QA 机械数量成为阻断、修改历史证据 |
| R3-D28 | 目录遵循明确中立/Native DAG，迁移当前 Runtime/Vulkan C++ 而非空目录 | 为每个 header 建单独 lib、长期 old+new 双权威 |

## 19.1 决策等级

本章以用户已经明确的语义为首要原则，**具体 `ScopeStorage`/`AttachmentLease` 的 class 名称与容器实现仍属实现提案**。C++ 文件结构及 ABI 要通过真实消费者与安装后公共头闭包确认；不能擅自推断此设计已经通过编译或 GPU qualification。任何必要调整都需同步正文、类型表、API、stage/work order 与一致性脚本，不得只加一篇 PATCH。


## 19.2 Legacy Graph/Material/UPLOAD 专项补充裁决（必须同时看第 29–32 章）

| Decision | 新增规范 | 拒绝 |
|---|---|---|
| R3-D29 | Legacy Compiler 对 forward-ref/declared external read、starved→dead、conditional/local-read、first/subsequent View、Compiled Recipe 的关键语义须保留或有等价证明 | 只有 DAG/toposort 便称完整 RenderGraphCompiler |
| R3-D30 | Native ResourcePlan 独立管理 Graph transient alias + exact physical pool + lazy memory/resize candidate，GPU in-flight never reused | Scoped MeshStorage/ResourceDomain 代替 transient pool 或每次图重编都重新分配 |
| R3-D31 | 复用已存在的 `material::MaterialGraph`/NodeCatalog/ShaderIR/Cooker；MaterialGraph 与 RenderEffect 的逻辑 Graph 不合并 | 第二套材质 Node 框架、按参数值重复 Shader/PSO 编译 |
| R3-D32 | 语义上明确 PROGRAM/CONTROL/UPLOAD、CPU bounded Upload Jobs 与 GPU Transfer Queue 三层；大 Mesh/Texture 使用独立 UPLOAD 与真实 ready/last-use Ticket | 将 P2-A 只在关闭时 drain 的 Upload 当作已完成，或用另一套不兼容 Native timeline/Queue |
| R3-D33 | Legacy Dedicated/Record-only、精确 Mip/Cube/mesh size、Queue-family ownership、线程源数据 Code/Byte Owner 保留，必要 GPU transfer worker 是狭窄专用服务而非第二 renderer | 把纹理大数据塞 PROGRAM、把 worker 变通用任务池、Queue alias 时并发 Submit |

---

## Source: 20_RD2_RD3R2到RD3R3迁移对照.md

# 20 — 已实现 P2-A 与 Legacy → RD3-R3 职责迁移表

## 20.1 实际仓库状态（2026-10-13 检查）

`codex/render-v2` 已前进到 `960e755395be2c9f78a86339a062c507826c4eb4`（P2-A V），父 I `483d20ed900b8c226a740704f3c888498b1dae11`。真实 P2-A 已做 DeviceSession、三层 Context 结构（但 InstanceContext 在 Session 内）、SlotMap<unique_ptr<ResourceDomain>>、Scene/View DomainId、EffectBinding 三元组、派生 ExecutionJob、GPU native graph sharing；文档报告 Windows x64 43/43 scoped tests PASS、2 Scene/4 View/FIF2/3 validation 0。**我未在本环境重跑这些测试；状态属于报告证据。**

## 20.2 必须调整的真实 C++ 状态

| P2-A 真实源码形态 | 新归属/做法 | 阶段 |
|---|---|---|
| `runtime/include/.../DeviceSession.hpp::InstanceContext` 成员 | native bootstrap owns stable InstanceContext; DeviceSession borrows; actual owner lifetime verified | R3-A |
| `DeviceSession::{domains_, domain_order_}` | 保留 native store 算法为内部 ScopeStorage candidate；把动态需求/RAII leases 绑定到 attachments，最终不再公开 DomainId 做资源权威 | R3-A / R3-B |
| `runtime/ResourceDomain.hpp` typed virtual entries and local `getOrCreate` | 复用 local type ownership 与 factory compatibility，重构成 ScopeKey+ResourceKey、保持 one Native owner/cold API，不追加第二 ResourceRegistry | R3-A / R3-D |
| `Scene.domain`, `View.domain` | 只保留 Scene/View logical facts；GPU ScopeKey 在 attach 根据验证的 SceneId/ViewRef 构造；过渡期按计划删除直接 Domain 依赖 | R3-A |
| `runtime/Objects.hpp::ExecutionJob` | 压缩为唯一 EffectAttachment authority 中的已准备数据 + cold-derived order，不保留可编辑双表 | R3-A |
| `RenderEffect{definition,initial_values,GraphId}` | RenderEffect 统一 own Features + one logical Graph，Native compiled Graph only Session; no second logical cache | R3-B |
| `NativeAssembly/Backend.hpp` 公开 VulkanOptions/EffectSource | 把 Native Factory/Session bootstrap 移到可选择实现的 Native Port，Engine 仅看中立 Client/Wire/Schema | R3-E（最小 API seam 可先做） |
| P2-A `getOrCreate` native Buffer/Factory OOM recovery | 保留 P2-A historical test; RD3-R3 对新业务 API 不承诺 OOM retry，非 OOM conflict/ABI last-good | R3-A |
| Legacy GPU storage & Transfer | 各专业 Storage 迁移保留成熟算法；Use leases + Native last use 替代纯 FIF serial | R3-D / L |
| Legacy `RenderSyncStage` + plugin `RenderFeatureSceneBinding` | 保留 change tracking/prepare-commit-discard/registration/codepin 的算法，变为可注册中立 DataProjection，不让每 Feature 生成 Proxy | R3-E |
| 当前 `vulkan::GraphExecutionContext` | 保留当前 Native storage/descriptor/tickets/slot 管理，绑定真实 ResourcePlan 和 Attachment-private history | R3-B/R3-C |

## 20.3 避免“文档内容变当前事实”

P2-A 不实现：完整 Effect-owned Feature/Graph composition、Compiler ResourcePlan/attach RAII resource sharing、Engine DataProjection plugin、mixed Effect final Target、WSI/history/Legacy 专业 Storage、DirectX、H1–H6。Native Graph sharing 已实测，不等于其它功能完成。下阶段先审核已有 P2-A I/V 的真实 diff 与 Native C++ include/link DAG，再冻结允许改动文件，避免在既有代码上再造一套中间层。

## 20.4 历史来源边界

原 RD3-R2 ZIP `RenderV2_RD3R2_DeviceSession_EffectGraph_Architecture_2026-10-12(3).zip` 的全部 86 F/I/H、25 W、51 REQ 与 16 Legacy GPU 算法追踪作为输入保留并更新；旧 ZIP 不包含本轮讨论最终 Scope lifetime/Projection/Instance ownership 决策。Legacy freeze `a669409a289a6fa4092f21176397795b1cdb7f3e`。原有 I/V 和历史性能 PARTIAL 不可在设计改写中悄悄改判。

---

## Source: 21_Vulkan三级Context与单DeviceSession.md

# 21 — Vulkan 三级 Context 与单 DeviceSession 的正确父子关系

## 21.1 Legacy 的价值与必要修订

Legacy `VulkanContext.hpp` 明确分 `InstanceContext`（VkInstance/debug/扩展）、`DeviceContext`（physical/logical device、VMA、Queue/Caps）、`ResourceContext`（Descriptor/Command pools）；Legacy `RenderContext` 才是过于庞大的渲染全局服务中心。新架构保留**能力分层**，不保留巨型 `RenderContext`、或认为 Context 就是新的 Renderer。

```text
Vulkan Native Bootstrap  [owns lifetime]
  └── InstanceContext (VkInstance, Debug/Validation, adapter discovery)
         └── borrows by one DeviceSession
              ├── DeviceContext (selected VkDevice, VMA, stable SubmissionQueue, enabled caps)
              ├── ResourceContext (descriptor/staging/transfer resource services)
              ├── Session-scoped Persistent Storage / Native Graph programs
              ├── ExecutionContexts / Native targets
              └── GPU ticket-based retirement
```

**InstanceContext 的 owner 在 DeviceSession 外**，且其稳定地址/析构必须在 Session 之后。当前 P2-A `DeviceSession::instance` 是真实已实装但不满足新目标的过渡结构。Native bootstrap 可把 `unique_ptr<InstanceContext>` 与借用它的 `unique_ptr<DeviceSession>` 封装进一个小的 move-only Native Owner Bundle，保证声明/销毁顺序；此 Bundle 是独占装配值，不需要 ContextManager 或 Sessions SlotMap。

## 21.2 多 GPU 的边界

启动先从 InstanceContext 枚举所有适配器，按用户 device policy/UUID/能力/Present support 选择一次；本 RenderBackend 一次只建一个 DeviceSession，不新增 DeviceSessionId，不实现跨 VkDevice Buffer/WSI 隐式共享。底层 InstanceContext/DeviceContext 不使用静态全局唯一 VkDevice，这让将来独立 GPU compute session 成为可能，但这不是当前可交付功能。

## 21.3 只暴露必要能力

InstanceContext 只负责 physical/device discovery/instance caps；DeviceContext 负责 logical device/queues/VMA/Queue ownership，正确处理同一 VkQueue 对应多个角色只实际拥有一个 SubmissionQueue；ResourceContext 提供 Descriptor、Staging/Transfer/Upload/Retirement 的真服务，但不默认直接持所有 Mesh/Texture/Light 业务资源，也不再额外创建与当前 GraphExecutionContext/SubmissionQueue 重复的 CommandPool。

专业 Storage Factory 接受所需最窄 Context：纯 Vulkan Buffer 使用 ResourceContext，ShaderPSO 使用 DeviceContext+Shader ABI，纯 Graph Author 无 Device Context。Native Factory/Session 只能由平台 Vulkan 实现建立，Engine/ECS 不持 Native 指针。

## 21.4 关闭与重建

停止新请求/提交 → Backend worker join/drain → attach use leases release → Native contexts 等真实 queue tickets → ScopedResources/Native Targets/compiled Shader/PSO 释放 → ResourceContext → queues/VMA/VkDevice → Native bootstrap InstanceContext。Device Lost 状态下不能用旧 Vk handles 重建新 Session；需要应用层受控重建 Backend/DeviceBundle，不宣称自动恢复 GPU。

## 21.5 设备选择的可移植呈现

neutral `AdapterSelection` 描述用户设备选择的政策/能力目标，不包含 VkPhysicalDevice；Vulkan Provider 将其解析到当前 Instance 的 physical device。Surface present 必须检查该 device 对目标 surface queue family 的支持，Offscreen 可以选择无 present 的 GPU。D3D12 Device Factory 可以用同一中立意图选择对应 Adapter，实现不同但 Engine ECS 源码不应改动。

---

## Source: 22_统一ResourceAPI与Concept.md

# 22 — 资源 API 一致性不等于资源数据结构同构

## 22.1 资源作者看到的最小接口

用户要求 `getOrCreate<T>()` camelCase。这个 API 应定位于**后端冷态 ScopeResources/Factory**，而不是 RenderEffect 或每帧 Executor 自己查找资源。ScopeKey 由 Backend 在已经确认 Scene/View/Effect/Target 之后创建；用户不必为了写 GridFeature 先查询 DeviceSession 的 DomainId。

```cpp
// 目标语义：class ScopeResources 也可只是 DeviceSession 内部轻量视图
const auto scope = makeScopeKey(ResourceScope::SCENE, validatedSceneId);
auto& resources = session.scopedResources(scope);
auto value = resources.getOrCreate<LightStorage>("lights.main", lightFactory);
// 借用者通过 attachment lease 保活；不能把 value 的 T& 存到 Effect 作者对象中
```

## 22.2 操作语义

| 操作 | 唯一合法语义 |
|---|---|
| `tryGetLocal<T>(key)` | 仅当前已验证 Scope 查找，返回允许范围内借用，不能用于跨资源 Owner 更改 GPU allocation |
| `tryGet<T>(scope,key)` | 在明确设计允许的只读 fallback/scope 查询中提供观察，不通过隐式父链偷改祖先 |
| `getOrCreate<T>(key,factory)` | 本 scope 条目已有时调用 `factory.isCompatible(existing)` 但不调用昂贵的 `factory()`；不存在时构建完成再发布 |
| `getOrCreate<T>(key,std::move(owned))` | 接受已构造好的唯一 owner；已占用必须清晰冲突或保持旧语义，绝不悄悄覆盖 |
| `acquireLease(key)` | 成功 attach/preload 使用权，严格保证 Native backing 在使用者存活期间不被回收 |
| `releaseLease` | 引用减一后依 retention 保留/释放；GPU 物理资源还要按 ticket 退休 |
| `replace(key,newConfig)` | 显式版本化替换事务，不隐含于 getOrCreate 或 per-frame 数据值更新 |

## 22.3 最小 Concept 草案

```cpp
template<class Factory, class T>
concept ResourceFactoryFor = requires(Factory f, const T& existing) {
    { f() } -> std::same_as<RenderResult<std::unique_ptr<T>>>;
    { f.isCompatible(existing) } -> std::same_as<bool>;
};

template<class Storage>
concept GraphBindingProvider = requires(const Storage& s, ResourceSemanticKey key) {
    // Exact return type remains backend-specific; `describeBinding` must return
    // owned/cold metadata or valid borrow of Native backing, not an owning copy.
    { s.describeBinding(key) };
};
```

本地 C++20 约束足以阻止半有效的 Factory 和重复 API；不强制所有 GPU Storage 都实现虚拟 `IResource` 接口，也不要求 `MeshStorage::update` 与 `GridData::update` 同参数。插件 API 通过 ABI-safe callback table，不能暴露 RTTI + owning STL。

## 22.4 Storage 键、兼容性与写权

条目身份 = DeviceSession + ScopeKey + Type canonical name/version + ResourceKey + GPU Layout/Config compatibility。TypeId 哈希必须保留原名 Collision 校验。多同型资源需要 key（如 `Light.main` 与 `Light.preview`）；Config 不匹配返回可恢复的 DefinitionMismatch；类型/Shader ABI 冲突需拒绝，而不是自动增长或调用另一 Feature 的 create()。

Asset 写入和 Attach 使用同一个 Storage Owner：CPU source update 只修改自身授权的 typed slice/dirty log；需要按 GPU-queue time 与真实 in-flight ticket 协调 descriptor/Buffer 替换。已验证的 binding recipe 由 `PreparedBindings` 保存，热态不再执行类型查找或 hash map 解析。

## 22.5 当前 P2-A 代码迁移

P2-A `ResourceDomain::tryGet()/getOrCreate` 的候选构造、命中不重复建立、地址稳定策略可以复用，但那个 Public `ResourceDomain` 类的完整层级树与显式 DomainId 不是最终用户层必须保留的类型。先沿用其 Native Buffer Factory 证明 ScopeResource/Lease/attach，再以具体 Consumer 和 GPU 使用权测试决定如何合并/删除旧 Domain，而不是创建 ScopeResourcesManager/ResourceDomainManager 两层平行权威。

---

## Source: 23_Legacy专业GPU存储迁移规范.md

# 23 — Legacy 专业 GPU Storage：保留高价值算法，按新语义重写

> 来源 `render_legacy/**` 冻结 719 源文件、commit `a669409a289a6fa4092f21176397795b1cdb7f3e`。这些不是 P2-A 已实现的业务功能。执行方必须逐项读源文件、Shader ABI、真实消费者和旧基准；不允许只复制类名/每个 `.hpp`。

## 23.1 底层 GPU Buffer 与分页

| Legacy 源码 | 应继承的具体算法、数据结构 | 新落点及必须改的接口 |
|---|---|---|
| `.../gpu/memory/GPUBuffer.hpp` `TGpuBuffer` | CPU_TO_GPU/GPU_ONLY 条件 policy、可选 Host mirror、Dirty epochs、reused coalescing scratch、非 coherent Flush & Range barrier、按 FIF 增量 | 专业 Storage 内部的 `GpuBuffer` primitive；按 native Queue Ticket 生命周期而非全局 FIF serial，避免双 CPU authority |
| `.../gpu/memory/PagedGpuStream.hpp` | 稳定 page slots、Dirty page coalescing、`runs_scratch_` 跨帧复用 | Instance/Chunk Storage，Hot Dirty 同样 O(changed)；不要每个 Record 注册 Scope entry |
| `.../resources/mesh/StableRecordPages.hpp` | Mesh CPU metadata 的固定 4096-record pages、稳定引用/分块增长 | MeshStorage 的 data layout，不能改为普通 dense SlotMap value 导致裸引用随 erase 移动 |
| `.../gpu/memory/ArenaAllocator.hpp` | VMA virtual TLSF、free/coalescing、live range、defrag copy plan | MeshStorage 内子分配，业务 VRAM budget 与真实 submitted copy tickets 协调 |
| `.../gpu/memory/ChainedArenaAllocator.hpp` | segment 逐步扩展、优先最近/旧空洞回收、候选成功才发布、`uint16` index bounds | Mesh VBO/IBO segmented arena；Segment Native Buffer owner 由 Storage 持有，arena 本身不必 own VkBuffer |

路径 `...` 均以前缀 `render_legacy/modules/function/render/vulkan/sinclude/lux/engine/render/` 为准；精确 Path 见 `appendix/LegacyGpuAlgorithms.csv`。

## 23.2 Geometry/Instance/Vertex Pool

`MeshResources` 中 VBO/IBO segmented arenas、真实 Index format、LOD ranges、GPU ready、CPU bounding box/BVH、StagingOnly fallback 具有实际价值；迁移为 Session Global `MeshStorage`，一次创建供多个 Scene 引用。`InstanceResources` 中 stable `InstanceSlotRegistry`、Transform/PrevTransform/CullMeta/Property streams、dense alive slots、page-relative transform/world origin rebase、per-stream Dirty 保留；迁移为 Scene-scope `InstanceStorage`，多个 View/Effect 共享实例 backing 但独立 Cull/History/visibility results。

`VertexPoolRegistry` 的 bindless vertex source slot 与 registration lease semantics 需继续有效，但按真实 native ready/last-use 和 new ScopeResources 的 attach lease 分离。**GPU Shader 读取的 stable slot index 不必等于 lux-cxx SlotMap 的物理 dense index**；统一 Host ID 不能破坏 shader buffer layout。

## 23.3 Texture、Bindless、Material

`TextureResources` / `BindlessCombinedSet` 的 stable slot generation、GPU-in-flight recycle deferral、mip residency/feedback、persistent region updates、2D/Cube bindings、fallback texture、batch uploads 是一体的内存/正确性优化；Root `TextureStorage` 是唯一 backing，多个 Scene/Effect 可以拿 lease。`MaterialResources` 的 per-family GPU table、shader variant/texture bindless slot、FIF descriptor revision 也要保留，但应让 native BindingProvider 接入当前 generated Shader ABI/Owner Shape，不重新发明固定 set/binding 配置。DeviceCaps 不支持时选择合法的 bindless fallback 或明确不支持。

## 23.4 Light、Skinning、PointCloud、其他专业存储

- `LightResources`：Directional/Point/Spot/Area SSBO、GPU ABI、FIF update、位置页与 shadow data；迁移 Scene LightStorage，实际 data writer 由 Engine LightProjection 负责；别强制 LightDataProvider 成为含空 pass 的 Feature。
- `SkinningResources`：GPU bone palette/预 skin compute/Ready/LOD，禁用每 View CPU skinning 替换真正 GPU pipeline。
- `PointCloudResources` + `PointCloudGlobalBuffer`：Chunk slot capacity reuse、octree nodes、ordered `clear/upload/reset` queue、GPU-safe deferred reuse；专用 PointCloudStorage，不强制套 Mesh record；Legacy soft splat 不等于 H6 真实 3DGS。
- Terrain/Water/Trajectory/Canvas/UI/Gizmo：按实际 Feature/Shader Data 与专业 Streaming/Storage 机制迁移，保留原业务性能特性和 Target final composition 的输出。

## 23.5 Transfer/Retirement

Legacy `TransferScheduler` 的 staging ring/overflow FIF buffers、batch copies、reused barrier scratch、Host/GPU flush；`FifOwned` / DeferredDestroyQueue 保护 accepted GPU work 的本质都值得继承。但是 Native R4 已有 Queue Sync2、real SubmissionTicket 和 retirement，请在同一 Native owner 内融合优化算法，**不能再次引入平行 Transfer Worker 或第二套“FrameSerial 等于 completed”机制**。Buffer/Texture 切换旧 backing 时，CPU logical release 和 GPU last use 分离；Native resource 必须在可审计 ticket 状态中退休。

## 23.6 每项算法迁移证据卡

最少含：`LegacyPaths`、`OriginalDataLayout/ShaderABI`、`OwnerScope/ScopeKey`、`ResourceRequirement/Factory`、`AttachLeaseBinding`、`Transfer/Ready/LastUse`、`RetainedAlgorithmAndReason`、`NewAPIAndWhy`、`GPUPixelNumericOracle`、`Dirty/VRAM/CPU/GPU`、`Affected Legacy CapabilityId / GoldenId`、`ImplementationSHA/VerificationSHA`。只有技术证据说明更简单且效果/性能不退，才可改变成熟数据结构；不能以改名/换容器作为“性能优化”。


## 23.7 RenderGraph transient allocator 和 Material/UPLOAD 不是普通 Storage 的附注

旧 `RGVulkanResourceAllocator` 的 exact-key pool、transient image VMA pool、独立 LazyMemory pool 和跨 Graph 重编/Resize backing 重用必须在第 29/32 章独立验证；这类 Graph transient 不经 SceneScope Registry 的 `getOrCreate<MeshStorage>`。Material Graph 另有稳定 Node/Cook/ShaderIR 工具链、GraphFamily GPU params/texture 和 refcounted Shader Variant Bucket（第 30 章），不能只迁移一个 `MaterialResources` Buffer。Legacy `RenderUploadClient/Session`→bounded bytes UploadQueue→`GpuTransferPipeline` 的 dedicated/record-only 模式与 QFOT（第 31 章）是真实 Mesh/Texture 性能路线；只有吸收到 R4 唯一 Queue Ticket/Retirement 才能称迁移，不照抄第二套 Submission Owner。

---

## Source: 24_ECS数据投影与插件扩展.md

# 24 — Engine/ECS → Render 的 DataProjection，不是 Feature 专用 Proxy

## 24.1 问题定位

GridFeature 需要 GridData；Engine 可能维护自己的 `GridComponent`、Editor Viewport 自有设置、robot sensor 数据也可能来自网络。Renderer/Shader 不应 include ECS Component 也不应知道 EnTT 或某具体 Asset 类型。**Engine 只观察业务事实并按中立 DataSchema 发布；Render Native 消费中立数据。**

## 24.2 值得继承的 Legacy 算法

Legacy `RenderFeatureSceneBinding` 支持特定 SceneSystem/Feature 的 `observations` 与 `create_sync_stage` 注册；`RenderSyncStage` 持有 `prepare/commitPrepared/discardPrepared`，`Builtin3DRenderStages` 的 `TExtractionChangeSet`/Component leave observer 能够 O(changed) 记录增量和资源离开；RenderSystem 在完整 PROGRAM 被接受后才 commit prepared source state。它们是有价值的**反应式观察与背压事务机制**，不能全部砍掉改每帧扫描 ECS。

但不需要恢复每 Feature 一份继承 `RenderSyncStage` 的长期 Proxy；新架构用注册型 `DataProjection` 描述输入组件/事件与输出数据的类型和生成函数，相关 Producer/Observer 的寿命属于安装它的 Engine/Editor Plugin 模块。核心 RenderSystem 不为每个 Feature 写 `case GRID`/`case MESH`，也不默认 Feature 自己拥有 ECS Registry。

## 24.3 统一投影过程

```text
Source ECS Component / Asset / Editor Grid Settings / External Sensor
 → registered ProjectionDescriptor {observes, produces DataSchema, Scope, authority,
                                     conversion ops, code pin}
 → typed change observation O(changed), entity leave/create/replace
 → prepare logical values/delta + neutral Wire packet (version/generation)
 → R2 PROGRAM/UPLOAD admission; accept → commitPrepared; backpressure → retain;
    cancel → discardPrepared without losing source-of-truth
 → Backend validates typed payload/ScopeKey/Writer Authority
 → if Storage active, patch typed dirty data + GPU Upload/Ready; if not attached,
    keep declared latest source revision/snapshot or request a cold full-sync on attach
 → Native attachment PreparedBindings reference correct Storage version
```

`ProjectionDescriptor` 可以是值/固定函数 Ops：`observes` 包含 ECS Component schema ID、必要/可选关系；`produces` 是 neutral DataSchema canonical name/version、合法 Scope/Key；Host 在安装时冷态解决 Producer 依赖和唯一 writer policy。插件 callback 需要 CodePin，安装后只调用已有固定 ops，不做每帧 FeatureTypeId 字符串查找。

## 24.4 Grid 示例

`GridData {spacing, majorWidth, minorWidth, opacity, origin}` 是渲染中立类型，GridFeature 构图时要求 View/Attachment 中的 `GridData`，Shader 使用验证后的 GPU Representation。Engine GridComponent/Editor UI 到 GridData 的转换由 `GridProjection` 实现并注册；若 Grid 只属于 Editor 设置，可以绕过 ECS，直接由 Editor Producer 使用相同 DataSchema 发布。**不会为了统一而强制每个 Editor 参数都做一个 ECS Component。**

## 24.5 Mesh/Light/Gaussian 示例

- `Mesh3D+WorldTransform+ResolvedAsset` → `MeshInstanceData`/resource deltas；稳定 Entity handle 必有 World/Scene+generation，不把裸 ECS bits 长期当 GPU stable slot；资产复用进入 Global MeshStorage，实例数据进入 Scene Scope。
- `LightComponent` → `LightGpuData` 及 Scene LightStorage 增量，多个 Effect 消费同一 Scene data；光源更新不触发图编译。
- 插件 `GaussianComponent`/external SLAM → `GaussianChunkData` GPU upload，Native Gaussian storage 可以由 Plugin 提供，但 Plugin 不得让 RenderBackend 内置识别其 C++ Component 类型。

## 24.6 背压与多 Writer

Producer 维护 last committed revision 和 pending delta；R2 transport `BACKPRESSURED` 不提交已准备 ECS 变更，保证后续重试只发送真实缺失的更新。Entity leaves/clearAll 的顺序与 DataVersion 被正确保留，删除之后新建相同 Entity ID 要 generation 区分。多个 Producer 写同一个 Scoped DataKey 需要显式 merge/partition/priority policy；不能最后注册/最后 publish 自动覆盖。

## 24.7 插件与可安装 SDK 验收

独立安装 Plugin 贡献 GridFeature/GridData/GridProjection 后，Engine 运行中的 ECS/Editor 修改通过 neutral client 形成 Native GPU 可验证变化；测试 Create/Modify/Remove、Backpressure、Project switch/Plugin unload/CodePin，且 Engine core 业务代码没有新增 case 分支。Wire Schema/Version/Host C ABI 值全链一致，无 RTTI/owning STL 跨 DLL。

---

## Source: 25_Protocol与可替换NativeBackend.md

# 25 — Engine ↔ Server 仅通过中立协议；Native Provider 可替换

## 25.1 职责断面

应用 Native bootstrap 选择 Vulkan 或未来 D3D12 Provider，并在运行前选择 GPU；Engine/Editor/ECS/Plugin 只拿 `RenderRuntime` neutral client。中立 `RenderBackend` 拥有 Scene/View/Effect/Target/Attachment 和唯一 FrameLoop，通过一个必要的 Native Session Port 调用具体图形 API。当前 `runtime/Backend.hpp` 直接接受 `vulkan::BackendOptions/EffectSource` 是 P2-A 的实际过渡而非最终公开 API。绝不要求 Engine `runtime.vulkanDevice()` 来编写 RenderSystem。

```text
Platform/App Native Factory  [Vk instance/physical device OR D3D12 adapter]
        │                      \ one concrete native implementation chosen
        ▼                       
RenderBackend (neutral owner-thread)  ←PROGRAM CONTROL UPLOAD→ RenderRuntime Client
        │  neutral Effect/ResourcePlan/GraphIR, data descriptors and target requirements
        ▼
Native session port (single narrow backend seam; no parallel frame loop)
        ├── Vulkan: bootstrap InstanceContext + one DeviceSession (Device/ResourceContext)
        └── Future D3D12: own Device/DescriptorHeap/Barrier/CommandQueue/Swapchain
```

`NativeSessionPort` 可以作为一个局部固定 Ops 表/小的 type-erased session 值，与 Native Provider 对象一道在 bootstrap 装配；**不是**暴露给 RenderFeature 和 Engine 的巨大 `IRendererService` 虚接口。哪几项操作成为 Ops 必须由真实 Compile/Prepare/Execute/ResourceFactory/Target/Retire 消费者证明。单 Native Provider 只能有一个最终 GPU work scheduler，不能创建第二 FrameLoop/Backpressure system。

## 25.2 继续沿用 R2

PROGRAM、CONTROL、UPLOAD 三 Lane、Reply Arena、Admission、Stop/Drain、Diagnostic 端口和 Protocol codegen 都已经真实存在。新 DataProjection/Effect/Attachment 消息应为同样的版本化 wire 语义，在 Handler 检查 owner/generation/data schema，并在完整接受后才 commit；不允许 Runtime 内并行维护第四个 EventBus。场景/效果变更的 hot revision 与 GPU accepted tickets 必须区分。

## 25.3 Backend 可移植性的边界

平台中立的 Compiled ResourcePlan/GraphIR/ShaderSemantics 可复用，具体 Vulkan 的 SPIR-V、DescriptorSet/VkPipeline/Sync2、WSI 对应到 D3D12 的 DXIL/RootSignature/PSO/ResourceState/Queues/DXGI；这是独立 Native Compiler/Execution 的工作。数据 Projection 不知道这些，因此更换 Backend 不修改 Engine Grid/Mesh/Light Component 转换代码。允许 provider 明确声明不支持某 Feature/Native command；必须走合法 fallback 或配置错误，不造假的等效性。

## 25.4 多设备不属于本轮

每 RenderBackend 恰好一个 DeviceSession，初始化选一次 GPU；`InstanceContext` 可以枚举多个 Adapter，却不等于当前运行多个 session。Future cross-GPU copies/device groups/async compute coexecution 单独设计，有真实消费者再增加；本期不出现 DeviceSessionId/sessions registry/跨 GPU Target Composition。


## 25.5 Bulk UPLOAD 是独立协议 Lane，不是裸 Vulkan worker API

当前 `RenderTransportCapacity` 已分别约束 `pending_uploads`、`upload_bytes`。Native Provider 的 `UploadConsumer/TransferService` 通过 Host/Server 内部接口接受已验证的 owned payload，产出 buffer/image 后续合法使用所需的 `ReadyTicket/NativeResourceState/QueueFamily`，并在资源的 LastUse 安全时退休；R2 client 不暴露 VkQueue/VMA。专用 Mesh/Texture/Chunk 大上传不能复用 PROGRAM 控制包到处传 `span`，亦不能在 D3D12 增加时改 Engine ECS Projection 的语义。详见第 31 章；P2-A Backend 的 Upload Lane 尚无 active consumer，状态为 NOT_IMPLEMENTED。

---

## Source: 26_Attachment生命周期与反例清单.md

# 26 — Attachment RAII、资源使用权和 GPU Ticket 的反例矩阵

## 26.1 问题：把 Scene 析构当作 GPU Storage 析构

错误实现：`~Scene() { delete scene.lightStorage; }`。真实 GPU 资源可能同时被 E1/E2 多个 Attachment 使用或还有未完成的 GPU submit；并且 Scope 存储的 Owner 本就位于 DeviceSession。正确行为：Scene 删除请求由 Backend owner-thread 确认/撤销关联，Attachment release CPU leases，GPU 资源等真实 Ticket 完成退休，最后使 Scene 的 SlotKey 失效。若选择 Busy 契约，则需先 detach；不能在 Scene destructor 里直接调用 Native `waitIdle` 或手动 destroy。

## 26.2 问题：同 View 不同 Target 历史复用

E1/ViewA/T1 的 TSR 与 E2/ViewA/T2 的 TSR 即使 ViewRef 相同也可能采样不同 width/format/jitter/Shader history ABI。`ResourceScope::Attachment` 必须把 EffectId、TargetId、具体兼容签名视为 part of key；View-scoped camera 可共享，但 History 不能默认按 ViewId 唯一。

## 26.3 问题：编译器以 shader 类型猜 Scope

只有 `buffer<LightRecord>` GPU read 并不证明其数据来自 scene；同型 Preview Lights 和 Main Lights 可能两个不同 DataKey。Compiler 必须看到 Feature/DataSchema 的 explicit requirement，验证 writer/access/schema，再生成 BindingSlot；缺失/冲突 cold-fail，不按注册顺序/字符串首匹配 fallback。

## 26.4 问题：热态查 Registry

编译计划 `[bindingSlot4]` + attach 准备的 native descriptor/Buffer/Ready 允许 executor 直接绑定。不允许每次 pass 查询 `session.domain(sceneId).find<T>()`、重新创建 Shader layout 或在没有结构变化时 rebuild Graph。资源更新通过 typed dirty/version 驱动必要 native descriptor patch，而非高频全局数据重建。

## 26.5 问题：多 Effect 同 Target 多次绘制

Target T 的 E1/E2 都输出中间片段；不可各自以 `execute(Ei,view,T)` 宣称已生成完整终图，再在 WSI 额外开启隐藏 Composite。Compiler 必须派生完整 Target Graph/Pass/Output，并由 Backend 在 Effect 更新时一次整体切换；若不同目标有显式依赖，遵守 Ready Ticket。

## 26.6 问题：Plugin 卸载和代码回调

插件提供 `GridProjection` 的 callback/Feature Ops/Native Resource destructor；Host 可能在 GPU 提交后继续使用其 Shader/native callbacks。CodePin 必须覆盖最后的 CPU callback 和 GPU/Native owner teardown。跨 DLL 只传版本化 Descriptor/opaque token，不能依赖 C++ RTTI/owning STL。

## 26.7 非 OOM 失败与 fatal OOM

普通参数/身份/DeviceCaps/ABI/Writer 冲突可恢复，在候选事务下完整拒绝且 last-good。OOM 按项目 Fatal 策略终止；不要把历史 P2-A “vk OOM 注入后可重试”作为新协议未来必须保留的能力。Native partial submit/device lost 必须保持已被接受 work 的 ticket 与 teardown 安全，而不假设总提交成功。

## 26.8 精确阶段反例见 appendix

`appendix/ExecutionScenarios.csv` 给出至少三十个 scenario 的阶段、Output Oracle、失效条件；必须区分已有 P2-A 曾通过的 B01–B21 历史事实与新增 RD3-R3 设计门禁。脚本不可因为被测功能未实现就将其全部标为 PASS。

---

## Source: 27_H1至H6高级渲染设计输入.md

# 27 — 面向 UE 类高级渲染的可实施接口与缺失能力

## 27.1 Native 先支持真正消费者需要的原语

H1：Cluster/HLOD/mesh page residency、GPU feedback、indirect/indirect-count 可选 mesh/task；H2：Scene radiance/SDF/probe/3D cache、async compute、可选 BLAS/TLAS/RayQuery；H3：MotionVector/Jitter/Reactive/History epochs/relative Target extents; H4：Virtual Shadow Atlas/clipmap/page table/cache dirty/feedback；H5：高级 BSDF、Texture3D、Volume marching、完整 Shader Owner/Layout；H6：Gaussian chunk GPU Storage、compute projection/binning/sort、blending/transmittance、Mesh depth composition。这些是分别需要开发的 Native primitives/data/shader，不是给一个 Feature class 加六种 boolean。

## 27.2 资源与 RAII 对算法的具体价值

| 方向 | Scope/Storage | Attachment-private 或 Temporal | 关键编译与执行能力 |
|---|---|---|---|
| H1 Virtual Geometry | Global mesh pages + Scene hierarchy instances | View visibility/feedback history（若确有） | GPU page ready/compute cull, indirect, HZB dependences |
| H2 Lumen-like | Scene radiance/probe/SDF/GPU cache + Global geometry | Temporal denoise/exposure per view output | 3D textures, compute graph, optional RT/native AS, fallback |
| H3 TSR | View camera & motion source | **必需按 target/effect/scale** history/reprojection | ping-pong history, reactive masks, explicit resize/cut invalidation |
| H4 VSM | Scene/Global shadow atlas and physical page pool | per light/view page requests if contract specifies | atlas mip-layer hazards, residency feedback, page invalidation |
| H5 BSDF/Volume | Global material/texture, Scene volume fields | temporal volume accumulation where useful | multiple Shader Owner ABI, Texture3D, compute/graphics linking |
| H6 3DGS | Global/Scene Gaussian chunk storage + Scene transforms | LOD/sort tile state per view/target | GPU tile binning/sort/splat/transmittance/indirect, Mesh depth resolve |

同一 Effect 内的多 Features 一起形成完整 Graph，不能每个高级算法建立自己的 Native FrameLoop 或 GlobalContext。Native DeviceSession 的 Memory budget/retention 可预留策略接缝，但不在 R3-A/R3-B 提前实现庞大的“Universal Page Residency Manager”。真实 H consumer 出现时保留算法灵活性而不新增重复资源身份。

## 27.3 能力等级与降级

每项功能应声明 `requiredNativeOps`, device-supported & enabled features, ShaderVariant/RayQuery/DescriptorIndexing/Memory Budget，并提供经验证 fallback（例如无 Mesh Shader 用 Indexed/Indirect，GI 可选软件追踪路径），否则明确 Capability Unsupported。真正没有支持的 RT/3D textures/local read 不能因作者 API enum 存在就标 READY。不同设备启动选择时 native provider 报告精确 capability 组合；单 DeviceSession 可以创建多个 Effect 质量档，但各档 Shader/Graph/ResourcePlan 版本必须合法。

## 27.4 质量与性能参考

每个 H1–H6 建真实复杂场景，固定输入、基准 GPU、主观/客观质量 oracle、Ghosting/Flicker/popping、VRAM page/atlas 峰值、GPU timestamp/CPU overhead 与异常素材回退。**不能直接声称“UE 同等画质”**；可以将同类机制作为目标，并以公开技术机制文档为参考，网址在 REFERENCES。当前 P2-A/本设计阶段 H1–H6 均为未实现。


## 27.5 图编译/材料图/专用传输作为 H1–H6 的必要证据

H1/H4/H6 的 page/cluster/atlas/chunk streaming 接第 31 章 UPLOAD→Native transfer/Residency/Graph Ready 真链路；H2/H3/H4 的多 Pass/LocalRead/Conditional/History/Tile LazyMemory 接第 29/32 章；H5 复杂材质不是从头发明一份 Shader Node 系统，应扩展当前 `material::MaterialGraph`、可注册 CodeLease Node、MaterialIR/ShaderIR、材质 GPU param/texture、真实 Forward/Depth/Shadow variants（第 30 章）。未有实际能力、像素、驻留和对照证据前，所有功能仍为 H 阶段 DESIGN_ONLY。

---

## Source: 28_目录与CMake模块边界.md

# 28 — RD3-R3 目标目录与真实依赖 DAG

> 本章重写 RD3-R2 `LAYOUT` 并与新的 Attachment/Compiler/Projection/Native Bootstrap 设计保持一致。目录建议按**稳定职责**划分，不预设每一个目录都要 CMake target；实际文件迁移按需要的消费者逐步进行。只有本章目录是 RD3-R3 目标，不再引用旧 RD3/R2 目录图作为当前事实。

```text
modules/function/render/
├── core/               # RenderResult/Strong IDs, neutral GPU/data descriptors
├── transport/          # 既有 R2 PROGRAM, CONTROL, UPLOAD, reply/admission/diagnostics
├── client/             # RenderRuntime & typed protocol client (NO Vk/D3D12)
├── data/               # neutral DataSchema, Version/Wire/Projection data contracts
├── scene/              # neutral Scene/View; Scene owns View SlotMap
├── target/             # neutral TargetDesc/Attachment placement/final output contracts
├── effect/             # RenderEffect, Feature author ops, logical Graph ownership
├── graph/              # existing RenderGraphBuilder/C0-C8, ResourcePlan (neutral)
├── backend/            # neutral RenderBackend; one editable Attachment relationship
│   ├── include/        # narrow native port/bootstrap-free neutral server API
│   └── src/            # owner-thread/attach transaction/cold target schedule
├── vulkan/             # selected concrete Native provider; Engine not depending on it
│   ├── context/
│   │   ├── InstanceContext.*    # Native bootstrap-OWNED, outlives Session
│   │   ├── DeviceContext.*      # Session-OWNED, selected VkDevice/VMA/Queue/Caps
│   │   └── ResourceContext.*    # Session-OWNED, native staging/pool services
│   ├── DeviceSession.*          # ONE selected device session, borrows Instance
│   ├── device/                 # existing R4 VulkanInstance/Device/Allocator RAII
│   ├── memory/                 # Buffer/Image/Arena primitive; no business Registry
│   ├── resources/              # PRIVATE ScopeResources/leases/native storage factories
│   ├── transfer/               # one queue/submission/staging/retirement mechanism
│   ├── shader/                 # SPIR-V/PSO/OwnerShape/Layout/NativeShaderProgram
│   ├── graph/
│   │   ├── Compiler.*          # C9-C13 fixed cold Native stages
│   │   ├── Graph.*             # immutable code, no per-View mutable business state
│   │   ├── Executor.*          # record/submit algorithm; single FrameLoop owner
│   │   └── ExecutionContext.*  # native FIFO/descriptors/tickets/transient/history
│   └── target/                 # Offscreen/Surface/Swapchain/Readback WSI backing
├── features/            # actual Mesh/Lighting/Shadow/Grid/Canvas/PointCloud/Robotics authors
│   └── <feature>/       # neutral algorithm & data schema; opt native providers separately
└── sdk/                 # only if installed plugin C ABI/authoring need distinct export

engine/scene/builtin_systems/render/
    # RenderSystem/registered ECS DataProjection, O(changed) + R2 Client only
engine/toolchain/shader/
    # existing one codegen/reflect toolchain; Vulkan SPIR-V or future D3D12 DXIL outputs
```

## 28.1 CMake 依赖有向图

`render_core` ← `render_transport`/`render_data`/`render_graph`; `render_client → render_core+transport+neutral_schema`；`render_effect → graph/data`；`render_backend(neutral) → core/scene/effect/target/graph/transport`；`render_vulkan → neutral backend/graph + R4 Device/Queue/ShaderToolchain`；Engine RenderSystem → client+neutral Schema。`vulkan` 不被 `RenderRuntime.hpp`、Engine/Editor ECS Project 以及中立 Feature 公开头链接或包含。需要时 Native Provider 自己构造 DeviceSession，并通过窄 native port 交给中立 Backend。

P2-A 真实代码当前仍在 `modules/function/render/runtime/**` 中，包括 `DeviceSession.hpp/.cpp`、`ResourceDomain.hpp`、`Objects.hpp`、`Backend.cpp`，Native DeviceSession/Instance 尚未完全分离。目标目录迁移必须根据 include/link/installed consumer 与实际额外修改影响判断，不能一次性造几十个空文件或为每种“概念”创建独立静态库。`RenderBackend::Impl` 当前 Vulkan 强耦合可有界迁移；中立 UI/client 必须保持可安装头的 backend independence。

## 28.2 文件组织约束

新 `Graph`/`ResourcePlan` 在中立 authoring 层；`ScopedResources` 仅 Native 侧存储，不新建 Engine 级 ResourceDomain Registry；`DataProjection` 在 Engine/插件数据源模块，不归 Vulkan Context/Effect 类；`Legacy MeshStorage` 落专业 GPU 资源实现并取得最窄 Native Context，不把每个 Mesh 登记进 Session 顶层 Scope 表。Compiler Native phases 不需要动态注册 OO class。拼写统一 camelCase `getOrCreate`，没有 `get_or_create` 或 `VulkanBackend` 大管家类型。


## 28.3 材质工具链与大资源传输的真实目录职责

**已有且必须复用**：`modules/function/material/`（MaterialGraph/NodeCatalog/Pin/Payload/ShaderIR 的中立作者核心），`engine/toolchain/material/`（MaterialCompiler/Cooker/GLSL+SPIR-V 编译，未来可增 DXIL backend），`engine/toolchain/shader/`（单份反射/ShaderABI）；不要在 `modules/function/render/features/material/` 新造同义 `MaterialGraph`/`MaterialNodeRegistry`。RenderFeature/Graph Author 引用的是 Cooked Shader/Material contract，实际 GPU 表放在 `vulkan/resources/material/` 等按业务组织的专业 Storage。

**Native 业务入口**：`modules/function/render/vulkan/transfer/` 保留 R4 SubmissionQueue/Ticket 与 staging；按真实用户增加有界 BulkUpload/Texture/Mesh Transfer 工具和必要 worker，不在 `client/transport` 放 Vulkan 对象，也不为 dedicated VkQueue 再新建一个不兼容的 Submission 同步库；逻辑 Graph 的外部 Ready/Use 合同仍在 `render/graph`。`render/vulkan/graph` 的 ResourcePlanner/LocalRead/TransientPool/ExecutionContext 等按实际消费者组织，不复制 `RenderGraphCompiler.cpp` 旧 1762 行总管。细节见第 29–32 章。

---

## Source: 29_Legacy_RenderGraph_Compiler源码审计与迁移合同.md

# 29 — Legacy RenderGraph / Compiler：源码级能力与不可丢失的算法合同

> **来源定位**：本章针对 `render_legacy/` 冻结源码逐文件阅读形成；不是声称当前 R4/P2-A 已具备这些全部功能。新 C0–C13 的真实能力应以当前 `modules/function/render/{graph,vulkan}/` 的实现为准。**优先沿用当前正确的 R4 算法，Legacy 作为功能/优化正确性检查单，不得倒迁旧错误或恢复重复 Scheduler。**

## 29.1 源码地图与职责

| Legacy 源文件（前缀 `render_legacy/modules/function/render/`） | 已有功能/事实 | RD3-R3 处理 |
|---|---|---|
| `graph/src/DependencyAnalyzer.cpp` | 资源→Pass 预建索引、显式 before/after、读写依赖、分阶段 Kahn 拓扑、Painter stage 排序、Local-Read 邻接约束 | 中立 C0–C8：同一版本化依赖真相，不重建业务排序表 |
| `vulkan/include/lux/engine/render/graph/RGBuilder.hpp` | transient、persistent、ping-pong buffer/image、imports、dynamic slot、tracked external、前向引用、conditionChain、read/write/inputRead/sideEffect | 新 Graph Author/IR 必须能表达；不搬旧 Vulkan/raw DS 到中立 Builder |
| `vulkan/src/graph/RenderGraphCompiler.cpp` | 顺序编译步骤、forward ref、usage union、resourceDS 依赖、LayoutPlan、starved/dead 剪枝、queue/条件/barrier、compiled kernel | 新固定 Compiler phases；按依赖关系排序，不能凭命名把步骤丢掉 |
| `vulkan/src/graph/RenderGraphCompiler.ExecutionPlan.cpp` | attachment load/store、LocalReadBoundary、条件链验证、MeshBucket/ViewAllocator、BarrierProgram、QueueSubmitProgram、ExecutionProgram 字节码 | 在 R4 Native Recipe/Executor 中对应有价值的静态优化；不必复制旧 bytecode 类 |
| `vulkan/src/graph/RenderGraphCompiler.LayoutAllocator.cpp` | 全图反射合并、descriptor 绑定兼容、Engine-shared domain layout、StageUsage limits、layout identity | 新 Shader Layout/OwnerShape/ABI full shape，校验设备 Enabled Caps |
| `vulkan/src/graph/RenderGraphCompiler.Barriers.cpp` | Per-resource state、layout/access/stage、多个读者聚合、first/subsequent View、最终 export、DS binding recipe | R4 Sync2/Compiler 继续提供真实 Queue/Slice/Transition/Descriptor 正确性 |
| `vulkan/src/graph/RGVulkanResourceAllocator.cpp` | transient physical pool、persistent/ring、VMA suballocation pools、lazy attachment memory probing、recompile reuse、idle GC | 按真实 MemoryRequirements/LastUse 整合进 Native ResourcePlanner/ExecutionContext |
| `vulkan/src/graph/RGGraphResize.cpp` | resize 创建全新 ResourceState 候选，失败还原旧状态，更新动态外部导入，成功才退旧资源 | R3-C Target/History 采用整体候选+真实 GPU/WSI retire |
| `vulkan/src/graph/RGVulkanRecorder.cpp` | prebuilt barrier replay、dynamic imported refresh、per-frame descriptor、Native execution fast program 与普通 fallback | Executor 只用 prepared indexed binding，必要时复用低层现有录制器算法 |
| `vulkan/include/lux/engine/render/graph/FrameExternalSync.hpp`、`FrameExtensionRegistry.hpp` | 外部资源 ready/扩展接缝 | 新版按明确 Graph import/output、Ready/LastUse 和 code pin 表达 |

## 29.2 不可任意重排的 Compiler 语义链

源码 `RenderGraphCompiler::compile()` 的顺序不是装饰：

1. **Resolve forward resources → texture usage union → 声明的外部 DS 消费显式计入数据依赖**。在此之前建依赖会漏掉真实访问。新版由 DataSchema/PassSchema/ResourcePlan 统一声明，禁止 `trackExternal*` 变成完全图外同步。
2. **全局依赖、资源合法性 → transient descriptor layout 角色推断/核对 → graph-wide reflected LayoutPlan**。Layout/Owner 结果必须在 Native Pipeline 创建之前确定。
3. **Compiled pass → topo → `eliminateStarvedPasses()` → `eliminateDeadPasses()`**。先裁剪没有有效输入的 Pass，再按输出可达性裁剪；有 SideEffect/External 输出时不能误删。死 Pass 不能贡献 Barrier、Transient alloc 或 Query 需求。
4. **Assign queue → cross-queue dependencies → merged rendering scopes → LiveAccessIndex → classify elective**。`LiveAccessIndex` 必须由活 Pass 和真实执行序计算一次，条件链和 AttachmentOps 共享此事实，避免两个不同索引语义漂移。
5. **Pipeline/Descriptor bind plans → attachment load/store → Sync2 barriers → prebuilt barrier arrays**。LOAD 也是 Attachment READ，必须加入 Hazard 推导；descriptor 省绑定只在 Layout/Set 完整身份相同时合法。
6. **Transient DS / Native resource backing → classified Native execution → precompiled commands/patches/queue template**。新 R4 已将多阶段操作组织为 Typed Command/Recipe，保留优化语义，而非创建解释器与 Native Executor 两套录制路径。

以上是 Legacy 的实际流程；新 RD3-R3 可用更强的 SSA/ResourceVersion 将其中若干步骤合并，但必须用等价反例证明其语义不丢失。

## 29.3 高价值的非显然约束

- **`inputRead`/Dynamic Rendering Local Read**：同一合并 rendering scope 的 producer 与 consumer 须合法相邻；用显式 dependency/producer version，而不是依赖 Feature 注册顺序；实现时校验 VkDevice 确实启用 KHR dynamic rendering local read，并为不支持设备提供拆分 render scopes 的合法 fallback。局部 by-region Barrier 不能伪装成非法 scope 内的普通 pipeline barrier。
- **Painter/after/before 与 WAW**：Stage key 只作为确定性的 tie-break，不覆盖真实生产/消费依赖；绘制顺序在最终 Target Composition 也须显式。
- **Conditional/Skip Chain**：Legacy `conditionChain()` 令一组 Pass 原子启停，编译器检查 `CONDITIONAL_CHAIN`、`INTRA_GROUP_ADDITIVE`、unconditional reader、首写 CLEAR。RD3-R3 不能用每 Pass `if(enabled)` 直接跳过导致下游读取未生产的 GBuffer/History。
- **LOAD/STORE 优化**：`Transient + 最后一次访问已结束` 才可 `STORE_OP_DONT_CARE`；跨 View/History/Export 需要保留数据；清除必须由有效首写执行者负责。
- **Barrier 性能/安全**：Legacy 已撤销错误的 split barrier：两个没有 VkEvent 的 NONE/NONE release/acquire 不能自动建立依赖；跨读者的 stage/access 应累积，不要把后续 RAR 当作不需继承前者同步。R4 Sync2 是后续唯一 native 真相，禁止重启旧失败算法。
- **Descriptor binding dedup 风险**：Legacy曾因跨 Pass 去重而漏了与 Pipeline Layout/Domain 不兼容的实际重绑。最终按**精确 Native PipelineLayout + binding set + slots + dynamic offsets + version + command scope**证明；无法证明便绑定，不能只比较逻辑 DomainId。
- **First View/Subsequent View**：Legacy 有两套 BarrierProgram，针对同一 Scene Graph 多 View/共享 Scene pass 调整输入状态；新实现必须依赖真实上一使用 Ticket、GraphResource state、donor/provenance，不能只按照 View 序号忽略危险依赖。
- **Native fast path**：Legacy 包括 MeshBucketLayout、ViewAllocatorPlan、BarrierProgram、QueueSubmitProgram、带动态 Patch 的 ExecutionProgram；且存在非 Kernel recorder fallback。新系统可用 R4 `NativePassCommand`/Recipe 实现同等热态效果，**不是要求复制解释器**，不能让每帧运行大量字符串查询/动态回调分支。

## 29.4 物理资源规划与缓存（不能混为一谈）

- **逻辑 Graph 的版本与资源 lifetime**：Transient、Persistent、Imported、External/Tracked、PingPong Current/Previous、Slotted image；History 的过去/现在互不别名且受 resize/camera-cut/version 约束。
- **同次 Graph 物理 Alias**：依赖可证明不重叠的完整生命周期、相同设备内存类别、alignment/size/usage/format/alias limits、真实 ResourceCell/barrier；不等于跨帧图像对象复用。
- **跨重新编译的 Physical Pool**：Legacy `TransientResourceKey` 全字段包含 image format/size/depth/mip/layers/samples/usage 与 buffer usage/size/memory usage；`RGVulkanResourceAllocator` 有 transient image VMA pool、Lazy Memory pool、recompile pool+GC。新系统可用小容量 flat exact-key cache 替代 hash multimap，但不能退化为“每 Resize/new Effect 都重新 VkAllocateMemory”。物理 owner 未完成 GPU 使用时不得进入可重用池。
- **Tile GPU**：Legacy 单独探测 `LAZILY_ALLOCATED` 是否真的可用，且缺失时也使用 pooled allocation fallback；新 Native Capability/MemoryPlan 要保留可查询事实，不要只因 `VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT` 设为 preferred 就断言真正分配到了惰性内存。
- **Resize**：先创建完整新 backing/Views/descriptor，再原子替换；旧状态正确退休。不要用“直接重建整个逻辑 Effect”来代替 Target 的物理重专门化。

## 29.5 新实现阶段及必须证明的反例

- **R3-B**：前向引用、同名不同资源/Scope/Key、外部 read 依赖、starved/dead、SideEffect、WAW、环、Reader 不可达、Shader Owner/Layout 负例；ResourcePlan 只由活资源/业务 demand 产生。
- **R3-C**：LocalRead/merge/attachment clear/load/store、dynamic import/resize/persistent history、conditional chain、multi queue DAG/ownership/sync2、初次/后继 View、Graph Retry/LastGood；`GraphExecutionContext` 使用 R4 模块，不回迁 Legacy 全套 NativeGraphRecorder。
- **R3-D/L1–L3**：MeshBucket/Indirect/Run-length Dirty/physical pooled alias/lazy GPU、VRAM 分配次数、稳定 Graph 重编频率、上传+渲染竞争/吞吐。
- **H1–H6**：反馈、Virtual Resources、时域/跨帧、AsyncCompute/RT/volume/GS 必须基于真实 `ResourcePlan`/Pass/Queue/Retirement 图证据，不能仅列一个枚举类型。

> 本章新增需求逐条在 `appendix/LegacyCompilerMechanisms.csv` 绑定源码、保留策略、阶段与测试；旧 86 项 F/I/H、W25、REQ51 的来源行不改。

---

## Source: 30_MaterialGraph_Shader变体与运行时集成.md

# 30 — 材质图（MaterialGraph）不是 RenderGraph：作者、Cooker、Shader 与 GPU Material Storage 的完整合同

> **核对事实**：材料节点图核心主要位于当前（非 `render_legacy` 冻结副本）`modules/function/material/**`，Shader 编译与 Asset Cook 位于 `engine/toolchain/material/**`；对应旧 Render 产品的 GPU backing/业务接入位于 `render_legacy/modules/function/render/{features,vulkan}/**`。三个层次当前均存在，但 **P2-A RenderBackend 尚未直接打通新的 RenderEffect+MaterialGraph 全流程**。不能把存在库/测试误称为新 Render V2 产品已集成。

## 30.1 已有来源与不可重复造轮子

| 现有源码 | 已完成的职责/边界 |
|---|---|
| `modules/function/material/graph/MaterialGraph.hpp` | `MaterialGraph` 作者模型：GraphTopology 与 Node/Pin/Payload、texture/param slots、render state、shading model；独立于 Vulkan |
| `MaterialNodeCatalog.hpp`/`MaterialNodePayload.hpp` | 可扩展 immutable Node 类型、canonical name/version/type token、typed callbacks、plugin `CodeLease`、Pin schemas、batch 注册失败不发布 |
| `MaterialGraph.cpp`/`MaterialSource.cpp` | 编辑事务/clone/serialize、名称与版本验证；断开节点/缺 Surface 允许存盘，编译时明确报错；Source 是持久资产而非 GPU runtime |
| `MaterialLowering.cpp`/`ShaderIR.hpp`/`MaterialIR.hpp` | 检查输出节点/循环/Pin type/input 需求并 lowering 为带 index 的 ShaderIR（SSA-like），计算 full structural fingerprint，render-state 加入 compile identity |
| `engine/toolchain/material/MaterialCompiler.cpp` | `lowerMaterial()` → `compileToSpirv` for GBUFFER & FORWARD → `rdesc::MaterialDescription`（参数槽、纹理槽、Alpha/DoubleSided 等） |
| `ShaderEmitter.cpp`/`ShaderCompiler.cpp` | ShaderIR → GLSL → shaderc SPIR-V；复用内嵌 canonical GLSL includes，真实 SPIR-V reflection（不是 hand-written layout） |
| `MaterialCooker.cpp`/`MaterialToGraph.cpp` | MaterialGraph → cooked `MaterialAsset`，Imported PBR Material → 通用材质节点图；同构参数以 GPU Material SSBO 值表达以共享 PSO |
| Legacy `GraphMaterialData.hpp`、`MaterialResources.cpp` | Param/Texture table → `GraphFamilyGPU`、TextureSlot→实际 bindless index、SlicedSSBO、MaterialHandle、动态材质 graph family |
| Legacy `VariantBucketManager.cpp` | exact graph bucket key（gbuffer & forward Shader handle+generation+Alpha/DoubleSide）、引用计数、free-list、GPU batch/bucket 再利用 |

**重要区分**：MaterialGraph 是生成 Shader/材质参数的**作者图**；RenderGraph 是组织 GPU Pass/Buffer/Image/Queue 的**执行图**。它们不能合并为一套 Node Registry，也不能让用户的每个材质节点默认生成一个 RenderGraph Pass。材质 Graph 与效果 Graph 通过 `ShaderVariantIdentity + MaterialBindingSchema + MaterialRecord/Handle` 连接。

## 30.2 三种材料变化，三种不同事务

1. **参数值变更**（BaseColor/roughness、材质 ParameterSlot 默认值或实例参数）：保持 Shader/PSO/Effect/RenderGraph 结构不变，只更新 Material Storage 对应记录的 Dirty range、revision/Ready。避免把每一个不同颜色材质编成独立 Fragment Shader。
2. **材质拓扑/Shader 源码变更**（新增节点、修改结构或变更 shading model）：资产编译生成 ShaderIR→SPIR-V/未来 DXIL 候选；全结构校验后对受影响 Native Shader Program/PSO/Graph 提供新版本，原有 Surface/Effect/Target 继续使用 last-good 直到候选可用。失败不能部分替换旧 PSO。
3. **Pipeline render-state/Material layout 变更**（AlphaMode、DoubleSided、TextureSlot/ParamSlot Schema）：属于 Pipeline/OwnerShape/record compatibility 身份的一部分；只要兼容性改变必须重新验证/生成相应 Native Code，不能只传新参数。

`MaterialGraph` / `MaterialSource` 的 NodeId/PinId/NodeTypeId 属于既有 Graph/Material 子系统身份，**不可把它们拿来当 RenderEffectId/RenderGraphId**。当前 MaterialGraph 的内部 unordered containers 是节点编辑存储，既不分配 Render V2 Runtime Object IDs，也不要求为统一 Runtime 身份规则大规模修改既有 Material 资产实现。

## 30.3 正确的数据/编译流

```text
Material Editor / imported asset / plugin material nodes
  → MaterialSource / GraphTopology / Nodes/Pins/CodeLease
  → MaterialGraph semantics & validation
  → MaterialIR + ShaderIR + material parameter & texture slots
  → Toolchain shader backend (GLSL/SPIR-V today; DXIL later)
  → ShaderVariant + reflection + full Material Binding/Layout Identity
  → cooked immutable MaterialAsset / revision
  → neutral Render client Asset/UPLOAD request (typed schema, asset identity)
  → Native backend Shader/PSO candidate + Global Material/Texture Storage
  → Effect-owned logical Graph uses the Material contract (not MaterialGraph itself)
  → attach ResourcePlan resolves Global/Scene material tables and per-view execution bindings
  → Executor uses material bucket/index/PSO and live GPU-ready tickets
```

Material assets/cooked shaders 应当跨 Scene/Effect 被引用；一个材质实例不应因为 8 个 View 使用相同材质就复制 8 份 Shader/Pipeline。Material CPU 作者图可在 Editor 与 Shader Toolchain 使用，但 Engine ECS 与 RenderRuntime Client 不得 include Vulkan/D3D12 类型；资产数据采用有效版本 Wire/Owned payload，不任意原样 memcpy STL/Payload/CodeLease。

## 30.4 Plugin、Node 与 Feature 的两个不同扩展点

- **Material Node 插件**：加入 MaterialNodeCatalog，声明 canonical type/version、PinSchema、Payload Codec/Validator/Compile callback；Host retain CodeLease 直到编译候选与回调不再引用代码。
- **RenderFeature 插件**：声明 GPU Pass 和 Material 数据需求，可消费 `MaterialAsset`/`MaterialRecord`/`MaterialShaderVariant`，不自动拥有 MaterialGraph Editor 或导入器。
- **Engine/ECS Projection**：将引擎材质组件/资源引用映射到中立 MaterialRecord/AssetId/Version 数据；编辑器材质图与材质实例编辑属于不同 Authoring Producer。

示例：GraphMaterial 的 Shader 经过 Toolchain 生成，在 Forward/GBuffer Feature 的 PassSchema 中被引用；Shadow/Depth/Visibility Pass 只有在支持合法专用材质变体时才参与，不能静默用不正确的 GBuffer Shader 替代。

## 30.5 当前完成度与不能夸大的能力

当前 `engine/toolchain/material/src/ShaderCompiler.cpp` 通过 shaderc 生成 Vulkan SPIR-V；没有 D3D12 DXIL Backend 实现。源码明确拒绝 Shadow/VisBuffer Material Pass；`RAW_EXPR` 仍显式 unsupported。ShaderEmitter 文件顶部的一些旧注释比实际 case 实现落后，**必须以代码分支/真实测试为准**：SampleTexture/Param/TBN 与 PBR Forward 代码路径已经存在，不能只凭顶部的“尚未实现”判掉。

Legacy `GraphMaterialData` 固定 `kMaxParams=16`、`kMaxTextures=8`，以及旧的 `set 2 / set 4` 是历史 Shader ABI，不应未经验证直接冻结成新统一 D3D12/Vulkan ABI。必须保留分离 Parameter/Texture binding、Stable Slot 与 Variant Bucket 复用机制；新 Layout 交给 Shader Schema/OwnerShape 和实际 Native Device caps 选择。材料资产格式/存档的历史版本兼容不可默默更改。

## 30.6 需要的端到端验收

- 对同一 MaterialGraph，两材质实例仅参数不同：**一份 Native Shader Variant/PSO**，两个独立 Material Records，输出像素不同。
- Graph 节点增删、循环、错误 Pin、多个 OutputSurface：编辑事务保存/编译错误行为与源合同一致，旧 cooked material/旧 PSO 仍有效。
- Material 的 AlphaMode/DoubleSided 或 Shader 代码变化：产生相容的新 Pipeline identity，且旧在途 draw 正确退休。
- Texture asset 未 ready、Mip replacement、Bindless slot Generation 复用：无错误纹理采样、无提前覆盖 in-flight Descriptor；Material 管线复用不偷换 Texture backing。
- RenderEffect 中至少一次 GBuffer + Forward/Depth Shader Variants 与 MaterialGraph 编译输出的实际 ABI 联通，真实 GPU Validation/像素校验。
- 插件 Node 卸载：Payload/NodeType/Callback/CodeLease 被清晰持有到最后使用；RenderEffect 本身无需知道 Engine ECS。

> 本章在 `appendix/MaterialGraphContracts.csv` 逐条列出真实 Source、目标 Owner、R3/L1/H5 阶段和验证；不能把 Node Graph 扩展功能只放在 H5。

---

## Source: 31_大资源UPLOAD与GPUTransferPipeline.md

# 31 — Mesh/Texture 大资源专用上传：三种队列、CPU/GPU 拥有权与真实 Ready Ticket

> **不能把三种 Queue 说成一回事**：① Engine→Server 的协议 `UPLOAD` Lane；② Server 内部 CPU Job/Completion Queue；③ Vulkan Transfer/Graphics `VkQueue`。它们的 producer、backpressure、生命周期和同步不同。Legacy 已有优化过的三层实现，而当前 P2-A 只保留第一层的 Transport 机制，Mesh/Texture 上传业务尚未接线。

## 31.1 已核对源码与关键机制

| 层 | 源文件 | 当前/Legacy 事实 |
|---|---|---|
| 新 R2 Transport | `modules/function/render/transport/include/lux/engine/render/transport/{Transport.hpp,Operation.hpp}` | PROGRAM/CONTROL/UPLOAD 三 Lane 与 packet/reply arena/admission，默认 `pending_uploads=64`, `upload_bytes=256 MiB`；这些是默认预算，不是吞吐资格 |
| 新 P2-A Runtime | `modules/function/render/runtime/src/Backend.cpp` | `UPLOAD` 目前只在 shutdown drain 时 `pollUpload`；**尚无 Mesh/Texture active upload consumer** |
| Legacy Client | `render_legacy/modules/function/render/client/include/lux/engine/function/render/client/{RenderUploadClient.hpp,RenderUploadSession.hpp}` | 线程安全 producer 封装、拥有数据的 PreparedUpload、请求/回复、队列 full/byte budget/stopping，coordinator SPSC 所有权 |
| Legacy Host | `render_legacy/modules/function/render/runtime/pinclude/lux/engine/render/detail/UploadQueue.hpp` | bounded MPMC enqueue with byte accounting → coordinator submit，有明确 backpressure，按请求序处理 |
| Legacy Native Transfer | `render_legacy/modules/function/render/vulkan/{sinclude/lux/engine/render/resources/lifecycle/GpuTransferPipeline.hpp,src/resources/lifecycle/GpuTransferPipeline.cpp}` | 仅 Mesh/Texture/Cube 的 typed jobs；一条 GPU transfer worker + SPSC jobs/results，支持 DEDICATED_QUEUE/RECORD_ONLY、部分 submit 与 stop/drain |
| Legacy Staging | `render_legacy/modules/function/render/vulkan/sinclude/lux/engine/render/gpu/transfer/TransferScheduler.hpp` | staging ring、overflow FIF reuse、coalesced Buffer/Image copies 与 pre/post barriers、贡献者 hook |
| 当前 R4 Queue | `modules/function/render/vulkan/include/lux/engine/render/vulkan/transfer/Submission.hpp` | Address-stable `SubmissionQueue`、`CommandBatch`、真实 `SubmissionTicket`；不能与 Legacy 老 timeline 又建两份 GPU 完成权威 |

## 31.2 正确的端到端数据流程

```text
Worker / Asset Loader / Plugin producer
   ├── Persistent asset / immutable source bytes (shared owned payload or validated BlobRef)
   └── request ID / schema / byte count / generation
       ↓ bounded UPLOAD admission (independent from small CONTROL/PROGRAM)
RenderTransport UPLOAD Lane
       ↓ Server owner validates full packet and reserves Resource/Queue budget
Native Transfer Service [one physical GPU submission truth]
       ├── staging ring / validated mip/cube/row pitch / buffer offsets
       ├── typed Mesh / Texture2D / Cube / Region / Mip jobs
       ├── DEDICATED_TRANSFER → Submit via existing R4 SubmissionQueue
       └── QUEUE_ALIASED → Record-only / graphics-submit on sole queue owner
       ↓ actual producer SubmissionTicket + layout/queue-family state
Resource-ready acquisition / Native Graph import & acquire barriers
       ↓ GPU consumer waits on real transfer provenance (not 'Upload operation accepted')
Native Graph execution (MeshDraw/TextureSample/Material) → release / retirement
```

**原则**：`UPLOAD accepted` ≠ `native scheduled` ≠ `GPU ready` ≠ `consumer-completed`。这些状态必须分别可观察；渲染资源未 Ready 时遵循明确合法 fallback 或不调度需要它的 Pass，而不能读取尚未完成的 VkBuffer/Image。

## 31.3 大小不同的数据走不同交通机制

- **低频 CONTROL**：Create/Attach/Detach、Scene/Effect/Target 的身份事务，数据体积小但要求确认/Reply。
- **每帧 PROGRAM**：Camera/Grid/Light/instance 等经 Schema 的紧凑变更或必要的 small scalar blob；有版本和提交阶段，不能用大 Texture Blob 阻塞。
- **大体积 UPLOAD**：Mesh VBO/IBO、Texture Mips/Cube/Regions、Gaussian/PointCloud chunks、预生成的 Shader/Material asset payload；拥有源内存的生命周期、队列长度与**字节级**背压，允许异步传输提交。
- **Native GPU Transfer Queue**：设备/driver 决定是否独立 VkQueue；仅选择真实可用的 dedicated queue 才作并发执行，Queue alias 时同一队列由一个 Owner 串行提交。协议 UPLOAD Lane 无需因此总是开独立 GPU thread。

`PROGRAM` 不应被重复用于传大量单独 texture slice；反之可变化的少量材质参数也不应每帧发完整 Texture asset。上传接口不能只暴露裸源指针而不保留拥有者；ZERO-COPY 只在上游 buffer 生命周期/协议同一地址域合法时成立，不因“使用 span”就假定零拷贝。

## 31.4 Legacy GpuTransferPipeline 中必须保留的专业契约

- **队列别名退化**：Dedicated Queue 时 worker 可 staging/record/submit；Queue Handle 别名时不能让 transfer worker 同时 submit graphics queue，必须 record-only，由唯一 queue owner 最终提交。Shared Queue 不等于独立 Queue Family。
- **QFOT**：Transfer queue family 不同且资源 exclusive sharing 时，需要 release/acquire ownership transfer + 正确 semaphore wait/Stage/Layout；不可只做普通 pipeline barrier。不同 Queue 但同 Family 仍需跨队列执行/内存同步。
- **稳定的源 Payload**：Legacy MeshTransferTask `data_owner` pin VBO/IBO 原始数据，TextureMipRef `shared_ptr<const void>` 保持每 Mip 的 bytes；当前要求采用可追踪 immutable owner/BlobRef，不允许 packet 消费后悬垂原始指针。
- **精确字节计划**：Texture2DUploadPlan 的每 Mip offsets/total bytes、Cube face stride、region row_pitch/format/chunk limits 都在 Server 验证。Transfer worker 不得二次猜测行对齐或从一个已丢失的用户配置重算。
- **有界并发**：Legacy 线程仅接受明确的 Mesh/Texture/Cube 任务，不是 `submit(F&&)` 任意任务线程池；BatchSlotLease 直到 GPU/GraphicsAcquire 证明后才回收 Command Pool 与 Staging。
- **停止与失败**：所有已接受 Request 应有最终 Reply/Failure/Abandon；部分 Vulkan Submit 成功、后继 Submit 失败时，不能通过一个假定“全部完成”的最终 Ticket 提前释放任何 staging/mesh/image。

## 31.5 与 R4 的合并要求（禁止双执行真相）

保留 Legacy **两级有界队列、专用大资源任务、Staging/Record-only/Dedicated 模式和精确 QFOT 逻辑的价值**。但是在当前 R4 已有 `SubmissionQueue` / `SubmissionTicket` / `RetirementQueue` 的前提下，Legacy 内部旧 timeline serial/Queue owner/command-pool 控制不能原样并排复制。

建议先用 RenderBackend owner-thread + `pollUpload` + 当前 SubmissionQueue 实现一条**真实 Texture + Mesh** UPLOAD-to-GPU 纵向路径；保持从 Server 接受包到 GPU ready 的完整状态。如果可测数据表明独立 CPU staging/recording worker 确有收益，可加入一个**狭窄、仅服务 transfer 的 worker**，但仍向 R4 唯一 SubmissionQueue 提交真实作业或由唯一 queue owner 提交。它不拥有第二个 FrameLoop、Renderer、Layout State Authority 或独立的第二种“完成凭据”。

GPU enqueue 是 Native SubmissionQueue 的批处理边界，不是 C++ `std::async` 或意图队列被推入就算完成。Shader Graph 本来已有 Queue dependency/ResourceUse，Upload-produced resource 用统一 `readyTicket + NativeResourceState + family` 作为 Graph import；不要另建隐式 Transfer Shader dependency path。

## 31.6 测试与性能要求

| 反例/压力 | 必需断言 |
|---|---|
| 64 包/256MiB 默认预算不足时 | 精确 BACKPRESSURED，已准备 Payload/Reply 可恢复，不阻塞 CONTROL/PROGRAM |
| Mesh + Texture 同时上传 | 各自源内存长于 worker consumption，完成结果归正确 owner，不串 AssetId/Generation |
| Dedicated Transfer vs aliased Queue | 两种分支真实 GPU 正确；别名同 handle 没有并发 submit race |
| Transfer family != graphics family | release/acquire、Stage/Access、layout、timeline wait; SyncVal 0 error |
| Texture mip/region/cube | image extent/row pitch/format/layer offsets、chunk staging bytes oracle，无越界 |
| Reupload/Rebind/Delete-in-flight | old backing/tickets 保持到最后消费者，bindless slot 不提前复用 |
| Stop + producer late + partial submit | admission/replies/accepted native jobs drain，资源计数/validation 0 |
| 资源少变/不变 | 不重新传整份 VBO/Texture，不因一个小参数变更而触发 full upload |
| 可比性能 | 包括 CPU 主线程提交延迟、UPLOAD p50/p95、实际 copy bytes、staging 峰值、Graphics GPU stall、VRAM、高压 backpressure |

**阶段入口**：R3-B 先把 ResourcePlan 的 `Upload/Ready/Access` 语义与数据 Scope 固定；R3-C 接正统 Graph import/cross-queue; R3-D 第一条真实 Mesh/Texture UPLOAD + pool/retirement；R3-E Engine/Plugin assets DataProjection 走中立 Lane。任何工作单都不得把 UPLOAD 写成“已有传输能力实现完毕”。

---

## Source: 32_GraphTransient与专业GPUStorage性能账本.md

# 32 — Legacy GPU Storage/Graph 物理规划性能账本与实施约束

## 32.1 两种池与四种生命周期必须区别

**不要将** `ResourcePlan` 中的 Scoped/Global/Scene/View/Attachment **等同于** Native Graph transient 的 GPU backing pool。前者表达专业资源被多个 Effect 共享的逻辑身份；后者表达 GPU 短生命周期 Buffer/Image 的别名与跨编译候选缓存。两者都是优化，解决的问题不同。

| 类别 | 优化源头 | 保留的特性 | 重要负例 |
|---|---|---|---|
| Resource-scope 专业存储 | `MeshResources` `InstanceResources` `TextureResources` `LightResources` `MaterialResources` | 稳定业务槽、结构化 GPU 数据、增量上传、跨 Effect 复用 | 别把每条 Mesh/Light 当 transient 重新分配 |
| Graph transient alias | `RenderGraphCompiler` / `PhysicalResourceAllocator` | 生命周期不重叠+内存兼容复用物理内存；Load/Store & precise sync | 输出/History 被 alias 写覆盖或 alias hazard 不同步 |
| Graph backing pool | `RGVulkanResourceAllocator` | exact TransientResourceKey、VMA image pools、LazyMemory capability、recompile/resize 资源池+GC | 别只按图片尺寸判断可复用，也别复用 GPU-inflight backing |
| Temporal / persistent | `RGBuilder` ping-pong/persistent，`RGGraphResize` | per-View/Attachment Epoch、FIF-independent current/previous、Resize 原子 swap | `frameIndex % FIF` 不等于所有 History 窗口都兼容 |

## 32.2 已审的代表性成熟优化

- `TGpuBuffer` 的 CPU_TO_GPU/GPU_ONLY 策略、HostMirror optional、epoch Dirty、reused `hm_upload_scratch_`、coalesced runs、non-coherent flush、FIF slice 与 GPU-only reserve migration。
- `TPagedGpuStream` Dirty-page runs 的 reused scratch，`TStableRecordPages` 4096-record 数据地址稳定；Instance 三种主流数据流及 GPU live/dynamic worklists，避免 CPU 每帧全量构建重映射。
- `ArenaAllocator` 的 VMA Virtual TLSF/free/coalesce/defrag plan，`ChainedArenaAllocator` 的 segment growth/returned freed blocks；`MeshResources` VBO+IBO segment 与 LOD/mesh GPU ready。
- `BindlessCombinedSet` 的 Generation/Slot deferred recycle、per-FIF safe replacement、texture mip feedback/requested residency、region/cube upload 与 staging chunk；`MaterialResources` 的 per-family SlicedSSBO+GraphFamily、bucket PSO 重用、parameter dirty revision。
- `PointCloudGlobalBuffer` 的 chunk slot growth/reset/clear/use-after-free protection，`TransferScheduler` 的 ring/overflow staging 和 batching；GPU stream read/write 的 Native owner 与 CPU source authority 明确分离。
- `RGVulkanResourceAllocator` 的 transient VMA pool + LAZILY_ALLOCATED 专用池，没 lazy memory 时仍用 ordinary pool；Pool cache 的成本/VRAM 上限和 LastUse 必须可观察。

## 32.3 为什么不能把这些压成 UniversalGpuStorage<T>

多个存储的真实 Shader ABI 不同：Mesh 多段 VBO/IBO/LOD、Instance stable BDA 页表/indirection、Material graph params/texture slots/PSO buckets、Texture bindless image/descriptor/mips、PointCloud chunk & octree。统一对外身份/创建/获取/绑定合同是正确的；统一所有内部 `upload(bytes)`、slot recycler、allocator 和 ready process 会丢掉优化甚至改变 GPU 数据布局。

建议的共通接口只在 Asset/Scope 层：`getOrCreate<T>()`（冷态、配置兼容）、tryGet、`ResourceRequirement/Ready/LastUse/BindingProvider`（结构与引用）以及纯 Native `Buffer/Image` 原语。具体业务保持 `updateTransform()`、`replaceMipRange()`、`uploadChunk()`、`updateMaterialParameters()` 等 typed operations；C++20 concept 约束 Factory/Binding outputs 而不要求热态 Storage 继承虚基类。

## 32.4 保留/优化/淘汰的客观准则

每类 Legacy 机制做 `Source → current R4/modern equivalent → retained algorithm → explicit adaptation → validation → benchmark evidence` 六列。没有真正差异和收益则复用现有实现或最小整合；改算法必须重跑数值、VRAM、dirty bytes、CPU/gpu timing 和 allocation，注明平台/设备和路径。对未验证的设备性能不得声称“更快”。Hash-based authoring/toolchain temporary lookup 不等于运行时对象 ID；新所有权身份仍必须通过 lux-cxx `SlotKey`，冷态 cache 的全结构相等才是正确性依据。

本章具体原始接口参看第 23/29/30/31 章；逐项测试索引在 `LegacyCompilerMechanisms.csv`、`MaterialGraphContracts.csv`、`UploadTransportCases.csv` 与旧 `LegacyGpuAlgorithms.csv`。
