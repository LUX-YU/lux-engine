# P07 复审 R1：编译操作的公共所有权不能被隐式复制

**日期：2026-09-29**  
**仓库：LUX-YU/lux-engine**  
**实施分支：codex/editor-redesign-v4**  
**本轮审阅 HEAD：`f6598e65f9d77620f1374065a76d82e6e6e00d79`**  
**P07 实现：`2997f7e87eb2634e64fa9c258251c8128c8bacec`**  
**首次独立验收：`cdbc81b6c78c71ca0576f28c87dbe79db2459b3d`**  
**P07 前置验收：`38fd05d551e4556f9d713b793de05f98db1bd156`**

## 0. 判定与施工边界

**P07 主体成果认可，暂不进入 P08。本轮只修正 R07-B01：MaterialCompileOperation 和 FlowCompileOperation 仍可隐式复制，而它们的析构具有取消并释放任务的所有权副作用。**

这是同一个问题在两种具体操作中的表现，不是两套新架构任务。新的公共操作对象应当有一个控制／析构责任；内部共享完成状态可以继续由任务和唯一公共 owner 共同保活。

本轮不改投影重建策略、GPU 算法、Flow 编译器／linker、SceneRuntime、RunStore、WriteCoordinator、SaveService、History、SessionState 或 ExecutionRuntime 的算法。不搬目录，不新增通用 OperationManager，不新增业务 target 或包，不进入 P08。

原 P07 的 131/131 CTest 是原测试集合的历史运行结果，不因新增缺口而删除或改判。新的补正记录写入 `dev_log/P07-R1/`。施工材料继续只维护 `.internal/editor-redesign/`；原 V4、阶段快照、原始失败和 source SHA 保持不变。

## 1. 审阅依据、范围与结论强度

### 1.1 固定提交与附件

已通过 GitHub 连接器核对远端分支及两级验收父链：

```text
2997f7e87eb2634e64fa9c258251c8128c8bacec  P07 实现
  → cdbc81b6c78c71ca0576f28c87dbe79db2459b3d  独立验收
    → f6598e65f9d77620f1374065a76d82e6e6e00d79  验收摘要规范化
```

读取了用户提供的 P07 README、FILES 清单，以及原 P07 启动补充。不能把最新 HEAD 的直接父提交误写为实现提交：中间有独立验收提交。

### 1.2 本轮实际阅读

主要阅读了 SceneProjection/Hub、ViewportPresentation、HighlightRenderer、MaterialCompilation、MaterialPreviewStore、FlowCompilationService、Material 派生产物发布、Highlight 后端寻址，以及投影／高亮／真实编译测试。为核对析构后果，另读取了当前 ExecutionRuntime 的 Task 收集、派发与释放代码。读取了 P07 CTest 汇总及安装消费者 CMake。

本轮没有逐行穷举全部 P07 变更，没有逐项独立核实所有旧成员删除，没有运行完整归档检查器，也没有复算全部远端日志哈希。

### 1.3 三层证据不要混同

| 结论 | 本轮证据强度 |
|---|---|
| 两个公共操作类型可复制、可赋值 | 公开声明 + GCC/Clang 声明级编译验证 |
| 副本析构会操作同一个任务 | 当前 shared_ptr 成员与两个实际析构实现直接支持 |
| 未派发完成可能被移除，原操作停在未就绪 | 当前 TaskRuntime 控制流推导；本轮没有实际 Lux runtime 复现 |
| 原 131/131、PLAYER 11/11、11 组 44/44 SDK | 实施方归档；阅读证据不等于独立重跑 |

本包 `probes/` **只编译两段公共类声明**，其它领域类型用前置声明／小型占位类型提供。它不是完整公共头编译、不是 SDK 链接、不是实际运行时故障复现。不要将其记入正式引擎 PASS 或 FAIL。

## 2. 已成立且本轮不回滚的设计

