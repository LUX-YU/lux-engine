# Lux Engine L1 EntityBehavior / Method Binding 最终语义闭环规范（Revision 3）

**文档性质：Normative Implementation Specification**  
**日期：2026-08-27**  
**实施基线 evidence HEAD：`d72cc8366d789533d00f8b6ed6a8eb81aa8017ca`**  
**被拒绝 production SHA：`dcb85fcb5fcf0124137e523b702885a2d2bb5fe0`**

本文完整取代 Revision 2 作为 EntityBehavior、Method Binding、运行时派发、
language backend 和 qualification 的规范真源。Revision 2 与旧 evidence 仅保留为历史记录。

## 1. Freeze 状态

```text
Build qualification                 PASS (historical)
Performance qualification           PASS (historical)
Independent correctness acceptance  FAIL
Independent semantic acceptance     FAIL
L1 formal freeze                     NO-GO
Formal L2                            BLOCKED
```

本轮产物只能成为新的 Freeze Candidate。独立 API/语义验收通过前不得写入 FROZEN。

## 2. 不变的体系结构

- Components 是 canonical world state；System 是 bulk/data-oriented behavior；
  EntityBehavior 是 opt-in local object-oriented behavior。
- `ScriptDescription v4` 只描述 reusable code exports，不保存 composition binding。
- `ScriptMountDescription` 保存 stable MountId、AssetId 和显式 bindings。
- C++ `LUX_METHOD`/`LUX_FUNC` 保持 generic Meta 语义；Script bridge 只做冷路径单向投影。
- Native C ABI 固定为 v2；Script Asset 固定 LXSA v2；Simulation 固定 LXSD v4。
- Event payload 由 System-owned typed buffer 按值持有；Session 只做同步 dispatch，绝不保存 frame 指针。
- 不新增 ScriptSystem、EventBus、全局 backend registry、service locator 或隐式 code lifetime。

## 3. Hook 与 Event 路由

普通 `SystemHookPoint` 没有 Entity-targeted 语义。唯一公开调用为：

```cpp
dispatchHook(ScriptHookSlot hook, const lux_script_call_frame& frame) noexcept;
```

`DispatchIndex` 固定为：

```text
Hook slot
  -> contiguous handlers (global + entity mounts)

Global Event slot
  -> contiguous global handlers

ENTITY_TARGETED Event
  -> exact Entity sparse slot
  -> event-major sparse range
  -> contiguous handlers
```

Hook handler 顺序固定为 global mount 声明顺序，然后 exact Entity bits、entity 内 mount 声明顺序、
binding 声明顺序。一次 Hook invocation 必须调用该 slot 的全部 ACTIVE handlers。
System 不得枚举 scripted Entity，也不得向 `dispatchHook` 传 Entity。

Entity 精确 generation lookup 只属于 `ENTITY_TARGETED Event`。Entity sidecar 不保存 Hook ranges。
同一 safe Hook 内先 drain Event occurrences，再调用 Hook handlers。

`SINGLE` Hook 在 Builder、authoring composition 和 runtime 三层聚合校验 0/1；
EntityBehavior 只能绑定 `MULTI` Hook。

## 4. 稳定所有权与 DispatchIndex

`MountRuntime` 与 `PreparedMethod` 必须拥有稳定地址。Handler 只引用稳定对象，不能引用会被
`vector` erase/reallocation 移动的元素。

DispatchIndex 使用双缓冲 candidate/fallback publication。构建、排序、容量检查或 lifecycle
失败不得把部分 candidate 暴露给 normal dispatch。quiescent mutation 返回前，当前 index 中
不得存在已销毁实例或已释放方法的指针。

Hook flat storage 与 Entity Event sparse storage 都使用调用方容量；prepare 后 hot dispatch
禁止分配、反射、字符串/名称查找、资产解析、signature adaptation 和 scene scan。

## 5. Lifecycle 状态与进度

运行状态仍为：

```text
CONSTRUCTING -> ACTIVE -> RETIRING -> DEAD
```

事务另外记录：

```text
backend_created
construct_entered / construct_completed
start_entered / start_completed
published
stop_called
```

不得用 runtime state 或单个 `lifecycle_started` bool 推断 staged ownership/cleanup。
hot callback failure 可把 ACTIVE mount 标记 RETIRING；staged lifecycle failure 只标记当前
MutationPlan 失败，不能令 staged resource 脱离 transaction ownership。

