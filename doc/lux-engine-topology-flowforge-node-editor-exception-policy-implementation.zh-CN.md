# Lux Engine Source Topology / FlowForge / Node Graph Editor / Exception Policy
## 并行实施规范

> 状态：Implementation Spec  
> 基线：`LUX-YU/lux-engine` main @ `c5d1b9395d86fea72407ee71b27b1b182d4b1617`  
> 适用范围：与正在实施的 L1 Script/System Endpoint Scale Closure **并行实施**  
> 原则：不保留兼容层、不复制旧模型、不为了目录整理增加 manager/registry/context/facade。

---

## 0. 本文解决什么问题

当前 main 已经开始形成新的分层架构，但 source tree 仍然带有明显的迁移期形态：

```text
engine/
  authoring/
  flowforge/
  graph_kit/
  simulation/
  toolchain/
  world/
```

这使几个本质不同的问题混在了一起：

1. `engine/graph_kit` 实际是 Editor-only 的通用节点图编辑器，但目录名和 layer classification 都不能准确表达它的职责。
2. `engine/flowforge` 同时包含 FlowForge 图语义、Simulation Script binding integration、MLIR/AOT compiler，导致 L0 language/model 与 L1/L5 依赖混层。
3. `engine/` 根目录平铺各层模块，source topology 无法一眼表达 dependency direction。
4. `engine/simulation/ecs` 既包含 ECS mechanism/data，也包含 concrete System；而 `ScriptSystem` 又在 `simulation/script`，用户使用 built-in System 时路径不统一。
5. Runtime/Builder/Codec/Toolchain 中都有 `try/catch`，缺乏项目级 exception contract。

本次实施的目标不是再设计一套新架构，而是让**物理目录、target ownership、public API 和 canonical layer contract 对齐**。

---

# 1. 与正在进行的 Script/Hook/Event 性能重构的关系

这两项工作必须视为正交任务。

正在进行的性能重构负责：

```text
Hook/Event sparse-dense storage
O(1) registration/target lookup
Script runtime flat layout
Script attach/detach scale closure
safe-point lifetime closure
backend runtime indexes
```

本文负责：

```text
source topology
FlowForge layer split
node graph editor ownership
Simulation built-in System placement
exception policy
```

## 1.1 冲突控制

实施顺序建议：

```text
A. 先完成文件内部的 Script/Hook/Event 性能语义修改
B. 再执行本文要求的 mechanical source move / include move
C. 最后统一 CMake / install surface / architecture gates
```

如果必须在同一个工作分支并行：

- 不在搬迁前复制 `ScriptSystem.cpp`、`HookPoint.hpp`、`EventPoint.hpp`。
- 使用 `git mv` 或等价 move，禁止“旧路径保留 + 新路径复制”。
- 最终提交中旧 source root 必须物理为空/删除。
- 不允许 forwarding headers、target aliases、namespace compatibility aliases。

---

# 2. 总体设计原则

## 2.1 Source topology 表达 ownership，不机械复制 namespace

物理目录回答：

> 这个代码属于哪一层？谁可以依赖它？

public include / namespace 回答：

> 用户使用的概念是什么？

因此允许：

```text
source:
engine/domain/simulation/...

public include:
lux/engine/simulation/...

namespace:
lux::simulation
```

禁止为了 source layer 增加：

```cpp
lux::domain::simulation
lux::tools::editor::...
```

除非这些层级本身就是用户 API 概念。

---

## 2.2 Lowering 只允许“删除上层依赖”，不能把上层依赖一起搬下去

例如 FlowForge graph/model 下沉 L0 时：

```text
允许依赖：
modules/platform
modules/core
modules/resource
modules/function lower peers
lux-cxx

禁止依赖：
engine/world
engine/simulation
engine/process
engine/scene
engine/authoring
engine/tools/editor
engine/tools/toolchain
```

如果一个现有类型依赖 Simulation，则先切断依赖，再移动；不能把 Simulation header 一起带进 L0。

---

## 2.3 不因目录拆分增加逻辑层

移动：

```text
engine/simulation/script
→ systems/script + scripting
```

不意味着增加：

```text
ScriptSystemManager
ScriptRuntimeFacade
ScriptBackendRegistry2
BuiltInSystemManager
```

目录拆分必须尽量只改变 physical ownership。

---

# 3. 最终 source topology

建议冻结为：

```text
modules/
  platform/
  core/
  resource/
  function/
    flowforge/
    ...

engine/
  domain/
    world/
    simulation/

  process/           # 有代码时存在，不创建空 placeholder

  scene/             # 有代码时存在，不创建空 placeholder

  authoring/

  tools/
    toolchain/
    editor/

  host/              # 有 Product/Host 代码时存在，不创建空 placeholder
```

## 3.1 各 source root 的含义

| Root | Canonical responsibility |
|---|---|
| `modules/` | L0 reusable mechanism / resource / function semantics |
| `engine/domain/` | L1 World facts + Simulation rules/synchronous mechanisms |
| `engine/process/` | L2 asynchronous orchestration |
| `engine/scene/` | L3 runtime composition |
| `engine/authoring/` | L4 mutable authored forms / editing-domain logic |
| `engine/tools/toolchain/` | L5 compiler/import/cook/build tools |
| `engine/tools/editor/` | L5 interactive editor execution/UI tools |
| `engine/host/` | L6 Product / Host composition |

