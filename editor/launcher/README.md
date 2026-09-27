# Launcher 与共享项目创建窗口

`lux_launcher` 拥有自己的 EngineContext、窗口、UI Root 和 UI Scene。
`editor_launcher` STATIC 库提供 ProjectCreationPane，Editor 可使用同一窗口创建新项目。
窗口只借用 Root、ExecutionRuntime 与安装位置，不需要临时 EditorContext/ProjectStorage。

阶段为位置及插件 → 2D/3D 内容预设及单分区 → Simulation → Scene/provider → Feature → 关系 → 确认。
SceneConfigurationElement 位于 editor/ui，已有 SceneEditor 的新场景窗口也复用它。
预设展开为可见的正式选择，返回页面不重置已编辑的系统配置。Grid 方案当前明确不可用。

提交后以模态等待显示进度，CPU 编码和 Blocking 文件操作不会阻塞 UI。
关闭请求先取消并收取已接纳操作，不能销毁仍被任务引用的表单。
项目提交后通过 platform/process 的 argv 接口启动独立 Editor，不拼 shell 命令。
启动失败保留已创建项目并允许重试；当前 Editor 的项目与未保存内容保持有效。

`lux_launcher --smoke` 验证短时 UI/GPU 推进与关闭；它不替代完整向导的桌面交互验收。
