# V5 A1：执行 cell 实验记录

## A0 身份与边界

本实验分支为 `codex/s6-execution-cell-a1`，从 `f92853ea8361a7dc90ce0da072497d3722c29f88` 创建。
A 是 `f7d2815bdd2025ee23a7c11449def822413f58e9` 的 v4 匹配产物；两份既有独立镜像
`script-v4/final-image`、`script-v4/final-flow-image` 的 44 个清单文件已逐项核验 SHA-256。
本轮 start.json 记录镜像、原 main 工作区修改及固定依赖清单身份。文档提交不作为测量身份。
沿用 [v4 结果](../script-v4/RESULT.zh-CN.md) 与 [v4 注记](../script-v4/NOTES.zh-CN.md)。

只改变 ScriptExecution 的结果与执行承载以及 Event/Timer 目标。Lua55 VM、GC、allocator、codec、
Native ABI6、backend frame、脚本资产及所有调度顺序固定。旧架构只存在于 A 镜像，没有运行时选择器。

## 合同与源码映射

下列位置以 A 的 `engine/domain/simulation/builtin/script/` 为根；最终实现仍在相同 owner 内。

| 合同 | A 的实际操作 | cell 实现要求 |
|---|---|---|
| 方法执行后才接纳 C | ScriptExecution::invokeStep / beginSuspension | 栈上 scope 不持 C；保留副作用、错误与 destroy→discard 顺序 |
| 独立的 A/C 配额 | reserveAwaitable / beginSuspension | 两个薄目录分别接纳；单一 C+A 稳定 bank，禁止隐藏完整旧池 |
| Event 失败优先级 | waitEvent / ScriptEventWaits::reserve / registerWait | source 验证→Event preflight→A→source commit |
| Timer 失败优先级 | ScriptTimers::startLocal / planWait / registerWait | A→duration/deadline/容量→source commit |
| 第一次本地挂起 | beginSuspension / attachWaiter | 已存在 provisional cell 原位晋升；其他 cell 的未绑定等待仍可关联 |
| 连续等待 | resumeOne / takeAwaitable | 先移出值并还 A，再执行用户恢复代码；同 execution 重新装填 local wait |
| pin 与物理接纳 | ResultWritePin / eraseAwaitableRecord | release_pending 不算 active，但最后 pin 退出前仍占 A 和 cell |
| Event 完成 | completeClaimedEventWaiter | claim→callbacks→copy/complete；copy 前后权限；queue-full 取消并 fault |
| Timer 完成 | ScriptTimers::completeDue | 回调前复制目标；queue-full 保留来源，既有稳定点重试 |
| 外部完成 | drainExternalCompletions / ScriptCompletionIngress | 保留 transport、frontier、lease、同容量；boxed target 指向同一执行 |
| 单一恢复队列 | ResumeRing / ResumeBatch / ScriptSystem stable point | stale pop 消耗预算；每 pop 后沿同 frontier 接纳；不去重 |
| 执行清理 | destroyContinuation | owner unlink→hook 解锁→归还 C→backend.destroy；重入前不保留旧 body 引用 |
| 实例清理 | invalidateAdmission / invalidateInstance | 先按 Event/Timer 顺序取消来源，再等待清理与 continuation 析构 |
| 未选中的等待 | invokeStep COMPLETED / attachWaiter | 保留 A 角色；scope 析构不取消，execution 退出后可降为 wait-only |

## 私有存储与借用

bank 一次准备 C+A 个地址稳定的 cell；header 的 cell epoch、wait epoch 位于 union 之外。
union 包含 execution + 可选 inline wait，或 boxed wait。两个薄 SlotMap 目录仅保存稳定 ticket，
公开 32 位 slot/generation 不变。来源目标持 cell/wait epoch，不能保存移动目录记录的指针。
inline wait 保存一份类型和有效大小、32B 字节及明确的 attached execution ticket；承载它的 cell
有 execution 角色不等于该 wait 已有消费者。额外等待与无精确 scope 的入口走 boxed。

StepInvocationScope 是调用栈借用；ScriptStepContext 的 owner 仍为 ScriptExecution，另由内部
访问器签发精确 scope 关联。初始调用不分配 cell，无用户代码的方法完成不消耗 A/C。
返回用户代码后以完整 ticket 重取状态，不跨用户代码缓存 ACTIVE。

