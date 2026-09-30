# P10 — 独立视图、双视口与桌面宿主完整接线

**实施包 V4 · 2026-09-28 · 调查基准 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`**  
**前置门槛：** P02, P03, P04, P06, P07, P08, P09。序贯交付时必须同时满足所有较早阶段的验收，不能因本阶段直接依赖较少而跳过中间文档。  
**责任域：** M11 / M16 的桌面部分；Flow、任务与项目 UI。**关联问题：** A02、A04、A06、T03。**V3 回归：** Q02, Q03, Q04, Q21, Q26, Q29, Q34, Q50, Q51。

> 本文是实施指令，不是已完成报告。旧符号依据固定基准；新路径和 API 是规定的目标，不冒充仓库已有内容。开始前读取实际 HEAD 与前阶段交接记录。若旧符号已由前阶段迁走，沿迁移记录定位新位置，不重新创建旧文件。本文可单独交给实施 LLM；完整包中的总契约、类型索引和删除账本用于交叉校验。

## A. 执行边界与交接前提

P00 首次执行按下文 C0 初始化账本，不要求不存在的前阶段收据；其余阶段先确认 `docs/editor-redesign/receipts/` 的前阶段记录与当前代码祖先关系，读取 `architecture.json`、`migration-ledger.json` 和 `test-coverage.json`。没有相应证据时只完成核验和缺口报告，不把未验收阶段当成已经通过。存在用户未提交修改时保留，不 reset、不覆盖、不清理无关文件。

本阶段只实现下面列出的职责；不得顺手重写 Lua、渲染后端、执行器、资源格式或全局插件系统。允许复用算法，不允许新模块 include 旧 Context/Editor 大类。禁止新增 `NewEditor`、`V2Session`、`LegacyManager` 等永久旁路。过渡代码必须进入唯一 `editor/transition/` 白名单、有调用方、单向依赖和不晚于 P12 的删除期限。

新增声明必须与实现、真实调用方或行为测试一起交付。只声明接口、默认返回成功、永久返回 Unsupported、空 `update()`、仅用 forwarding wrapper 包住旧 owner，都不算完成。接口草图使用 C++20 与现有 `lux::cxx::expected`，不要求升级标准；需要分配的函数不得未经处理一律标 `noexcept`。

## B. 本阶段类型、文件和责任清单

路径按仓库根目录解释。表内归为“复用”的类型不再造同义 wrapper；表内多种小值允许放在同一语义头，禁止堆入全局 `Types.hpp`。公开头放 `include/`，仅实现需要的状态放 `src/` 私有头，不重新开放兄弟模块的 pinclude/sinclude。

| 类型 / 角色 | 目标文件 | 组合、继承与 owner | 必须实现的 API / 不变量 |
| --- | --- | --- | --- |
| `ViewHost / DesktopShell` | `editor/desktop/include/lux/engine/editor/desktop/{ViewHost,DesktopShell}.hpp` | ViewHost : views::IViewHost；Shell 组合平台/Root/呈现 | Host 独占顶层 Pane；Shell 不持作者模型，不解析具体工具绑定。 |
| `SceneView / SceneViewState / SceneViewBinding` | `editor/tools/scene/ui/include/lux/engine/editor/scene/SceneView.hpp` | final : ui::Pane；组合视图状态/绑定/呈现 | Unbound/EditedSceneBinding/RunningSceneBinding 显式 variant；无 history/save/run owner。 |
| `OutlinerView / InspectorView / ResourceView / SceneCreationView` | `editor/tools/scene/ui/include/lux/engine/editor/scene/{OutlinerView,InspectorView,ResourceView,SceneCreationView}.hpp` | 真实 Pane 或 Element UI 特化 | 调用 Scene/Run 窄访问；不成为 SceneSession::Impl 的 friend。 |
| `MaterialView / MaterialViewState / MaterialViewServices` | `editor/tools/material/ui/include/lux/engine/editor/material/MaterialView.hpp` | Pane + typed SessionKey + 图交互/预览 | 视图有零/一绑定；不自动新建或销毁 MaterialSession。 |
| `FlowView / FlowViewState / FlowViewServices` | `editor/tools/flowforge/ui/include/lux/engine/editor/flowforge/FlowView.hpp` | Pane + FlowSessionKey + FlowInteraction | 图 UI 不拥有源/history/compiler；支持已有变量/导出/函数交互。 |
| `makeSceneView / makeMaterialView / makeFlowView` | `editor/tools/{scene,material,flowforge}/ui/src/ViewFactory.cpp` | 普通工厂函数，避免无意义工厂继承树 | 输入受限 services/create info；返回 DetachedView，不接管/复用/聚焦。 |
| `TaskView / TaskQueryPort` | `editor/tasks/ui/include/lux/engine/editor/tasks/{TaskView,TaskQueryPort}.hpp` | 只读任务面板与受限请求 | 复用执行器真实任务状态；不是新的任务管理器。 |
| `ProjectView / AssetPickerElement / ProjectCatalogAccess / AssetOpenRequests` | `editor/project/ui/include/lux/engine/editor/project/{ProjectView,AssetPickerElement,ProjectCatalogAccess,AssetOpenRequests}.hpp` | 项目 UI 与只读查询/明确请求接口 | 不持 EditorContext；打开资产是请求，不返回任意 Pane 引用。 |
| `Editor widgets / Inspector 字段适配` | `editor/widgets/ 与 editor/tools/scene/ui/codegen/` | 通用控件仅 UI；领域 adapter 在 tool/ui | 模板/生成代码不传播 scene/runtime 到通用 widgets。 |

## C. 先宿主，再工具视图，最后完整双视图切片

ViewHost 实现 P08 的 IViewHost，依赖 view_api、底层 UI 与布局模型；不能 include SceneSession/MaterialSession/FlowSession/RunStore 实现。DesktopShell 复用 WindowInput/WindowOutput/Presentation/UiRenderSyncStage 的平台与呈现算法，不持 SaveService 或 Source。

工厂只创建完整离树对象；adopt 由 Host 负责，show 和 requestFocus 是独立动作。焦点因 modal 被拒绝不等于创建失败；可见性改变不触发资产读取。Host 查询返回 ViewInfo，不返回内部 vector/unique_ptr 容器。

### C1. SceneView 的成员白名单

允许：SceneViewBinding、SceneViewState、ViewportPresentation、受限 SessionAccess/RunInspectAccess、交互组 key、观察连接/版本、真正 UI 子控件。禁止：SceneSource/ScenePackage 的可写工作副本、EditHistory、PersistenceCheckpoint、SaveOperation、RunSession/SceneInstanceLease 运行责任、ProjectStorage/EngineContext 引用、候选内容读取任务。

UnboundSceneBinding 是合法完整状态，可显示未打开内容的空壳；不得用无效 SessionKey 假装已经绑定。EditedSceneBinding 携 SceneSessionKey+interaction group，RunningSceneBinding 携 RunId+相应交互 group。读/编辑路由通过 binding 的具体类型，不再调用 inspectedScene()/inspectedHistory() 根据一个隐含 run_scene 是否存在切换。

视图重绑定的准备需建立新观察/呈现关联和解除旧交互的计划，成功才交换。失败保留原绑定、相机和已有内容；不先 clear，再尝试绑定新对象。

### C2. 多视图的真实验证要求

两个 SceneView 绑定同一 Session：从任一个编辑/undo，另一个收敛到同一 StateId；相机/输出尺寸/hover 独立，选择组是否共享由配置明确；共享投影不意味着全局共享高亮写入目标。关闭其中一个只退休它的资源，另一视图和 Session/history 完整。

增加作者视图+运行视图并存：相同底层 Entity 数值不能误路由；运行相机导航不改作者相机对象。实际双视口 GPU/验证层测试是本阶段必测项，不能仅用两个 FakeViewInfo 代替；缺 GPU 时状态明确 BLOCKED，不把阶段称为已完成。

## D. 逐文件拆分旧通用 UI

| 原 editor/ui 的文件/职责 | 新归属 | 必须修改 |
| --- | --- | --- |
| `src/TaskPane.cpp` | tasks/ui 的 TaskView | 使用 TaskQueryPort / 真实任务通知，删除 Context 查询 |
| `src/WindowInput.cpp`、`WindowOutput.cpp` | desktop | 只做平台输入/输出，不顺手处理项目/内容 |
| `src/Presentation.cpp`、`UiRenderSyncStage.cpp` | desktop | 保留 UI 资源同步与安全点，与 ViewHost 生命周期接线 |
| `src/SceneConfigurationElement.cpp` | tools/scene/ui | 作者配置提交 SceneEditBatch，候选场景创建走明确请求 |
| `src/SpatialInteraction3D.cpp` | scene/interaction 的数学/状态 + scene/ui 的事件适配 | 屏幕事件转射线与作者/运行拾取分别绑定 |
| `src/SceneElement.cpp` | scene/ui + projection | UI 绘制/输入与派生资源 owner 分离 |
| `src/ComponentEditors.cpp` | scene/ui 的 schema/字段适配 | 不直接写出裸 ECS 指针，不访问 SceneEditor friend |
| `src/CodegenInput.cpp`、codegen/cmake 脚本 | scene/ui 的 host codegen 集成 | generated 输出使用新字段编辑接口；工具依赖不 PUBLIC 链接进运行目标 |
| `src/asset/AssetPickerElement.cpp` | project/ui（通用列表/输入留 widgets） | 查询源通过 ProjectCatalogAccess，选择只返回 asset 地址或请求 |

旧 `editor_ui` target 在 P12 删除之前仍服务旧产品；本阶段新代码不许 PUBLIC 链接它获取方便头。确实共享的纯 UI 算法提到 widgets 后由新旧两方使用，不复制第二套 renderer/input。

## E. Material、FlowForge 和辅助工具不漏功能

MaterialView 保留原图编辑、常量/槽配置、编译/发布按钮、预览导航；按钮发应用请求/调用受限编译入口，不拥有 compilation_。FlowView 保留变量、函数、导出、字面值、节点连接和链接重试反馈，不能为了新视图简单而只迁移一个画布。

TaskView、项目浏览/资产选择、SceneCreationView、资源失败重试、Inspector codegen、原项目/launcher 的用户入口列入功能矩阵。辅助工具不一定是编辑会话：任务面板无需 IEditSession；项目列表无需伪造 History；“不继承 Session”不等于可以漏掉它们。

增加两个明确边界 `tasks_ui` 和 `project_ui`，它们不是新的 editor_common。前者仅任务接口，后者仅项目/资产读入/请求接口。bootstrap 负责把具体服务绑定进去，不给它们一个全局 services getter。

## F. 完整新路径演练与旧路径期限

本阶段使用只供测试的 integration harness：真实 SessionStore、新模型、SaveService、RunStore、投影、ViewHost、三个工具视图。harness 可在 owner 中静态调用工厂，不需要 P11 动态插件注册；其产品功能实现都来自正式新模块，不能另写一套测试专用 Editor。harness 不安装成第二个产品，不新增用户可选 Old/New 开关。

旧唯一产品入口仍可运行到 P12，但新集成链必须无旧 Context/SceneEditor/MaterialEditor/FlowForgeEditor/PaneManager include。构造/关闭/失败/拖拽/文本输入/焦点/IME 的现有行为按测试矩阵迁移，不能只拍一张窗口截图证明生命周期正确。

## G. 本阶段删除与暂留边界

| 旧文件 / 符号 | 本阶段动作 | 最终去向 / 删除期限 | 不得误删 |
| --- | --- | --- | --- |
| 新 SceneView/MaterialView/FlowView 内的源、history、save、run、candidate owner | 禁止；逐成员白名单审查 | 模型/保存/运行/用例各 owner；P10 | 已验证算法、资产兼容读取与行为回归不删 |
| SceneContentElement/OutlinerPane/InspectorPane/ResourcePane 对 SceneEditor::Impl 的 friend 访问 | 新控件全部改窄 API；旧 friend 随旧类 P12 删除 | SceneSessionAccess / RunInspectAccess / interaction；P10 新路径；P12 全局 | 已验证算法、资产兼容读取与行为回归不删 |
| editor/ui 中平台/领域/通用控件混合实现 | 按 D 表提取唯一算法与新调用方；不把整库改名为 common | desktop / widgets / scene_ui / tasks_ui / project_ui；P10 新路径；旧总 target P12 | 已验证算法、资产兼容读取与行为回归不删 |
| 旧 rooted Pane 工厂/旧工具 UI 与 TestAccess | 暂留旧产品，已迁测试改为公共行为和受限测试 seam | P12 删除旧类与旧安装入口；P12 | 已验证算法、资产兼容读取与行为回归不删 |

删除操作必须同步覆盖：声明、定义、调用方、friend、注册生成输入、CMake source/target、导出与安装清单、示例、活动测试和文档。改扩展名、挪到 backup、注释整段、`#if 0`、`EXCLUDE_FROM_ALL`、残留 alias 都不算删除。用户资产/布局备份、版本化旧格式只读解码器和负向测试样本不属于垃圾代码，按独立保留清单保护。

