# ScriptSystem SR-1：源码映射、所有权与实施契约

日期：2026-09-06。状态：SR-1 文档交付，等待独立审阅；SR-2 未开始。

本轮依据用户批准的《SR-1：ScriptSystem 源码映射与重构设计计划》执行。附件
《LUX ScriptSystem 结构性重构：LLM 实施主指令》v1.0 的六组件、准备辅助、Entity
身份和阶段边界是设计输入；其中的后续任务单、历史工作日志不是本轮编码授权。
本阶段未修改生产实现、测试语义、generated 文件或构建逻辑，未运行构建、CTest 或性能测试。

文中标记：**现状**指在下述源码上读到的实现；**设计**指相关后续阶段应实现的契约；
**缺口**指尚无充分验证或尚未实现的目标，不能作为已复现缺陷或已通过测试报告。
方法草案不是现有 API；本文件不宣称重构已经完成。

## 1. 源码、依赖和证据身份

### 1.1 本次只读核查结果

| 对象 | 实际身份 / 状态 |
|---|---|
| Engine 设计基线 | `codex/s6-deep-optimization`，`54de8af521887c4e5aaf0041b45422a817694005` |
| 远端核对 | `git ls-remote origin refs/heads/codex/s6-deep-optimization` 返回同一 SHA；没有 fetch、切换或改写分支 |
| 分析工作树 | `E:/SyncForder/CodeRepos/lux-engine-deep-optimization`；生产源码与该 commit 无差异 |
| 该工作树原有差异 | 修改 `cmake/RunScriptV3Measurements.ps1`；未跟踪 `cmake/RunScriptV3Stress.ps1`、`semantic-architecture-debt.txt`；本轮不纳入、不修改 |
| 主工作树 | `main@a577c49409e2519029693fbe780fe8ce3ab2dc1e`；目标分支在其后 45 个提交；未切换主工作树 |
| main 原有五项修改 | `.gitignore`、`ScriptSystem.hpp`、`CppStaticScriptBridge.hpp`、`WorldPartition.hpp`、`modules/resource/description/include/lux/engine/description/Script.hpp`；保持原样 |
| 配套 lux-cxx 工作树 | `lux-cxx-script-codegen`，分支 `codex/script-codegen-closure`，`3100f54d0743c5ed94a4ccf5943df04e933de255`，干净 |
| 实际依赖构建源码 | `lux-cxx-v3r`，同一 `3100f54…`，干净；不是相邻主目录的 `7716c087…` |
| lux-cxx 远端 | 同名分支为 `e2355ca4ce4e862f4e108f74422a30fce8afb736`；本轮没有升级 |
| toolset 参照 | `lux-cmake-toolset-s6-relocation@99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`，干净 |
| 已有 Engine 资格目录 | `build/RelWithDebInfo/q2/d` 的源码目录是 `lux-engine-v3r@9d40ec68489c17b8f4c91091974cb6b54263d0b1`；不能声称是在 `54de8af` 上新运行 |

源码事实通过 `git status --short`、`git rev-parse HEAD`、`git branch --show-current`、
`git worktree list --porcelain`、相关 `git log/diff`、`git ls-remote`、逐符号源码阅读核对。
仓库未找到 CMakePresets 文件；构建入口以 `.vscode/settings.json` 和实际缓存为准。

### 1.2 安装与编译参数核查

`q2/d/CMakeCache.txt` 指向 `install/q2/c/share/lux-cxx` 和
`install/q2/toolset/share/lux-cmake-toolset`；prefix 顺序为 toolset、q2/c、
`install/RelWithDebInfo`。`q2/c` 的源码根指向 `lux-cxx-v3r`。
编译器为 MSVC `14.44.35207/bin/Hostx64/x64/cl.exe`，Ninja，
`CMAKE_BUILD_TYPE=RelWithDebInfo`；已读到 `/Zi /O2 /Ob1 /DNDEBUG /EHsc -MD -std:c++20`。
`q2/d/compile_commands.json` 的 ScriptSystem TU 使用 `install/q2/c/include`；
EventWait 测试 TU 在 `/DNDEBUG` 后有 `/UNDEBUG`。这是现有配置证据，不是本轮编译结果。
`q2/d/build.ninja` 的 simulation_script DLL 链接参数为 `/machine:x64 /debug /INCREMENTAL`，
由同一 MSVC toolset 的 link.exe 经 CMake vs_link_dll 调用；链接输入包含当前 description、
binding authoring、codec 与 ScriptSystem 对象文件，和 ScriptArtifact/Simulation/ECS/L0 库。

| 抽查产物 | SHA-256 / 比较结论 |
|---|---|
| `StableSlotMap.hpp`，q2/c 安装与 lux-cxx-v3r 源文件 | 均为 `A612E3D5F1A5B0628C0B1C027E5F22781F7CFD357684B8CB453842AB2FFC43FA` |
| 同头，lux-cxx-script-codegen 工作树 | `4A826E62A0B44C65F4A3E7667529F7AB6C96FAF9B5E9DF2CA1448A04E132A83A`；将 CRLF 归一为 LF 后与安装头逐字符相同，不能误报为内容漂移 |
| `lux_meta_generator.exe`，安装 / q2/c 构建 | 均为 `313E8D6708F78A84FC8E4FB4B264D1BC8EB055EC8D77A7DBC42DB1540600F980` |
| `reflection_generator.dll`，安装 / q2/c 构建 | 均为 `CC2ACAD0095BA988506D89BB8C388D9C64DE17C637026A58AB4487373E8DF7FE` |
| `lux_engine_simulation_script.dll`，q2/d 安装 / 构建 | 均为 `771B1FA1C091A68044B8AB5C8EF97D4BA79DED1D28AA115E0CEAB8D5D90B6C7C`；该已有产物绑定 §1.1 的旧资格目录，不是本轮构建 |

核查命令为 `Get-FileHash`、`[IO.File]::ReadAllText(...).Replace(CRLF, LF)` 比较及缓存/编译数据库读取。
这些结果证明抽查文件相符，不证明安装前缀的全部头、库和生成输出已通过资格。
本轮不运行或复制这些二进制，不修改安装树。之后依赖发生变化，需重新界定证据有效性。

### 1.3 历史失败不是本轮结果

[历史测试范围记录][history] 及 [原始 Developer 输出][history-tests] 对 `9d40ec68` 记录
Developer 112/118、Toolchain 106/106。六项失败为：

- `scene_script_lua_runtime_test`
- `scene_script_lua_runtime_workers_0`
- `scene_script_lua_runtime_workers_2`
- `scene_script_lua_runtime_workers_4`
- `scene_script_lua_runtime_interpreter_test`
- `scene_script_lua_runtime_interpreter_workers_4`

记录中的失败断言是 `scene_script_lua_runtime_test.cpp` 的
`activeContinuationCount() == 0U`，退出码 `0xc0000409`。本轮未复跑、未定位原因；
不能通过多加恢复点、减少工作或放宽该断言消除失败。该问题不阻塞 SR-1 文档交付，
但不能据现有历史日志宣称后续资格全绿。

## 2. 真实边界和调用链

下列源码引用均相对于本文件所在仓库，符号以 Engine 基线为准。

| 入口 / 边界 | 现有实现与调用方向 |
|---|---|
| 持久化描述 | [Description][description] 的 `EntityScriptMount::object` 是 WorldObjectId；builder 对 mount id、重复对象和绑定做校验；[Codec][codec] 编解码 `lux.simulation.script` 数据 |
| Scene 安装 | [Scene bridge][scene] 的 `installScriptRuntimeSystem()` 从 `SimulationDescription::findData()` 解码，持有 `owned_description`，把 `host->world` 传入 `ScriptSystem::create()`，随后 prepare、安装、bindSimulation |
| 运行时挂载 | [State][system] 的 `RuntimeMount::authored` 借用描述；`initializeMount()` 通过 resolver 得到 Entity；`processLifecycle()` 对未解析对象继续 queueDirty |
| Simulation 权威 | [Simulation][simulation] 建一个 TaskGraph，Script Hook 选 caller affinity 并参与执行区域依赖；Hook 依次 before、seal、callable、Event consume、after、结构提交、committed、reset；失败走 failed/discard |
| Scene 时间桥 | `ScriptRuntimeSystem::bindSimulation()` 在 stable before 排 real-delay 完成并 `beginStableAdmission()`；before/committed 处理 lifecycle；after 的唯一 stable 标记调用 `executeStablePoint()` |
| 通用执行协议 | [ScriptRuntime][runtime] 的 `ScriptStepContext`、Awaitable、`ScriptBackendContinuation`、`ScriptResumePacket`；State 的 invoke / beginSuspension / resumeOne / drainResumes |
| CppStatic | [C++ backend][cpp] 的 `createInstance()` 构造对象，按需执行 attach，再 prepare；`PreparedCall::invokeStep()` 和 `resumeCoroutine()` 经同一 ScriptStepContext 返回 step 结果 |
| Lua | [Lua backend][lua] 的 prepared call / `ExecutionScope`、`invokePreparedStep()`、`resumeLuaContinuation()`；私有 VM/coroutine/host handle 留在 backend |
| Native / FlowForge | [Native backend][native] 的 executable 校验、prepared call、step frame、`resumeNativeContinuation()`；代码模块 lease 由 backend 持有，FlowForge 只生成正式 artifact/module |
| 生成和普通 Ability | [Ability 模板][ability-template]、[Lua projector 模板][lua-template]、[C++ executable 模板][cpp-template]；prepared provider receiver 不绕行 ScriptSystem 重新查名或搜 provider |

**现状澄清：** `simulation_script` 直接链接名为 `world::identity` 的窄 identity leaf，
该 leaf 分类是 DOMAIN，不应仅凭名字认定已有 DAG 违规。SR-2 要移除的是脚本执行核心
对持久化身份及其描述的语义依赖，并从自身 target 去掉不再需要的直接链接；不是修改全引擎 ontology。

## 3. 状态所有权映射

表中的字段都来自 [ScriptSystem::State][system]，除非另有链接。
“写入口”是目标组件必须提供的操作边界；构造、失败清理和统计更新也属于该 owner。
其他组件只能读不可变视图、身份和值结果，不能得到可写记录。

