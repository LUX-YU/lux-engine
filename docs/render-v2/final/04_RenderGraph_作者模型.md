# 04 — RenderGraph 作者模型、完整资源/Pass 语义与 View-independent 复用

## 4.1 Graph 是可执行工作说明，不是 Runtime Owner

作者仅声明“哪类 Pass、使用哪些资源、以什么方式访问、产生什么值/副作用”。`RenderGraphDefinition` 是 immutable/canonical owning value，`LogicalGraphPlan` 是算法输出，`ExecutableGraphPlan` 是 Native 编译后的完整实际执行规则。Graph Foundation 不能持有 `VkDevice`、`RenderScene`、ECS，也不能以 `GraphResourceId` 身份创建持久 SceneData。

**用户侧主接口必须是 Builder，不是 `vector<GraphPass>`。** 仅测试/工具可用显式 direct constructor，但必须走同一 validator；不能允许外部 Feature 绕过 typed PassParams 手工塞无约束 array/descriptor facts。

## 4.2 数据模型（C++20 语义型值）

```cpp
using PassKey = StrongPassKey;           // author/stable cold identity, NOT numeric draw id
using GraphTexture = StrongGraphTexture; // definition-local, distinct from GraphBuffer
using GraphBuffer  = StrongGraphBuffer;

enum class ResourceOrigin : uint8_t { GraphTransient, External };
enum class PersistentScope : uint8_t { None, Runtime, Scene, View };
// Origin is NOT ownership scope: an Imported resource may be Scene/View-persistent.
enum class AccessKind : uint8_t { Read, Write, ReadWrite };
enum class PassKind : uint8_t { Graphics, Compute, Transfer, HostReadback };
enum class ExecutionScope : uint8_t { Scene, View, Target };

struct TextureDesc {
    PixelFormat format;
    ExtentDesc extent;       // absolute, target-relative, dynamic extent class
    ImageDimension dimension;
    uint32_t mip_count{1}, array_layers{1}, samples{1};
    TextureUsageFlags usage;
};
struct ImageRange { AspectMask aspect; uint32_t base_mip, mip_count, base_layer, layer_count; };
struct BufferRange { uint64_t byte_offset, byte_count; };
struct ReadAccess { GraphResourceId resource; SubresourceRange range; AccessStage stage; };
struct WriteAccess { GraphResourceId resource; SubresourceRange range; AccessStage stage; };
```

示例为类型语义合同，真实 C++ 定义可与现有 `render_core` 强 ID 对齐。`std::variant<ImageRange,BufferRange>` 作为 `SubresourceRange` 的封闭和类型；Pass 内可以对同资源使用多个**不重叠**范围；重叠读写必须是明确的 ReadWrite/feedback-loop 语义，不能用同 Pass 两个独立 `read`+`write` 悄悄绕开验证。

必须支持的资源语义：

- `Transient`：Graph 内物理存储，受 compiled schedule/lifetimes 管理，允许经证明安全 alias；compile-only 无 native backing。
- `Imported`：Graph 外有权威 native owner，需声明 initial/access/layout/queue-family/ready evidence、terminal/export obligations；不能仅用非零 token 冒充生命周期证明。
- `PersistentScope::Scene/View/Runtime`：这是独立于 Origin 的真正长期资源所有权范围。通常通过 `ResourceOrigin::External` 和 `ImportContract` 加入 Graph，由 Scene/Feature/View/ResourceDomain 对应 native owner 持有；不是自动全局 HistoryManager。
- `TemporalHistory`：由 owning Feature/View 暴露 current/previous roles、validity epoch/resize/cut invalidation，Graph 显式历史读/当前写与 inter-frame sync。
- `Exported`：是**输出边界/副作用属性**，不与资源生命周期 enum 混合；同一 imported/current image 可被输出给 Target、readback、下帧 history。
- `External semantic resource`：Feature-owned buffer/image 可以参与 read/write dependency 而不让 Graph 分配；需携带真实 native barrier ownership contract。
- Buffer `offset/length`、Image `aspect/mip/layer` 是一等访问范围，完整覆盖 `0`、`REMAINING`、array/dynamic offsets 和合法对齐/设备界限。
- Sampled, Storage, Uniform, Vertex, Index, Indirect, Color, Depth/Stencil, Resolve, Transfer, Present, InputAttachment/local-read 是**逻辑使用语义**，与 `AccessKind` 共同决定 barriers/validation。

## 4.3 Authoring API（拟定的最终语义，具体拼写 F1 golden 固化）

```cpp
class RenderGraphBuilder final {
public:
    [[nodiscard]] GraphTexture texture(PassResourceName, TextureDesc);
    [[nodiscard]] GraphBuffer buffer(PassResourceName, BufferDesc);
    [[nodiscard]] GraphTexture importTexture(ResourceSemantic, TextureDesc, ImportContract);
    [[nodiscard]] GraphBuffer importBuffer(ResourceSemantic, BufferDesc, ImportContract);
    [[nodiscard]] RenderResult<void> exportTexture(GraphTexture, ExportContract) noexcept;

    template<class Params> requires GraphPassParameters<Params>
    [[nodiscard]] PassKey compute(PassName, ComputeShaderKey, Params&&);
    template<class Params> requires GraphPassParameters<Params>
    [[nodiscard]] PassKey graphics(PassName, GraphicsPipelineKey, Params&&);
    template<class Params> requires GraphPassParameters<Params>
    [[nodiscard]] PassKey transfer(PassName, TransferPassKey, Params&&);

    [[nodiscard]] RenderResult<RenderGraphDefinition> finish() && noexcept;
};
```

