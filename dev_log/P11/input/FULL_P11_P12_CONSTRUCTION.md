# Lux Engine：更新后的 P11 / P12 收尾施工总册

日期：2026-10-01  
基线：`22ab1a30862f6bff943cef86d4232839173c7107`  
定位：实施指导，不是已修改仓库或已运行产品测试的证明。

## 使用顺序

先阅读总指令；实施 P11 后独立提交、复审；再实施 P12，并在 P12 内完成唯一入口切换及残留清零。删除不是附加的 P13 任务。

本文件合并完整施工正文；压缩包中保留分阶段文件、来源索引、原始参考规范、证据模板和只读盘点工具。

## 目录

1. [P11 / P12 收尾施工总指令](#unit-1)
2. [P11 — 正式命令、不可变贡献与两阶段工厂](#unit-2)
3. [P12 — 完整产品接通、唯一入口切换与残留清零](#unit-3)
4. [旧代码、目录、构建与安装残留的实际移除计划](#unit-4)
5. [验收、阶段交接与后续质量约束](#unit-5)

---

<a id="unit-1"></a>

<!-- 合并来源：00_MASTER.md -->

# P11 / P12 收尾施工总指令

版本：2026-10-01 · Closeout 1  
适用仓库：`LUX-YU/lux-engine`  
工作分支：`codex/editor-redesign-v4`  
参考验收 HEAD：`22ab1a30862f6bff943cef86d4232839173c7107`  
参考实现：`e72e9f7f931c5825dcd6c2c5a5b40dd66ccf62a5`

> 本包是更新后的施工方案，不是已执行的代码修改或阶段验收。目标只有两个：P11 建好、接通并验证正式命令和扩展；P12 接通唯一完整产品，删除旧产品与补偿性代码。不要再插入一次顶层架构设计，不把删除留到 P13。

## 1. 一次明确的路线

```text
当前五层实现
  → P11：命令 / 不可变注册 / 两阶段内容工厂 / 工作台工厂 / 扩展 SDK
  → P11 单独提交与复审
  → P12：真实产品用例 / 单一入口切换 / 旧代码与旧安装产物清零
  → P12 单独提交与复审
```

收到整个包时，默认先执行 P11 的 A–G，完成后停止。P12 文档是下一阶段完整设计，不等于允许跳过 P11 的复审。用户另行明确授权连续实施时，仍必须产生独立的 P11 资格与提交，不能用 P12 最后一次构建掩盖 P11 缺项。

P13 保留为最终资格/交付范围核验；不是第三次业务迁移，不接收 P12 应删除的旧 Context、旧 Editor、transition 或旧产品回落。

## 2. 基线事实和适用范围

### 2.1 本次实际核对的事实

- 当前正式源码已有 `editing / authoring / activities / workbench / application` 五层。
- `editor/CMakeLists.txt` 的正式段配置五层；另有旧 metadata、plugins、context、ui、tools、launcher、app 和 assets 测试段。
- 现有 `lux_editor` 仍定义在 `editor/app/CMakeLists.txt`，链接旧 editor_app、context 和三大旧 Editor；不能称为已切换的新产品。
- `SessionStore` 已有 `reserve/prepare/publish/prepareClose/close`；本文不得假定不存在的批量原子 API 已经实现。
- `ViewHost` 已有 `adopt/describeAll/close/show/focus/drain`；完整布局批量提交需要先判断这些能力是否足够，不能把逐项 adopt 的半完成说成全有或全无。
- `SaveService` 已有注册、保存、完成吸收与采用。已准入完成在角色回调期间仍可可靠接收，此约束不能回退。
- `editor_assets` 和 `editor_editing_scene` 已分别是活动层的真实导入和运行编辑 provider；旧名称不代表旧实现。

具体来源在 `reference/SOURCES.md`。本包核对了代表性代码，不声称逐行审查全部 P10Q 实现或独立重跑其 Windows/GPU 资格。

### 2.2 历史测试数量不是新增配额

输入报告中的 Editor 209、CPU 183、PLAYER 12、21 组 SDK 和 115 个公共头用于建立映射。不得为维持数量保留已淘汰的旧产品实现；也不得通过删除对应行为使数字下降后隐瞒。

核心模型/存储/运行未改行为时保留现有测试及断言。旧产品 API 被删除时，把同一用户行为移植到正式新产品测试，记录旧入口→新入口→等价观察的映射。

### 2.3 用户已调整的资格范围继续有效

- 本轮不建立 Linux 环境。进行 C++20、平台 API、路径、工具链和 CMake 的静态可移植性审查；Linux 构建和运行保持 `NOT_RUN`。
- 不补旧 50k 深链剩余样本，不重跑与本次修改无关的昂贵性能基线。
- 保留短的复杂度、容量、字节 owner、身份和结果回收测试；功能测试不是可随长测一起取消的项目。
- 系统 IME 未实测就保持 `NOT_RUN`；OS 字符输入或 SendInput 不能冒充 IME 组合资格。
- 原历史 `PARTIAL/FAIL/NOT_RUN` 收据不改写。新路径修复后另记相同契约的新证据。

## 3. 覆盖旧文档的明确裁定

| 编号 | 裁定 |
|---|---|
| D01 | 旧 V4 的顶层 `editor/commands`、`editor/extensions`、`editor/workflows`、`tools/*/integration` 不是新目录目标。命令/内容操作归 activities；视图协议归 workbench；跨层组合归 application。 |
| D02 | 不依类型名删除。纯 ProjectBuilder 继续归 authoring/project；实际 SceneEditing/RegistryFieldEdit 供 Run 使用，继续归 activities/scene；正式 editor_assets、editor_storage 等可保留真实 target 名。 |
| D03 | C04 按当前 `editor/app/test/baseline_failures.cpp` 的原断言定义：菜单连接失败必须使 create 返回错误，不能返回成功对象再令 outcome 失败。退出排空另列 X12-09，不重定义 C04。 |
| D04 | 原 V4 P11 的 G 段明确允许仍由旧产品使用的旧工厂头/方法在 P12 删除；P10Q-structure 的 retained-product 将整个 metadata target 简写为 P11。现按“P11 新机制和可迁贡献完成、P12 旧产品最后消费者/旧协议物理清零”逐符号拆账，而不是把全部 metadata 无条件延期。历史快照不改。 |
| D05 | P11 不允许新增新→旧桥。不再生成 V6→V7 长期适配器或同义 Port；仅现存、确有旧产品消费者的具体声明/接线可列入 P12 移除清单。零消费者及已经被替代的原算法立即删除。 |
| D06 | 新 ABI 与旧 ABI 显式区分，复用原插件装载能力。指纹/版本检查发生在贡献回调调用前；不声称 OS 装载 DLL 时不会执行静态初始化，不声称任意跨编译器 C++ ABI 稳定。 |
| D07 | 原 `root()` 或 `Element(parent, id)` 并非一律废弃。删除的是会在顶层节点构造时自动注册活动 Root、依赖旧 EditorContext 的兼容入口；保留有明确唯一 owner 的离树子节点组合和合法已挂载访问。 |
| D08 | P11/P12 不修改 Lua、Renderer、Process 核心算法，不发明 Session 管理器的管理器；确有批量发布/关闭缺口时只在原 owner 添加窄能力，并补真实失败验证。 |
| D09 | P12 的清理是完成条件，不是建议。最终必须只有五层 + tests 的 tracked Editor 一级目录，不留旧 C++ 系统、旧 target 空壳、永久 legacy 或 Old/New 运行开关。 |
| D10 | 本包提供新施工入口，不冒充 P10Q 全面复审报告。P11-A 核验已有结构收据、STRICT 门禁和当前祖先关系；有真实缺口先修复该缺口，不重新跑一轮无关调查。 |

D04 是明确的期限细化：P11 交付单必须逐项说明哪些旧声明因旧 executable 尚未切换仍有消费者。未列明的项目不得自动获得 P12 豁免；不能给任何项目增加 P13 期限。

## 4. 五层的实际施工落点

```text
editor/
  editing/                       # 身份、History、SessionStore/State、必要纯共享值
  authoring/{scene,material,flow,project,layout}/
  activities/
    commands/                    # 新 P11：语义命令、不可变条目与派发
    sessions/                    # 新 P11/P12：安装、内容打开/重载/关闭、SaveAll
    persistence/                 # 原保存/写协调器/执行/文件发布
    scene/ material/ flow/       # 原 Run/投影/编译，加入具体工厂/领域操作
    project/ workspace/ tasks/   # 原目录/IO/布局存储/任务观察
  workbench/
    desktop/                     # 原 Host/Shell；菜单绑定、视图工厂、布局应用
    viewport/ widgets/           # 保持通用，不依赖具体作者模型
    scene/ material/ flow/       # 具体交互和窗口
    project/ tasks/ settings/    # 项目、任务、偏好/扩展配置界面
  application/
    src/                         # EditorApplication、OpenAndShow、ExitEditor 等组合
    extensions/                  # 原装载器的 Editor 贡献安装，非 ServiceLocator
    launch/                      # 原 launchEditor + 正式 launcher/main
  tests/                         # 跨层、新产品、安装和依赖验证
```

目录不是新库配额。多个紧密协作的短值、句柄、枚举和函数放在同一语义头。`activities/sessions` 的新增理由是实际跨领域内容操作，不建立新源码/History 权威。

## 5. 永远不重复的权威

| 事实 | 唯一 owner | P11/P12 允许新增什么 |
|---|---|---|
| 作者源和历史 | SessionStore 独占的具体 Session | 角色句柄、已固定操作输入、只读观察 |
| 保存基线/绑定/准入 | SessionState | 用户决定所依据的 stamp，不再造 dirty |
| 写目标顺序及 Unknown | WriteCoordinator | 对已有票据的组合等待，不建另一条写队列 |
| 任务执行/完成运输 | ExecutionRuntime/Task/TaskScope | 业务操作与任务相关 ID，不建线程池 |
| Run 意图 | RunStore/RunSession | 退出时的固定 RunId 集，不复制运行状态 |
| 重实例/单步终态 | SceneRuntime/lifetime | 现有退休凭据及其结果，不建墓碑仓库 |
| GPU 和资源退休 | 原 RenderRuntime/资源 owner | 等待现有结果，不在 App 手动释放在途资源 |
| 顶层 Pane | ViewHost | 布局准备和关闭集合，不另维护 PaneManager |
| 项目目录 | ProjectCatalogModel | 定位到哪个工作副本的有限索引，不复制目录 |
| 当前扩展/命令版本 | 对应不可变注册快照 | 一次安装批次固定的快照句柄，不建全局服务表 |

## 6. 执行顺序与停点

### P11

| 批次 | 完成内容 |
|---|---|
| A | 读当前旧注册/入口、结构账本和原 X11；冻结成员/生成器/消费者清单。 |
| B | Command 数据、目标策略、不可变条目/快照/句柄；真实 query/execute 分离。 |
| C | 两阶段内容工厂和角色准备；所有公开入口不能观察半安装会话。 |
| D | ViewFactory/控件贡献、Editor 贡献安装与代码寿命；复用原 loader。 |
| E | 三种真实工具接线、新菜单/快捷键，外部安装 SDK。 |
| F | 删除已替代协议/原体；C03 新旧活动路径检查；逐项固定剩余旧产品消费者。 |
| G | 同一最终 SHA 完整适用资格；实现和验收分别提交，停下复审。 |

### P12

| 批次 | 完成内容 |
|---|---|
| A | 以 P11 验收为入口，固定完整用户功能和最终删除清单。 |
| B | 内容 Open/Reload/SaveAll/CloseSessions，以及视图/内容关联的唯一管理。 |
| C | 布局准备/提交、独立恢复、最后视图三选择和未保存提示。 |
| D | 三工具、运行 Inspector、项目/任务/设置、插入和发布的全部产品入口。 |
| E | 正式 EditorApplication、启动失败和可靠退出；复用唯一 engine 驱动。 |
| F | 将已有 lux_editor/launcher/内置扩展/安装入口切到正式装配，不安装第二产品。 |
| G | 删除所有旧目录、旧 owner、过渡入口、旧导出及生成/安装残留。 |
| H | 新前缀、新产品、失效回调、故障/退出/GPU、引用与包检查；停下复审。 |

同阶段内部可以连续实施，不要求每个小提交询问用户。发生真实阻塞时记录已经完成的批次和未完成项，不默认成功，也不通过减功能通关。

## 7. 工作区、证据与实际删除

1. 先记录 `git status`、当前分支、HEAD、远端以及 main 的 OID。不要把原工作区仍在旧提交误认为远端没更新。
2. `ProjectBuilder.cpp` 的用户修改位于原工作区；已有重定位补丁不是已经应用。使用独立资格检出，保存原字节和补丁，不混入实现、不 reset。
3. 正常提交到现有分支；不 force push、不 rebase 公共历史、不运行 `git clean -fdx`。
4. `.internal/editor-redesign/` 中现有账本增加 `closeout` 节点，仍然只有一份可变事实源。
5. 每阶段分别冻结 `dev_log/P11/`、`dev_log/P12/`；冻结记录指向实现 SHA，证据提交在实现之后。
6. 删除指向被替代的生产内容；用户项目/资产/备份、版本化只读迁移、原失败数据、仓库历史不删。
7. 旧测试入口迁移后可删除源文件，但须保留行为映射和历史原文件；不靠保留老 product library 才能运行回归。
8. 本包脚本只盘点，不替实施者做破坏性删除，不用“脚本返回 0”代替代码审查和真实资格。

## 8. 最终汇报必须能回答

- 哪些真实用例现在通过哪个正式入口执行？
- 哪些新类型确实承担事实/寿命？哪些同义转发已经删除？
- P11 留下的每个旧消费者在 P12 哪次提交退出？
- `lux_editor` 的实际链接和运行是否仍加载旧实现？
- 根目录、源定义、CMake、生成器、安装包、动态符号和 SDK 是否同时清理？
- C01/C03/C04 原契约在当前实现上的结果是什么，历史结果如何保留？
- 哪些资格没有运行，不能被 Windows/GPU 测试替代？

相关文件：`01_P11_IMPLEMENTATION.md`、`02_P12_IMPLEMENTATION.md`、`03_REMOVAL_PLAN.md`、`04_ACCEPTANCE_AND_HANDOFF.md`。


---

<a id="unit-2"></a>

<!-- 合并来源：01_P11_IMPLEMENTATION.md -->

# P11 — 正式命令、不可变贡献与两阶段工厂

本文件可独立交给实施 LLM，但必须同时遵守 `00_MASTER.md`。只执行 P11，完成后停下复审，不自动执行 P12。

参考：`lux-engine@22ab1a30862f6bff943cef86d4232839173c7107`。本文件中的“新增”是目标签名/职责，不声称当前仓库已有实现。

## 1. 完成定义

P11 完成时，必须同时成立：

1. 正式新工作台的菜单/快捷键使用分离的 query 和 execute；点击目标和注册绑定政策明确。
2. query、execute、创建、恢复和清理期间，当前 entry、闭包、元信息和代码 owner 不会被注册更新提前释放。
3. Scene/Material/Flow 都能经正式工厂读入、在 owner 构造并完整发布会话与角色，不由 worker 构造线程亲和对象。
4. 正式视图工厂只产生完整 DetachedView，不接管、聚焦或借旧 PaneManager 打开资产。
5. 至少一个真实外部 Editor 扩展仅靠安装 SDK 构建、装载和使用；不兼容版本在贡献回调前拒绝。
6. 已迁出的原算法/注册定义只有一份。仍服务旧 executable 的有限声明有明确消费者和 P12 删除项，不建立新兼容桥。
7. 五层边界、C03 契约和既有保存/运行/交互回归仍成立。

本阶段不是“给每个功能添加 virtual 接口”。新增类型有真实消费者才加入；一个语义文件可以容纳多个小型值。

## 2. 当前输入与第一批检查

### 2.1 必读现有位置

| 文件/目录 | 用途 |
|---|---|
| `editor/metadata/.../CommandRegistration.hpp` | 当前混合 `EditorContext& + ui::Command` 的 QUERY/EXECUTE 回调。 |
| `editor/context/.../EditorContext.hpp` 与 CPP | setCommands、commands span、assetEditors、PaneManager 等旧消费者。 |
| `editor/metadata/.../EditorPluginExports.hpp`、`src/EditorPlugin.cpp` | 当前 V6 出口及原插件验证/装载，不重新发明 loader。 |
| `editor/metadata/.../ComponentEditorRegistry.hpp`、`ConfigurationValue.hpp`、`SceneRegistrations.hpp` | 分清纯值、UI 创建、元信息和应用安装。 |
| `editor/editing/include/.../SessionStore.hpp` | 预留、准备、发布与 owner 限制。 |
| `editor/activities/persistence/include/.../SaveService.hpp`、`SaveSource.hpp` | 现有保存角色及可靠完成纪律。 |
| `editor/workbench/desktop` | Host/Shell、DetachedView、关闭错误、Root 安全点。 |
| `editor/app/product/ProductAssembly.cpp`、`ProductCommands.cpp` | 旧内置贡献实际清单，不能只注册新测试命令。 |
| `editor/app/test/baseline_failures.cpp` | C03 原可观察契约，以及不得改义的 C01/C04。 |
| `dev_log/P10Q-structure/retained-product.json`、`target-map.json` | 当前 provider/consumer 的起始事实，不直接作为未来最终清单。 |

若路径已变，沿 Git 和 file-plan 定位唯一新位置，不恢复已删除目录。不要只看 README 就宣布协议已经迁移。

### 2.2 冻结四张映射

在现有账本记录：

- 每个内置命令：旧 id、菜单位置、快捷键、query 输入、execute 目标、调用的真实业务、接收者寿命。
- 每项贡献：稳定类型/SchemaId、反射来源、旧 factory、所有消费者、代码 owner、目标层。
- 每种会话：load/decode/construct/publish/close、历史/保存/SaveAs 能力和具体实现。
- 每项 SDK 出口：公共头、导出函数/版本、库、运行依赖、消费者和最终删除时间。

旧主菜单的用户功能必须全部映射。P12 才实现的产品组合命令可暂不在新正式菜单显示；不得以“永远返回 Unsupported 的处理器”占位并计入完成。

## 3. 最小类型与文件布局

### 3.1 命令

建议目标（同模块内，不是一行一个库）：

```text
editor/activities/commands/
  include/lux/engine/editor/commands/Command.hpp
  include/lux/engine/editor/commands/CommandRegistry.hpp
  src/CommandRegistry.cpp
  src/CommandDispatcher.cpp
  test/commands.cpp
```

| 类型组 | 责任/所有权 | 禁止承担 |
|---|---|---|
| `CommandDescriptor` | id、显示名、分组、默认键位的纯描述 | EditorContext、Pane*、可变注册容器 |
| `CommandQuery` / `CommandState` | 一次只读查询的确定目标与可用/勾选/原因 | 遍历全局服务、保存完整作者源、隐式切换目标 |
| `CommandInvocation` | 触发时固定的身份、明确参数、来源策略 | 延后执行时读取当前焦点、裸临时 span |
| `DispatchReceipt` | 区分立即结果、已接受业务操作、拒绝 | 将 Task.finished 冒充业务完成 |
| `CommandEntry` | query/execute 的实际 callable，外层代码 owner | 同一语义再包 Port→Provider→Adapter |
| `CommandRegistrySnapshot` | 不可变条目集合和修订 | 回调内原地 erase/替换 |
| `CommandHandle` | 固定 snapshot/entry，调用期持有寿命 | 借一个容易失效的 span 或只保存下标到 current vector |
| `CommandDispatcher` | 有界等待、检查目标/注册政策、执行 | 第二事件总线、通用工作流系统、领域编辑权威 |

Descriptor 的显示信息与快捷键映射可在同一值中，但 activities 不得 include 带 Pane/ImGui 的 Menu 实现头。复用已有纯身份；若当前 CommandId 与重 UI 头绑定，只提取唯一纯声明，不另造两套相同字符串 ID 再转换。

### 3.2 工厂与角色

```text
editor/activities/sessions/
  include/lux/engine/editor/sessions/SessionFactory.hpp
  include/lux/engine/editor/sessions/SessionInstallation.hpp
  include/lux/engine/editor/sessions/SessionActions.hpp
  src/SessionInstallation.cpp
  test/installation.cpp
```

这些逻辑 include 属于各自真实 provider，不得让 `editing` 因共享名字前缀而链接 activities。

| 类型组 | 决定 |
|---|---|
| `SessionKindDescriptor`、`SessionFactoryRegistration` | 描述支持的源与实际 load/owner-construct 入口，放 activities；不把 UI 创建混进来。 |
| `SessionLoadJob` / `PreparedSessionData` | 复用现有具体 codec 与 Process；拥有 decode 数据、代码和环境，不能持 Pane。 |
| `PreparedSessionInstallation` | move-only 的未发布安装责任：原 reservation、角色 owner、已预留的注册位置；不第二次拥有已交给 Store 的 Session。 |
| `InstalledSession` | 会话 ID 对应的角色/注册 token/关闭依赖元数据；Session 本体仍只在 SessionStore。 |
| `HistoryActions` | 调用真实具体历史的窄能力；不再创建 UndoStack。 |
| 保存/SaveAs 能力 | 优先直接复用 ISaveSource/FrozenSave/IPreparedRebind。若描述可用性已经足够，不额外制造一套 SaveAsActions。 |

具体 Scene/Material/Flow 工厂实现在现有 `activities/{scene,material,flow}/src` 中，可各一个 `SessionFactory.cpp`。不要重建旧 `tools/*/integration` 目录。静态 helper 实例化在这些提供者 CPP 中，通用 sessions 头不 include 三个具体模型。

### 3.3 工作台工厂与应用安装

```text
editor/workbench/desktop/
  include/lux/engine/editor/views/ViewFactory.hpp
  src/ViewFactoryRegistry.cpp
  src/CommandMenu.cpp             # 原 UI 机制与正式命令的接线

editor/application/extensions/
  include/lux/engine/editor/extensions/EditorExtensionExports.hpp
  src/LoadEditorExtension.cpp
  src/InstallEditorContributions.cpp
  src/BuiltinContributions.cpp    # 内容过多才按真正领域分 CPP
```

`EditorExtensionExports.hpp` 是外部模块加入整个 Editor 时的装配契约，可以组合下层贡献类型；E0–E3 不能反向 include 这个总出口。下层各自发布窄契约，不建立人人都要 include 的 ExtensionHost。

通用视图工厂注册只保存创建/恢复纯 UI 配置的能力；创建参数携身份与明确初始绑定。不得把 `ViewHost&`/`PaneManager&` 或可定位任何服务的 Context 传入 factory。

## 4. 命令的目标和执行规则

### 4.1 四种输入关系不要压成一个布尔开关

| 命令类型 | 固定内容 | 执行时核验 |
|---|---|---|
| 保存 | 原 SessionId、项目/绑定语义 | 在实际 admission 捕获该会话当前已提交内容；不因焦点变化转向别的会话。 |
| 删除选择 | 原 Session/History、用户当时的具体对象集合 | 对象仍属于该域且存在；严格编辑按需要保留完整 expected。 |
| 严格草稿提交 | 原 payload + based_on | 内容变化明确冲突；不得执行时补 current。 |
| 仅显示/聚焦 | 原 ViewId/代际 | 目标仍是同一视图；失效拒绝，不查同类型第一个替代。 |
| 应用级命令 | 明确 application 作用域 | 执行阶段与授权能力，不依赖当下任意 Pane。 |

注册绑定同样显式：默认 `PINNED`，队列持接受它的 entry；少数明确选择 `CURRENT_REGISTRATION` 的命令在执行时重新查同 id，重新验证输入兼容性和目标政策。禁止 revision=0 这种魔法哨兵。

### 4.2 参数与错误

不能要求所有领域 edits 都经过命令总线。生成 Inspector/Graph 手势继续直接使用具体领域接口。

参数封闭时用具体值/variant；外部开放参数需要类型擦除时只擦除一次，payload 必须拥有数据与代码寿命。可由注册的 prepare-invocation 函数构建绑定具体参数的 owning callable；不建立 `map<string,any>` 大袋子，也不让 UI 保存借用 payload。

`DispatchReceipt` 只表示派发事实。AcceptedOperation 可以携现有 SaveId/CompileId/RunId 的标识或窄观察描述，不再存一份它们的终态。不存在的通用 cancel 不要为了接口对称而增加。

返回原领域失败或一次准确诊断投影；BUSY、STALE、CLOSED、权限、版本冲突、Unknown 不能都映射为“不可用”。

### 4.3 查询不能隐藏写能力

查询输入应由装配提供必要只读状态，而不是整个 Context。查询不打开资产、不启动任务、不修改焦点、不提交注册。

C++ const 不是插件安全沙箱。测试仍要覆盖 query 企图更新注册、销毁自己持有的句柄、递归派发等情况；更新只能进入 pending 或准确拒绝。不能声称任意恶意回调已被隔离。

## 5. 不可变注册的具体实现纪律

### 5.1 条目只构造一次，快照共享条目

推荐结构：snapshot 拥有只读 entry 列表；entry 外层持 CodeLease，再持 owning callable/元信息。

有 move-only callable 时，不能为了发布下一版本重新复制全部闭包。新 snapshot 可以共享未变化的 `shared_ptr<const CommandEntry>`，只构造变更条目。不要用不可复制闭包逼迫整个注册体系改回 std::function 或 raw void*。

snapshot/entry 是有明确 lifetime 的共享，不是所有操作都变成 shared ownership。可调用控制 owner 的特殊成员须显式裁定。

### 5.2 调用期必须独立持有

query/execute 进入时先取得一个本地强 handle/entry 及代码 owner；回调返回和错误值构造结束前不释放。只保证 registry 对象活着，不能保证原条目活着。

不允许外层保存 `Entry&` 后执行回调，再访问那个可能被回调撤销的可变容器。调用 helper 可以按值接收 handle，避免原持有者在回调中释放自己后破坏正在用的记录。

### 5.3 发布点

1. 验证完整候选：重复 ID、缺 callable、非法键位/元信息、容量及代码 owner。
2. 失败保持原 snapshot；候选清理在自己的 code 生命周期内。
3. owner 安全点发布；不得在 query/execute/工厂/通知/析构回调栈中直接替换。
4. old snapshot 由在途 handle 自然释放；待发布更新有界、策略明确，不能无界积累。
5. 通知失败不撤回已经发生的发布；保留 revision，消费者可恢复同步。

整批布局/恢复固定同一组 ViewFactory/命令/内容工厂快照。application 可以为一个批次组合几个只读 handle；E2 不因此知道 E3 类型。多注册目录安装时先全准备，在一个不可重入、无任意回调的发布点变为可见；不把“逐个替换并触发通知”称为原子安装。

## 6. 两阶段内容工厂：发布前不能有半个会话

### 6.1 阶段图

```text
接纳打开请求与固定工厂
  → worker：读取拥有型 bytes / decode / 校验预算 / 返回 PreparedSessionData
  → owner：reserve<T> 取得真实 SessionId
  → owner：构造具体 Session、History、角色
  → owner：SessionStore.prepare 转移唯一 Session 到不可见槽位
  → owner：完成角色注册位置、关闭 hook 等准备
  → 一个安全发布点：Session 与角色共同可用
  → 发布后通知
```

禁止 worker 创建 EditHistory、SessionState 或 LuxObject 后再搬到主线程。PrepareData 不得捕获临时 Registry、TaskReporter 引用、Pane、未保活的元信息 span。

`SessionStore.prepare()` 之后，Session 已在 Store 的未发布槽位，不能继续让候选 unique_ptr 保有它。reservation 失败清理必须撤销该隐藏槽位；清理角色后才能结束它所借用的 Session/代码。

### 6.2 现有接口不足时怎样补

当前 `SaveService::registerSource()` 是立即注册入口，不能直接假定它能与另一个 owner 的 publish 原子组合。

先证明现有 gate/注册可见性是否足够。如果不够，只添加保存注册的窄 prepare/commit/abandon 机制，与 `PreparedSessionInstallation` 组合：

- prepare 预留资源但外部查询不可见；
- commit 只进行事先准备好的无普通失败状态转移，不执行外部 callback/分配/IO；
- abandon 释放准备资源，不调用不存在会话的角色；
- 原 registerSource 可由这套唯一算法实现，但不能保留第二套注册算法。

E0 SessionStore 不 include SaveService。跨 owner 的安装编排在 E2；E0 只提供会话发布的必要事实。不得建立公共 TransactionManager 为这一项兜底。

### 6.3 必须插入失败点的位置

读入前；decode 后；reserve 后；Session 构造后；Store.prepare 后；第一角色准备后；最后角色准备后；publish 前；通知时。

在每个 publish 前失败点都检查：Store 已发布数量不增加、无可查新 SessionId、无对外可调用的保存/历史角色、输入/代码只释放一次、后续正常安装仍可用。

publish 后通知失败要报告“已安装但通知失败”，不能抹掉会话或伪装未发生。

## 7. 代码 owner 与 ABI

### 7.1 每一种晚释放对象都要列入

命令 callable、prepared data、动态 Session、SaveSource、编码 job、rebind candidate、DetachedView、反射/配置值、错误 payload、已返回的编译/运行结果。

外层 code 字段先声明只是必要条件之一。还要检查移动赋值、swap、reset、异常展开和虚析构尾部；旧 payload/对象析构时旧 code 必须仍然存在。

不要让插件对象仅以自己的成员 CodeLease 自证安全；该成员结束后虚析构尾部仍可能在插件代码中。使用原 Store/DetachedView/OwnedEncodeJob 等外部拥有关系。

### 7.2 当前 V6 出口与新出口

当前已核对 `lux_editor_exports_v6` / interface_version=6，且表里包含旧 PaneRegistration、AssetEditorRegistration、CommandRegistration。

P11 定义新的 Editor 贡献契约版本和出口（具体新编号先核对当前分支，不能复用相同版本改变布局），先验证 header/version/大小/数量/依赖，才调用贡献入口。旧 V6 仅作为负例输入，不由正式新 loader 静默接受。

复用 `lux::project::loadPluginLibrary`、PluginDescription、runtimeCode 和依赖 pin。不要在 Editor 再实现 LoadLibrary/dlopen 封装或第二 PluginManager。

SDK 指纹应限定真正的 C++ 接口和工具链契约，不把 editor 的变化污染 runtime 插件；也不宣称哈希相同就意味着跨所有编译器 ABI 稳定。

### 7.3 首版不做任意热卸载

有对象/任务/回调在途时，卸载请求准确拒绝或等待原 owner 退休。只改注册快照不等于 DLL 已卸载；最后资源、payload、动态 deleter 结束后才允许释放原 code owner。

缺少真实装载运行的测试，只能称编译/链接资格；不能以 shared_ptr witness 代替全部插件装载资格。

## 8. concept / virtual / signal 的落地选择

| 场合 | 选择 |
|---|---|
| 具体工厂和 codec 的编译期适配 | concept + 小 helper，实例化在提供者 CPP。 |
| 未知 Session/角色/视图 factory 的运行时集合 | 复用窄 virtual 或单次 owning type erasure。 |
| 通知目录/命令可用性变化、控件事件 | 原 LuxObject/Connection。 |
| 返回 Result 的同步查询/准入 | 普通准确方法，不强行改成无返回信号。 |
| 某种业务没有替换需要 | 具体服务引用，不为 mock 创建一套 I 类。 |

可使用如下目标签名检查；实际 Result/类型名以唯一生产定义为准，不复制示例类型：

```cpp
template<class Q>
concept CommandQueryCallable = requires(Q& query, const CommandQuery& input) {
    { query(input) } -> std::same_as<CommandResult<CommandState>>;
};

template<class E>
concept CommandExecuteCallable = requires(E& execute, CommandInvocation& input) {
    { execute(input) } -> std::same_as<CommandResult<DispatchReceipt>>;
};
```

执行器仍拥有排队 invocation。execute 的可修改引用仅用于成功准入后的拥有型输入交接，目标、注册政策和 based_on 不得被重写；BUSY 必须保留原载荷。若任务 API 在准入成功后才调用 factory，应在 factory 内移动输入，不能提前按值消费后再声称可重试。

不要给能够分配/解码的函数无依据强加 noexcept。concept 证明不了 query 无副作用、snapshot 深层拥有或工厂线程亲和；这些另用真实测试证明。

不要求引入所有列出的概念。只有存在正式 helper 和真实用途才添加；至少用两种实际内置类型或一个真实开放扩展验证它。独立玩具示例不是 SDK 正例。

## 9. 各批具体施工与删除

### P11-A：登记真实消费者

读取当前 frozen 账本，展开 metadata/plugins/context/app 的实际 source、public headers、生成输入、target、安装包和依赖。

输出本阶段精确成员计划，不复制一整份新的可变账本。所有 `consumers=[]` 的生成目标先查 generator 和 MANUALLY_ADDED_DEPENDENCIES，不能直接删。

### P11-B：命令核心与菜单接点

先实现 B 节的数据、目标政策和 snapshot；再在新工作台使用真实菜单和快捷键产生 invocation。替换新链的旧 `ui::Command::QUERY/EXECUTE` 混合入口。

普通点击不能新建用户看不见的会话。queued target 失效应拒绝，不转向另一个同类型窗口。实际删除目标和替换 registry 的测试必须在入队与执行之间发生。

### P11-C：内容工厂与安装

实现三类真实 decode/construct/roles 发布；复用对应 `SceneCodec/MaterialCodec/FlowCodec`、PersistenceAccess 和原模型创建器。

没有定义支持的源类型不得假成功。无保存能力/无历史能力的类型应有准确 capability，不能将所有工具强制变成作者 Session。

### P11-D：贡献拆分

- ConfigurationValue 的纯值/codec/反射事实归适合的作者域或已有正式值。
- 组件/配置 UI factory 归 workbench 对应域，复用原生成器，不复制生成实现。
- 保存/内容工厂归 activities；UI factory 归 workbench。
- 装载和组合归 application/extensions。
- 原 engine runtime 插件接口和纯 schema 不跟 Editor 总表一起改名。

### P11-E：真实内置与外部消费

Scene/Material/Flow 的内置注册由 application 组合，业务 consumer 只接收其必要服务。

至少验证一个外部安装扩展：一个实际可构造/销毁的最小 Session 或对现有 Session 的保存角色，一个 DetachedView 和一个 query/execute 命令；验证结果、关闭和最后代码寿命。无实际贡献的空 DLL 不计。

### P11-F：删除与旧产品短暂共存

已经迁出的配置/控件/反射算法删除原体，更新所有直接消费者。优先使旧产品直接消费同一正式不可变注册能力；不能把旧 `invoke(Context&, Command&)` 包在一个同名新接口里冒充迁移。

旧 product 在 P12 才切换，无法独立替换的原工厂声明/创建接线可按 D04 逐项暂留，必须给出路径、当前消费者和 P12 的具体替代步骤。禁止增加新的 bridge、扩展旧白名单或保留零消费者文件。

C03：正式新路径的同义原断言必须通过。若旧 executable 当前仍保留 query 路径，其活动 probe 也不得继续发生代码提前释放；可用调用期保活的最小修正或直接迁移到同一已验证机制，不重写第二命令框架。修正记录随旧实现 P12 删除。历史 C03 FAIL 证据原字节保留。

本阶段结束不能宣称旧工厂所有头已删除而实际仍导出它们。交接中分列“P11 正式新契约”“P12 旧产品最后消费者”；P12 前冻结的有限剩余项不得再次增长。

### P11-G：资格与停点

同一最终实现 SHA：完整适用 Windows 构建、二次无工作、原回归、真实依赖负例、安装 SDK、新外部扩展、C03、新工作台命令场景。

C01/C04 若仍是旧路径失败继续准确记录；不得把它们泛化为任何未修问题的豁免。输出实现与独立验收提交，停在 P11。

## 10. P11 验收主题

沿用原 X11-01–07 的含义；下面的补充属于这些主题，不要求每行一个 executable。

| ID | 必测输入/时机 | 观察 |
|---|---|---|
| X11-01 | query 请求替换自身；释放外部 handle；闭包继续执行 | 原 entry/代码到返回后仍活；更新延迟；无悬垂或重复调用。 |
| X11-02 | 一批视图/内容准备中，第一个 factory 请求更新贡献 | 全批固定原 snapshot；失败候选清理代码顺序正确；下一批才见新版本。 |
| X11-03 | 点击 A→入队→焦点 B→替换注册→A 被关/槽位复用 | PINNED 正确、CURRENT 明确；不漂移；旧身份拒绝；参数非借用。 |
| X11-04 | worker decode、owner 延后构造，逐角色注入准备失败 | Session/History owner 线程正确；任何提前失败无半发布、无半注册。 |
| X11-05 | 命令/动态源/View/错误值最后 owner 与虚析构尾部 | 最后外部 code pin 晚于所有使用；move/reset/异常展开也正确。 |
| X11-06 | 新安装前缀构建真实外部扩展，另装不兼容 V6 | 正式扩展可用；V6 拒绝在贡献调用前；无源树私有 include。 |
| X11-07 | 更改 Editor 契约版本，独立构建 runtime 插件 | runtime 不引入 Editor，拒绝/升级范围准确。 |
| X11-A | 注册队列满、查询递归、调用中取消/销毁 | 无无限排队，无角色过早销毁；BUSY 不丢已接受任务完成。 |
| X11-B | Save 在旧菜单点击后源更新；DeleteSelection 的原集合改变 | 各自政策正确，不把所有命令用同一 revision 规则。 |
| X11-C | factory 不匹配、重复类型、未知配置、预算失败 | 完整批次拒绝或按明确策略跳过；无默认成功空实现。 |
| X11-D | 安装后新 Window/Task/Save 分别释放，再释放扩展 | 当前 producer 先撤销，已接受结果能结清，用户数据不被自动删除。 |

负例须失败于指定规则；缺包/缺链接器不算 concept 或禁边成功。Linux/IME 未测不阻塞此阶段限定范围。

## 11. 给 P12 的最小交接

- 实现与验收 SHA、当前真实分支和新依赖/SDK版本；
- 命令与工厂完整入口表；原功能矩阵和尚属 P12 的组合功能；
- 新贡献版本/导出符号与外部 SDK 资格；
- 三类 InstalledSession 所有权、关闭/保存能力和销毁顺序；
- 固定的旧文件/符号/target/安装包/P12消费者清单；
- C03 新旧活动路径结果、历史失败保全、C01/C04当前状态；
- 结构门禁和剩余测试环境范围。

P11 本身不得以“P12 会接线”为理由，只交付没有真实使用者的接口。也不得为让旧产品继续工作而允许任何正式内层依赖旧 Context。


---

<a id="unit-3"></a>

<!-- 合并来源：02_P12_IMPLEMENTATION.md -->

# P12 — 完整产品接通、唯一入口切换与残留清零

本文件只有在 P11 实施与复审完成后才开始执行。遵守 `00_MASTER.md`，以实际 P11 验收 SHA 为基线，不 reset 到本文引用的旧 SHA。

> P12 的完成标准是用户运行的正式 Editor 已经使用新系统，旧框架没有活动消费者并已删除。集成 harness 能运行、不代表产品已切换；删除原文件夹、不代表原责任已迁移。

## 1. 本阶段出口

- 原有适用功能全部有正式新产品入口，并有行为映射。
- Open/Reload/SaveAll/Close/Exit/Layout/Restore/模型插入的组合语义完整。
- C01、C04 对应契约在新产品实际通过，P11 的 C03 继续通过；历史失败原样保留。
- 现有 `lux_editor` target 只定义一次，定义位置归 `editor/application`，不再链接旧 editor_app/context/ui/三大 Editor。
- launcher 保留其真实启动和创建项目功能，但不再是旧 Editor 的另一入口。
- `editor` tracked 一级目录只剩 `editing, authoring, activities, workbench, application, tests`。
- 旧 source、声明、friend、TestAccess、导出、生成器输入、CMake 入口、空转发 target、包 alias 和可运行旧产品路径同时清零。
- 正式保留的 engine/modules 能力、纯数据兼容读取、用户数据和历史记录没有被误删。

P13 不接收以上未完成项。未完成则 P12 PARTIAL/BLOCKED，不能通过修改门槛或改名字收尾。

## 2. 不再建一套 Workflow Framework

用户用例确实需要状态，但不需要共同的 Workflow 基类或通用反射调度器。

- 同步、无在途责任的操作使用自由函数或现有服务方法。
- 跨帧操作用具体 move-only 记录或实际服务内有界记录；每个状态必须对应真实等待条件。
- 根应用组合这些服务，不替各服务保存 source、write outcome、compiler result 等第二份事实。
- 现有业务 IDs 与 TaskId 分开；task cancellation 不等于业务已撤销。
- UI 消失不自动取消应用拥有的内容、保存、编译或运行。

以下类型名是建议的语义分组；可以合并紧密相关的值到同一头，不要求每个值对应单独 CPP、库或类。

## 3. 类型、层与文件责任

| 类型/职责 | 正式位置 | 持有内容 | 不应持有 |
|---|---|---|---|
| `OpenAssetOperation / OpenResult` | `activities/sessions` | 固定地址/工厂、加载结果、发布事实 | Pane/Host、第二 Session owner |
| `ReloadSessionOperation / ReloadOutcome` | `activities/sessions` | 审阅 stamp、旧写入等待、完整新候选 | 旧 Editor、UI 对话框 |
| `SaveAllOperation / SaveAllReport` | `activities/sessions` | 固定 SessionId 集、现有 SaveId 和每项结果 | Pane 集、另一份保存状态机 |
| `CloseSessionsOperation / CloseReport` | `activities/sessions` | 确定内容集合、选择版本、原 ClosePermit | 具体 Pane/Renderer 私有结构 |
| `InstalledSession` / 安装角色目录 | 复用 P11 `activities/sessions` | 角色/注册/关闭能力、SessionId | 源/历史重复所有权 |
| `ModelCreationOperation` | `activities/scene` | 已加载模型、原目标/stamp、参数、一次提交结果 | 视图指针、RuntimeEntity 代替作者身份 |
| `ApplyLayout` 的 UI 准备/提交 | `workbench/desktop` | 固定布局计划、原 ViewId、候选和挂载准备 | 文件 IO、打开内容、保存源 |
| `OpenAndShow / CloseView / RestoreWorkbench / ExitEditor` | `application/src` | 跨内容和工作台的组合进度 | 两边私有状态或万能 service map |
| `EditorApplication / Config` | `application` | 正式服务、相位/总生命周期 | 所有具体操作的共享大状态袋 |
| 未保存/错误/缺扩展提示 UI | `workbench/desktop` | 拥有型问题值、明确用户选择 | 未核验的延迟裸指针 |

构建目标按真实闭包划分，不为每项 Operation 新建 library。已有保存、编译、Run、ProjectStorage 和 WorkspaceStore 的 target 与算法优先保持。

## 4. 需要先补齐的窄 owner 能力

### 4.1 会话枚举

当前 SessionStore 有 size/describe，没有可假定存在的全量枚举 API。SaveAll/Exit 必须枚举会话，不能改为枚举 View。

若 P11 未补足，添加一个 owner-thread `snapshotIds()` 类窄方法，返回拥有的 SessionId 列表和准确错误，不暴露槽位、IEditSession* 或可写容器。按实际命名保持唯一接口。分配函数不无依据声明 noexcept。

枚举结果只固定这次操作集合；每项使用前仍检查代际。新开的 Session 不自动加入既有 SaveAll；退出期间新建请求按明确的应用退出准入拒绝或延期。

### 4.2 一组关闭许可的提交

当前 `close(ClosePermit&)` 单项接口不自动证明多会话原子关闭。

优先复用现有完整准备能力。如果逐项 close 会在第二项失败时已经删除第一项，必须在原 Store 增加窄的“验证全部已准备许可→一次提交集合”能力；不在 application 通过假 rollback 补救。

该能力只能知道 Session/permit/身份，不能 include ViewHost、SaveService 或 RunStore。逻辑删除的可见性与随后对象析构清理分开；析构回调中的 Store 访问继续遵守原 reclaiming 保护。

### 4.3 工作台多视图事务

当前 ViewHost 的 adopt/drain 是单视图/队列能力。完整布局计划和批量关闭要检查是否能满足失败不改原 UI。

只在原 ViewHost/Root 准备协议中补需要的批量候选、路由预留和一次安全提交；不创造第二 DockManager/WorkspaceHost。PreparedViewBatch 是一次性拥有责任，不能复制。

### 4.4 共同原则

准入时保证必需资源/依赖，连续无回调的内部处理复用验证结果；跨回调、异步和外部 IO 后重验可能变化的事实。不要把“少写 if”当作完成标准。

## 5. P12-A：冻结真实功能和清理计划

从 P11 最终提交出发，读取：

- 正式五层的 headers/targets/source map；
- P11 内置命令、SessionFactory、ViewFactory、角色和动态 SDK 的资格；
- 当前旧 `editor/app/product` 菜单、旧三个工具、launcher、settings、旧协议测试；
- `retained-product.json` 经 P11 更新的精确剩余消费者；
- 原 C01/C03/C04 断言与已有真实 IO/GPU/输入测试。

每个适用用户功能记录五列：旧入口、正式新入口、底层唯一 owner、等价测试、旧实现退出提交。不得只列类型名。

必须包含创建/打开/保存/另存为/导出副本、撤销重做、对象/组件/父子/配置、材质/Flow 图功能、编译/重链接/发布、模型插入、作者和 Run 切换、运行检查、资源重试、项目浏览/创建、任务取消、设置、布局/恢复、最后视图和退出。

新机制明确拒绝的非原有能力可记录边界；原已支持功能不能以 unsupported 保留到 P13。

## 6. P12-B：内容操作

### 6.1 OpenAsset 与 OpenAndShow

**内容打开放 E2，显示组合放 E4。** 不让 activities include IViewHost。

状态和提交点：

```text
REQUESTED
  → 校验 project instance / AssetAddress / SessionKind
  → 查已打开或正在打开的同一工作副本
  → 固定 P11 工厂 snapshot
  → Process 读取和 decode
  → owner 构造与完整安装
  → CONTENT_PUBLISHED
  → E4 复用或创建视图
  → PRESENTED / CONTENT_WITHOUT_VIEW / PRESENTATION_FAILED
```

要求：

1. 默认按 `(项目实例, 源定位, 会话种类, 工作副本政策)` 复用，不按 Pane 类型找第一个。
2. 同一内容的并发重复打开合并到已有操作或明确复用；不能发布两个意外工作副本。
3. 需要显式第二工作副本时必须是独立意图，不能由第二视图隐式触发。
4. 单个等待者取消不销毁其他等待者已需要的共同工作；所有已接受工作最终结清。
5. Session 发布前取消/失败不留可查半对象；发布后取消是部分完成事实，不能悄悄删除已发布内容。
6. 新 Session 成功但 ViewFactory/adopt/focus 失败，保留可访问的无视图会话，报告具体失败。用户之后可以重新打开视图或关闭内容。
7. 读到的字节版本与 SourceBinding 一致；源在读入期间变化时按正式版本规则处理，不把新路径和旧内容拼成一个假绑定。
8. Source 与角色 owner 先后依照 P11 安装协议；不把一个 std::unique_ptr 同时交给 Store 和 InstalledSession。

不能用“一次创建 Session 再立即关掉”的补偿来抹掉已经通知外部的发布事实。

### 6.2 ReloadSession

重载保留 SessionId，但建立新的内容/History 域；这与 Save As 完全不同。

顺序：

1. 确认目标会话、来源、未保存内容，以及用户回答所依据的完整 ContentStamp。
2. 识别同目标、旧来源的在途写入。Unknown 仍可能晚发布时不继续采用。
3. 得到可读取的稳定来源，固定工厂，读取/解码新源；不先 clear 原内容。
4. 通过现有具体 PreparedReload/作者内部能力构造完整新源、索引、History、checkpoint 和角色候选。
5. 最终采用点再次核对被审阅 stamp、绑定版本和写入集合。读取期间新增的旧来源写入也必须结清或拒绝本次候选。
6. 所有可能执行 codec/工厂回调的准备结束后，在原 owner 短安全点采用；不能在“最后一次写入核验”和交换之间再调用任意业务回调。
7. 内容变化需新审阅或返回冲突；临时 BUSY 保留候选，不自动换 expected。
8. 成功后旧来源的编译、投影、保存采用按 History/内容身份准确失效；已发生的旧文件发布事实不抹掉。

若现有 WriteCoordinator 缺少所需的在途目标查询，可加窄只读查询/正式协调请求。不要再创建一份文件写队列；不要只等 `Task.finished`。

不要求锁住 UI 直到磁盘完成。允许继续编辑时就必须真正通过最终 stamp 校验；不要在读入初期持长时间 READING 来省掉这个问题。

### 6.3 SaveAll

输入来自 `SessionStore.snapshotIds()`，不是 PaneManager 或 ViewHost。

每项分别记录：无保存角色、未绑定需 Save As、用户取消、准入失败、已接纳、发布 Unknown/成功/失败、基线采用结果。结果引用现有 SaveId，不重跑 encoder、不复制保存状态机。

两个视图同源只保存一次；无窗口但 dirty 的会话也在本次集合。后续新开会话不偷偷加入本次操作。

工作台触发保存时，对当前明确涉及的交互组先完成或取消其未提交手势；冲突/无效草稿不得被静默提交。该 UI 预处理在 E3/E4，SaveService 和 E2 SaveAll 只保存已提交源，不暗中结束其他窗口的编辑。

“所有 request 已被接受”不是“全部已保存”。某项完成后又有新编辑，可以报告该捕获版本已保存、当前仍 dirty，不篡改 SaveReceipt。

### 6.4 CloseSessions

先收集决定，再启动不可逆关闭。

```text
固定 Session 集和原观察
  → 收集带 stamp 的 Save / Discard / Cancel 决定
  → 执行必要保存，保留磁盘真实结果
  → 重验所有最终决定与 current / dirty
  → 准备全部关闭许可、角色撤销、相关 view/run 依赖计划
  → 全部可提交后进入不可逆关闭阶段
  → 原 owner 完成资源退休和 Store 逻辑关闭
  → 逐项/聚合结果
```

在用户决定尚未收齐或第二张许可失败时：不删除任何会话、不关任何视图、不先停止另一个独立 Run。A 已经成功保存的磁盘结果保留，不伪装回滚。

Discard(S10) 不能授权删除 S12。保存 S10 后用户编辑到 S12，不能以刚才保存成功直接通过关闭。

内容关闭时存在 Run：明确询问/政策决定 Stop 或 KeepFrozenRun；选择保留只能在证明它完全依赖冻结源和环境时成立。不能由“关闭 View”隐式推断停止 Run。

内容关闭关联 UI 的具体组合由 E4 负责。E2 返回准备好的内容关闭结果/许可；E3 准备视图解绑；不要让 E2 源码包含 Pane、Root、SceneView。

进入不可逆阶段之后发生的资源失败要返回真实 pending/failed 状态，继续保留足够 owner 安全收尾；不恢复已销毁对象，不让错误直接落入析构。

已准入 Save/Compile/Run/GPU 的完成依照原服务结清。控制关闭责任不得通过一个新的万能 SessionCloseDependencies::get<T>() 实现。

## 7. P12-C：工作台整体操作

### 7.1 Hide / CloseView / CloseSession / Exit

| 动作 | 默认效果 |
|---|---|
| HideView | 只改变可见性，不触发内容关闭。 |
| CloseView | 关闭一个视图，退休它的资源；不关闭共享源。 |
| 最后视图 CloseView | 明确询问 keep session / close content / cancel；回答前不先销毁。 |
| CloseSession | 使用上述内容关闭用例，处理关联视图/运行政策。 |
| ExitEditor | 对固定的项目/内容集合进行总审阅和资源结束，再退出平台。 |

ViewHost 的原 close request 不是产品“已完成全部关闭”的证明。操作记录需要查询真正的 drain/retirement 和关闭错误。

### 7.2 ApplyLayout：真实批量准备，而不是循环创建后假回滚

输入：E2 WorkspaceStore 读到的 DockLayout，E1 纯 LayoutPlan，E3 当前视图观察和固定 ViewFactory 快照。

具体步骤：

1. 完整解析/验证；坏 Dock 数据必须在任何 visibility/focus/挂载变化之前拒绝。
2. 固定当前 ViewId、restore key/type、原配置和相关工作台版本；计划只做精确匹配。
3. 默认保留额外窗口、现有绑定和 dirty 内容；不打开 opaque locator。
4. 一次准备所需 DetachedView、配置校验、Dock 输出、挂载/路由槽、解绑和焦点变更。
5. provider 构造或配置验证失败时，销毁未发布候选，当前 Root、可见性、选择和内容不变。
6. 提交前重验 View 代际和工作台版本；变化则取消或重新规划，不将旧计划应用到新对象。
7. 对受影响交互默认 CancelPreview，按原 gate 完成；未成功前不修改结构。不能暗中提交草稿。
8. 安全点只执行已准备的拥有权/结构转移，不再调用会失败的 provider 创建函数或文件 IO。
9. 提交后通知及偏好持久化单独记录。偏好写失败不意味着布局没有应用；通知产生的结构请求进入下一批。

本用例只保证契约内 UI 结构准备，不承诺回滚任意插件偷偷执行的网络/文件副作用。工厂收到的能力应足够窄，违规行为单独拒绝或诊断。

C01 的核心观察必须覆盖：原窗口 hidden，输入 malformed dock，结果拒绝且窗口仍 hidden、数量不变、原 dock bytes/布局语义不变。

### 7.3 RestoreWorkbench

从独立 RecoveryManifest 经 OpenAsset 恢复内容，再由 E4 创建/复用视图并绑定。固定同一组 P11 贡献快照。

- 不聚合全部历史布局的 locator；继承 P09 R1 selected 范围。
- 不声称只凭资产 locator 可以恢复未保存修改；没有真实恢复快照就明确只能恢复已发布内容。
- 缺 provider/类型/资产时保留未知数据并报告，不能无提示删除清单项。
- 已存在用户修改过的 recovery/marker 不自动覆盖；旧格式只读导入保持幂等。
- 单项失败不隐藏其他项实际已打开的内容；结果区分复用、新建、未恢复和未显示。
- 自动恢复出现错误不能陷入每帧重开无限循环；重试条件和用户明确触发分开。

## 8. P12-D：所有实际工具入口

### 8.1 三种作者工具

继续直接使用已有 Session/Interaction/Preview/Compilation 服务，不重写纯编辑和编码算法。

- Scene：对象创建/删除、父关系、组件增删/字段、配置、相机、模型放置、资源状态和重试。
- Material：图、常量、纹理/参数、持久节点布局、编译、预览、发布。
- Flow：变量、函数与签名、Pin/连接、字面值、导出、布局、编译、固定产物链接重试和发布。
- 所有 UI 草稿/队列保留 based_on，显示刷新不能使旧草稿变新。
- 两个视图共享同一 Session/History；作者/运行目标路由显式，不因焦点变化误写。

### 8.2 运行 Inspector 不得靠保留旧 SceneEditor 继续提供

P10 原报告没有宣称新的运行 Inspector 全部完成。本阶段先从旧功能矩阵确认已有支持范围，再用 RunInspectAccess/原运行编辑能力接入正式 Inspector。

只读检查必须可用。原来支持的运行编辑若属于产品功能，继续经 activities/scene 的真实 Registry 编辑能力执行，并且不写作者 checkpoint/history。不得让作者 Inspector 获得任意可写 Run Registry。

不因 `editor_editing_scene` 名字像旧模块就删掉其正式运行消费者。消除旧 UI 适配，不删除有用的原运行算法。

### 8.3 模型插入

用户触发时固定资产引用、目标 Session/History、内容戳、放置参数。Process 只读入/解码；owner 在实际提交前核验目标和许可，调用一次原 SceneEditBatch 插入。

模型读完但目标已关闭/换 History、预算不足或取消：最终结果不是 Inserted，源不半改。UI 不可通过视图已关闭来丢掉已接纳操作的结清责任。

### 8.4 项目、任务、设置和启动器

项目创建复用纯 ProjectBuilder 与原 ProjectCreation/Storage；AssetImporter 继续在 E2。TaskView 借 TaskMonitor，不抢 Runtime observer。

设置/插件选择保留“下次打开生效”与“立即更新注册”的明确区别；首版不要求任意热重载。重开项目的流程不得自动保存/清除未经审阅内容。

原 launcher 的项目选择/创建 UI 迁到 `workbench/project` 或 settings 的合理位置；其进程启动继续复用 `application/launch/launchEditor`。只作为选择/启动器的独立 exe 可以存在，但不能是第二份旧 Editor。

## 9. P12-E：EditorApplication 与唯一推进

### 9.1 构造失败必须真实失败

`EditorApplication::create(Config)` 返回完整应用或准确错误。步骤至少覆盖原引擎/项目服务、插件依赖、内容角色、工作台、输入与菜单连接、初始布局准备、必要持久化设施。

每一步失败都清理局部候选，不能发布“已构造成功但 outcome 已失败”的对象。

特别保留 C04：菜单连接失败引起 create 返回 `object.connect` 或新准确等价诊断；不把它换成退出失败测试。构造期 Result 和运行期 fault channel 分开。

允许在真正准备好的无业务失败位置使用 noexcept；含分配、插件构造、打开文件和任意 callback 的地方不得未经处理统一 noexcept。

### 9.2 相位与驱动

将目前正式集成使用的真实时序写成一份循环，不让每个 View/RunStore 自己 tick 原 SceneRuntime。

阶段必须覆盖：输入收集、Process 完成吸收、业务结果采用、操作推进、UI 准备与借用结束、唯一 Scene 驱动、Render 提交/采用和安全退休。具体顺序遵守已有 runtime/UI 同步依赖，不机械照抄一行伪时序。

不能停止消息泵后再等待只有该泵才能完成的 task/GPU/dispatch。背压保留原包，不能重复 draw 以重放业务。

通用 activity 不读全局 App。应用装配通过必要引用与 Connections 接线，禁止 getService<T> 或 string service map。

### 9.3 退出顺序与不可逆点

推荐用明确的阶段状态表达，而不是一个 closing bool：

```text
RUNNING
  → REVIEWING（可取消，无提前销毁）
  → COMMITTING_EXIT（决定与许可最终核验）
  → DRAINING（停止新准入，已接纳结果可靠结清）
  → RELEASED（平台/执行器等最后依赖结束）
```

这些是本应用的生命周期，不替各服务复制状态。

进入 DRAINING 后：

- 停止新用户业务准入，但继续接收已准入任务/文件/资源完成。
- 按确定政策结束保存/编译和 Run/预览；Unknown 文件发布未能确认 writer 退休时不能假完成。
- 结束工作台交互、路由、视图和它们的 GPU 责任；窗口和 surface 活到真实退休。
- 撤销保存/历史/扩展角色的未来调用，等待当前回调结束。
- Session/source/history 及仍需插件代码的结果/闭包按实际引用依赖结束。
- 释放代码/插件，再结束必要原平台和执行器；Runtime 必须活得比自己的 Task/Scope/lease 长。

不要将这一列表机械写成固定析构成员顺序；检查真实借用图。析构不弹对话框、不隐式保存、不启动新业务，不强制终止线程来伪造退出成功。

错误不能抹掉已经成功的文件发布、已停止 Run 或部分关闭事实；报告首个原始失败和后续实际清理状态。

## 10. P12-F：切换正式产品

1. 正式新组合先经集成测试和实际 UI/GPU 路径运行。
2. 将当前 `lux_editor` 的定义从 `editor/app/CMakeLists.txt` 移到 `editor/application`；同一阶段只保留一个定义和安装入口。
3. 内置菜单、三工具、插件贡献、项目创建、launchEditor、示例和外部包指向正式新契约。
4. 新产品路径不得 include 旧 EditorContext/PaneManager/三大 Editor/旧 TestAccess；不得用旧 dll 完成隐藏工作。
5. 新 harness 仅作为测试程序，不安装成 NewEditor，也不保留 `--old-editor` 或自动回落。
6. 第一次安装到新空前缀并独立运行，避免旧开发 SDK 的残留补齐未声明依赖。
7. 实际启动已安装 `lux_editor`，证明使用新 bootstrap；仅运行链接新库的测试 exe 不足以证明产品入口切换。

`editor_bootstrap` 是可采用的真实装配 target 名，不是必须新建空转发 target。它若存在必须有实际装配源，不只是链接全部 Editor 库的 INTERFACE 壳。

## 11. P12-G：残留清零

严格依照 `03_REMOVAL_PLAN.md` 逐文件/符号/target 删除。九个旧根目录到阶段结束全部消失：

```text
editor/app
editor/assets
editor/context
editor/launcher
editor/metadata
editor/plugins
editor/tools
editor/transition
editor/ui
```

同时清理隐藏在正式根中的旧协议，例如 `editor/editing` 内的 old editor_editing/AssetSave/CloseRequest，或活动 support 中只服务旧窗口的 TAssetSave/Legacy*。不能只检测上面的目录。

必须保护：

- 正式的 editor_assets/AssetImporter、editor_storage/ProjectStorage、editor_editing_scene/运行编辑、editor_launch；
- History/SessionState/Store、原 codec、纯分区解码及其 Process 复用、renderer/process/对象基建；
- 版本化 LegacyWorkspaceImporter，它是数据格式读取，不是旧产品 owner；
- 用户文件、ProjectBuilder 用户补丁、历史失败/收据和负向样本。

原 legacy 根中的测试有价值则移入实际新测试域后删除旧入口；不能在 tests/legacy 再编译一套旧 Editor 来维持测试数量。

删除时同时处理 `.hpp/.cpp`、声明、定义、调用、friend、生成输入、CMake、source include 根、包/导出、visibility 头、运行库安装、示例、脚本和长期文档。历史 dev_log 中的旧名字应保留，不作全局替换。

## 12. P12-H：最终资格

### 12.1 原 X12 核心含义保留

| ID | 输入/故障 | 必须观察 |
|---|---|---|
| X12-01 | Session 已发布，视图创建/采用/聚焦失败 | 无视图会话仍可用；OpenResult 表达真实部分完成。 |
| X12-02 | 原保存晚发布、Unknown、重载期间新编辑/新保存 | 不采用过时读取；不丢未经审阅内容；失败保留原源和历史。 |
| X12-03 | A 保存后 B Cancel，第二张许可失败 | 所有内容/视图仍在；A 真保存保留；许可释放。 |
| X12-04 | 最后视图 keep/close/cancel | 三种语义明确，回答前无销毁；Run政策明确。 |
| X12-05 | dirty 双视图、额外窗口、缺 provider、坏 dock、偏好写失败 | 内容不变；结构提交准确；偏好失败独立；C01对应断言通过。 |
| X12-06 | 两窗同源、无视图源、无绑定源、新开源 | 按固定 Session 集去重并逐项报告，不假全部保存。 |
| X12-07 | 模型加载成功，提交前目标换代/预算失败 | 最终不报插入成功，作者源原子不变。 |
| X12-08 | 构造每个必要步骤及菜单连接失败 | create 返回失败，没有已发布半应用，C04含义保持。 |
| X12-09 | 编码、运行、GPU、插件回调同时在途退出 | 可靠完成与真实退休；无消息泵先停、无代码提前释放。 |
| X12-10 | 实际 exe/插件/安装/源码/target 扫描和启动 | 唯一新产品；零旧 owner/bridge/回落；五层 + tests。 |

### 12.2 不可省的组合补充

- content open 去重后单个等待者取消，不取消其他等待者。
- 保存 S10 后编辑 S12，关闭仍要审阅；Discard(S10) 也不能丢 S12。
- 布局中第 N 个 factory 失败，所有原窗口可见性/数量/绑定保持；提交后通知失败不假回滚。
- layout/recovery 请求中的旧 ViewId、新 generation 不发生误绑定。
- 新产品中运行 Inspector、编译与固定对象重链接、Save As 历史保留、Export Copy 不改 checkpoint。
- 视图关闭后已准入保存完成；扩展更新后旧结果仍安全释放。
- 旧 SDK/包目录不参与新安装消费者；删除旧导出后新插件和原 runtime 插件各自仍可用。

### 12.3 允许范围

Windows Editor/CPU/PLAYER、当前适用安装消费者、实际新双视口/验证层、原生输入、真实文件 IO、依赖/概念/operation负例和必要生成重建。

Linux/系统 IME/ASan 没有真实执行就保持 NOT_RUN。不要把此处改成新的环境建设项目，也不要因未测而宣称整个引擎跨平台失败或已通过。

原慢算法性能样本不补跑；仅改到的性能路径做有决策价值的短回归。测试必须使用隔离目录，不用隐蔽自动重试掩盖 Access denied。

## 13. 最终提交前的十问

1. 实际运行的 lux_editor 是否只进入新 EditorApplication？
2. 任一正式层是否仍通过公开头、私有 include、模板或传递链接回到旧产品？
3. P11 的每一条 P12 暂留项是否已经关闭？
4. 旧根是否真实不在 Git 树，是否被改名藏到新的子目录？
5. 正式运行/导入等仍有消费者的算法是否被误删？
6. SDK 的旧包/头/DLL/生成脚本是否仍能让旧 C++ 体系运行？
7. 所有用户功能是否有实际正式入口，而非仅通过模型测试调用？
8. 退出是否仍可靠结清，并保留文件和运行的真实结果？
9. C01/C03/C04 的原意是否由当前正式代码通过、历史原结果是否未被改写？
10. 所有未验证环境是否仍准确列出，main 和用户修改是否保全？

全部得到可复核答案才能报告 `PASS（明确的 Windows/当前范围）`。P12 若仍有旧 Context/Editor 活动消费者，不允许用“架构基本完成，P13再删”交付。


---

<a id="unit-4"></a>

<!-- 合并来源：03_REMOVAL_PLAN.md -->

# 旧代码、目录、构建与安装残留的实际移除计划

这不是可以直接执行的批量 rm 清单。实施者必须为每项找到真实消费者，完成替代并运行对应行为，再删除原实现。完成删除意味着源、依赖和运行入口同时退出，不是文件换个目录。

## 1. 四类动作

| 动作 | 判据 | 出口 |
|---|---|---|
| KEEP_FORMAL | 已是五层中的真实正式 provider | 保持其语义/行为，可修正不准确的文档，不能按旧名字误删。 |
| EXTRACT_AND_DELETE | 旧文件中仍有唯一有用算法/配置/控件 | 迁到唯一正式 owner，消费者切换，删除原体和原导出。 |
| REPLACE_AND_DELETE | 旧 owner/协议已被新系统替代 | 实際消费者改用正式路径，原声明/定义/目标/包退出。 |
| RETAIN_DATA_OR_HISTORY | 数据读取兼容、用户备份、历史证据 | 保留并隔离说明，不被活动产品编译成旧系统。 |

不新增“先移 legacy，以后再说”第五类。

## 2. 九个旧根目录的最终处置

| ID | 当前根 | P11 处置 | P12 必须完成 | 删除前的等价证明 |
|---|---|---|---|---|
| R01 | `editor/metadata` | 拆分纯值/控件/角色/出口；正式新链不使用旧总表；删除已迁原体 | 剩余旧工厂与可变注册协议退出，旧 metadata aggregate target/包删除，目录清空 | 组件/配置/命令/Session/View 插件实际可用，代码寿命正确 |
| R02 | `editor/plugins` | 迁 Editor 贡献与生成输入到 application/extensions 或对应 workbench provider | 不再通过旧 exports/metadata 注册；原路径、生成 CMake 和多余 wrappers 删除 | 内置 scene/render/physics 等当前支持扩展逐项注册、配置和构造 |
| R03 | `editor/context` | 新链禁入；旧 query 的 C03 安全责任不得忽略 | 删除 EditorContext、PaneManager、私有 ContextAccess、旧共享 app owner | 新 App 服务组合、内容打开、工具复用、退出完整工作 |
| R04 | `editor/ui` | 已抽唯一算法直接共享；剩余控件/策略确定新归属 | 删除旧 UI 聚合、旧 InspectorInteraction/Spatial 兼容入口及生成支持 | 原控件/配置/任务/viewport 适用功能迁至正式 UI并实测 |
| R05 | `editor/tools` | 仅旧工具；新厂/新视图不得经它们获得业务能力 | 删除旧 SceneEditor/MaterialEditor/FlowForgeEditor、Impl、friends/TestAccess；settings/project旧壳迁完删 | 三工具正式功能矩阵+真实产品操作+旧行为等价测试 |
| R06 | `editor/launcher` | 现有 launchEditor 已共享；不提前复制项目创建 UI | 唯一真实 UI 归 workbench，main/进程接线归 application/launch；旧根删除 | 无项目启动、创建项目、打开项目、启动正式 Editor |
| R07 | `editor/app` | 旧 executable 仍工作；新命令/贡献以正式机制验证 | 当前 lux_editor target移至application；旧 Editor.cpp/Startup/Loop/Menu/Workspace/Platform/Impl/TestAccess 删除 | 安装后的真实 exe 启动/菜单/布局/退出；C01/C04通过 |
| R08 | `editor/assets` | 当前剩旧保存适配测试，不是新 AssetImporter provider | 有用测试迁到正式测试位置，旧 TAssetSave/SceneSave 等私有输出与CMake退出 | 正常保存/失败/重试/组合产物的原用户行为不丢 |
| R09 | `editor/transition` | 禁止新增；已无消费者的桥立即删 | 所有桥、LegacyPersistenceState、编译输入/配置/生成适配全部删，白名单为空 | 每项真实消费者的替代调用、构建和行为证据 |

P11 仍需要旧产品头的情形必须逐文件记录，不自动以这张根表作为所有内容的延期授权。P12 结束上表九个根都不应再出现在 `git ls-tree HEAD:editor`。

## 3. 正式目录内部的旧残留也必须处理

| ID | 位置/符号族 | 处理 |
|---|---|---|
| R10 | `editor/editing` 内 old `editor_editing` target、EditHistoryTarget 与旧混合资产/关闭协议 | 逐消费者迁移后删除旧 target/源；不要删除同目录的 edit_history/edit_sessions/editor_contracts。 |
| R11 | `AssetEditing.hpp / AssetOpenRequest.hpp / AssetSave.hpp / CloseRequest.hpp / CloseStatus.hpp` 等旧业务入口 | 替换为具体 Open/Save/Close 和明确 Result；删除已无消费者声明，不在原路径留 forwarding header。 |
| R12 | `activities/project/sinclude`、`activities/sinclude` 中仅旧产品使用的 `TAssetSave` 或 SaveRequest 类型 | 根据真实调用图删除旧算法/注册。Shared Result/Signal helpers 若仍为正式调用所需，不按所在旧逻辑 include 误删。 |
| R13 | 旧 `SceneSaveCapture::copied`、旧 SceneSave/MaterialSave/FlowSave、CompiledAsset 重编码 helpers | 保留新 SceneSaveSource/MaterialSaveSource/FlowSaveSource 与 Codec；只清旧接口和重复输出通道。 |
| R14 | 旧 `setCommands/setRegistrations → span`、`invoke(EditorContext&, ui::Command&)` | 正式当前条目使用不可变 snapshot + handle；当前回调不依赖可变集合。最后旧消费者退出后删全部原接口。 |
| R15 | 旧 PaneRegistration/AssetEditorRegistration/CommandRegistration | 新角色和 factory 完成后删除旧可执行 C++ 合同；数据中旧 type 字符串可由版本化导入器读取。 |
| R16 | 顶层 Root 自动注册构造、旧 `root()` 假设 | 删除精确兼容签名及调用，保留离树子节点组合、合法挂载访问和底层唯一 owner。 |
| R17 | 旧 TestAccess、friend、baseline-only 生产写入口 | 测试转为正式能力或窄故障点；不在安装 SDK 开放任意修改内部状态。 |
| R18 | 旧名字的新壳、INTERFACE 聚合 alias、旧 find_package 回落 | 只有真实实现才能保留 target 名；只有转发到新包的旧壳一律移除。 |

匹配符号族是调查提示，不是精确 AST 删除命令。`SceneSaveSource` 包含 SceneSave 字样但不是旧 SceneSave；`LegacyWorkspaceImporter` 是需要保留的数据导入；不能以子串替换破坏它们。

## 4. 明确的保留清单

| 对象 | 为什么保留 |
|---|---|
| `editor/activities/project` 的 editor_storage / editor_assets | CMake 中分别有真实 ProjectStorage 和 AssetImporter 源，不是空兼容 target。 |
| `editor/activities/scene` 的 editor_editing_scene | 真实 SceneEditing/ComponentEdit 被 RunStore 消费；清理旧窗口不等于删除运行编辑。 |
| `editor/application/launch` 的 editor_launch | 已有唯一进程启动实现；复用它而非重写平台启动。 |
| 纯 ProjectBuilder/ProjectBuildConfig | 只构造和验证配置，不是旧编译工作流。 |
| LegacyWorkspaceImporter/原 codec/旧格式只读数据 | 保留用户数据可读性；不允许它们成为重新打开旧业务系统的入口。 |
| 原 World 分区纯解码与 Process 复用 | 本次结构整改已经消除反向依赖，不能再将纯解码拉回 Process。 |
| CodeLease/History/Session/Runtime 的共享状态边界 | 不通过一键 STATIC 在多个 DLL 中复制“唯一”身份或注册状态。 |
| 历史 dev_log、原提交、原 FAIL/NOT_RUN 证据 | 删除当前生产实现不删除可复核历史。 |
| 用户工作区、ProjectBuilder 用户差异、项目/资产/布局备份 | 不是本阶段施工垃圾。 |

## 5. metadata 的逐职责处理

不要把整个 metadata 目录改名为 extensions 后保留原依赖。

| 内容 | 目标位置 | 注意 |
|---|---|---|
| 纯字段/配置描述、PortableValueCodec、配置值 | authoring 对应域或已有纯基础定义 | 不让 UI factory 通过 PUBLIC 传播至纯作者模型。 |
| ComponentEditorRegistration 的 UI create | workbench/scene | 当前 create 仍依赖 SceneEditing 和旧 InspectorInteraction，不是纯 schema；改为正式作者字段能力/明确运行能力，保留两种语义差别。 |
| ComponentEditorRegistry 真正不可变目录算法 | workbench 合理 provider | 名字有实际语义可保留，原 aggregate target 和旧函数签名不保留。 |
| Command/Session/View 可执行 factory | 各自 activities/workbench | 不能组合成 E0 的万能服务表。 |
| EditorPluginExports/LoadEditorPlugin 装载与安装 | application/extensions | 复用 engine/project loader，显式新出口版本。 |
| 内置生成注册函数 | 实际贡献所在 target | 查清 generator->consumer，不能按 consumers=[] 将有生成职责的目标当作死代码。 |

## 6. 每项删除必须完成的九步

1. **定位：** 确认当前 ref 中实际文件、blob、声明/定义、CMake provider 和直接/传递消费者。
2. **分类：** 标记替代、提取、正式保留或历史保留，写明理由。
3. **替代：** 完成对应正式能力，不新增空默认成功实现。
4. **接线：** 修改实际产品、内置贡献、测试、示例、SDK 调用与生成输入。
5. **回归：** 运行同义行为，原结果/身份/失败可观察性不丢。
6. **删除：** 删除原声明、定义、转发、source list、安装/export 和无引用生成物，不只注释/改后缀。
7. **闭包：** 检查真实 source/depfile/CMake File API/最终链接，确认没有静态 LINK_ONLY 暗藏旧库。
8. **安装：** 新前缀独立消费；开发前缀仅按既有安装清单删除已确认废弃文件。
9. **证据：** 记录替代提交、删除提交、验证命令和旧/新测试映射，清空相应暂留项。

删除不使用整目录无差别 rm，不对用户安装前缀使用无来源递归清除。自动化脚本只能删除经过人工审核、在受控安装清单中且路径规范化后位于指定前缀内的条目；仓库生产删除正常用明确 `git rm`。

## 7. CMake 与安装残留

- root 删除九个旧 `add_subdirectory`；五层内部不再引用旧 source roots/pinclude/sinclude。
- 删除旧 editor_context/editor_ui/editor_app/三大 Editor aggregate 的 source target、导出宏和 alias。不能改成无源 INTERFACE 继续发布旧名字。
- 正式新 provider 逻辑包名正确且有真实源码时可以保留，不能因它曾来自旧路径就全部更名。
- 清理旧生成器的 installed cmake scripts、类型扫描输入、生成入口函数和 target dependencies；唯一新生成器继续工作。
- SDK 不安装私有 headers、TestAccess、施工账本、历史日志或旧 API。
- 安装内置扩展的新版库、符号、配置路径与 runtime/editor版本声明；原 runtime 插件闭包保持独立。
- Windows 检查实际链接/装载清单，旧 build DLL 不能被 PATH 悄悄找到。无需本阶段新增 Linux 动态装载实测，但其平台分支不得硬编码 Windows。
- 同一公共 logical include 只由一个 provider 安装；共享 build include root 不代表可重复 install 整个目录。

## 8. 当前与历史测试的区别

原 `baseline_failures.cpp` 使用旧 Editor/PaneManager，是历史失败输入。P11/P12 建立相同语义的正式后继测试，通过后把旧测试源作为历史引用而非当前旧产品构建依赖。

必须保留三个契约：

- C01：拒绝坏布局时，原可见性、数量和 dock 不变化；
- C03：query 自替换时，旧闭包/代码到回调返回后才释放；
- C04：菜单连接失败使 create 失败，不发布成功应用。

不要用 WILL_FAIL 反转运行期缺陷断言。新契约通过与历史原实现失败可以同时存在，不改写过去报告。

同理，旧协议测试的 name 可保留或有映射地改名；测试数字不决定质量。不得在新 `tests/legacy` 中链接一个完整旧 Editor 来维持“原测试未删除”。

## 9. 最终清零验收范围

必须同时检查：

| 维度 | 合格结果 |
|---|---|
| Git 一级目录 | 五层 + tests；无旧九根。 |
| 活动源定义 | 无旧 owner/协议及同义改名替身；无 .old/.bak/#if0 缓存生产实现。 |
| 头与 include | 无原根转发壳、私有反向 include、旧安装补依赖。 |
| CMake/生成 | 无旧构建入口、alias 或长期 EXCLUDE_FROM_ALL 旧系统；生成器实际可重建。 |
| 产品运行 | 安装的 lux_editor 走唯一新装配；没有 old/new/fallback 启动模式。 |
| 动态装载 | 新扩展版本明确，旧 Editor 出口不再参与生产；共享身份不重复。 |
| SDK | 真实独立消费者运行，旧包不存在或明确在不兼容负例中被拒绝。 |
| 用户功能 | 原适用用例都有实际 UI/命令入口与结果，非仅模型函数能调用。 |
| 保全 | 用户变更、原 codec 数据、历史收据和未测范围保留。 |

任何一行不成立，就不能因为目录数已经达到六个而报告 P12 完成。


---

<a id="unit-5"></a>

<!-- 合并来源：04_ACCEPTANCE_AND_HANDOFF.md -->

# 验收、阶段交接与后续质量约束

## 1. 验收基线与权威

以每一阶段最后一个实际测试的实现 SHA 为资格对象。该 SHA 之后单独写证据提交，避免收据自引用。

P11 输入以当前结构交付及核验结果为准；P12 输入必须是已复审的 P11。原 `dev_log/P10Q`、`P10Q-structure` 和更早快照保持原字节；新的 scope 决定另记。

原工作区和独立检出分别记录，不把“资格检出干净”说成“用户工作区无修改”。ProjectBuilder 的原用户补丁迁移时应保存新旧路径和 hash，未经用户明确合并不得纳入阶段实现。

## 2. 最终运行顺序

每阶段内部按影响范围测试，不每次迁文件都跑完整性能矩阵。阶段末统一：

1. `git diff --check`、提交集合和实际未提交状态；
2. 正式阶段 gate（P11 或 P12）及 `LUX_EDITOR_LAYERING_MODE=STRICT`；
3. 最终实现的完整 Windows 构建，二次无新增工作；
4. 完整适用 CTest、独立 CPU 配置、PLAYER；
5. 全新安装前缀，原适用消费者、新增命令/扩展/产品消费者和公共头解析；
6. 原与新增实际依赖负例、concept 和 operation 特殊成员负例；
7. 真实文件 IO、实际插件、正式新桌面/双视口/GPU验证层/原生输入；
8. 必要生成输出删除后重建与正确 provider 检查；
9. source/target/install/runtime 残留检查；
10. 冻结证据的迁移、缺失/篡改拒绝和恢复正例。

CPU 配置不被 GPU/工具链运行测试强迫依赖环境；关闭测试能力不能关闭本应交付的生产功能。所有日志记录真实配置、设备、包前缀、工具和退出码。

## 3. 层次与依赖负例

以下列为最低主题，不为每项创建一个测试程序；复用既有架构检查器和失败原因验证。

| 编号 | 非法边/行为 | 正例恢复 |
|---|---|---|
| N11-01 | commands 纯策略 include 旧 EditorContext 或 UI Menu 实现 | 使用纯描述/目标契约。 |
| N11-02 | activities/sessions include workbench/ViewHost | E4 组合发布与显示，E2只返回内容结果。 |
| N11-03 | 具体 SessionFactory 反向进入 application Extension 总表 | 工厂依赖自己的 E2 contract，App安装它。 |
| N11-04 | 新插件直接 include pinclude/sinclude/TestAccess | 只用安装公共契约编译并运行。 |
| N11-05 | authoring 模型因新工厂拉入 Process/Compiler | 工厂实现归 E2，纯源仍无执行依赖。 |
| N11-06 | 旧 ABI 被强制视为新接口调用 | 实际 header/version mismatch 拒绝。 |
| N12-01 | 应用新入口仍 LINK_ONLY 链接 editor_context/editor_ui | 完全移除原 provider，真实链接通过。 |
| N12-02 | Layer 借 target alias/prefix 黑洞掩盖反向依赖 | 跟踪真实 provider、PUBLIC/INTERFACE/生成头。 |
| N12-03 | Material 使用 viewport 时链接整套 Scene UI | 只依通用 viewport。 |
| N12-04 | TaskMonitor 通过 tasks_ui 引入 ImGui | E2 editor_tasks 独立消费者。 |
| N12-05 | engine/modules 或 PLAYER 反向 include/link Editor | 无 Editor 编译输入和运行依赖。 |
| N12-06 | 新前缀消费者从旧源根/开发SDK找到旧头 | 清洁搜索路径与新 install manifest通过。 |
| N12-07 | 以运行时 option 或 #if0 留旧可运行产品 | 旧 source/target/安装物实际删除。 |

只有指定规则拒绝且同夹具去除非法边后通过，才计合格。缺头、缺包、网络失败或构建器缺失不计负例成功。

## 4. 五条必须用真实系统组合验证的链

### A. 命令与注册

真实 Menu/Shortcut → 固定 invocation → query/execute snapshot → 对应真实 Session 或活动 → 结果。中途变化焦点、注册、身份与代码 owner。

### B. 内容工厂与保存

真实三模型 decode → owner Session/role安装 → 编辑 → Save/SaveAs/Export → 真实文件 → 关闭 View → 迟到结果结清。检查文件事实与 checkpoint 不混。

### C. Run/编译/退出

作者编辑、Run 单步、编译和输出同时在途，Exit 决定与保存完成交错。原 runtime 和 Process 被驱动到真实终态；单步晚读与代码 payload lifetime保留。

### D. 布局与恢复

真实 Root 中 dirty 双视图+额外窗口 → 布局加载/纯计划 → provider失败/坏dock → 无副作用；随后成功应用 → 偏好失败独立。恢复只从独立manifest打开内容。

### E. 产品与安装

只使用新安装前缀启动 `lux_editor`，通过正式菜单打开/编辑/保存/运行/关闭。新外部 Editor 扩展和独立 runtime 插件分别工作；不借测试专用旧 Editor。

## 5. 删除阶段如何保留测试

### 5.1 没改的核心算法

原模型、保存、运行、交互、P10Q 共享字节与容量测试尽可能保持源码/断言。迁移 include 路径不能破坏命名空间/ABI/序列化身份。

### 5.2 旧产品测试确实需要迁移

记录：原测试路径和 blob、旧接口、用户行为、原断言、正式新路径、新观察和测试名。

当旧行为是“调用旧 Editor 读取业务结果”，用新 EditorApplication/activities 的正式查询检查相同事实；不要因为旧类型删除而放弃验证，也不要通过一个永久旧 TestAccess 包装继续运行。

移植测试允许合理 API 更名和状态拆分，但 must-observe 不能减少。特别保留完整源编码、History/current/dirty/绑定、磁盘事实、视图状态、身份代际和代码销毁顺序。

### 5.3 历史证据

原 frozen FAIL 继续保留。当前后继实现通过是新增证据，不是把旧文件中的 FAIL 改成 PASS。若测试声明变化，说明为什么保持原契约含义，不能将 C04 从 startup 换成 exit。

## 6. 平台与性能的终态表达

| 项目 | 当前可接受表达 |
|---|---|
| Windows 功能/安装/GPU | 实际命令通过，限定设备/工具链和运行范围。 |
| Linux | NOT_RUN；完成源码/构建静态审查，不宣称可运行。 |
| 系统 IME | 未做候选/组合/提交实测则 NOT_RUN；Unicode注入不替代。 |
| ASan/UBSan | 只有真实匹配构建与运行才记录通过；无资格仍NOT_RUN。 |
| 原50k长测 | 原PARTIAL和66/100保持；本轮不补样本、不用于阻塞。 |
| 轻量性能回归 | 只对修改路径运行有界容量/复杂度/字节owner等短测试。 |

保留 P13 的后续平台资格定位，但不能以 P13 为名延期清除旧源码。

## 7. 证据组织

复用唯一账本：`.internal/editor-redesign/` 的 `closeout` 节点。建议字段可见 templates；不得创建第二套持续变化的 registry of receipts。

每阶段冻结：

```text
dev_log/P11/ 或 dev_log/P12/
  README.md
  receipt.json
  source-map.json
  behavior-map.json
  removal-plan.json
  dependency-map.json
  logs/
  failures/                  # 真实开发失败，不冒充最终通过
  protected/                 # 用户改动来源/补丁/验证，不写入生产代码
  archive-probes/
```

这是证据内容类别，可合并已有同义文件，不要求新建多个几乎为空的 JSON。

必须有实际 `implementation_sha`、命令 argv/目录/环境摘要、开始结束和退出码、相对日志路径/内容校验，以及实现/证据提交区别。

验证器读取冻结归档和固定 Git 对象，不依赖生产机器 E: 绝对路径；缺少真正必需日志、修改真实日志、实现SHA不匹配都拒绝。不能因目录移动就改写历史check脚本的原义。

## 8. P11 交付声明模板

```text
P11 已完成，停在 P11 待复审。
实现：<sha>；验收：<sha>。
正式命令/不可变注册/两阶段工厂/外部SDK已实际接通。
C03 当前契约：<结果及新旧调用路径>；历史FAIL保留。
本阶段删除：<具体协议/文件/target>。
P12最后消费者：<精确列表/原因/替代批次>，没有新增兼容桥。
Windows实际资格：<明细>；Linux/IME等按实际范围记录。
未修改main/用户补丁/原历史快照；未进入P12。
```

不能只写“接口完成、测试通过”，必须列出仍存旧 executable 的事实。每次交付附实际可供用户查看的新工作区路径与 HEAD；原工作区仍在旧 SHA 时明确说明，用户补丁是否已应用也单独报告。

## 9. P12 交付声明模板

```text
P12 已完成，唯一产品已切换，停在P12待复审。
实现：<sha>；验收：<sha>。
安装的lux_editor来自<正式bootstrap>；无old/new回落。
九个旧根已清空；旧Context/Editor/注册/保存桥及旧安装入口清零。
已保留的正式旧名target及理由：<清单>。
C01/C03/C04当前等价契约通过；历史原FAIL仍保留。
完整功能/真实IO/双视口/输入/退出/插件/SDK/PLAYER：<结果>。
Linux/IME/旧性能未测或PARTIAL没有被改写。
用户修改、main、数据兼容与历史归档保全。
```

存在待删旧 owner 或实际仍加载旧 DLL 时，不能使用上述“清零”措辞。最终六目录指提交中的 tracked 源码结构，不授权删除用户原工作区的未跟踪文件或缓存。

## 10. P11/P12 之后继续遵守

- 不新增顶层 adapters/core/services/tools 混合分类，五层职责固定。
- 不让窗口成为作者内容、保存、编译、Run 或代码装载 owner。
- 不为一个单一具体服务自动创建 I/Port/Adapter 三层。
- 动态贡献只擦除一次；编译期复用用 concept约束小算法，不模板化整个应用。
- 通知用 LuxObject，返回值用准确Result，有限工作走Process，资源寿命走原Runtime。
- 构造建立必需依赖；回调/异步/代际/外部IO边界保留复查。
- 保留静态库的实际独立闭包，不用全符号导出/whole-archive隐藏缺依赖。
- 废弃实现随最后消费者一起删除；不会再生成“临时但没有期限”的第二体系。