| 现有状态组 / 字段 | 目标 owner | 读者与唯一写入口 | 失效 / 清理 |
|---|---|---|---|
| `description`、`world`、`RuntimeMount::authored` | SR-2 加载侧 | loader 解码、解析、提交 runtime 输入；内核只收 resolved 数据 | loader 结束或描述卸载；runtime 不保留其地址 |
| `simulation`、`registry`、`clock`、`limits`、`host`、`artifacts`、`real_delay`、`backends` | 配置分发给实际使用者；`ScriptSystem` 组合 | 启动时固定；Instances 取 registry/host/asset/backend，Timers 只读 clock，Ingress 取运输容量 | 外部 owner 必须活过所有借用与销毁；不因复制 descriptor 获得 context ownership |
| `mounts` 中 `scope/entity/behavior/instance/state`、`instances` 中 id 与 mount 关联 | `ScriptInstances` | reserve、建立宿主、提交构造、激活、撤销准入、回收；Execution 借窄调用访问 | 逻辑退休撤销代次权限；物理回收后才复用存储 |
| `gameplay_lifetime_started`、`pending_end_reason`、`active_counted/active_mount_count` | `ScriptInstances` | 生命周期转换、领取一次 EndPlay 资格、激活计数 | 退休领取资格时先清零；EndPlay 失败也继续清理 |
| `artifact/backend/backend_instance`、`capabilities/event_sources/event_layout_epoch`、方法区间及生命周期方法位置 | `ScriptInstances` | 接收 Preparer 提交的当前实例资源；release prepared / backend / artifact | backend 销毁后方可释放被借用的 capability/source/behavior 与资产 |
| `methods` 的 `symbol/backend/used_by_binding` | `ScriptInstances` | 准备、发布只读方法引用、逆序 releaseMethod | 只读引用限当前 incarnation；不以旧 method slot 接受新实例 |
| `PreparedMethod::active_hook` | `ScriptExecution` | admission single-flight、continuation 终结清除 | continuation 销毁/实例退休；不留在 prepared 静态数据中 |
| `bindings`、mount 的 binding 区间、`HookBucket/EventBucket::handlers`、token、handler_capacity | `ScriptBindings` | 编排绑定、发布/撤销、连接/断开；System 消费调用目标 | logical retire 先失效，物理 unlink 遵守 dispatch 约束；断开 busy 保留 token |
| `hook_endpoints/event_endpoints` 及两个 endpoint index | `ScriptBindings` | 冷期建立 canonical endpoint 索引；Preparer/EventWaits 借不可变 endpoint 视图 | System 关闭并断开后释放；endpoint 的底层 context 仍由 Simulation owning provider 保证 |
| `published_capabilities` | `ScriptPreparer` 的冷期输入目录 | 构造时收集；仅 prepare 查找并建立实例局部关联 | System 关闭；不作为热调用 locator；目录不拥有 provider 对象 |
| `continuations`、`ContinuationRecord::*`、实例 active_continuations / first_continuation | `ScriptExecution` | admit、resume、unlink、destroy；Instances 不读写链表 | 每条独立 generation；销毁回调返回前实例/代码仍有效 |
| `awaitables`、state/result_type/value/error/continuation/resume_enqueued | `ScriptExecution` | reserve、关联、完成、take、cancel | take 时唯一结果移入 resume 局部 owned outcome；取消不再 READY |
| `write_pins/release_pending`、全局 `result_write_pins/pending_awaitable_releases` | `ScriptExecution` | acquire/release 写窗口、延迟回收 | pin 归零才可销毁/复用；逻辑 live 数不包含 release_pending |
| 实例 first_awaitable、AwaitableRecord 的 ownership 链 | `ScriptExecution` | 以 ScriptInstanceId 管理本组件索引 | 取消/消费更新；不是实例 Active 状态的第二权威 |
| `AwaitableRecord::event_waiter` | Execution 的非 owning 来源关联 | 原始 waiter id 改为有限来源 token；通过显式 detach/取消操作维护 | 来源解绑同时清除；不允许 Execution 改 waiter 链表 |
| `resumes` records/head/count/high_water | `ScriptExecution` | 合法终态关联时 enqueue、预算 pop、关闭 clear | stale 通知可被验证后消费；ring 不存第二份 payload |
| `event_waiters/event_wait_routes/event_wait_sequence`、实例 first_event_waiter | `ScriptEventWaits` | register、claim、unlink、cancelInstance | 每 occurrence 从路由分离；取消按代次使领取结果无效 |
| `claimed_event_waiters/active_claimed_waiters` | `ScriptEventWaits` | 嵌套 ClaimBatch 的 acquire/finish；System 按值领取 id | 已取消 claim 仍占遍历 reservation，直到 batch 退出；不得重用其遍历位置 |
| `next_step_waits/first/last`、`simulation_delays/delay_sequence`、`DelayProvider` | `ScriptTimers` | 登记、按 snapshot 到期、撤销来源 | 新 per-instance timer 索引归此组件；time authority 不迁入 |
| `ingress`、[ExternalCompletionRing][ingress] cells/tickets/position/count/closed/high_water/capacity_failures | `ScriptCompletionIngress` | issue/revoke ticket、producer post、owner peek/ack、stop | 外部 lease 只延长 transport；不延长 Registry/VM/System |
| `external_admission_frontier/remaining/prepared` | `ScriptCompletionIngress`，捕获时机由 System 决定 | beginAdmission、在同一 window peek/ack；不得自行扩大 frontier | 下一个合法 stable window 替换；关闭拒绝新消息 |
| `retirement_queue/dirty_current/dirty_processing/lifecycle_candidates/initialized/retirements` | `ScriptSystem` 的批次协调 | 只存 id/slot 或不可变 retirement work，不暴露实例记录；observer 通知、批次换队 | 保留稀疏去重与 follow-up 下轮规则；物理回收由各 owner 完成 |
| `retirement_queued` | `ScriptSystem` 去重索引 | enqueueRetirement/完成批次 | 从 RuntimeMount 移出；只表示队列 membership，不决定实例生命周期 |
| `retiring_instance/retiring_continuations` | 分别为 Instances 的退休身份、Execution 的待销毁所有权 | System 以 id 协调，不搬运链表头 | 各自资源闭环后清除 |
| `constructed/updated/destroyed/suppress_attachment_signal` | `ScriptInstances` 的 ECS 关联职责 | 连接信号、折入存量、维护自己 attachment；仅报告退休/dirty id | observer 不执行 ECS 结构修改；System lifecycle 安全点才提交 |
| `endpoint_dispatch_depth`、`user_invocation_depth` | 分别为 Bindings dispatch scope、Execution/Instances 的窄借用计数 | 各 owner 的 scope 维护；System 聚合是否可清理 | 退出 scope 只释放保护，不递归泵完整 lifecycle |
| `event_admission_scope/next_event_layout_epoch` | `ScriptInstances` 的实例 prepared authority | mint/validate 窄操作；EventWaits 只读验证结果 | 换 runtime / incarnation / layout 失效，不以整数 slot 单独认证 |
| `last_stable_step/stopping/prepare_state`、owner-affinity probe | `ScriptSystem` | 系统准入、prepare/rollback、关闭、stable 去重 | 保留 step=0 fixture 特例；线程 probe 不是生产排他证明 |
| failures | `ScriptSystem` | 各组件返回结构化失败，由协调层保留 mount/symbol/status 并按原容量记录 | 超 failure_capacity 的既有截断语义不扩容；EndPlay 错误不吞掉 |
| 调用/恢复、waiter visit、copy bytes、lease construction、backing/queue 统计 | 状态所属组件 | 操作发生处更新；System 汇总值快照 | 保留现有计数点和 logical/physical 区别；跨线程使用 Scene stats exchange |

### 3.1 迁移后不可出现的共享

Instances 不再有 first_continuation/first_awaitable/first_event_waiter/first_timer；
Execution、EventWaits、Timers 的 per-instance 索引用 generation 完整的 ScriptInstanceId 关联。
这些索引只回答“本组件拥有哪些资源”，不能复制一套 Active 权威。

SR-3 提前移出这些索引和 `active_hook` 时，尚未 SR-4 拆出的执行代码可以继续在 State 的
**剩余执行区**维护它们；必须一次改完读写入口，不能继续通过 Instances record 跨写。
SR-4 再把该状态及全部操作交给正式组件。不需要提前创建一批空转发类。

## 4. 操作映射与迁移依赖

以下均为 [State][system] 的现有实际函数名。多 owner 行表示拆解操作，不表示共享记录。

| 现有操作 | 目标 / 调用者 | 保留的依赖和迁移顺序 |
|---|---|---|
| `create()`、`buildLayout()` | System factory + Preparer + Bindings/Instances 冷期预留 | 先定 SR-2 runtime input；所有会被 backend 保存地址的 backing 在发布前固定 |
| `initializeMount()` | System 串联 Instances 与 Preparer | SR-2 移出 resolver；SR-3 拆验证/资源提交；construct→backend attach→prepare 次序不倒置 |
| `claimLifecycleMethod()`、签名验证、capability/event 关联 | Preparer | 准备一次，移交当前实例；无永久 AssetId/裸地址校验缓存 |
| `createInstanceRecord()`、`ownsAttachment()`、`projectAttachment()` | Instances | id 代次、Entity 关联、单脚本约束一起迁移 |
| `beginPlayMount()`、`endPlayMount()`、`invokeLifecycle()` | System 决定批次，Instances 管资格，Execution 执行受控入口 | 先保护再用户调用；不得把生命周期开成外部 invoke 后门 |
| `publishMount()`、`activateMount()` | System 协调，Bindings 发布、Instances 激活 | 发布失败撤销部分绑定和 attachment；所有已开始 lifetime 的实例保留退休资格 |
| `bindMount/removeMountBindings/addEventHandler/removeEventHandler` | Bindings | token 的 owner 和撤销责任一起移动 |
| `connectEndpoints/disconnectEndpoints/invokeHookLane` | Bindings，System 启停 | 每 endpoint 一个实际脚本订阅；允许零普通 callback 的 wait-only Event endpoint |
| `dispatchEvent()` | System occurrence 协调 | Bindings 遍历 callbacks，EventWaits 提供 claim batch，Execution 拷贝/完成；不拆成两个 channel subscriber |
| `invoke()`、`beginSuspension()`、`resumeOne()`、`destroyContinuation()` | Execution | 实例访问通过 Instances；方法 single-flight 与活动调用计数一并移动 |
| `createAwaitableRecord/createAwaitable/attachWaiter/finishAwaitableOwner/takeAwaitable` | Execution，backend step factories 使用 | final result 的唯一 owner 与 eager/late 两条关联入口一起移动 |
| `eraseAwaitable/discardAwaitable/cancelAwaitables/ResultWritePin` | Execution + 显式 source detach | 回收与来源取消闭环；无递归取消循环 |
| `waitEvent()`、路由/ownership unlink、`claimEventWaiters()` | EventWaits；System 的窄等待装配操作调用 | 验证由 Instances/Preparer 提供只读事实；结果预留属于 Execution |
| `completeClaimedEventWaiter()` | System 协调 EventWaits 和 Execution | 领取条目快照→结果写窗口→copy→重验→撤销来源→发布终态 |
| `startNextStep/startSimulationDelay/startRealDelay`、两个 promote | Timers；generated Delay provider 调用 | real delay 仍由 L3 Process bridge 执行；不在 L1 新建线程/Timer |
| `AwaitableIngress::*`、`drainExternalCompletions()` | Ingress 运输 + Execution 应用；System 按 window 调度 | 先启动容量/表示验证，后 provider start；owner-only 完成不开放给 worker |
| `invalidateAdmission/invalidateInstance/beginRetirement/finishRetirement/releaseMount` | System 串联各组件，Instances 决定最终资格 | logical revoke→source cancel→execution teardown→EndPlay→backend/lease；清理回调也算重入 |
| `handleAttachmentSignal/queueDirty/queueRetirement` | Instances 报告，System 排序/去重 | 保留真实结构提交时机；observer 不运行第二次 apply |
| `prepare/processLifecycle/executeStablePoint/beginStableAdmission/shutdown/rollbackPrepare` | ScriptSystem | 这些是必须保留的可读主流程，不能整块塞入 Execution |

