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
