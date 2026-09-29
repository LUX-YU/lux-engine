# P06 复审 R1：实例回收不能隐式清除尚未确认的单步结果

**日期：2026-09-29**  
**项目：LUX-YU/lux-engine**  
**实施分支：`codex/editor-redesign-v4`**  
**本次审阅验收 HEAD：`cc3f455095f52e9a2740bf0a31a55e6672240916`**  
**最终验收绑定的实现：`26cca1c47adabdbf52346e149b44f7264479bf40`**  
**前置已验收 HEAD：`fae890a2a848ee68a16ff976e8047ea1b2a3a560`**

## 0. 结论与边界

**P06 主体成果认可，暂不放行 P07。本轮只补正一项结果寿命问题，编号 P06 R1 / B01。**

已接受的单步结果目前完全存放在可回收的 Runtime 实例记录中；RunStore 的查询又始终回到该记录。实例回收后，即使 RunId 仍有效、Run 还未被 acknowledgeStop，已完成／失败／取消的单步都只能得到 INVALID_ID。实例资源寿命与操作结果观察寿命被绑定在一起。

本轮不重做 SceneRuntime、RunStore、RunController、SceneSession、History、执行器或渲染，不重新移动目录，不更改包名，不扩展 ApplyRunChanges。不把这一项新问题挂到 C01/C03/C04。

**不是要求在 acknowledgeStop 后无限保留所有 StepTicket。** 本次需要保证的是：只要 Run 记录尚未被明确确认清除，其尚未确认的单步终态不能因底层实例回收而消失。资源可以先退休，轻量结果按明示确认协议回收。

原 P06 的“单步票据报告真实完成／失败／退休取消”“确认终态释放容量”是既有要求；本补充明确该要求跨越实例退休的具体边界。确认整个 Run 是否同时确认其余单步，原规范没有详细展开，本文件在第 5 节明确采用兼容现有入口的有限规则，不把它冒称为旧规范中已有的逐字条款。

## 1. 本次实际审阅范围

已通过 GitHub 连接读取：

- 分支 HEAD、验收父链、P06 README、文件清单及归档检查器。
- SceneRuntime.hpp、SceneInstanceLease.hpp、SceneRuntime.cpp 的构造、借用、控制、驱动、退休与单步实现。
- SceneDriver.cpp、SceneInstanceImpl.hpp。
- RunStore.cpp 的运行状态、调试、单步、停止、更新、确认和启动准备／采用。
- RunController.hpp、RunStore.hpp、RunTypes.hpp、RunDebugEdits.hpp、execution README/CMake。
- 实际 `runs.cpp` 测试与原 Runtime 测试，EditorLoop 和旧 ScenePlayback 的接线。
- CTest 汇总、冷构建依赖修正提交及对应验收说明。
- 原 P06 与 P05 R2 后的启动补充。

**没有独立构建 Lux 引擎、运行 121 项 Editor CTest、11 项 PLAYER CTest、安装消费者、GPU 或 Android，也没有独立复算全部远端归档哈希。** 新 B01 是由已读控制流直接推导的缺陷；附带测试片段尚未在真实 SDK 中编译／运行。不得将它登记为复审方已经完成的真实失败复现。

另一次尝试直接将公开 raw 文件下载到容器，因本环境 DNS 不可用失败；不影响已通过 GitHub 连接取得的源码阅读。这不是产品构建或测试失败。

## 2. 已成立、不得回滚的设计

### 2.1 运行与作者内容已经分离

RunStore 独占私有 RunSession；RunSession 组合启动快照、来源、环境、实例 lease 和运行调试状态，不继承 SceneSession，也不持有作者可写源。RunController 先准备冻结包，owner 实际实例构造成功后才发布 RunId。

RunStore.update 读取实际进度，不调用 driveFrame。原 SceneRuntime 仍持实际实例／时钟／驱动；EditorLoop 保持单一产品驱动入口。实例的立即回收留在 Runtime 的安全点。

### 2.2 退休动作本身已经正确延迟

SceneInstanceLease 的退休动作只将预分配 lifetime 标记为 requested 并唤醒。Runtime 的 beginRetirement 请求系统停止；维护完成、遍历和回调返回后 collectRetired 才移除、析构实例，最后设置 completed。

新的 Registry 借用在 requested 时即被拒绝。InstanceRetirement 可以在 slot 回收后继续查询完成。这些设计应当保留。

### 2.3 正常控制与准备完成有真实测试

