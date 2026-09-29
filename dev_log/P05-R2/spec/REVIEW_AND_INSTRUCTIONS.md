# P05 R1 复审与 R2 补正：防重入不能丢失已接收任务的完成结果

**日期：2026-09-29**  
**项目：LUX-YU/lux-engine**  
**实施分支：codex/editor-redesign-v4**  
**本轮审阅 HEAD：`5a1ccbb47d2d08ffa8cfda907f74319c4a694068`**  
**R1 最终验收所绑定的实现：`1b2366616ba918bba29a3dcd1349274c574de398`**  
**R1 主修正提交：`f2b5a00c60e93dc81d7c72b127ad08b219ccfa23`**  
**R1 前置／原 P05 验收：`5866f990f8a5c4d68e19e0e395c9487c6c0f2779`**

## 0. 结论与范围

**上轮 B01（角色失效与递归采用）、B02（确认后版本衔接）的针对性修正认可；暂不进入 P06。**

本次发现一处由 R1 新拒绝策略引出的集成回归：`completeEncoding()` 在服务 dispatch 中返回 BUSY，但既有 `SaveExecution` 的一次性完成回调仍将结果移入这个函数并忽略返回值。若收集发生于角色回调期间，已经完成的编码结果被丢弃，保存永久停在 ENCODING，同目标后继被阻塞。

本轮只补正“已接受任务的完成交付”与“业务回调防重入”的交接，不回滚 R1，不重新设计 SaveService、WriteCoordinator、三类模型、History 或执行器。编号使用 **P05 R2 / R05-R2-01～05**，避免改写已归档的 R05-01～08。

R1 的 111/111 归档是已覆盖场景的历史成绩，不能因新增缺陷就删除或改判；但它不足以放行 P06。R2 完成后仍停在 P05 等待复审。

## 1. 已确认成立的 R1 修正

### 1.1 角色注册与操作执行期

- SaveService 增加私有 `DispatchScope`，请求、注册、采用、取消、确认等路径在保护下执行。
- `requestSave()` 在 describe 和 capture 返回后复查同一个注册 State 中的原 source 指针；撤销后不继续调用 capture，也不切换到后来注册的角色。
- `adoptCompletions()` 在已经 dispatching 时不重复进入；`acknowledge()` 在回调栈内返回 BUSY，避免删除外层仍持有的操作。
- status 和注册 token 的 RAII 撤销继续可用；模型的 SessionState gate 不被复制。
- 新测试使用真实 MaterialSaveSource 包装器，覆盖撤销、重复采用、异常恢复、重绑定 apply 和析构回调。

本次使用当前两份生产 `.cpp` 的隔离复跑中，上轮的 describe 自撤销、accept 递归采用和确认后 W4 保存都已经通过。隔离运行的定义见第 4 节，不冒称完整 SDK 成绩。

### 1.2 写入链与观察记录回收

- 当前成功记录证明“有效发布前提 → 确认发布版本”，不再只查输出版本。
- 证据限定到同规范目标、有效 Session/Binding、当前 chain_begin 和前序 ticket。
- 成功消费新观察到的外部版本会开启新链，旧来源链不能无条件继承。
- 没有新版本容器；有界记录及 lane 继续按原条件回收。
- 三模型真实文件与纯 coordinator 测试覆盖确认时机、不同来源、外部覆盖、失败空洞、Unknown 与容量。

**R2 不应触碰这一组已成立的算法，除非真实新增证据表明必须修改。**

## 2. R2-B01：BUSY 消耗了不可重放的编码完成

### 2.1 精确位置

| 文件 | 相关符号与责任 |
|---|---|
| `editor/persistence/src/SaveService.cpp` | `completeEncoding()` 新增 dispatching → BUSY；状态仍为 ENCODING |
| `editor/adapters/project_io/src/SaveExecution.cpp` | 编码 TaskScope completion：将 owning result 移入 completeEncoding，并用 `(void)` 丢弃返回值 |
| `engine/process/execution/src/Task.cpp` | `TaskRuntime::collect()` 对 group task 调用 deliver 后 reset operation 并 retire record；不重试业务交付 |
| `engine/process/execution/src/ExecutionRuntime.cpp` | `collectCompletions()` 只识别自身的收集重入，不知道 SaveService 的独立 dispatch 状态 |
| `editor/persistence/include/lux/engine/editor/persistence/SaveService.hpp` | 当前“所有 mutating service calls 返回 BUSY”的概括，需要区分已准入工作的结果接收 |

