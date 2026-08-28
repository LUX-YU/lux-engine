# Lux Engine Script / FlowForge 概念压缩、拓扑收敛与性能闭环实施规范

**状态：Normative / Implementation Freeze**  
**基线：`main @ 25809082edf0e21a94409b22de2c75bf31d31697`**  
**适用范围：Script Core、Script Artifact、Simulation Scripting、ScriptSystem、HookPoint/EventPoint、Script Authoring、Lua Toolchain、FlowForge、Node Graph Editor、相关 CMake/安装拓扑与架构门禁。**

---

## 0. 文档定位

本文件整合此前多轮 ScriptSystem / HookPoint / EventPoint / FlowForge 审阅结论，以及当前仓库的目录、命名、类型冗余审计。

本文件不是新架构提案，而是**现有正确方向的收口实施规范**。实施目标不是增加更多抽象，而是：

1. 保留已经正确且高性能的 runtime 机制；
2. 删除重复 representation、dead API、单字段 options bag、无生产价值 wrapper；
3. 让物理目录与真实职责一一对应；
4. 将 Script cooked content 从 generic resource 层收回 Script domain；
5. 将 FlowForge 编译链压缩为一个清晰的 compiler；
6. 消灭会随场景规模产生全局扫描或二次方增长的算法；
7. 禁止实施方为了迁移创建 compatibility layer、Manager、Catalog、Context、Facade 等第二套结构。

若本文件与更早的 Script / FlowForge 重构文档在本文覆盖的事项上冲突，以本文为准。

---

# 1. 不可改变的架构不变量

## 1.1 HookPoint 与 EventPoint 是两个正交语义

```text
固定执行位置 reached here
        -> HookPoint

离散事实 something happened
        -> EventPoint
```

HookPoint 不是 EventPoint 的特殊写法，EventPoint 也不是带 payload 的 HookPoint。

典型语义：

```text
Render.BeforeRender      -> HookPoint
Physics.BeforeStep       -> HookPoint
Animation.AfterPose      -> HookPoint

Physics.Collision        -> EventPoint
Trigger.Enter            -> EventPoint
Animation.Notify         -> EventPoint
Input.Action             -> EventPoint
```

不得重新引入 `UniversalTriggerPoint`、`ScriptTrigger` 或其它把两种语义折叠为一个 public abstraction 的类型。

---

## 1.2 Binding 没有 Entity；Instance 可以有 Entity scope

冻结规则：

```text
ScriptBinding
    = ScriptSymbolId + Hook/Event target

ScriptInstance
    = Simulation scope
      OR
      Entity scope(self)
```

即：

> **绑定没有 Entity；实例可以属于 Entity。HookPoint 永远不按 Entity 路由，`self` 由已经绑定好的 ScriptInstance scope 决定。**

不得恢复：

```text
HookScriptTarget + Entity
ScriptBinding.entity
dispatchHook(hook, entity, ...)
null Entity == global
```

---

## 1.3 HookPoint 必须支持高 fan-out

不得再以“HookPoint 通常只有几个 subscriber”为实现前提。

复杂度合同：

```text
connect                  O(1) expected/amortized
disconnect(token)        O(1)
lookup token             O(1)
dispatch                  O(F)
```

其中 `F` 是**实际需要调用的 handler 数量**。

`dispatch` 不可能对 F 个真实 callback 做到总 O(1)；正确目标是：

> **除真实 callback 数量之外，不允许再出现与全局 Entity 数、全局 binding 数、全局 endpoint 数相关的额外扫描。**

当前 `SlotMap + dense values()` 方向正确，必须保留。

---

## 1.4 Targeted Event 必须是 output-sensitive

Entity-targeted Event 的合同：

```text
target lookup            O(1)
dispatch                 O(A + K)
```

其中：

- `A` = match-all subscriber 数；
- `K` = 当前 target 的 subscriber 数。

禁止：

```text
for each event occurrence
    for each event handler
        if target matches ...
```

当前 sparse target index + per-target bucket 的方向必须保留。

---

## 1.5 ScriptSystem 不得扫描全场景寻找目标实例

禁止所有类似：

```text
event(Entity X)
    -> scan all ScriptInstances
    -> scan all ScriptBindings
    -> compare entity
```

正确路径：

```text
target Entity
    -> target index
    -> exact handler bucket
    -> exact ScriptInstance
```

---

# 2. 当前基线已经闭环的内容：只允许保留或强化，不允许回退

以下是前几轮审阅曾发现、当前基线已经整改的项目。新重构不得再次“修复”为旧实现。

## 2.1 EventPoint drain 重入

当前 `EventOccurrenceBuffer::prepare()` 在 `draining_` 时返回 `DISPATCH_ACTIVE`。

必须保留：

```text
callback -> event.prepare()
    => DISPATCH_ACTIVE
    => 不清空/resize drain 正在遍历的 storage
```

不得为了简化 EventPoint 删除这一保护。

---

## 2.2 HookPoint stable registration

当前 HookPoint 使用 SlotMap：

```text
EndpointConnectionToken
    -> slot + generation
    -> SlotMap erase
```

这解决了高 fan-out 下 O(N) disconnect 的问题。

禁止退回：

```text
std::vector<Handler>
erase(remove_if(...))
```

---

## 2.3 Targeted Event sparse target index

当前 targeted EventPoint 已具备：

```text
SlotMap<Handler>
entt::basic_sparse_set<Entity> target_index
TargetBucket
```

必须保留 O(1) target lookup。

---

## 2.4 ScriptSystem per-binding registration handle

当前 `RuntimeBinding` 已保存：

```cpp
HandlerKey registration;
```

