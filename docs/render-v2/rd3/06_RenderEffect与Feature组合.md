# 06 — RenderEffect、无状态优先 Feature 与共享 Graph 编译事务

## 6.1 RenderEffect 是算法配置的唯一 Owner

`RenderEffect` 是独立于 Scene 的、Backend-owned、稳定 `RenderEffectId` 的可编辑渲染资产。其拥有 Feature 的类型描述/配置，以及必要的纯构建期状态/插件 Code Pin。Feature 构建过程生成中立 Graph Definition；Graph/Shader/PSO/Native Context 不引用 Feature 实例的 `this`。同一 Effect 可以被不同 Scene 的任意多个 View 使用，修改一次 Effect 对所有相关 View 同时生效。

`RenderGraphId` **不是** `RenderEffectId`：Effect E1 同时有 HDR/SDR Native 变体；E2 可能与 E1 产生结构相同的 Graph。缓存可共享不可变 Native Code，但不合并 Effect 编辑身份和 Hot 参数权限。

## 6.2 Feature 与 Effect 生命周期

Feature 不默认安装到 Scene；Scene 不拥有 Feature 或 CompiledGraph。无状态 Feature 可以只有 `static build(builder,settings,contracts)` 的调用；需要保留配置的具体 Feature 由 RenderEffect 管理。编译候选的短期 Feature 实例可以构造后即析构。Graph 构建完成后，全部跨线程/插件代码的 native callbacks 只能是被显式验证与 pin 的独立内容，不能捕获临时 `this`。

允许 resource-only 行为以普通资源初始化请求/ResourceDomain 处理，例如 LightProvider、Mesh storage；不为“创建一个 LightBuffer”强制安装 `LightFeature`。Scene/View 的长期 GPU data 与 RenderEffect 无关；Feature 删除不影响其他消费者仍在用的资源。

## 6.3 C++20 作者接口与共享算法

```cpp
// 目标 API，SDK 编译示例阶段需进一步冻结
struct MeshCull {
    static RenderResult<void> build(RenderGraphBuilder& graph, const MeshCullOptions& o) noexcept;
};
struct DeferredLighting {
    RenderResult<void> build(RenderGraphBuilder& graph, const LightingOptions& o) const noexcept;
};

RenderResult<void> buildGpuCull(RenderGraphBuilder&, const CullConfig&) noexcept;
RenderResult<void> buildIndirectDraw(RenderGraphBuilder&, const DrawConfig&) noexcept;
// Forward / Deferred / MeshShadow / PointCloud 使用公共函数、typed config 或组合模块。
```

作者可以使用普通函数、组合 C++ 配置、必要时 CRTP/继承去复用真正相同的算法与状态；系统**不要求**实现 `IRenderFeature` 虚基类。动态插件/Feature 类型装配仅在 cold boundary 用 generated `FeatureOps` 指针表：存在可选 `build/prepare/detach/params` 才生成相应函数指针，缺席为 nullptr，热态 Graph Submit 不经过它们。

## 6.4 Feature 需求合同，但不决定数据 Owner

每个 Feature 声明需要的：类型/schema/version、资源 semantic/key、读写角色、可选/required/fallback、可用设备能力、目标语义输出、特定 Pass 的排序约束。冷态 Feature composition 只解析结构性资源 interface 和合法 Provider；真实 Scene GPU backing 留待 View 的 Domain 链在绑定时获取。`required<LightGpu>` 不等于 LightFeature 必须存在于 Scene，亦不自动产生 `after()` GPU Pass/Barrier。

Graph 只有对相同 Buffer/Image byte/mip/layer 真实 `GraphResourceUse` 的 producer/version 和 RAW/WAW/WAR 冲突才生成边；纯 CPU-owned data upload 则在 Native import 的 Ready Ticket 证明。两个只读消费者可并行，两个重叠 GPU writer 必须有显式生产版本/先后/范围分区；不从 Feature 附加顺序选 winner。

## 6.5 Effect 更新：行为矩阵

| 修改 | 是否重编 Graph | 是否影响全部共用 Effect 的 View |
|---|---|---|
| 改 Scene A 灯光数值/变换 | 否，Scene Domain 增量上传 | 不影响 Scene B 的数据，GraphId 不变 |
| 改 E1 Tonemap Exposure/Gamma | 否，更新 E1 Hot 参数快照和 GPU Binding | 是，后续同一版本帧统一生效 |
| 为 E1 新增 Shadow/SSAO 或改变算法变体 | 是，按 Target/Device signature 找或建新 Graph | 是，事务发布到所有引用 E1 的 View |
| View A 改选另一个 Effect E2 | 可能复用已编译图，无需自动重编 | 只影响 View A |
| Effect E1 删除 | 先撤销新引用，拒绝仍被 View 依赖的直接删除或显式解除绑定 | 其他 Effect 的相容 Graph 不受影响 |
| Feature 插件卸载 | 禁止新组装，旧 code pins/GPU last-use 完整退休 | 受影响 Effect 先成功替换/下线 |

运行时效果修改必须建立 `candidate → compile → check all active consumers → atomic publish/rollback`；冷态 Shader/layout 结构完成后才能重建 Binding Context，不允许 Graph 自身可变以“省重编”。

## 6.6 Feature→IR 的完整组装路径

```text
Effect(config/revision, list of Feature type+settings)
 → generated FeatureOps(only present hooks), capability/required resource resolution
 → RenderGraphBuilder contribute passes, explicit target outputs, all intermediates
 → immutable Definition
 → RenderGraphCompiler C0–C8
 → RenderGraphIR
 → Native Compiler (shader/layout/resource/recipes/sync) per compatible signature
 → Backend Graph Cache (RenderGraphId)
 → View EffectBinding + Context binding, grouped execution
```

No `FeatureManager/EffectGraphStateManager/SceneFeatureSet` authority. Backend 中可以有 `SlotMap<RenderEffect>`，Effect 内部可以有 `vector<FeatureEntry>` 这种内部值记录；不存在额外全局 Feature cache 负责 Scene 安装状态。

## 6.7 与 Legacy 的真实功能对照

Legacy `RenderFeature` 除 addPasses 外负责 attach/detach、per-view state、onFrameBegin、rebase、hot params、sampled target、CodePin；在新模型中：资源初始化归 ResourceDomain/协议，View Camera/History 归 View Context，动态参数归 Effect 或 Scene/ View 资源，Graph Pass 构建归 Feature，跨帧模拟来自 ECS/projection，场景原点重定位是 Scene 数据事务，Target 结果由 Graph 显式输出，CodePin 由 Backend 注册/Native retirement 管。它们都不是删除需求，而是移动到正确 Owner。

Legacy `GpuDrivenMeshFeatureBase`、`PCFeatureIndirectBase` 共享大量 Cull/Indirect/PSO 逻辑；新系统将公共部分变为按真实资源合同工作的 C++20 纯算法，避免多层 Base 类复制 Native owner。插件既可新增数据 T 又可新增 Feature，不需要增加核心 `switch (FeatureTypeId)`。

## 6.8 特殊 Scene/View 需求

同一个 Graph 在 Scene A/B 中解析不同的 `LightBuffer`、`Transform`、Material catalog 和 Camera：Graph IR 的每个 Import/Binding 要求稳定的类型与 semantic shape；初始的 Domain 继承查找只在 Effect→View 绑定、Domain GPU backing 变更或 cold graph replacement 时执行，热 Record 只用具体验证后的 binding slot。Feature 可以声明不能在无 Camera 等情形运行，并提供合法 fallback 或候选拒绝，不能让 Executor 在热态猜测缺失资源。