当前关键代码形状：

```cpp
// SaveService.cpp
PersistenceResult<void> SaveService::completeEncoding(
    SaveId id, PersistenceResult<EncodedArtifact> result)
{
    if (!impl_->onOwner())
        return failed(EPersistenceError::WRONG_THREAD);
    if (impl_->dispatching)
        return failed(EPersistenceError::BUSY);
    // ... 吸收结果、结清快照额度、把票据设为 READY 或 NotPublished
}
```

```cpp
// SaveExecution.cpp，结果交付一次后由 TaskRuntime 清理
[this, id](process::TTaskResult<EncodedArtifact, PersistenceFailure>&& result) noexcept {
    if (result)
        (void)service_.completeEncoding(id, std::move(*result));
    // error/cancel 分支同样消费并忽略 completeEncoding 的返回值
}
```

这不是调用方能在未来简单重试的 BUSY：`completeEncoding` 按值接收，返回时输入已被清理；TaskScope 回调也没有保留待交付值。后续 `collectCompletions()` 不会再发一次相同结果。

### 2.2 可到达的 owner 线程调用顺序

1. W1 已捕获、编码、真实发布，但还没有执行角色 accept。
2. W2 已准入并由 `SaveExecution` 提交编码；worker 已完成，拥有型结果等待 owner 收集。
3. owner 在收集函数之外调用 `saves.adoptCompletions()`，开始 W1 的 `ISaveSource::accept()`；此时 SaveService dispatching=true。
4. 受控 accept 回调调用一次 `runtime.collectCompletions()`。
5. 这不是 collect 递归：进入步骤 3 前 runtime 没在收集，线程仍是 owner；现有 ExecutionRuntime 检查不会因 SaveService 独立保护而拒绝它。
6. W2 的 TaskScope completion 被调用，向 SaveService 交付拥有结果。
7. `completeEncoding()` 返回 BUSY。SaveExecution 忽略这个结果；任务随后被 collected/retired，其编码值不再有可重试 owner。
8. 外层 W1 正常采用结束，SaveService dispatching 恢复 false，但 W2 永久保持 ENCODING，其票据仍 RESERVED。

不需要非法跨线程、自删除角色或递归进入已经运行的 collector。也不要求每次正常保存都走这条路径；缺陷条件是“合法 owner 收集与服务回调作用域发生嵌套”。

同类交接可能出现在 capture/describe 中收集其他已接收任务的结果。本轮先固定 accept 路径，避免扩大到无穷多插件组合。

### 2.3 可观察后果

- 编码已经完成，但 `status(W2)` 没有终态，长期 ENCODING。
- `requestCancel(W2)` 只设置 cancel_requested；等待一个已经丢失的 completion，仍无法结清。
- `takeEncoding()` 只返回 CAPTURED 项，不能重新取出 W2；反复 submitReady/collect 不会恢复它。
- 同目标 W3 即使已编码 READY，也不能越过 W2 的 RESERVED 票据。
- 额度及票据无法正常确认回收；销毁服务还会命中已有“仍有 ENCODING”的终止契约。这最后一点来自源码推导，隔离测试主动在析构前退出，没有把它冒称为新的真实引擎崩溃。

本次隔离日志同时覆盖成功编码和类型化编码错误：两者都被 BUSY 丢弃。R1 前同一交付顺序能够正常吸收结果，所以这不是从旧 P05 一直未修的相同故障。

### 2.4 不应把它解释成“回调已经得到 BUSY，责任结束”

新业务请求被 BUSY 拒绝，可以让调用者决定是否再次请求；**已经接受的编码工作，其结果必须由某一层可靠接收或保留**。

上轮指导允许不安全的重入业务操作明确拒绝，也要求不静默丢请求。需要在这里细化的是两个契约的交接，不能为了照字面执行“所有修改一律 BUSY”，丢掉 TaskScope 已经一次性交付的结果。

同样，不应该回滚 B01、允许递归 accept/ack，或把所有业务调用重新无保护开放。

## 3. 修正方法：优先把结果接收做成不调用业务代码的窄操作

### 3.1 必须成立的关系

