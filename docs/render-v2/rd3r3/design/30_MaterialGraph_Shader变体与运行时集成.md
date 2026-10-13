# 30 — 材质图（MaterialGraph）不是 RenderGraph：作者、Cooker、Shader 与 GPU Material Storage 的完整合同

> **核对事实**：材料节点图核心主要位于当前（非 `render_legacy` 冻结副本）`modules/function/material/**`，Shader 编译与 Asset Cook 位于 `engine/toolchain/material/**`；对应旧 Render 产品的 GPU backing/业务接入位于 `render_legacy/modules/function/render/{features,vulkan}/**`。三个层次当前均存在，但 **P2-A RenderBackend 尚未直接打通新的 RenderEffect+MaterialGraph 全流程**。不能把存在库/测试误称为新 Render V2 产品已集成。

## 30.1 已有来源与不可重复造轮子

| 现有源码 | 已完成的职责/边界 |
|---|---|
| `modules/function/material/graph/MaterialGraph.hpp` | `MaterialGraph` 作者模型：GraphTopology 与 Node/Pin/Payload、texture/param slots、render state、shading model；独立于 Vulkan |
| `MaterialNodeCatalog.hpp`/`MaterialNodePayload.hpp` | 可扩展 immutable Node 类型、canonical name/version/type token、typed callbacks、plugin `CodeLease`、Pin schemas、batch 注册失败不发布 |
| `MaterialGraph.cpp`/`MaterialSource.cpp` | 编辑事务/clone/serialize、名称与版本验证；断开节点/缺 Surface 允许存盘，编译时明确报错；Source 是持久资产而非 GPU runtime |
| `MaterialLowering.cpp`/`ShaderIR.hpp`/`MaterialIR.hpp` | 检查输出节点/循环/Pin type/input 需求并 lowering 为带 index 的 ShaderIR（SSA-like），计算 full structural fingerprint，render-state 加入 compile identity |
| `engine/toolchain/material/MaterialCompiler.cpp` | `lowerMaterial()` → `compileToSpirv` for GBUFFER & FORWARD → `rdesc::MaterialDescription`（参数槽、纹理槽、Alpha/DoubleSided 等） |
| `ShaderEmitter.cpp`/`ShaderCompiler.cpp` | ShaderIR → GLSL → shaderc SPIR-V；复用内嵌 canonical GLSL includes，真实 SPIR-V reflection（不是 hand-written layout） |
| `MaterialCooker.cpp`/`MaterialToGraph.cpp` | MaterialGraph → cooked `MaterialAsset`，Imported PBR Material → 通用材质节点图；同构参数以 GPU Material SSBO 值表达以共享 PSO |
| Legacy `GraphMaterialData.hpp`、`MaterialResources.cpp` | Param/Texture table → `GraphFamilyGPU`、TextureSlot→实际 bindless index、SlicedSSBO、MaterialHandle、动态材质 graph family |
| Legacy `VariantBucketManager.cpp` | exact graph bucket key（gbuffer & forward Shader handle+generation+Alpha/DoubleSide）、引用计数、free-list、GPU batch/bucket 再利用 |

**重要区分**：MaterialGraph 是生成 Shader/材质参数的**作者图**；RenderGraph 是组织 GPU Pass/Buffer/Image/Queue 的**执行图**。它们不能合并为一套 Node Registry，也不能让用户的每个材质节点默认生成一个 RenderGraph Pass。材质 Graph 与效果 Graph 通过 `ShaderVariantIdentity + MaterialBindingSchema + MaterialRecord/Handle` 连接。

## 30.2 三种材料变化，三种不同事务

1. **参数值变更**（BaseColor/roughness、材质 ParameterSlot 默认值或实例参数）：保持 Shader/PSO/Effect/RenderGraph 结构不变，只更新 Material Storage 对应记录的 Dirty range、revision/Ready。避免把每一个不同颜色材质编成独立 Fragment Shader。
2. **材质拓扑/Shader 源码变更**（新增节点、修改结构或变更 shading model）：资产编译生成 ShaderIR→SPIR-V/未来 DXIL 候选；全结构校验后对受影响 Native Shader Program/PSO/Graph 提供新版本，原有 Surface/Effect/Target 继续使用 last-good 直到候选可用。失败不能部分替换旧 PSO。
3. **Pipeline render-state/Material layout 变更**（AlphaMode、DoubleSided、TextureSlot/ParamSlot Schema）：属于 Pipeline/OwnerShape/record compatibility 身份的一部分；只要兼容性改变必须重新验证/生成相应 Native Code，不能只传新参数。

`MaterialGraph` / `MaterialSource` 的 NodeId/PinId/NodeTypeId 属于既有 Graph/Material 子系统身份，**不可把它们拿来当 RenderEffectId/RenderGraphId**。当前 MaterialGraph 的内部 unordered containers 是节点编辑存储，既不分配 Render V2 Runtime Object IDs，也不要求为统一 Runtime 身份规则大规模修改既有 Material 资产实现。

