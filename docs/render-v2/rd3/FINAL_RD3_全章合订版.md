# Render V2 / RD3 — 全章合订版

> This is a generated concatenation of the 21 independently rewritten chapters. Original zip and Git implementation remain unchanged.

## 目录

- `00_架构总纲.md`

- `01_设计哲学与成熟引擎对照.md`

- `02_模块分层与所有权.md`

- `03_数据描述与类型身份.md`

- `04_ResourceDomain与GPU资源.md`

- `05_通信与发布事务.md`

- `06_RenderEffect与Feature组合.md`

- `07_RenderGraph推导与编译.md`

- `08_Shader与布局自动生成.md`

- `09_NativeCompiler与Executor.md`

- `10_SceneViewTarget与ViewGroup.md`

- `11_插件SDK与扩展边界.md`

- `12_Legacy功能完整迁移.md`

- `13_先进渲染兼容性.md`

- `14_Cpp20类型和编码纪律.md`

- `15_端到端API示例.md`

- `16_性能模型与可比实验.md`

- `17_完整验收与正确性矩阵.md`

- `18_阶段计划与交付门禁.md`

- `19_RD3架构裁决.md`

- `20_RD2到RD3迁移对照.md`


---


<!-- SOURCE FILE: 00_架构总纲.md -->


# Render V2 / RD3 — RenderEffect、共享 RenderGraph 与层级资源域：设计总纲

> **状态：完整候选方案（2026-10-12），尚未获生产施工授权。** 原包 `RenderV2_RD2_Architecture_and_Implementation(2).zip` 全章重写，不在旧类型上堆一层 facade。**本文件是未来架构规范，不是现有源码已实现的声明。** 审查仓库：`LUX-YU/lux-engine`, `codex/render-v2`, HEAD `246fe21eb6d060801b9a16ddcadde2be89dbbecc`。Frozen Legacy 原基线：`a669409a289a6fa4092f21176397795b1cdb7f3e`、719 文件。

## 0.1 一句话终态

**RenderEffect 描述可以跨 Scene/多 View 共享和修改的渲染效果；RenderFeature 是它的无状态优先构建算法；RenderGraphCompiler 生成平台无关 IR；各 Native Compiler 将 IR 编译为不可变的、带 Native 资源与执行配方的 Graph；RenderBackend 以 RenderGraphId 管理缓存并将相容 View 分组，Executor 借每执行配置的 GraphExecutionContext 运行 Graph；单次 execute 产出明确 Target 的最终画面。**

Scene 持有 View 及 Scene 语义；View 选 RenderEffect 而非独占 Graph；Source Engine ECS/资产系统为 CPU 业务数据权威；Backend 持有层级 ResourceDomain、GPU 存储和 Native Ticket。RenderRuntime 是协议 Client，而不是 Vulkan 对象的容器。

## 0.2 不可变设计决策

1. `RenderRuntime`：Client/PROG-CONTROL-UPLOAD 协议入口。`RenderBackend`：Server 端逻辑、所有权与独立帧节奏。`backend/vulkan/Backend`：选定设备的 Native 实现。C/S 是依赖与通信边界，不要求跨进程。
2. `Scene`（Backend-neutral）管理 View 的逻辑生命周期；`View` 只有稳定 `RenderEffectId` 选择权威，当前 `RenderGraphId` 是 Backend 的派生执行绑定。`Target` 与 Scene/View 独立，由 Backend 拥有。
3. `RenderEffect`（稳定 ID、可编辑配置、Feature 构建组合）归 Backend；`RenderGraphId` 标识特定不可变 Native 编译产物，Graph 缓存归 Backend，同一 Graph 能由不同 Effect/Scene/View 使用。
4. `RenderFeature` 非强制虚继承：C++20 普通对象/函数，按需持 Effect 配置；Graph 只保留自有型数据与 Native owner pin，不捕获 Feature `this` 或插件裸函数指针作为热录制语义。
5. 资源通过 `ResourceDomainId` 的树归属：Root→Scene→View 是典型形状，不按 C++ 类型强制 Global/Scene/View 分类；子域可读祖先、不可隐式写入父域或兄弟域。`ResourceRegistry` 唯一通用实现。GPU 物理 backing 的最后存活期由 Native RAII/真实 completion 决定。
6. 逻辑 Graph：`Builder → Definition → RenderGraphIR`，只包含 GPU pass/use/resource/version/order、输出合同、Shader/ABI 结构性需求。Vulkan Native Compiler C9–C13 生成不可变 `vulkan::Graph`。`vulkan::Executor` 通过 `GraphExecutionContext` 的 FIF slots、bindings 和 tickets 执行。
7. Graph 内部的 GBuffer、Shadow intermediates、HDR、Tonemap 等都在**同一张 Graph**中声明、排序、复用和清理；`execute(graph,view,target)` 的成功提交意味着画面将在 GPU 完成后成为该 Target 最终输出，不能藏一个未经 Graph 声明的后处理 Graph。跨帧 history、外部摄像头或另一个 Offscreen Target 的**最终产物**只有显式导入才能消费。
8. 多 View 共享单 Effect 更新时：构建所有需要的 Native Graph 变体、确认所有活跃消费者的资源/Target/Context 合法，安全点整体发布；失败保留 last-good。独立 Scene 数据更新不编译 Graph，Effect hot 参数修改也不必重编 Graph。
9. 高级 UE-like 功能需求全保留，但不仿写 Epic 私有算法、不提前发明 Universal Material/Instance/Renderer/CompilerPassManager。原功能 `F01–F36`、基础 `I01–I44`、先进 `H01–H06` 共 86 项；`W01–W25`、`REQ01–REQ51` 逐项追踪。
10. 不在当前阶段重新开放已经正确的 C0–C13 算法、R2 Transport/R4 Native 核心；**允许推翻类型命名与容器结构**，但应复用已验证行为，任何下层改动必须追踪受影响 GPU/性能资格。

## 0.3 整体调用与拥有关系

```text
Engine ECS / Assets / Editor / Plugins (CPU authoritative)
                 │ typed deltas + retain/release
                 ▼
       RenderRuntime [client]
                 │ PROGRAM / CONTROL / UPLOAD + replies
                 ▼
       RenderBackend [server, owner/thread]
       ├── Scene A → View A1/A2 → EffectId E1
       ├── Scene B → View B1    → EffectId E1
       ├── Effect E1 → Features/configuration
       ├── GraphCache → GraphId G1/G2 (immutable, shared)
       ├── ResourceDomains → Root/Scene/View registries
       ├── Targets → offscreen / surface, output contracts
       ├── ViewGroups: GraphId → prepared execution jobs (non-owner)
       └── backend/vulkan/Backend
           ├── Device/Allocator/Queues/Retirement
           ├── Compiler(IR) → vulkan::Graph
           └── Executor + GraphExecutionContexts
                  └── execute(Graph,View(s),Target) → FINAL output
```

## 0.4 概念与身份不能混淆

| 名称 | 精确定义 |
|---|---|
| `RenderEffectId` | 一份可编辑 Effect 配置的稳定身份；改版本不改 ID |
| `RenderGraphId` | 设备内一个经过编译、不可变、相容签名固定的 Native 程序；重编/特化可能得到新 ID |
| `SceneId`、`ViewId`、`TargetId` | Server-owned 生成式资源地址，Client 通过协议持有，不是裸 C++ 指针 |
| `ResourceDomainId` | 归属/层级查找/资源撤销；不等于 Descriptor Set 或 GPU 资源句柄 |
| `GpuResourceId` | 后端内某一 GPU 资产引用的语义/生成式身份，与物理 Buffer/Image 不同 |
| `GraphResourceId` | 一个 Graph IR 内的本地虚拟资源号，不跨 Graph/Frame 通用 |
| `DataSchema<T>` | 值、Shader GPU ABI 和可选 Wire 描述；不等于 CPU 数据镜像实例 |
| `PassSchema<P>` | Pass 的 Shader 参数和实际读写角色；不等于 DataSchema |
| `BindingLayout` | Shader/Descriptor 结构性事实；绝非 Domain owner 的另一个名字 |
| `GraphExecutionContext` | 绑定到执行配置的可变物理 slots、descriptor、scratch、历史和真实 GPU tickets |
| `SubmissionTicket` | 已提交/完成 GPU 使用的证据；FrameSerial、修订版不替代它 |

## 0.5 文件阅读顺序和权威

01 哲学 → 02 类型/所有权/模块 → 03 Schema/IDs → 04 Domains/资产 → 05 协议 → 06 Effect/Feature → 07 通用 IR → 08 Shader/Layout → 09 Native 编译/执行 → 10 Scene/View/Target/帧 → 11 SDK → 12 Legacy → 13 H1–H6 → 14 C++20 → 15 端到端 → 16 性能 → 17 验收 → 18 阶段门禁 → 19 决策 → 20 完整迁移；另见 appendix 四个更新后的矩阵、work_orders、合订版和 SHA 校验。

本 ZIP 只规定**未来目标**；仓库历史 Implementation/Verification Git SHA 和 I/V/STOP 协议不可变。当前 F6.b 功能/ABI/Normal/ASan/GPU 测试已验证但总体仍 `PARTIAL`（Record/Submit p95 不确定、冷态 Layout 回退），F4/F5 历史债亦保留；F6.c 未获授权。新方案获审阅并明确批准后，再单独做 docs-only 设计门禁和受控实施。


---


<!-- SOURCE FILE: 01_设计哲学与成熟引擎对照.md -->


# 01 — 面向可扩展渲染效果的设计哲学

## 1.1 我们真正要优化什么

不是最大化接口/类型数量，而是让作者能通过 Effect 与 Feature 组合任意 GPU 工作、让一个 Graph 跨 N Scene/N View 共享，且新功能无需向 Scene、Graph 核心和 Frame 主循环加入业务 `switch`。代码外观优先简洁可读：类型代表真实拥有或不变量；数值配置用 `struct`、算法用普通函数，生命周期由 RAII 和类型/生成式 ID 保证。

**承认 Vulkan/DirectX/GL 的差异**：通用 Graph IR 保留访问、依赖、输出和能力需求；具体 Backend 完成设备协商、Pipeline/Shader 编译、Barrier 和命令录制。多 Backend 不等于制作 N 份 Scene/View/Effect 框架，亦不强制 GL 模拟 DX12/Vulkan 多 Queue 原语。

## 1.2 从 Legacy 继承什么、明确放弃什么

保留：Scene 持有 View、Target 有独立语义、Feature 的 Pass 声明、事务式 Graph 编译与 LastGood、Pipeline 缓存、真实 GPU 驱动的 Mesh/Canvas/PointCloud、Resource 共享与延迟释放、Editor 插件能力。**不保留**：Scene 对 RenderGraph 的独占权威、Feature 默认拥有 GPU Light/Mesh 等跨消费者数据、编译图长期捕获 Feature `this` 的 Native 回调、EFieldOwner 固定槽决定真实资源归属、多个业务专用 FrameLoop。

已验证的 V2 机制必须继承：C0–C8 精确 Hazard/Scope，C9–C13 Native Shader/Layout/Sync2，F5 多 Scene/View 下共享 immutable code 与独立 Invocation，F6.a DataSchema 和 F6.b 多 Owner Binding。新的名词不能成为外部转发 Facade；真正的数据和算法应迁移到最终 Owner 下。

## 1.3 五个不可逆的结构原则

1. **业务数据与执行程序分离**：Scene/View/Root 提供资源，Effect 提供算法，Graph 是纯编译产物，Executor 组合它们。不论同一图被几个 Scene 使用，都无权在 Graph 内引用某个 Scene 对象。
2. **编辑配置与编译代码分离**：稳定 EffectId + 修订版 vs 不可变 GraphId + Native 兼容签名。前者修改是候选事务，后者没有可变 Pass 序列。
3. **逻辑生存与物理存活分离**：CPU `release` 使业务引用失效，不说明已通过 GPU/Present/Plugin 最后使用；缓存策略可后续发展，但当前必须有正确 Retain/Release/Retire。
4. **冷态结构解析、热态数字化执行**：冷态 Registry/Schema/Feature/Shader 解析；热态使用已冻结的资源槽与 Native 配方，禁每 Pass 反射或 TypeID 查找，稳定拓扑时 CPU 首方零分配。
5. **扩展算法而非扩展中央控制类**：H1 Nanite-like、H2 GI、H3 TSR、H4 VSM、H5 Material/Volume、H6 3DGS 用现有 Effect→Graph→Backend，不引入功能专属第二 Renderer。

## 1.4 公开作者体验示例

```cpp
// 目标 API 示意，不是当前可编译源码
RenderEffectBuilder effect;
effect.add<DepthPrepass>(depth_settings);
effect.add<GpuCull>();
effect.add<DeferredLighting>(lighting_settings);
effect.add<Tonemap>(tonemap_settings);
auto effect_id = runtime.createEffect(effect.finish()); // 异步 request 在真实实现中封装
runtime.setViewEffect(viewA, effect_id);
runtime.setViewEffect(viewB, effect_id);
// 修改算法配置同时影响两 View；更改 Scene Light 则完全不用重编译 Graph。
```

一个 `Feature` 不必继承虚基类；对共享 Cull + Indirect 的算法，使用普通 `buildMeshCullPasses(builder, params)`；共享的 GPU Buffer 由 ResourceDomain 拥有而非复制到各 Feature。若两个 Feature 真有类型/不变量上的继承关系，可正常使用 C++ 继承，但系统扩展点不强制继承。

## 1.5 反例与非目标

- 不增加 `GraphInstanceManager`、`FeatureLifecycleManager`、`ResourceDomainManager`、`TargetCompositionManager` 以及仅改名的 Impl/Adapter。
- 不新写第二套 `ICompilerPass/PassManager/Registry/Factory` 框架；LLVM 风格在这里指固定可验证的分析/Lowering 阶段，**不是**给每个业务 Feature 一个 Native Compiler Pass。
- 不给每个 Feature 附一套数据 GPU cache 或手工 set/binding；不把 DataSchema 当作第二 ECS。
- 不编译所有高级 Native 原语以供未实现的业务使用；DeviceCaps 的 supported/enabled、fallback 和实际性能由真实新消费者驱动。
- 不将 FrameSerial 和 CPU revision 当作 GPU Completion；不用全设备 waitIdle 处理普通 Frame Rebind。

## 1.6 成熟引擎对照只是机制来源

Unreal RDG 的 Pass/资源声明与资源使用驱动同步、EnTT 的类型资源查找、LLVM IR→Backend lowering、Vulkan Device/Queue 真实状态追踪，都能提供可借鉴的设计原则，但**不是**移植目标，也不等于性能证明。候选实现需要提供同负载 GPU oracle、真实编译/绑定成本、显存峰值和端到端 Editor 质量。先进视觉目标与架构可扩展性不可互相冒充。


---


<!-- SOURCE FILE: 02_模块分层与所有权.md -->


# 02 — 模块 DAG、类型边界、唯一所有权与线程

## 2.1 完整、无重复的模块层次

```text
render/core       identity, RenderResult, common contracts
render/transport  R2 PROGRAM/CONTROL/UPLOAD + Reply + diagnostic lanes
render/client     RenderRuntime (无任何 Vulkan/D3D12 public include)
render/scene      Scene, View 中立对象和逻辑状态
render/effect     RenderEffect、Feature 构建合同、FeatureOps (cold)
render/resource   ResourceDomain、唯一 ResourceRegistry、强类型资源 Handle
render/target     Target、TargetDesc、TargetLayer、输出格式/语义
render/data       DataSchema<T>、Generated GPU/Wire representation
render/graph      Builder、Definition、通用 Compiler、RenderGraphIR
render/backend    RenderBackend（中立 Server，拥有对象表和帧入口）
render/backend/vulkan   Device/Resources/Graph Compiler/Graph/Executor/Target backing
render/features   Mesh/Light/Shadow/Postprocess/Canvas/... 业务模块
engine/scene/...  RenderSystem / Client ECS projection（在 render 模块外）
engine/toolchain/shader 现有唯一 Shader/Meta 生成链
```

