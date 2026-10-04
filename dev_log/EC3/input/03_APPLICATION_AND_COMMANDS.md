# EC3-B：准确的业务 owner、中性命令路径与生命周期 Application

## B1. 固定边界

EditorApplication 类的核心职责是产品生命周期与具体装配：创建、激活、主循环、请求退出、停止新工作、结清已接受工作、销毁顺序和最终错误出口。

application 层仍可以承载真正跨内容、工作台和平台的用例，但它们不必都是 EditorApplication::Impl 的方法和成员。

持有 ProjectStorage 不等于实现项目保存政策；启动时恢复工作台不等于在 Application 中实现布局目录政策；安装命令不等于成为全部命令 receiver。

禁止把所有 Impl 字段搬进 `EditorServices/EditorController/Context2` 后继续让每个功能捕获它。只迁有完整输入、结果和唯一责任的用例。

## B2. 现有命令不是两套系统

当前菜单通过 CommandMenu→CommandDispatcher→CommandRegistry；程序化 facade 通过 Application::execute→同一 CommandRegistry。[S07,S08,S09]

保留这一点：命令描述、query、execute、拥有型 invocation、PINNED／CURRENT_REGISTRATION、代码 pin 和 publication Batch 是已经存在的真实能力。

删除的是下面这些外围问题，而不是重新造 ICommand 层：

- Application::execute 对 `AcceptedOperation.kind == "save"` 的解释。
- 菜单 takeCompletions 后同样的 save 特判。
- save() 已登记，外层又登记 pending/results 的分散责任。
- 先安装基础 save，再按名字 erase，最后安装项目 save 的替换政策。
- Scene role 八字符串既生成 ID，又在 callback 里解释一次，再在 showSceneTool 解释一次。
- 大量命令 capture 整个 Impl，而不是其实际所需 provider。

## B3. 项目保存迁移——不是把原 SaveService 再包一层

### 当前迁出位置

`editor/application/src/EditorSaving.cpp` 的 prepareSave、save、settleSaves 中关于物理路径、AssetId、项目源登记、manifest 发布和采用的算法；以及 Impl 中与这些结果相关的记录。[S10]

### 目标归属

在 `editor/activities/project` 的真实项目活动 target 中形成 `ProjectContentSaving`（目标名称）。如已有等价活动，扩充它而不是新增同义类。

该类型拥有的是**源保存加项目登记这一复合活动**，不拥有作者 Session，不复制 SaveService 的编码状态机，不复制 WriteCoordinator。以原 SaveId 或现有可辨别的复合操作身份关联记录，不再建第二份 source outcome。

它的依赖是原 SessionStore／保存角色、SaveService、ProjectStorage、WriteCoordinator 及执行接线；不得依赖 Pane、ReviewView、EditorApplication 或 CommandInvocation。

### 必须迁入的政策

| 算法／状态 | 处理 |
|---|---|
| source 规范名、版本和默认后缀选择 | 读取已有 factory 的 SourceAuthoring；不得恢复三类型 switch |
| Save As 目标存在／其他资产占用验证 | 在实际物理解析后验证；路径文本 hash 不能替代物理身份 |
| AssetId 分配 | 仍一次分配并固定于实际请求；重试不得改号 |
| 源已发布但目录登记失败后的恢复 | 保留原物理绑定和部分完成事实，不猜当前目录 |
| manifest 基于最新记录形成候选 | 保留当前 compiled 信息；不能让旧完成覆盖较新字段 |
| 文件冲突、Unknown、清理失败 | 分别保存，不能降格为普通 BUSY 或成功 |
| report/pending/acknowledge | 活动准入成功时即建立完整责任，不靠命令回执补登记 |
| SaveAll | 固定 Session 集合，包含无视图工作副本；源和项目登记分别有结果 |
| Close／Exit 借阅结果 | 与原结果 owner 协调，不由两个地方重复 acknowledge |