detach 一个 mount 时，只遍历**该 mount 自己的 bindings**，然后精确 erase registration。

必须维持：

```text
detach mount
    O(binding_count_of_mount)
```

禁止恢复：

```text
for every HookBucket
    erase-remove all handlers

for every EventBucket
    erase-remove all handlers
```

---

## 2.5 Dirty queue membership 已闭环

当前 `SparseMountQueue` 使用：

```text
values[]
present[]
```

dirty insert 为 O(1)，不再做 `std::find` 去重。

必须保留。

---

## 2.6 ScriptAttachment ownership 已闭环

当前 ScriptSystem 有：

```text
ownsAttachment()
projectAttachment()
removeOwnedAttachment()
```

并在 release/rollback/shutdown 路径移除自己创建的 runtime projection。

不得恢复持久化 `ScriptComponent`，也不得让 ScriptAttachment 变成 authored state。

---

## 2.7 Shutdown endpoint detach 已改为可失败合同

当前 `ScriptSystem::shutdown()` 在 `disconnectEndpoints()` 失败时停止 teardown，而不是继续销毁 bucket。

必须保持：

```text
detach endpoint failure
    => shutdown failure
    => bucket / lane context 仍保持存活
```

禁止忽略 disconnect/flush 结果。

---

## 2.8 Lua structured record payload 已有 narrow marshaller seam

当前 Lua backend 已支持 `LuaRecordMarshaller`，record argument 不再被固定限制为 scalar。

这一 seam 可以继续演进，但禁止恢复 global SemanticCatalog / TypeCatalog。

---

## 2.9 Start/Stop 半成品 backend contract 已删除

当前 `ScriptBackendDescriptor` 只保留：

```text
createInstance
prepareMethod
releaseMethod
destroyInstance
```

不要在这轮目录/类型重构里重新加入没有实际 backend 实现的 Start/Stop callback。

生命周期若以后真正需要，必须作为独立需求重新设计并让所有 shipped backend 同步实现。

---

# 3. 物理拓扑规则：Package 与 Collection 必须二选一

这是本轮最重要的目录规则。

## 3.1 Package

一个真正的 build/package root 可以拥有：

```text
CMakeLists.txt
include/
pinclude/        # 可选
src/
test/            # 可选
```

它可以在 `src/` 内有内部目录。

但一个 package root **不得同时在同级再挂一批独立 CMake 子包**。

---

## 3.2 Collection

一个 collection root 只负责聚合多个 package：

```text
CMakeLists.txt
child_a/
child_b/
child_c/
```

collection root **不得再拥有自己的**：

```text
include/
src/
pinclude/
test/
```

否则它既是 package 又是 collection，物理职责不纯。

---

## 3.3 当前最典型的违规点

当前：

```text
engine/domain/simulation/scripting/
  include/
  src/
  cpp_static/
  lua/
  native/
```

这是明确违规。

最终必须改为：

```text
engine/domain/simulation/scripting/
  CMakeLists.txt           # collection only

  core/
    CMakeLists.txt
    include/
    src/
    test/

  cpp_static/
    CMakeLists.txt
    include/
    src/
    test/

  lua/
    CMakeLists.txt
    include/
    src/
    test/

  native/
    CMakeLists.txt
    include/
    src/
    test/
```

不是把几个 backend 塞回一个巨型 module；它们确实有不同第三方依赖和构建条件，因此可以是独立 package。

问题在于父目录不能同时也是一个 package。

---

# 4. 最终顶层目录

`engine/tools` 删除。

最终：

```text
modules/
  core/
  resource/
  function/

engine/
  domain/
  authoring/
  editor/
  toolchain/
```

职责：

```text
modules/core
    最低层通用机制

modules/resource
    generic asset/storage + persistent resource descriptions

modules/function
    可复用功能域；不依赖 editor/toolchain

engine/domain
    runtime/domain integration

engine/authoring
    authored-data mutation、诊断、stable authoring state

engine/editor
    interactive UI/editor tooling

engine/toolchain
    offline compiler/cook/package/build tooling
```

`tools/toolchain` 是同义叠加，必须消失。

---

# 5. 最终目标拓扑

```text
modules/
  resource/
    asset/                         # generic only
    description/
      ... 
      Script.hpp

  function/
    script/                        # collection only
      core/
      artifact/
      lua/
      native/

    flowforge/                     # FlowForge graph/model itself
      include/
      src/
      test/

engine/
  domain/
    simulation/
      scripting/                   # collection only
        core/
        cpp_static/
        lua/
        native/

      systems/
        script/
        transform/
        ...

  authoring/
    script/

  editor/
    node_graph/

  toolchain/
    flowforge/
    lua/
```

---

# 6. 必须执行的目录迁移

| 当前 | 最终 |
|---|---|
| `engine/tools/editor/node_graph` | `engine/editor/node_graph` |
| `engine/tools/toolchain/flowforge` | `engine/toolchain/flowforge` |
| `engine/tools/toolchain/script/lua` | `engine/toolchain/lua` |
| `engine/domain/simulation/scripting/include` | `engine/domain/simulation/scripting/core/include` |
| `engine/domain/simulation/scripting/src` | `engine/domain/simulation/scripting/core/src` |
| `modules/resource/asset/script` | `modules/function/script/artifact` |

完成后删除整个：

```text
engine/tools/
engine/toolchain/script/
modules/resource/asset/script/
```

不得保留 forwarding CMakeLists、compatibility header 或 ALIAS target。

---

# 7. Asset / Artifact 规则

## 7.1 generic resource 层不拥有 domain-specific XxxAsset 类型

`modules/resource/asset` 只负责：

```text
AssetId
AssetTypeId
AssetCodecDescriptor / AssetCodecSet
CookedAssetImage
AssetProvider
AssetVfs
Pak
storage
```