## 3.2 为什么使用 `domain`

L1 中：

```text
World      = domain facts
Simulation = domain rules + synchronous runtime mechanisms
```

`domain/` 比 `runtime/`、`foundation/`、`L1/` 更准确，也不会污染用户 API。

---

# 4. engine/CMakeLists.txt 目标形态

当前根 CMake 直接 add：

```cmake
add_subdirectory(world)
add_subdirectory(simulation)
...
add_subdirectory(flowforge)
add_subdirectory(graph_kit)
add_subdirectory(toolchain)
```

完成后应表达 layer ownership，例如：

```cmake
add_subdirectory(domain)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/process/CMakeLists.txt")
    add_subdirectory(process)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/scene/CMakeLists.txt")
    add_subdirectory(scene)
endif()

if(LUX_BUILD_PROFILE MATCHES "^(EDITOR|TOOLCHAIN)$")
    add_subdirectory(authoring)
endif()

if(LUX_BUILD_PROFILE MATCHES "^(EDITOR|TOOLCHAIN)$")
    add_subdirectory(tools)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/host/CMakeLists.txt")
    add_subdirectory(host)
endif()
```

具体 profile gating 可沿用项目既有 policy，但**不能再由 `engine/flowforge` 自己根据 profile 决定“今天我是 Editor，明天我是 Toolchain”**。

一个 target 的 layer ownership 必须固定。

---

# 5. Node Graph Editor：替代 `graph_kit`

## 5.1 当前职责

现有 `engine/graph_kit` 已经具备：

```text
GraphEditor
GraphCommandStack
GraphLayout
GraphTypes
IGraphSchema
IGraphView
PinIdBimap
```

并直接依赖：

```text
function::ui
imgui-node-editor
```

它不拥有 FlowForge graph，也不应拥有 Material graph。

因此它不是 graph data toolkit，而是：

> **domain-independent interactive node graph editor framework**

---

## 5.2 新位置和名字

物理路径：

```text
engine/tools/editor/node_graph/
```

建议 target：

```text
node_graph_editor
lux::engine::editor::node_graph
```

建议 public include：

```cpp
#include <lux/engine/editor/node_graph/GraphEditor.hpp>
#include <lux/engine/editor/node_graph/IGraphView.hpp>
#include <lux/engine/editor/node_graph/IGraphSchema.hpp>
#include <lux/engine/editor/node_graph/GraphCommandStack.hpp>
```

建议 namespace：

```cpp
namespace lux::editor::node_graph
```

如果实施成本过大，可以保留类型名：

```text
GraphEditor
IGraphView
IGraphSchema
GraphLayout
GraphCommandStack
```

不需要机械改成：

```text
NodeGraphEditor
INodeGraphView
INodeGraphSchema
```

module/namespace 已经提供足够语义。

---

## 5.3 必须删除

迁移完成后删除：

```text
engine/graph_kit/
lux/engine/graph_kit/*
lux::graphkit
lux::engine::graph_kit::graph_kit target
```

不提供 forwarding header。

不提供：

```cmake
add_library(graph_kit ALIAS node_graph_editor)
```

除非安装系统的内部 target namespace 本身需要一个 canonical exported alias；不能为了旧消费者保留 compatibility alias。

---

## 5.4 Editor adapter ownership

Node Graph Editor 只能通过：

```text
IGraphView
IGraphSchema
```

观察/修改领域 graph。

FlowForge adapter 属于：

```text
engine/tools/editor/flowforge/
```

Material Graph adapter 未来属于：

```text
engine/tools/editor/material_graph/
```

**当前没有现成 adapter 代码时，不创建空 target/空目录。**

---

# 6. FlowForge：拆成 L0 language/model 与 L5 compiler

## 6.1 最终依赖

```text
modules/function/flowforge
        ▲
        │
        ├──────── engine/tools/toolchain/flowforge
        │                 compiler / MLIR / AOT
        │
        └──────── engine/tools/editor/flowforge
                          editor adapter only
```

FlowForge model 不依赖 Editor。

FlowForge compiler 不依赖 Editor。

Editor 可以依赖 model 和 compiler frontend API，但不是 compiler 的 owner。

---

# 7. `modules/function/flowforge`：L0 FlowForge semantic model

建议物理路径：

```text
modules/function/flowforge/
  CMakeLists.txt
  include/
  src/
  test/
```

public namespace 可继续：

```cpp
lux::flowforge
```

public include **可以保留现有概念路径**：

```cpp
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
```

这不是兼容层：文件安装路径本身可以直接保持该路径，只是 source ownership 移到 `modules/function/flowforge`。

不要仅因为 physical layer 改变而制造无意义 API churn。

---

# 8. 哪些 FlowForge 类型应下沉

当前 `engine/flowforge` 中，下列概念属于 FlowForge language/model：

```text
FlowGraph
NodeBase / Node
ControlNode
FunctionalNode
ObjectNode
ArithmeticNode
NodeRegistry   （若它只注册 FlowForge semantic node kinds）
StateLayout semantic calculation
Pin / Link semantic types
Graph-local variable semantics
```

它们的共同定义是：

