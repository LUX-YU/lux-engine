# EditorApplication：职责、设置与命令链专项调查

日期：2026-10-03  
仓库：LUX-YU/lux-engine  
核查分支：codex/editor-redesign-v4  
固定 HEAD：126f1b4df14316df30957208787ded9be9f9f461

## 0. 结论与证据边界

三个疑问都有实际代码依据，但需要准确区分：

1. Application 为管理生命周期而持有服务是合理的；亲自实现项目保存、布局选择、配置发布等政策则不是单纯生命周期职责。
2. 构造参数与默认值没有问题。当前不足是尚未形成窗口、字体、插件设置等从持久数据到生效对象的统一链路。不是把构造函数改成直接读配置文件就能解决。
3. 项目已有真实命令注册、查询、捕获、排队与执行机制。问题不在于没有为每个命令写一个虚类，而在于 Application 仍是很多命令的具体业务接收者，并在通用返回链上特判保存。

这是代码与已给项目规范的定向审阅，没有运行 Editor、编译、CTest、GPU、SDK、输入或性能测量。已查到的调用方不是全仓 AST 穷尽调用图，更不覆盖仓库外的用户代码。

EC2 R1 的限定通过保持；本报告不是 EC2 R2，也不自动启动新实施阶段。已延期的原生输入以及既有免验、未测范围保持原判定。

## 1. 当前 Application 的真实内容

### 1.1 合理的生命周期与装配

- 创建并管理平台窗口、引擎上下文、消息派发和桌面宿主。
- 选择具体提供者，加载本产品的内置及扩展贡献。
- 在唯一主循环中推进完成、输入、安全点、Runtime 帧和退休。
- 请求退出，保持已接受操作的完成路径，按依赖顺序销毁。
- 建立产品级错误出口与外层 owner/重入准入。

这些责任不需要全部抽象成插件，也不要求 App 不再持有任何服务引用。[S02,S05,S06]

### 1.2 不应继续留在同一个 Impl 中的业务政策

| 当前位置 | 具体行为 | 建议去向 |
|---|---|---|
| EditorSaving.cpp::prepareSave | 项目相对路径、物理目标身份、资产登记冲突、新 AssetId、源格式记录 | activities/project 的正式项目内容保存活动 |
| EditorSaving.cpp::settleSaves | 根据源发布回执更新 manifest；准备、发布、采用目录；管理两层回执 | 同一项目保存活动；组合原 SaveService/ProjectStorage/WriteCoordinator |
| EditorSaving.cpp::askSave/receiveSaveAnswer | Save As 表单、回答保留与重试、结束预览的交互政策 | workbench 的保存交互；业务输入交给活动 |
| EditorWorkspace.cpp::executeWorkspaceIntent | 保存/读取/命名/删除布局、选择写回、迁移与恢复政策 | 纯 IO/政策归 activities/workspace，带 Host 的组合归 workbench |
| EditorWorkspace.cpp::applyLayout | 贡献快照下准备工厂输入并调用 Host 批量提交 | 工作台的布局应用能力；不让 activities 反向依赖 Host |
| EditorSettings.cpp::maintainProjectSettings | 插件选择草稿比对、ProjectUpdate、发布、重试、确认 | 项目插件选择活动；原设置面板成为特定页面 |
| EditorLifecycle.cpp 中逐类检查结果 | 手动解释保存/项目/运行等所有记录是否结束 | 生命周期只询问真实 owner 的停止/待完成/已排空事实；不复制状态 |

EditorApplication 这个类与 application 这一层不是同一个概念。确实需要组合内容、窗口和平台的流程，可以作为这一层中独立的用例存在；不需要全部成为 EditorApplication::Impl 成员。

已有 ViewHost 批量准备/提交与原 WorkspaceStore 文件 codec 不重做。当前 App 不是第二套 dock 算法，但它确实仍承载外围工作区政策。[S04,S07,S08,S09]

### 1.3 保存路径为什么不是“仅仅编排”

当前 prepareSave 会从会话对应的 SessionFactory 取得开放的 source 描述。它已经不是旧的 Scene/Material/Flow 三分支；这项 EC1 成果应保留。

但它还验证目的路径是否缺失、逐条解析现有资产的物理路径以避免所有权冲突、生成新 AssetId、恢复曾发布但未进入目录的保存绑定。settleSaves 随后亲自发布 manifest，并构造 ProjectPublicationReceipt 采用结果。[S07,S08]

这些政策在无窗口批处理和插件工具中同样需要。若只能通过 EditorApplication 才能得到完整项目保存，就还未达到共享基础能力的目标。