它不应该变成：

```text
resource/asset/script
resource/asset/texture
resource/asset/material
resource/asset/mesh
...
```

---

## 7.2 Script cooked content 改为 ScriptArtifact

当前：

```cpp
ScriptAssetContent {
    rdesc::Script description;
    vector<byte> payload;
}
```

本质是 Script domain 的 cooked artifact，而不是 runtime `Asset` 对象。

最终：

```cpp
namespace lux::script
{
    class/struct ScriptArtifact final
    {
        rdesc::Script description;
        std::vector<std::byte> payload;
        // 可带 non-persistent runtime lookup index，见性能章节。
    };
}
```

对应命名同步：

```text
ScriptAssetContent
    -> ScriptArtifact

ScriptAssetCanonicalName
    -> ScriptArtifactCanonicalName

ScriptAssetPrimaryMagic
    -> ScriptArtifactPrimaryMagic

scriptAssetCodecDescriptor()
    -> scriptArtifactCodecDescriptor()
```

不保留旧 alias。

---

## 7.3 不恢复所有 XxxAsset

以后 Mesh / Material / Texture 等是否需要专用 cooked representation，由各自 domain 决定。

正确范式类似：

```text
ClassicMeshBatch
MaterialGraphArtifact
ScriptArtifact
```

而不是为了形式统一恢复：

```text
MeshAsset
MaterialAsset
TextureAsset
ScriptAsset
...
```

只有当某个类型真的表达独立 ownership/lifetime/serialization contract 时才允许存在。

---

# 8. Script core 类型压缩

当前 Script core 有多层对 generic semantic / C ABI 的重复包装。

目标：

> **Script core 只保留 Script-specific identity 和真正 hot-path executable handle。**

---

## 8.1 保留

```text
ScriptSymbolId
InvalidScriptSymbolId

BoundScriptCall

lux_script_abi.h
```

`BoundScriptCall` 的两个字段虽然很少，但它表达一个真实且高频的概念：

```text
prepared executable invocation = invoke + context
```

保留。

---

## 8.2 删除 ScriptCallFrame.hpp

删除：

```text
ScriptCallFrame.hpp
lux::script::CallFrame
```

生产代码直接使用 C ABI frame 或自己的 typed bridge。

当前该 wrapper 的主要 consumer 是 contract/install test；测试本身不能成为保留 public API 的理由。

同时删除只为该 wrapper 存在的 install/contract assertions。

---

## 8.3 删除 ScriptValue.hpp

删除：

```text
ConstValueView
ValueView
make_const_view
make_view
read<T>
```

不得为这些 wrapper 再建新文件或新 namespace。

---

## 8.4 删除 ScriptSignature.hpp

删除第三套 signature representation：

```text
TypeDesc
FunctionSignature
```

系统内部只允许：

```text
lux::semantic::Type / SignatureView / Layout
```

持久化 artifact 允许：

```text
rdesc::ScriptValueType
rdesc::ScriptFunction
```

C ABI 边界允许：

```text
lux_script_type_desc
lux_script_function_desc
```

不得存在第四套 representation。

---

# 9. ScriptSemantic.hpp 大幅退役

当前已有 generic：

```text
lux::semantic::TypeId
lux::semantic::EValuePass
lux::semantic::EAbiKind
lux::semantic::Type
lux::semantic::SignatureView
lux::semantic::Layout
lux::semantic::TypeTraits<T>
lux::semantic::makeType<T>()
lux::semantic::builtinLayout()
lux::semantic::sameSignature()
```

因此删除 Script mirror：

```text
EScriptPassMode
ScriptSemanticType
ScriptFunctionSignatureView
ScriptSemanticLayout
ScriptSemanticTypeTraits<T>
scriptSemanticTypeId()
makeScriptSemanticType()
scriptBuiltinLayout()
sameScriptSignature()
ScriptBuiltinSemanticLayouts
```

所有调用点直接迁移到 `lux::semantic::*`。

---

## 9.1 ScriptSymbolId 单独保留

将 `ScriptSymbolId` 移到一个极小的 Script-specific header，例如：

```text
modules/function/script/core/include/.../ScriptSymbol.hpp
```

不要为了迁移再创建：

```text
ScriptSemanticCompat.hpp
ScriptSemanticLegacy.hpp
ScriptTypeFacade.hpp
```

---

## 9.2 SimulationStepInfo 双 Traits 删除

当前同时存在：

```text
semantic::TypeTraits<SimulationStepInfo>
script::ScriptSemanticTypeTraits<SimulationStepInfo>
```

最终只允许：

```text
semantic::TypeTraits<SimulationStepInfo>
```

---

# 10. `rdesc::ScriptValueType` 不删除

它与 `semantic::Type` 不是重复类型。

区别：

```text
semantic::Type
    non-owning
    in-process semantic view

rdesc::ScriptValueType
    owning/persistent
    cooked artifact description
    carries ABI layout/pass data
```

这是一条真实 persistence boundary，因此保留。

不要为了“统一”让 persistent description 持有 `string_view`。

---

# 11. ScriptBackend.hpp 职责收缩

`ScriptBackend.hpp` 只应该定义 backend contract。

当前不应继续放在这里的：

```text
ResolvedScriptAsset
ResidentScriptResolver
ScriptWorldResolver
```

它们是 ScriptSystem composition/integration dependency，不是 backend contract。

迁移到 ScriptSystem 所在 package，并同步更名：

```text
ResolvedScriptAsset
    -> ResolvedScriptArtifact

ResidentScriptResolver
    -> ScriptArtifactResolver

ScriptWorldResolver
    -> WorldObjectResolver
```