> 编译器、Editor、测试、未来其它 producer 都需要理解它们；它们不依赖 Simulation/Editor UI 才有意义。

因此应该移动到 L0。

---

# 9. FlowGraph 下沉前必须移除 Editor sidecar

当前 `FlowGraph` 中存在：

```cpp
using NodeUserDataPtr = std::unique_ptr<void, void (*)(void*)>;

struct NodeStorage
{
    std::unique_ptr<Node> node;
    size_t index;
    NodeUserDataPtr user_data;
};
```

`user_data` 不属于 FlowForge language semantics。

必须从 L0 model 中删除。

Editor 需要的：

```text
canvas position
selection
collapsed state
preview state
node widget cache
imgui node id mapping
```

由 Editor adapter 使用 stable node ID 单独保存：

```text
FlowForgeNodeId
    ↓
Editor sidecar
```

不要创建一个 `EditorNode` 包裹 FlowForge Node。

不要把 UI state 加回 FlowGraph。

---

# 10. RuntimeObject / RefType 在 FlowForge L0 中的处理

当前 FlowGraph variable 使用：

```cpp
const lux::meta::RefType* type;
lux::meta::RuntimeObject default_value;
```

它们来自 L0 `core/meta`，所以**从 dependency layer 角度允许存在于 `modules/function/flowforge`**。

本次迁移不要求为了“看起来更 POD”再发明：

```text
FlowForgeValue
FlowForgeType
GraphVariant
SerializedRuntimeObject
```

但是必须明确：

- `RuntimeObject` 是 in-memory semantic/default value holder，不等于 wire persistence format。
- `RefType*` 是 process-local metadata pointer，不得直接序列化。
- 如果未来需要 canonical FlowForge asset wire description，应由 codec 使用 stable semantic identity 编码/解码，不要复制一套平行 graph model。

本次不创建 `FlowGraphDescription` 与 `FlowGraph` 双模型，除非后续 persistence contract 证明两者确实具有不同 ownership/lifetime。

---

# 11. `flowforge_script` 当前必须拆除 Simulation coupling

当前 `ScriptGraph.hpp` 同时包含：

```cpp
lux::rdesc::ScriptValueType
lux::simulation::script::ScriptBindingTarget
```

并定义：

```cpp
ExportMethodNode
BindingEdge
```

其中：

```cpp
BindingEdge {
    FlowForgeExportNodeId export_node;
    ScriptBindingTarget target;
};
```

这使 FlowForge graph 直接知道 Simulation ScriptSystem endpoint binding。

这个依赖必须删除。

---

## 11.1 正确 ownership

FlowForge 负责：

```text
“这个 graph 导出哪些 executable functions？”
```

Script authoring / ScriptSystemDescription 负责：

```text
“某个 ScriptSymbolId 绑定到哪个 System Hook/Event？”
```

因此：

```text
FlowForge Export
    ↓ produces
Script asset export manifest

Script authoring
    ↓ independently authors
ScriptSystemDescription bindings
```

FlowForge compiler **不能**生成 Simulation binding template。

---

## 11.2 `BindingEdge` 的处置

如果 `BindingEdge` 唯一用途是把 FlowForge export 连接到 `ScriptBindingTarget`：

> 删除该类型。

不要移动到 L0。

不要改名成：

```text
FlowForgeRuntimeBinding
FlowForgeScriptBinding
CompiledBindingEdge
```

Binding 的 canonical SSOT 已经是 `ScriptSystemDescription`。

---

# 12. FlowForge compiler 下沉/上移后的职责

新路径：

```text
engine/tools/toolchain/flowforge/
  compiler/
  dialect/
  ...
```

编译器是 Toolchain，不是 Editor ownership。

Editor 只是 compiler 的一个 client。

headless cook / CI / command-line build 也应能使用 compiler。

---

# 13. `ScriptArtifactCompiler` 必须去 Simulation 化

当前 API 形态包含：

```cpp
#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/simulation/script/ScriptSystemDescription.hpp>

FlowForgeScriptArtifact {
    Script description;
    FlowForgeAotAbiManifest abi;
    vector<ScriptBindingDescription> binding_template;
};

compileFlowForgeScript(
    module_name,
    simulation_scope,
    graph_exports,
    graph_bindings,
    SimulationDescription&,
    state
);
```

这不应保留。

目标 API 应收敛到：

```cpp
struct FlowForgeScriptArtifact
{
    lux::rdesc::Script description;
    FlowForgeAotAbiManifest abi;
};
```

编译入口只消费 FlowForge semantic graph / exports / state information 与编译目标：

```text
FlowForge graph semantics
module identity
state layout
compiler options / target backend
```

不再消费：

```text
SimulationDescription
ScriptSystemDescription
ScriptBindingTarget
simulation_scope authored binding choice
```

是否 Simulation scope / Entity scope 是 Script Mount 的属性，不是 FlowForge compiler 的属性。

---

# 14. FlowForge compiler 输出边界

Compiler 输出的是 executable/resource artifact information，例如：

```text
Script description/export manifest
AOT ABI manifest
compiled payload / object/module data（若现有 pipeline 已负责）
compiler diagnostics
```

Compiler 不输出：

```text
ScriptSystemDescription
binding_template
SystemInstanceId
HookPointId
EventPointId
WorldObjectId
Entity
```

