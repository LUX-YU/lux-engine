# LuxEngine Render V2 — FINAL Architecture F1 / 2026-10-10 — 合订版

> **只读阅读副本**，权威为 00–19 独立章节（以及受控附录）。修改须回到独立章节，重新生成本副本。

# 00 — LuxEngine Render V2：最终架构与实施入口（F1）

> **状态：FINAL DESIGN / IMPLEMENTATION CONTRACT。** 日期：2026-10-10。用户要求本文件集作为直接交付实施方的目标规范。**本压缩包没有自动修改 GitHub 仓库，也不表示新 API 已实现。** 文档入库/实施按 §00.7 与第 17 章执行。
>
> 基线仓库：`LUX-YU/lux-engine`，远端 `codex/render-v2` 设计审阅 HEAD `7de3ddaa3f7614995745b41d73dfc98302310a4c`（R4 verification）。实施时必须首先读取实时远端 HEAD，若已前进则记录新提交并比较，不得盲目 reset 或覆盖。
>
> 冻结 Legacy 原始 SHA：`a669409a289a6fa4092f21176397795b1cdb7f3e`；719 个旧 Render 文件（加 archive guard）不得改写。R1 Core、R2/R2-FIX Transport、R3 Logical Graph、R4 Vulkan Foundation 的历史资格判定保持不变。**R3 API/实现准许按本设计重写；不能为 API 兼容保留永久并行 Graph 系统。** 已验收 Core/Transport/R4 不得擅自修改。

## 00.1 一句话终态

**一个 Runtime、一个 VulkanBackend、一个完整可编译/执行的 RenderGraph 机制、多个 Scene、每 Scene 多 View；SceneData 持久化，View 是观察者而非 Graph 的默认 owner；同渲染路径的 View 共享编译计划，共享可证明的 Scene 工作，View-local 工作分别或经 Multiview 合并执行。** Engine/Editor/ECS/外部插件经 typed RenderData + CONTROL/PROGRAM/UPLOAD 发布，Shader/Graph 由同一 PassParams 作者契约生成，稳定帧无隐式 Graph rebuild、Shader 编译、Pipeline/Layout 创建或基础设施堆分配。

```text
World / Editor / Asset / Procedural / Plugin Sources
      │                    │
      │ O(changed)         │ shared upload/asset demands
      ▼                    ▼
Scene RenderSystem       RenderResourceDomain (per Runtime integration domain)
(one per Scene)          semantic asset/receipt/retain ledger, NO Vulkan backing owner
      ├── typed RenderProjection / data source
      ├── PROGRAM  owner-thread SPSC     (state deltas)
      ├── CONTROL  owner-thread SPSC     (lifecycle/query/reconfiguration)
      └── UPLOAD   bounded MPMC          (large immutable payload/asset)
                     │
                     ▼
RenderRuntime — one independent backend execution domain, stop/error/reply/pacing
                     │
                     ▼
VulkanBackend — R4 Vulkan native owners + shared compile/exec mechanisms
      ├── SceneRenderer  → N RenderScenes (persistent SceneData/Capabilities/Features)
      │                         └→ N RenderViews (camera/output/history/path-choice)
      ├── Graph Authoring (cold) → Definition → LogicalGraphPlan
      ├── Shader Interface/Descriptor LayoutPlan → Vulkan ExecutableGraphPlan
      ├── Backend-scope compiled-plan cache (not per-View graph by default)
      └── FrameExecutor + VulkanFrameDriver
           ├── shared scene work once per proven dependency/invocation
           ├── view-local work: separate or compatible Vulkan Multiview
           ├── cross-target composition/readback/present
           └── fence-proven lifetime retirement
```

## 00.2 共同哲学与权威术语

- **事实与机制分离**：plain value/强类型 ID 表示数据；普通函数/无状态算法做转换；Builder 只负责暂态声明；RAII class 才拥有资源与生命周期。`Manager/Context/Controller` 名称本身不能证明职责。
- **一份事实只有一个 authority**：Source ECS、Scene-side asset ledger、Backend SceneData、native GPU backing、Graph transient plan 分别仅有一个合法 owner。非拥有索引不产生第二份真相。
- **三种依赖不能混同**：`RenderDataTypeId` = 传输数据型别；`SceneCapabilityId` = 持久 provider 的依赖；`GraphResourceId` = 图内部资源访问/版本。强 ID 之间不得隐式转换。
- **三条时间轴不同**：Simulation tick/revision、Publication revision、Render frame serial/time。任何一步都不强制与另一步一一对应。Render 无 PROGRAM 仍能合法持续绘制最后已提交状态，禁止 busy-spin。
- **Graph 是完整的声明→编译→执行系统**：不能只实现排序；需要 typed PassParams、shader reflection/LayoutPlan、子资源 hazards、schedule/culling、物理资源、barriers、queue sync、native execution、调试诊断。
- **View 是观察者**：不因两个相机不同就建两份图。允许不同 RenderPath；Graph cache 默认 Backend scope，View history/target 仍私有。结果共享必须有数据依赖证明，不能单凭 Pass 名称。
- **Feature 是 concrete composition**：Provider 可以没有 Pass；Effect 可以读多个 Capability；Compute 负责 GPU 工作；类型可组合但不创建一套继承树。内置与外部使用同一公开作者 API。
- **现代 C++20 是设计约束**：Concepts、`if constexpr`、值语义、`std::span` 仅作短 borrow、生成 metadata、零 hot dynamic-dispatch churn。不得暗中使用 C++23 的 `std::expected/std::move_only_function`；继续使用 `cxx::expected`。
- **Legacy 功能等价是不可裁剪项**：Forward/Deferred/PBR、Shadow PCF/EVSM/CSM、HZB/Cluster、Skinning、RenderCluster、五种 PointCloud 策略、Terrain/Water、Canvas2D、Picking、UI/Editor/外部 Feature 等详见第 10 章及 CSV，不是仅支持某个基类。
- **UE 级画质目标需真实算法**：Nanite-like、Lumen-like、TSR-like、VSM-like、3D Gaussian Splatting 与多表示混合 等须独立实现对应可验证 GPU 算法；图“能表达”不等于实现这些算法。见第 11 章。

## 00.3 规范的层级与冲突优先级

1. 本最终文档集 `00–19` 与附录矩阵为**同一个版本的权威目标**；相同概念首先采用第 19 章的冻结决策，再读对应负责章节。
2. `17_LLM实施合同` 的交付、变更与 STOP 门禁，和 `12_Cpp20`、`13_性能生命周期` 的硬性约束同时强制。
3. **冻结旧源码、当前 R0–R4 已发生提交/验收事实不被设计文档追溯修改。** 现有 `docs/render-v2/00–10` 中相冲突的“旧目标描述”需要由首先的文档集成提交逐章替换/标注 superseded，而不是改历史验证报告。
4. 压缩包 `_references_R2_readonly/` 是用户原 R2 历史设计的逐字参考，不参与运行时规范；`appendix/CPP20_讨论稿只读.md` 是早期思想来源，正式规范以第 12 章为准。
5. 真正的第三方/标准（Vulkan、SPIR-V、C++20）要求优先于本设计中任何伪代码。例如 device capability 不支持的优化必须退回保守正确实现，不能伪造 PASS。

## 00.4 文档清单及阅读顺序

| 文件 | 唯一责任 | 关键产物 |
| --- | --- | --- |
| `01_哲学_原则_完整性.md` | 最终设计哲学、事实范围、反偷工减料 | 不变量、保留/重写/新增分类 |
| `02_分层_类型_所有权.md` | 模块 DAG、最终类型、authority/borrow/lifetime | Type Registry / owner 图 |
| `03_通信_数据_事务.md` | PROGRAM/CONTROL/UPLOAD、typed RenderData、订阅/背压、资源事务 | wire + revision/receipt contracts |
| `04_RenderGraph_作者模型.md` | Builder、PassParams typed hooks、资源/Pass/Scope 等语义 | 完整 Authoring API |
| `05_Shader_自动绑定.md` | 同源 codegen、ShaderInfo、LayoutPlan、反射/重定位、材料接口 | 单一 binding authority |
| `06_逻辑图编译_优化.md` | hazards/subresource/culling/conditional/cache/lifetime | `LogicalGraphPlan` |
| `07_Vulkan编译_执行_同步.md` | native plan、queue/barrier/alias、descriptor/pipeline/record | `ExecutableGraphPlan` |
| `08_Runtime_Scene_View_Target.md` | 真实 Runtime 与多 Scene/View/Target、独立进度、Multiview | 真 GPU FrameExecutor |
| `09_Feature_Capability_插件.md` | Provider/effect/compute、配置、代码 pin 和 SDK | concrete Feature + public Ops |
| `10_Legacy功能_业务数据.md` | 各旧特性的输入、输出、资源、Graph 行为、异常 | 完整 parity 路线 |
| `11_UE级画质_先进能力.md` | Nanite/Lumen/TSR/VSM 对齐算法与预算 | 高级画质验收 |
| `12_Cpp20_实现哲学.md` | 现代 C++20、value/free algorithm/Concepts/RAII | 明确语法与禁止样式 |
| `13_错误_性能_生命周期.md` | 失败、异步停止、GPU lifetime、零分配 | 测量与故障注入 |
| `14_API范例_全链路.md` | 7 个可实现的端到端场景 | 编译/生成/GPU oracle |
| `15_实施阶段_工作范围.md` | 真实独立阶段、目录准入、迁移计划 | 阶段 I/V 和 STOP |
| `16_验收与功能矩阵.md` | 每个契约的验收、证明及否决条件 | CSV + qualification |
| `17_LLM实施合同.md` | 给实施 Agent 的唯一执行合同与首阶段授权 | 可复制 work order |
| `18_历史规范迁移.md` | R2→最终设计映射，R0–R4 preservation | 文档迁移规则 |
| `19_冻结决策与歧义消解.md` | 已关闭设计决定和技术降级边界 | D-01～D-32 |

附录：`appendix/LegacyCapabilityMatrix.csv`、`TypeOwnership.csv`、`RequirementsTrace.csv`、`GoldenWorkloads.csv`、`EVIDENCE_INPUTS.md`、`MANIFEST.sha256`。CSV 是执行级追踪文件而不是旁注。

## 00.5 非目标与不允许的替代方案

- 不新增第二 Renderer、FrameLoop、通用 RHI 抽象、Universal History/Resource/Manager、全局 EventBus、默认后台异步 Writer、隐藏锁或无界队列。
- 不用 RenderData `TypeId` 代替 Capability 与 Graph Resource；不允许 Feature 通过私有 VkDevice/RenderScene 头实现 SDK。
- 不让 CPU 不可信 borrow 越过异步边界；不把 GPU fence completion 等同于 CPU frame index；不在录制时临时创建 pipeline/descriptor layouts。
- 不用 mock Vulkan、CTest 数字增长、编译成功、预留 TODO、空 Feature、假兼容层、丢弃历史失败或空文件覆盖来满足验收。
- 不把 `RenderGraph` 简化为一个资源访问数组，也不复制 V1 的巨大 Compiler/Server/Context 层级。

## 00.6 可验证的完整性定义

功能被标为 `IMPLEMENTED + QUALIFIED` 的最低条件同时是：① 独立编译了正式生产目标；② 使用 public author API/真实 Shader/Graph/Backend；③ 真实 GPU 或合适的 CPU oracle 验证具体功能；④ 资源失败/stop/resize/rebase/卸载/背压覆盖适用项；⑤ 真实依赖闭包未越界；⑥ 性能数据同口径；⑦ 验收证据有确切 SHA，且历史 `NOT_RUN` 未伪报 `PASS`。

**完整 Legacy parity 与高画质新算法分别计分。** GPU-Driven PointCloud 的软 splat 并不是 3DGS；SSR 并不是 Lumen；可绘制 Shadow Atlas 并不是 VSM；图存在 motion-vector pass 也不是 TSR。

## 00.7 直接交付实施方的启动规则

开始时必须在独立工作树核实远端 `codex/render-v2` HEAD、R4 I/V 及本包 `MANIFEST.sha256`。首先**只做文档集成阶段 F0**：把 `00–19` 转入当前仓库 `docs/render-v2/final/` 或同等明确且唯一的权威目录，保留原验收报告，标记旧目标规范 superseded，验证目录、依赖和历史提交 unchanged，单独 doc-only commit、独立审核后 STOP。第 15 章为后续阶段目录；**本包不自动授权所有未来开发阶段无审查连跑**。用户在审阅 F0 后按对应 work order 放行 F1、F2 等；用户本次明确要求“可直接执行”的范围是 F0 文档整合及完整后续工作单准备，而不是先改 Vulkan/Graph 再补文档。

实施方不允许以本包某段话未能从当前已安装依赖构建为理由自行删掉要求；要提供可复现的阻断报告和最小设计修订候选，STOP 等用户判决。

---

# 01 — 设计哲学、不可协商原则与 Legacy 功能完整性

## 1.1 为什么这次必须完整设计

之前 R3 CPU Graph 的单元测试和依赖闭包通过，不等于存在成熟可供第三方 Feature 使用的 Graph API。`GraphPass { vector<GraphResourceUse> }` + `CompiledGraphPlan::compile()` 能验证顺序，却没有替代 Legacy 的 `RGBuilder/RGPassBuilder → GraphDescription → Compiler → CompiledGraph → Recorder/Allocator`，也没有替代同源 `LUX_PASS_PARAMS → codegen → LayoutPlan/Shader reflection → SPIR-V relocation`。

本重设计以**真实作者面对的表达力**与**后台可验证的执行计划**为两端，同时保证中间算法是可推理的。LLM 不能为了最小测试跳过真实能力，也不能因为有 Legacy 名称就机械复制旧 owner hierarchy。

## 1.2 四个原则：每段代码都必须可归类

1. **数据（what）**：plain `struct` + strong IDs + value equality；没有为了隐藏 vector 而写 getter/setter。`TextureDesc`、`GraphAccess`、`FrameValues` 必须描述完整语义，不允许多个字段各有不同 owner。
2. **算法（how）**：依赖分析、拓扑排序、访问交集、资源生命周期、Shader Interface validation、Physical Plan compilation 是显式的算法阶段（自由函数或无状态编译函数），不在结果类中塞巨型 `static buildEverything`。
3. **所有权（who lives）**：`RenderRuntime`、`VulkanDevice`、`Buffer`、`GraphCompiledNativeBacking` 等拥有生命周期的对象使用 move-only RAII、fallible complete construction、清楚借用，绝不公开僵尸状态或 `init/shutdown`。
4. **扩展（who may add）**：内置和外部 Feature 使用同一套 public typed 作者接口。C++20 concepts 检测可选 hook，运行时只在插件/动态注册边界擦除；已绑定的热路径没有反射、名称查询或二次注册。

**不是教条主义的“所有类改 struct”**：需要稳定 invariant 的类型允许 private construction；Builder 的可变状态正确；native owner 的成员操作不可拆成没有所有权依据的自由函数。代码结构服从语义。

## 1.3 三个不许混用的正交域

| 域 | 表达什么 | 典型示例 | 谁消费 | 非法替换 |
| --- | --- | --- | --- | --- |
| RenderData | 从上层发布状态变更的**数据类型** | `MeshInstanceDelta`, `ViewCameraData` | backend typed sink | 不能表示 Graph read/write |
| SceneCapability | 某个 Scene 中持久 Provider 所提供的能力 | `MeshSceneData`, `LightSceneData` | concrete Feature 的 attach/resolution | 不可直接作为上传消息 |
| Graph Resource | 某次图或模板中的逻辑输入输出及访问版本 | `SceneDepth`, `HZBMip2`, `SceneColor` | graph compiler/executor | 不拥有 Scene/provider lifetime |

三个域不能使用公共 `DependencyId`、混合 `requires[]`、跨作用域裸 `uint32`。`SceneCapabilityIndex` 只能是非拥有 cold index；热路径使用 attach 时一次解析的稳定 typed pointer/handle。

## 1.4 保护 Legacy 行为，拒绝 Legacy 负债

**保留行为**包含：所有具体 Feature 的视觉输出和通信词汇、多 Scene/View/Target、异步资源生命周期、材质 Shader 变体、完整 Shadow/Lighting/Compute/2D/Robot 功能、插件安装/卸载、真实资源和 GPU 错误路径。`appendix/LegacyCapabilityMatrix.csv` 逐条列出 66 项原始追踪并扩展新验收列。

**不保留过时的结构**：旧 `RenderServer`/`Renderer` 并行帧循环、`RenderContext/ResourceContext` owner ladder、Feature 虚基类、string lookup per draw、`std::function` per-frame 默认、多个重复 descriptor authority、巨大 `RenderGraphCompiler` 中直通 Mesh/material domain 的实现。

**不误认“目录存在 = 功能曾可靠运行”**。例如 `render.transfer_idle` 原有遗留限制、skinning cross-frame WAR、Clang/UBSan 历史失败、原生 IME/Surface/Minimize 测试缺口继续在 `V1_KNOWN_ISSUES.md` 中保留。V2 必须重新取证；不能把 V1 未关闭项归类为回归已解决。

## 1.5 每一个 Feature 必须分类，而非强制有 Pass

- **Provider**：Camera/Light/Material/MeshSceneData，拥有持久 typed 数据和 native backing；可以没有 GPU Graph Pass。
- **Pass producer**：Forward/Deferred/Shadow/Bloom/GUI 等声明 GPU 工作；Graph 只借其必要稳定能力。
- **Compute / data transform**：Skinning/HZB/Cull/Cluster/Feedback，通常创建 transient buffers/images、依赖可包含跨帧写后读。
- **View state owner**：ViewCamera、HZB/TAA history、view-specific picking 等，state lives at actual view/feature owner，不自动建第二张图。
- **Asset/stream source**：Terrain paging、PointCloud chunks、Mesh/Texture/Material streaming，经共享 ResourceDomain/UPLOAD，而不是每 Scene 自设上传线程。

这些是行为组合，不创建五个 `I*Feature` 虚接口或 `FeatureManager` 类型。一个 concrete Feature 可以同时提供多个行为。

## 1.6 共享不等于“看起来类似”

Graph template 共享：严格以完整冷态编译键证明逻辑/Native Plan 相等。Frame 绑定、Camera、per-view History/Target 不包含在静态模板键中，但可能使 native target signature 或 pipeline variant 不兼容。不同 Shader/Pass 行为**即使逻辑读写一样也不可误复用 Native Plan**。

GPU 执行结果共享：必须看 Scene/Light/View/RenderTime、算法输入 revision、输出生命周期、条件和副作用。`Skinning` 同一骨骼姿态可共享，View GBuffer 不可因同 Scene 共享，CSM 不可因为同一 DirectionalLight 就无条件共享。Multiview 是 GPU 合并优化，不改变不同 Camera 的图像语义。

帧缓存/不执行：Simulation 没有更新并不意味着 per-frame View/RenderTime/History 不变，不能自动跳过渲染。无 PROGRAM 仍能新帧，在合法目标和 pacing 下持续绘制；没有目标时允许事件/条件休眠，不能 CPU 空转。

## 1.7 明确错误的实施方式与对应逻辑

| 错误做法 | 为什么错 | 正确职责 |
| --- | --- | --- |
| 给旧 R3 的 vector API 加一个薄 Builder 即报告 Graph 完成 | 资源绑定、Native compile、执行/同步/诊断仍空缺 | PassParams 同源贯通 C++/Shader/Graph/Native Plan |
| 每个 View 持有/编译完整 Graph | 同渲染路径复制编译和共享工作，违背 View 语义 | Backend-scope compiled plans，临时 View group |
| 将 Light Provider 变成空 `addPasses()` | 虚假统一、增加无用 thunk | concrete Feature 可只有数据提供行为 |
| Plugin 直接 include `RenderScene.hpp`/VMA private header | SDK/ABI、owner 与卸载不可控 | public restricted Feature/Pass/Resource API |
| 给 UE 画质组件命名为 Lumen/TSR/Nanite | 名称与 Pass 存在不能证明算法 | 独立技术/画质/GPU oracle 门禁 |
| 用 `if (!x) return` 规避尚未准备好的 resource | 合法状态悄悄跳帧，产生假成功 | admission/ready/retained candidate/明确失败 |
| 空头文件或兼容 Facade 让产品构建绿色 | 真实调用路径未恢复 | 真实产品/安装消费者在 cutover 恢复 |

## 1.8 “优雅”和“简洁”的可检验标准

一个抽象应至少使下述一项获得可测提升：减少调用方重复、将无效状态移到构造/编译失败、隔离唯一 authority、减少不必要生命周期耦合、从热路径移走动态工作或显著降低依赖图复杂性。类型名、注释长度、模板技巧和源码行数都不是证据。第 12 章规定 LLM 必须为每个公开类型提供类别、职责和无法简化的理由。

本设计的实现验收不仅比较生成图是否有效，还要检查**Feature 作者需要写多少资源角色/绑定/回调配置**；Tonemap、HZB、Compute Skinning 和外部插件四个代表性 Authoring fixture 必须像 Legacy 一样表达清楚，并比旧接口少重复事实。

---

# 02 — 最终模块 DAG、具体类型结构、唯一权威和生命周期

## 2.1 CMake 构建依赖（箭头为“使用/依赖”）

```text
lux-cxx + existing core::error                (external/generic foundations)
                  └── render_core            (IDs, errors, common Render values)
                       ├── render_transport  (three lanes/typed op traits/packet/reply)
                       ├── render_graph      (neutral graph authoring+logical compiler)
                       └── render_data       (feature-facing schema-only typed data)

modules/resource/description (neutral ShaderInfo/PassSchema/LayoutContract)
      ├── engine/toolchain/shader (meta + .lglsl + shaderc + SPIR-V reflection, offline)
      └── render_vulkan (ShaderInterface validation + native LayoutPlan compiler)

render_vulkan (ONE existing component, internal subdirectories):
  device/memory/descriptor/pipeline/transfer/retirement [preserve R4 RAII]
  graph/native_compile, graph/executable, graph/record
  feature/public_author_ops [narrow public API]
  backend/scene, backend/view, backend/target, backend/frame [after Graph stages]
  DEPENDS render_graph + render_core + Vulkan + VMA + neutral shader description
  MUST NOT DEPEND render_features / Scene / ECS / Editor

render_runtime → render_transport + render_vulkan + render_core
render_features → render_vulkan public author API + render_graph + render_data + render_content
scene_render → Scene/ECS + assets + render_runtime + render_transport + render_data
EngineRendering/Product → scene_render + explicit installed built-ins/plugins
```

`render_graph` 与 `render_transport` 平级、只可依赖 core；`render_data` 是 schema/typed contract target，不拥有 Scheduler/Manager；`render_content` 仅放 cooked assets/codecs，不与状态协议混放。Feature Execution API 按原约束位于 `render_vulkan/feature`，不为了第二不存在的 Backend 建 `IRenderBackend` / `render_feature` 平行组件。

## 2.2 全量最小类型清单（状态与设计职责）

### 2.2.1 Foundation 与协议

```cpp
struct RenderError;                 // existing structured error
using RenderSceneId   = StrongSceneId;    // scoped/generation-safe
using RenderViewId    = StrongViewId;
using RenderTargetId  = StrongTargetId;
using FeatureTypeId   = StrongFeatureTypeId;
using FeatureHandle   = StrongFeatureHandle;
using RenderDataTypeId = StrongDataTypeId;
using SceneCapabilityId = StrongCapabilityId;
using GraphResourceId = StrongGraphResourceId;
```

ID 是强类型、清楚局部 owner/generation，`FeatureTypeId`/canonical schema 身份在冷态注册，Runtime-local route 在成功绑定后以数字访问；两次不同 Runtime 的 local ID 数值相同不意味着同一对象。复制数值 ticket 不复制所有权。

### 2.2.2 Backend / lifetime

```text
RenderRuntime
  ├── Transport ports + ReplyArena + bounded diagnostic port
  ├── one backend execution thread/domain and stop/wake/terminal state
  └── VulkanBackend
       ├── VulkanInstance → VulkanDevice → VulkanAllocator  [R4]
       ├── SubmissionQueue + StagingArena + RetirementQueue [R4]
       ├── backend-scope Shader/Pipeline/Layout/CompiledGraph resources
       ├── SceneRenderer (single authority for RenderScene membership)
       │    └── RenderScene [N]
       │         ├── persistent SceneData providers / SceneCapability nonowning projection
       │         ├── concrete FeatureInstances, code pins and dependencies
       │         ├── View set [N]
       │         │    └── RenderView: camera/output/render path choice/per-view history
       │         └── scene-local GPU resource handles/state
       ├── FrameExecutor (logical schedule, Scene/View/Target expansion)
       ├── VulkanFrameDriver (native submit, fence, acquire, present)
       └── Target state (surface/offscreen/readback, layered composition)
```

`CompiledGraphPlan` 的 authoring/compile result 不归 View 独占。Backend-scope 的 PlanCache 可有多 key/variant；每个 View 只保存可选 `RenderPathId`（未指定继承 Scene 默认）、camera/output/history。`FrameExecutor` 的 ViewGroup/GraphInvocation 是**每帧暂态 scratch**，不是额外 manager。`SceneRenderer` 是 scene membership authority，不重复持有 CPU source ECS。

### 2.2.3 图与 Shader 数据分类

| 名称 | 类型类别 | 主要字段/操作 | Owner / Lifetime |
| --- | --- | --- | --- |
| `TextureDesc`, `BufferDesc` | value | format/dim/usage/extent/mip/layers/memory | declaration value |
| `GraphTexture`, `GraphBuffer` | strong local ID | definition-local resource reference | graph scope only |
| `SubresourceRange`, `BufferRange` | value | aspect/mip/layer or offset/length | pass declaration |
| `GraphAccess` | value | typed resource, range, stage/access/role, read/write | one pass |
| `PassDefinition` | canonical owning value | stable PassKey, execution scope, resource uses, pipeline key, conditions | Definition |
| `RenderGraphBuilder` / `PassBuilder` | mutable short-lived builder | `create/import/export`, `add*Pass`, `finish() &&` | cold composition task |
| `RenderGraphDefinition` | immutable validated value | resources, passes, deps, exports | compiled source/transaction |
| `LogicalGraphPlan` | read-only owning value | order, versioned hazards, lifetimes, culling, imports, scopes | Backend shared logical cache |
| `ShaderInterface` / `PassSchema<T>` | generated readonly schema | field roles/layout offsets/stages/name/id | Toolchain/cooked asset |
| `LayoutPlan` | cold native compile result | logical→final descriptor set/binding, complete shapes, relocation | Vulkan compiled plan |
| `ExecutableGraphPlan` | native owner | pipelines, descriptor recipes, barriers, queues, record/submit program | Backend shared native cache + retirement |
| `FrameGraphBindings` | short borrow/value | frame serial/time/slot, concrete imports, View params | one execution call, no ownership |
| `RenderResourceDomain` | real Scene-side owner | asset identity → CPU semantic retain/receipt/readiness | Runtime integration, not per Scene |
| `RenderDataWriter<T>` | typed publishing helper | validates source ownership and encode semantics | producer owner thread |
| `SceneCapabilityIndex` | nonowning cold index | unique provider for `CapabilityId` | RenderScene only |