依赖总方向：`client` 只依赖 Core/Transport/协议公共描述；`scene/effect/target/resource/graph/data` 不含 Native API 头；`backend/vulkan` 可依赖中立层，但中立层不可依赖 Vulkan；Server 装配使用一次受控 Native factory/ops，不需要每 Pass 虚派发、也不允许 Client 通过 `RenderRuntimeBuilder::backend()` 持有 Vulkan Backend 的生产依赖。`render/features` 依赖 Authoring SDK 和各自数据结构，不依赖 Native 私有头。

## 2.2 唯一所有权树

```text
Application / bootstrap (owns Server + Client endpoint lifetime)
  RenderRuntime [Client: protocol producer; NOT Backend owner]
  RenderBackend [Server: owner thread, transport consumer, frame cadence]
    scenes: SlotMap<Scene>                         Scene owns its SlotMap<View>
      Scene.domain_id → ResourceDomains            View.domain_id, effect_id
    effects: SlotMap<RenderEffect>                  immutable config candidate transaction
    graphs: SlotMap<compiled Native Graph reference>   exact structural dedupe cache
    targets: SlotMap<Target>                      target owns ordered layers metadata
    domains: SlotMap<ResourceDomain>              unique ResourceRegistry instance per domain
    execution_jobs/groups: value indices          no View/Graph ownership duplication
    native: backend/vulkan/Backend
      Device → Allocator/Queue/WSI owners
      Native compiled Graph/Shader/PSO/layout owners (shared immutable)
      Executor → GraphExecutionContexts → per-job/FIF slots & native scratch
      Retirement Queue → GPU/composition/plugin completion owners
```

`Scene` 里的 View 是真正的逻辑 owner。Backend 中禁止再建立另一份可变 View SlotMap 做业务权威；Backend 可以维护只含 `(SceneId, ViewId)` 的非拥有索引用于按 GraphId 分组。`GraphExecutionContext` 在 Native Executor 中存在，View 只保存可校验的 Native context 关联/句柄，不包含 Vk 类型。

## 2.3 C/S 生命周期与设备替换

单进程同进程线程通信已经是 C/S 结构：Client 提交 typed packet、Backend owner thread 接纳和修改状态。一个启动装配由应用持有 Transport、RenderRuntime、RenderBackend 的明确生命期，停止次序：禁止新提交 → drain/cancel 已接纳 → GPU/WSI 完成与退休 → 销毁 Backend → 销毁 Transport → Client 失效。无第四通信机制或内部业务 Worker。

可换 D3D12 的要求：Client/Scene/View/Effect/GraphIR 编译消费者不包含 Vulkan 头；`backend/d3d12` 提供其 Native Compiler/Executor/Target/Resources；设备启动时协商能力，当前未实现的 Backend 不能标记支持。Device Lost 是失败的 Native session，而不是将旧 Vk 句柄直接复用到新设备。Native handles 及 ResourceDomainId 有 Backend/Device 归属校验。

## 2.4 六种生命周期分别说明

| 对象 | 创建 | 谁能改 | 删除 / 热替换 |
|---|---|---|---|
| `Scene` | CONTROL 创建/Server 接纳 | Backend owner-thread；Client 通过消息 | 先撤销 View/Domain 的逻辑引用，GPU last-use 退休 |
| `View` | Scene 内创建 | 相机/Effect/状态通过 Server owner 更新 | RenderGraph 绑定和 Native Context 失效，旧 slot safe retire |
| `RenderEffect` | Effect 构建定义经协议注册 | 候选 Feature 配置或 Hot 参数事务 | 不影响仍在用的 Native Graph、Shader codepin |
| `RenderGraph` | Backend Compiler 冷态产生 | 不可改 | 没有逻辑消费者且 GPU in-flight 结束后缓存/退休 |
| `ResourceDomain` | Backend 在 Root/Scene/View 创建 | 只允许本域 owner 修改；祖先按权限访问 | 先递归撤销子域、阻断新使用；Native 延迟退休 |
| `Target` | Server 根据 offscreen/window 声明建立 | 图像/尺寸/Layer 变化由 Server 提交事务 | WSI Acquire/Present fence 与 GPU Submission Completion 均需证明 |

## 2.5 执行 Context 为什么不是重复 View

`View` 记录相机、Effect 选择、History 语义和 Domain；`GraphExecutionContext` 记录与某 Graph 版本、Target signature、一次输出视图组合相容的 Descriptor、Native transient、FIF slots、真实 resource state/tickets。一个 View 可以因不同目标规格、Effect 热变更暂时关联多个新旧 Context，旧 Context 在 GPU 结束前继续存在。**Context 不拥有 View，也不重新存一份 Shader/PSO。**

## 2.6 失败路径与所有权检查

所有创建、更换采用候选完整构造→验证→原子/安全点发布；不允许部分将新图推送给某些 View 后才发现另一 View 失败。Protocol 错误/Backpressure 不能偷偷丢掉状态版本。`GraphId` 是具体 immutable code，而非某 Scene 内任意数组索引；Generation 不能复活已删除对象。改写旧 R4 native owner 前必须出 source、reproducer、GPU alias/Queue/WSI/ASan/性能影响报告。


---


<!-- SOURCE FILE: 03_数据描述与类型身份.md -->


# 03 — DataSchema、PassSchema、强身份与三种 ABI

## 3.1 先声明值类型，不预设存储模式

`DataSchema<T>` 负责稳定 canonical identity、字段、版本与可选 CPU/Wire/GPU 表示，并交由现有 `lux-cxx` 生成器产生。它**不申请资源**、不决定 Realm/Domain 归属、不意味着有 CPU `TypedStorage<T>`，更不改变 Graph 的 Buffer/Image 与 PassUse 基本模型。Shader-visible T 的 CPU 布局未必等于 GPU std140/std430 的 stride。CPU-only、Wire-only、GPU-only 都合法，只有作者真实使用相应能力才生成/检查对应合同。

```cpp
// 目标结构语义，不代表此时生成器已支持全部注解拼写
struct LightGpu { float position_radius[4]; float color_intensity[4]; };
// Generated DataSchema<LightGpu> 的 GPU layout 经 SPIR-V reflection 核验。
// Engine ECS LightComponent 可是完全不同的 C++ 类型；RenderSystem 显式 pack delta。
```

类型 ID：`RenderDataTypeId` (canonical name/version + collision/full shape validation)；GPU Binding 是 `ShaderBindingKey`/结构性 `BindingLayout`，资源实例则是 `GpuResourceId` 加 DomainId/Generation；`GraphResourceId` 局部于单份 Definition/IR。绝不将字节大小相同视为 Schema 相同。

## 3.2 身份表

| Identity | 作用范围 | 相等条件 |
|---|---|---|
| `RenderEffectId` | 一个 Server session | owner + index + generation |
| `RenderGraphId` | 同一个 Native device/session | 可校验生成式 ID，缓存命中需 full structural equality |
| `SceneId/ViewId/TargetId` | Server session | owner + generation |
| `ResourceDomainId` | Backend/device Domain 树 | owner + generation；不可跨 Backend 比数值 |
| `GpuResourceId` | 对应 Native GPU Asset 身份 | Domain/asset key/generation 和 native owner proof |
| `GraphResourceId` | 单份 Graph IR | 位置索引；不能做 Backend Asset ID |
| `FeatureTypeId` | 代码/SDK 注册表 | canonical name/version + full descriptor |
| `SubmissionTicket` | 真 Queue 提交与完成 | Queue/device/serial 与真实完成事实 |

不可把 `RenderGraphId` 直接等同于 `RenderEffectId`：E1 可同时含多个 Target/device specialized Graph；E1/E2 在确定结构完全相同时也可引用同一 G1。Hot effect 参数 revision 不改变 `RenderGraphId`。

## 3.3 运行时资源类型与 Domain 不耦合

`ResourceRegistry::get<T>()` 表示当前 Domain 或最近祖先里一个明确的资源/资源集合 T；多 Mesh 是一个 `MeshStorage` 的内部多记录，不是注册 N 次同一个类型。若一个 Domain 的 `T` 必须有 `main/preview`，使用明确 `ResourceKey`，省略只在唯一且合同允许时合法；否则 cold reject ambiguity。`getLocal<T>` 与修改 API 不能悄悄写父域。

结构性的 `BindingLayout` 只需要 Shader interface/role/access/layout 等 facts；不能在缓存 key 中混入 SceneId、DomainId、GpuResourceId、Camera 值或 frame revision。实际绑定在 Context 冷态/资源变更时解析。

## 3.4 数据来源与权威

- **CPU authored**：Engine ECS/资产管理器/Tool/Plugin 持业务权威，RenderSystem 提交 typed deltas/snapshots。Backend 保留 GPU backing、stable slot maps、容量、ready/dirty application metadata 与必要的 staging，不建立第二份 ECS World 或完整业务 CPU record 长期镜像。
- **GPU authored**：如 Particle/HZB/Compute Cull/Visibility/History，不暴露伪同步 `get<T>`；GPU Pass 通过真实 Graph resource access/write/read version 和 Queue tickets 持有权威。
- **Externally authored**：Camera/Video/CUDA/interop GPU 资源由外部 owner 提供显式 import、layout/state/ready/retention。Backend 能借用但不能错误 double free。插件 ABI 的 codepin 必须覆盖 callback/析构所需 DLL。

## 3.5 不重复声明 Shader 与 Wire

`DataSchema<T>`（长期类型数据表示）与 `PassSchema<P>`（某 Pass 的读写与 Shader 参数）由同一生成器/反射系统衔接：校验 offset/stride/array/matrix/layout/stage、descriptor role/count、完整 Owner/Binding shape、sampler/view format。R2 PacketValue 并不因一个 T trivial copy 就允许任意指针/`std::span`/`string_view` 跨线程逃逸；Wire codec 的版本、拷贝 ownership 与 GPU ABI 分离。C++20 Concept 正负例需在 Installed SDK 独立编译核验。

## 3.6 与 F6.a/b 已完成事实的关系

F6.a 生成 DataSchema/GPU pack 正负例，F6.b 引入多 Shader Owner shape 和两实 Buffer/one shader oracle，这是可复用的算法基础。`OwnerShape/EFieldOwner` 旧名称与实际 Domain owner 语义需要解除混淆，建议降为编译器私有 `BindingLayout` 事实；不能为达到“新名字”直接删除多 Owner full equality、SPIR-V ABI 负例和 PSO 共享资格。原布局冷态性能回退必须单列追踪，而不是借全面架构改名豁免。


---


<!-- SOURCE FILE: 04_ResourceDomain与GPU资源.md -->


# 04 — ResourceDomain、GPU 资产身份、Residency 与数据流

## 4.1 Domain 不是一个新的资源管理器层次

公共 `ResourceDomain` 是 Backend-managed、生成式 ID、parent ID 与单个 `ResourceRegistry` 的小型 owner 记录；`ResourceRegistry` 是唯一的类型化资源容器。典型树：Root（共享 Shader/Texture/Mesh/Material 资产存储）、Scene（Light/Transform/Scene Buffer）、View（Camera/History/局部 Surface 数据）。允许具体调用者显式建立其他子域，**不依据资源 C++ T 编译时锁死 Global/Scene/View**。

```cpp
// 仅 Backend owner-thread 中的目标 API
ResourceRegistry& r = backend.resources(view.domain());
auto& camera = r.get<CameraBuffer>();           // 最近匹配：View
auto& lights = r.get<LightBuffer>();           // 祖先 Scene
auto& meshes = r.get<MeshStorage>();           // 祖先 Root
auto& mine = r.getLocal<CameraBuffer>();
// 写操作要求在实际拥有该资源的 Domain 上获得授权，不能通过透明父链写入。
```

同 Domain 同 Type+Key 的歧义必须被拒绝；当前 Domain 若遮蔽祖先同 Type，需固定明确定义的最近查找规则或 cold reject，不能依赖插入顺序。Client 绝不能直接持 Backend Registry 引用：跨线程只能通过 R2 typed protocol 和 Session handles。

## 4.2 Domain 与 GPU 物理存储

Domain 负责：资源可见性/归属、撤销新访问、logical generation、budget accounting 和本域 Registry 的析构事务。Backend Native Buffer/Image/Arena 负责：真实分配、capacity/offset/layout、GPU ready/last-use、descriptor backing versions、physical retire。**ResourceDomainId 绝不代替 VkAllocation lifetime**；是否缓存最后 release 的资产由 CPU 策略和 Backend 预算将来协商，不得自动强制 LRU。

CPU 资产 `AssetId`、Client GPU handle `GpuResourceId`、Native VkBuffer/VkImage 和 Graph 局部 `GraphResourceId` 四者不同。业务 retain/release 是逻辑合法性；缓存还在不允许旧 generation resurrect；GPU 使用完之前，即便逻辑已 release，也禁止覆写/释放旧物理 byte range。

## 4.3 数据更新不复制 ECS World

```text
Engine ECS/Assets (CPU single authority)
  └─ change events + generation + data packing
       └─ RenderSystem delta projection (O(D))
            └─ PROGRAM/UPLOAD
                 └─ Backend ResourceDomain's GPU-resident storage
                       Native buffers/images, versions, ready, descriptors
```

Backend 必须能保存必要的 GPU 容量/slot ID/上传 staging/packing metadata，但不能再建立一份所有 ECS Components 的常驻 CPU Store。Mesh large vertices、Texture pixels 可以仅在上传 admission 时暂持 owner-pinned bytes；若 Device Lost 需要重供，应由资产系统重新提供。GPU-authored `Particle/History/IndirectArgs` 不需要 CPU 读回才被下一 Pass 使用。

## 4.4 物理布局由消费需求驱动

| 工作负载 | 适用物理布局 | 必须验证 |
|---|---|---|
| 频繁零散 Light/Instance 增量 | stable slots + dirty range/packed scatter | O(D) CPU 构造、GPU gather 成本、无旧 slot 覆写 |
| Dense linear GPU traversal | dense Buffer/active ID workset | 重排时 Generation/Native 借用正确 |
| Mesh/PointCloud/Gaussian streaming | chunk/arena/page + GPU workset | GPU residency、LOD/fallback、峰值显存、异步 Ready |
| Texture/3D image/cubemap | Native Image + view/sampler contracts | subresource layout/format/usage/alias |
| HZB/TSR/TAA history | View-dependent persistent image versions | resize/cut/epoch/target change 与上一帧票据 |
| Cull scratch/DrawArgs 临时 | GraphExecutionContext 中 Graph transient | Pass live intervals、GPU Queue overlap、别名证明 |

DataSchema 的 GPU ABI 不因 Native Compiler 为省内存随意改变 AoS→SoA。需要专门访问模式必须先选择 ABI/生成 Shader accessor，再生成相容 Native Graph 变体。Shader 热路径不做按数据类型的动态 map/hash 查询。

## 4.5 ResourceDomain 的安装、移除与热替换