这样 FlowForge Toolchain 可以完全独立于 Simulation。

---

# 15. 删除旧 `engine/flowforge` root

迁移完成后，`engine/flowforge` 不应继续作为混合 root 存在。

内容分别进入：

```text
modules/function/flowforge/             # semantic graph/model
engine/tools/toolchain/flowforge/       # compiler
engine/tools/editor/flowforge/          # 仅已有 Editor adapter 时
```

旧：

```text
engine/flowforge/
```

物理删除。

不提供 compatibility CMake target。

---

# 16. Simulation 内部重排原则

当前 `simulation/ecs` 中混合了：

```text
ECS primitive
component schema
snapshot
TaskGraph/ECS task bridge
hierarchy data/index/maintenance
transform data + TransformSystem
```

而 ScriptSystem 在：

```text
simulation/script
```

问题不是 ECS 目录存在，而是：

> concrete System 的目录归属由“它碰巧使用什么底层数据”决定，而不是由“它是一个 System”决定。

必须分开：

```text
ECS mechanism/data
≠
System that operates on ECS
```

---

# 17. Simulation 最终内部 topology

建议：

```text
engine/domain/simulation/

  description/
  asset/

  system/                     # generic System mechanism
    HookPoint
    EventPoint
    SystemConcept
    SystemRegistry
    SystemAccessSpec
    SystemError
    ...

  systems/                    # concrete built-in Systems
    transform/
    hierarchy/
    script/

  ecs/                        # ECS mechanism/data only
    core/
    schema/
    snapshot/
    task/
    transform/
    hierarchy/

  scripting/                  # script language/backend mechanism
    core/
    cpp_static/
    lua/
    native/
```

---

# 18. `system/` 与 `systems/` 的明确区别

```text
system/
    “什么是 System，以及 System 如何存在/暴露 endpoint”

systems/
    “Lux 内置了哪些 concrete Systems”
```

这两个目录同时存在是有意设计，不应合并。

禁止把 generic HookPoint/EventPoint 塞进 `systems/core`。

禁止把 concrete TransformSystem 再放回 `ecs/transform`。

---

# 19. Transform 拆分

当前：

```text
simulation/ecs/transform/
  Transform.hpp
  TransformSchema.hpp
  TransformSystem.hpp
  TransformSystem.cpp
```

目标：

```text
simulation/ecs/transform/
  Transform.hpp
  TransformSchema.hpp
  ... ECS data/index only

simulation/systems/transform/
  TransformSystem.hpp
  TransformSystem.cpp
  System-specific private detail
```

public include 建议统一：

```cpp
#include <lux/engine/simulation/systems/TransformSystem.hpp>
```

ECS data：

```cpp
#include <lux/engine/simulation/ecs/Transform.hpp>
```

---

# 20. Hierarchy 拆分

必须先按真实职责分类，不能看文件名机械移动。

保留在 ECS hierarchy：

```text
Parent
HierarchyIndex
HierarchySchema
纯 index/data maintenance helper（若不是 System object）
```

如果存在真正注册进 Simulation 的 concrete hierarchy System，则移动到：

```text
simulation/systems/hierarchy/
```

如果当前 `HierarchyMaintenance` 只是 `HierarchyIndex` 的内部 reactive helper，不要为了目录对称强行改造成一个 `HierarchySystem`。

**不创建不存在的 System abstraction。**

---

# 21. Script 拆分

当前 `simulation/script` 同时拥有：

```text
ScriptSystem
ScriptSystemDescription + codec
ScriptBackend abstraction
ScriptEndpointBridge
CppStatic
Lua
Native
```

目标拆成两个 ownership：

## 21.1 Concrete System

```text
simulation/systems/script/
  ScriptSystem.hpp
  ScriptSystem.cpp
  ScriptSystemDescription.hpp
  ScriptSystemDescription.cpp
  ScriptSystemDescriptionCodec.hpp
  ScriptSystemDescriptionCodec.cpp
  System-specific private runtime/link detail
```

public include 建议：

```cpp
#include <lux/engine/simulation/systems/ScriptSystem.hpp>
#include <lux/engine/simulation/systems/ScriptSystemDescription.hpp>
```

## 21.2 Script execution/backend mechanism

```text
simulation/scripting/
  ScriptBackend.hpp
  ScriptBehavior.hpp / equivalent host seam
  cpp_static/
  lua/
  native/
```

public include：

```cpp
#include <lux/engine/simulation/scripting/ScriptBackend.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
```

---

# 22. ScriptEndpointBridge 放哪里

判断规则：

- 如果它仅仅服务 `ScriptSystem` 连接 generic Hook/Event，作为 ScriptSystem implementation seam：放 `systems/script`，尽量 private。
- 如果它是外部 composition 创建 backend/endpoint descriptor 时必须使用的 public scripting API：放 `scripting/`。

不要因为当前它是 public header 就自动保持 public。

先搜索真实 consumers；若只有 ScriptSystem/test 使用，应缩成 private implementation header。

---

# 23. Built-in System 的用户路径必须统一

最终用户应能自然发现：

```cpp
#include <lux/engine/simulation/systems/TransformSystem.hpp>
#include <lux/engine/simulation/systems/ScriptSystem.hpp>
#include <lux/engine/simulation/systems/PhysicsSystem.hpp>       // future
#include <lux/engine/simulation/systems/AnimationSystem.hpp>     // future
```

