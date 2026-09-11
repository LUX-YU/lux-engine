# NA1 第二轮：等待与结果交接的修改合同

工作起点 `7d5cd844`，生产对照为 `21d106014e05cf0a7746d45fd05eac8ae01575f2`。
最终生产/资格源码为 `7f20191692d3f5e1037a14273a61fc99eef95714`。本文和后续报告提交不是新测量源码。
保留七组件、唯一恢复环、稳定点、真实 step、原预算/frontier、单飞与 Event 多飞合同。

| 修改 | 移走的工作 | 保留的边界与失效条件 | 验证入口 |
|---|---|---|---|
| Event 默认复制 | `copyPayload` 的类型哈希强制 constexpr；descriptor 冷期选默认标量直接复制，省第二次 callback 分派 | 实际 data/kind/type/size/输出尺寸仍检查。仅默认 callback 且声明尺寸等于 `sizeof(Payload)` 才直拷；自定义 callback 保留原执行时点 | `testPreparedScalarProjection`：31/36 独立结果，五类坏输入不进入 custom，错声明尺寸拒绝且 guard 不变 |
| 已准备 Event 完成 | `finishPreparedEvent` 不再重复校验已经准备并复制的固定布局，不再二次 `releaseSource` | source 仍在旧时点释放；copy 后重验权限，PENDING/release_pending/stopping/队列容量仍检查。普通/外部完成继续 checked 通用入口 | `testPayloadFailures`、`testResumeQueueFailure`、四个 `testCopy*`、`testNestedDispatch` |
| 记录构造与 key | Awaitable/EventWaiter 直接向容器转发构造参数；cancel 一次 key 转换；已定位记录的物理释放直接构造其完整 key | 全部初始字段仍初始化；StableSlotMap/SlotMap 仍校验代次。插入失败、source rollback、write pin 和 release_pending 占槽不变 | `testCapacityAndCancellation`、`testAwaitableCapacityFailure`、`testCopyNestedAdmission`、`testBroadcastRouteReuse` |
| Event 来源访问 | 48 B 值快照改为固定 prepared entry/scope 的 const 借用，避免整份 prepared payload/scope 往返 | 实际 scope/instance/layout epoch/local ordinal、Entity 有效性仍在入口验证；借用仅到无用户代码的 source commit，不跨回调或退休 | `testPreparedAdmissionProvenance`、`testTargetedScopeRejection`、`testTargetedAndRetirement` |
| 恢复结果移交 | `takeAwaitable` 直接 emplace 到调用方的 outcome，不先构造完整局部 outcome 再转换 optional；ResumeBatch 不再返回嵌套 optional/expected | 先移出结果，再释放旧逻辑等待槽，再进入 backend。完整 ID、状态、关联与用户代码后的重新定位保留。stale pop 仍消耗一单位预算 | `testSyncAndContinuation`、`testNonPowerOfTwoResumeWrap`、`testResumeBudget`、Timer reuse/cancel |
| 完成窗口空路径 | Ingress 自己维护当前窗口是否仍可能有可接入项，已排空后每次 pop 只读这个状态 | 预留未发布的头项不视为空；backpressure 仍按原 allowance 重试；不 recapture、不跳头、不清 stale 项腾预算 | `testIngressWindow`：空窗口后提交、未发布头、FIFO、同 frontier、后 frontier、重试额度耗尽及新窗口恢复 |

源码入口：[Execution](../../engine/domain/simulation/builtin/script/pinclude/lux/engine/simulation/script/ScriptExecution.hpp)、
[Instances](../../engine/domain/simulation/builtin/script/pinclude/lux/engine/simulation/script/ScriptInstances.hpp)、
[EventWaits](../../engine/domain/simulation/builtin/script/pinclude/lux/engine/simulation/script/ScriptEventWaits.hpp)、
[Ingress](../../engine/domain/simulation/builtin/script/pinclude/lux/engine/simulation/script/ScriptCompletionIngress.hpp)、
[System 顺序](../../engine/domain/simulation/builtin/script/src/ScriptSystem.cpp)、
[Endpoint 模板](../../engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptEndpointBridge.hpp)。

以上内部操作都在原 owner 线程/执行保护区域内。Instances 仍独占权限写入；Event source 只暴露 const entry，
Execution 仍独占最终结果；来源取消的双向闭环不变。`ResultWritePin` 仍覆盖自定义 copy 和回收窗口；
不跨用户代码保存 moving SlotMap 指针，也不缓存 ACTIVE。

`finishPreparedEvent` 是私有窄入口，仅由成功复制并完成 copy 后检查的 claimed Event 路径调用；
不是公开 trusted/skip 参数。copy 可以重入，因此终态和容量检查不能提升到 prepare。
公开统计、同容量下的登记成败、Event queue-full 与 Timer retry 的区别均保留。

## 未删除与未宣称

- 固定 lux-cxx 没有已定位记录直接删除的 API；容器 find/erase 的必要检查仍存在，不修改依赖或绕过封装。
- 原 Outcome、Awaitable、EventWaiter 的存储表示没有缩小；移除临时对象不等于每次必然减少 `sizeof(T)` 字节复制。
- 本轮不修改 Lua 的 pcall、主栈、registry、自定义 codec、VM/GC/allocator，也不借此修改 Lua 可观察语义。
- 仍有来源 unlink、身份关系、结果搬运和合法回收保护；不能将剩余全部计为跨语言成本，或认定都是必要安全成本。
- 未改 Native ABI6、wire、普通脚本功能、handler 顺序、backend 分组、恢复预算或代码生成类型范围。

## 过程中发现的边界

`479d8db1` 的首次编译遗漏一个私有类型的 namespace 限定，由 `c650e3e2` 修正。
随后审查发现：省去旧 scalar copy 的 `sizeof` 检查，必须同时证明 semantic 声明尺寸与 C++ 对象尺寸相等。
最终 `7f201916` 在冷期补齐这个编译期条件，并增加 guard 负例；没有给正常内建标量增加动态分支。
这是本轮候选的修正，不能称为旧生产版本的缺陷。

中间 `c650e3e2` 的四个完整计时进程和一个被中止的进程均保留，明确不进入最终配对。
所有最终正确性/安装/计时和采样重新绑定 `7f201916`；原 `21d10601` 镜像及历史证据不覆盖。
