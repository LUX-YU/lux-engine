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
