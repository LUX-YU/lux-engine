# Lux Engine L5 Editor、Runtime Capability 与 Product Composition 架构总览

Status: **Normative Architecture / Implementation Baseline (v3)**  
Date: **2026-09-02**  
Reviewed repository checkpoint: `main@4593ce9b02ddbe35d81de2ded309666ede0bb8da` (`docs(qualification): record l5 v2 foundation closure`).  
Canonical topology source checked on `main`: `.internal/directory-target-product-architecture.md`.

> 本文是当前 L5 架构基线。B/V1/V2/A/F/G 的方向已经进入当前实现；v3 不重新设计这些 foundation，而是先通过 `07` 定义的 R0 Foundation Requalification gate 修复可复现性与两个生命周期/并发缺陷，再进入 Lux UI 与 C/D/E。当前 HEAD 若移动，只允许重新映射物理路径/target；不得自行重新解释本文已冻结的层级语义。精确执行规则见 `08-normative-execution-contract.md`。

---

## 1. 设计目标

L5 Editor 的目标不是简单恢复 legacy Editor，也不是重新制造一套 retained-mode GUI framework。当前 Lux 已经具备：

- `LuxObject / Object<T>`：对象身份、thread affinity、typed signal、direct/queued/auto delivery；
- Runtime/Static Reflection 与 code generation；
- `ui::Pane / UISession / CommandRouter / ViewportElement`，并将在 Wave U 收敛为 backend-isolated Lux UI public API；
- L1 authoritative ECS/Simulation；
- L3 Scene composition；
- L4 Material / FlowForge / Asset Toolchain；
- L0 AssetVfs；
- Platform FileWatcher。

L5 应把这些能力组合成一个高性能、插件友好、生命周期清楚的 Editor 应用体系；同一批低层 runtime capability 也必须能被项目专用的最终游戏二进制复用，而不是被 Editor 私有化。

核心目标：

```text
1. UI 热路径尽量由 typed codegen 完成，而不是 runtime reflection 解释；generated/Editor/plugin code 只调用 Lux UI，不直接调用 Dear ImGui。
2. LuxObject / Signal 用于 control plane，不侵入 ECS component 数据热路径。
3. EditorContext 作为显式传递的 Editor-wide capability root。
4. Toolset 维护长期 L4 tool 实例；Pane/Editor window 不拥有工具生命周期。
5. MaterialEditor / FlowForgeEditor / SceneEditor 等名称本身就是窗口级 Editor，不再追加 Pane。
6. L2 Process 负责异步执行机制；L4 Tool 负责具体编译/转换语义。
7. AssetVfs 是 product/application-wide Resource capability，不属于 EditorContext；EditorContext 只借用只读 VFS view / asset-loading capability。
8. 最终游戏不是固定 `Player` 程序，而是由项目配置、选定模块/System 与 generated code 组合出的项目专用 executable target。
9. Scene Outliner 不预设实体一定构成层级树；Hierarchy 是一种可选 projection。
10. GraphKit 的共享价值进一步下沉为纯 Graph Source Topology；Renderer 与 Compiler 都消费同一 source。
11. 共享 Graph topology，不共享 Material 与 FlowForge 的语言语义。
12. Delay、资产加载、GPU/physics query 等跨时间操作必须挂起脚本逻辑而不是阻塞 game/main thread。
13. `modules/function/ui` 是公共 UI 边界；Dear ImGui/imgui-node-editor 只允许存在于其 private backend。
14. Legacy 可作为 selected visual/interaction/behavior reference，但绝不是 ownership/architecture source of truth。
```

---

## 2. L0–L5 与 Product Composition（Product 不是架构层）