```text
任务已准入
  → owning 编码结果产生
  → 结果仍有唯一负责交付的 owner
  → 服务可靠吸收，或有界保留待稍后吸收
  → 更新 ticket / 操作状态与额度
  → 业务 accept 在独立安全点执行
```

结果“交付尝试过”不等于“已经被吸收”；只有真正吸收后，负责交付的 owner 才能释放该结果。

### 3.2 推荐的最小方向 A：允许完成事实的窄吸收，继续禁止业务重入

审查现有 `completeEncoding()` 的全部操作，把它明确限制为：查找既有 ENCODING 操作、恰好一次转移拥有结果、结清快照计量、将对应写票据置 READY 或明确未发布、更新阶段。

在能够证明该路径不调用 extensible role、不递归采用、不删除外层正在执行的记录、不新增业务准入的前提下，使其在 owner dispatch 期间也可以可靠吸收已接收任务的完成事实。

注意：

- 外层 dispatch 必须持续有效。不能简单删掉 BUSY 检查后继续构造一个析构时无条件 `active=false` 的内层 DispatchScope；这样会提前解除外层保护。
- 可用保留先前状态的作用域、明确嵌套语义，或拆开“外层调度进入”与“叶子完成吸收”。只在原服务内部实现，不另建 gate。
- `acknowledge/registerSource/requestSave/requestCancel/adoptCompletions` 的必要重入保护保持原契约。
- `completeEncoding` 不执行 ISaveSource::accept/IPreparedRebind::apply；记录磁盘事实和业务采用仍分离。
- 外来／过期／重复结果保持明确返回，不能把任何状态都强行改 READY。
- 对 snapshot_bytes、op.cancel_requested、coordinator 容量和失败空洞的计量要做实际回归。检查与外层 capture 配额预留嵌套时不会双减或泄漏。
- SaveExecution 也要审查正常可能出现的返回值，不以 `(void)` 吞掉正常背压／交付失败；若选择此方案，应把“合法已接收结果能吸收”的契约落实为测试。

这只是建议的实现方向，不是本次已提交的补丁，也不声称未经测试删除一条 if 就足够。

### 3.3 备选方向 B：执行适配层的有界完成暂存

若服务明确不能在 dispatch 期间吸收完成，则由现有执行适配层可靠拥有完成结果，等下一个 owner 安全点交付。

要求：

- 每个已接受编码工作至多一个 owning 待交付结果，容量与已准入工作相联系；不能新增无限队列或额外线程。
- TaskScope 回调返回前先把结果转交给这个确定 owner。
- 尝试交付遇到 BUSY 时，结果不能已被按值函数消费。必须调整交付契约或明确推迟调用；仅“移走后看到 BUSY 再重试”是无效补丁。
- drain/shutdown 必须排空或正确结清暂存结果；不能 join 结束就忘掉未交付事实。
- 状态与清理仍在现有 SaveExecution/SaveService 边界，不引入通用 CompletionManager/OperationManager。

**在 A/B 中选择一个有证明的最小方案，不同时维护两套完成通道。** 不需要改动通用 ExecutionRuntime 的调度算法。

### 3.4 明确禁止的修法

- 去掉所有 DispatchScope，重新放开递归 accept 和执行期 acknowledge。
- BUSY 时把工作伪装成 CANCELLED/NotPublished，或删除 SaveOperation，让测试不再等待。
- 保存一次成功结果后重新运行 encoder，尤其是读取最新 live 内容；原任务保存的是已冻结内容。
- 把完成处理改为同步等待 dispatch 结束，阻塞 owner，或在回调内用 sleep 自旋。
- 增加无上界的全局完成队列，永久保活所有 Session/adapter。
- 只在 README 中禁止 collect，而既有公共组合仍能静默丢结果；若采用可检查的禁止策略，也必须保留已到达的结果供后续处理。
- 用进程终止代替正常 BUSY 的交接。
- 更改 C01/C03/C04 判定，或将这项新保存故障挂到旧 C03 延期。

## 4. 本次独立运行结果与证据边界

### 4.1 两份编译实现保持 Git Blob 一致

| 源码版本 | SaveService.cpp Blob SHA-1 | WriteCoordinator.cpp Blob SHA-1 |
|---|---|---|
| 原 P05 `5866f990...` | `78784eb7047bf7c1b939a83c5c771e2033196bac` | `3f8fa33d91f671a2de1c8f494148d36501ea1fb9` |
| 当前 P05 R1 `5a1ccbb47...` | `48b7bcf677b857bf1fe2574fad2c3559d40a36d6` | `32df3af3b63d3065fc955c0b00f11317466e35d2` |