而不是：

```text
simulation/ecs/transform/TransformSystem
simulation/script/ScriptSystem
simulation/physics/runtime/PhysicsSystem
```

注意：这是 public include organization，不要求所有 implementation `.cpp` 平铺在同一个 source folder。

---

# 24. 不要新增 BuiltinSystem 总注册器

目录统一不意味着创建：

```text
BuiltInSystemRegistry
BuiltInSystemCatalog
DefaultSystemFactory
SystemManager
```

现有 `SystemRegistry` 已经是 canonical runtime owner。

新的 `systems/` 只是代码组织。

---

# 25. Exception Policy：最终工程规则

项目必须冻结：

> **Lux semantic error model 不使用 C++ exception。**

同时：

> **编译器层面暂时保留 exception support，用于承接 STL / foreign library 的异常，并在少数边界转换成 explicit Lux error。**

两者不矛盾。

---

# 26. Public API 规则

Runtime/domain public API 默认：

```cpp
noexcept
+
expected<T, Error> / enum error / explicit result
```

禁止：

```cpp
throw InvalidEntity{};
throw MissingAsset{};
throw ScriptError{};
```

禁止 exception 穿越：

```text
DLL/shared-library boundary
System callback boundary
Task callback boundary
Script ABI boundary
plugin boundary
```

---

# 27. Production code 禁止主动 throw

Lux-owned production code中：

```cpp
throw ...;
```

默认禁止。

业务/状态失败统一：

```cpp
return unexpected(Error::...);
```

或：

```cpp
return EError::...;
```

断言只用于 programmer invariant，不用于 recoverable runtime failure。

---

# 28. `catch(std::bad_alloc)` 为什么暂时允许

只要使用：

```text
std::vector
std::string
std::unordered_map
std::make_unique
```

默认 allocation failure 就通过 `std::bad_alloc` 报告。

如果 public API 又要求：

```cpp
noexcept
expected<..., ALLOCATION_FAILURE>
```

就需要在某个 cold boundary 转换：

```cpp
try
{
    ... STL allocations ...
}
catch (const std::bad_alloc&)
{
    return unexpected(Error::ALLOCATION_FAILURE);
}
```

这是 exception containment，不是 exception-driven control flow。

---

# 29. 允许出现 try/catch 的位置

允许：

```text
Builder finalization
Codec encode/decode
cold object construction/factory
asset/tool import boundary
Toolchain compiler top-level entry
foreign/plugin containment boundary
```

主要用于：

```text
std::bad_alloc
foreign library exception
user/plugin constructor exception（仅明确支持时）
```

---

# 30. 禁止出现 try/catch 的位置

禁止在 normal hot path 出现：

```text
Hook dispatch
Event record
Event drain
System update/task execution
ECS query/update loop
Script BoundScriptCall dispatch
Script attach/detach safe-point inner loop
render/physics/audio inner loops
```

hot runtime 必须通过预分配、nothrow operation、explicit status 工作。

---

# 31. `catch(...)` 政策

默认禁止：

```cpp
catch (...)
```

唯一例外是明确的 foreign containment boundary，例如：

```text
plugin entry
third-party compiler invocation
user supplied reflected constructor（若 contract 允许 throw）
```

且必须立即转换成 Lux error/diagnostic。

不得吞异常继续运行。

---

# 32. 不全局启用 `-fno-exceptions`

本轮禁止直接全局：

```text
-fno-exceptions
/EHs-
```

原因：

- STL allocation failure 语义仍然依赖 exceptions。
- MLIR/LLVM/第三方库不一定满足 no-exception contract。
- 为了去异常重写 `vector/string/map` 会违反“不重复造轮子”。

长期目标可以是：

```text
runtime/domain targets
    → 证明 no throwing dependency 后逐步 no-exception compile

toolchain/editor
    → 保留 foreign exception containment
```

但不是本次实施范围。

---

# 33. 构造函数失败策略

对于可能失败的 Runtime object：

避免：

```cpp
Foo(...); // 内部可能 throw / abort
```

优先：

```cpp
static expected<Foo, EFooError> create(...) noexcept;
```

普通构造函数只承担：

```text
trivial/nothrow initialization
move ownership
already validated state
```

这尤其适用于：

```text
SystemRegistry-owned systems
script backend
runtime pools
resource/session objects
```

---

# 34. Toolchain exception 边界

FlowForge MLIR/AOT compiler 可以依赖会抛异常的第三方实现，但 Lux-facing API 必须：

```cpp
expected<Artifact, CompileError> compile(...) noexcept;
```

或项目既有 equivalent result type。

第三方 exception 必须在 toolchain implementation 顶层 containment：

```text
MLIR/LLVM/foreign library
        ↓
catch boundary
        ↓
FlowForge diagnostic / CompileError
```

Editor 不应该写 `try/catch` 来包 compiler。

---

# 35. Target classification 修正

## Node Graph Editor

当前 `graph_kit` 被分类为 AUTHORING + EDITOR，职责不准确。

新 target 必须属于 canonical L5 Editor tool tier。

如果现有 classifier 对 L5 只使用 `TOOLCHAIN` layer + `PRODUCT EDITOR`，沿用已有枚举；不要为了目录名字另加一套 layer enum，除非全项目 layer taxonomy 同步更新。