**不为数据+算法分离再添加 `GraphDefinitionManager/GraphExecutionController/PassRegistryManager`。** 若必须持有 cache，真实 Backend compiled-plan cache 是已有 Backend authority 下的成员/私有机制，并非全局单例。

## 2.3 Graph resource vs Shader binding vs GPU backing 三层

```text
GraphTexture / GraphBuffer [Definition-local logical identity]
         ↓ logical lifetime/read-write info
PassSchema logical shader resource slots [author contract]
         ↓ LayoutPlan [final VkDescriptorSet+binding+stage/array]
Vulkan resource handle / ImageView / Buffer / VkDescriptorSet [R4/native owner]
```

- `GraphResourceId` 在相邻两个 graph 中数值相等毫无同一性。不要跨异步 Packet 运送。
- `ShaderResourceSemanticId`/generated field path 在 Shader Toolchain/布局编译冷态唯一，不等于 Runtime `RenderDataTypeId`。
- R4 的 `VkImage/VkBuffer/VMA` 只由明确 native owner 管理；Graph transient allocator 不新增第二个 `VulkanAllocator`，只创建/lease 可证明复用的物理 backing。
- `FrameGraphBindings` 借用 plan 和本帧 import backing 的合法期，不得跨 frame/async 保存裸 `span`；需要在途时由 actual frame/slot owner pin 住对应原生 backing。

## 2.4 每类资源的完整所有权和销毁链

| 资源 | 权威 owner | 借用者 | 发布/替换/销毁门槛 |
| --- | --- | --- | --- |
| World ECS component | Simulation Registry | RenderProjection 同线程同步观察 | source revision snapshot，绝不被 Backend mutate |
| Asset ID → semantic retain | shared RenderResourceDomain | 多 RenderSystem / Scene | CPU request/receipt，停止新引用；跨 Scene 不重复解析/上传 |
| Mesh/Light/Material SceneData | RenderScene provider | typed consumer Feature | Provider 先 disable capability/消费者 detach，再释放 backing |
| View camera/history | RenderView / concrete per-view Feature state | FrameExecutor/Pass | resize/cut/rebase/teardown invalidation，GPU 退休后释放 |
| Scene native GPU backing | Vulkan provider/resource owner | compiled plan/Frame | 最后 semantic retain + last GPU fence/serial |
| transient Graph allocation | Graph execution resource instance / allocator | Graph passes in current frames | computed physical interval + async overlap fences |
| Compiled logical/native plans | Backend shared plan cache/native plan owner | N Scene/View compatible invocations | cold swap after new plan complete，old code/backing pinned while in-flight |
| Feature plugin DLL | plugin code pin authority | registered ops + compiled callbacks | freeze new submissions → quiesce CPU callbacks → retire native ops/refs → unload |
| Target surface/offscreen image | Backend target native owner | FrameDriver/View outputs | acquire/present/composite and fence proven retirement |

RAII 的析构顺序通过成员顺序、ownership 和显式 stop/drain contract 保证，不以大量 `valid=false`、`closing=true` 对每个 semantic object 补偿。

## 2.5 依赖层级与头文件规则

`render_core`、`render_transport`、`render_graph` public headers 均不含 Vulkan/Scene/ECS/Editor，也不 include frozen Legacy。`render_vulkan` 的 device/allocator/memory/private graph compiler 不 include concrete `render_features`；对外 Feature author API 仅声明受限 cmd/recipe/typed handles。

Author C++20 `PassSchema<T>` 由工具链**预生成可安装 headers**；不能让外部 SDK 编译时偷偷 include `sinclude/pinclude` 或需要 Engine 源目录的 generated paths。工具链的源码语法无需暴露 Vulkan-private owner。最终产品要验证安装消费者，不得凭 bootstrap include 同步冒充可安装 ABI。

## 2.6 变更条件：何时允许新类型

每新类型必须在类型登记中记录：`category=value|algorithm|builder|owner|runtime authority|plugin ABI`、唯一责任/数据、thread affinity、ownership/borrow、public/private、cold/hot、失败语义、替代它的最小表达为何不足。更改 namespace/target/公共数据布局都需要证明下游依赖/ABI 和真实消费者；无消费者时不发明预制万能框架。详见 `appendix/TypeOwnership.csv`。

---

# 03 — Engine/Scene/Render 通信、typed 状态、资源、事务、背压与回复

## 3.1 三 Lane 是最终通信结构，不再引入第四个事件系统

| Lane | writer/reader | 承载内容 | 背压与容量 | 不能做什么 |
| --- | --- | --- | --- | --- |
| `PROGRAM` | one owner-thread producer → Backend | Scene/View/Feature typed state changes、可编译的状态 revision、业务增量 | bounded SPSC；未接纳 packet 由生产者**原样保留**，不得默默丢弃 | 不上传无界大块 Asset；不强制每 Simulation tick 一包 |
| `CONTROL` | one owner-thread producer → Backend | Scene/View/Feature create/remove/attach/reconfigure；Query、Stop 相关命令 | bounded SPSC + reply admission/consume-or-abandon | 不与 PROGRAM/UPLOAD 假定全局顺序 |
| `UPLOAD` | many allowed producer threads → Backend | chunked immutable bytes、cooked asset、GPU-ready transfer requests | bounded MPMC + byte accounting、`ExternalDataRef` owned attachment/显式 lifetime | 不自动开启后台 Writer、无界队列、隐式复制/解码 CPU |

R2-FIX 已接受 **pre-reserved reply cells** 替代 V1 response pending-retry：每个被接纳的请求在执行前预留结果槽。接收方必须消费终态结果或 `inbox.abandon(ticket)`；Packet reset/promise 取消使结果可读，但**不代替接收责任**。`WRITING` abandon 交由写者完成字节访问后回收，generation 防止陈旧 completion 污染新请求。缺失所有 ticket 而未 abandon 是接收方容量泄漏，不属于正常 backpressure。不要重写已验收的 R2 算法。

## 3.2 一份数据作者契约

示意 schema，名称以同源 codegen 实际生成结果为准：

```cpp
struct LUX_RENDER_DATA(id="lux.render.mesh.instance.v2", lane=program)
MeshInstanceDelta final {
    RenderSceneId scene;
    RenderEntityId entity;
    RMeshHandle mesh;
    RMaterialHandle material;
    TransformGpu transform;
    uint64_t source_revision;
    InstanceFlags flags;
};

struct LUX_RENDER_DATA(id="lux.render.terrain.page.v2", lane=upload)
TerrainPageUpload final {
    RenderSceneId scene;
    TerrainWireId page;
    ExternalDataRef owned_payload; // owned storage or verifiable lifetime
};
```

标记语法是**规范性的含义示例**，不允许实施 LLM 建第二个 parser；应扩展现有 `luxop/meta-generator`，并在 Phase F2 前以 golden fixtures 确定可通过 C++20 解析的正式 annotation 拼写。IDL/metadata 必须含稳定 canonical `DataTypeId`、lane、schema version、字段类型、wire ownership、typed receiver 签名与错误分类。数据型别属于业务 `render_data`，`render_core` 只放通用机制。

`PROGRAM/CONTROL` writer 热路径应为 `RenderDataWriter<T>` 或生成的 typed proxy；lane 通过编译期 traits 固定，不能每消息 name map/`switch(TypeId)` 才决定。Backend register 冷态将 canonical stable `TypeId` 绑定 Runtime-local route；执行期使用 `RouteId + payload` + 预先固定的 typed handler。

## 3.3 ECS 与非 ECS 数据源统一，但不建立“万能 Scene Sync”

`Simulation` 推进 World 状态；`RenderSystem` 是该 Scene 的唯一渲染发布者，内部拥有多个 domain-specific `RenderProjection`（Mesh/Light/Camera/Canvas/Water...）。Projection 可以从 ECS observer、Editor UI、Procedural producer 或已经完成的资产结果取得**已成立**的状态变化，不直接操作 GPU，也不能给每 Feature 新建一个 SceneSystem。

路径：

```text
Simulation Registry / Editor input / Procedural source
    ↓ typed observer / source revision
RenderProjection<Domain>
    ↓ prepare immutable candidate [may allocate cold on observed changes]
RenderSystem publication transaction
    ↓ retained owning PROGRAM packet until admitted
Backend typed handler
    ↓ persistent RenderScene state (same backend owner thread)
Feature-owned GPU state/dirty revision
    ↓ FrameExecutor GPU consumption independent of new packet
```

`RenderProjection` 的泛型部分只实现 observer/batch/revision/encode 机制；Mesh、Light、2D、Terrain 的业务 transform/asset resolution 由具体 Projection 与 typed wire owner 实现。不以反射数据库每帧扫全 ECS 检查变化。steady update complexity 近 `O(changed)`, scene size 无变化时不生成一包。

## 3.4 Publication 原子事务：不会被背压吃掉的 revision

明确保留：

- `observed_revision`：Source 已看见、应传递的修订；单调且匹配真实 source event。
- `published_revision`：已**成功 admission** 的最高修订；不等于 Backend 已消费、也不等于 GPU 已执行。
- `prepared_candidate`：由 `prepare` 构建的拥有型、不再可变的 packet；失败则 `discardPrepared()` 不清理 source dirty/departure。
- `retained_candidate`：submit 因容量失败时按原字节与身份保留的候选；背压期间**新** source changes 进入后续待处理 state，不能改写已经准备好的 packet。

推荐算法：`observe → prepare(candidate) → attempt admission → (accepted: commitPrepared/published_revision; blocked: retain unchanged; failed-before-admission: discard prepared) → observe later revisions without loss → next candidate`。失败后不得将当前 observed revision 当已 published。create→update→update→remove 同一 Entity 在断开的 Lane/资源 ready 延迟场景下，必须保证有序最终结果；根据业务可合法合并中间更新，但不可合并成没有明确的 create/remove 生命周期语义。

缺少资源时：`RenderProjection` 不阻塞 Simulation；`MeshInstance` 的引用只能在 Asset/receipt ready 后被 Backend 合法解析。未 ready 时保留待处理事实或显式降级 Placeholder（由具体 Feature 定义并可测试），不能偷偷引用 future handle。相同 stable entity id 的 destroy/recreate 需新 generation/revision 不污染旧实例。

## 3.5 跨 Lane 因果关系（不能用“队列到达顺序”）

```text
AssetRead decoded/cooked
   → owned UPLOAD request [resource_id + source revision]
   → backend upload acceptance
   → native transfer submission ticket / GPU readiness
   → resource receipt (CPU semantic + required GPU state)
   → MeshSceneData/material provider resolved
   → PROGRAM instance referencing stable generation
   → Feature pass may consume within valid GPU completion/sync
```

UPLOAD 并不自动比 PROGRAM 早处理；如果 PROGRAM 先到必须根据 handle generation、ready fence/resource state 防止未初始化访问。删资源需要先阻止新 semantic retains，保留在途 packet references、in-flight GPU references 直到 fence-proven retirement；不允许运行时全局 `waitIdle` 替代此协议。

## 3.6 RenderResourceDomain 的 scope（强制）

**默认一个 Runtime integration domain 拥有一个 `RenderResourceDomain`**，多个 Scene/RenderSystem 借用；它负责 `asset identity → handle/receipt/semantic retain`，不拥有 Vulkan native backing。真实 backing 属于 Vulkan Feature/provider/native resource owner。Scene-specific resource（instance，view、scene light）属于该 RenderScene/Feature，不能错误共享至另一 Scene。Graph transient image 是某 Plan/Frame 的生命周期事实，不是 AssetDomain 中的长期 asset。

例子：两个 Scene 引用相同 Mesh 和 Texture → 一个语义资产装载/UPLOAD/共享 GPU backing，两个独立 Scene 的 instance records、visibility、cull outputs；卸载 Scene A 不得破坏 Scene B，最后 CPU/GPU retain 才触发 backing retire。

## 3.7 具体领域的通讯与持久数据

| 来源与事件 | Lane / wire 示例 | Backend persistent owner | 消费者/作用 | 特别验收 |
| --- | --- | --- | --- | --- |
| ECS mesh transform/flags/visibility | PROGRAM `MeshInstanceDelta` / `RemoveInstance` | `MeshSceneData`/InstanceResources | GPU Cull、Forward、Deferred、Shadow | scene + entity generation、O(changed) |
| Bone palette/skin batch | PROGRAM blob/bulk + optional UPLOAD for large sets | `SkinningResources` | Scene-scope skin compute → shared vertex pool | input/output buffer WAR/fence |
| Material graph asset, texture | UPLOAD owned bytes + CONTROL create/retire | runtime-shared material/texture native owner | GraphMaterial/Shader variant | ready receipt、live preview candidate/rollback |
| Light directional/point/spot/area | PROGRAM typed Light delta | `LightSceneData` provider | Deferred/Forward/Shadow | light intensity interpolation, remove/retire |
| Camera/View create/rebind/resize | CONTROL lifecycle + PROGRAM ViewCamera state | `RenderView` + ViewCamera data provider | View GBuffer/HZB/History | independent render camera update |
| Terrain pages + parent/child hierarchy | UPLOAD/CONTROL + PROGRAM visible domain deltas | Terrain SceneData/PageCache | GPU page-select/cull/draw | fallback/page eviction/rebase |
| PointCloud chunk variants | UPLOAD chunk + CONTROL remove/clear + PROGRAM size | PointCloud SceneData | direct/indirect/LOD/splat/transient | stable chunk slot/generation & octree |
| Canvas image/tile/pixel field | PROGRAM owner instance delta; UPLOAD textures | Canvas2D arena | 2D composite/target groups | order keys, dirty-only transforms |
| Water surfaces | CONTROL creation/retire, PROGRAM transform/params | Water SceneData/provider | water composite/scene-depth | surface handle/transition |
| Trajectory points | UPLOAD or owned blob CONTROL create/append/replace | TrajectoryGlobalBuffer | line strip graph pass | atomically replace + clear/remove |
| RenderCluster/pick | UPLOAD hierarchical cluster / CONTROL request/result | RenderCluster SceneData + View query | GPU cull/LOD/pick readback | request/view generations, result correctness |
| Debug/selection/overlay | PROGRAM highlight target list/grid values | Feature/View local state | mask/blur/composite/gizmo | GUI unaffected by Scene tick |

更详尽的 `SourceFile / OperationSchema / Owner / Test` 见 CapabilityMatrix。各 Feature 可以提供多个 wire op，但通信中不存在具体 Feature 代码要求 Runtime include 其业务头。

## 3.8 Reply、Query、Readback、异步诊断

- 请求 linked completion：必须预留 bounded cell；`RenderReplyTicket<T>` 消费/abandon；从 `WRITING` 到回收不能提前复用。对应操作不会自动撤销已经接受的 GPU side effect。
- 不关联单个 request 的诊断（Validation message、GPU warning、Device Lost、resource terminal failure）走**单独的 bounded diagnostic port**，含 event epoch/count/drop accounting，而非滥用 reply cell；仅唯一 Runtime observer 读写。
- GPU readback：结果 buffer/inflight ownership 由 Backend/frame/Feature 拥有，完成后通过明确定义的 Query/receipt 交回小结果/owned attachment。RenderFrame serial 不代表 GPU complete serial。
- Stop：关闭 admission → 终止/取消未接受候选与必要回执 → 结束已有执行/回调或将失败处理为 terminal → fence/drain（正常）或 Device Lost terminal cleanup（异常） → destroy；不让未消费 ticket 永久阻止 destructor。

## 3.9 通信验收不可退让

真实 typed op 正例/错误 lane 编译负例、cross-runtime IDs、inline/deferred/abandon/stop/ticket stale、单槽 10k 次回收、4/3 UPLOAD MPMC、racing reply、同一 entity revision backlog、跨 Lane 反序、两个 Scene 共享一份 asset、GPU-ready 延迟、View camera 高速更新、无 packet 时独立 Frame progress；稳定状态发布 0 alloc，无源码外隐藏 runtime async worker。

---

# 04 — RenderGraph 作者模型、完整资源/Pass 语义与 View-independent 复用

## 4.1 Graph 是可执行工作说明，不是 Runtime Owner

作者仅声明“哪类 Pass、使用哪些资源、以什么方式访问、产生什么值/副作用”。`RenderGraphDefinition` 是 immutable/canonical owning value，`LogicalGraphPlan` 是算法输出，`ExecutableGraphPlan` 是 Native 编译后的完整实际执行规则。Graph Foundation 不能持有 `VkDevice`、`RenderScene`、ECS，也不能以 `GraphResourceId` 身份创建持久 SceneData。

**用户侧主接口必须是 Builder，不是 `vector<GraphPass>`。** 仅测试/工具可用显式 direct constructor，但必须走同一 validator；不能允许外部 Feature 绕过 typed PassParams 手工塞无约束 array/descriptor facts。

## 4.2 数据模型（C++20 语义型值）

```cpp
using PassKey = StrongPassKey;           // author/stable cold identity, NOT numeric draw id
using GraphTexture = StrongGraphTexture; // definition-local, distinct from GraphBuffer
using GraphBuffer  = StrongGraphBuffer;

enum class ResourceOrigin : uint8_t { GraphTransient, External };
enum class PersistentScope : uint8_t { None, Runtime, Scene, View };
// Origin is NOT ownership scope: an Imported resource may be Scene/View-persistent.
enum class AccessKind : uint8_t { Read, Write, ReadWrite };
enum class PassKind : uint8_t { Graphics, Compute, Transfer, HostReadback };
enum class ExecutionScope : uint8_t { Scene, View, Target };

struct TextureDesc {
    PixelFormat format;
    ExtentDesc extent;       // absolute, target-relative, dynamic extent class
    ImageDimension dimension;
    uint32_t mip_count{1}, array_layers{1}, samples{1};
    TextureUsageFlags usage;
};
struct ImageRange { AspectMask aspect; uint32_t base_mip, mip_count, base_layer, layer_count; };
struct BufferRange { uint64_t byte_offset, byte_count; };
struct ReadAccess { GraphResourceId resource; SubresourceRange range; AccessStage stage; };
struct WriteAccess { GraphResourceId resource; SubresourceRange range; AccessStage stage; };
```

示例为类型语义合同，真实 C++ 定义可与现有 `render_core` 强 ID 对齐。`std::variant<ImageRange,BufferRange>` 作为 `SubresourceRange` 的封闭和类型；Pass 内可以对同资源使用多个**不重叠**范围；重叠读写必须是明确的 ReadWrite/feedback-loop 语义，不能用同 Pass 两个独立 `read`+`write` 悄悄绕开验证。

必须支持的资源语义：

- `Transient`：Graph 内物理存储，受 compiled schedule/lifetimes 管理，允许经证明安全 alias；compile-only 无 native backing。
- `Imported`：Graph 外有权威 native owner，需声明 initial/access/layout/queue-family/ready evidence、terminal/export obligations；不能仅用非零 token 冒充生命周期证明。
- `PersistentScope::Scene/View/Runtime`：这是独立于 Origin 的真正长期资源所有权范围。通常通过 `ResourceOrigin::External` 和 `ImportContract` 加入 Graph，由 Scene/Feature/View/ResourceDomain 对应 native owner 持有；不是自动全局 HistoryManager。
- `TemporalHistory`：由 owning Feature/View 暴露 current/previous roles、validity epoch/resize/cut invalidation，Graph 显式历史读/当前写与 inter-frame sync。
- `Exported`：是**输出边界/副作用属性**，不与资源生命周期 enum 混合；同一 imported/current image 可被输出给 Target、readback、下帧 history。
- `External semantic resource`：Feature-owned buffer/image 可以参与 read/write dependency 而不让 Graph 分配；需携带真实 native barrier ownership contract。
- Buffer `offset/length`、Image `aspect/mip/layer` 是一等访问范围，完整覆盖 `0`、`REMAINING`、array/dynamic offsets 和合法对齐/设备界限。
- Sampled, Storage, Uniform, Vertex, Index, Indirect, Color, Depth/Stencil, Resolve, Transfer, Present, InputAttachment/local-read 是**逻辑使用语义**，与 `AccessKind` 共同决定 barriers/validation。

## 4.3 Authoring API（拟定的最终语义，具体拼写 F1 golden 固化）

```cpp
class RenderGraphBuilder final {
public:
    [[nodiscard]] GraphTexture texture(PassResourceName, TextureDesc);
    [[nodiscard]] GraphBuffer buffer(PassResourceName, BufferDesc);
    [[nodiscard]] GraphTexture importTexture(ResourceSemantic, TextureDesc, ImportContract);
    [[nodiscard]] GraphBuffer importBuffer(ResourceSemantic, BufferDesc, ImportContract);
    [[nodiscard]] RenderResult<void> exportTexture(GraphTexture, ExportContract) noexcept;

    template<class Params> requires GraphPassParameters<Params>
    [[nodiscard]] PassKey compute(PassName, ComputeShaderKey, Params&&);
    template<class Params> requires GraphPassParameters<Params>
    [[nodiscard]] PassKey graphics(PassName, GraphicsPipelineKey, Params&&);
    template<class Params> requires GraphPassParameters<Params>
    [[nodiscard]] PassKey transfer(PassName, TransferPassKey, Params&&);

    [[nodiscard]] RenderResult<RenderGraphDefinition> finish() && noexcept;
};
```

`PassResourceName` 为 cold authoring/diagnostics semantic ID 或唯一字符串，运行时 route 使用 numeric local ID；不能因为两个不同 Feature 同名就暗中共享资源。跨 Feature 输入要使用明确 exported semantic slot 或 typed producer dependency，required/optional 分别受约束。用于编译时的 named resolution **只发生冷态**。

```cpp
struct LUX_PASS_PARAMS() BloomExtractParams {
    LUX_RESOURCE(role=sampled_read)  SampledTexture hdr;
    LUX_RESOURCE(role=storage_write) StorageTexture bloom;
    float threshold{1.0f};
};

graph.compute("Bloom.Extract", shaders.bloom_extract,
    BloomExtractParams{.hdr=scene_hdr, .bloom=bloom_half, .threshold=1.3f});
```

该作者代码隐式生成 Graph reads/writes、Shader logical descriptor fields、stage access 和 runtime binding recipe；不得再要求作者重复 `pass.read(hdr)` 和 `.bindDS(1,...)`。由 RenderGraphBuilder 以完成的 Schema 统一捕获依赖值/资源槽，而不是保存包含借用指针的临时 Params；必要跨编译期的参数使用 owning cold schema snapshot/recipe。

## 4.4 Shader 字段之外的 Pass 资源与副作用

Push/UBO/SSBO/Sampled/Storage 由同源 PassParams 产生；深度/颜色 attachment、LOAD/CLEAR、Resolve、TransferCopy、Present、Readback、Export 等需要 typed attachment/operation contract（可使用 `LUX_RESOURCE(role=color_attachment_write)` 或明确 typed Pass attachement member）。不把它们假装全能由 SPIR-V Shader reflection 推导。

`GraphicsPassParams` 可声明 color array、depth/stencil、MSAA resolve、load/store、clear value、view mask eligibility。`ComputePassParams` 明确 dispatch bound/indirect offset、storage read/write。`TransferPassParams` 明确 src/dst ranges/size。HostReadback 必须 Graph terminal/side-effect，不能被 pass cull。

Condition/cull/side-effect：`PassCondition` 仅允许冷编译证明的可跳过行为。可选 producer 跳过后 consumer 的输入必须有显式 fallback、上帧有效 history 或同帧其他 producer，且没有无定义数据；否则 cold compile error。`has_side_effect` 不能随作者任意 flag 让非法写入被掩盖，副作用有 typed contract（Present/Readback/ExternalWrite/Feedback）。

## 4.5 Pass Scope 是语义，不是三套实现

- `Scene`：工作结果只依赖 Scene-scope 标识、相关 source revisions、resource handles、RenderTime/Frame 状态及声明输入；每个合法 invocation key 只执行一次，典型 skinning、共享 static upload、非 view-dependent Shadow。
- `View`：输出与 Camera、View visibility/extent/history/jitter/render path 相关；各 View 分别输出，也可在兼容时经 Vulkan Multiview 合并提交。
- `Target`：output/present/cross-target composite，依赖实际 target/image acquire 与 blend/load contract。

**默认安全性**：`ExecutionScope::Scene` 是作者/编译器要证明的契约，而不是“Shader 没写 Camera 就默认为 Scene”。如果 Pass 读取 View-dependent 资源、Time/History/Target、副作用或明确不同 View 内容，不能提升到 Scene。编译器可拒绝 `Scene` scope；没有证明时保持 View-level。`Multiview` 是 View pass 的一个 native 编译优化，不增加 `MultiviewFeature/MultiviewPass` 多态层。

## 4.6 多 View 不再复制编译结果

一个 RenderScene `View A,B,C` 相同 `RenderPath` 时：Graph build/compile 同拓扑应复用 1 logical/native Plan（实际 native key 含 target compatible signature）；View-specific frame bindings/history/target 仍不同。View C 不同 shader/effects 时可引用另一个 native plan；可能共享能证明相同的 logical scheduling analysis，但是不能错误复用 shader/pipeline execution recipe。

`RenderGraphDefinition` 应能同时表达 Scene-level 与 View-level pass 模板（有显式 scope），由 `FrameExecutor` 将**每帧实际需要的 View group** 展开为可执行 invocation。**不为每 View 建新的 owner graph**。每帧 expansion 仅使用预保留 scratch，不能隐式重新构建/编译图。

## 4.7 Builder、Definition、Pass 参数与缓存生命周期

Builder 是 cold 短寿命 mutable object，完整使用 `finish() &&` 转移；Definition 完成后只读、自有存储；LogicalPlan 是只读 value；NativePlan 持 native owner/code pin 与 typed recipes；执行期间借用 plan/bindings 并通过 Frame owner pin 在途 backing。失败 candidate 一律丢弃，旧计划继续合法执行；涉及 output 不兼容必须保留 old output 或明确暂停该 View 的该目标，不能借最后好图读取不兼容资源。

拓扑比较包含资源、pass、scope、effect/behavior identity、access/usage、shader interface identity、attachment signature 真实相关字段。`View` 的 Camera matrix/render time/descriptor dynamic offset/current swapchain image 不成为稳定 plan key。Native plan key 还须含 device caps/features, pipeline variant, render formats, shader hashes, LayoutPlan signature（详见第 07 章）。

## 4.8 必须覆盖的编译负例

