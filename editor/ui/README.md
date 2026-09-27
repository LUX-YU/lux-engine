# 共享 Editor UI

editor_ui 拥有 SceneElement、空间交互、Inspector 支持与生成器，以及 Presentation。
它不链接具体 SceneEditor、MaterialEditor 或 FlowForgeEditor。
SceneEditing/FieldEdit 位于静态库 editor_editing_scene，SceneRegistrations 位于 editor_metadata。

SceneElement 是 Element，必须位于 Pane 内容树中。借用 SceneRuntime、SceneInstanceId 和 RenderResources，
绑定明确的 SystemInstanceId 与相机 Entity，通过 RenderViewRequest/Result 维护视口；不借用 RenderSystem，
不执行 SceneDriver，也不拥有内容历史。每次访问重新验证 ID；可写 Registry 仅在安全点取得。
请求采用、发布和实际输出代次一致且已有 producer 证据后才换图，等待时保留旧图。
析构撤销 stop_token、释放图像引用；专用请求实体由系统在安全维护中回收。关闭后不再访问实例，只读取回执。
内部 ImageElement 使用统一 TextureHandle，按最终矩形和 framebuffer scale 请求离屏尺寸。

图像发布、共享资产和 GPU 退役继续由 RenderResources/RenderSystem/RenderRuntime 承担。
关闭 SceneElement 不停止共享 Renderer，也不能把一次 poll COMPLETE 当成 GPU 已退役。

InspectorPane 属于 SceneEditor 的私有 UI，不由本包导出。本包提供组件 Element 工厂及生成支持，
可以由消费者自己的 Pane/Layout 直接装配；工厂显式借用 SceneEditing 与 Entity。
生成器产出固定控件和 Layout；动态序列、映射、可选值和变体在维护阶段更新有限的行控件。
Registry 是字段权威，控件只持有交互缓冲。finishEditing 在切换目标/关闭前提交当前有效交互，BUSY 保留原目标。
组件记录保证先完成 Element 析构，再释放对应的插件代码寿命。
Element::finishEdit 统一结束控件交互，Inspector 不识别 TextEdit/NumericEdit 的具体类型。颜色、四元数等专用绘制仅编辑 Element 的候选值，再通过相同的 SceneEditing apply/patch/提交协议采用；不保留 Registry 字段裸指针或绘制缓存。

构造过程中用局部 EditorResult 汇总信号连接和子树工厂失败，完整工厂检查后才发布。该结果不保存为长期 UI 状态。生成器的字段校验没有生成副作用，固定字段和动态行共享控件选择与配置。

Presentation 持有 SceneRuntime 创建的正式 UI 场景 ID、固定帧槽和可选原生窗口输出。
UI 场景使用空 Simulation，仅安装带 UI Feature 的 RenderSystem；保持 invalid，不执行游戏规则。
Root::update 内 collectDrawData 捕获后由 drawDataReady 立即固定纹理引用，
Presentation::applySceneInput 在安全点将固定帧写入 Registry 的私有 UiFrame。
UiRenderSyncStage 经 RenderSystem 的正式 FRAME 包发布；Presentation 不再拥有独立包或 submit 旁路。
背压保留同一输入，帧槽必须没有 Registry/后端借用且 submission 完成才可复用。
tryAcquireDrawData 只提供可写槽位，不承担收集。RenderFrame 的 DrawData 字段名为 draw_data。
析构撤销原生视口需求、销毁 UI 场景，收取必要回执后才允许宿主销毁原生窗口。
应用级图形集成使用 EngineContext 的 RenderContext；独立测试可显式构造 SceneRuntime/RenderRuntime/Resources。
makeRenderConfiguration 返回正式 portable 字体配置，RenderSystem 使用已有 codec 转为 attach 输入。
SceneElement 与编辑绑定已迁移 Runtime＋ID；多个显示组件不会增加该实例的 Simulation 步数。
宿主在 Root.update 前后执行 applyPendingChanges。工具实例/History 的采用和私有视口子树装配在该外层安全点完成；
update 只登记这些结构意图。无可写 DrawData 时仍执行维护与采用；ExecutionRuntime 的析构等待不排空这些 UI 动作。
资产激活、菜单及文件选择归实际 Editor/UI owner，不经过 Presentation。
DocumentView/GuiView/DocumentPane/GuiDocumentProvider 与二次 attach 已删除；具体文档一次装配完整 UI 子树。
独立安装消费者验证本库无需链接具体 SceneEditor；最终桌面和性能证据见仓库 `.internal` 审计。

源码路径为 `editor/ui`；继续使用 `editor_ui` target 和 `lux-engine-editor-ui` 安装包。
公共 include/namespace 统一为 `lux/engine/editor/ui` / `lux::editor::ui`，没有 gui 转发头。
SpatialInteraction 只计算导航、射线和放置候选，不保存另一个场景视口。
InspectorInteraction 只保存当前交互 token、结构操作及错误，使用 optional 表达活动交互；
finish() 使用自身的 SceneEditing 借用，外部通过查询获取错误和只读状态。

AssetPickerElement 保存资产字段的显示值和类型约束，用户选择通过 edited 信号交给 History owner。
setValue 不发编辑信号；外来、过期或类型不符的拖放保留原值。AssetDragDrop 只负责票据编码约定与解码。
控件不读文件、不创建 GPU 资源。Mesh Inspector 和材质纹理槽共用该控件。
生成器 CUSTOM_ELEMENTS 可将业务值映射到持久控件，仍经 TFieldElement 的身份验证和 patch/历史协议。
ComponentEditorRegistry 由 Context 一次装配，具体 Inspector 只查询固定记录，不重复合并目录。

组件工厂和生成字段控件仅接收 `simulation::ecs::Entity`，实例关联固定在 `SceneEditing`。
`SceneWriteTarget` 独立保存 scene_id、Entity、History 状态和 revision；从另一个实例转交的票据会被拒绝。
SceneEditing 固定借用 Runtime＋ID；实例被销毁后旧编辑票据返回 STALE_TARGET，不能命中新实例。