关键约束：

```text
node_graph_editor 不能是 AUTHORING layer
```

## FlowForge Model

```text
modules/function/flowforge
→ L0 FUNCTION
```

## FlowForge Compiler

```text
engine/tools/toolchain/flowforge
→ L5 TOOLCHAIN
```

不能根据 build profile 动态改变 layer identity。

---

# 36. CMake dependency gates

建议增加/更新 architecture checks：

## 36.1 L0 FlowForge 禁止上层 include

扫描 `modules/function/flowforge` production sources，禁止：

```text
lux/engine/world/
lux/engine/simulation/
lux/engine/process/
lux/engine/scene/
lux/engine/authoring/
lux/engine/editor/
lux/engine/toolchain/
```

## 36.2 Node Graph Editor 禁止 domain dependencies

允许：

```text
function::ui
imgui-node-editor
core UI-neutral utility
```

禁止：

```text
FlowForge model direct ownership
Material graph ownership
Simulation
World
```

领域接入只能通过 adapter/interfaces。

## 36.3 FlowForge compiler 禁止 Editor dependency

禁止：

```text
imgui
node_editor
GraphEditor
IGraphView editor implementation
```

## 36.4 Runtime exception gate

至少增加简单 gate：

- `engine/domain` production code 禁止主动 `throw`。
- Hook/Event/System execution hot files禁止 `catch`。
- `catch(...)` runtime 默认禁止。

不要写复杂脆弱的 CMake parser；初期只做明确 token gate + small allowlist。

---

# 37. 旧目录与命名的删除清单

最终删除：

```text
engine/graph_kit/
engine/flowforge/
engine/world/              # move 后旧 root
engine/simulation/         # move 后旧 root
```

取代为：

```text
engine/domain/world/
engine/domain/simulation/
engine/tools/editor/node_graph/
engine/tools/toolchain/flowforge/
modules/function/flowforge/
```

禁止：

```text
old path forwarding header
old target alias
old namespace alias
symlink
compatibility CMake package
```

---

# 38. FlowForge 类型删除/保留表

| Current concept | Action |
|---|---|
| `FlowGraph` | 保留，净化后移到 L0 |
| FlowForge Nodes | 保留，移到 L0 |
| `NodeRegistry` | 若为 semantic node registry，保留 L0；若含 Editor-only entries，拆掉 Editor 部分 |
| `StateLayout` | 保留 L0 semantic calculation |
| `NodeUserDataPtr` in FlowGraph | 删除，移交 Editor sidecar |
| `ExportMethodNode` | 若只描述 export semantics，可保留/并入 L0 export model |
| `BindingEdge` to `ScriptBindingTarget` | 删除 |
| `flowforge_script` target | 若仅承担 Simulation binding graph，则删除 |
| `FlowForgeScriptArtifact::binding_template` | 删除 |
| compiler dependency on `SimulationDescription` | 删除 |
| compiler dependency on `ScriptSystemDescription` | 删除 |

---

# 39. Node Graph Editor 类型删除/保留表

| Current | Target |
|---|---|
| `GraphEditor` | 保留 |
| `GraphCommandStack` | 保留 |
| `GraphLayout` | 保留 |
| `IGraphView` | 保留 |
| `IGraphSchema` | 保留 |
| `PinIdBimap` | private 保留 |
| `graph_kit` module name | 删除 |
| `lux::graphkit` namespace | 删除/迁移为 editor node_graph namespace |

不要新增：

```text
NodeGraphContext
NodeGraphManager
NodeGraphDocument
GraphEditorService
```

除非现有代码已经存在明确独立 ownership。

---

# 40. Simulation 文件迁移映射

建议映射：

```text
engine/simulation/
→ engine/domain/simulation/
```

内部：

```text
simulation/system/
→ simulation/system/                         # generic mechanism，基本机械移动

simulation/ecs/core/
→ simulation/ecs/core/

simulation/ecs/schema/
→ simulation/ecs/schema/

simulation/ecs/snapshot/
→ simulation/ecs/snapshot/

simulation/ecs/task/
→ simulation/ecs/task/

simulation/ecs/transform/Transform*
→ simulation/ecs/transform/Transform*

simulation/ecs/transform/TransformSystem*
→ simulation/systems/transform/TransformSystem*

simulation/script/ScriptSystem*
→ simulation/systems/script/ScriptSystem*

simulation/script/ScriptSystemDescription*
→ simulation/systems/script/ScriptSystemDescription*

simulation/script/{lua,native,cpp_static}
→ simulation/scripting/{lua,native,cpp_static}

simulation/script/ScriptBackend.hpp
→ simulation/scripting/ScriptBackend.hpp
```

`ScriptEndpointBridge` 按第 22 节 consumer audit 后决定。

---

# 41. World 文件迁移

当前：

```text
engine/world/
```

机械移动：

```text
engine/domain/world/
```

public include/namespace 默认不改：

```cpp
#include <lux/engine/world/...>
namespace lux::world
```

不要为了物理目录改成 `lux::domain::world`。

---

# 42. public include 路径策略

分两类处理。

## 42.1 纯 layer physical move

例如：

```text
engine/world → engine/domain/world
engine/simulation → engine/domain/simulation
```