当前副本由前版源码按连接器读到的差异恢复，再以 Git blob 规则核对。编译前已匹配；不是为触发缺陷而修改生产实现。两套源都在 probes 中。

### 4.2 结果

GCC 14.2，O0/O2，均启用 ASan/UBSan。16 次运行包括：当前源码上四项旧探针 × 两种优化，原/新源码上两类完成结果 × 两种优化。

| 测试 | 原 P05 | 当前 P05 R1 |
|---|---|---|
| 上轮自撤销、递归采用、确认后版本衔接 | 以前的失败记录继续保留，不在本轮全部重跑 | 两优化配置均通过当前目标 |
| accept 内交付另一个已完成的成功编码 | 正常吸收并继续发布 | BUSY；停在 ENCODING；取消不完成；后继阻塞 |
| accept 内交付另一个已完成的编码错误 | 正常记录失败并终结 | 同样 BUSY、丢失错误完成、持续阻塞 |

当前回归的典型输出：

```text
completion_accepted=0 rejected_busy=1 stuck_encoding=1
cancel_requested=1 still_encoding=1 follower_ready=1 follower_blocked=1 final_disk=V1
```

测试遇到持续 ENCODING 时主动以 **20** 退出，表示目标契约失败；避免再进入服务析构的终止路径混淆结果。不是 ASan 新报了内存错误，也不是原 111 项引擎测试运行失败。

### 4.3 不能扩大表述的边界

- 使用 shim 提供 expected、身份、CodeLease 的基础形状；C++23 只为 std::expected 替身，不要求仓库升级 C++20。
- 完成结果由测试在 accept 回调内直接交付，模拟 SaveExecution 现有调用形状。
- **本次没有编译实际 SaveExecution/TaskScope/stdexec，也没有真实线程执行器集成运行。** 实际收集→交付→退休路由来自固定源码阅读。
- 没有独立运行 Lux 的三模型、111 项 CTest、真实文件 IO、SDK 消费者、GPU、Android 或全套归档哈希验证。
- 真实 SDK 的新增 R2 回归必须由实施方完成，不能将本包隔离探针记为正式引擎通过或失败。

独立结果见 [probes/current_runs/results.json](probes/current_runs/results.json)。

## 5. 文件级实施与删除范围

| 文件／对象 | 本轮动作 | 不应做什么 |
|---|---|---|
| `editor/persistence/src/SaveService.cpp` | 区分服务业务 dispatch 与可靠完成吸收，维护嵌套期保护与恰好一次计量 | 不回滚角色撤销检查、递归采用/确认保护 |
| `editor/adapters/project_io/src/SaveExecution.cpp` | 核对成功、域错误、执行错误、取消的结果交付；按选定方案处理返回或保留结果 | 不继续吞掉允许出现的 BUSY，不重写执行器 |
| `.../persistence/SaveService.hpp` | 更新回调期 API 契约，说明完成结果是否总能吸收／如何保留 | 不再将所有 mutating call 一句话视为相同业务 |
| `.../io/SaveExecution.hpp` | 仅方案 B 确需时增加本对象拥有的有界暂存成员 | 不新建通用完成系统或库 |
| `editor/persistence/README.md` | 写清结果拥有者、吸收与采用的区别及 shutdown 顺序 | 不仅增加调用者自觉条款 |
| `editor/tests/persistence/models.cpp` 和同 target 测试文件 | 真实 runtime＋真实模型的五组回归；可拆同 target CPP 以免继续膨胀 | 不以本包 shim 替代实际执行器 |
| 现有测试 CMake、安装消费者配置 | 登记实际新场景、安装后执行一次关键交接 | 不创建新的业务 target/包 |
| `WriteCoordinator`、三 model、History、SessionStore、SessionState | 保持，跑回归 | 不重做版本衔接或另造当前状态 |
| `.internal/editor-redesign/` | 更新 R2 原因、责任、测试、修改文件和 SHA | 不创建第二份可变账本 |
| `dev_log/P05-R2/` | 冻结新的修复前/后结果与资格记录 | 不改写 P05/P05-R1 历史快照 |

淘汰的是“正常可达的结果交付失败被无条件忽略”的路径，以及“回调期所有操作只能 BUSY”的过宽契约。不是要求删除整个 SaveExecution 或 DispatchScope。