## 5. 跨组件接口契约草案

### 5.1 线程、存储与错误约定

除 Ingress 的 producer post/active/lease 外，所有操作只在当前 Simulation owner/caller 的合法
安全区域执行。Simulation 的 TaskGraph 排他区域和 Hook authority 保留；不新增全局 busy、递归锁
或队列化全部重入。所有 fallible 操作使用 noexcept + expected/结构化错误。

实际排他依据是 Simulation build 向任务加入同一 `lux.simulation.execution` fence：Script Hook
取 write，其他执行任务取 read，并由 TaskGraph resource 依赖调度；caller affinity 只是执行位置。
迁移不得只保留 affinity 却丢掉该 fence，或把 fence 改成跨 Simulation 全局锁。

实例/方法/endpoint 视图不能越过其访问 scope；无调用中的容器扩容可使借用地址失效。
逻辑失效并不立即回收 backing。调用用户代码前取得保护，调用返回后重新查 id/代次/权限。
显式资源移交成功后只有接收方负责清理；局部未提交对象负责反向撤销，不引入通用事务框架。

### 5.2 操作边界

下表名字为设计名。O=owner 安全区；P=外部 producer；所有 reentry 都允许导致取消/退休，
但禁止绕过当前操作的物理回收保护。

| 操作 / owner | 线程、前置条件 | 返回与有效期 | 用户代码 / 重入 | 失败、回滚与后置条件 |
|---|---|---|---|---|
| `reserveInstance(input)` / Instances | O；有效 Entity 或 Simulation scope；配额及唯一关联校验 | 未发布 instance reservation；宿主 backing 稳定 | 预留不调用用户代码 | 失败无关联；成功尚无普通调用资格 |
| `prepareInstance(reservation, contracts)` / Preparer | O；借用 Instances 的 construction access | 当前实例的临时 prepared bundle，提交后转给 Instances | resolver/backend construct/attach/prepare 可重入；construction access 覆盖全程 | 每项 acquired 标记唯一清理者；外部停止或返回失败不发布 |
| `publishBindings(instance, prepared)` / Bindings | O；已开始 gameplay lifetime；只读 prepared 视图 | owned connection tokens；调用目标按完整 instance id 引用 | endpoint connect/disconnect 按可重入外部调用处理 | 部分成功逆向撤销；busy 保留尚未断开的 token，不伪造成功 |
| `acquireInvocation(instance, method)` / Instances | O；普通调用要求 ACTIVE、未关闭且 Entity 实际有效 | move-only `InvocationAccess`，仅不可变 prepared entry、host/scope 借用；退出归还 pin | 获取本身不调用用户代码 | 失败不执行；同代次退休后不再授予新访问 |
| 生命周期 construction/EndPlay access / Instances | O；受控构造或已领取退休资格 | 仅内部 Execution 可使用的生命周期 entry 访问 | invoke 可能重入；EndPlay 资格先消费 | BeginPlay 不成功不创建资格；EndPlay 错误记录后继续释放 |
| `invoke/resume` / Execution | O；持 InvocationAccess，调用级 quota/single-flight 有效 | step outcome；同步参数限调用窗口；挂起参数按 backend owned 操作保存 | 是；保留嵌套调用 | 回来重新验证 instance/continuation；未接管的 backend continuation 精确销毁一次；错误返回 System 故障退休 |
| `reserveAwaitable(instance, type, sourceKind)` / Execution | O；合法 instance access，结果大小/对齐/容量已校验 | 未提交 reservation；最终结果 backing 归 Execution | 不运行用户代码 | 来源登记失败 abort；内部 Event 不 mint 外部 completion lease |
| `registerEvent(instance, source, awaitable)` / EventWaits | O；validated prepared admission，targeted 时只能 self | 非 owning SourceToken；登记序号与路由在此提交 | 登记步骤不调用脚本 | 路由/槽失败撤销路由及 reservation；不留下半条 waiter |
| `registerTimer(instance, condition, completion)` / Timers | O；合法快照、duration、source reservation | Timer token；到期/取消前由 Timers 持有 | real-delay provider start 可能 eager 完成 | 外部启动前表示/容量拒绝；失败取消 reservation；NextStep/零时长不提前一个 step |
| `claimOccurrence(endpoint, target)` / EventWaits | O；本 occurrence cutoff 固定 | move-only ClaimBatch，按值读 id，不暴露 vector/span 可写 backing | claim 本身不执行 callback；其后 callback 可嵌套 | batch reservation 保留到退出；取消只使 id 无效，不破坏遍历；无每 waiter 重复 route 查找 |
| `writeResult(instance, awaitable, projection, frame)` / Execution | O；claim 仍有效、来源已验证 | 内部 ResultWriteAccess；唯一最终存储 | copy 可重入；pin 覆盖地址与析构有效期 | 返回后重验；失败/取消不发布 READY；释放未提交 bytes，pin 最后退出才复用 |
| `complete/associateContinuation` / Execution | O；身份一致且各自提交点合法 | 终态事实和至多一次通知 | 终态提交本身不调用 backend | queue 满在终态/关联写入前返回；来源按既有策略保留重试或故障退休 |
| `cancelWait(instance, awaitable)` / 窄装配操作 | O；允许重复/stale id | 无借用逃逸 | source completion lease 释放/cleanup 按可重入处理 | 先逻辑标记 cancelled，取出并清空 source token，再取消 Event/Timer/Ingress，最后回收结果；重复取消 no-op |
| `issue/revoke/stop`、`post` / Ingress | issue/revoke O；post P；stop 由 O 与 P 原子协调 | shared transport lease，仅 producer 接口可跨线程 | 不执行脚本、ECS、VM | closed/stale/duplicate 明确返回；full 保留可重试资格；stop 不等待未知 provider |
| `beginAdmission/peek/ack` / Ingress | O；由 System 在原 before/stable 时机触发 | 有界当前 window；消息视图仅到 ack，不能跨用户调用借用 ring cell | 不执行脚本 | 未发布 head 不等待、不越过；Execution 接受后 ack，backpressure 保留 head |
| `revoke/retire/reclaim` / Instances + System | O；System 聚合所有组件 quiescence | immutable retirement work；无记录写权限外泄 | continuation destroy、EndPlay、backend destroy、lease release 都可能重入 | 先进入不可重复清理状态，再调用外部操作；所有保护与资源计数归零后才复用 |

### 5.3 来源取消与 backend context 的闭环

`ScriptStepContext` 现有 create/discard/wait_event 三个入口继续使用，不扩展为运行时 service locator。
其 opaque context 收窄为私有、不可逃逸的调用服务视图，只能到达 Execution 和来源登记/取消操作；
不含完整 State、Registry、VM 或实例可写表。普通 Ability provider 路径不经过这层。

等待装配由 ScriptSystem 所在私有编译面实现，借用相关具体组件完成短事务：
Execution reserve → EventWaits/Timers register → Execution commitSource；逆向失败撤销。
discard 使用有限 Event/Timer/External 来源种类和 token，先清关联再调用来源取消，防止递归相互取消。
Event 成功先 detach 来源，再提交终态；timer 到期由来源 owner 摘除或在 backpressure 时保留原顺序；
外部完成只运输，最终状态仍由 Execution 决定。所有持有的 id 必须检查 runtime/instance/代次。

Timer 的 per-instance 索引所需身份从 owner-only completion authority 与 Execution 关联取得；
现有 Delay provider 仅接收 completion，不能假定它已经有公开的 instance getter。需要的窄访问
放在现有 completion access 契约内，校验 instance/A 对后再登记；不得让 Timers 读 Execution 表。

Instances 中稳定的 ScriptBehavior 和 prepared authority 创建仍需受控权限。
当前 [Backend][backend] 的 `ScriptBehavior::attach` 与 [Runtime][runtime] 的 admission 私有字段
只 friend ScriptSystem：迁移时用其窄构造/认证操作提交不可变结果，或单一的核心 access helper；
不得让多个组件互为 friend，也不能把全部字段改成 public。helper 只能 mint/validate 宿主或 admission，
不能获得实例容器。这属于 SR-3/4 最小头接口调整，保持 backend 操作表/C ABI 不变。

## 6. 四条流程与明确的状态转移

### 6.1 挂载、批次与回滚

**现状初始 prepare：** CREATED→PREPARING；buildLayout；连接三个 attachment 信号；按 mount 顺序
完成所有 initialize；按成功列表完成所有 BeginPlay；按列表 publish；按列表 activate；最后连接
Hook/Event endpoints；清批次队列，进入 PREPARED。[State::prepare][system]

initialize 中 host behavior 先设置正确 scope/host；验证 artifact、lifecycle、imports，建立 instance id；
backend createInstance 内部完成实际脚本对象 construct/attach；随后逐方法 prepare 和绑定签名校验；
成功进入 INITIALIZED。不要混淆设置宿主数据与 C++ 对象 construct 后的 attach。

