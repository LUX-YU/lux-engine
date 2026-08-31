# Directory / Target / Product Architecture

本文件是 lux-engine source topology、CMake target 与安装产品边界的设计真源。

## Canonical source topology

```text
modules/
  core/
  resource/
  function/

engine/
  domain/
    partition/
    spatial/
    world/
      identity/
    simulation/
  process/
  scene/
  authoring/
  editor/
  toolchain/
```

- `modules/` 是可复用 L0 package。
- `engine/domain/world/identity` 与 `engine/domain/partition` 是中立的 L1 engine-domain identity；
  `engine/domain/spatial` 是具体空间查询机制。
  两者不拥有 World、Simulation 或 streaming policy。
- `engine/domain/world` 与 `engine/domain/simulation` 是 sibling runtime domains，均可依赖窄义 `DOMAIN`
  foundation，但不得互相依赖。
- `engine/process` 是 L2 collection；`execution` 叶包保持领域盲，具体 `world`/`asset` 叶包可拥有明确的
  time-spanning workflow，但不得把领域语义反向塞入 execution。
- `engine/scene` 是 L3 runtime composition：`core` 只组合一个 World、一个 authoritative Registry、
  一个 Simulation 与 Scene cancellation source；`runtime/world` 提供机械 World IO/materialization；
  `runtime/presentation` 首先承载 latest-state handoff。Streaming policy 只属于 concrete developer System。
- `engine/authoring` 持有 authored state、composition 与诊断。
- `engine/editor` 持有交互式 Editor UI/tooling。
- `engine/toolchain` 持有离线 compiler、cook、package 与 build tool。
- `engine/tools` 是同义叠加的退休目录，不得恢复。

public include 与 namespace 使用职责概念名，不包含 `domain`、层级编号或物理聚合目录名。

## L0–L3 canonical semantics

```text
World       = durable/cooked facts + whole-world storage metadata
Simulation  = concrete Systems + synchronous rules + compiled schedule
Process     = domain-blind execution substrate + package-scoped asynchronous domain workflows
Scene       = one World + one authoritative Registry + one Simulation
Presentation= independently sampled runtime concern, not an architecture layer
```

Canonical user-facing `Transform2D/3D` 与 `WorldTransform2D/3D` 使用 double。Render、physics、nav
等 consumer 只能在自己的 local-origin boundary 先用 double 相减，再显式 narrow 到 float/native。

Scene 不拥有 mandatory streaming/index/residency/process/render/main-loop state；Simulation 不拥有
World IO、partition lifecycle 或 wall clock；Process workflow不得认识 Scene 或 gameplay policy。

3D Render integration 是可选的 L3 leaf：`engine/scene/runtime/render` 在 Simulation stable point把
`Mesh3D/Light3D/WorldTransform3D` 合成为 bounded `RenderProgram(StateUpdate)`；Presentation只转发更新并
author `RenderProgram(Frame)`。StateUpdate只修改 retained RenderScene，只有 Frame推进渲染生命周期。
该 leaf不改变 headless Scene core，也不授权 Presentation Registry、Asset resolver或residency framework。

## Package 与 collection

- package root 可以拥有 `include/`、`sinclude/`、`pinclude/`、`src/` 和 `test/`。
- collection root 只拥有聚合用 `CMakeLists.txt` 与 production child packages。
- collection root 不得同时拥有自身 public/private source tree。
- package root 的 `test/`、`cmake/` 与 `src/` 内部目录不视为 production child package。

`modules/function/script` 与 `engine/domain/simulation/scripting` 都是 collection；Script backend、
Script artifact 与 Simulation scripting core 各自是独立 package。

`engine/process` 也是 collection；`execution` 只提供通用 Sender/Timer mechanism。`world` 与 `asset` 仅在
存在真实workflow时创建；Render sender、Streaming owner和dynamic fan-out不得以空package或shim占位。

## Product closure

- PLAYER 只包含 Runtime/Domain 能力，不得链接 Editor、Toolchain、FlowForge compiler、MLIR/LLVM。
- EDITOR 可以依赖 Authoring 和通用 Node Graph Editor。
- TOOLCHAIN 可以依赖 Authoring、FlowForge compiler 和语言 packager，但不得依赖 Editor UI。
- build-tool 关系通过 custom command/generated file 表达，不作为 Runtime link dependency。

## Canonical Script / FlowForge ownership

- generic Asset storage 位于 `modules/resource/asset`，不持有 domain-specific cooked artifact。
- Script semantic/runtime primitives 位于 `modules/function/script/core`。
- Script cooked content/codec 位于 `modules/function/script/artifact`。
- reusable FlowForge graph/model 位于 `modules/function/flowforge`。
- Script runtime composition 位于 `engine/domain/simulation/scripting` 与 `systems/script`。
- Node Graph Editor 位于 `engine/editor/node_graph`。
- FlowForge compiler 位于 `engine/toolchain/flowforge`。
- Lua source packager 位于 `engine/toolchain/lua`。

旧路径、target 与类型迁移不保留 shim、alias 或双路径安装。
