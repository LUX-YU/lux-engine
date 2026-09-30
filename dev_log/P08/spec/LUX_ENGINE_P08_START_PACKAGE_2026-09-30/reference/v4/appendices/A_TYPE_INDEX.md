# 类型、文件与责任索引

这些是阶段要求中的类型组/角色，不代表全部必须新建成独立类。标记复用的基础类型保持单一实现；小互斥值与结果可以共用语义头。`{A,B}` 表示明确的两处目标，不是文件名。具体声明以对应阶段正文为准；V3 为逻辑参考，V4 的变更决议见附录 D。

| 阶段 | 类型/角色 | 目标文件 | 关系与 owner | 契约 |
| --- | --- | --- | --- | --- |
| P00 | architecture.json（数据，不是 C++ 类型） | docs/editor-redesign/architecture.json | 构建边界唯一清单 | 逐 target 记录逻辑名、真实 CMake target、源码/公开头目录、直接依赖、禁边与首次实施阶段。 |
| P00 | migration-ledger.json | docs/editor-redesign/migration-ledger.json | 迁移管理记录，不进入产品 | 每个旧符号/文件有事实证据、引用点、去向、责任阶段、删除期限、当前状态和实际实现 SHA。 |
| P00 | test-coverage.json / baseline.json | docs/editor-redesign/test-coverage.json | 证据记录 | 保留全部既有测试的语义与替代去向；未执行、失败、通过分别记录。 |
| P00 | 阶段收据模式 | docs/editor-redesign/receipts/schema.json | 交付契约 | 禁止用测试数量替代功能覆盖；implementation_sha 与收据提交分离。 |
| P00 | EditorArchitectureChecks.cmake | cmake/EditorArchitectureChecks.cmake | 独立构建检查 | 按实际 target 属性检查直接/传递边与生成工具边；不修改被测代码使错误消失。 |
| P00 | check_editor_boundaries.py | editor/tests/architecture/check_editor_boundaries.py | 测试工具 | 检查 include 路径和编译数据库；不能把正则结果宣传成完整 C++ 语义分析。 |
| P01 | CodeLease（复用既有代码保活机制） | editor/contracts/include/lux/engine/editor/contracts/CodeLease.hpp | 小值组合，不依赖 extensions host | 包装并命名已有生命周期责任；实际对象、回调及 deleter 析构结束后才释放代码。 |
| P01 | HistoryId / StateId / Revision / EditOperation / PreparedEdit / EditHistory | editor/history/include/lux/engine/editor/editing/{EditTypes,EditOperation,EditHistory}.hpp | 保留历史协议继承和原算法 | execute/undo/redo/clear/close 保留；移出保存票据、保存中与 clean 状态；不依赖 sessions。 |
| P01 | SessionId / SessionKey<T> / SessionKindId | editor/sessions/include/lux/engine/editor/sessions/SessionId.hpp | 身份值；Store 制造 typed key | 代际与类型检查；不是 AssetId、HistoryId；不把 RTTI hash 序列化。 |
| P01 | ContentStamp / ObservationVersion / BindingRevision | editor/sessions/include/lux/engine/editor/sessions/ContentStamp.hpp | 不同事实类别 | ContentStamp=SessionId+StateId；不另造重复 SourceEpoch；通知版本不能决定 dirty。 |
| P01 | SourceBinding / PersistenceCheckpoint / PersistedState | editor/sessions/include/lux/engine/editor/sessions/PersistenceCheckpoint.hpp | SessionState 值组合 | 来源未命名/已绑定互斥；基线与持久顺序只有一个 owner；不依赖 IArtifactStore。 |
| P01 | SessionState / EditGate / EditScope | editor/sessions/include/lux/engine/editor/sessions/SessionState.hpp | 具体会话组合；scope 局部且不可复制/移动 | 管理身份、绑定、编辑准入；不保存窗口、编译、布局状态。 |
| P01 | ClosePermit / BindingChangePermit | editor/sessions/include/lux/engine/editor/sessions/SessionPermits.hpp | move-only 跨阶段许可 | 未消费析构只解除限制；已消费不再解锁被复用槽位。 |
| P01 | IEditSession / SessionInfo | editor/sessions/include/lux/engine/editor/sessions/IEditSession.hpp | 窄接口继承的边界 | describe；private prepareClose 给 Store；无 save/play/compile/Pane。 |
| P01 | SessionStore / SessionAccess<T> / SessionReservation | editor/sessions/include/lux/engine/editor/sessions/SessionStore.hpp | Store 唯一拥有 unique_ptr<IEditSession> | reserve→prepare→publish；代际查找；close permit 消费删除；不提供任意 erase 给 UI。 |
| P01 | LegacyPersistenceState / LegacySaveTicket / LegacyPersistenceOutcome（限期桥） | editor/transition/LegacyPersistenceState.hpp | 旧产品私有适配；不进新 SDK | 替代旧历史保存 API，使旧工具仍可编译；只维护本工作副本唯一 checkpoint，P12 删除。 |
| P02 | SceneSource | editor/tools/scene/model/include/lux/engine/editor/scene/SceneSource.hpp | SceneSession 值/独占组合 | 唯一可写作者事实；保留 WorldObjectId、schema、分区、配置；不是运行 Registry。 |
| P02 | SceneSession | editor/tools/scene/model/include/lux/engine/editor/scene/SceneSession.hpp | final : sessions::IEditSession | 组合 SessionState、SceneSource、EditHistory；describe/read/apply/undo/redo/capture。 |
| P02 | SceneReadView / SceneSnapshot / SceneChangeSet | editor/tools/scene/model/include/lux/engine/editor/scene/SceneSnapshot.hpp | 短借用 / 冻结拥有值 / 有界变更值 | 短借用不能跨 tick；Snapshot 深度冻结插件数据并保活代码；变化裁剪有 ResetRequired。 |
| P02 | SceneEditBatch / SceneEditReceipt / SceneEditError | editor/tools/scene/model/include/lux/engine/editor/scene/SceneEdit.hpp | 领域意图和值 | 一次批量编辑一个历史提交；precondition 使用 ContentStamp 或 StateId。 |
| P02 | SceneSessionAccess | editor/tools/scene/model/include/lux/engine/editor/scene/SceneSessionAccess.hpp | 复用 SessionAccess<SceneSession> 的窄别名/适配 | 读与编辑能力分配给不同消费者；无 engine()/panes()/project()。 |
| P02 | SceneObjectRef / SceneObjectLocator | editor/tools/scene/model/include/lux/engine/editor/scene/SceneObjectRef.hpp | 作者对象身份值 | 复用已有 WorldObjectId；跨会话引用带 SessionId/HistoryId；禁止裸 ECS Entity 作为持久作者地址。 |
| P02 | SceneObjectEdit / SceneFieldEdit / SceneConfigurationEdit | editor/tools/scene/model/src/edits/ | 继承既有 EditOperation/PreparedEdit | 准备候选、预算、逆操作；无 Context、Pane 或可写运行世界；不要从 UI 借 memento。 |
| P02 | PreparedSceneReload（内部值） | editor/tools/scene/model/src/PreparedSceneReload.hpp | 私有候选 | 源、全新历史、checkpoint 和索引一起准备；失败保留旧会话。 |
| P03 | MaterialSession | editor/tools/material/model/include/lux/engine/editor/material/MaterialSession.hpp | final : sessions::IEditSession；组合 source/state/history | describe/read/apply/undo/redo/capture；不持有 preview/compiler/window。 |
| P03 | MaterialSource（复用 lux::material::MaterialSource） | 既有 modules 中定义；model 只做需要的受限适配 | 复用已有领域数据 | 不再造一份同名源并双向同步；作者图布局若被序列化仍属于源。 |
| P03 | MaterialReadView / MaterialSnapshot | editor/tools/material/model/include/lux/engine/editor/material/MaterialSnapshot.hpp | 短借用 / 不可变拥有值 | 节点、参数/纹理槽、状态与图布局均可准确捕获；无渲染句柄。 |
| P03 | MaterialEditBatch / MaterialEditReceipt | editor/tools/material/model/include/lux/engine/editor/material/MaterialEdit.hpp | 具体领域值 | 校验引脚类型/连接/节点与槽引用；提交一次历史。 |
| P03 | MaterialGraphEdit / MaterialValueEdit | editor/tools/material/model/src/edits/ | 保留 EditOperation 协议继承 | 逆操作拥有真实节点/值与代码寿命，不引用 GraphElement。 |
| P03 | MaterialSessionAccess / PreparedMaterialReload | editor/tools/material/model/include/lux/engine/editor/material/MaterialSessionAccess.hpp | 受限访问 / 私有候选 | 重载保留 SessionId、更新 HistoryId，候选失败不改源。 |
| P04 | FlowSession | editor/tools/flowforge/model/include/lux/engine/editor/flowforge/FlowSession.hpp | final : sessions::IEditSession | 组合 SessionState、Flow 作者源、EditHistory；无 compiler/linker/UI。 |
| P04 | FlowAuthoringSource（仅当既有 FlowSource 不能表达可编辑图时） | editor/tools/flowforge/model/include/lux/engine/editor/flowforge/FlowAuthoringSource.hpp | 具体模型适配，不是又一份同步源 | 可复用旧 Content{id,name,FlowGraph}，改准确语义名；FlowSource 是编码/捕获值时不混同。 |
| P04 | FlowSnapshot / FlowReadView | editor/tools/flowforge/model/include/lux/engine/editor/flowforge/FlowSnapshot.hpp | 拥有快照 / 限时只读视图 | 变量、导出、函数签名、字面值、节点布局与环境寿命完整。 |
| P04 | FlowEditBatch / FlowEditReceipt | editor/tools/flowforge/model/include/lux/engine/editor/flowforge/FlowEdit.hpp | 领域值 | 具体编辑不继承 MaterialSession；共享图 UI 不代表共享语义。 |
| P04 | FlowGraphEdit / FlowVariableEdit / FlowLiteralEdit | editor/tools/flowforge/model/src/edits/ | 沿用 EditOperation/PreparedEdit | 保留可逆编辑与引用校验；不依赖 linker。 |
| P04 | FlowSessionAccess / FlowSourceEnvironment | editor/tools/flowforge/model/include/lux/engine/editor/flowforge/FlowSessionAccess.hpp | typed access；环境复用既有类型 | 元信息和 module lease 长于节点、history 与快照。 |
| P05 | SaveId / SaveRequest / SaveStatus / SaveOutcome | editor/persistence/include/lux/engine/editor/persistence/SaveTypes.hpp | 业务值与互斥结果 | 区分 admission、运行状态、已提交事实、采用结果、取消及不确定发布。 |
| P05 | FrozenSave / EncodedArtifact / IEncodeJob | editor/persistence/include/lux/engine/editor/persistence/EncodeJob.hpp | 异步边界接口；FrozenSave 拥有输入 | 无 Session/Pane 裸指针；encoding 输出拥有字节，CodeLease 在外层 owner 中长于 job 虚析构。 |
| P05 | ISaveSource / SaveSourceRegistration | editor/persistence/include/lux/engine/editor/persistence/SaveSource.hpp | 具体会话的角色适配 | owner 上 captureForSave / accept；注册销毁先于 Session；不继承会话或 UI。 |
| P05 | SaveService / SaveOperation | editor/persistence/include/lux/engine/editor/persistence/SaveService.hpp | 服务拥有稳定操作记录 | requestSave/status/requestCancel/acknowledge；worker 仅投递 ID+拥有结果。 |
| P05 | WriteTargetKey / WriteTarget / WriteTicket / WriteLane | editor/persistence/include/lux/engine/editor/persistence/WriteLane.hpp | 同目标顺序机制 | 准入时订票；编码可并行，发布只调度队首；失败/取消/不确定分别处理。 |
| P05 | WriteCoordinator | editor/persistence/include/lux/engine/editor/persistence/WriteCoordinator.hpp | 应用拥有、所有写入生产者借用的唯一具体协调器 | reserve / provideEncoded / cancelBeforePublish / status / reconcile；内部拥有 WriteLane，SaveService、SaveAs、编译发布、WorkspaceStore 共用同一实例。 |
| P05 | IArtifactStore / PublicationOutcome / CommitReceipt | editor/persistence/include/lux/engine/editor/persistence/ArtifactStore.hpp | 真实 IO 副作用接口 | Published / NotPublished / PublicationUnknown；持久性保证独立字段，不伪称全局 CAS。 |
| P05 | ProjectArtifactStore | editor/adapters/project_io/include/lux/engine/editor/io/ProjectArtifactStore.hpp | final : IArtifactStore | 复用现有 ProjectStorage/文件系统；规范化同目标别名与实际版本检查。 |
| P05 | SceneSaveSource / SceneEncodeJob / SceneCodec / ScenePersistenceAccess | editor/tools/scene/persistence/include/lux/engine/editor/scene/SceneSaveSource.hpp | 具体适配，不拥有 SceneSession | capture 内容戳/绑定；显式编码输出；受限基线采用。 |
| P05 | MaterialSaveSource / MaterialEncodeJob / MaterialCodec | editor/tools/material/persistence/include/lux/engine/editor/material/MaterialSaveSource.hpp | 对应材质源的具体适配 | 源保存不是编译产物发布；节点快照与 lease 完整。 |
| P05 | FlowSaveSource / FlowEncodeJob / FlowCodec | editor/tools/flowforge/persistence/include/lux/engine/editor/flowforge/FlowSaveSource.hpp | 对应 Flow 作者源的具体适配 | 编码快照环境而非读取 live FlowGraph；不执行 linker。 |
| P05 | SceneSaveAsOperation / EncodedSceneClone / PreparedSceneRebind | editor/tools/scene/persistence/include/lux/engine/editor/scene/SceneSaveAs.hpp | 具体用例组合 permit/candidate/write ticket | 克隆 package 与映射显式输出；发布前备妥采用，成功不清历史。 |
| P05 | MaterialSaveAsOperation / FlowSaveAsOperation / SaveAsOutcome | editor/tools/material/persistence/include/lux/engine/editor/material/MaterialSaveAs.hpp；editor/tools/flowforge/persistence/include/lux/engine/editor/flowforge/FlowSaveAs.hpp | 具体用例；不建立万能基类 | 绑定冻结范围相同；各自处理格式身份；Export Copy 不改来源。 |
| P05 | PreparedSceneData / PreparedMaterialData / PreparedFlowData | editor/tools/{scene,material,flowforge}/persistence/include/lux/engine/editor/{scene,material,flowforge}/PreparedData.hpp | 拥有解码产物值 | 后台 decode 与 owner 构造分离；P11 通过 SessionLoadJob 接入。 |
| P06 | SceneInstanceLease / InstanceRetirement | engine/scene/composition/include/lux/engine/scene/SceneInstanceLease.hpp | move-only 退休责任；host 存实际记录 | 不增加第二套 Runtime；析构不在 BUSY 回调中同步 destroy。 |
| P06 | SceneRuntime 明确控制 API | engine/scene/composition/include/lux/engine/scene/SceneRuntime.hpp | 原宿主原算法重整 | borrowInstance/borrowClock/pauseSimulation/resumeSimulation/requestStep/retireInstance/driveFrame。 |
| P06 | RunId / RunInfo / RunProvenance / StepTicket / StopTicket | editor/tools/scene/execution/api/include/lux/engine/editor/scene/RunTypes.hpp | 运行域值，不属于 Session 身份 | 票据指向真实一次步进/停止，区分 accepted/completed。 |
| P06 | RunSession / RunStore | editor/tools/scene/execution/include/lux/engine/editor/scene/RunStore.hpp | RunStore 独占 RunSession；不继承 SceneSession | 持启动快照、provenance、instance lease、执行状态；不持 UI 或作者可写引用。 |
| P06 | RunController / StartRunOperation | editor/tools/scene/execution/include/lux/engine/editor/scene/RunController.hpp | 具体启动/控制服务 | capture→prepare→owner 实例创建→发布 RunId；只借用既有执行器。 |
| P06 | RunInspectAccess / RunningObjectRef | editor/tools/scene/execution/api/include/lux/engine/editor/scene/RunInspectAccess.hpp | 轻量读取能力 | RunId+实例/对象代际隔离；UI 不取得 tick/EngineContext。 |
| P06 | RunDebugEdits（仅保留已有临时调试能力） | editor/tools/scene/execution/src/RunDebugEdits.hpp | 运行自己的编辑/undo 记录 | 暂停调试不改变作者源；stop 不回写。RunChangeProposal 未实现时明确不可用。 |
| P07 | SceneProjection / ScenePresentationHub / ProjectionVersion | editor/tools/scene/projection/include/lux/engine/editor/scene/SceneProjection.hpp | 共享派生 owner；不拥有作者源 | 按 ContentStamp/增量更新一个作者投影；多视图共享场景，不共享可写作者事实。 |
| P07 | ViewportPresentation / ViewportConfiguration | editor/tools/scene/projection/include/lux/engine/editor/scene/ViewportPresentation.hpp | 每视口组合资源 | 独立相机/输出尺寸/overlay；以渲染视口身份寻址，不依赖具体 SceneView。 |
| P07 | HighlightRenderer / HighlightKey / PreparedHighlight / SubmitOutcome | editor/tools/scene/projection/src/HighlightRenderer.hpp | 私有分阶段派生组件 | desired/prepared/accepted；背压保留 pin；拒绝分类；后端 GPU 退休独立。 |
| P07 | MaterialCompileId / MaterialCompileOperation / MaterialCompileSettings | editor/tools/material/preview/include/lux/engine/editor/material/MaterialCompilation.hpp | 实际异步编译业务；TaskId 仅关联进度 | 捕获快照+编译配置+环境版本；过期结果不覆盖新预览。 |
| P07 | MaterialPreviewStore / CompiledMaterial | editor/tools/material/preview/include/lux/engine/editor/material/MaterialPreviewStore.hpp | 拥有编译结果和派生预览 | 失败保留作者内容；资源采用与 GPU 退休准确。 |
| P07 | FlowCompileId / FlowCompilationService / FlowCompileOperation / LinkSettings | editor/tools/flowforge/compilation/include/lux/engine/editor/flowforge/FlowCompilationService.hpp | 编译与链接的具体阶段 | 输入 FlowSnapshot；重试链接固定原产物和来源，不捕获 FlowSession*。 |
| P07 | PublishCompiledMaterialOperation / PublishFlowArtifactOperation | editor/tools/material/preview/include/lux/engine/editor/material/PublishCompiledMaterial.hpp；editor/tools/flowforge/compilation/include/lux/engine/editor/flowforge/PublishFlowArtifact.hpp | 具体派生产物发布用例 | 使用 P05 同一写入协调器；不更新作者源保存基线。 |
| P07 | ResourceStatusSnapshot / ResourceRetryRequest | editor/tools/scene/projection/include/lux/engine/editor/scene/ResourceStatus.hpp | 投影资源状态与明确重试值 | 与作者结构修改错误分开；不从旧 SceneEditor 直接查 GPU 私有字段。 |
| P08 | SceneInteractionGroup / SceneSelection / InteractionGroupId | editor/tools/scene/interaction/include/lux/engine/editor/scene/SceneInteraction.hpp | 交互服务拥有；视图仅关联 | 来源带作者/运行身份；共享选择不共享相机；不拥有 Session。 |
| P08 | EditedObjectRef / RunningObjectRef / Gesture | editor/tools/scene/interaction/include/lux/engine/editor/scene/InteractionTypes.hpp | 封闭来源值 / 私有手势状态 | 作者 UUID 与运行 Entity 不混；预览与已提交编辑分开。 |
| P08 | MaterialInteraction / FlowInteraction | editor/tools/{material,flowforge}/interaction/ | 具体图交互组合 | 临时拖拽/选中/候选连接；最终调用具体域编辑，不共用万能图模型。 |
| P08 | ViewId / ViewTypeId / ViewRestoreKey / ViewInfo | editor/views/api/include/lux/engine/editor/views/ViewInfo.hpp | 视图身份和只读值 | ViewId 是 Host 代际身份；ViewRestoreKey 是持久键；ViewTypeId 直接复用 PaneTypeId。PaneId 留作 UI 树注册名，不与前两者互转。 |
| P08 | DetachedView / IViewHost / ViewRequests | editor/views/api/include/lux/engine/editor/views/IViewHost.hpp | 外层 CodeLease+unique_ptr<Pane>；窄 host 接口 | 工厂结果未挂 Root；请求 close/show/focus 不暴露 owning vector。 |
| P08 | Pane/Element 的 detached 构造与内容拥有 | modules/function/ui/include/lux/engine/ui/{Pane,Element}.hpp | 保留真实 UI 继承，明确树 owner | Detached 状态无活动 Root 注册；节点是 owner 线程对象；非 worker 构造。 |
| P08 | PreparedMountBatch / PreparedDetachBatch / AttachmentState | modules/function/ui/ 的 private UI 实现 | Root 局部准备/提交机制 | 先准备容量与路由，提交不调用可失败插件代码；Root 只登记不双重删除。 |
| P09 | LayoutId / LayoutSlotId / DockLayout / DockTree | editor/workspace/layout/include/lux/engine/editor/workspace/DockLayout.hpp | 持久纯值 | 布局组织不含作者源、运行实例或 live SessionId。 |
| P09 | ValidatedLayout / LayoutPlan / LayoutPlanner | editor/workspace/layout/include/lux/engine/editor/workspace/LayoutPlan.hpp | 验证后值与纯计划 | parse→validate→resolve；不修改 Root，不创建活动视图。 |
| P09 | LayoutCatalog / LayoutSummary / CatalogVersion | editor/workspace/layout/include/lux/engine/editor/workspace/LayoutCatalog.hpp | 查询快照 | 目录不是请求；没有 pending/result/action 组合袋。 |
| P09 | RecoveryManifest / RecoveryEntry | editor/workspace/recovery/include/lux/engine/editor/workspace/RecoveryManifest.hpp | 独立恢复描述 | 存可持久定位与视图绑定；不声称能恢复未持久化修改。 |
| P09 | UserPreferences / VersionedViewState / PreservedOpaqueState | editor/workspace/layout/include/lux/engine/editor/workspace/WorkspaceValues.hpp | 独立值/版本化载荷 | 未知 type/schema 字节保留；偏好坏引用可回退。 |
| P09 | WorkspaceStore / LayoutCommitReceipt / PreferenceWriteResult | editor/workspace/storage/include/lux/engine/editor/workspace/WorkspaceStore.hpp | 真实布局/偏好 IO 适配 | save/rename/remove/read/list 各有准确事实，偏好写失败不撤销布局成功。 |
| P09 | LegacyWorkspaceImporter（只读格式适配） | editor/workspace/storage/src/LegacyWorkspaceImporter.cpp | 保留的数据迁移边界，不是旧 owner | 解析旧数据到新值；不 include WorkspaceRequest/旧 PaneManager，不执行未知 provider。 |
| P10 | ViewHost / DesktopShell | editor/desktop/include/lux/engine/editor/desktop/{ViewHost,DesktopShell}.hpp | ViewHost : views::IViewHost；Shell 组合平台/Root/呈现 | Host 独占顶层 Pane；Shell 不持作者模型，不解析具体工具绑定。 |
| P10 | SceneView / SceneViewState / SceneViewBinding | editor/tools/scene/ui/include/lux/engine/editor/scene/SceneView.hpp | final : ui::Pane；组合视图状态/绑定/呈现 | Unbound/EditedSceneBinding/RunningSceneBinding 显式 variant；无 history/save/run owner。 |
| P10 | OutlinerView / InspectorView / ResourceView / SceneCreationView | editor/tools/scene/ui/include/lux/engine/editor/scene/{OutlinerView,InspectorView,ResourceView,SceneCreationView}.hpp | 真实 Pane 或 Element UI 特化 | 调用 Scene/Run 窄访问；不成为 SceneSession::Impl 的 friend。 |
| P10 | MaterialView / MaterialViewState / MaterialViewServices | editor/tools/material/ui/include/lux/engine/editor/material/MaterialView.hpp | Pane + typed SessionKey + 图交互/预览 | 视图有零/一绑定；不自动新建或销毁 MaterialSession。 |
| P10 | FlowView / FlowViewState / FlowViewServices | editor/tools/flowforge/ui/include/lux/engine/editor/flowforge/FlowView.hpp | Pane + FlowSessionKey + FlowInteraction | 图 UI 不拥有源/history/compiler；支持已有变量/导出/函数交互。 |
| P10 | makeSceneView / makeMaterialView / makeFlowView | editor/tools/{scene,material,flowforge}/ui/src/ViewFactory.cpp | 普通工厂函数，避免无意义工厂继承树 | 输入受限 services/create info；返回 DetachedView，不接管/复用/聚焦。 |
| P10 | TaskView / TaskQueryPort | editor/tasks/ui/include/lux/engine/editor/tasks/{TaskView,TaskQueryPort}.hpp | 只读任务面板与受限请求 | 复用执行器真实任务状态；不是新的任务管理器。 |
| P10 | ProjectView / AssetPickerElement / ProjectCatalogAccess / AssetOpenRequests | editor/project/ui/include/lux/engine/editor/project/{ProjectView,AssetPickerElement,ProjectCatalogAccess,AssetOpenRequests}.hpp | 项目 UI 与只读查询/明确请求接口 | 不持 EditorContext；打开资产是请求，不返回任意 Pane 引用。 |
| P10 | Editor widgets / Inspector 字段适配 | editor/widgets/ 与 editor/tools/scene/ui/codegen/ | 通用控件仅 UI；领域 adapter 在 tool/ui | 模板/生成代码不传播 scene/runtime 到通用 widgets。 |
| P11 | CommandDescriptor / CommandState / CommandQuery / CommandInvocation | editor/commands/include/lux/engine/editor/commands/Command.hpp | 描述、只读输入、确定目标意图 | query 不取全局 Context；execution 重新验证目标和可用性。 |
| P11 | DispatchReceipt / OperationRef / RegistryBinding | editor/commands/include/lux/engine/editor/commands/DispatchReceipt.hpp | 准入结果/仅 UI 跟踪身份/明确注册绑定策略 | ImmediateCompletion 与 AcceptedOperation 分开；不携所有业务状态的全局 any。 |
| P11 | CommandEntry / CommandRegistrySnapshot / CommandHandle | editor/commands/include/lux/engine/editor/commands/CommandRegistry.hpp | 不可变记录和拥有 snapshot 的调用句柄 | query/execute 同样保活 entry+code；批次内不替换正在迭代容器。 |
| P11 | SessionKindDescriptor / ViewFactoryRegistration / SessionFactoryRegistration | editor/extensions/api/include/lux/engine/editor/extensions/Registrations.hpp | typed 描述与执行入口组合 | 构造、接管、复用、显示分开；无 PaneManager& factory。 |
| P11 | SessionLoadJob / PreparedSessionData / PreparedSessionInstallation | editor/extensions/api/include/lux/engine/editor/extensions/SessionFactory.hpp | worker 准备角色与 owner 安装值 | worker 只拥有 decode 结果；owner 构造线程亲和 Session/History；发布前角色齐备。 |
| P11 | HistoryActions / SessionRoleBundle / SaveAsActions | editor/extensions/api/include/lux/engine/editor/extensions/SessionRoles.hpp | 按实际需要定义的窄角色适配 | history query/undo/redo、保存源、SaveAs；可缺省角色明确可用性，不用万能服务表。 |
| P11 | ExtensionSnapshot / ExtensionPublisher | editor/extensions/host/include/lux/engine/editor/extensions/ExtensionHost.hpp | 批次发布服务，不是全局 ServiceLocator | 只在 owner 发布阶段替换；在途实例/任务独立保活旧代码。 |
| P11 | SceneToolModule / MaterialToolModule / FlowToolModule | editor/tools/{scene,material,flowforge}/integration/ | 装配函数/对象，不给业务传递 | 绑定具体工厂、角色、命令、编译/运行与视图；不封装可达整个引擎的 getters。 |
| P12 | OpenAssetOperation / OpenId / OpenResult | editor/workflows/include/lux/engine/editor/workflows/OpenAsset.hpp | 具体操作集合拥有，不是万能 workflow 基类 | session publication 与 presentation 结果分开；相同资产默认复用工作副本。 |
| P12 | ReloadSessionOperation / ReloadOutcome | editor/workflows/include/lux/engine/editor/workflows/ReloadSession.hpp | 候选与原子采用用例 | 处理在途写入、审阅戳；保留 SessionId，成功更新 HistoryId，失败不 clear。 |
| P12 | CloseViewOperation / CloseSessionsOperation / CloseViewPolicy / CloseReport | editor/workflows/include/lux/engine/editor/workflows/CloseSessions.hpp | 不同生命周期动作 | 固定被审阅内容集合与 stamp；全部许可后才提交删除；Cancel 不回滚已保存。 |
| P12 | SaveAllOperation / SaveAllReport | editor/workflows/include/lux/engine/editor/workflows/SaveAll.hpp | 固定会话集合的保存编排 | 按 Session 而非 Pane 保存；逐项记录准入/结果/缺角色，不能重复保存两视图同源。 |
| P12 | ApplyLayoutOperation / LayoutApplyReport / RestoreSessionOperation | editor/workflows/include/lux/engine/editor/workflows/ApplyLayout.hpp | 布局结构提交与内容恢复分开 | pure plan→prepare→safe commit→report；偏好保存独立结果。 |
| P12 | InstalledSession / SessionViewBindings / SessionCloseDependencies | editor/workflows/src/SessionInstallation.hpp | 角色注册与关联记录，不拥有第二份源 | 撤销角色/视图/运行依赖后 Store 消费许可；领域退休 hook 由 integration 注入。 |
| P12 | ModelCreationOperation | editor/tools/scene/integration/src/ModelCreationOperation.hpp | 领域特定长期操作 | IO产物校验后一次 SceneEditBatch；载入完成不等于模型插入完成。 |
| P12 | EditorApplication / EditorApplicationConfig | editor/application/include/lux/engine/editor/EditorApplication.hpp | 生命周期装配根 | create 返回完整成功对象；exec 驱动既有宿主；不保存各用例中间字段。 |
| P12 | 工具/桌面/用例装配函数 | editor/application/src/ 与 editor/tools/*/integration/ | 连接具体依赖，不作业务参数 | 唯一产品 lux_editor 指向 editor_bootstrap；没有 old/new 路由开关。 |
| P13 | 最终 architecture/retirement/test evidence | docs/editor-redesign/ | 证据，不新增产品类型 | 账本中全部过渡已关闭；最终模块、公开头、安装和功能覆盖一致。 |
| P13 | 分层 CI workflow |  .github/workflows/ 下对应 Editor/Player 工作流 | 工程门禁 | 每次相关 PR 跑 native/边界；desktop/GPU/toolchain/installed 按真实环境运行并保留结果。 |
| P13 | 独立消费者与公共头测试 | editor/tests/installed/ 与 editor/tests/architecture/ | 测试支撑不进产品库 | 安装后 clean prefix 构建，不读取源码/构建私有头；负向依赖夹具仍有效。 |
| P13 | 性能/容量基准 | editor/tests/benchmarks/ | 可复现实验，不作为新框架 | 测启动/交互/快照/多视图/GPU 退休/长期任务保留，不预设提速。 |