```text
L0  Reusable Modules / Foundations
    ├─ core/object              LuxObject / Signal / Dispatcher
    ├─ core/meta/type_info      reflection + code generation projections
    ├─ resource/asset           AssetId / AssetVfs / provider contracts
    ├─ function/ui              Lux UI public API / Pane / UISession / Theme / ViewportElement / private ImGui backend
    ├─ function/material        Material source language
    ├─ function/flowforge       FlowForge source language
    └─ function/graph           shared pure source topology (Wave F frozen owner)

L1  Domain
    ├─ ECS Registry / ComponentSchema / Component operations
    ├─ Transform / Hierarchy / Visual components
    └─ Simulation systems and authoritative synchronous behavior

L2  Process
    ├─ execution                Sender scheduling / timers / structured lifetime
    ├─ asset_loading            time-spanning typed asset-load workflow
    └─ world_loading            explicit world-loading workflow

L3  Scene
    ├─ SceneMetaManager
    ├─ SceneDescription
    ├─ Scene composition + authoritative Registry
    └─ optional Render SceneSystem integration

L4  Toolchain
    ├─ Material compiler/cooker
    ├─ FlowForge compiler
    ├─ Shader/Texture/Model cooker
    └─ pack/export/build tools

L5  Editor
    ├─ EditorApplication composition leaf
    ├─ EditorContext (non-owning capability aggregate)
    ├─ Toolset
    ├─ EntityInspector / AssetBrowser / SceneEditor
    ├─ MaterialEditor / FlowForgeEditor
    └─ graph editing/render protocol + editor file monitoring

Product / Application Composition（不是 L6）
    ├─ EditorApplication executable
    └─ generated project-specific game executable target
```

`Product` 是 build/product closure 维度，不再定义 `Product/Application composition/Product` 架构层。`EditorApplication` 可以位于 L5 的 application leaf；最终游戏由项目 target generation 直接组合 L0–L4/runtime 能力与项目代码。不得为了数字层级整齐创建 `HostManager/ProductHost/ApplicationServices`。

### 2.1 依赖方向

允许：

```text
EditorApplication -> L5 feature packages -> L4/L3/L2/L1/L0
GeneratedGameTarget -> L3/L2/L1/L0 + project selected runtime modules
L5 -> L4/L3/L2/L1/L0
L4 -> L2/L1/L0 (only when semantically needed)
L3 -> L2/L1/L0
L2 domain workflow -> lower-layer contracts; execution leaf remains domain-blind
```

禁止：

```text
L0/L1/L2/L3 -> L5
L0/L1/L2/L3 runtime -> L4 compiler
L4 Toolchain -> L5 Editor
L2 execution -> Material / FlowForge / Editor vocabulary
runtime system -> EditorContext
```

### 2.2 Product target model

`PLAYER` 只能继续作为 runtime-clean **qualification profile** 使用；它不是最终交付产品的固定 executable contract。

最终目标：

```text
Project configuration
    + selected engine modules/backends
    + selected SimulationSystems / SceneSystems
    + project native/generated code
    + reflection/codegen products
    + cooked assets / pak configuration
            ↓
      project-specific target
            ↓
         MyGame executable
```

Shipping target SHOULD 支持 monolithic/LTO/dead stripping 等 whole-program 优化；精确 target generator/project manifest 由独立 Product Target 规格冻结，L5 coding wave 不得自行发明固定 `LuxPlayer` 架构。

## 3. EditorApplication 与 EditorContext：owner 和 capability view 分离

`EditorContext` 仍然是所有 Editor window 构造时显式传入的统一访问路径，且 **当前 contract 不拥有 process/application-wide resource**。它是一个非 owning capability aggregate。

物理 ownership 冻结为：

```text
L5 EditorApplication
├─ owns process-lifetime L2 ExecutionRuntime
├─ owns mutable AssetVfs control plane
├─ owns/installs production/editor AssetRead endpoint/port
├─ owns RenderRuntime / platform-window state when configured
├─ owns immutable SceneMetaManager
├─ owns Toolset
├─ owns EditorSelection
├─ owns UISession
├─ owns Editor root TaskScope
│
└─ constructs EditorContext
      ├─ references Toolset
      ├─ carries AssetVfsView (read capability)
      ├─ carries AssetReadPort / typed asset-loading capability
      ├─ references ExecutionRuntime + root TaskScope
      ├─ references EditorSelection / UISession / SceneMetaManager
      └─ never owns the above lifetimes
```