现有 Run 测试验证了：两实际 Run 和两个只读消费者；暂停维护与真实完成采用；32 个 FIFO 单步；正常恢复 dt；发布失败；冻结作者 S10、作者后续 S12 与运行编辑 S20 分离；准备回调中的嵌套完成可靠接收与防重复采用。

P05 R2 的“已接受完成可靠接收，不解除外层保护”得到继承。本轮不修改保存模块。

### 2.4 旧代码和目录保持收敛

已读 SceneRuntime 公开头不再保留旧 valid/invalid/getSceneRegistry/getClock/tick/TickResult/destroy；旧 ScenePlayback 实际调用 RunStore/Controller，而不是保留另一套运行驱动。归档检查器包含旧符号扫描和调用方迁移验证。

新 execution 仍在 `editor/tools/scene/execution/`，轻量 API target 和静态实现属于同一主题，没有为每种票据新增 DLL。旧冻结输入桥仍是 P12 前的限定接线；不应在本轮扩大它的消费者。

## 3. B01：真实完成的结果随实例记录提前消失

### 3.1 精确责任链

| 位置 | 现有行为 | 缺口 |
|---|---|---|
| `engine/scene/composition/src/SceneRuntime.cpp`，`Impl::Record::steps` | 32 个 `Step{ticket,status}` 存在 Record 内 | 结果与重实例一起析构 |
| 同文件 `beginRetirement()` | 将 QUEUED/EXECUTING 标为 CANCELLED，保留已有终态 | 赋值不等于消费者一定能观察到 |
| 同文件 `collectRetired()` | erase slot，reset Record，最后设置 lifetime.completed | 此时 steps 已经删除，lifetime 仅剩退休标志等数据 |
| 同文件 `findStep()` | 必须先 `find(ticket.scene)`，再搜索 steps | 回收后只有 INVALID_ID，无结果通道 |
| `editor/tools/scene/execution/src/RunStore.cpp`，`stepStatus()` | 验证 RunId/instance，再调用 Runtime.stepStatus | 即使 Run 仍 STOPPED/FAILED 且未确认，也没有晚读终态路径 |
| 同文件 `acknowledgeStep()` | 先查上述 status，再确认 Runtime，再移除 run.steps | 回收后连明确确认单步都无法完成 |
| `RunSession::steps` | 仅保留 `vector<SceneStepTicket>` | 保留了地址，却未保留该地址的最终事实 |

关键控制流可简写为：

```cpp
// Runtime：有结果，但它只存在于待销毁 Record。
step.status = {CANCELLED, stopped_failure};
...
records_.erase(key);
record.reset();                 // 包括 32 个 StepStatus
lifetime->completed = true;     // 此后 StopTicket 完成

// RunStore：保留的 Run 仍有身份，但查询只寻找旧实例。
auto found = impl_->find(ticket.run);  // 成功
return runtime.stepStatus(ticket.step); // 旧实例 slot 已无记录，INVALID_ID
```

这里不是主张一份已失效的 Registry 借用应该继续可用，也不是要求重新访问已销毁 Scene。需要保存的是已结算结果本身。

### 3.2 最小、无竞争的失败顺序

1. 使用真实 Fixture 创建 Run，并暂停到 PAUSED。
2. 请求一次 step，等待它 COMPLETED；**暂不 acknowledgeStep**。
3. 再接受两次 step，尚未执行时调用 RunStore.stop，得到 StopTicket。
4. 正常 driveFrame + RunStore.update，直到 StopTicket.complete 且 RunInfo 为 STOPPED。
5. **不调用 acknowledgeStop，也不复用 Run 槽位。** 此时 RunId 仍然有效。
6. 查询前三张票据。

期望结果分别为 COMPLETED、CANCELLED、CANCELLED，后两项携带停止原因。当前控制流三项都会走 Runtime 的已删除实例查找，返回 INVALID_ID。

“之前那一步确实完成”不能在停止时降级成 CANCELLED；“之后两步被停止取消”也不能只表现为不明原因的未知身份。

### 3.3 失败结果同样存在晚读缺口

当前 `runs.cpp::failure()` 在 driver 报错后立即读取单步 FAILED，然后才执行 RunStore.update 和后续退休，因此能通过。

合法消费者也可能在下一轮安全点、实例回收之后才读取。此时 RunInfo 仍保留 FAILED 及整体错误，但单步票据对应的失败结果已经消失。整体 Run 错误不是所有单步结果的等价替代：已完成步骤、失败步骤和未执行而取消的步骤不能仅凭整体状态推断为同一种结果。

