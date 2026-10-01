# 逐路径、类型与 target 处置表

以下是**施工计划种子，不是已经人工审完的逐文件删除清单**。L0 从真实 Git tree 展开到每文件并核验消费者；路径不存在则记录“已不存在”，不为满足表格重建。较长路径规则优先于较短前缀；SPLIT/REVIEW_REQUIRED 绝不自动移动。

CMake/README 多源汇入同一个领域目录时必须合并职责，不能以最后一个文件覆盖前一个。source/cmake/include/安装support要共同核对。

## 1. 路径规则

| ID | 原路径 | 目标 | 层/批次 | 动作与限制 |
|---|---|---|---|---|
| F01 | `editor/editing/history/include/` | `editor/editing/include/` | E0 / L1 | MOVE：唯一正式历史公开头 |
| F02 | `editor/editing/history/src/` | `editor/editing/src/history/` | E0 / L1 | MOVE：原算法逐文件迁入 |
| F03 | `editor/editing/history/CMakeLists.txt` | `editor/editing/CMakeLists.txt` | E0 / L1 | MERGE：保留edit_history实际目标，不创建转发target |
| F04 | `editor/editing/sessions/include/` | `editor/editing/include/` | E0 / L1 | MOVE：Session/Stamp/Permits唯一公开定义 |
| F05 | `editor/editing/sessions/src/` | `editor/editing/src/sessions/` | E0 / L1 | MOVE：Store/State/checkpoint不换算法 |
| F06 | `editor/editing/sessions/test/` | `editor/editing/test/sessions/` | TEST / L1 | MOVE_EDIT：scope_compile及SDK路径同步 |
| F07 | `editor/editing/sessions/CMakeLists.txt` | `editor/editing/CMakeLists.txt` | E0 / L1 | MERGE：原session目标和共享状态保持 |
| F08 | `editor/contracts/include/lux/engine/editor/views/ViewInfo.hpp` | `editor/editing/include/lux/engine/editor/views/ViewInfo.hpp` | E0+E3 / L1 | SPLIT：纯观察留E0；UI错误到workbench/desktop/include/.../views/ViewError.hpp |
| F09 | `editor/contracts/` | `editor/editing/` | REVIEW / L1 | REVIEW_REQUIRED：必须逐文件排除UI-only；不整目录覆盖 |
| F10 | `editor/editing/sinclude/lux/engine/editor/editing/InteractionDelivery.hpp` | `editor/workbench/sinclude/lux/engine/editor/workbench/InteractionDelivery.hpp` | E3 / L3+L5 | MOVE_EDIT：namespace改workbench::detail；双工具PRIVATE消费 |
| F11 | `editor/editing/include/lux/engine/editor/EditorError.hpp` | `editor/editing/include/lux/engine/editor/EditorError.hpp` | E0 / L1 | KEEP_RECLASSIFY：纯错误定义真实provider，不要求consumer链接旧editor_editing |
| F12 | `editor/editing/scene/` | 按符号裁定／暂留原位 | TEMPORARY / L0 | REVIEW_REQUIRED：旧Registry适配按消费者到P12；不可认证E0 |
| F13 | `editor/editing/include/` | 按符号裁定／暂留原位 | REVIEW / L0 | REVIEW_REQUIRED：除已列纯头外识别旧AssetSave/Close/EditHistoryTarget |
| F14 | `editor/editing/src/` | 按符号裁定／暂留原位 | REVIEW / L0 | REVIEW_REQUIRED：原EditHistoryTarget及历史桥不混进E0 |
| F15 | `editor/editing/sinclude/` | 按符号裁定／暂留原位 | REVIEW / L0 | REVIEW_REQUIRED：SignalDelivery/TaskResult按实际consumer归属，不默认新common |
| F16 | `editor/tools/scene/model/` | `editor/authoring/scene/` | E1 / L1 | MOVE_EDIT：PersistenceAccess按作者状态语义保留 |
| F17 | `editor/tools/material/model/` | `editor/authoring/material/` | E1 / L1 | MOVE_EDIT：原MaterialSource/图/快照不重写 |
| F18 | `editor/tools/flowforge/model/` | `editor/authoring/flow/` | E1 / L1 | MOVE_EDIT：物理flow；逻辑flowforge/schema保留 |
| F19 | `editor/project/ui/` | `editor/workbench/project/` | E3 / L3 | MOVE_EDIT：纯UI；CatalogModel不跟随 |
| F20 | `editor/project/src/ProjectBuilder.cpp` | `editor/authoring/project/src/ProjectBuilder.cpp` | E1 / L1 | PROTECTED_MOVE：D01纯Builder；用户未提交bytes单独保护 |
| F21 | `editor/project/` | `editor/authoring/project/` | E1 / L1 | MOVE_EDIT：仅真实纯Manifest/Builder/Catalog；混合副作用另分 |
| F22 | `editor/workspace/layout/` | `editor/authoring/layout/` | E1 / L1 | MOVE_EDIT：纯Layout/Recovery/Preferences/plan |
| F23 | `editor/persistence/` | `editor/activities/persistence/` | E2 / L2 | MOVE_EDIT：core/execution继续实际两个边界 |
| F24 | `editor/storage/src/FileArtifactStore.cpp` | `editor/activities/persistence/src/FileArtifactStore.cpp` | E2 / L2 | MOVE_EDIT：真实文件后端不叫compatibility |
| F25 | `editor/storage/src/FilePublication.cpp` | `editor/activities/persistence/src/FilePublication.cpp` | E2 / L2 | MOVE_EDIT：唯一平台文件发布算法 |
| F26 | `editor/storage/include/lux/engine/editor/storage/FileArtifactStore.hpp` | `editor/activities/persistence/include/lux/engine/editor/storage/FileArtifactStore.hpp` | E2 / L2 | MOVE_EDIT：逻辑include可保留 |
| F27 | `editor/storage/include/lux/engine/editor/storage/FilePublication.hpp` | `editor/activities/persistence/include/lux/engine/editor/storage/FilePublication.hpp` | E2 / L2 | MOVE_EDIT：实际后端契约唯一 |
| F28 | `editor/storage/` | `editor/activities/project/` | E2 / L2 | REVIEW_REQUIRED：逐文件；ProjectStorage/Creation/Publication，CMake拆两主题 |
| F29 | `editor/assets/` | `editor/activities/project/` | REVIEW / L2 | REVIEW_REQUIRED：源读取导入归E2；旧TAssetSave不可整包迁入 |
| F30 | `editor/tools/scene/persistence/` | `editor/activities/scene/` | E2 / L2 | MOVE_EDIT：实际角色target保持，不并入通用保存core |
| F31 | `editor/tools/material/persistence/` | `editor/activities/material/` | E2 / L2 | MOVE_EDIT：具体源格式及采用 |
| F32 | `editor/tools/flowforge/persistence/` | `editor/activities/flow/` | E2 / L2 | MOVE_EDIT：固定源编码/采用 |
| F33 | `editor/tools/scene/execution/api/include/` | `editor/activities/scene/include/` | E2 / L2 | MOVE_EDIT：纯RunInspect/API实际闭包保持 |
| F34 | `editor/tools/scene/execution/` | `editor/activities/scene/` | E2 / L2 | MOVE_EDIT：RunStore与Controller原实现 |
| F35 | `editor/tools/scene/projection/test/` | `editor/tests/integration/projection/` | TEST / L2 | MOVE_EDIT：组合viewport为测试依赖；生产不倒灌 |
| F36 | `editor/tools/scene/projection/` | `editor/activities/scene/` | E2 / L2 | MOVE_EDIT：SceneProjection/Hub/ResourceStatus；多个CMake要合并审查 |
| F37 | `editor/tools/material/preview/test/` | `editor/tests/integration/material_activity/` | TEST / L2 | MOVE_EDIT：编译/预览真实fixture整合后配置 |
| F38 | `editor/tools/material/preview/` | `editor/activities/material/` | E2 / L2 | MOVE_EDIT：compile/preview保留不同真实闭包 |
| F39 | `editor/tools/flowforge/compilation/` | `editor/activities/flow/` | E2 / L2 | MOVE_EDIT：固定编译对象重试 |
| F40 | `editor/workspace/storage/` | `editor/activities/workspace/` | E2 / L2 | MOVE_EDIT：只文件/迁移，不调用ViewHost |
| F41 | `editor/tasks/ui/include/lux/engine/editor/tasks/TaskMonitor.hpp` | `editor/activities/tasks/include/lux/engine/editor/tasks/TaskMonitor.hpp` | E2 / L2 | MOVE_EDIT：新增真实CPU target editor_tasks |
| F42 | `editor/tasks/ui/src/TaskMonitor.cpp` | `editor/activities/tasks/src/TaskMonitor.cpp` | E2 / L2 | MOVE_EDIT：不再编进tasks_ui |
| F43 | `editor/tasks/ui/test/monitor.cpp` | `editor/activities/tasks/test/monitor.cpp` | TEST / L2 | SPLIT：纯observer测试留E2；含View的段移integration |
| F44 | `editor/tasks/ui/` | `editor/workbench/tasks/` | E3 / L3 | MOVE_EDIT：剩余TaskView；包仍是真UI而非旧alias |
| F45 | `editor/views/api/include/` | `editor/workbench/desktop/include/` | E3 / L3 | MOVE_EDIT：View契约与Host同主题；view_api目标保留 |
| F46 | `editor/views/api/` | `editor/workbench/desktop/` | E3 / L3 | MOVE_EDIT：CMake和README需合并非覆盖 |
| F47 | `editor/views/viewport/` | `editor/workbench/viewport/` | E3 / L3 | MOVE_EDIT：ViewportElement等不带Scene作者 |
| F48 | `editor/widgets/` | `editor/workbench/widgets/` | E3 / L3 | MOVE_EDIT：原node-editor backend保留 |
| F49 | `editor/desktop/` | `editor/workbench/desktop/` | E3 / L3 | MOVE_EDIT：Host/Shell唯一实现 |
| F50 | `editor/tools/scene/interaction/` | `editor/workbench/scene/interaction/` | E3-CPU / L3 | MOVE_EDIT：CPU target不能加GUI |
| F51 | `editor/tools/material/interaction/` | `editor/workbench/material/interaction/` | E3-CPU / L3 | MOVE_EDIT：CPU target保持 |
| F52 | `editor/tools/flowforge/interaction/` | `editor/workbench/flow/interaction/` | E3-CPU / L3 | MOVE_EDIT：完整ContentStamp选择来源保持 |
| F53 | `editor/tools/scene/ui/test/` | `editor/tests/integration/scene_views/` | TEST / L3 | MOVE_EDIT：跨工具/桌面/SDK fixture最后配置 |
| F54 | `editor/tools/scene/ui/` | `editor/workbench/scene/` | E3 / L3 | MOVE_EDIT：代码生成器/support同步 |
| F55 | `editor/tools/material/ui/` | `editor/workbench/material/` | E3 / L3 | MOVE_EDIT：草稿/queue原语义保持 |
| F56 | `editor/tools/flowforge/ui/` | `editor/workbench/flow/` | E3 / L3 | MOVE_EDIT：逻辑namespace/include不机械改名 |
| F57 | `editor/tools/settings/` | `editor/workbench/settings/` | REVIEW / L4 | REVIEW_REQUIRED：只迁无Context的真实UI；旧接线暂留 |
| F58 | `editor/metadata/` | 按符号裁定／暂留原位 | REVIEW / L4 | REVIEW_REQUIRED：纯值/工厂/注册/装载四分，旧P11登记暂留 |
| F59 | `editor/plugins/` | 按符号裁定／暂留原位 | REVIEW / L4 | REVIEW_REQUIRED：贡献归相应层，安装归application/extensions |
| F60 | `editor/app/` | 按符号裁定／暂留原位 | TEMPORARY / L4 | REVIEW_REQUIRED：已可分离装配归application；旧EditorImpl到P12 |
| F61 | `editor/launcher/` | 按符号裁定／暂留原位 | TEMPORARY / L4 | REVIEW_REQUIRED：真实新launch可迁；旧产品流程到P12 |
| F62 | `editor/context/` | 按符号裁定／暂留原位 | TEMPORARY / L4 | REVIEW_REQUIRED：不改名成新的Context |
| F63 | `editor/ui/` | 按符号裁定／暂留原位 | TEMPORARY / L4 | REVIEW_REQUIRED：旧产品UI，未引用原体删除；不搬永久legacy |
| F64 | `editor/transition/` | 按符号裁定／暂留原位 | TEMPORARY / L4 | REVIEW_REQUIRED：按消费者删除，禁止新增 |
| F65 | `editor/tools/` | 按符号裁定／暂留原位 | TEMPORARY / L4 | REVIEW_REQUIRED：剩余三大旧Editor/旧项目UI，只旧consumer |
| F66 | `editor/tests/` | `editor/tests/` | TEST / L6 | KEEP_EDIT：归属/路径/negative fixtures更新；历史日志不改 |
| F67 | `editor/CMakeLists.txt` | `editor/CMakeLists.txt` | BUILD / L4 | REWRITE：新五层配置+显式暂留旧岛 |
| F68 | `editor/README.md` | `editor/README.md` | DOC / L4 | REWRITE：现状/目标/旧产品区分 |
| F69 | `editor/editing/CMakeLists.txt` | `editor/editing/CMakeLists.txt` | BUILD / L1 | REWRITE：E0实际目标与暂留旧目标明确区分 |
| F70 | `editor/editing/README.md` | `editor/editing/README.md` | DOC / L1 | REWRITE：新语义，旧业务不是正式内核 |
| F71 | `editor/` | 按符号裁定／暂留原位 | REVIEW / L0 | REVIEW_REQUIRED：所有未匹配tracked文件必须人工裁定，不忽略 |

