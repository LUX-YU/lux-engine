# EC3-C：可扩展设置、窗口环境与生效链

## C1. 先明确现状

当前 EditorApplicationConfig 是启动输入，提供项目／安装位置、title、width/height、offscreen、font 和 user_directory；尺寸有默认值，调用方可覆盖，不是硬编码在不可变构造体内。[S21,S22]

当前工作区有真实偏好／布局／恢复文件读写；UserPreferences 主要保存 selected_layout 与保留数据。SettingsView 主要是项目插件选择，不是通用窗口／字体／快捷键／插件自定义设置中心。[S24,S25]

本章新增目标，不得在 EC3 完成之前把上述能力描述成已经存在。默认值本身不是错误；缺少的是来源、覆盖、验证和应用的完整关系。

## C2. 设置不是一份无限扩张的 Config

| 作用域 | 例子 | 实际保存与责任 |
|---|---|---|
| 安装缺省 | 内置 UI 默认值、声明默认快捷键 | 安装只读资源／静态描述；不可原地覆盖 |
| 用户首选项 | 字体、UI scale、个人快捷键 | OS user config root 下的版本化文档 |
| 项目共享设置 | 项目插件选择、项目工具默认策略 | 项目自身文档／manifest；团队共享 |
| 用户的项目状态 | 窗口布局、上次内容、个人覆盖 | 用户根下的项目作用域，必要时只读迁移旧位置 |
| 启动覆盖 | --font、显式尺寸、offscreen | 本次内存层；不自动保存回文件 |
| 作者内容 | 某 Scene 的 RenderFeature／系统配置 | 原 Session、History 和资产保存；不迁入全局设置 |
| 运行参数 | 队列容量、线程策略、diagnostics | 原参数类型和合法性；并非全部都必须出现在 UI |

不得将这六类来源简单放在同一张字符串 map 中按插入顺序覆盖。Descriptor 必须明确允许作用域与 merge policy。

默认优先级：安装缺省 < 允许的项目共享缺省 < 用户首选项 < 用户项目覆盖 < 显式启动覆盖。对只允许用户／只允许项目的项，非法来源拒绝而非静默忽略。插件启用仍由项目 manifest 负责，不受用户同名设置覆盖。

用户项目键使用已有持久项目身份；若当前没有，采用明确版本化的规范项目位置派生策略，处理项目移动与冲突，不能用进程内 project_instance 做持久目录名。

## C3. 最少必要的角色，优先复用原类型

| 目标角色 | 处理 |
|---|---|
| SettingsDescriptor / SettingEntry | 在原配置描述和贡献目录上增补 stable ID、schema、作用域、默认值、生效策略；只有一份字段 schema |
| SettingsDocument | 文档值、schema version、未知段、来源与物理版本；不持活动 UI |
| SettingsDraft | 用户编辑的值及 based_on；复用已有 ConfigurationValue/codec，不能成为第二作者 Session |
| SettingsResolution | 固定有效值、各项来源、待重启与诊断；是结果不是服务 |
| SettingsStore | 版本化读写、候选和发布记录；使用原 WriteCoordinator／文件 backend |
| 设置实际应用者 | 窗口、字体、按键或具体插件原 owner；不要由 SettingsStore 做 GPU／窗口工作 |
| SettingsView | 注册页面、编辑草稿、显示 desired/applied/persisted，不负责唯一解析算法 |

以上是责任角色。C0 发现已有等价 provider 时直接复用。新类型只在存在独立不变量或真实生命周期时引入，禁止一角色一 DLL、一枚举一文件。

物理建议：纯设置值和描述放当前 `authoring/layout` 的准确 provider；存储与解析接线放 `activities/workspace`；设置 UI 放对应 workbench/project/settings 主题；平台观察放原 modules/platform/window；启动组装留 application。不得把所有东西塞进新 `editor/settings` 顶层。

公开逻辑 include 不随目录裁定无意义改名。不要新增全局 SettingsManager/ConfigurationManager 或中央字符串解释器。

## C4. 插件描述与载荷寿命

扩展沿现有 ContributionDraft／Snapshot 增加 settings 集合，复用同一稳定描述、CodeLease、原子批次和代码验证。不要另装一套独立插件设置注册器。

每项描述至少明确：稳定 ID、schema 版本、原 ConfigurationDescriptor／codec、默认值来源、允许作用域、纯校验、merge policy、生效方式、可选控件工厂和所属提供者代码。