### 留在 UI／产品组合的内容

ReviewView 的路径输入、用户回答、关闭 review Pane 与取消临时交互。UI 得到保存请求数据后调用同一个 ProjectContentSaving。无 Root 消费者直接调用同一 API。

命名／overwrite 的用户决定与文件实际许可分开。UI 较早说“可以覆盖”不能取消真正发布前的物理文件版本检查。

普通 Save 的目标政策应按 C0 冻结的原行为保留：会话目标固定；当前内容在正式准入时确定。明确携带 based_on 的请求不得偷偷丢弃或重绑定。不同入口存在语义差异时先统一为有明确名称的两种请求，不默改现有测试。

Export Copy 不采用作者 checkpoint；Save As 不清 History；编译成功不等于源已保存。原 P05／P11 的可靠完成接收与清理保护一并继承。

## B4. 工作区、恢复与最近项目

原 WorkspaceStore 继续负责文件、版本、迁移字节和 WriteCoordinator 接入。实际 Host 布局准备／提交仍在工作台。

迁出 `EditorWorkspace.cpp` 的 save/rename/delete/fallback/selected-layout/迁移推进政策，到现有 workspace 活动；需要 Host 的完整工作台组合放 `workbench/desktop` 的 `WorkspaceActions`（目标角色），不让 activities 反向依赖 UI。

内容恢复只读取 RecoveryManifest，不解析任意 Layout opaque 去打开资产。跨内容打开与 Host rebind 的协调可为 application 中独立 `RestoreWorkbench` 用例，Application 只启动、驱动、等待它。

保留两个事实：布局在 Host 提交成功；selected_layout 文件写入可能失败。不能为简化事务回滚已真实应用的 UI，也不能把后者成功假定为前者成功。

最近项目是用户级文档，不是通用 EditorApplication 私有状态。将读取／去重／上限／发布放可复用的项目浏览活动；RecentPane 不持 Impl&。已有同名 activity 时复用，禁止增加全能 PreferencesManager。

ProjectCreation、ModelImporter、ArtifactPublicationOperation 已有真实 owner，直接使用它们，不再次迁出同一算法。Application 中仍有未迁政策时以函数与调用清单定位，不按文件名猜测全部过时。

## B5. 项目插件选择

`ProjectManifest.plugins` 继续是项目启用选择的唯一权威。通过 `ProjectPluginSelection` 这项项目活动（目标角色）复用 ProjectPublicationOperation，负责草稿基线、发布、重试、放弃和结果。

新设置中心可以展示它，但不得再保存另一份 `plugins_enabled` 到用户设置文件。当前激活集合与下次启动选择不同，要明确显示需重新打开项目。

运行期插件和 Editor 扩展加载继续用原 PluginLibrary／EditorExtension。不是因为增加设置就重做装载器或默认允许热卸载。

## B6. 模块内声明及安装职责

固定描述由真正功能模块声明；有状态绑定函数接收准确 receiver，不接收 EditorApplication&。下表是默认归属，不等于必须新增相同数量的 target：

| 命令组 | 声明／绑定所在主题 |
|---|---|
| Undo/Redo、无项目基础源保存 | activities/sessions 的实际角色主题 |
| 项目 Save/Save As/Export/SaveAll/Reload | activities/project 的保存活动；命名 UI 接线在工作台 |
| Close View／Another View | workbench/desktop 的内容视图动作 |
| Scene 四工具 | workbench/scene；直接绑定 ESceneTool |
| Play/Pause/Resume/Step/Stop | 原 Run activity + 工作台控制的明确组合 |
| 新 Scene/Material/Flow | 各领域的 preparation provider 与视图创建接线 |
| Assets/Tasks/Import/Settings/Results/Recent | 各自工具模块的视图与命令贡献 |
| 工作区与恢复 | workspace 工作台主题／独立恢复用例 |
| Exit、关于产品信息 | application 的生命周期／产品声明 |

