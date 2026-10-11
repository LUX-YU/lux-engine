# 06 — Concrete Feature、Data Provider/Consumer、Capability 与组合事务

## 6.1 Feature 的唯一职责

一个 Feature 是具体算法/行为单元：可响应 typed events、准备 Frame Input、声明 Graph Pass、提供/需求 Scene Capability 或 DataSet。**Feature 本身不等于数据 Storage、Renderer、FrameLoop 或 Vulkan owner**。Passive 数据提供者可以没有 Pass；纯后处理 Consumer 可以没有 CPU 持久状态；新 Feature 不需要从 `IRenderFeature` 虚基类继承。

`DataType`（数据结构） ≠ `DataSet`（一份数据实例） ≠ `SceneCapability`（功能契约） ≠ `FeatureType`（行为类）。例如 `LightGpu` 是 Type；Scene A 的 `Light.main` 是 DataSet；`HasSceneLights` 是 Capability；`DeferredLightingFeature` 消费 Dataset 且声明 Compute Pass。一个 DataSet 可以独立于 Feature 保留，多 Consumer 不必拷贝一份。

## 6.2 先做数据依赖解析，再造唯一 Graph

**确定性冷态 Pipeline：**

```text
installed Builtin/Plugin Feature descriptors + Scene/View/Target configuration
       ↓ [capability requirements, data demands, writer authority]
Composition candidate: resolve {Scope, DataType, DataKey, Version, Access}
       ↓ [cold error on missing required, ambiguous writer/provider, ABI mismatch]
Type-safe **logical** DataSet slots + frozen neutral StorageInterfaceShape;
no VkBuffer required at authoring time
       ↓ [Feature declares actual per-scope GPU Pass authoring and required accesses]
RenderGraphBuilder + generated PassSchema accesses + typed producer outputs
       ↓ [unify data-derived edges with versioned pass reads/writes/after clauses]
RenderGraphDefinition → existing compileLogicalGraph [C0–C8]
       ↓ [C1 graph producers/semantics/import-export; C2 pass resource interfaces only]
       ↓ [solve NativeStoragePlan from real usage under frozen interface ABI]
C9 Layout/Shader; C10 physical backing; C11 native pass recipes;
C12 queue/sync; C13 executable assembly [existing responsibilities]
Complete candidate (data bindings, resident/temporary resources, executable plan)
       ↓ backend safe publish; old candidate continues until GPU/CPU/code safe
FrameExecutor [Scene-scoped once when proven; View-scoped per view]
```

**关键反例**：`Feature A requires<LightGpu>()` 并不代表它的 GPU Pass 必须 `after(LightFeature)`。如果 Light 被 CPU Dirty-upload，一次 `ready ticket + Graph import state` 是它需要的 GPU 依赖；如果 Light 是 GPU Compute 写入，则应通过真实 `GraphResourceVersion` 和 Pass Producer 建 RAW edge。没有实际 GPU 读写的 Feature dependency **不能**自动产生无意义的 GPU barrier。

## 6.3 数据需求合同

一项需求必须明确：

- 预期 `DataSchema<T>`、稳定 ID/ABI revision；
- DataSet Scope（Runtime/Scene/View）和 Key/Selector；
- 必需或可选、可选时明确 fallback/empty behavior；
- CPU Read/Write 或 GPU Read/Write 权限，单 writer authority/可并行区间策略；
- 使用时点（cold attach / per Frame GPU read / dataset change callback）；
- 对其他 Capability 的 required/optional；
- 是否允许复用同 Scope 的现有 DataSet，多个 Provider 冲突的确定规则。

不允许“按类型查找第一个 Storage”作为默认行为，尤其在一个 Scene 具有多个同 T Dataset 时。若作者省略 DataKey，仅当匹配的数据集恰好一份且 API 合同明确定义 Default 时才合法；否则应返回 Ambiguous，而非以注册顺序选第一份。

## 6.4 目标 C++20 Feature 模型

```cpp
class DeferredLighting final {
public:
    // 真实最终拼写须经 F6 设计/安装 SDK 编译验证。
    static constexpr auto needs = data_requirements(
        required<LightGpu>(scene_scope, "main", gpu_read),
        optional<ShadowAtlas>(scene_scope, "main", gpu_read));

    RenderResult<void> declareViewPasses(
        const ViewGraphContext& view, RenderGraphBuilder& graph) noexcept;
};

// has optional hook → if constexpr 编译期生成对应 fixed FeatureOps；
// 没有 hook 就是 nullptr，不生产 dummy manager/base method。
```