| 起始状态 / 触发 | 当前可观察处理 | 目标资格与回滚 |
|---|---|---|
| INACTIVE，disabled | initialize 成功返回但不创建实例 | 没有 Begin/EndPlay 资格；SR-2 loader 不提交 disabled |
| INACTIVE，无法解析 / Entity 无效 | 回 INACTIVE，返回 WORLD_OBJECT_NOT_RESOLVED；prepare/lifecycle 将其 queueDirty 并继续其他 mount | SR-2 移至 loader 保留 pending；不把普通失败也当 pending |
| CONSTRUCTING，资产/契约/容量/构造/方法准备失败 | releaseMount/reset，返回原失败；prepare 全批回滚，增量 lifecycle 标失败 mount FAULTED | 无 gameplay 资格；仅已取得资源逆序释放；不发布 handler |
| INITIALIZED，BeginPlay 成功或未声明 BeginPlay | `gameplay_lifetime_started=true` | 普通调用仍未开放；声明了 EndPlay 者将来有一次调用资格 |
| INITIALIZED，BeginPlay 失败 | 失败实例无 gameplay 标记；prepare 回滚已成功 Begin 的其他实例 | 成功进入 lifetime 的调用 EndPlay，失败/尚未进入者不调用；不撤销用户已产生的世界副作用 |
| INITIALIZED，publish 失败 | 撤部分绑定/自己 attachment，释放该实例；prepare 回滚整批 | 保留已开始 gameplay 的 EndPlay；错误 mount/symbol/status 不丢 |
| INITIALIZED，publish 完成 | activate→ACTIVE，计数加一 | 只有此后授予普通调用/等待权限 |
| ACTIVE，脚本失败 | FAULTED，撤普通权限/等待，queueRetirement | 不立即释放当前调用对象；EndPlay reason=FAULTED |
| ACTIVE，attachment on_destroy | RETIRING、ENTITY_DESTROYED，撤权限并 queueDirty | 源组件仍可读时提取关联；不在信号中修改 ECS |
| RETIRING，安全物理清理 | 现有 beginRetirement/finishRetirement，最后 INACTIVE 或 FAULTED | 不重新按持久化身份追踪；SR-2 以后只有 loader 的新 Entity 输入才能重建 |

**现状增量 lifecycle：** 先接纳 fault retirement queue，再交换 dirty_current/dirty_processing；
对 dirty mount 收集 retirement 和 candidate；全部 beginRetirement 后逐个 finishRetirement；
RETIRE_ONLY 将候选留待之后而不创建；ALLOW 再批量 initialize→BeginPlay→publish→activate。
过程中记录 first_error，未解析项不遮盖另一个 fatal，观察者 follow-up 入下一轮 dirty。
这里的 retirement finish 顺序与 shutdown 逆序不同，必须分别保留。

**回滚故障：** rollbackPrepare 先尝试 disconnect；DISPATCH_ACTIVE/WRITER_ACTIVE 使其
ROLLBACK_PENDING 并返回 ENDPOINT_BUSY，不销毁仍连接的对象。断开成功才 releaseSignals，
逆序 releaseMount，回 CREATED。prepare 对 PREPARED 幂等、对 PREPARING 拒绝、对
ROLLBACK_PENDING busy。不能改成“所有失败立即清空 State”。

### 6.2 调用与借用

Hook lane 或 Event handler 提供完整 instance/method 目标。Execution 获取 InvocationAccess；
Hook 的 active continuation 限制只阻止同方法的 single-flight，Event 可以多飞。
同步调用使用 prepared invoke，resumable 调用使用 prepared step；不重新查 symbol/provider。

后端返回 COMPLETED 时不应残留 continuation；SUSPENDED 必须有合法 waiting_on 和 backend
continuation。准入失败精确销毁尚未转移的 continuation、取消等待并报告错误。
返回后重新查 instance 和 continuation；失效则停止发布/执行，不利用旧 record 指针继续操作。
resume 的 owned outcome 在本次调用窗口保持有效；再次挂起参数由 backend/generated ownership
保存，不能留住 packet、stack frame 或 Channel 的裸地址。

**现状与设计区别：** 现有 UserInvocationScope 保护 create/invoke/resume/copy/lifecycle 调用；
`destroyContinuation()`、`finishRetirement()` 中的 backend release/destroy 并非全部包在该 scope。
不能声称现状已经有完整的逐实例 teardown guard。SR-3/4 必须把 cleanup 重入纳入访问/回收协议，
先取得清理资格并标记 in-progress，再调用销毁函数；不使用整个 runtime 的全局禁止重入开关。
这项新增保护需按 §9 的窄用例验收，不能当作已复现崩溃报告。

### 6.3 等待—完成—恢复

令 A=Awaitable、K=Continuation；下表的“终态”是 READY 或 FAILED。CANCELLED 不允许正常恢复。

| 输入 / 中间态 | 提交点与结果 | 失败处理 |
|---|---|---|
| 预留 A；来源尚未登记 | A 的容量/最终存储已取得，尚未返回合法 wait | 来源失败 abort A；外部 provider 不得在验证之前启动 |
| 来源登记成功 | 建立 A↔source 非 owning 关联后对 backend 返回 waiting_on | 不留下仅有来源或仅有 A 的半次登记 |
| A=PENDING，无 K，先完成 | 校验结果后写终态；不入 ring | 重复完成 ALREADY_TERMINAL；非法结果不改状态 |
| 已有终态 A，后关联 K | 先验证 ring 容量，再关联并排一次通知 | attachWaiter queue 满返回 RESUME_QUEUE_FULL；取消 A 并销毁未接管/失败 K |
| A=PENDING，先关联 K，后完成 | 先验证 ring 容量，再写终态并排一次通知 | 不允许 READY 无通知；按来源策略保留重试或故障退休 |
| 取消与结果 copy 重入 | A 逻辑取消，释放来源/权限；write pin 保留 backing | copy 返回不得提交 READY；release_pending 到最后 pin 释放才 erase |
| 从 ring pop | 验证 instance、K、A 三者关系；take 将唯一结果 move 至调用局部并 erase A | stale 通知不调用 backend；现状仍消耗一次 pop 预算，不改为仅成功 resume 才算预算 |
| resume 再挂起 | 同一 K 关联新 A，使用相同 eager/late 协议 | 关联失败取消新 A、destroy K、故障退休 |
| resume 完成/失败 | destroy K、清 active_hook、归还 per-instance 配额 | 失败保留返回错误和 failures；不继续 resume-until-finished 当作销毁 |

**Event：** 每个 `dispatchEvent()` 先捕获 `event_wait_sequence` 和 claimed 起点；
仅从本 endpoint/target 的路由领取该 occurrence 已存在 waiter；然后普通 callbacks；
最后逐 id 完成仍有效 claim；嵌套 occurrence 使用自己的区间并恢复外层边界。
callback 新 waiter 不能消费当前 occurrence，但可消费同一 sealed batch 后续 occurrence。
claimed 取消后其物理遍历 reservation 必须保留；已有公式
`event_waiters.size() - active_claimed_waiters + claimed_event_waiters.size()` 的容量含义不能丢。

copy 直接写 A 的 owned bytes：StableSlotMap 保证 record 不因删除其他 record 移动，ResultWritePin
保证取消不回收正在写的 backing。copy 返回重验 stopping、release_pending、instance/ACTIVE，
再 erase waiter、finish A。当前 PreparedResumeType/ScriptOwnedResumeValue 是类型描述和 owned bytes，
没有通用递归 C++ 对象析构协议；本阶段不借“部分构造清理”引入 SR-5 值转换系统。

**Timer：** NextStep 取当前 step+1；SimulationDelay 使用 elapsed+ceil(ns)、minimum_step=step+1，
以 deadline、sequence 排序。零时间不会获得额外同一步恢复；realSeconds(0) 走 NextStep，
正 real delay 走 Scene 的 Process provider。现有 next-step backpressure 保留队头，
simulation-delay backpressure 将同一记录放回原排序位置，不跳过合法等待。

**稳定恢复的完整顺序：** Scene stable before 先 drain real-delay provider，再捕获 ingress
enqueue frontier；随后正常 lifecycle。Hook 交付事件后唯一 stable after 调用 executeStablePoint：
按 step 去重→必要时建立原 admission window→lifecycle→NextStep→simulation delay→external drain→
按 resumes_per_stable_point pop。**每次 pop/resume 后还会 drain 同一个 frontier 内的外部完成**，
不能把这次补充准入删掉，也不能捕获新 frontier。step=0 是 standalone fixture 特例，不能推广到生产。

外部 ring 的已 reserve 未 publish head 立即返回 unavailable，不等待、不越过后面的 cell。
post 成功只表示运输入队，最终 A 在 owner 应用时才完成。过期 id 丢弃；队列背压保留 head；
取消旧 ticket 后迟到消息不得误投新代次。transport 与终态所有权是两个不同层次。

### 6.4 退休和关闭

逻辑退休按当前触发时机发生：故障返回或真实 ECS attachment 销毁提交；不能提前在发出
DESTROY_ENTITY 命令时就退休。Instances 撤销普通调用/新等待；Bindings 失效；
EventWaits/Timers 撤来源；Execution 取消 A 和通知资格；Ingress revoke 旧 ticket。
纯 id 通知可留在 ring 被验证后消费，不要求为了“零队列”改变预算或顺序。

物理回收要求当前实例 invoke/resume/copy/construction/cleanup 保护均结束，且 EventWaits 的
领取遍历不再借用其资源。System 调各 owner 的 quiescence/finish 操作，不直接改计数。
顺序为 destroy continuation/owned 参数→有资格者一次 EndPlay→逆序 releaseMethod→destroyInstance→
释放 capabilities/event source/host 借用与 artifact lease→回收实例槽。代码 provider、Native module、
Lua VM owner 必须活过最后一个 destroy 调用。

现状 `beginRetirement()` 先取出并清除 gameplay 标记；`invalidateInstance()` 销毁旧 continuation，
`finishRetirement()` 才 EndPlay。批次保证所有要退休者先失效，EndPlay 不再调用另一退休实例。
EndPlay 失败只记录错误，不跳过其他资源清理；shutdown 的成功返回不抹掉 failures 记录。

shutdown：设置 stopping 并关闭 ingress；有 user invocation/write pin 返回 ENDPOINT_BUSY；
随后清 timer、断开 endpoint（可 busy 重试）、释放信号；按 mount 顺序 beginRetirement，
**逆序** finishRetirement；清池/队列，进入 SHUT_DOWN。新增 cleanup pin 必须保留这个忙碌重试契约。
析构对无法完成 shutdown 仍有 terminate 约束；用户回调只允许逻辑关闭，不允许销毁尚在返回栈上的
ScriptSystem 本体。外部完成 lease 可在关闭后活着，但只保活已关闭 transport。