错误资源 ID、循环、forward required producer 缺失、optional 无合法 fallback、同 pass 重叠冲突访问、未初始化 transient mip read、实际 subresource OOB、buffer range overflow、离散 read/write 漂移、两个 provider 输出同 semantic 且不显式合成、Scene pass 偷读 View-local history、异步队列/alias 不安全、multiview 不兼容不许默默启用、native Shader/Pass access 不一致、target format/sample count 不匹配。

Legacy R3 的 300 张随机图/10k 绑定复用等 oracle 必须保留或等价升级，不得删除之前的 45/59 项测试以便签名重写。算法与复杂度见第 06 章，工具链见第 05 章。

---

# 05 — Shader/PassParams 单一作者契约、自动 Descriptor Layout 和材质编译

## 5.1 唯一事实来源的具体含义

作者不写 `set=`, `binding=`，也不为同一 sampled/storage buffer/texture 再手写一份 Graph 使用列表和 `vkUpdateDescriptorSets`。**一份 C++ `LUX_PASS_PARAMS` 定义描述字段的逻辑身份、资源角色、所有权频率和访问范围；同源生成 C++ PassSchema、GLSL/.lglslh 资源声明和 binding metadata；SPIR-V reflection 用于校验实际 shader。** LayoutPlan 在冷态确定真正的物理 set/binding；native execution recipe 预绑定数值索引与资源 backing。

旧链路已存在：`LUX_PASS_PARAMS/LUX_RESOURCE`, `pass_params_hpp.template`, `pass_params_glslh.template`, `engine/toolchain/shader/lglsl/LglslEmitter.cpp`, `LayoutContract.hpp`, `ShaderInfo.hpp`, `SpirvPatcher`, `LayoutPlan`, `EngineSetShapes`。必须复用并升级既有 `lux-cxx meta generator`/Shader emitter，而不是引入第二份反射/Parser。在新设计中，**共享 Descriptor Set 的完整 shape 来自其资源 owner，不是某一 Shader 实际使用的子集**。

## 5.2 最终作者字段/类型（完整需求，而非 v0 模板）

```cpp
struct LUX_PASS_PARAMS() DeferredLightingParams {
    LUX_RESOURCE(role=sampled_read, semantic="GBuffer.Albedo")
    SampledTexture albedo;
    LUX_RESOURCE(role=sampled_read) SampledTexture normal;
    LUX_RESOURCE(role=read_only_storage, scope=scene)
    StorageBuffer<LightGpu> lights;
    LUX_RESOURCE(role=storage_write, range=all)
    StorageTexture hdr_output;
    LUX_RESOURCE(role=sampler, for=albedo)
    SamplerHandle linear_sampler;
    float exposure{1.0f};
};
```

具体宏拼写须在 F2 通过 existing generator 的 grammar/golden 固定，不准发明 C++ 不支持的成员属性。字段支持：

| 类型族 | 字段信息 | Graph access | Shader binding |
| --- | --- | --- | --- |
| `SampledTexture`, sampled image array, cubemap | format/dimension/sample/access image range, optional sampler pairing | READ aspect/mip/layer | SRV/sampled image + independent/combined sampler |
| `StorageTexture` | format/access range, read/write/readwrite, stage | READ/WRITE/UAV | image storage descriptor |
| `UniformBuffer<T>` | block size/array/alignment, update frequency | READ buffer range | UBO + dynamic offset if legal |
| `StorageBuffer<T>` | stride, count, offset range, read/write/access | buffer RAW/WAW/WAR | SSBO/storage texel etc |
| Indirect/index/vertex | typed buffer roles, offset/stride | READ with stage-specific usage | some read dependencies do not come from shader reflection |
| Color/depth/stencil/resolve attachment | typed attachment declarations, range, load/store/clear | implicit read on LOAD; write/resolve | not necessarily shader descriptor |
| Sampler | independent semantic handle, sampler flags/filter, paired resource | no Graph content dependency alone | `VkSampler` resolved by cold descriptor recipe |
| Push constants / scalars | generated strict offsets/stage/size | no graph resource access | static value per-frame or per-draw |
| Nested structs/arrays | recursive compile-time field path/count/size/stride | recursively derive fields | generated shader declarations, strict reflection |
| Optional shader resource | required or fall-back typed input with default | compile-time selection/condition proof | layout variant/known placeholder, never silent null |

不允许把 `read_write` 简化成“任意同一 subresource 的读写都合法”；feedback-loop/local read 要显式 device capability 和 Vulkan barrier/attachment semantics，必要时编译失败并提供显式两 pass fallback。

## 5.3 生成链的准确数据流（只使用一套 meta）

```text
LUX_PASS_PARAMS Author Header
   │ existing lux-cxx meta_generator, emitted source metadata
   ├─ GeneratedPassSchema<T> / C++ typed binding recipe
   ├─ generated .lglslh (resource declarations + PC/UBO/SSBO layout)
   ├─ GraphResourceUse metadata (field roles/ranges/scope/version)
   ├─ ABI/field-layout/checksum/version metadata
   └─ Diagnostic name/source location tables (cold only)
                         ▼
              .lglsl emitter / shaderc
                         ▼
                       SPIR-V
                         ▼
                SPIR-V reflection
                         ▼
           Cold contract reconciliation
                         ▼
     LayoutPlan (final set/binding/shape/owner)
                         ▼
           SPIR-V relocation/validation
                         ▼
             Vulkan PipelineLayout/PSO
                         ▼
          Fixed binding recipe per compiled pass
```

合同校验必须对齐 Resource field path/semantic name、Shader data kind/image dimension, access/stage, array count including runtime arrays, struct `offset/stride/matrix major`, descriptor flags/count, push constants sizes, shader specialization/entry point, layout required capabilities。`std::bit_cast`/C++ trivially copyable 不足以自动保证 std140/std430/ScalarBlockLayout；必须以生成静态 offset asserts + reflected SPIR-V + real shader readback 双重验证。对于未声明字段、Shader 多出资源、类型/数量不一致、版本 drift、重定位冲突/重复、大小溢出，**cold compile failure**，绝不静默部分绑定。

## 5.4 三类 Descriptor owner 与全量 shape

1. **Engine/Scene shared**：View/camera uniforms, Mesh/Instance/Light/Material/Texture bindless 等完整 Descriptor Shape 由其**真实持有者**在 Schema registry 中描述。某个 Shader 只访问其中一部分也不得为其造不兼容的 subset layout。`EngineSetShapes` 旧契约需要保留行为并删掉与 field metadata 的重复声明。
2. **Feature shared**：Shadow/Cluster/GPU Cull 等多个 pipeline 共用的一份 Feature Descriptor Shape，由 Feature provider 的公开 typed contract 定义；Scope 和卸载/替换必须明确，不能按物理 `VkDescriptorSetLayout` handle 的偶然数值猜 owner。
3. **Pass-local**：Tonemap/Bloom/SSAO 的 sampled input, storage outputs 等，来自 generated PassSchema + Shader reflection；在固定 Graph/variant 编译时完整确定、按 frame slot 有界预留 set/arena。

`LayoutPlan` 是最终位置**唯一事实**；它持有 `LogicalResourceKey→Physical{set,binding,offset,count,stages,flags,owner}`，按更新频率/所有权/设备限制规划。Stage/Descriptor Count 应统计布局合并后的最坏消耗，而不是单独 Shader 子集。移动端 `maxBoundDescriptorSets` 常见严格限制，必须查询设备的**实际值**、各类型 per-stage descriptor limits、update-after-bind limits、dynamic offsets、variable descriptor count、alignment。编译器须进行真实 budget feasibility：超出限制必须给出可读诊断或选择有证明的预定义 fallback 变体，不能无声丢掉 binding。

建议初始 logical frequency domain 为 `GLOBAL / SCENE / VIEW / MATERIAL / FEATURE / PASS_LOCAL / BINDLESS`，最终并非一域一个固定 Vulkan Set；LayoutPlan 可合并兼容域。一个 domain 不是另外一个共享 owner。**不同 Feature 私有 set 即使都曾使用 set=1 也不应相互冲突**，最终 ID 包含实际 owner scope/Shader interface，而不单靠数字。

## 5.5 SPIR-V 重新定位与一致性证明

旧 `SpirvPatcher` 已使用两 pass 扫描 `OpDecorate DescriptorSet/Binding`；这是有效的冷态算法，必须保留字级 relocation 不改变 SPIR-V 指令长度的机制。重点升级：

- 以 **pre-patch** 位置信息匹配且仅修改完整合法的 descriptor pair，避免 `set`/`binding` decorations 扫描顺序引发错误；重复 decoration、类型失配、未识别变量必须明确失败。
- 补强 `SPIR-V 1.x` 版本、decorations/grouped/alias 对于所支持 compiler 输出的约束及验证；现有算法无法覆盖的合法输入应清晰限制受支持的 cooked shader format，而不是对任意外来 SPIR-V 伪报可靠。
- 完成后重新反射或运行二进制验证，确保所有 descriptor 实际位置和 LayoutPlan 一致；记录 shader source/canonical and patched binary hashes，防止缓存 key 错配。
- 候选 Shader/PipelineLayout/DescriptorSetLayout/PSO 以**同一事务**创建；任一失败保留 last-good pipeline/graph，等待 native fence 完成再退休旧 owners。热路径没有 patch/reflect/allocate layout。

## 5.6 Material 与 Shader 变体

`ShaderAsset/ShaderInfo` 是 neutral/cooked 资源类型；`engine/toolchain/shader` 负责离线编译/反射/代码生成；`render_vulkan` 只处理经过验证且能明确 cold-fail 的装载/本地变体/PSO 编译，不可把 runtime shaderc 放进 draw。编辑器 shader/material 热更新属于冷态事务；失败继续 last-good 或向目标显示明确错误，不用临时替代 Shader 默默继续。

Material 支持现有 Graph Material 的 upload/modify/retire、Unlit/PBR/Stylized、metal-rough、alpha mask、透明、材质层/纹理绑定、Shader variants/vertex layouts，并为后续 Substrate-like 复杂 BSDF 预留**具体编译/ABI能表达的 typed material interface**。没有实际 Shader/Lighting/GPU oracle 不能标称 advanced material 已实现。

## 5.7 Push constants 和 layout

不固定把每个 Pass 的 `[0,8)` 永久当 `scene_index/view_index`，也不默认为 128B 都可随意写。完整规则：生成器根据真实 PC bytes/stage visibility/device maxPushConstantsSize 编译时决定哪些字段使用 PushConstant，哪些需要 UBO/storage 或 split；与 View/Scene 通用前缀合并必须是一份 layout 真相。Scalar struct 的 `offsetof/sizeof/alignof` 与 SPIR-V layout 必须可验证，未支持矩阵/向量布局不允许默默由 memcpy 实现。

## 5.8 C++20 工具链使用约束

C++20 没有通用标准静态反射，因此沿用已有 meta generator 并生成 `PassSchema<T>` Traits；用 `concept GraphPassParameters<T>` 在编译期拒绝未标注/字段类型不安全的 Params。生成器位于 cold/build time，插件作者只 include **安装后的 public generated headers**，不应在客户端链接引擎 private codegen runtime。保持 ExternalConsumer 的跨 DLL binary contract（具体见第 09 章）。

## 5.9 Golden fixtures 与必须证明的自动绑定能力

1. Tonemap 单 Sampled/单 ColorAttachment/一个 Scalars：不手工 set/binding，C++ ↔ GLSL/SPIR-V ↔ Descriptor/GPU readback 完整相等。
2. Highlight Blur + Composite：多 sampled fields、sampler pairing、declaration order 改变后最终 LayoutPlan 一致；test fallback/compile cache key。
3. Skinning：StorageBuffer READ_WRITE/range、one update per scene pose, cross-frame WAR 正确。
4. HZB：storage image 分 mip、sample previous mip，Shader/Graph subresource 逐个一致。
5. Clustered Lighting：多个 GPU compute 共享 Feature set；完整 shape 不能依据某单独 shader 的 reflection subset 缺字段。
6. Canvas2D/PointCloud：bindless & dynamic textures/vertex pool, GPU-driven indirect；支持真实 Shader variants。
7. 外部 Feature：安装后的独立 consumer+自己的 shader/PassParams、无私有头、ABI mismatch 被拒、卸载安全。
8. 负例：错 field role、错 std430 field offset、重复 binding、不同 Shader 同名不同类型、set budget 超限、SPIR-V remap 未覆盖、代码生成器产物失步、Runtime 错把 Schema revision 当 Shader binary revision。

**必须审查完整生成工件与哈希**；不能因为 generated `declareGraphIO` 存在就跳过真正 descriptor 写入与 GPU shader 采样。

---

# 06 — 逻辑 RenderGraph Compiler、Hazards、资源版本、裁剪和可复用计划

## 6.1 只有一个 Logical Graph Compiler

`render_graph` 不知道 Vulkan Native；输入 `RenderGraphDefinition`（同源 PassSchema）和必要的 neutral capabilities/target class；输出只读拥有型 `LogicalGraphPlan` 或明确错误。推荐**namespace-scope 编译函数**，而不是一个持有 Device/Registry/Scene 的持久 CompilerManager。

```cpp
[[nodiscard]] RenderResult<LogicalGraphPlan>
compileLogical(const RenderGraphDefinition& graph) noexcept;
```

编译成本是 cold path，允许有界/真实所需堆分配并可复用 scratch；不要求在每个 C++ template 实例中复制算法实现。需要生成 Graph diagnostics JSON/DOT、pass/resource timestamps 和可重复的 plan fingerprint，不能为了哈希速度把 canonical equality 降为可能碰撞的 hash-only 判断。

## 6.2 已冻结逻辑编译顺序 C0—C8

| 步骤 | 输入 → 结果 | 关键失败与不变量 |
| --- | --- | --- |
| C0 Canonicalize | Builder declarations → own IDs、typed resources/pass、normalized ranges | 类型/零 ID/重复 semantic/非法 dimensions/effect keys |
| C1 Resolve | required/optional provider outputs、exports/imports → concrete graph resource handles | required 缺失 fail，optional 必须有合法 fallback；禁止按安装顺序隐式选 first |
| C2 Validate Interfaces | generated PassSchema + static shader interface values → accesses/pass pipeline contract | Shader/Graph 不一致，未定义 alias/attachment role fail |
| C3 Build Versioned Hazard Index | access ranges、explicit deps、logical scope → producer/reader/resource version edges | transient 未初始化读、feedback-loop 缺声明、WAR/WAW/RAW 缺边 |
| C4 Topological Schedule | DAG → deterministic pass execution order | cycles 包含具体 pass/resource/path；固定 tie-break 不靠 pointer/hash 迭代随机性 |
| C5 Root/SideEffect Reachability | exports, present/readback/external effects → live pass set | 不删除 side effect，不因 optional chain 异常留下未定义输入 |
| C6 Lifetime/Usage | live passes/versions/ranges → logical first/last use + effective usage flags | 逻辑区间不能被当 GPU 完成证明；子资源粒度清晰 |
| C7 Scope & View Group Proof | Scene/View/Target typed inputs + retained data/time → share eligibility | Scene scope 偷读 View、Time/History/cross-scene 依赖 fail |
| C8 Emit LogicalPlan | immutable Definition snapshot、ordered nodes、hazards、culling reasons、logical range lifetimes、imports/exports | 候选失败保留 last-good，不发半张 Graph |

`VulkanGraphCompiler` 的 C9–C13 后续步骤在第 07 章，**C0–C8 不得偷偷接触 Vulkan**。

## 6.3 资源依赖与范围算法

每个资源版本的定义不仅是 `GraphResourceId`，至少有 `resource + subrange + producer/writer epoch`。Texture range 有 aspect/mip/layer、Buffer range 是 checked `[start,end)`；两个访问的 overlap 决定 RAW/WAW/WAR hazard。`READ_WRITE` 要求合法初始内容，视同读旧版本且写新版本。第一次读取 transient 未被初始化（对应 mip/层/区间）必须失败；imported 只有满足具体 initialization/ready contract 的范围才可以读。

```text
write mip[0]    : produces Version(mip0,v1)
read mip[0]     : RAW from mip0 v1
write mip[1]    : does not require WAW against mip0
readwrite mip1 : requires mip1 initialization + RAW/WAW as appropriate
```

**不能仅因为 Pass 在排序上先于另一个 Pass 就把所有资源共享写串行化。** 未重叠 subresource/Buffer ranges 可以并行；同一 Pass 内多范围也合法，只要不重叠或有合法 explicit feedback loop。若无法证明 ranges 或外部 Alias，采用 conservative whole-resource hazard，但必须记录 plan reason/性能成本，不能装作完成了子资源分析。

显式 `after/before` 是 `PassKey` 的依赖合同，不是字符串碰巧相等；多个 Feature 冷态贡献 pass 后合并，跨 Feature required semantic consumer 能用真正 produced resource 建立边，而非依赖调用注册顺序。无向约束/环必须暴露到 PassKey path 并生成诊断。

## 6.4 Versioned read 与“Latest”歧义

如果一个资源可能被多个 Pass 写入，必须区分读取哪个版本。默认的“最近前一个 writer”只在明确的 declaration/ordering semantics 下才合法；对于跨 Feature 自动排列，不能让隐式先后决定读的是 GBuffer 之前还是之后的颜色。推荐 typed `GraphValue/GraphTexture` 的写操作返回**新逻辑版本**，消费者显式接收对应 output handle；复用 import handle 表示仍可对同一 underlying resource 的后续读写，但 Compiler 会通过版本/顺序合同判断。Opaque mutable-version 本身不得成为结果缓存身份。

每个 Pass 有稳定生产者/消费者边；将 `SceneColor` 连续合成时，Feature 作者通过明确 semantic slot + producer version/final exported alias 获取正确写入链，不依赖名字“SceneColor”的注册顺序。对有意 painter-order 的 Grid/Gizmo/Transparent 提供 typed ordering constraint（不是强制为业务编码一个巨大的 RenderStage 枚举）。

## 6.5 Pass culling、条件执行与被动共享

**Culling roots**：Present/target export/readback/declared external writes/feedback、明确必须执行的副作用；所有其他不被可达根使用的 pass 可裁剪。Pass 作者不能任意标为 `side_effect=true` 绕过验证真实资源依赖。

Conditional Pass（在运行时根据 View/Feature/Debug 状态）有三种合法方案：

1. topology 不变，Pass 有可安全的 `skip predicate`，所有下游输入依赖有效 fallback；编译器固定需要的 barriers/reset invariants。
2. 可选 Feature topology 真改变，由冷态重编译并按候选事务替换；避免每帧 build/compile。
3. 用 GPU indirect/no-op dispatch 在既定 Pass 内改变工作量；不等于跳过必需的初始化/barriers。

在同一 Plan 中存在依赖被跳过的无 fallback input 时必须拒绝构建，或在已声明的 atomic conditional chain 中一起禁用。不创建通用 `ConditionChainManager`；可通过 Builder 的短生命周期 scoped `ConditionGroup` 表示原子条件。

## 6.6 逻辑生命周期、alias eligibility 与 GPU 完成是三件事

`LogicalLifetime { first_pass, last_pass }` 用于验证访问范围与产生**候选**物理重用关系，但它不是 GPU native memory aliasing 证明。Native compiler 要结合 queue schedule、跨 Queue happens-before、in-flight frame 与 target backing conditions 建立实际 overlap 后才能 alias。保守 fallback 是**不别名**，允许更高内存使用但必须正确；不能为追求峰值显存直接按 pass index 复用。

History/Persistent View/External resources 不能因为某帧 logical lifetime 结束就返还 Graph transient pool。需考虑 View resize/camera cut、跨帧 read/write、fence-proven retirement。相同图模板跨 N Views 共享 Plan 不自动共享 history/backing。

## 6.7 Graph 编译缓存语义

逻辑 key：canonical resources/pass identity/scope/explicit edges/Shader-declared access signatures/meaningful target shape、Feature topology + logical conditional chain。排除 Camera matrices/render frame serial/render time、current import handles/dynamic offsets、Simulation revisions。

Native key：逻辑 key **加** shader bytecode/interface hashes、Pipeline specialization/state、attachment formats/sample/local-read mode、Vulkan device capability variant、complete LayoutPlan signature、queue family/alias strategy等真正影响生成 native commands 的事实。**同访问结构不同 Shader 可以共享逻辑 schedule，但 Native Plan 必须独立。**

`matches()` 默认走 exact canonical equality；hash 只做 lookup acceleration，命中时仍按 collision-safe full identity；不能每帧再造候选 Definition 检查 plan 相等。Cache 应由已有 Backend lifetime owner 管理；不为一个 cache 新建第二 authority。Compilation telemetry `compile_count/cache_hit/cache_miss/invalidation_by_reason/compile_cpu/record_cpu` 必须可供工具消费。

## 6.8 必须实现的逻辑 Graph oracles

- 保留 R3 300 个 fixed-seed graph + 10k binding reuse + imported/transient/errors 等逻辑向量，新增 multi-writer version/required semantic/side-effect/root/subresource tests。
- 生成 300–1000 组合图，独立 O(P²R) CPU oracle 检查 every overlapping access hazard+deterministic order+version monotonicity；GPU native 另测。
- 2 View same camera path 共享计划、不同 camera bindings/extent compatible；2 View different shaders **不能** Native plan 复用。
- 1 Scene 3 View 的 Scene-scope Skinning 执行次数为 1，View-scope GBuffer 为 3；Scene input revision 分歧、camera change/RenderTime、Shadow projection 分歧时共享 key 正确变化。
- Required producer absent、optional fallback absent、skip-chain dangling input、pass graph cycles、array access OOB、write-only first use uninitialized、MIP WAR+RAW 编译负例均返回具名 error，不致 terminate/CPU busy spin。

---

# 07 — Vulkan Graph Native Compiler、Descriptor、Barrier、队列、内存与录制

## 7.1 R4 是 native 基础，不是“Graph 已实现”

当前 R4（I `83ffbb6d9859d87ca62a8a2acbb0083c2a8de420`, V `7de3ddaa3f7614995745b41d73dfc98302310a4c`）已验收 `VulkanInstance/VulkanDevice/VulkanAllocator`, Buffer/Image、DescriptorSetLayout/Pool、ShaderModule/PipelineLayout/ComputePipeline、bounded staging、`SubmissionQueue`、`RetirementQueue`。其验证范围是 Windows/MSVC 单 GPU/单 queue native Foundation，**不是** Graphics pipeline catalog、多 Queue native dependency plan、完整 Graphics RenderGraph/Frame/Target。后续必须扩展现有 `render_vulkan` 组件的机制，不能新建平行 Vulkan Foundation。

## 7.2 VulkanGraphCompiler C9–C13 阶段

| 阶段 | 主要输入 | 必须输出 | Fail / fallback |
| --- | --- | --- | --- |
| C9 Layout/Shader | LogicalPlan + ShaderInterface/PassSchema + device caps | `LayoutPlan`，SPIR-V relocated validated shaders，Descriptor layouts/pipeline layout | 类型/shape/limit mismatch fail；last-good |
| C10 Physical resources | logical lifetimes/format/sample/size/history/alias eligibility | PhysicalResourcePlan，per-FIF allocations、compatibility class、import/export states | memory budget fail or explicit no-alias fallback |
| C11 Native pass execution | shader variant/graphics/compute/transfer pass contract | full PSO, render state, immutable pass binding/record recipes | missing shader/PSO fail at cold compile; never lazy record |
| C12 Queue/sync | logical hazards + stage/access/queue/device caps + external sync | BarrierProgram, queue family transfers, submit waits/signals, target final layout | unsupported queue→single-queue fallback only when semantically correct |
| C13 Executable assembly | C9–C12 complete candidates | `ExecutableGraphPlan`, all native owners/code pins/import slot recipes, validation/diagnostic tables | candidate rollback, old plan stays until in-flight safe retire |

`VulkanGraphCompiler` 可以是 namespace-scope compile function + private scratch/transaction object；不把它变成持有 Scene/Feature/Device/Target 的全局 Service。`ExecutableGraphPlan` 是实际运行所需一切稳定事实，生命周期明确，不能再在 `record()` 里查 Shader 名或缓存注册。

## 7.3 同步正确性合同（Synchronization2）

每个 GraphAccess 需能产生真实的 `VkPipelineStageFlags2`, `VkAccessFlags2`, image layout、aspect/mip/layer range 或 Buffer offset/range、source/destination queue family。完整 hazard 推导还需包括 implicit reads（attachment LOAD、Depth/Stencil load/store、present read、copy/resolve）和 shader multi-stage usages。对 unavailable/unsupported usage 标明 native compile fail；不把 native validation error 当 success。

- **同 queue**：在 producer→consumer 合适边界提交 full `vkCmdPipelineBarrier2` with non-empty overlapping execution scopes；只在语义允许时才 coalesce/barrier hoist。
- **跨 queue**：判断 `same queue family / different queue family`；使用正确的 wait/signal timeline/binary semaphore 顺序、queue family ownership release/acquire（需要时）和 image layout transition。只发 `NONE/NONE` 两端普通 pipeline barriers 不能被当作已经建成依赖链。
- **跨帧**：GPU resources/inflight descriptor/indirect/staging 带唯一 native submission evidence；不能以 `frame_serial - frames_in_flight` 算“已完成”。Read-after-write/write-after-read 应使用实际已完成 fence/serial。
- **Device Lost**：精确保留 `VkResult` 并将 Session 进入 terminal failure，阻断进一步提交；不伪造已经完成的 GPU serial，也不让析构依赖无法等待的 fence 永久挂住。

## 7.4 Multi-Queue 的真实边界与保守 fallback

R4 `SubmissionQueue` 仅持一个 native queue/family。Graph native compiler 若要用 `Graphics / Compute / Transfer` 多队列，需要扩展真实的 queue owner domain：明确每个 Queue 的 serial owner、跨 Queue ticket 完成谓词、Submit dependency/semaphore、CPU external serialization、原生资源 owner。**禁止复制三个独立 `SubmissionQueue` 就宣称 async compute 完成**。

`Pass` 声明 eligible queue，而不是硬指定无条件 Async。Compiler 可基于 device caps/queue topology/实际依赖决定 async eligibility 和 schedule；正确的单 queue fallback 始终存在。异步 Queue 可以变慢，应比较 GPU timestamp/perf，但不能仅以 enum 或 submitted queue_count 证明并行。

## 7.5 Transient memory aliasing 和物理资源表

`PhysicalResourcePlan` 标记：format/extent/sample/usage/heap flags/alignment/memory type、lifetime、import flag、alias compatibility group 和 **actual GPU overlap proof**。仅 overlapping lifetime 和 queue concurrency 排除后才复用 backing；aliasing 后续 write/transition 需要正确的 memory dependencies/undefined-content 初始化策略。Debug `no_alias` 与 optional initialized-poison mode 必须能运行同一 workload 进行 GPU readback 对照。