目标不是新增第二保存器。应当将“源文件保存 + 项目登记”的组合归入现有活动主题，继续复用原保存与文件发布算法，保留部分成功、Unknown、来源戳、较新 cooked 信息与回执确认语义。

## 2. 配置现状：有持久化，但不是通用设置系统

### 2.1 启动参数

EditorApplicationConfig 当前包括 project_file、installation、title、width=1440、height=900、offscreen、可选 font、可选 user_directory。公开定义没有 fullscreen 字段。[S01]

正式 main 解析 project 和 font；installation 来自 executablePath；随后构造 Config 并传给 create。已读 main/create/assemble 链没有将窗口大小、显示模式和字体从通用 Editor 设置文件解析进来。[S02,S03]

这表示：可以通过 C++ 调用方覆盖窗口大小，通过 --font 指定本次字体；不是只能修改源码才可变。但当前链路没有完成这些偏好的跨次持久化与设置面板。

### 2.2 已存在的持久化

| 数据 | 当前实现 |
|---|---|
| 布局 | .lux/workspace/layouts/<id>.layout；encodeLayout/decodeLayout |
| 工作区偏好 | .lux/workspace/preferences.toml；encodePreferences/decodePreferences |
| 内容恢复清单 | .lux/workspace/recovery.toml；encodeRecovery/decodeRecovery |
| 项目插件选择 | ProjectManifest.plugins，使用原 ProjectPublicationOperation |
| 最近项目 | 用户目录下 lux/editor/recent-projects.toml 的现有独立路径 |

UserPreferences 实际字段为 schema、selected_layout、opaque、legacy_origin。opaque 保留未知数据，不等于已有设置元数据、验证、作用域解析和运行应用机制。[S10,S11]

尤其要注意，WorkspaceStore 以 project_root 为根；名为 UserPreferences 的这份记录实际位于项目内，并不是所有项目共享的用户首选项文件。

### 2.3 当前 SettingsView 的真实功能

当前 SettingsView 显示插件列表，编辑选择并将它发布到项目；界面明确说明下次打开项目生效。它没有窗口、全屏、字体、快捷键等通用分组，也没有动态注册设置页面的完整机制。[S09,S12]

ContributionDraft 的 configurations 当前是 scene::ConfigurationEditor。ConfigurationEditor 包含配置值描述和 Element 工厂；这是可复用的控件/类型基础，不是通用 Editor 设置系统的完成证明。[S13,S14]

### 2.4 建议的设置责任分配（目标，未实现）

```text
持久设置文档 + 注册的设置描述 + 本次启动覆盖
    → 读取/迁移/验证/作用域解析
    → 固定有效设置
    → 对应功能提供者准备并采用
    → 设置页面显示期望、实际应用与持久化结果
```

建议保留以下语义角色，而不是规定一角色一类/一文件：

- 启动选项：项目、配置根、安装位置、测试/嵌入模式和命令行覆盖。
- 持久设置值：有 schema/version 的纯值与未知扩展数据。
- 设置描述：类型、默认值、合法范围、允许作用域、更新政策。
- 解析与存储：复用已有 codec/反射、平台路径与 WriteCoordinator。
- 应用行为：由真正的窗口、字体、输入、工具或引擎能力 owner 承担。
- 设置工作台：显示/修改草稿，发出请求，不是唯一配置构建器。

### 2.5 作用域与更新

不要对所有设置采用一个没有例外的全局覆盖顺序。每组设置声明允许哪些来源：

- 窗口/字体/个人快捷键通常属于用户层；项目不应悄悄覆盖它们。
- 团队共享的作者默认、项目插件选择属于项目层。
- 当前机器工具路径或窗口工作状态可以是用户的项目覆盖。
- CLI/测试覆盖仅影响本次执行，不能自动写回持久配置。
- 场景中的 RenderFeature 选择仍是作者资产配置，不因出现在一个设置中心就迁入全局偏好。

改变设置时，分别记录期望值、当前生效值、写入结果。字体/GPU 更新可能需要准备与安全点；插件启用可以继续下次启动生效。可注册新设置不意味着必须支持任意代码热卸载。

启动顺序分为内置引导设置和扩展设置：先定位配置并解析创建窗口所需的稳定内置部分，再加载获准扩展、注册其 schema 并解释扩展段。不要为了读取插件启用列表，先要求插件自己已经激活。

缺少文件可以用默认值；权限、损坏和不支持版本不能自动当成“第一次启动”并覆盖。插件缺失时保留其未知段；配置应用失败不伪装成已生效。下层引擎保持自己的类型和合法性验证，不链接 Editor 的设置 UI。