- `createDomain(parent)` 冷态事务，生成稳定身份；先验证 parent/Backend owner 及容量，再发布。
- Domain 将废弃时先撤销新 lease、处理所有子域，进入逻辑不可见状态；旧 Native GPU owners 由 last-use/Presentation 的完成证据延迟销毁。
- GPU backing resize/new version：创建完整候选分配、copy/upload/Ready→构造 bindings→发布新 version；旧 GPU reader 未完成前不能回收物理存储。
- Schema/Binding ABI 改动需要新 Shader/PSO/Graph；仅同 ABI 的资源实例变更通常只更新执行 Context binding，不重新编译。
- 同一 Mesh 被 Scene A/B 使用：Root 资源一份、两 Scene 各自记录 Mesh instance/Transform，资产引用计数独立于最后 GPU Ticket。

## 4.6 状态与回执粒度

建议区分 `observed/accepted/applied/gpu_ready`；Backpressure 不清原始 Dirty，合并连续更新只在未接纳且不丢 create/delete/generation 语义时允许。`SubmissionTicket` 不是 revision。测试包括 10k/100k/1M records、D=0/0.1/1/10/100%、clustered/random、Scene A/B 同一 Asset、多 View/2–3 FIF、GPU-only 路径和 fault-injected resize/retire。

## 4.7 不是“TypedStorage 全面禁用”

若 render backend **确实**自己生产了 CPU-authoritative 非 ECS 数据，局部 `TypedStorage<T>` 作为普通 C++ 容器算法是允许的；但它不是系统中心的第二 ECS，也不因所有 GPU 资源具有类型 Schema 就自动创建。业务权威来源必须在每种真实消费者的设计中标明。Shader/Native 背景所有权仍只有一个 Backend。


---


<!-- SOURCE FILE: 05_通信与发布事务.md -->


# 05 — RenderRuntime Client、协议与跨 Lane 发布事务

## 5.1 Client/Server 关系不等于 C++ 成员包含

Engine RenderSystem/Editor/AssetLoader 持有 `RenderRuntime` 作为 Client endpoint；`RenderBackend` 是独立的 Server 状态与 GPU owner-thread，由应用启动装配明确拥有。Runtime 不暴露 `vulkan::Backend&` 给 Engine，也不直接决定 Shader/Resource native 类型。Native Backend 的替换只影响启动工厂/能力配置和具体编译执行代码，而不会要求 Renderer 业务层重写协议。

单进程传输继续严格沿用 R2 三 Lane：`PROGRAM`（Scene/View/Effect 运行参数与增量）、`CONTROL`（创建/移除/切换/Target Resize/Effect topology 事务）、`UPLOAD`（并行来源 large blobs/owned buffers）。保持 bounded admission、reply consume/abandon、停机、错误通知、线程亲和和接纳因果。**禁止第四 EventBus、后台业务 Worker、每 Feature 私有 Queue**。

## 5.2 最小逻辑协议家族

| 请求类别 | 内容 | 返回 |
|---|---|---|
| `CreateScene/RemoveScene` | 无 Native 指针的配置与 Scene owner identity | `SceneId` 或具体错误 |
| `CreateView/RemoveView/SetViewEffect` | 归属 SceneId、EffectId、Camera/Target 关联 | `ViewId`、采用版本与资源就绪 |
| `CreateEffect/UpdateEffect/RemoveEffect` | 一份可编译的 Feature 描述和 revision | `RenderEffectId`、候选编译/发布结果 |
| `CreateTarget/ResizeTarget` | Offscreen/Surface 描述、Attachment 格式、图层 | `TargetId`、Target epoch、状态报告 |
| `UploadAsset/Retain/Release` | `GpuResourceId`、AssetId/typed payload/Domain owner | accepted/ready/error，绝不假装 GPU 已完成 |
| `PublishScene/View/EffectParams` | typed deltas、revision/generation、field/Schema identity | accepted/applied 状态；Hot data 不重编译 |
| `Readback/QueryStatus/Diagnostics` | 明确 Target/Graph export/format 和 Completion | 根据 ticket 可读的结果或 NotReady |

消息格式可以合并传输实现，但语义区分不能混乱。Wire 中禁止 STL owning objects、指针和任意 C++ `T` 直接按内存字节发送；结构化元数据必须版本化、存在合法对齐与解码合同。以 `LUX_OP` 和现有 Transport 代码生成机制为唯一 source，避免重写 parser。

## 5.3 关键异步状态

`RenderRequest` 的消息接纳不等于 Effect 已编译、不等于 Graph 已发布、不等于 GPU 命令完成。要求明确区分 `Created/Accepted/Compiled/Published/Submitted/Completed` 的真实可观察状态（不要求另建一组长期 enum 对象）。失败时返回精确原因，不能无限延迟/悄悄黑屏。Device Lost/Stop 将当前 Session 置为终态、取消/报告未完成请求，不自动把旧 GPU handles 复活。

## 5.4 共享 Effect 的更新事务

1. Client 在旧 `effect_revision` 上提交候选 `UpdateEffect(effect_id, expected_revision, edits)`；冲突 revisions 明确拒绝，不默默覆盖另一个 Editor 操作。
2. Server 在安全冷态快照所有受影响 View/Target 相容签名；执行 Feature 构建、IR 编译、缓存查询和每个需要的 Backend Graph 变体编译；同形状命中旧缓存仍必须 full identity 验证。
3. 对全部当前消费者预先验证 GPU 资源 ABI/Target capabilities/ExecutionContext Slot capacity、插件 codepin 和最大预算。缺任何必要资源/不满足 fallback 则整体失败，继续使用已发布版本。
4. 一个 Backend 安全发布点更新 Effect active revision 与所有相关 View 的派生 graph bindings/group index。一个 revision 不得在同一逻辑帧中一半 View 旧一半新；已提交的 GPU 旧帧允许继续使用旧 native owners。
5. 仅改变 Exposure 等 Hot GPU 参数时，发布参数快照与 GPU binding version，GraphId 和 Shader/PSO 保持不变；如果不同 View 希望不同参数，必须选新的 Effect 或显式 View 参数覆盖合同。

这不允许隐藏的 `EffectUpdateManager`，候选编译与发布由 `RenderBackend` 的 Control handler + 既有 Compiler/Executor 实现。

## 5.5 Scene 的 CPU projection

ECS Source 仍是 CPU 权威。每个 RenderSystem 只保持 SceneId/ViewId/GpuResourceId、Change Cursor、未确认变更的压缩日志与 request receipts；无每帧全量 ECS scan/复制。Source 以 10Hz 更新但 RenderBackend 以 144Hz 渲染时，Frame Loop 使用最近被接受的合法 GPU 版本，独立推进。两个 Scene 的 RenderSystem 可以共享同一 Client，不共享 ECS 数据权威。

## 5.6 Backpressure、跨 Lane 因果与异常

`UPLOAD` 的实际 GPU ready 必须有 Native Ticket/Host ready 证据，在依赖它的 `PROGRAM` 引用被执行前保留正确顺序；不能因为两个 Lane 中各自的 revision 单调就假定跨 Lane GPU 完成。CREATE、UPDATE、DELETE 的 generation 不允许在 Queue 满时被中间合并而丢掉。`onStop` 不调用 `waitIdle` 逃避在途 partial-submit fault；按已有 Queue tickets/Retirement 与有限故障语义处理。错误诊断通道和有 reply 的 request 不混为一谈。

## 5.7 Engine 侧目标使用

```cpp
// 语义示例；实际接口返回异步 RenderRequest/typed reply
auto effect = runtime.createEffect(deferred_effect_desc);
auto scene = runtime.createScene();
auto view = runtime.createView(scene);
runtime.setViewEffect(view, effect);
runtime.publishSceneChanges(scene, light_changes);
// 跨 Engine/Server 线程不允许：backend.resources(scene).get<LightBuffer>()
```


---


<!-- SOURCE FILE: 06_RenderEffect与Feature组合.md -->


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


---


<!-- SOURCE FILE: 07_RenderGraph推导与编译.md -->


# 07 — Renderer-independent Graph IR 和两级编译器

## 7.1 Graph 是单次最终 Target 输出的完整程序

**一个 `execute(graph, view(s), target)` 包含该目标画面全部 GPU Pass**。GBuffer、Depth、Shadow Scratch、HDR、Tonemap、Overlay/Composite 等中间变量都在同一份 `RenderGraphDefinition` 中声明；Builder 明确读写和最后 target output，Compiler 从真实 Producer/Use/范围确定 Pass 顺序、分配和状态绑定。执行结束（GPU completion 后）即是这次 Target 最终结果，没有图外隐藏 Tonemap、内部纹理第二次 Graph 读取或私有 UI 绘制环路。

Graph 可以显式输入 persistent Scene/View/Root 数据、上一帧 History、Camera/Video 或另一 Offscreen Target 已经发布的**最终输出**，但这是一份经过授权的外部 Import，绝非将当前 Graph 内中间临时图像延长到 Graph 外。在确实有不同 Target 之间的明确外部输出依赖时，Server 只对完整 Target job 进行因果排序，不增加第二套内部 Pass scheduler。多 View 同一 Target 的最终合成由其完整 Graph 中的 composition passes 表达；普通单 View 使用 `execute(graph,view,target)`。

## 7.2 中立 RenderGraphIR 的必要数据

- Graph-local 资源：Buffer/Image、format/aspect/extent（可相对 Target）、import/transient/history/export、semantic 以及访问许可和必要初始化合同。
- Pass：Graphics/Compute/Transfer/Readback 等基本工作、Shader reference、typed PassSchema/Draw/Dispatch/Indirect/Copy ops、conditions、explicit order for real business constraints。
- 子资源/字节精确 `READ/WRITE/READ_WRITE`、Producer Version/fallback、输出 Root/SideEffect 与 Scene/View reuse 证明数据。
- 不携带 `VkDevice/VkBuffer/VkPipeline`，不携带具体 SceneId、DomainId 或 CPU 指针；业务 Light/Mesh/Gaussian 不是 Graph 的通用 enum 变体。
- 编译结果拥有结构性身份和完整比较事实，hash 仅索引缓存，hash 相同不等于允许跳过相容校验。

## 7.3 C0–C8 保留现有分析算法

| 阶段 | 算法与输出 |
|---|---|
| C0 | 规范化 Builder 的稳定声明、pass/resource IDs、精确 ImageRange/BufferRange |
| C1 | 解析 explicit producer/import/semantic/fallback 到唯一资源版本，歧义 cold reject |
| C2 | `PassSchema`、资源接口、Shader 结构性需求、usage 与写权限验证 |
| C3 | Subresource segment + version hazard：RAW/WAR/WAW/ORDER/FALLBACK edges |
| C4 | 确定性拓扑排序，真实 cycle path 和 producer conflicts 诊断 |
| C5 | 从输出/外部副作用反向裁剪无效 Pass，条件组合法 fallback |
| C6 | Logic live interval、usage union、potential alias candidates（非 GPU completion 证明） |
| C7 | Scene-scope Pass 在多 View 可共享的真实输入证明，View-scope work 必须分开 |
| C8 | 拥有型 immutable `RenderGraphIR`、diagnostic 和完整 cache identity |

逻辑 Pass 的作者顺序不一定就是执行顺序；Compiler 可在依赖许可内改善 Pipeline/Descriptor 切换局部性，但不得改变 Transparent/Blend/Overlay 的真实顺序要求，也不能跨资源读写边任意调换。

## 7.4 Backend C9–C13：像 LLVM 后端，但无无意义注册框架

| 阶段 | Vulkan Compiler 的真实工作 |
|---|---|
| C9 | DeviceCaps/Shader Stage/PassSchema→布局/Shader 编译或再定位、SPIR-V reflection 验证、完整 `BindingLayout`/PSO key |
| C10 | 从 IR 实际访问与能力选择 Native Buffer/Image/Transient plan、usage、alignment、alias/noalias 与 Target signature 专门化 |
| C11 | 编译 native pass/PSO/Descriptor/Pipeline bind recipe、Dynamic Rendering/Copy/Indirect 等固定命令数据；禁止 lazy GPU 对象构造 |
| C12 | 精确 Sync2 src/dst stage/access、Subresource barriers、Queue family transfer、external ready wait/signal 与 submission budget |
| C13 | 验证并构造不可变 `vulkan::Graph`，保留 Pipeline/Shader/Layout owner lease；给 Executor 可直接解释的指令序列 |

`vulkan::GraphPass` 是**编译后的 GPU Pass 数据**；`CompilerPass` 是可测的分析/变换函数，二者不可混用。默认各 C9–C13 是固定函数/实现文件，只有新 Native primitive 真实需要扩展时才增加阶段处理函数。**不默认实现运行时 `ICompilerPass`/`PassManager`/`PassRegistry` 动态插件。**

## 7.5 最小化绑定与屏障

Compiler 静态消除等价布局转换与多余 hazard、合并兼容 Pass 和复用 PSO、预排序常用 Binding，生成 Descriptor/PushConstant/Pipeline 切换配方。Executor 维护**当前 CommandBuffer 的**真实绑定状态，避免重复 `vkCmdBindPipeline` 和 `vkCmdBindDescriptorSets`；状态绝不跨新的 CommandBuffer/Queue 无条件沿用。相同 Struct 多实例与完整 Shader Owner 的不同 named binding 要正确区分，不能只按类型/频率归并。一个新的 GPU Binding 读者不应该要求作者再次手写 `set=3,binding=7`。

## 7.6 GraphId、Native 变体和 Cache

`RenderGraphId` 唯一标识一个已编译的 Device-local immutable Graph。Cache key 为完整 IR identity、Native Backend、设备 enabled capabilities/ABI、Shader/Material variant、真实的 Target attachment format/sample/local-read/必要 extent signature 等；**不含 Scene/View ID、Domain、当前 Camera、Effect Hot 参数或 FrameSerial**。同一个 Effect E1 在两个相容 Target 上直接共享 Graph G1；必要 extent specialization 产生 G2；Graph 经 full equality 命中无需重编。旧 Graph 在 GPU 完成后安全可退役，不因 Effect 改版本而原地写入其 pass/PSO。

## 7.7 Scene Pass 共享与真正 GPU 工作

Graph immutable 复用并不意味 GPU passes 在多 View 只需运行一次。C7 将可跨同 Scene/View-independent inputs 的 Pass 标记为候选，Executor 通过实际 Scene resource revision/ready/layout/side-effect 等快照核验，只有成立才借用一次已接受 GPU 结果并延长最后 consumer 的 use ticket。Scene A/B 即使同 Graph，也不因此共享各自 Light/Instance 结果；不同场景共用真正 Root Asset 仍遵守 Native owner。

## 7.8 禁止的架构滑坡

- 不因 Target output 需要 readback 就在 Graph 内部偷偷保留永久 Texture；只有作者明确导出结果才获得跨生命周期身份。
- 不通过 Frame 序号推测资源初始化/同步；Native Compiler 证明逻辑 access，Executor 证明实际 ready/state/ticket。
- 不每执行一次 Graph 都构建 `RenderGraphDefinition`、重排 Pass 或重建 Descriptor/Pipeline。
- 不因为未来 Ray/3DGS 需要新 Native op 就写一个跨 API 的万能 IRenderer；在 IR 与具体后端添加受限能力。


---


<!-- SOURCE FILE: 08_Shader与布局自动生成.md -->


# 08 — 统一 DataSchema、PassSchema、Shader 与 Native BindingLayout

## 8.1 一份事实，从 C++ 到 SPIR-V