这些 callback-pair 类型有真实 dependency-injection 价值，可以保留；不要进一步包成：

```text
ScriptSystemContext
ScriptSystemServices
ScriptRuntimeEnvironment
```

---

## 11.1 删除 `ScriptInstanceCreateContext::mount`

当前生产 backend 不消费 `context.mount`。

删除：

```cpp
ScriptMountId mount;
```

结果应同时消除：

```text
scripting core
    -> ScriptSystemDescription.hpp
```

这一反向依赖。

正确修复是删 unused field，不是再抽一个 `ScriptMountId.hpp` 只为维持旧字段。

---

# 12. ScriptSystem public surface 压缩

## 12.1 删除单字段 `ScriptSystemOptions`

当前：

```cpp
struct ScriptSystemOptions {
    size_t failure_capacity;
};
```

没有独立语义。

删除，`create()` 直接接受：

```cpp
std::size_t failure_capacity
```

如果未来真的出现多个独立 tuning knob，再重新评估是否需要 options object。

不得因为“以后可能会扩展”保留现在没有价值的类型。

---

## 12.2 `ScriptSystemFailure` 保留

这是可观察 diagnostics，不是冗余 wrapper。

必须继续记录：

```text
error
mount
symbol
status
```

特别是 invocation failure 不得丢 `ScriptSymbolId`。

---

## 12.3 `ScriptSystemDescription::findMount()` 删除

如果全仓生产 consumer 仍为 0，则删除 public API 和只为它存在的测试。

不要为了让 `findMount()` O(1) 再创建 mount index；没有 consumer 的 API 最快的实现是不存在。

---

## 12.4 `ScriptSystemResult<T>` alias 删除

codec 中若只有局部用途，直接写：

```cpp
lux::cxx::expected<T, EScriptSystemDescriptionError>
```

不要给一次性 alias 建 public vocabulary。

---

# 13. Lua backend public surface 压缩

这些统计 getter 如果仍只有测试使用：

```text
loadedInstanceCount()
chunkLoadCount()
preparedReferenceCount()
cachedTracebackCount()
```

从 public production API 删除。

测试应优先通过 observable behavior 验证。

确有必要的内部计数可以：

- 放 private state；
- 通过 test-only compile seam 暴露；
- 不进入 installed public headers。

---

# 14. CppStatic 处理原则

本轮不机械删除 `CppStaticScriptDescriptor`。

它当前同时持有：

```text
projected rdesc::Script
stable descriptor key
reflected class/function pointers
callable mapping
attach function
```

这构成了真实的 prepared/lifetime object。

因此当前判断：

```text
CppStaticScriptDescriptor     KEEP
CppStaticScriptBackend        KEEP
```

但必须满足：

1. 不再使用 ScriptSemantic mirror；
2. 不创建第二个 `CppStaticScriptRegistration` / Catalog；
3. descriptor 只表示静态 executable contract，不承担 global registry 职责；
4. installed-consumer test 不能成为增加更多 API 的理由。

---

# 15. FlowForge：只允许一个 Compiler

当前 `flowforge_script_compiler` 与 `flowforge_compiler` 的职责切分对用户不可理解，必须消失。

最终 public toolchain package：

```text
engine/toolchain/flowforge/
  CMakeLists.txt

  include/
    lux/engine/flowforge/Compiler.hpp

  pinclude/      # 仅确有跨 TU 私有接口时
    ...

  src/
    Compiler.cpp
    mlir/
      ...

  test/
```

public target：

```text
flowforge_compiler
```

只保留一个。

---

## 15.1 统一 pipeline

最终逻辑：

```text
FlowGraph
   |
   v
validate / resolve stable symbols
   |
   v
lower to IR / MLIR
   |
   v
LLVM/native code generation
   |
   v
build rdesc::Script
   |
   v
ScriptArtifact
```

public compiler API 最终返回 Script domain 的 canonical artifact。

不得再暴露两种“最终结果”。

---

## 15.2 删除 `FlowForgeScriptArtifact`

删除：

```text
FlowForgeScriptArtifact
```

它与新的 `ScriptArtifact` 重复表达 cooked script artifact。

---

## 15.3 `FlowForgeAotAbiManifest` 下沉或删除

如果其字段最终都进入：

```text
rdesc::NativeModuleScript
rdesc::Script.exports
```

则直接删除。

如果 MLIR/AOT 中间阶段确实需要，必须保持 private implementation detail，不进入 public installed headers。

---

## 15.4 `AotArtifact` / IR / Passes 不作为产品 API

以下属于 compiler internals：

```text
AotArtifact
AotOptions
IR internals
Passes internals
MLIR dialect detail
TypeSizeMap
```

除非外部 consumer 有经过证明的需求，否则移动到 `pinclude/` 或 `src/`。

---

## 15.5 dialect target 不得成为 public 产品 target

TableGen / MLIR 构建若必须拆 helper target，可以存在：

```text
_flowforge_mlir
_flowforge_dialect
```

但必须：

```text
PRIVATE
not installed
not exported
not documented as consumer API
```

禁止继续：

```text
flowforge_compiler
flowforge_script_compiler
flowforge_compiler_dialect
```

三套 public 名词并存。

---

# 16. FlowForge Symbol 规则

Compiler 不得从 display name 临时推导新的 executable identity。

稳定身份必须来自 authored/cooked `ScriptSymbolId`。

禁止：

```text
event display name
    -> hash at compiler time
    -> executable symbol id
```

正确：

```text
authored ScriptSymbolId
    -> FlowForge graph export
    -> compiler
    -> ScriptArtifact
    -> backend
```

display name 只是人类可读 metadata。

---

