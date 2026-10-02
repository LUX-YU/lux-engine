# P11 R1 复审结论与 P12 启动补充

**日期：2026-10-01。性质：源码与提交证据复审、下一阶段施工补充。**

## 0. 结论与授权范围

P11 R1 在本次定向源码、测试源码和部分归档证据审阅范围内通过。上轮两个阻塞点已经得到对应修正；未发现需要继续阻止进入 P12 的前置缺口。

**允许在 `codex/editor-redesign-v4` 上执行既定 P12，完成后停下复审；不自动进入 P13，不修改 main。** 这不是全仓无缺陷证明，也不是独立环境的完整引擎认证。

| 固定事实 | 值 |
|---|---|
| 本次核实的远端验收 HEAD | `0a6644d8781dd3aeea1da549a87dfd6303a1c1dc` |
| HEAD 的直接父提交／R1 实现 | `1afb8f3f6e584075c6a9204e4faa7c406e55e146` |
| 原 P11 验收 | `24896eaa89ea426c29b4c92daa0f4a0c3bba6fe3` |
| 分支 | `codex/editor-redesign-v4` |
| 本轮资格范围 | 原约定 Windows 范围；P11＋STRICT |

实施方开工时重新核对实际 HEAD 与祖先关系。有后继提交则记录差异，不 reset 到这里的 SHA，不覆盖用户工作。

## 1. 文档优先级与阅读方式

1. 用户最新决定及本次明确启动补充。
2. [既定 P12 详细施工方案](reference/closeout/02_P12_IMPLEMENTATION.md)与[残留移除计划](reference/closeout/03_REMOVAL_PLAN.md)。
3. [总执行约束](reference/closeout/00_MASTER.md)、[验收与交接](reference/closeout/04_ACCEPTANCE_AND_HANDOFF.md)。
4. 已验收五层实现、P11/P11-R1 当前公共契约与唯一施工账本。
5. 参考包中的旧 V4，只作历史语义对照，不恢复旧路径、旧账本或旧接口。

本补充更新的是前置提交、R1 必须继承的用法和 P12 出口，不替代既定 P12 功能矩阵，也不再增加一个架构整改阶段。`reference/closeout/` 是原收尾施工包逐文件原字节副本；里面的旧基准 SHA 不要求回退。

## 2. R1 的实际复审结论

### 2.1 拒绝输入的清理作用域已闭合

当前 `SaveService::prepareSource()` 保留外层 `Input incoming` 以管理准入前的输入。owner/dispatch 检查通过后先建立原 `DispatchScope`，再立即移动整个输入为内部 `Input owned`。

```cpp
Input incoming{std::move(code), std::move(source)};
// 现有 owner / dispatch 准入检查。
const Impl::DispatchScope dispatch{impl_->dispatching};
Input owned{std::move(incoming)};
```

取得准入后，重复注册、无效身份等提前返回先销毁内部 source，再释放其 code，最后退出 dispatch。成功路径仍移交 source 至原注册记录。准入前已有 BUSY 时不建立或解除另一层保护。

这项修正没有删除身份／重复检查，没有增加第二 gate，也没有重新修改 `completeEncoding()` 算法。

### 2.2 已接受完成没有被防重入一并封死

真实 Material／SaveExecution 回归先让 worker 完成，但尚未收集 owner 结果；然后在被拒绝角色的析构清理内首次 collect。测试在 collect 返回后检查：

- 编码执行及完成接收一次，原操作从 ENCODING 进入 READY；
- `canPrepareSource()` 和 `requestSave()` 仍为 BUSY；
- 当前作者完整编码、内容／观察／绑定／dirty、完整历史状态均未改变；
- 退出清理后正常发布、采用、确认和后续保存仍成立；Undo/Redo 继续正确影响 clean 状态。

修复前 completion 日志为 `prepare_BUSY=0 request_BUSY=0`，修复后均为 1；`completion_received=1` 前后都为真。修正是在保留完成接收的同时恢复新业务限制，不是通过拒收完成来避开重入。

