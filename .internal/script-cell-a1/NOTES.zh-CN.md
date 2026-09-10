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

当前仅 A0 完成。A1–A6 未验证，不将以下设计当作完成证据。
最终逐项填写附件 T01–T36 的真实入口、断言、原始日志和限制；最终七腿三对完整业务比较。
v4 scalar 回退与 Native metadata 债务仍由原报告登记，不归因于本实验、不在本轮顺手修正。