## H. 验收场景与反作弊检查

| ID / 测试名 | 输入、故障点或操作 | 必须观察到的结果 |
| --- | --- | --- |
| X10-01 / `two_real_views_share_one_author_history` | 真实双 SceneView 编辑、undo、不同相机/尺寸 | 一个作者源/历史；两个输出都正确且资源无验证层错误 |
| X10-02 / `close_one_view_keeps_session` | 双视图关闭一个再创建另一个 | SessionId/HistoryId不变，关闭视口资源最终退休 |
| X10-03 / `author_and_run_views_do_not_alias` | 同源启动 Run，两个窗口分别编辑/导航 | 绑定路由正确，运行修改不入作者历史 |
| X10-04 / `material_flow_auxiliary_feature_matrix` | 执行原 Material/Flow/Task/Project/Inspector 全部核心操作 | 未靠遗漏入口或删除测试获得通过 |
| X10-05 / `factory_failure_no_ghost_and_rebind_keeps_old` | 创建/attach/rebind 各失败点注入 | Root 与 Session 集合一致，原绑定未丢失 |
| X10-06 / `input_capture_focus_and_ime_lifecycle` | 拖拽/文本组合输入/焦点切换/回调关闭 | 焦点捕获按阶段释放，IME实测与未测分开报告 |
| X10-07 / `new_integration_harness_has_no_old_dependencies` | 检查完整新 GUI harness 链接/include | 无旧 Context/PaneManager/工具大类；harness 不作为第二产品安装 |