推荐 public 形态：

```cpp
struct EditorContextCreateInfo final
{
    Toolset& toolset;
    asset::AssetVfsView vfs;
    process::asset_loading::AssetReadPort asset_read;
    process::ExecutionRuntime& execution;
    process::TaskScope& tasks;
    EditorSelection& selection;
    ui::UISession& ui;
    const scene::SceneMetaManager& scene_meta;
};

class EditorContext final
{
public:
    explicit EditorContext(EditorContextCreateInfo) noexcept;

    [[nodiscard]] Toolset& toolchain() noexcept;
    [[nodiscard]] asset::AssetVfsView vfs() const noexcept;
    [[nodiscard]] process::asset_loading::AssetReadPort assetRead() const noexcept;
    [[nodiscard]] process::ExecutionRuntime& execution() noexcept;
    [[nodiscard]] process::TaskScope& tasks() noexcept;
    [[nodiscard]] EditorSelection& selection() noexcept;
    [[nodiscard]] ui::UISession& ui() noexcept;
    [[nodiscard]] const scene::SceneMetaManager& sceneMeta() const noexcept;
};
```

硬约束：

- MUST 显式传入 `EditorContext&`；禁止 `EditorContext::instance()`、TLS global、静态 service locator。
- `EditorContext` MUST NOT value-own `AssetVfs`、`ExecutionRuntime`、`TaskScope`、`Toolset` 或 `EditorSelection`；这些 owner 都是 `EditorApplication`。
- UI window MUST NOT 获得 VFS `mount/unmount` control plane；常规 UI 只拿 `AssetVfsView`。Project/application composition 负责 mount table 变更。
- `EditorContext` MUST NOT 提供无限制 `get<T>()`；typed dynamic lookup 只允许存在于语义明确的 `Toolset`。
- feature-local graph/document/camera/popup 数据不得进入 Context。
- v1 Selection 仍是一个 EditorApplication-owned `EditorSelection`，使用 L5 `EditorSceneHandle` 代际身份；Context 只引用它。

所有主要 Editor UI surface：

```cpp
MaterialEditor(EditorContext& context, ...);
FlowForgeEditor(EditorContext& context, ...);
EntityInspector(EditorContext& context);
AssetBrowser(EditorContext& context);
SceneEditor(EditorContext& context, ...);
```

## 4. Toolset：长期 L4 工具实例容器

Editor 中需要长期存在的 L4 capability 由 `Toolset` 统一拥有：

```cpp
context.toolchain().get<MaterialGraphCompiler>();
context.toolchain().get<FlowForgeCompiler>();
```

`Toolset` 的 v1 语义：

```text
Toolset
├─ install<T>(...): composition phase only
├─ get<T>(): required capability; missing = fail-closed programmer/composition error
├─ find<T>(): optional capability probing only
└─ freeze(): after composition no install/remove mutation
```

Key MUST 使用稳定 typed identity/type token，不得使用用户字符串。

Toolset 中的 Compiler MUST 是：

```text
long-lived capability
+ immutable environment/configuration
+ copyable scheduler handle
- current graph
- current IR
- current diagnostics
- in-flight job collection
- cancellation source
```

也就是说 `MaterialGraphCompiler`/`FlowForgeCompiler` 是 **logically stateless, reentrant Sender factory**。同一个实例必须允许多个 compile invocation 并发执行。

一次 compile 的 mutable state 不进入 Toolset，而进入 Sender operation state：

```text
Compiler.compile(owned snapshot)
          ↓
       Sender
          ↓ connect/start
OperationState
├─ owned immutable source snapshot
├─ request options
├─ stop token/callback
├─ temporary IR/context/files
├─ diagnostics under construction
└─ receiver/result
```

关闭 `MaterialEditor`/`FlowForgeEditor` 不影响 Toolset，也不隐式取消已经由 EditorApplication root `TaskScope` 接管的 task。

## 5. UI 数据热路径 vs Control Plane

L5 明确采用“双通道”设计。

### 5.1 数据热路径

