# SR-4 异步职责迁移：实施记录（2026-09-07）

## 开工身份与决定

本轮用户明确接受已量化 Event 差距作为阶段债务，授权实际 SR-4；不改写 SR-3 历史结论。
开工本地/远端 `ba6c7be44797e6e7efe4a272d0f208d247f7366c`，运行时等于 `99c1d095`，
验证代码 E0=`8a6e6ef468f7eb60265678ab25116719b42a01a6`，H0=`8145598c18421d03da1dac21251200af3290e27d`。
lux-cxx=`3100f54d0743c5ed94a4ccf5943df04e933de255`，toolset=`99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`。
114 个 cxx 安装头重新核对；main 五项和工作树测量脚本/两个未跟踪文件的哈希已固定，均不纳入提交。
本文件先登记当前映射及内部契约，完成实施后补实际 SHA、验证与限制；当前不宣称阶段已完成。

## 原 State → 正式 owner 与完整操作

| Owner | 实际状态 | 构造、修改、失败与销毁 |
|---|---|---|
| ScriptExecution | execution_instances（移出 first_event_waiter）、active_hooks、ContinuationStorage、StableSlotMap Awaitable、owned value、ResultWritePin、ResumeRing、执行统计 | fallible prepare 固定 backing；initialize/beginSuspension/attachWaiter；eager/late finish；取消先清来源；pin 延迟擦除；逐完整代次重查 continuation；撤权/执行销毁/关闭 |
| ScriptEventWaits | event_waiters、event_wait_routes、sequence、claimed_event_waiters reservation、active_claimed_waiters、事件实例链与统计 | prepare 路由/槽/领取 backing；preflight→登记；claim 当前 occurrence；取消 unlink、领取位置保留；嵌套 ClaimBatch 退出恢复范围；关闭/统计 |
| ScriptTimers | next_step_waits、simulation_delays、deadline/minimum_step/sequence、Delay provider、Timer 每实例链及来源 token | prepare 固定容量；登记验证与来源关联；按原 step/deadline 顺序到期；G03 显式取消立即移出索引并安全归还槽；队头背压不越过；real delay 保持现有 Process endpoint |
| ScriptCompletionIngress | AwaitableIngress 的 shared transport、ExternalCompletionRing/ticket、frontier/remaining/prepared、producer callbacks 与统计 | prepare physical ticket 容量；open/close；构造 lease；producer push 仅入运输；owner capture/peek/ack；close 保留迟到消息安全性，transport 可晚于 System 析构 |
| ScriptSystem | prepare/shutdown、dirty/retirement/lifecycle 队、ECS observer、dispatch depth、last_stable_step、失败记录 | 批次装配/初始化/BeginPlay/publish/activate；撤权和统一故障；每 occurrence claim→callbacks→complete；唯一 stable 的 lifecycle→NextStep→delay→external→预算 resume，每 pop 后同 frontier 补接入 |

Instances 保留唯一 ACTIVE/完整 incarnation/宿主/prepared 权威；Bindings 保留普通 handlers/token；Preparer 保留冷目录与准备移交。
四新组件均在原 target 的 pinclude/src，不传 State、不返回可写记录/容器、不建立第二条生产执行路径。

## 跨组件契约与资源有效期

所有操作除 producer transport callbacks 外，只能在原串行 owner 边界调用。System 的 owner-affinity 检查与 reentry 行为不扩张。

| 操作 | 验证/借用与用户代码 | 提交、失败和归属 |
|---|---|---|
| Execution 创建 Awaitable / Event 登记 | Instances 完整身份/ACTIVE、prepared admission provenance、结果布局及来源容量；内部 Event 不构造外部 lease | 先 Execution 预留最终 owned bytes，EventWaits 登记成功后记录有限 source token；失败撤销登记与结果，不能留下半关联 |
| 调用 / ResumeBatch | 现有 Invocation 借用 host/prepared；scope 覆盖 backend 与返回/失败清理；返回后 current() 重验 | Execution 独占 single-flight/continuation/awaitable；System 统一故障。stale pop 也消耗原预算，System 每 pop 后补接入同一 ingress window |
| ClaimBatch / complete claimed | EventWaits 返回有限值身份，reservation 到批次退出才归还；Execution pin 最终结果存储，再执行 projection copy | callback 可嵌套登记/取消/退休；copy 后重验、再从来源 unlink，仍有效才 READY。取消不能复用被 pin 的字节，Execution 唯一持有 payload |
| SourceToken 取消 | token 仅 kind+slot+generation；Execution 先撤销/清空关联，再通知来源；来源反向取消返回 instance/awaitable 值 | EventWaits/Timers 独占链和物理 unlink；重复/旧代次取消无效果。取消自身/claimed/复制中结果保持原回收保护 |
| Timer 登记 / 到期 | 从受限 owner-completion access 匹配本 Execution 的 owner context 和两个完整 token；不假设公开 getter | 固定 timer slot、每实例链与 indexed deadline heap；登记失败精确回滚。到期跨 owner completion 前复制有限 completion，返回后按 token 重查；背压保留原有效条目 |
| Ingress capture/peek/ack | producer 不读取 Registry/Instances/Execution；peek 是到 ack 为止的 const packet 借用，未 publish head 不越过 | post 成功只表示运输接受；peek 消耗本 window 次数，队列背压不 ack。最终结果验证/提交仍在 Execution，stop 后 shared transport 可安全存活 |
| 退休 / 关闭 | Instances 先撤新调用资格，Execution 撤等待资格；System 等待 invoke/resume/copy/claim/cleanup 保护 | continuation 销毁→有资格 EndPlay→逆序 releaseMethod→backend→host/import/artifact/code lease；busy 不伪装成功，不 resume-until-finished |