## 结果与测试登记

A0–A6 已实际实施；最终 clean 资格源码为 `caf34bd307912450d894da922b59af4238154508`。
实现与已测合同通过，成本结果为 **REGRESSION**，不建议采用本候选替代 v4。详见
[RESULT](RESULT.zh-CN.md)、[实际数据](result.json) 和 [T01–T36 对应关系](TEST_MATRIX.csv)。
v4 scalar 回退与 Native metadata 债务仍由原报告登记，不归因于本实验、不在本轮顺手修正。

## 最终实际 owner 与调用链

`ScriptOperationStorage.hpp` 是 ScriptExecution 的私有存储实现，不是新的 runtime/scheduler owner。
Instances/Bindings/Preparer 的 authority、prepared、token 与资源移交没有迁出；EventWaits 保有路由、claim、
序号及取消；Timers 保有 step/deadline/heap/FIFO；CompletionIngress 保有外部 publication/transport。
ScriptSystem 的 stable point、每次 pop 后同 frontier 接入及各来源处理顺序未改。

| 操作 | v4 | A1 实际入口 |
|---|---|---|
| 首次 Event 登记 | waitEvent → reserveAwaitable → 完整 StableSlotMap AwaitableRecord → source register | waitEvent(context) → scopeFor(context) → reserveAwaitable → Awaitables::admit：先 A 目录，后 provisional cell/local body → registerWait(id, LocalWaitTicket) |
| 首次挂起 | beginSuspension → 独立 ContinuationStorage.tryEmplace → attachWaiter | 原晚接纳点 → Continuations::tryEmplace：先 C 目录，选中可晋升 cell 后 emplace execution role → attachWaiter |
| Event 完成 | claim → callbacks → 按 AId 找结果 → copy/终态 → ready | 相同 occurrence 顺序 → checkedLocalWait(ticket, AId, owner) → ResultWritePin → copy 前后 authority → 终态 → ready |
| Timer | A → planWait → Timer source；到期通过 association 完成 | 同样的 A/参数/来源失败优先级；association 携 local ticket，completeDue 先复制 association，再进入 Execution |
| take/resume | AId/CId 查找，move owned result，erase A，调用 backend | ready 完整 C/A/owner + cell/wait epoch → 验证 body → takeValue 到栈上 outcome → erase A → backend.resume |
| 后续本地等待 | 新独立 ARecord，关联现有 CRecord | 本次 StepInvocationScope 携原 execution ticket；A 重新接纳，原 cell local wait 重装填、wait_epoch 增长 |
| 外部或额外等待 | 独立 ARecord | 同一个 A 目录、同一 cell bank 的 boxed arm；外部线程仍只持旧 completion，不发布 cell 指针 |
| destroy | unlink → hook 解锁 → C erase → backend.destroy | 顺序不变；先保存 backend/身份，erase execution role，空 cell 归还；仍有 A 的 cell 降为 wait-only |

**没有删除消费者关联的语义。** `attached_execution` 与承载 wait 的 cell 明确分离，支持另一合法执行使用
未绑定 token、承载 cell 自己正在 boxed wait 上挂起、COMPLETED 调用留下未绑定等待。被取消但有 write pin
的 A 目录项和 cell 仍保留，`active_awaitables` 下降不会提前给新登记腾出物理槽。

## 内部接口合同与角色提交点