## 3. execute 的含义和真实调用者

### 3.1 exec 与 execute 是两件事

- exec()：运行整个 Editor 主循环，直到 RELEASED。
- execute(CommandId, CommandInvocation)：程序化调用一个已注册命令，返回派发结果。

execute 不代表“所有业务逻辑都应放在 Application”。APPLICATION scope 也只是目标范围，不要求以 EditorApplication 类作为命令接收者。[S01,S06,S15]

### 3.2 已有真实命令基础

CommandEntry 拥有描述、query 与 execute callable；CommandInvocation 保存目标/参数；CommandRegistry 查找、验证、固定代码寿命，并先 query 再执行；CommandDispatcher 处理有界排队；CommandMenu 将菜单/快捷键接到该机制。[S15,S16,S17]

不需要为每个命令建立一个虚类。闭包捕获实际业务接收者可以是正常的命令表达。问题在接收者是否恰当，而不是有没有 ICommand 基类。

### 3.3 两条正式链路

```text
程序化调用：
调用方 → EditorApplication::execute
       → Application owner/phase/gate
       → CommandRegistry 查找与 execute
       → 条目回调 → 具体接收者

菜单/快捷键：
Root → CommandMenu::receive
     → 捕获目标和参数 / query
     → CommandDispatcher::enqueue
     → CommandMenu::update / drain
     → CommandRegistry::execute
     → 条目回调 → 具体接收者
```

菜单并不需要绕道公开的 App.execute。[S04,S16,S17]

### 3.4 已核查的调用者

| 来源 | 实际用途 |
|---|---|
| editor/application/main.cpp | 调用 exec()，不是逐条业务 execute() |
| editor/tests/integration/application/application.cpp | 直接执行新建、设置、项目等命令及 owner/reentry 测试 |
| cmake/installed-consumers/editor-application/main.cpp | 外部 SDK 调用与 WRONG_THREAD 分类 |
| cmake/installed-consumers/editor-ec1-skeleton/main.cpp 的 APP 路线 | 插件 rename、Undo/Redo、Save/SaveAs、Reload、恢复 |
| 同骨骼消费者的非 APP 路线 | 直接使用 CommandRegistry、Session/Save 等正式能力 |
| 正式菜单/快捷键 | CommandMenu/Dispatcher/Registry 链，不调用公开 App.execute |

这些是已核查的真实链路与代表性消费者，不是全仓静态调用图或所有外部用户清单。[S03,S17,S18,S19,S20]

### 3.5 当前真正的问题

A. App.execute 在通用派发返回后解析 AcceptedOperation.kind == "save"，将 value 转成 SaveId 并登记 pending_saves_。

B. EditorLifecycle.cpp 中对菜单 takeCompletions() 又做同一项保存特判。

C. 本身的 save() 已经登记 save_reports_ 和 pending_saves_；外围又补同类登记。存在去重，所以这不等于已证明重复保存，但说明业务登记散在多条入口。

D. installContributions 先取得 builtinSessionCommands，再按 ID 删除其中的 save，安装捕获 App Impl 的项目保存版本。普通保存与项目保存确有不同语义，但应通过明确接收者和装配选择表达，而不是在生命周期中心事后覆盖。

E. 许多 execute callable 捕获整个 this，业务实现又位于 Impl；换成 ICommand 虚类但内部继续调用 app.save()，并不能解决职责问题。[S04,S06,S07,S08]

### 3.6 目标

命令派发只处理命令、目标、版本和调用资格，不解释 SaveId、布局类型或发布阶段。

项目保存活动接纳请求时，即建立可靠的保存/项目登记责任；结果展示可以观察它，但不能通过“收到命令回执”才开始承担文件责任。即使没有 ResultsView、菜单或 App.execute，已接受的操作也能完成并被正确查询。

命令构造函数可以放在相应领域/工作台主题，用 typed receiver 或受约束 callable 构建 CommandEntry；运行时扩展在原贡献边界擦除一次。宿主只组合贡献，不是所有命令的接收者。

目前 AcceptedOperation 的 kind/value 对观察可以有用途，但不应由宿主用它建立一套业务 switch。是否强化其类型，需要由实际跨插件结果查询需求决定；不要只为替换字符串而制造一套全局 OperationRegistry。

## 4. 建议的 Application 最终边界

保留：

- 生存期与阶段；
- 产品级装配；
- 唯一主循环与退出驱动；
- 已接受工作的排空；
- 销毁顺序和最终错误。

迁出：

- 项目保存规则及 manifest 发布流程；
- 工作区保存/选择/迁移政策；
- 插件选择发布活动；
- 通用结果的业务身份解释；
- 资产/工作台用例的完整私有状态。

