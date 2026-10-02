# 应用共用设施

EngineContext 拥有 ExecutionRuntime、SceneRuntime 与资产 AssetVfs，可选拥有 RenderContext。
SceneRuntime 统一拥有实例、逐实例 Clock/Driver 和共享执行器；Context 不保存项目选择、窗口或播放意图。
宿主通过 sceneRuntime().tick() 维护和推进场景，Context 不提供通用服务查找。
各业务继续接收需要的窄依赖并管理自己的操作和 TaskScope。

`engine_context` 为不链接图形库的 STATIC 组件。无图形游戏只需此核心。
图形宿主另链接 `engine_context_render`，通过 initializeRendering 一次构造 RenderContext 和统一日志出口。
RenderContext 拥有 RenderRuntime、资源 TaskScope、RenderResources 及运输完成接线；
registerFeatures 冷装配沿 Runtime 的候选/回滚协议执行。EditorApplication 和 Launcher 只借用资源设施。
Editor 和 Launcher 必须具备 Renderer，不能把空指针当成启动成功。

宿主先释放业务、视口和项目 owner，再销毁 EngineContext。各 owner 的析构等待自身已接纳工作；
Context 依次销毁 SceneRuntime（取消并收取集中 timer）、RenderContext（Resources 退役、任务收取、Runtime join），再 join 执行设施。等待只推进完成设施，不运行 UI 或 Simulation。
图形完成适配既采用回执，也转交已接纳上传和释放命令，因此析构不依赖外层 exec 再跑一帧。
资产 VFS 仅承载资产；项目清单、插件描述、journal 和外部导入路径仍使用原生文件 IO。