日志里的 `outer_preserved` 只在独立的准入前 BUSY 场景采样。其他场景默认值为 0，不能据此断言外层保护失效；completion 的实际保护由 collect 之后的两个 BUSY 观察证明。

### 2.3 命令 owner 现在参与复合批次保护

`CommandRegistry` 新增自己的窄 `Batch`，不依赖 application 或 ContributionRegistry。

| 入口／操作 | 当前职责 |
|---|---|
| `readBatch()` | 获得只读批次的发布保护，不预留或消耗命令 revision。 |
| `preparePublication(snapshot)` | 检查原 owner、活动状态和 revision 容量；持有候选和一次提交权限。 |
| `Batch::commit()` | 一次无分配、无任意回调交换，返回旧 snapshot；scope 仍持续存在。 |
| `Batch` 析构 | 先清理未提交候选，再解除 owner 的批次保护。 |
| 普通 `publish`／另一个 batch／dispatcher drain | 原批次存在时准确拒绝。 |
| 固定句柄 query/execute、只读 snapshot、后批 enqueue | 保留原来允许的使用方式。 |

Batch 不可复制、不可移动赋值，可移动构造；移动后只有新 owner 负责解除保护。它不是一个第二注册服务，也不保存一串历史目录。

### 2.4 贡献准备、失败、清理和通知覆盖同一边界

`ContributionRegistry::applyPending()` 在自身 scope 内、取出候选和执行反射回调前，取得命令 owner 的 publication Batch。失败时候选清理仍在两个 scope 内；成功时调用一次授权 commit，再交换贡献目录；旧值清理和 changed 通知也仍受保护。

`withSnapshot()` 同样在调用 factory/read callback 前取得 `readBatch()`，固定原贡献快照。

原先“检查一次 canPublish → 跨越外部回调 → 普通 publish”的生产路径已经替换。没有用回调后检查来假装回滚已发生的目录修改。

### 2.5 测试验证的是实际事实一致性

读取到的真实 SDK 贡献回归包含：

- factory 中直接调用参与者 `commands.publish()` 被拒，独立 registry 和固定句柄查询仍然可用；
- 反射回调尝试直接发布后，故意令配置校验失败，实际命令／贡献仍为 A，临时 `R11DraftOnly` 不进入正式反射目录；
- 旧值析构与 DIRECT changed 通知中直接发布被拒，新候选 D 能入队但只在下一外层批次采用；
- 通知不只比较 revision，也比较两个目录中的实际共享条目；
- 放弃的命令候选在 Batch 解除保护前清理；批次结束后普通合法发布恢复。

修复前反射场景 `direct_calls=1 blocked=0 consistent=0`；修复后 `direct_calls=1 blocked=1 consistent=1`。notify 两次调用均被阻止且目录一致。

## 3. 证据与独立审阅边界

本次阅读了远端提交链、实现差异、全部关键生产改动、相关真实测试源码、文件范围、行为映射，以及保存／贡献的部分修复前后 SDK 日志和完整 CTest 汇总。

归档包含 Windows `222/222`。实施报告记载 CPU `196/196`、PLAYER `12/12`、24 组新前缀 SDK、真实 V7 DLL、双视口 GPU、原生输入与归档负例通过。不同组与其内部测试数量不混合相加。

**本次没有独立执行完整引擎构建、CTest、SDK、DLL 卸载、GPU、归档验证器或所有远端文件哈希复算，也没有新增独立引擎运行成绩。** 以下源码推理与观察到的已提交运行记录保持区分。

14 个修改文件均集中于原保存／命令／贡献模块及原测试、说明和消费者登记。没有新增生产文件、目录、target 或平行框架。V7 导出表未改，Editor SDK 指纹已按改动重新生成；运行 ABI 的独立性继续继承。

## 4. P12 使用 P11 R1 时的具体纪律

### 4.1 Batch 是短同步作用域，不是跨帧操作令牌