## 6. 真实 SDK 必测项

### R05-R2-01：真实执行器在 accept 中交付成功编码

复用现有 Fixture、HookSource 和真实 MaterialSaveSource，不修改模型私有状态。

1. W1 实际写文件但不采用；W2 通过真实 SaveExecution 的 CPU 调度进入 ENCODING。
2. 等 W2 worker 已完成但尚未 owner 收集。用现有 TaskInfo/明确测试同步判断完成，不依靠固定 sleep 碰运气。
3. 在 collector 之外调用 adoptCompletions。W1 的一次性 accept hook 内调用 runtime.collectCompletions。
4. 记录 collector 返回、W2 的任务完成与收集、保存阶段；退出 hook 后继续原正常 owner 驱动。
5. W2 必须完成发布/采用或准确执行已请求的取消；其原冻结字节不可替换。最终文件及基线准确，记录可确认回收。
6. 原版本若持续 ENCODING，先打印状态再失败退出；保留真实失败记录，不用重跑 encoder 避开。

此场景是首次收集与服务回调的嵌套，不是 runtime 已收集中再次 collect。不要构造一个只触发 ExecutionRuntime 自身 INVALID_STATE 的无关负例。

### R05-R2-02：域错误、取消与后继票据

通过实际任务结果通道交付编码失败与取消；在上面同样的 dispatch 重叠点吸收结果。W2 准确终结 NotPublished，W3 已 READY 的同目标写入能够继续。错误信息、取消意图和票据责任都保留。

至少测真实 encoder 的类型化错误；任务取消需用既有 stop 机制建立确定时序，不直接伪造“已 published 但取消成功”。

### R05-R2-03：describe/capture 期间收集另一已接收工作

已有工作 W1 的完成等待交付；请求 W2 的受控 describe/capture hook 内收集 W1。覆盖外层正常返回与抛普通异常/撤销拒绝。

W1 的完成不能丢；W2 的准入失败仍正确回收自己额度，不能把 W1 的账一起回滚；回调后服务保护恢复，无双减、超额或挂起。

### R05-R2-04：新防重入与原 R1 同时成立

保持 R05-01～08 原断言。尤其验证：允许接收编码结果之后，外层 accept 尚未返回时的递归 adopt 与 acknowledge 仍受阻，status 可读、token 撤销有效。

回调期完成吸收不得顺便调用其他 ISaveSource::accept；业务采用仍在既定安全点。已有保存事实、不同工作副本冲突、Unknown 和 Save As 高水位继续有效。

### R05-R2-05：drain、容量与安装消费者

以有限容量重复上述交接；正常收集、停止并 drain 后无 ENCODING 残留，无悬空 job/结果、无未结 RESERVED 票据，终态确认后恢复容量。

在安装 SDK 消费者中运行关键成功／失败完成交接，不能借源码构建目录补 DLL。保留原九组消费者与其实际用例，记录新增/改名映射，不以总计数代替语义。

## 7. 其他已披露事项——与本轮业务阻塞分开

R1 README 记录了一个 Physics2DDescription 冷构建顺序失败：编译先于静态类型头生成，后续重新构建通过。记录称相关规则未在本轮修改；本次没有独立复现或定位其全部依赖边。

应保留为独立工程风险并明确责任，最终 clean-build 资格不得忽略。不能把“后续构建成功、第二轮无工作”说成“首次全新构建无失败”。本轮不要求顺手重写 Physics2D 或生成系统，也不挂到 C01/C03/C04。

同一 README 说明主工作区有未提交的 `editor/project/src/ProjectBuilder.cpp` 非本轮修改，最终资格使用独立检出的实现 SHA。不要声称本次已独立确认用户主工作区干净；下一实施者应先核对并保护，不 reset、不纳入无关补正。

## 8. 验收与停点

- 原 111 项测试及原断言保留，新增 R2 行为先在本次固定 HEAD 上取得真实失败证据。
- 显式 `LUX_EDITOR_MIGRATION_STAGE=P05`；完整构建、二次无工作、相关 IO/模型/R1、实际依赖负例、SDK 重装与原九组消费者回归。
- 原 C01（P09/P12）、C03（P11）、C04（P12）继续为原 FAIL。R2 是新保存链自己的交付缺陷，不延期到旧编号。
- 实现与验收分开提交，正常推送；无 force-push，不修改 main。材料冻结至 `dev_log/P05-R2/`，仍停 P05，不实施 P06。
- 必测未运行或结果仅来自本包隔离程序，不允许报告完整 P05 R2 PASS。