以 Inspector 修改 Transform3D 为例：

```text
Generated Component Inspector Binding
        ↓ typed access
Registry.try_get<Transform3D>()
        ↓
Lux UI typed widget
        ↓ direct write
registry.patch<Transform3D>()
        ↓ EnTT on_update
TransformSystem marks dirty entity
        ↓ next stable point
WorldTransform3D update
        ↓
Render/Physics/Nav consumers
```

禁止把它改成：

```text
Lux UI / backend-specific escape
 -> runtime RefField
 -> generic mutation request
 -> LuxObject signal
 -> EditorModel
 -> Scene adapter
 -> Registry
```

### 5.2 Control Plane

LuxObject / Signal 用在语义边沿：

```text
selection changed
active document changed
compile job completed
asset catalog changed
file change stabilized
pane focus/visibility changed
project opened/closed
```

它们不是每个 component 字段变更的 transport。

---

## 6. Codegen-first Editor UI

第一方和第三方插件都使用公开的 Lux reflection/codegen SDK 生成 UI binding。

原则：

```text
Known at compile/codegen time
    -> generate typed C++

Truly dynamic runtime metadata
    -> not required for standard Editor UI path
```

因此不规划 runtime reflection fallback 作为 Inspector 正常工作方式。

生成 projection 可以并列：

```text
Annotated Source
     │
     ├─ ECS schema projection       (L1)
     ├─ serialization projection    (where applicable)
     ├─ Editor UI binding           (L5 build product)
     └─ Graph node presentation     (for graph-based languages)
```

同一 annotation source 描述语义，但不同层的 projection 互不依赖。

Generated Editor binding 的 public path 冻结为：

```text
Generated Component/Graph Presentation Binding
        ↓
EditorValueBinding<T> / Lux presentation adapter
        ↓
modules/function/ui public API
        ↓ private backend only
Dear ImGui / imgui-node-editor
```

MUST NOT 生成 `#include <imgui.h>` 的 Editor/plugin code。UI backend isolation、Theme、Frame/scope/leaf-widget 模型见 `10-lux-ui-foundation-and-legacy-visual-parity.md`。

---

## 7. Editor window 命名与职责

统一采用功能名：

```text
EntityInspector
AssetBrowser
SceneEditor
SceneOutliner
MaterialEditor
FlowForgeEditor
```

这些对象本身可以继承 `ui::Pane`，无需 `XxxPane` 二次命名。

窗口负责：

- UI drawing；
- 局部交互状态；
- 从 EditorContext 获取长期共享工具/资源；
- 对 domain/source data 发出直接或 canonical mutation；
- 命令上下文和 focus。

窗口不负责：

- Toolchain tool 生命周期；
- VFS 生命周期；
- process executor 生命周期；
- unrelated feature service；
- 关闭窗口时自动取消所有后台业务任务。

---

## 8. Product-wide VFS 与 Editor 只读资源访问

`AssetVfs` 属于低层 Resource/product runtime 能力，不属于 Editor 语义。Editor 与最终项目游戏必须共享同样的 `/Game`、`/Engine`、patch/plugin mount 解析规则。

```text
EditorApplication / GeneratedGame composition
                 │ owns mutable AssetVfs
                 ├─ mount / unmount control plane
                 └─ publishes AssetVfsView
                          │
          ┌───────────────┼──────────────────┐
          ▼               ▼                  ▼
   EditorContext       AssetRead endpoint   runtime systems
     vfs() view            / Sender          narrow capability
```

Editor windows 通过 `EditorContext.vfs()` 做 mounted-view 查询，但不拥有 VFS，也不改变 mount table；可能触盘的 asset image/content 读取同样应走 `assetRead()`。脚本/运行时异步资产读取不得直接调用同步 `AssetVfs::open()`，而通过 L2 `asset_loading` / `AssetReadPort`。

禁止把 VFS 改成 static 方法、`AssetVfs::Get()` lazy singleton 或隐藏静态 state；初始化顺序和 shutdown 仍由 product/application composition root 显式控制。

---

## 9. Scene 不是必然 hierarchy

