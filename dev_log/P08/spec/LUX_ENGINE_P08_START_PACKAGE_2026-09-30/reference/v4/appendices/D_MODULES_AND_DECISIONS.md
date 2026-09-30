# 模块依赖与 V3 的执行化决议

下表是最终逻辑 target DAG。不是要求每个逻辑边界一个 DLL，也不是允许创建空 INTERFACE target 假装实现。相邻实现可经明确静态组件组织，但边界检查仍要证明禁止 include 和反向调用；不能把整个 Editor 静态合成一个巨库然后说内部已经解耦。

实际 CMake target/alias 使用现有 add_component/安装生成习惯，在 P00 的 architecture.json 填入对应关系。表内逻辑名是权威设计标识；安装 alias 不得转发至旧 editor_context/editor_scene 以伪装完成。只列 Editor 内部依赖；真实 engine/modules/第三方依赖必须在实现时枚举、按公开签名或实现需求导出，不得给全部目标统一链接 EngineContext。

“首次阶段”指实现该职责；P11 的 SessionLoadJob 具体注册适配先在外部消费者/测试切片中使用，P12 由同一个 integration target 正式链接，不能复制成第二份实现，也不能为了预定义整目标依赖而创建空 workflows。CMake 只加入当前已经实现、依赖已满足的源码集合。

| 逻辑 target | 目录 | 首次阶段 | 允许的直接 Editor 依赖 |
| --- | --- | --- | --- |
| editor_contracts | editor/contracts | P01 | 无 |
| edit_history | editor/history | P01 | editor_contracts |
| edit_sessions | editor/sessions | P01 | editor_contracts, edit_history |
| scene_model | editor/tools/scene/model | P02 | edit_sessions, edit_history |
| material_model | editor/tools/material/model | P03 | edit_sessions, edit_history |
| flowforge_model | editor/tools/flowforge/model | P04 | edit_sessions, edit_history |
| editor_persistence | editor/persistence | P05 | editor_contracts, edit_sessions |
| project_io | editor/adapters/project_io | P05 | editor_persistence |
| scene_persistence | editor/tools/scene/persistence | P05 | scene_model, editor_persistence |
| material_persistence | editor/tools/material/persistence | P05 | material_model, editor_persistence |
| flowforge_persistence | editor/tools/flowforge/persistence | P05 | flowforge_model, editor_persistence |
| scene_execution_api | editor/tools/scene/execution/api | P06 | edit_sessions |
| scene_execution | editor/tools/scene/execution | P06 | scene_execution_api, scene_model |
| scene_projection | editor/tools/scene/projection | P07 | scene_model, scene_execution_api |
| material_preview | editor/tools/material/preview | P07 | material_model, editor_persistence |
| flowforge_compilation | editor/tools/flowforge/compilation | P07 | flowforge_model, editor_persistence |
| scene_interaction | editor/tools/scene/interaction | P08 | scene_model, scene_execution_api |
| material_interaction | editor/tools/material/interaction | P08 | material_model |
| flowforge_interaction | editor/tools/flowforge/interaction | P08 | flowforge_model |
| view_api | editor/views/api | P08 | editor_contracts |
| editor_widgets | editor/widgets | P08 | view_api |
| layout_model | editor/workspace/layout | P09 | view_api |
| recovery_model | editor/workspace/recovery | P09 | view_api, edit_sessions |
| workspace_store | editor/workspace/storage | P09 | layout_model, recovery_model, editor_persistence |
| desktop_shell | editor/desktop | P10 | view_api, layout_model |
| scene_ui | editor/tools/scene/ui | P10 | view_api, editor_widgets, scene_model, scene_interaction, scene_projection, scene_execution_api |
| material_ui | editor/tools/material/ui | P10 | view_api, editor_widgets, material_model, material_interaction, material_preview |
| flowforge_ui | editor/tools/flowforge/ui | P10 | view_api, editor_widgets, flowforge_model, flowforge_interaction, flowforge_compilation |
| tasks_ui | editor/tasks/ui | P10 | view_api, editor_widgets |
| project_ui | editor/project/ui | P10 | view_api, editor_widgets, project_io |
| editor_commands | editor/commands | P11 | editor_contracts, edit_sessions, view_api |
| extension_api | editor/extensions/api | P11 | edit_sessions, editor_persistence, view_api, editor_commands |
| editor_extensions | editor/extensions/host | P11 | extension_api |
| editor_workflows | editor/workflows | P12 | edit_sessions, editor_persistence, view_api, layout_model, recovery_model, workspace_store, editor_commands, extension_api |
| scene_tool | editor/tools/scene/integration | P12 | scene_model, scene_persistence, scene_execution, scene_projection, scene_interaction, scene_ui, editor_workflows, extension_api |
| material_tool | editor/tools/material/integration | P12 | material_model, material_persistence, material_preview, material_interaction, material_ui, editor_workflows, extension_api |
| flowforge_tool | editor/tools/flowforge/integration | P12 | flowforge_model, flowforge_persistence, flowforge_compilation, flowforge_interaction, flowforge_ui, editor_workflows, extension_api |
| editor_bootstrap | editor/application | P12 | desktop_shell, editor_workflows, editor_extensions, project_io, scene_tool, material_tool, flowforge_tool, tasks_ui, project_ui |
| lux_editor | editor/launcher | P12 | editor_bootstrap |