不要为 Graph transient 再建第二个 runtime-wide VMA allocator；借 R4 `VulkanAllocator`，由现有 native graph resource owner 管理已创建的 Buffer/Image lease 以及 in-flight retirement。资源池限容量且支持压力/峰值 telemetry；`alloc failure` 保留旧 plan/new candidate error，绝不 drop frame 并声称成功。

## 7.6 Descriptor 与 Shader 完整预编译

`LayoutPlan` 负责 shared/feature/pass local descriptors 的 full shape；Native compiler 创建所有 stable layouts、pipeline layouts、shader modules、PSO/variants、fixed binding recipes，并预备每帧的有界 transient descriptor pool/set slots、dynamic offsets/arrays。Graph `record()` 只根据 prebound indexes/explicit params 获取实际已验证的 native handles；不能每帧 `getOrCreatePipeline`, `vkCreateDescriptorSetLayout`, shaderc, SPIR-V patch, global map lookup。

R4 的 `writeStorageBuffer` 只是 Foundation smoke；后续必须补真实 sampled/storage images、sampler、uniform, descriptor indexing, dynamic offsets, arrays, update-after-bind/variable descriptor count capability-gated 语义，且通过实测 Shader 输出验证，不得用一个 storage descriptor 验收全部绑定系统。

## 7.7 Graphics / Attachment / local read

需要完整的 GraphicsPipeline owner（shader stages、vertex/mesh/point source、raster/depth/stencil/blend、topology、MSAA、dynamic rendering/render pass key）、multiple color/depth/stencil attachment、per-aspect load/store、clear/resolve、final target layout/present/export。

`LOAD` 视为读取旧 Attachment 内容并进入依赖分析；`CLEAR/DONT_CARE` 对 undefined/input correctness 必须明确。Multi-view output array-layer、Depth/Stencil separate aspects、local read/subpass input、tile-based GPU 的 dynamic rendering local read 可以作为 device capability variant 选择；不支持时走保守 separate pass/output path，带图像等价验收。Shader input attachment 与 sampled image 不应在 backend 伪装成语义相同的任意访问。

## 7.8 Vulkan Multiview（可选优化，不是另一套 Feature）

兼容 View group 才允许 `VkRenderingInfo::viewMask` + layered attachment + Shader per-View index。条件：`VkPhysicalDeviceMultiviewFeatures::multiview`，支持的 view count/layer count，compatible formats/sample/extent、Shader/pipeline state、DeviceCaps、per-view transforms/viewport/scissor strategy、per-view draw visibility/indirect payload 必须合法。

当 View A/B 的可见集合不同，Compiler 不能假定 `vkCmdDraw` 共用的所有实例都恰好属于两个 View；可选 per-view visibility bit/instance data/indirect work or fallback separate draws。GPU 输出 readback 与独立双 View 的正确图像容差一致，且必须实测 GPU 负担、不要为了减少 CPU drawcall 而显著增加 overdraw。

## 7.9 Graph Executor 与 native owner

```text
FrameExecutor (Backend thread)
   → resolves Scene work and compatible View group per-frame scratch
   → fills FrameGraphBindings (per frame / view / target)
   → ExecutableGraphPlan::record (precompiled pass recipes)
   → VulkanFrameDriver acquire/submit/readback/present
   → update GPU last-use tickets → RetirementQueue.collect(completed)
```

`ExecutableGraphPlan` owns immutable native PSO/layout/barrier programs + Feature code pin; `GraphFrameState` owns per-FIF scratch/decriptor pools/command streams; physical backing owner tied to plan/scene/view as appropriate. `PassRecordContext` 是受限借用，不能直接暴露 `VulkanBackend` mutable root。第三方 Feature 使用 public typed `GraphicsPassContext`/`ComputePassContext` 等限定 GPU 指令接口，内部编译成稳定函数表/record recipe；仅极明确、经批准的 Vulkan extension tier 允许有限原生能力。

资源图拓扑/Target resize/Shader variant/Feature unload 的替换执行 `prepare new complete plan → validate → no-fail-ish publish → fence-retire old plan/resources/callback pin`；失败不破坏旧合法状态。同资源的旧 descriptor 与提交仍有效直到最后 native reference completed。不可将已退休 plan 的裸 spans 存到下一帧。

## 7.10 实际 GPU 验收

- Subresource image mip chain, Buffer overlapping/non-overlapping offsets; barrier/image range 精确；Validation **0 errors**。
- Cross-queue compute→graphics→transfer readback，故意反序/同时写、queue ownership 家族差异、单 queue fallback；读回值完整一致。
- Async alias on/off、noalias poison/guard mode、2/3 FIF、resize/rebuild/stop/Device Lost injection、GPU fence retirement 前不 destroy。
- 真实 graphics PSO + depth color/MSAA blend/stencil/resolve、multi render targets 和 present/offscreen、local read fallback。
- 真实 Shader layout placement/merged shared shapes/descriptor budget，native compilation last-good rollback。
- 1 Graph / N View Plan reuse + independent Camera outputs + Multiview readback compare，P50/P95 record CPU、GPU timestamps、allocation、compile_count。

**以 59/59 R4 CTest 为回归基线。** 对新 Graph/native 增量另加 GPU/sanitizer/prod build 正例，不能通过 fake native device 代替真实验证。

---

# 08 — 真实 RenderRuntime、Backend、Scene/View/Target 与独立 Render Progress

## 8.1 终态只有一个实际执行域

`RenderRuntime` 是有明确单一拥有者的程序端对象，负责 Transport 前后端端口、唯一后台执行线程的建/关、wake/stop/terminal error、request/diagnostic，并只拥有一个 `VulkanBackend`。`VulkanBackend` 组合 R4 device/session/queue/retirement、SceneRenderer、Graph/Shader compiled caches、FrameExecutor、FrameDriver、Target owners。`RenderRuntime` 不认识 ECS/Material/Mesh，`VulkanBackend` 不知道 Engine Scene System 的源码类型。

不要复活 V1 `GeneralRenderServer/RenderServerFacade/RendererThread` 的多层 Server/Renderer，也不额外加 Editor-specific `UIRenderServer::tick`。Editor/Player/Headless/Offscreen 使用同一 Backend execution domain，按 Target 和 Feature 组合差别运行。

## 8.2 精确拥有关系与对外 API（概念性）

```cpp
struct RenderRuntimeOptions {
    RenderTransportCapacity lanes;
    RenderDeviceRequirements gpu;
    uint32_t frames_in_flight{2};
    // Pacing policy explicit: vsync/surface/offscreen demand,
    // NEVER a mandatory Simulation packet clock.
};

class RenderRuntime final {
public:
    [[nodiscard]] static RenderResult<std::unique_ptr<RenderRuntime>>
        create(RenderRuntimeOptions) noexcept;
    ~RenderRuntime() noexcept;
    RenderRuntime(const RenderRuntime&) = delete;
    RenderRuntime& operator=(const RenderRuntime&) = delete;
    // typed ports for single producer ownership;
    // request/completion/diagnostic observation and shutdown contract.
};
```

真实接口应落在已认可的 `RenderTransport`/runtime types 上，不为示例创造重复的 `RuntimeManager`, `RenderCoordinator` 或后台 TaskScope。`RenderView` 只持 View identity/generation、camera/view data reference、target association、extent/projection/effect settings、per-view history/feature state、可选 `RenderPathId`。**没有每 View 必有 `GraphInstance` 属性**。

## 8.3 Scene Renderer 与持久数据

`SceneRenderer` 是 `RenderSceneId → RenderScene` membership 的唯一 authority。`RenderScene` 拥有 FeatureInstance membership、persistent `SceneData` provider/capabilities、View set、scene-local GPU semantic resource state/ids，**不拥有** Runtime、VulkanDevice global owner、Scene ECS、Transport queues 或独立的全局 Graph compiler。`SceneCapabilityIndex` 是非拥有 cold compose index，真实 Provider object 在 FeatureInstance/SceneData authority 下存活。

同一 Scene 可以同时有 Mesh、PointCloud、Terrain、Canvas2D、Gizmo 和 UI Pass，所有消费者使用同一有序 FrameExecutor/Graph execution，而不是各领域拥有第二套 RenderFrame。数据 Provider 一旦安装，其 SceneData 持久存储；View 只有 Camera 变化时无需重传 Mesh、重新创建 GPU buffer 或重建 Graph。

## 8.4 Frame scheduling（Simulation/Publication/Rendering 独立）

```text
Simulation 10 Hz:   S0 ---------------- S1 ---------------- S2
Publication:        P0 ---------------- P1 ---------------  P2
Render 60/144 Hz:   F0 F1 F2 F3 F4 F5 F6 F7 F8 F9 F10 ...
```

FrameExecutor 使用最近一次**完整提交**的 RenderScene state。合法 Target/ready/pacing 下，即使没有 PROGRAM 更新仍可渲染；Transport 被背压不自动暂停 Present。Renderer FrameSerial/RenderTime、Simulation revision、Publication revision、GPU completed serial 是四个独立数值，不允许假定相等。

`RenderRuntime` 的等待/唤醒必须同时考虑：Transport epoch、新的 Control/Upload/diagnostic、Target acquire/present readiness、明确离屏需求/测试帧触发、帧 deadline/vsync、GPU in-flight capacity、Stop/terminal failure、resize/wake。不能 `while(true) if(queue.empty) sleep_for(1ms)`，不能 `atomic.wait` 在没有 packet 时永久阻塞帧 deadline，也不能在不可渲染 surface 上 CPU 忙轮询。

**Camera 独立性**：Editor UI Camera 可经 PROGRAM 的 per-view data 或合法 backend-local controller 更新独立于 Simulation，仍需通过受控线程域和版本保护提交；Backend 不得每帧读取 ECS live mutable state。没有通用 TimeManager/InterpolationManager；若具体 Feature 需要 camera/transform interpolation，约束其 History/Source Time 在该 Feature/View 范围内。

## 8.5 统一 FrameExecutor 的多 Scene、多 View、多 Target 算法

1. Backend frame scheduler 获取当前所有 active target 与对应可渲染 `(Scene, View)`，检查 SceneData/Graph readiness/generation。
2. 按 Scene 与 RenderPath（native compile key）形成**临时**可兼容 View groups；同 RenderPath 的多个 Camera 不重复编译 Graph。
3. 按 `ExecutionScope::Scene` 的**真实输入相等和本次 invocation key**去重共享计算（例如 Scene GPU Skinning）。不能因同名 Pass 就合并不同姿态/时间/阴影投影。
4. View-local pass 对不同 Camera/extent/history 保持自己的输出；如果 Vulkan Multiview 设备与 Shader/visibility/target compatible 可合并 Draw，否则普通分别 Record。同 View 的多个 Target 如果真正结果可用则可复用对应 buffer/image，但必须满足格式、内容、读写/目标 lifetime 合同。
5. Target-level pass 处理 ordered composition、UI overlay、present/readback/export；所有 Acquire/Present/semaphore/fence 统一交给 VulkanFrameDriver。
6. 记录 Scene/View/Target work counters，编译计划 cache_hit/record_cpu/frame_submit、GPU timestamp和实际 in-flight 资源 use；无新 PROGRAM 也可执行新帧绑定。

`ViewGroup` 为预留 scratch value 而非长寿 owner；**唯一 FrameLoop** 在 RenderRuntime/VulkanBackend，Feature 不得拥有自己的 tick/submit loop。

## 8.6 Target 类型与合成

`RenderTarget` 可以是 Surface Swapchain、Offscreen Image Pool、Readback/Image Export。一个 Target 可以按明确 Layer 顺序组合多个 `(Scene,View)` 的输出与 UI 叠加；同一个 `(Scene,View)` 可以绑定多个 compatible target（必须区分重用结果与重新渲染）。Target 核心状态只包括必要 `extent/format/sample/generation/acquire/resize/rebuild/present`，不重新实现 separate UI-specific NativeBackends。

Resize/present-out-of-date：先 prepare new candidate、验证 format/layout/owners、在 safe transition 发布，旧 swapchain/attachment 直到最后 GPU/Present usage 完成才退休。不得在已 reset fence 的帧中间强行 swapchain recreate 导致死锁；Surface minimize 无 active target 是合法 idle，而不是 RenderRuntime fatal 或无限循环。多 Surface 同一帧可能有不同 acquire/present availability，提交记录必须对应真正 acquire 成功的 Target。

Readback 由 Target/Feature拥有 staging/readback native backing，CPU 结果通过 bounded request/receipt 返回，可与 Scene/View/Frame/Request generation 绑定避免过期 Picking/截图被误投送。

## 8.7 Scene/View/Feature 生命周期顺序

`CreateScene` 由 CONTROL request admission，Backend 完整创建 candidate Scene/providers/Feature members、返 SceneId/receipt，再允许发布 Scene state；`CreateView/Resize/Remove` 经同类 generation+receipt，不允许 Source 端提前猜 GPU View 已经存在。View remove 必须先撤消 Camera/History/Feature-local state 的未来引用，GPU-safe retire 等待最后 submission；Scene remove 先卸载/失效 dependent Views/Features/capabilities，再回收 Scene resources。

连续 create/remove/recreate 相同 numeric handle 时必须检查 generation。Backend 未消费旧 PROGRAM 时，Scene/Feature/Resource 创建/删除次序由 acceptance/receipt 因果合同负责，不能依赖跨 Lane 偶然排序。RenderSystem 发布的 retained candidate 不可被新一轮 Scene remove 清理不安全 bytes，需明确 terminal cancel/retire policy。

## 8.8 错误状态、Device Lost 与关闭

正常 Device/session：`ACTIVE → STOPPING → STOPPED`；`VK_ERROR_DEVICE_LOST`：一次 terminal failure，所有新请求返回 `kDeviceLost`/`kTransportStopping` 之类结构化错误，已接纳的 pending replies 可读或 abandon，诊断持久记录首因，FrameExecutor 不再录制/提交。没有隐式自恢复/第二 device，除非将来独立正式设计并验收。

析构必须在约定线程/domain，完成停止/wake、CPU callbacks join、GPU inflight retirement（正常）或设备损失时 safe teardown；不在普通场景资源销毁使用 `vkDeviceWaitIdle`。`RenderRuntime` 的 owner 销毁前 Scene-side ResourceDomain/RenderSystems/Plugins 按真实 pin/lease 完成关闭，不随意 `shared_ptr` 引入循环。

## 8.9 真实最小到完整的阶段验收

早期真实 GPU vertical slice：one Runtime → Backend thread → one Scene → one View → one offscreen clear color Graph → readback；随后真实 Surface/Present，24+ sequential frames 无 PROGRAM 仍正确，pacing 不忙等。之后 1 Scene/4 Views、multi-scene/two-scene-shared-asset、same path shared native plan、different paths independent, per-view history, Multiview GPU readback fallback、multi target composition、resize/minimize/device-lost/stop/recreate/error/SDK consumer。完整场景 parity 与性能在第 10/16 章。

---

# 09 — Concrete Feature、SceneCapability、不同数据域、可选 Hook 与第三方扩展

## 9.1 没有 Feature 虚基类，没有第二 Feature 框架

一个 Feature 是 concrete C++ 类型加显式 descriptor 和 compile-time `FeatureOps`，可以实现自身实际需要的方法；无方法就无 thunk，不需要 `RenderFeature`/`SceneFeature`/`ViewFeature` 继承层级。与其让所有 Feature 继承 `addPasses()`，不如根据真实职责组合：Resource/Data Provider（Camera, Light, Material, Mesh），compute producer（Skinning/HZB/Cull），graphics effect（Forward/Deferred/Shadow/Postprocess），View state owner（TAA, Camera），Asset streaming consumer（Terrain/PointCloud）。

## 9.2 一套具体接口与 Concepts（C++20）

```cpp
template<class T>
concept HasScenePasses = requires(T& t, SceneGraphContext& ctx, RenderGraphBuilder& graph) {
    { t.declareScenePasses(ctx, graph) } noexcept -> std::same_as<void>;
};

template<class T>
concept HasViewPasses = requires(T& t, const ViewGraphContext& view, RenderGraphBuilder& graph) {
    { t.declareViewPasses(view, graph) } noexcept -> std::same_as<void>;
};

template<class T>
concept HasFrameUpdate = requires(T& t, SceneFrameContext& frame) {
    { t.prepareSceneFrame(frame) } noexcept -> std::same_as<void>;
};

struct FeatureOps {
    void (*declare_scene)(void*, SceneGraphContext&, RenderGraphBuilder&) noexcept{};
    void (*declare_view)(void*, const ViewGraphContext&, RenderGraphBuilder&) noexcept{};
    void (*prepare_scene_frame)(void*, SceneFrameContext&) noexcept{};
    // only essential callback slots; no every-method default virtual thunk
};
```

已知 concrete internal Feature 可 static direct call；跨动态 Feature 注册边界才按 concept 用 `if constexpr` 生成无捕获函数表，并通过 `FeatureInstance`/registration 绑定一次。`FeatureOps` 的函数指针不能逃逸到插件 code pin 之外。可选 hooks 不生成 dummy `return success` thunk。`noexcept` 的前提是业务通过 `RenderResult`/cold configure 返回可恢复错误，不靠抛异常跨线程/插件。

`RenderFeatureContext` 只能暴露受限 typed create/submit/readback/Graph author APIs；不暴露 Backend Device owner、Renderer root、Scene native private resource tables。必要的高级 Vulkan 功能通过独立声明 capability 的受限 extension tier（单独批准），不是私下 `#include VulkanBackend.hpp`。

## 9.3 Descriptor、Capability 与 SceneData 不是同一个东西

`FeatureDescriptor` 声明稳定 FeatureTypeId、版本、实际 Schema/Data sinks、`provides/requires SceneCapability`、可选/硬依赖、scope（runtime/shared/scene/view）、合法设备功能要求。`SceneCapabilityIndex` 是 RenderScene 非拥有冷态投影：每个 Capability 默认最多一个 active authoritative provider；多个 provider 冲突返回 `AMBIGUOUS_SCENE_CAPABILITY`，不靠注册顺序选择 first。对确有多 provider 语义（如多个独立 shadow techniques）须分开 qualified capability key 或有明确合成规则。

消费者 `attach` 时一次 resolve typed pointer/handle 并固定 dependency pin；热路径不再 `find<T>()`、字符串 Hash。Provider 卸载前必须先撤销 capability 对外可见性、阻止新消费者绑定、处理依赖消费者的 detach/rewire，等最后 CPU callback/inflight/native usage 完成再释放 owner。

**例子**：`MeshSceneData` provider 持 InstanceResources；Forward/Deferred/MeshShadow/Highlight 是消费者并贡献 Pass。`LightSceneData` provider 无需 graph pass；DeferredLighting/Shadow 读取。`ViewCameraData` 提供 Camera/Jitter/History epoch；HZB/TAA/Depth 使用但不直接访问 ECS Live Registry。

## 9.4 Feature 装配事务

1. 冷态 `registerFeatureType`：验证 stable type ID/schema version、ABI、FeatureOps、code pin、provided/required capabilities、port routes；生成唯一 runtime-local descriptor/route，不做 eager shader/material GPU work。
2. `prepareAttach`：Feature 构造完整 candidate，解析 required capabilities、资源需求、Shader/pipeline/Graph declared contributions、设备要求；失败释放 candidate，不影响运行中 Scene。
3. `validateComposition`：检查 required/optional capability、View path restrictions、multiple provider conflicts、Graph producer semantics、plugin code lifetime，准备完整新的 RenderPath/Graph Plan/Descriptor candidate。
4. `publish`：在 Backend safe composition point 替换 Feature membership/compilation key；只发布完整 SceneData/Graph，old plan 留到所有 CPU/GPU references 已 safe。
5. `detach`：先 stop receiving typed updates + disable capability + revoke new paths，依赖消费者 detach，old native plan/GPU backing fence-retire，最后销毁 concrete Feature 并解除 DLL code pin。

不需要公共 `RenderFeatureManager`、`FeatureScheduler`、`FeatureStateStore` 与 `FeatureFactoryBase`；生命周期 authority 始终是 RenderScene 的 FeatureInstance set，唯一注册 catalogue 是 Runtime/Backend cold type registry。

## 9.5 数据来源与参数更新的区别

- **Feature 配置**（路径/Shader/格式/Capability 拓扑）：通过 CONTROL typed configuration transaction；可能 native cold Graph/PSO 重新编译；失败保留旧 Feature。
- **运行时数据**（光照值、Camera、颜色/曝光、Instance flags、water surface transform）：通过 PROGRAM typed RenderData 更新已有 persistent Feature/Scene/View state；不触发 Graph compile。
- **大型资产**（Texture/Mesh/Terrain/PointCloud chunks/Shader cooked material）：通过 UPLOAD + receipt + native transfer；CPU source retain 与 GPU in-flight 不同。
- **View 参数变化**（Camera/rect/jitter/output history epoch）：View 的独立 state更新，可在没有 Simulation tick 时生效；必要的 per-frame bindings 不构成新 Graph。
- **实时 Editor 操作**（Selection/Grid/Gizmo/2D overlay）：使用相同 typed transport/Feature APIs，不增加 Editor private Vulkan pass。支持 Feature params reflection+schema typed proxy，Editor 可自动构建控件但不绕开命令合同。

### 具体 Feature 状态模型示例

```text
LightFeature: UpsertLight/RemoveLight PROGRAM → LightSceneData provider → consumers Forward/Deferred/Shadow
SkinningFeature: Bone batch → SkinningResources/Scene pose epoch → Scene-scope Compute → consumed vertex-pool
HZBFeature: View depth + history validity → mip chain Compute per View → cull next frame / resize reset
Canvas2DFeature: Image/Tile/PixelField delta → per-Scene GPU resident arena → View/Target composite
PointCloud variants: chunk upload → shared octree/points; selected render algorithm → cull/indirect/splat/draw
```

每个 Feature 不必拥有自己的 Frame loop 或 GraphCache；GPU data state 由具体 provider/Feature ownership 管理，Graph 表达实际 Render work。

## 9.6 Public SDK 和外部插件的两层接口

**Authoring C++20 层**：公开版本化 FeatureDescriptor、typed RenderData schema、PassParams generated traits、GraphBuilder、Scene/View/Frame restricted contexts、cooked Shader asset interface。外部插件应能创建 graphics/compute/transfer Pass、共享 SceneCapabilities、处理自己拥有的 GPU 资源和 Query/readback，但不能包含 private Vulkan native `pinclude/sinclude`。

**跨动态库 ABI 边界**：使用显式 `extern "C"` 版本化注册入口、`struct_size/abi_version/feature_canonical_id`、稳定布局 POD/opaque handles/function tables、host allocator/error/retain/lease conventions；插件 C++20 模板仅在自己的编译单元里使用。跨插件边界默认不传 `std::vector`, C++ exceptions, owning `std::unique_ptr`/`shared_ptr` 或依赖某一 CRT 的隐含所有权（除非完全相同编译器/CRT 的受控 ABI 经单独验证）。插件 callbacks 到 Host 的同步/错误/线程规则在注册时固定，不能 runtime RTTI/ServiceLocator。

`FeatureOps` 负责受限能力请求/Pass record，binary/memory ownership 由 host 的 opaque typed IDs 管理。高级原生扩展必须遵循清楚的 capability isolation 与危害可审计 boundary，不等价于 `VkDevice*` 外泄。

## 9.7 Code pin / plugin unload 的完整证明

```text
plugin register → FeatureInstance active + code pin
   → Graph compilation stores callback/Shader/Pipeline needed pin
   → Frame/Backend recorder borrows only while pin held
   → user requests detach/unload
   → revoke new work and SceneCapability attachment
   → wait for CPU callback quiescence / stop new Graph use
   → GPU submissions complete and native plan/resource retirement
   → destroy Feature owned state and record programs
   → release last code pin
   → plugin DLL unload permitted
```

注意 GPU 不执行 CPU callback，但 native plan/compiled callbacks may hold code/owning objects until CPU retirement. 不能因为一次帧结束就卸载 callback，不能通过无限 `shared_ptr` 循环把 DLL 永久 pin，也不能让 Feature 析构访问已销毁 Device/VMA。

## 9.8 验收必须是真独立消费者

- Native library under installed prefix + independently compiled external plugin (outside repo source)；正向 create/attach/PassParams/shader load/record/readback output。
- 负例：缺 required capability、两个 competing provider、ABI/schema version mismatch、非法 unsupported GPU capability、外部 include private header、过期 handles/code pin/unload while GPU in flight。
- plugin unload/reload 不得破坏另一个仍 active Feature；跨 DLL 真实 host authority 不重复静态 registry/local TypeId。
- 输入未变时 Provider 不频繁发包/无 empty Pass；多 View/多 Target 与 built-ins 共图执行。

Legacy 外部样例只作为迁移 provenance；不能直接复制其 `void* scene + RenderFeature abstract base` 设计作为 V2 合格 SDK。

---

# 10 — Legacy 全功能等价：业务数据提供、真实算法、Feature 与通信闭环

> 本章是**不可裁剪的完整性合同**，不只是推荐功能。每条 `Fxx/Ixx` 对应 `appendix/LegacyCapabilityMatrix.csv`，以冻结 SHA `a669409a289a6fa4092f21176397795b1cdb7f3e` 下的真实源码和相应操作 Schema 为行为参照。矩阵列 `RequiredAcceptance` 是交付的独立最低行为；阶段报告不能只写“已支持 Mesh/Shadow”。

## 10.1 Mesh/Instance/Material 的真实数据链

**Source**：World/ECS Mesh component、Transform、Material assignments、visibility/instance flags、optional Editor selection/streaming data。**CPU ownership**：`RenderProjection<Mesh...>` + Runtime integration-scope `RenderResourceDomain` asset retain/receipt。**Wire**：PROGRAM upsert/remove/retire/batch state；UPLOAD decoded/cooked Mesh/Texture/Material chunks；CONTROL destroy/query/feature attach。**Backend**：persistent MeshSceneData/InstanceResources + runtime-shared GPU mesh/vertex/texture/material owners；native resources ready/last-use fence。

**Graph**：Frame/Scene-scope GPU skinning (pose-dependent), cull candidate/compact/indirect visible list（View-dependent as needed）, Forward/Deferred/MeshShadow graphics, correct depth/prepass/alpha mask/opaque/translucent ordering；same View mixed Mesh+PointCloud+Terrain/Gizmo；shared input Asset does not duplicate uploads across 2 Scenes。

**必须恢复细节**：Stable RenderObject/Entity generations、instance create/update/update/delete、per-View visible mask、instance draw flags/cast/receive shadow、fade/retire transition、MDC grouping、vertex-source pool and indirect args、node-graph materials、PBR/Stylized/Unlit、normal/clearcoat/sheen/emission/alpha cutout and material specialization variants、shader live preview candidate rollback。现有 `ForwardMeshFeature`、`DeferredGBufferFeature`、`StandardMeshStackFeature`、`StandardMaterialFeature` 等不是一个“Mesh Feature”能替代的算法集合。