## 6. StopReason ABI

```cpp
enum class EBehaviorStopReason : std::uint32_t
{
    MOUNT_REMOVED,
    ENTITY_DESTROYED,
    SIMULATION_STOPPED,
    INITIALIZATION_FAILED,
};
```

canonical semantic name 为 `lux.simulation.BehaviorStopReason`，ABI kind 为 `UINT32`。
该枚举值不序列化进 LXSA/LXSD，因此不升级 wire/schema version。

## 7. 初始与动态激活

每个 mutation batch 固定顺序：

```text
discover/diff all authored mounts
preflight capacities
resolve all new assets and leases
create all staged instances
attach all host/self contexts
prepare all unique staged methods
run all staged CONSTRUCT handlers in deterministic order
build candidate index and survivor fallback index
cross retirement barrier
STOP/release/destroy all retiring old instances
run all staged START handlers
mark staged mounts ACTIVE
publish candidate index
commit pending authored descriptions
release obsolete retained methods
```

所有 CONSTRUCT 必须先于任何 START。normal Hook/Event callback 只能看到 START 成功并已发布的实例。

## 8. Failure 与 rollback

- resolution/create/prepare/CONSTRUCT/shadow failure 发生在 retirement barrier 前：释放所有 staged
  resource，保留旧实例和旧 index；RETIRING 旧实例继续禁止 normal callback，dirty 留待重试。
- 任何 CONSTRUCT 或 START handler 已进入后失败：执行一次
  `STOP(INITIALIZATION_FAILED)`，再释放 methods、destroy instance、release lease。
- lifecycle point 的 CONSTRUCT/START 在首个失败后停止；STOP 的每个 handler 都尝试一次，
  单个 STOP 失败只记录，不阻止其余 STOP 或资源销毁。
- replacement 采用 teardown-first：candidate/fallback 已构建后，先 STOP/销毁旧实例，再 START 新实例。
  START 失败时旧实例不复活；销毁 staged 新实例并发布仅含 unaffected survivors 的 fallback index，
  authored owner 保持 dirty，在下一 safe point 重试。
- STOP exactly once；backend destruction 必须发生在 STOP 后。

## 9. Binding-only edit 与 method retirement

same MountId + same AssetId 只修改 bindings 时保留 backend object、private state 和 lifecycle 状态。
MutationPlan 计算 retained/new/obsolete symbols：

- new methods 在 staging prepare，失败时只释放本批 new methods；
- candidate index 只引用 desired methods；
- publication 后旧 index 已失效，才 release/erase obsolete methods；
- rollback 恢复旧 authored bindings 与旧 method table。

`prepared_methods` 容量按当前 live + 本批新增的峰值预检；成功 publication 必须立即回收 obsolete，
后续 edit 以真实 live count 计算。

## 10. Dirty epoch

Session 使用 `dirty_current` 与 `dirty_next`。进入 apply 时交换本批；signal 始终写入 next。
lifecycle callback 对 ScriptComponent 的 patch 因此进入下一 quiescent epoch，不得被当前 apply 尾部清除。

失败时 current 与 next 按 exact Entity 合并，reason 优先级固定：

```text
ENTITY_DESTROYED > MOUNT_REMOVED
```

任一队列溢出设置 next full-resync；不得在 callback 中增长容量。

## 11. Lua host lifetime 与 component contract

Lua `self.has_component/get_component/patch_component` closure 必须捕获 full-userdata host handle，
不能捕获裸 `Instance*`。handle 至少保存 backend owner、host pointer 和 alive bit；destroy mount
先令 handle dead、清空 host，再 unref instance table。逃逸 closure 调用 dead handle 时：

```text
has_component   -> false
get_component   -> nil
patch_component -> false
```

`LuaComponentBinding` 必须深度拥有 name/canonical name，并完整保存 component type、semantic type ID、
ABI kind、size 与 alignment。Host API 提供 cold `describeComponent`；Lua create 阶段逐字段核对。
第一版仅接受已支持 builtin scalar，u64/STRUCT_REF cold reject。配置冲突必须结构化返回，不能 type-pun。

Lua backend 通过 fallible factory 创建；不得用半有效 constructor 隐藏 allocation/config error。