### 3.4 现有测试为什么没有覆盖

- `controls()` 在每次 frame 后立刻读取结果，并在停止前确认全部 32 张票据。
- `failure()` 在实例退休前读取唯一失败票据，后续只确认整个 Run。
- 原 Runtime 回调退休测试只检查 InstanceRetirement 和一次析构，不组合未确认单步。

本次新增的是“已有单步协议 × 已有退休协议”的组合验证，不是追加新的编辑器功能。

## 4. 解决方向：轻量结果状态与实例资源解耦

### 4.1 先保留唯一责任，不增加第二套 scheduler

Runtime 继续唯一分配 step 序号、排队、驱动、认定 COMPLETED/FAILED/CANCELLED。RunStore 不能另维护一套仿真计数，不能自己重跑 step 或推算虚假完成。

结果保存可以独立于实例本体：

```text
SceneRuntime
  ├─ 活动实例记录：Registry / SceneDriver / clock / 系统资源
  └─ 有界单步结果状态：唯一生产者仍是 Runtime
        ↑
        └─ Run 记录／退休凭据持有轻量结果寿命

实际实例销毁
  ≠ 单步结果已确认
  ≠ Run 记录已确认
```

结果寿命独立不代表新建全局 SessionManager、TicketManager 或通用事件框架。

### 4.2 优先论证复用预分配的 InstanceLifetime

当前已经有共享的 InstanceLifetime 与 InstanceRetirement。优先考虑把原 32 个 step 槽的唯一结果状态放入或交接到这条明确的轻量寿命链，而不是在 RunStore 里再镜像一个活动队列。

可选择：

- 创建实例时就预分配唯一 step ledger，由 Runtime 写入，活动 Record 和结果句柄引用同一份；或
- 在回收前把最终轻量结果移交给已预留的退休完成状态，活动 Record 随后销毁。

只能选择一套可证明的方案。不能在回收后依赖已经不存在的 Record 补取结果；不能要求 RunStore.update 必须恰好在 erase 前被调用。

**不强制要求新增某个公开类型名称。** 私有 Step/ledger 可以仍在现有 CPP 内。若确实需要新的查询 overload 或窄 receipt 方法，先写明它只读结果、不借 Registry、具有 owner／身份校验，再同步真实消费者。不要给每个小结果建 target。

### 4.3 查询与确认

RunStore.stepStatus 应同时处理活动和已退休但尚未确认的 Run。已退休时读取真实终态，不绕回 live slot 查找。

RunStore.acknowledgeStep 应能确认已退休 Run 的已结算票据。确认只释放相应结果槽，不重新销毁实例、不发送第二次退休请求、不触发新的 step。

SceneRuntime 原只凭 SceneInstanceId 查找的方法是否继续仅在活动实例期有效，应明确记录。无需为所有已结束实例建立永久全局 tombstone；Run 层可通过持有的退休／结果句柄查询。本轮必须使公开 Run 结果协议完整，不用未定义的低层回收规则敷衍它。

### 4.4 结果本身的代码与内存寿命

最终结果不能借用已销毁的 Registry、SceneInstance、RunSession 或 renderer。保留结果也不能迫使整张图、飞行任务或 GPU 场景一直存活。

如果保存的 failure/any payload 包含插件定义类型或自定义 deleter，应只保留它确实需要的代码／元信息 owner，且先销毁结果值、后释放这些 owner。不能将“只有一条错误记录”误当作完全没有代码寿命依赖。

shared_ptr 只表达寿命，不自动证明线程安全。保留既有 owner-thread 查询契约；退休完成跨线程标志沿用现有语义，不顺手开放任意线程查询可写结果槽。

## 5. 本轮固定的有限生命周期契约

### 5.1 有效期与终态

| 时点 | stepStatus | acknowledgeStep |
|---|---|---|
| 实例仍活跃，票据未完成 | QUEUED/EXECUTING | BUSY，不能清理正在执行的责任 |
| 已真实完成／失败，未确认 | 原 COMPLETED/FAILED 与原结果 | 成功并释放该票据结果 |
| 停止导致未完成票据取消，重实例已回收，Run 尚未确认 | CANCELLED 与停止原因 | 成功 |
| step 已确认 | 明确无该结果／过期票据 | 重复确认明确拒绝 |
| 整个 Run 已 acknowledgeStop | 旧 RunId/StepTicket 明确失效 | 明确拒绝，不命中新 Run |

