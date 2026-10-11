# RD2-F6｜DataSchema / 多 Owner Shader / Feature SDK（预备范围，未授权）

**仅 F5 PASS 且用户再次授权后启用。** 不允许将本工作单与 F5/F7 一次实施。

## F6.a — DataSchema 同源生成与类型目录

目标：扩展现有 `lux-cxx` generator 与 `modules/resource/description` 的既有 metadata contract，使一个作者数据 T 生成：稳定 canonical/TypeID/version、合法 Scope/Authority/Indexing/Residency constraints、C++ DataSchema<T> Traits；CPU-only 按真实存储生命周期约束，跨线程/ABI 时才生成既有 PacketValue/codec 所需 Wire schema，Shader-visible 时才生成可选 GPU Layout/Representation（offset/stride/alignment/type/matrix major）、GLSL 与必要打包转换。`LUX_RENDER_DATA` 拼写必须经真实现有 meta parser、MSVC C++20 golden positive/negative 先固定，不能新建第二个 parser/DSL/toolchain。

仅给出冷态可注册的类型 descriptor：注册类型不自动创建 GPU buffer/Feature 实例。Host 拥有元数据 value/canonical names；TypeID 碰撞和不兼容版本须 fail；invalid std430/struct 仅适用于 GPU-facing 表示；裸指针/任意 C++ T 不得绕过 Wire/GPU 合同，合法 CPU-only 非平凡类型不因此被拒绝；不能把 `std::is_trivially_copyable` 冒充 GPU ABI。首组 golden `LightGpu`, `SpriteGpu`, GPU-written `ParticleGpu`、嵌套/array type、plugin GaussianChunk；不要求在本阶段提前做完所有 F7 GPU residency。

## F6.b — 唯一 LayoutPlan 多真实 Binding Owner

源代码已知 `OwnerAssignment {scene,feature,pass_local}` 每类单槽；修正为由 Type+Schema Layout+Semantic Slot+Scope Class 组成的冷态 `BindingSlotKey/ShaderOwnerId`，不是具体 `DataSetId`，GPU native backing 由每 Invocation 的 DataSet binding 提供。一个 Shader 同时读 Light/Material 两个独立 schemas，另一个 Scene 同结构 DataSet 可以共享 Pipeline Layout/PSO。多个 Owner 的 Full Shared Shape 冷态规划物理 sets，保留 device descriptor/PC limit与旧 F3 fixed-binding 性能门禁；旧 F3 PSO/relocation/validation/immutability 不退步。必须在真实 GPU 测试中验证两 Dataset 身份不冲突、同 Schema 两 Key 不误绑定、不同场景 Shape 等价的 Shader Layout 未复制。

## F6.c — 最小真实 DataSet GPU Bind（不预做完整 F7）

为了让外部插件的 type/schema 能真正影响 GPU pixel，不允许只有 Type Registry/空 `DataSetRef<T>`。由同一 Backend owner 提供**最小固定容量的 Buffer-backed Typed DataSet**：Scope/Type/Key identity、生效/失效、Host R4 RAII native Buffer、通过现有 Submission/Staging 上载简单初始数据、绑定到 Schema 与 Pass 并 GPU readback，旧 Backing 正确 retire。这是 F7 真正通用 ResidentStorage 的一条可延伸的最简单存储 Profile；不得引入之后又删除的试验 StoreManager/双重 authority。复杂 sparse/dense/dirty/backpressure、GPU-owned Particle、Chunk residency 留给 F7；F6 不声称它们已支持。

## F6.d — Feature/Plugin Public API

只允许 concrete Feature + C++20 Concepts optional hooks + generated fixed Ops，static internal direct calls 和动态 C ABI边界 type erasure；实现 typed required/optional DataSet selection (Type/Key/Scope/Writer Access)、真正 SceneCapability、插件 ABI/code pin、冷态 Composition 在 Graph 前解析 DataSet/Capability/权限，降低 typed Graph 声明，原 C1/C2 只解析图内关系和验证 Pass 接口；完整候选事务。`Feature` 不拥有默认 DataSet；它可以按合同请求创建新数据集，也可读取已有 one。不能创建独立全局 EventBus/FeatureManager/GPUStorageManager。

两份外部 installed consumer case：内置 Light Type 的 Feature + 自定义 GaussianChunk Data Type 的独立插件，完全不含 private Vulkan header。真实 GPU readback/computed output、插件 while in-flight unload、DataSet survives Feature under explicit retention、same type multiple dataset ambiguity negative；无业务 core switch。

## 共同门禁

保留 frozen 719/Stage I+clean V、Normal/full-ASan/real GPU GPU Validation、CMake FileAPI/target/install header/codegen dependency closure、ABI/protocol negative cases、0 warm allocations、F3/F4 perf comparison。遇到必须大改 F2 或 Device 授权接口，先 STOP 给 user 提交最小复现与模型对比；F7 生产 GPU storage 不在 F6 实施。完成 F6 PASS 后 STOP 等用户允许 F7。
