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
