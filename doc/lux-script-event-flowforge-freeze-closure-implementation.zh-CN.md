# Lux Engine L1 Script / Event / FlowForge Freeze Closure 实施规范

**状态：实施中；独立验收通过前 L1 仍为 NO-GO。**

本规范是 `lux-script-flowforge-concept-compression-implementation.zh-CN.md` 的窄范围验收修订。
旧规范和 qualification evidence 保留为历史记录；本规范覆盖 prepare rollback、FlowForge compiler、
targeted Event 数据布局、Script backend allocation 和 Script description validation。

## 1. ScriptSystem prepare rollback

`ScriptSystem::prepare()` 不得释放仍被 Endpoint lane 引用的 context。部分连接失败后，rollback 必须先断开
全部已连接 lane；遇到 active dispatch 或 writer 时返回 `ENDPOINT_BUSY` 并进入 `ROLLBACK_PENDING`。
该状态下所有 runtime state、mount、backend instance、lease、signal connection 和 lane context 保持存活，
仅 `shutdown()` 可以重试 teardown。全部 lane 断开后才允许释放 runtime。

## 2. FlowForge compiler

FlowGraph 持有声明顺序稳定的 export 表。每个 export 由非零 `FlowForgeExportNodeId`、稳定 entry node ID 和
authored `ScriptSymbolId` 组成。第一版 entry 必须是 `OnEventNode`；display name 与 signature 从 node 投影，
不得复制第二份 signature truth，也不得从 display name 派生 executable identity。

唯一 public compile surface 接受 `FlowGraph + FlowForgeCompileOptions` 并返回
`FlowForgeResult<ScriptArtifact>`。编译必须完成 graph validation、MLIR/LLVM lowering、native object codegen、
reproducible shared-module link，并将完整 module bytes 写入 `ScriptArtifact::payload`。不得返回空 payload 的
metadata-only artifact，也不得把 AOT/IR/Pass/dialect helper 暴露为 installed API。

TOOLCHAIN profile 始终包含完整 MLIR/AOT compiler；删除 `LUX_ENABLE_FLOWFORGE_MLIR`。其他 profile 不构建
FlowForge compiler，因此 MLIR/LLVM 不得进入 PLAYER、EDITOR 或 DEVELOPER closure。

## 3. Scripting core ownership

`simulation_scripting_core` 是 header-only foundation，只导出 `scripting/*` 头及其直接依赖。它不得导出或
转发 concrete `systems/script/*` 头。使用 ScriptSystem 的 consumer 必须显式依赖 `simulation_script`。

## 4. Targeted Event storage

Generic EventPoint 与 ScriptSystem 使用同一个内部 dense entity handler storage：generation-safe SlotMap 只
保存 registration `{target, dense_index, connect_all}`；exact Entity sparse index 定位 dense target bucket；
dispatch 只遍历连续 Handler 数组。disconnect 通过 token O(1) 定位并 swap-pop，更新被移动 registration。
dispatch 不得逐 handler 执行 SlotMap lookup 或 intrusive key chasing。

Event occurrence buffer 继续保留 per-producer writer ownership，并额外维护 `active_writer_count`，使 topology
busy 检查为 O(1)。record/drain/dispatch 不分配。

## 5. Backend allocation 与 description validation

Lua prototype 为每个 `(AssetId, ScriptSymbolId)` 缓存一次 function ref 和 marshaller selection。per-instance
PreparedCall 来自 backend-local fixed-capacity pool，不持有自己的 vector，不使用 general `new/delete`。

CppStatic backend 使用 fallible factory 和 per-descriptor capacity。每个 reflected descriptor 持有固定大小、
正确对齐的 object slab；instance create/destroy 只分配/归还 slab slot。

`ScriptArtifact::create()` 单次遍历同时验证 description、拒绝 duplicate symbol并构建 export index。encode、
decode、FlowForge compiler 和 ScriptSystem 不得重复执行同一 description validation。standalone
`validScriptDescription()` 也必须保持线性。

## 6. Freeze qualification

最终证据必须来自干净 production exact SHA，并覆盖 Developer RelWithDebInfo/Debug、Hardened contracts、
PLAYER、EDITOR、强制 MLIR/LLVM 的 TOOLCHAIN、fresh install、全部 installed consumers、架构扫描和 benchmark
v12。Android build 不属于验证矩阵；公共模块头仍同步 Android install include mirror。

完成本规范只产生新的 Freeze Candidate。再次独立 API/语义验收通过前，L1 保持 NO-GO，Formal L2 保持
BLOCKED。