跨功能组合可以在 application 中保留一个显式装配函数，但它只拼接模块贡献，不逐个认识其 command_id，不把所有描述再抄一份。

基础源 Save 和项目 Save 使用**同一稳定声明**，由装配选择一个语义明确的 receiver；不得两者先装上再删一个。无项目消费者仍有基础保存能力，产品使用项目保存能力。不要为避免重复 ID 删除合法的低层 API。

## B7. 消除字符串二次解释

Scene 的四工具与四运行控制分别形成声明组。showSceneTool 接收原 `ESceneTool`，不再接受任意 role string；删除未知字符串自动落到 CONFIGURATION 的回退。

Pause／Resume／Step／Stop 直接绑定具名 handler 或已有封闭枚举。Save 三模式仍可用 `{descriptor, ESaveMode}` 的表；Undo/Redo 用两个具名条目共享原算法。不是每个有限模式都需要扩展接口。

动态 `lux.editor.tool/<ViewTypeId>` 在贡献加载时形成一次，使用动态 backing 与数值目录；后续每帧不重复拼字符串。标题、group 与 ViewTypeId 的复用遵守寿命，不缓存插件原始临时字符指针。

`AcceptedOperation` 可以继续是对既有业务结果的观察，但 canonical kind 需要进入既有稳定身份体系。不能只把 `"save"` 换成 hash 常量后继续在 Application 特判同一业务。

结果展示按实际活动提供的观察与准确操作构造；Activity 是结果 owner，ResultsView 不是另一个 OperationRegistry。不得引入一个知道所有活动的全局结果取消器。

## B8. 程序化入口和菜单入口

原 `EditorApplication::execute()` 的消费者是迁移对象：Application 测试、installed editor-application consumer、骨骼插件 APP 路线和其他实际调用者。C0 必须核对全部真实引用。

最终 Application 不再解释或执行业务命令。程序化调用方取得正式 CommandRegistry/Dispatcher 或产品组装返回的窄 command capability；菜单使用同一 Registry。需要直接保存的 C++ 工具可以绕过命令模式调用活动 API，但不能绕过领域准入。

如果一个中性 facade 确实简化 product API，可暂保留同名 execute，但必须只有 owner/phase 准入和原 Registry 转调，无字符串特判、业务 state 或结果登记；账本明确其理由与实际消费者。默认目标是迁出而非保留空转发，不能以 facade 为借口新增 RuntimeDispatcher 等层。

WRONG_THREAD 与 BUSY 分类继续准确。不要为了减少重复判断让外线程读到 owner 私有状态，也不要删除 query→execute 间因状态可能变化而必要的检查。

PINNED 句柄保持原语义；CURRENT_REGISTRATION 使用当前目录但验证兼容版本。拒绝不消费 invocation；BUSY 队首保持原目标；回调入队进入后批。

## B9. 生命周期最终结构

Application 持有按实际依赖顺序构造的产品对象，驱动具体 owner 的 update，不使用字符串调度各子系统。

退出阶段仍必须：拒绝新请求 → 收集用户决定 → 提交真正许可 → 停止新任务／运行 → 持续收集已接受完成 → 关闭视图 → 等待原资源退休 → 撤销贡献 → 释放引擎与平台。

实际顺序按原资源依赖调整，不能照抄上面文字强迫先后；需要保留真实 safe point 和 GPU 引用。重点是代码 lease 不早于 callback/payload/deleter，任务不因 UI 消失而丢结果。

不允许一种新活动在每帧必须遍历另一个活动的全部私有记录。用明确的公共结果／版本／通知；现有 TaskMonitor、LuxObject 与 Process 完成链继续复用。

完成判据是独立使用和唯一责任，不是 EditorApplication.cpp 低于多少行。禁止将业务体改名后放 `EditorApplicationInternalServices.cpp` 并继续依赖整个 Impl。