## 10.2 光照、阴影、Clustered 与 GBuffer

`LightSceneData` provider 拥有 directional/point/spot/area light SoA、create/update/delete/intensity transition；Forward/DeferredLighting/Shadow/MeshShadow 作为消费者 attach resolve。Deferred Lighting 要真实 GBuffer MRT/depth producer，Clustered pipeline 包含 build/count/scan/fill/draw，资源上限/overflows/光簇和 depth 判定需 GPU 验证。

ShadowMap 保留 PCF/EVSM、方向光 CSM、atlas slice/page 分配与 light technique switch；MeshShadow 保留 GPU-driven caster cull/compact/alpha-material handling，Shadow resources 同时供 Forward 与 DeferredLighting 读取。EVSM atlas/filter 的 code path 必须真实存在。Camera 不同的 CSM 不能无条件只做一份；Point/Spot 可按 shared light projection/cache key 复用。

`DepthPrepass → GBuffer/Forward → Lighting → LinearDepth/SSAO/Fog/Water → Tonemap → Highlight/Grid/Gizmo/UI` 仅为示例拓扑；实际颜色/深度/attachment LOAD/clear/store、HDR→LDR/sRGB 和 cross-pass dependency 通过 Graph compiler 决定，不能依靠注册顺序凑出“看起来能画”的画面。

## 10.3 Geometry GPU compute：Skinning/HZB/Cull/RenderCluster

**Skinning**：Bone palette owned blob/batch 输入，correct layout/stride/vertex pool IDs、GPU compute output pool、material/mesh draw 共用 producer output；cross-frame WAR 旧限制必须新测试覆盖。相同 Scene 姿态/输入 epoch 的两个 View 不重复 pre-skin，但 View-specific visibility/camera cull 仍需独立。

**HZB**：从 depth 构建 mip chain，每 mip subresource RAW/WAW/barrier，history current/previous valid epoch 由 View/Feature owner 持有，resize/camera cut/rebase/target change invalidates；cull consumer GPU indirect / occlusion re-projection 不得读取 stale mip。

**SpatialCullGrid**：world/page origin、distance cells, optional coarse cull mask, scene large-world rebasing；它可以是只有 SceneData/cull mask provider 的 Feature，不强迫添加 Graph Pass。

**RenderCluster**：树形 cluster parent/children、GPU cull/LOD expansion/hierarchical fallback、GPU-visible count、Picking ID target/readback/request generation/stale View recreation rejection。picking 不能因为读回时 View 被销毁而把旧 ID 回给新 View。

## 10.4 PointCloud 不得缩成一个 Feature

| 原策略 | 输入与计算 | 必须保留的独立结果 |
| --- | --- | --- |
| Simple | chunk/PointCloudPoint16 → GPU resident points → direct draw | 颜色/点大小/增长/清理/稳定窗口 |
| GPUDriven | octree point chunks + compute frustum cull/visible compact → indirect draw | GPU cull accuracy/indirect counts/latency |
| LOD | HZB/Camera/point-world-size/Octree → screen/world size scheduling | depth-scale/LOD stable transitions |
| Splatting | Gaussian **soft-splat point cloud** with blending/point splat frag | alpha/depth correct、GPU-driven batching；**非完整 3D Gaussian Splatting** |
| Transient | owner-frame point data, per-frame ring/generation | 下一帧清理，无 stale memory, no unbounded realloc |

资源基础还包括 PointCloudGlobalBuffer、GpuOctreeNodeBuffer、PointCloud chunk create/replace/remove/clearAll/query/pointSize、跨 Scene 共享可证明的 asset、per-view visibility/LOD states。五条算法必须分别有 shader/GPU readback/image/performance fixture，不允许用一个 PointListDraw 替代。**未来 3D Gaussian Splatting / 3DGS 是另一套高阶表示算法**：需要带协方差/旋转/颜色与不透明度的 splat 参数、view projection、screen-space tile binning/sorting、alpha transmittance compositing、GPU 数值稳定性与深度混合；它应作为同一个 Scene/View/Target/Graph 内可组合的 Feature，而不是新增 `GaussianSplatRenderer`/第二 Backend。旧 F33 soft-splat 不能被当成完整 3DGS。见第 11 章 H06。

## 10.5 Terrain/Water/Large World

Terrain `UploadTerrainPagePayload` 包含 page ID、parent/children、quadtree/page min-max data；真实 GPU page selection/cull/indirect/draw、page fallback、HLOD/transition；页式缓存有 logical capacity、fallback capacity、eviction、resident/ready stats，不得因 UPLOAD 背压丢 parent fallback。Terrain 受 Camera/View/Scene origin high precision binding 影响，`rebaseSceneOrigin` 不得重传全静态 Mesh；GPU bounded expansion budget 实测。

Water `SurfaceCreate/Update/Destroy/Stats` 是 SurfaceSceneData provider，支持 scene transform、material/depth/fog/local reflection 依赖、实际 Graph composite、过期 handle 清理与 transition。水面如果只是屏幕空间覆盖层不能冒称高级流体/反射水体；这些属于第 11 章新增算法目标。

Large-world CPU 坐标可以使用 page+local 或双精度，投影到 GPU 必须有**唯一明确的精度表示**（origin page、high/low split/view-relative local 均可选并经数值误差 fixture 证明）；所有需要绝对位置的 Feature（Shadow/RenderCluster/Terrain/Water/Trajectory/PointCloud）都必须响应 rebase，不得静默继续用 stale position。

## 10.6 Canvas2D/UI/Editor

Canvas2D 含 GPU-resident Image2D、PixelField、TileMap、ordered priority/instanced batches、premultiplied alpha、offscreen subgroups 和 group composite；`TransformBatch` 只发 dirty，`Canvas2DEnabled` retained bit 控制合法绘制。2D Camera 和 UI 像素坐标不强制伪装为 3D Scene projection。混合 2D/3D/Editor 应通过同一 Target chain/FrameExecutor，不能单独 UIRenderServer 的 FrameLoop。

Grid2D/Grid3D（under/over painter order）、Gizmo transient Lines/Triangles、Highlight selection mask/cull/blur/composite、StreamingFeedback 面片与可选 shader pattern、Skybox cubemap/equirect、RenderCluster Picking、Scene Target UI overlay 等保留各自 Effect 参数和正确深度/颜色关系。`Transient` 意味受限跨帧资源，不能直接复用永久 Asset GPU pool。Editor camera 更新不依赖 Simulation tick。

## 10.7 Postprocess 完整接口与图输出

Tonemap (exposure/operators/ACES fitted、HDR→sRGB correct), LinearDepth (nonlinear→linear), SSAO (depth sampling/AO history/fallback), Fog (depth+scene color), Highlight Gaussian Blur/composite、Water composite、StreamingFeedback mask/composite 都必须通过 typed PassParams+Shader binding+Graph native execution 实际接通。`Fog` 和 `Water` 的 required/optional dependency 必须有显式 fallback；不能因为缺一层上游 pass 就不报错地采样旧资源。

Layered target/present、multi offscreen/readback、swapchain image import/final layout、MSAA/depth/resolve、multiple overlays、UI/Editor handoff 需要真实图像和 resize/minimize/fence 证明，不能只做 CPU Graph compilation。

## 10.8 插件、Toolchain、Core/Transport 的历史行为完整性

- `LUX_OP`, `LUX_COMM_CONFIG`, stable registration/proxy/typed op codegen；`PROGRAM/CONTROL/UPLOAD` 成熟 queue/packet/blob/borrow/retention 算法。
- `LUX_PASS_PARAMS`, `LUX_RESOURCE`, `.lglsl` emitter、`ShaderInfo`、Shader reflection、complete EngineShared/LayoutPlan、SPIR-V Descriptor relocation、vertex layout/variants/pipelines。
- `RenderRuntime` stop/wake、reply/diagnostic/resource status、Scene feature registration；一套 FrameDriver/fences/semaphores、surface/offscreen/presentation/teardown。
- Engine Scene RenderSystem 单入口、asset dedupe、SceneReceipt/ViewReceipt、View camera extraction、World-specific Projection、UI data sources 和 external feature plugin installed SDK example。

都要在新的完整测试链复现。R0 719 冻结文件、R1–R4原验收事实不得因设计文档“新增功能”被重新定义为已实现。

## 10.9 每个业务能力的规范交付记录

实施日志每 `Fxx` 必须维护：`SOURCE_SHA/source_path + historical operation contract + author-facing API + source input + typed wire + backend persistent owner + Graph/Shader/PSO chain + state/error/stop/retirement + positive/negative GPU oracle + same-workload performance + exact implementation SHA`。一行未绑定实际证据则仍是 `NOT_MIGRATED/PARTIAL`，不能标记 PASS。

示例：对 `F33 PointCloud Splatting` 的通过标准是**旧 soft-splat 语义等价 + 真 GPU raster/alpha output**；“给未来 3DGS 留 Pass 接口”不得替代该行。对 `F08 Shadow PCF/EVSM/CSM`，允许分子门禁并最终汇总，但只移植 PCF 不能把整行关闭。

## 10.10 跟踪矩阵与来源证明

主表 `appendix/LegacyCapabilityMatrix.csv` 保留原 F01–F36、I01–I30 编号，并添加 `EvidenceRequired`, `NewDesignChapter`, `AcceptanceTag`, `ActualImplementationSHA`, `QualificationSHA`, `QualifiedStatus` 字段；初始新 Graph/业务未实现状态均为 `NOT_MIGRATED`。接受功能时实施方必须追加准确测试/证据哈希，不可改原 SourceFile/SourceSHA 指向另一处代码。另设新增 UE 高级能力的 `Hxx` 行，不与 Legacy parity 结果混合。

---

# 11 — UE 级画质的真实算法、基础设施依赖、GPU 预算和降级合同

## 11.1 “对齐 UE”不是复制 UE 实现或使用 UE 名称

目标是对齐**同级的视觉问题覆盖、资源规模与画质指标**，不声明与 Epic 源码或专有技术等同。本章的 Nanite-like/Lumen-like/TSR-like/VSM-like 都是新算法里程碑，拥有独立成功条件；Graph 能 express clustered passes、ray queries、history 或 virtual pages 并不代表算法完成。

参考官方资料（实施时核对 API/UE 版本）：
- https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine
- https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine
- https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-global-illumination-and-reflections-in-unreal-engine
- https://dev.epicgames.com/documentation/en-us/unreal-engine/temporal-super-resolution-in-unreal-engine
- https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine
- https://dev.epicgames.com/documentation/en-us/unreal-engine/substrate-materials-in-unreal-engine

## 11.2 先形成完整现代 raster base（Legacy parity 的质量上限不能降低）

基础完整路径应包括线性/HDR correct color pipeline、PBR GGX/Smith/metal rough、normal mapping、alpha test/translucency、multi lights、Forward/Deferred/HDR GBuffer、depth prepass、clustered lighting、Cascaded/PCF/EVSM shadows、SSAO、Fog/Water、Tonemap/exposure、Postprocessing、MSAA/TAA foundations、Camera jitter/motion velocity/depth、UI layered output。不同 View 的输出可分别 Forward/Deferred，仍共享 SceneData 和可共享 Graph Plan/RenderPass 编译。

对色彩空间/roughness/normal orientation、shadow peter panning、distance attenuation, cluster overflow、HDR tone map/sRGB、透明混合、MSAA resolve 等要有固定 reference scenes 与 image difference/SSIM/perceptual comparisons，不允许仅“没有 validation errors”就宣称视觉等价。

## 11.3 Virtual Geometry（Nanite-like）

**必需真正算法**：mesh cluster/meshlet 或 page tree hierarchy、per-cluster bounds/error metric、屏幕空间误差选择、GPU instance/cluster traversal、view-dependent frustum/occlusion、indirect dispatch/draw、page residency/streaming requests 和 parent/low-detail fallback、movement/alpha/shadow/terrain integration。对大场景要求稳定 GPU memory budget 和异步 chunk upload；动态/变形 Mesh 需要明确不兼容时的传统 path fallback。

Graph/Feature 依赖：Mesh SceneData, sparse/virtual pool, HZB history, queue-safe compute+draw, streaming feedback, multi-view per-camera visibility, frame-clock independent. 核心类型可由 `GeometryPageId`, `ClusterDesc`, `VisibleClusterRange`, `PageResidencyRecord` 等 plain values 表达，native backing 是资源 owner；不能以“通用 GeometryManager”取代明确 page-cache authority。

验收：高 polygon reference assets 多距离/镜头运动、LOD transition/popping、防闪烁/occlusion error、驻留峰值、page overflow/fallback、GPU timestamps/throughput，对比传统 Mesh 路线（图像容差、显存、CPU/GPU 耗时）。**仅 GPU-driven MDC/RenderCluster 不是 Virtual Geometry 的全部**。

## 11.4 Dynamic GI/Reflections（Lumen-like）

**必需选择一个明确、真实的场景表示与追踪流程**：可选 screen-space hit、software SDF/proxy surface cache、ray tracing capable device path 或组合，但至少要能进行 indirect irradiance cache、probe/radiance accumulation、temporal/spatial denoise、反射追踪/roughness filtering、动态 light/object changes invalidation 和 fallback。不要用 `RenderGraph.addPass("Lumen")` 假实现。

Core Graph 需求：screen-space HDR/Depth/Normal/Motion, SceneRepresentation provider, persistent history/radiance cache, subresource compute storage, async queue eligible scheduling, half/quarter-resolution buffers, complete shader binding shapes, cross View shared static geometry/probe fields vs view-dependent traces. RT 硬件不可用时软件路径必须真实输出或明确禁用高级选项并使用质量已定义的 SSAO/SSR/IBL fallback。

验收：室内/室外开门/关灯/动态遮挡、镜面/粗糙反射、镜头运动、Scene rebasing、history flush 与 ghosting、不同 View 相同全局 cache/不同 temporal history、GPU memory/time budget，使用固定 HDR reference 和视觉误差/噪声指标。

## 11.5 Temporal Anti-Aliasing / Upscaling（TSR-like）

**必需真实算法**：subpixel jitter, motion-vector generation（物体+Camera），Depth/Reactive/Transparency masks, history reprojection & validation、disocclusion, neighborhood clamp/rejection、color-space exposure handling、jitter phase、dynamic output/input resolution、sharpening 与 temporal stability。对 View local history 按 Camera cut/resize/projection change/rebase/Feature graph replacement 单独 invalidate；无统一全局 HistoryManager。

必须防止只是 temporal average 的 ghosting/双影/烂边，提供 fast motion、subpixel patterns、thin geometry、foliage、transparent particles、disocclusion 和 static scene 样例，度量 temporal flicker、detail stability、PSNR/SSIM/LPIPS（视参考条件）。Hardware caps 不足时使用 native resolution/TAA/FXAA 等有定义 fallback。

## 11.6 Virtual Shadow Maps（VSM-like）

必需 shader/lifetime 算法：虚拟 shadow address translation、page table/physical atlas page allocator、clipmap or directional/spot/point allocation policy、GPU feedback map、page request/physical cache residency/priority/eviction、light/caster changes dirtiness/invalidation、coarse missing-page fallback、shadow filtering。跨 View 共享由实际 light projection/cascade/clipmap key 决定，不可简单将同光源的 CSM 视为可共享。

验收 camera/light motion、近距高频阴影、world origin rebase、动态对象、page pool exhaustion、故意乱序 Streaming requests、GPU time/page hit/cache churn/quality side-by-side 对比 PCF/EVSM。旧 PCF/EVSM/CSM 仍作为 fallback，不因 VSM 新路径删掉其 CapabilityMatrix 行。

## 11.7 高级材质、后处理与阴影质量

`Substrate-like` 方向采用 typed layered BSDF closure/graph material compiler、clearcoat/sheen/transmission、foliage/subsurface、specular AA/energy conservation、pipeline permutation budget 与一致的 Shader LayoutContract。应支持多 Shader stage/variant、材质编译缓存、编辑器热更新事务而不是每材质一套手工 Renderer。Volume Fog、Atmosphere、Bloom、Depth of Field、Motion Blur、SSR/SSGI、Contact Shadows、Shadow denoise 等真实算法各有 PassParams/History/synchronization 需求与独立效果验收。**增加 Feature 名称不等于效果实现**。

## 11.8 GPU Pipeline 设备能力与画质档位（一个 Backend，多 RenderPath）

建议三组明确的能力档位（不是新 Runtime）：

- **Compat**：单 graphics/compute queue、传统 forward/deferred、PCF/EVSM、保守 transient/no alias、无硬件 RT，支持所有旧业务 Feature。
- **Standard**：Clustered, HZB, GPU-driven, MSAA/TAA, SSR/SSGI or alternate GI、可验证异步 compute/alias/Multiview（设备支持时）。
- **High**：Virtual Geometry + Advanced GI/reflections + Temporal Upscaling + Virtual Shadow Map（每项独立 runtime capability/quality budget）。

档位不是“简单把某些 Shader 关掉”；不同 View 可以选择不同 RenderPath/quality profile，编译出兼容 native plan 并保留 shared SceneData。所有降级必须显式指定画质/正确性，附真实 GPU 输出和 Perf baseline。

## 11.9 3D Gaussian Splatting 等可扩展表示（H06）

3DGS 是与传统 PointCloud Simple/LOD/soft-splat **不同的表现与渲染算法**。完整 3DGS-like Feature 至少包含带空间高斯协方差/scale/rotation/SH 颜色和透明度的 owned typed data/UPLOAD、每 View 的投影与可见性、GPU tile binning + sorting 或其他经证明正确的顺序无关替代、front-to-back alpha transmittance/blending、与 Mesh 深度/颜色/Target 的明确融合、streaming/chunk updates，以及与多个 View/History/输出路径的资源与同步合同。

H06 的目标是使 3DGS/未来 4DGS-like dynamic representation 可以作为**普通公开 Feature/SceneData**接入同一个 Runtime/Backend/RenderGraph，同屏混合 Mesh、Terrain、PointCloud 与 Gizmo。对于动态高斯/4DGS 要再定义独立的时间和形变模型，不因 H06 完成便冒称实现所有 4DGS 算法。GPU sort/composite 的正确性需与小规模 CPU 参考做像素/alpha oracle，实测多 Camera、深度合成、容量、吞吐、逐帧更新及长寿数据；不允许新建独立 GaussianRenderer/frame loop。

## 11.10 “高级效果系统真的支持了吗”的判定表

| Feature | Minimum implementation proof | 不可接受的冒充 |
| --- | --- | --- |
| Nanite-like | hierarchical virtual geometry GPU traversal + streaming fallback + visual/perf reference | 只使用 indirect draw/RenderCluster |
| Lumen-like | scene-space GI/reflections with actual multi-bounce/temporal/surface cache variant proof | 只有 SSAO/SSR/GI Pass 空名字 |
| TSR-like | verified motion vector + history rejection/reconstruction at fractional render scale | `history = blend(prev,current)` |
| VSM-like | real page table, feedback, page residency and invalidation with shadow output | 旧 Shadow Atlas 重新命名 |
| Advanced BSDF | compiled layered shader closure with material reference | 多加一个 Clearcoat bool |
| 3DGS-like | GPU splat projection/tile sort/transmittance with Mesh depth and pixel reference | 旧 soft-splat 点渲染重命名 |

## 11.11 实际画质验证矩阵与可比性

每种 GPU 算法要保存：固定 reference assets/scenes/cameras/seeds、目标分辨率与输入 render scale、format/tonemap/exposure、设备驱动/API/capability、Shader hash/feature settings，至少 warm frame stable、camera motion、object/light motion、resize/rebase/device loss、resource pressure 几组。记录图像比较（像素/深度/运动/HDR 数值）、flicker/ghosting、GPU p50/p95/max、CPU record、VRAM peak/page cache churn；不能通过改变质量配置偷取性能优势。

**先进算法实施顺序受数据/Graph/Shader/Native SDK 基础门禁约束**。Legacy parity 必须独立达标，不能以新技术的计划代替旧功能交付。

---

# 12 — LuxEngine Render V2：C++20 架构哲学与代码风格硬性合同

> **状态：FINAL CONTRACT。** 2026-10-10。与本压缩包第 00–19 章同级生效；生产实现按第 15/17 章分阶段授权、验证与 STOP。
>
> **约束来源：** 冻结 Render Legacy 的 `RGBuilder → RGGraphDescription → RenderGraphCompiler → RGCompiledGraph → RGVulkanRecorder` 职责链、`LUX_PASS_PARAMS`/shader emitter/`LayoutPlan`、当前 R1–R4 基础，以及后续确认的 **View 不默认拥有 Graph；同 RenderPath 的 View 共享计划；Scene-global GPU 工作按可证依赖只执行一次**。
>
> 本文的示例说明目标语义，不表示截至 R4 当前生产源码已经具有这些 API。**不得以 C++20 语法先进为由牺牲行为完整性、异常安全、ABI、线程与资源生命周期正确性。**

## 1. 核心理念：职责先于类型、数据先于机制

**总体优先顺序：**

1. 用语义和真实数据流决定模块边界，而不是按旧类名重命名或新建 Manager。
2. 用 **plain value / 强类型 ID** 描述“是什么”，用 **独立函数和明确阶段** 描述“如何计算”。
3. 只有真实 ownership/lifetime/invariant 的地方才需要 owning `class`，并采用 move-only RAII。
4. 临时声明态采用 Builder；最终图定义采用 validated owning value；编译产物采用只读完整值或显式 native owner。
5. 跨模块/插件采用 typed API + 显式 cold registration；仅不可避免的运行时边界 type-erase，一次装配、多次无查找调用。
6. **允许重写已通过 R3 的公开 Graph API。** 不能以维持原 `CompiledGraphPlan::compile()` 签名为目标而牺牲最终接口。
7. 代码行数最少不等于设计最简。减少概念、重复真相和运行时条件分支，比减少类型数量更重要。

## 2. 类型分类（强制）

| 类别 | 默认 C++20 表达 | 不变量/所有者 | 典型例子 | 禁止模式 |
|---|---|---|---|---|
| 纯数据/参数 | `struct` aggregate，成员具名初始化，必要时 `std::variant` | 由外部 owner 或值本身 | `TextureDesc`, `BufferRange`, `PassAccess`, `GraphFrameValues` | 无意义 getter/setter/PImpl、`Context` 嵌套转发 |
| 强身份 | `cxx::StrongId` 或显式 strongly typed handle | Owner scope 明确 | `GraphTexture`, `GraphPassId`, `FeatureTypeId` | 原始 uint32 混用、为 ID 建全局 runtime registry |
| 只读视图 | `std::span<const T>`, `std::string_view` | 明确 borrowed，不可悬空 | Graph compiler input, shader field metadata | 逃逸到 async job、长寿 plan、wire packet |
| 算法/转换 | namespace-scope pure/free functions | 无自身 owner | `compileLogical`, `analyzeHazards`, `makeLayoutPlan` | 没有状态却新增 CompilerManager/Service |
| Cold authoring | 可变短生命周期 `GraphBuilder` / scoped PassBuilder | 只拥有草稿 | `addPass`, `createTexture`, `finish() &&` | 让 Builder 自己保留 Vulkan 设备或活过 execution |
| Native owner | `class final`，move-only，factory `expected`，RAII 析构 | 唯一真实 native owner | `VulkanDevice`, `Buffer`, `PipelineLayout` | public `init/shutdown`, `initialized_`, 同时存在两个 owning containers |
| Runtime authority | composition-only `class final`，明确 thread/lifetime | 唯一 Scene/Frame authority | `RenderRuntime`, `VulkanBackend`, `RenderScene` | `I*` base、Manager/Context 叠层、并行可写真相 |
| Plugin execution | concrete Feature + generated static ops table | FeatureInstance/code pin | `MyFeature`, `FeatureOps` | public virtual Feature base，依赖内部 `RenderScene`/VMA |

### 2.1 数据与算法分离不等于所有成员函数都要禁止

以下成员函数仍合适：
- `std::span`/getter：对状态的只读观察；
- `GraphBuilder::addPass()/finish() &&`：短期一致性构建操作；
- RAII owner::`create()/native()/destructor`：所有权不变量；
- `RenderRuntime::submit()/pump()`：真正 authority 的操作。

但一个没有内部生命周期与可变事务状态的逻辑编译器，不默认新建带成员状态的 `LogicalGraphCompiler` 对象。

```cpp
struct TextureDesc {
    TextureFormat format;
    TextureExtent extent;
    std::uint32_t mip_levels{1};
    std::uint32_t array_layers{1};
};

[[nodiscard]] RenderResult<LogicalGraphPlan>
compileLogical(const RenderGraphDefinition& definition) noexcept;
```

**不采用**“所有算法塞进所操作的数据类型”：

```cpp
class CompiledGraphPlan {
public:
    static RenderResult<CompiledGraphPlan>
    compile(const RenderGraphDefinition&) noexcept;
    // 计划和生产计划的算法被合并在同一语义类型中
};
```

上例并非不合法 C++，但新设计默认采用独立 `compileLogical(...)`，除非有明确更强理由。

## 3. C++20 功能使用表（强制审查）

| 机制 | 推荐场景 | 明确禁忌 |
|---|---|---|
| `concept` / `requires` | PassParams、Feature 可选 hook、Shader schema、typed callback 入参的编译期契约 | 一个 concept 不承载可测试语义；多层元编程替代简单函数 |
| `if constexpr` | `FeatureOps` 的可选 thunk 生成、已知字段的静态分派 | 在每个 Pass/Draw 做 runtime kind switch 后假装“模板化” |
| `constexpr` / `consteval` / `constinit` | frozen schema、stable descriptor、编译期检查和静态表 | 把运行时图全放进编译期、造成过量模板实例与编译时间 |
| `std::span` / `std::string_view` | 同步 scoped borrow、固定容量批量读 | packet/task/plan 隐藏借用，跨 stop/unload 悬空 |
| `std::array` / `std::to_array` | 静态 metadata、固定小集合 | 为动态数量写巨大 `std::array<max>` |
| `std::variant` | 真正封闭的 sum type，如 `std::variant<BufferDesc, TextureDesc>` | 资源状态改成数十种 bool/枚举排列、每 draw 反复访问多层 variant |
| `[[nodiscard]]` / `[[no_unique_address]]` | 必须处理的结果、经过布局验收的空策略成员 | 装饰性使用代替错误处理/字节 ABI 审查 |
| designated initializers | 平面参数与配置值 | 公开 aggregate 随意 ABI 变更、位置依赖重构 |
| `std::ranges` | cold 侧 schema、构建、验证、静态变换 | 热路径层层 views/filter 捕获、隐式分配或复杂调试 |
| `std::atomic::wait/notify` | 有明确 epoch/predicate 的通知机制 | 没有帧 deadline 的无限阻塞、空自旋 |
| `std::jthread` / `stop_token` | 真实 owner 需要受控关闭线程时 | 自行引入框架内未请求的后台 worker、模糊 join 顺序 |
| `std::bit_cast` | 已验证的 trivially-copyable wire/标量转换 | 绕过 Endianness、版本及 Shader ABI 检查 |
| C++20 coroutines | 有实际异步语义、取消和 lifetime 证明时 | Graph 每帧调度/Upload 管线无需求却全改 coroutine |
| `std::pmr` | profile 证明 cold allocator/growth 真的需要统一 arena 时 | 为零分配门禁新造 GenericAllocator 层 |

