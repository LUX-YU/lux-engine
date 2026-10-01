# P11 独立复审与 R1 定向补正

日期：2026-10-01  
仓库：`LUX-YU/lux-engine`  
实施分支：`codex/editor-redesign-v4`  
实现：`3bbc7312a3d130a76f2c1807a9e3bca0d413e7ee`  
验收 HEAD：`24896eaa89ea426c29b4c92daa0f4a0c3bba6fe3`

## 0. 决定、范围与证据等级

**P11 主体交付认可；暂不进入 P12。只执行本文件定义的 P11 R1。**

本轮需要修正两项同类但不同位置的保护作用域问题：

- **R11-A：** `SaveService::prepareSource()` 自己取得 dispatch 准入后，拒绝输入的析构晚于 dispatch 退出，析构回调可以准入新业务。
- **R11-B：** `ContributionRegistry` 的 active 保护没有覆盖同一复合发布使用的 `CommandRegistry`。批次／反射／清理回调仍可直接发布命令目录，使两个目录不再属于同一批。

不改变五层设计，不新增主阶段，不再开展目录大改。复用现有业务和执行框架；这不是任意插件沙箱、安全隔离或任意异常恢复工程。

### 本轮已做

核对远端 HEAD 与直接父提交；读取新版 P11 施工规范、P11 README/收据片段和 CTest 汇总；审阅 CommandRegistry、CommandDispatcher、CodeLease、SessionFactory、SessionInstallation、SaveService 注册路径、Contributions、EditorExtension、BuiltinContributions、CommandMenu，以及命令/贡献/三模型安装相关测试和构建入口。

### 本轮未做

没有构建完整 Lux 引擎、实际 Windows SDK、V7 DLL、GPU 或运行归档验证器；没有复算全部远端归档哈希。新增源码问题应称为**源码控制流审阅结论**，真实 SDK 修复前失败需实施方补取。

`probes/` 是缩小的 C++20 控制流见证，不是完整生产 CPP，更不是实际 Lux SDK。它只交叉检查局部对象析构次序、两项独立 guard 的可达性和说明性修正对照。它不证明完整引擎崩溃、文件损坏、DLL 卸载、性能或跨平台资格。

## 1. 本轮应保留的成果

1. 命令 query/execute 分离；CommandInvocation 固定 target 和 owning arguments；默认 PINNED 与显式 CURRENT_REGISTRATION 区分。
2. CommandRegistry 的调用期持有独立强 handle；自己的 query/execute 不允许重入发布。Dispatcher 使用有界队列，BUSY 保留队首与输入。
3. 三种内容工厂用原 Process 读取与解码，再在 owner 线程构造真正 Session/History；SessionStore 仍独占源。
4. SessionInstallation 使用隐藏会话和预备保存注册，无回调提交点再共同可用。立即 registerSource 复用同一注册准备算法。
5. V7 使用原 PluginLibrary；正式 loader 在贡献回调前核对出口、表大小、版本、ABI 和数量；不接受 V6 为正式新协议。
6. 接收端 code pin、控制块分配归属和未变条目解包避免包装链的修正保留。不能通过无限保活 DLL 替代这些真实生命周期。
7. 旧 ConfigurationValue/EditorReflection 原体已迁到正式作者层；需要的旧产品接线按 P12 限定，不重新包装为正式接口。

README 记载完整 213 项、CPU 187 项、PLAYER 12 项、24 组安装消费者及实际 GPU/输入。此次读取的完整 CTest 尾部确有 213/213。其余成绩按该提交的归档记录理解，不冒充本次独立复跑。

## 2. R11-A：拒绝的保存源在服务保护之外析构

### 2.1 位置

```text
editor/activities/persistence/src/SaveService.cpp
    SaveService::prepareSource()
    SaveService::Impl::prepare()
    SaveService::canPrepareSource()
```

当前核心顺序：

