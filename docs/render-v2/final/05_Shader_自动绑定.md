# 05 — Shader/PassParams 单一作者契约、自动 Descriptor Layout 和材质编译

## 5.1 唯一事实来源的具体含义

作者不写 `set=`, `binding=`，也不为同一 sampled/storage buffer/texture 再手写一份 Graph 使用列表和 `vkUpdateDescriptorSets`。**一份 C++ `LUX_PASS_PARAMS` 定义描述字段的逻辑身份、资源角色、所有权频率和访问范围；同源生成 C++ PassSchema、GLSL/.lglslh 资源声明和 binding metadata；SPIR-V reflection 用于校验实际 shader。** LayoutPlan 在冷态确定真正的物理 set/binding；native execution recipe 预绑定数值索引与资源 backing。

旧链路已存在：`LUX_PASS_PARAMS/LUX_RESOURCE`, `pass_params_hpp.template`, `pass_params_glslh.template`, `engine/toolchain/shader/lglsl/LglslEmitter.cpp`, `LayoutContract.hpp`, `ShaderInfo.hpp`, `SpirvPatcher`, `LayoutPlan`, `EngineSetShapes`。必须复用并升级既有 `lux-cxx meta generator`/Shader emitter，而不是引入第二份反射/Parser。在新设计中，**共享 Descriptor Set 的完整 shape 来自其资源 owner，不是某一 Shader 实际使用的子集**。

## 5.2 最终作者字段/类型（完整需求，而非 v0 模板）

```cpp
struct LUX_PASS_PARAMS() DeferredLightingParams {
    LUX_RESOURCE(role=sampled_read, semantic="GBuffer.Albedo")
    SampledTexture albedo;
    LUX_RESOURCE(role=sampled_read) SampledTexture normal;
    LUX_RESOURCE(role=read_only_storage, scope=scene)
    StorageBuffer<LightGpu> lights;
    LUX_RESOURCE(role=storage_write, range=all)
    StorageTexture hdr_output;
    LUX_RESOURCE(role=sampler, for=albedo)
    SamplerHandle linear_sampler;
    float exposure{1.0f};
};
```

具体宏拼写须在 F2 通过 existing generator 的 grammar/golden 固定，不准发明 C++ 不支持的成员属性。字段支持：

| 类型族 | 字段信息 | Graph access | Shader binding |
| --- | --- | --- | --- |
| `SampledTexture`, sampled image array, cubemap | format/dimension/sample/access image range, optional sampler pairing | READ aspect/mip/layer | SRV/sampled image + independent/combined sampler |
| `StorageTexture` | format/access range, read/write/readwrite, stage | READ/WRITE/UAV | image storage descriptor |
| `UniformBuffer<T>` | block size/array/alignment, update frequency | READ buffer range | UBO + dynamic offset if legal |
| `StorageBuffer<T>` | stride, count, offset range, read/write/access | buffer RAW/WAW/WAR | SSBO/storage texel etc |
| Indirect/index/vertex | typed buffer roles, offset/stride | READ with stage-specific usage | some read dependencies do not come from shader reflection |
| Color/depth/stencil/resolve attachment | typed attachment declarations, range, load/store/clear | implicit read on LOAD; write/resolve | not necessarily shader descriptor |
| Sampler | independent semantic handle, sampler flags/filter, paired resource | no Graph content dependency alone | `VkSampler` resolved by cold descriptor recipe |
| Push constants / scalars | generated strict offsets/stage/size | no graph resource access | static value per-frame or per-draw |
| Nested structs/arrays | recursive compile-time field path/count/size/stride | recursively derive fields | generated shader declarations, strict reflection |
| Optional shader resource | required or fall-back typed input with default | compile-time selection/condition proof | layout variant/known placeholder, never silent null |

不允许把 `read_write` 简化成“任意同一 subresource 的读写都合法”；feedback-loop/local read 要显式 device capability 和 Vulkan barrier/attachment semantics，必要时编译失败并提供显式两 pass fallback。

## 5.3 生成链的准确数据流（只使用一套 meta）