public include 不改。

## 42.2 概念本身命名错误

例如：

```text
graph_kit → node_graph editor
simulation/script backend → simulation/scripting
concrete systems → simulation/systems
```

public include 应同步修正，不保留 forwarding header。

---

# 43. namespace 策略

建议：

```cpp
lux::world
lux::simulation
lux::flowforge
```

保持。

Node editor 新 namespace：

```cpp
lux::editor::node_graph
```

Script backend 可继续：

```cpp
lux::simulation::script
```

或者迁移到：

```cpp
lux::simulation::scripting
```

这里不要仅因为 source folder 改名就自动改所有 semantic namespace。

建议优先保留既有 `lux::simulation::script`，除非项目决定 `script` 这个 namespace 本身也存在语义混淆。

目录可以叫 `scripting/`，namespace 仍可叫 `script`。

---

# 44. 不要把 layer 名写进 target/API 名

禁止：

```text
lux_l1_simulation
lux_l5_node_editor
lux_domain_world
L0FlowForge
```

使用概念名：

```text
simulation
world
flowforge
node_graph_editor
flowforge_compiler
```

层次由 source topology 和 build classification 表达。

---

# 45. 实施阶段

## Phase A — 建立新 source roots

创建必要 root：

```text
engine/domain/
engine/tools/
engine/tools/editor/
engine/tools/toolchain/
modules/function/flowforge/
```

只创建有实际内容的目录。

不创建 `process/scene/host` 空 placeholder。

---

## Phase B — Node Graph Editor move

1. `graph_kit` 改名 `node_graph_editor`。
2. 移到 `engine/tools/editor/node_graph`。
3. 调整 include/namespace/target/install package。
4. 更新 consumers。
5. 删除旧 root 和旧 API。
6. 确认 runtime/toolchain build 不链接 `imgui-node-editor`。

---

## Phase C — FlowForge model purification

1. 从 `FlowGraph` 删除 `NodeUserDataPtr` ownership。
2. 审计 FlowForge graph/model 对 Editor、Simulation 的 include。
3. 删除 `BindingEdge -> ScriptBindingTarget`。
4. 把 export semantics 与 Simulation binding 解耦。
5. 确保 graph/model 只依赖 L0。
6. 移到 `modules/function/flowforge`。

---

## Phase D — FlowForge compiler move

1. 将 compiler/dialect/AOT 移到 `engine/tools/toolchain/flowforge`。
2. 删除 `SimulationDescription` 输入。
3. 删除 `ScriptSystemDescription` 输入/输出。
4. 删除 `binding_template`。
5. 编译结果只描述 executable/resource artifact。
6. 保证 headless Toolchain profile 可以独立构建 compiler。

---

## Phase E — domain move

机械移动：

```text
engine/world      → engine/domain/world
engine/simulation → engine/domain/simulation
```

先不改 public include。

更新根 CMake 和 architecture checks。

---

## Phase F — Simulation internal reorg

1. 建 `simulation/systems`。
2. `TransformSystem` 移入 `systems/transform`。
3. `ScriptSystem` 移入 `systems/script`。
4. hierarchy 按真实 role 分类，不强制造新 System。
5. backend machinery 移 `scripting`。
6. public built-in System include 路径统一。

---

## Phase G — Exception policy cleanup

1. 搜索 active production `throw`。
2. semantic/domain error 改 explicit result。
3. 将 `catch(std::bad_alloc)` 收缩到 cold allocation boundaries。
4. 删除 hot path try/catch。
5. 审计 `catch(...)`，只保留明确 foreign boundaries。
6. Toolchain 顶层转换第三方 exception 为 diagnostics/error。
7. 增加 architecture checks。

---

# 46. Commit strategy

不要一个提交同时包含所有 semantic change + thousands of path moves，难以 review。

建议 commit 序列：

```text
1. refactor(flowforge): remove simulation binding ownership
2. refactor(flowforge): purify L0 graph model
3. refactor(editor): rename graph kit to node graph editor
4. refactor(topology): move world and simulation under domain
5. refactor(simulation): group concrete built-in systems
6. refactor(flowforge): move compiler under toolchain
7. refactor(flowforge): move graph model to modules/function
8. refactor(errors): enforce exception containment policy
9. chore(architecture): update gates/install consumers/docs
```

如果与 Script scale closure 同一个 PR 合并，仍建议保留上述 logical commits。

---

# 47. 验收：Node Graph Editor

必须满足：

```text
[ ] source root = engine/tools/editor/node_graph
[ ] no engine/graph_kit files
[ ] module/target no longer named graph_kit
[ ] PRODUCT = EDITOR
[ ] not classified as AUTHORING
[ ] links UI + node_editor only
[ ] no FlowForge/MaterialGraph ownership
[ ] FlowForge can adapt through IGraphView/IGraphSchema
[ ] runtime profile links zero node_editor
```

---

# 48. 验收：FlowForge L0

```text
[ ] model resides modules/function/flowforge
[ ] no Simulation includes
[ ] no Editor includes
[ ] no Toolchain includes
[ ] no NodeUserDataPtr editor sidecar in FlowGraph
[ ] FlowGraph remains canonical semantic graph; no duplicate Description model
[ ] stable node/function/variable identity preserved
[ ] RuntimeObject only used as L0 in-memory value holder, not wire identity
```