## 12. Owning authoring values

Authoring target catalog 与 FlowForge persistent graph 使用 owning `rdesc::ScriptValueType`。
catalog 深度复制 target/signature strings；`SimulationDescription` 销毁后仍可枚举和执行纯 signature
compatibility filtering。

Simulation 层提供一个不解析 target identity 的纯 signature compatibility helper；runtime target resolver、
authoring catalog 和 FlowForge 共用同一 exact model/cardinality/parameters/returns/pass truth。

## 13. FlowForge identity

compiler 生成 exports 时必须建立：

```text
FlowForgeExportNodeId -> generated export index / ScriptSymbolId
```

BindingEdge 只能按 node ID 解析；human diagnostic name 不参与 identity。重名 overload 合法，
`foo(i32)` 与 `foo(f32)` 必须得到不同 symbol 并绑定到各自 exact-compatible target。

## 14. Python/Lua authoring catalog

Python dotted import 只做最长 alias prefix 的一次替换：

```text
import lux.simulation             -> root lux remains lux
import lux.simulation as sim      -> sim maps to lux.simulation
from lux.simulation import X as Y -> Y maps to lux.simulation.X
```

禁止 import/exec 用户 Python module，Python 仍为 Tier 1 authoring-only。

项目 record 通过显式 `ScriptSemanticTypeTraits<T>` 和
`makeScriptAuthoringRecordEntry<T>()` 加入 project semantic catalog。Record 默认 ABI 为 STRUCT_REF，
参数 pass 仅 CONST_REF，并使用 canonical name hash、`sizeof(T)`、`alignof(T)`。不建立全局 registry。

## 15. 公共 API 断代

- 删除 `dispatchHook(ScriptHookSlot, ecs::Entity, frame)`，不留 shim。
- 新增 `INITIALIZATION_FAILED` StopReason。
- Authoring/FlowForge graph signature 改为 owning `rdesc::ScriptValueType`。
- compatible-target catalog filtering 不要求 SimulationDescription 保活。
- Lua component binding 使用完整 owning contract；Lua backend 使用 fallible factory。
- Host context 增加只读 component contract query；仍不暴露 Registry。

## 16. Correctness acceptance

必须覆盖：

- 一次 Hook invocation 调用 global + 多个 entity subscribers，且不做 Entity lookup；
- exact Event generation lookup 保持不变；
- partial CONSTRUCT、START、STOP failure 与 repeated retry 无 zombie/leak；
- replacement STOP-old-before-START-new，START failure 发布安全 survivor index；
- `A,B -> A -> A,C` 在 capacity=2 下回收 obsolete method；
- lifecycle reentrant ScriptComponent patch 留到下一 epoch；
- Lua escaped closure 在 destroy 后安全返回；
- FlowForge overload 按 node ID；
- Python dotted import；
- authoring catalog 独立于 SimulationDescription lifetime；
- custom CollisionEvent project catalog 经 Python/Lua importer 形成 CONST_REF export 并匹配 System Event。

## 17. Benchmark v8

benchmark schema 保持 v8。旧 `hook_entity_multi` targeted-Hook 测量作废，最终语义为：

```text
10,000 EntityBehavior subscribers
one Hook invocation
callbacks             = 10,000
handlers_visited      = 10,000
entities_examined     = 0
target_range_lookups  = 0
frame_builds          = 1
hot allocations       = 0
asset/target lookup   = 0
```

Entity-targeted Event sparse benchmark、1M/2M ratio、owned worker Event buffer、5 warmups、30 samples、
reactive exponent、targeted exponent和全部原 v8 thresholds继续有效。

## 18. Qualification 与冻结

最终 production exact SHA 必须在 clean detached worktree 执行：RelWithDebInfo、Debug、Hardened
Object/UI/Simulation checks、Android arm64 PLAYER/NDK30、TOOLCHAIN+FlowForge/MLIR+Python/Lua importer、
fresh install、全部 installed consumers、源码/安装架构扫描、完整 CTest 和 benchmark v8。

evidence-only commit 的唯一父提交必须是被验证 production SHA，并记录本文 SHA-256、工具链、命令、
测试计数、benchmark summary 与产物 hashes。该提交仍只标记 Freeze Candidate；独立验收前保持 NO-GO。