作者只声明 Shader-visible GPU 数据类型与 Pass 真实访问，由现有 `lux-cxx` 和 Shader toolchain 产生 DataSchema/PassSchema、GLSL 结构/字段、GPU Layout、反射/重定位所需元信息。Renderer 不维护私有的业务 shader registry；Native Compiler C9 负责核对所有 Pass/Shader 资源绑定。CPU struct/可选 Wire bytes/实际 GPU representation 独立进行 ABI 验证。禁止“类型 trivially copyable → 直接 memcpy 当 std430”。

## 8.2 BindingLayout 与 ResourceDomain 是两种身份

旧 `OwnerShape/ShaderOwnerId/OwnerAssignment/EFieldOwner` 的**功能本质**是结构性 Shader Binding 形状与字段归属。目标设计用内部 `BindingLayout` + frozen `BindingKey` 表达各字段 role/stages/descriptorCount/array stride/dimension/format/complete Schema。它们**不表明真实 GPU Buffer 属于 Scene 还是 View**。实际资源归属与可读链来自 ResourceDomain，Native ExecutionContext 冷态选择确切 backing。一个 Shader 从 Scene `lighting.main` 和 `lighting.preview` 读取同一种 `LightGpu` 时，必须保留两个独立 binding semantics；跨 Scene A/B 使用相同 shader/layout/PSO 时可绑定不同实际 Buffer。

## 8.3 两层相容性

1. **结构性**：`PassSchema`, Shader Stage Interface、Data GPU Layout、DescriptorType/Count、PushConstant、Target Attachment Signature、Device enabled features。负责 Shader/PSO/Layout/Graph Cache identity；需 full equality，hash 只是加速。
2. **具体实例**：Domain owner/generation、GpuResourceId、Native device、Buffer range/Image view、ready ticket、实际 states、Capacity 是否足够。只在 Context binding/执行前验证，不进入 PSO 结构键。

一个 Effect 的 View A/B 共享结构性 Graph，但动态数据完全独立。`BindingLayout` 是编译算法的数据，不能为它另建持久 `ShaderOwnerManager`。物理 Descriptor Set 合并/重定位须由 C9 同一 Layout 算法完成；不能一个 Feature 就强制分配一个 Vulkan set，设备预算不足必须明确拒绝或选择合法的被声明变体。

## 8.4 Pipeline/Descriptor 热优化

C9/C11 冷态建立 PipelineLayout、Shader relocation、PSO、Descriptor recipe；热态 Executor 的 Slot 保存已分配的 Descriptor 实例和数字化 binding，Record 维护当前 CommandBuffer Pipeline/Layout 与动态 offsets，跳过冗余绑定。在多视图共享一个 Graph 时不能多编 PSO；资源实例的变化若 Layout 不变仅对 Context 重绑定并遵守旧 descriptor in-flight 不可更新规则。动态 UI/Material variant 可选在低延时条件预热编译，但不能在热态隐式 PSO 创建。

## 8.5 Shader/Device 能力

完整能力由 Device 创建时的 supported/enabled 两部分决定。当前 Vulkan compute/VS/FS/有限 Native draws 已验证的子集与未来 Mesh/Task、AS/RT、3D Image/CUBE、descriptor indexing 等明确区分。Feature 要求未启用设备能力时，只能在 Effect 选择时提供已经描述/验证的 fallback 或 cold reject；不能无设备同意偷偷启用新扩展。OpenGL/D3D12 后端有自己的 Shader 翻译和 binding lowering，IR 本身绝不包含 VkDescriptorSet。

## 8.6 SDK 与插件

第三方 Feature 只 include public installed headers。插件可注册新的 `GaussianChunkGpu` Schema 或读取既有 `LightGpu`，Host 深拷贝可信结构描述及完整 ABI 合同；跨 DLL 不允许 STL owning、RTTI hash 或未 pin 的 `string_view`。卸载：撤销新注册→替换 Effect/Graph→等待所有旧 callback/native graph 的代码/资源使用安全退出→release CodePin。一个插件在 Graph 编译完成后不会被 Editor 的热调度反复回调 Native Vulkan 录制。

## 8.7 F6.b 已知性能问题（不能丢失）

源码已通过多真实 Owner shader GPU oracle，但 `layout_full_shape` cold 构造每操作分配数增加（正式验证报告约 10→24）且 CPU 耗时明显上升；`native_graph_record_submit_256x256` p95 CI 未能证明回退在 +5% 内。新方案应复用 canonical full shape hash 和 full equality，不过要消除可证明的临时字符串/指针数组重复分配，先测再动。不能为了“新的 BindingLayout 名称”再次复制或二次扫描全部字段。历史资格保留在 16/18 与审计页。


---


<!-- SOURCE FILE: 09_NativeCompiler与Executor.md -->


# 09 — Backend Native Graph、Executor 与 GraphExecutionContext

## 9.1 C9–C13 之后的 Native Graph 是什么

`vulkan::Graph` 为不可变、无 `execute()` 方法的完整 Native 程序，含 GPU pass/PSO/Layout、当前 Device 句柄的 RAII owner lease、资源规划、Image/Buffer use/alias、Barrier/Queue/Submission 和预编 Binding Recipes。它是“数据”而不必 `trivially_copyable`：Pipeline 需要真实 Device lifetime，Graph 缓存的共享只共享不可变 Native owner，不共享 per-view Descriptor/Transient/FIF scratch。

```cpp
// 目标语义
namespace lux::render::vulkan {
  struct Graph { /* immutable compiled code + pinned native owners */ };
  class GraphExecutionContext { /* mutable resources, slots, states, tickets */ };
  class Executor {
  public:
    RenderResult<GraphSubmission> execute(
      const Graph&, const View&, Target&, GraphExecutionContext&) noexcept;
  };
}
```

此处 `View`/`Target` 仍无 Vulkan 头，具体 backend TargetImage/Acquire/Present 由 Executor/Backend Native 资源表解析。对多 View target composite，Context 与 Graph 声明明确的多 View 输入，允许 `execute(graph,span<views>,target,ctx)` 的已验证调用，不隐式依序执行多个独立后处理图。

## 9.2 GraphExecutionContext 的必要成员

- 对应 `GraphId` 与具体 View(s)/Target compatibility 的稳定执行关联、generation；Effect 更新时旧新 Context 可短暂并存。
- 实际已校验的 ResourceDomain Resolve/Native resource ref、Descriptor sets、DynamicOffsets、ImageViews、Sampler、native state ranges 等。
- 有界数量的 FIF ExecutionSlots：per-pass scratch、transient Buffer/Image、accepted values、pass/cell/queue tickets、当前正确 `VkImageLayout`/Access/Queue family。
- Scene Pass legal donor 的真实同帧 submission/last-use lease；对同一 Device 的别名/Imported GPU memory overlaps 必须在执行前拒绝未经证明的危险情况。
- 最后的 GPU Submission 结果和 output ready/retirement 信息。History 语义归 View Domain；Context 仅持执行所需 image/version lease。

**这个 Context 的创建容量以实际活跃 View/并发执行需求为界**，不能按 `max_views × FIF` 给每个 Graph variant 预建满额所有 GPU attachment；正常 Record 不热分配，也不能在 GPU 仍读旧 slot 时复用。

## 9.3 Executor 的唯一执行流程

```text
backend frame begin → acquire Target native images and attach final output contract
preflight all actual jobs: capacity/slots/import readiness/queue submission budget
for each prepared GraphId group respecting final target job order:
  for job: resolve pre-bound Context → validate actual Native imports/tickets
           → optional proven Scene-pass reuse
           → bind/record native pass recipes (minimize redundant cmd binding)
           → Sync2+Queue submit → GraphSubmission receipt / output states
finalize declared final Target image → Present or Offscreen ready/export
retire old Graph/Context/Asset after true Queue/WSI completion
```

`execute()` 负责整个 Graph 的 Pass，包括后处理和必要合成，**不会**在图外再发隐藏图形渲染工作。GPU 提交成功不代表像素已经可 CPU 读取；readback 必须等 GraphSubmission 完成并使用正确 Host publication barrier。允许多 Queue 真 Sync2 + single queue fallback，所有 Queue 失败/partial submit 不制造虚假 join。

## 9.4 稳定 Native 状态的录制成本

Compiler 固定 Pipeline/Descriptor/Resource/Barrier recipes；Executor 只对当前 CommandBuffer 保持本地 binding cache，尽可能少 `vkCmdBindPipeline`, `vkCmdBindDescriptorSets`, `vkCmdPipelineBarrier2`，不跨 CommandBuffer 误用状态。C++ first-party heap 在固定 Graph/Context capacity 热态为零。计量需区分 native driver allocation bytes（可能不可观测）与 C++ heap allocation count；一个微基准 PASS 不代表大型 Frame PASS。

## 9.5 WSI、Offscreen、ExternalInterop

`Target` 逻辑对象独立；Vulkan backend/target 管理真实 Surface/Swapchain Image、Acquire/Present semaphores/fences，Offscreen backing/ImageViews/Readback pool。双窗口下 Present completion 不等于 graphics Queue completion，重建/最小化时不能复用 pending binary semaphore。Resize 先创建新候选与必要 Context/Graph target variant，失败继续 last-good；old native resources 经 Queue/Present 双证据 safe retire。

外部 CUDA/Vulkan 等零拷贝只经显式 Backend native interop contract、external waits/signals/UUID/memory types，不能把 `VkBuffer` 作为 Editor Feature 公共成员。其他 Backend 可拒绝未支持互操作，不能声称全平台同功能。

## 9.6 生命周期与故障

Native Graph、Pipeline、Layout owner 的生命周期不得短于任何 Context 或在途 command buffer；Context 销毁不得短于其全部 Queue Ticket；Imported GPU Asset 的 CPU release 只撤销新引用，不能释放在途 physical Buffer/Image；Partial Submit 或 Device Lost 通过明确 Terminal/Retirement path 处理。`waitIdle` 只能在确需 session teardown/rebuild 时合法使用，不是每个 hot update 的同步补丁。

## 9.7 现状与迁移事实

当前 `ExecutableGraphPlan` 共享 immutable `NativeGraphCode`，`NativeGraphInvocations` 持 move-only `NativeGraphBacking/GraphSlot`，真实 Vulkan Sync2/Queue/ABI 正负例已经实现。新设计需要**合并职责和清理命名/双份状态**，不是在旧接口上再套 `Graph+GraphExecutionContext+Executor` 三层空转发。由于 F4/F5/F6.b 都有历史 PARTIAL 性能资格，任何移植影响必须提交同负载比较、I/V 完整原始证据。


---


<!-- SOURCE FILE: 10_SceneViewTarget与ViewGroup.md -->


# 10 — Backend Scene/View/Target、ViewGroup 与单帧输出

## 10.1 Backend-neutral Scene/View/Target

`Scene` 的唯一业务职责：资源域关联、逻辑 Scene 值和 Scene-owned View 容器；不持 Feature、Graph、Native PSO。`View` 包含 Camera/History validity/epoch、ResourceDomainId、RenderEffectId、启用状态等，不存可自由修改的“当前 GraphId 权威”。Backend 可为 View 维护派生 graph/context handle，但在 Effect 版本变更时以统一发布事实驱动。`Target` 是独立 output 语义对象：`TargetDesc` 指定 Offscreen/Surface、颜色/深度/额外语义输出、采样数/尺寸/resize；`TargetLayer`/多 View input 描述 compositor graph 的显式层顺序。三个公共类型不能包含 Vk 头和 WSI owner。

## 10.2 `execute(graph,view,target)` 的精确定义

一次成功接纳 `execute()` 是一张完整 Graph 的执行任务；其 Pass 已含全部物理中间过程，最终写出 Target 的完整输出（GPU completion 后可显示/readback），不允许在外部偷偷执行独立的 Tonemap、Lighting 或 UI 后处理。普通 View→Target 一对一时使用三参接口；同一 Target 多个 View 组合要求 Effect Graph **显式声明多个 View 输入和 Composite Pass**，然后一个完成目标的 Graph 进行执行，内部可使用借来的 `span<View>`。若只是把已完成 View Target 最终画面复制给第二个 Target，必须通过明确的目标输出引用/导出方式或在该目标 Graph 内表达，而不是重做每个 Scene Pass。

跨帧 history 是显式 persistent import/export；别的 Offscreen Target 的**最终结果**是独立存在的实际 GPU 资产，可在别的 Graph 显式导入。在这种受控情形，Backend 需要 Target job 的 readiness/dependency 次序，但不能变成对同一 Graph 的中间 texture 进行额外 Graph 调度。

## 10.3 ViewGroup 精确语义

`RenderGraphId → 该编译产物下的准备执行 Job 集合`；Job 是值型 `(GraphId, TargetId, ViewIds/Layer inputs, ContextHandle, output operation)`。为保持必要 Target/Layers 依赖，分组是合法顺序里的**连续相容段**，相同 GraphId 可以出现在多个不相邻的段，不要求为此复制 Native Code 或增加第二套 Graph instance table。只有真实完全独立的 Job 才可安全重新排序改善 Pipeline/Descriptor locality。稳定拓扑在 Effect/View/Target 变更时冷态更新索引，不每帧重新创建 map/vector。

```cpp
// 实际 executor API 可以有 Frame-bound target acquisition；以下仅展示责任
prepareFrame(); // acquire/preflight + ready/queue capacity of ALL jobs
for (auto& group : view_groups) {
    auto& graph = graphs.get(group.graph_id);
    for (auto job : group.jobs()) {
        executor.execute(graph, job.views, targets.get(job.target), contexts.get(job.context));
    }
}
finishTargetsAndPresent(); // only WSI finalization/export; no hidden GPU rendering graph
```

## 10.4 GraphId 与 Effect 更新绑定

View 的唯一公共选择是 `effect_id`。Backend 根据 Effect active revision、Device enabled caps、Target compatibility 和 Shader/Graph identity 获取 GraphId、Context，并放进分组。Effect 修改可能一对多新 Graph 编译；候选先对每个当前使用 Effect 的 View 验证 Native Graph/资源/History/fallback/Slot，全部成功才整体切换。旧 Graph/Context 在途 GPU 工作不受影响。

## 10.5 共享与独立执行状态

Scene A/B 的 View 可以共享同一 Graph code/PSO/Shader，不共享 Scene 数据、View Camera/HZB/TAA 或 Descriptor Instance。Scene-scope Pass 仅在**相同 Scene、真实 inputs/revisions/side-effects 可证**的 View 间复用。View-scope work 如 Cull/HZB/GBuffer 各自执行；物理 multiview 必须按实际设备和 shader/array-view 支持另行选择，有 separate-pass fallback。

## 10.6 Target Size 与 History

Graph 可按 Target-independent layout 复用，但 Format/Samples/LocalRead/必要 Relative Extent 变化时须产生兼容 Native variant。View 历史关联的 Native resource 必须在 resize、camera cut、dynamic resolution、Effect topology/History ABI change 时明确更新 epoch/fallback，不能用 Scene revision 假装 History 有效。一个 View 面对两个不同尺寸 Target 需要 Context 区分，不要求给 View 复制两个 Camera/Scene 业务权威。

## 10.7 Runtime 独立帧节奏与故障

Backend 即便 PROGRAM 空也按合法 Target deadline 渲染，CPU Sim 10Hz + Render 144Hz 是明确 Golden。所有 Target inactive/minimized 时应无无意义空转。Open/Close/Resize/Stop/Device Lost 的 GPU/WSI completion 真实受证，Failed resize/Graph update last-good 不被破坏。Capacities 明确 bounded，实际 FIFO/Views/Native slots 不等于 `capacity.views * FIF` 的无条件物理预分配。


---


<!-- SOURCE FILE: 11_插件SDK与扩展边界.md -->


# 11 — 安装式 Feature/Data SDK、插件 ABI 与代码保留

