# LUX Engine Editor V3：问题—原则—类型—子系统的完整重设计指导

**文档日期：2026-09-28**  
**固定源码基准：`2bb33ff1a1f11025cf404e074c0e9b259239d8a4`（2026-09-27 20:00:13 UTC）。** 本次通过 GitHub 分支接口确认 `main` 仍指向该提交。[S00]  
**性质：目标架构与实施指导，不是已经完成的重构、可编译 SDK 或运行验收报告。**

> 本文的目的不是保住现有 SceneEditor、EditorContext、PaneManager 的形状，而是从编辑器真实行为推导新的模型。旧代码可以复用，旧职责边界没有优先权。
>
> 每项重要设计都回答五个问题：**它解决哪个现有问题；维护哪个不变量；由哪些类型表达；这些类型属于哪个模块；用什么观察结果验收。**

## 阅读路径

- [第 1—2 章](#issues)：证据范围、现有 26 项问题及其根因。
- [第 3—5 章](#principles)：12 条设计原则、基础契约与所有权模型。
- [第 6—12 章](#session-types)：核心类型、组合/继承、编辑、保存、运行、视图和扩展的详细设计。
- [第 13—15 章](#modules)：16 个子系统、具体 target 依赖、目录与旧代码去向。
- [第 16—18 章](#workflows)：跨模块完整流程、线程/失败契约、52 项验收测试。
- [第 19—21 章](#implementation)：分阶段实施、反例与[最终追踪矩阵](#traceability)。
- [附录](#type-index)：类型归属索引、术语修订、源码与技术参考。

文中 `A/T/C/B` 编号沿用原调查；`P` 是设计原则；`M` 是子系统；`Q` 是验收场景。它们不是四份互不关联的清单，文末矩阵将每项问题完整串接起来。

<a id="scope"></a>
## 1. 边界、证据与本轮已经确定的选择

### 1.1 哪些是已知事实，哪些是设计决定

原调查中的 26 项作为固定版本的调查记录继承；本轮重新核对了分支头、SceneEditor 内部声明、EditorContext、Pane、SceneRuntime、CommandRegistration、WorkspaceRequest，以及 EditHistory/EditOperation/EditTypes。对其余问题采用原调查已给出的文件与符号证据，不伪称本轮重读了仓库全部实现。定位使用固定提交链接和符号，不编造精确行号。[S00] [S08] [S13] [S14] [S19] [S22] [S24] [S34] [S35] [S36]

**F** 表示声明或控制流事实；**R** 表示有特定触发条件的风险；**D** 表示架构判断。本文没有执行引擎构建、CTest、桌面交互或 GPU 故障复现。因此“必须通过”是实施验收要求，不是声称已通过。

后文新类、目录和协议全部是提案，不能当成仓库已有 API。C++ 片段用于规定签名方向与所有权；省略了实现和部分辅助声明，不是可直接编译的补丁。沿用现有 `lux::cxx::expected` 与 C++20 能力，不把升级 C++ 标准或替换任务系统作为前置条件。

### 1.2 对前两版的两项重要修正

**第一，不再把旧架构上的安全迁移方法当作最终设计。** 例如旧窗口内部先提取 Session 可以短期发生，但最终 Session 必须能独立于窗口存在，业务层不得继续通过旧 Context 获取全系统。

**第二，不把项目已经做对的事情重新包装成新发明。** 现有 `EditTypes.hpp` 已有 `HistoryId`、`StateId`、`Revision` 和 `event_sequence`；`EditOperation/PreparedEdit` 已有准备、提交、发布分界；`EditHistory` 已有不可重入和保存票据规则。新方案必须复用或明确迁移这些机制，不能声称它们完全不存在。[S34] [S35] [S36]

同样，已有 UI 安全点、代码生命周期保活、SceneRuntime 的维护/仿真/发布分界和工作区非破坏性恢复是重要基础。需要改变的是责任分配与接口边界，而不是删除它们所保护的不变量。[S13] [S22] [S24] [S25] [S32]

### 1.3 本文选择的目标，而不是留给实施者猜的选项

编辑内容由 `SessionStore` 拥有；视图由 `ViewHost` 拥有；运行由 `RunStore` 拥有。三者独立。Scene 与 Material 具有各自具体模型；只在异构会话管理、异步编码及真正的扩展边界使用窄接口。共同能力通过组合或角色适配器复用，不建立万能文档继承树。

正常保存允许用户继续编辑；同一物理目标的发布被明确排序。第一版 Save As 在捕获到提交/采用这一窗口内冻结该会话的编辑，而不是悄悄处理身份重映射与并发修改。普通 Save 不受此限制。运行默认从确定快照开始，停止不隐式回写作者内容。

应用布局默认非破坏性：不覆盖已有未保存内容、不关闭额外视图、不用布局文件重新创建作者事实。缺失扩展状态以有版本的 opaque payload 保留。跨工具链稳定二进制 ABI、任意插件热卸载、通用工作流引擎均不是默认交付目标。

这些是**本设计的明确选择**，不是声称它们是 C++ 唯一正确做法。调整它们需要单独决策记录，并连带修改类型、流程与测试，不能只改一处实现。

<a id="issues"></a>
## 2. 现有问题：从局部症状追到错误的概念边界

### 2.1 根因分组

| 根因 | 典型现象 | 对应问题 | 目标上的消除方式 |
|---|---|---|---|
| 用窗口生命周期代表内容生命周期 | 打开内容、保存、运行、退出都挂在 Pane | A02、A04、A08、T03 | Session、View、Run 三种 owner 独立 |
| 用通用上下文代替依赖设计 | 每个功能都可抵达 Engine/窗口/注册表 | A01、A03、A05 | 窄依赖；装配权与使用权分开 |
| 用字段组合代替过程契约 | pending/result/action/bool 同时表示阶段 | T01—T09 | 区分值、身份、借用、操作与完成；阶段拥有相应资源 |
| 将准备、提交、观察到结果合并 | 部分修改、假完成、缓存丢重试 | C01—C05 | 每个模块明确自己的提交点与失败保证 |
| 目录标签没有成为编译约束 | 大 UI target、全局 SDK 指纹、特定环境测试 | A06、A07、B01—B04 | target DAG、公开头边界、分层测试、独立安装验证 |

### 2.2 逐项问题与替代方向

以下每项先说明旧问题，再给目标设计；详细类型在后续章节展开。不能把“新增一个类名”当作解决完成，必须满足末尾的验收场景。

<a id="a01"></a>
### A01 — 应用入口拥有多个流程的中间状态

**证据等级：F/D。定位：** `EditorImpl.hpp；Editor::Impl::handleRequests()`。[S01] [S02]

**问题与成因。** 关闭、菜单、Save All、工作区读写、启动与资产打开的中间状态保存在同一个 Impl。分文件没有限制它们互相修改字段；推进顺序仍由应用根理解各业务内部条件来维持。

**目标解决方法。** 把应用根改成装配与阶段驱动者。OpenAssetOperation、CloseSessionsOperation、SaveAllOperation 各自拥有状态；用例完成值交给下一用例，不传 SharedEditorState&。不再在 Editor 中保存这些操作的每一个阶段字段。

**落地关系。** 原则：P01、P02、P04；替代类型：`OpenAssetOperation / CloseSessionsOperation / SaveAllOperation / EditorApplication`；模块：M15、M16；验收：Q19、Q40、Q42。

<a id="a02"></a>
### A02 — 内容、视图和运行实例被绑定到同一个窗口对象

**证据等级：F/D。定位：** `SceneEditor.hpp；SceneEditorImpl.hpp 的 scene/run_scene/history/save 与多个 UI friend`。[S18] [S19]

**问题与成因。** SceneEditor 既继承 Pane 和历史目标，又管理编辑内容、运行副本、资产切换、保存、选择和高亮。多个 UI 类型可以访问整个 Impl。这是概念合并，而非仅仅实现太长。

**目标解决方法。** SceneSession 独立存在于 SessionStore；SceneView 只是 Pane 的视觉特化；RunSession 由 RunStore 独立持有。视图只持有代际可验证的会话键。删除窗口对内容和运行生命周期的默认支配权。

**落地关系。** 原则：P01、P02、P03；替代类型：`SceneSession / SceneView / RunSession / SessionStore`；模块：M03、M04、M08、M11；验收：Q01、Q02、Q03、Q22。

<a id="a03"></a>
### A03 — Context 隐藏真实依赖并泄漏管理权限

**证据等级：F/D。定位：** `EditorContext::engine/execution/panes/setCommands/openAsset；PaneManager::setFrozen`。[S08] [S10]

**问题与成因。** 普通工具可沿 EditorContext 取得完整引擎、窗口 owner 与注册替换入口。openAsset 的返回类型直接是 Pane 工厂结果。依赖和能力同时被合并。

**目标解决方法。** 删除业务层的 EditorContext 参数。编译、保存、视图分别注入所需能力；SessionStore 的采用、删除仅向用例层开放，注册发布仅向扩展装配开放。打开内容与显示视图在应用用例中组合。

**落地关系。** 原则：P02、P05、P11；替代类型：`SceneSessionAccess / SaveService / ViewRequests / ExtensionPublisher`；模块：M03、M06、M11、M14、M15、M16；验收：Q01、Q45、Q51。

<a id="a04"></a>
### A04 — 窗口工厂混合构造、采用、复用和显示

**证据等级：F/R/D。定位：** `PaneRegistration::create；PaneManager::create/adopt/show；Pane 构造`。[S10] [S11] [S12] [S24]

**问题与成因。** 工厂接受整个 Manager 并返回已存在对象的引用，可能已自行注册或复用窗口。现有 Pane 构造关联 Root，因此改成 unique_ptr 返回值也不等于消除了副作用。

**目标解决方法。** 目标 UI 支持脱离活动树的构造。工厂产生 DetachedView，ViewHost 在安全阶段接管；复用策略属于 OpenAssetOperation，显示和聚焦是单独动作。新增 attach 失败契约，不假定 RAII 能撤销任意插件副作用。

**落地关系。** 原则：P01、P04、P05；替代类型：`DetachedView / ViewFactory / ViewHost / OpenAssetOperation`；模块：M11、M14、M15；验收：Q34、Q51。

<a id="a05"></a>
### A05 — 借用跨越回调边界，const 也没有表达真正只读

**证据等级：F/R/D。定位：** `PaneManager::panes/registrations；Context::commands；restoreWorkspace 中的 entries`。[S05] [S08] [S10]

**问题与成因。** span&lt;const unique_ptr&lt;Pane&gt;> 没有令 Pane 本身只读，还暴露存储表示；注册表 span 跨插件回调迭代时，若回调替换容器，后续迭代会失效。现有运行时借用文档已有期限，不能声称整个项目没有借用契约。

**目标解决方法。** 长期保留身份或不可变快照，短期同步借用必须在明确 owner 阶段结束。注册调用固定一个只读 CommandRegistrySnapshot，发布替换推迟到批次结束。普通观察者读取 ViewInfo/SessionInfo，不得到 owning 容器。

**落地关系。** 原则：P03、P05、P06；替代类型：`SessionKey<T> / SessionInfo / ViewInfo / CommandRegistrySnapshot / ExtensionSnapshot`；模块：M03、M11、M14；验收：Q06、Q38、Q39。

<a id="a06"></a>
### A06 — 通用 UI target 聚集领域与平台依赖

**证据等级：F/D。定位：** `editor/ui/CMakeLists.txt`。[S27]

**问题与成因。** 共用控件、场景交互、项目访问、插件适配、窗口输出和渲染同步在同一个 target 内。复用简单控件也容易带入宽依赖闭包。

**目标解决方法。** 拆出纯 View API/基础控件、desktop_shell、scene_ui、material_ui、scene_projection、material_preview。目录与 target 都表达边界，不以一个新的 editor_common 汇总所有实现。

**落地关系。** 原则：P01、P05、P11；替代类型：`ViewHost / SceneView / MaterialView / SceneProjection / MaterialPreviewStore`；模块：M09、M10、M11、M16；验收：Q01、Q45、Q46。

<a id="a07"></a>
### A07 — metadata 同时表示描述和可执行扩展

**证据等级：F/D。定位：** `metadata/PaneRegistration.hpp；CommandRegistration.hpp`。[S12] [S13]

**问题与成因。** 记录中不仅有名称，还有工厂、恢复函数、调用闭包和代码保活句柄。问题是概念命名与依赖范围，不是类型擦除天然错误，也不是已经证明运行层反向依赖 Editor。

**目标解决方法。** 纯描述与执行入口分开命名和分发；typed extension contracts 归 extensions，反射/模式描述归原领域。描述和入口仍可由同一发布快照关联，不能复制出两套互相同步的事实。

**落地关系。** 原则：P01、P05、P10；替代类型：`CommandDescriptor / CommandEntry / ViewFactoryRegistration / CodeLease`；模块：M13、M14；验收：Q38、Q46、Q48。

<a id="a08"></a>
### A08 — Scene 与 Material 重复维护同类内容切换生命周期

**证据等级：F/D。定位：** `SceneAssets.cpp；MaterialAssets.cpp；AssetEditing.hpp`。[S16] [S20] [S21]

**问题与成因。** 未保存审阅、Save/Discard/Cancel、读取、候选、采用和失败恢复由各工具重复维护。领域差异真实存在，但打开新内容为何需要“替换某个窗口内部模型”，主要来自旧所有权结构。

**目标解决方法。** 打开资产首先产生或复用会话；在当前标签显示另一内容只是重新绑定视图。关闭旧会话是独立策略。真正重载同一会话才使用 ReloadSessionOperation。共同的是用例协议，不是万能 AssetEditor 基类。

**落地关系。** 原则：P01、P02、P04；替代类型：`OpenAssetOperation / ReloadSessionOperation / SessionFactory / SceneSession / MaterialSession`；模块：M03、M04、M05、M07、M15；验收：Q05、Q15、Q51。

<a id="t01"></a>
### T01 — WorkspaceRequest 是请求、回复和内部阶段的混合袋

**证据等级：F/D。定位：** `WorkspaceRequest；EWorkspaceAction；workspaceRequest/applyWorkspaceResult`。[S05] [S14]

**问题与成因。** action/name/new_name 与 layouts/revision/message/pending/result 同时存在；字段有效性随阶段改变。STATUS 还参与内部持久化阶段，接口很难说明到底是请求还是完成值。

**目标解决方法。** 按真实概念拆分 SaveLayout、RenameLayout、ApplyLayout 等命令，LayoutCatalog 是查询结果，LayoutApplyReport 是完成结果，操作内部阶段私有。不是给每个旧字段加 wrapper。

**落地关系。** 原则：P01、P03、P04；替代类型：`SaveLayout / RenameLayout / LayoutCatalog / LayoutApplyReport`；模块：M12、M15；验收：Q31、Q35、Q36。

<a id="t02"></a>
### T02 — WorkspaceData 合并布局、目录和偏好

**证据等级：F/D。定位：** `EditorWorkspace.hpp；EditorWorkspaceStorage.cpp`。[S06] [S07]

**问题与成因。** 磁盘布局值夹带布局列表、当前选择等目录服务信息，使本不相关的读写被绑定到一次“大工作区操作”。

**目标解决方法。** DockLayout 仅表达布局；LayoutCatalog 仅表达目录；RecoveryManifest 描述内容重开；UserPreferences 保存偏好。布局使用稳定 LayoutId 作为文件身份，名称只是标签。

**落地关系。** 原则：P01、P04、P07；替代类型：`DockLayout / LayoutId / LayoutCatalog / RecoveryManifest / UserPreferences`；模块：M12、M15；验收：Q04、Q35、Q37。

<a id="t03"></a>
### T03 — 关闭结果与取消/释放准备状态混在一起

**证据等级：F/D。定位：** `CloseRequest/CloseDecision；beginExitReview/applyCloseDecisions`。[S02] [S15]

**问题与成因。** 枚举与可选错误可以形成矛盾组合；成功隐藏后的状态清理也复用 CANCEL。一个退出协议还处理窗口隐藏和销毁，业务取消与资源释放没有独立含义。

**目标解决方法。** 分开 HideView、CloseView、CloseSession、ExitApplication。内容审阅返回 UserCloseChoice；全部条件满足后取得 ClosePermit，由 Store 提交删除。permit 的释放不是“用户取消”；对外结果用互斥的完成类型。

**落地关系。** 原则：P01、P02、P04、P07；替代类型：`CloseViewOperation / CloseSessionsOperation / ClosePermit / CloseReport`；模块：M03、M11、M15；验收：Q19、Q20、Q21。

<a id="t04"></a>
### T04 — 已接收被写成已执行完成

**证据等级：F/D。定位：** `receiveMenu/applicationCommand；工作区事件入口`。[S02] [S04] [S05]

**问题与成因。** 排队命令和异步工作区请求映射为 EXECUTED；默认成功结果无法说明工作只是被接受还是副作用已经完成。自动化调用尤其容易误解。

**目标解决方法。** 同步操作返回 ImmediateCompletion；真正异步工作返回 Accepted{operation_id}，终态通过对应操作结果取得。查询、准入、完成为不同接口；不要求把所有普通函数消息化。

**落地关系。** 原则：P03、P04；替代类型：`CommandState / DispatchReceipt / OperationRef / SaveOutcome`；模块：M06、M13、M15；验收：Q12、Q17、Q40。

<a id="t05"></a>
### T05 — valid/invalid 和 safe 没有表达实际动作或时效

**证据等级：F/D。定位：** `SceneRuntime::valid/invalid/tick；SceneEditor::Impl::safe`。[S19] [S22] [S23]

**问题与成因。** valid/invalid 控制时间推进，不是对象是否存在；safe 只是某次 Registry 借用成功，并不保证跨 tick、等待或结构变更后仍安全。

**目标解决方法。** 运行接口分开生命状态、仿真模式、维护和发布；pause/resume/step 返回实际控制结果。读取直接取得有期限的 Borrow，不以先 safe 再取数据代替有效借用。

**落地关系。** 原则：P01、P03、P06；替代类型：`RunSession / RunExecutionState / StepTicket / SceneInstanceLease`；模块：M08、M09；验收：Q23、Q24。

<a id="t06"></a>
### T06 — 合法状态依赖跨字段组合与魔法哨兵

**证据等级：F/D。定位：** `两个 Impl；候选字段；save.index/placement.index；commands_revision==0`。[S01] [S19] [S20] [S21]

**问题与成因。** 候选内容及其阶段分散，状态枚举和若干 bool/optional 同时表达一件事。variant 下标、零 revision、位置 bool 又隐含业务策略。不能据此把所有 bool 或 optional&lt;Result&lt;T&gt;> 都判错。

**目标解决方法。** 候选成为拥有领域准备结果的 PreparedSessionData；飞行中工作成为稳定地址操作记录；内部互斥阶段携带所需资源。状态快照只从内部状态派生。显式 CurrentRegistration/PinnedRegistration 策略，真实二值属性仍保留 bool。

**落地关系。** 原则：P02、P03、P06；替代类型：`PreparedSessionData / SaveOperation / RegistryBinding`；模块：M04、M05、M06、M13、M15；验收：Q05、Q11、Q40。

<a id="t07"></a>
### T07 — 通用错误包装承载控制逻辑，展示源也混淆

**证据等级：F/D。定位：** `EditorFailure；reportMenuFailure；Scene submit(bool)`。[S04] [S17] [S19]

**问题与成因。** 错误原因开放容器有合理用途，但提交阶段、是否已生效、背压等决策信息散落在其他字段。菜单显示函数还接收非菜单错误；一些 bool 压平背压和永久失败。

**目标解决方法。** 各领域返回能用于决策的结果；Backpressured、Rejected、Accepted 保持区别。统一诊断只在边界转换，不参与业务真假判断。插件开放原因可保留，但不以字符串或 any_cast 承担核心流程分支。

**落地关系。** 原则：P03、P04、P10；替代类型：`SubmitOutcome / SaveOutcome / LayoutApplyReport / Diagnostic`；模块：M06、M09、M12、M13、M15；验收：Q17、Q29、Q36。

<a id="t08"></a>
### T08 — const 编码输入隐藏共享可写输出

**证据等级：F/D。定位：** `SceneSaveCapture::copied；SceneEncoder::operator()`。[S19] [S20]

**问题与成因。** 编码器除返回字节，还经 const 输入中的 shared_ptr 写出复制包。源码有 CPU 写、终态后读的说明，不能把语义不透明直接定性成已发生的数据竞争。

**目标解决方法。** 编码函数输出完整 EncodedScene；普通保存只写不可变捕获。Save As 将新身份包和采用计划作为显式阶段值，只有存储提交成功后由 owner 采用。公共写服务只看编码产物，不拿可写 SceneSession。

**落地关系。** 原则：P03、P04、P06；替代类型：`FrozenSave / SceneEncodeJob / EncodedSceneClone / SceneSaveAsOperation`；模块：M04、M06、M07、M15；验收：Q11、Q15、Q16。

<a id="t09"></a>
### T09 — EditingGuard 没有落实独占且可以被复制

**证据等级：F/R/D。定位：** `SceneEditorImpl.hpp::EditingGuard`。[S19]

**问题与成因。** 构造写 true、析构写 false，没有禁止复制或说明嵌套。嵌套/复制可提前解除外层 busy；本次没有证明现有调用已触发。

**目标解决方法。** 把准入与执行范围分开。公开操作返回 Busy 等合法拒绝；通过准入后创建不可复制、不可移动的 EditScope。关闭使用另一种跨阶段 ClosePermit，不复用 bool guard。

**落地关系。** 原则：P02、P03、P06；替代类型：`SessionAccessGate / EditScope / ClosePermit`；模块：M03、M04、M05；验收：Q09、Q10、Q20。

<a id="c01"></a>
### C01 — 布局在最后的 docking 验证前已修改窗口

**证据等级：F/R。定位：** `Editor::Impl::restoreWorkspace；Root::restoreDockState`。[S05] [S25]

**问题与成因。** 先恢复/创建窗口和设置可见性，后解析恢复 docking；损坏 docking 数据可令前面修改已发生而整体返回失败。尽力恢复的允许范围与报告也不够明确。

**目标解决方法。** 纯 parse/validate 生成 LayoutPlan，解析失败不触及视图；准备脱离树的新视图；在 Host 修改阶段提交结构计划。未知插件使用可保留 payload 的占位项；可容忍失败必须列入报告。结束编辑手势的副作用单独说明。

**落地关系。** 原则：P04、P07；替代类型：`ValidatedLayout / LayoutPlan / PreparedMountBatch / LayoutApplyReport`；模块：M11、M12、M15；验收：Q31、Q32、Q33、Q34。

<a id="c02"></a>
### C02 — 多文件已部分提交，却只返回整体失败

**证据等级：F/R。定位：** `EditorWorkspaceStorage::writeWorkspace`。[S05] [S06]

**问题与成因。** 布局保存/重命名/删除后再写 settings，再枚举目录；后续失败不能撤销已发生的磁盘变化。单文件替换不构成多文件事务，更不证明断电持久性。

**目标解决方法。** 先消除无必要的联合写入：LayoutId 与显示名称分离，重命名不改偏好引用。布局提交、活动选择持久化、目录刷新各有结果；真正需要强一致的数据才同记录提交或使用专门日志，不建设全局事务框架。

**落地关系。** 原则：P01、P04、P07；替代类型：`LayoutId / LayoutCommitReceipt / PreferenceWriteResult / CatalogRefreshResult`；模块：M12、M15；验收：Q35、Q36、Q37。

<a id="c03"></a>
### C03 — QUERY 期间可销毁正在执行的注册闭包

**证据等级：F/R。定位：** `EditorMenu.cpp::applicationCommand 的 QUERY 分支`。[S04] [S13]

**问题与成因。** 执行分支复制 registration，查询分支直接经 found->invoke 调用；若该回调替换自身注册表并继续访问捕获，存在生命周期风险。不是内置命令必然崩溃。

**目标解决方法。** 查询和执行都持有 CommandRegistrySnapshot/entry 的代码与闭包生命期；发布注册变更到下一安全批次。查询输入是快照值而不是 EditorContext；闭包纯度仍需契约与测试，C++ const 不是沙箱。

**落地关系。** 原则：P05、P06、P10；替代类型：`CommandRegistrySnapshot / CommandEntry / CodeLease`；模块：M13、M14；验收：Q38、Q39。

<a id="c04"></a>
### C04 — 创建接口可能成功发布已失败的编辑器

**证据等级：F/R。定位：** `initializeMenu；EditorStartup::Impl::create`。[S03] [S04]

**问题与成因。** 菜单连接失败调用运行态 fail，但 initializeMenu 返回 void，创建入口可能继续启动并返回成功对象。创建成功与运行时致命失败混合。

**目标解决方法。** 构造函数只建立局部不变量；需要失败处理的装配返回 Result。所有关键订阅和服务接线准备完成后再发布 EditorApplication。运行期故障是另一个通道，禁止用运行态 flag 替代创建失败。

**落地关系。** 原则：P02、P04；替代类型：`EditorApplication::create / StartupFailure`；模块：M16；验收：Q42、Q43、Q44。

<a id="c05"></a>
### C05 — 高亮缓存先提交新键，准备失败后可能不再重试

**证据等级：F/R。定位：** `SceneEditor::Impl::updateHighlight；highlighted_*`。[S19]

**问题与成因。** 更新 highlighted_* 早于资源捕获；若捕获失败且无待提交程序，下一轮选择等未变，可能跳过本应重试的构建。还需区分已有旧 pending 与新 desired 的组合。

**目标解决方法。** 将高亮单独建模为 desired、prepared、accepted 三个阶段，accepted 只在后端确认接受后前移；准备失败保留 dirty，背压保留完整候选和资源 pin。GPU 完成退休与已接受不是同一事件。

**落地关系。** 原则：P02、P04、P08；替代类型：`HighlightRenderer / HighlightKey / PreparedHighlight / SubmitOutcome`；模块：M09；验收：Q28、Q29、Q30。

<a id="b01"></a>
### B01 — 架构标签不等于依赖约束

**证据等级：F/D。定位：** `TargetClassification.cmake；顶层 CMakeLists.txt`。[S29] [S30]

**问题与成因。** 已审阅的分类函数设置属性，但未在这些入口看到关键反向依赖的强制验证。不能因此断言当前已经存在某条 Runtime→Editor 链。

**目标解决方法。** 按本文 target 依赖白名单校验 target 图，并加公开头自包含/负向 include 测试；区分代码依赖与代码生成工具依赖。新 target 不可通过全局 include path 或 INTERFACE 汇总绕过边界。

**落地关系。** 原则：P05、P11、P12；替代类型：`target 依赖白名单 / 公开头独立编译检查（构建机制）`；模块：M16；验收：Q45、Q46、Q47。

<a id="b02"></a>
### B02 — Editor 缺少与复杂度匹配的持续验证入口

**证据等级：F/D。定位：** `现有 .github/workflows/deb.yml`。[S31]

**问题与成因。** 该工作流主要配置 PLAYER；其中 Lua 依赖测试不是 editor 主项目 CTest。提交说明中的本地成绩不能自动转化为下一提交的持续保护。

**目标解决方法。** 建立 domain、workflow、desktop、GPU、installed 五层验证；常规 PR 跑可无 GPU 的 Editor 测试；专项条件明确标记。新架构的契约测试而非只检查类是否存在。

**落地关系。** 原则：P11、P12；替代类型：`native CTest / FakeArtifactStore / FakeViewHost`；模块：M16；验收：Q01、Q45、Q49、Q50。

<a id="b03"></a>
### B03 — 平台资格、产品构建与安装消费者测试混杂

**证据等级：F/D。定位：** `editor/app/CMakeLists.txt；工具测试配置`。[S28]

**问题与成因。** BUILD_TESTING 配置包含特定 lld-link 查找路径、PowerShell 和 .ps1 包装；安装消费者源码与产品测试复用交织。不是断言 Linux 绝对无法安装这些工具。

**目标解决方法。** 协议与领域测试直接用 CTest 启动原生测试；桌面平台脚本独立标签；安装消费者另项目，只能使用 install tree。test_support 有自己的 target，不向产品库或公开头注入 TestAccess ABI。

**落地关系。** 原则：P05、P11、P12；替代类型：`test_support / installed_consumer / desktop_integration`；模块：M16；验收：Q46、Q49。

<a id="b04"></a>
### B04 — 全局 SDK 指纹扩大耦合且不能证明 ABI 兼容

**证据等级：F/D。定位：** `cmake/LuxPlugins.cmake`。[S33]

**问题与成因。** 全局构建输入指纹可用于拒绝混装，但会将不相关 SDK 变更关联起来；名字包含 ABI 不等于进行了 ABI 差异验证。当前代码保活也不能据此被断言失效。

**目标解决方法。** 先明确同工具链同 SDK 的加载契约，区分接口版本、构建指纹和能力协商。运行扩展与编辑扩展的依赖闭包分别计算；稳定跨编译器 C ABI 不是本轮默认目标。

**落地关系。** 原则：P05、P10、P11；替代类型：`InterfaceVersion / SdkBuildFingerprint / CodeLease`；模块：M14、M16；验收：Q46、Q48、Q52。

<a id="principles"></a>
## 3. 新设计哲学：从可观察行为推导结构

<a id="p01"></a>
### 3.1 P01 — 概念先于类名，类名先于目录

先写清“用户打开的是一份内容”“用户关闭的是一个视角还是一份内容”“运行的是哪个内容状态”，再定义 Session、View、Run。不能从 SceneEditor 有哪些成员出发，每 200 行分一个 Manager。

由此得到本方案最重要的区分：**打开资产、打开工作副本、创建视图、绑定视图、显示视图，是可以组合的不同操作。** 打开一个项目资源时，产品可以默认复用已有工作副本；用户明确要求独立副本时可以另开。复用是策略，不是工厂的暗中行为。对应 A02/A04/A08/T01/T02。

<a id="p02"></a>
### 3.2 P02 — 谁维护不变量，谁拥有相关状态

一组字段如果必须共同变化，就应由同一个对象维护。`SaveOperation` 拥有保存阶段、捕获和写入票据；不是 Session 保存 `saving`、窗口保存 `save_result`、Application 再保存 `save_pending`。

资源所有权和业务生命周期要分开理解。后台编码可以共享不可变快照，但不能因为它持有 shared_ptr 就决定编辑内容仍算“打开”。SessionStore 决定内容是否可被用户访问，快照自己的所有权决定后台计算是否安全。对应 A01/A02/T06/T09。

<a id="p03"></a>
### 3.3 P03 — 类型首先表达事实类别，而不是添加装饰

区分值、身份、观察快照、短期借用、长期操作、完成回执。`StateId` 不是窗口身份；`Accepted` 不是完成；`const shared_ptr<T>` 不是不可变数据；析构不是用户选择。

已有强类型有正确语义就复用。新增类型必须说明防止哪一种混用，或者保护哪一个不变量。没有不变量的数据可以是 struct；仅包含两种合法取值的属性可以是 bool。对应 T01—T09。

<a id="p04"></a>
### 3.4 P04 — 每个有副作用的流程都明确提交点

接口至少回答：拒绝是否无副作用；准备成功时是否已对外可见；什么时候外部状态正式改变；完成结果对应什么事实。文件编码结束、文件发布成功、会话采用新身份、GPU 接受提交、GPU 执行完成，都有不同含义。

不是所有函数都需要四个对象。同步 setSelection 可以立即完成；跨 IO 的保存才需要长期操作记录。统一的是判断规则，不是一个强迫所有业务继承的 Operation 基类。对应 C01—C05/T04/T08。

<a id="p05"></a>
### 3.5 P05 — 依赖不只是“能调用谁”，还包括“能改变谁”

工具需要保存，不等于它需要直接接触文件系统和全局会话删除入口。提供 `SaveService` 的正常使用入口与提供服务装配/注册修改入口，是两种权限。

优先具体引用和小接口；禁止通过注入的某个成员再次取得全局 Editor。工厂被绑定的能力也必须列入模块清单，不能把万能 Context 捕获进 lambda 后宣称参数已经解耦。对应 A03/A05/A06/A07。

<a id="p06"></a>
### 3.6 P06 — 稳定身份跨时刻，短期引用留在当前访问阶段

编辑模型仅在其 owner 线程变更；后台工作读取不可变捕获并发布完成值。完成值带 SessionId、StateId 或操作代际，采用时验证。借用不得跨越 owner 调度、结构变更、挂起或可重入回调。

这不是为所有对象加锁，也不是要求业务模块自己创建线程。继续使用现有 ExecutionRuntime 和任务基础设施；新的模块边界只规定提交、快照和结果采用位置。对应 A05/T06/T09/C03。

<a id="p07"></a>
### 3.7 P07 — 用正确边界减少事务，必要事务才准确实现

工作区名称变化不应迫使偏好文件重写；显示第二个视图不应迫使第一份内容关闭。这些耦合可以直接消除。

真正无法避免的跨阶段副作用必须准确报告。Save All 可以有部分内容已落盘，随后用户取消退出；不能声称先前保存回滚了。文件替换也不能被写成无条件的断电持久事务。对应 C01/C02/T03。

<a id="p08"></a>
### 3.8 P08 — 作者事实与派生状态分开，缓存不成为第二份真相

作者内容和历史是编辑事实；场景渲染投影、运行实例、选择高亮和编译预览都由它派生。派生状态可以落后，但必须知道自己对应哪个源状态，以及如何追赶、重建或报错。

“准备过某个键”不等于“这个键的输出已被接受”；GPU 接受也不等于执行完成。对应 A02/C05，以及多视图和运行隔离。

<a id="p09"></a>
### 3.9 P09 — 默认组合；继承只表达实际可替代的角色

SceneSession 可以作为“有身份和关闭准入的编辑会话”被通用集合管理，因此实现窄 `IEditSession` 合理。它不是一种 Pane，也不是一种运行实例。

SceneView 是实际的 UI Pane，可以继承 Pane；它通过组合持有视图状态、交互绑定和呈现对象。Scene 与 Material 不共享领域模型，不为了复用保存就继承一个带几十个 virtual 的 Document。对应 A02/A08。

<a id="p10"></a>
### 3.10 P10 — 开放扩展只在边界擦除类型

插件注册记录、动态加载代码、未知视图配置需要开放边界，允许有版本的字节载荷、代码 lease 和有限动态接口。进入 Scene/Material 内部后立即恢复具体类型，不把 void*、std::any 和字符串 action 一路传到领域算法。

不为全部工具建立一个不断增长的全局 variant，也不将所有领域原因塞进一个全引擎错误枚举。对应 A07/T07/B04。

<a id="p11"></a>
### 3.11 P11 — 模块必须有可检查的编译边界

一个模块至少有独立责任、拥有的状态、公开入口和禁止依赖。只有文件夹，没有这些约束，不计为模块化。

分开逻辑子系统与链接产物：一个子系统可以含 API、实现、平台适配三个 target；一个类型不必是一个库。PUBLIC/PRIVATE/INTERFACE 按实际使用需求配置，而不是把所有链接都改 PRIVATE。[R02] [R03]

<a id="p12"></a>
### 3.12 P12 — 用完整行为证明抽象，而不是用类图证明

一个场景内容两个视图、保存期间继续修改、独立运行、关闭一个视图、非破坏性应用布局，是最小验证链。随后用 Material 检验共同接口是否真正通用。

类数、目录数、代码减少量都不是验收指标。边界测试、失败后的状态、安装消费者与实际性能证据才是。对应 B01—B04。

上述原则与 C++ Core Guidelines 对精确接口、由类维护不变量、以真实层次关系使用继承的方向一致；具体模块方案是针对本项目的设计判断，不是该指南规定的唯一结构。[R01]

<a id="vocabulary"></a>
## 4. 先固定语义词典与不变量

### 4.1 词典：每种事实只能有一个主表达

| 术语 / 类型 | 精确定义 | 不等于什么 | 归属 |
|---|---|---|---|
| `AssetAddress` | 项目中的持久化资产地址；底层可复用 Project/Asset 身份 | 一个当前打开的窗口或工作副本 | M03 的来源契约，使用现有资产基础类型 |
| `SessionId` | 本进程中一份打开工作副本的身份，含代际 | AssetId、HistoryId、ViewId | M03 |
| `SessionKey<T>` | 已校验具体会话类型的会话身份 | 对模型的永久裸指针、拥有者 | M03 |
| `StateId` | 某条历史中的一个确定内容状态，已有 HistoryId+serial | 单调通知计数或文件版本 | M02，复用现有语义 |
| `ContentStamp` | SessionId 与 StateId 的组合，定位一次捕获的作者状态 | 整个文件的 hash、随意的 revision | M03 |
| `ObservationVersion` | 历史身份和通知版本；用于观察者判定需不需要刷新 | 干净状态或保存先后序 | M02/M03 |
| `BindingRevision` | 当前来源/保存目标绑定的版本 | 内容版本；Save As 可改变它而普通编辑不改变 | M03 |
| `ViewId` | 一个视图实例的身份 | 内容身份 | M11；若与 PaneId 同义，直接复用，不再包第二层 |
| `LayoutSlotId` | 持久化布局中的符号槽位 | 本次进程的 ViewId | M12 |
| `RunId` | 一次运行会话的身份 | 作者 SessionId 或底层 SceneInstanceId | M08 |
| `SceneInstanceLease` | 对引擎场景实例的释放责任 | 编辑源、UI 视图、第二套 Runtime | 引擎运行时与 M08 适配边界 |
| `OperationId` 的各领域特化 | 某次跨时刻操作，如 SaveId、OpenId、CloseId | 普通 getter 的调用序号 | 定义在各拥有操作的模块 |
| `CommitReceipt` | 存储后端已经完成约定发布步骤的事实 | 编码已结束、当前内容已干净、必然断电持久 | M06 |
| `Diagnostic` | 展示和记录所需的诊断数据 | 核心业务控制流的真相 | 应用边界；不反向控制领域 |

不要求全部新造整数包装。现有 Id 能准确表达同一概念就复用；只有生命周期或防混用需求不同才新增。`ContentStamp` 不再重复增加一个与 HistoryId 同步的 SourceEpoch：**重载生成新 HistoryId，本身就能隔离旧历史结果。** 若未来允许不换历史却整体换源，应另立契约，不偷偷复用旧戳。

### 4.2 十二个跨模块不变量

| 编号 | 不变量 | 主要执行者 |
|---|---|---|
| N01 | 同一份作者内容只有一个可写拥有者 | SceneSession / MaterialSession |
| N02 | 视图显示、隐藏和布局变更不隐式改写内容 | ViewHost / ApplyLayoutOperation |
| N03 | 操作结果只能作用于其指定代际与内容/绑定状态 | SessionStore / 各操作 owner |
| N04 | 编辑内容和历史作为一次逻辑提交同时对外可见 | 领域会话 + EditHistory |
| N05 | 保存清除的是被保存状态的未保存标记，不是“回调时的当前内容” | SaveService + PersistenceCheckpoint |
| N06 | 同一发布目标不会被较旧的已准入保存无序覆盖 | 写入通道与目标版本检查 |
| N07 | 关闭内容是显式决定，引用计数不决定产品生命周期 | CloseSessionsOperation / SessionStore |
| N08 | 运行暂停不停止必要维护；停止运行不隐式回写作者源 | RunController / engine runtime |
| N09 | 未成功准备/接受的渲染结果不能推进已接受缓存键 | HighlightRenderer / SceneProjection |
| N10 | 当前回调、虚析构和异步结果清理完成前，所需插件代码保持存活 | CodeLease / CommandRegistrySnapshot / operation records |
| N11 | 领域及通用运行层不依赖桌面 Shell、UI 私有头或应用根 | CMake 依赖校验 + 公开头测试 |
| N12 | 对外结果准确报告已提交、未提交、部分完成和取消的边界 | 各操作结果与应用呈现 |

### 4.3 来源状态与历史状态：不制造第二套保存真相

目标中 `EditHistory` 专注历史与编辑提交；保存目标和已持久化基线由会话组合的 `PersistenceCheckpoint` 记录。现有 beginSave/finishSave 是可用的迁移起点，但最终不能同时由 History::saved 和另一个 tracker 独立决定 dirty。[S34] [S36]

这是一项明确的职责迁移：保留历史算法、StateId、准备/提交/发布机制，迁出保存票据与目标相关状态。迁移期可有**单向适配**，禁止两个可修改的 saved 字段长期双写。

干净判断采用历史状态身份，而非实时全量内容哈希：当前 StateId 等于本绑定的保存基线时为干净；撤销回保存点可以变干净。重新执行操作构造出字节相同内容，但未回到同一状态身份，第一版仍算 dirty。这个保守定义必须文档化，不能承诺未实现的内容等价识别。

重载成功保留 SessionId，让现有视图仍定位这份工作副本；生成新 HistoryId 和初始 StateId，并一次性更新源、历史与绑定基线。旧保存或编译结果因历史身份不同而不能采用。失败则保留原工作副本，不能先 clear 再读取。

<a id="ownership"></a>
## 5. 运行时所有权：三个独立集合，有限的跨界引用

### 5.1 目标所有权图

```text
EditorApplication（装配与生命周期根；不是供业务传递的 Context）
│
├── SessionStore
│   ├── SceneSession A
│   │   ├── SessionState（身份、来源、基线、编辑准入）
│   │   ├── SceneSource（作者态数据）
│   │   └── EditHistory（编辑提交与撤销重做）
│   └── MaterialSession B
│       ├── SessionState
│       ├── MaterialSource
│       └── EditHistory
│
├── ViewHost / DesktopShell
│   ├── SceneView 1 ── SessionKey<SceneSession> ──► A
│   ├── SceneView 2 ── SessionKey<SceneSession> ──► A
│   ├── OutlinerView ── InteractionGroupId / typed access ──► A
│   └── MaterialView ── SessionKey<MaterialSession> ──► B
│
├── RunStore
│   └── RunSession R
│       ├── provenance = A 的 ContentStamp（来源记录，不是可写引用）
│       ├── 不可变启动快照 / 执行描述
│       └── SceneInstanceLease（实际记录由 engine runtime 托管）
│
├── SaveService
│   └── SaveOperation：不可变捕获 → 编码 → 有序发布 → 完成回执
│
├── PresentationHub（派生资源的拥有者/缓存，不是内容 owner）
│   ├── SceneProjection(A)
│   ├── ViewportPresentation(View 1)
│   └── ViewportPresentation(View 2)
│
└── 既有执行器、项目存储、渲染运行时、插件加载和平台服务
```

同一 Session 可以有零个、一个或多个视图；相机、尺寸、悬停、输入捕获和临时拖拽属于视图/交互。历史、作者内容、来源与持久化基线属于 Session。Run 只保留启动来源与独立执行状态。

### 5.2 强拥有、借用、关联、代码保活不能混写

| 关系 | 推荐 C++ 表达 | 生命周期约束 |
|---|---|---|
| Store → 多态 Session | `unique_ptr<IEditSession>` | Store 唯一释放；关闭前取得许可并解绑相关服务 |
| SceneSession → Source/State | 值成员；大而稳定的对象可 unique_ptr | 不公开可写所有权容器 |
| SceneSession → History | 值成员或现有工厂产生的 unique_ptr | history 中的 edit/memento 必须先于所依赖的 source 销毁 |
| View → Session | `SessionKey<T>` | 每次业务访问验证代际；不延长“打开”状态 |
| 后台工作 → 数据 | 冻结快照值或 `shared_ptr<const FrozenData>` | 必须实际不可变，不能仅把可写图改成 const 指针 |
| 执行调用 → 服务 | 构造时注入的具体引用/窄接口引用 | 服务由装配根保证比调用者长寿 |
| 已发布注册 → 插件代码 | `CodeLease` | 闭包、虚析构、allocator/deleter、异步产物清理都完成后释放 |
| View → 选择组 | InteractionGroupId；需要联动时由组对象拥有选择 | 关闭一个视图不删除其他视图共享的选择事实 |
| 渲染 → GPU 资源 | 既有资源 pin + 提交/退休票据 | 不能在 C++ 对象离开作用域时假定 GPU 已使用完 |

### 5.3 为什么不让所有对象共享 Session 的 shared_ptr

shared_ptr 可以正确解决内存存活，却不能回答“用户已经关闭这份内容了吗”。保存任务、隐藏窗口、诊断回调都可能保留强引用，使产品生命周期变得偶然。

本方案让内容在 SessionStore 中被明确标记/移除。后台任务只保留冻结输入和操作记录；迟到结果进入 SaveService，而不是捕获 SceneSession*。Session 不存在时仍可保留“文件已经提交”的事实，但不能访问旧对象，也不能把相同槽位的新 Session 当成旧 Session。

### 5.4 生命周期策略必须在用例层明说

**关闭普通视图：** 只释放该视图与对应 GPU 视口资源。  
**关闭最后一个视图：** 默认采用编辑器的“询问是否同时关闭内容”的显式策略；可以选择保留无视图工作副本。策略名字写进 `CloseViewPolicy`，不藏在析构里。  
**关闭内容：** 审阅未保存状态、处理关联操作、解绑视图，然后删除 Session。  
**内容关闭时仍有运行：** 第一版默认显式询问/停止关联 Run；若产品选择保持运行，Run 必须仅依赖已冻结输入，且其视图绑定的是 RunId。  
**退出：** 对固定 Session 集进行审阅，全部取得许可后再提交关闭；退出仍需处理任务结果、GPU 退休和插件寿命。

### 5.5 所有权成立后，目录才有意义

若 SceneView 仍创建/删除 SceneSession，或者 SaveService 仍能经 EditorContext 修改窗口，则即使类图画了三个 owner，实际所有权仍未改变。验收必须搜索真实创建、删除与写入入口，而不只检查目录名称。

<a id="session-types"></a>
## 6. 替代类型设计一：会话、历史、身份与编辑权限

### 6.1 关系选择总则：哪些继承，哪些组合

| 具体关系 | 选择 | 理由与限制 |
|---|---|---|
| SceneSession / MaterialSession → IEditSession | **公开接口继承** | 两者都可以被异构会话集合识别、观察、准备关闭；基类不包含 UI、运行、编译和领域编辑 |
| SceneSession → SceneSource / SessionState / EditHistory | **组合** | 源、通用状态和历史是会话的组成，不是它的基类 |
| MaterialSession → MaterialSource / SessionState / EditHistory | **组合** | 与 Scene 共用机制，不共用领域模型 |
| SceneView / MaterialView / OutlinerView → ui::Pane | **公开 UI 继承** | 它们确实是一种可显示的 Pane；仅 UI 角色继承，不再兼任内容 |
| 领域编辑操作 → EditOperation / PreparedEdit | **保留真正的协议继承** | 历史需要异构、可准备与可重放的编辑操作；不得在操作里读取 EditorContext |
| SceneSaveSource / MaterialSaveSource → ISaveSource | **适配器继承** | 向保存服务提供统一捕获协议；适配器借用具体会话，不拥有它 |
| SceneEncodeJob / MaterialEncodeJob → IEncodeJob | **异步边界接口继承** | 已冻结输入的编码是可替代的工作；只有编码接口，不承担文件写入/会话采用 |
| ProjectArtifactStore / FakeArtifactStore → IArtifactStore | **副作用边界接口继承** | 实际存储与故障注入实现具有同一精确定义的发布契约 |
| RunSession → SceneSession | **禁止继承** | 运行不是一种作者工作副本，停止不能触发编辑析构逻辑 |
| SceneSession → Pane / LuxObject 以获得所有 UI 服务 | **禁止作为通用办法** | 通知能力可以组合；不能为了发信号而继承整个 UI 生命周期 |
| 各用例 → UniversalOperation | **不引入该基类** | 相同“有阶段”不等于相同业务接口；共享 ID/终态通知即可 |
| CommandEntry / ViewFactoryRegistration | **值组合 + 有界回调** | 动态注册的描述、入口与 CodeLease 组合；不再建立 IPlugin 万能对象 |

这不是“组合总比继承好”的口号。继承成立的必要条件是：基类的每个公开操作，对所有派生类都有真实而一致的含义；不存在一串默认返回 Unsupported 的功能。

### 6.2 公共会话角色必须足够窄

下面是签名方向。`Result<T,E>` 代表现有 expected 别名；辅助 Id、错误和只读快照在所属头文件声明，不在一个巨大 Types.hpp 中全部展开。

```cpp
// sessions/include/.../SessionInfo.hpp
struct ContentStamp {
    SessionId session;
    editing::StateId state;  // 已含 HistoryId，不再重复 SourceEpoch。
};

struct SessionInfo {
    SessionId id;
    SessionKindId kind;
    SourceBinding binding;          // 拥有的值；不是 filesystem 服务。
    ContentStamp current;
    ObservationVersion observed;
    DirtyState dirty;               // 由当前状态和基线派生。
    EditAvailability editing;       // 查询快照；不是另一个可写 gate。
};

// sessions/include/.../IEditSession.hpp
class IEditSession {
public:
    virtual ~IEditSession() noexcept = default;
    [[nodiscard]] virtual SessionInfo describe() const = 0;

private:
    friend class SessionStore;
    // 由生命周期 owner 调用；不是 UI 的冻结/解冻入口。
    [[nodiscard]] virtual Result<ClosePermit, ClosePrepareError>
    prepareClose(ContentStamp expected) = 0;
};
```

基类不提供 open/reload/save/play/compile/undo。打开与重载是用例，保存由角色适配器接入；历史快捷键通过 `HistoryActions` 适配到具体会话；运行与编译有独立 owner。这样，未知工具也不必实现无意义的运行或图形方法。

`prepareClose` 在基类中是生命周期 owner 的私有入口。派生类也将覆写保持 private。它不弹对话框、不写文件，只验证该状态能否被锁定并返回 permit。`friend SessionStore` 是单一所有权边界的紧密协作，不是让六种 UI 类型访问全部 Impl。

### 6.3 SessionStore 负责身份与拥有，不负责全部业务

**拥有：** 稳定槽位中的 `unique_ptr<IEditSession>`、代际、具体类型标识。  
**负责：** 私有候选采用、发布、按身份查询、准备关闭、在许可下删除。  
**不负责：** 文件读写、保存对话框、菜单、运行、视图创建、通用事件总线。

异构存储不能靠随意 `void*`。目标使用带类型的 `SessionKey<T>`；从 factory 插入时验证 T 是 IEditSession 派生，记录对应类型 token；恢复过程将普通 SessionId 转成 typed key 时校验类型和代际。正常访问不需要每个对象查询都 dynamic_cast。 类型 token 采用显式注册且在当前 SDK 契约下可验证的 kind/type 映射，不能把 typeid(T).hash_code 的数值当作跨 DLL、跨进程稳定身份。Store 在构造候选前预留 SessionId 槽位，候选使用该保留身份；只有 Session 与配套角色均准备好才发布。预留失败或放弃不暴露半个 Session。

```cpp
// 面向使用者的受限访问；不提供 Store 的 adopt/erase。
template<class T>
class SessionAccess {
public:
    [[nodiscard]] Result<SessionInfo, SessionAccessError>
    describe(SessionKey<T>) const;

    // 借用规则：owner 线程、当前同步访问范围；不得跨 suspend/tick/reentry。
    [[nodiscard]] Result<std::reference_wrapper<const T>, SessionAccessError>
    read(SessionKey<T>) const;

    // 可写入口可仅向对应工具控制器提供，普通观察者不取得它。
    [[nodiscard]] Result<std::reference_wrapper<T>, SessionAccessError>
    edit(SessionKey<T>);
};
```

C++ 的 reference_wrapper 不能从语言上禁止调用者保存引用。因此该接口必须有生命周期文档、debug 代际/阶段检查和违约测试；不能声称返回一个 Borrow 类型就自动获得 Rust 式借用检查。需要跨时刻读取时使用真正拥有数据的 Snapshot。

Store 采用失败不能留下半注册 Session。用例先准备 Session 和适配器注册，再在单个 owner 发布阶段提交；撤销注册先于销毁会话。`InstalledSession` 在用例层记录配套注册资源，Store 仍是内容唯一 owner；该安装记录不是第二个内容对象。

### 6.4 SceneSession 的具体组成和 API

```cpp
// scene/model/include/.../SceneSession.hpp
class SceneSession final : public IEditSession {
public:
    [[nodiscard]] SessionInfo describe() const override;
    [[nodiscard]] SceneReadView read() const;

    [[nodiscard]] Result<SceneEditReceipt, SceneEditError>
    apply(SceneEditBatch);  // 一次作者编辑；不是菜单命令分发。

    [[nodiscard]] Result<SceneEditReceipt, SceneEditError> undo();
    [[nodiscard]] Result<SceneEditReceipt, SceneEditError> redo();

    [[nodiscard]] Result<SceneSnapshot, SnapshotError>
    capture(SnapshotBudget) const;

private:
    friend class ScenePersistenceAccess; // 精确的持久化适配，不是全局 Context。
    Result<ClosePermit, ClosePrepareError>
    prepareClose(ContentStamp expected) override;

    SessionState state_;
    SceneSource source_;
    std::unique_ptr<editing::EditHistory> history_;
    // 析构顺序：history_ 先于它可能引用的 source_。
};
```

`SceneSource` 可以由现有 World/ECS 存储承载作者态数据；不要求改成每帧全量序列化的另一套数据库。它必须划定作者字段和派生字段，且不能直接携带 UI 相机、GPU 高亮程序或执行时钟。

`SceneReadView` 是短期、只读领域观察；Outliner 可以取得拥有字符串与稳定对象身份的缓存快照，但不要每帧复制全场景。`SceneEditBatch` 表达具有同一历史含义的修改，内部继续利用已有 EditOperation/PreparedEdit 算法。SceneSession 对外不暴露可写 Registry。[S35]

`ScenePersistenceAccess` 只有记录已持久化基线、准备/提交来源重绑定所需的有限访问；可做小的 friend 适配器或私有服务对象。禁止用一个可访问全部字段的 `SceneSessionTestAndServiceAccess` 替代旧 Impl friend。

### 6.5 MaterialSession 不是 SceneSession 的特例

```cpp
class MaterialSession final : public IEditSession {
public:
    [[nodiscard]] SessionInfo describe() const override;
    [[nodiscard]] MaterialReadView read() const;
    [[nodiscard]] Result<MaterialEditReceipt, MaterialEditError>
    apply(MaterialEditBatch);
    [[nodiscard]] Result<MaterialSnapshot, SnapshotError> capture() const;
    [[nodiscard]] Result<MaterialEditReceipt, MaterialEditError> undo();
    [[nodiscard]] Result<MaterialEditReceipt, MaterialEditError> redo();

private:
    SessionState state_;
    MaterialSource source_;
    std::unique_ptr<editing::EditHistory> history_;
    Result<ClosePermit, ClosePrepareError> prepareClose(ContentStamp) override;
};
```

编译器输入为 MaterialSnapshot；编译完成带内容戳和编译配置戳，交给 MaterialPreview 采用。MaterialSession 不持有 GPU 预览场景、不调度主循环，也不因编译失败回滚合法的作者编辑。诊断可供界面读取，但不是对模型状态的另一份写权。

### 6.6 SessionState 不能成为换名后的 SharedEditorState

允许它保存的内容只有：SessionId、SourceBinding、BindingRevision、PersistenceCheckpoint、编辑/关闭准入门。当前内容状态来自 History，不复制一份 `current_state`；保存任务来自 SaveService，不复制一份 `save_pending`；当前选择属于交互组，不放进 SessionState。

`PersistenceCheckpoint` 记录“本来源绑定下，哪个 StateId 已被约定发布”。普通保存完成时，若内容已从 S10 前进到 S12，则基线更新为 S10，当前仍 dirty。若 Save As 已使绑定版本变更，旧绑定保存的完成不改变新绑定基线。

### 6.7 编辑独占与跨阶段关闭许可是两种类型

`EditScope` 表示当前同步调用内的不可重入编辑段，既不可复制也不可移动；由私有 `SessionAccessGate::tryEnterEdit()` 创建。它不携带 UI 对话框，也不能被存入后台任务。

`ClosePermit` 则是 owner 在确认某个 ContentStamp 后，持有的一次关闭授权。它可以 move，不可复制；持有期间拒绝新编辑/重载/重绑定；由 Store 删除时消费。未消费析构只释放准入限制，不表示“用户点击取消”。Store 必须在销毁会话之前将 permit 与 gate 脱钩，避免 permit 析构再次访问已释放 gate。

对话框开启期间可以允许继续编辑，但回复必须关联当时审阅的戳；最终取得 permit 前再验证。不能在用户审阅 S10 后，允许 S12 无提示地被丢弃。该规则用 Q20 验证。

<a id="edit-data"></a>
## 7. 替代类型设计二：作者数据、编辑操作、交互与通知

### 7.1 作者对象的身份不能由某个运行 Registry 的 Entity 代替

场景编辑 API 使用作者稳定身份，例如已有 WorldObjectId；运行对象使用 RunId + 运行实例中带代际的 Entity/对象身份。两者不能放入一个无来源的 `Entity` 字段后靠“当前正在播放吗”猜测。

```cpp
struct EditedObjectRef {
    SessionId session;
    editing::HistoryId history; // 整体重载后旧对象引用不能误指新对象。
    lux::world::WorldObjectId object;
};
struct RunningObjectRef {
    RunId run;
    RuntimeObjectId object;    // 需要具备后端对象代际，不只是裸 index。
};
using SceneSelectionTarget = std::variant<EditedObjectRef, RunningObjectRef>;
```

该 variant 属于 **Scene interaction**，不是编辑器全局可扩展对象全集。Material 有自己的 MaterialNodeRef。未来其他工具无需修改这个 variant，除非它确实要参与同一场景选择语义。

### 7.2 编辑操作与 UI 命令彻底分开

“删除当前选择”是 UI 命令，需要焦点和选择组；“删除这些作者对象并记录一次历史”是 SceneEditBatch。命令解析出一组明确对象后调用领域操作，不能把快捷键、窗口焦点、字符串 action 存进历史。

操作应至少具有：目标 ContentStamp/前置条件、所需领域数据、一次历史标签、可逆信息或确定的重放策略。现有 EditOperation/PreparedEdit 已可承担机制；需要删除的是它们对大 SceneEditor 的依赖，将目标变成具体 SceneSource/受限编辑服务。[S35]

一次提交应按以下顺序：验证目标与编辑 gate → 准备可应用数据和历史资源 → 在不可重入区域共同提交模型与历史 → 发布已提交的通知。通知回调不得在模型已变、历史未变时观察；回调触发新的编辑请求应进入下一允许阶段或明确拒绝。

### 7.3 手势预览不应污染保存快照

拖拽 gizmo 的过程中常有连续预览。本方案选择：`SceneInteractionGroup` 持有短期 gesture，预览变换优先作为视图呈现 overlay；结束时一次性形成 SceneEditBatch。普通保存捕获已提交内容。

若某类现有交互必须实时写作者数据，也必须使用明确的 EditTransaction：捕获前显式 finish/cancel，不能把未完成的临时状态悄悄视为已提交。迁移时可以保留该路径，但不得同时存在“预览 overlay”和“作者临时写入”两份未经定义的真相。

`finishInteraction()` 可能提交编辑，因而不是只读函数。应用布局或关闭触发它之前，应先完成纯格式检查和必需准入；失败保证分别说明对“布局结构”和“刚结束的编辑”是否有影响。

### 7.4 选择是交互状态，不是文件内容

`SceneInteractionGroup` 拥有 SceneSelection、当前 gesture、选中目标的有效性版本。SceneView、Outliner 与 Inspector 可以绑定同一组；独立视图可以新建组。组通过稳定对象身份关联内容，不拥有内容。

相机、投影方式、视口大小、鼠标悬停属于各 SceneView 的 `SceneViewState`。选择高亮的构建与 GPU 资源属于 HighlightRenderer，不放进选择模型。改变选择不生成内容历史，除非某个产品功能明确把选择作为可撤销交互历史，且那应是独立历史，不与资产 dirty 混在一起。

### 7.5 通知与状态快照的双层接口

通知只表示“哪个会话、哪个状态或观察版本已变化”，不携带必须跨帧借用的指针。接收者可以按需读取只读快照。

对于投影更新，可提供带上界的变更流：`changesSince(version)` 返回增量或 `ResetRequired`。增量被裁剪、插件模式变更、整体重载时，消费者必须有完整重建路径。通知不是可靠存储日志；不能因为错过一次事件就永远不一致。

已有信号/观察机制可以继续用，不新增全局 EventBus。领域会话如果需要发信号，组合一个通知源即可；无需为了通知继承全部 UI 对象体系。

### 7.6 删除对象与多视图一致性

删除由 SceneSession 提交，生成对象集合变化通知。多个交互组根据新版本移除失效目标，Outliner 和 Inspector 不再各自“顺便删一次”；所有视图从同一作者态收敛。运行中的同名对象不受影响，因为它属于不同 RunId。

这条流程的验收是 Q02/Q26，不是单独检查某个 selectChanged 信号发出了几次。

<a id="persistence-types"></a>
## 8. 替代类型设计三：保存、另存与存储提交

### 8.1 保存不是 Session 的长驻子状态，也不是窗口方法

Session 提供捕获事实和接受已验证基线的有限能力；`SaveService` 拥有 SaveOperation；`ISaveSource` 是二者之间的适配角色。SceneSaveSource/MaterialSaveSource 各自知道如何捕获本领域，却不操作窗口或菜单。

```text
SceneSession ──捕获──► SceneSnapshot ──组合──► SceneEncodeJob
       ▲                                       │
       │ ContentStamp + BindingRevision        ▼
SceneSaveSource : ISaveSource              EncodedArtifact
       │                                       │
       └──────────► SaveService ──有序发布──► IArtifactStore
                         │                     │
                         └──── CommitReceipt ◄─┘
                                  │
                         owner 阶段验证并更新保存基线
```

`ISaveSource` 不继承 IEditSession，Session 也不多重继承“可保存/可播放/可编译/可检查”。领域类保持具体，接口复用由外部角色适配器完成。

### 8.2 必须明确的类型与输入输出

```cpp
struct FrozenSave {
    ContentStamp content;
    BindingRevision binding;
    WriteTarget target;                  // 已解析的目标，不是默认空路径。
    std::unique_ptr<IEncodeJob> encoding; // 实际冻结输入由 job 拥有。
};

struct EncodedArtifact {
    SharedBytes bytes;                   // 拥有字节存活期。
    ArtifactFormat format;
};

class IEncodeJob {
public:
    virtual ~IEncodeJob() noexcept = default;
    [[nodiscard]] virtual Result<EncodedArtifact, EncodeFailure>
    run(std::stop_token) const = 0;
};

class ISaveSource {
public:
    virtual ~ISaveSource() noexcept = default;
    [[nodiscard]] virtual Result<FrozenSave, SavePreparationFailure>
    captureForSave() = 0;                 // owner 线程。

    // 这是回执采用，不是“将当前状态标成干净”。
    [[nodiscard]] virtual ReceiptAdoption
    accept(const SaveReceipt&) = 0;       // owner 线程；验证历史与绑定。
};

struct SaveReceipt {
    SaveId request;
    ContentStamp captured;
    BindingRevision binding;
    CommitReceipt commit;
};

class SaveService final {
public:
    [[nodiscard]] Result<SaveId, SaveAdmissionFailure> requestSave(SessionId);
    [[nodiscard]] SaveStatus status(SaveId) const;
    [[nodiscard]] CancelDisposition requestCancel(SaveId);
    [[nodiscard]] Result<void, ObservationError> acknowledge(SaveId);
};
```

`IArtifactStore` 的契约位于 persistence 边界，实际 `ProjectArtifactStore` 使用既有项目存储。其发布接口必须返回 `Committed`、`NotPublished`，以及后端确实无法确定时的 `PublicationUnknown`；不能以一个无阶段 error 掩盖目标是否已被替换。真实文件后端若已确认 rename 成功、只是后续持久性步骤失败，应报告已发布但持久性未确认，而非“没有保存”。

`ISaveSource::accept` 只更新自己 Session 的持久化观察；业务 Session 被关闭时，适配器注册被撤销。SaveService 仍保留磁盘提交事实，只跳过对已不存在 Session 的采用。不能让任务抓住会话裸指针以延迟调用。

### 8.3 SaveOperation 的状态与资源共址

| 内部阶段 | 必须拥有的数据 | 允许离开阶段的条件 |
|---|---|---|
| Captured / Encoding | ContentStamp、BindingRevision、WriteTicket、不可变输入、CodeLease | 编码成功、失败或取消已确认 |
| ReadyToPublish | EncodedArtifact、相同写票据 | 它是目标 lane 中下一个允许发布项 |
| Publishing | 后端工作句柄、目标预条件、字节 owner | 后端返回准确发布结果 |
| AwaitingAdoption | SaveReceipt 或准确失败/不确定结果 | owner 已做适用性判断并记录终态 |
| Terminal | Saved / CancelledBeforePublish / Failed / PublicationUnknown | 观察者确认或保留策略清理 |

实现可以使用 private variant，也可以使用具有良好不变量的具体记录；不要求公开这些内部阶段。必须删除旁边重复的 `pending`/`done`/`has_result` 真相。飞行中记录要么地址稳定，要么完成仅投递 SaveId；不能让 std::variant 的移动使捕获的 this 悬空。

任务进度可以保留既有 TaskId；SaveId 是业务操作身份，两者不同但通过显式关联连接。不要复制一套线程池、future 管理器和全局 OperationManager。

### 8.4 同一目标的有序发布：仅丢弃旧回调是不够的

目标身份必须先规范化为 `WriteTargetKey`：同一项目资产经不同路径/别名定位，应落到同一发布通道。路径字符串大小写、相对路径、符号链接等由实际后端契约处理，不能简单字符串相等就声称已规范化。

本方案选择：**接受保存时预留 WriteTicket，编码可并行，同一目标发布按准入顺序串行。** 不等编码完成后才排队，否则旧编码完成更晚仍可能覆盖新内容。

```text
接受 Save(S10) → ticket 41 ──编码较慢─────► ready
接受 Save(S12) → ticket 42 ──很快 ready───► 等待 41
发布 41 → 发布 42 → 磁盘最终为 S12
```

较早任务失败/在发布前取消时，明确终结其 ticket，后续才能推进。不能让 owner 线程阻塞等待，更不能占满所有 worker 等前序结果；只有当前可发布项被调度到写入阶段。

`StateId.serial` 不能用作保存顺序：用户可能撤销后保存更早的内容。以发布 ticket/提交序号判断较新落盘事实，而不是假设内容历史序号单调前进。

同一 Session 同一绑定的连续保存可以链接到前序已知提交；不同 Session 的独立工作副本不能未经确认继承另一 Session 的提交预条件。默认打开同一资产复用工作副本；独立副本遇到目标冲突应返回冲突并让用户重载、另存或显式覆盖，不做隐含 last-writer-wins。

**保证范围：** 本轮保证由同一 SaveService/协作项目写入端管理的提交顺序。普通文件系统的“检查文件版本后再替换”不能自动防止任意外部程序在两步之间写入；需要更强多进程保证时，后端必须实现实际锁/条件发布协议。没有实现之前，文档只能宣称外部修改检测，不得宣称全局 CAS。

### 8.5 回执采用的四个检查

采用正常保存回执时检查：Session 代际仍是同一个；HistoryId 仍属于同一内容世代；BindingRevision 仍对应此次保存目标；发布序号没有比已经采用的回执更旧。

**不要求当前 StateId 等于被保存 StateId。** S10 保存期间编辑到 S12 是合法情况；接受 S10 回执后基线为 S10，当前仍 dirty。整体重载到另一 HistoryId 或 Save As 改变绑定时，旧回执不能更新新基线。

完成通知可能以不同调度顺序被观察，即使后端发布已排序，采用端也必须按提交顺序防止基线回退。存储事实和会话适用性分开记录，不能把“会话已关闭，未采用”误报为“磁盘写入失败”。

### 8.6 Save As 的明确语义与冻结范围

本方案将两种产品动作分开：

| 动作 | 磁盘行为 | 当前会话行为 |
|---|---|---|
| Save As | 写入新持久化目标 | 成功后重绑定当前会话来源 |
| Export Copy / Duplicate Asset | 写出一个独立副本 | 默认不改变当前会话来源和保存基线 |

第一版 Save As 从捕获到持久化/采用完成期间持有该会话的 `BindingChangePermit`，禁止新编辑、重载和新普通保存；进入前处理完该会话已有保存。等待时间在 UI 可见，可以在发布前取消。普通 Save 不冻结编辑。

本文把 SaveAsOperation 作为用例统称；**实际领域实现为 `SceneSaveAsOperation` 和 `MaterialSaveAsOperation`，位于各工具持久化适配子模块。** 它们组合相同 WriteLane/发布服务，不为尚未证实的共同实现再增加万能基类。对应用暴露共同的 SaveAsOutcome 即可。

Scene 的编码函数明确返回 `EncodedSceneClone {artifact, package, identity_mapping}`，没有 `const input.copied` 隐藏输出。owner 在写入前准备好完整 `PreparedSceneRebind`；写入成功后，只执行已准备的采用步骤。写入失败则销毁候选并释放 permit，旧来源、旧历史和旧基线保持不变。

**目标模型约束：** 作者逻辑对象身份与打包后的资产地址分离。Save As 改变的是持久化绑定和打包身份映射，不应为了文件根资产换地址而清空用户编辑历史。已有 ScenePackage 复制算法可在 codec 适配中复用；新的 SceneSource/历史操作不能依赖“原文件根 AssetId 永不变化”这一隐含条件。

若某种插件格式确实不能在保留历史的前提下完成身份转换，必须在写入前返回 `RebindUnsupported`；仍可提供明确的 Export Copy。禁止写完文件后才发现无法采用，或无提示丢弃历史。它是格式能力限制，不是要求万能模板自动修复任意插件的身份模型。

### 8.7 取消、失败与持久性

取消是请求，不是时间倒流。发布前取消成功可得到 CancelledBeforePublish；目标已发布后，取消结果是 TooLate/AlreadyCommitted，最终仍保留已保存事实。acknowledge 仅清理已终结结果的观察，不取消正在执行的保存。

`CommitReceipt` 明确后端保证，例如“目标命名空间已替换”与“后端完成了所承诺的持久性步骤”。本文不因存在 `.next → rename` 就宣称断电安全。额外文件、索引或目录更新若不在同一提交记录内，应分别报告。

`PublicationUnknown` 不触发自动成功采用，也不直接重试覆盖。保留目标、操作身份与可校验的产物信息，使用后端查询/读取验证或用户明确决策来消除不确定。Save As 遇到此情况先保持旧绑定；若释放冻结让用户继续编辑，后续确认也不能自动采用旧候选，必须重新检查状态并重新取得绑定许可。不得无限冻结会话，同时又不给出可退出的结果。

<a id="run-types"></a>
## 9. 替代类型设计四：运行、作者投影与实际引擎运行时

### 9.1 RunSession 独立，不继承 SceneSession

```cpp
struct RunProvenance {
    ContentStamp source;
    RunConfiguration configuration;
};

class RunSession final {
public:
    [[nodiscard]] RunInfo describe() const;
    [[nodiscard]] Result<void, RunControlError> pause();
    [[nodiscard]] Result<void, RunControlError> resume();
    [[nodiscard]] Result<StepTicket, RunControlError> step();
    [[nodiscard]] Result<StopTicket, RunControlError> requestStop();

private:
    RunId id_;
    RunProvenance provenance_;
    SceneSnapshot startup_source_;
    SceneInstanceLease instance_;
    RunExecutionState state_;
};
```

`RunStore` 拥有 RunSession；`RunController` 处理“捕获→准备→创建运行→发布 RunId”的用例。RunSession 自己不是线程池，不另建一套场景 tick。真正场景记录仍在 engine runtime 中，以 lease 或受限 handle 表达释放责任。

### 9.2 SceneRuntime 可以重整，但不得出现两套驱动真相

目标将共享运行宿主与实例责任区分：宿主持有记录和统一调度，实例 lease 负责按规则退休记录。lease 的析构不能在任意回调中同步调用可能 BUSY 的 destroy；采用显式 `retire()` 或由已分配的退休记录在 host 安全阶段完成，析构只做受限的最终资源归还。

其接口至少区分：`find/borrowInstance`、`pauseSimulation`、`resumeSimulation`、`requestStep`、`retireInstance`、`driveFrame`。不再以 valid/invalid 同时暗示存在性和时间推进。现有 Builder、WorldMaterializer、Simulation 与资源算法可复用；改变接口不要求复制它们。[S22] [S23]

单个场景每帧只能有一个驱动入口。EditorApplication 调用统一宿主，RunController 不再从另一个路径额外 tick 同一记录。作者呈现实例可以使用相同宿主的“禁用仿真、启用维护/发布”模式，但绝不成为可写作者状态的另一个入口。

### 9.3 单步完成必须关联实际运行

step() 返回 StepTicket，意味着请求了一次步进；只有该 RunId 的实际仿真计数完成对应步数时，票据才终结。单个 `single_step=true` 与 `step_baseline` 分散在窗口中的形状不应继续保留。

暂停期间维护、资源完成、空间同步和渲染发布继续推进；恢复时明确重置/衔接时间基准，不把暂停期间的整个墙钟间隔当成下一仿真步。这里保留的是现有职责分界，不是旧函数名。[S23]

### 9.4 运行编辑与回写作者源

运行可以有临时调试修改，但它们属于 RunSession。第一版停止运行不会自动写回 SceneSource。需要“应用运行变更”时，先生成 `RunChangeProposal`，记录来源 ContentStamp 和涉及对象；对当前作者源做冲突验证，再作为普通 SceneEditBatch 一次性提交，进入作者历史。

不能用“把运行 Registry 整体覆盖编辑 Registry”实现应用变更；那会把运行临时字段、删除/新建对象和过期作者变更一起混入。未实现冲突规则之前，该功能应明确不可用，而不是隐式生效。

### 9.5 作者投影和视图呈现是两层

`SceneProjection` 维护作者数据到引擎可渲染数据的投影，对应一个 Session/源状态；`ViewportPresentation` 维护某个 View 的相机、尺寸、输出目标、gizmo 和高亮呈现。两个 SceneView 不应分别复制一份完整作者世界。

允许共享投影和 GPU 场景资源，但每个视图独立拥有相机与输出目标。选择组可以共享，具体高亮提交与视图相关资源不能不加区分地共享。PresentationHub 是派生资源缓存/owner，不保存可修改的作者内容，也不决定 Session 是否关闭。

### 9.6 HighlightRenderer 的提交协议

```cpp
struct HighlightKey {
    ProjectionVersion projection;
    SelectionVersion selection;
    RenderFeatureVersion feature;
};
struct PreparedHighlight {
    HighlightKey key;
    RenderProgram program;
    RenderResourcePins resources;
};

class HighlightRenderer final {
    std::optional<HighlightKey> desired_;
    std::optional<PreparedHighlight> prepared_;
    std::optional<HighlightKey> accepted_;
    // GPU 完成/资源退休票据由后端或独立 in-flight 记录拥有。
};
```

规则固定如下：desired 改变使组件需要更新；准备失败不改变 accepted，且下一轮仍允许重试；背压保留 prepared 全部资源；成功接受后推进 accepted 并移交/保留 GPU 所需 pin。若已准备旧键但 desired 更新，可在未提交时丢弃旧候选，重新准备最新键；已提交的资源必须按退休票据释放。

M09 的 SubmitOutcome 应是 Accepted{后端回执}、Backpressured{重试依据}、Rejected{结构化原因} 的互斥结果；它是资源提交结果，不继承保存或命令的结果类型。accepted key 只随第一种结果改变。后端回执可能仅表示接收，实际 GPU 完成仍由退休机制跟踪。

永久失败与暂时背压不同。不能每帧打印同一个永久错误；对同一失败键保留诊断并在依赖版本变化或明确 retry 时重试。资源暂时不可用的情况则保持 dirty/等待依赖，不能把失败当成完成。这一行为必须由具体失败分类决定，不使用 bool submit() 压平。

### 9.7 Material 编译与预览采用同样的派生原则，不强行共享实现

MaterialCompileOperation 输入 MaterialSnapshot + CompileSettings，输出带这两个版本的 CompiledMaterial/诊断。MaterialPreview 只采用当前仍适用的结果；过期结果可缓存，但不能覆盖更新的内容预览。编译失败不撤销用户合法编辑，也不把 dirty 清掉。

SceneProjection 与 MaterialPreview 可以复用结果采用和资源退休的小机制；它们不必继承共同的 PreviewDocument，领域输入输出继续保持具体。

<a id="view-types"></a>
## 10. 替代类型设计五：视图、UI 所有权与桌面 Shell

### 10.1 SceneView 仍可继承 Pane，但只能是一种视图

```cpp
struct UnboundSceneBinding {};
struct EditedSceneBinding {
    SceneSessionKey session;
    InteractionGroupKey interaction;
};
struct RunningSceneBinding {
    RunId run;
    InteractionGroupKey interaction;
};
using SceneViewBinding = std::variant<
    UnboundSceneBinding, EditedSceneBinding, RunningSceneBinding>;

class SceneView final : public lux::ui::Pane {
public:
    SceneView(ViewId, SceneViewBinding, SceneViewServices);

private:
    SceneViewBinding binding_;           // 三种真实绑定，不使用 null SessionId 猜测。
    SceneViewState view_state_;          // 相机、投影、可视选项等。
    ViewportPresentation viewport_;      // 未绑定时可呈现空视口，不含作者数据。
    SceneSessionAccess& scenes_;         // 受限访问，没有 Editor/Engine getter。
    RunInspectAccess& runs_;             // 轻量运行观察契约，不提供第二个 tick。
};
```

SceneViewServices 中只允许该视图实际需要的 Scene 读取/编辑能力、交互组访问、视口呈现工厂和正常 UI 请求。它不是全项目统一 services 包。SceneView 不拥有 EditHistory、SaveOperation、RunSession，不调用 asset reload 再清空自身模型。

Outliner/Inspector 可以是独立 Pane 或 Pane 中的 Element，取决于真实 UI 层次；它们都不能成为 SceneSession 的 friend。Inspector 的场景字段编辑通过 SceneEditBatch，运行字段检查通过 RunInspectAccess；两条路径在绑定类型上区分，不靠 inspectedScene() 隐式切换。

### 10.2 需要调整底层 UI：脱离树构造是目标前提，不是现有能力

当前 Pane 构造需要 Root/parent，具有注册关联。因此“工厂返回 unique_ptr”不足以保证纯准备。[S24]

目标 UI 改为：Pane/Element 可以先构成**未挂载的拥有树**；只有 ViewHost 通过 Root 的受限 attach 接口发布到活动 UI。Root 维护注册、路由、输入捕获与观察关系，ViewHost/Pane 拥有实际对象；禁止 Root 和 ViewHost 同时对同一对象承担删除责任。

```cpp
struct DetachedView {
    CodeLease code;                       // 声明在对象之前，最后析构。
    std::unique_ptr<lux::ui::Pane> pane;   // 未接入 Root，内部子树完整。
};

class ViewHost final : public IViewHost {
public:
    [[nodiscard]] Result<ViewId, ViewAttachError> adopt(DetachedView);
    [[nodiscard]] Result<void, ViewAccessError> show(ViewId);
    [[nodiscard]] FocusResult requestFocus(ViewId);
    [[nodiscard]] ViewInfo describe(ViewId) const;

private:
    // 只由生命周期/布局用例在 host 修改阶段调用。
    friend class ViewLifecycleAccess;
    Result<void, ViewDetachError> detach(ViewId);
};
```

`adopt` 先准备 Host 容量、Root 注册槽、路由和必要资源，成功后转移 owning pointer 并挂载；失败时 DetachedView 仍可被安全销毁。返回的是身份，不是“可能归自己拥有的某个引用”。显示与焦点是独立动作，模态限制导致无法聚焦不等于构造失败。

### 10.3 现有 UI 安全点怎么保留

保留“绘制/事件回调中不能同步销毁正在执行的 Pane”的约束；业务调用 `ViewRequests::requestClose(ViewId)`，在 owner 的结构修改阶段由 Host 执行卸载与销毁。[S24] [S25]

卸载顺序为：停止新的输入路由 → 解除捕获/焦点与观察引用 → 从 Root 注册中移除 → 排出正在执行的回调 → 将 GPU 资源移交退休 → 析构 Pane。若当前回调尚未返回，只能排队，不能靠 shared_ptr 暂时延长对象寿命后仍继续向逻辑关闭视图发事件。

为了批量布局提交，Root 可提供私有的 PreparedMountBatch：准备阶段可失败，提交阶段只做已准备的链接和状态交换，用户回调在提交后的通知阶段执行。这是 UI 子系统的局部机制，不是全编辑器通用事务引擎。

如果某 provider 的构造/恢复会自行访问网络、写文件或改变 Session，必须先改成阶段化输入输出；不能把该 provider 直接塞进批量 no-fail commit，然后声称有原子性。

### 10.4 视图工厂、视图复用与内容绑定各归其位

**ViewFactory：** 输入已验证的具体视图参数或恢复描述，产生 DetachedView。注册时绑定窄依赖；不得接收整个 ViewHost/EditorContext。  
**OpenAssetOperation：** 决定复用哪份 Session，以及要复用已有视图还是开第二个视图。  
**ViewHost：** 接管对象，维护 UI 活动树。  
**SessionViewBindings：** 用例层维护 SessionId ↔ ViewId 的关联，支持查找“关闭内容时要解绑哪些视图”；不拥有或复制内容。  
**SceneView：** 通过 SceneViewBinding 明确持有未绑定状态、typed SessionKey 或 RunId；仅在明确的 rebind 操作中改变目标。

通用 ViewHost 不解析 Scene 的绑定，不实现“当前是不是播放”的分支。运行视图与作者视图可以共用 UI 外观，但其绑定类型在 Scene UI 模块明确为 `EditedSceneBinding` 或 `RunningSceneBinding`。布局只创建外壳时使用 `UnboundSceneBinding`，不能构造一个无效 SessionKey 后让整个类型处于半初始化状态。Rebind 先准备新目标的观察和呈现关联，再交换绑定；失败保留旧绑定。RunInspectAccess 属于轻量运行 API，UI 不因此依赖运行实现。

### 10.5 ViewState 与内容状态不能再只差一个下划线

SceneViewState 包含相机、视口显示选项、局部面板展开状态；SceneSource 包含可持久化作者内容；SceneProjection 包含派生可渲染状态。采用这三个名词后，禁止重新出现 `content` 表示领域数据、`content_` 表示 UI 元素的同一作用域写法。

存储视图状态的格式必须标识 schema 版本。未知扩展的状态可以原样保留，但普通内部状态不使用 `map<string, any>` 替代类型。

<a id="layout-types"></a>
## 11. 替代类型设计六：布局、恢复记录和偏好

### 11.1 四个数据对象，四个不同问题

| 类型 | 内容 | 明确不包含 |
|---|---|---|
| DockLayout | 布局 ID/标签、槽位、DockTree、纯视图配置 | 作者模型、进行中的保存任务、运行 Registry |
| LayoutCatalog | 可用 LayoutId 与显示元数据的目录快照 | 每个布局的完整内容或当前 IO action |
| RecoveryManifest | 要重开的内容 locator、工具 kind、可恢复视图绑定 | 本进程裸指针、SessionId/RunId 作为永久身份 |
| UserPreferences | 活动 LayoutId、用户偏好 | 已打开内容的唯一事实来源 |

RecoveryManifest 不等于自动保存。只记录资产地址不能恢复未保存修改或未命名内容；要承诺崩溃恢复，必须另有真正持久化的恢复快照与版本协议。本轮不能因为有 manifest 就声称已有该能力。

### 11.2 类型草图

```cpp
struct LayoutSlot {
    LayoutSlotId id;
    ViewTypeId type;
    ViewRestoreKey view_key;       // 跨布局可复用的持久符号，不是内存地址。
    VersionedViewState state;      // 仅视图配置；作者绑定在恢复记录中。
    bool visible;
};
struct DockLayout {
    LayoutId id;
    std::string label;
    std::vector<LayoutSlot> slots;
    DockTree docking;
};
struct LayoutCatalog {
    CatalogVersion version;
    std::vector<LayoutSummary> entries;
};
struct LayoutApplyReport {
    LayoutId layout;
    std::vector<ViewId> created;
    std::vector<ViewId> reused;
    std::vector<SkippedLayoutItem> skipped;
    std::vector<PreservedOpaqueState> unavailable;
    LayoutApplyExtent extent;      // Complete / AppliedWithOmissions。
};
```

DockTree 使用分割节点和 tab 组表达结构，不保留一份字段列表再拼接字符串供最终提交时第一次解析。`ValidatedLayout` 的构造只由解析/验证函数完成：检查版本、槽位引用、重复实例、比例范围、树结构与 provider 约束。

持久 `ViewRestoreKey` 用来判断不同布局是否指向同一可复用视图。运行中 ViewId 的分配和重用有自己的规则。若现有 PaneId 已是满足此语义的稳定身份，可以直接复用其编码；不能为同一含义堆叠两种 ID，也不能把可重用容器 index 序列化后当永久身份。

`SaveLayout { DockLayout layout; }` 和 `RenameLayout { LayoutId id; std::string label; }` 是明确命令值，不附带 pending/result。存储完成返回 LayoutCommitReceipt，包含稳定布局身份、已发布版本和实际保证；偏好写入与目录刷新分别返回 PreferenceWriteResult、CatalogRefreshResult。它们都属于 M12，组合为上层呈现报告，但不能相互覆盖成功事实。命令协调归 M15，纯值与具体磁盘适配按第 14 章分 target。

### 11.3 采用非破坏性恢复，而不是“先关全部窗口”

布局只调整视图组织。匹配已有 view_key/type 的视图可复用，保持它当前绑定的内容；没有匹配时创建未绑定视图或明确的缺失扩展占位项。恢复内容绑定只在用户请求“恢复会话”时通过 OpenAssetOperation 进行，不由 ApplyLayout 暗中执行。

原布局之外的额外视图保持；已有未保存内容不被覆盖。多个同类型视图不能随意拿“找到的第一个”复用。身份映射不明确时创建新视图或报告遗漏，比重绑另一份用户内容更安全。

### 11.4 ApplyLayout 的严格阶段

| 阶段 | 输入 / 输出 | 可观察副作用 |
|---|---|---|
| Parse & Validate | bytes → ValidatedLayout | 无 UI/内容修改 |
| Resolve | 布局 + registry snapshot + 当前 view 快照 → LayoutPlan | 仅计算；固定 provider 版本 |
| Prepare | LayoutPlan → DetachedView / PreparedViewState / PreparedMountBatch | 不修改活动树；可持有候选资源 |
| Settle interaction | 明确结束/取消需重绑定视图的手势 | 可能产生一次作者编辑，单独报告 |
| Commit structure | PreparedLayoutApply → 已挂载结构 | 单个 Host 修改阶段；不调用任意用户可失败代码 |
| Publish & report | 生成 LayoutApplyReport | 焦点/可见通知；回调里的变更进入下一批次 |
| Persist preference | 可选记住所选 LayoutId | 独立结果；失败不撤销已应用布局 |

常规 provider 缺失或准备失败，按已确定的“保留/占位/跳过”规则进入报告；格式不合法在第一阶段返回失败，不接触窗口。内部严重故障导致只完成部分操作时必须返回精确报告，不能假称整体无变化。

**不承诺对任意插件副作用做自动回滚。** 可进入结构提交的 provider 必须遵守无副作用准备与受限采用契约；违背契约的扩展不应被当作可事务恢复的合法实现。

### 11.5 用稳定 LayoutId 消除重命名事务

建议持久化形式为 `layouts/<LayoutId>.layout`，文件内存 label。RenameLayout 只更新该记录的 label；偏好引用 LayoutId，因此不需要同步重命名文件与改写 settings。

SaveLayout 返回 LayoutCommitReceipt；选择布局后的 PreferenceWriteResult、目录刷新后的 CatalogRefreshResult 独立。目录刷新失败只令目录显示过期，不让已保存文件变成“未保存”。

删除活动布局后，偏好中的引用可能暂时悬空；加载侧必须把不存在的 LayoutId 当成可诊断、可回退的偏好，而不是启动致命失败。也可尽力清除偏好，但它不是删除成功的前置或后置原子保证。只在真正存在必须共同提交的元数据时再选用单记录/日志方案。

### 11.6 旧工作区格式迁移

提供单独迁移器：解析旧 WorkspaceData/PaneState，将布局组织、视图配置、内容 locator 和偏好拆开。已识别的内容 locator 进入 RecoveryManifest；未知 payload 连同原 schema/type 保留，不执行未知命令。

第一次迁移先写新文件，再记录迁移完成；保留旧文件作为备份/只读来源，禁止两个格式同时成为写入真相。新产品入口切换后只写新格式。缺失插件或迁移不完整要报告，不能把旧 opaque 字节解释成任意当前 C++ 类型。

<a id="commands-extensions"></a>
## 12. 替代类型设计七：命令、扩展与代码生命期

### 12.1 命令只是应用入口，不是领域模型的存储格式

```cpp
struct CommandDescriptor {
    CommandId id;
    std::string label;
    MenuPlacement menu;
    Shortcut shortcut;
};
struct CommandState {
    CommandAvailability availability;
    CheckState checked;
    std::string disabled_reason;
};
struct AcceptedOperation { OperationRef operation; };
struct ImmediateCompletion {};
using DispatchReceipt = std::variant<ImmediateCompletion, AcceptedOperation>;

struct CommandEntry {
    CommandDescriptor descriptor;
    CodeLease code;  // 回调销毁完成后才允许释放。
    std::function<CommandState(const CommandQuery&)> query;
    std::function<Result<DispatchReceipt, CommandFailure>(const CommandInvocation&)> execute;
};
```

CommandQuery 是已解析目标的只读查询输入，不包含 EditorContext。CommandInvocation 包含确定的目标身份与调用策略；执行仍需重新验证可用性，不能信任此前菜单绘制时的查询结果。

command 的 OperationRef 仅用于界面跟踪/路由，不能作为所有领域结果的无类型容器。需要程序化等待保存的调用者使用 SaveId/SaveOutcome；现有任务展示可以关联 TaskId。同步完成的命令不必平白生成异步 operation。

### 12.2 目标身份与内容版本的绑定策略不能混为一个 revision

用户从某个视图触发命令时，默认固定那个 ViewId/SessionId，入队后焦点变化不改目标。删除并复用槽位时旧 token 必须失效。

具体内容规则由命令定义：Save 在执行准入时捕获该 Session 的当前已提交状态；DeleteSelection 可以固定收到命令时的对象集合并验证；某些精确操作必须固定 ContentStamp。不能用一个“revision 必须完全相等”的通用规则阻止所有合法操作，也不能把零值表示“随便用当前目标”。

RegistryBinding 是 M13 中表达注册选择的封闭策略值，明确区分 CurrentRegistration 与 PinnedRegistration。普通已排队命令默认固定接受它的 entry/版本；注册更新后，不应偷偷调用同名但语义不同的新实现。

### 12.3 CommandRegistrySnapshot 的生命期和发布规则

调用方固定一个不可变 CommandRegistrySnapshot，snapshot 中 entry 同时持有闭包和 CodeLease。query 与 execute 一视同仁。所有注册替换先进入待发布列表，在 owner 批次结束后一次性发布新快照；当前批次继续使用旧快照。

命令集合具体使用 CommandRegistrySnapshot；完整扩展批次使用 ExtensionSnapshot。两者都组合其自身 entry 与版本，不增加一个允许访问全系统的通用 Registry 基类。这样同时解决“当前回调被销毁”和“外层 span 后续迭代失效”。不需要每次鼠标移动复制整个注册表；快照可以共享不可变记录，变更时构造新版本。

只读参数并不能阻止闭包修改自己捕获的对象。查询无副作用是提供者契约；模块装配不向 query 注入写能力，registry 发布门也拒绝即时替换。仍应测试恶意/误用式重入，不能声称 const 或接口拆分构成同进程安全沙箱。

### 12.4 扩展描述、注册和实例分开

| 层次 | 例子 | 拥有什么 |
|---|---|---|
| 描述 | SessionKindDescriptor、CommandDescriptor、字段 schema | 可读取的数据描述和版本 |
| 可执行注册 | SessionFactoryRegistration、ViewFactoryRegistration、CommandEntry | typed 入口、受限依赖、CodeLease |
| 活实例 | SceneSession、SceneView、SceneEncodeJob | 业务状态/资源，并保活其所需代码 |
| 发布集合 | ExtensionSnapshot | 某一批可用注册的不可变集合 |

异步 Session 工厂应具有两个显式阶段：SessionLoadJob 是 M14 规定的加载角色，后台产物 PreparedSessionData 拥有解码数据和代码保活；其具体 Scene/Material 实现位于 M07。owner 阶段在预留 SessionId 下构造 IEditSession 派生对象，并准备对应角色适配器。动态边界可以通过一个仅提供 owner 构造的窄 prepared-data 接口或有类型的注册回调擦除格式，不公开 any_cast/getService，也不在各模块传播无语义 void*。应用用例只有在两阶段均完成后发布。

SessionStore 的实际槽记录必须保活派生对象代码，例如先声明 CodeLease，再声明 unique_ptr&lt;IEditSession&gt;；释放时会话及其历史/快照析构完成，随后才释放代码。此 lease 复用低层插件生存期机制，不使 M03 反向依赖 M14 的扩展发布实现。

插件仍通过 typed domain exports 提供这些条目，不增加一个拥有 getService/stringInvoke 的万能 IPlugin。不将 field schema、系统运行入口和 UI 工厂塞到一个“metadata” target 后向所有运行消费者公开。

### 12.5 CodeLease 到底保护什么

CodeLease 可以封装现有 shared_ptr&lt;const void&gt; 等保活机制；它是基础设施语义，不因为内部类型擦除就算设计偷懒。它必须覆盖：正在执行的函数、闭包析构、插件对象的虚析构、插件定义的错误对象/快照析构、分配器和自定义 deleter，以及尚未清理的异步结果。[S13]

包含插件对象的结构中，声明 lease 在前、对象在后，或使用显式清理顺序，保证对象先销毁、代码后释放。单独复制函数指针并不能保活 DLL；仅保活回调 body 而忘记结果析构也不够。

第一版只要求项目切换/正常关闭时安全释放，不承诺任意时刻热卸载。新设计不得为了实现本文而额外启动一个复杂热更新框架。

### 12.6 SDK 承诺与模块边界

本轮默认提供“匹配工具链、构建配置和 SDK 契约的 C++ 插件”能力，分别记录 InterfaceVersion、SdkBuildFingerprint 与实际 capability。Fingerprint 用于识别构建输入，不等价于跨编译器 ABI 兼容证明。

运行时插件只依赖运行扩展契约；Editor UI 头变化不应自动进入其依赖闭包。Editor 扩展依赖 view/session/command 的稳定公开契约，而不是安装 app 私有 Impl。

确实需要跨工具链稳定 ABI 时，再设计受版本管理的 C ABI 或序列化边界；那是独立工程，不是通过把 std::function 改成函数指针就已完成。

<a id="modules"></a>
## 13. 明确的子系统框架：16 个责任域及其内部类型

这里的“模块”首先是责任域。它不等同于一个 DLL、一个 C++20 module 或一份安装包。后续 target 表将区分 API、实现与适配，避免出现逻辑分层画得很好、实际仍循环链接的情况。

### 13.1 子系统总览

| 编号 | 子系统 | 核心类型 | 独立拥有的事实 |
|---|---|---|---|
| M01 | 最小公共支撑 | typed key 原语、OperationRef、Diagnostic | 仅通用值/基础机制，不拥有业务 |
| M02 | 编辑历史 | EditHistory、EditOperation、PreparedEdit、StateId | 历史状态图、游标、准备/提交规则 |
| M03 | 编辑会话与身份 | IEditSession、SessionStore、SessionState、SessionKey、ClosePermit | 打开内容集合、代际、来源与关闭准入 |
| M04 | Scene 作者模型 | SceneSession、SceneSource、SceneSnapshot、SceneEditBatch | 场景作者数据与合法编辑 |
| M05 | Material 作者模型 | MaterialSession、MaterialSource、MaterialSnapshot、MaterialEditBatch | 材质作者数据与合法编辑 |
| M06 | 保存编排 | SaveService、SaveOperation、ISaveSource、IEncodeJob、WriteLane | 保存准入、飞行中工作、提交顺序、终态 |
| M07 | 项目存储与领域持久化适配 | ProjectArtifactStore、SceneSaveSource、SceneCodec、MaterialCodec、SaveAs 操作 | 实际 IO；领域编码/身份映射的适配状态 |
| M08 | 场景运行会话 | RunStore、RunSession、RunController、RunProvenance | 每次独立运行的控制、来源和结果 |
| M09 | 投影与预览 | SceneProjection、ScenePresentationHub、ViewportPresentation、HighlightRenderer、MaterialPreviewStore | 派生渲染资源、版本和退休状态 |
| M10 | 编辑交互 | SceneInteractionGroup、SceneSelection、Gesture、MaterialInteraction | 选择、交互事务和跨视图联动 |
| M11 | UI 视图与桌面宿主 | SceneView、MaterialView、IViewHost、ViewHost、DesktopShell、DetachedView | 活动 UI 树、视图局部状态、平台输入输出 |
| M12 | 布局与恢复数据 | DockLayout、LayoutCatalog、LayoutPlan、RecoveryManifest、UserPreferences | 布局/恢复/偏好各自的数据和持久化结果 |
| M13 | 命令 | CommandDescriptor、CommandEntry、CommandQuery、DispatchReceipt | 命令入口、目标绑定与查询/准入结果 |
| M14 | 编辑扩展 | ExtensionSnapshot、ExtensionPublisher、typed FactoryRegistration | 编辑扩展发布集合和其代码寿命 |
| M15 | 应用用例 | OpenAssetOperation、ReloadSessionOperation、CloseSessionsOperation、SaveAllOperation、ApplyLayoutOperation | 跨模块产品流程与用户决策 |
| M16 | 装配、工具集成和验证 | EditorApplication、SceneToolModule、MaterialToolModule、测试适配 | 应用生命周期、依赖接线、构建验收 |

`ScenePresentationHub` 和 `MaterialPreviewStore` 是前文 PresentationHub 逻辑分组的具体实现，不增加一个能访问所有工具模型的全局 PreviewManager。

<a id="m01"></a>
### 13.2 M01 — 最小公共支撑，严格防止长成新的 common

**允许内容：** 复用已有 expected、字节 owner、基础 typed ID 模板；供应用记录的 OperationRef/Diagnostic。一个领域的错误、SceneSnapshot、ViewInfo 不因为“多个文件使用”就进入这里。

**不允许：** 注册表、服务定位、全局 Context、SessionStore 实现、可写业务状态。CodeLease 优先封装/使用已有底层插件寿命机制，而不是将 editor 扩展加载器作为所有基础值的依赖。

**类型关系：** 值类型、组合，不建立 FoundationObject 继承树。若这些设施已有合适 target，直接复用；新 target 可以仅有少量头或不必新增编译单元。

**对应问题：** A03、T07、B01。**检查：** 任何 M01 公开头不得 include Scene、Pane、DesktopShell 或 EditorApplication。

<a id="m02"></a>
### 13.3 M02 — 编辑历史，复用机制但迁出持久化职责

EditHistory 继续拥有历史条目、当前状态、撤销重做位置、预算和提交阶段。EditOperation/PreparedEdit 保留真实的异构行为接口，其派生编辑操作归具体领域，不放进历史模块。

目标从该模块迁出 SaveTicket 和目标相关的保存准入/完成；它只公开足以识别当前内容状态的快照。Session 的 PersistenceCheckpoint 是保存基线唯一事实来源。已有实现的迁移应先做等价测试，不能用重命名顺带更换历史算法。[S34] [S35] [S36]

**允许依赖：** 基础对象/内存/错误工具。**禁止依赖：** UI、文件系统、运行时调度、项目存储、具体 Scene/Material 模型。

**可独立验收：** 伪模型执行/undo/redo、准备失败原状态不变、回调重入拒绝、预算和回收边界。

<a id="m03"></a>
### 13.4 M03 — 会话集合与生命周期契约

SessionStore 只拥有与发布异构 Session。SessionState 是被具体 Session 组合的值/机制，包含来源与保存基线及 gate；不复制 History 当前状态。SessionAccess&lt;T&gt; 是普通业务访问入口，生命周期采用/删除不向它开放。

ClosePermit 与 BindingChangePermit 是不同操作的准入责任：前者供内容关闭，后者供 Save As 重绑定；都不执行 IO。来源描述仅是持久化地址/版本值，不包含存储实现。

**允许依赖：** M01、M02、现有资产身份/模式数据。**禁止依赖：** M04/M05 的具体实现、M06 保存调度、M11 UI、M15 用例和 app。

**类型关系：** IEditSession 是唯一核心异构会话接口；Store 组合 owning pointers，具体 Session 组合 SessionState。允许短小的类型校验/代际访问辅助，但不提供 queryInterface(void*) 万能能力查询。

<a id="m04"></a>
### 13.5 M04 — Scene 作者域

SceneSession 是公开领域入口；SceneSource 是作者数据；SceneSnapshot 是真正冻结的捕获；SceneEditBatch/具体 EditOperation 表达合法修改。场景树查询、字段编辑、对象创建/删除和作者引用校验属于这里。

**不放入：** Pane、Outliner、视口相机、GPU 高亮、运行副本、文件选择器、异步保存生命周期。模型资源导入的“读取/解码”不属于领域；得到 PreparedModelInsertion 后，由这里验证并一次编辑采用。

**允许依赖：** M01/M02/M03，现有数学、world/ECS/schema 和纯场景数据。若现有 engine/scene 头把数据类型与渲染运行时捆绑，必须拆出低层数据契约或专用头，而不是公开链接整个 engine 汇总 target。

**独立验收：** 无窗口、无 GPU、无 ProjectStorage 的场景编辑和快照测试；未创建任何视图也能编辑、undo 和 capture。

<a id="m05"></a>
### 13.6 M05 — Material 作者域

结构与 Scene 平行但模型不同。MaterialSource 管理图/参数和资源引用；MaterialEditBatch 处理连接、节点、参数合法性；MaterialSnapshot 给编码或编译使用。

**不放入：** shader 编译执行器、GPU 预览场景、Asset picker、菜单路由。编译器需要哪些输入由 snapshot/compile settings 描述，不需要完整 MaterialSession。

**允许依赖：** M01/M02/M03、纯 material/graph/schema 数据。**禁止依赖：** SceneSession、SceneEditor、desktop 或 engine render 实现。

**验收目的：** 证明 IEditSession、历史、保存角色确实能支持第二种工具，而不是一个暗藏 Scene 假设的框架。

<a id="m06"></a>
### 13.7 M06 — 保存编排

SaveService 拥有操作集合，WriteLane 对规范化目标排序。ISaveSource 和 IEncodeJob 是跨领域所需的窄接口；IArtifactStore 是可注入副作用边界。该模块只看到冻结输入、编码产物、目标与回执，不解析 ScenePackage。

**允许依赖：** M01/M03、既有 ExecutionRuntime（实现私有依赖）及字节/任务基础。**禁止依赖：** SceneSession/MaterialSession 的具体头、GUI、文件选择器、具体文件系统后端。

**状态约束：** 保存操作状态在这里；Session 中只有持久化基线，界面状态由二者组合查询。`SaveAllOperation` 不在这里，因为它包含选择哪些会话和用户决策；普通单次保存不需要了解整个应用。

<a id="m07"></a>
### 13.8 M07 — IO 与领域适配：同一责任域中的多个真实 target

划为 `project_io`、`scene_persistence`、`material_persistence`、需要时的 `asset_import`。不能为了“适配层”这个名字把它们链接成一个万能 IO 包。

ProjectArtifactStore 实现 IArtifactStore，调用现有项目存储和平台文件能力；SceneCodec/MaterialCodec 做格式映射；SceneSaveSource/MaterialSaveSource 组合具体 SessionAccess 与 codec；SceneSaveAsOperation/MaterialSaveAsOperation 拥有各自显式的重绑定候选。

**允许依赖：** 对应具体领域、M06、既有项目/资源/执行能力。**禁止依赖：** ViewHost、桌面窗口、app Impl。失败返回值供 M15 决策，不能自行调用 reportMenuFailure。

**持有方式：** SaveSource 不拥有 Session，只持 typed key 和访问能力；适配器寿命由 InstalledSession 的注册关系管理。编码 job 拥有独立快照和代码保活，不借用 live Session。

<a id="m08"></a>
### 13.9 M08 — 运行会话

RunController 将 SceneSnapshot 转成运行输入，RunStore 持有 RunSession。RunSession 内部持有引擎实例 lease、暂停/步进/停止状态与来源。底层 WorldMaterializer、SceneRuntime、Simulation 仍由 engine 实现。

拆成轻量 `scene_execution_api`（RunId、配置、结果）和 `scene_execution`（实际实现），以免只需要 RunId 的交互代码被迫链接整个仿真。

**允许依赖：** M03/M04、既有 engine 场景运行时/执行器。**禁止依赖：** SceneView、布局、命令注册、EditorApplication。运行开始/停止是动作，菜单按钮是 M16 的绑定。

<a id="m09"></a>
### 13.10 M09 — 派生投影与呈现

SceneProjection 从作者快照/变更流生成渲染可用状态；ScenePresentationHub 复用同一工作副本的投影；ViewportPresentation 按视图分配相机和输出目标；HighlightRenderer 独立维护准备/接受/退休。MaterialPreviewStore 处理材质编译与 GPU 预览。

**允许依赖：** 对应作者数据契约、现有场景/渲染/资源后端。**禁止依赖：** 保存服务、工作区存储、ViewHost 内部 owning 容器、菜单。运行呈现通过明确的 engine render source 接入，不反向拥有 RunStore。

**关键边界：** 这个模块可以落后于作者状态，但必须带版本；不能通过直接修改作者 Registry 来“让预览看起来同步”。暂时没有视图时可释放派生资源，而 Session 继续存在。

<a id="m10"></a>
### 13.11 M10 — 交互而非 UI 宿主

SceneInteractionGroup 组合选择与 gesture；MaterialInteraction 保存图编辑交互。交互算法可以与平台事件适配分开，因此不应需要具体 OS Window 或 GPU。Scene 的拾取结果由 M09 提供稳定对象候选，交互决定选中什么，再通过 M04 提交作者修改。

**允许依赖：** 对应领域 API、M08 的轻量 RunId/对象引用契约。**禁止依赖：** DesktopShell、项目 IO、保存状态、插件加载器。

**组合关系：** 视图引用一个交互组；组不继承 Pane，不拥有 Session。材质不为了复用鼠标输入而继承 SceneInteractionGroup。

<a id="m11"></a>
### 13.12 M11 — 视图 API、工具视图与桌面实现

这里必须分成 `view_api`、`desktop_shell`、`scene_ui`、`material_ui`，以及必要的 `editor_widgets`。view_api 包含 ViewId、DetachedView、ViewInfo、IViewHost/正常 ViewRequests 契约；desktop_shell 才链接平台窗口、UI Root 和实际呈现。

**IViewHost 是一个允许的窄多态边界。** ViewHost 与 FakeViewHost 实现相同的结构修改契约，使布局/关闭用例不依赖真实桌面。普通工具只拿 ViewRequests；用例层才注入 IViewHost/其生命周期访问权限。具体实现声明为 `ViewHost final : public IViewHost`；不让用例反向依赖 desktop_shell 的实现头。

SceneView/MaterialView 等实际工具 UI 依赖自身领域、交互与预览；它们不包含 M15 用例实现。菜单与工具装配由 M16 将 typed requests 接起来。

**禁止依赖：** view_api 不依赖 layout 实现、commands 实现、具体 Scene/Material；desktop_shell 不依赖场景作者模型；scene_ui 与 material_ui 不互相依赖。

<a id="m12"></a>
### 13.13 M12 — 布局模型与工作区持久化

`layout_model` 包含 DockLayout、验证器与纯 LayoutPlan 计算；`recovery_model` 包含恢复会话描述；`workspace_store` 分别提供 layout/catalog/preferences/recovery 的读写。它们可以共享底层原子文件替换工具，但不共享一个 action 驱动的 WorkspaceData。

**允许依赖：** M01、view_api 的纯身份/视图状态契约，recovery_model 可引用 M03 的持久化来源描述；workspace_store 使用实际磁盘适配。**禁止依赖：** SceneSession、RunStore、EditorContext。

应用布局调用 Host、请求用户结束交互及保存偏好等组合行为归 M15，纯 parse/plan 不需要窗口。

<a id="m13"></a>
### 13.14 M13 — 命令查询与分发契约

CommandDescriptor/State/Invocation/Receipt 在这里；CommandEntry 组合 query、execute 与 CodeLease。命令目标使用 Session/View/Application 等已有应用身份，不通过引入具体 Scene 类型构造全局命令 variant。

**允许依赖：** M01、M03 的轻量会话身份和 view_api 的身份。**禁止依赖：** Scene/Material 具体模型、布局存储、desktop_shell、EditorApplication。

实际“Save 当前场景”“Pause 当前运行”的 handler 在 M16 工具装配中，捕获对应窄接口。M13 不通过巨大 switch 实现全产品业务。

<a id="m14"></a>
### 13.15 M14 — typed 编辑扩展的发布边界

`extension_api` 声明 SessionFactory、ViewFactory、Command 与角色注册契约；`editor_extensions` 使用现有插件加载结果并发布不可变 ExtensionSnapshot。Loader 不需要知道 SceneSession 的私有成员。

**允许依赖：** M03/M06/M11/M13 的公开契约和既有低层插件寿命机制。**禁止依赖：** app、workflows 实现、Scene/Material 私有头。通用 engine 的运行插件仍留在 engine，不反向链接这一 Editor 集合。

对打开内容的异步工厂，应分开 worker 解码与 owner 构造：`SessionLoadJob` 拥有读入参数与代码寿命，结果是 `PreparedSessionData`；在 owner 阶段创建拥有正确线程契约的具体 Session，再准备角色适配器和发布。不能在任意 worker 创建捕获 owner-thread 身份的 EditHistory 后直接搬到 UI 线程。

<a id="m15"></a>
### 13.16 M15 — 产品用例，不是另一个万能 Controller

每种长期流程使用具体记录：OpenAssetOperation、ReloadSessionOperation、CloseSessionsOperation、SaveAllOperation、ApplyLayoutOperation。它们可以在同一 target 内，但每个记录拥有自己的状态和结果，不能共享可写 ApplicationState。

OpenAssetOperation 只通过 extension factory、SessionStore 生命周期能力和 ViewRequests/Host 协作，不 include SceneSession.hpp。Scene 的特殊编解码和 Save As 在 M07，通过已注册 typed 能力组合。

CloseSessionsOperation 拥有被审阅的参与者、其内容戳、用户选择和 ClosePermits；不拥有 Session 本体。ApplyLayoutOperation 拥有 LayoutPlan、候选 views 和最终报告，不去修改作者模型字段。

**允许依赖：** M03/M06/M11/M12/M13/M14 的稳定契约。**禁止依赖：** desktop_shell 具体实现、Scene/Material 私有模型/Impl、bootstrap。

<a id="m16"></a>
### 13.17 M16 — 应用装配、工具接线与工程验证

SceneToolModule 将 Scene factory、SaveSource、视图工厂、组件编辑器和命令绑定到已有服务；MaterialToolModule 平行实现。它们可以知道很多具体类型，因为职责就是装配；但不能成为业务普遍传递的参数，也不能以 getters 暴露完整依赖图。

EditorApplication 创建/持有执行器、存储、Store、Host、用例服务与扩展快照，并执行明确的驱动阶段。它不持有每个操作的 candidate_source、pending_result 或关闭选择字段。

test_support、native contract tests、desktop/gpu tests 和 installed consumers 独立组织。产品 target 不依赖测试 target，不在公开头新增仅为测试使用的可写后门。

<a id="target-graph"></a>
## 14. 具体编译依赖与目录框架

### 14.1 target 依赖表：以下箭头均为“使用方 → 被使用方”

名称为建议逻辑名，实际 CMake 可统一加 `lux_editor_` 前缀并提供 alias。表中列出 Editor 内部依赖；已有 engine/core/asset/math/render/process/platform 依赖在各卡片中限定范围。

| target | 所属 | 允许的直接 Editor 依赖 |
|---|---|---|
| editor_contracts | M01 | 无 |
| edit_history | M02 | editor_contracts |
| edit_sessions | M03 | editor_contracts、edit_history |
| scene_model | M04 | edit_sessions、edit_history |
| material_model | M05 | edit_sessions、edit_history |
| editor_persistence | M06 | editor_contracts、edit_sessions |
| project_io | M07 | editor_persistence |
| scene_persistence | M07 | scene_model、editor_persistence |
| material_persistence | M07 | material_model、editor_persistence |
| scene_execution_api | M08 | edit_sessions |
| scene_execution | M08 | scene_execution_api、scene_model |
| scene_projection | M09 | scene_model |
| material_preview | M09 | material_model |
| scene_interaction | M10 | scene_model、scene_execution_api |
| material_interaction | M10 | material_model |
| view_api | M11 | editor_contracts |
| editor_widgets | M11 | view_api |
| scene_ui | M11 | view_api、editor_widgets、scene_model、scene_interaction、scene_projection、scene_execution_api |
| material_ui | M11 | view_api、editor_widgets、material_model、material_interaction、material_preview |
| layout_model | M12 | view_api |
| recovery_model | M12 | view_api、edit_sessions |
| workspace_store | M12 | layout_model、recovery_model、editor_contracts |
| desktop_shell | M11 | view_api、layout_model |
| editor_commands | M13 | editor_contracts、edit_sessions、view_api |
| extension_api | M14 | edit_sessions、editor_persistence、view_api、editor_commands |
| editor_extensions | M14 | extension_api |
| editor_workflows | M15 | edit_sessions、editor_persistence、view_api、layout_model、recovery_model、workspace_store、editor_commands、extension_api |
| scene_tool | M16 | scene_model、scene_persistence、scene_execution、scene_projection、scene_interaction、scene_ui、editor_workflows、extension_api |
| material_tool | M16 | material_model、material_persistence、material_preview、material_interaction、material_ui、editor_workflows、extension_api |
| editor_bootstrap | M16 | desktop_shell、editor_workflows、editor_extensions、project_io、scene_tool、material_tool |
| lux_editor | M16 | editor_bootstrap |

这是 **31 个候选 target 节点**，不意味着 31 个 DLL。header-only API 可为 INTERFACE target；稳定边界相同、拆分无收益的内部实现可合并编译，但合并不能放松列出的 include 与禁止依赖规则。一个大型 `editor_everything` 汇总 target 只能用于最终产品，不可作为领域库的依赖。

**重要例外：** 运行时代码依赖与 host 工具/代码生成依赖分开检查。由 generator 生成头，不等于运行库应 PUBLIC 链接 generator；反过来，不能把真正的跨域头依赖伪装为“只是生成需要”。

### 14.2 依赖图中的关键断边

```text
                         editor_bootstrap / tool assembly
                         /       |       |             \
                  workflows   extensions  desktop      concrete tools
                     |             |         |          /  |   \
                  stable API     typed API  view API   UI  IO  runtime/preview
                     |                                   \ | /
                  sessions / persistence                domain models
                          \                               /
                                 history + contracts
```

从 SceneDomain 到 UI 不存在箭头；从 engine SceneRuntime 到 Editor 不存在箭头；从通用命令模块到 SceneSaveSource 不存在箭头。产品层可以同时依赖它们，领域层不反向知道它们。

模块间存在运行期调用回调并不自动形成编译环。例如 workflows 调用 IViewHost，desktop_shell 实现 IViewHost；两者都依赖 view_api，由 bootstrap 将具体对象注入。不能让 workflows 直接 include DesktopShell.hpp，再说“概念上已经反转依赖”。

### 14.3 建议目录树

```text
editor/
  contracts/                 # M01：少量跨域值和基础约定
  history/                   # M02：历史机制与协议
  sessions/                  # M03：身份、Store、生命周期、来源基线
  persistence/               # M06：保存与目标发布编排
  commands/                  # M13：描述、查询、调用、回执
  extensions/
    api/                     # M14：typed contracts
    host/                    # M14：注册发布与加载适配
  views/
    api/                     # M11：ViewId、DetachedView、IViewHost
    widgets/                 # M11：真正通用的控件
  desktop/                   # M11：ViewHost、Root、平台输入/呈现
  workspace/
    layout/                  # M12：模型、解析与规划
    recovery/                # M12：恢复描述，不等于自动保存
    storage/                 # M12：布局/偏好/恢复的具体持久化
  adapters/
    project_io/              # M07：现有项目/文件存储适配
  workflows/                 # M15：open/reload/close/save_all/apply_layout
  tools/
    scene/
      model/                 # M04
      persistence/           # M07：codec、SaveSource、SaveAs
      execution/
        api/                 # M08：轻量 Run 契约
        src/                 # M08：RunSession/Controller
      projection/            # M09
      interaction/           # M10
      ui/                    # M11：SceneView、Outliner、Inspector 适配
      integration/           # M16：工厂/命令/能力接线
    material/
      model/                 # M05
      persistence/           # M07
      preview/               # M09
      interaction/           # M10
      ui/                    # M11
      integration/           # M16
  bootstrap/                 # M16：应用入口、装配与退出次序
  tests/
    support/
    domain/
    workflows/
    desktop/
    gpu/
    installed/
```

每个需要公开边界的模块使用 `include/` 与 `src/`。仅在确有多个 target 必须共用非安装契约时设置明确命名的 `internal/` target；不继续默认开放所有 sibling 的 sinclude/pinclude。一个私有头若被第三个模块需要，应先审查职责，而不是新增 include_directory。

### 14.4 CMake 的具体要求

target 的公开头出现的类型和必须传播的链接需求应如实表达。不能仅将 PUBLIC 改为 PRIVATE 就称为解耦；静态库的必要链接闭包仍需正确传播。依赖的检查同时看 LINK_LIBRARIES、INTERFACE_LINK_LIBRARIES、公开头和实际 include，不只读一个架构属性。[R02] [R03]

配置时/CI 校验本文白名单，反向边必须失败；每个公开头独立编译；最小 Scene/Material domain consumer 不链接窗口、GPU、ProjectStorage；installed consumer 只使用 install prefix，不能从源码树借私有头。

禁止把 `BUILD_TESTING` 下所有测试共同绑定到特定本机 LLVM/PowerShell 路径。平台集成测试可以要求平台工具，但普通领域/流程 CTest 不应因此无法配置。

### 14.5 尺度控制：何时允许合并 target，何时绝不能合并责任

可以将没有独立编译收益的辅助值头并入其模块；可以暂时让 scene_persistence 与 scene_tool 编进同一静态库，但必须保留禁止反向 include 的检查。不能把 scene_model 与 scene_ui 合并后允许模型任意 include Pane，这会失去最重要的隔离证据。

因此 target 数量不是完成指标。最小可配置构建图、独立安装的公开契约和实际禁止依赖，才是判断这个划分是否有效的依据。

<a id="old-new"></a>
## 15. 现有文件/系统如何拆分，哪些应删除

| 现有区域 | 目标去向 | 拆分动作 | 最终删除条件 |
|---|---|---|---|
| EditorImpl.hpp / Editor.cpp | M15 各用例 + M16 app 驱动 | 分离流程状态 owner；根仅持有服务和生命周期 | 根不再保存各流程 candidate/pending/decision 字段 |
| EditorMenu.cpp | M13 分发协议 + M16 handlers + M11 菜单呈现 | 分开 query、admission、completion 和具体业务 | 无 query/execute 共用可变 Command phase 协议 |
| EditorWorkspace*.cpp/hpp | M12 数据/存储 + M15 ApplyLayout | 拆布局/目录/偏好/恢复；引入纯验证与计划 | WorkspaceRequest/WorkspaceData 不再是公共协议 |
| EditorContext | M16 装配 + 各模块窄依赖 | 逐项迁出服务使用和管理权限 | 新旧业务均不再接收万能 Context，删除旧类型 |
| PaneManager | M11 ViewHost + M14 注册 + M15 复用策略 | 分离 owner、factory、reuse、show/focus | 不存在返回“可能已注册引用”的工厂 |
| SceneEditor / SceneEditorImpl | M04/M07/M08/M09/M10/M11 | 编辑源、持久化、运行、投影、交互、UI 各归其主 | SceneView 不拥有编辑历史和保存/运行操作 |
| MaterialEditor / MaterialAssets | M05/M07/M09/M10/M11 | 作者模型、编码、编译预览、交互、UI 分离 | MaterialSession 可脱离任何 Pane 测试 |
| EditHistory / EditOperation | M02 + 领域派生操作 | 保留算法，迁出保存票据，删除对窗口依赖 | 保存基线只由 Session 组合的 checkpoint 决定 |
| TAssetSave | M06 + M07 | 保留可用调度/存储逻辑，改显式 FrozenSave/编码结果 | const 输入不再隐藏额外可写输出 |
| ui/TaskPane、Asset picker 等 | M11 widgets 或 M16 对应工具 UI | 按真实领域依赖分开；不是全部都叫 common | 简单控件消费者不继承场景渲染闭包 |
| metadata 中的执行注册 | M14 extension_api | 描述和执行角色语义分明，保留 typed exports | 纯描述消费者无须包含 UI 工厂 |
| SceneRuntime valid/invalid | engine runtime 清晰控制 API + M08 | 拆时间控制/实例存在/退休；保留单一驱动 | 没有同一实例双 tick 或独立 EditorRuntime 真相 |
| UI Pane/Root 构造与注册 | M11 底层 UI 配合 | 支持脱离树准备、受限挂载和安全点退休 | 工厂失败不改变活动 UI；无双 owner |
| 现有 ExecutionRuntime/stdexec | 原模块复用 | 调整输入冻结与完成采用位置，不新建线程池 | 新领域逻辑不靠捕获窗口 this 接收完成 |
| CMake/测试/SDK 指纹 | M16 + M14 | 依赖白名单、测试分层、指纹语义与闭包拆分 | Editor PR 验证及独立 install consumer 可重复 |

重构时不能仅把旧 SceneEditor::Impl 重命名为 SceneSession。逐一检查成员：相机进 ViewState，run_source 进 RunSession，save 进 SaveOperation，highlight 程序进 HighlightRenderer，pending workspace 进 ApplyLayoutOperation，作者源与历史才进入 SceneSession。

旧 API 只可作为**单向兼容入口**调用新用例；新领域 API 禁止反过来调用旧 EditorContext/SceneEditor。兼容层必须有明确删除阶段，不允许同时双写旧字段和新 owner。

<a id="workflows"></a>
## 16. 跨子系统用例：让类型关系在实际操作中成立

下列流程规定的是产品行为和提交边界，不要求实现成一个通用工作流引擎。流程图中的记录由 M15 或特定领域操作拥有；被调用子系统仍拥有自己的状态。阶段推进使用具体函数和有类型的完成值，而不是共享一个可写 Context。

### 16.1 打开资产并显示：内容发布与视图显示不是同一个成功事实

**参与者：** M15 OpenAssetOperation；M14 固定 ExtensionSnapshot；M07 读取/解码；M03 SessionStore；M11 ViewHost；M16 工具接线。

```text
用户提交 OpenAssetIntent(address, reuse_policy, display_policy)
  → 固定本次的扩展快照，解析支持该内容的 SessionFactory
  → 按复用策略查已发布 Session；命中时不重新解码
  → 未命中：后台读取/解码 → PreparedSessionData
  → owner 线程构造候选 Session、SaveSource/HistoryActions 等角色
  → 准备发布槽位与可回退的角色注册
  → 发布 InstalledSession，取得 SessionId                 [内容提交]
  → 复用已有视图，或构造 DetachedView
  → 安全阶段 attach，绑定 SessionKey 并设置显示/聚焦       [视图提交]
  → OpenResult 同时描述内容和视图的结果
```

**默认失败策略明确选择：** 发布之前任何失败不留下新会话或注册项。内容已经发布而视图创建失败时，保留可再次显示的无视图会话，返回“内容已打开，显示失败”；不能返回一个无上下文的失败而让用户不知道内容仍在。已有视图不得因本次新视图失败而被删除。

接口结果可组合 `SessionOpened` 与 `ViewPresentationOutcome`，无需强制把两次提交伪装成一笔全局事务。若产品有“必须连窗口一起打开，否则什么也不要”的特殊操作，应显式使用不同策略，在内容未公开前准备视图和全部挂载资源，不能更改默认语义却沿用同名结果。

同时到达的相同资产打开请求由用例层按规范化地址和复用策略合并或排序。工厂本身不维护“当前唯一窗口”。独立副本策略生成不同 SessionId，并由保存的目标版本冲突规则保护它们，不能因为 AssetId 一样就共享可写历史。

**验收：** Q01、Q02、Q05、Q51。关联 A02/A03/A04/A08/T02。

### 16.2 重载：保留会话身份，一次性替换作者源和历史

ReloadSessionOperation 捕获 SessionId、当时 ContentStamp、BindingRevision 和来源。需要舍弃修改时进行明确审阅；读取、解码、校验都在旧内容仍然可观察的情况下准备。用户在准备期间继续修改，结果不能直接覆盖，应在采用前重新验证或请求新的确认。

候选历史在 owner 线程创建；成功采用时用新 HistoryId、初始状态及来源基线整体替换，随后发布一次重置通知。投影、选择和未完成任务根据新历史身份失效。失败保留旧内容、旧历史、选择以及来源；不使用 `clear(); load();` 作为重载实现。

旧保存可能已经落盘，这是与“旧完成值不可改写新会话基线”不同的事实：Reload 必须先处理同目标尚未发布的写入，或向用户明确采用哪个已确认的存储版本。不能只靠忽略迟到回调就声称解决了重载与保存的磁盘竞争。

**验收：** Q05、Q06、Q18、Q20。关联 T04/T06/N03/N12。

### 16.3 两个视图共同编辑：共享作者事实，不共享所有 UI 状态

SceneView V1 和 V2 均持有 SceneSession A 的 typed key；每个视图拥有自己的相机和输出目标。Outliner 与 V1 绑定交互组 G1，V2 可以使用 G1 或显式独立组 G2。

```text
V1 输入 → G1 形成 gesture 预览
  → gesture 结束，构造明确目标的 SceneEditBatch
  → SceneSession A 校验并准备
  → Source + History 一次提交到 S11
  → 通知携带 A/S11/ObservationVersion
  → G1/G2 校验选择，V1/V2 更新查询快照
  → SceneProjection(A) 追赶 S11，各自 Viewport 输出
```

V2 不再接收“一套命令再执行一次”，否则会重复修改历史。视图订阅的是变化事实；唯一领域操作只执行一次。两个不同手势竞争同一对象时，领域按前置状态/字段版本拒绝或使用显式合并策略；不能默认为最后帧写入获胜。

**验收：** Q02、Q09、Q26、Q30。关联 A02/P02/P08/N01/N04。

### 16.4 正常保存与继续编辑：内容版本、写入序号和观察版本分离

例：当前为 S10，向绑定 B3 的目标 T 发起保存 W1；W1 编码期间作者变为 S12，并再次保存 W2。

| 时刻 | 会话事实 | 保存服务事实 | 磁盘允许发生的动作 |
|---|---|---|---|
| 接受 W1 | 当前 S10，基线可能 S8 | 预留 T 的发布票据 #1；捕获 S10/B3 | 无 |
| 接受 W2 | 当前 S12 | 预留票据 #2；捕获 S12/B3 | 无 |
| W2 先编码完成 | 仍 S12 | #2 Ready；#1 Encoding | #2 不能越过尚未终结的 #1 |
| W1 发布完成 | 基线可更新为 S10；当前 S12 仍 dirty | #1 Committed，保留回执 | T 对应 #1 的内容 |
| W2 发布完成 | 基线 S12；若没有新编辑则 clean | #2 Committed | T 对应 #2 的内容 |

用户随后撤销回 S10 并再次保存 W3，W3 虽然内容 StateId 更“早”，却是最新保存意图，应排在 #2 之后。这证明不能拿 StateId.serial 充当文件发布序号。

若 W1 编码失败或取消，明确终结 #1 后 #2 可以推进；不得留一个永不释放的序号空洞。发布票据只在有界操作集合中存在；达到容量上限在准入阶段返回拒绝，不无限增长队列。独立 Session 对同目标的版本冲突必须进入明确的 Conflict 结果，而不能自动把另一工作副本的成功当作自己的前驱。

**验收：** Q11—Q14、Q17、Q18。关联 T04/T06/C02/P04/N05/N06。

### 16.5 Save As：有意选择一个较简单、但诚实的第一版契约

SceneSaveAsOperation 与 MaterialSaveAsOperation 各自拥有自己的候选内容/格式逻辑，共用 M06 的目标预留、编码产物提交和存储回执。

顺序为：验证目标 → 等待/处理该会话的既有保存 → 取得 BindingChangePermit → 捕获作者态 → 准备编码和保持历史的重绑定候选 → 发布新目标 → owner 采用新绑定 → 释放 permit → 结束。进入冻结前必须先解决可能需要用户长时间选择的路径问题。

失败发生在文件发布之前：当前来源、历史和基线不变。文件已发布、会话因应用终止等无法采用时：保留“新目标已写出”的终态信息，不能报告什么都没有发生。正常进程路径通过 permit 和全部预准备，排除发布后再进行可失败的模型重建。

不能把 Export Copy 当作 Save As 的隐式退化。无法保持作者身份与历史的格式必须在写入前拒绝重绑定，或让用户显式选择导出独立副本。

**验收：** Q15、Q16、Q17、Q43。关联 T06/P03/P04/N05/N12。

### 16.6 启动、暂停、步进、停止：控制运行，不控制作者源

RunController 捕获 A/S12 并返回 RunId R 的准入；准备成功后构造独立运行输入，在 engine owner 阶段创建实例。RunSession 保留 provenance=A/S12，即使 A 继续编辑也不变。

Pause 请求改变时间推进策略，维护/资源完成/发布继续发生。Step 请求返回或关联 StepTicket；完成条件是 R 对应仿真步的实际完成序号达到本票据要求，不是“调用过一次 tick”。多个 Step 按明确队列/拒绝策略处理，不覆盖一个 shared bool。

Stop 分为请求停止、停止调度、完成使用者退出和资源退休；只有这些步骤完成才是 Stopped。Run 视图可以先显示 Stopping，不能把 C++ 删除 RunSession 当作 GPU 资源已经可释放。

将运行结果应用到作者内容是单独的 `ApplyRunChanges` 用例：记录启动基线、选定属性和目标当前状态，构造领域 edit；冲突需要验证或显式选择。成功生成作者历史条目，失败不改作者源。不是 stopRun 的隐藏副作用。

**验收：** Q22—Q25、Q29。关联 A02/T05/P08/N08。

### 16.7 应用布局：先验证再准备，不借布局偷偷恢复内容

ApplyLayoutOperation 先完成纯语法和引用合法性检查，再固定一次 ExtensionSnapshot。用当前 ViewRestoreKey 匹配现有视图，计算增加、移动和状态变化；未知 provider 的配置保留为 opaque payload；额外视图保留。

准备阶段产生 DetachedView、各 provider 的 PreparedViewState 与 Host 的容量预约。随后请求结束必须结束的 gesture；此步可能提交一次合法作者编辑，应与布局结构结果分别记录。提交前再次检查视图代际，全部结构修改在安全点采用。偏好记录写入是后续独立操作，失败不能把已经成功的布局采用改成“未发生”。

已有视图不应在 restore callback 中自行重载资产；需要找回内容的需求归 RecoveryManifest。缺少 provider 的视图可以保留占位和原载荷，待插件可用再实现；不得假装恢复成功，也不得丢弃未知字段。

**验收：** Q04、Q31—Q39。关联 A04/T01/C01/C02/C03。

### 16.8 关闭一个视图、关闭内容与退出必须走不同入口

**CloseViewOperation：** 解析当前 ViewId，询问 CloseViewPolicy。普通视图只停输入、清理其 gesture/订阅并安全退休。最后一个视图触发“保留无视图内容”或显式内容关闭流程；不得先销毁最后视图后才发现关闭内容被取消。

**CloseSessionsOperation：** 固定参与 Session 集，记录各自被审阅的 ContentStamp。Save/Discard/Cancel 是业务决定；取得 ClosePermit 是删除准入。即使选择 Discard，也必须验证审阅后没有新增未确认修改。

推荐阶段如下：

| 阶段 | 拥有的资源 | 可以发生的副作用 | 失败/取消语义 |
|---|---|---|---|
| Collecting | 固定目标 Id 列表和来源信息 | 无销毁 | 无内容变化 |
| Reviewing | 审阅快照、用户决定 | 可能启动明确的保存 | 已完成保存不回滚 |
| Settling | 对未完成保存、Run、gesture 的处理结果 | 明确的停止/保存/编辑提交 | 逐项报告，不声称全局无副作用 |
| PreparingClose | 已取得的 ClosePermits | 阻止对应内容新增写入 | 后一项失败则释放全部 permit；Session 均仍在 |
| Committing | 全部许可、已准备解绑与退休记录 | 解绑能力和视图，删除 Session | 不再执行普通可失败业务；分配资源此前准备 |
| Finished | CloseReport | 无新副作用 | 终态可查询/清理 |

在所有参与者都取得关闭许可前，不能先销毁第一份内容，再因为最后一份被取消而只剩半个工作区。相反，之前用户允许的保存可能确实已经写入磁盘，这是合法且必须报告的部分完成。

固定参与集之后新开内容怎么办：普通“关闭所选”不包含新内容；应用退出进入最终准备时必须拒绝新打开或重新收集。不能只在退出开始时读一次集合，然后遗漏期间加入的会话。

**验收：** Q03、Q18—Q21、Q43。关联 A01/T03/P04/P07/N07/N12。

### 16.9 导入模型：IO 完成不等于插入完成

ModelImportOperation 先加载/解析资源并准备具有稳定作者身份的插入描述，再在 owner 阶段交给 SceneSession 验证目标会话、历史、父对象和基线。资源读取成功只表示 ReadCompleted；对象插入并进入历史才表示 Inserted。

初版对结构基线变化采用明确的 StaleBase 拒绝，或者对仍有效的目标父对象重新准备；两种策略必须在命令中选择，不允许通过“当前选中的父节点”悄悄改变用户原先目标。异步加载期间对象被删除、会话重载、窗口关闭的情况不能依赖保存一个裸 Entity 或 SceneEditor*。

失败不会留下只有一部分组件或缺少历史记录的模型对象。取消发生在插入提交之后，结果为已插入；撤销是另一条明确的编辑操作。

**验收：** Q05、Q06、Q09、Q11、Q26。关联 T04/T06/P03/P04。

<a id="execution-contracts"></a>
## 17. 线程、阶段、失败和释放契约

### 17.1 谁可以在哪个阶段做什么

| 对象/模块 | 默认执行位置 | 后台允许持有的东西 | 禁止行为 |
|---|---|---|---|
| SessionStore、Scene/MaterialSession、EditHistory | 会话 owner 线程同步阶段 | 真正不可变的 capture | worker 读取 live Registry；跨等待持有可写借用 |
| SaveOperation | owner 管理阶段；编码/IO 委托任务 | frozen job、字节 owner、目标票据、CodeLease | 完成闭包捕获窗口/会话裸 this；在 worker 修改 checkpoint |
| RunStore/RunController | engine owner 的控制阶段 | 已冻结启动输入 | UI 与 app 各自 tick 同一实例 |
| SceneProjection/MaterialPreview | owner 采用；后端任务按原系统调度 | 版本化资源、编译输入与输出 | 过期输出覆盖新版本；以构建开始推进 accepted key |
| ViewHost/Root | UI owner 安全阶段 | 不跨线程使用 Pane 指针 | 事件/绘制回调中同步删当前 Pane |
| ExtensionSnapshot/CommandEntry | 读取可共享不可变快照；发布在 owner 阶段 | 对应代码 lease | 边迭代边替换同一可写容器；提前卸载代码 |
| Layout/Recovery 纯值 | 纯计算可在 worker；应用在 owner | 拥有的数据和固定注册描述 | worker 调用 UI restore 并修改活动树 |

通常 UI 与内容 owner 可以是同一线程，但这是装配选择，不需要给每个模块建线程。继续使用已有执行器和任务组合，领域接口不自行开后台循环。

### 17.2 一次主循环的参考阶段，不是散落的全局隐含顺序

1. 收集平台输入和任务完成通知，放入有界的拥有值队列；不在任意完成回调里直接重写模型。
2. 在 owner 维护阶段采用已验证完成值，推进具体用例；接受正常的编辑与控制请求。
3. 结束必须提交的交互，执行领域编辑；结构性视图采用/删除仅发生在已确认安全的阶段。
4. engine 按既定调度统一维护、仿真、同步与发布；临时 Registry 借用在驱动前结束。
5. 投影和预览消费稳定版本，构建并提交视口输出；资源背压保留明确状态。
6. UI 绘制/事件通知只观察一致状态；回调产生的新工作进入下一个允许处理点。
7. 回收已满足 CPU/GPU/插件寿命条件的退休对象，记录终态与诊断。

具体 engine 中的维护、同步、GPU 提交顺序应与其真实实现对应，不能机械强塞上述编号。必须保证的契约是：单一驱动、借用不跨驱动/重入、通知不观察半提交状态、退休不早于最后使用者。主循环只调用子系统的阶段入口，不读取每个 Save/Close 内部的 bool 来决定谁先走。

### 17.3 预期失败、契约违规与资源耗尽应分开

正常业务失败，如资产不存在、目标冲突、格式错误、请求过时，使用所属模块的结构化错误返回。取消用终态表达，不依赖字符串消息。跨模块边界可转换成 Diagnostic，但保留 origin/error code；不得把诊断文本当成机器协议。

错误线程、重入、失效内部借用等是契约违规，应有 debug 检查与测试；安全点检查不能为了“干净”而删除。发布 API 对不可信外部参数仍返回明确错误，内部受控不变量与用户输入不要一律混成 abort。

**内存分配政策必须单独确定。** 本文不承诺所有 OOM 都可恢复。若项目策略允许分配失败终止进程，需要在契约和测试中明确；若某个接口承诺返回 AllocationFailure，就必须在相关准备/提交路径真正做到，并在 fault injection 中证明，不能标上 noexcept 后让抛出变成 terminate。[S35] 的现有提交说明已包含宿主分配策略，迁移不能忽略这个前提。

“提交不可失败”只能针对已经完成资源准备、不会调用可失败业务的正常进程路径；不代表硬件故障、进程崩溃和任意恶意插件都能回滚。

### 17.4 析构负责释放，不承担交互和长期业务

析构不得弹保存确认、启动新保存、等待一个需要当前线程继续泵事件才能结束的任务。ClosePermit 未消费析构仅归还准入；SceneInstanceLease 的析构只能按规定登记/执行安全退休，不即刻假定 GPU 完成。

App 正常关闭顺序：阻止新准入 → 完成或取消用例并观察其终态 → 停止关联运行并处理持久化发布 → 解绑视图/交互/角色注册 → 等待既有任务和 GPU 退休到明确边界 → 销毁会话与相关快照/历史 → 释放注册快照和插件代码 → 释放底层运行时/平台。

这不是简单依靠所有成员倒序析构。根对象应有显式 `shutdown()` 协议，析构只处理已停止对象或执行明确定义的紧急清理。正常退出允许取消时，不应提前进行不可逆的资源拆除；一旦进入不可取消的最终拆除阶段，界面必须准确反映。

### 17.5 完成记录必须有界，但清理记录不能撤销事实

Save/Run/Open 等模块可以使用不同 Id 与状态类型，共享终态观察机制即可。操作集合有容量、取消策略、终态保留期限或显式 acknowledge；达到上限在准入时拒绝或合并明确定义的重复请求。

`acknowledge` 只表示调用方不再需要保留观察记录，不是取消 IO、释放仍被 GPU 使用的资源或改变 dirty。清理操作必须满足任务、回执采用和代码保活的真实寿命条件。错误信息过大可截断诊断展示，但不能因此丢失判断终态所需的结构化字段。

### 17.6 同步、版本验证与权限各自解决不同问题

Id 代际防止对象重用混淆；HistoryId 防止旧历史结果污染重载后内容；BindingRevision 防止旧目标结果改写新来源；写入队列控制同目标物理发布顺序；owner 线程约束模型访问；CodeLease 防止代码提前卸载。它们不是互相替代的“安全包装”。

新增任一版本或锁时必须指出它解决哪种竞态，以及已有哪个机制不够。不能因为一个问题需要绑定版本，就给所有对象再加一枚无语义的 global_revision。

<a id="acceptance"></a>
## 18. 验收：52 个可执行的观察场景

**以下全部是待实现/待运行的验收场景，不是本次测试结果。** 优先用确定性 fake 控制完成顺序和故障点，再将真正涉及文件系统、平台窗口、GPU 或插件加载的部分纳入真实集成测试。sanitizer、编译检查和 GPU 验证不能互相替代。

测试命名应反映行为，例如 `save_older_encoding_finishes_last_preserves_publish_order`，而不是沿用某轮交付编号作为永久产品契约。每项保留输入、失败点、可观察状态和必要日志；一次测试可覆盖多个问题，但不能只检查返回值而不检查实际状态。

| 编号 | 场景 | 模块 | 故障/操作 | 必须观察到的结果 | 建议层次 |
|---|---|---|---|---|---|
| <a id="q01"></a>Q01 | 领域无桌面启动 | M02/M03/M04/M05 | 不创建 Root、Pane、GPU 或 ProjectStorage，建立 Scene/MaterialSession 并编辑/捕获 | 领域行为可用；domain consumer 的链接与 include 闭包无 UI/渲染实现 | native/build |
| <a id="q02"></a>Q02 | 双视图单一作者态 | M04/M10/M11 | V1/V2 绑定同一 Session，在 V1 编辑并从 V2 undo | 一次编辑只有一个历史条目；两视图观察同一状态，相机仍独立 | native/desktop |
| <a id="q03"></a>Q03 | 关闭一个视图 | M03/M11/M15 | 关闭两个视图中的一个 | 会话、历史及另一视图保持；只退休该视图资源 | native/desktop |
| <a id="q04"></a>Q04 | 布局不替换作者内容 | M04/M11/M12/M15 | 有未保存场景时应用含同类窗口的布局 | 内容/HistoryId/dirty 不被布局恢复覆盖；视图只重新排列或绑定 | native/desktop |
| <a id="q05"></a>Q05 | 失败和过期读取 | M07/M15 | 读入失败、乱序完成、完成前重载或更换目标 | 原工作副本保留；旧结果不能采用；终态说明拒绝原因 | fake IO |
| <a id="q06"></a>Q06 | 身份代际与类型校验 | M03/M04/M08 | 释放后复用槽位；使用错类型 key、旧 HistoryId 或旧运行对象 | 返回失效/类型错误；不访问新对象，不靠同一整数推断相同身份 | native |
| <a id="q07"></a>Q07 | 撤销回保存点 | M02/M03/M06 | 保存 S10、编辑 S11、undo 到 S10 | 本绑定基线仍 S10；当前恢复 clean；通知版本可继续增长 | native |
| <a id="q08"></a>Q08 | 字节相同不伪造状态等价 | M02/M03 | 重新执行编辑得到相同字节但新 StateId | 按文档保守定义仍 dirty；不能混用内容 hash/通知版本清除标记 | native |
| <a id="q09"></a>Q09 | 编辑准备故障 | M02/M04/M05 | 在 prepare 校验、预算或预备资源阶段注入失败 | 作者内容、历史游标和可观察选择不发生半提交 | fault injection |
| <a id="q10"></a>Q10 | 编辑门和许可语义 | M03/M04 | 编译期复制 EditScope；嵌套进入编辑；移动/释放 ClosePermit | EditScope 不可复制/移动；重入被拒；permit 未消费只解除限制 | compile/native |
| <a id="q11"></a>Q11 | 冻结快照与后台寿命 | M04/M05/M06/M07 | 捕获后继续编辑/关闭视图，延迟编码和完成采用 | 编码对应捕获状态；无 live Session/Pane 裸指针跨任务；所有输出显式拥有 | native/sanitizers |
| <a id="q12"></a>Q12 | 保存旧状态时继续编辑 | M03/M06 | 捕获 S10 后编辑到 S12，再完成保存 | 基线 S10、当前 S12，仍 dirty；不标记回调时的当前状态已保存 | fake IO |
| <a id="q13"></a>Q13 | 反序编码、有序发布 | M06/M07 | W2 编码先完成；W1 延迟/失败/取消；最后再保存撤销后的旧 StateId | 同目标按准入发布；空洞可终结；最后意图决定最终文件，不按 StateId.serial 排序 | fake IO |
| <a id="q14"></a>Q14 | 两个副本同目标冲突 | M03/M06/M07 | 两 Session 基于同存储版本独立保存到同一规范化地址 | 后一写入按后端契约报告冲突，不能静默串接成同一工作副本的后继 | fake/real IO |
| <a id="q15"></a>Q15 | Save As 发布失败 | M03/M07 | 重绑定候选已备妥，在新文件发布前注入失败 | 源地址、BindingRevision、历史和保存基线不变，无部分身份采用 | fake IO |
| <a id="q16"></a>Q16 | Save As 冻结与普通 Save | M03/M06/M07 | 分别在正常 Save 和 Save As 飞行期间编辑、重载及再次保存 | 正常 Save 允许编辑；Save As 按 permit 拒绝冲突操作，结束准确释放 | native |
| <a id="q17"></a>Q17 | 提交后取消与记录清理 | M06/M07 | 后端已发布，随后 cancel 或 acknowledge | 结果为已提交/取消过晚；acknowledge 不撤销文件、不清除未完成资源责任 | fake IO |
| <a id="q18"></a>Q18 | 关闭与迟到保存结果 | M03/M06/M15 | 处理关闭期间保存；删除 Session 后注入已允许保留任务的完成 | 无 UAF；已发布事实保留，不能采用到重用槽位；新绑定不被旧回执污染 | native/sanitizers |
| <a id="q19"></a>Q19 | 全体关闭后项取消 | M03/M06/M15 | A 已保存，B 选择取消；或第二个 permit 失败 | 任何 Session 均未被提前销毁；A 的真实保存保留并报告；permit 全部释放 | native |
| <a id="q20"></a>Q20 | 审阅戳过期 | M03/M15 | 对 S10 显示丢弃确认，答复前内容变成 S12 | 不能丢弃未经审阅的 S12；重新审阅或明确拒绝 | native |
| <a id="q21"></a>Q21 | 最后视图策略 | M11/M15 | 关闭最后视图，分别选择保留内容、关闭内容、取消 | 三种行为可区分；取消不先销毁最后可用视图 | native/desktop |
| <a id="q22"></a>Q22 | 运行隔离 | M04/M08 | 从 S10 启动 R；编辑作者源到 S12；运行中改变对象 | 作者和 R 不互相改写；provenance 保留 S10；无双可写事实 | native/runtime |
| <a id="q23"></a>Q23 | 暂停仍维护 | M08/M09 | 暂停仿真后完成资源加载/同步/发布 | 仿真时间不前进，必要维护和呈现可前进；valid 不再被误用作存在性 | runtime/GPU |
| <a id="q24"></a>Q24 | 步进回执 | M08 | 连续 Step、旧 RunId 的 Step 和执行失败 | 票据仅在对应真实仿真步完成后成功；旧运行和失败不假完成 | native/runtime |
| <a id="q25"></a>Q25 | 显式应用运行修改 | M04/M08/M15 | 运行基线后作者发生冲突修改，再请求 ApplyRunChanges | 冲突按策略拒绝/审阅；成功只通过一次作者编辑并可 undo；stop 不回写 | native |
| <a id="q26"></a>Q26 | 选择身份隔离 | M04/M08/M10 | 作者/运行对象数值相同；对象删除/重载/代际复用 | 不会误选另一来源对象；失效选择清理，不重复删除领域内容 | native |
| <a id="q27"></a>Q27 | 材质编译迟到 | M05/M09 | S10 编译晚于 S12 完成；或较新编译失败 | 旧结果不能覆盖新配置/内容；合法作者编辑和历史不因编译失败回滚 | fake compiler |
| <a id="q28"></a>Q28 | 高亮捕获失败重试 | M09 | 选择保持不变时资源捕获首次失败、第二次成功 | accepted key 未前移；下一轮仍尝试或消费保留候选 | fake render |
| <a id="q29"></a>Q29 | 背压与资源退休 | M08/M09/M11 | 后端暂拒提交；随后接受但 GPU 尚未完成；关闭视图 | desired/prepared/accepted 分明；资源和代码 pin 留到真实最后使用点 | fake render/GPU |
| <a id="q30"></a>Q30 | 投影漏事件恢复 | M04/M09 | 裁剪增量队列或重载导致 changesSince 不可提供 | 返回 ResetRequired 并重建；不永久卡在旧投影，不读取过时借用 | native |
| <a id="q31"></a>Q31 | 坏 docking 数据 | M11/M12/M15 | 合法部分 Pane 状态配损坏 dock 记录 | 纯验证失败前不创建活动窗口、不改可见性；作者内容不变 | native/desktop |
| <a id="q32"></a>Q32 | 缺失扩展载荷 | M12/M14/M15 | 布局含未安装 provider 和未知 schema 扩展字段 | 报告不可用；载荷原样/按协议保留，保存往返不静默丢失 | native |
| <a id="q33"></a>Q33 | 额外视图和未保存内容 | M11/M12/M15 | 布局不包含当前额外窗口且内容 dirty | 默认保留额外窗口和工作副本；显式报告处理策略 | native/desktop |
| <a id="q34"></a>Q34 | 工厂/挂载准备失败 | M11/M14/M15 | DetachedView 构造、状态准备、Host 容量准备失败 | 无幽灵 Pane 或半注册窗口；已有布局保持；不得依赖任意插件外部副作用可回滚 | fault injection |
| <a id="q35"></a>Q35 | 稳定布局身份重命名 | M12 | 重命名显示标签，同时令 preferences 不可写 | 布局按稳定 LayoutId 保持可选；重命名不需要跨文件更新活动引用 | fake/real IO |
| <a id="q36"></a>Q36 | 布局成功、偏好失败 | M12/M15 | 布局已保存或采用，再令偏好保存/目录刷新失败 | 结果区分两件事；不得把已提交布局报告成未发生 | fake IO |
| <a id="q37"></a>Q37 | 删除活动布局回退 | M12/M15/M16 | 删除当前布局后重启，偏好仍引用缺失 Id | 按明确默认回退并报告，不因旧偏好指针使启动崩溃 | native/desktop |
| <a id="q38"></a>Q38 | QUERY 自替换注册 | M13/M14 | 查询回调请求替换包含自身的注册表并在随后访问捕获状态 | 旧 entry/code 存活到调用返回；发布延迟/拒绝，不边销毁边执行 | native/sanitizers |
| <a id="q39"></a>Q39 | 恢复批次注册变化 | M12/M14/M15 | 恢复第一个视图时扩展集合请求变更 | 整个批次使用固定 snapshot；不出现迭代失效与半批次不同版本 | native |
| <a id="q40"></a>Q40 | 准入不是完成 | M06/M13/M15 | 命令被接受，实际保存/打开随后失败或目标消失 | DispatchReceipt 表示准入，最终 outcome 表示失败；界面不提前显示完成 | native |
| <a id="q41"></a>Q41 | 焦点改变不漂移目标 | M03/M11/M13 | 提交后切换焦点、关闭旧目标并复用槽位 | 操作仍针对原目标或拒绝失效，不自动作用于当前焦点的新内容 | native |
| <a id="q42"></a>Q42 | 菜单初始化失败 | M13/M16 | 注入菜单信号/注册初始化失败 | EditorApplication 创建返回失败；不得发布已标记退出的成功对象 | fault injection |
| <a id="q43"></a>Q43 | 有序关闭与代码保活 | M06/M08/M09/M11/M14/M16 | 飞行中编码、GPU 提交、插件回调同时遇到退出 | 停止准入→排空/终态→退休→析构→卸载；不依靠 UI 泵已停止后等待 | integration/sanitizers/GPU |
| <a id="q44"></a>Q44 | 部分初始化与 OOM 契约 | M03/M11/M16 | 各初始化点失败和受测分配点耗尽 | 按声明策略清理；声称可恢复的 API 不偷偷 terminate；终止策略不伪称可恢复 | fault injection |
| <a id="q45"></a>Q45 | 负向依赖检查 | 所有模块 | 人为让 scene_model include Pane、workflows 链接 bootstrap 或 engine 依赖 editor | 配置/CI 明确失败；测试失败不能被 PUBLIC/PRIVATE 标签掩盖 | build |
| <a id="q46"></a>Q46 | 独立安装消费者 | M14/M16 | 仅安装前缀在干净构建树编译插件与工具消费者 | 不读取源码私有头、测试后门或原 build tree；所需依赖被正确导出 | installed/build |
| <a id="q47"></a>Q47 | Player 与通用运行层独立 | M08/M16 | 禁用所有 editor target 配置/编译 Player 和 runtime consumer | 不需要编辑器头/库/插件表；运行职责未搬到 editor | build |
| <a id="q48"></a>Q48 | SDK 闭包独立 | M14/M16 | 只修改编辑器扩展契约，检查运行插件构建契约 | 运行插件不无故依赖编辑器 SDK 指纹；兼容拒绝有准确范围 | build/installed |
| <a id="q49"></a>Q49 | 普通测试跨平台配置 | M16 | 无 PowerShell 和本机定制 LLVM 路径环境配置 native tests | 普通测试可用；平台/GPU 组明确选择与缺失原因，不阻断领域测试 | build/CI |
| <a id="q50"></a>Q50 | 性能和容量测量 | M04/M06/M09/M11 | 单/双/多视图、保存快照、投影积压、持续保存 | 记录时间/峰值内存/复制/分配/退队情况；无无界保留，无未测量提速结论 | benchmark |
| <a id="q51"></a>Q51 | 坏加载/工厂无幽灵对象 | M03/M07/M11/M14/M15 | 准备失败或内容已发布但视图失败 | 发布前无半会话；发布后明确保留无视图内容并报告；无幽灵 Pane | native/desktop |
| <a id="q52"></a>Q52 | SDK 与载荷版本拒绝 | M12/M14 | 加载不匹配 SDK 契约或不支持 schema 的状态 | 在调用不兼容入口前拒绝；未知载荷保留；不把指纹相等当作无限 ABI 保证 | installed/native |

### 18.1 测试与架构的对应关系

Q01—Q11 证明作者模型、身份和历史边界；Q12—Q21 证明保存、审阅和关闭事实；Q22—Q30 证明运行与派生状态；Q31—Q41 证明布局、扩展和命令；Q42—Q52 证明启动、退出、构建、安装和性能约束。它们共同验证第 4 章的不变量，而不是仅覆盖函数行。

### 18.2 测量不预设好看的数字

Q50 必须先固定代表性内容、资源与构建配置，并建立同机器基线。至少记录内容大小/对象数、视图数、快照保留量、编码并发、主线程最长阻塞、投影延迟、每帧分配量和 GPU 资源退休峰值。记录冷启动与稳定态，不把两者混成一个平均值。

预算由产品目标与实测确定，不能在本指导中凭空承诺“零拷贝”“零分配”或某个提升百分比。每个运行中集合必须有上界或明确的背压策略；快照可共享、增量或写时复制，选择需有证据。多视图不应默认复制多份完整作者世界，这一结构性要求可以先通过对象归属验证。

<a id="implementation"></a>
## 19. 实施路线：目的地独立设计，切换过程控制风险

### 19.1 总体规则

本方案允许改变旧 API、内部目录与拥有关系；不要求维持每个旧类型。但是数据格式、用户已有资产和已保存的布局不能在没有迁移方案的情况下被破坏。旧格式读取适配归持久化边界，不能让旧业务 owner 永久进入新模型。

每阶段只建立一条事实来源。兼容入口只能调用新模型，不能同时操作旧字段以“保持两边一致”。任何阶段完成都必须说明哪些旧职责已经停用、哪几个新类型现在是真实 owner；不能仅提交新文件并保留旧运行路径不变。

### 19.2 阶段与交付门槛

| 阶段 | 实施内容 | 可交付的完整行为 | 必须删除/禁止 | 核心验收 |
|---|---|---|---|---|
| G0 语义与证据冻结 | 固定本文基准；盘点旧入口/格式；记录依赖边；先写关键失败测试 | 旧行为和缺陷触发条件可追踪 | 不把静态分析写成已复现；不先批量改名 | Q31、Q38、Q42 的失败用例与事实核对 |
| G1 历史与领域独立 | 抽离 SceneSource/MaterialSource；建立 SessionStore 和窄 IEditSession；保留历史算法 | 无窗口打开工作副本、编辑、undo/capture | 新域不得 include SceneEditor/EditorContext/Pane；不把旧 Impl 改名搬来 | Q01、Q06—Q11、Q45 |
| G2 持久化事实闭环 | 冻结捕获、SaveService、有序发布、checkpoint 单一归属；Scene/Material 编码适配 | 保存期间继续编辑；失败/冲突/取消结果准确 | 删除该新路径的历史 saved 双写；无可写隐藏编码输出 | Q12—Q18、Q50 |
| G3 UI 独立与双视图 | 底层 detached 构造/attach；ViewHost；SceneView/MaterialView/交互绑定 | 一份内容两个视图；关闭其中之一；再打开视图 | UI 不拥有内容/history/save；无工厂自行采用 | Q02—Q04、Q21、Q26、Q34、Q51 |
| G4 运行与投影 | 独立 RunStore/RunController；单一 engine 驱动；SceneProjection 和预览/高亮 | author/play 隔离，pause/step/stop 与多视图正常 | 删除 Pane 内运行实例 owner；不新增平行 EditorRuntime | Q22—Q30、Q47、Q50 |
| G5 工作区与产品流程 | 布局/恢复/偏好拆分；Open/Reload/Close/SaveAll 用例；命令 query/execute 分离 | 非破坏性布局、全体关闭可取消、完整打开/退出 | 删除 WorkspaceRequest 混合协议和 CANCEL 清理复用 | Q05、Q19—Q21、Q31—Q42 |
| G6 扩展、安装与旧入口删除 | typed 扩展发布、CodeLease、SDK 闭包、真实安装消费者；迁移旧格式读取 | 外部工具接入且不访问源码私有头 | 删除 EditorContext/旧 PaneManager/SceneEditor owner；新域无兼容反向边 | Q38—Q49、Q52 |
| G7 产品验收 | 全量故障场景、桌面/GPU、性能、干净克隆与 CI 配置 | 仅新路径运行产品，所有剩余限制明确记录 | 不以局部通过替代总体；无双架构长期共存 | 全部 Q，按平台标记真实结果 |

G0 中明显且局部可修的正确性问题可先修，以保护迁移；但这些修补不决定新架构形状。G2 与 G3 可以分支并行研究，最终采用仍必须满足同一所有权契约。阶段不是固定工期，也不是必须把 G1 全做完才允许验证一个 GUI 原型；它们是合并与责任切换的门槛。

### 19.3 第一个端到端切片必须同时穿过新边界

选择一个实际小场景和一个材质：在 SessionStore 中建立内容，SceneSession 完成一次结构编辑/撤销；SaveService 完成正常保存；SceneView 通过 typed key 显示同一内容；再创建第二视图。随后加入 Run 和 Layout。先使这一条路径完整，不先迁移所有菜单和边缘工具。

Material 必须在较早阶段进入验证，以防通用契约暗含 Scene 假设；但不要为了它一次性建设“任何未来文档”的完整能力系统。第二个工具揭示的真实共同部分才进入 M02/M03/M06/M11 等契约。

### 19.4 每个实现提交必须附的证据

提交描述至少写清：关联 A/T/C/B 编号；新增/修改类型的拥有者与模块；改变的公开语义；成功及失败后事实；旧责任去向；执行了哪些 Q 场景及其结果。未执行的测试明确列为未执行，不把“添加测试源码”写成通过。

新增模块同时提交直接依赖表、公开头独立编译样例和禁止反向依赖测试。新增异步路径同时提交迟到、取消、顺序颠倒和 owner 销毁测试。新增长期状态同时说明容量、终态清理和代码寿命。

### 19.5 兼容与数据迁移

旧布局读取可转换成新 DockLayout/RecoveryManifest，保留无法理解的 payload 和版本；输出新格式时明确记录 schema。迁移失败保留旧文件与诊断。稳定 LayoutId 的建立应有确定映射和冲突检测，而非每次启动重新随机生成。

现有资产编码可以先复用；只有确实需要新身份/引用语义时才调整格式。调整前验证 Save/SaveAs/ExportCopy 与 WorldObjectId 的关系及旧文件往返。不要为重构 C++ 结构顺带引入不相关的资源格式破坏。

旧插件 SDK 不假设自动兼容。迁移窗口可提供显式版本适配，但适配只在扩展边界；新领域模型不接受旧 Context。正式切换后，无法安全适配的插件在调用入口前被拒绝，不能尝试运行再期待捕获异常。

### 19.6 何时算完成结构性重构

不仅是新类参与了执行，而是旧职责已被替代：打开内容不再产生 Pane 类型结果；SceneSession 在无窗口环境下可用；视图不再拥有源与历史；运行不受窗口析构支配；保存基线只有一个 owner；布局应用不隐式恢复作者内容；查询不修改正在执行的注册容器；引擎不反向依赖 editor。

同样，旧 EditorContext 不应留作“方便扩展的备用通道”；旧 friend/TestAccess 不应成为新模块间常规访问方式；旧 target 不应保留全部 transitive dependencies 使分层测试失去意义。删除工作是迁移的一部分，不是未来优化。

<a id="review-rules"></a>
## 20. 实施评审规则：优雅不是抽象更多，而是责任更可证明

### 20.1 直接拒绝的伪重构

| 看似改进 | 实际仍然错误 | 接受标准 |
|---|---|---|
| 将 SceneEditor::Impl 改为 SceneSession | 仍含 Pane、run_history、workspace、GPU 高亮和保存任务 | 按第 15 章迁出不属于作者域的成员 |
| 增加多个 Manager，各持 SharedEditorState& | 谁都能改整个流程，拥有边界不存在 | 每个流程只拥有自身状态；通过窄完成值协作 |
| Context 改名 Services | 仍返回 engine/panes/plugins/project 全部权限 | 具体依赖集合有限，不能再取全局根 |
| 工厂返回 unique_ptr | 构造时已注册活动 Root，有失败残留 | 真正 detached 的准备与显式 attach 契约 |
| 一切都用 shared_ptr | 关闭由引用计数决定，任务拖住内容身份 | 产品生命周期由 Store；任务仅保活独立快照/代码 |
| 一切改成 variant | 仍允许阶段与资源不匹配，公开暴露 index | variant 只表达互斥合法阶段；操作检查契约明确 |
| 新增一层通用 Document | 大量工具对 play/compile/save 默认 Unsupported | 只有所有派生者真实满足的窄角色继承 |
| 所有业务都走 CommandId/void* | 类型直到运行时才恢复，历史依赖焦点 | UI 命令到边界后转换为明确领域操作 |
| 每个概念一个 target | 数量增加但目标仍公开依赖大 engine/editor_ui | target 有可验证边界，允许合理合并物理产物 |
| 全部链接改 PRIVATE | 公开头/静态库闭包仍需要依赖，安装消费者失败 | 如实表达使用需求，同时真正拆出低层契约 |
| query 参数变 const | 闭包仍有全局写权、注册在执行中被销毁 | 能力限制、固定快照和发布规则同时成立 |
| 缓存字段换成 last_revision | 失败前已更新 accepted，仍丢失重试 | desired/prepared/accepted/retired 分工正确 |
| 全部 noexcept 并统一 catch | 内部分配仍可终止，失败输出不完整 | 每个公共失败承诺有故障测试和 OOM 政策 |
| 单测通过所以总体完成 | 未验证安装/GPU/部分失败与关闭顺序 | 分层报告真实验证范围和剩余限制 |

### 20.2 每一个新类型的审查问题

新类型必须说明它是值、身份、借用、拥有对象、操作、能力还是完成结果；回答它维护的不变量；列出哪些对象可以创建/修改/销毁它；说明线程和失效点；指出所在模块及允许依赖；提供至少一个能证明它不是旧类型别名的行为测试。

不要求每个简单坐标 struct 写长设计报告。要求的是凡是新增“框架级类型”，都不能只凭名字高级就进入公共 API。没有独立语义的 wrapper 可删除；只服务于某一实现的状态不公开。

### 20.3 函数与成员的命名规则

`find/read/describe/query` 不隐式创建或发布；`request/begin` 返回的是准入或操作身份；`commit/publish` 对应明确定义的可见提交；`finish/complete` 仅用于真的终态；`cancel` 说明是否仍可阻止提交；`release/retire` 说明释放何种责任及是否等待后端；`restore` 必须说明恢复布局、视图状态还是作者内容。

不能用 `valid/invalid` 同时表达存在性、启用、暂停和失败。布尔字段用于真正的二元属性，例如视图可见性；阶段不通过一组 `busy/pending/done/failed` 可任意组合地表达。查询派生状态不复制为第二个可写字段。

### 20.4 变更设计时保持追踪链同步

每条架构决策记录包括动机、替代方案、选择、代价、受影响类型/模块/Q 场景。若将 Save As 改成允许并发编辑，需要同时重写 BindingChangePermit、作者身份映射、历史保存基线与 Q15/Q16，而不是删除一处 Busy 检查。

如果运行要默认保留到内容关闭之后，需要调整 CloseSessionsOperation 的策略和 Run 视图绑定、快照寿命、Q18/Q21/Q43。这样，四个维度才真正联动，而不是文档各章节各说各话。

<a id="traceability"></a>
## 21. 总追踪矩阵：每项问题对应什么设计、类型、模块与测试

该矩阵是实施导航，不替代第 2 章证据卡或后文类型契约。评审一个补丁时从左向右检查：问题有没有真的消失；原则是否落实；新类型是否维护自己的不变量；依赖是否进入正确模块；测试是否验证实际效果。

| 现有问题 | 设计原则 | 关键替代类型/关系 | 归属模块 | 验收场景 |
|---|---|---|---|---|
| [A01 应用入口拥有多个流程的中间状态](#a01) | [P01](#p01)、[P02](#p02)、[P04](#p04) | OpenAssetOperation / CloseSessionsOperation / SaveAllOperation / EditorApplication | [M15](#m15)、[M16](#m16) | [Q19](#q19)、[Q40](#q40)、[Q42](#q42) |
| [A02 内容、视图和运行实例被绑定到同一个窗口对象](#a02) | [P01](#p01)、[P02](#p02)、[P03](#p03) | SceneSession / SceneView / RunSession / SessionStore | [M03](#m03)、[M04](#m04)、[M08](#m08)、[M11](#m11) | [Q01](#q01)、[Q02](#q02)、[Q03](#q03)、[Q22](#q22) |
| [A03 Context 隐藏真实依赖并泄漏管理权限](#a03) | [P02](#p02)、[P05](#p05)、[P11](#p11) | SceneSessionAccess / SaveService / ViewRequests / ExtensionPublisher | [M03](#m03)、[M06](#m06)、[M11](#m11)、[M14](#m14)、[M15](#m15)、[M16](#m16) | [Q01](#q01)、[Q45](#q45)、[Q51](#q51) |
| [A04 窗口工厂混合构造、采用、复用和显示](#a04) | [P01](#p01)、[P04](#p04)、[P05](#p05) | DetachedView / ViewFactory / ViewHost / OpenAssetOperation | [M11](#m11)、[M14](#m14)、[M15](#m15) | [Q34](#q34)、[Q51](#q51) |
| [A05 借用跨越回调边界，const 也没有表达真正只读](#a05) | [P03](#p03)、[P05](#p05)、[P06](#p06) | SessionKey&lt;T&gt; / SessionInfo / ViewInfo / CommandRegistrySnapshot / ExtensionSnapshot | [M03](#m03)、[M11](#m11)、[M14](#m14) | [Q06](#q06)、[Q38](#q38)、[Q39](#q39) |
| [A06 通用 UI target 聚集领域与平台依赖](#a06) | [P01](#p01)、[P05](#p05)、[P11](#p11) | ViewHost / SceneView / MaterialView / SceneProjection / MaterialPreviewStore | [M09](#m09)、[M10](#m10)、[M11](#m11)、[M16](#m16) | [Q01](#q01)、[Q45](#q45)、[Q46](#q46) |
| [A07 metadata 同时表示描述和可执行扩展](#a07) | [P01](#p01)、[P05](#p05)、[P10](#p10) | CommandDescriptor / CommandEntry / ViewFactoryRegistration / CodeLease | [M13](#m13)、[M14](#m14) | [Q38](#q38)、[Q46](#q46)、[Q48](#q48) |
| [A08 Scene 与 Material 重复维护同类内容切换生命周期](#a08) | [P01](#p01)、[P02](#p02)、[P04](#p04) | OpenAssetOperation / ReloadSessionOperation / SessionFactory / SceneSession / MaterialSession | [M03](#m03)、[M04](#m04)、[M05](#m05)、[M07](#m07)、[M15](#m15) | [Q05](#q05)、[Q15](#q15)、[Q51](#q51) |
| [T01 WorkspaceRequest 是请求、回复和内部阶段的混合袋](#t01) | [P01](#p01)、[P03](#p03)、[P04](#p04) | SaveLayout / RenameLayout / LayoutCatalog / LayoutApplyReport | [M12](#m12)、[M15](#m15) | [Q31](#q31)、[Q35](#q35)、[Q36](#q36) |
| [T02 WorkspaceData 合并布局、目录和偏好](#t02) | [P01](#p01)、[P04](#p04)、[P07](#p07) | DockLayout / LayoutId / LayoutCatalog / RecoveryManifest / UserPreferences | [M12](#m12)、[M15](#m15) | [Q04](#q04)、[Q35](#q35)、[Q37](#q37) |
| [T03 关闭结果与取消/释放准备状态混在一起](#t03) | [P01](#p01)、[P02](#p02)、[P04](#p04)、[P07](#p07) | CloseViewOperation / CloseSessionsOperation / ClosePermit / CloseReport | [M03](#m03)、[M11](#m11)、[M15](#m15) | [Q19](#q19)、[Q20](#q20)、[Q21](#q21) |
| [T04 已接收被写成已执行完成](#t04) | [P03](#p03)、[P04](#p04) | CommandState / DispatchReceipt / OperationRef / SaveOutcome | [M06](#m06)、[M13](#m13)、[M15](#m15) | [Q12](#q12)、[Q17](#q17)、[Q40](#q40) |
| [T05 valid/invalid 和 safe 没有表达实际动作或时效](#t05) | [P01](#p01)、[P03](#p03)、[P06](#p06) | RunSession / RunExecutionState / StepTicket / SceneInstanceLease | [M08](#m08)、[M09](#m09) | [Q23](#q23)、[Q24](#q24) |
| [T06 合法状态依赖跨字段组合与魔法哨兵](#t06) | [P02](#p02)、[P03](#p03)、[P06](#p06) | PreparedSessionData / SaveOperation / RegistryBinding | [M04](#m04)、[M05](#m05)、[M06](#m06)、[M13](#m13)、[M15](#m15) | [Q05](#q05)、[Q11](#q11)、[Q40](#q40) |
| [T07 通用错误包装承载控制逻辑，展示源也混淆](#t07) | [P03](#p03)、[P04](#p04)、[P10](#p10) | SubmitOutcome / SaveOutcome / LayoutApplyReport / Diagnostic | [M06](#m06)、[M09](#m09)、[M12](#m12)、[M13](#m13)、[M15](#m15) | [Q17](#q17)、[Q29](#q29)、[Q36](#q36) |
| [T08 const 编码输入隐藏共享可写输出](#t08) | [P03](#p03)、[P04](#p04)、[P06](#p06) | FrozenSave / SceneEncodeJob / EncodedSceneClone / SceneSaveAsOperation | [M04](#m04)、[M06](#m06)、[M07](#m07)、[M15](#m15) | [Q11](#q11)、[Q15](#q15)、[Q16](#q16) |
| [T09 EditingGuard 没有落实独占且可以被复制](#t09) | [P02](#p02)、[P03](#p03)、[P06](#p06) | SessionAccessGate / EditScope / ClosePermit | [M03](#m03)、[M04](#m04)、[M05](#m05) | [Q09](#q09)、[Q10](#q10)、[Q20](#q20) |
| [C01 布局在最后的 docking 验证前已修改窗口](#c01) | [P04](#p04)、[P07](#p07) | ValidatedLayout / LayoutPlan / PreparedMountBatch / LayoutApplyReport | [M11](#m11)、[M12](#m12)、[M15](#m15) | [Q31](#q31)、[Q32](#q32)、[Q33](#q33)、[Q34](#q34) |
| [C02 多文件已部分提交，却只返回整体失败](#c02) | [P01](#p01)、[P04](#p04)、[P07](#p07) | LayoutId / LayoutCommitReceipt / PreferenceWriteResult / CatalogRefreshResult | [M12](#m12)、[M15](#m15) | [Q35](#q35)、[Q36](#q36)、[Q37](#q37) |
| [C03 QUERY 期间可销毁正在执行的注册闭包](#c03) | [P05](#p05)、[P06](#p06)、[P10](#p10) | CommandRegistrySnapshot / CommandEntry / CodeLease | [M13](#m13)、[M14](#m14) | [Q38](#q38)、[Q39](#q39) |
| [C04 创建接口可能成功发布已失败的编辑器](#c04) | [P02](#p02)、[P04](#p04) | EditorApplication::create / StartupFailure | [M16](#m16) | [Q42](#q42)、[Q43](#q43)、[Q44](#q44) |
| [C05 高亮缓存先提交新键，准备失败后可能不再重试](#c05) | [P02](#p02)、[P04](#p04)、[P08](#p08) | HighlightRenderer / HighlightKey / PreparedHighlight / SubmitOutcome | [M09](#m09) | [Q28](#q28)、[Q29](#q29)、[Q30](#q30) |
| [B01 架构标签不等于依赖约束](#b01) | [P05](#p05)、[P11](#p11)、[P12](#p12) | target 依赖白名单 / 公开头独立编译检查（构建机制） | [M16](#m16) | [Q45](#q45)、[Q46](#q46)、[Q47](#q47) |
| [B02 Editor 缺少与复杂度匹配的持续验证入口](#b02) | [P11](#p11)、[P12](#p12) | native CTest / FakeArtifactStore / FakeViewHost | [M16](#m16) | [Q01](#q01)、[Q45](#q45)、[Q49](#q49)、[Q50](#q50) |
| [B03 平台资格、产品构建与安装消费者测试混杂](#b03) | [P05](#p05)、[P11](#p11)、[P12](#p12) | test_support / installed_consumer / desktop_integration | [M16](#m16) | [Q46](#q46)、[Q49](#q49) |
| [B04 全局 SDK 指纹扩大耦合且不能证明 ABI 兼容](#b04) | [P05](#p05)、[P10](#p10)、[P11](#p11) | InterfaceVersion / SdkBuildFingerprint / CodeLease | [M14](#m14)、[M16](#m16) | [Q46](#q46)、[Q48](#q48)、[Q52](#q52) |

### 21.1 一条完整的对应关系示例

**A02：SceneEditor 同时是 Pane、编辑内容和运行 owner。** 原则不是“拆小类”，而是 P01/P02/P09：先识别三种独立生命周期，再选择能够表达它们的拥有关系。

类型上，SceneSession 组合 SceneSource/SessionState/EditHistory，并实现窄 IEditSession；SceneView 继承真正的 UI Pane，以 typed key 或明确绑定状态关联 Session；RunSession 不继承也不拥有 SceneSession，只保留启动来源和独立实例责任。模块上，三者分别归 M04、M11、M08，共同依赖 M03 的身份契约，领域不反向依赖 UI。验收上，Q01 证明无窗口编辑，Q02 证明双视图共用内容，Q03 证明关闭视图不删内容，Q22 证明运行不改作者态。

这条链中任何一环缺失都不能算完成：若 SceneSession 仍由 SceneView 析构删除，类名和目录再符合文档，A02 仍然存在。其余问题应使用相同标准评审。

<a id="type-index"></a>
## 附录 A. 类型归属与关系索引

此表索引重要的框架级角色及其生命周期，不要求每个辅助名词都生成一个独立 .cpp 或 DLL。同一职责内部的微小实现类型可以保持 private；公开类型必须具有本文定义的独立语义。泛称和实际类名区分：SaveAsOperation 指一类用例，实际是两个领域操作；PresentationHub 指派生呈现责任，实际分为 ScenePresentationHub 与 MaterialPreviewStore。

| 类型/类型族 | 唯一归属 | 种类与关系 | 实际所有者/生存边界 | 不承担的职责 |
|---|---|---|---|---|
| SessionId / SessionKey&lt;T&gt; | M03 | 身份值；typed key 组合身份和类型契约 | 值可复制；每次解析验证代际 | 不拥有会话，不代替 AssetId |
| IEditSession | M03 | 窄多态接口；Scene/Material 实现 | SessionStore 拥有派生对象 | 不含 save/play/compile/Pane |
| SessionStore | M03 | 组合 unique_ptr&lt;IEditSession&gt; 与代际表 | 应用根；唯一会话拥有者 | 不执行资产 IO、创建窗口或菜单 |
| SessionAccess&lt;T&gt; / SceneSessionAccess | M03 的通用机制；M04 的别名 | 受限访问适配；组合服务引用 | 不长于 Store；借用不跨调度 | 不公开 adopt/erase/global Context |
| SessionState | M03 | 被具体会话组合的通用状态 | 会话生存期 | 不复制当前历史、选择或 SaveOperation |
| SourceBinding / BindingRevision | M03 | 来源值与来源版本 | SessionState | 不表示内容版本或存储实现 |
| PersistenceCheckpoint | M03 | 保存事实值；与 binding 共同解释 | SessionState 唯一修改路径 | 不调度保存，不与 History.saved 双写 |
| ContentStamp / ObservationVersion | M03/M02 | 作者状态定位/观察变化，语义分离 | 快照或通知拥有的值 | 不充当文件发布序号 |
| EditScope | M03 | 不可复制/移动的同步 RAII guard | 当前 owner 编辑作用域 | 不表达用户审阅或异步关闭 |
| ClosePermit | M03 | move-only 关闭许可 | CloseSessionsOperation 持有，Store 消费 | 析构不表示业务 CANCEL |
| BindingChangePermit | M03 | move-only 来源变更许可 | 领域 SaveAs 操作持有 | 不替代普通保存并发规则 |
| EditHistory | M02 | 组合历史记录；驱动编辑协议 | Scene/MaterialSession | 不拥有 UI 或保存目标 |
| EditOperation / PreparedEdit | M02 | 真实协议基类；领域操作派生 | 历史/单次准备记录 | 不读取焦点或全局 Editor |
| HistoryId / StateId / Revision | M02 | 复用已有强类型值 | 历史生成，快照复制 | 不需要重新发明另一套同义 ID |
| SceneSession | M04 | 继承 IEditSession；组合 Source/State/History | SessionStore | 不持有 Pane、Run、SaveOperation、GPU |
| SceneSource | M04 | 具体作者态存储 | SceneSession | 不包含编辑器相机/运行时临时字段 |
| SceneSnapshot / SceneReadView | M04 | 拥有的冻结值/短期只读借用 | 调用方值 owner/当前访问范围 | 不得互换来掩盖后台可变读取 |
| SceneEditBatch / SceneEditReceipt | M04 | 编辑输入/已提交结果 | 单次领域调用或历史记录 | 不是 UI 命令或文件保存结果 |
| MaterialSession / MaterialSource | M05 | 接口继承+具体组合；不继承 Scene | Store/MaterialSession | 不运行 shader 编译或 GPU 预览 |
| MaterialSnapshot / MaterialEditBatch | M05 | 冻结数据/具体编辑输入 | 捕获消费者/同步编辑 | 不成为万能 Scene variant 分支 |
| ISaveSource | M06 | 保存捕获与回执采用角色接口 | InstalledSession 配套注册拥有适配器 | 不拥有 Session，不打开文件对话框 |
| SceneSaveSource / MaterialSaveSource | M07 | 继承 ISaveSource；组合 typed access+codec | 会话角色安装记录 | 不转发到旧 SceneEditor |
| IEncodeJob | M06 | 冻结输入的异步工作接口 | SaveOperation 拥有 | 不写文件、不更新 Session |
| SceneEncodeJob / MaterialEncodeJob | M07 | 继承 IEncodeJob；组合领域 snapshot | 对应 SaveOperation；带代码保活 | const 输入不藏可写旁路输出 |
| FrozenSave / EncodedArtifact | M06 | 捕获记录/明确编码结果 | 保存阶段记录 | 不表示已落盘 |
| SaveService / SaveOperation | M06 | 服务组合有界操作记录；无万能 Operation 基类 | 应用根/SaveService | 不解析具体 ScenePackage 或改 UI |
| WriteLane / WriteTicket | M06 | 目标发布排序机制/准入票据 | SaveService 或协作写入端 | 不按历史 serial 猜写入先后 |
| IArtifactStore | M06 | 实际副作用接口 | 应用根注入给保存服务 | 不承诺后端没有实现的全局 CAS |
| ProjectArtifactStore / FakeArtifactStore | M07/测试支撑 | 继承 IArtifactStore | 装配根/测试夹具 | 不直接更新领域 dirty |
| CommitReceipt / SaveReceipt | M06 | 实际存储提交事实/关联捕获身份的业务回执 | 终态记录按保留策略持有 | 不等于 GPU 完成或当前内容 clean |
| SceneCodec / MaterialCodec | M07 | 具体编解码组件，可复用现有算法 | 无状态组件或受限资源 owner | 不控制窗口和会话集合 |
| SceneSaveAsOperation / MaterialSaveAsOperation | M07 | 具体长期用例；组合 permit/候选/发布能力 | 对应持久化服务的稳定操作集合 | 不要求共同基类，不静默清历史 |
| EncodedSceneClone / PreparedSceneRebind | M07 | 显式结果/预准备领域采用记录 | SceneSaveAsOperation | 不藏在 const 输入的 shared_ptr 中 |
| RunId / RunProvenance | M08 轻量 API | 运行身份/启动来源值 | RunStore/RunSession | 不等同作者 SessionId |
| RunStore / RunSession | M08 | 组合 owning 运行记录；不继承编辑会话 | 应用根/RunStore | 不由 Pane 析构隐式销毁 |
| RunController / StepTicket / StopTicket | M08 | 控制服务/实际操作票据 | Run 子系统 | 不创建第二套引擎驱动 |
| SceneInstanceLease | engine runtime；M08/M09 使用 | move-only 实例退休责任 | Run 或投影实例 owner | 不成为作者源或双重 runtime owner |
| RunInspectAccess | M08 轻量 API | 运行读取能力 | 运行服务长于其使用者 | 不提供通用 EngineContext getter/tick |
| SceneProjection / ScenePresentationHub | M09 | 作者到呈现的版本化投影/共享派生 owner | 呈现服务；可独立于 View 保留/释放 | 不接受作者编辑写入 |
| ViewportPresentation | M09 | 视图局部资源组合 | 具体 SceneView 或运行视图 | 不复制整个作者世界 |
| HighlightRenderer / HighlightKey | M09 | 派生阶段拥有者/输出对应键 | 视口呈现 | prepared 不能冒充 accepted |
| MaterialPreviewStore | M09 | 组合编译记录与版本化预览资源 | 呈现服务 | 不因编译失败撤销作者编辑 |
| SceneInteractionGroup / SceneSelection | M10 | 组合选择和 gesture 状态 | 交互服务；视图关联 | 不拥有 Session 或文件 dirty |
| EditedObjectRef / RunningObjectRef | M10 | 带来源的对象身份值 | 选择/编辑输入/观察快照 | 不能以裸 Entity 互换 |
| Gesture / MaterialInteraction | M10 | 交互过程或领域交互组合 | 交互组/视图绑定 | 不等于持久化作者内容 |
| SceneView / MaterialView / OutlinerView | M11 工具 UI | 继承 ui::Pane；组合视图状态和绑定 | ViewHost | 不继承 EditHistoryTarget 以兼任内容 |
| SceneViewBinding | M11 Scene UI | 未绑定/作者/运行的封闭 variant | SceneView | 不做全编辑器的万能对象 union |
| IViewHost / ViewHost / FakeViewHost | M11 API/desktop/测试 | 窄宿主接口及其实现 | 应用根/测试夹具 | 不解析领域内容或实现资产复用策略 |
| DetachedView / PreparedMountBatch | M11 | 离树拥有对象/局部挂载准备记录 | 工厂调用方/Host 批处理 | 不能已注册活动 Root 后仍叫 detached |
| ViewRequests / ViewInfo | M11 | 正常用户动作入口/只读快照 | 宿主与调用方 | 不暴露 owning 容器或任意 erase |
| SceneViewState / MaterialViewState | M11 对应 UI | 视图局部值 | 视图 | 不保存作者源或运行世界 |
| DockLayout / ValidatedLayout / LayoutPlan | M12 | 持久布局值/验证值/纯计划 | 调用用例拥有 | 不执行文件 IO、载入资产或关闭内容 |
| LayoutId / LayoutSlotId / ViewRestoreKey | M12；视图键在 view_api | 布局身份/槽位/持久视图匹配符号 | 持久数据及计划 | 不序列化本进程裸槽位当永久身份 |
| LayoutCatalog / UserPreferences | M12 | 目录快照/独立偏好值 | 工作区存储/应用设置 | 不共享一个 action/result 数据袋 |
| RecoveryManifest | M12 | 可恢复来源与视图绑定记录 | 恢复持久化模块 | 没有实际快照时不声称恢复未保存内容 |
| CommandDescriptor / CommandEntry | M13 | 描述/组合 query+execute+CodeLease | 不可变命令快照 | 不继承全局 Plugin 或拥有编辑源 |
| CommandQuery / CommandInvocation / DispatchReceipt | M13 | 查询输入/固定目标的意图/准入结果 | 单次查询或分发 | 不把 accepted 当 final success |
| ExtensionSnapshot / ExtensionPublisher | M14 | 不可变注册集合/受限发布者 | 扩展服务与调用者保活 | 不让普通 UI 边查询边替换容器 |
| CodeLease | 既有插件基础；M14 使用 | 代码寿命责任值 | 注册、对象、任务与产物 | 仅保活代码，不保证业务插件纯净或 ABI 兼容 |
| SessionLoadJob / PreparedSessionData | M14 契约；M07 实现 | 后台加载协议/拥有的解码结果 | OpenAssetOperation | 不在 worker 构造 owner-thread EditHistory |
| InstalledSession / SessionViewBindings | M15 | 配套角色注册/跨集合关联 | 用例服务 | 不成为第二份内容 owner |
| OpenAssetOperation / ReloadSessionOperation | M15 | 具体产品流程；拥有各自阶段 | 应用用例服务 | 不把所有字段放进 ApplicationState |
| CloseViewOperation / CloseSessionsOperation | M15 | 不同生命周期的关闭用例 | 应用用例服务 | 不将 discard/cancel/release 混为一个值 |
| SaveAllOperation / ApplyLayoutOperation | M15 | 跨模块选择与协调记录 | 应用用例服务 | 不取代单次保存或纯布局模型 |
| SceneToolModule / MaterialToolModule | M16 | 具体工具装配组件 | 应用启动/模块安装 | 不被业务当 services locator 传递 |
| EditorApplication / DesktopShell | M16/M11 | 装配生命周期根/平台呈现宿主 | 唯一产品入口 | 根不保存每个流程内部中间状态 |

### A.1 类型数与实现规模

上表包含现有类型的复用、角色族、别名和实现内部记录，不是要求按每行新建一个框架对象。能够保持事实类别和所有权的简单 struct、值成员或局部函数优先；只有跨真实替换边界才引入虚接口。名称统一后，禁止同时保留两个不同类型代表同一身份或同一保存基线。

<a id="semantic-mapping"></a>
## 附录 B. 旧语义如何替换：不是一对一改名

| 现有类型/调用形状 | 替代语义 | 类型/函数方向 | 关键行为变化 |
|---|---|---|---|
| SceneEditor : Pane + EditHistoryTarget | 作者内容、UI、运行独立 | SceneSession / SceneView / RunSession | 视图数量不决定作者内容数量 |
| MaterialEditor 同时管理模型与预览 | 作者材质与编译呈现独立 | MaterialSession / MaterialPreviewStore / MaterialView | 编译失败不改变合法作者编辑 |
| EditorContext& | 用例需要的最小能力 | 具体服务引用/受限访问 | 无全局引擎/窗口/注册可达性 |
| openAsset → Pane reference | 打开内容，再明确显示 | OpenResult{session, presentation} | 可以准确表示内容成功、显示失败 |
| PaneRegistration::create(PaneManager&) | 构造未发布 UI | ViewFactory → DetachedView | 不在工厂里自行采用/复用 |
| PaneManager::create/adopt/show 混合 | 分别准备、拥有、显示、聚焦 | ViewHost::adopt/show/requestFocus | 无半注册引用结果 |
| WorkspaceRequest | 命令/查询/完成/内部状态分开 | ApplyLayoutIntent、LayoutCatalog、LayoutApplyReport | STATUS 不再是内部 IO 阶段 |
| WorkspaceData | 不同数据的持久化边界 | DockLayout / RecoveryManifest / UserPreferences | 布局不携带作者恢复语义 |
| CloseRequest::CANCEL 清理准备 | 业务取消 vs 资源许可释放 | UserCloseChoice::Cancel / permit 析构 | 成功关闭后不发送语义错误的 Cancel |
| prepareExit/cancelExit 到处传播 | 根用例审阅与生命周期许可 | CloseSessionsOperation + ClosePermit | 不依赖多个工具各自维护同一退出状态机 |
| valid/invalid 控制时间 | 明确运行控制 | pauseSimulation/resumeSimulation | 存在性查询与动作分开 |
| safe() 后长期用 Registry | 有时限的读写借用 | read/borrow + owner 阶段契约 | 一次有效性检查不保证跨 tick 后仍有效 |
| inspectedScene() 隐式切播放分支 | 显式绑定来源 | EditedSceneBinding / RunningSceneBinding | 对象和修改路由不靠全局播放 bool 猜测 |
| content 与 content_ | 领域源与视图/呈现 | SceneSource / SceneViewState / SceneProjection | 不同名词对应不同 owner |
| save.index()!=0 | 业务准入或操作查询 | SaveAvailability / SaveStatus | 不依赖 variant alternative 的位置含义 |
| single_step + baseline | 一次实际步进请求 | StepTicket | 只有对应仿真完成才终态成功 |
| SceneSaveCapture::copied | 明确编码与身份映射产物 | EncodedSceneClone | 不通过 const 输入写共享旁路输出 |
| EditingGuard(bool&) | 实际不可重入编辑许可 | EditScope | 不可复制；进入失败不能先把 busy 改掉 |
| finishSave 标记当前 clean | 采用被保存状态的回执 | accept(SaveReceipt) | S10 完成不把 S12 清成 clean |
| save callback 判断谁新 | 物理发布排序+回执排序 | WriteTicket + CommitReceipt | 既防磁盘旧覆盖，也防基线回退 |
| query/execute 共用可变 Command | 查询与动作准入分离 | CommandQuery / CommandInvocation | 查询期间不修改正在读取的注册集合 |
| code_lifetime: shared_ptr&lt;const void&gt; | 显式代码责任 | CodeLease | 覆盖闭包、虚析构、deleter 与异步产物寿命 |
| highlighted_* 先写后捕获 | 输出提交阶段分开 | desired/prepared/accepted key | 捕获失败可以重试，背压不丢候选 |
| fail() 用于创建与运行 | 构造结果 vs 已运行故障 | create→Result / running failure channel | 不发布创建失败但 Result 成功的应用 |
| 架构标签与本机测试脚本 | 实际编译约束和分层执行 | target allowlist / native / desktop / installed | clean consumer 和负向依赖检查进入 CI |

<a id="references"></a>
## 附录 C. 固定源码与技术参考

源码 S00—S33 继承首轮调查的固定提交证据；S34—S36 为本轮补核的历史/编辑机制。文中的新设计是方案，不从指南或现有代码推定已经实现。链接不带未经验证的行号，定位以文件和符号为准。

| 引用 | 文件/说明 | 本文主要用途 |
|---|---|---|
| [S00] | `基准提交` | 固定源码基准与提交说明；不将提交自述视作本次执行结果 |
| [S01] | `editor/app/pinclude/lux/engine/editor/detail/EditorImpl.hpp` | 对应第 2 章事实、控制流或架构判断 |
| [S02] | `editor/app/src/Editor.cpp` | 对应第 2 章事实、控制流或架构判断 |
| [S03] | `editor/app/src/EditorStartup.cpp` | 对应第 2 章事实、控制流或架构判断 |
| [S04] | `editor/app/src/EditorMenu.cpp` | 对应第 2 章事实、控制流或架构判断 |
| [S05] | `editor/app/src/EditorWorkspace.cpp` | 对应第 2 章事实、控制流或架构判断 |
| [S06] | `editor/app/src/EditorWorkspaceStorage.cpp` | 对应第 2 章事实、控制流或架构判断 |
| [S07] | `editor/app/pinclude/lux/engine/editor/detail/EditorWorkspace.hpp` | 对应第 2 章事实、控制流或架构判断 |
| [S08] | `editor/context/include/lux/engine/editor/EditorContext.hpp` | 对应第 2 章事实、控制流或架构判断 |
| [S09] | `editor/context/src/EditorContext.cpp` | 对应第 2 章事实、控制流或架构判断 |
| [S10] | `editor/context/include/lux/engine/editor/PaneManager.hpp` | 对应第 2 章事实、控制流或架构判断 |
| [S11] | `editor/context/src/PaneManager.cpp` | 对应第 2 章事实、控制流或架构判断 |
| [S12] | `editor/metadata/include/lux/engine/editor/metadata/PaneRegistration.hpp` | 对应第 2 章事实、控制流或架构判断 |
| [S13] | `editor/metadata/include/lux/engine/editor/metadata/CommandRegistration.hpp` | 对应第 2 章事实、控制流或架构判断 |
| [S14] | `editor/context/include/lux/engine/editor/WorkspaceRequest.hpp` | 对应第 2 章事实、控制流或架构判断 |
| [S15] | `editor/editing/include/lux/engine/editor/CloseRequest.hpp` | 对应第 2 章事实、控制流或架构判断 |
| [S16] | `editor/editing/include/lux/engine/editor/AssetEditing.hpp` | 对应第 2 章事实、控制流或架构判断 |
| [S17] | `editor/editing/include/lux/engine/editor/EditorError.hpp` | 对应第 2 章事实、控制流或架构判断 |
| [S18] | `editor/tools/scene/include/lux/engine/editor/scene/SceneEditor.hpp` | 对应第 2 章事实、控制流或架构判断 |
| [S19] | `editor/tools/scene/pinclude/lux/engine/editor/scene/detail/SceneEditorImpl.hpp` | 对应第 2 章事实、控制流或架构判断 |
| [S20] | `editor/tools/scene/src/SceneAssets.cpp` | 对应第 2 章事实、控制流或架构判断 |
| [S21] | `editor/tools/material/src/MaterialAssets.cpp` | 对应第 2 章事实、控制流或架构判断 |
| [S22] | `engine/scene/composition/include/lux/engine/scene/SceneRuntime.hpp` | 对应第 2 章事实、控制流或架构判断 |
| [S23] | `engine/scene/composition/src/SceneRuntime.cpp` | 对应第 2 章事实、控制流或架构判断 |
| [S24] | `modules/function/ui/include/lux/engine/ui/Pane.hpp` | 对应第 2 章事实、控制流或架构判断 |
| [S25] | `modules/function/ui/src/Root.cpp` | 对应第 2 章事实、控制流或架构判断 |
| [S26] | `modules/function/ui/include/lux/engine/ui/Ids.hpp` | 对应第 2 章事实、控制流或架构判断 |
| [S27] | `editor/ui/CMakeLists.txt` | 对应第 2 章事实、控制流或架构判断 |
| [S28] | `editor/app/CMakeLists.txt` | 对应第 2 章事实、控制流或架构判断 |
| [S29] | `cmake/TargetClassification.cmake` | 对应第 2 章事实、控制流或架构判断 |
| [S30] | `CMakeLists.txt` | 对应第 2 章事实、控制流或架构判断 |
| [S31] | `.github/workflows/deb.yml` | 对应第 2 章事实、控制流或架构判断 |
| [S32] | `editor/tools/scene/test/workspace_checks.hpp` | 对应第 2 章事实、控制流或架构判断 |
| [S33] | `cmake/LuxPlugins.cmake` | 对应第 2 章事实、控制流或架构判断 |
| [S34] | `editor/editing/include/lux/engine/editor/editing/EditHistory.hpp` | 本轮重新核对，防止把已有机制误判为缺失 |
| [S35] | `editor/editing/include/lux/engine/editor/editing/EditOperation.hpp` | 本轮重新核对，防止把已有机制误判为缺失 |
| [S36] | `editor/editing/include/lux/engine/editor/editing/EditTypes.hpp` | 本轮重新核对，防止把已有机制误判为缺失 |
| [R01] | C++ Core Guidelines | 精确接口、不变量和真实接口继承方向 |
| [R02] | CMake target_link_libraries 官方文档 | 使用需求、链接范围的真实含义 |
| [R03] | CMake buildsystem 官方手册 | target 与传递依赖边界 |

### 文档与实施的界线

本次交付仅为源码证据支持的设计与指导，没有修改仓库，没有执行引擎编译、CTest、安装消费者或 GPU 验证。接口草图需经实现与本文验收矩阵验证后才能成为稳定 SDK；任何性能与兼容性结论均需独立实测。

**最终目标不是让旧框架看起来更整洁，而是消除旧概念强迫维护者遵守的隐含规则：内容不属于窗口，运行不等于作者源，准备不等于提交，目录不等于模块，类型名称不代替真实所有权。**

[S00]: https://github.com/LUX-YU/lux-engine/commit/2bb33ff1a1f11025cf404e074c0e9b259239d8a4 "基准提交"
[S01]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/pinclude/lux/engine/editor/detail/EditorImpl.hpp "editor/app/pinclude/lux/engine/editor/detail/EditorImpl.hpp"
[S02]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/src/Editor.cpp "editor/app/src/Editor.cpp"
[S03]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/src/EditorStartup.cpp "editor/app/src/EditorStartup.cpp"
[S04]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/src/EditorMenu.cpp "editor/app/src/EditorMenu.cpp"
[S05]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/src/EditorWorkspace.cpp "editor/app/src/EditorWorkspace.cpp"
[S06]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/src/EditorWorkspaceStorage.cpp "editor/app/src/EditorWorkspaceStorage.cpp"
[S07]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/pinclude/lux/engine/editor/detail/EditorWorkspace.hpp "editor/app/pinclude/lux/engine/editor/detail/EditorWorkspace.hpp"
[S08]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/context/include/lux/engine/editor/EditorContext.hpp "editor/context/include/lux/engine/editor/EditorContext.hpp"
[S09]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/context/src/EditorContext.cpp "editor/context/src/EditorContext.cpp"
[S10]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/context/include/lux/engine/editor/PaneManager.hpp "editor/context/include/lux/engine/editor/PaneManager.hpp"
[S11]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/context/src/PaneManager.cpp "editor/context/src/PaneManager.cpp"
[S12]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/metadata/include/lux/engine/editor/metadata/PaneRegistration.hpp "editor/metadata/include/lux/engine/editor/metadata/PaneRegistration.hpp"
[S13]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/metadata/include/lux/engine/editor/metadata/CommandRegistration.hpp "editor/metadata/include/lux/engine/editor/metadata/CommandRegistration.hpp"
[S14]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/context/include/lux/engine/editor/WorkspaceRequest.hpp "editor/context/include/lux/engine/editor/WorkspaceRequest.hpp"
[S15]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/include/lux/engine/editor/CloseRequest.hpp "editor/editing/include/lux/engine/editor/CloseRequest.hpp"
[S16]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/include/lux/engine/editor/AssetEditing.hpp "editor/editing/include/lux/engine/editor/AssetEditing.hpp"
[S17]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/include/lux/engine/editor/EditorError.hpp "editor/editing/include/lux/engine/editor/EditorError.hpp"
[S18]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/include/lux/engine/editor/scene/SceneEditor.hpp "editor/tools/scene/include/lux/engine/editor/scene/SceneEditor.hpp"
[S19]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/pinclude/lux/engine/editor/scene/detail/SceneEditorImpl.hpp "editor/tools/scene/pinclude/lux/engine/editor/scene/detail/SceneEditorImpl.hpp"
[S20]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/src/SceneAssets.cpp "editor/tools/scene/src/SceneAssets.cpp"
[S21]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/material/src/MaterialAssets.cpp "editor/tools/material/src/MaterialAssets.cpp"
[S22]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/engine/scene/composition/include/lux/engine/scene/SceneRuntime.hpp "engine/scene/composition/include/lux/engine/scene/SceneRuntime.hpp"
[S23]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/engine/scene/composition/src/SceneRuntime.cpp "engine/scene/composition/src/SceneRuntime.cpp"
[S24]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/modules/function/ui/include/lux/engine/ui/Pane.hpp "modules/function/ui/include/lux/engine/ui/Pane.hpp"
[S25]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/modules/function/ui/src/Root.cpp "modules/function/ui/src/Root.cpp"
[S26]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/modules/function/ui/include/lux/engine/ui/Ids.hpp "modules/function/ui/include/lux/engine/ui/Ids.hpp"
[S27]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/ui/CMakeLists.txt "editor/ui/CMakeLists.txt"
[S28]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/CMakeLists.txt "editor/app/CMakeLists.txt"
[S29]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/cmake/TargetClassification.cmake "cmake/TargetClassification.cmake"
[S30]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/CMakeLists.txt "CMakeLists.txt"
[S31]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/.github/workflows/deb.yml ".github/workflows/deb.yml"
[S32]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/test/workspace_checks.hpp "editor/tools/scene/test/workspace_checks.hpp"
[S33]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/cmake/LuxPlugins.cmake "cmake/LuxPlugins.cmake"
[S34]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/include/lux/engine/editor/editing/EditHistory.hpp "editor/editing/include/lux/engine/editor/editing/EditHistory.hpp"
[S35]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/include/lux/engine/editor/editing/EditOperation.hpp "editor/editing/include/lux/engine/editor/editing/EditOperation.hpp"
[S36]: https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/include/lux/engine/editor/editing/EditTypes.hpp "editor/editing/include/lux/engine/editor/editing/EditTypes.hpp"
[R01]: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines "C++ Core Guidelines"
[R02]: https://cmake.org/cmake/help/latest/command/target_link_libraries.html "CMake target_link_libraries"
[R03]: https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html "CMake buildsystem manual"
