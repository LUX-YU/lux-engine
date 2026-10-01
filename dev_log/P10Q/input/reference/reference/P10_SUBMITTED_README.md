# P10：正式桌面宿主、独立工具视图与双视口

状态与最终实现 SHA 以本目录 `receipt.json` 为准。阶段只到 P10；没有切换产品入口，没有实施 P11。

前置验收：`3363f83dcc448db9e79cea97c5176a546c05cf5a`。原 `dev_log/P00`～`P09-R1` 保持原样，历史检查按各自 implementation_sha 执行。用户 `ProjectBuilder.cpp` 差异不进入实现提交；其哈希在收据中固定。

## 实现及唯一所有权

- `ViewHost` 独占完整 DetachedView，Root 仅登记对象树。准备挂载容量与路由后接管，在提交后通知；通知回调内请求进入有界后批。关闭先结束交互，撤销焦点/捕获/路由，再销毁；临时 BUSY 保留原节点。64 次槽位复用验证代次与容量恢复，不宣称由此排除所有泄漏。
- `DesktopShell` 组合 Root、UI Presentation 和 Host，借用执行与渲染服务，不成为第二个 SceneRuntime 驱动者。集成装配只有一个 frame 驱动 owner。
- SceneView 使用明确 Unbound/Edited/Running 绑定。共享作者投影，相机、输出、高亮和拾取结果按本视口隔离。重绑定失败保留原绑定、相机和手势。
- MaterialView、FlowView 只持显示与交互状态。源、History、checkpoint、保存、编译、Run 仍由原 SessionStore/服务负责。关闭视图不取消已经准入的结果责任，不隐式关闭会话。
- 生成 Inspector 的显示缓冲经过原 Session gate，不暴露作者 Registry。Preview 不改编码；Commit 是一次领域批次。完整 SDK 组件覆盖嵌套/容器/optional/variant/Eigen 的生成，实际标量和集合修改验证精确编码、Undo/Redo。
- 任务与项目视图只借用明确查询/请求端口；任务取消后结果仍由应用 owner 接收。BUSY、权限和 IO 错误不清空已有目录。资源面板从真实 NOT_FOUND 状态重试到 READY。

## 功能入口与证据

| 范围 | 正式入口 | 实际验证 |
|---|---|---|
| Scene 双视图 | SceneView、ScenePresentationHub、ViewportPresentation | 一个 Session/history；编辑与另一视图 Undo，独立相机/大小；一窗关闭和重开 |
| 作者/运行 | EditedSceneBinding、RunningSceneBinding、RunInspectAccess | 同源运行独立导航与选择，不写作者 checkpoint |
| Scene 辅助工具 | OutlinerView、InspectorView、ResourceView | 创建 2D/3D/空对象、父关系、删除与 Undo；组件增删与字段交互；真实资源失败重试 |
| 配置与创建 | SceneConfigurationView、SceneCreationView | 页大小 1024→256，一次 Apply，Undo/Revert；正式依赖绑定与显式创建请求 |
| Material | MaterialView | 图连接、常量、参数/纹理槽、节点位置；真实编译、预览与发布，视图关闭后保存完成 |
| Flow | FlowView | 变量、签名、函数、导出、字面值和连接；真实编译及固定产物链接重试/发布 |
| 项目/任务 | ProjectView、AssetPickerElement、TaskView | 实际保存后读回 Material；定位身份检查；计时任务取消和关闭后可靠接收 |
| 桌面输入 | DesktopShell → Root → 正式 Elements | 实际 OS 鼠标拖动、窗口外捕获、Unicode TextEdit、焦点与 DIRECT 回调关闭 |

逐项 X10/Q 及日志引用见 receipt；测试不是按数量替代语义。原测试体迁移列表见 `evidence/test-migrations.json`，包含明确 API/命名空间替换。依赖负例同时执行修复后的正例，不能以缺第三方头冒充拒绝成功。

## GPU 与输入资格

新 harness 创建正式 SceneView、真实 RenderRuntime、真实 mesh 和材质，开启 Vulkan 验证层。双输出像素回读验证左/右高亮隔离、拾取和单视口退休。实际命令、环境设备、零 validation_errors 和资源回执见 `logs/desktop-detail.log`、新增 SDK 消费者日志及 `logs/gpu-device.log`。

旧 `GPU_UI` 与 `EDITOR_SCENE_PANE` 模式独立配置并保留原回归；它们不是新双视口证明。新 harness 不安装为第二产品。新桌面原生输入由 Windows OS SendInput 进入真实平台路径，**不等于系统 IME 候选/组合/提交实测**；该项 NOT_RUN。P13 的完整多环境产品资格仍独立，不能把 P10 演练扩大为完整产品验收。

## 删除和限期转换

平台输入/输出、UI 呈现、SceneElement、配置 UI、Inspector 生成器、资产选择、节点/树绘制与相机计算已迁出原算法体，新旧共享同一实现。文件清单见 FILES.md，逐成员责任见 migration-ledger.json 的 p10 段。

旧 editor_ui 只继续服务原 Editor、launcher、旧工具及其回归。私有 ProjectCatalogAdapter、LegacySceneConfiguration、LegacyComponentEditors/CodegenInput 和旧字段策略具有明确消费者，P12 删除。新模块不链接旧 Context/editor_ui/工具 Editor，不借其私有头。ViewInfo 仍只在 contracts 定义。所有新增模块为 STATIC，不新增 DLL。

SDK 清除七个已到期的旧公开头，新的生成器与支持文件由 scene_ui 包安装。旧包代码生成支持输出暂留原消费者至 P12。没有修改磁盘格式、Workspace 恢复来源、marker、WriteCoordinator、执行器、History 或 SessionStore 算法。

## 限制和失败记录

- C01 完整旧布局应用、C03 旧命令代码寿命、C04 旧 bootstrap 仍按原 FAIL，责任分别保持 P12/P11/P12；不由新模块测试改判。
- Run 只读检查通过 SceneView/Outliner/RunInspectAccess；新 InspectorView 面向作者组件。完整新产品的运行 Inspector 装配不据此宣称通过。
- 配置窗口固定作者绑定；不在视图内部偷偷加载新资产。用户项目恢复/marker 不被测试或布局 opaque 解析覆盖。
- 开发期间的构建、输入、依赖、安装生成失败原样保留在 development。整组件交换使用 schema 已有的无业务失败移动构造契约，不使用仅反射字段交换而丢失其他状态。
- 原冷构建失败及 P06 窄修复证据保持原样；本阶段独立干净源码的增量构建不冒充完全空构建树首次冷构建。
- Android 不在本轮验证矩阵；未修改 modules 公共头，不需要新增三前缀头同步。

## 验收组织

完整命令绑定最终实现 SHA，在独立 clean clone、显式 `LUX_EDITOR_MIGRATION_STAGE=P10` 上运行。顺序为全量 all/-j4/-k0、二次无工作、完整 CTest、模型/保存/运行/交互历次回归、SDK/PLAYER、依赖负例，再执行两个旧显式 GPU 模式。新双视口和输入在正式 native/SDK 测试中运行。

`check_receipt.py` 只读取归档相对路径和 Git 实现对象；生产目录不可用时仍可核验，缺失或篡改必需日志必须拒绝。实现和验收分别提交，推送后停在 P10 等待复审。