**标准底线：只允许 C++20。** `std::expected`, `std::move_only_function`, `std::mdspan`, `std::ranges::to`, explicit object parameter/deducing-this 等属于 C++23 或以后；必须继续使用已经接受的 `cxx::expected`，不得暗中提高语言标准或再造 duplicate polyfill。

## 4. 静态 Feature 接口：Concepts + Generated Ops，而不是继承和虚函数

真实 Feature 是普通的 concrete type，可选择实现需要的方法；没有方法就不产生对应 thunk，不需要返回成功的空实现。

```cpp
struct SceneFrameContext;
struct FeatureOps {
    void (*prepare_scene_frame)(void*, SceneFrameContext&) noexcept{};
};

template<class T>
concept PreparesSceneFrame = requires(T& feature, SceneFrameContext& frame) {
    { feature.prepareSceneFrame(frame) } noexcept -> std::same_as<void>;
};

template<class T>
[[nodiscard]] constexpr FeatureOps makeFeatureOps() noexcept {
    FeatureOps ops{};
    if constexpr (PreparesSceneFrame<T>) {
        ops.prepare_scene_frame =
            +[](void* object, SceneFrameContext& frame) noexcept {
                static_cast<T*>(object)->prepareSceneFrame(frame);
            };
    }
    return ops;
}
```

规则：
- `std::same_as`、`noexcept` 约束应具体表达作者承诺；不满足编译失败，不引入 dummy `RenderFeature` 基类。
- `FeatureOps` 是低频、固定结构、预绑定函数表；注册时完成 `FeatureTypeId → local RouteId`、`SceneCapability` 等绑定，帧热路径使用直达 thunk/typed pointer，不用 `dynamic_cast`/string map/ServiceLocator。
- 插件代码/函数指针的实际 lifetime pin 必须独立证明，静态函数表不是 code pin。
- Feature 类型不必为所有 hook 实例化通用无内容函数。
- Concepts 不承诺自动优化所有间接调用；静态调用适用于已知 concrete 类型，外部插件不可避免的 erasure 发生在边界且只调用一次/Pass 或更粗粒度。

## 5. 一份 PassParams：一套 C++/Shader/Graph 真相

复用并扩展现有 `LUX_PASS_PARAMS` / `LUX_RESOURCE`、`lux-cxx meta generator` 与 `.lglsl` 工具链。只允许一种作者级资源声明。

示意：

```cpp
struct LUX_PASS_PARAMS() BloomExtract {
    LUX_RESOURCE(role=sampled_read)  SampledTexture hdr;
    LUX_RESOURCE(role=storage_write) StorageTexture bloom;
    float threshold{1.0f};
};
```

编译期生成 `PassSchema<BloomExtract>` 或等价的 `GeneratedPassSchema<BloomExtract>`，使用 C++20 concept 限定 Graph Builder 入参。生成的同源输出应包含：

- Typed C++ 参数访问和字段描述；完整字段路径、类型与访问角色；
- Graph read/write + subresource ranges + side effects；
- GLSL/HLSL 或现有 `.lglsl` resource declarations；
- Shader reflection/Codegen 逐字段 reconciliation；
- Descriptor layout plan 的 logical resource identity、update frequency、owner scope；
- PushConstant/UBO/SSBO CPU↔Shader layout checks；
- cold validation/errors 和无需 dynamic string lookup 的 runtime binding recipe。

仅凭 C++20 本身尚无可用的标准通用静态反射，因此依赖既有生成器是明确设计选择，不伪造 runtime reflection。

**Shader 资源的最终 set/binding 由 LayoutPlan 决定；Feature 作者不能为了绑定同一资源再次写 `pass.read()`、手工传裸 set/binding 或重复 `vkUpdateDescriptorSets()`。** 只有非 Shader 资源依赖（attachment, copy, present, external side effects 等）采用 typed 显式声明。

## 6. RenderGraph：Value + Pure Compilation + Native Plan

```cpp
class RenderGraphBuilder final {
public:
    [[nodiscard]] GraphTexture createTexture(TextureDesc description);

    template<class P>
        requires GraphPassParameters<P>
    [[nodiscard]] GraphPassId addComputePass(
        PassKey name, ComputeShaderKey shader, const P& parameters);

    [[nodiscard]] RenderResult<RenderGraphDefinition> finish() && noexcept;
};

// GraphDefinition is a validated owning value.
[[nodiscard]] RenderResult<LogicalGraphPlan>
compileLogical(const RenderGraphDefinition& definition) noexcept;

// Native compile consumes backend/device/shader capabilities; preserves last-good
// candidate and owns/pins native pipeline, callback and resource recipes.
[[nodiscard]] RenderResult<ExecutableGraphPlan>
compileVulkan(const LogicalGraphPlan&, const VulkanCompileInputs&) noexcept;
```

`GraphPassParameters<P>` 必须由 generated schema/traits 确立，不是 `std::is_trivially_copyable_v<P>` 之类不足以描述 Graph/Shader 语义的弱约束。

算法源码建议按真实编译阶段拆为独立函数/文件：
- normalize & validate declarations
- resolve resource/producers/optional fallbacks
- access/subresource hazard analysis
- graph dependency/culling/lifetime
- shader contract/layout validation
- queue schedule/barriers/physical lifetime and aliasing
- pipeline materialization and executable recipe

不根据阶段数量建立同等数量的 `*Manager` 或公开纯虚接口。**编译中间态可临时拥有 scratch，不能成为长期运行的第二份 Graph authority。**

编译缓存身份分为 logical topology key 和 native execution key；native key 必须含 shader/pass behavior、device caps、target/attachment signature 及真正影响编译的事实。不能使用 R3 `plan.matches()` 作为所有 native PSO/recorder 等价性证明。

## 7. View 不默认拥有 Graph：复用语义与代码组织

按本最终规范已经冻结：

```text
RenderScene owns SceneData and Feature membership
RenderView owns Camera, output, per-view history and optional RenderPath choice
VulkanBackend owns compiled logical/native graph plan caches
FrameExecutor constructs ephemeral compatible View groups and frame bindings
```

- 两个 View 如果仅 Camera 不同，默认共享同一 Graph Definition/CompiledPlan；绝不按 View 数重新编译相同 topology。
- Scene-global computation（例如同一姿态的 skinning、上传）根据实际数据依赖只执行一次；View-local GBuffer/HZB/TAA 等各自输出。
- Compatible multiview draw 只是 view-local 的一种可选优化，不强迫不同相机共享不可共享的像素输出；无 Multiview 时正确执行 fallback。
- `ViewGroup` 尽可能是 FrameExecutor 的临时 value/scratch，不自动成为一个 Manager、长期 Owner 或第二帧循环。
- 多个不同 View 可以选择不同 RenderPath，代码使用同一 Graph/Feature authoring 能力，不复制第二套渲染器。

## 8. Hot Path、借用与 ABI

- **热路径零分配口径：** warm fixed topology/capacity/resource backing，first-party C++ heap allocation per steady frame = 0。无 new/delete、无意外 `vector` growth、无字符串/RTTI动态调度、无 lazy shader/pipeline/layout 创建。
- `std::span`/`std::string_view` 仅做短期 borrow。异步 Transport packet、resource upload、callback capture 或跨 plugin unload 需要拥有数据/attachment pin，不能保存来自临时 vector 的 span。
- `std::function` 可以用于冷配置，但不得作为 per-Pass/per-Draw 稳态回调的默认形式；使用 prebound function pointer + typed state 或 generated pass operations，并实测。
- `std::shared_ptr` 仅用于真实共享生命周期、代码 pin、跨线程被动所有权。资源的 single authority 不以 `shared_ptr` 降级成模糊多人所有。
- `std::vector` 在冷态 owning output 中完全正常；稳定帧预留容量，并保留 capacity 后 clear/reuse。SoA/AoS 用真实访问模式选择。
- 裸 Vulkan handle 不跨 public Feature API 持有；native resource 使用 R4 RAII owner 与 fence-proven retirement。
- 插件跨 DLL 使用版本化 C entry / function table（参数布局、struct size、ABI version、allocator/ownership、exception prohibition 显式）；C++ templates/concepts 用于插件**内部**编译期作者体验。不得在不受控跨 ABI 边界交换 `std::vector`/C++ exception。
- C++20 `std::atomic::wait` 必须搭配合法的 frame deadline/pacing，不能让渲染在无 PROGRAM 时永久阻塞。

## 9. 禁止的 LLM 代码外观（自动化审查重点）

**以下变更一旦出现，必须给出针对具体消费者/数据的证明，否则阶段 FAIL：**

1. 无独立生命周期的 `*Manager`, `*Controller`, `*Coordinator`, `*Context`, `*Service`, `IRender*`。
2. `class` 仅包装三个原始数组和十几个 getter，外加将算法塞进数据产物的 `static compile(...)`，却无语义封装收益。
3. 用继承/虚方法表示 Feature 的可选 hook，或者空 `override` 以满足基类。
4. 为一条代码路径同时维护 Graph resource roles 与手工 Shader Descriptor bindings 两份真相。
5. `void*` 公开作者 API、`std::any`、`std::type_index` 或仅靠字符串解析 runtime data route。
6. 通过 `std::function`、`shared_ptr`、`unordered_map` 在每 frame/entity/draw 通用分发，未有 profile 或必要生命周期。
7. 大量 `if (!x) return` 把构造不变量失败变成“成功但黑屏/缺画面”。
8. 用 native Vulkan 所有权类作为通用 Graph 逻辑描述；Graph Foundation public header include Vulkan。
9. 单纯给每个 View 增加 GraphInstance 或每帧重新构建计划；重复 Scene-global Work。
10. 用 C++23 库/语法假冒 C++20，可编过本机不代表满足 SDK 合同。
11. 为减少代码行数省略 Shader metadata、subresource/barriers、GPU retirement、Feature unload、实际负例测试。
12. 通过多个公共别名/Adapter 兼容刚刚重写的 R3 API，形成未来永久并行接口。

## 10. 代码交付审查清单（门禁）

每一个新类型必须在设计评审中回答：**它表达的语义？唯一 owner？持久 state？public/private？热路径成本？为什么不用 value/free function？** 无明确答案不得合入。

每一个新模板/concept 必须证明：它**在编译期禁止哪种无效代码**、对应哪项必需的 semantic contract、负例测试怎样触发、编译大小与增量编译成本如何控制。

每一个生成型 PassParams 必须证明 `C++ author → generated C++/shader → SPIR-V reflection → LayoutPlan → executable recipe → GPU readback` 同源且一致；错误型别、数量、范围、布局必须在 cold 编译期间明确失败。

每一个新 Feature 必须证明未定义的 hook 不生成 thunk；插件 public SDK independent consumer 构建，且卸载时 code pin、回调与在途 GPU 的 lifetime 可证明。

每一个新 Graph/Runtime 必须证明同 Scene 两 View 相同 RenderPath 共享图计划，Scene-global 工作不重复；不同路径的 View 可以合法拥有不同 native plan；无新的 PROGRAM 仍可连续渲染。

## 11. 实施方必须提交的证据

1. Type Inventory：所有新增 public/private 类型、职责、owner、可替代的 simpler design。
2. Algorithm Inventory：纯逻辑算法/函数、输入/输出、错误类型、原 Legacy provenance。
3. 真实 positive/negative C++20 compile tests：Concepts、strong IDs、borrow temporary、Feature optional hook、public include closure。
4. CMake real DAG + compile_commands、installed SDK 头编译、MSVC/Clang/GCC 支持状态与 NOT_RUN。
5. Shader codegen 的一份作者声明与 C++/GLSL/SPIR-V/Descriptor layout golden outputs。
6. 相同功能的 before/after：CPU p50/p95/max、alloc_count/bytes、binary/template bloat（有可比样本时）。
7. Native GPU validation 与真实算法输出/资源生命周期 tests；不得以 compile-only 模拟 GPU correctness。
8. 生产路径 grep/AST/clang-tidy 审核清单：但不得只依据关键词 grep 宣称 ABI/ownership/线程正确。
9. 设计外新增类型/类/公共接口的单独审批记录；不能“为了进度”偷带。
10. 独立 qualification I/V commit、证据索引、PASS/PARTIAL/FAIL、STOP。

## 12. 对当前已定稿的变更要点

- **02 类型所有权**：移除 `RenderView` 必有 `ViewGraphInstance`；将共享 logical/native plans 放 Backend scope；View 保留可选 RenderPath 与独立 FrameBindings/history。
- **04 Graph**：加入 typed PassParams + Concepts/生成 schema；Builder/Definition/Compiler/Plan 的数据/算法/owner 分界；Scene/View/Target execution scope、合法共享和 multiview fallback。
- **05 Shader**：分清 generated schema、shader reflection、LayoutPlan、native descriptor owner；C++20 编译期约束替代运行时字符串分发。
- **09 Feature**：明确具体 Feature type，`requires` 检测可选 hook，生成非虚 `FeatureOps` + code pin，公共作者 API 不触碰 backend private。
- **16 验收**：加入 code-style/semantics negative compile tests、真实 plugin independent build 和架构静态检查；**不能以功能测试通过豁免设计硬门禁**。
- **19 决策**：C++20 Contract 为 D27；View 默认不拥有 Graph 为 D02；历史规范逐章映射见第 18 章。
- **最高优先级第 17 章 LLM 实施合同**：本最终文档按 F0 doc-only 集成提交；不 retro-edit R0–R4 验收事实。

> **最终裁决标准：** 不以类数量、模板数量或源代码行数作为优雅程度指标。以“是否存在唯一事实、职责是否不可再合并、作者接口是否简洁、无效使用能否在编译期失败、热路径成本是否可测、插件/Resource lifetime 是否完整”为验收依据。


## 13. 本章的实现风格最终裁决

- `GraphBuilder` 是**带暂态一致性约束**的 Builder class；`GraphDefinition` 是不可随意修改的拥有型 value；`compileLogical` 是无状态算法；`ExecutableGraphPlan` 是持 native/code pin 的 RAII owner。遵循语义分工，不只按 `struct/class` 标签分。
- 内置 Feature 与外部插件复用相同 C++20 authoring/schema；external ABI 的跨 DLL erasure 要明确版本与函数指针 lifetime。
- view/render path 编译缓存归 Backend；`RenderView` 不默认独立持有 Graph，`ViewGroup` 为 FrameExecutor 预留的每帧 scratch。
- 对热路径“零分配”的测量口径与 cold/growth 例外见第 13 章；不能通过提前无界分配、隐藏 driver allocations 或禁用真实功能来获得零。
- 每条禁令都需要自动化编译负例、源码/编译依赖审查或真实 GPU oracle，不能仅在报告中写遵守。

---

# 13 — 错误语义、线程、RAII、CPU/GPU 生命周期与可测性能门禁

## 13.1 完整对象与预期失败

拥有 native backing / Runtime lifetime 的类型只允许 `expected<T,Error> create(...)` 或其它等价**完整构造工厂**，成功后立即可按 invariant 使用；没有分步 `init()`/`shutdown()`、`initialized_`、`setDevice()` 或对象半活状态。构造 candidate 部分失败由局部 RAII 自动 unwind；公开资源状态只有合理的 ready/pending/retired 业务含义，不能额外发明一组语义 zombie flags 补偿 ownership 不清。

| 失败类别 | 发生时机 | 规则 |
| --- | --- | --- |
| Invalid author contract / schema mismatch | Cold parse/compile/Feature attach | `RenderResult` 精确 source/path/field/PassKey，拒绝 candidate，保留旧正确状态 |
| Native Vulkan create/out-of-memory | Device/Graph/Resource cold build | 返回完整 `VkResult` + context，自动 rollback；不 publish 半成品 |
| Backpressure/no capacity | PROGRAM/CONTROL/UPLOAD admission | 明确容量/可重试状态；保留原 owning candidate/byte charge，不丢 revision |
| Resource not ready/handle stale | Consumption boundary | typed generation/receipt/membership error，合法延迟直到 ready，不用 silent default |
| Logical invariant broken after validation | Production hot path | fatal contract or structured terminal failure，不能 `if(!x) return success` 继续黑屏 |
| Device Lost | Any GPU owner | single terminal session failure；不伪造 completed serial，不暗中重启第二 device |
| Plugin ABI/callback code stale | Install/unload | reject/stop; code pin covers last invocation and native retirement |
| No active Target/minimized | Frame scheduler | 合法 idle with wake/pacing，不能 error 或 busy loop |

错误回执、Log/diagnostics 是不同 API：`RenderResult<T>` 给具体请求；unsolicited diagnostic bounded port 给 Runtime/Validation/Drop 报告。不要新建第二套 ErrorRegistry，也不要以 `printf`/`assert` 代替 release build error contract。

## 13.2 CPU 数据生命周期与 C++20 borrow

`std::span`、`std::string_view`、raw pointer、GraphFrameBindings 的 borrow 均必须明确生产者生存期，只在同步调用范围内有效；不得把返回 `span` 的临时 vector、栈上 Buffer、插件可卸载对象保存在 queued Packets、async callbacks、native Graph caches 里。跨线程交接采用拥有型 Packet/BlobRef/ExternalDataRef 及显式 pinned attachment；通过 R2 typed route/packet ownership，而不是新建 `shared_ptr` 消息对象每次拷贝。

`std::shared_ptr` 仅在确有跨线程 passive owner/code pin/lifetime 时使用；`std::unique_ptr`/move-only RAII 是默认 owner；Graph CPU compile output 可用 owning `vector`/`variant` 值。`std::function` 可用于冷态工具或配置，不是每 Pass/Entity/Draw 的通用 dispatch 机制。 `if constexpr`/generated ops cold bind，Frame hot direct recipes。

## 13.3 单一线程与多队列不变量

- PROGRAM/CONTROL 仅 owner-thread producer SPSC → Backend consumer。其它线程必须先进入已有 owner domain，不因为一处 Editor call 增加隐藏 MPMC 或 mutex。
- UPLOAD 是 bounded MPMC，worker 线程只由真实外部调用方拥有；Render/Transfer 不擅自建立异步 writer 队列。
- Backend Scene/Frame/native mutable state = Backend execution domain owner；Feature callback 本身不能在未知线程 mutate Registry/View/Device。
- Native Vulkan queue submit 需由一个明确 Queue-domain owner 串行化，跨 Queue 的同步用实际 semaphores/fences/ownership barriers 证明，不靠 C++ `std::mutex` 抽象掉 GPU 次序。
- `std::atomic::wait/notify` 需要和实际 frame/present deadline 结合，Stop/terminal/lost wakeups 明确 predicate；没有 PROGRAM 时不能无限等 packet 而使离屏渲染停帧。

## 13.4 三种互不相等的完成进度

`request accepted` ≠ `backend consumed` ≠ `GPU submitted` ≠ `GPU completed` ≠ `present complete`。RenderRuntime/RetirementQueue/Texture/Scene/View 需通过对应合法 receipt/ticket/serial 计算 retire eligibility。`FrameSerial` 即使未提交 GPU 工作也可能继续前进；不能凭 `frame_id >= retire_frame + FIF` 销毁 GPU 资源。

资源替换：`prepare candidate → validate → atomically publish semantic owner state → stop old new references → GPU last_use proof → retire native backing`。成功发布后 old handle 可能仍被其它 old program frame/descriptor/consumer borrow 引用，因此必需 in-flight pin。`Device Lost` 不能把未完成的 fence 当已完成，走针对 terminal native teardown 的单独故障路径。

## 13.5 三类资源生命周期必须分开

1. **CPU Asset/semantic**：Shared ResourceDomain/Scene source retain/receipt，last CPU retain 可触发逻辑退休请求，但不等于 device allocation 立即 free。
2. **Backend persistent SceneData/native owner**：Feature/RenderScene/Runtime 共享 GPU backing，从 upload ready 到最後 semantic use；GPU submission/in-flight frame pin 有自己的 serial。
3. **Graph/Frame transient**：只有在 native schedule+queue overlap/aliased memory proof 之内复用；History 持久状态不属于 transient；Target swapchain/acquire/present 拥有另一组 native 信号生命周期。

## 13.6 稳定热路径与基准口径

**HOT**：warm、固定规模 Scene/View/Feature/Graph、所有 pipelines/layouts/PSO 已建立、buffers/scratch/capacities 足够、不发生新的 asset/topology/shader/resize。目标 `CPU first-party C++ heap allocations/frame = 0`、不重复 Graph compile、无冷态 descriptor layout/pipeline/shader创建；preallocated `FrameGraphBindings`, per-FIF native scratch 与 batch storage。

**COLD/GROWTH**：new Asset, Shader hotreload, Feature attach/Scene/View topology changes, Device recreation, swapchain resize, capacity growth、首次 compile/PSO、subresource/alias plan rebuild、fault path。这些允许可解释且有 budget 的分配，必须分别报告；不能通过将 warm path heap 提前无限分配改变吞吐/内存真实性。

Perf 基础指标：同机/同源/同工作量 `p50/p95/max`, `alloc_count/bytes`, `FrameCPU Record`, `Graph compile_count/hit/miss`, `native pipeline creation count`, `GPU frame timestamp`, `queue waits`, `VRAM peak`, `upload bytes/s`, `stop/wake counts`, `driver validation errors`, `present latency/jitter`。分辨率、Camera、draw counts、shader/PBR quality 和 culling strategy 必须对齐，否则不比较百分比。**对可比路径未解释的 >5% p50/p95 回退为 STOP**；不是对完全不同的工作负载机械套基线。

验证图像/计算结果时不可因优化删掉实际 Shader work 或降低 Shadow/Texture/LOD fidelity 以换取基准通过。记录 GPU driver/OS 内部 allocation 与 first-party C++ heap instrumented 不是同一口径。

## 13.7 C++ 错误纪律与 ABI

C++20-only；无需新 exception framework；通过 `cxx::expected` 传错误，`noexcept` 的接口若遇 OOM 需遵循仓库全局 fatal policy，不在局部 catch bad_alloc 后伪造 `transport busy`。Generated PassParams `std::is_trivially_copyable` 等静态检查只是一部分，PushConstant/SSBO ABI 还需 SPIR-V/GLSL 实际 offset验证。

跨 DLL ABI 用 C/versioned tables/opaque IDs、函数和对象的唯一 allocation/free owner；不从一个 CRT 创建另一个 CRT 释放。Native resources 不公开给基础 Feature 作者。外部插件的真实卸载要覆盖最后 CPU callback/native Graph plan/cooked Shader/code pin 后才准 unload。

## 13.8 必需的故障注入和 Sanitizer 计划

在验证环境可行时跑 MSVC-ASan、Clang/GCC ASan/UBSan（没有则 `NOT_RUN`, 不许 Windows 测试冒充 Linux/Android），并覆盖：Vulkan create failure at every owner stage、Native Descriptor/Pipeline partial candidate rollback、Upload byte exhaustion、R2 reply `WRITING` abandon/stop race、graph cycle/scope error、alias forbidden / disabled mode、2–3 FIF cross-frame WAR、View/Scene/Target resize/delete while in-flight、plugin code pin/unload、Device Lost injection。Real GPU ValidationLayers/SyncValidation 要实际启用并记录 0 errors；测试钩子不可进入生产公共库或 benchmark。

---

# 14 — 七条 end-to-end API/GPU 验收场景（示意代码→实际职责→失败/输出）

> **接口示例是设计目标语义，不等于已存在于当前 R4 源码的可直接编译 API。** 各阶段须将示例变成真实独立 source fixture、使用生成接口并作为 GPU/SDK acceptance oracle。不可仅提交同名空函数。

## E01 — Tonemap：一份 PassParams 驱动 Graph 与 Shader

```cpp
struct LUX_PASS_PARAMS() TonemapParams {
    LUX_RESOURCE(role=sampled_read) SampledTexture hdr;
    LUX_RESOURCE(role=sampler, for=hdr) SamplerHandle sampler;
    LUX_RESOURCE(role=color_attachment_write) GraphTexture ldr_target;
    float exposure{1.0f};
};

graph.graphics("Tonemap", shader.tonemap,
               TonemapParams{.hdr=hdr_color, .sampler=linear,
                             .ldr_target=target, .exposure=view.exposure});
```

**编译链**：meta→generated C++/GLSL + C++/Shader layout assert→SPIR-V reflection→LayoutPlan set/binding→Graph sampled read/HDR and color attachment write→PSO/barrier/Descriptor recipe→GPU tonemap→HDR/SDR reference readback。

验收：作者不写裸 `VkDescriptorSet`、`set/binding`、`pass.read`；Exposure 每帧变化不 compile；Shader 输出/颜色空间与 reference 逐值接近，缺 HDR producer/format mismatch/field rename fail cold。

## E02 — Mesh/Light：ECS source 到 GPU Forward/Deferred/Shadow

```text
World ECS Mesh/Light/Transform
→ MeshProjection + LightProjection observe dirty source revision
→ Asset Domain dedupe Mesh/Material/Textures; UPLOAD owned immutable bytes
→ ready receipt → PROGRAM upsert instance/light
→ RenderScene persistent MeshSceneData/LightSceneData
→ scene pose epoch Skinning maybe once
→ View Group A/B: cull/Depth/GBuffer or Forward per path
→ Shared LightShadow if compatible light projection; clustered light → Tonemap
→ one Target or multiple composited outputs
```

验收：两个 Scenes 共用 asset GPU backing、每 Scene 自有 instance/light state；一个 Scene 两个不同 View 的 camera matrix、visibility GBuffer 不同但没有两份资源上传；改一盏灯不重建 graph；删 Scene A 后 GPU fence complete 不破坏 Scene B。故意 PROGRAM 比 UPLOAD 早到不能渲染未 ready resource。

## E03 — 同 Scene 四 View：同 RenderPath 计划共享和实际共享 GPU work

```text
Scene S: Skinning producer + Scene lights + MeshSceneData
 View A: Camera A, Deferred, 1920×1080, HDR + Tonemap
 View B: Camera B, Deferred, 1920×1080, HDR + Tonemap
 View C: Camera C, Deferred, 1920×1080, HZB history C
 View D: Camera D, Forward, 1280×720, HDR + Tonemap
```