# 17. Lua Toolchain：删除 generic `script/` 父目录

当前 Lua 工具做的是：

```text
Lua source
+ @lux.method
+ stable symbol ledger
+ semantic metadata
    ->
ScriptArtifact bytes
```

它不是 Symbol Importer。

最终：

```text
engine/toolchain/lua/
  package_lua_script.py
  test/
```

工具/target 命名：

```text
lua_script_packager
```

不要：

```text
importers/lua_script
symbol_importers/lua
toolchain/script/lua
lua_script_compiler
```

除非以后真的生成 Lua bytecode/native code，否则 `compiler` 也不是准确名称。

---

# 18. `engine/authoring/script` 保留

这里的 `script` 是合法职责，不需要为了“减少 script 字样”而硬改名。

它拥有：

```text
binding authoring
binding compatibility diagnostics
stable ScriptSymbolLedger
```

这些是真正 authoring-only 的职责。

约束：

```text
authoring/script
    may depend on persistent descriptions + SimulationDescription

authoring/script
    must NOT depend on FlowForge compiler
    must NOT depend on editor UI
    must NOT own runtime backend
```

`ScriptSymbolLedger` 保留，因为 stable authored identity 是真实状态，不是 runtime catalog。

---

# 19. Node Graph Editor

通用节点编辑器最终：

```text
engine/editor/node_graph
```

它的定位：

```text
generic node graph editing UI
```

可被：

```text
FlowForge editor
Material Graph editor
其它 node-based authoring
```

复用。

因此 `node_graph` 不得依赖 FlowForge。

依赖方向必须是：

```text
FlowForge editor integration
        ->
node_graph
```

而不是：

```text
node_graph
        ->
FlowForge
```

旧 `graph_kit` 名称不得恢复。

---

# 20. RuntimeObject：保留，但严格限制用途

`modules/core/meta/RuntimeObject` 有真实使用场景。

当前 FlowForge 的 `DataInPin` 使用它保存：

```text
任意 reflected pin 类型的默认常量
```

这是合理的 dynamic/reflection value holder 场景。

因此：

```text
RuntimeObject      KEEP
```

---

## 20.1 RuntimeObject 的允许区域

允许：

```text
meta/reflection utility
FlowForge graph authoring/model
editor property/default values
cold toolchain transformation
```

禁止：

```text
HookPoint hot dispatch
EventPoint occurrence storage
ScriptSystem handler routing
per-frame simulation component path
Script ABI call-frame replacement
```

不能因为已有 RuntimeObject，就用它替换 `lux_script_value_slot` 或 semantic typed endpoint。

---

## 20.2 RuntimeObject 不成为新的 UniversalValue

禁止派生：

```text
RuntimeValueManager
DynamicValueCatalog
ScriptRuntimeObject
EventRuntimeObject
```

它只是 reflection value holder。

---

## 20.3 RuntimeObject exception/allocation cleanup

在本轮允许做小范围质量修复：

1. heap allocation 使用真实 type alignment；
2. 不使用 `std::abort()` 处理普通 allocation failure；
3. `defaultOf()` 的 heap path 不应允许未捕获 `bad_alloc`；
4. 不让 std::string special case 成为新的 global registry dependency 模式；
5. 保持其不进入 simulation hot path。

不要把 RuntimeObject 重写成大型 Any/Variant framework。

---

# 21. Exception policy

目标不是全仓强制 `-fno-exceptions`。

LLVM、标准库和第三方代码可能仍需要异常支持。

Lux Engine 自己的 policy：

> **异常不是 engine control-flow protocol。**

---

## 21.1 禁止

engine-owned 代码禁止主动：

```cpp
throw ...
```

运行时 API 不用 exception 表达：

```text
not found
capacity exceeded
invalid input
backend failure
asset failure
```

这些继续用：

```text
expected
enum error
bool/result
```

---

## 21.2 允许的 try/catch

允许在 **cold allocation boundary**：

```cpp
try {
    vector.reserve(...)
    unordered_map.reserve(...)
}
catch (const std::bad_alloc&) {
    return ALLOCATION_FAILURE;
}
```

原因是标准容器 allocation API 本身通过异常报告失败。

这不是业务异常。

---

## 21.3 catch(...) 只允许 foreign boundary

例如：

```text
third-party callback
reflection constructor not guaranteed noexcept
LLVM/foreign API boundary
```

且必须转换成明确 error。

不得在普通内部逻辑中用：

```cpp
catch (...) {
    return false;
}
```

掩盖 invariant bug。

---

## 21.4 Hot path 零异常机制

以下路径必须：

```text
no allocation
no throw
no try/catch
```

包括：

```text
HookPoint::dispatch
EventPoint::record/drain
ScriptSystem lane dispatch
Script prepared call invocation
targeted event lookup
dirty queue insertion
mount handler detach
```

所有 capacity 在 prepare/materialization 阶段准备。

---

# 22. 性能合同：不仅 hot path，construction 也不能二次方

## 22.1 允许的复杂度

对 N 个输入构建 N 个 runtime object：

```text
O(N)
```

是不可避免且正确的。

不要求“处理 N 项整体 O(1)”。

真正要求：

> **单 key lookup / membership / registration mutation 使用 O(1)；枚举使用 output-sensitive O(K)；完整构建只允许线性 O(N)，禁止 accidental O(N²)。**

---

# 23. 当前 ScriptSystem 仍需消除的 prepare-time 扫描

当前 `ScriptSystem::State::buildLayout()` 仍存在：

```text
findHookEndpoint()   -> linear find
findEventEndpoint()  -> linear find
ensureHookBucket()   -> linear find
ensureEventBucket()  -> linear find
ensureMethod()       -> linear find within mount
findExport()         -> linear export scan
```