COMPLETED/FAILED 终态不能被停止统一改成 CANCELLED。单步“已经开始”但未达到现有成功定义的完成边界时，应遵循原取消／失败语义，不伪造完成。

### 5.2 整个 Run 的确认是显式的观察清理

为保持现有 stop 消费者行为，本轮允许 `acknowledgeStop(run)` 作为整个已终结 Run 的最终确认，清理其余尚未单独确认的终态票据。它已经是调用者明确要求移除 Run 的操作。

因此无需强迫所有旧调用方先逐一 acknowledgeStep，才能执行已有 acknowledgeStop。需要把上述“聚合确认”语义写入公共注释与 README，测试也验证：确认之前结果有效，确认之后整体失效。

**不允许 Runtime 自行回收重实例等价于这个显式确认。**

### 5.3 容量与回收

沿用每实例 32 项未确认票据限制和 RunStore 的容量。结果在 stopped Run 中暂留仍应有明确有界 owner；不增加无限历史 map、全局墓碑或永久数组列表。

单项确认释放对应轻量结果；整体确认释放其余结果和 Run slot。重复停止、重复确认、slot 复用都不能导致双重释放或新旧身份混淆。资源退休计数与结果记录计数必须分开验证。

## 6. 文件与符号处置清单

| 文件／对象 | 本轮动作 | 禁止事项 |
|---|---|---|
| `engine/scene/composition/src/SceneRuntime.cpp`，Record::Step/steps、InstanceLifetime | 调整唯一结果 owner／交接位置；保持原发号与 FIFO 算法 | 不能保留两份可写 steps 或第二个 next_step |
| 同文件 beginRetirement/collectRetired/findStep／公开转发 | 回收前结算，结果脱离重实例；保持成功／失败终态 | 不为保存结果延迟本可完成的 Registry/GPU 回收 |
| `SceneInstanceLease.hpp`、`SceneRuntime.hpp` | 必要时加入窄的结果查询／确认能力，说明有效期与身份域 | 不恢复旧 destroy/tick 等双 API |
| `editor/tools/scene/execution/src/RunStore.cpp`，stepStatus/acknowledgeStep/update/acknowledgeStop | 接通保留结果，明确聚合确认与槽位失效 | 不推算假状态、不依靠每帧及时轮询 |
| `RunStore.hpp`、`RunTypes.hpp` | 按需要补充结果寿命契约；等价既有身份优先复用 | 不引入平行 RunId 或 Session 状态 |
| `execution/README.md` | 明确实例、结果、Run 三种寿命及确认 | 不把生命周期问题推给调用者“必须更快查询” |
| `execution/test/runs.cpp`、原测试 CMake | 加入第 7 节真实组合回归；保持原测试语义 | 不删除旧单步失败、FIFO 或暂停断言 |
| `engine/scene/composition/test/runtime.cpp` | 对必要的新轻量结果能力增加真实低层验证 | 不用空 Fake 替代实际宿主 |
| `cmake/installed-consumers/scene-execution/` | 安装后验证晚读结果与确认 | 不借源码目录私有头或旧 build 的 DLL |
| 旧 ScenePlayback 与其它确受 API 变化的消费者 | 只迁移必要调用；聚合确认保持原功能 | 不借本轮切换完整新 UI 或扩大私有桥 |
| 当前 `.internal/editor-redesign/` | 记录 B01、结果 owner、回收边界、测试和期限 | 不用历史 V4 seed 覆盖当前账本 |
| `dev_log/P06-R1/` | 冻结实际 before/after 和最终验收 | 不改写原 P06 或之前快照 |

旧 `Record.steps` 若整体移走，应删除原成员与全部原访问；不能留下第二份无作用字段。被替代的 helper 同步删除，不添加 deprecated 转发壳、注释大段旧逻辑或 backup 文件。

SceneSession、保存/R1/R2、History、SessionState、ExecutionRuntime 算法、资源格式、Physics2D 实现、目录结构与包名原则上均不需要修改。

## 7. 必测真实场景（R06-R1-01～05）

### R06-R1-01：排队后停止，回收后首次查询

在现有真实 Run Fixture 暂停 Run，接受两步，不驱动它们即请求 stop。正常驱动到 StopTicket.complete，RunInfo 仍可查 STOPPED，不调用 acknowledgeStop。首次查询两张票据必须是 CANCELLED 与原停止原因；可以逐项确认。验证没有仿真补跑、没有重新创建已退休实例。