```cpp
struct Input {
    CodeLease code;
    std::unique_ptr<ISaveSource> source;
};
Input owned{std::move(code), std::move(source)};
if (!impl_->onOwner()) return wrongThread();
if (impl_->dispatching) return busy();
const Impl::DispatchScope dispatch{impl_->dispatching};
if (!owned.source) return invalid();
auto prepared = impl_->prepare(id, *owned.source, owned.code);
if (prepared)
    prepared->state_->owned_source = std::move(owned.source);
return prepared;
```

以上是删除命名细节后的说明摘录，不是可直接粘贴的生产补丁。

成功时 source 被转移，不触发这个分支；失败时 source 仍在函数外层 `owned` 中。`dispatch` 后构造、先析构，因此拒绝返回的退出顺序为：

```text
impl_->prepare 返回 INVALID_ARGUMENT 或重复注册 BUSY
→ DispatchScope 恢复 dispatching=false
→ owned.source 析构
→ 外部 ISaveSource 析构回调调用 canPrepareSource/requestSave
→ 服务再次接受新业务
→ owned.code 才释放
```

**代码 pin 保住了虚析构，却没有保住服务的业务准入作用域。** 这两项不能混为同一个保证。

### 2.2 实际可构造的 SDK 场景

使用已安装的真实 MaterialSession、MaterialSaveSource、SaveService 和 WriteCoordinator：

1. A 是正常已发布、已注册保存角色且有可写目标的会话。
2. 准备一个新的 `ISaveSource` 包装器，内层使用真实 MaterialSaveSource；其析构记录 `canPrepareSource()`，并尝试 `requestSave(A)`。
3. 调用 `prepareSource(A.id, wrapper, code)`，制造已有来源的重复注册拒绝。也独立测无效 SessionId。
4. 保留返回值和析构期间观察，再在正常 owner 点清理意外准入的工作。
5. 目标断言：拒绝保持原错误；包装器销毁一次；析构中服务仍拒绝新业务；退出之后可正常发起保存；原注册、作者内容、History、observed、checkpoint 不变。

当前源码可推导出第 5 步的“析构中拒绝”不成立。**此处不声称磁盘已经写坏或作者内容已经被修改。** 明确的问题是：公开准备方法清理尚未完成，新的业务请求已经能进入。

### 2.3 修改方案

先建立能够保证 code/source 顺序的外层输入，以处理准入本身失败；获得服务 dispatch 后，立即将整个输入移动到后声明的局部拥有单元，再执行可能拒绝的检查：

```cpp
Input incoming{std::move(code), std::move(source)};
// owner 与现有 dispatch 的原准入检查。
const Impl::DispatchScope dispatch{impl_->dispatching};
auto admitted = std::move(incoming);
// 从此只使用 admitted.source / admitted.code。
// 成功时转移 source；失败和异常清理发生在 dispatch 析构之前。
```

约束：

- 不删除重复注册/无效身份校验，不把错误改成成功。
- 不因这个问题改变 Session gate 或引入第二个保存 busy 标志。
- 已经 BUSY 的入口仍不改变外层保护；不强行解除外层 dispatch。
- 保留 P05 R2：已接受编码完成能够被可靠接收，且内层完成不能解除外层保护。
- 析构中的新业务准入可以明确 BUSY；已接受工作完成不是新业务，不能一起丢弃。
- Wrong-thread 属于既有调用契约，不能用本次修补偷偷保证任意线程均能销毁全部线程亲和对象。
- 外层输入和内层输入属于一次拥有权转移，不是两份源或两份清理责任。

### 2.4 原测试为何没有覆盖

`editor/tests/integration/session_factories/installation.cpp` 的 `installationStages()` 在八个边界提前退出，检查可见性、销毁次数和容量回收；`ReentrantSource` 在 describe 中验证 PreparedSessionData 的 BUSY 前置检查。

这两类测试有价值，但并没有在 **prepareSource 拒绝后的输入虚析构**中重新调用服务。不得删原断言；补这一组合即可。

## 3. R11-B：复合注册批次仅保护了一半 owner

### 3.1 位置

```text
editor/application/extensions/src/Contributions.cpp
    ContributionRegistry::withSnapshot()
    ContributionRegistry::applyPending()

editor/activities/commands/src/CommandRegistry.cpp
    CommandRegistry::canPublish()
    CommandRegistry::publish()
```

