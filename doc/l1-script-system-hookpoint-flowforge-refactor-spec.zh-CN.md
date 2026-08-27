# L1 ScriptSystem / HookPoint 与 FlowForge 包化重构规范

状态：实施中的唯一 normative source

基线：`main@3f3f8a5194dbc78a80d8c57fdb5ef86f8de72e54`
设计输入：`lux-script-system-refactor-plan.md`，SHA-256
`39048603EC81C5AE91EB067F1632387B5790D7B12096ED7BB506C1CA9DA68700`

## 1. 目标与边界

本规范取代设计输入中已经过时的仓库审计结论，但保留其核心方向：Script 是由
`ScriptSystem` 解释的普通 Simulation 数据；System endpoint 不依赖 Script ABI；
FlowForge 与 GraphKit 是独立、按产品付费的工具包。

本轮不保留旧 API、转发头、target alias 或 wire decoder。旧 qualification/evidence
仅作为历史记录，不能证明本规范的生产提交。

硬边界如下：

- L0 提供稳定类型身份、资源描述、Script 调用 ABI 与语言运行时机制。
- L1 Simulation 提供 System、HookPoint、EventPoint、ScriptSystem 与同步 ECS 投影。
- L1 不依赖 Process、Scene、Authoring、Toolchain、Editor 或异步 Asset 服务。
- Player/Android 不链接 Lua、Python、FlowForge、GraphKit、MLIR 或 LLVM。
- Python authoring 与 runtime 全部删除。

## 2. System identity 与 endpoint

### 2.1 稳定身份

下列 ID 均为显式、非零、可序列化值；名称只用于诊断：

- `SystemInstanceId`
- `HookPointId`
- `EventPointId`

System type 继续使用稳定 `SystemTypeId`。实例、Hook、Event 的 wire/reference 不再使用
名称作为身份，也不得由显示名称隐式生成。

### 2.2 描述

`SystemDescription` 使用与 Script 无关的 `HookPointSpec` 和 `EventPointSpec`。
Hook v1 只允许 `MULTI + void`；不提供 SINGLE 或业务返回值。

`EventPointSpec` 显式声明 route：

- `SIMULATION_BROADCAST`
- `ENTITY_TARGETED`

事件 payload 为稳定 type identity/schema；generic System 层不得 include ScriptSemantic、
Script ABI 或 Script asset 头。

### 2.3 运行时 primitive

生产者拥有 `HookPoint<void(Args...)>` 与 `EventPoint<Route, Payload>`。订阅变更只在
quiescent safe point 提交，dispatch 期间不得修改 live subscriber storage。

Hook dispatch 调用该 slot 的全部订阅者；普通 Hook 永远不按 Entity 路由。
Entity exact-generation routing 只属于 `ENTITY_TARGETED Event`。

事件缓冲由生产 System 拥有，worker 保存真实 Payload value，safe Hook 按 producer
ordinal/local append 顺序 drain；同一 Hook 中 Event 先于 Hook handlers。

## 3. Simulation Description 与 wire

`SimulationDescription` 只包含 global SimulationData、system types、system instances、
capabilities、Hook/Event 描述和 system-to-system dependencies。Script mounts/bindings 从
SimulationDescription 移除。

LXSD 升级为 v5：

- fixed little-endian header 与 section directory；
- endpoint 使用 stable ordinal/ID；
- 删除 global script mount 与 script binding sections；
- 拒绝 LXSD v1-v4，不提供兼容 decoder；
- 解码使用调用方 budgets，先进入 draft，再由 Builder 验证。

## 4. Script Description 与 symbol identity

Script Description 升级为 v5，LXSA 升级为 v3。common exports 保存：

- 非零 `ScriptSymbolId`；
- owning diagnostic name；
- owning stable type identity、pass、ABI kind、size 与 alignment；
- void-only Hook/Event compatible signature。