## 明确执行化的决定

**FlowForge 不能遗漏。** 固定基准存在完整 FlowForgeEditor 及同样的 Context、保存、关闭依赖。本包增加其 model/persistence/compilation/interaction/ui/integration，复用会话与保存机制，不继承 MaterialSession。M17 表示这项额外领域职责，不重编号 V3 的 M01–M16。

**任务与项目 UI 有归属。** tasks_ui/project_ui 是 UI 内部的具体边界，不放回 editor_widgets 通用控件；只借 TaskQueryPort/项目读接口，不得重新注入 Context。代码生成/组件控件属于 Scene UI 和其安装工具链。

**写入只有一个协调 owner。** WriteCoordinator 属于 M06，由应用拥有；普通保存、Save As、编译产物发布、WorkspaceStore 都借用它。同一路径不能因 API/命名域不同获得两条独立 lane。底层 IArtifactStore 实现可以有多个受限根，但路由层必须保证目标规范化与物理写入责任统一。它不持有会话，不成为通用 OperationManager。

**V3 的加载角色不能造成依赖环。** 编码/解码算法与 PreparedScene/Material/FlowData 留 M07；实现 extension_api 的动态 SessionLoadJob 和角色装配回调放工具 integration。否则 persistence→extension_api→persistence 会成环。

**视图身份明确而不猜测。** 基准 PaneId 是 StableNameId，不是代际句柄。ViewId 是运行 Host+slot+generation，ViewRestoreKey 是持久键，PaneId 是唯一低层树注册名；ViewTypeId 则复用 PaneTypeId。角色不同才分型，不再对同一事实重复包装。

**过渡兼容的边界和期限。** P01 为维护唯一纯历史实现允许 LegacyPersistenceState 私有桥；P08 原 rooted 构造只供旧产品临时使用。新模块不得依赖它们；P12 同时删除。其他临时桥须在账本中单项说明必要性、调用方、无状态复制与删除期限，不能默认创建第二套 adapter 框架。

**应用关闭无环依赖。** workflows 只借 SessionCloseDependencies 等窄回调；Scene 工具 integration 提供 Run 控制的实现，workflows 不链接 Scene 工具或 bootstrap。Bootstrap 调用所有组件，但不得从组件取回对全 Bootstrap 的引用。

**新产品何时成为唯一入口。** P02–P11 的新模块通过原生/桌面测试切片执行，不能安装另一个长期 NewEditor；原 launcher 暂供旧产品使用，不允许同一工程内容同时双写。P12 一次性完成用例接线、切换唯一 launcher、删除旧框架。P13 只验收和收尾，不是旧架构无限期避难阶段。

**领域的保留与继承。** SceneSession/MaterialSession/FlowSession 仅为 Store 的异构描述/关闭边界实现 IEditSession；具体编辑不泛化成 BaseDocument。SceneView/MaterialView/FlowView 继承真实 Pane；RunSession 不继承作者 Session。业务 owner 以组合维持私有不变量，不用多个 Manager 共同读写一份状态。

## 构建检查必须同时检查三种边

链接边（含静态导出闭包）、头文件/生成器编译边、运行时反向能力。单纯 target_link_libraries(PRIVATE) 不会自动移除头文件依赖，链接图无环也不能证明返回 Impl& 的 API 没有越界。P00 建可重复检查，P13 用真实构建数据库与安装消费者复核。
