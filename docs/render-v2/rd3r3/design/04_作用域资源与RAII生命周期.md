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