```text
LUX_PASS_PARAMS Author Header
   │ existing lux-cxx meta_generator, emitted source metadata
   ├─ GeneratedPassSchema<T> / C++ typed binding recipe
   ├─ generated .lglslh (resource declarations + PC/UBO/SSBO layout)
   ├─ GraphResourceUse metadata (field roles/ranges/scope/version)
   ├─ ABI/field-layout/checksum/version metadata
   └─ Diagnostic name/source location tables (cold only)
                         ▼
              .lglsl emitter / shaderc
                         ▼
                       SPIR-V
                         ▼
                SPIR-V reflection
                         ▼
           Cold contract reconciliation
                         ▼
     LayoutPlan (final set/binding/shape/owner)
                         ▼
           SPIR-V relocation/validation
                         ▼
             Vulkan PipelineLayout/PSO
                         ▼
          Fixed binding recipe per compiled pass
```

合同校验必须对齐 Resource field path/semantic name、Shader data kind/image dimension, access/stage, array count including runtime arrays, struct `offset/stride/matrix major`, descriptor flags/count, push constants sizes, shader specialization/entry point, layout required capabilities。`std::bit_cast`/C++ trivially copyable 不足以自动保证 std140/std430/ScalarBlockLayout；必须以生成静态 offset asserts + reflected SPIR-V + real shader readback 双重验证。对于未声明字段、Shader 多出资源、类型/数量不一致、版本 drift、重定位冲突/重复、大小溢出，**cold compile failure**，绝不静默部分绑定。

## 5.4 三类 Descriptor owner 与全量 shape

1. **Engine/Scene shared**：View/camera uniforms, Mesh/Instance/Light/Material/Texture bindless 等完整 Descriptor Shape 由其**真实持有者**在 Schema registry 中描述。某个 Shader 只访问其中一部分也不得为其造不兼容的 subset layout。`EngineSetShapes` 旧契约需要保留行为并删掉与 field metadata 的重复声明。
2. **Feature shared**：Shadow/Cluster/GPU Cull 等多个 pipeline 共用的一份 Feature Descriptor Shape，由 Feature provider 的公开 typed contract 定义；Scope 和卸载/替换必须明确，不能按物理 `VkDescriptorSetLayout` handle 的偶然数值猜 owner。
3. **Pass-local**：Tonemap/Bloom/SSAO 的 sampled input, storage outputs 等，来自 generated PassSchema + Shader reflection；在固定 Graph/variant 编译时完整确定、按 frame slot 有界预留 set/arena。

`LayoutPlan` 是最终位置**唯一事实**；它持有 `LogicalResourceKey→Physical{set,binding,offset,count,stages,flags,owner}`，按更新频率/所有权/设备限制规划。Stage/Descriptor Count 应统计布局合并后的最坏消耗，而不是单独 Shader 子集。移动端 `maxBoundDescriptorSets` 常见严格限制，必须查询设备的**实际值**、各类型 per-stage descriptor limits、update-after-bind limits、dynamic offsets、variable descriptor count、alignment。编译器须进行真实 budget feasibility：超出限制必须给出可读诊断或选择有证明的预定义 fallback 变体，不能无声丢掉 binding。

建议初始 logical frequency domain 为 `GLOBAL / SCENE / VIEW / MATERIAL / FEATURE / PASS_LOCAL / BINDLESS`，最终并非一域一个固定 Vulkan Set；LayoutPlan 可合并兼容域。一个 domain 不是另外一个共享 owner。**不同 Feature 私有 set 即使都曾使用 set=1 也不应相互冲突**，最终 ID 包含实际 owner scope/Shader interface，而不单靠数字。

## 5.5 SPIR-V 重新定位与一致性证明

旧 `SpirvPatcher` 已使用两 pass 扫描 `OpDecorate DescriptorSet/Binding`；这是有效的冷态算法，必须保留字级 relocation 不改变 SPIR-V 指令长度的机制。重点升级：

