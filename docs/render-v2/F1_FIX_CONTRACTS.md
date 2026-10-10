# F1-FIX — 作者合同、类型责任与来源

本文件记录修复后的实际 API 与本阶段边界，不改写 FINAL 或 F1 历史报告。

## 格式唯一权威

`TextureDesc::format` 直接使用 `lux::rdesc::ETextureFormat`（Description 的中立 `Image.hpp`）。
删除 Graph 自有枚举，不保留别名，不改变既有格式数值或 ABI。
`textureFormatClass` / `textureAspectMask` 使用显式 case 区分 Color、Depth、Depth-Stencil；
`supportsTextureUsage` 排除 sRGB/compressed/depth storage、compressed color attachment 等非法组合。
这是逻辑资格，不能替代 F3/F4 的真实设备 format feature、tiling、sample count 查询。
生成 StorageImage 的 `rgba16f` 必须与 `RGBA16_SFLOAT` 一致；原 r32f/rgba32f/rgba8 保留。
Graph 不链接 Description 聚合、Math、Script、Vulkan 或 Shader Toolchain。

## Buffer 与纹理视图

- `BufferRange{offset, kRemainingBytes}` 表示从 offset 到声明 backing 末尾；默认即此语义。
  `finish()` 通过减法边界检查解析为有限非零 byte_count，不把 sentinel 传给后续编译阶段。
- `UniformBuffer<T>` 默认 `{0, sizeof(T)}`。生成 IR 提供 T 的 size/alignment；
  UBO 起点满足至少 16 字节的 std140 对齐，范围至少覆盖 T；SPIR-V 对账证明 T 的真实 block layout。
  显式 `{0,1}` 不能代表一个 64 字节 UBO。Descriptor 数组的每个元素独立捕获和验证。
- `StorageBuffer<T>` 默认完整 backing，解析后 count 必须为 T stride 的整倍数、至少一个元素，
  offset 必须符合 T 的对齐。合法 element_count = byte_count / element_stride；runtime array 的
  访问上界就是这个有限 view。Shader 作者仍有义务只访问此上界；本阶段没有 GPU 执行或 robust access 保证。
- Graph CPU 布局对齐不冒充 native device 的 minUniform/StorageBufferOffsetAlignment；后者由 F3/F4 验证。
- `SampledTexture` 默认从 mip/layer 0 覆盖全部剩余范围。显式范围定义真正的 native image view；
  Shader LOD 和 layer 索引相对该 view，未来 native binding 必须使用完全相同的 view，不可偷偷绑定更大范围。
  默认不能只登记 mip 0 而允许隐式 LOD 读取全部 mip。维度、array/multisample、aspect 同时验证。
- Storage/Attachment/Transfer image 默认单 mip/layer；调用方可显式给出合法范围。
  `GraphResourceUse` 与 `GraphFieldBinding` 在 finish 后拥有同一份有限范围值。
  `field_index` 只连接当前 Pass 中同源生成的字段快照，无字符串热查找。
- 完整 subresource scheduling 仍在 F2。旧 whole-resource oracle 保留；不能对部分资源的图谎称完成调度。

## 多 Stage Shader 合同

生产 lux-cxx Meta IR → 原 PassSchema projection → Python IR lowering（不解析 C++）保留。
`PassShaderContract::stage_declarations[vertex,fragment,compute]` 从同一份字段与 Stage mask 生成。
通用类型声明可共享，资源和 push fields 仅出现在所属 Stage；共享资源使用整个 Contract 的声明序槽位，
因此 Stage 内允许 binding 空洞，但共享字段的槽位绝不因局部过滤而漂移。
这些是 F1 冷编译 provisional slots，最终 LayoutPlan/relocation 仍属于 F3。

`emitPassGlsl` 保留输入 `#version`（无则补 450），将既有开头注释、extension、include、define 放在
生成声明之前，再接 Shader body；重复/晚到 version 明确失败。预处理仍交给 glslc，未新增预处理器。
实际 `LegacyTonemap.vert.lglsl` 逐字来自冻结 V1 tonemap.vert；新 Stages Shader 另覆盖 extension/include 顺序。
任何 include 私自引入额外资源仍必须通过真实 SPIR-V validator，不能成为手写 binding 逃生口。