## 9. 可直接发给实施方

> 上轮角色失效/递归采用和版本衔接补正已认可，但暂不进入 P06。只执行 P05 R2：新 dispatch BUSY 与现有 SaveExecution 一次性完成交付的衔接。
>
> 先用真实 SaveExecution/ExecutionRuntime＋MaterialSaveSource 包装器复现：W1 accept 中首次 collect 已完成的 W2 编码，当前 completeEncoding 返回 BUSY 被丢弃，W2 保持 ENCODING。同目标 W3 和取消也要观察；不要仅直接调用 shim 冒充实际 runtime。
>
> 保留 R1 的 callback 防重入。区分新业务准入与已接收工作的完成：让结果在同一 owner 可靠吸收，或由已有适配层有界暂存并交付；不能丢掉 owning result、重跑 encoder、强行取消或放开递归 acknowledge。嵌套完成返回不得解除外层 dispatch。
>
> 修正限于 SaveService/SaveExecution 交接、契约和测试。三模型、History、SessionState、WriteCoordinator 版本衔接、目录与包名保持；不新增通用管理器或线程池。
>
> 执行 R05-R2-01～05 与原 111 项、原 R05-01～08、真实 IO、依赖负例、SDK 消费者，显式 P05。原快照和冷构建失败保留；保护 ProjectBuilder.cpp 的既有工作区差异。实现及 P05-R2 验收分别提交，推送后停 P05。

## 10. 固定源码与证据来源

下列链接固定到本轮 HEAD；第 4 节隔离结果是本次独立运行，不能与仓库完整运行记录混同。

- [R1 验收说明](https://github.com/LUX-YU/lux-engine/blob/5a1ccbb47d2d08ffa8cfda907f74319c4a694068/dev_log/P05-R1/README.md)
- [SaveService.cpp](https://github.com/LUX-YU/lux-engine/blob/5a1ccbb47d2d08ffa8cfda907f74319c4a694068/editor/persistence/src/SaveService.cpp)
- [WriteCoordinator.cpp](https://github.com/LUX-YU/lux-engine/blob/5a1ccbb47d2d08ffa8cfda907f74319c4a694068/editor/persistence/src/WriteCoordinator.cpp)
- [SaveService.hpp](https://github.com/LUX-YU/lux-engine/blob/5a1ccbb47d2d08ffa8cfda907f74319c4a694068/editor/persistence/include/lux/engine/editor/persistence/SaveService.hpp)
- [SaveExecution.cpp](https://github.com/LUX-YU/lux-engine/blob/5a1ccbb47d2d08ffa8cfda907f74319c4a694068/editor/adapters/project_io/src/SaveExecution.cpp)
- [TaskRuntime collect 与退休](https://github.com/LUX-YU/lux-engine/blob/5a1ccbb47d2d08ffa8cfda907f74319c4a694068/engine/process/execution/src/Task.cpp)
- [ExecutionRuntime 的收集检查](https://github.com/LUX-YU/lux-engine/blob/5a1ccbb47d2d08ffa8cfda907f74319c4a694068/engine/process/execution/src/ExecutionRuntime.cpp)
- [真实模型及 R1 callback 测试](https://github.com/LUX-YU/lux-engine/blob/5a1ccbb47d2d08ffa8cfda907f74319c4a694068/editor/tests/persistence/models.cpp)
- [Coordinator 原与 R1 测试](https://github.com/LUX-YU/lux-engine/blob/5a1ccbb47d2d08ffa8cfda907f74319c4a694068/editor/persistence/test/write_coordinator.cpp)
- [111 项 CTest 归档](https://github.com/LUX-YU/lux-engine/blob/5a1ccbb47d2d08ffa8cfda907f74319c4a694068/dev_log/P05-R1/logs/ctest.log)
- [R1 九个修改文件清单](https://github.com/LUX-YU/lux-engine/blob/5a1ccbb47d2d08ffa8cfda907f74319c4a694068/dev_log/P05-R1/files.json)

原始 P05、启动补充与 P05 R1 指令继续作为历史规范，不覆盖当前施工账本。