## 7. 地址、资源依赖与私有构建面

| 资源 / 当前地址保证 | 迁移后必须保持的有效期 |
|---|---|
| State 是 unique_ptr；mounts/methods/buckets 在 buildLayout 完成后发布 | Instances/Bindings 的已发布 backing 固定；SR-2 后迟到 Entity 不能通过增长 vector 搬走旧宿主或 prepared entry |
| Awaitable 是 lux-cxx StableSlotMap，create 预留、tryEmplacePrepared 不补页 | 逻辑容量按 limits 检查，physical slot 可 page-rounded；ticket_capacity 使用实际 backing capacity；write pin 防复用 |
| continuation/instance SlotMap 记录会因操作失效 | 不跨用户调用保留其裸 record 指针；先保存 id/必要 backend handle，回来重新 find；记录 pin 与对象 backing 保证分别说明 |
| Lua prepared_calls/instances、C++ 对象池/协程池、Native instances/frames | 留在 backend；core 只决定调用与销毁时机；不得把 Lua 栈布局或 C++ coroutine frame 拆进通用组件 |
| ScriptBehavior 的 ScriptHostApi 指针 | host API backing 不移动；Instances 持行为对象直到 backend destroy 结束；Entity 是否有效每次依 host 契约检查 |
| Prepared capabilities 与 event_sources | 逐实例的 provider/endpoint 关联不跨实例缓存；所引用的 artifact/Simulation provider 比销毁更长寿 |
| Native 模块 lease | backend 的 module owner 保留至所有 continuation/instance 销毁；核心 artifact lease 不能替代 executable lease |
| C++ descriptor / 代码 owner | 当前是外部组合持有的 descriptor/backend；文档不虚构已有共享代码 lease；组合必须保证其生存期覆盖调用和销毁 |
| 外部 completion owner-only callbacks | 只有当前 owner 控制的 timer/调用路径可访问；外部 retained lease 仅 post，关闭后不能调用悬空 owner context |

SR-3/4 在 `engine/domain/simulation/builtin/script` 内增私有实现文件，继续一个
`simulation_script` production target；不为六组件各开 DLL，不安装其容器或 Lua 私有类型。
System 拥有组件并决定停机顺序，不能依赖成员默认逆序析构代替运行中退休。
runtime public `ScriptSystem` facade 保留；SR-2 的输入变更是明确例外，其他 C ABI 和 backend
descriptor 不随拆类改版。L0 Ability 模板、Lua/C++ projector 在自身包内演进，禁止跨 sibling pinclude。

## 8. SR-2 最小实施方案（设计交付，尚未编码）

### 8.1 包与类型归属

设计将持久化描述移至独立 L3 leaf `engine/scene/integration/script_description`，target
`scene_script_description`（SCENE / RUNTIME / DOMAIN）；不把 codec 塞入依赖 Process Timer 的
runtime bridge，也不让 Simulation 链接 Scene。公开 namespace 使用 `lux::scene::script`，
include 使用 `lux/engine/scene/script/`，不出现 domain 或层级编号。

`ScriptSystemDescription/Builder`、持久化 `ScriptMountDescription/Scope`、`EntityScriptMount`
和 codec/error/codec limits 迁入该 leaf。共同的 `ScriptMountId`（只作配置/诊断关联）、
HookScriptTarget/EventScriptTarget/ScriptBindingTarget/ScriptBindingDescription 留在 L1
`lux::simulation::script` 的独立 `ScriptBinding.hpp`；runtime 不 include 持久化头。
`ScriptBindingAuthoring` 的签名只需 binding targets、artifact、SimulationDescription、entity_scope
布尔量，改 include 即可保留在原语义包，不为文件名机械迁入 L3。

新 leaf 使用 L1 binding/description 合约、DOMAIN world identity 和现有 L0 序列化依赖；
`scene_script_runtime` 链接它；`simulation_script` 删除 description/codec 源和 world identity
直接链接/导出 find_package。world identity 其他合法用户保留。
更新父 CMake、安装导出和精确架构检查；不得放松整个 DAG 或新增禁词扫历史文档。

### 8.2 最小 runtime 输入及容量

以下是 SR-2 建议 API 的语义草案，替换旧 create 输入时一次迁完消费者：

```cpp
// 位于 L1 runtime 公开头；本文件中的草案，不是已存在 API。
struct ScriptRuntimeMount final
{
    ScriptMountId id; // 配置/错误关联，不用于查 World 或跟踪下一次物化。
    lux::asset::AssetId asset;
    ScriptInstanceScope scope;
    std::vector<ScriptBindingDescription> bindings;
};

struct ScriptRuntimeCapacityPlan final
{
    std::size_t mount_capacity{};
    std::size_t enabled_mount_capacity{};
    std::size_t binding_capacity{};
    std::size_t method_capacity{};
    std::vector<ScriptEndpointCapacity> endpoint_capacities;
};
```

这里 `ScriptEndpointCapacity` 是由 `ScriptBindingTarget target` 与 `std::size_t handler_capacity`
组成的冷期描述，按现有 endpoint 顺序列出各 Hook/Event 的 handler 上界；不含 Entity、资产或
WorldObjectId。它用于保留“尚无实例也已经知道未来 binding 容量”的现有能力。

create 保留 simulation/registry/clock/limits/artifacts/capabilities/backends/endpoints/host/real_delay，
以 capacity plan + 初始 resolved mount span 替换 `ScriptSystemDescription` 和 WorldObjectResolver。
create/批次输入的 span 只借到返回；runtime 冷期取得自己的 scope/asset/binding 值，禁止指向 loader
description。没有 self 的 Simulation scope 不用 NullEntity 冒充 Entity scope。

容量 plan 是当前 buildLayout 的派生存储预算，不是扩大用户配额：`mount_capacity` 取原描述
mount 总数（含 disabled），`binding_capacity` 取启用 binding 总数，`method_capacity` 保留原上界
`binding_capacity + 2 * mount_capacity`；实际方法仍按每 mount 的 symbol 去重。启用 mount 数仍受
原 instance_capacity 校验：`enabled_mount_capacity` 取完整描述的启用数，create 验证它不超过
mount_capacity 和 limits.instance_capacity，不能因初始 resolved 子集较小而推迟该容量拒绝。
disabled 不提交。保留原有界策略并对求和/乘法先检查溢出。
逐 endpoint 的 handler 上界从全部启用描述计算，不仅从当前已解析子集计算；因此 pending mount
未来使用的 Hook 仍可在初始 prepare 连接，Event 的 wait-only 连接条件也保持不变。
直接 runtime fixture 从自身有限输入计算同一 plan。一次预留对应 backing，迟到 Entity 使用
尚未发布的位置；物理 retirement 完成后才可回收位置。
这些位置是原 mount 实现的内部资源索引，不是多脚本 slot/product 管理体系。

不为每次 invoke 分配堆存储；不允许 mount 输入使已发布 host/method/bucket 搬迁。
容量不足返回既有 CAPACITY_EXCEEDED；错误类型新增只限替换 runtime 的
WORLD_OBJECT_NOT_RESOLVED 语义，使用 INVALID_INPUT/SCOPE_MISMATCH 表达非法 runtime scope，
持久化未解析属于 loader pending，不再混入 runtime failures。

### 8.3 加载侧提交与再物化

在已有 `ScriptRuntimeSystem` 私有实现中加入 resolved-input 装配，不新增 SceneSystem。
WorldObjectResolver 移到 L3 host contract，保存于 loader；未解析描述和它们的重试状态留在那里。
初始安装先解析所有可用 Entity，保留源 mount 顺序；create + prepare 仍执行原初始批次语义。
“一个 pending + 另一个 fatal”继续返回真实 fatal，不因 pending 提前成功。

增加一个低层 owner-only `mountResolvedBatch(span<const ScriptRuntimeMount>)` 装配入口：
仅供 loader/测试/显式 runtime composition 使用；不提供游戏脚本可调用的挂载、排序、热重载接口。
入口只验证并提交有限 resolved 数据到下一次既有 lifecycle 批次，不执行脚本、不自行 tick。
结构/容量预留失败不提交半批；执行阶段仍保留 processLifecycle 的逐实例错误与 first_error，
不承诺用户 BeginPlay 世界事务。validate 所有 Entity 的代次和单脚本关联；同批重复 Entity
或与仍存活关联冲突必须拒绝，不能覆盖旧实例。

loader 在已有 before/committed lifecycle 调用之前，按原候选顺序交付本次新物化输入；
不在 Event callback 或 worker 调用。初始列表按源顺序，增量候选按原 dirty 插入/去重/换队顺序，
不能一律改为排序或按 Entity 数值处理；fault retirement 保持原优先级。
本步结构提交产生的 Entity 在同一个原 committed 安全点可被 loader 观察并提交，不能为方便加一步
无条件延迟。loader 对每条已提交描述保存关联的 Entity 和 admission 状态，防止每 Hook 重复提交；
诊断 id 仅在 loader 关联失败，不要求 runtime 根据同一 id 寻找“下一个对象”。

为避免用可能截断的 failures 日志判断是否可以重建，装配面提供 owner-only
`queryMountStatus(ScriptMountId)`：返回当前这次配置提交的只读 admission/lifecycle 状态、
instance id 和 scope 值，不返回 record 或可写容器。该查询不解析对象、不追踪持久化身份；
状态直接来自 Instances/待提交批次的权威记录。loader 据此区分已提交、ACTIVE、正在退休、
已回收和 FAULTED；ACTIVE 拒绝覆盖，FAULTED 不自动重启，同批重复诊断 id 拒绝。
正常 RETIRING 的替代 Entity 可进入同一 lifecycle 的候选输入，但必须等旧实例物理回收后才
initialize/activate；不能为了接口简单延后到另一个 Hook。输入暂存同样有界，不移动旧宿主。
再次接受同配置 id 时必须伴随 loader 提交的独立有效 Entity，runtime 不据 id 自行 remount。

System 将原稀疏 dirty 队列发生的 mount 状态变化按插入顺序提供给 loader 的有界
`collectMountStatusChanges()` 只读装配入口；loader 消费后与自己的 pending 队列按原规则合并。
这只是交接原本已经存在的 lifecycle 通知，不另建消息总线或调度器，也不改为每 Hook 扫描所有
ACTIVE 实例。query 用于候选的最后重验，变化报告和 query 均不依赖可能截断的 failures 日志。

