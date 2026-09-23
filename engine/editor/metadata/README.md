# 元信息目录与插件装配

本模块拥有 Editor 使用的安装描述、DLL 装载与配置编辑对象。Scene 和 Simulation 消费各自的运行注册表，
不读取这里的 JSON，也不初始化完整 ReflectionRegistry。插件支持同一 SDK ABI 下的追加装配，不支持热卸载或替换活动实例。

## 安装产物

| 路径 | 内容 |
| --- | --- |
| `share/lux-engine/editor/capabilities.json` | 已安装插件的纯数据目录，可在加载 DLL 前读取 |
| `share/lux-engine/plugins/<module>.json` | 单个插件的身份、依赖、能力、系统、配置、组件与 Feature 契约 |
| `share/lux-engine/editor/schema/plugin.schema.json` | 描述格式及字段约束 |
| `share/lux-engine/plugins/LuxPlugins.cmake` | 外部插件身份与安装描述生成入口 |
| `bin/` | 运行库和可选 Editor 工具库；描述中的位置相对于安装根 |

`EngineMetadata` 拥有目录字符串和记录，读取时不加载代码。追加保持已有记录地址稳定，按 Ability 查询实现使用追加时建立的索引。
读取校验结构、重复身份、路径和运行声明 SHA-256；展示文字不进入运行声明摘要。插件依赖按明确版本解析，缺失和环均返回错误。

`PluginLibrary::load()` 校验实际库的 `lux_plugin_identity_v1`，核对模块、接口版本、SDK ABI、构建身份和声明摘要，
随后取得有类型的导出表并核对描述与实际注册。这里的校验不是不可信代码沙箱：操作系统加载库时已可能执行 DLL 初始化代码。
SDK ABI 是相同编译环境和公共头契约的约束，不承诺任意编译器之间的 C++ ABI 兼容。

## 运行契约与工具契约

| 导出 | 运行消费者 |
| --- | --- |
| `lux_simulation_exports_v1` | `SimulationSystemRegistry`：工厂、配置 codec、访问声明、能力 |
| `lux_scene_exports_v1` | SceneSystem 注册：构造、要求、typed 连接与阶段 |
| `lux_component_exports_v1` | `ComponentSchemaSet`：组件生命周期、持久 codec 与引用访问 |
| `lux_render_exports_v1` | `RenderFeatureRegistration`：工厂、portable 配置及可选择性 |
| `lux_render_scene_exports_v1` | `RenderFeatureSceneBinding`：实际组件观察和提取 stage |
| `lux_editor_exports_v1` | 可选工具库：显式反射登记函数和 typed 配置编辑回调 |

运行注册保留代码 lease。Simulation、Scene、提取阶段、Renderer 操作和在途资源不能比所调用代码活得更久。
Render 后端保留已接受的模块直到关闭完成。库已登记不表示当前文档已选择其系统或 Feature；正式描述才决定实例装配。

`SceneCreateInfo` 接收组件 schema、Simulation 注册与 SceneSystem 注册。连接使用
`sceneSystemConnection<Signal, Method, Delivery>`，编译期检查签名，不按反射成员名寻找方法。
`object` target 只提供对象、dispatcher、连接和 typed signal；按名字的反射适配位于可选 `object_reflection`。
组件 schema 中不再保存 Editor 可见性策略。

## 配置对象和反射采用

`ConfigurationValue` 使用工具库的 RefClass 实际构造和析构 C++ 对象，保留工具代码 lease；
`std::string`、`std::vector` 等非平凡对象不能放进字节数组后直接当对象使用。
编辑回调接收 `ui::Frame` 和真实对象；portable codec 负责持久内容。解码先构造候选值，失败保留原值。
Physics2D 提供实际 scalar 配置编辑回调；内置 Render 工具库提供显式反射登记。

`ReflectionRegistrationDraft` 先在私有目录登记、解决类型引用和检查重复。放弃 draft 不改变已发布反射。
Editor 在 Main 持有共享反射生命周期，Flow 文档的反射环境也保留这个生命周期；加载新插件不销毁并重新初始化全局目录。

## Renderer 追加协议

```text
读取描述、加载并验证实际导出
  → Main 准备反射 draft 和未来文档输入
  → RenderRuntime.beginFeatureRegistration()
  → 现有 Control 队列登记；poll 消费回执
  → READY：私有候选全部就绪
  → Main 同一轮 commit：Render catalog、反射、文档输入、可用插件列表
```

失败或取消沿原队列反向撤销已接受的登记，未完成回执保持其 owner。活动文档继续使用原注册和实例。
关闭先取消加载并收取已接受工作，再关闭 UI、Scene、Renderer，最后释放借用的代码和执行资源。

冷登记和 Control 登记调用同一后端实现。同 ID、同完整契约的重复登记复用原操作并计数；
不同契约同 ID、同名不同 ID 拒绝。Feature 最多 16 个操作，第 17 个在登记前拒绝；中途失败回收本次前缀。
Dispatcher 不覆盖旧名称，耗尽的槽位和代次不回绕复用。Catalog 追加不移动旧记录。

Feature 的 attach 顺序由正式选择列表和显式依赖决定。缺少必要依赖、版本错误、冲突或环均失败，
不自动添加默认 Feature。作者 Scene 与 Run 都使用正式 SceneDescription，不按组件类型补 Grid、Highlight 或加载系统。
当前 Scene 编辑器要求正式声明 WorldLoadingSystem，缺少时给出结构化打开错误。

## 生成与外部插件

现有声明解析任务同时输出静态类型、typed 操作、工具反射和元信息字段投影。内置运行描述由 build tool 读取实际导出契约生成；
Physics2D 描述从实际 system/access/schema 声明生成。内置 Feature 列表从现有 `LUX_COMM_CONFIG` / `LUX_COMM_VARIANT` 生成，
Editor 根部没有平行的具体 Feature 名单。

外部插件通过 `lux_add_plugin_exports(TARGET ... DESCRIPTION ... INPUTS ...)` 生成库身份和描述；
`INPUTS` 应覆盖决定插件行为的源码及生成产物。生成器先准备全部输出，再发布成套结果。
同一输入内容不重写输出。需要自己的目录时调用 `lux_generate_capabilities(BASE <installed-capabilities>)`。

安装示例位于 `share/lux-engine/examples/external-feature`。它仅依赖公开 SDK，包含一个实际 Vulkan 绘制 Feature、
一个 ECS Tint 组件和观察 stage；测试读回红色像素，再通过 `Registry::patch` 修改组件并验证绿色像素。

```sh
cmake -S <sdk>/share/lux-engine/examples/external-feature -B <consumer-build> -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=<sdk> \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build <consumer-build> --target all -j 4 -- -k 0
ctest --test-dir <consumer-build> --output-on-failure
```

Windows 使用与 SDK 相同的 x64 Visual Studio 工具链，并提供 Python 和 Vulkan glslc。
第三方依赖通过自身工具链提供；插件不引用引擎源码树、`pinclude` 或 `sinclude`。
独立 object 核心消费者位于 `cmake/installed-consumers/object-core`，配置时检查没有引入 meta 或 object_reflection。

此路径没有实现 Launcher、项目目标生成、运行中系统替换、任意插件 ABI 兼容或不可信代码隔离。