## 30.3 正确的数据/编译流

```text
Material Editor / imported asset / plugin material nodes
  → MaterialSource / GraphTopology / Nodes/Pins/CodeLease
  → MaterialGraph semantics & validation
  → MaterialIR + ShaderIR + material parameter & texture slots
  → Toolchain shader backend (GLSL/SPIR-V today; DXIL later)
  → ShaderVariant + reflection + full Material Binding/Layout Identity
  → cooked immutable MaterialAsset / revision
  → neutral Render client Asset/UPLOAD request (typed schema, asset identity)
  → Native backend Shader/PSO candidate + Global Material/Texture Storage
  → Effect-owned logical Graph uses the Material contract (not MaterialGraph itself)
  → attach ResourcePlan resolves Global/Scene material tables and per-view execution bindings
  → Executor uses material bucket/index/PSO and live GPU-ready tickets
```

Material assets/cooked shaders 应当跨 Scene/Effect 被引用；一个材质实例不应因为 8 个 View 使用相同材质就复制 8 份 Shader/Pipeline。Material CPU 作者图可在 Editor 与 Shader Toolchain 使用，但 Engine ECS 与 RenderRuntime Client 不得 include Vulkan/D3D12 类型；资产数据采用有效版本 Wire/Owned payload，不任意原样 memcpy STL/Payload/CodeLease。

## 30.4 Plugin、Node 与 Feature 的两个不同扩展点

- **Material Node 插件**：加入 MaterialNodeCatalog，声明 canonical type/version、PinSchema、Payload Codec/Validator/Compile callback；Host retain CodeLease 直到编译候选与回调不再引用代码。
- **RenderFeature 插件**：声明 GPU Pass 和 Material 数据需求，可消费 `MaterialAsset`/`MaterialRecord`/`MaterialShaderVariant`，不自动拥有 MaterialGraph Editor 或导入器。
- **Engine/ECS Projection**：将引擎材质组件/资源引用映射到中立 MaterialRecord/AssetId/Version 数据；编辑器材质图与材质实例编辑属于不同 Authoring Producer。

示例：GraphMaterial 的 Shader 经过 Toolchain 生成，在 Forward/GBuffer Feature 的 PassSchema 中被引用；Shadow/Depth/Visibility Pass 只有在支持合法专用材质变体时才参与，不能静默用不正确的 GBuffer Shader 替代。

## 30.5 当前完成度与不能夸大的能力

当前 `engine/toolchain/material/src/ShaderCompiler.cpp` 通过 shaderc 生成 Vulkan SPIR-V；没有 D3D12 DXIL Backend 实现。源码明确拒绝 Shadow/VisBuffer Material Pass；`RAW_EXPR` 仍显式 unsupported。ShaderEmitter 文件顶部的一些旧注释比实际 case 实现落后，**必须以代码分支/真实测试为准**：SampleTexture/Param/TBN 与 PBR Forward 代码路径已经存在，不能只凭顶部的“尚未实现”判掉。

Legacy `GraphMaterialData` 固定 `kMaxParams=16`、`kMaxTextures=8`，以及旧的 `set 2 / set 4` 是历史 Shader ABI，不应未经验证直接冻结成新统一 D3D12/Vulkan ABI。必须保留分离 Parameter/Texture binding、Stable Slot 与 Variant Bucket 复用机制；新 Layout 交给 Shader Schema/OwnerShape 和实际 Native Device caps 选择。材料资产格式/存档的历史版本兼容不可默默更改。

## 30.6 需要的端到端验收

- 对同一 MaterialGraph，两材质实例仅参数不同：**一份 Native Shader Variant/PSO**，两个独立 Material Records，输出像素不同。
- Graph 节点增删、循环、错误 Pin、多个 OutputSurface：编辑事务保存/编译错误行为与源合同一致，旧 cooked material/旧 PSO 仍有效。
- Material 的 AlphaMode/DoubleSided 或 Shader 代码变化：产生相容的新 Pipeline identity，且旧在途 draw 正确退休。
- Texture asset 未 ready、Mip replacement、Bindless slot Generation 复用：无错误纹理采样、无提前覆盖 in-flight Descriptor；Material 管线复用不偷换 Texture backing。
- RenderEffect 中至少一次 GBuffer + Forward/Depth Shader Variants 与 MaterialGraph 编译输出的实际 ABI 联通，真实 GPU Validation/像素校验。
- 插件 Node 卸载：Payload/NodeType/Callback/CodeLease 被清晰持有到最后使用；RenderEffect 本身无需知道 Engine ECS。

> 本章在 `appendix/MaterialGraphContracts.csv` 逐条列出真实 Source、目标 Owner、R3/L1/H5 阶段和验证；不能把 Node Graph 扩展功能只放在 H5。
