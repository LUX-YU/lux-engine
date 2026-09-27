# Editor plugin leaves

RenderSystem 与 Physics2D 的配置编辑、反射注册和 Editor 插件出口由本目录拥有。
运行时 Systems 留在 Scene/Simulation，本目录通过其公开接口注册元信息。
两个 target 均分类为 EDITOR/PLUGIN，安装到 `lux-engine-editor-plugins`，不进入 Player 闭包。
Runtime/Editor DLL 的身份与描述在 `engine/CMakeLists.txt` 中一次配对，继续使用既有插件协议。