原 registry 必须活得比 Batch 长，Batch 的使用和结束遵守同一个 owner 线程。`commit()` 只能由持有未消费 publication 权限的一方调用一次；readBatch 不能拿来提交。

**不得把 Batch 保存在异步 Task、跨帧 workflow、插件对象或退出后才清理的永久记录里。** 等待文件、GPU、用户决定或下一帧时，持有的是不可变 snapshot／拥有型准备结果和 code owner，不是长期锁住命令发布的 scope。

进入 owner 的同步 factory／prepare／commit 段时，再取得所需 scope，重新核对仍可能变化的身份。整批来源一致由固定不可变快照保持；短 scope 保护执行段，不负责把整个异步过程变成不可打断事务。

不要从 `withSnapshot()` 回调里直接通过普通 CommandRegistry.publish 强制同步更新。新更新排到既有后批，失败候选保持原目录；不要设计 unlock/relock 旁路。

### 4.2 完成事实接收与新业务准入仍分开

退出／关闭／扩展更新中，新的 Save/Open/Compile/命令可以被拒绝，但已经接纳的完成必须进入原 owner 结算。不能因为 application 正在 closing、某个 View 消失或批次存在，把原结果直接丢掉。

角色撤销阻止未来调用，不抹掉文件发布事实，也不意味着工作线程／GPU 已退休。source、任务、结果、注册控制块和 code pin 仍按真实借用图释放。

### 4.3 不把现有单项 API 当成完整产品事务

P11 工厂和 `InstalledSession` 提供的是实际安装与有限角色能力，不是 SaveAll、整组关闭或退出的全部政策。单项 close/adopt 通过，不证明多项最终提交不会半完成。

P12 必须按原文检查 Store 的有界会话枚举、全部关闭许可准备、原 Host/Root 的布局与批量挂载准备。确实缺少的能力只补到现有 owner；不增加通用 TransactionManager、SessionManager 或第二 ViewHost。

### 4.4 不把测试中允许的清理扩展成任意插件沙箱

本轮证明约定入口和真实测试场景下的保护。它不证明任意扩展都可在任意线程销毁 registry，或任意代码可违反生命周期契约后继续安全。继续限制能力注入、owner 线程和实际销毁顺序，不扩大热卸载承诺。

## 5. P12 开工核验：不是再次重做 P11

P12-A 先核对：

1. 当前实施 SHA 继承本次 P11 R1 验收；工作区与原用户补丁独立保全。
2. P11 的 `command-map.json`、`source-map.json`、`removal-plan.json` 与当前实际消费者对应，已迁项不重建。
3. 实际使用的新 V7 SDK 来自当前安装前缀；不要用 P11 原旧指纹或旧开发 DLL 补依赖。
4. 实际产品仍通过旧 app/context 构建的部分，在功能矩阵中逐项指定替代入口及删除批次。
5. Task/Session/History/Run/preview/source/publication 的原唯一 owner 保持。

不要求为这个起点重新执行无关微基准，也不重新设计命令／工厂／五层结构。

## 6. P12 的内部施工顺序保持 A–H

| 批次 | 实施工作 | 必须观察到的结果 |
|---|---|---|
| A | 冻结所有适用用户功能与每项最后消费者 | 每项有旧入口、新正式入口、唯一 owner、行为测试、删除落点。 |
| B | Open/Reload/SaveAll/CloseSessions | 内容去重、版本复查、许可与真实结果完整；不依赖 Pane。 |
| C | 最后视图、布局准备／提交、Recovery | factory/坏 dock 失败不改变原 UI；布局与打开内容分开。 |
| D | 三工具、运行 Inspector、项目／任务／设置、插入与发布 | 原用户功能有正式 UI／命令入口，不仅是模型能被测试调用。 |
| E | EditorApplication、启动失败、唯一驱动与退出 | 完整成功或准确构造失败；继续泵送直到已准入工作与资源结清。 |
| F | 实际 lux_editor、launcher、内置 V7 与安装切换 | 用户运行的唯一产品使用新体系，无 old/new 回落。 |
| G | 逐项删除旧体系 | 九个旧根、正式目录内旧协议、target／安装／生成残留均清零。 |
| H | 最终固定 SHA 资格与封存 | 真实安装产品、原行为、SDK、GPU／输入、依赖与删除证明一致。 |