删除 `EScriptModel` 与 Python kind/body。Native ABI 保持 v2；CppStatic、Native、Lua
继续使用同一 Script v5 executable contract。LXSA v1/v2 全部拒绝。

`ScriptSymbolId` 不得由 module/name/signature hash 生成。Authoring-only
`ScriptSymbolLedger` 为每个资产单调分配 ID，ID 永不复用；显式 rename map 把旧 source
identity 迁移到新 source identity并保留 ID。source 只保留 `@lux.method` 或 generic
`LUX_METHOD`/`LUX_FUNC`，ledger 不进入 Player hot path。

## 5. ScriptSystem 数据模型

`ScriptSystemDescription` 是 schema `lux.simulation.script` 的持久 SimulationData：

```text
ScriptSystemDescription
  mounts[] : ScriptMountDescription

ScriptMountDescription
  id       : ScriptMountId
  asset    : AssetId
  scope    : SimulationScriptMount | EntityScriptMount{WorldObjectId}
  enabled  : bool
  bindings : ScriptBindingDescription[]
```

`ScriptBindingDescription` 只保存 symbol 与强类型 Hook/Event target（stable system instance
和 endpoint ID）。binding 永远不包含 Entity；lifecycle 不作为 binding target。

`ScriptSystemDescription` 使用独立 `LXSS v1` fixed little-endian codec。mount ID 在一个
description 内唯一；声明顺序有语义。旧 Simulation global mounts、公开 ScriptComponent
和 snapshot schema 全部删除。

## 6. ScriptInstance 与生命周期

`ScriptInstance` 是唯一运行单元，scope 为：

```text
SimulationScriptScope | EntityScriptScope{exact Entity}
```

不得拆分 global/entity manager、session 或 instance table。Entity authored scope 通过
`WorldObjectId` 在同步边界解析；private ECS projection 仅可为
`detail::ScriptAttachment{WorldObjectId}`，不反射、不序列化。

实例生命周期固定为：

```text
create backend
attach scope and host
CONSTRUCT
prepare each unique exported method once
register Hook/Event handlers
START
ACTIVE
STOP(reason)
unregister
DESTROY lifecycle
release prepared methods
destroy backend instance
```

任何初始化失败都使用 `INITIALIZATION_FAILED` 做确定性 cleanup。调用失败只写入预分配
failure storage，实例进入 runtime fault/retiring 状态并在 safe point 解绑；不得修改持久
`enabled`。

`ScriptSystem` 符合当前静态 `System` concept，对外提供 `prepare()`、
`flushMutations()`、`shutdown()`。输入只允许 immutable descriptions、Registry、同步 resident
asset resolver、endpoint references、backend descriptors、host capabilities 与显式 capacities；
不得接受 Process、Scene、AssetManager 或全局 registry。

## 7. Dispatch index

每个 Hook 使用一个 `HookPointScriptTable` lane，同时包含 simulation-scope 与 entity-scope
instances，排序为：

```text
ScriptMountId -> binding declaration order
```

一次 Hook invocation dense-dispatch 全部 handlers；调用方不传 Entity，不做 scene scan。

Event 使用 `EventScriptTable`：broadcast 访问全局 lane；targeted event 使用 exact Entity
generation sparse lookup。Simulation-scope instance 不得绑定 entity-targeted Event。

prepare 后热路径要求：零 Lux 分配、零 asset/name/reflection lookup、零 signature adaptation、
零虚调用和零共享所有权增减。

## 8. Backend 与 host

Backend 使用冷路径 instance-first descriptor：create instance、prepare method、release method、
destroy instance。一个 mount 一个 backend instance，一个 symbol 一次 prepare，多个 targets
复用 prepared call。

- CppStatic：由 Meta bridge 单向投影，hot path 只调用 cold-resolved invoker。
- Native：ABI v2、显式 module lease、完整 export/state layout 校验。
- Lua：每 asset/session 一个 prototype、每 mount 一个 instance table、cached method refs；
  Player/Android 不构建 Lua。