| 范围 | 已读内容支持的进展 | 保留的限制 |
|---|---|---|
| 作者投影 | Hub 按来源及配置／环境共享记录；复用 buildSceneSnapshotPackage；原 Runtime 管实例和退休 | 变化内容使用完整快照重建，尚非组件级增量优化 |
| 高亮提交 | desired/prepared/accepted/rejected 分开；同 key 捕获失败后重试；背压保留 program 和提交寿命 | 受控 serial 测试不等于真实新产品 GPU 完成 |
| 后端视口 | Highlight 目标按完整 ViewHandle 保存；record kernel 读取当前 view 的目标 | 完整新产品双视口像素、布局和 GPU 退休仍是 P10/P13 范围 |
| Material | 真实编译、一次编码、冻结内容／配置／环境／目标键；迟到完成不冒充当前效果 | 本轮不扩大预览功能，也不重新设计目标身份 |
| Flow | 真实编译对象、失败链接、固定对象重试；有界记录和尝试数 | 不更改 ScriptArtifact ABI，不将无 linker 解释为作者域不可用 |
| 产物发布 | 派生产物不使用源码保存的来源继承资格；借用同一协调器 | 旧窗口源码＋pak 组合保存仍按报告作为旧适配暂留 |
| 目录 | 三个主题归组：scene/projection、material/preview、flowforge/compilation | 逻辑职责不应继续投射成大量细碎库 |

附件的删除清单列出了三个旧私有编译／预览头和旧算法成员的退出。本轮是对关键新路径的审阅，不把清单存在等同于已独立穷举所有旧调用。

## 3. R07-B01：共享 Impl 被误当成可复制的公共控制对象

### 3.1 精确位置

```text
editor/tools/material/preview/include/lux/engine/editor/material/MaterialCompilation.hpp
    MaterialCompileOperation

editor/tools/material/preview/src/MaterialCompilation.cpp
    MaterialCompileOperation::~MaterialCompileOperation()
    MaterialCompileOperation::cancel()
    MaterialCompileOperation::Impl::{task,completed}

editor/tools/flowforge/compilation/include/lux/engine/editor/flowforge/FlowCompilationService.hpp
    FlowCompileOperation
    FlowCompilationService::operation()

editor/tools/flowforge/compilation/src/FlowCompilationService.cpp
    FlowCompileOperation::~FlowCompileOperation()
    FlowCompileOperation::cancel()
    FlowCompileOperation::Impl::{task,completed,attempts}
```

两种类目前均为类似形状：

```cpp
class MaterialCompileOperation final {
public:
    ~MaterialCompileOperation();
    // 查询、cancel、result……
private:
    struct Impl;
    explicit MaterialCompileOperation(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl_;
};
```

没有显式禁止复制构造、复制赋值。私有的创建构造函数和工厂返回 `unique_ptr`，并不会使另外的隐式复制操作自动不可用。

实际析构均执行：

```cpp
cancel();
impl_->task = {};
```

复制操作会复制 shared_ptr，因此两个公共对象引用**同一个 Impl、同一个 Task、同一个 completed**。其中任意一个对象析构，都会取消并清除共同任务。当前注释说的是最后公共 owner 释放 Task，但语言层允许产生不止一个公共 owner，析构也没有“最后一个副本”的语义。

### 3.2 很普通的一次 auto 即可触发

Material：

```cpp
auto owning = take(MaterialCompileOperation::start(...));
{
    auto accidental_copy = *owning; // 当前声明允许
} // 并非数据快照析构；实际操作共同的 Task
// owning 仍然存在。
```

Flow：

```cpp
auto id = take(service.start(...));
{
    auto accidental_copy = take(service.operation(id)).get();
    // get() 返回 const FlowCompileOperation&，但 auto 去掉引用并复制对象。
} // 会操作本应由 service 记录独占控制的 Task
// service 中的原 operation 仍然存在。
```

这里没有 const_cast、裸指针删除、跨线程访问或非法内存写。只读借用经过一次普通值复制，就获得了会在析构时取消任务的独立外壳。

`std::move(*owning)` 也不是当前可行的所有权转移：没有真正的移动特殊成员时，右值可以被复制构造接受。本次声明级探针中 move traits 为 true，**不代表存在正确的 move constructor**。

### 3.3 为什么不只是“提前取消但结果正常返回”