**先在固定修复前生产版本实际执行并保留输出。** 主张中的 INVALID_ID 要以本次真实运行记录核对，不能把文档静态推导冒充原日志。

### R06-R1-02：已完成未确认 + 排队取消

完成一张票据但不确认，再接受两张排队票据，然后停止回收。首次晚读结果分别为 COMPLETED/CANCELLED/CANCELLED。全部状态不依赖消费者是否曾在退休前读过。检查作者源、current、observed、dirty、binding 不变。

附带 `tests/step_results_after_retirement.inc` 提供可加入现有 Fixture 的草图，未在本环境编译运行。

### R06-R1-03：实际发布失败，回收后读取原失败

沿用 Probe 的实际 publication failure（原因 731），提交单步后只正常驱动，不在“失败已产生但尚未回收”的窄窗口提前读取。等 RunInfo 为 FAILED、实例已经消失，再检查相应票据仍为 FAILED，保留原阶段和错误 payload；不能改写为一般 STOPPED 或 INVALID_ID。

当前 driver 会把受同一失败影响的待办票据设为 FAILED。保留这一现有语义；本轮不重新设计仿真失败策略。

### R06-R1-04：回调内停止、在途资源与最终结果

组合原 callback retirement 测试与未确认单步。用真实可控维护／publication 端点建立 pending，不靠 sleep。回调中请求停止不销毁实例、不解除外层保护；在途责任结清后实例只析构一次，StopTicket 才完成；单步结果之后仍可读。

不要保留跨 driveFrame 的可写 Registry 借用来人工修改测试状态。沿现有 runtime 夹具的外部 in_flight 控制方式或等价测试端点控制待办。

### R06-R1-05：确认、代际与容量回收

覆盖每实例 32 项限制，终态单项确认，整体 acknowledgeStop 的聚合确认，以及 Run/实例槽位复用。旧票据不可命中新对象；停止完成但尚未确认的 Run 占用自己的有界容量；确认后可以再次启动。

重复有限轮次（例如 64 次），同时观察重实例实际析构次数、轻量结果计数／容量恢复、旧结果失效。没有必要新增产品性能指标或无限结果历史。

安装后 consumer 至少运行 R06-R1-01/02 的核心路径和聚合确认。不要只 include/link 即宣称通过。

## 8. 实施顺序

1. 确认当前分支、HEAD 和祖先关系；不 reset 到旧基准。
2. 读取原 P06、启动补充、本文件及当前账本。先保护 ProjectBuilder.cpp 既有用户差异，不把它带入本轮资格实现。
3. 添加真实晚读场景，在固定旧生产实现上运行，保留失败状态与仍有效的 RunId／已完成 StopTicket。
4. 先明确结果寿命和聚合确认契约，再调整 Runtime 的单一结果 owner／交接，最后接通 RunStore 查询与确认。
5. 同步必要公开头与实际消费者，删除被替代的旧结果成员／访问；不改变 FIFO、Clock 或暂停策略。
6. 在最终实现 SHA 上显式 P06 门禁，运行全量构建、第二轮、原 Editor 121 与 PLAYER 11 的对应回归、R06-R1、保存/R1/R2、真实依赖负例。
7. 重装 SDK、重新构建原十组消费者，运行新增实际 Run 晚读用例；测试总数变化按语义登记，不以总数替代断言。
8. 实现和 `dev_log/P06-R1/` 验收分别提交并正常推送；停在 P06，等待复审，不实施 P07。

本次产物不是仓库补丁，不应直接把测试草图当作已编译代码或把本文件标成 PASS。必测未运行或真实结果不符合要求时，只能报告 PARTIAL/BLOCKED。

## 9. 保留的工程事实

- 已读归档有 Editor CTest 121/121；P06 README 记录 PLAYER 11/11、十组安装消费者和两套二次无工作。复审没有独立重跑这些矩阵。
- Physics2DDescription 的缺失生成依赖已在本轮截止提交修正为实际 `_generate` 目标。P06 记录了移走生成输入后的顺序验证；这是窄依赖修复，不是全仓首次冷构建认证。原失败和修复记录均保留，不重新称其完全未修复。
- 用户 ProjectBuilder.cpp 修改按 P06 记录仍未纳入验收实现；本次没有访问用户本地工作区，不宣称已独立确认它干净。
- C01（P09/P12）、C03（P11）、C04（P12）仍为原 FAIL，本轮不修改这些判定。
- 原 P06 冻结输入桥只供 editor_scene、SDK 不安装、P12 删除。单步结果补正不需要新增桥，也不扩大旧桥权限。

