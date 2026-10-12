# 11 — 安装式 Feature/Data SDK、插件 ABI 与代码保留

## 11.1 插件有两个真实入口

**作者 C++20**：内置/外部 Feature 可以通过 `build(RenderGraphBuilder&,settings)` 声明 Pass，使用已有 `DataSchema<T>`、`PassSchema<P>`、Resource requirement、Target output 和能力合同，不含 Native Vulkan/D3D12 私有头。不实现某个 Hook 的具体类型不生成空回调，动态注册边界冷态生成固定 `FeatureOps`，Graph 执行不会在每帧调用插件功能类。**版本化 C ABI**：插件模块提供 canonical FeatureTypeId/Schema/type descriptors、函数表和 abi/struct_size，Host 验证和深拷贝稳定字段；禁止跨 DLL owning STL、异常跨界、裸 `typeid.hash_code` 身份。

## 11.2 注册、组装、卸载是三个不同阶段

1. **RegisterType**：仅声明 Feature/数据类型和操作，检验 canonical full shape、版本、冲突和 enabled device caps，不分配一份 GPU backing 或强行创建 Graph。
2. **Effect composition**：Effect 使用注册的 Feature types 与配置构建 Graph IR；required/optional resource/fallback/Target/Writer 欲望先 cold resolve。需要的资源由 Root/Scene/View Domain 在执行绑定时提供，不给 Feature 默认拥有一份 GPU Store。
3. **Unload**：首先阻止新 Effect/Graph/消息使用该模块；对引用它的 Effect 先生成合法替换候选并完整发布；quiesce CPU callbacks；Native Graph/Context/Resource codepin 按所有 in-flight GPU tickets、WSI/析构借用安全退出后释放模块。不可简单因 Graph 编译完成就卸载其仍依赖的插件自定义析构/Codec 代码。

## 11.3 注册相容性与插件资源

插件 A 可以只声明 `GaussianChunkGpu` Schema 而不贡献 Pass；插件 B 可以消费该类型，并通过已有 Chunk Buffer 接入自定义 RenderEffect。若已有 `LightGpu/MaterialGpu/TextureStorage` 足够，直接引用公共类型，不复制 `PluginLightGpu`。同名 Type 不同 layout/ABI 必须拒绝或经明确迁移事务升级。插件资源在 Domain 中的逻辑 lifetime 与任何 RenderEffect/Feature 的删除独立；若资源需要插件定义的析构和 allocator，Host 必须 pin DLL 直到该资源真正释放。

## 11.4 GPU 安全边界

第三方只用 PassSchema 声明 Graphics/Compute/Transfer 真实资源访问，Native Compiler 与 Executor 决定具体 Vulkan/D3D12 命令。Plugin 不能直接 `vkCmdPipelineBarrier` 绕过 hazard 或把 `VkDevice*` 借给长期代码。确需 CUDA/RT/Mesh Shader/Interop 时添加设备能力与受限 backend-native extension，插件没有声明该能力不能默默使用。Device capability supported≠enabled；不支持必须 fallback/reject。

## 11.5 Editor 参数配置与共享效果

RenderEffect Editor 只查看 type/params 的安全快照和 revision；编辑一个共享 RenderEffect 参数时，所有使用此 Effect 的 View 采用同一新版本。想对单 View 生效，复制/选择不同 Effect，或使用显式局部 override。结构修改候选失败时 old Graph 保持有效，插件代码不能通过“注册之后的原始对象指针”修改已编译不可变 Graph。

## 11.6 Installed SDK 强制 Golden

两个真正外部进程/插件独立于源码树进行 build/install/run：一个插件读取内置 LightGpu 并增加正确 Compute/Graphics Pass，卸载后其他 Feature 继续使用共享 GPU 资源；另一插件声明 GaussianChunkGpu，并由另一 Feature 读取，真实 Upload/Compute/Readback、ABI 冲突、错误 owner/generation、热更 Effect、GPU in-flight 后安全卸载。检查 SDK 不包含 `pinclude/sinclude` 或 backend 私有 headers，release/debug/ASan 和代码 pin 行为真实有效。未做这些测试不能称“插件 SDK 已实现”。