## 11.1 插件有两个真实入口

**作者 C++20**：内置/外部 Feature 可以通过 `build(RenderGraphBuilder&,settings)` 声明 Pass，使用已有 `DataSchema<T>`、`PassSchema<P>`、Resource requirement、Target output 和能力合同，不含 Native Vulkan/D3D12 私有头。不实现某个 Hook 的具体类型不生成空回调，动态注册边界冷态生成固定 `FeatureOps`，Graph 执行不会在每帧调用插件功能类。**版本化 C ABI**：插件模块提供 canonical FeatureTypeId/Schema/type descriptors、函数表和 abi/struct_size，Host 验证和深拷贝稳定字段；禁止跨 DLL owning STL、异常跨界、裸 `typeid.hash_code` 身份。

## 11.2 注册、组装、卸载是三个不同阶段

1. **RegisterType**：仅声明 Feature/数据类型和操作，检验 canonical full shape、版本、冲突和 enabled device caps，不分配一份 GPU backing 或强行创建 Graph。
2. **Effect composition**：Effect 使用注册的 Feature types 与配置构建 Graph IR；required/optional resource/fallback/Target/Writer 欲望先 cold resolve。需要的资源由 Root/Scene/View Domain 在执行绑定时提供，不给 Feature 默认拥有一份 GPU Store。
3. **Unload**：首先阻止新 Effect/Graph/消息使用该模块；对引用它的 Effect 先生成合法替换候选并完整发布；quiesce CPU callbacks；Native Graph/Context/Resource codepin 按所有 in-flight GPU tickets、WSI/析构借用安全退出后释放模块。不可简单因 Graph 编译完成就卸载其仍依赖的插件自定义析构/Codec 代码。

## 11.3 注册相容性与插件资源

插件 A 可以只声明 `GaussianChunkGpu` Schema 而不贡献 Pass；插件 B 可以消费该类型，并通过已有 Chunk Buffer 接入自定义 RenderEffect。若已有 `LightGpu/MaterialGpu/TextureStorage` 足够，直接引用公共类型，不复制 `PluginLightGpu`。同名 Type 不同 layout/ABI 必须拒绝或经明确迁移事务升级。插件资源在 Domain 中的逻辑 lifetime 与任何 RenderEffect/Feature 的删除独立；若资源需要插件定义的析构和 allocator，Host 必须 pin DLL 直到该资源真正释放。

## 11.4 GPU 安全边界

第三方只用 PassSchema 声明 Graphics/Compute/Transfer 真实资源访问，Native Compiler 与 Executor 决定具体 Vulkan/D3D12 命令。Plugin 不能直接 `vkCmdPipelineBarrier` 绕过 hazard 或把 `VkDevice*` 借给长期代码。确需 CUDA/RT/Mesh Shader/Interop 时添加设备能力与受限 backend-native extension，插件没有声明该能力不能默默使用。Device capability supported≠enabled；不支持必须 fallback/reject。

## 11.5 Editor 参数配置与共享效果

RenderEffect Editor 只查看 type/params 的安全快照和 revision；编辑一个共享 RenderEffect 参数时，所有使用此 Effect 的 View 采用同一新版本。想对单 View 生效，复制/选择不同 Effect，或使用显式局部 override。结构修改候选失败时 old Graph 保持有效，插件代码不能通过“注册之后的原始对象指针”修改已编译不可变 Graph。

## 11.6 Installed SDK 强制 Golden

两个真正外部进程/插件独立于源码树进行 build/install/run：一个插件读取内置 LightGpu 并增加正确 Compute/Graphics Pass，卸载后其他 Feature 继续使用共享 GPU 资源；另一插件声明 GaussianChunkGpu，并由另一 Feature 读取，真实 Upload/Compute/Readback、ABI 冲突、错误 owner/generation、热更 Effect、GPU in-flight 后安全卸载。检查 SDK 不包含 `pinclude/sinclude` 或 backend 私有 headers，release/debug/ASan 和代码 pin 行为真实有效。未做这些测试不能称“插件 SDK 已实现”。


---


<!-- SOURCE FILE: 12_Legacy功能完整迁移.md -->


# 12 — Legacy 36 业务功能、44 基础合同的完整迁移

## 12.1 核对权威

冻结 `render_legacy/**` 719 个源文件与原 tree `a669409a289a6fa4092f21176397795b1cdb7f3e`，逐 Blob/Mode/Size 验证，不可编辑。原 `F01–F36` 36 项业务、`I01–I44` 44 项基础设施/新增合同，加 H01–H06 共 86 项，本包 `appendix/CapabilityMatrix.csv` 一一对应。原 25 个 Golden W 与 51 条 Requirements 继续按原 ID 追踪；新列指向新的 `RenderEffect/Graph/Domain` 对象模型。历史源码名与 OperationSchema 仅作为**来源证据**，不是新类型命名要求。

## 12.2 旧 Feature 的职责重新拆分，而非删除需求

| Legacy 业务形态 | 新模型里的数据与 Owner | 新 RenderEffect/Feature 贡献 |
|---|---|---|
| MeshStack/Forward/Deferred/GPU Cull | Root MeshStorage/Material/Texture + Scene instance/transform GPU resource | Forward/Deferred Effect 添加 Cull/Indirect/Depth/GBuffer/PBR Pass |
| LightProvider/Cluster | Scene LightBuffer；GPU Cluster/visible list 按 Scene/View 使用 | Lighting Feature 定义 Cluster Compute 与 Shader 消费 |
| Shadow PCF/EVSM/CSM | Scene/Root Atlas/Shadow residency，View cascades/metadata | Effect 中的 Shadow Pass/算法变体，必要 GPU feedback/history |
| Camera/HZB/TAA | View ResourceDomain Camera/History/Depth mips | Graph 中 camera consume/HZB chain/temporal pass |
| Material/ShaderVariant | Root catalog + shader/toolchain cache | Effect Shader ABI、完整 Layout/PSO variants、Hot params |
| Canvas2D/Grid/Gizmo/UI | Scene或View 的 Sprite/Tile/Line/PixelField数据 | 单张输出 Graph 内 2D/UI/Composite Pass；不创建 Mesh Instance |
| Terrain/Water/Trajectory | Scene/Root page/vertex/trajectory/feedback backing | 流式地形/水面/Overlay Pass 与显式 Dependency |
| PointCloud Simple/GPUDriven/LOD/Splat/Transient | Chunk/octree/point data，View-specific visible/sort buffers | 五种独立正确渲染算法，绝不把 soft splat 冒充 Full 3DGS |
| Robotics/Picking/Selection | Scene/Editor controlled data，View readback/Target semantic | Picking/Highlight/Overlay 的真实 GPU/读回路径 |

`LightFeature`、`StandardMeshStackFeature`、`StandardMaterialFeature` 在 Legacy 中确有“只创建资源、不产生 Pass”的实际行为；新实现把真实资源提供给 ResourceDomain 的协议或资源构造操作，**不需要**按这些旧类名复制一个空 `RenderFeature`。`TonemapFeature` 的 Hot Params 搬到 RenderEffect/执行参数，Shadow View-slice 产生的 per-view 数据搬到可验证的 View Context/domain，GPU Pass 仍由 Feature build。

## 12.3 F01–F36 逐组进场标准

| 分组 | IDs | 最低不可删业务行为 |
|---|---|---|
| Legacy Geometry A | F01–F03 | Mesh instance/transform/visibility/retire、Forward GPU-driven、Deferred GBuffer，实际 GPU draw output |
| Legacy Light & Material | F04–F07 | Clustered Light/Provider、Material Graph/Shader variants、Depth Prepass |
| Legacy Shadow & Visibility | F08–F14 | PCF/EVSM/CSM、MeshShadow、HZB、Spatial Cull、GPU Skinning、RenderCluster/GPU picking、Camera |
| Legacy Postprocessing | F15–F20 | Tonemap/exposure、SSAO、LinearDepth、Fog、Highlight、Skybox |
| Legacy Editor/2D | F21–F25 | Grid2D/3D、Gizmo line/tri、Canvas2D image/pixel-field/tile/grouping |
| Legacy Geo/Robotics | F26–F29 | Terrain streaming/LOD、Water、Trajectory、StreamingFeedback |
| Legacy PointCloud/Robotics | F30–F36 | Simple、GPUDriven、LOD、SoftSplat、Transient 五算法、GPU octree/point storage、Scene trajectory buffer |

原 `I01–I44` 在新文档中明确重分配：Graph Builder/Compiler/Sync/Transient→`graph/`、`backend/vulkan/graph/`；Shader/Toolchain→`data/graph`+现有工具链；Frame/Target/Runtime→`RenderBackend`、`Target`、`vulkan::Executor`；Asset/ECS projection→Engine RenderSystem+Domain；Plugin→`effect/`+ABI；先进的新需求 I31–44 继续分别进行 GPU oracle，不能只靠类名存在判 PASS。

## 12.4 每项迁移的证据合同

每条 F/I/H 须交付：原 Legacy SourceFile/operation/Shader 特性 → 新 Feature 构建行为/资源来源及所有权 → 中立 Graph Pass/IR access → Native Shader/Layout/PSO/Executor/Target 正确结果 → 真实 GPU 图像/数值参考、性能/VRAM、容量、resource ready/retirement、负例/GPU SyncValidation/ASan → Implementation I / Verification V/代码 SHA。没有业务消费者的 `DataSchema` Test 不代表该 Feature 已迁移。

## 12.5 不允许退化

- GPU-driven 的 Cull/Indirect/Bone/PointCloud 不得退回每帧 CPU 全量实例遍历；Shader 的属性和材料语义与旧画质指标不能偷降。
- Canvas2D/PointCloud/Terrain/Water/Trajectory 无需强制 Mesh Instance/Material，而它们的特殊操作、shader 变体与输出必须保留。
- Legacy 多窗口/Offscreen/Custom UI Composite 功能必须通过**声明在完整 Graph 内的 Target 输出和受限插件合成**恢复，不能另建 UI Renderer/隐形 Graph。
- 当前产品停留在 V2 核心测试阶段：即使 F6.b 200/200 Normal/ASan，不能声明业务 F01–F36 或先进 H 通过。


---


<!-- SOURCE FILE: 13_先进渲染兼容性.md -->


# 13 — H1–H6 高级渲染所需机制及按需 Native 扩展

## 13.1 UE 等级的目标与可验证承诺

目标是具备可实现 Nanite-like Virtual Geometry、Lumen-like Dynamic GI/Reflection、TSR、VSM、Layered Materials/Volumes、3DGS/4D Mixed Rendering 的**算法落点**，而非宣称复制 Unreal 的私有实现。先恢复 F01–F36 的基础光栅/Editor/2D 质量 fallback，各 H 项再单独固定质量参考、正确性 oracle、GPU timing、显存预算、跨 View/Device fallback。Shader 能编译/Graph 能排序不等于算法 SOTA 已完成。

## 13.2 H1 — Virtual Geometry

Root GPU geometry chunk/cluster hierarchy/page backing，可共享多个 Scene；Scene Instances/Transforms 独立；View HZB/Visibility 与 GPU feedback 持续。Effect 组合 ClusterCull→PageResidency Feedback→Indirect/Mesh draw→Target final。真正缺 page 时上级 coarse fallback；GPU-only residency map 不镜像至全体 ECS 记录。Native 必要时扩展 indexed indirect count、mesh/task shader command 与 device capability，不能新增 `VirtualGeometryRenderer`。

## 13.3 H2 — Dynamic GI/Reflections

Scene SDF/Proxy/Surface cache/ProbeRadiance、View normal/depth/motion/history，均经合适 Domain 归属。Effect 包含 screen-space/software tracing、可选 RT/RayQuery、radiance cache updates 和 denoise，并在一个 Graph 内声明 access/order。需要时 Native 新增 3D Texture/AccelerationStructure 与 Shader Stage/Profile；低等级硬件回退 SSAO/SSR/IBL，Device create 阶段明确 enabled features。

## 13.4 H3 — TSR-like Reconstruction

View History、Jitter/current previous transforms、Motion/Reactive/Depth/Color 与 Dynamic Resolution。Graph 内有 prepare/velocity/resolve/reprojection/temporal accumulate/upscale/tonemap，完整输出写 Target；跨帧 History 显式 Import 与 Export 而非隐藏中间图像。相机 cut、target resize、history epoch、jitter 重置须可验证，比较 ghosting/disocclusion/flicker/清晰度与性能，合法回退 TAA/native scale/FXAA。

## 13.5 H4 — Virtual Shadow Maps

Scene/Root 共享 ShadowAtlas residency/PageTable、GPU feedback/page requests、dirty regions，View/Light-dependent projection 和必要分区。Effect Graph 包含 feedback/priority/page assign/draw/filter/coarse fallback。真实 Native physical page residency 能安全延期回收，不能为 VSM 复制第二套 GPU Device/Allocator 或为了 Page 生命周期滥用 Graph transient。旧 PCF/EVSM/CSM fallback 始终保持。

## 13.6 H5 — Advanced Materials、Volume、Multiview

Root Material/Texture catalog 及不同 Instance Shader binding，支持完整 BindingLayout/DescriptorIndexing 的能力协商；Layered BSDF/GGX/clearcoat/sheen 等按真实 Shader 变体与质量参考验收。Volume/Atmosphere/Fog/DOF/Bloom 使用 3D Images 与多 Pass/History，Device 无对应功能时合法退化。多 View 可以共享 Effect/Graph 结构，但 Shader 使用的 Camera/History/Target 各自绑定；Vulkan native multiview 只是有证明的物理优化，非唯一实现。

## 13.7 H6 — Full 3D Gaussian Splatting / 4D

独立 `GaussianChunkGpu` + Chunk Bounds/Page/LOD、View Projection/TileRanges/Sort Scratch/Depth Merge；GPU pipeline 做 frustum/tiling/binning/sort/front-to-back transmittance/composition，和 Mesh/PointCloud 的深度正确融合。Chunk 不等于 MeshInstance，也不应该为一百万个 Gaussian 创建一百万个 C++ handle。Graph 仍只见 Buffer/Image/typed access/Queue，新增一种算法不会修改 Scene/Backend `switch (GeometryKind)`。4D deformation/temporal tracking 是之后单独算法，不因 H6 SPV/GPU oracle 自动完成。

## 13.8 不随 H 扩展而新增类型帝国

所有 H 作为 RenderEffect/Feature build 的算法集合，不为每项建立第二个 RenderBackend、FrameLoop、RenderGraphCompiler、Shader parser、ResourceRegistry 或独立 Target compositor。真实新 Native primitive 只在 IR schema/DeviceCaps/具体 Native Compiler/Executor 扩展，与旧正确性证据进行影响评估。公开 API 给插件使用的 Capability/Pass 必须经 installed SDK real GPU 验证。


---


<!-- SOURCE FILE: 14_Cpp20类型和编码纪律.md -->


# 14 — C++20 类型美学、单一职责和性能纪律

## 14.1 真正的类型才有 class