Scene 的“有哪些对象”与“这些对象如何组织展示”必须分开。

```text
SceneOutliner
    ├─ Flat projection
    ├─ Parent/Hierarchy projection
    ├─ Spatial/partition projection
    └─ future domain-specific projection
```

如果 Scene/Simulation 使用 `Parent`，Outliner 可以展示树；如果没有 Parent，就显示 flat entity/object list。

Inspector 只关心当前 selection 对应对象及其 editor-visible components，不关心 Outliner 的 projection。

---

## 10. Shared Graph Source

新的长期目标：把 GraphKit 中真正通用的 topology 下沉为纯 source-data foundation。

```text
Shared Graph Source
├─ GraphTopology
├─ GraphLayout
├─ NodeId / PinId / Link
└─ structural invariants
       │
       ├─ MaterialGraph + typed Material payload
       └─ FlowGraph     + typed Flow payload
```

然后产生两条消费路径：

```text
Source -> L4 lowering -> domain IR -> final artifact
Source -> L5 render protocol -> editor rendering / interaction intents
```

共享 topology，不共享 language semantics；禁止 `GenericGraphIR`、万能 property bag、字符串 payload。

---


## 11. L2 Process、异步资产与耗时 Toolchain

L2 `process/execution` 负责时间与执行，不负责 Material/FlowForge 语义。

当前 reviewed foundation 的 L2 vocabulary 包含：

```text
PortSender     OperationPort -> stdexec Sender adapter
TimerQueue     bounded timer owner
TimerSender    cancellable timer Sender
AssetLoadSender<T>  ReadAssetImage -> typed decode Sender
```
当前 active L2 还已有 `engine/process/asset_loading::loadAsset<T>()` Sender：它把 `ReadAssetImage` OperationPort 与 typed decode 组合起来。current contract 将其视为 runtime async-asset 主干，而不是 Editor 专用机制。生产 endpoint MUST 保证实际阻塞 storage read 不在 game/main thread inline 执行。


L5/L4 异步 Toolchain 复用的 P0 foundation 冻结为：

```text
ExecutionRuntime
├─ CpuScheduler
├─ MainScheduler
├─ TimerQueue/TimerClient (existing)
└─ runtime stop/join + bounded admission

TaskScope
└─ structured ownership of arbitrary Sender operations
```

Wave B 之外的扩展：

```text
BlockingScheduler  # authorized/required by Wave V2 when blocking storage needs IO isolation
ProcessSender       # still P1; only for real subprocess lifecycle needs
```

依赖方向：

```text
L5 MaterialEditor
       ↓
L4 MaterialGraphCompiler  -- immutable/reentrant
       ↓ returns domain Sender
L2 CpuScheduler
       ↓ executes
L4 deterministic compile core
       ↓
L2 MainScheduler
       ↓
L5 applies result by stable document/revision identity
```

关键约束：

1. L2 MUST NOT 定义 `MaterialCompileOperation`/`FlowForgeCompileOperation` 等 domain vocabulary。
2. Compiler async API MUST consume an **owned source snapshot**；禁止 worker 借用 UI 正在修改的 graph reference/pointer。
3. Compiler instance MUST NOT 保存 invocation mutable state；per-call MLIRContext、shaderc compiler/options、temporary directory、IR 等都属于调用状态。
4. Compile failure（invalid graph/type mismatch/shader diagnostics）是 domain result；Execution stop/rejection 是 execution channel，二者不得混为同一个错误概念。
5. `TaskScope` 负责 operation lifetime/cancellation；Compiler 不提供模糊的 `compiler.cancel()`。
6. `MainScheduler` 是 host-drained mailbox，不创建第二个“main thread”。
7. Wave B 只实现 CPU/Main/TaskScope；不得因 FlowForge 未来会等待 linker 而提前扩展 Blocking/Process framework。

## 12. 与 legacy 的关系

Legacy 是 **selected visual / interaction / behavior reference + algorithm/code mine**，不是 architecture source of truth。

当当前 Lux Editor 明确希望延续旧 UX 时，可以参考/尽量保留：