## 2. 类型／成员动作

| 类别 | 动作 | 必须删除或维持的内容 |
|---|---|---|
| EditHistory/SessionStore/SessionState | KEEP_MOVE | 算法与 authority 不换；不新增 façade、current 或 dirty |
| IEditSession | KEEP_DYNAMIC | 保留异构描述/关闭；不为了concept把Store模板化 |
| CodeLease/Permits/ContentStamp | KEEP | 数据与code析构顺序、域/代际不可删 |
| ViewInfo | SPLIT_HEADER | 纯观察仍唯一定义；UI错误出E0；不另造同步目录 |
| ProjectBuilder/ProjectBuildConfig | KEEP_AUTHORING | D01纠正旧表；纯配置验证不改成执行服务 |
| ProjectCatalogModel/Snapshot | KEEP_AUTHORING | 一份Data/数组/索引/revision，Storage只组合 |
| ProjectStorage | MOVE_ACTIVITIES | 文件/目录采用责任；解除仅为纯头而链接旧editing |
| TaskMonitor | SPLIT_TARGET | E2应用观察服务；从tasks_ui source列表彻底删除 |
| TaskView | MOVE_WORKBENCH | 借用Monitor；不直接抢observer |
| InputDelivery stage/helper | MOVE_CONSTRAIN | 工作台私有共享；旧editing路径删除 |
| CanvasRequest/NodePropertiesDraft | KEEP | payload+based_on+phase同一输入，不执行时补current |
| ISaveSource/IEncodeJob/IPreparedRebind | KEEP_DYNAMIC | 一个实际开放边界；不嵌套多层新wrapper |
| MaterialCompileOperation/FlowCompileOperation | KEEP_OWNER | 禁复制/按值移动；结果仍可共享 |
| Publish*Operation旧静态壳 | MUST_STAY_DELETED | 已在P10Q退出，不重建；自由函数使用唯一发布准入 |
| 三个公共SessionAccess别名头 | MUST_STAY_DELETED | 不重建；真正detail访问实现不误删 |
| DetachedView/Mount准备 | KEEP_MOVE_ONLY | 移动赋值/销毁均保持code晚于节点 |
| Runtime/RunStore/PreviewStore | KEEP_AUTHORITY | 不因分层重复记录实际资源/结果 |
| 旧Context/Editor/注册体 | RETAIN_LIMITED | 精确consumer直到P11/P12；不是新的authoring/application |

