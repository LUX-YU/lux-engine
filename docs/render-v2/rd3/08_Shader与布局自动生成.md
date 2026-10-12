# 08 — 统一 DataSchema、PassSchema、Shader 与 Native BindingLayout

## 8.1 一份事实，从 C++ 到 SPIR-V

作者只声明 Shader-visible GPU 数据类型与 Pass 真实访问，由现有 `lux-cxx` 和 Shader toolchain 产生 DataSchema/PassSchema、GLSL 结构/字段、GPU Layout、反射/重定位所需元信息。Renderer 不维护私有的业务 shader registry；Native Compiler C9 负责核对所有 Pass/Shader 资源绑定。CPU struct/可选 Wire bytes/实际 GPU representation 独立进行 ABI 验证。禁止“类型 trivially copyable → 直接 memcpy 当 std430”。

## 8.2 BindingLayout 与 ResourceDomain 是两种身份

旧 `OwnerShape/ShaderOwnerId/OwnerAssignment/EFieldOwner` 的**功能本质**是结构性 Shader Binding 形状与字段归属。目标设计用内部 `BindingLayout` + frozen `BindingKey` 表达各字段 role/stages/descriptorCount/array stride/dimension/format/complete Schema。它们**不表明真实 GPU Buffer 属于 Scene 还是 View**。实际资源归属与可读链来自 ResourceDomain，Native ExecutionContext 冷态选择确切 backing。一个 Shader 从 Scene `lighting.main` 和 `lighting.preview` 读取同一种 `LightGpu` 时，必须保留两个独立 binding semantics；跨 Scene A/B 使用相同 shader/layout/PSO 时可绑定不同实际 Buffer。

## 8.3 两层相容性

1. **结构性**：`PassSchema`, Shader Stage Interface、Data GPU Layout、DescriptorType/Count、PushConstant、Target Attachment Signature、Device enabled features。负责 Shader/PSO/Layout/Graph Cache identity；需 full equality，hash 只是加速。
2. **具体实例**：Domain owner/generation、GpuResourceId、Native device、Buffer range/Image view、ready ticket、实际 states、Capacity 是否足够。只在 Context binding/执行前验证，不进入 PSO 结构键。

一个 Effect 的 View A/B 共享结构性 Graph，但动态数据完全独立。`BindingLayout` 是编译算法的数据，不能为它另建持久 `ShaderOwnerManager`。物理 Descriptor Set 合并/重定位须由 C9 同一 Layout 算法完成；不能一个 Feature 就强制分配一个 Vulkan set，设备预算不足必须明确拒绝或选择合法的被声明变体。

## 8.4 Pipeline/Descriptor 热优化

C9/C11 冷态建立 PipelineLayout、Shader relocation、PSO、Descriptor recipe；热态 Executor 的 Slot 保存已分配的 Descriptor 实例和数字化 binding，Record 维护当前 CommandBuffer Pipeline/Layout 与动态 offsets，跳过冗余绑定。在多视图共享一个 Graph 时不能多编 PSO；资源实例的变化若 Layout 不变仅对 Context 重绑定并遵守旧 descriptor in-flight 不可更新规则。动态 UI/Material variant 可选在低延时条件预热编译，但不能在热态隐式 PSO 创建。

## 8.5 Shader/Device 能力

完整能力由 Device 创建时的 supported/enabled 两部分决定。当前 Vulkan compute/VS/FS/有限 Native draws 已验证的子集与未来 Mesh/Task、AS/RT、3D Image/CUBE、descriptor indexing 等明确区分。Feature 要求未启用设备能力时，只能在 Effect 选择时提供已经描述/验证的 fallback 或 cold reject；不能无设备同意偷偷启用新扩展。OpenGL/D3D12 后端有自己的 Shader 翻译和 binding lowering，IR 本身绝不包含 VkDescriptorSet。

## 8.6 SDK 与插件

第三方 Feature 只 include public installed headers。插件可注册新的 `GaussianChunkGpu` Schema 或读取既有 `LightGpu`，Host 深拷贝可信结构描述及完整 ABI 合同；跨 DLL 不允许 STL owning、RTTI hash 或未 pin 的 `string_view`。卸载：撤销新注册→替换 Effect/Graph→等待所有旧 callback/native graph 的代码/资源使用安全退出→release CodePin。一个插件在 Graph 编译完成后不会被 Editor 的热调度反复回调 Native Vulkan 录制。

## 8.7 F6.b 已知性能问题（不能丢失）

源码已通过多真实 Owner shader GPU oracle，但 `layout_full_shape` cold 构造每操作分配数增加（正式验证报告约 10→24）且 CPU 耗时明显上升；`native_graph_record_submit_256x256` p95 CI 未能证明回退在 +5% 内。新方案应复用 canonical full shape hash 和 full equality，不过要消除可证明的临时字符串/指针数组重复分配，先测再动。不能为了“新的 BindingLayout 名称”再次复制或二次扫描全部字段。历史资格保留在 16/18 与审计页。