- panel composition、toolbar placement、spacing rhythm、icons、selection/highlight visual language；
- Inspector 的 property grouping、vector/value editing、asset picker 用户交互；
- AssetBrowser 的 grid/list/search/breadcrumb；
- Scene Outliner 的 rename/delete/create/context-menu UX，以及 hierarchy mode 的视觉语言；
- Graph 的 node chrome、pin/link readability、undo、stable ID、drag-connect 经验；
- FileWatcher debounce/stable write 经验；
- AsyncRuntime 的 background CPU/main-thread completion 概念。

目标是 recognizable visual/interaction parity，而不是像素级复制。Theme/design tokens 必须集中在 Lux UI；feature code 不复制 Legacy magic colors/spacing。

禁止为了视觉一致恢复：

```text
巨大 EditorScene
raw pointer retarget web
callback injection web
AssetManager/Authoring manager ownership
panel-owned compiler pipeline
universal EditorContext service locator
runtime reflection-driven hot path
raw ImGui call-site structure
retained widget tree copied from another GUI model
GenericGraph/UniversalGraph language
```

详细 UI/Legacy parity contract 见 `10-lux-ui-foundation-and-legacy-visual-parity.md`。

---

## 13. 当前实现 checkpoint 与 Foundation Requalification

前一轮 review 已确认历史 Pre-L5 Material/Graph 风险在当前代码方向上基本关闭：Material malformed source preflight、installed relocatability、shared shader include boundary 与 Graph transaction rollback 都已有实质实现。B/V1/V2/A/F/G 也已经进入当前 `main`。

但 reviewed HEAD 仍不能直接视为“可继续 feature development 的 clean qualified foundation”，因为存在三个当前阻断项：

```text
R0.1 committed-source reproducibility
    CMake references engine/editor/application/test/editor_application_test.cpp
    but the reviewed Git tree does not track that file.

R0.2 EditorApplication public lifecycle
    installTool<T>() must not dereference a disengaged Toolset after shutdown;
    COMPOSING/RUNNING/STOPPING/JOINED behavior must be explicit.

R0.3 TaskScope re-entrant admission
    eager Sender start/spawn and stop callbacks must not run while TaskScope
    holds its own state/admission mutex; close must account for in-flight starts.
```

因此下一步不是回滚 foundation，而是完成 `07` 的 R0 gate，从一个全新 clean checkout 重新执行 profile/build/test/install-consumer qualification，然后进入 Wave U/C/D/E。

---

## 14. 脚本跨时间操作

Delay、资产加载、GPU/physics query 等不得通过 `sleep`、`sync_wait`、GPU `Wait()` 阻塞 game/main thread。

```text
script invoke
  -> run until async node
  -> start Sender/domain async operation
  -> persist continuation/state
  -> return control to Simulation
  -> operation completion enqueues resume fact
  -> resume at an explicit Simulation stable/resume point
```

Sender 是异步 operation protocol；FlowForge continuation/state machine 是脚本 suspension protocol。GPU query 的具体语义留在 Render/Scene domain，L2 只提供 scheduling/cancellation mechanism。精确约束见 `09-product-runtime-vfs-and-async-script.md`。

---

## 15. 文档集

本设计集包含：

```text
README.md
00-L5-architecture-overview.md
01-editor-context-toolset-and-plugins.md
02-entity-inspector-codegen-ui.md
03-asset-vfs-filewatch.md
04-scene-editor-outliner-viewport.md
05-shared-graph-source-and-graphkit.md
06-toolchain-process-async-execution.md
07-implementation-roadmap-and-gates.md
08-normative-execution-contract.md
09-product-runtime-vfs-and-async-script.md
10-lux-ui-foundation-and-legacy-visual-parity.md
```

实现时优先读取 `00` 与 `08`，再读取唯一 implementation DAG `07`。任何 Editor UI work 必须同时读取 `10`；涉及 product runtime/VFS/脚本异步时必须同时读取 `09`。

---

> Coding implementation MUST also comply with `08-normative-execution-contract.md`.