## 3. target 处置计划

下表中的“建议新增”是本施工决策；没有标为建议的新名字不得被当作已存在target。实际 target 名从 L0 解析，不根据路径猜。

| 现有边界 | 目标层 | 默认二进制决定 | 改动 |
|---|---|---|---|
| editor_contracts / edit_history / edit_sessions | E0 | 保留当前已资格的共享状态边界 | 合并物理include/src；多target仍可同目录 |
| scene_model / material_model / flowforge_model | E1 | STATIC | 源路径迁入authoring，不引入toolchain/UI |
| editor_project | E1 | 原STATIC保持 | 纯Manifest/Model/Builder，不加文件执行 |
| 原纯layout target | E1 | 原形式保持 | 只纯值/计划，ViewInfo provider改后续实际定义 |
| editor_persistence | E2 | STATIC | 不include具体Session/后端 |
| editor_persistence_execution | E2 | STATIC | TaskScope接线，与core主题相同、closure不同 |
| 原FileArtifactStore/FilePublication targets | E2 | 原STATIC保持 | 迁入persistence主题，安装不借旧storage头 |
| 原三类persistence targets | E2相应域 | STATIC | 实际格式/采用角色，不形成新port层 |
| scene_execution_api / scene_execution | E2 | 按现有格式 | CPU inspect边界保持，render_client已声明依赖不能丢 |
| scene_projection | E2 | STATIC | viewport仅测试依赖，不进生产目标 |
| material编译/预览、flowforge_compilation | E2 | 按实际CPU/toolchain/GPU边界保留 | 不为同域对称合成巨型服务 |
| editor_tasks（建议新增真实target） | E2 | STATIC | 只编TaskMonitor，证明无GUI安装消费 |
| tasks_ui | E3 | STATIC | 只编TaskView，消费editor_tasks |
| view_api / view_host / desktop_shell / editor_viewport | E3 | 沿现有真实边界 | 公共契约/host/图形独立，不一库打包 |
| scene/material/flow interaction | E3-CPU | STATIC | 路径在workbench不等于依赖ImGui |
| scene_ui/material_ui/flow_ui/project_ui/widgets | E3 | STATIC | 同域公开API＋通用设施，不互相带入工具UI |
| 新组合入口/launcher | E4 | exe或真实内部STATIC | 不安装第二测试产品，不构造万能Context |
| 旧metadata/context/editor_ui/旧三大Editor | 临时旧产品 | 本轮保留已必要形式 | 精确登记，不换名认证新层；零消费者即删除 |

target 与安装包保持原名时只需 source/provider 改变，不算“兼容壳”。反之，留下一个旧 target 只 `INTERFACE_LINK_LIBRARIES` 转发到新 target 来保持旧名，就是本轮禁止的空壳。

## 4. 同时必须更新的非 Editor 路径

- `cmake/EditorTests.cmake`：读取layering规则/测试能力/阶段顺序的实际路径。
- `cmake/EditorArchitectureChecks.cmake` 与 `editor/tests/architecture`：源、头、target角色、实际依赖负例和历史SHA规则。
- `cmake/installed-consumers/**`：真实代码源路径、find_package、模式、运行库搜索与测试名映射。
- 代码生成器、meta/IR输入、support install、导出配置；相对路径以新目录为准。
- 根 `AGENTS.md` 和正式架构说明：新层/旧产品区分，禁止把参考演示代码复制进生产。
- 外部仓库只有在真实消费闭包需要变更时才改；不以本轮目录迁移为由修改所有engine/modules接口。

`dev_log/P00..P10Q`、原规格包、原始before负例和用户数据文件不批量改路径。当前计划/测试使用新路径，历史验证使用旧实现SHA。