批次是同一 P12 内部顺序，不是八个新阶段。不每移动一个文件就重跑全部长期测试；按影响范围回归，最后一次完成最终矩阵。

## 7. 完整用户流程不得缩水

### Open 与显示

E2 固定项目／资产／会话种类和不可变工厂，按工作副本政策去重；E4 负责显示组合。第二视图不重新读入第二个作者副本。内容发布后显示失败，保留可访问的无视图会话并报告部分完成；不悄悄回滚已经对外发布的内容。

### Reload 与保存

重载保留 SessionId，采用新的 History 域；Save As 保留原 History。这两者不得混用。等旧写入和 Unknown 的真实责任结清；IO 后、最终采用前重验 stamp、binding 和新增旧来源写入。不得用长 READING 锁住整个磁盘等待。

SaveAll 按会话而非视图固定集合；两窗同源只保存一次，无视图 dirty 会话也包含。请求获准、文件发布、保存基线采用分别报告。

### Close 与退出

先收齐带内容戳的决定，再取得全部可提交许可。A 保存成功后 B 取消，A 的磁盘保存保留，但所有原内容／视图不能已经被提前删除。Discard(S10) 不能授权销毁 S12。关闭视图不自动停止独立 Run。

实际退出继续接收编码／IO／GPU／任务完成；不能先停消息泵再等它才能完成的工作。注册撤销、逻辑关闭、对象清理、DLL 和执行器释放按实际依赖顺序，不将所有状态压成 closing bool。

### 布局与恢复

ApplyLayout 使用完整纯计划和一次准备／提交；第 N 个 factory 失败时原可见性、数量、绑定和 dirty 内容保持。偏好写入或通知失败是独立结果，不抹掉已提交结构。

Recovery 只从独立清单恢复；继承 P09 R1 的 selected 作用域，不将所有旧布局 locator 再次并集，也不自动覆盖现有用户 recovery／marker。

### 实际工具功能

运行 Inspector 及其原支持范围在 P12 正式接入，不能因 P10 只验证作者 Inspector 而永久漏掉。模型读取完成后还要在原目标／版本有效时完成实际插入。编译和派生发布不替代作者保存，原 source／pak 组合用户入口若存在，要明确迁移语义而不是直接删掉。

## 8. 删除是 P12 本体，不是 P13 的后续任务

最终 tracked 的 `editor` 一级目录只保留：

```text
editing/
authoring/
activities/
workbench/
application/
tests/
```

必须删除原九根：`app, assets, context, launcher, metadata, plugins, tools, transition, ui`。先迁真实消费者，再删原声明、定义、调用、friends/TestAccess、CMake source/target、生成输入、导出／安装、示例和运行接线。

九根消失还不够；同时清理正式目录内只服务旧产品的 `editor_editing` 旧协议、TAssetSave/LegacyPersistence 等。不得搬进 `application/legacy` 或 `tests/legacy` 再编译旧体系，不保留空 INTERFACE/alias、转发头、EXCLUDE_FROM_ALL 或可运行旧产品选项。

### 明确保护的正式能力

| 正式能力 | 删除时的保护 |
|---|---|
| `activities/project` 的 `editor_assets` / AssetImporter | 旧根 assets 退出不等于删除正式导入。 |
| `editor_storage` / ProjectStorage | 有真实项目 IO／目录职责，不按旧包名误删。 |
| `activities/scene` 的 `editor_editing_scene` | 是 Run 使用的真实运行编辑，不是待删作者桥。 |
| `application/launch` 的 `editor_launch` | 复用实际启动算法，不重做平台启动。 |
| 纯 ProjectBuilder／ProjectBuildConfig | 仍归 authoring，不把纯配置构造改成另一个执行器。 |
| History／Store／Runtime／codec／纯分区解码 | 唯一实现与已有正确语义继续保留。 |
| LegacyWorkspaceImporter／版本化只读旧格式 | 数据兼容不是旧代码框架兼容。 |
| 原历史快照、负例与用户数据 | 不作全仓替换或递归清除。 |