精确而无竞争的负例应安排为：worker 的 TaskInfo.finished 已成立，但尚未执行业务 `dispatchTaskEvents()`，所以编译操作 `ready()` 仍为 false。然后销毁复制对象。

当前代码路径为：

```text
副本析构
  → cancel()
  → 清空共享的 Impl::task
  → TaskRuntime::release()
       请求停止
       必要时 collect 到任务已 collected
       清理 operation
       从 deliveries 移除尚未派发的 TaskId
       retire 任务记录
  → 原公共操作仍保有 Impl
  → 原 completed 尚未 emplace，并且原通知已经移除
```

Material/Flow 编译使用 `execution.submit` 返回的独立 Task，而不是在本路径自动 deliver 的 TaskScope group。普通 Task 的 collect 与业务 dispatch 分开。清空 Task 会按原 RAII 契约移除尚未派发通知；执行器没有义务在公共 owner 已释放时仍调用它。

因此，按当前控制流，原 Material 操作可长期 `ready()==false`；Flow 服务中的原记录也可能不可 acknowledge，继续占用容量。反复 collect／dispatch 不会重新生成那个已退休任务的完成。

这是**源码推导**，不是本次已经在真实引擎中运行出的输出。本轮正式实施必须先建立真实负例。禁止把文中的预期日志或声明级探针抄作运行证据。

### 3.4 复制赋值同样必须关闭

仅禁止复制构造还不够：

```cpp
*first = *second;
```

会替换公共操作的 impl_，造成控制身份混同并放弃原控制关系。无需等待这个变体出现另一个崩溃，才关闭显然不属于该类型语义的隐式赋值。

不要把它改成共享取消句柄后继续保留同一个类名：现有 Material 工厂与 Flow 服务的拥有模型，并没有要求任意公开副本共享任务控制权。

## 4. 替代契约：公共 owner 唯一，异步状态仍可共享

### 4.1 不新增生产类型

推荐直接在两种现有操作类 public 区域明确删除四个特殊成员：

```cpp
MaterialCompileOperation(const MaterialCompileOperation&) = delete;
MaterialCompileOperation& operator=(const MaterialCompileOperation&) = delete;
MaterialCompileOperation(MaterialCompileOperation&&) = delete;
MaterialCompileOperation& operator=(MaterialCompileOperation&&) = delete;
```

Flow 同样处理。

本轮选择“对象自身不可复制、不可按值移动”，因为实际 API 已有足够的转移机制：

- Material 的唯一公共 owner 是工厂返回的 `unique_ptr<MaterialCompileOperation>`；转移该指针。
- Flow 的操作对象由 `FlowCompilationService` 的 `unique_ptr` 记录持有；调用方保留 FlowCompileId 或借用 const 引用。
- 内部 shared_ptr<Impl> 保留，以支撑在途工作／完成交付的数据寿命。

不需要为用户本来不需要的按值移动，添加 moved-from 空状态、额外 cancel 分支或另一套所有者计数。若发现真实现有调用方确需按值迁移，先记录具体用途；不要默认一个 `= default` move 就正确。

### 4.2 查询要复制值，不复制控制对象

| 需要 | 继续使用的现有类型或操作 |
|---|---|
| 记住请求身份 | MaterialCompileId / FlowCompileId |
| 读取当前状态 | 按既有期限借用操作引用并查询 |
| 保存已编译结果 | result() 返回的拥有型 `shared_ptr<const CompiledMaterial/CompiledFlow>` |
| 保存独立描述 | key、观察版本、TaskId 等值；仅在确实需要时复制已完成 attempt 值 |
| 转移 Material 任务所有者 | move `unique_ptr<MaterialCompileOperation>` |
| Flow 取消／结果回收 | service.cancel(id) / service.acknowledge(id) |

不增加可复制 OperationHandle，只为让原错误的 auto 写法继续编译。

### 4.3 原执行器无需为错误公共 owner 兜底

不要改 TaskRuntime::release，强迫所有已丢弃 Task 仍派发回调；那会改变其它任务的取消与安全销毁协议。不要回滚 P07 已修复的 task/state 环，也不要移除当前析构中的 Task 清理。