预期：A/B/C 有相容的 Graph/PSO key时 compiled-plan count 不随 3 个 Camera 增长，D 使用独立 native plan；Scene-scope Skinning 同 pose epoch 执行一次；GBuffer/Depth/HZB/History per Camera 不共享结果。开启 compatible Vulkan Multiview 时 A/B 的 graphics 可合并，GPU 输出与 separate fallback 等价；不支持或可见集合不同不许强制合并。删除 B 不使 A/C 重新编译或释放它们的 history/backing。

## E04 — HZB mip + async Compute + GPU cull

```cpp
// logical image with 5 mips, input depth from this View
for (uint32_t mip = 0; mip < 5; ++mip) {
    graph.compute("HZB.Mip", hzb_downsample,
        HzbMipParams{.src=(mip==0 ? depth : hzb.mip(mip-1)),
                     .dst=hzb.mip(mip)});
}
// GPU cull reads current/previous valid history per actual View contract.
```

验收：GPU readback verifies each mip resolution/value、No RAW gap、不重叠 mip 无假 WAW、queue-eligible async compute 有 native wait/signal/ownership barrier；单 queue fallback 结果相同。View resize/camera cut 无陈旧 history、mip invalidation 正确，故意缺 producer/读未初始化 mip cold fail。

## E05 — Terrain + RenderCluster + 五点云策略 + Streaming Feedback

Scene receive bounded UPLOAD pages/chunks with per-owner generations + ready receipts；Terrain PageCache parent fallback、LOD/error selection；RenderCluster producer GPU cull/LOD/pick；PointCloud Simple/GPUDriven/LOD/Splatting/Transient 各自切换 RenderPath/Feature variant；UI display picking feedback。多 View 可共享 Mesh/PointCloud buffer/octree, 但各自的 Camera/visibility/cull output 不强制共享。Texture/Terrain page cache 容量压力不会错误抛弃后续 revisions。

验收：空中/地面 Camera移动、大世界 rebase、Parent fallback、Chunks remove-reuse、GPU pick after View destroy, Alpha soft-splat blending, Transient reset, Upload backpressure。验证 GPU output、错误回执、resident bytes、overflows、GPU memory peak、FPS CPU/GPU。

## E06 — External Plugin：Shader/PassParams/能力/卸载

```text
External Plugin SDK consumer (separate project, installed-prefix-only)
  declares LUX_PASS_PARAMS() OverlayParams + shader/.lglsl
  publishes FeatureDescriptor {provides OptionalHeatmapCapability}
  registers generated FeatureOps table from concrete OverlayFeature
  contributes pass using GraphBuilder public API
  Backend codegen/Shader reflection/LayoutPlan native compile
  integrated with built-in Mesh + UI Target, real GPU output readback
  uninstall while prior executable graph/callback in-flight
  stop new work, quiesce, GPU retire, release last code pin, DLL unload
```

验收：missing provider cold diagnostic、private include negative compile、ABI mismatch reject、Feature re-install with new DLL version no stale callback、fence pending cannot unload early。宿主暴露对能力的受限 API，不允许直接 `VulkanBackend*`。

## E07 — Simulation 10Hz / Render 60–144Hz & Target stop/resize

一次 PROGRAM 建 Scene/mesh/camera，后连续输出多帧（无新 PROGRAM），每帧增量 FrameSerial/RenderTime，Camera via legit UI update independent of Simulation；Graph compile_count stays fixed，GPU stable output/updated camera正确。多个 surface/offscreen targets 有各自 readiness，minimize 时 idle 不忙等，恢复时重建 GPU-safe；Device Lost 注入立即 terminal、无假继续渲染。

测试同时记录 **R2 input packet_count ≠ Frame rendered count** 和 **GPU completed serial ≠ frame serial**，CPU wake/sleep/latency、公平 UPLOAD 进度、独立 CTest + GPU validation。

## 14.8 哪些是编译期负例，哪些是 GPU 正例

- 编译期负例：错误 `GraphTexture` 传入 Buffer 参数、无 generated `PassSchema<T>`、临时 plan/borrow 泄漏、Feature 缺正确 `noexcept`、private SDK include。
- 冷编译负例：Shader/Graph access mismatches, invalid attachment format, missing required producer, incorrect capability or LayoutPlan budget。
- GPU 正例：真实 Tonemap/PBR/Shadow/HZB/alias/async/Multiview/Plugin/Target 输出与同步，不能由 fake native graph 替代。
- 生命周期正例：3 lane backpressure/stop/ticket abandon、Scene/View remove + GPU in-flight、Plugin unload、Asset shared across Scene。

以上七条是完成系统闭环的**最低真实集成测试**，业务 `Fxx` 和 UE `Hxx` 仍需要更完整的专项 oracle；不能将这七条冒称完整 Legacy parity。

---

# 15 — 真正可执行的阶段划分、文件准入、迁移依赖与强制 STOP

> 此阶段计划取代此前“R3.1 加 Builder”以及在图体系未定型时直接开始 R5 Runtime 的推进方式。R0–R4 的历史 I/V 不改、Legacy 不动。每个阶段必须先用户审查入场基线、明确 ALLOWED/READ-ONLY/FORBIDDEN、独立验证并 STOP。**不得同一个实施 Agent 自动连做多个阶段并自判审批。**

## 15.1 可执行依赖关系

```text
F0 (Design Check-in / Code Freeze)                  docs only
   └── F1 (PassSchema + Shader Toolchain + Builder authoring)
         └── F2 (Complete neutral Logical Graph / R3 replacement)
                └── F3 (Shader LayoutPlan + native Pipeline/Descriptor compilation)
                       └── F4 (Native Vulkan Graph compiler/recorder/resource/queue/alias)
                              └── F5 (One true RenderRuntime + N Scene/N View/N Target execution)
                                     └── F6 (Feature public SDK + Plugin + Capability lifecycle)
                                            └── F7 (Scene RenderSystem / Projection / ResourceDomain / data wiring)
                                                   └── F8 (Mesh/Material/GPU-driven/Animation/Camera)
                                                          └── F9 (Lighting/Shadow/Forward/Deferred/Postprocess)
                                                                 └── F10 (2D/Editor/Terrain/Water/PointCloud/Robotics)
                                                                       └── F11 (Legacy all-row parity + SDK/product cutover)
                                                                              └── H1..H6 (advanced quality algorithms)
```

跨阶段的最小 native test 允许使用专用 fixtures，如 F3 compute/graphics PSO GPU readback；**不许为 fixture 提前引入正式 Runtime/Feature/Scene**。F4 Graph-native CPU/GPU tests 可以直接使用 R4 Device/Queue/Retirement，F5 才建立真实唯一 Runtime。F0 文档集成是本包交付后首个准入，后续代码分别审阅授权。

## 15.2 各阶段必须交付的完整可验收产物

| Phase | ALLOWED 重点 | 必须交付的实际代码/行为 | 独立验收（失败即 PARTIAL） |
| --- | --- | --- | --- |
| **F0** | `docs/render-v2/final/**`, design mappings/test inventories | 完整 00–19 + CSV/manifest、type inventory、R0–R4 fact preservation、旧规范 supersession 索引 | Doc-only I，matrix completeness、SHA、无源码变更、clean clone V，STOP |
| **F1** | `render_graph` authoring/schema, `modules/resource/description`, `engine/toolchain/shader`, `luxpass` 现有模板；minimal bootstrap | typed texture/buffer/access/pass/Builder，`LUX_PASS_PARAMS` 生成 typed GraphUses/C++/GLSL，ShaderInfo bridge & compile-time C++20 Concepts | Tonemap/Blur/Storage/HZB Params fixtures + compile negatives + installed-like public headers；不创建 native graph |
| **F2** | `render_graph/**` logical | versioned Subresource Graph、scope、required/optional semantic/cull/condition/side-effect、complete LogicalPlan & safe cache matching；替换 R3 API | retain R3 300 random graphs/10k binding tests, augment subresource/scope/alias conditions, no Vulkan/Scene/Transport deps |
| **F3** | `render_vulkan/graph/compiler/`, descriptor/pipeline extensions, neutral Shader tooling | Complete LayoutPlan/relocation/Shader+Graph reconciliation, graphics/compute pipeline owner/catalog, all descriptor kinds/limits | real GPU output: Tonemap, compute SSBO, multiple set owners, wrong schema/limits fail, rollback; R4 native tests still pass |
| **F4** | `render_vulkan/graph/record/resource/sync`, minimal bootstrap tests | VulkanGraphCompiler, ExecutableGraphPlan, native barriers, physical allocation/alias, external import/export, single/multi queue fallback, record/submit/readback | real GPU HZB/transfer/graphics/local-read/MSAA/alias/multi-queue or documented unsupported fallback, validation 0 error; no Runtime |
| **F5** | `render_runtime`, `render_vulkan/backend`, target/frame/scene/view; bootstrap | **real** Runtime Backend thread + one FrameLoop, SceneRenderer/N Scene/N View/Target, graph cache owner, frame pacing, offscreen/readback/present, independent RenderProgress | one Scene/View clear GPU, no PROGRAM many frames, 10Hz vs 60–144 target, 1 Scene/4 View shared plan, multiScene, resize/minimize/lost/stop; no mock Runtime |
| **F6** | `render_vulkan/feature/public`, `render_features` minimal, SDK schema | Feature concrete+static ops, Capability/Provider attachment, shader/Graph pass SDK, versioned external plugin code pin/unload; no private includes | installed consumer external plugin real GPU draw/compute/readback, unload race, provider conflicts, ABI/code pin |
| **F7** | `render_data`, `scene_render`, `render_runtime` integration, `RenderResourceDomain`; scripts | one RenderSystem per Scene, typed `RenderProjection`, three lanes integrated, O(changed), asset/ready/receipt, backpressure revisions, CPU resource ownership | Scene mesh/light/camera create/update/remove/recreate, 2 Scenes shared asset and GPU retire, lane reorder, 10Hz Sim/60Hz View |
| **F8** | `render_features` Mesh/Material/Camera/Skinning/GPU cull/RenderCluster basics + realistic Shader assets | functional GPU-driven Mesh, Stage/VertexPool/Material variants, Camera, Skinning, GPU cull/compact/indirect, shader graph material preview | F01/F02/F03/F06/F07/F11/F12/F13/F14 and relevant Ixx GPU/oracle, perf before/after |
| **F9** | `render_features` Lighting/Shadow/PP | Light provider, Forward/Deferred/clustered, PCF+EVSM+CSM, MeshShadow, HZB, Skybox, LinearDepth/SSAO/Fog/Tonemap/Highlight/Water prerequisites | F04/F05/F08/F09/F10/F15–F20/F27 etc; real HDR GPU/image/reference |
| **F10** | `render_features` 2D/robotic/terrain/point/GUI and SDK consumers | Grid2D/3D, Gizmo, Canvas all kinds, Terrain fallback, Water, Trajectory, five point strategies, StreamingFeedback, UI/Picking/multi target | F21–F36 full algorithm matrix, MSVC GPU 0 validation, target composition, rebase, cache pressure, external SDK |
| **F11** | root/product build only now plus strict parity, installs | complete Legacy Capability Matrix PASS, actual Editor/Player/Toolchain, installed SDK, product feature demo + quality comparison | no old/dual renderer fallback, CTest/ASan/GPU/V1 oracle as available, historical NOT_RUN unchanged, deployment |
| **H1** | advanced virtual geometry | page hierarchy, GPU selection/streaming/LOD fallback, dedicated tests | real geometry quality/perf/capacity evidence |
| **H2** | advanced GI/reflection | dynamic GI/reflection cache, denoise, fallback paths | real dynamic HDR reference and hardware/software capability tests |
| **H3** | TSR/time-domain imaging | motion vectors, history validation/reprojection/upscale | temporal flicker/ghosting/resolution reference evidence |
| **H4** | VSM & advanced shadows | virtual page feedback/atlas/cache/clipmap | page miss/fallback/shadow quality/VRAM results |
| **H5** | advanced materials/volumetrics & large multi-view | layered BSDF, volume/shader variants and scalable Graph | reference scenes/NSight/RenderDoc equivalent, SDK consumer proof |
| **H6** | 3DGS-like Gaussian representation and mixed rendering | GPU projection, tile sort, transmittance, streaming, Mesh depth composition | actual 3DGS camera/GPU/reference and same Runtime/Graph/plugin proof |

F8/F9/F10 的具体能力所属子阶段可以在 F7 完成后按真实依赖细分，但不能删掉任何 Fxx 测试或者把已完成的 F1–F7 隔离门禁省略。H 阶段依赖平台 capability，设备不支持必须有可验证 fallback 而不是高档功能伪运行。

## 15.3 目录权限默认模板

| Phase | 允许修改 | 默认只读/禁止 |
| --- | --- | --- |
| F0 | `docs/render-v2/final/**` 和明确的 supersession 指针文档 | Core/Transport/Graph/Vulkan/Legacy/Engine/Root/Product；全部验收报告 |
| F1–F2 | Graph+指定工具链 Schema 模板+bootstrap最小接线 | Core/Transport/R4 Vulkan/Legacy/Runtime/Scene/Editor/Product |
| F3–F4 | `render_vulkan` native graph/pipeline/descriptor/transfer wrappers + tests | Core/Transport/R3 logical production除批准修复、Legacy/Scene/ECS/Editor/Runtime |
| F5 | `render_runtime` + Vulkan backend/frame/scene/target + tests | Feature concrete/Scene/ECS/Editor/Product/Legacy (minimal fixed test path 限此阶段) |
| F6–F7 | Feature public ops、minimal concrete Feature、`render_data`, `scene_render`、相关最小 runtime additions | Legacy、未授权产品替换、其它 FrameLoop/Manager |
| F8–F10 | 具体业务 Feature/Shader/Data /测试，必要依赖修正必须单独说明并审查 | Legacy、已有阶段验收历史报告、未授权核心架构重写 |
| F11 | Root/product/SDK integration + product tests | frozen Legacy 所有 blob unchanged，不引入 dual renderer/compat bridge |

单个 work order 必须重新列出确切目录/文件 `ALLOWED / READ-ONLY / FORBIDDEN`；上表不可视为对任何阶段所有变更的宽泛授权。发现必须修改已验收 lower layer 的行为时 STOP、附源码/失败 test 和最小改动提案，不准夹带。

## 15.4 每阶段 I/V 执行与用户工作区保护

- 确认远端 HEAD/父提交、过去 Implementation/Verification SHA、当前 source 内容和实际消费者/测试，记录冻结 V1 719 blobs identity。
- 在独立 Worktree 实施 `I`，对完整修改范围做 static/clang/source closure/功能/性能审查；不可污染用户原 Worktree 6 处已存改动。
- I 提交后从确切 SHA 创建**独立 clean clone**，无硬链接、无未提交补丁；执行两轮 full bootstrap build（第二轮应无工作），旧 tests + 新 tests、真实 dependency FileAPI、compiled include/link closure、Shader toolchain/codegen asset closure、sanitizers/GPU 在范围内。
- 对可比路径做同设备/同源/同质量基准；新增功能没有基准时先建立新 baseline，不能编造相对性能。
- 验收报告 V 必须单独**只新增验收**；发现 bug 先回到 I 新提交并对新基线完整重验；V 不准夹带生产 fix。
- 只以 PASS/PARTIAL/FAIL + exact NOT_RUN scope/report/hash 宣布完成；push 后 STOP，用户审查放行下一阶段。

## 15.5 旧 R5–R18 的迁移

历史计划 `R5 Runtime`, `R6 Minimal Feature`, `R7 typed data`, `R8 Scene RenderSystem`, `R9 ResourceDomain`, `R10 Mesh`, `R11 PointCloud`, `R12 Capability`, `R13 complex builtins`, `R17 product cutover` 等职责仍保留，但不依旧编号做“原计划依序编，Shader/Graph 以后补”的错误路径。新的 F1–F4 先完成唯一 Graph/Shader 完整接口，F5 才接真正 Runtime，F6/F7 再 Feature/Scene；F8–F11 完成全部旧功能闭环。**产品不因现有 R4 GPU PASS 而变成已可用。**

---

# 16 — 功能等价、编译/运行/GPU/SDK 资格矩阵与拒绝条件

## 16.1 真实性原则

一项“完成”不等于编译通过、宏能生成、CTest 总数增长或看上去颜色正确。分层证据：`C=public C++20 compile`, `L=logical expected result`, `S=Shader source+reflection+LayoutPlan`, `N=Native GPU validation/readback`, `T=Transport/lifecycle/stop`, `P=Performance`, `K=SDK independent consumer`, `X=Cross platform/product`。每项新功能的 `appendix/LegacyCapabilityMatrix.csv` 或 `GoldenWorkloads.csv` 标记所需门禁；不存在“单个万能冒烟测试覆盖所有 Feature”。

## 16.2 分类验收

| 层 | 正例 | 必须的负例/故障 |
| --- | --- | --- |
| Core/Transport | unchanged R1–R2-FIX headers/tests、PROGRAM/CONTROL/UPLOAD + typed generator + reply | wrong owner/stale ticket、WRITING abandon、cancel/stop、backpressure revision loss、dependency escape |
| Authoring C++20 | `GraphBuilder` with typed Params, optional Feature hooks, zero virtual callbacks | invalid GraphBuffer/Texture, absent generated Schema, unexpected mutable span/temporary plan, private SDK include |
| Graph Logic | deterministic order, RAW/WAW/WAR per Image/Buffer range, versioning, culling/scope, multiple logical plans | cycles/missing required producer, invalid optional fallback, uninit mip, unsafe Scene-scope share, conditions dangling |
| Shader/Toolchain | C++ params → generated C++/GLSL/SPIR-V → LayoutPlan → real Shader output | wrong std140/std430 offsets, reflected extra fields, shared set subset mismatch, budget overflow, SPIR-V relocation discrepancy |
| Native Vulkan | real graphics+compute+transfer, descriptor/PSO/queue/barriers, transient alias on/off, GPU retire | queue family hazard, alias unsafe overlap, GPU 0 validation requirement, Device Lost/candidate rollback |
| Runtime/Scene | one backend thread/frame loop, N Scene/N View/N Targets, Scene provider, cameras, independent frame | no Program frame stall, overflow wake busy-spin, resize/minimize deadlock, stale scene/view generation, Stop fence leak |
| Plugins | installed public SDK Feature shader/params, host registration, correct GPU result, unload | ABI mismatch, missing capability, private includes, unload while callback/plan/inflight refs active |
| Legacy business | 36 Feature/variant/owner rows + 30 infrastructure rows, actual GPU/error/perf | all variants not collapsed, wrong render path, source revision lost, quality config mismatched |
| Advanced quality | Nanite-like/Lumen-like/TSR-like/VSM-like algorithms individually, quality oracles | wrong visual claim, fallback hidden lower quality, shader/resource budget exceeded |

## 16.3 Golden workloads（高风险必须真实验收）

| Workload | 被验证的核心语义 | 反作弊判据 |
| --- | --- | --- |
| W01 Tonemap/Blur/Composite | generated PassParams + LayoutPlan + GPU sampled output | no manual binding in author code; 2-input binding accurate |
| W02 HZB + mip-specific storage | subresources, read/write version, barriers | arbitrary mip orphan/initial read rejected |
| W03 Compute→Graphics→Readback | queue sequencing/barrier/PSO | real GPU values, single-queue fallback; no fake async enum |
| W04 Transient alias vs no-alias | GPU overlap/alias proof, memory budget | same output, measured lower VRAM when safe; debug mode valid |
| W05 Scene 1/4 Views | shared plan vs per-camera work, Multiview optional | one compile key per compatible path; actual independent images |
| W06 2 Scenes shared asset | ResourceDomain dedupe, Scene-specific state | one asset owner/ready, no Scene data alias, teardown isolated |
| W07 Skinning across frames | pose-driven once/Scene, WAR, GPU vertex output | actual in-flight fence not frame arithmetic |
| W08 Shadow PCF/EVSM/CSM | lighting/shadow quality and projection key | all techniques, correct cascade sharing or separation |
| W09 Terrain streaming/page fallback | native page cache/parent child, backpressure | no missing geometry after eviction/reorder |
| W10 Five PointCloud modes | direct, cull+indirect, LOD, soft splat, transient | five distinct GPU workloads; not 3DGS false claim |
| W11 Canvas2D + UI + picking | 2D source/ordering/composite/target | per-View camera/grid/selection and stale pick rejection |
| W12 Plugin external host | compiler/public ABI and code pin | separate installed SDK + actual DLL unload GPU race |
| W13 10Hz Sim/144Hz Render | independent progress and pacing | frame_count > packet_count, no busy wait, compile stays same |
| W14 Lost/minimize/resize | native session terminal/target lifetime | no fence stall, no early VkImage destruction |

`appendix/GoldenWorkloads.csv` 在每个 Workload 给出 stage、expected oracle、source、device requirement、evidence status。若设备实际不支持 Multiview/async/local-read，运行对应**正确 fallback**，unsupported feature 原生专属测试必须明确 NOT_RUN 而非 PASS。

## 16.4 性能模型和 5% 纪律

以同 OS/Compiler/Device/driver、相同 HDR/shader/material/scenes/camera/quality/ref resolutions 和 prepared state 的 before/after 配对数据比较。R2 ten-case benchmark 是 Transport 参照，R3 1M binding benchmark 只代表当时简化 API，不是新完整 Graph 的全面性能基准；R4 native 59/59 只代表单 queue Foundation。新完整 Graph 要建立自身 throughput+VRAM+record_cpu+shadercompile/pipeline-build/validation oracles。

**对真正同功能同质量测量到的 p50/p95 中位 >5% 性能回退且无解释，不得标 PASS。** 若为了正确性引入必须成本，报告数据/替代优化策略，由用户决定；不能降图像质量、删 Feature 或隐藏 CPU allocations 来“恢复原数”。记录 warm fixed-capacity zero allocation（首方 C++）、cold/growth path 独立，native Vulkan/driver allocations separately。

## 16.5 文档/类型设计本身的资格

- 每种 public/nontrivial internal 类型必须有 inventory：语义、category、owner、dependent types、thread/lifetime、why not simpler value/function。
- 每个 Concept 必须指定它在编译期拒绝哪类错误，并有正负例 TU。不能将 `requires` 当装饰。
- 每个 Graph pass author code 必须产生相同 schema C++/GLSL/Shader reflection、实际 layout、backing accesses；Ghost/duplicate binding 负例明确失败。
- 每个新增 `Manager/Context/Interface` 属于默认禁止，只有带真实 consumer/性能/所有权证明且用户批准的设计修订才允许出现。
- 每个 Feature provider/consumer 只能有一个 authoritative data owner；未来的 Capability lookup 不能在热路径重复 name map。
- ViewGraph 默认 ownership 错误、Sim tick 驱动 frame、单独 PointCloud/2D Renderer、Plugin private include 等按静态/动态验收共同拒绝。

## 16.6 强制源追踪：66 项旧能力 + 新目标

`LegacyCapabilityMatrix.csv` 保留 F01–F36/I01–I30，旧源码 sha `a669409...`，行里的迁移状态初始 `NOT_MIGRATED`/基础部分 `VALIDATED_BASELINE`；每行通过时必须含 `ImplementationSHA`, `QualificationSHA`, `CTest/GPU Evidence`, `BeforeAfterMeasurement`（可比时）。测试未运行的应写清操作系统/工具缺失与依赖，不可以 pass-through。

新增 H01–H06 分别对应高级几何、GI/reflections、Temporal Upscaling、VSM、Advanced BSDF/volumetrics、3DGS，并定义独立效果算法/画质。现有 `CapabilityMatrix` 是**功能映射表**；源码保护/普通构建不等于功能取证。修复旧缺陷时必须保留旧行为对照和错误分类。

## 16.7 每阶段 clean clone/记录/PASS 裁决

每阶段必须有 I（生产实现，源状态固定）与独立 V（只新增报告）。正式验证从独立 `clone --no-hardlinks --no-checkout` 检出确切 I SHA，编译入口为 `cmake/render-v2-bootstrap/`，实际 File API/compile_commands/Ninja deps/Toolchain generator/Sdk header inputs/Legacy tree protection。两轮 full build，第 2 轮 no work；完整 CTest、独立公开头/编译负例、真实 GPU 测试或明确 NOT_RUN、性能原始日志、Sanitizer when available、原用户 workspace status/hash 保护、证据文件 SHA256 manifest；验收源不能现场打补丁。

FAIL 的示例：logic-only graph 能建图但 native barriers 未实现却报告完整 RenderGraph；延迟 `vkDeviceWaitIdle` 算资源退休；Shader 需要作者写两个绑定声明；复制旧 RenderFeature 继承类；UI Feature 在另一个 FrameLoop 输出；Panel 一片黑但 `if (!ready) return` 当成功；五 PointCloud 策略只做一个 direct points；外部 Plugin 编译必须 include internal Renderer header；性能用空 draw 替代真实 PSO。

---

# 17 — LLM 实施总合同、首个 F0 工作单与每阶段硬性禁令

> **本章对所有受委派的实施/测试/审核 LLM 具有最高执行优先级。** 它与 00–19 的功能/类型/性能合同一起使用。用户此次交付允许实施方**开始 F0 文档入库与结构验收**，但不意味着可以不经独立审阅自动修改 F1–H6 的生产代码。每阶段验收后 STOP，后续经用户明确放行。

## 17.1 你不是为了让 CTest 变绿而编程

交付目标是：**完整继承 Legacy 所有功能，建立可以容纳 UE 级先进算法的系统性 RenderGraph/Shader/Feature/Engine 模型，并按 C++20 值/算法/RAII/Concepts 明确职责。** 不接受空桩、最小删功能 vertical slice 代替完整阶段、不接真正 GPU 的接口声明、`Manager` 补偿依赖问题、只修测试让 PASS、自行削减范围、事后改报告掩盖失败。

## 17.2 每阶段开始的 Preflight

1. 获取实际 `origin/codex/render-v2` HEAD，不猜提交；检查是否与批准基线一致及是否有后续 commit。
2. 读取本 Final Docs `00–19`、阶段直接相关上下层、`appendix/CapabilityMatrix/TypeOwnership/RequirementsTrace/GoldenWorkloads`，核对 SOURCE SHA/hash/原工作树。
3. 读取真实当前消费者、CMake target/include/link/codegen graph，不依据过去助手陈述推断一个 API 已存在。
4. 列出**具体路径** `ALLOWED / READ-ONLY / FORBIDDEN` 与每个修改文件对应的阶段证据/设计条款。
5. 把功能清单中本阶段负责的 `Fxx/Ixx/Hxx` 行与至少一个 real consumer / result oracle 绑定。
6. 对新类型提交 Type Responsibility Inventory；对新模板/Concept 提交编译正负例与 ABI/perf 影响。
7. 检查是否产生两份 authority/重复缓存/额外 EventBus/FrameLoop/线程；有则先报告，不得实施。