`PassResourceName` 为 cold authoring/diagnostics semantic ID 或唯一字符串，运行时 route 使用 numeric local ID；不能因为两个不同 Feature 同名就暗中共享资源。跨 Feature 输入要使用明确 exported semantic slot 或 typed producer dependency，required/optional 分别受约束。用于编译时的 named resolution **只发生冷态**。

```cpp
struct LUX_PASS_PARAMS() BloomExtractParams {
    LUX_RESOURCE(role=sampled_read)  SampledTexture hdr;
    LUX_RESOURCE(role=storage_write) StorageTexture bloom;
    float threshold{1.0f};
};

graph.compute("Bloom.Extract", shaders.bloom_extract,
    BloomExtractParams{.hdr=scene_hdr, .bloom=bloom_half, .threshold=1.3f});
```

该作者代码隐式生成 Graph reads/writes、Shader logical descriptor fields、stage access 和 runtime binding recipe；不得再要求作者重复 `pass.read(hdr)` 和 `.bindDS(1,...)`。由 RenderGraphBuilder 以完成的 Schema 统一捕获依赖值/资源槽，而不是保存包含借用指针的临时 Params；必要跨编译期的参数使用 owning cold schema snapshot/recipe。

## 4.4 Shader 字段之外的 Pass 资源与副作用

Push/UBO/SSBO/Sampled/Storage 由同源 PassParams 产生；深度/颜色 attachment、LOAD/CLEAR、Resolve、TransferCopy、Present、Readback、Export 等需要 typed attachment/operation contract（可使用 `LUX_RESOURCE(role=color_attachment_write)` 或明确 typed Pass attachement member）。不把它们假装全能由 SPIR-V Shader reflection 推导。

`GraphicsPassParams` 可声明 color array、depth/stencil、MSAA resolve、load/store、clear value、view mask eligibility。`ComputePassParams` 明确 dispatch bound/indirect offset、storage read/write。`TransferPassParams` 明确 src/dst ranges/size。HostReadback 必须 Graph terminal/side-effect，不能被 pass cull。

Condition/cull/side-effect：`PassCondition` 仅允许冷编译证明的可跳过行为。可选 producer 跳过后 consumer 的输入必须有显式 fallback、上帧有效 history 或同帧其他 producer，且没有无定义数据；否则 cold compile error。`has_side_effect` 不能随作者任意 flag 让非法写入被掩盖，副作用有 typed contract（Present/Readback/ExternalWrite/Feedback）。

## 4.5 Pass Scope 是语义，不是三套实现

- `Scene`：工作结果只依赖 Scene-scope 标识、相关 source revisions、resource handles、RenderTime/Frame 状态及声明输入；每个合法 invocation key 只执行一次，典型 skinning、共享 static upload、非 view-dependent Shadow。
- `View`：输出与 Camera、View visibility/extent/history/jitter/render path 相关；各 View 分别输出，也可在兼容时经 Vulkan Multiview 合并提交。
- `Target`：output/present/cross-target composite，依赖实际 target/image acquire 与 blend/load contract。

**默认安全性**：`ExecutionScope::Scene` 是作者/编译器要证明的契约，而不是“Shader 没写 Camera 就默认为 Scene”。如果 Pass 读取 View-dependent 资源、Time/History/Target、副作用或明确不同 View 内容，不能提升到 Scene。编译器可拒绝 `Scene` scope；没有证明时保持 View-level。`Multiview` 是 View pass 的一个 native 编译优化，不增加 `MultiviewFeature/MultiviewPass` 多态层。

## 4.6 多 View 不再复制编译结果

一个 RenderScene `View A,B,C` 相同 `RenderPath` 时：Graph build/compile 同拓扑应复用 1 logical/native Plan（实际 native key 含 target compatible signature）；View-specific frame bindings/history/target 仍不同。View C 不同 shader/effects 时可引用另一个 native plan；可能共享能证明相同的 logical scheduling analysis，但是不能错误复用 shader/pipeline execution recipe。

`RenderGraphDefinition` 应能同时表达 Scene-level 与 View-level pass 模板（有显式 scope），由 `FrameExecutor` 将**每帧实际需要的 View group** 展开为可执行 invocation。**不为每 View 建新的 owner graph**。每帧 expansion 仅使用预保留 scratch，不能隐式重新构建/编译图。

## 4.7 Builder、Definition、Pass 参数与缓存生命周期

Builder 是 cold 短寿命 mutable object，完整使用 `finish() &&` 转移；Definition 完成后只读、自有存储；LogicalPlan 是只读 value；NativePlan 持 native owner/code pin 与 typed recipes；执行期间借用 plan/bindings 并通过 Frame owner pin 在途 backing。失败 candidate 一律丢弃，旧计划继续合法执行；涉及 output 不兼容必须保留 old output 或明确暂停该 View 的该目标，不能借最后好图读取不兼容资源。

拓扑比较包含资源、pass、scope、effect/behavior identity、access/usage、shader interface identity、attachment signature 真实相关字段。`View` 的 Camera matrix/render time/descriptor dynamic offset/current swapchain image 不成为稳定 plan key。Native plan key 还须含 device caps/features, pipeline variant, render formats, shader hashes, LayoutPlan signature（详见第 07 章）。

## 4.8 必须覆盖的编译负例

错误资源 ID、循环、forward required producer 缺失、optional 无合法 fallback、同 pass 重叠冲突访问、未初始化 transient mip read、实际 subresource OOB、buffer range overflow、离散 read/write 漂移、两个 provider 输出同 semantic 且不显式合成、Scene pass 偷读 View-local history、异步队列/alias 不安全、multiview 不兼容不许默默启用、native Shader/Pass access 不一致、target format/sample count 不匹配。

Legacy R3 的 300 张随机图/10k 绑定复用等 oracle 必须保留或等价升级，不得删除之前的 45/59 项测试以便签名重写。算法与复杂度见第 06 章，工具链见第 05 章。
