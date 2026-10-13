# 08 — Shader ABI、数据类型与直接 Native Binding

## 8.1 统一的描述来源

沿用现有 Schema/Generated PassParams、Shader reflection/SPIR-V patch、F6.a/b full structural OwnerShape 算法；不写第二个 parser 或手工固定 `SCENE/FEATURE/PASS` 三个 Descriptor Set。`DataSchema<T>` 描述长期中立数据类型，`PassSchema<P>` 描述某一 Pass 的真实 GPU 读写，`ResourceRequirement` 描述共享与存储范围，`NativeLayout` 根据全部使用需求确定 Descriptor/Pipeline Layout。CPU/Wire/GPU representations 可不同，由既有 meta/toolchain 显式生成、验证并版本化。

结构代码 Identity 包含 Shader bytecode/entrypoint/ABI、full OwnerShape、pass layout、GPU enabled caps、Target compatibility；运行时 SceneId、ViewRef、Effect hot params、ResourceLease/vk handles 不在代码 Identity 中。真实 Bindings 在 attach 冷态解析成 indexed recipe，更新数据时只在 backing/descriptor revision 改变时 patch。若 Native descriptor compatibility 不符应候选失败；不能通过魔法 `first registered light` 解决。

## 8.2 Shader 资源与复杂业务

Mesh/Material/Texture/Light/PointCloud 等 Storage 允许持不同底层 native Buffer/Image/Descriptor 实例，但对 Graph Builder 统一提供 `ResourceBindingDesc`（Buffer/Image/Subresource/ImageView/Access/ABI/Ready）；专业 Storage 的内部 `updateTransform`, `uploadChunk`, `replaceMip` 等仍应按领域独立，不强制为每种业务提供同一个上传虚函数。

图形 Native 实现用 Vulkan SPIR-V；未来 D3D12 从中立 Shader variant/Schema 生成并验证 DXIL/Root Signature/PSO。不能把 SPIR-V 字节直接当 DXIL，或要求 Engine/Projection 直接调用 `vkCmd*`。

## 8.3 优化与负例

固定 topology record/submit 预构建 Pipeline、Descriptor layout、Native bindings；同一 CommandBuffer 上只有完整 bind state 相等才能跳过 descriptor/PSO bind。Shader resource subrange、Queue family、layout、ready barrier 仍经唯一 Native compiler/Executor 的真实同步状态证明。编译期 Scope 合并不得错误地把两个 Scene 的相同 Type Buffer 合为同一个 VkBuffer；仅共用实际 Global ResourceKey 才允许 GPU backing 共享。

## 8.4 插件合同

插件提供的 Schema canonical name + version + FieldABI/Hash 必须由 Host 复制；不能留下一份指向插件二进制临时内存的 span/string_view。Native callback/Shader specialization/destructor 使用第三方代码时，CodePin 覆盖直到最后 CPU/GPU use。Unregistered plugin type 的旧 ABI 引用不得复活。Installed SDK 的用户代码不得 include Vulkan private headers 才能描述 Shader/Pass/ResourcePlan。


## 8.5 MaterialGraph 输出与 Native Shader 身份（规范索引：第 30 章）

现有 `modules/function/material/**` 已有注册 Node/Pin/Payload+ShaderIR，`engine/toolchain/material/**` 已有 ShaderIR→GLSL→SPIR-V 与 Cooker，而 Legacy `GraphMaterialData`/`MaterialResources` 已有 GPU param/texture + graph variant bucket。**必须将这三层贯通，不得将 MaterialGraph 直接当 Effect 的 RenderGraph，也不得为不同材质参数重复编译同一个 PSO**。材质 Node 插件与 RenderFeature 插件有不同 code pin/ABI 作用，Node Type 与 Render Runtime ID 不混同。Shader 结构修改需重编候选，参数值更改只更新 GPU records；Alpha/DoubleSided、shader code、Owner/Layout 为 exact Native compatibility。Shader toolchain 的 Vulkan-only SPIR-V 编译路径尚非 DXIL；未实现功能见第 30 章。
