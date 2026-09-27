# Editor 插件元信息

本模块解释 Editor 导出、构造配置编辑对象，并生成 SceneRegistrations。项目插件描述、依赖解析、
Runtime 装库和模块寿命由 [project/plugins](../../project/plugins/README.md) 提供；游戏无需链接 editor_metadata。

## 采用边界

`project::PluginManager` 在一次项目打开时固定 Runtime 模块集合。Editor 按依赖顺序调用
`loadEditorPlugin(description, runtime, dependencies)`，返回 `EditorPlugin` 记录：模块身份、代码引用及已校验的
`EditorPluginExports` 表。它不复制 Runtime 注册，也不转发 Runtime 查询。

没有 Editor 扩展的插件返回空导出记录，这是合法情况。声明了工具库而实际缺失、ABI 或版本不匹配时准确失败。
Runtime 的 `PluginLibrary::load()` 从不打开工具库；公共二进制身份校验复用 `project::loadPluginLibrary()`。

当前 Editor 启动只读取 ProjectManifest version 2 的 `plugins` 选择，不再从 main 填入初始插件名单，
也不支持在运行中追加/替换插件。SettingPane 的私有插件内容修改清单，经 Project 原有发布事务保存，下次完整打开项目生效。
模块在候选装载期间不会修改反射、Scene Registry 或 Renderer。

## 配置与反射

`lux_editor_exports_v6` 提供显式反射登记、typed 配置编辑回调、可选组件 Element 工厂表和 独立的 PaneRegistration 窗口工厂表、AssetEditorRegistration 资产路由表。
Runtime 导出维持自己的版本；只提供 Runtime 的游戏插件不依赖工具库。
`ConfigurationValue` 通过工具 RefClass 构造、销毁真实 C++ 对象，并持有相应代码引用。
portable codec 编码对象；解码先准备候选值，失败保留原值。非平凡对象不放进任意字节数组中强制解释。

Main 使用 `ReflectionRegistrationDraft` 准备反射、校验配置类型，再向 RenderRuntime 登记完整 Feature 集合。
未 commit 的失败沿原协议撤回；commit 后的后续启动失败关闭本次 Runtime。不能把 commit 解释为可热卸载。
Scene 实例自己的正式描述决定系统和 Feature 选择，插件可用不等于所有实例必须使用它。

`SceneRegistrations` 提供组件 schema、Simulation Registry、SceneSystem 和 RenderFeature 装配输入。
EditorContext 持有这组固定共享登记；具体工具借用固定装配输入。
ComponentEditorRegistry 在 Context 创建时合并产品和插件条目，验证 schema、provider、工厂与唯一性。
它只保存编辑扩展及 schema 关联；Runtime schema 仍是组件类型与运行操作的权威。
没有 UI 工厂是合法情况。Inspector 持有固定目录，组件行先销毁 Element，再释放工厂代码引用。
旧 ComponentBinding 公共头及每次打开时重新合并列表的路径已删除。

普通信号使用 LuxObject::connect 的模板检查与成员 TSignal，不使用按名称反射接线或静态信号代码生成。
业务组件和配置仍使用原有反射/生成工具。

## 代码寿命

EditorPlugin 的代码引用覆盖导出表使用期；ConfigurationValue、反射、生成控件及异步捕获继续保留必要的
代码引用。Runtime 注册各自保留 Runtime 二进制及其依赖。先完成消费者和 GPU 的关闭，再释放登记和模块。

## 验证

`plugin.physics2d` 覆盖声明/实际导出一致性、依赖版本、Runtime 不读取 Editor 库、配置及代码寿命。
`plugin.external_gpu` 使用仅依赖 project_plugins 和公开 Runtime SDK 的外部插件，验证组件 patch 后实际像素变化。
安装产物与外部消费者命令见共享插件模块 README。
