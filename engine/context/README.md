# EngineContext

EngineContext 拥有应用共用的 ExecutionRuntime、SceneRuntime 和 AssetVfs。Headless 程序不依赖 Render。
CPU 任务和阻塞 IO 使用同一执行设施中不同的调度资源，Scene TaskExecutor 不创建私有线程池。

图形应用组合根另外构造 RenderExecution 与 RenderAssets，借用 `engine.execution()`；
通过 SceneCapabilityProvider 向具体 Scene 提供窄依赖。ContextExtensions 仅用于冷态装配，
发布后不补写 Render 服务，也不通过查询偷偷创建对象。

关闭必须先撤销新业务并清偿 Scene、Render、文件提供者和插件回调，然后销毁这些借用者，
最后销毁 EngineContext。Scope 完成不等于外部 Producer 的调用栈已经退出。
等待期间推进实际完成通路，不运行新的 Simulation 或 UI 业务。

完整图形应用接线合同见 [统一接线设计](../../docs/render-v2/engine-editor-integration.md)。
