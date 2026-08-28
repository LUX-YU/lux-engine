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
    world/
    simulation/
  process/
  authoring/
  editor/
  toolchain/
```

- `modules/` 是可复用 L0 package。
- `engine/domain` 持有 World/Simulation runtime domain integration。
- `engine/process` 是 L2 collection；当前 `execution` 叶包只提供 Timer 与 L0 OperationPort 的
  Sender adapter，不拥有 CPU pool、main loop、Render thread 或 domain workflow。
- `engine/authoring` 持有 authored state、composition 与诊断。
- `engine/editor` 持有交互式 Editor UI/tooling。
- `engine/toolchain` 持有离线 compiler、cook、package 与 build tool。
- `engine/tools` 是同义叠加的退休目录，不得恢复。

public include 与 namespace 使用职责概念名，不包含 `domain`、层级编号或物理聚合目录名。

## Package 与 collection

- package root 可以拥有 `include/`、`sinclude/`、`pinclude/`、`src/` 和 `test/`。
- collection root 只拥有聚合用 `CMakeLists.txt` 与 production child packages。
- collection root 不得同时拥有自身 public/private source tree。
- package root 的 `test/`、`cmake/` 与 `src/` 内部目录不视为 production child package。

`modules/function/script` 与 `engine/domain/simulation/scripting` 都是 collection；Script backend、
Script artifact 与 Simulation scripting core 各自是独立 package。

`engine/process` 也是 collection；Wave 0/1 只配置 `execution`。File IO、Render sender、
Asset/Streaming workflow 和 dynamic fan-out 在对应后续 Wave 到来前不得以空 package 或 shim 占位。

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