公开 App 可以最终收窄到 create、exec、update、requestExit、phase 等。open/show/layout/command 等使用者转向准确的正式能力。若确有嵌入场景需要中性 facade，它不能持有另一套业务记录，也不能解释某一类命令的回执。

不允许将所有原成员机械搬到 EditorApplicationServices 或 EditorController，然后让每个模块持有它。那只是重命名同一个全知对象。

## 5. 实施应先解决什么

### 第一步：迁出完整项目保存与工作区职责

先明确每组状态的唯一 owner，迁出实际算法和查询入口，内置菜单、插件、测试使用相同能力。保留原关窗/关闭内容/退出区别，保留保存部分成功和来源版本。

### 第二步：让命令构造绑定准确接收者

移除 App.execute 和菜单完成循环中的保存特判；确认不是以丢失 pending_saves 责任换取代码简短。统一通过实际活动的准入和完成查询支撑无菜单调用。

### 第三步：实现最小但贯通的设置系统

先完成用户窗口/字体配置、一项内置工具设置、一项真实外部扩展设置。用同一描述/验证/codec/存储/应用路径贯通启动、界面修改和重启。没有实际需求时不增加远程同步、分布式配置、任意热重载和通用事务框架。

### 第四步：验证新的职责边界

- 无 App/Root 可以执行项目保存及项目登记。
- 直接活动调用、程序化命令、真实菜单调用产生相同业务后果。
- 新插件命令或设置页不要求修改 Application 核心实现。
- 启动读取用户配置，CLI 仅覆盖本次；错误配置不覆写原文件。
- 缺失插件的设置保留；恢复插件后按版本正常读取。
- UI 关闭/应用退出仍接收原已准入完成，不破坏代码、资源与用户数据寿命。

这些是建议的下一工作范围，不是当前任务已经授权的代码施工或新增通过声明。

## 6. 本次使用的固定源码索引

全部以下路径固定于本报告 HEAD；源码阅读与测试执行分别记录。

- S01: editor/application/include/lux/engine/editor/application/EditorApplication.hpp，完整公开 API。
- S02: editor/application/src/EditorApplication.cpp，创建、装配、字体和原服务初始化。
- S03: editor/application/main.cpp，正式命令行和启动路径。
- S04: editor/application/src/EditorCommands.cpp，完整公共 execute、贡献装配和菜单目标捕获。
- S05: editor/application/pinclude/lux/engine/editor/application/EditorApplicationImpl.hpp，已读成员/私有状态区段。
- S06: editor/application/src/EditorLifecycle.cpp，退出前半段及 update/exec/排空区段。
- S07: editor/application/src/EditorSaving.cpp，prepareSave/save/askSave/rememberSave/settleSaves。
- S08: 同 S07 的后半段，目录采用和 installSaveCommands。
- S09: editor/application/src/EditorSettings.cpp，完整插件选择接线和推进。
- S10: editor/authoring/layout/include/lux/engine/editor/workspace/WorkspaceValues.hpp，完整偏好类型与 codec 声明。
- S11: editor/activities/workspace/src/WorkspaceStore.cpp，读写、路径、选择、冲突与发布。
- S12: editor/workbench/project/tools/src/SettingsView.cpp，完整实际设置页面。
- S13: editor/application/extensions/include/lux/engine/editor/extensions/Contributions.hpp，公开贡献种类。
- S14: editor/workbench/scene/api/include/lux/engine/editor/scene/ConfigurationEditor.hpp，配置控件契约。
- S15: editor/activities/commands/include/lux/engine/editor/commands/Command.hpp，命令值及结果。
- S16: editor/activities/commands/src/CommandRegistry.cpp，注册、校验、query/execute 关键区段。
- S17: editor/workbench/desktop/src/CommandMenu.cpp，完整菜单/快捷键到派发链。
- S18: editor/tests/integration/application/application.cpp，前 320 行，直接调用/线程回归。
- S19: cmake/installed-consumers/editor-application/main.cpp，完整 SDK 调用。
- S20: cmake/installed-consumers/editor-ec1-skeleton/main.cpp，无 App 路线前半段及 APP 路线 405–590 行。
- S21: editor/application/src/EditorWorkspace.cpp，完整工作区政策、布局组合及面板接线。
- S22: editor/application/CMakeLists.txt，源与 target 依赖。
- U01: 已提供 AGENTS.md，尤其 L15–22 的层次与职责、L174–204 数据/行为/错误边界、L306–313 持续质量规则。

本报告没有修改仓库、施工账本、历史验收、main 或用户补丁。