同时执行本阶段相关 V3 场景与此前受影响的回归。只有“新增测试文件”不算执行；只有 fake 通过不替代该阶段要求的真实 IO、桌面或 GPU 结果。编译负向用例必须证明失败原因正是禁止依赖/非法操作，不能因缺少第三方头而误判通过。

## I. 交付格式、允许停点与禁止续行

提交按“契约与测试 → 实现和调用方迁移 → 旧代码删除与构建/安装更新”组织，但阶段末必须是可构建、可验证的整体。写入 `docs/editor-redesign/receipts/P10.json`，记录 `implementation_sha`、实际命令、测试结果、对应日志路径/校验和、新 owner、已删除项与有期限的暂留项。`implementation_sha` 指测试过的实现提交；收据在之后的证据提交写入，避免把收据自己的 SHA 写进自身形成循环。

输出中文报告：完成了哪些行为；哪些真实失败仍存在；删除了哪些旧符号/文件；剩余项属于哪个后续阶段；是否满足本阶段全部门槛。状态只允许 `PASS / BLOCKED / PARTIAL`；缺少必须环境或某项必测未运行时不能写 PASS，不能自动进入下一阶段。失败测试、崩溃日志和负结果保留，不改成 SKIP、降低断言或删除功能来凑通过。

提交前运行迁移审计脚本并人工核对 AST/引用清单。脚本只能检查文本、路径与清单，不能证明所有权正确，也不能代替编译和运行。最后检查 `git diff --check`、当前工作树、安装导出及阶段期限；不执行本文件之后的下一阶段。

**配套规则：** [总执行契约](../00_EXECUTION_CONTRACT.md)、[类型索引](../appendices/A_TYPE_INDEX.md)、[逐成员迁移账本](../appendices/B_RETIREMENT_LEDGER.md)、[测试映射](../appendices/C_TEST_MATRIX.md)、[固定源码证据](../appendices/E_SOURCE_REFERENCES.md)。

**交给下一阶段的最小材料：** 本阶段代码与证据提交、上述收据、更新后的迁移账本与测试覆盖表、已知限制。下一位 LLM 不需要猜测本轮聊天中没有写进仓库的决定。