## 17.3 不可变原则（违反立即 FAIL）

- Frozen Legacy `render_legacy/**` 不可编辑，719 个 files blob/mode/size 保持完全一致；旧 V1 oracle 使用独立 checkout，不与 V2 链接。
- 已通过的 R1 Core、R2/R2-FIX Transport、R4 Vulkan Foundation 的 source/行为/验证历史保持；R3 Graph 可按新设计重写，但不得保留旧 graph 永久 shadow implementation。
- `PROGRAM/CONTROL owner-thread SPSC`, `UPLOAD bounded MPMC`，bounded reply `consume or abandon`（WRITING writer-owned recycle），不增加 runtime fourth event bus。
- Simulation/Publication/Rendering 三条独立进度，Backend 不读 ECS，Render 不因无 PROGRAM 停帧，也不 busy spin。
- 单 Runtime/Backend/FrameExecutor/FrameDriver；N Scene, N View, N Targets；View 不默认创建 GraphInstance，同路径 compiled plan 共享；必须证明共享 GPU work 不改变输出。
- RenderData/SceneCapability/GraphResource 三域隔离；单 authority/typed provider；不得在 stable record/draw 每次查 registry/hash。
- `LUX_PASS_PARAMS` 及现有 meta/.lglsl toolchain 是 single source，不让 Feature 重复声明 shader binding 和 Graph read/write，作者不写裸 set/binding。
- Native owner 只在真实 Vulkan lifetime 作用域 RAII；no public init/shutdown/semantic zombie；GPU-backed retirement fence-proven，不以 WaitIdle 普通销毁。
- C++20 Concepts/value/free algorithms/static optional FeatureOps；无默认 Feature 虚基类、IBackend、GenericAllocator、TimeManager、UniversalHistoryManager。
- 外部 Plugin 真实可安装 SDK consumer + code pin/ABI/卸载；任何私有头引用为 fail。
- stable full frame zero first-party C++ allocation，Graph record 无 lazy pipeline/layout/Shader compile；性能/Correctness 必须同时达标。
- 高级 GPU 功能为**真实算法**、画质/错误/资源预算各有 oracle，不以 `Pass` 占位名称覆盖。

## 17.4 静态代码外观红线

下列新增模式默认拒绝，除非先有书面设计批准+真实消费者/性能/lifetime 证明：

```text
IBackend / IRenderFeature / Universal*Manager / *Controller / *Context wrapper
new Scripting/ShaderReflection/RenderData parser parallel to lux-cxx tooling
new secondary Renderer/FrameLoop/EventBus/TaskQueue/auto background worker
per-view GraphInstance by default; per-frame rebuild/compile with stable topology
per-draw std::function/shared_ptr/unordered_map/string lookup/reflection
Graph public include vulkan.h or Scene/ECS/Feature private headers
per Feature private VulkanDevice/DescriptorPool owner duplication
C++23 std::expected/std::move_only_function/std::ranges::to under cxx_std_20
std::span/string_view/raw callback borrowed across async packet or DLL unload
if (!some_required_resource) return success; while(!resource) spin
vkDeviceWaitIdle for routine view/scene/asset retirement
error status encoded as bool + out-param / second registry
compiled logical topology key misused as complete native plan equivalence
```

禁止只是 grep 关键词就自判这些全部已满足：必须审查真实 CMake File API/compile_commands、运行时 lifetime 和实际 output。`std::shared_ptr`, `std::function`, `Context` 在确有冷态/真实生命周期场景下可以被局部使用，但不能以字面禁词替代语义审查。

## 17.5 首阶段 F0：Document Authority Integration（现在可以执行）

### 入场与基线

```text
Repository: https://github.com/LUX-YU/lux-engine
Branch: codex/render-v2
Reviewed design-time HEAD: 7de3ddaa3f7614995745b41d73dfc98302310a4c
R4 implementation: 83ffbb6d9859d87ca62a8a2acbb0083c2a8de420
Frozen legacy SHA: a669409a289a6fa4092f21176397795b1cdb7f3e
```

实施时必须核对实时 HEAD；若不同，比较变化并反馈用户，不能重置或覆盖未经授权提交。

### ALLOWED

```text
docs/render-v2/final/**              (全新设计 00–19 + appendix)
docs/render-v2/00_README.md         (最小入口指针/新规范优先级)
docs/render-v2/F0_*                (work order、独立验收报告)
```

### READ-ONLY

```text
docs/render-v2/R0*_VERIFICATION.md, R1*/R2*/R3*/R4*_VERIFICATION.md
所有实际旧方案事实证据和问题报告
modules/function/render/{core,transport,graph,vulkan}/**
render_legacy/**, user checkout six modifications
```

### FORBIDDEN

```text
Root/product CMake, Engine, Scene/ECS, Editor/UI, Runtime/Feature implementation,
任何生产 C++ 头/源、第三方工具链/CMake target/test 变更
```

### F0 实施动作

1. 将本包 `00–19`、appendix CSV/manifest 同目录无缺失导入权威 `docs/render-v2/final/`；**保持字节一致或为路径/编码合法化作最小、逐项记录的更改**，不改 R0–R4 结果。
2. 更新 `docs/render-v2/00_README.md` 入口指针，标注**历史旧目标说明被新 Final Docs 的目标契约替换**。R2-FIX、R3、R4 已通过的事实/未运行范围不可被删除/改写。
3. 对比本包内 R2 旧章节/新的 00–19 与当前仓库五份已批准架构补丁：Simulation/Render independence、bounded reply, Device owner, Frame lifetime must all be present in Final Docs。列出 conflict mapping。
4. 运行 `MANIFEST.sha256`、Markdown fence/link检查、CSV 主键唯一性/追踪章号、初始 Status 合规检查、frozen Legacy manifest、用户工作区 status/hash 保护。
5. 仅提交 doc-only I；独立 clone 核查 docs 和 untouched source；V 只新增 F0 Verification（具体证据索引/manifest），push 后 STOP。

### F0 完成条件

- Final 00–19 一份权威（旧章节可保留为历史参考但不能二套同时生效）；版本号、Frozen SHA、已验收事实对齐。
- `CapabilityMatrix` 每一旧能力有来源路径和实现/验收目标，后续阶段 owner/测试可追踪；`TypeOwnership` 的所有独立 owner 无双权威；`RequirementsTrace` 每条 HARD GATE 有章节和测试。
- 无生产行为改动、Core/Transport/Graph/R4/native/Legacy 完全未改；用户 worktree untouched。
- STATUS=`F0 PASS` 仅代表文档集成，不等于 RenderGraph/UE 高画质已经实现。

## 17.6 后续阶段 F1–H6 的 work order 生成要求

F0 STOP 后依第 15 章 **单独逐阶段**生成 `Fxx_WORK_ORDER.md`；必须引用本 Final Design 精确章节和 source/targets/Capability ID、ALLOWED/READ-ONLY/FORBIDDEN 路径、负例/性能/GPU oracles、I/V/source identity、提交与 STOP。用户审查通过后才开始下一个阶段，不准同一个实现提交偷偷提前做 F3–F11。

## 17.7 资格验收规则

实现 I 提交固定后，独立 clone 构建两遍，第二遍 no work，CTest 既有全量+新增，无隐藏 `#include old SDK`、依赖 DAG 越界；实际 Codegen input/Shader source/Link packages hash，公共头独立 TU 与 negative compile target，Native GPU Validation/ASan when required；原 workspace diff/status/hash 未改变、719 legacy blobs 匹配。

对 GPU/installed SDK/product/Linux/Android 未运行的环节，只能写 `NOT_RUN` 并给理由；若本阶段硬性准入要求必须真实 GPU，缺 GPU 的阶段只能 `PARTIAL`。不能用“工具环境不允许”把未运行自动按 PASS。验收发现实现 bug，先新 I，重建 clean clone 验证，不能 V report 提交夹带修改。

## 17.8 STOP 报告模板

```text
Phase: Fxx / I SHA / V SHA / remote HEAD
Scope: changed files and authoritative design chapter IDs
Type changes: new/removed owner/value/algorithm/builder/ops, why
Legacy capability IDs: migrated / partial / not_run
Tests: old/new exact counts, clean clones, sanitizer/GPU, raw evidence
Performance: workload, warm/cold, p50/p95/max, alloc_count, gpu time, VRAM
Invariants: single owner, no double render loop, no private include, three lanes
NOT_RUN: exact items and platform/blocker
Bugs/risks: exact source/expected behavior/next user decision
Final STATUS=PASS|PARTIAL|FAIL, V2_PRODUCT=EXPECTED_UNAVAILABLE until cutover
STOP, await explicit user approval; no auto next phase
```

---

# 18 — 原 R2 文档与实际 R0–R4 历史的迁移、冲突消解

## 18.1 时间事实顺序与对历史的保护

- 原用户 zip：`LuxEngine_RenderV2_FinalDocs_R2(2).zip`（12 个目标设计章节，另有 SHA）；其文本先于 R2-FIX、R3、R4 的实际生产提交，属于**设计历史**。
- R0 frozen Legacy：`a669409a289a6fa4092f21176397795b1cdb7f3e`，719 文件逐 blob/size/mode 保留。R0 verification `db2c42c3d982` 前后确切 SHA 以 Git 记录为准。
- R1 Render Core `1d2fa0c0733d`, qualification `14319331d616`。
- R2 Transport `c33ae729016ed7cfbee1218d660dedb59a327157` 原资格 PARTIAL，R2-FIX `4ae55059cca949178a8a1f2eaffe32f720384e8d` + `e0eabe1640d94fafb80a57bd334507162f3b2f9f` 解决 reply lifecycle 并获授权改算法。
- R3 Logic Graph `ed4b78c1a4606f917eec1b30c654567935e6de48` + `f9b943c9e768ef2c9d408962f0d5a32d2cf3de74` 已有 CPU 45/45 的独立验收记录，但**最终 API 可重写**；不能据此说 Native RenderGraph 成立。
- R4 Vulkan Foundation `83ffbb6d9859d87ca62a8a2acbb0083c2a8de420` + `7de3ddaa3f7614995745b41d73dfc98302310a4c`，已报告 Windows/MSVC+native single GPU 59/59、MSVC ASan 13/13。R4 单 Queue native 基础不能虚称完整 Graph/Vulkan Backend。

新 Final Docs 是**目标规范**，不改变这些历史 I/V 和已经标记的 NOT_RUN；新系统是否符合目标，需要 F1–H5 的独立资格结果。项目原工作区 6 处修改不可污染。`V1_KNOWN_ISSUES.md` 仍含 transfer_idle / cross-frame WAR / Clang/UBSan / Linux/IME 等历史限制。

## 18.2 原 R2 的 00–11 到新文件的准确映射

| 原始 R2 文档 | 最终目标位置 | 必须修改的要点 |
| --- | --- | --- |
| `00_README_总览与文档索引` | `00`, `01`, `18` | 明确 F1 权威目标、当前历史实现分离，View 不默认有 Graph |
| `01_现状问题与Legacy冻结策略` | `01`, `10`, `18`, capability matrix | 完整列旧 36 Feature/变体 + 30 Infra，冻结和原有失败事实不变 |
| `02_分层架构与依赖规则` | `02`, `09`, `12` | values/algorithm/RAII、schema-only render_data、Backend-scope plan cache、public plugin ABI |
| `03_Core与Transport_数据契约和通信` | `03`, `13`, `17` | R2-FIX pre-reserved reply/explicit abandon/writer recycling 作为唯一终态；背压保留 revision |
| `04_RenderGraph与VulkanFoundation` | `04`, `05`, `06`, `07` | Builder+Params 同源、logical/native compiler、subresource、alias/queue/PSO/record 完整，不只三值类型 |
| `05_VulkanBackend_Runtime_Frame_Target` | `07`, `08`, `13` | independent render progress、multiview、Scene/View/Target execution，真实 pacing/no busy spin |
| `06_Feature系统与SceneCapability` | `09`, `10`, `12` | concrete Feature+Concepts+generated ops、provider/capability/code pin、public SDK，非虚框架 |
| `07_RenderSystem_Projection_ResourceDomain` | `03`, `08`, `09`, `10` | one Scene RenderSystem / O(changed) publisher、shared domain、CPU/GPU owner 分离 |
| `08_Cpp_RAII_错误语义与热路径规范` | `12`, `13`, `17` | C++20语言负例、zero alloc、error/retirement/ABI/stop 可验证 |
| `09_实施阶段_VerticalSlice_算法迁移` | `15`, `16`, capability matrix | F0–F11/H1–H6 新增 Graph/Shader 优先阶段，完整旧功能逐行迁移 |
| `10_验收门禁与LLM实施合同` | `16`, `17`, `19` | 已关闭决策，目录准入，独立 I/V+STOP，防偷工减料 |
| `11_R0_实施工作单` | `_references_R2_readonly/`（仅历史） | 不重开 R0，不通过 old API/fake wrappers 重做已结束动作 |

## 18.3 两处尤其不能覆盖的已批准修订

**R2-FIX 文档 03 §13、05 §18、07 §21、09 阶段门禁及 10 顶层 HARD GATE**：预留 reply cell 正式获批；Simulation/Publication/Rendering 独立；不要求恢复 `pending_reply_publish_`；现行 R3/R4 的历史验收陈述不得被原 zip 旧语句取代。

**最近关于 View 的修订**：过去 “RenderScene stores graph definition/cache；View 可有独立 Graph” 的表达改为：Backend compiled plan cache 是共享 ownership，`RenderView` 只持 Camera/Output/History/可选 RenderPath；同路径共享 plan 和可共享的 Scene work，Camera 不同不自动重编译；不同 View 可以选择不同 graph/effect/targets；Vulkan Multiview 是可选执行优化。任何文档仍写 “per-View GraphInstance mandatory” 都应被标注 superseded。

## 18.4 旧算法迁移与新设计目标的边界

旧 DependencyAnalyzer、RenderGraphCompiler/Recorder、SceneGraphCache、LayoutPlan/SpirvPatcher、PassParams/Shader emitter、ResourceRegistry/retirement、RenderTransport 及具体业务 Feature 的算法和 test vectors 应被逐个 source→invariant→new function→test 映射。**可以从 Legacy 学习实现思想，不能复制旧 owner hierarchy/Context 梯子。**

如果为适配新设计必须更改成熟 GPU 算法，其前提是写清 V1 与新结果的相同输入、不同语义、错误路径、before/after performance，必要时先 STOP。不能直接声称“重写后更现代，因此旧 sync assumptions 可以删”。

## 18.5 GitHub 文档集成原则

F0 doc-only 提交把 Final Docs 存入 `docs/render-v2/final/` 或等价唯一权威目录，并在旧 `docs/render-v2/00_README.md` 建立明确指针和 supersession 声明；绝不覆写 R0–R4 历史 qualification reports。`_references_R2_readonly` 仅供回溯，其他开发者不得当执行规范。仅用户批准的 F1 及以后阶段才会触碰生产源码。

该文件集在包内以 SHA manifest 防混淆；作为 F0 执行时必须再绑定实际提交 I/V SHA 和 evidence manifest，不能直接用本压缩包本地检查代替 Git qualification。

---

# 19 — 已冻结的设计决定 D01–D32：LLM 不得擅自改选

> 本表关闭此前 RD0 中 O-01–O-12 的阻断性开放方案：它们现在有**确定的执行策略与兼容降级边界**。实施方若遇到实际设备/工具链约束，须拿编译/源码/性能/GPU 证据请求设计更改，**不能自行选更容易的方案或声明某条“不在 MVP 中”**。

| ID | 决定（FINAL） | 不允许的替代 | 对应章节 |
| --- | --- | --- | --- |
| D01 | 恰好一个 `RenderRuntime`、一个 `VulkanBackend` 和一个 FrameExecutor/Driver；N Scene/N View/N Targets | 第二 Server/Renderer/FrameLoop/同步桥 Manager | 02,08 |
| D02 | `RenderView` 是 Camera/Output/History/可选 RenderPath owner，**非**默认 GraphInstance owner | 同 Scene 两相机各完整建/编译一图 | 04,08 |
| D03 | 相容 View 共享 immutable logical/native plans；Native key 包含 Shader/PSO/Target/Device 实际身份 | 仅用 logical hazards/hash 比较复用不同效果的 PSO | 06,07 |
| D04 | Scene-global pass 仅在 input/scope/epoch/side-effects 可证明相同的 invocation 合并结果 | 因 Pass 名称相同或 Shader 不含 Camera 就共享 | 04,08 |
| D05 | Vulkan Multiview 是可选 View-level native optimization，合法设备/attachment/shader/data 下使用，完整 separate fallback | 新 `MultiviewFeature/Renderer` 或强行合并不同可见集 | 07,08 |
| D06 | `RenderDataTypeId`、`SceneCapabilityId`、`GraphResourceId` 三个独立域，不能同一 requires/ID | GenericDependencyId、字符串动态消息分派 | 01–04 |
| D07 | 一 Scene 一个 `RenderSystem`，内部 typed Projection；CPU source 数据不能被 Backend 直接读取 | 每 Feature 一个 SceneSystem；全量每帧遍历 ECS | 03,09 |
| D08 | 三 Lane：PROGRAM/CONTROL owner-thread SPSC、UPLOAD bounded MPMC，跨 Lane 无总序，使用 ready/receipt causality | 全局 EventBus、新后台 writer、隐式多线程消费者 | 03,13 |
| D09 | R2-FIX reply 采用已验收 pre-reserved cell + consume/abandon + writer reclamation | 恢复 V1 pending retry 或让取消自动抛弃接收责任 | 03,17 |
| D10 | 共享 `RenderResourceDomain` 默认每 Runtime integration 一份，是 CPU semantic asset ledger，不是 native GPU owner | per-Scene 重复 Asset upload/第二 GPU backing authority | 02,03 |
| D11 | Concrete Feature + C++20 Concepts optional hooks + generated fixed ops，FeatureInstance 单一 membership | virtual Feature base/empty callbacks/FeatureManager 层级 | 09,12 |
| D12 | Provider 能没有 Pass；Capability 每 Scene 一个 active provider 默认；互相依赖 cold attach resolve/teardown | 隐式安装顺序、热路径 registry.find<T>() | 09 |
| D13 | `RenderGraphBuilder` 是主 Authoring API；Definition validated owning value；Logic 编译独立函数；Native plan RAII owner | raw vector<Pass> 为主 API、static factory 包揽所有算法 | 04,06,12 |
| D14 | 使用现有 `LUX_PASS_PARAMS` / `LUX_RESOURCE` + lux-cxx meta generator + `.lglsl` emitter，并扩展语义到所有资源 | 第二 Shader parser、手写重复 read()/bindDS()/Vk binding | 04,05 |
| D15 | 完整类型化 Shader contract + reflection reconciliation + LayoutPlan 唯一最终 descriptor 位置与完整 shape | Shader 写 set/binding 或按单 shader 反射子集创建全局 set | 05 |
| D16 | Shared Set Shape 由真实 Engine/Feature owner 的完整契约决定；pass-private 才按 schema/reflection 生成 | 以数值 set slot 当 owner identity、私有 set 合并出错 | 05 |
| D17 | Cold SPIR-V descriptor relocation 与 schema/reflect/binary/layout checksum 校验；Pipeline candidate transaction | record 时 patch/重新编译/缺 Layout 就 new 一个 | 05,07 |
| D18 | Logical Graph 支持 texture aspect/mip/layer 与 buffer byte ranges/versioned RAW/WAW/WAR | 永久 only whole-resource，read before write 靠注册次序 | 04,06 |
| D19 | Pass scope Scene/View/Target，Culling/condition/side-effect 必须有证明和 fallback | skip producer 后读未定义数据、所有 Pass 每帧 rebuild | 04,06 |
| D20 | Stable topology compiled plan 持久复用；camera/frame/render_time/offset 只改变 Bindings | 每帧 GraphBuilder→analyze→compile→record | 06,08 |
| D21 | Graph native compilation C9–C13 包含 Shader/Pipeline/Descriptor/Barrier/Queue/Physical/Record recipe | CPU Graph PASS 冒称全 RenderGraph 完成 | 07 |
| D22 | Transient aliasing 只在 queue/GPU overlap proof 成立时使用；真实 noalias fallback + GPU oracle | 以 pass first/last index 直接复用 native memory | 06,07 |
| D23 | Temporal history 由具体 View/Feature 持有；Graph 只表达 current/previous/import 的读写 | UniversalHistoryManager 或所有 View 共享 TAA/HZB | 04,08 |
| D24 | R4 native Foundation RAII owner 保留；扩展它的 Graphics/Descriptor/Multi-queue 能力，不建第二套 VMA/Device | 新 VulkanFoundation2、旧 Context/ResourceContext owner ladder | 02,07 |
| D25 | Multi-queue 真 semaphore/ownership/sync2；不支持按合法 single queue fallback，真实 GPU 验证 | 只有 `AsyncCompute` enum、NONE/NONE 假 split barriers | 07 |
| D26 | 公共插件接口为 typed Feature/Graph/Shader/capability + versioned C ABI/code pin，插件独立安装消费者验证 | private VkDevice/RenderScene 头、跨 DLL `std::vector` 默认 owning ABI | 09 |
| D27 | C++20 values/free functions/short Builders/RAII owners/Concepts 静态约束，既有 cxx::expected | “现代 C++”=所有类都加模板/继承或偷偷升级 C++23 | 12 |
| D28 | 正常 failure 返回结构化 `RenderResult`，未满足 runtime invariant fail-fast，native GPU 严格 fence lifetime | `if (!ready) return success`, per-resource WaitIdle, silent black frame | 13 |
| D29 | Fixed warm stable frame 0 first-party C++ heap、无 lazy PSO/layout、zero recompile；cold growth 分开计量 | 删除功能/偷降画质/提前无界内存分配换得数字 | 13,16 |
| D30 | 719 Legacy frozen，F01–F36/I01–I30 分条真实功能 parity，H01–H06 独立算法，不能删任意旧变体 | PointCloud 五策略改成一种、只有 PCF 冒称 Shadow 完成 | 10,11,16 |
| D31 | F0–F11/H1–H6 独立 I/V 阶段和 STOP；旧 R0–R4 验收/NOT_RUN 不回溯修改，F11 真实产品/SDK cutover | V 提交夹带生产修复、为了 green 产品加兼容伪目标 | 15–18 |
| D32 | Runtime 独立进度；无 PROGRAM 仍能 paced frame，GPU completion/callback/diagnostics与 FrameSerial 分开 | tick→packet→frame 一一绑定、empty queue busy spin | 03,08 |

## 19.1 对此前 O-01–O-12 的具体封闭

| 旧议题 | 最终决定（无需再问实施方） | 执行阶段 |
| --- | --- | --- |
| O-01 PassParams grammar | 现有 luxpass annotations 扩展成 typed resource kind/role/range/scope/required/array/struct；Build-time frozen golden schema；生成 C++/GLSL 是单一输出，非法 C++20 会编译失败 | F1 |
| O-02 Pass recording public interface | `GraphicsPassContext/ComputePassContext/TransferPassContext` 的**受限 typed GPU ops**，在已验证 compiled recipe 上录制；仅 backend-native extension 经额外 capability 审批；不做通用 RHI | F3/F4/F6 |
| O-03 intra-pass hazards | field schema 明确 overlap ranges、load/readwrite/stage/subresource；feedback/local-read 单独合法设备变体，unsupported fail 或 explicit multi-pass fallback | F2–F4 |
| O-04 plan borrow/retirement | Definition/LogicalPlan own values, NativePlan owns compiled/native/code pin, FrameBindings short scope borrows; candidate-before-publication + fence-retire | F2–F5 |
| O-05 queue topology | 首先保留 R4 single queue，Graph C12 实现可选真实多 Queue-domain schedules/timeline/binary semaphores/ownership；不支持时正确单 queue | F4 |
| O-06 physical allocator | Graph transient planner 复用 R4 `VulkanAllocator` 创建 native owners，按真实 queue overlap/compatibility lease/retire；保守 noalias + debug poison | F4 |
| O-07 builder/direct arrays | Builder 是唯一正式 author path；direct Definition 只供内部测试/serialization 和一致性 validator；类型/字段不可绕过 PassSchema | F1/F2 |
| O-08 cross-feature names | 使用 typed semantic export slot/capability provider 非拥有解析；required/optional/fallback 有 cold validation，不靠安装顺序或字符串 first hit | F2/F6 |
| O-09 material ownership | neutral `resource/description` metadata、Toolchain offline shaderc/.lglsl/meta、render_content cooked format；运行时默认只装已 cook shader，Editor hotreload 冷态候选 | F1/F3/F8 |
| O-10 large-world/time/history | CPU scene page+local/double or high/low, GPU view-relative projection有单一 schema；View camera独立，history/cut/resize invalidate explicit, no universal time manager | F5/F7/F8 |
| O-11 SDK ABI/link | versioned C table/opaque handle+single host registry/allocator；插件内部 C++20 typed API生成 ops，跨 DLL不传 owning STL；外部独立安装消费者实测 | F6/F11 |
| O-12 GPU/quality budgets | 固定 correctness/quality scenes、设备、resolution、camera、shader/material hash；可比 performance >5% unexplained STOP；UE 新算法单独指标 | F4–H5 |

## 19.2 设计允许的设备能力降级（不属于偷工减料）

当 GPU 不支持 async compute、Multiview、Local Read、Hardware Ray Tracing、descriptor indexing、dynamic rendering 变体或格式，Implementation 可按既定**保守且正确**的功能路径降级；记录 device caps、明确输出质量、真实 GPU test/NOT_RUN。不能把“降级”变成删除相应 Feature 或不提供正确单队列/单 View/常规资源方案的理由。

资源不足、设备丢失和 ABI mismatch 属于明确错误/终止/容量约束，不能通过伪造 Ready、虚构 GPU Completed Serial 或静默丢包降级。UE 高画质特性在 Compat 档位不必工作，但基础旧功能（相应设备支持下）必须有真实图像输出与 fallback。

## 19.3 最终检查与改变设计的唯一方式

每个阶段默认遵守本表；如果发现 Dxx 决策与新 native device 或真正业务的事实相冲突，必须提交：`冲突条款 + 精确源码 + 最小失败测试 + 可复现环境 + 备选设计对比（类型/Owner/ABI/性能） + 迁移计划`。保持当前阶段 PARTIAL/STOP，等用户审查批准新的 doc commit 才能改。**不得以 LLM 对语言风格的个人偏好、方便编译、缩小 Scope 或可扩展性口号静默修改。**