单一公共 owner 结束时取消和清理是合理行为。错误的是类型允许凭空产生另一个也执行同一行为的 owner。

## 5. 文件、成员与删除／保留清单

| 文件 | 本轮修改 | 必须保留 |
|---|---|---|
| MaterialCompilation.hpp | 删除隐式复制／赋值与右值复制入口，明确对象及 unique_ptr 转移契约 | start/result/key 等既有公共方法、名称和返回方式 |
| FlowCompilationService.hpp | FlowCompileOperation 同样不可复制／移动；说明 operation(id) 是借用，不转移控制权 | service 的 start/retryLink/cancel/acknowledge 与结果身份 |
| MaterialCompilation.cpp | 原则上不改算法；如需修改误导性的“最后公共 owner”注释，可改为“唯一公共 owner” | 完成共享状态、恰好一次完成及既有析构收尾 |
| FlowCompilationService.cpp | 同上；核对调用方无需复制操作对象 | 固定对象链接重试、尝试历史和原容量 |
| 原 compilation.cpp 或同 target 新测试 CPP | 加类型契约断言、指针转移／借用正向回归；原测试保留 | 真实编译、乱序、失败、固定重试、真实发布和所有旧断言 |
| 原测试 CMake | 如需增加编译负例，挂原 target／测试分组 | 不为本轮新建业务库或编译框架 |
| installed-consumers/projection-compilation | 使用安装头执行同一类型契约与正确使用正向用例 | 原 projection/compilation 两消费者及已有依赖隔离 |
| 两主题 README | 区分公共 operation owner、任务内部状态和可复制结果值 | 不改 stage/gpu/toolchain 等验收边界 |
| .internal/editor-redesign | 记录 R07-B01、修改／删除入口、测试映射 | 保持唯一可变账本 |
| dev_log/P07-R1 | 新的修复前、后证据与冻结收据 | 原 dev_log/P07 不回写 |

**本轮没有必须删除的整个生产文件，没有必须引入的新生产类。淘汰的是两个类型的隐式复制构造、复制赋值以及右值退化复制入口。**

实际源码中的按值复制调用若被编译器揭露，应改为正确的借用、ID、结果值或 unique_ptr 转移。不能换名为 LegacyOperation 继续保留副本副作用。

## 6. 必测项 R07-R1-01～05

### R07-R1-01：真实公共头的不可复制／不可移动契约

本包 `tests/owner_contract.cpp` 是使用真实头的目标断言，不是已运行 SDK 测试。

在修复前安装 SDK 编译一次，记录失败明确来自 static_assert；修复后重新安装再编译，断言全部通过。包含 copy_constructible、copy_assignable、move_constructible、move_assignable 四项，两个类型都检查。

不能因为缺少依赖头、找不到包或 linker 失败，就声称负向契约验证成功。建议以仅编译／语法检查区分这类错误。

### R07-R1-02：Material 副本提前销毁的真实失败基线

使用原 `compilation.cpp` 的真实 MaterialSession/materialSource、MaterialCompileOperation、ExecutionRuntime。

1. 开始一次编译，保留工厂返回的 unique_ptr。
2. 等实际 TaskInfo.finished 成立，尚未派发业务完成；断言原 operation.ready()==false。
3. 在局部块中复制 `*operation`，块退出销毁副本；不要销毁原 unique_ptr。
4. 执行常规 collect/dispatch；检查原操作能否得到真实结果。
5. 修复前如丢失通知，输出 owner_alive、worker_finished、ready、可观测任务状态等，再按目标失败退出；禁止无限等候或重跑编译。

该用例在修复后应当**编译不通过**；保存在 before/ 中，不把包含非法复制的测试移进常规构建再要求其运行。常规测试改为 unique_ptr 转移，并验证原编译仅产生一次正常终态。

### R07-R1-03：Flow 只读借用被复制的真实失败基线

使用真实 FlowSession、FlowCompilationService(capacity=1)、Compiler 和 ExecutionRuntime。