Entity 真正销毁时由原 attachment observer 退休旧实例。runtime 不再自动 queueDirty 重试
inactive authored mount；loader 解析到新的有效 Entity 后，重新提交全新关联。旧 Entity、
ScriptInstanceId、continuation 和 completion 权限均不可复用。故障实例不会因 loader 每次扫描而
自动重启；保留原 FAULTED 终止语义。pending resolver false 不改变现有非 pending 资产错误策略。

新增提交入口在 PREPARING、ROLLBACK_PENDING、dispatch/user invocation 中拒绝；关闭后 SHUT_DOWN。
接受 span 之后不保留任何 WorldObjectId、resolver、authored pointer，也不接受其 uint64 包装。
为保持 Scene private loader context 地址稳定，存储由 Scene runtime 的私有 owning 状态持有，
不要让绑定给 Simulation 的 context 随移动失效。

### 8.3.1 SR-2 开工契约（2026-09-06）

mountResolvedBatch 的成功是整批 owned 输入已接受，不是初始化或激活成功。预检和容量预留
不执行用户代码；错误不改变已接收配置、资源计数或反馈。每配置的资产、scope 类别与有序
binding 在第一次接受后冻结；同 ID 仅重关联新的 Entity，拒绝 ACTIVE 覆盖、FAULTED 重启、
资产替换与绑定编辑。提交与实际 initialize 前分别校验 Entity 代次及 attachment 冲突。

状态由 runtime 唯一拥有：当前实例 lifecycle/identity/scope/reclaim 与待关联 scope、提交序号、
提交结果分别表达。RETIRING 旧实例与 ACCEPTED 替代输入可以同时存在；完成物理回收后才
初始化替代输入。已接受 Entity 失效产生 REJECTED 结果，不创建 backend，由 loader 恢复 pending。
queryMountStatus 返回值快照、不消费；未知 ID 返回空。collectMountStatusChanges 按首次变脏
顺序复制到调用方 span，只消费已复制项，返回 written/remaining；零容量不消费。每个配置
最多一个队列位置；修订号递增。提交结果、退休身份和回收事实保留到消费，未消费的终态
阻止同 ID 下一次提交（ENDPOINT_BUSY）；不会因 failures 截断失去装配所需反馈。

完整 capacity plan 在 create 校验溢出、启用数量、总 binding/method 上界和 endpoint 预算。
一次准备 mount/method/binding/handler/反馈/待提交 backing。首次接受配置分配固定范围，
后续同 ID 复用；永不因晚到输入移动已发布宿主、prepared 或 bucket。配置范围本身保留到
runtime 销毁，实例资源只在实际退休后复用；每配置最多一个待关联 Entity，整批预留失败
不发布任何范围或输入。禁止在 invoke/resume/copy/claim/cleanup 保护期提前清理旧资源。

loader 只持持久化配置、解析进度和提交关联；不复制具有决定权的 lifecycle。初始顺序、
fault retirement 优先级、dirty 插入/去重/换队、RETIRE_ONLY 与既有 before/committed 位置
保持。以旧路径与新路径同场景的构造/Begin/绑定调用/continuation 析构/End/backend 析构
轨迹为验收依据，不能以容器顺序代替证据。real-delay/frontier 和恢复机会保持原位置。

本补充为实现契约，构建/测试证据另行记录；未运行的项目不得标为通过。

#### 配置位置补充

SR-2 输入另携 configuration_index：有限装配配置的原始位置，包含被跳过的 disabled/pending
位置。它不是世界身份或可编辑脚本槽位。首次迟到提交必须给出有效且未占用的位置；同 ID
重交不得改变位置。完整初始批次可省略并按其输入顺序分配，既有 ID 重交可省略并复用。
loader 从持久化描述序号生成此值，runtime 不回查持久化记录。初始批次按配置位置、增量
按原候选顺序、关闭按配置位置逆序；迟到到达顺序不再影响逆序清理。接口反馈队列仍按首次
变脏顺序消费，配置位置不替代 dirty 顺序。

### 8.4 持久化兼容与真实消费者

codec 迁移保持 `ScriptSystemDataCanonicalName="lux.simulation.script"`、schema/wire version 1、
目录布局和 record sizes（mount 56 bytes、binding 32 bytes）、编码顺序、enabled 和 scope 编码不变。
仍可将 bytes 放入 SimulationDescription 的通用 data；这不要求 L1 包认识其中的持久化类型。
公共 C++ include/namespace 的迁移一次完成，不留 alias/shim；旧 serialized bytes 继续读取。

| 当前消费者组 | 实际文件 / 入口 | SR-2 迁移 |
|---|---|---|
| Scene host + 安装 | [Scene header][scene-header]、[Scene bridge][scene] `ScriptRuntimeHost/installScriptRuntimeSystem` | L3 codec/resolver、pending/resolved 装配、原 hook 时序 |
| 直接 runtime 测试 | [runtime test][test-runtime]、[lifecycle test][test-lifecycle]、[continuation test][test-continuation]、[event test][test-event] | Entity fixture 直接 runtime input；再物化显式提交；旧业务断言保留 |
| 持久化 / authoring 测试 | [description test][test-description]、[binding test][test-binding] | codec/builder 用 L3；纯 binding 只依赖 L1 共同类型；增旧 wire golden |
| Simulation 与 backend 集成 | [hook execution][test-hook-execution]、[Lua coroutine integration][test-lua-coroutine] | 更新 create fixture；不改 Hook authority 和恢复机会 |
| Scene 回归 | [Scene runtime test][test-scene]、[Scene Lua test][test-scene-lua] | 持久化加载路径继续被测；六个失败单独复核 |
| Physics | `engine/domain/simulation/builtin/physics2d/test/physics2d_lua_integration_test.cpp`、`physics2d_flowforge_integration_test.cpp` | 测试装配输入迁移；Physics runtime 不依赖 L3 描述 |
| benchmark | `engine/domain/simulation/builtin/script/benchmark/script_runtime_benchmark.cpp`、`engine/domain/simulation/builtin/physics2d/benchmark/physics2d_script_benchmark.cpp` | 原规模/容量/工作量不改，只适配输入；测量脚本未知改动不合入 |
| installed consumers | `cpp-generated-script`、`cpp-coroutine-script`、`lua-script-packager`、`script-authoring`、`system-event-await-runtime`、`system-hook-script-binding` 下的 main.cpp | 直接 runtime 改 Entity 输入；需要编码描述者显式 find/link L3 leaf；验证安装后的唯一入口 |

此表限定实际含旧 mount/description/create 调用的消费者，不把所有 Native backend 单测都说成
需要改 API。Native/FlowForge 另通过现有真实集成和 consumer 验证通用执行协议没有语言分叉。

### 8.5 SR-2 编码顺序与验收

1. 抽出共同 binding 类型与 runtime input，设计限定的 L3 描述/codec leaf；同步 CMake 安装面。
2. create/buildLayout/initialize 改用 owned resolved 数据和派生容量；删除 runtime resolver/authored 依赖。
3. 一次接通 loader pending→resolved batch 与再物化，在原 hook 安全点提交；移走旧自动持久化重试。
4. 迁移表中消费者和 fixture，保存原断言对应关系；新增 runtime 无持久化依赖、双 Entity、无 self、
   同 Entity 冲突、旧代次、pending+fatal、批次顺序和旧 wire 的窄用例。
5. 全量 all、受影响测试、L3/runtime installed consumers 与依赖审查；不提前 SR-3 拆执行状态。

完成门槛：仅凭有效 Entity 可完成现有挂载，Simulation scope 保留，无持久化 resolver 进入核心；
未解析对象只在 loader；旧对象退休与新对象关联独立；headless 直接 runtime 不链接新的 L3 leaf；
持久化 bytes 兼容，旧 runtime 协议无 fallback。后续 SR-3/4 可以基于唯一新入口继续，不依赖双路径。

## 9. C01—C13 与真实测试追踪

全部为 **SOURCE_REVIEWED / NOT_RUN_THIS_ROUND**。文件中的 main 子场景不是独立 CTest 名，
下表不把函数名冒充测试注册。SR-2 迁移 fixture 时保留相同业务预期；补测项不是已存在符号。