字段级通用编辑器可从已有反射构建；复杂页面可以提供控件工厂。引擎层定义参数和合法性，Editor 提供存储／页面接线，底层不得 include Editor。

同一插件的动态 label、默认值、codec 和应用回调必须被同一适当 owner 保活。删除注册后尚未确认的草稿／发布／应用结果仍不能调用已卸载代码。

同一 stable ID 的不同 schema 更新需显式迁移；不同提供者同时声明同一 ID 拒绝。hash 碰撞按本阶段通用冷路径规则拒绝，不用 label 相等判断身份。

新贡献布局改变 Editor 扩展协议时，在当前 V8 后演进一个版本（当前目标 V9／lux_editor_exports_v9），同步内置扩展、外部骨骼插件、ABI 指纹和真实 SDK。运行插件 ABI、资产文件类型和脚本 ABI 不随此无关变更。若 C0 发现版本已被后继占用，采用下一个真实版本并记录，不能覆盖历史 V8 定义。

## C5. 文档读写与未知数据

沿用现有 TOML／codec／文件 backend，不另造配置语言。定义每份文档的 schema、大小／项数／嵌套上限和受支持版本。

缺文件可以使用默认值；权限、格式、checksum/版本冲突不能伪装成缺文件，不能自动覆盖损坏数据。

插件暂时缺失时保留其未知段与 opaque 值，禁止因为没有加载 editor widget 就删除。已知字段变更只替换对应节点；未修改未知节点保持值等价。若当前 codec 无法保留原注释／排版，明确承诺值保留而非逐字节保留；不要声称没做到的 round-trip。

读取新于支持的 schema 时保留原文件，提供只读／明确迁移失败，不用空默认对象覆盖。

沿用文件版本与原 WriteCoordinator 的冲突／Unknown／对账／确认；配置文件写入不是特权路径，不能直接 `ofstream` 覆盖绕过发布规则。

旧项目 `.lux/workspace/preferences.toml` 的 selected_layout 与布局／恢复关系保留。迁移到用户项目作用域要有读旧、准备新、成功标记、重复幂等与失败保留，不为了重命名 UserPreferences 一次性删除旧文件。

最近项目文档同样保留上限、绝对规范路径与用户根约束。项目移动、不存在和权限错误在 UI 显示，不能通过 hash 后丢弃原路径语义。

## C6. 草稿、持久化与实际生效

明确三项状态：desired（用户想要的值）、applied（实际已生效）、persisted（落盘确认）。不能用一个 dirty 布尔量替代全部状态。

草稿带 SettingsDocument 的来源版本和 descriptor/schema 版本。外部文件或注册变化后，提交旧草稿必须检测冲突；可显式重载／合并，不自动 rebase。

默认生效协议：准备并验证候选 → 由准确 owner 申请生效 → 记录实际结果 → 根据用户保存意图发布文档。不得承诺全部设置都可跨系统原子切换；每项描述声明 immediate、safe-point 或 restart-required。

字体与 UI scale 由原字体／UI／渲染 owner 准备与切换。首次交付可明确为重新启动生效，必须读回后真实生效；若实现 live reload，则旧 atlas／GPU 引用必须活到最后使用，不可在 draw 中销毁。

窗口状态由原平台窗口 owner 应用；实际系统可能限制位置或尺寸。保存用户意图与实际结果，不能把 OS 拒绝改成成功。

项目插件启用默认 next-project-open/restart-required。动态注册设置不等于即时重装插件。

UI 显示逐项准备失败、应用失败、已应用未保存、已保存待重启。Apply／Save／Revert 按这些真实事实工作；失败不丢草稿，取消不释放别人已接纳任务。

恢复 defaults 是构造来自 descriptor 的新草稿，不直接抹除磁盘文档或未知插件段。

## C7. 命令快捷键属于用户覆盖，不修改固定声明

CommandDescriptor 中的 shortcut 是内置默认值。用户覆盖放设置文档，以稳定 CommandId 引用；canonical identity、scope 和 input_version 不因改快捷键而改变。

在配置解析边界调用唯一 shortcut parser，得到原键／修饰符值。冲突必须有确定政策：拒绝或明确禁用冲突项并说明；不能按 map 偶然顺序赢者通吃。

插件命令缺失时保留覆盖但不激活；重新加载后按兼容的稳定身份恢复。不得保存 snapshot 内存地址或临时命令序号。