1. 开始编译，可以沿已有场景使用不存在的 linker，使任务稳定完成一个可重试的链接错误。
2. 等 worker TaskInfo.finished，暂不 dispatch，原 operation 尚未 ready。
3. 使用 `auto copy = take(service.operation(id)).get();`，随后销毁 copy，原 service 保持存在。
4. 常规 collect/dispatch 后检查原记录是否有完成；若丢失，则 acknowledge 仍 BUSY、第二次准入仍 CAPACITY，输出实际结果。
5. 不对 private Impl 写标志，不使用假完成，不拿本包声明级程序替代真实 SDK。

修复后，该值复制成为编译负例；正确的 `const auto&` 借用继续允许，等待完成、固定对象 retryLink、终态确认和容量恢复全部正常。

### R07-R1-04：正确拥有和结果复制保持可用

Material 的 unique_ptr 可以移动给另一个唯一 owner；移走后原指针为空，操作身份／原快照不变，编译完成正常，最终清理一次。不要按值移动 *operation。

Flow 用 ID 或 const 引用查询；复制 `result()` 返回的共享不可变产物不会触发任务取消。确认 service 记录后，外部已持有的产物仍可按其拥有契约读取／发布。保留原错误重试、链接次数上限及已有回归。

两种独立操作之间相互不取消，ID 和结果不会因为测试使用 auto 而混同。源码模型 current/dirty 仍保持原约束。

### R07-R1-05：安装与原完整回归

显式 P07。保留原 131 个测试名及行为断言、PLAYER 11 项、原 11 组消费者。测试总数可以合理增加，不预先杜撰应变成多少项。

SDK 重装后，以原 `projection-compilation` 消费者使用安装头验证类型契约和正向流程，不借源码／构建树头补齐。原 X07-01～07、P05/R1/R2、P06/R1、实际编译／文件 IO／后端绑定与依赖负例继续通过。

本轮不要求新增 Android 或 P10/P13 完整新产品 GPU 资格。没有修改 modules 头时，不为形式新增无关的 modules 同步动作。

## 7. 本包独立声明探针的实际输出

GCC 14.2 和 Clang 17，C++20，均得到：

```text
MaterialCompileOperation copy_constructible=1 copy_assignable=1 move_constructible=1 move_assignable=1
FlowCompileOperation copy_constructible=1 copy_assignable=1 move_constructible=1 move_assignable=1
```

启用 `REQUIRE_UNIQUE_OWNER` 后，两编译器均因八个目标断言失败返回非零。正常 traits 程序编译／运行均退出 0。

这些 0 表示成功测出了当前类型特征，不表示类型契约已经满足。move traits 仅说明右值可被当前构造／赋值接受，不证明具有正确移动语义。

探针不运行上述类的析构，不运行 Lux 任务，不检测泄漏；错误通知被丢弃的运行后果目前仍来自源码推导。`probes/results.json` 保存命令、编译器、退出码及日志哈希。

## 8. 交付与禁止扩张

实施先保护 `ProjectBuilder.cpp` 既有修改，核对分支与祖先关系，不 reset、不修改 main。

先取得真实头和真实 runtime 的修复前证据，再收紧两类特殊成员，迁移真实调用，更新安装与测试。将准确测试过的实现 SHA 绑定到新收据，实现和验收分开提交，正常推送后停在 P07。

C01（P09/P12）、C03（P11）、C04（P12）保持原 FAIL。本轮的新编译操作问题不挂到旧 C03。Physics2D 历史失败及 P06 窄修复记录保持；复用已有构建树不宣称首次全新冷构建资格。

若没有完成必测真实 SDK／runtime 的前后核验，仅跑本包 traits 不得报告完整 P07 R1 PASS。若修复前现实观察与上文控制流推导不符，应提交原始结果并解释具体代码路径，不能改测试制造失败。

## 9. 可直接交给实施方