| 契约 | 代码锚点 | 已读的实际测试 / 观察值 | 剩余窄用例与阶段 |
|---|---|---|---|
| C01 单图与排他 | [Simulation][simulation] build 执行区域、caller affinity、HookCallbacks；[Endpoint bridge][endpoint] authority | [hook_regions][test-hook-regions] `run()` 顺序 1..9、caller 检查、两个 Simulation barrier；[hook_execution][test-hook-execution] step/未绑定执行约束 | 迁移 owner bridge 后保留 worker live-state 不重叠探针；仅检查 thread id 不够；SR-3/4 |
| C02 通用协议 | [Runtime][runtime]、[C++][cpp]、[Lua][lua]、[Native][native] step/resume/destroy | [continuation][test-continuation] `testSyncAndContinuation/testAsyncAbilityInvocation`；C++ bridge、Lua coroutine、FlowForge 集成正式入口 | 同一 eager/late/cancel trace 通过三 backend；缺精确同场景配对时补在 SR-4 |
| C03 步号、唯一恢复、预算 | executeStablePoint/drainResumes；Scene bindSimulation | [Scene][test-scene] main：同一 Scene stable 重复调用不多 resume；零 elapsed 仍以 step 控制；[Event][test-event] `testResumeBudget` | 检查预算耗在 pop（含 stale）、每 pop 同 frontier 补接入；timer 更换 owner 后重跑；SR-4 |
| C04 claim/callback/complete | dispatchEvent/claimEventWaiters/completeClaimedEventWaiter | [Event][test-event] `testBroadcastSemantics` callback=1、stable 前 resumes=0、stable 后值 {42,42}；`testTargetedAndRetirement` callback 销毁后不恢复 | 显式记录 claim→callback→copy 顺序，callback 取消不同 claimed waiter；SR-4 |
| C05 cutoff 与 self | waitEvent 的 admission/target 验证；dispatchEvent cutoff | `testRegistrationCutoff` 值 {10,20}，但该测试用两次 deliver；`testTargetedScopeRejection`、`testTargetedAndRetirement` 错 target visit 不增长 | **缺同一次 sealed batch 两个 occurrence**：callback 新 wait 不吃第一个、应吃第二个；SR-4 |
| C06 单一结果跨 reset | AwaitableRecord::value、takeAwaitable、ScriptResumePacket | `testBroadcastSemantics/testResumeBudget`；continuation main 的 LargePayload：deliver 后 resumes=0，stable 后=1 | 明确在 Channel reset 后跨多个真实 step 恢复并校验 owned 值；无第二 payload owner；SR-4 |
| C07 可重入及地址 | UserInvocationScope、ResultWritePin、StableSlotMap；backend cleanup | `testCopyRetirementPin` 写期间 pin=1/deferred=1/live A=0，结束均零且不 resume；`testCopyOtherRecordRemoval` 两次 copy、8 bytes、0 completion leases；`testCopyNestedAdmission` 新建 3 waiter 不扩 backing | cleanup/lease release 中嵌套调用、退休、shutdown；构造/BeginPlay 中同类行为的明确析构轨迹尚不足；SR-3/4 |
| C08 身份与权限 | instance/A/K generation、admission scope/layout_epoch、ingress ticket | `testPreparedAdmissionProvenance`；`testIncarnationAndPendingContinuation` 旧 completion INVALID_ID、旧 K 销毁；[Ingress][test-ingress] main 同 slot 新 generation 拒旧消息 | SR-2 加无需 World 身份的 Entity 复用；prepared 跨 runtime/artifact/layout 继续拒绝 |
| C09 内外完成 | createAwaitableRecord(false)、supportsExternalResumeLayout、ExternalCompletionRing | `testCopyOtherRecordRemoval` capability_constructions=0；[continuation][test-continuation] `testExternalAdmission<…>` provider_starts Supported?1:0；[Ingress][test-ingress] full 保留 active、duplicate 拒绝、frontier 之外留待下次 | 外部 close/post 用 rendezvous 明确中间态，不能依赖随机 race 命中；SR-4 |
| C10 构造与生命周期 | initializeMount、C++ createInstance construct/attach；begin/finishRetirement | [lifecycle][test-lifecycle] `testInitialLifecycle` first_begin_create_count=3、ends/destroys=3；`testOptionalAndFailureLifecycle` 第 2 Begin 失败后 ends=1/destroys=3；end-only ends=1；EndPlay 失败仍 destroys=1 | attach/prepare/publish 每个失败点与清理重入补计数/顺序；SR-3 |
| C11 prepared 热路 | invoke、waitEvent prepared admission；backend prepared calls；[生成模板][cpp-template] | `testPreparedAdmissionProvenance`；[Lua prepared storage][test-lua-storage]、[C++ prepared shape][test-cpp-shape]；generated installed consumers | SR-3 改 prepared owner 后检查 provider 查找仅冷期；SR-6 用实际产物/同量 workload 性能证据 |
| C12 批次/结构/错误 | prepare/processLifecycle；Simulation applyEcsCommands/committed/reset | `testPendingDoesNotMaskFatal`；`testInitialLifecycle`；[hook_regions][test-hook-regions] order 1..9、NextFrame 第 2 步出现、command failure 的 producer/command 保留 | SR-2 loader 替代 resolver 的 pending+fatal、再物化顺序与原断言对应；不放宽错误 |
| C13 容量与回收 | 各逻辑限额、prepared backing、reserved claims、queue error 分支 | `testCapacityAndCancellation` 配额归还后第二 Hook 可挂起；`testResumeQueueFailure`；`testCopyNestedAdmission` waiter capacity=1 拒嵌套占用；`testOutputSensitiveRetirement` visit 按 owned 资源 | timer stale 记录撤销后的容量/计数轨迹、有限生产停止后按真实稳定点 drain；SR-3/4 |

补充必须保留的真实测试：`testCopyShutdownAndFailure` copy 中 shutdown 返回 ENDPOINT_BUSY，
回调后 pin/deferred/A/waiter 均零且不恢复；`testPrepareRollbackBusy` 覆盖断开 busy 后的 rollback 状态。
`script_ingress_frontier_test.cpp` 明确制造 reserved-but-not-published head，连续 front 返回空，不越序；
publication 后只消费捕获 frontier 内两条，第三条留队，再验证复用代次和 stop。
该测试的手工 ring 中间态是运输协议探针，不代替完整 backend/provider 关闭竞态测试。

### 9.1 已发现差距的归类与处理

| 编号 | 分类 / 实际依据 | 处理边界 |
|---|---|---|
| G01 | 目标差距：RuntimeMount::authored、world resolver 和自动 pending 重试仍在 core | SR-2 按 §8 移出；不是在 SR-1 中偷偷删字段 |
| G02 | 所有权差距：InstanceRecord 聚集执行/事件索引，active_hook 混在 PreparedMethod | SR-3 建窄边界，SR-4 正式迁移各 owner |
| G03 | 目标差距：startSimulationDelay 满时才清 inactive completion；NextStep 到期才摘除；缺 per-instance timer cancellation | SR-4 显式来源取消；单列其回收时机/统计影响，不伪称完全相同的物理容器状态；不改变 deadline、恢复次数或扩大限额 |
| G04 | 验证缺口：destroyContinuation/finishRetirement 外部 cleanup 的完整重入保护尚未由当前 scope/测试证明 | SR-3/4 先补有界 reentry trace，再加窄回收保护；不得未经复现称已知崩溃 |
| G05 | 既有规范差异：[State][system] `waitEvent/startSimulationDelay` 有局部 bad_alloc catch，而 [AGENTS][agents] 禁止热路径 try/catch | SR-1 如实登记；相关 owner 迁移时将可能分配的存储准备留在 fallible factory，热路径有界操作不抛；若无法保持容量/错误语义，相关编码子项阻塞，不能全局关异常或吞错 |
| G06 | 历史失败：六个 Scene Lua 注册在旧资格输出中失败 | 编码前单独复核并归因；不拿未成功的路径作等价性能参照，不扩大成修全部历史 S6 待办 |

## 10. 后续验证和阶段边界

SR-1 本轮执行的是源码/元数据/哈希核查与文档校验。生产构建、单测、安装、性能全部
NOT_RUN_THIS_ROUND；Debug/额外 Release/Android 为 NOT_REQUESTED_THIS_ROUND。
没有数据支持“无性能回退”或“全部行为通过”，不作此结论。

之后编码每阶段必须全量 `cmake --build <build> --target all -j 4 -- -k 0`，
仅 RelWithDebInfo；CMake 变化第二轮必须 `ninja: no work to do`。测试/实机与构建不得并行；
构建失败不能继续运行旧 exe 当新证据。测试断言保持 `/UNDEBUG` 等局部约定，不全局启用 _DEBUG。
修改 modules 公共头后同步 Debug、RelWithDebInfo、Android 安装 include；这不代表验证 Android 构建。

正式 foundation/closure 先对 clean tracked commit 执行 ValidateTrackedSnapshot，再从该 commit
独立 clean clone 配置。不能直接重配置当前 q2/d 到另一工作树，不能借未跟踪/被忽略文件补源码。
source/config/dependency/driver 任一变化都重新绑定结果身份，原始日志与表格放仓库 evidence 或可取附件。

SR-2 验证 runtime identity 与加载/codec/consumer 闭环；SR-3 验证实例、准备、绑定和失败资源；
SR-4 验证所有通用挂起协议与来源；各阶段仅跑受影响测试集，但 all 构建规则不变。
SR-5 的反射双向转换独立批准，不让 Execution 增加逐字段转换。

SR-6 才做正式配对性能：沿现有 benchmark 选同步、prepare、Event fan-out、suspend/resume、退休，
影响图时加 Physics；固定 workload/seed/容量/VM/worker/预算/warmup，至少五组独立进程配对。
保留实际调用/恢复完成数、backlog、错误、allocation/bytes 和 READY→resume 延迟；
p50/p95/必要 p99 按 workload 报告，不平均多个 p95。诊断探针与正式计时分开。
本轮不运行旧测量脚本，不恢复历史冻结/合并任务，不自动推送或合入 main。

## 11. 阶段日志与交付判定

| 本轮步骤 | 结果 |
|---|---|
| 复核批准范围、AGENTS、Engine/依赖/工作区身份 | 完成；固定 54de8af / 3100f54；远端差异和未知修改分开记录 |
| 沿 Scene→Simulation→ScriptSystem→三个 backend→生成/consumer 查源码 | 完成；实际字段、函数和所有权落在 §2—4 |
| 接口、四条流程、稳定地址和清理关系 | 完成设计；现状不足明确列为 G03—G05 与补测，不假称已实现 |
| C01—C13 与实际断言映射 | 完成源码核查；测试未运行；同 sealed batch、cleanup 重入等窄缺口明确 |
| SR-2 输入/loader/包/消费者方案 | §8 给出最小单路径设计；尚未修改 public API、CMake 或 codec |
| 本轮实际变更 | 仅本设计文档；原有工作区改动不属于交付 |
| 文档验证 | 33 个引用目标存在、33 个引用名均有定义、21 个引用测试符号在源码中找到、代码围栏成对、无尾随空白；结果仅说明文档自洽，不代表 C++ 编译通过 |

仓库 `.gitignore` 的 `.internal` 规则会忽略新文档；交付时只将本文件强制加入暂存区，
使其进入可审阅 diff，不修改忽略规则、不加入其他工作区文件。本轮不创建 Git commit 或推送。

自查结论：设计不允许组件借完整 State 或 mutable getter；prepared 与执行状态分开；
身份验证与 backing 有效期分别有 owner；所有来源有成功/失败/取消/退休/关闭闭环；
回收不要求 backend 恢复到结束。SR-2 边界明确，SR-3/4 保留执行协议和安全检查。

下一步是独立审阅本文件，尤其 §5 的窄接口、§6 的可观察顺序、§8 的 loader 批次与容量派生。
只有收到 SR-2 实施授权才执行 §8；本阶段完成不代表代码重构或最终性能资格完成。