- 以 **pre-patch** 位置信息匹配且仅修改完整合法的 descriptor pair，避免 `set`/`binding` decorations 扫描顺序引发错误；重复 decoration、类型失配、未识别变量必须明确失败。
- 补强 `SPIR-V 1.x` 版本、decorations/grouped/alias 对于所支持 compiler 输出的约束及验证；现有算法无法覆盖的合法输入应清晰限制受支持的 cooked shader format，而不是对任意外来 SPIR-V 伪报可靠。
- 完成后重新反射或运行二进制验证，确保所有 descriptor 实际位置和 LayoutPlan 一致；记录 shader source/canonical and patched binary hashes，防止缓存 key 错配。
- 候选 Shader/PipelineLayout/DescriptorSetLayout/PSO 以**同一事务**创建；任一失败保留 last-good pipeline/graph，等待 native fence 完成再退休旧 owners。热路径没有 patch/reflect/allocate layout。

## 5.6 Material 与 Shader 变体

`ShaderAsset/ShaderInfo` 是 neutral/cooked 资源类型；`engine/toolchain/shader` 负责离线编译/反射/代码生成；`render_vulkan` 只处理经过验证且能明确 cold-fail 的装载/本地变体/PSO 编译，不可把 runtime shaderc 放进 draw。编辑器 shader/material 热更新属于冷态事务；失败继续 last-good 或向目标显示明确错误，不用临时替代 Shader 默默继续。

Material 支持现有 Graph Material 的 upload/modify/retire、Unlit/PBR/Stylized、metal-rough、alpha mask、透明、材质层/纹理绑定、Shader variants/vertex layouts，并为后续 Substrate-like 复杂 BSDF 预留**具体编译/ABI能表达的 typed material interface**。没有实际 Shader/Lighting/GPU oracle 不能标称 advanced material 已实现。

## 5.7 Push constants 和 layout

不固定把每个 Pass 的 `[0,8)` 永久当 `scene_index/view_index`，也不默认为 128B 都可随意写。完整规则：生成器根据真实 PC bytes/stage visibility/device maxPushConstantsSize 编译时决定哪些字段使用 PushConstant，哪些需要 UBO/storage 或 split；与 View/Scene 通用前缀合并必须是一份 layout 真相。Scalar struct 的 `offsetof/sizeof/alignof` 与 SPIR-V layout 必须可验证，未支持矩阵/向量布局不允许默默由 memcpy 实现。

## 5.8 C++20 工具链使用约束

C++20 没有通用标准静态反射，因此沿用已有 meta generator 并生成 `PassSchema<T>` Traits；用 `concept GraphPassParameters<T>` 在编译期拒绝未标注/字段类型不安全的 Params。生成器位于 cold/build time，插件作者只 include **安装后的 public generated headers**，不应在客户端链接引擎 private codegen runtime。保持 ExternalConsumer 的跨 DLL binary contract（具体见第 09 章）。

## 5.9 Golden fixtures 与必须证明的自动绑定能力

1. Tonemap 单 Sampled/单 ColorAttachment/一个 Scalars：不手工 set/binding，C++ ↔ GLSL/SPIR-V ↔ Descriptor/GPU readback 完整相等。
2. Highlight Blur + Composite：多 sampled fields、sampler pairing、declaration order 改变后最终 LayoutPlan 一致；test fallback/compile cache key。
3. Skinning：StorageBuffer READ_WRITE/range、one update per scene pose, cross-frame WAR 正确。
4. HZB：storage image 分 mip、sample previous mip，Shader/Graph subresource 逐个一致。
5. Clustered Lighting：多个 GPU compute 共享 Feature set；完整 shape 不能依据某单独 shader 的 reflection subset 缺字段。
6. Canvas2D/PointCloud：bindless & dynamic textures/vertex pool, GPU-driven indirect；支持真实 Shader variants。
7. 外部 Feature：安装后的独立 consumer+自己的 shader/PassParams、无私有头、ABI mismatch 被拒、卸载安全。
8. 负例：错 field role、错 std430 field offset、重复 binding、不同 Shader 同名不同类型、set budget 超限、SPIR-V remap 未覆盖、代码生成器产物失步、Runtime 错把 Schema revision 当 Shader binary revision。

**必须审查完整生成工件与哈希**；不能因为 generated `declareGraphIO` 存在就跳过真正 descriptor 写入与 GPU shader 采样。