| 责任 | 正确形式 | 禁止形式 |
|---|---|---|
| Client 协议端点 | `RenderRuntime` 一个有语义 owner 的类 | `RenderApplication`/`ClientManager`/另一个 EventBus |
| 可编辑渲染程序配置 | `RenderEffect` + 少量 FeatureEntry 值 | EffectManager/FeatureLifecycleController 多层状态镜像 |
| 逻辑 Scene/View | Backend-neutral `Scene`, `View`, `Target` | SceneState 与 RuntimeSceneState 各一份权威 |
| 图构建数据 | `RenderGraphBuilder`, owning IR value | 可变 Native Graph + 外部 Builder/Recorder 多重真相 |
| 逻辑/Native Compiler | 无状态函数/具体 `Compiler` | `ICompilerPass`/Registry/Factory/Manager 全家桶 |
| Native code owner | immutable `vulkan::Graph` + 真实 RAII | 裸 VkPipeline lifetime 无保证 |
| Native 执行 | `vulkan::Executor`, 内部 `GraphExecutionContext` | 第二 GraphInstanceManager，重复 Graph 缓存 |
| 资源存储 | `ResourceDomain` + 唯一 `ResourceRegistry` | VulkanResourceRegistry 与根 ResourceRegistry 双权威 |

`Impl` 仅用于必要的 ABI/编译隔离，不能掩盖职责重叠。真实一次执行需要临时 `Frame` 状态并不意味着需要 `FrameDriver/FrameExecutor/FrameOrchestrator/FrameScheduler/FrameContext` 五类长期转发。普通 free algorithms 可以横跨实现文件组织，但必须明确所属层和不变量。

## 14.2 源码与 SDK 语言合同

只采用 C++20；保留 `RenderResult`/`cxx::expected`，不改用 C++23 `std::expected`, `mdspan`, `ranges::to`；异常/普通 OOM 仍按已有 fatal 合同处理，不为 OOM 再造 Recoverable allocator error。Typed handles 与 Generation、`Concepts/if constexpr` 生成静态可选 FeatureOps；`std::span` 只表达同步借用；Plugin wire 使用固定 versioned C ABI，Host 深拷贝易逃逸文本/元数据，不跨 DLL 保存 STL owning。

## 14.3 Feature 无继承不等于没有复用

Forward/Deferred/Shadow mesh 等共享 Cull/Indirect/Variant Build：抽出普通 C++ 模板/非模板函数并复用具体配置/所需 GPU Resource references。Shared camera/geometry 参数使用普通组合值，真正共享 GPU Buffer 从 Domain 获取，不能让 Feature 复制数据 owner。允许合理的局部 C++ 继承（真实 is-a，不作为 SDK 强制扩展基类）；没有注册的 Method 用 `if constexpr` 跳过，不生成 Dummy hook。

## 14.4 Hot/Cold 路径不可交换

正常 topology 下 `execute` 不分配 C++ heap、无同帧 graph compiler、无按 resource canonical name map 查找、无 Native Pipeline/Layout/DescriptorPool lazy allocation；动态参数通过已准备的 backing/binding slots、更新 Buffer 范围和 revision，不能为每次曝光变化重建 Graph。Cold graph build/full shape comparison 可以 O(N)，但新 Layout Identity 的额外分配和临时串构建必须按 F6.b 原始性能债测量、优化前后同工作负载比较。

## 14.5 生命周期与错误

Graph/Context/Target/Native backing 的析构时机必须可用 GPU Ticket/WSI fence/Plugin code lease 证明；历史 `FrameSerial` 只是逻辑索引，不是 GPU 完成条件。构造候选失败回滚到旧合法状态；不允许 `std::terminate` 掩盖用户可预期的 Unsupported/Busy/Capacity 等错误；对内部绝不可能发生的不变量破坏保留已有 fail-fast，必须有相应最小负例而非只靠 debug assert。

## 14.6 类清单审核方法

新增每个 public 类提交：为什么值/自由函数无法解决、谁实际拥有它的成员、创建和销毁者、线程边界、是否在 GPU in-flight 引用下存活、是否有第二份权威、谁是第一个真实消费者、对 API/分配/性能的可测改善。若没有真实不可替代的生命周期或算法责任，拒绝类型。以 `RenderEffect/Graph/Scene/View/Target/ResourceDomain/Executor` 为有限的主词汇，不引入过多近义 Controller/Manager/State。

## 14.7 调整已有 F6.a/b 下层

允许因新的 Effect/Domain Binding API 改写 Header/类型名称，但禁止掩盖 Full Shader Owner Shape、DataSchema validation、SPIR-V reflection、多 Owner 真实 readback 的正确性验证。历史性能的 F4 cold、F5 Record/Submit、F6.b p95/cold Regression 分别保留。正式 I/V 在每次改动后按真实 include/link DAG 与 GPU/ASan 资格重测受影响部分。


---


<!-- SOURCE FILE: 15_端到端API示例.md -->


# 15 — 端到端 C++20 Authoring / Engine Client / Graph / Executor 使用示例

> 下面 API 全是**目标语义伪代码**，尚非 HEAD `246fe21...` 已安装可编译 SDK；签名/返回值需在 N 阶段先做 public header + clean-clone installed consumer 的 C++20 正负例。Client 的创建、提交、结果均为 R2 异步请求，示例为方便阅读省略某些 poll/await 层。

## E01 共享渲染效果的创建与两个场景使用

```cpp
auto effect_spec = RenderEffectBuilder{};
effect_spec.add<DepthPrepass>(depth_options);
effect_spec.add<GpuCull>(cull_options);
effect_spec.add<DeferredLighting>(light_options);
effect_spec.add<Tonemap>(tonemap_options);
auto effect = runtime.createEffect(std::move(effect_spec).finish());

auto scene_a = runtime.createScene();
auto scene_b = runtime.createScene();
auto view_a = runtime.createView(scene_a);
auto view_b = runtime.createView(scene_b);
runtime.setViewEffect(view_a, effect);
runtime.setViewEffect(view_b, effect);
// 修改一次 Effect 结构，两 View 都在安全 publish 点升级；
// 更新 scene_a 的 LightBuffer 只影响 A 的数据。
```

`RenderEffectId` 维持稳定，两个 View 经过 Native Graph full structural compatibility 可以得到同一个 `RenderGraphId`；它们的 `SceneResourceDomainId` 不相同。Graph shared pipeline/layout，Context/per FIF slot 独立。Effect 结构更新失败时两 View 保留旧版本，不能只更新其中一个。

## E02 Backend-neutral Scene/View/Target 与资源域

```cpp
// 仅 Backend owner-thread；不是 Client 跨线程调用
Scene& scene_a = backend.scene(scene_a_id);
View& view_a = scene_a.view(view_a_id);
const auto& lights = backend.resources(view_a.domain()).get<LightBuffer>();
const auto& mesh_storage = backend.resources(view_a.domain()).get<MeshStorage>();
// 最近 ancestor 读取；写入 LightBuffer 需在实际 owning Scene Domain 上授权。
```

Root MeshStorage 一份 geometry，被 A/B Scene 的不同实例 Transform 引用。Client 通过 GpuResourceId 保留 Asset retain/release，Backend last-use 真实推迟物理销毁。

## E03 RenderEffect 的普通 Feature 及算法复用

```cpp
RenderResult<void> buildCull(RenderGraphBuilder& graph, const CullOptions& p) noexcept;

struct ForwardMesh {
    static RenderResult<void> build(RenderGraphBuilder& graph, const ForwardOptions& options) noexcept {
        if (auto r=buildCull(graph,options.cull); !r) return r;
        return buildForwardDraw(graph,options.draw);
    }
};
struct GBufferMesh {
    static RenderResult<void> build(RenderGraphBuilder& graph, const GBufferOptions& options) noexcept {
        if (auto r=buildCull(graph,options.cull); !r) return r;
        return buildDeferredDraw(graph,options.draw);
    }
};
```

两个 Feature 没有共同虚基类也没有重复的 Native Pipeline owner。`buildCull` 创建 Graph 内部 `VisibleSlots/DrawArgs`（中间变量），Forward/GBuffer 消费真实版本和 Sync。长期 GPU Mesh/Instance/Light 由外部 Domains 注入并在冷态 Binding 准备。

## E04 一张 Graph 产出完整 Target 最终画面

```cpp
RenderGraphBuilder g;
auto target_color = g.importTarget("SceneColor", target_output_contract);
auto depth = g.texture(depth_desc,"Depth");
auto gbuf = g.texture(gbuffer_desc,"GBuffer");
auto hdr = g.texture(hdr_desc,"HDR");
auto args = g.buffer(indirect_desc,"DrawArgs");
g.compute("Cull", cull_shader, cull_params(args));
g.graphics("GBuffer", gbuffer_shader, gbuf_params(gbuf,depth,args));
g.compute("Lighting", lighting_shader, lighting_params(gbuf,depth,hdr));
g.graphics("Tonemap", tonemap_shader, tonemap_params(hdr,target_color));
g.output(target_color);
RenderGraphDefinition def = std::move(g).finish().value();
RenderGraphIR ir = RenderGraphCompiler::compile(def).value();
// Native Compiler compile(ir,caps,target_signature) -> immutable GraphId
// Executor.execute(Graph, View, Target, Context) 后只做合法 Present/Export。
```

这些函数是简化的作者侧语义，真实 Pass 以现有 generated PassSchema、typed access/use 和 Result 表达。GBuffer/HDR/args 没有外部 Domain owner，也不允许 Graph 完成后被另一个 Graph 以“内部变量”方式读取。

## E05 多 View 共享 Effect 的统一结构更新

```cpp
auto e = runtime.effect(effect_id); // Client-side editable snapshot + revision
// 结构更新请求使用 expected revision 防并发冲突
runtime.updateEffect(effect_id, e.revision, [](EffectDraft& x) {
    x.replace<ShadowCSM, VirtualShadowMaps>(new_config);
});
```

Backend 编译所有 Target/Device 需要的 Native Graph 变体，验证 A/B/多 View 的 Domain 资源、History、Capabilities/Context slot，统一发布 active revision。任意无合法 fallback/ready 的 View 失败，整个更新保持 LastGood；Native GPU in-flight 旧 Graph 待 true completion 退休。

## E06 Effect 热参数与 Scene 数据

```cpp
runtime.updateEffectParams(effect_id, TonemapSettings{.exposure=1.2f});
runtime.publishSceneChanges(scene_a, compressed_light_delta);
// 两者均不得因为单个数据值变化而重编 Graph/Shader/PSO。
```

Effect Hot 参数使共用 Effect 的所有 View 在后续逻辑帧统一使用新快照；Scene A Light 仅更新 Scene A backing，不影响 B 的资源权威。若要求 View A 单独曝光，则建立独立 Effect 或有明确声明的 View override contract。

## E07 多 View 同一 Target + UI 合成

一个 Editor Target 可以输出 Camera A + Camera B + UI/Overlay。`TargetDesc/TargetLayer` 首先确定无歧义的区域、Blend/LoadStore、格式和顺序，Effect/Graph Builder 根据该 Target 的组合定义生成一张完整 Graph，提供多个 View 源和 Composite/Overlay Pass，`execute(graph, views, target)` **只执行一次完整的最终输出程序**。UI 自定义 Native 操作必须通过受限 Pass 接口申明访问，不能在 Graph 外私自 `vkCmdDraw` 改变最终像素。不同 Target 之间只可显式导入另一个 Offscreen Target 已经作为最终输出发布的 Texture，而不是 Graph 中间图。

## E08 GPU-authored Particle / HZB / 3DGS

GPU Particle Buffer 在 View/Scene Domain 创建，有 GPU-authority；Graph Pass Compute Update→Graphics Read，下一帧 History/import Ready 由真实 Queue Ticket 追踪，无 CPU 全量 readback。新 3DGS Feature 声明 `GaussianChunkGpu` + Projection/Sort/Splat 等 Pass，不要求 Mesh Instance 或核心修改 `switch(GeometryKind)`。`DataSchema<GaussianChunkGpu>` 与 Shader reflection 必须在真正 GPU oracle 中核验。

## E09 插件与 Installed SDK

插件注册 `FeatureOps` 及可选 `DataSchema<T>`，由 Effect 组合构建 Graph。Host 将插件的描述字段完整复制到稳定 Type Registry，并在任何旧 Graph Native owner、custom resource destructors 和 CPU callbacks 在途时 pin 插件模块；未提供 GPU capability 的插件应获得明确错误。外部 installed SDK Consumer 编译时不访问 `runtime/pinclude` 或 `backend/vulkan` 私有文件。

## E10 必须测试的负例

`GraphId` 被当成 `EffectId`；修改共享 Effect 只有一个 View 生效；ResourceDomain 从子域误写 Root；SceneB 借用 SceneA LightBuffer；外部 GPU 资产在 last use 完成前被回收；不同类型同 sizeof 错绑定；同 T/main 与 preview 被错误合并；Graph 中间 HDR 被当 persistent Image；Target 更新/缩放导致 History 旧 slot 生效；跨插件 DLL 析构或 callback 在卸载后调用；不同 Native VkImage 同 memory overlap 导致未证明 alias；未启用 RT/Mesh capability 仍执行；Record 热态重复 PSO 创建。


---


<!-- SOURCE FILE: 16_性能模型与可比实验.md -->


# 16 — 性能模型、Graph Cache、内存容量和不可豁免的历史债

## 16.1 真正优化的对象

CPU 性能：已编译 Graph 在多个 View 使用不重复 Shader/PSO/Layout、Executor 以预编 Recipe 减少 native cmd bind/barrier、无热态 C++ heap allocation、Scene ECS 变更发布 O(D) 而非 O(N)。GPU 性能：Shader Workset（Cull/Arena/Compact/Indirect）按实际 bandwidth/occupancy/alias/cache 优化，绝不以 GPU 非常规 `GeometryKind` switch 换取抽象 API。内存：Graph code 共享，Native ExecutionSlot/attachments 按真实并发 Job 而非 `capacity.views × FIF × variantCount` 最大值预分配。

## 16.2 度量定义

| 符号 | 含义 |
|---|---|
| `N` | CPU ECS 源记录总量/指定业务负载 |
| `D` | 一个发布区间真正改变的记录数 |
| `A` | GPU 参与 Cull 的 Active candidates |
| `V` | GPU Cull 后的可见 work items |
| `K` | Pipeline/Material-compatible draw groups |
| `S` | Dirty packing 形成的复制/上传 ranges |
| `G` | Effect 经真实 Target specialization 产生的 Graph 数 |
| `J` | 同帧实际需要执行的 View/Target jobs 数 |
| `F` | frames in flight |

目标 CPU dirty extraction 接近 O(D)、CPU 绑定不随未变化 N 每帧做 map 查询；Native code 数与不同的实际 Graph compatibility signature 相关而非 Scene/Views 个数；物理 Slot 总量与真实并发 job/FIF 需求相关，不依赖对整个 App 总 View 的悲观复制。No-lazy-PSO/no-heap 在固定拓扑被明确证实。

## 16.3 实验切分

Cold：Effect composition、Graph IR C0–C8、Full Schema/Binding identity、Shader relocation/Native C9–C13、PSO/Layout allocations、Graph cache hits、Native Context attach/resize、真实 GPU bytes used/reserved。Hot：Engine Δ publication、Transport admission/backpressure、per Graph ViewGroup iteration、Record/Submit p50/p95/max/cycles、Native `vkCmdBind*` 次数、GPU timestamp、显存 page/alias、WSI/present times。分别报告 CPU/GPU 耗时，不将排队延迟算成 GPU kernel execution。Asset Retain/Release 缓存行为按 device pressure 真实测量，不称 optional policy 已完成。

## 16.4 必须逐项保留的当前历史状态（2026-10-12）