当前 ContributionRegistry 的 Scope 只修改自己的 `impl_->active`。CommandRegistry 的 canPublish 只看自己的 owner、calling、dispatching 和 revision；它不知道自己正在参与一个受保护的贡献批次。

`applyPending()` 的前置 canPublish 在反射回调前执行；之后仍会执行注册函数、配置 reflection、反射 commit、旧快照析构及通知。单次前置检查不是这些后续步骤之间的排他权。

### 3.2 最小反例：固定批次内直接发布命令目录

初始完整贡献 A 中含命令 A，实际 CommandRegistry 也发布 A。

```text
ContributionRegistry.withSnapshot(callback)
→ active=true，callback 获得 A 的强快照
→ callback 或其中的真实 ViewFactory 调用 commands.publish(C)
→ commands.calling/dispatching 都为 false，公开发布成功
→ callback 返回
→ ContributionRegistry 仍公布 A，CommandRegistry 已公布 C
```

这里使用现有公开发布入口，不需要改内存、强制类型转换或跨线程访问。可以选择把这种调用定义为“不准在此处执行”，但必须由能力限制或原 owner 的可检查准入落实，不能留下一个会成功的旁路。

### 3.3 更严重的组合：候选失败，但一部分目录已变

```text
准备候选 B
→ applyPending 通过 commands.canPublish
→ 执行 B 的反射回调
→ 回调通过 commands.publish(C) 改变正式命令目录
→ B 的后续 reflection/configuration 检查失败
→ applyPending 返回错误，贡献 current 仍为 A
→ 命令 current 却已成为 C
```

源码当前注释声称候选失败保持所有活动目录不变；这一组合不能满足该声明。

同类边界也存在于已提交批次的旧值析构和 changed 通知：仅令嵌套 `registry.applyPending()` 返回 BUSY，不足以阻止嵌套 `commands.publish()`。

不能把整个问题解释为必须沙箱任意恶意插件。这里只约束本系统提供的注册变更入口，正对应本阶段“回调中不直接替换、整批固定快照”的契约。

### 3.4 修正方向

复合批次的保护应真正延伸到参与的 CommandRegistry：

- 进入固定快照回调或复合发布前，由 CommandRegistry 自己授予有界的读取/发布作用域。
- 普通 publish 在该作用域中明确返回 BUSY，或只能把候选入队留到下一外层安全点。
- 复合发布持有者在完成反射及全部验证之后，凭自己的窄准备责任执行既定无普通失败 commit；不能临时解除保护再调用公开 publish。
- 下层 CommandRegistry 不 include ContributionRegistry 或 application。窄 scope/permit 定义在原命令 owner 所在模块，由外层组合；不增加全局注册管理器、互斥锁或通用事务框架。
- 继续支持独立 CommandRegistry 的合法用例；没有必要让所有单独命令用户都建立 ContributionRegistry。
- guard 的退出顺序覆盖候选/旧值清理及通知。进入前失败不夺走现有外层责任，退出恢复原状态。
- 合法 query、固定 handle 调用、enqueue、完成结果接收的行为应逐项裁定，不能为了方便把所有读操作一律封死。

**不能用以下方式修：**

1. 只在反射回调之后再检查 canPublish；反射或其他目录可能已经提交，且已发生的命令变更不能被这次检查抹掉。
2. 只要求业务“不要直接调用”，却继续向该调用路径开放能立即成功的发布能力。
3. 将失败候选导致的命令变化算成“允许的部分成功”，偷改原原子安装语义。
4. 全局锁住 Editor 或复制 CommandRegistry 到第三个 owner。
5. 保活所有旧快照/DLL，借此掩盖保护遗漏或包装链。

### 3.5 原测试为何没有覆盖

`editor/application/extensions/test/contributions.cpp` 检查了工厂/代码清理/通知中的 `registry.applyPending()` 被 BUSY 拒绝，并允许 enqueue 等待下一轮。