`needs` 只记录静态类型层面的需求，`declareViewPasses()` 消费 cold-bound DataView 而非每 Frame 再通过字符串查 Registry。对于可由 GPU Pass 产生的数据集，Feature 提供的 typed output slot 在 Graph 中成为一个明确可用的 `GraphValue`/Producer Version。

## 6.5 Feature 之间的 Graph 依赖如何确定

1. **数据源归属**：同 DataSet 的生产者在组合阶段唯一确定（或显式 merge policy），不能隐式选择最近安装的 Feature。
2. **访问映射**：Composition 用已解析 Typed DataView 与 Generated PassSchema 将字段降低成中立 Graph Buffer/Image range、read/write、stages 和 producers。Graph 不查 DataSet Registry；实际实例/backing 留到 Invocation 绑定。其它 Attachment/Indirect/Copy side effects 也由同源 typed params 描述。
3. **producer versions**：DataSet 如果在 Graph 内先写后读，读 Pass 要引用该写的版本或显式“latest if unambiguous”。多个可能 Writer 必须 cold-fail ambiguous，不因 `feature list` 顺序决定。
4. **ordering**：无资源依赖但有真正业务顺序（透明合成、特定工具覆盖层）由受限 typed order contract 表达，并经 C0–C8 cycle check；不得建 `Stage enum` 保证所有 Feature 进入固定模板。
5. **conditional**：Feature 可跳过时，消费者必须有合法上帧数据/可用 fallback 或由新组合候选替换 topology；绝不读未初始化资源。
6. **dedupe**：Scene-scope Pass 只有在真实 input scope/revisions/epochs/side effects 可证明相同才跨 View 只执行一次；Gpu Multiview 是另外的物理优化。

## 6.6 注册/装配/卸载事务

`registerType/feature → prepareAttach → validateComposition → compile candidate → publish atomic → old retain/retire`。同一 Feature 可声明多个 DataSet Output、且可以消费自己或他人的 DataSet。对于 DataSet 而非 Feature 的生命周期：解绑最后 Producer 不意味着删数据；按 Scope/Retention Policy 决定，卸载插件前 Host 必须复制 Schema 并对仍使用插件 ABI/code 的 Callbacks 保留 Code Pin。

错误：required 缺失、同 key 不兼容类型/布局、多个 Writer、循环依赖、跨 Scope 偷读、缓存错误复用、Feature 卸载后仍持有裸指针、unready backing、无授权写入；编译/attach 阶段拒绝且旧 Graph/Store 不受污染。

## 6.7 业务 Example：2D 甚至无需 Instance/Geometry

`Sprite2D` Dataset 可以直接由 Editor/Canvas Producer 维护；`SpriteCullFeature` 消费 Sprite 和 Camera2D，输出 View-scope `VisibleSpriteSlots`；`SpriteDrawFeature` 消费 GPU visible result 和 Atlas，生成 Indirect draw；`UICompositeFeature` 消费该 View 的 color 与当前 Target。没有 Mesh, 3D Transform, GeometryKind 分支，也没有独立 UI FrameLoop。Shader 物理资源来自 DataSet/PassSchema，而非 Feature 自建 Descriptor Set Authority。

## 6.8 Cold Contract Identity 与 Runtime 实例不可混同

Feature 依赖解析以 Scope+Type+Key 确定当次运行中实际 `DataSetId`；Shader/Layout 冷态建立**结构性** `BindingSlotKey` 与 DataSchema full shape，不能把具体 DataSetId 写进共享 Pipeline Layout key。若 Scene A/B 都使用 `Light.main`、相同 shader/target/layout，可以共享一份 immutable Plan，而在各自 FrameBindings 中绑定各自 Native backing；如果同一 Pass 使用 `Light.main` 与 `Light.preview`，Schema Type 虽相同，但 BindingSlotKey 不相同，必须正确分成两个资源槽。版本/ready/generation 只负责 actual invocation 及输出工作共享的合法性。