安装首次使用新空前缀，证明没有旧开发 DLL 或旧头补依赖；开发前缀的废弃文件只按受控安装清单删除。最后实际启动已安装 `lux_editor`，不能用只链接新库的测试 exe 代替产品切换证明。

## 9. 验收政策与历史缺陷

沿用原 X12-01～10、P11/R1 与此前适用行为。原 CTest 名称有语义迁移记录即可；不得为保持数量，在新测试目录重新构建一套旧 Editor。

| 项目 | 本次交接政策 |
|---|---|
| C03 | 当前已修，P12 继续保留相同回调代码寿命测试；历史 FAIL 不变。 |
| C01 | P12 实际坏布局拒绝且原 visibility/count/dock 不变；不是仅 codec 拒绝。 |
| C04 | P12 实际菜单连接失败导致 create 失败；不改成退出测试。 |
| Windows | 最终实现上的全量构建、二次无工作、适用功能／CPU／PLAYER／SDK。 |
| GPU／输入 | 正式新双视口、资源退休、原生输入，并运行受迁移影响的既有行为。 |
| 插件／SDK | 当前 V7、真实 DLL 及卸载/结果寿命，运行 ABI 独立，安装头及依赖负例。 |
| Linux／IME／sanitizer | 没有运行就继续 NOT_RUN，不新增环境建设门槛。 |
| 旧慢算法计时 | 历史 PARTIAL 保留，不补样本。 |
| P13 | 不承接旧 Context／Editor／旧桥未删除，或原功能未接入。 |

本轮放行不把本来没有运行的环境变成通过，也不更改历史报告的原始结论。

## 10. 工作区与提交纪律

报告指定的新代码查看位置为 `E:/SyncForder/CodeRepos/lux-engine-p11`。原 `lux-engine` 工作区仍停留在保全旧提交，不能以其目录树判断最新结构。

P12 开工记录实际 working directory、branch、HEAD、是否干净、新旧工作树关系及 ProjectBuilder 用户补丁位置。该用户补丁目前没有应用到新检出、没有进入验收；不得擅自重置、删除或把它混入本阶段实现。

唯一可变账本继续 `.internal/editor-redesign/`。P12 阶段末冻结 `dev_log/P12/`；旧 P11/P11-R1 以及更早快照按各自实现 SHA 保留，不覆盖它们。

正常提交／推送，implementation commit 与 evidence commit 分开。收据绑定实际测试的实现，不循环包含自己的 SHA。最终交接说明用户应查看哪个新工作区及补丁是否已应用。

## 11. 可直接交给实施方的启动指令

> P11 R1 在本次源码与提交证据复审范围内通过，允许进入 P12，仅执行 P12。
>
> 以当前验收 `0a6644d8781dd3aeea1da549a87dfd6303a1c1dc` 为已验收前置，核对真实后继、工作区与用户差异，不 reset、不修改 main。
>
> 读取本补充、既定 P12 施工、移除计划及 P11 最后消费者映射，按 A–H 完成内容用例、工作台组合、正式产品切换及旧体系删除。
>
> 继承 R1：拒绝输入先在原保护内清理；已接受完成不因新业务 BUSY 而丢失；贡献批次与命令目录由参与 owner 的短同步 scope 共同保护。跨帧只保留不可变快照和拥有型结果，不把 Batch 当长期任务令牌。
>
> 不重做五层、Process、Runtime、History 或插件加载，不新增通用管理器。内容活动不反向依赖 UI，实际装配只在 application。批量关闭与布局需要的窄准备能力补到原 Store/Host。
>
> 切换用户实际安装运行的唯一 lux_editor；九个旧根及正式目录内旧协议、桥、导出、生成和安装残留在 P12 同阶段删除，不留永久 legacy 或 old/new 回落，不延期 P13。
>
> 保留正式导入、运行编辑、纯 ProjectBuilder、codec、数据只读迁移和用户补丁。C01/C04 在正式新产品关闭，C03 保持；Linux/IME 等未测如实记录，不补旧性能长测。
>
> 最终显式 P12＋STRICT，实际 Windows／SDK／DLL／IO／双视口／输入／依赖／生成回归及删除证明完整后，分别提交实现与验收，正常推送并停在 P12 等待复审，不自动进入 P13。