它没有在同一位置调用被复合发布使用的 **CommandRegistry::publish()**，因此只证明了顶层重入门有效，没有证明所有参与者共享同一发布纪律。

## 4. 允许修改与禁止扩张

主要生产范围：

- SaveService.cpp，必要的现有公开契约注释。
- CommandRegistry.hpp/.cpp，必要的窄作用域/发布权限。
- Contributions.cpp，复合批次的作用域接线。
- 现有测试、对应安装消费者和证据验证器。

不改五层目录、语言版本、包名、磁盘格式、Writer FIFO、SourceBinding、History、高水位、SceneRuntime、Renderer、Process 或原 node-editor。若新增公共头只是为了一个作用域，优先放进现有相关语义头。

V7 若只是实现修补，不机械升版本。若确实改变导出表布局/调用约定，必须使用新版本且准确迁移；无论是否升号，重算实际 ABI 指纹、重新安装和构建消费者，不用旧二进制冒充新资格。

## 5. R11 验证矩阵

每行是行为主题，不要求每行新增 executable；沿用现有测试程序。

| 编号 | 输入/时机 | 必须观察 |
|---|---|---|
| R11-01 | 重复保存角色、无效 id，拒绝输入虚析构调用 canPrepareSource/requestSave | 原拒绝准确；清理内新业务 BUSY；退出后恢复；源/注册/历史不变；只清理一次 |
| R11-02 | 准入前已 BUSY、最后 code pin、正常成功 source 转移 | 原外层 gate 不变；节点/角色先于 code；成功后注册照常使用；不制造泄漏 |
| R11-03 | 清理回调首次接收另一个已准入编码完成 | 完成能被可靠接收；新业务仍拒绝；不丢结果、不重复 encoder、不释放外层保护 |
| R11-04 | withSnapshot 内的真实工厂直接 commands.publish | 被拒或明确排后批；整个当前批仍使用原命令/工厂；退出后普通合法发布可用 |
| R11-05 | 反射回调试图 commands.publish，之后候选验证失败 | 命令集合、贡献集合及反射相关实际事实仍为旧批；不能只比较返回码 |
| R11-06 | 成功发布后的旧对象清理与 changed 通知试图 commands.publish；另 enqueue 下一批 | 当前已提交各目录一致；嵌套更新不立即发生；合法排队下一轮可见；无历史包装链回归 |
| R11-07 | 三种真实内容工厂、所有隐藏准备失败点、C03、新旧菜单 | 原语义保持；不以 mock 代替真实源；原 C03 后继继续 PASS |
| R11-08 | V7 真实装载与卸载、最后 weak/control block、头与 ABI、SDK | 接收模块控制块/代码尾部寿命保持；不兼容输入在回调前拒绝；干净安装消费 |

### 真实反例必须先取

在原实现 SHA 上叠加仅测试改动。保留测试差异与 stdout/退出码，说明 actual 与 intended；不能使用本包缩小探针当真实 SDK 的 before。

本轮不要求新建 Linux 环境、不补旧慢算法样本，不新开任意热卸载或跨平台矩阵。已经存在的 DLL、GPU 和原生输入回归继续按原适用范围运行。

## 6. 缩小探针结果的含义

见 `probes/results.json`。GCC 14.2 与 Clang 17，分别 O0/O2：

- cleanup 原控制形状：拒绝后析构看到业务可用；说明性修正对照在作用域内拒绝。
- batch 原控制形状：贡献版本未变，命令版本可单独推进；说明性对照阻止。
- failed_candidate 原控制形状：外层失败而命令目录已改变；说明性对照保持一致。

共 24 次短运行：12 次原形状违反目标断言，12 次说明性对照满足断言。数字只是脚本结果索引，不是完整 SDK 认证或统计性能实验。

## 7. 验收、提交与停点