这些虽然不在 dispatch hot path，但在大量 scripted entities/mounts 下会累积成不可接受的 build/materialization 成本。

本轮必须闭环。

---

## 23.1 Endpoint resolution

prepare 时一次性创建 endpoint lookup index：

```text
(system id, hook id)   -> endpoint ordinal
(system id, event id)  -> endpoint ordinal
```

之后每个 binding：

```text
O(1) lookup
```

不要为 Hook/Event 分别再创建 `EndpointCatalog` 类。

可以直接是 `ScriptSystem::State` 内部 hash index。

优先复用仓库/第三方现成 hash container；不要为了这件事自己实现 hash table。

---

## 23.2 Bucket 不再 `ensure + scan`

直接让 endpoint ordinal 对应 bucket ordinal。

prepare 先：

```text
hooks.resize(hook_endpoint_count)
events.resize(event_endpoint_count)
```

binding 解析后直接：

```text
bucket_slot = endpoint_ordinal
```

无 `ensureHookBucket()` / `ensureEventBucket()` linear search。

无 binding 的 bucket 可以 capacity=0，不连接 endpoint lane。

---

## 23.3 Script export lookup O(1)

`ScriptArtifact` 在 decode/prepare 时建立 non-persistent：

```text
ScriptSymbolId -> export ordinal
```

同一个 artifact 被成千上万个 Entity mount 使用时，不得反复线性扫描 `description.exports`。

提供：

```cpp
const rdesc::ScriptFunction* findExport(ScriptSymbolId) const noexcept;
```

要求平均 O(1)。

这个 index 是 `ScriptArtifact` 的内部运行时辅助数据，不进入 wire format。

不要单独创建 public `ScriptExportIndex` 类型。

---

## 23.4 `ensureMethod()` 消除二次方

一个 mount 内多个 binding 可能重复引用同一 symbol。

prepare mount 时使用临时/internal symbol -> method-slot index，使该 mount 的 method preparation 总体 O(binding_count)。

不得：

```text
binding 0 -> scan 0
binding 1 -> scan 1
...
binding N -> scan N
```

---

# 24. ScriptSystemDescriptionBuilder 复杂度闭环

当前 `addMount()` 有：

```text
binding duplicate prefix scan
mount id any_of
WorldObjectId any_of
```

大量 mount 时为 O(N²)。

必须改为 builder-internal indexes：

```text
mount id set
entity WorldObjectId set
binding duplicate set (per added mount)
```

最终：

```text
addMount                 O(binding_count) total
mount uniqueness         O(1) per mount
entity scope uniqueness  O(1) per mount
build                     O(total bindings)
```

这些 set 是 builder internal state，不创建 public Catalog。

---

# 25. SimulationDescription stable-ID lookup

Script builder/materialization 依赖：

```text
SystemInstanceId
HookPointId
EventPointId
```

当前按 ID 的部分 lookup 仍由 `std::find_if` 实现。

对于会进入 runtime/materialization 的 stable-ID lookup，应建立 derived index：

```text
SystemInstanceId -> system ordinal
(system type, HookPointId) -> hook ordinal
(system type, EventPointId) -> event ordinal
```

目标：

```text
findSystem(id)        O(1)
findHookPoint(id)     O(1)
findEvent(id)         O(1)
```

name-based lookup 是 authoring/diagnostic convenience，可以暂时保持非 O(1)，但不得被 runtime hot/materialization loop 调用。

不要为了 lookup 创建 `SimulationCatalog` service。

index 直接属于 immutable `SimulationDescription` 的 derived runtime state。

---

# 26. ScriptSystem steady-state复杂度验收

必须满足：

| 操作 | 合同 |
|---|---|
| Hook connect | O(1) |
| Hook disconnect(token) | O(1) |
| Hook dispatch | O(F) |
| Event target lookup | O(1) |
| Targeted Event dispatch | O(A + K) |
| Script mount dirty insert | O(1) |
| Script mount detach | O(bindings of mount) |
| Script target handler detach | O(1) / binding |
| Entity -> active script mount routing | O(1) |
| ScriptSymbolId -> export | O(1) |
| Endpoint stable ID -> descriptor | O(1) |
| full prepare | O(mounts + bindings + endpoints + exports touched) |

任何与：

```text
total entities
total script instances
total bindings in all mounts
all event subscribers
```

无关的单-target 操作，不得扫描这些全局集合。

---

# 27. 不要滥用 RuntimeObject / unordered dynamic storage 修 hot path

高性能 runtime 数据应继续：

```text
SlotMap
SparseSet
pre-reserved vector
dense storage
stable integer/slot key
```

`RuntimeObject`、`std::string`、reflection lookup、unordered string lookup 不进入 per-event/per-hook 路径。

---

# 28. CMake target 收敛

最终允许的核心 target 语义应类似：

```text
script_core
script_artifact
script_lua
script_native

simulation_scripting_core
simulation_script_cpp_static
simulation_script_lua
simulation_script_native
simulation_script_system

flowforge
flowforge_compiler

node_graph_editor
lua_script_packager
script_authoring
```

具体 target 命名可遵循现有工程前缀约定，但**同一职责只能有一个 canonical target**。

---

## 28.1 必须删除/禁止的 target vocabulary

```text
flowforge_script_compiler
flowforge_compiler_dialect     # 若作为 public/exported target
script_asset
script_binding
script_runtime_manager
script_service
script_catalog
semantic_catalog
target_catalog
```

private MLIR helper target 不在此限制内，但不能 export/install。

---

# 29. 禁止 compatibility layer

这是 breaking cleanup。

禁止：