| 阶段/指标 | 已报告状态 | 本次设计处理 |
|---|---|---|
| F4 C9/Full Native cold | `PARTIAL`、历史限定延期 | 不回写历史门禁，架构更换后仍需 A/B 验证 |
| F5 native record/submit p95 | `PARTIAL`，此前 CI 上界约 +7.9439% | 不因“新 Executor”名称直接关闭 |
| F6.b fixed_native_binding | `PASS`，p50 约 -1.6949%，p95 约 +0.4000% | 作为 Binding 正确实现的比较证据 |
| F6.b Record/Submit 256×256 p95 | `PARTIAL`，配对 p95 +5.0936%，CI95 约 [-13.7143%, +17.8182%] | 不断言真实回退已证实或已排除 |
| F6.b `layout_full_shape` cold | 回退实测，约 600ns→1700ns p50；10→24 分配 | 完整身份检查必须保留，优化临时 canonical construction |
| V2 Product/Legacy parity | `EXPECTED_UNAVAILABLE` | 不把 Graph/SDK 机制测试等于 F01–F36 已迁移 |

上述来源：仓库 `docs/render-v2/F6B_VERIFICATION.md` 和原 F4/F5 报告，不是本包重新跑的 benchmark 或 GPU 证据。比对 Native 后端时使用真实锁定 workload、AA/BB 自比、AB 配对流程、95% CI 及 5% 已批准门槛；不通过重试直到得到好结果。

## 16.5 真负载矩阵

1 Scene/1 View, 1 Scene/4 Views, 2 Scenes/4 Views 共享一份 Effect/Graph，2/3 FIF，HDR/SDR/Target differing; 1080p/4K GBuffer/Shadow/HZB/Tonemap + Headless/Offscreen/双 Surface；10Hz ECS/144Hz backend 无 PROGRAM 持续输出；Asset Root 跨 Scene retain/release、graph effect hot mutate A/B atomic; N=10k/100k/1M × D 比率 {0,0.1,1,10,100}% × clustered/random；64+ imported native subresource cells 的 alias/ready validation 成本；Device Lost/Resize/WSI semaphores；GPU-only Particle/3DGS Chunk/Data Schema 两真实 Owner。

## 16.6 质量不牺牲性能

同分辨率/场景/Shader 输出、相同 GPU 工作量与确定性参考下测前后差异，不能通过删 Shadow/GBuffer/HZB Pass 让新架构变快。每个 H1–H6 真实 GPU 功能还需独立质量/reference 与 budget 证据，不能拿 UE 官方宣传指标冒充 LuxEngine 本机 SOTA。任何新布局（AoS/SoA/page/indirection）必须给出真实 Shader throughput/caches 的测量，不把 CPU sparse O(1) 算法当成 GPU 更快的证据。


---


<!-- SOURCE FILE: 17_完整验收与正确性矩阵.md -->


# 17 — 新对象模型的端到端 Golden 与正确性矩阵

## 17.1 四类必须分开的事实

- **Docs/Source**：类型职责、所有权树、Backend-neutral include DAG、头文件安装、同一数据事实和没有多重 Registry/FrameLoop。设计文档审查通过不能称 GPU PASS。
- **CPU/Protocol**：Effect revision/GpuAsset retain, Scene/View/Target IDs/generation/Domain parent rules、CPU dirty, 三 Lane backpressure、Plugin ABI 的编译期与单元负例。
- **Native GPU**：真实 Vulkan Validation/SyncValidation、跨 Queue/Memory alias、Shader ABI 和 output readback、History/Resize/Presentation/Device Lost/Retirement。
- **Performance/Product**：hot 0 allocation、Record Submit/compile/VRAM/p95 CI、真实 F01–F36/I01–I44、Editor/Player installed SDK 与先进 H01–H06 独立画质。

## 17.2 新模型专属验证（NM01–NM18）

| Gate | 必须证明的正/负例 |
|---|---|
| NM01 | 纯 Client RenderRuntime 和中立 Scene/View/Target/Effect/GraphIR 公共头无 Vulkan headers，DX12 skeleton 不通过“包装 Vulkan”作弊 |
| NM02 | Backend 是 Scene 的唯一逻辑 owner；Scene owns View；删 Scene 时 View/Domain/Target relations 与 GPU receipts 对齐，不存在 RuntimeViewState 第二份权威 |
| NM03 | 相同 RenderEffectId 的 2 Scene/4 View 共用结构一致的 Native GraphId，GPU Scene data 不串话，4 View 独立 Camera/History/FIF |
| NM04 | E1 结构修改产生 G2，所有 E1 View 原子切换；任何不兼容 resource/target/capability 使整体 last-good rollback；E2/G1 不受影响 |
| NM05 | Effect Hot Tonemap/Exposure 参数不触发 shader/PSO/Graph recompilation；Scene Light 值更新仅更新对应 Scene GPU backing |
| NM06 | E1/E2 不同 EffectId 产生相同结构 Graph 时同一 Native Graph cache 命中，删除 Effect E1 不使 E2 Graph 失效 |
| NM07 | Group by RenderGraphId 且 Target final work order 正确：一个 Graph 以多个非连续批次出现时依赖正确，不重复 Native Code |
| NM08 | Graph 内 GBuffer/HDR/Tonemap scratch 只在 execute 生命周期；未 export 无法跨 Graph read；单 execute 产出可见 final Target |
| NM09 | 多 View 一个 Target 的 Camera/Layer/Overlay/ImGui（受限 Pass）在一张完整 Graph 合法合成；Offscreen/Surface/resize/WSI 输出 oracle |
| NM10 | Executor/Context 不持另一份 PSO/Graph；真实 FIF slots 容量按 J、Context/Graph hot replacement GPU last-use 退休，禁止最大 Views 预分配 |
| NM11 | ResourceDomain Root/Scene/View 继承只读查找、同 T 多 key、遮蔽歧义、parent revoke/child teardown、cross-scene denial、wrong device |
| NM12 | ECS-owned Light/Transform CPU 数据不复制第二套常驻 records；发布 O(D)、Backpressure delta preserved、GPU-only particle no fake CPU get |
| NM13 | 数据/Pass 同源 GPU ABI/SPIR-V reflect：main/preview Light + Material 两真 Owner、同 Shader 两 Scene 多 View 完整数值正负例 |
| NM14 | Graph C0–C13 resource version/conditional/fallback/culling exact hazards、真实 barriers、readback host visibility、overlap memory/noalias |
| NM15 | Native bind optimization：Pipeline/Descriptor redundant commands 明确减少或不回退，原固定热分配 0，真实 p50/p95 及 AA/BB CI |
| NM16 | 插件 installed SDK + external Feature 新 Schema/data、effect hot replacement + plugin codepin/unload，在途 GPU 借用正确 |
| NM17 | Engine ECS/Editor uses only Client, no direct Vulkan/Backend resource access，10Hz sim/144Hz render / Stop/DeviceLost/resize |
| NM18 | 原 86 Capability/25 Golden/51 REQ 全部逐项保留审查，H1–H6 按算法实际 GPU/quality/caps 验收而非机制占位 |

## 17.3 原 25 工作负载与 51 要求

`appendix/GoldenWorkloads.csv` 保留 W01–W25 真实 Oracle 和阶段要求，新增新的 Graph/Effect/Domain 证明列。`appendix/RequirementsTrace.csv` 保留 REQ01–REQ51 的需求语义，但**旧 Owner/章节引用被重写为 RD3 责任归属**。`appendix/CapabilityMatrix.csv` 共 86 ID，`TypeOwnership.csv` 是**全新类型库存**，不以旧 TypeOwner 列表冒充新设计。旧 ZIP 文件仍是历史证据，不能从本 ZIP 伪造其 SHA 已提交到 Git。

## 17.4 强制拒绝矩阵

- 身份：过期 Scene/View/Effect/Graph/Domain/Resource generation；foreign Backend/Device/Queue；hash collision 后不做完整相等检查；Effect hot params wrongly included in graph key。
- Domain：子域偷偷写 Root、已撤销 parent 的新访问、同 Type main/preview 绑定错、跨 Scene 借同名本地 Buffer、GPU writer 被 CPU mirror 覆写。
- Graph：缺失 producer、不可达 output、冲突 readers/writers、条件 fallback 未初始化、Time/Camera/History scope 错位、Target format/extent 不兼容。
- Native：未就绪 Imported 资源、不同 image handles 实际 allocation overlap、Queue family transfer 不合法、in-flight Descriptor 改写、partial submit 无 last-use proof、GPU HOST readback barrier 缺失。
- Target/WSI：resize/minimize 期间 Acquire/Present 语义、旧 SWAPCHAIN/ImageView 悬空、Surface lost 错误吞掉、多个 View 层渲染图外再做 hidden draw。
- Plugin：codepin 提前 release、Host 借用临时 metadata、跨 DLL owning STL、unsupported device capabilities 默默默认开启。

## 17.5 独立 I/V/STOP 与污染保护

正式文档门禁只输出 docs/目录内容与 matrix 审计；生产阶段每一张工作单显式列 `ALLOWED/READ-ONLY/FORBIDDEN`、I commit、独立无硬链接/可复核 clean-clone V（V 仅添加验证报告）、source closure/build/ASan、真实 GPU/性能原始样本和 GPU/用户工作区 byte protection，push 后 STOP，不自动进下一阶段。报告明确 `PASS/PARTIAL/NOT_RUN`，尤其 F4/F5/F6.b 现存性能债不能从新架构修订中自动消失。


---


<!-- SOURCE FILE: 18_阶段计划与交付门禁.md -->


# 18 — RD3 实施顺序与最小完整 Gate（新架构候选，全部待授权）

## 18.1 时间事实与状态边界

本包基于 `codex/render-v2` HEAD `246fe21...`，对应最终 F6.b I `7406965112f09807b9f70aa3e739ff43539969c8`、V `246fe21eb6d060801b9a16ddcadde2be89dbbecc`。F6.a 的 DataSchema、F6.b 的多真实 Owner GPU ABI/Binding 已完成对应限定功能的验证；**F6.b 整体 PARTIAL**（Record/Submit p95 不确定且 cold Layout 成本上升），F4 cold 与 F5 native record/submit 债务仍存。`F6.c`、`F6.d`、`F7` 未获得生产授权；旧 RD2 的 F5 `PENDING` 等属于旧时点并不能覆盖后续真实资格。

新计划是一份**替代实施路线的建议**，不是重置既有项目为 R0 或删除已完成 I/V：不重做已经 PASS 的测试只是为了更换名称；如果迁移确实改变下层源码，必须按真实受影响 DAG 重新资格验收。

## 18.2 新阶段及依赖顺序

```text
RD3-D0  文档冻结及完整迁移矩阵（docs-only） → explicit user approval
  ↓
RD3-P1  RenderRuntime Client/Server 装配与中立 Scene/View/Target IDs/Owner
  ↓
RD3-P2  RenderEffect/Feature 作者 API + GraphIR 的真正独立 GraphCache/GraphId
  ↓
RD3-P3  Vulkan Graph/Compiler/Executor/GraphExecutionContext + final Target execution
  ↓
RD3-P4  ResourceDomain/Registry + GPUAsset retain/release + ECS-no-mirror projection
  ↓
RD3-P5  Effect transaction/Full SDK/Plugin/Editor/Engine Client 集成闭环
  ↓
RD3-L1/L2/L3  全量 Legacy F01–F36/I01–I44 恢复 + F11 产品切换
  ↓
H1–H6  各先进图形算法独立 gate
```

Phase P1–P5 的先后可以按真实实现依赖做少量有界合并/重排，但**每个阶段只授权下一张工作单**，不一次性允许全目录重写。P2 使用 F6.a/b 现有 Schema/Binding 与固定测试数据，不必 P4 先实现通用 GPU 资产系统；P3 移植既有 Native Algorithms，不允许暂时重建第二套 Vulkan Queue/Allocator。P4 先用真实 Light/Material/Sprite/Particle 纵向路径，后按需要为大量 Mesh/PointChunk 分配专用 StorageProfiles。

## 18.3 各阶段最小完成条件

| Gate | 交付 | 明确禁令 |
|---|---|---|
| D0 | 全部 RD3 docs/CSV/Manifest/SHA 与旧需求一一对应，实际 Git 受控验证、用户独立批准 | docs-only 时不能改 F2/F3/Vulkan C++、历史 qualification |
| P1 | Client public 无 Native 头、Server Native factory、唯一 Scene owns View，Target/Effect/Graph ID 的协议边界与请求生命周期 | 新 EventBus、RenderRuntime 直接返回 VulkanBackend&、重复 View table |
| P2 | RenderEffect 内容配置、C++20 Feature build/GraphBuilder、CacheIdentity→GraphId，全 Effect revision/last-good CPU fixture，多 Scene View 选择同一效果 | Scene owns Feature/Graph、Graph 长期捕获 Feature this、再造 CompilerPassRegistry |
| P3 | 既有 C9–C13 移植至 Vulkan Compiler/Graph/Executor/Context、真实 target final、FIF/WSI/2–3 FIF/multi View、GPU oracle/Perf | GraphInstanceManager、Native Slot 最大配额满额常驻、隐藏 postprocessing graph |
| P4 | Root/Scene/View Domains 及唯一 Registry、Asset retain/release、GPU ready/retirement、no ECS mirror、O(D) delta | 并行第二 ECS/Native Registry、CPU 释放立即物理回收 |
| P5 | Feature/plugin C ABI+CodePin、Effect hot modify all consumers atomic、Engine/Editor installed consumer, Shader ABI real sample | 插件 private Vulkan、半发布、未定义 DeviceCaps fallback |
| L1–L3 | F01–F36 业务 GPU parity、I01–I44、独立 F11 Editor/Player 产品切换 | 只以能运行 Shader 编译或三角形而声称 36 F 完成 |
| H1–H6 | 固定参考质量、Native 扩展/fallback、跨 View/Target real GPU evidence | 以 Graph 能表示算法为由宣称算法完成 |

## 18.4 生产施工范围、独立资格及 STOP

每阶段开始前，实施方必须读取 HEAD、冻结 Legacy 719、用户 dirty 7 files、历史 I/V reports，按具体文件和真实 Include/Link DAG 设计 `ALLOWED/READ-ONLY/FORBIDDEN`。超出已有受保护下层时先提交最小失败复现和影响评估。I 包含生产修改和测试，V 在独立 clean clone 的精确 I SHA 上验证，**V 只能新增独立验收文档**，不可在 V 混入修复。普通/Full ASan complete all 两轮 + no-work、真实 GPU oracle/SyncValidation、installed SDK、benchmark raw samples 和保护 hash 以阶段影响为准；不能因点估计下于 5% 而无视 CI/AA/BB。Push 后 STOP 等用户审核授权下一阶段。

## 18.5 历史并未被抹掉

若本计划启用，旧 `R0–R4/F0–F6.b` 的 Git 提交、源文件、资格与 `PARTIAL` 继续保留；旧文档 `work_orders/02_F4`, `03_F5` 已经执行或部分收尾，不能作为本方案自动重新授权。新 `work_orders/` 全部重新写成 RD3-D0/P1–P5 工作单，且默认 `DRAFT / NOT_AUTHORIZED`。


---


<!-- SOURCE FILE: 19_RD3架构裁决.md -->


# 19 — RD3 架构裁决与禁止性规则（待用户批准）

> **本页不是历史 RD2-D0 的冻结决策本身。** 原 RD2 的 N01–N17 / FINAL 的 D01–D32 是历史设计来源，已在这次对象模型修订中重新审议。只有用户对 RD3 明确批准后，下面的 RD3-D 编号才成为后续实施的设计权威；Git 已验收测试和 Legacy 操作要求无论是否批准都不能被重写为另一结果。

## 19.1 RD3 核心决策（新，非旧 N 编号的兼容继承）