[agents]: ../AGENTS.md
[system]: ../engine/domain/simulation/builtin/script/src/ScriptSystem.cpp
[description]: ../engine/scene/integration/script_description/include/lux/engine/scene/script/ScriptSystemDescription.hpp
[codec]: ../engine/domain/simulation/builtin/script/src/ScriptSystemDescriptionCodec.cpp
[scene]: ../engine/scene/integration/script/src/ScriptRuntimeSystem.cpp
[scene-header]: ../engine/scene/integration/script/include/lux/engine/scene/ScriptRuntimeSystem.hpp
[simulation]: ../engine/domain/simulation/composition/src/Simulation.cpp
[runtime]: ../engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptRuntime.hpp
[backend]: ../engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptBackend.hpp
[endpoint]: ../engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptEndpointBridge.hpp
[ingress]: ../engine/domain/simulation/builtin/script/pinclude/lux/engine/simulation/script/ExternalCompletionRing.hpp
[cpp]: ../engine/domain/simulation/scripting/cpp_static/src/CppStaticScriptBridge.cpp
[lua]: ../engine/domain/simulation/scripting/lua/src/LuaScriptBackend.cpp
[native]: ../engine/domain/simulation/scripting/native/src/NativeScriptBackend.cpp
[ability-template]: ../modules/function/script/core/template/script_ability.template
[lua-template]: ../modules/function/script/core/template/script_ability_lua.template
[cpp-template]: ../engine/domain/simulation/scripting/cpp_static/template/cpp_static_script.template
[test-runtime]: ../engine/domain/simulation/builtin/script/test/script_system_runtime_test.cpp
[test-lifecycle]: ../engine/domain/simulation/builtin/script/test/script_system_lifecycle_test.cpp
[test-continuation]: ../engine/domain/simulation/builtin/script/test/script_system_continuation_test.cpp
[test-event]: ../engine/domain/simulation/builtin/script/test/script_system_event_wait_test.cpp
[test-description]: ../engine/scene/integration/script_description/test/script_system_description_test.cpp
[test-binding]: ../engine/scene/integration/script_description/test/script_binding_authoring_test.cpp
[test-ingress]: ../engine/domain/simulation/builtin/script/test/script_ingress_frontier_test.cpp
[test-hook-execution]: ../engine/domain/simulation/composition/test/hook_execution_test.cpp
[test-hook-regions]: ../engine/domain/simulation/composition/test/hook_regions_test.cpp
[test-scene]: ../engine/scene/integration/script/test/scene_script_runtime_test.cpp
[test-scene-lua]: ../engine/scene/integration/script/test/scene_script_lua_runtime_test.cpp
[test-lua-coroutine]: ../engine/domain/simulation/scripting/lua/test/lua_coroutine_integration_test.cpp
[test-lua-storage]: ../engine/domain/simulation/scripting/lua/test/lua_prepared_storage_test.cpp
[test-cpp-shape]: ../engine/domain/simulation/scripting/cpp_static/test/prepared_shape_test.cpp
[history]: s6-test-pruning-2026-09-06.md
[history-tests]: evidence/s6-test-pruning/developer-ctest.txt


## 12. SR-2 实施记录（2026-09-06）

§1—7、§9 的源码观察属于 54de8af 基线；§8 的 SR-2 补充与本节描述新入口。
本阶段未实施 SR-3/4 owner 拆分，未增加 scheduler、Hook、恢复预算或 backend ABI。

### 12.1 实际边界与交接

- `simulation_script` 公开 `ScriptBinding.hpp`、`ScriptRuntimeInput.hpp`；持久化头和源码已迁到
  `engine/scene/integration/script_description`，安装包名 `lux-engine-scene-script-description`，
  target `lux::engine::scene::scene_script_description`。codec/builder 测试也归此 leaf。
- L3 冷期 `planScriptRuntimeCapacity(description)` 计算完整预算；`resolveScriptRuntimeMounts`
  只在装配侧解析并产出值。直接 runtime fixtures 和四个直接 installed consumers 使用 Entity
  输入，runtime 无持久化描述、WorldObjectResolver、WorldObjectId 或 authored 指针。
- Scene 私有 Loader 保存描述位置、解析候选和输入副本；只有 runtime 决定 lifecycle。
  pending/processing 采用既有换队和去重方式，ACTIVE 配置不被反复扫描解析。提交前消费全部
  有界反馈，再查询候选权限；本次接受的配置按 admission 序号参与既有 lifecycle 初始化批次。
- RuntimeMount 固定数组按 configuration_index 布局；binding 描述与运行时索引、prepared method
  预留到完整预算。配置首次接受后保留范围，重交仅更新 Entity 关联，不重新分配配置范围。
  Entity 关联索引同时保留受保护的旧关联和一个替代关联，按 2×enabled 上界预留。

| 接口 | 线程、权限与借用 | 用户代码与重入 | 失败、消费和后置状态 |
|---|---|---|---|
| create | owner 冷装配；输入 span 仅借到返回，独立复制 scope/asset/bindings | fallible factory 准备 backing；不执行脚本 | 预算/位置/身份/endpoint 校验失败不返回 runtime；成功仅建立 owned 初始输入 |
| mountResolvedBatch | PREPARED owner；初次迟到提交必须提供配置位置；已知 ID 的资产、scope 类别、有序 bindings 不变 | 不调用用户代码；dispatch/invoke/prepare/rollback 拒绝；不自行跑 lifecycle | 先整批预检再无分配提交；结构错误 INVALID_INPUT/SCOPE_MISMATCH，容量 CAPACITY_EXCEEDED，忙碌 ENDPOINT_BUSY，关闭 SHUT_DOWN；失败不发布半批 |
| queryMountStatus | owner 安全边界；按值返回 optional 状态；未知 ID 为空 | 不执行用户代码；dispatch/invoke 中忙碌 | 不消费；保留当前实例状态与独立提交结果；shutdown 完成后仍可读，直到 runtime 销毁 |
| collectMountStatusChanges | owner 安全边界；调用方输出 span 仅借到返回 | 不执行用户代码；无分配、无通知回调 | written/remaining；按首次变脏顺序，只确认已复制项；零容量不消费；确认后才允许覆盖该配置上一次提交终态 |

status 的 current state/instance/scope 与 submitted_scope/submission_state 独立。
RETIRING + ACCEPTED 不代表旧实例回收或新实例初始化。reclaimed 在真正完成依赖顺序清理后
才置 true；retired_instance 保留退休身份。ACCEPTED 输入在 initialize 前失效则 REJECTED，
无 backend 构造；loader 收到后恢复解析候选。FAULTED 不自动重新接收。
配置位置与 ID 都不是世界身份；没有根据任一数值重新查找 Entity 的 runtime fallback。

`processLifecycle` 增加现有 result write pin/claimed 窗口的显式忙碌保护，清理调用处保留
UserInvocationScope，避免新装配入口在 cleanup 重入时复用资源。没有提前释放受保护存储。
新增 stats 记录 configured/pending 数量及 mount/method/binding/feedback backing 字节；这些
字节仅表示列出的预留数组，不冒充 backend、VM、DLL 或整个进程的总分配量。

### 12.2 旧断言到新入口的对应

| 原断言或场景 | 新入口及保留的业务预期 |
|---|---|
| testInitialLifecycle | create(capacity, resolved) → prepare；所有构造先于首个 Begin，begins/ends/destroys 不变 |
| testOptionalAndFailureLifecycle / testPrepareRollbackBusy | 同一 prepare/shutdown；Begin 失败不补 End，busy 期间不提前释放，原析构/lease 计数保持 |
| testIncarnationAndPendingContinuation | 旧 resolver 自动再找 Entity 改为 collect + mountResolvedBatch；旧 continuation 全销毁先于 End，旧 completion 仍 INVALID_ID |
| testPendingDoesNotMaskFatal | 未物化配置由装配方保留；只提交有效替代 Entity；另一配置 Begin 失败仍返回 INVOCATION_FAILURE，原计数保留 |
| runtime test 的 destroy/recreate | 显式新 Entity 重交；active=1→2、creates=3、prepares=10 与释放次数保持 |
| continuation 的 endpoint_missing | endpoint 布局在 create 校验，旧 prepare 的 SCRIPT_ENDPOINT_NOT_FOUND 移至 create；backend creates=0 保持 |
| testTargetedAndRetirement / testPreparedAdmissionProvenance | 显式再提交后原 payload {3,9}、旧 admission UNDECLARED_SOURCE、旧 Entity 不命中新实例仍成立 |
| testCopyRetirementPin / OtherRecordRemoval / ShutdownAndFailure / NestedAdmission | 保留原 pin、copy、busy、waiter/lease 数量与不恢复断言；不改结果所有权 |
| Scene runtime 的长期 pending | 旧 delay、稳定点和 derived 计数保留；增加 L3 resolve→ACTIVE、ACTIVE 不反复解析、新 Entity 得到新 instance 的断言 |
| generated C++ / coroutine installed consumers | 新 Entity 显式交接后仍检查 attach/begin/end/destroy 一次、旧 generation 与 continuation 拒绝 |
| description/authoring codec | 原断言搬到 L3；增基线编码 288 字节 golden，SHA256 8002ffd5678f822d79949b4d0a36d1687613216af4b26b876f967fa24cc76e52 |
| benchmark churn | workload/规模/预算不变，rematerialize 内显式提交；原 creates/destroys/begins/ends 业务计数保持 |

新增 testOwnedRuntimeInput、testResolvedBatchProtocol、testResolvedInputExpiryAndReuse：验证输入销毁
后可 prepare；无半批与容量失败回滚；查询不消费、部分反馈、忙碌终态；RETIRING+ACCEPTED、
RETIRE_ONLY；初始化前失效；64 次重建后 backing 不增长且资源回收闭环；同 ID 编辑被拒绝。
安装新增 script-runtime-input 与 script-description，分别检查 runtime 无 L3/World 导出依赖、
描述 leaf 无 Process/Scene runtime 导出依赖。

### 12.3 验证记录与限制

新建基线：`build/RelWithDebInfo/sr2-baseline`，源码为独立 clean clone 的 54de8af。
Toolchain 106/106 通过；Developer 112/118 通过，两者全量 all、第二轮 no-op 和安装均完成。
六个失败均为 scene_script_lua_runtime 的默认/worker/interpreter 注册，在原第 487 行
`activeContinuationCount() == 0U` 失败；没有修改该预期。

工作树迭代证据位于 `build/RelWithDebInfo/sr2-work/d`；ctest-6 为受影响 102 项，只有六个
历史 Scene Lua 失败（新文件第 489 行、同一断言）。此前迭代失败日志保留，不能用其二进制
冒充最终候选。初始输入误入 dirty 队列造成的 Simulation scope 回退已修正；遗漏的 fixture
显式重交已补齐。该工作树记录不是 clean commit 的最终资格。

`cmake/RunScriptSR2Probes.py` 从各自固定源码复制诊断 fixture，链接各自安装库；不修改基线
checkout。sr2-probes-1 的 initial/failures/signatures/incarnations/pending-fatal/cross-batch
六组逐事件轨迹完全相等；双方 v1 编码字节相等。五组独立进程小规模装配测量保留原始数据，
有实际新增交接开销，不据此宣称所有路径无性能变化。

最终 clean commit 的构建、安装、consumer、配对性能结果及原始日志清单在验收完成后补记。
仅 RelWithDebInfo；未改 modules 公共头，三前缀同步规则未触发；未进行 Android 构建。