```text
旧 include path forwarding header
using OldName = NewName
deprecated wrapper
CMake ALIAS 为旧 target 保命
双路径安装
旧 wire model adapter
```

测试、demo、installed consumer 全部一次性迁移。

仓库未冻结 public ABI，不为错误架构支付长期兼容成本。

---

# 30. Architecture gate 必须增加的负面规则

新增静态门禁，至少拒绝：

```text
engine/tools/
modules/resource/asset/script/
engine/toolchain/script/

ScriptAssetContent
ScriptCallFrame.hpp
ScriptValue.hpp
ScriptSignature.hpp

ScriptSemanticType
ScriptSemanticLayout
ScriptSemanticTypeTraits
sameScriptSignature
scriptBuiltinLayout

ScriptInstanceCreateContext::mount   # 可按字段/依赖间接检查
flowforge_script_compiler
```

同时拒绝 simulation hot packages include：

```text
lux/engine/meta/RuntimeObject.hpp
```

至少覆盖：

```text
engine/domain/simulation/system
engine/domain/simulation/systems/script
```

若 cpp_static cold bridge 因 reflection 需要 RuntimeObject，必须明确例外到具体 source，而不能允许整个 simulation layer。

---

# 31. Package-purity gate

增加 CMake/脚本检查：

若一个目录拥有任一：

```text
include/
src/
pinclude/
```

则其直接子目录不得再包含独立 package `CMakeLists.txt`，除非该子目录只是 `src` 内部实现目录。

反之，collection root 如果 `add_subdirectory(child)` 聚合 package，不允许自己再有 include/src。

此门禁用于防止再次出现：

```text
scripting/
  include/
  src/
  lua/
  native/
```

---

# 32. 测试与 benchmark

## 32.1 HookPoint 高 fan-out

至少覆盖：

```text
100k handlers
1M handlers（stress profile）
random connect/disconnect
generation token stale rejection
dispatch exact call count
```

验证：

```text
disconnect 不扫描 handlers
dispatch 不发生 allocation
```

不要只测 4~16 个 handler。

---

## 32.2 Targeted Event

构造：

```text
1M distinct Entity targets
每 target 1~N handlers
少量 connectAll observers
```

随机 dispatch target。

必须验证算法访问范围只包含：

```text
target index
all bucket
exact target bucket
```

不要用 wall-clock 作为唯一正确性证明；测试版可加入 probe counter 验证没有 global scan。

---

## 32.3 ScriptSystem 大场景

至少建立 synthetic profile：

```text
1,000,000 entities
20,000 scripted entities
多个共享 ScriptArtifact
高 fan-out hook
高 cardinality targeted events
```

验收：

```text
一次 target event 不随 20,000 script instance 数增长
一个 mount detach 不随全局 binding 数增长
dirty enqueue 不随 dirty queue 长度增长
```

---

## 32.4 Prepare scaling

构造 N / 2N / 4N mounts+bindings。

禁止出现近似二次方 scaling。

更优先使用 operation/probe counter 证明：

```text
endpoint resolution count == O(bindings)
export lookup count == O(bindings)
duplicate validation == O(bindings)
```

不要仅依赖不稳定的 CI timing ratio。

---

## 32.5 Lifecycle regression

继续保留：

```text
prepare during Event drain -> DISPATCH_ACTIVE
shutdown during busy endpoint -> failure, no UAF
prepare failure -> no stale ScriptAttachment
shutdown -> zero owned ScriptAttachment
invocation failure -> exact ScriptSymbolId diagnostic
```

---

## 32.6 Lua record event chain

保留真实 structured payload，例如 Collision record：

```text
Physics EventPoint
 -> ScriptEndpointBridge
 -> LuaRecordMarshaller
 -> Lua callback
```

确保不退回 scalar-only。

---

# 33. 实施顺序

## Phase A — Topology canonicalization

1. `engine/tools/editor` -> `engine/editor`
2. `engine/tools/toolchain` -> `engine/toolchain`
3. `toolchain/script/lua` -> `toolchain/lua`
4. `simulation/scripting` parent 改 collection-only，root code 移入 `core`
5. 更新 root CMake/profile wiring
6. 删除旧目录，不留 forwarding

完成后先跑全部 topology/install consumer gate。

---

## Phase B — ScriptArtifact ownership

1. 新建真实 package `modules/function/script/artifact`
2. 移动当前 script asset codec/content
3. `ScriptAssetContent` -> `ScriptArtifact`
4. 全量更新 resolver/backend/codec consumer
5. 删除 `modules/resource/asset/script`
6. generic resource asset 不再依赖 Script domain

---

## Phase C — Script type compression

按顺序删除：

1. `ScriptCallFrame.hpp`
2. `ScriptValue.hpp`
3. `ScriptSignature.hpp`
4. ScriptSemantic mirror API
5. `ScriptInstanceCreateContext::mount`
6. `ScriptSystemOptions`
7. `ScriptSystemResult`
8. 无生产 consumer 的 `findMount`
9. Lua test-only public counters

同时将所有 semantic usage切到 `lux::semantic::*`。

这一阶段禁止创建 compatibility aliases。

---

## Phase D — Runtime/materialization complexity closure

1. ScriptSystem endpoint O(1) index
2. bucket ordinal direct mapping
3. ScriptArtifact export O(1) index
4. per-mount method symbol index
5. ScriptSystemDescriptionBuilder uniqueness indexes
6. SimulationDescription stable-ID indexes
7. benchmark + probe tests

---

## Phase E — FlowForge compiler collapse