## 12. 本次读取的主要来源

以下是固定提交的来源，不是新执行成绩。

- [远端 HEAD 的提交记录](https://github.com/LUX-YU/lux-engine/commit/0a6644d8781dd3aeea1da549a87dfd6303a1c1dc)
- [R1 实现差异](https://github.com/LUX-YU/lux-engine/commit/1afb8f3f6e584075c6a9204e4faa7c406e55e146)
- [R1 验收报告](https://github.com/LUX-YU/lux-engine/blob/0a6644d8781dd3aeea1da549a87dfd6303a1c1dc/dev_log/P11-R1/README.md)
- [SaveService.cpp](https://github.com/LUX-YU/lux-engine/blob/0a6644d8781dd3aeea1da549a87dfd6303a1c1dc/editor/activities/persistence/src/SaveService.cpp)
- [CommandRegistry.cpp](https://github.com/LUX-YU/lux-engine/blob/0a6644d8781dd3aeea1da549a87dfd6303a1c1dc/editor/activities/commands/src/CommandRegistry.cpp)
- [Contributions.cpp](https://github.com/LUX-YU/lux-engine/blob/0a6644d8781dd3aeea1da549a87dfd6303a1c1dc/editor/application/extensions/src/Contributions.cpp)
- [保存模型回归](https://github.com/LUX-YU/lux-engine/blob/0a6644d8781dd3aeea1da549a87dfd6303a1c1dc/editor/tests/persistence/models.cpp)
- [贡献回归](https://github.com/LUX-YU/lux-engine/blob/0a6644d8781dd3aeea1da549a87dfd6303a1c1dc/editor/application/extensions/test/contributions.cpp)
- [保存清理修复前日志](https://github.com/LUX-YU/lux-engine/blob/0a6644d8781dd3aeea1da549a87dfd6303a1c1dc/dev_log/P11-R1/before/input-completion.log)
- [贡献反射修复前日志](https://github.com/LUX-YU/lux-engine/blob/0a6644d8781dd3aeea1da549a87dfd6303a1c1dc/dev_log/P11-R1/before/batch-reflection.log)
- [安装保存消费者细节](https://github.com/LUX-YU/lux-engine/blob/0a6644d8781dd3aeea1da549a87dfd6303a1c1dc/dev_log/P11-R1/evidence/sdk/persistence-ctest-details.log)
- [安装贡献／工厂消费者细节](https://github.com/LUX-YU/lux-engine/blob/0a6644d8781dd3aeea1da549a87dfd6303a1c1dc/dev_log/P11-R1/evidence/sdk/p11-models-ctest-details.log)
- [最终 CTest 汇总](https://github.com/LUX-YU/lux-engine/blob/0a6644d8781dd3aeea1da549a87dfd6303a1c1dc/dev_log/P11-R1/logs/ctest.log)
- [R11 行为映射](https://github.com/LUX-YU/lux-engine/blob/0a6644d8781dd3aeea1da549a87dfd6303a1c1dc/dev_log/P11-R1/behavior-map.json)
- [修改范围](https://github.com/LUX-YU/lux-engine/blob/0a6644d8781dd3aeea1da549a87dfd6303a1c1dc/dev_log/P11-R1/files.json)