`validatePassSpirv` 检查一个真实模块的 Stage 子集；`validatePassShaders` 要求 Vertex+Fragment 或 Compute
完整程序，拒绝重复/缺少/错误 Stage 和无法由该程序覆盖的字段。共享资源各自对同一 Schema 反射校验。
不能把单模块通过等价为完整 Graphics Pass 通过。Shader interface 的跨 Stage location link/native PSO 属于 F3。

## 冷态稳定身份

- `passKey(name)`、`graphResourceKey(name)` 复用已有 `cxx::Fnv1a64` + `StrongId`。
- Builder `addPass(name, ShaderReference{asset, variant}, kind, scope, params)` 拷贝 name 和 Shader canonical name；
  不再提供手工数字 PassKey/ShaderKey 的作者入口。空 Pass name 或缺失 Shader 身份拒绝。
- Shader canonical name 为精确 UTF-8 `asset#variant`；两项非空，均禁止 `#`。不会执行路径/大小写归一化。
  这是稳定逻辑 Asset/Variant 引用，不是 Shader 字节码版本、pipeline 或 native cache key。
  F3 必须把实际 Shader content/layout/device 等纳入 native plan 身份。
- `texture(desc, name)` / `buffer(desc, name)` 支持有名临时资源；匿名 transient 仅在该 Definition 本地有效。
  import 入口强制 `importTexture(name, desc, persistent_scope)` / `importBuffer(...)`，必须非空且有 scope。
  Definition 保留 name、semantic hash、Origin、Scope 和 descriptor，为未来 ImportContract 接口提供作者身份。
- 重复 semantic/hash 的资源或 Pass 拒绝，绝不因不同 Feature 同名而暗中共享。Definition 比较保留完整名称，
  不把 hash 相等当作唯一真相；无全局名字注册表。Provider 解析、readiness、Export 与 native import proof 仍未实施。

## 新增/扩展类型责任清单

| 类型/事实 | Owner / 存活期 | 责任与禁止事项 |
|---|---|---|
| rdesc::ETextureFormatClass / ETextureUsage | 中立值 | 格式分类与逻辑使用资格，不拥有设备/资源 |
| GraphResourceKey | Definition 的作者语义值 | 区分本地 GraphTexture/Buffer slot 与稳定名称，不是 Runtime handle |
| ShaderReference | addPass 调用期间借用字符串 | 指明 asset+variant，Builder 复制 canonical name，不保留输入指针 |
| GraphResource 名称与 semantic | Definition 自有 | 提供组合时冷态冲突证据，不解析 Provider |
| GraphPass 名称与 shader_name | Definition 自有 | 完整稳定身份比较，不拥有 Shader 或 native pipeline |
| GraphResourceUse 范围/stride/alignment/field_index | Definition 自有 | 证明有限访问范围并对应同源字段，不实现 Hazard scheduler |
| GraphFieldBinding 范围 | Definition 自有 | 未来 native view 必须保持相同范围，不拥有 backing |
| PassShaderContract stage_declarations | 生成静态存储 | 同源 stage 子集；Builder 仍持 owning union snapshot 与全部字段 mask |
| PassShaderModule | 冷态 validator 调用期借用 | 真实 SPIR-V words，调用结束即释放借用，无 Runtime owner |

## 来源与回归义务

基础为 F1 I `78a1248d1f5e9c59eb8f88ddd4869244e512050f` 的实际生成器/Builder/validator。
Texture format 权威来自同一基线 `modules/resource/description/.../Image.hpp`，没有复制 V1 format enum。
Legacy fixture 原路径：`modules/function/render/features/assets/shaders/postprocess/tonemap.vert.lglsl`，
对应 Frozen V1 SHA `a669409a289a6fa4092f21176397795b1cdb7f3e`，验收比对 Git blob。
原 R3 Graph.cpp、Plan.cpp、Bindings.cpp、Benchmark.cpp 不改写算法；F1 HZB oracle 显式声明 mip 0 view，
保留其原有“读 mip 0 / 写 mip 1”的意图，而新默认 whole-view 的冲突由专门回归覆盖。

新用例：formats、ranges、identities、HalfStorage、Stages.vert、Stages.frag、LegacyTonemap.vert、stage_program。
新增 runtime_array 反射负例拒绝以固定数组冒充运行时数组。所有旧测试名字和语义保留。GPU/Native Graph/Runtime/产品/完整 installed SDK 不属于本修复资格；
原有 R4 GPU 测试只作为保留回归运行，不宣称 F1 Shader 新增 GPU 功能已验收。