1. `engine/toolchain/flowforge` 只保留一个 package
2. public `Compiler.hpp`
3. `compileFlowForge...` 最终直接产生 `ScriptArtifact`
4. 删除 `FlowForgeScriptArtifact`
5. ABI manifest 下沉/删除
6. AOT/IR/Passes 移 private
7. MLIR dialect target private-only
8. 删除 `flowforge_script_compiler`
9. 删除 compiler 内 display-name-derived ScriptSymbolId

---

## Phase F — Architecture/exception gates

1. negative path/type gate
2. package-purity gate
3. hot-path RuntimeObject include gate
4. hot path no-allocation tests
5. high-cardinality performance tests
6. installed consumer exact include/target verification

---

# 34. 实施方禁止新增的类型

除非出现本文没有覆盖、且无法用现有 primitive 表达的真实新 invariant，否则不得新增：

```text
ScriptRuntimeManager
ScriptBindingManager
ScriptSubscriptionManager
ScriptEndpointManager
ScriptContext
ScriptRuntimeContext
ScriptEnvironment
ScriptService
ScriptCatalog
SemanticCatalog
TargetCatalog
ScriptRegistry
ScriptExportIndex              # index 应为 ScriptArtifact 内部字段
EndpointCatalog
SimulationCatalog
FlowForgeScriptCompilerFacade
ScriptAssetAdapter
LegacyScriptArtifact
```

也不要把本文中的内部 hash index 各自抽成一个 class。

---

# 35. 允许保留的核心类型

为了避免“删类型”误伤真实 semantic boundary，以下默认保留：

```text
ScriptSymbolId
BoundScriptCall

ScriptMountId
SimulationScriptMount
EntityScriptMount
ScriptMountScope

HookScriptTarget
EventScriptTarget
ScriptBindingTarget
ScriptBindingDescription
ScriptMountDescription
ScriptSystemDescription
ScriptSystemDescriptionBuilder

SimulationScriptScope
EntityScriptScope
ScriptInstanceScope

ScriptBehavior
ScriptHostApi
ScriptHostComponentContract

ScriptBackendInstance
ScriptBackendDescriptor
EScriptBackendResult

ScriptSystem
EScriptSystemError
ScriptSystemFailure

CppStaticScriptDescriptor
CppStaticScriptBackend

LuaComponentBinding
LuaRecordMarshaller

HookPoint
EventPoint
EndpointConnectionToken
```

如果要删除其中任何一个，必须先证明其语义已经完全被另一个现有类型覆盖，而不是仅因为字段少。

---

# 36. Freeze / Go-NoGo 验收

只有同时满足以下条件才允许认为本轮完成。

## Topology

- [ ] `engine/tools` 不存在
- [ ] `modules/resource/asset/script` 不存在
- [ ] `engine/toolchain/script` 不存在
- [ ] `simulation/scripting` 是纯 collection root
- [ ] node editor 位于 `engine/editor/node_graph`
- [ ] FlowForge compiler 位于 `engine/toolchain/flowforge`

## Vocabulary

- [ ] 只有一个 FlowForge compiler public target
- [ ] 只有一个 canonical Script cooked artifact type
- [ ] 无 `ScriptAssetContent`
- [ ] 无 public `ScriptCallFrame/ScriptValue/ScriptSignature`
- [ ] 无 Script semantic mirror layer
- [ ] 无 dead `ScriptInstanceCreateContext::mount`
- [ ] 无单字段 `ScriptSystemOptions`

## Runtime correctness

- [ ] Event drain reentrancy protection保持
- [ ] shutdown detach failure保持
- [ ] ScriptAttachment ownership/rollback保持
- [ ] invocation failure保留 symbol
- [ ] Lua record event payload保持

## Performance

- [ ] Hook registration/remove O(1)
- [ ] Hook dispatch O(actual fan-out)
- [ ] Event target lookup O(1)
- [ ] targeted dispatch O(actual subscribers)
- [ ] mount detach O(own bindings)
- [ ] dirty insert O(1)
- [ ] ScriptSymbol export lookup O(1)
- [ ] endpoint stable-ID lookup O(1)
- [ ] ScriptSystem prepare 无 accidental O(N²)
- [ ] ScriptSystemDescriptionBuilder 无 accidental O(N²)
- [ ] hot dispatch 无 allocation

## Tooling

- [ ] Lua tool 明确为 packager，不叫 Symbol Importer
- [ ] FlowForge graph model不依赖 compiler
- [ ] node_graph 不依赖 FlowForge
- [ ] compiler internals不作为 installed API

## Cleanup

- [ ] 无 forwarding header
- [ ] 无 legacy alias target
- [ ] 无 compatibility adapter
- [ ] 无新增 Manager/Catalog/Context 层

---

# 37. 最终架构一句话

最终应当可以用下面这张图解释整个 Script / FlowForge 链，而不需要再解释十几个相似名词：

```text
                      authored model
                           |
        +------------------+------------------+
        |                                     |
     Lua source                            FlowGraph
        |                                     |
 LuaScriptPackager                      FlowForgeCompiler
        |                                     |
        +---------------+---------------------+
                        |
                  ScriptArtifact
                        |
                generic Asset storage
                        |
                    ScriptSystem
                        |
          +-------------+-------------+
          |             |             |
        Lua          Native       CppStatic
       backend       backend       backend
          |             |             |
          +-------------+-------------+
                        |
              HookPoint / EventPoint
```

需要长期保留的不是更多 class，而是少数明确不变量：

```text
稳定身份
明确 ownership
O(1) key lookup
output-sensitive dispatch
cold-path allocation
hot-path零分配
generic endpoint 不知道 Script ABI
Binding 不知道 Entity
Artifact 不等于 Asset runtime object
Compiler 只有一个 canonical output
```

当实现无法用这些概念直接解释时，优先删除中间层，而不是给中间层再命名。
