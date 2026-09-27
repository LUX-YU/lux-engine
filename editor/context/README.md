# EditorContext

每个项目的 EditorContext 借用 EngineContext 的 ExecutionRuntime、资产 VFS 与 RenderContext。
RenderRuntime、RenderResources 和 SceneRuntime 由 EngineContext 一侧拥有；这里不另建渲染缓存。

Context 拥有 ProjectStorage、固定插件/元信息、AssetImporter 和 PaneManager。
PaneManager 最后构造、最先析构，只拥有一级窗口的 vector<unique_ptr<Pane>>；具体工具以成员 RAII 管理子 Pane/Element。
窗口登记为 PaneRegistration，资产路由为 AssetEditorRegistration，History 不再放入工厂登记。
工厂决定复用或新建；编辑工具默认多实例，同一资产（含正在加载的资产）通过同步值查询复用。
窗口 ID 与资产/类型独立，窗口查找只供 owner 线程短借用；退出审阅期间冻结创建/删除。

产品通过 Editor::create 的冷装配函数，在 Root/Context/Presentation 完整构造后登记工具。
具体工具名单只在产品 executable 的 ProductAssembly.cpp 中；通用 app 不链接具体编辑器。
插件导出 v5 独立提供窗口和资产登记；候选登记验证失败保持旧集合，旧代码保留至窗口析构返回之后。
项目切换使用新 Editor 进程，不热替换正在运行的插件。

Project、Importer 和资源管理器用 Process CompletionWork 按完成事实推进；主循环没有服务 poll 名单。
后台捕获固定输入与窄能力，不跨线程查询可变 Context。隐藏窗口不停止共享任务。
Context 将任务目录变化桥接为 taskChanged/tasksReset，taskRevision 支持遗漏通知后的重查。
TaskPane 只读取这些事实；编译/加载结果仍定向交付给其 owner。

析构先结束所有顶层窗口，再完成 Importer/Project 已接纳任务，最后释放元信息和插件代码。
借用的 EngineContext 必须仍存活；waitUntil 只收取完成，不绘制 UI、不派发新的业务或替换实例。
本模块为 STATIC、EDITOR library，不链接任何具体编辑器。