> P07 主体方向认可，暂不进入 P08。只执行 P07 R1：MaterialCompileOperation 与 FlowCompileOperation 的隐式复制／赋值造成公共控制 owner 重复。
>
> 先以当前固定实现和真实 SDK 验证两个类型可复制，并建立 worker 已完成、业务完成尚未派发时复制外壳再销毁的真实负例。保留原 unique_ptr/service，检查通知、ready、确认及容量，不使用隔离声明程序冒充运行时证据。
>
> 两个 operation 对象明确禁止复制和按值移动；Material 转移 unique_ptr，Flow 通过 service 的 ID／借用使用。保留内部 shared_ptr<Impl> 以支撑异步状态寿命，保留原析构清理，不改 TaskRuntime 兜底，不加 shared-owner facade、use_count 协议或新管理器。
>
> 使用真实公共头 static_assert／编译负例锁定四种特殊成员，验证正确指针转移、只读借用、独立结果值、完成与重试。保留原 131 项及断言，执行 PLAYER、实际 IO／编译／后端／依赖及 11 组 SDK 回归，显式 P07。
>
> 不改目录、包名、三模型、History、SessionState、SceneRuntime、RunStore、保存协调器或编译算法。原证据保持，更新唯一施工账本并冻结 dev_log/P07-R1；C01/C03/C04 保持 FAIL，保护 ProjectBuilder.cpp。实现与验收分别提交并正常推送，停在 P07，不进入 P08。

## 10. 固定源码索引

以下均指向本次固定验收 HEAD，不随分支更新漂移。

- [Material 操作公开声明](https://github.com/LUX-YU/lux-engine/blob/f6598e65f9d77620f1374065a76d82e6e6e00d79/editor/tools/material/preview/include/lux/engine/editor/material/MaterialCompilation.hpp)
- [Material 编译、完成及析构](https://github.com/LUX-YU/lux-engine/blob/f6598e65f9d77620f1374065a76d82e6e6e00d79/editor/tools/material/preview/src/MaterialCompilation.cpp)
- [Flow 操作与服务公开声明](https://github.com/LUX-YU/lux-engine/blob/f6598e65f9d77620f1374065a76d82e6e6e00d79/editor/tools/flowforge/compilation/include/lux/engine/editor/flowforge/FlowCompilationService.hpp)
- [Flow 编译、重试、完成及析构](https://github.com/LUX-YU/lux-engine/blob/f6598e65f9d77620f1374065a76d82e6e6e00d79/editor/tools/flowforge/compilation/src/FlowCompilationService.cpp)
- [普通 Task collect、dispatch、release](https://github.com/LUX-YU/lux-engine/blob/f6598e65f9d77620f1374065a76d82e6e6e00d79/engine/process/execution/src/Task.cpp)
- [真实编译与发布测试](https://github.com/LUX-YU/lux-engine/blob/f6598e65f9d77620f1374065a76d82e6e6e00d79/editor/tools/material/preview/test/compilation.cpp)
- [现有安装消费者](https://github.com/LUX-YU/lux-engine/blob/f6598e65f9d77620f1374065a76d82e6e6e00d79/cmake/installed-consumers/projection-compilation/CMakeLists.txt)
- [投影与有界回收](https://github.com/LUX-YU/lux-engine/blob/f6598e65f9d77620f1374065a76d82e6e6e00d79/editor/tools/scene/projection/src/SceneProjection.cpp)
- [高亮三阶段与受控提交测试](https://github.com/LUX-YU/lux-engine/blob/f6598e65f9d77620f1374065a76d82e6e6e00d79/editor/tools/scene/projection/test/highlight.cpp)
- [真实 Highlight 后端](https://github.com/LUX-YU/lux-engine/blob/f6598e65f9d77620f1374065a76d82e6e6e00d79/modules/function/render/features/src/renderer/features/highlight/HighlightFeature.cpp)
- [P07 验收](https://github.com/LUX-YU/lux-engine/blob/f6598e65f9d77620f1374065a76d82e6e6e00d79/dev_log/P07/README.md)
- [P07 CTest 汇总](https://github.com/LUX-YU/lux-engine/blob/f6598e65f9d77620f1374065a76d82e6e6e00d79/dev_log/P07/logs/ctest.log)

本文是已有 P07 唯一 owner／完成寿命要求的具体补正，不把新设计草图冒称为已经提交的实现。
