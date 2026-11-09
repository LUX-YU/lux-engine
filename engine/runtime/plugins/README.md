# 项目插件

`plugin_runtime` 是共享静态库，包含插件目录、依赖解析及 Runtime 动态库装载，不依赖 Render、Editor、Pane、ImGui 或工具反射。
它位于宿主组合边界；低层组件/系统不反向依赖它。

- `PluginCatalog` 读取不可变描述并建立能力与分区方案索引，记录安装根与描述来源，不打开 DLL。
- `PluginLibrary` 校验二进制身份及 Runtime 导出，保留依赖代码；不打开声明中的 Editor 库。
- `PluginManager::create(catalog, selection)` 先验证完整选择及依赖，再加载一次固定模块集合。
  缺失、重复、版本不匹配或依赖环返回结构化失败，不按搜索顺序替用户选实现。
- `loadPluginLibrary()` 复用单个二进制的路径/ABI/声明校验；具体层解释自己的导出表。
  Editor 侧通过这一边界打开工具扩展，共享层不认识 EditorPluginExports。
- `SceneRegistrations` 从已验证的 Runtime 导出构造实际 Scene 注册值。
  渲染的 SceneSystem 与组件走同一接点；应用通过 RenderSystemDependencies 提供自己的 RenderExecution 和资产设施。
  Render Feature/Storage 的外部登记直接使用 Render SDK，不再经过额外的项目渲染接纳层。

系统数据输入/输出进入插件描述，区分允许的 schema 与阶段完成的数据。当前写出 v4，字段合同只保存
`id/type`；控件范围、默认显示、单位和编辑权限不属于运行目录。v3 描述仍在读取边界校验原始摘要，
其中的旧展示字段不会进入运行对象。
System 不声明 World 分区器名称；Scene 冷态组合检查真实供给与顺序，PluginLibrary 核对二进制登记和目录声明。
内置 lux.builtin.runtime v2 不依赖图形；lux.builtin.scene_render v1 依赖 runtime v2，
其动态库链接当前 scene_render 实现，不再要求旧 render 插件目录条目。

模块保留的注册对象仍持有代码引用，因此任务和 GPU 消费者完成关闭前不会卸载被调用的函数。
目录和模块引用是不同事实，不能用纯数据目录替代代码寿命。

## 安装

| 路径 | 内容 |
| --- | --- |
| `share/lux-engine/plugins/catalog.json` | 已安装插件描述集合；格式仍是 lux.engine.capabilities v1 |
| `share/lux-engine/plugins/<module>.json` | v4 单个插件描述（与 catalog 根版本独立） |
| `share/lux-engine/plugins/schema/plugin.schema.json` | 插件描述 schema |
| `share/lux-engine/plugins/LuxPlugins.cmake` | 外部插件身份生成与安装接点 |
| `include/lux/engine/plugins/` | 共享公开接口 |

CMake 消费者使用 `find_package(lux-engine-plugins REQUIRED COMPONENTS plugin_runtime)`，
链接 `lux::engine::plugins::plugin_runtime`。不需要 editor_metadata。

产品根据项目选择传入 MetadataIdentity 数组，选择之外的目录条目不会自动加载。
目录可来自安装根，也可来自明确的项目描述位置。严格核对模块身份、版本、SDK ABI、构建身份及声明摘要；
本接口不承诺不同编译环境间的 C++ ABI 兼容。

外部插件使用 `lux_add_plugin_exports(TARGET ... DESCRIPTION ... INPUTS ...)`，INPUTS 覆盖影响实现的源码和生成产物。
有反射字段的值通过 `VALUE_HEADERS` 和实际编译该声明的 `VALUE_SOURCE` 提供，模板属于本模块的
`template/value_contract.template`。DLL 复用 OBJECT 源时，用 `VALUE_TARGET` 指定 OBJECT target；
否则默认使用插件 target。生成字段由 Schema 或明确的 `_cpp_type` 身份与声明关联；不透明配置继续使用
自己的 Codec。产品装配这些输入，Core/ECS 生成器不查找插件或 Editor。
可用 `lux_generate_capabilities(BASE <sdk>/share/lux-engine/plugins/catalog.json)` 合并 SDK 目录。
外部 Render 扩展使用已安装的 FeatureRegistration/StorageFactory/CommandHandler 合同；
冻结的旧 Vulkan 插件示例不参与当前安装。

当前 Editor 项目文件为 `.luxproj`：`format = "lux.editor.project"`、`format_version = 1` 或 `2`。v2 可以显式保存 `content.geometries/partitions` 的精确允许集合。
`plugins` 只保存正式插件身份与精确版本；空数组表示明确的空选择。插件身份与目录共同使用
`plugin_identity` 的 `[A-Za-z_][A-Za-z0-9_.-]*` 规则；纯选择值不链接插件装载器。
项目显示名称不是插件身份。实际清单文件的规范路径由 Editor 的 ProjectDescription 保留，
项目格式和读写由 editor/project 提供，游戏层不依赖它。旧 `.luxproject` 及 ProjectStorage 仅属于冻结参考源码。

Scene 插件导出 `lux_scene_exports_v3`：同一表包含 Scene System、几何声明和分区算法声明。
`SceneRegistrations` 是唯一不可变项目快照；读取时校验实际 DLL 导出与描述元数据，并保留实际代码寿命。
旧项目没有 `content` 时，仅在装配时从已选择的插件声明展开允许集合；显式空集合表示不允许该类内容。
Editor 展示元数据不参与 Geometry/Partition 类型资格，配置有 Codec 而没有控件描述是合法情况。


组件导出使用 `lux_component_exports_v2`，只描述当前 Schema，不提供格式转换声明。
描述生成器只写出当前版本 4 清单；加载器拒绝其他版本和旧 migrations 字段。
场景内容的 Schema 身份或版本不匹配时直接拒绝，不升级旧资产，也不注册旧名称别名。
旧 SceneRegistrations 装配及插件产品路径仍待迁移；这些格式约束不代表插件产品已通过验证。