| ID | 正式目标决策 | 必须拒绝的实现 |
|---|---|---|
| RD3-D01 | `RenderRuntime` 是 Client，`RenderBackend` 是 Server；启动装配而非 client API 知道具体 Native backend | Client 返回 `VulkanBackend&` / 强制包含 Vk 头 |
| RD3-D02 | Scene/View/Target 中立类型，Scene 唯一拥有 View，Backend owns Scene/Target | Renderer 和 Runtime 独立维护两份 Scene/View 值 |
| RD3-D03 | Backend owns `RenderEffectId`：可编辑算法与 Feature 配置，跨 Scene/View 共享 | 每 Scene 默认安装全套 Features 并独占 Graph |
| RD3-D04 | Backend owns `RenderGraphId`：immutable Native compiled program；同构 Graph 共享 | EffectId 与 GraphId 混为一个索引或 Graph 可原地修改 Pass |
| RD3-D05 | View 公开选择权威只有 EffectId；GraphId/ExecutionContext 是 Backend 派生绑定 | 用户分别设置 EffectId 和 GraphId 导致不一致 |
| RD3-D06 | Feature 作者只 build/声明算法；可选状态随 Effect，构建 Graph 不存 Feature 原始引用 | Graph callback 捕获 `[this]` Native record、Feature 必须虚继承 |
| RD3-D07 | RenderGraph 定义全部中间资源和 Pass，单 execute 完成 Target 最终输出 | Graph 内中间 Texture 在另一个隐式 Graph 才完成 Tonemap/Composite |
| RD3-D08 | IR 与 Native graph 分离，前者无具体 API，后者是 immutable data + pinned native owners | GraphIR 泄漏 Vk 对象或 Native Graph 保存 mutable Frame scratch |
| RD3-D09 | 独立 Compiler C0–C8/C9–C13，有真实 bind/barrier 优化；不新增默认动态 CompilerPass 注册系统 | 一个 Feature 一个 CompilerPass，或第二套 Shader Parser |
| RD3-D10 | `GraphExecutionContext` 存 per actual execution config 可变 slots/tickets，Graph 仅保留 code | 每 View 复制 PSO 或按 maxViews×FIF 对每 variant 预分配 GPU backing |
| RD3-D11 | ViewGroup 按 RenderGraphId 组织**合法顺序的连续 job**，非拥有索引 | 简单全局 Graph sort 导致 Target final-output 先后依赖错误 |
| RD3-D12 | Target 中立、有明确 final output/layers；Native WSI/offscreen 实现交给后端 | Target 只等于 VkFramebuffer，隐藏 UI/compositor Renderer |
| RD3-D13 | ResourceDomain 逻辑 owner+父链查找，Registry 唯一通用类型容器 | Vulkan/Scene/Effect 各一份重复 Registry 权威 |
| RD3-D14 | 子 Domain 只读祖先，本域写；多 T Key 明确不歧义，资源修改 cold-binds into Context | 热态每 Pass `std::unordered_map<TypeId>` 或子域写 Root |
| RD3-D15 | Engine ECS/Asset CPU authority，Backend 默认 GPU backing+必要元数据，无第二 ECS CPU 镜像 | F7 Universal TypedStorage 成为所有数据的强制长期 CPU owner |
| RD3-D16 | GPU Resource handle/Domain/Native allocation/Graph-local Resource ID 分离 | CPU release 导致在途 GPU Buffer 立刻被销毁或 old ID 复活 |
| RD3-D17 | Retain/Release 是业务引用操作；未来缓存策略 CPU 可选，Native Retirement 必须 true GPU/WSI complete | FrameSerial/Effect revision 代替 Queue Ticket、热态 `waitIdle` |
| RD3-D18 | Feature/Scheme/Shader 单源生成、BindingLayout 完整结构相容，实际 Domain 绑定留到执行 Context | EFieldOwner::SCENE/FEATURE/PASS 充当真实 Data owner，PSO keyed by SceneId |
| RD3-D19 | Effect 所有受影响 View 的更新是一次候选完整编译与整体安全发布 | 一半 View 已新 Graph、另一半失败，却向 Client 返回成功 |
| RD3-D20 | Effect Hot params/Scene data 不自动重新编译 Graph；Graph 结构/ABI/caps changes 才新 graph key | Exposure/light value 使 Shader/PSO/Native graph 全量 rebuild |
| RD3-D21 | Plugin C++20 concrete Feature + generated optional Ops；extern C ABI、CodePin、installed SDK | 强制 IRenderFeature 虚继承 + 插件可持私有 Native 头或跨 DLL owning STL |
| RD3-D22 | GPU enabled capability 与 Shader/Native op/fallback 精确验证，不宣称未来 H 已实现 | OpenGL 假装支持多 Queue、GPU 能 Shader 编译就称 Nanite/Lumen PASS |
| RD3-D23 | 单 Backend FrameLoop 独立 Sim Tick，三 Lane 不变；统一 GPU/Target submit/present | UI/Terrain/3DGS 独立帧调度或第四 EventBus |
| RD3-D24 | Stable topology Record/Submit 无首方 heap / lazy native construction，冷态有界扩容 | 每帧 Shader Compile、Registry domain lookup 或动态 allocate Pipeline |
| RD3-D25 | Legacy 719 freeze、F01–F36/I01–I44/H01–H06、W01–W25/REQ01–REQ51 不减少 | 改文件名/类型同时删除旧业务或改判历史 PARTIAL |
| RD3-D26 | 每阶段独立 I / clean clone V / STOP、真实 GPU oracle/perf/ASan/质量按影响重测 | 在 doc-only 门禁修改生产 C++；V 中偷偷修生产 |

## 19.2 需求冲突如何解释

本方案不再采用旧 RD2 “每个 Runtime/Scene/View 固定一个 Scope DataSet 必须长期拥有 CPU Records”作为公共基础。ResourceDomain 为 GPU 所有权语义，允许 Backend 自己有真正 CPU 数据容器，但不是第二 ECS；数据来源/authority 由真实 producer 决定。D16 类的原 Binding owner 问题用真实 `BindingLayout` 结构身份解决，资源归属由 DomainId 与 GPU lease 处理。DataSchema 与 PassSchema 的正确性检验继续保留，不允许因术语重构而删去 SPIR-V ABI 负例。

同样，旧 Scene 对 Feature/Graph 的拥有关系已经被替换：RenderEffect 独立于 Scene，Graph 归 Backend；View 选择 Effect，Backend 将其解析成 Graph。Scene 仅拥有本 Scene 的 View/Domain/semantic data，不再负责 effect graph invalidation。GraphExecutionContext 可以在 Effect 替换时保留旧 Native backing，真实 GPU 退休后销毁。

## 19.3 阶段授权优先级

本 ZIP 是**候选**；提交给用户的目标 API 签名不是生产事实。批准顺序：用户审阅本完整方案 → Docs-only Git D0/I/V → 审核 D0 → 明确授权 P1 → 每阶段 I/V/STOP。现有 `codex/render-v2` 的 F6.b 总体 PARTIAL、F4/F5 性能债、F6.c 未授权都是当前已知事实。任何“只要编译通过即视为已授权”均无效。


---


<!-- SOURCE FILE: 20_RD2到RD3迁移对照.md -->


# 20 — 原 RD2 全章与现有源码向 RD3 对象模型的迁移映射

## 20.1 来源、证据与新类型不要混合

原输入：用户上传 `RenderV2_RD2_Architecture_and_Implementation(2).zip`，其中 00–20、5 张 work orders、四表 CSV、合订版及 SHA 清单。交接历史：`RenderV2_RD2_Architecture_and_Implementation_HANDOFF_2026-10-11(3).md` 的 HEAD 为 F5 V `28bff...`，当时 F5 PARTIAL、F6 未授权；此后远端已进到 F6.b Verification V `246fe21...`。**两者属于不同时间的真实快照，不能说交接文档最新或 F6.b 未实施。**

本包重写了正文 21 章、工作单、TypeOwnership、需求/Golden/功能映射和合订版；**没有**编辑用户原 ZIP 或 `render_legacy` 原 Git Blob。历史“Legacy/old type”只存在本页和 Capability 来源字段，用于删除/迁移的审计证明，不构成新 API 类型名。

## 20.2 原 00–20 章到新章的对应

| 原章 | 新章落点 | 真正修订 |
|---|---|---|
| 00 总纲 | 00 | 核心从 Scope DataSet→RenderEffect/Graph/Context/Domain，版本事实更新到 F6.b |
| 01 哲学 | 01、14 | 保留 C++20/单向依赖/热态 0 分配；明确编译程序/Scene 数据独立 |
| 02 模块与所有权 | 02 | Client/Server 分离、中立 Scene/View/Target，Graph & Effect Backend owns |
| 03 DataSchema | 03、08 | 保留完整 Data/Wire/GPU ABI，移除“必须有 Scope DataSet CPU mirror”的预设 |
| 04 TypedStorage | 04 | 使用可层级查找的 ResourceDomain/Registry 和真实 GPU asset，CPU authority 归 ECS |
| 05 通信 | 05 | R2 保留，新增 Effect revision atomic publish 与 ResourceDomain asset handles |
| 06 Feature Composition | 06 | Feature 属 Effect 而非 Scene，无状态优先，资源由 Domains 提供 |
| 07 Graph C0–C13 | 07 | 一份 Graph 最终画面、immutable IR、Native graph、Executor 降命令数 |
| 08 Shader Layout | 08 | BindingLayout 是结构形状，不是 EFieldOwner 资源生命周期身份 |
| 09 Vulkan Native | 09 | Compiler / immutable Graph / Executor / GraphExecutionContext，Code/Slot 分离 |
| 10 Runtime/View/Target | 10 | Target 是完整输出合同，ViewGroup 按 GraphId 非拥有分组，Scene 不持 Graph |
| 11 Plugin | 11 | Effect 作者 API、Host C ABI/CodePin，无 native hot callback 捕获 Feature this |
| 12 Legacy | 12 | 业务资源归 Domains、算法归 Effects，F01–F36/I01–I44 原义全保留 |
| 13 H1–H6 | 13 | 高级算法仍按需求扩展同一 Effect→Graph→Backend，fallback/质量独立 |
| 14 C++20 | 14 | 少类型、无管理层泛滥、强身份、真实 GPU Lifetime |
| 15 API | 15 | 共享 Effect/Graph、跨 Scene View、Domain 与 Executor 的全链演示 |
| 16 性能 | 16 | 引入 Graph caches/context capacity/bind dedup，历史 F6.b debt 不豁免 |
| 17 验收 | 17 | NM01–NM18 新证据，保留 W01–W25/REQ01–REQ51/86 IDs |
| 18 阶段 | 18 | RD3-D0 + P1–P5 + L1–L3 + H1–H6，不再“立即 F6.c Dataset” |
| 19 决策 | 19 | 用 RD3-D01–D26 自主决策，不伪装为旧 N 决策兼容 |
| 20 对照 | 本页 | 全章、源码、工作单和 CSV 类型追踪 |

## 20.3 现有源码到新对象的责任迁移

| 当前 Git 生产机制 | 新的唯一责任位置 | 严格迁移要求 |
|---|---|---|
| `runtime/Runtime.cpp::RenderRuntime` + Builder | `client/RenderRuntime` / `backend/RenderBackend` / Bootstrap native factory | Client 不再直接持 VulkanBackend 的生产 public API，Transport/线程生命周期不丢 |
| `RuntimeBackendState::RuntimeSceneState` | 中立 `Scene` | Scene 只保存语义/Domain/View，删以 Path/Pass 索引为数据权威的 ScenePassValue |
| `RuntimeBackendState::RuntimeViewState` | 中立 `View` + `vulkan::GraphExecutionContext` | View 无 Vk，Native Slot/Ticket 移到 Context，不建立双 View tables |
| `RuntimePathState/RuntimeNativeVariant` | Effect→Graph Cache 的 `RenderGraphId`、Graph specialization、Context pool | 缓存与物理资源责任分开；不能旧旧新新四类共存作长期权威 |
| `LogicalGraphPlan` | 中立 `RenderGraphIR` | 复用 C0–C8 精确版本/Scope/diagnostic，不加第二分析器 |
| `ExecutableGraphPlan/NativeGraphCode` | `vulkan::Graph` | 保留 immutable native code、shader/pipeline ownership，移动真正数据 |
| `NativeGraphInvocations/NativeGraphBacking/GraphSlot` | `vulkan::Executor/GraphExecutionContext/ExecutionSlot` | 真实 Sync2/import/alias/ticket/retirement 保留；避免一层空 wrapper |
| `RuntimeTargetState/PresentationTarget` | 中立 `Target` + `vulkan::target` Native backing | Target final output/WSI/Offscreen/resize/Layer 与 GPU Lifetime 分离 |
| `OwnerShape/OwnerAssignment/LayoutPlan` | 编译器私有 `BindingLayout` / C9 Layout lowering | 不丢 F6.b full shape equality、多真实 Data owners/Shader ABI |
| `DataSchema<T>` | 公共 `data/` 及生成器继续 | 不作为必须建立 CPU Store 的触发器，retain valid CPU/Wire/GPU contracts |
| `SceneGraphCache` / Legacy `RenderFeature` 生命周期 | `RenderEffect` + immutable Backend Graph Cache | Scene 不再拥有 Feature/Graph；静态作者代码不参与 Native hot record |
| Legacy `ResourceRegistry` 全局+场景 | 唯一 `resource/ResourceRegistry`、Backend domains | 根/子 Domain 查找/写权限/真实 GPU asset 退休替代固定类别 |

这些迁移是**设计责任**，不是已运行的机械文件移动。实施前按实际 source/include/Git diff 对应到 ALLOWED scopes 并用最小 GPU/integration tests 锁定每一步。未列的功能不得隐式删除。

## 20.4 原决策 D01–D32 的需求保留/结构替换

| 原范围 | 新合同 |
|---|---|
| D01–D05 | 保留唯一 Frame/Backend、N Scene/View/Target、真实 immutable Graph reuse、可证 Scene work sharing 和 optional multiview；Scene/Graph 归属替换为 Effect→GraphCache |
| D06–D12 | 身份/协议/CPU projection/asset resource 能力保留；数据真实 Owner 变为 Domain，CPU ECS 是唯一权威、Feature 属 Effect |
| D13–D21 | Builder/Graph C0–C13/Shader 同源、full binding shape 与 native staging 全保留；型别和命名可重构 |
| D22–D26 | alias proof、View History、R4 RAII、Queue Sync2/Plugin 都必须保留；History 归 View 资源+Context，Plugin 属 Effect authoring |
| D27–D32 | C++20、错误/线程/Hot zero heap、719 frozen、Legacy full parity、I/V/STOP、独立渲染节奏保留，不自动改历史资格 |

## 20.5 新工作单取代旧工作单

旧 `work_orders/01_D0/02_F4/03_F5/04_F6/05_F7` 都是产生时的历史任务，不应以相同标题在新包继续当未来授权入口。新包 `work_orders/01_RD3_D0` 到 `07_RD3_Legacy` 对新模型按实际变更逐 Gate 施工；F4/F5/F6.a/b 的真实代码与历史 I/V 只读引用，旧性能/错误债在 16/17/18 追踪。

## 20.6 终态限制

业务作者依赖 `Scene/View/Target/Effect/RenderGraphBuilder/ResourceDomain/PassSchema`；Backend Native 作者依赖 `Graph/Compiler/Executor/GraphExecutionContext/Resource`。任何新增抽象必须证明不可能落入已有责任、并以真实第二消费者/重复事实减少/不变量正负例举证，否则拒绝新的 Manager/Context/Registry 层。


---