G03 单列允许的差异：NextStep 和模拟 delay 取消后及时释放物理来源容量/统计；不修改 step+1、elapsed+ceil(ns)、
minimum_step、deadline/sequence、真实背压和恢复时机。正 real delay 仍由 Scene/Process 排空，L1 不新增线程。
热路径存储在 fallible prepare 预备；任何移除旧 catch 的位置必须有容量上界和不分配操作依据，不能靠 noexcept 掩盖可能分配。

## Scene Lua 定位检查点

E0 Developer 宏指向 E0 Toolchain 实际生成的 lua_portability_fixture.lxsa；已核对编译命令、资产哈希及
engine/toolchain/lua/test/lua_portability_fixture.lua。实际业务是 eager beginOperation→nextStep→simulationSeconds(2e-9)，
不是 fallback tick。先保留原四步失败和 default/interpreter 的内部 trace，再按真实 frontier、ceil(ns) 与 step 推导预期。
实际 root cause：第一次 tick 在 step 1 的 frontier 捕获后发布 eager completion，因此 step 2 才接入并 resume；
NextStep 在 step 3 resume 后登记 simulation delay，实测 MSVC ceil 为 2 ns，elapsed=3 + 2 = 5，step 4 不到期。
原测试在 step 4 要求 continuation=0，与真实资产协议不符。独立提交 89fb01bc 将预期固定为逐 step 1—5 表，
检查 calls/resumes/suspensions/NextStep/delay/external/active、读写/业务值和重复 Scene stable 不推进；EndPlay 原断言保留。
未改 runtime、资产、frontier、恢复点或预算。原失败与两策略内部轨迹见 sr4/diagnostics/lua-before，
修后六配置均通过（lua-fix-ctest.log），迁移后再次六配置通过（split-ctest-2.log）。

## 阶段日志

- 已完成：开工身份/未知修改/依赖安装核对，逐操作协议映射，实际 Lua 资产分支与生成输入定位。
- 四组件主体已迁移，迭代 target all 与第二轮 no work 通过；受影响 80 个测试在 VS 环境中全过。
- 首次未初始化 VS 环境的编译负例未命中业务诊断，判为无效环境试验；保留 split-ctest-1.log，不计候选功能回退。
- 新增 testTimerSourceCancellation：NextStep/模拟 delay 各在 2 槽容量下 32 次显式 discard、32 次退休重建；
  不推进 step，另一实例始终有效，旧 completion 不可影响新代次，失败 starter 已登记来源亦回滚；最终销毁闭环。
- 正在执行：最终窄测试、独立 clean tracked qualification、安装消费者、同量 E0/H0 配对与证据归档。

## 非平凡操作的源码索引

- ScriptExecution：createAwaitableRecord → createRegistration / waitEventErased → attachWaiter / beginSuspension；
  finishAwaitableOwner / completeClaimedEventWaiter / drainExternalCompletions 唯一提交最终结果；
  releaseSource / eraseAwaitable / ResultWritePin / cancelAwaitables / destroyContinuation 处理双向取消、延迟回收和销毁；
  invoke / resumeOne 保留原票据及返回检查。原 State 的 InstanceRecord 执行链现仅 ExecutionInstance 私有。
- ScriptEventWaits：preflight / registerWait、claim / ClaimBatch、cancel / cancelNext 与 finishClaim 独占路由、实例索引及 reservation。
  路由数不超过 ACTIVE waiter 数；prepare 的 dense_map.reserve(capacity) 同时预留 packed 与 bucket（实际 EnTT dense_map.hpp:1032），
  trivial key/value 的有界 try_emplace 无分配，后续 SlotMap.tryEmplace 失败撤销新路由。序号/claim/copy/cleanup 统计保留。
- ScriptTimers：registerWait 预留 slot、连接本实例和全局队列/heap，attachTimer 成功才形成双向关系；
  cancel 使用源完整代次，先断链，再 erase；cancelNext 只访问指定实例的源链。
  simulation heap 以 deadline/sequence 排序，heap_index 在 swapHeap 同步维护，取消是 O(log n)，NextStep O(1)。
  completeDue 复制 completion lease 跨入 Execution，禁止持有可失效记录；背压不移除来源。
- ScriptCompletionIngress：Transport 只拥有运输记录、publication 与 lease，producer 不接触业务状态；
  capture / beginDrain / peek / ack 精确保持窗口，registration/open/close/stop 维护外部权限。
- ScriptSystem：初始与增量生命周期、occurrence ClaimBatch→Bindings callback→Execution complete、唯一 stable 顺序，
  ResumeBatch.next 每 pop 后同 frontier drain、shutdown 顺序与错误聚合；不再拥有等待/结果/运输容器。

G03 物理差异：以前已撤权限 NextStep 可留到 step 到期，模拟 delay 可留到到期/满容量扫描；现在 cancel/terminal 即返还。
对应 next_step_waits/simulation_delay_waits 反映实际来源存储，因此旧实例取消后的计数下降是本次显式修复。
ResumeRing 中旧通知仍按原协议保留，stale pop 仍消耗预算；不得把来源物理回收扩展为通知越过预算。
Timer backing 以唯一 awaitable owner 的同时存活上界 min(next_limit + delay_limit, awaitable_limit) 预留；先 clamp 后防溢出相加，两个逻辑来源上限保持原值。移除初版引入的 combined-capacity 拒绝，避免结构迁移新增输入限制。新增超大逻辑容量/小结果容量的工厂回归。