1. 继续 `codex/editor-redesign-v4` 的正常后继；不 reset，不修改 main，不覆盖用户文件。
2. 原 P11 归档原字节保持，新增 `dev_log/P11-R1/`；唯一可变账本继续原 `.internal/editor-redesign/`，不创建第二份事实源。
3. 实现与验收分别提交。最终资格绑定同一实现 SHA，显式 P11 + STRICT。
4. 保留原 213 项行为和原断言；新增回归归入原程序。CPU、PLAYER、原 24 组 SDK、实际 V7 与相关 GPU/输入重跑，数量按实际映射报告，不设凑数配额。
5. C03 正式与旧活动路径继续 PASS；历史 C03 FAIL 保留。C01/C04 当前责任仍 P12，不将此次问题挂到旧编号。
6. Linux、IME、sanitizer 未运行仍 NOT_RUN；原性能 PARTIAL 不改写。
7. 报告实际新工作树路径/HEAD，以及 ProjectBuilder 用户补丁是否应用。原工作区停在旧提交是保全事实，不写成已经升级。
8. 正常推送后**停在 P11，等待本轮复审；不自动进入 P12。**

## 8. 可直接发给实施方

> P11 主体成果认可，暂不进入 P12。只执行 P11 R1：修正 prepareSource 拒绝输入清理作用域，以及 ContributionRegistry 与 CommandRegistry 的复合发布保护。
>
> 先取得真实 SDK 反例；保留输入所有权与原 dispatch、CommandRegistry 的准确边界，禁止新业务从清理或批次回调中绕过准入。已准入异步完成继续可靠接收。复合发布的保护由参与 owner 的窄作用域承担，不能只在回调前检查一次，更不能放开全部接口或引入全局 Manager。
>
> 修改仅限相关原模块和回归，不改目录与底层引擎。原 P11 快照、C03 已修结果、C01/C04 责任、用户补丁和 main 保持。显式 P11+STRICT，完成后分别提交、推送并停下复审。

## 9. 主要源码索引（固定 HEAD）

所有路径均相对于 `LUX-YU/lux-engine@24896eaa89ea426c29b4c92daa0f4a0c3bba6fe3`。

| 标识 | 路径 | 已读对象/范围 |
|---|---|---|
| S01 | `dev_log/P11/README.md` | 交付、失败与修复、范围、P12 交接 |
| S02 | `dev_log/P11/receipt.json` | 实现绑定与构建片段 |
| S03 | `dev_log/P11/logs/ctest.log` | 最后测试和 213/213 汇总 |
| S04 | `editor/activities/commands/src/CommandRegistry.cpp` | 全文；blob ac2ad4f135c23e64335d4032f3fe6b353f19e75d |
| S05 | `editor/activities/commands/src/CommandDispatcher.cpp` | 全文 |
| S06 | `editor/activities/commands/include/lux/engine/editor/commands/CommandRegistry.hpp` | 全文 |
| S07 | `editor/application/extensions/src/Contributions.cpp` | 全文；blob 57c73d563b94dca7c32f9d887ec08b6458dfc1ec |
| S08 | `editor/application/extensions/test/contributions.cpp` | 全文 |
| S09 | `editor/activities/persistence/src/SaveService.cpp` | 行 1–300，注册准备和 dispatch；blob 558bb0fe2b7ad5649c996b7442229e4300bb0568 |
| S10 | `editor/activities/sessions/src/SessionInstallation.cpp` | 全文 |
| S11 | `editor/activities/sessions/src/SessionFactory.cpp` | 全文 |
| S12 | `editor/tests/integration/session_factories/installation.cpp` | 行 1–480，真实材料角色、失败边界及三模型安装 |
| S13 | `editor/application/extensions/src/EditorExtension.cpp` | 全文 |
| S14 | `editor/application/extensions/src/BuiltinContributions.cpp` | 全文 |
| S15 | `editor/workbench/desktop/src/CommandMenu.cpp` | 全文 |
| S16 | `editor/editing/include/lux/engine/editor/contracts/CodeLease.hpp` | 全文 |

施工依据：本对话所附新版 `01_P11_IMPLEMENTATION.md`，尤其完成定义、5.3 复合发布、6.2 prepare/commit/abandon 以及调用期代码寿命。没有用更早的旧目录方案覆盖本版施工要求。