## 10. 给实施方的简要指令

> P06 主体方向认可，暂不进入 P07。只执行 P06 R1：实例回收后、Run 尚未确认时，已接受且未确认的单步结果不能变成 INVALID_ID。
>
> 先以真实 RunStore/SceneRuntime 复现“排队后 stop，重实例已回收，RunInfo 仍为 STOPPED，首次读取 StepTicket”；同时测试已完成未确认和实际失败票据。保留旧实现的真实输出，不把静态分析或测试草图记为运行证据。
>
> 在原 Runtime/退休凭据/Run 记录中建立唯一且有界的轻量结果寿命。重实例按原安全点回收；结果保留到单项确认或整个 Run 的显式最终确认。保留原 COMPLETED/FAILED/CANCELLED 和原因，不能让 RunStore另调度或推算假结果。
>
> 不保活整图／GPU 来保留状态，不新建全局票据管理器、无限墓碑、平行队列或第二套 History。不修改目录、包名、保存链与执行器算法。被替代的成员和调用完整删除。
>
> 执行 R06-R1-01～05、原 P06/PLAYER/持久化回归、实际依赖负例和 SDK 消费者，显式 P06 门禁。原 P06、冷构建修复及用户差异记录保持；C01/C03/C04 原 FAIL 不变。实现与验收分别提交，推送后仍停 P06。

## 11. 固定源码与规范来源

以下固定到本次验收 HEAD，除提交差异另行注明：

- [P06 验收说明](https://github.com/LUX-YU/lux-engine/blob/cc3f455095f52e9a2740bf0a31a55e6672240916/dev_log/P06/README.md)
- [SceneRuntime.cpp](https://github.com/LUX-YU/lux-engine/blob/cc3f455095f52e9a2740bf0a31a55e6672240916/engine/scene/composition/src/SceneRuntime.cpp)
- [SceneRuntime.hpp](https://github.com/LUX-YU/lux-engine/blob/cc3f455095f52e9a2740bf0a31a55e6672240916/engine/scene/composition/include/lux/engine/scene/SceneRuntime.hpp)
- [SceneInstanceLease.hpp](https://github.com/LUX-YU/lux-engine/blob/cc3f455095f52e9a2740bf0a31a55e6672240916/engine/scene/composition/include/lux/engine/scene/SceneInstanceLease.hpp)
- [RunStore.cpp](https://github.com/LUX-YU/lux-engine/blob/cc3f455095f52e9a2740bf0a31a55e6672240916/editor/tools/scene/execution/src/RunStore.cpp)
- [RunStore.hpp](https://github.com/LUX-YU/lux-engine/blob/cc3f455095f52e9a2740bf0a31a55e6672240916/editor/tools/scene/execution/include/lux/engine/editor/scene/RunStore.hpp)
- [RunTypes.hpp](https://github.com/LUX-YU/lux-engine/blob/cc3f455095f52e9a2740bf0a31a55e6672240916/editor/tools/scene/execution/api/include/lux/engine/editor/scene/RunTypes.hpp)
- [真实 Run 测试](https://github.com/LUX-YU/lux-engine/blob/cc3f455095f52e9a2740bf0a31a55e6672240916/editor/tools/scene/execution/test/runs.cpp)
- [真实 Runtime 测试](https://github.com/LUX-YU/lux-engine/blob/cc3f455095f52e9a2740bf0a31a55e6672240916/engine/scene/composition/test/runtime.cpp)
- [运行模块契约](https://github.com/LUX-YU/lux-engine/blob/cc3f455095f52e9a2740bf0a31a55e6672240916/editor/tools/scene/execution/README.md)
- [旧窗口调用迁移](https://github.com/LUX-YU/lux-engine/blob/cc3f455095f52e9a2740bf0a31a55e6672240916/editor/tools/scene/src/ScenePlayback.cpp)
- [原 Editor 121 项日志](https://github.com/LUX-YU/lux-engine/blob/cc3f455095f52e9a2740bf0a31a55e6672240916/dev_log/P06/logs/ctest.log)
- [Physics2D 生成依赖修正](https://github.com/LUX-YU/lux-engine/commit/26cca1c47adabdbf52346e149b44f7264479bf40)
- 规范：已提供的原 `P06_scene_runtime.md` 与 `LUX_ENGINE_P06_START_AFTER_P05_R2_2026-09-29.md`；不改写旧文档和当前施工账本。