默认菜单 group、显示 label 和个人快捷键分开；翻译文本不能作为设置文件键。无需本阶段完成整个国际化系统，但不得堵死此边界。

## C8. 启动分两段，避免插件配置循环

```text
解析 Editor 启动选项
    → 确认安装／项目／用户根
    → 读取内置引导设置和项目插件选择
    → 查询显示环境（非 offscreen）
    → 得到基本窗口／字体的有效配置
    → 创建平台与必要引擎能力
    → 加载获准扩展，登记 settings 描述
    → 解释对应扩展段并验证
    → 按 owner 激活设置与工具
    → 创建完整工作台
```

哪些引擎能力必须早于读取字体等任务建立，按原实际依赖安排；不能要求还没有 ExecutionRuntime 就提交 font task，也不能为了同步读取方便阻塞错误线程。

读取 bootstrap 设置不依赖插件提供的 UI。Settings schema 未加载前保留原段，不能先激活插件才能判断它是否应该激活。

`EditorApplicationConfig` 可以保留为已解析创建输入，但不再混合“0 表示自动”“用户明确设置 0”“尚未解析”等含糊状态。使用已有 option/value 类型；必要时增加轻量 `EditorLaunchOptions`，把显式覆盖与解析结果区分。不要同时维护三份独立窗口设置。

## C9. 显示器与窗口策略

当前窗口链直接使用传入宽高，没有自动选择显示器工作区。[S22,S23] 新策略遵循：显式覆盖 > 合法保存状态 > 选定显示器环境 > 安全缺省。窗口构造只消费解析结果，不在构造内隐式读设置文件。

复用／补齐原 platform/window 的 monitor 观察接口。它返回明确坐标单位的工作区、content scale、可用显示模式及当前临时 monitor 身份。不要在 Editor 与 Launcher 各写一段 Win32/GLFW 查询。

不得把物理 framebuffer 像素直接当窗口坐标。目标显示器名字不保证唯一，GLFW 指针与枚举次序也不能持久化；保存足够的可解释提示，重启后匹配失败则回退，不猜一个有效指针。[E01,E02]

解析时至少处理：

| 场景 | 要求 |
|---|---|
| 首次启动 | 使用工作区推导有限默认矩形，比例／上限集中为一份政策 |
| 显式尺寸 | 验证单位、数值与上限；合法覆盖优先，不每帧按环境重写 |
| 保存矩形 | 恢复 ordinary-window 矩形；验证可见区域与目标 monitor |
| 原显示器断开 | 回退到可用工作区，保证主要交互区可达，不保存旧 pointer |
| 负屏幕坐标 | 正常多屏坐标，不当成非法 unsigned 数 |
| 极小工作区 | 合理裁剪；最小尺寸不可满足时明确退化，不整数下溢 |
| 装饰边框／任务栏 | 区分 content rect 与外框，不遮掉全部标题栏 |
| DPI／scale 变化 | 调整 UI／字体与输出尺寸的正确环节，不双重乘 scale |
| 最大化 | 与恢复用普通矩形分开保存；不覆盖普通矩形为最大化尺寸 |
| 全屏 | 明确一种支持模式及目标 monitor，保留恢复矩形；不要求本阶段实现任意独占刷新率编辑 |
| offscreen | 使用明确确定尺寸，不查询 monitor，不伪造物理窗口 |
| 未保存设置 | 仅本次启动覆盖，不无意写回用户配置 |

本阶段至少实现普通窗口、最大化恢复，以及原平台实际可支持的一种全屏切换；无法支持的后端明确返回 unsupported，不显示已经生效。不要新增平台特例到 application 内部。

窗口调整／monitor 事件通过原平台事件与安全点处理；不是每帧重算默认布局。持久化使用已有发布链并合并频繁事件，在退出时结清实际已接受写入。

## C10. 最小真实交付

必须接通：窗口模式／恢复矩形、字体选择、UI scale、快捷键覆盖、现有项目插件选择，以及一个真实外部插件自定义设置页。

每项不必支持即时应用，但必须有明确策略并通过保存→关闭→重开→实际应用的完整路径。不要只生成 schema 和漂亮界面而不调用实际功能。

外部骨骼插件可提供网格／骨骼显示选项或编辑习惯项；引擎功能扩展提供自己参数值与合法性，Editor 仅提供 UI/持久化接线。运行参数不引用 Editor。

无 UI 消费者必须能读取、验证、合并和发布设置。UI 消费同一 API；不能让 SettingsView 成为唯一 parser。