---

# 49. 验收：FlowForge compiler

```text
[ ] compiler resides engine/tools/toolchain/flowforge
[ ] compiler does not include SimulationDescription
[ ] compiler does not include ScriptSystemDescription
[ ] compiler does not know ScriptBindingTarget
[ ] compiler artifact has no binding_template
[ ] Simulation/Entity scope not compiler input
[ ] headless Toolchain build works without Editor/node_editor
[ ] Editor may call compiler but compiler does not call Editor
```

---

# 50. 验收：Simulation topology

```text
[ ] engine/domain/simulation is physical L1 root
[ ] generic System mechanism under simulation/system
[ ] concrete built-in Systems under simulation/systems
[ ] TransformSystem no longer hidden under ecs/transform
[ ] ScriptSystem under systems/script
[ ] ECS directories contain mechanism/data, not arbitrary Systems
[ ] script language/backend mechanism separated from ScriptSystem source ownership
[ ] public System include path is coherent
[ ] no BuiltinSystemManager/Catalog introduced
```

---

# 51. 验收：World / layer visibility

```text
[ ] World and Simulation visibly grouped under engine/domain
[ ] tools/editor and tools/toolchain visibly separated
[ ] authoring remains its own layer root
[ ] no L0_/L1_/L5_ prefixes
[ ] public namespace not polluted by physical layer names
[ ] no empty placeholder layer directories required
```

---

# 52. 验收：Exception policy

```text
[ ] Lux runtime semantic code does not actively throw
[ ] public runtime failures use explicit result/error
[ ] no exception crosses System/Task/Script ABI/module boundary
[ ] Hook/Event/System hot paths contain no try/catch
[ ] catch(std::bad_alloc) exists only at justified cold allocation boundaries
[ ] catch(...) exists only at explicit foreign containment boundaries
[ ] Toolchain converts foreign failures into compiler diagnostics/error
[ ] global -fno-exceptions is NOT introduced in this migration
```

---

# 53. LLM 实施红线

实施方不得因为本文增加以下类型：

```text
LayerManager
DomainManager
ToolManager
BuiltinSystemManager
BuiltinSystemCatalog
GraphKitCompatibility
NodeGraphContext
NodeGraphService
FlowForgeDescription2
RuntimeFlowGraph
AuthoringFlowGraph
FlowForgeBindingRegistry
FlowForgeSimulationAdapterRegistry
CompilerContextService
ExceptionManager
ErrorRouter
```

不得增加：

```text
旧目录 forwarding headers
旧 target aliases
旧 namespace aliases
双写 FlowForge graph representations
GraphEditor 对 FlowForge 的直接依赖
FlowForge L0 对 Simulation 的反向依赖
```

遇到不确定归属时使用以下判断：

```text
是否没有上层也成立？
  yes → 可以考虑下沉
  no  → 留在拥有该语义的更高层

是否已经有 canonical owner/type？
  yes → 扩展/移动现有 owner，不创建第二套

是否只是为了转发调用？
  yes → 不创建 facade/manager
```

---

# 54. 最终依赖图

```text
                           ┌─────────────────────┐
                           │       Host          │
                           └─────────┬───────────┘
                                     │
                           ┌─────────▼───────────┐
                           │       Scene         │
                           └─────────┬───────────┘
                                     │
                           ┌─────────▼───────────┐
                           │      Process        │
                           └─────────┬───────────┘
                                     │
             ┌───────────────────────▼──────────────────────┐
             │                    Domain                    │
             │              World + Simulation             │
             └───────────────────────┬──────────────────────┘
                                     │
                                     ▼
┌────────────────────────────────────────────────────────────────────┐
│                              L0 Modules                            │
│ Platform | Core | Resource | Function                            │
│                                   └── FlowForge semantic model    │
└────────────────────────────────────────────────────────────────────┘

L5 tools:

Node Graph Editor ───────────────┐
                                 │ adapter
                                 ▼
                         FlowForge semantic model

FlowForge Compiler ──────────────► FlowForge semantic model
        │
        └── produces Script/executable artifact

Script authoring / SimulationDescription
        └── independently binds ScriptSymbolId → System Hook/Event
```

---

# 55. 最终冻结语句

本轮实施完成后，应满足以下架构陈述：

> `modules/function/flowforge` 定义 FlowForge 本身是什么；它不知道 Editor，也不知道 Simulation。

> `engine/tools/toolchain/flowforge` 负责把 FlowForge 编译成 executable/resource artifact；它不是 Editor module。

> `engine/tools/editor/node_graph` 是通用节点图编辑器；它不知道 FlowForge 或 Material Graph 的领域语义。

> `engine/domain` 使 World/Simulation 的 L1 ownership 在 source tree 中一眼可见，而 public API 仍使用 `lux::world` / `lux::simulation`。

> `simulation/system` 定义 System mechanism；`simulation/systems` 收纳 concrete built-in Systems；`simulation/ecs` 只表达 ECS mechanism/data。

> Lux semantic code 不使用 exception 作为控制流；异常只作为 STL/foreign implementation detail，在少量 containment boundary 转换为 explicit errors。

这套整理应与 Script/Hook/Event scale closure 一起形成下一次仓库审阅的基线。
