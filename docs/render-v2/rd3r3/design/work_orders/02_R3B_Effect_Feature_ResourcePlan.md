# R3-B — RenderEffect + Feature 作者模型 + Compiler ResourcePlan 纵向闭环

**状态：未授权；须先审核 R3-A 实际 I/V。** 入口 `03/05/06/07/08/11/14/15/19/22/26`。目标不是写一套抽象齐全的 SDK，而是让两三个真实 Feature 从数据需求到 Graph 再到 Native output 可以跑通。

## 任务

1. 令 Backend SlotMap 的 RenderEffect 真正持一份 logical RenderGraph 与 Feature 作者配置/必要 CPU state；移除固定 `vulkan::EffectSource` 作为唯一可编辑 author 模型。native compiled `RenderGraphId` 仍由单 DeviceSession 分配；保留 P2-A 全结构 key / Shader/PSO 的共享事实，不建立另一个逻辑 GraphCache。
2. 定义最小 C++20 GraphAuthor concept 和 `FeatureOps` 可选 cold Hook；用 `GridFeature` 与一个实际 Compute/Lighting/Tonemap Feature 验证。纯 Feature 无 DeviceSession；需要 Native caps/prepared resources 的 Feature 用 narrow DeviceContext/ResourceContext 在 Native phase 创建，不能 Effect 创建时抢占 Scene A 的 LightStorage。
3. Compiler 用 Feature 资源需求 + DataSchema + PassSchema + Graph ResourceUse 生成 immutable `ResourcePlan`，校验 Scope/Type/Key/GPU ABI/required optional/Provider/Writer/fallback/Retain；C0–C8 复用原算法。Native C9–C13 只接入 ResourcePlan 的相容 requirements，不新建 compiler dispatch system。
4. Attach 在 owner-thread 用已验证三元组解析 ScopeKey、惰性创建真实外部 Buffer、获取 RAII Lease、准备 index-based Native Bindings，再原子发布 Attachment；非 OOM 错误保留 last-good。GPU record 正常不再调用 ResourceRegistry/hash/find<>()。
5. Effect 结构修改前先构建全部候选 Graph/Plan/context 与受影响 Target 的闭包，owner-thread 串行 safe publish；失败全量回退。Hot GridData/Exposure/Scene Light values 只更新数据，不重新编 Graph。

## 必须有的例子

- 2 Scenes/4 Views 使用同一个 Effect/Compiled Graph，绑定不同 Scene Light/GPU View values，输出不同正确像素；另一个同构 Effect 可共享 Native program 但有不同 hot defaults。
- 一个 Scene `Light.main` 和 `Light.preview` 同型不同 Key，分别创建且不会被 compiler 合并；两个 Feature 都引用 `Light.main` 只分配一份 Scene scope owner。
- 缺 required provider、多个 Writer、无真实 access 却插 barrier、不同 ABI 资源冒名、Shader owner layout 冲突等全部 cold error。
- attach 热重建候选过程中 Mock non-OOM missing capability/Shader ABI failure 保持旧 Effect 和所有 View/Target 一致。

## 禁止

先迁完整 36 个 Legacy Feature、插件 ABI 大全、跨多个 Target 完整 WSI 合成、每个 Feature 一个独立 CPU Proxy/Renderer、Feature hot `this` callback 录制、每帧重新 resolve Scope。未实现的 H1–H6 native primitive 不可用枚举占位标记通过。I/V/STOP；未来 R3-C 须另授权。


## 本次新增的强制源码/验证输入

先读第 29、30、31 章：GraphBuilder 完整 input/out/conditional/sideEffect/forwardRef 合同；MaterialGraph 是独立作者图，只通过 cooked Shader/Param contract 注入 Feature PassSchema；ResourcePlan 的 External/Upload-needed/Ready semantics 不得因为中立 Graph compiler 无 VkQueue 就消失。至少加 `eliminateStarved` before dead、同型不同 Key 的材质/纹理、Material 参数更新不重编 Shader 和缺外部 Producer 的负例。当前 Material 工具链已有模块，禁止新增第二 Node parser。