scope-aware `ScriptBehavior` 提供受控 self/read/has/patch/command。Simulation scope 没有 self；
Entity scope 使用 exact Entity。不得暴露 Registry，也不得从脚本自动推导 TaskGraph hazard。

## 9. Authoring

Authoring 枚举 Script exports 与 Simulation endpoint，按 route、scope 和 exact signature 过滤，
支持一个 export 到 0..N targets。运行时仍做 defensive cold validation。

Script ledger、authoring signature view 与诊断只存在于 EDITOR/TOOLCHAIN closure。删除旧
`ScriptAuthoringSemanticCatalog`、target catalog JSON 和 Python importer；中立类型 SSOT
由 L0 提供，authoring 只做 owning projection。

## 10. FlowForge 包

实际 graph、dialect、passes 与 AOT 从 legacy 迁入 `engine/flowforge`，安装为独立
`lux-engine-flowforge` 包。组件至少分为 graph、dialect、compiler/passes、AOT。

FlowForge graph 显式声明 exports 与 binding edges；compiler 生成 Script v5、ledger identity、
Native ABI v2 manifest 和独立 binding template。不得从 target 名称反推 symbol，也不得把
legacy `ScriptInstance` 作为生产 runtime。

FlowForge 只在 EDITOR/TOOLCHAIN 构建；MLIR/LLVM 只在 TOOLCHAIN 且显式选项开启时进入闭包。
旧 canonical minimal adapter 与 legacy FlowForge 物理删除，不留 shim。

## 11. GraphKit 包

GraphKit 从 legacy 迁入 `engine/graph_kit`，安装为独立 `lux-engine-graph-kit` 包。它只依赖
通用 UI 与 imgui-node-editor，提供 graph view/schema/connect/undo/layout；不得依赖
FlowForge、Simulation、asset、MLIR 或 LLVM。

GraphKit 仅属于 EDITOR。FlowForge 可提供可选 adapter 依赖 GraphKit，但 FlowForge graph、
compiler 和 AOT 不得反向依赖 GraphKit。本轮不创建假的 Editor executable。

## 12. 物理删除与产品闭包

最终必须删除且不兼容：

- `ScriptBindingSession`、公开 `ScriptComponent`、`EntityBehavior`；
- `SystemHookPoint`、SINGLE、旧 name-routed Event/Hook；
- SimulationDescription global mounts 与 lifecycle binding variant；
- hash-based symbol generation、semantic/target catalogs、Python importer/runtime；
- `engine/simulation/script_binding` target/package；
- 旧 minimal FlowForge paths 与全部 migrated legacy paths。

Player/Android 安装和链接闭包不得包含 Lua、Python、FlowForge、GraphKit、MLIR 或 LLVM。

## 13. 验证与冻结

每个阶段执行 Developer PowerShell 全量 `all -- -j 4 -k 0` 和完整 CTest；CMake 变更跑
两轮，第二轮必须 `ninja: no work to do`。修改 `modules/*` 公共头后，先同步 Debug、
RelWithDebInfo、Android 三个安装前缀，再运行 meta generation。

benchmark schema 升级到 v9，覆盖：Hook 1/8/64/1024 mixed scopes、broadcast/targeted Event、
1M/2M sparse ratio、mutation flush、CppStatic/Native/Lua。使用 5 warmups/30 samples；targeted
2M/1M median ratio不超过 1.20，调用与 notification 数量精确。

最终从 production exact SHA 创建 clean detached worktree，执行 RelWithDebInfo、Debug、
Hardened contracts、Android arm64 PLAYER/NDK30、EDITOR+GraphKit、TOOLCHAIN+真实
FlowForge/MLIR/AOT、fresh install、全部 installed consumers、源码/安装架构扫描和 benchmark
v9。evidence-only commit 的唯一父提交必须是已验证 production SHA。
