# 项目插件

`project_plugins` 是共享静态库，包含插件目录、依赖解析及 Runtime 动态库装载，不依赖 Render、Editor、Pane、ImGui 或工具反射。
它位于宿主组合边界；低层组件/系统不反向依赖它。

- `PluginCatalog` 读取不可变描述并建立能力与分区方案索引，记录安装根与描述来源，不打开 DLL。
- `PluginLibrary` 校验二进制身份及 Runtime 导出，保留依赖代码；不打开声明中的 Editor 库。
- `PluginManager::create(catalog, selection)` 先验证完整选择及依赖，再加载一次固定模块集合。
  缺失、重复、版本不匹配或依赖环返回结构化失败，不按搜索顺序替用户选实现。
- `loadPluginLibrary()` 复用单个二进制的路径/ABI/声明校验；具体层解释自己的导出表。
  Editor 侧通过这一边界打开工具扩展，共享层不认识 EditorPluginExports。
- `project_plugin_rendering` 是可选 STATIC 接纳层，解释 RenderFeature/Scene binding 导出。
  通用库加载成功不表示图形导出已被核验；图形产品必须采用该接纳层。

系统类内 SupportedWorldTypes 进入插件描述 v2；只允许精确分区名称或单独的 `*`。
2D/3D 内容预设不改变分区名称。PluginLibrary 核对二进制登记与目录声明一致性。
内置 lux.builtin.runtime v2 不依赖图形；lux.builtin.scene_render v1 依赖 runtime v2 和 render v1。

模块保留的注册对象仍持有代码引用，因此任务和 GPU 消费者完成关闭前不会卸载被调用的函数。
目录和模块引用是不同事实，不能用纯数据目录替代代码寿命。

## 安装

| 路径 | 内容 |
| --- | --- |
| `share/lux-engine/plugins/catalog.json` | 已安装插件描述集合；格式仍是 lux.engine.capabilities v1 |
| `share/lux-engine/plugins/<module>.json` | v2 单个插件描述（与 catalog 根版本独立） |
| `share/lux-engine/plugins/schema/plugin.schema.json` | 插件描述 schema |
| `share/lux-engine/plugins/LuxPlugins.cmake` | 外部插件身份生成与安装接点 |
| `include/lux/engine/project/` | 共享公开接口 |

CMake 消费者使用 `find_package(lux-engine-project-plugins REQUIRED COMPONENTS project_plugins)`，
链接 `lux::engine::project::project_plugins`。不需要 editor_metadata。

产品根据项目选择传入 MetadataIdentity 数组，选择之外的目录条目不会自动加载。
目录可来自安装根，也可来自明确的项目描述位置。严格核对模块身份、版本、SDK ABI、构建身份及声明摘要；
本接口不承诺不同编译环境间的 C++ ABI 兼容。

外部插件使用 `lux_add_plugin_exports(TARGET ... DESCRIPTION ... INPUTS ...)`，INPUTS 覆盖影响实现的源码和生成产物。
可用 `lux_generate_capabilities(BASE <sdk>/share/lux-engine/plugins/catalog.json)` 合并 SDK 目录。
实际 Vulkan 插件示例安装到 `share/lux-engine/examples/external-feature`，只链接公开 Runtime SDK。

编辑器的 `.luxproject` 格式是 version 2，启用项保存身份、版本和可选项目相对描述路径；空 plugins 是明确的空选择。
它属于 editor/project 的项目描述；带发布事务的 ProjectStorage 留在 editor/storage，不迁入游戏层。
