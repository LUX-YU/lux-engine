# 08 — DataSchema + PassSchema + Shader + LayoutPlan 的唯一绑定事实

## 8.1 一份类型声明，一份 Pass 行为声明

- `DataSchema<T>`：稳定数据类型/字段/CPU 存储约束、Schema 兼容性；不声明某个 Pass 的 access。GPU-facing 数据另由同源可选 GpuLayout/GpuRepresentation 描述 Shader ABI，CPU-only 不要求 GPU Layout。
- `PassSchema<P>`：该 Pass 使用哪些 typed resources、访问角色/范围/stage/fallback、Push/UBO 参数以及目标 attachment；与现有 `LUX_PASS_PARAMS/LUX_RESOURCE` 保持兼容。
- `.lglsl` / GLSL emitter：使用同源生成的可选 GPU 表示、布局声明、必要打包转换与函数，不手写第二套 `set/binding`, `struct stride`。
- `LayoutPlan`：按照真实 **DataSchema/Storage full-shape 结构性绑定合同 + pass-local** 决定 Vulkan set/binding/descriptor type/count/push/align，冷态 relocate SPIR-V 后再验证。具体 Scene/View 的 `DataSetId` 只在 Invocation 将该绑定槽实例化为 native backing，不能进入共享 PipelineLayout 的身份。

被锁定的 F1/F3 机制必须复用；禁止新 Parser、按 shader reflection subset 再造共享 Set、脚本热路径生成 shader。

## 8.2 修正原三槽 `OwnerAssignment`

F3 当前 `OwnerAssignment {scene, feature, pass_local}` 每类只有一个 `ShaderOwnerId`，不足以表达同一个 Shader 同时读取 Light / Material / Shadow 三个独立数据绑定合同。RD2 **明确分开两种身份**：

1. `BindingSlotKey/ShaderOwnerId`：冷态结构性合同 `{namespace, TypeSchemaLayout, resource semantic/key, scope class, full shape}`，供 Shader Layout/PSO 缓存，两个不同 Scene/View 只要结构兼容即可共享同一 Plan。
2. `DataSetId/NativeBackingGeneration`：运行时具体数据实例及 Vulkan allocation，在 FrameInvocation `Bindings` 中绑定到相应结构槽，**不纳入共享 Shader/PipelineLayout 的缓存身份**；需要尺寸/资源规格不同才分叉 Native ResourcePlan。

`scene/feature/pass_local` 只是类别/频率提示，不能代替两个真实不同的绑定槽。两个 Feature 读同一个 Dataset 可以共用真实 backing，而相同 Type 的 `main` 与 `preview` 可以占两个明确不同的逻辑绑定槽。

目标冷态 `BindingSlotKey`：`{BindingNamespace, TypeSchemaLayoutFingerprint, ResourceSemantic, ScopeClass, AccessRole, StageVisibility, ArrayShape}`；它**不是** `{DataSetId}`。Cold Composition 将 Pass Field 按 Type/Key 解析到这些独立槽，Frame Invocation 将槽映射到当前真实 `DataSetId` 与 backing。物理 Set 的数目必须由 LayoutPlan 按设备限制、Owner full shape 和更新频率合并，不准每个 DataSet 实例强制一个 Vulkan Set。完整 Owner Shape 来自稳定 DataSchema/Storage contract，不因某个 Shader 使用字段子集就裁剪成不兼容 set。

例子：一个 Shader 的 `LightGpu`、`MaterialGpu` 都可来自 Scene DataSet，不能因为二者都是 `scope=feature` 就重叠绑定；F6 内以两个独立的共享 Owner、相同 role、不同 canonical Data Schema 的真实 GPU oracle 验收。

## 8.3 不同数据访问形式

| 作者数据字段 | Graph role | Native descriptor / command | 访问条件 |
|---|---|---|---|
| `StorageBuffer<T>` | shader read/write + exact byte/stride | SSBO / Storage buffer；可用 StoragePlan | T GPU ABI、read/write stage, offset |
| `UniformBuffer<T>` | uniform read | UBO/dynamic offset 或受限 Push fallback | std140/ABI/alignment/limit |
| `SampledTexture` | sampled read | ImageView + Sampler | format/dimension/usage/layout/state |
| `StorageTexture` | storage read/write | storage Image | storage format/capability/actual layout |
| Vertex/Index/Indirect | explicit `VERTEX/INDEX/INDIRECT` accesses | prebound geometry/Draw Indirect ops | shader/graph roles + GPU sync |
| Attachment/Resolve/Depth | output/write/load/store/resolve | native rendering recipes | format/sample/depth/stencil/local-read |
| external GPU resource | import/ready/last-use | borrowed Buffer/Image/View with real Native backing receipt | must not outlive owner's actual allocated memory |

Native future `AccelerationStructure`/Sparse Image 是新的 Vulkan Primitive（必要时扩大 Graph primitive/PassSchema roles），不能单纯把它们冒充 `StorageBuffer<T>`。CPU 类型开放不意味着所有 Native resource classes 都不再变化。

## 8.4 编译/缓存/错误

`ShaderVariantIdentity` 包含 asset/schema bytes/stage/pipeline identity 与 device enablement；`LayoutIdentity` 包含完整 Owner full shape、实际 descriptor sets、shader fields 和 push ranges；`NativeProgramIdentity` 在此基础上加入 pass, device, driver, attachment/sample state。保持 full equality 或 hash+exact compare，缓存冷态 safe publish/last-good 回退。凡未知 GPU struct fields、std430/std140/offset/stride 不符、两个 schema 同名不兼容、Owner 冲突、Descriptor count 设备不支持、变量数量/Update-after-bind 未启用，都要在 cold build/compile 拒绝，不允许 silent dummy descriptor 或 black output。

支持更多 Shader Stage（Mesh/Task/Ray/Geometry/Tessellation）属于以后真正需求驱动的扩展：只扩展工具链 Stage Meta/Capability 和 Native Pipeline，不向每个普通 Feature 加一套 IShaderStage/RenderPath。现有 F3 只支持 VS/FS/CS 且 Descriptor Indexing 默认未启用，不能把**硬件支持**当作**当前设备已启用**。

## 8.5 支持插件的数据声明

外部插件若使用已有 `LightGpu`，由已安装的 DataSchema 直接 `require<LightGpu>(key)`；若是新 `GaussianChunkGpu`，只需插件自带源类型/Schema，Host 对 Schema value 做深拷贝和 ABI/code-pin 验证，然后通过同一 DataSet/Graph/Shader Pipeline 组合。跨 DLL 不能传 `std::vector<T>`、C++ 异常或原生 VMA pointers 作为不受控 ABI。