| 接口 | 权限、寿命、失败与提交点 |
|---|---|
| StepInvocationScope / scopeFor | 仅 owner 执行线程；栈上绑定准确 ScriptStepContext 地址和完整实例身份。方法入口不接纳 A/C，不取得 cell。嵌套 frame 不能借用另一个 context 的 scope。析构只撤 scope，不取消未选中等待。 |
| Awaitables::admit | 仅已通过公共/来源资格验证的 owner 调用；先预留 A 目录，后选择/取得 cell。32B、max_align_t 内且有准确 scope 的合法本地值可 inline；external、无 scope、过大/超对齐、local 已占用走 boxed。失败撤销本次目录/body；不执行用户代码，spill 仍可返回分配失败。 |
| Continuations::tryEmplace | backend step 已返回 SUSPENDED 后才执行；每实例限制仍在 beginSuspension 检查。C 目录成功后晋升或取得 execution cell，attach 失败保留原 destroy/discard 次序。不能将 hosting role 当成 token 已有消费者。 |
| ScriptCellDirectory | 固定容量 vector，条目只有票据、free link、generation、occupied；运行中不增长/移动。C、A 独立容量，不使用公开 token 高位分类。generation 耗尽退休槽，不 wrap。 |
| checkedLocalWait / cells.wait / cells.execution | 仅来自 owner 内部签发的 ticket；先检查整个 bank 生命周期有效的 header owner、epoch、kind，再读取 body 和 wait epoch / CId。公开句柄、外部完成仍经目录和完整身份。不是任意原始指针的公共验证器。 |
| takeValue / erase | 完整结果移出到栈上独立 outcome；先撤来源与 owner 链并归还 A，再调用用户 resume。不能为了零拷贝保留旧 inline payload 跨用户代码。最后 pin 退出才允许物理 erase。 |
| ready / resumeOne | 一条 FIFO，条目64B，包含独立执行/等待目标；旧条目不去重。每 pop 消耗原预算；所有用户代码返回后重取完整票据与当前权限，不保存 moving SlotMap 指针。 |

bank 容量证明：每个非空 cell 至少由一个已接纳 C 或一个未物理归还 A 支撑；一个 cell 可同时有 C+A，
因此 C+A 个 cell 能接纳原逻辑上可共存的任意角色组合。选中 foreign wait 时可单独新建 C cell；extra waits
各占自己的 A。代次耗尽的物理槽不可复活，是极端耗尽边界，不通过隐藏扩容替代。所有者非复制、非移动；
ScriptSystem 移动的是稳定 State 的 owning pointer，已有地址不变。union body 析构不结束 header 的寿命。

## 三后端与不兼容登记

Lua 原 Event/LocalAsync C 入口、CppStatic 的 coroutine context、Native ABI6 StepAdapter 均透传准确的
`ScriptStepContext`；本轮改变公共 C++ EventWaitFactory 回调从 `(void*, ScriptInstanceId, admission)`
为 `(void*, const ScriptStepContext&, admission)`，factory 持准确 context 借用。直接构造该 factory 的
CppStatic/Lua provenance fixtures 已迁移，全部后端和 SDK 一起重编译。不能混用旧 C++ 头与新 DLL。
Native C ABI6、32位公开 token/generation、资产 wire/schema 不变；三份基准 cooked 资产逐字节同 hash。

`ScriptRuntimeStats::awaitable_record_bytes` 现在报告 local body 176B；`awaitable_storage_bytes` 是 A 薄目录
backing，**不含 cell bank**。新增 bank、C 目录、boxed、ready 字段用于完整统计；旧口径不可直接相减。
shutdown 后目录 vector/ready 的容量保留到 State 析构；bank 已释放。boxed body 在320B union内预留，不是
在 bank 之外再建第二完整池；超过 inline 的有效 payload spill 要另计。控制 free/epoch/owner 在 cell header 内。

## 测量与可见性限制

正式42进程使用独立 A/B 匹配镜像，关闭可选观察；所有配对按原业务字段逐帧核对。observer 是同一 clean
源码、单独打开已有 HOTPATH_OBSERVATION 的产物，完成后构建槽恢复 OFF；观察 DLL 没有安装覆盖正式 SDK。
新增 count 只在 observer 编译时写入。A 无新字段记 null；旧 CPP 的累计 failures 不可观测也记 null。

layout probe 只在仓外复制头的 access label（private→public），不改成员/算法；用实际MSVC类型和原容器
reserve 测量 requested bytes，包含 STL 对齐/array cookie，不包含 probe 自己的记录头或下层 heap 元数据。
并非生产补丁或替代性能 benchmark。补充 both-full 与 same-name Delay fixture 也只在独立复制的测试中增强
断言，分别链接 A/B 固定 DLL；其新测试 hash 和原测试 hash 都保留。

T29 的原提议“重绑定同名 Delay 后 starter 执行”不符合两侧当前装配合同：builtin 已发布时 duplicate
contract 返回 AMBIGUOUS_PROVIDER。实测双方均冷拒绝、provider/backend=0；另有 prepared local catalog 的
错 context/dispatch 拒绝和通用自定义异步 provider 的真实 completion 测试。不为满足设想而开放旧系统不存在的 override。
