# 06 — RenderEffect 是 Graph + Feature 集合；Feature 编写与资源准备分离

## 6.1 语义不再混同

`Shader` 实现 GPU 计算与着色；`RenderFeature` 定义可组合的 GPU Pass、所需中立数据和可选 CPU 预处理逻辑；`RenderGraph` 定义实际 GPU 工作的资源、生产者、依赖、条件与输出；`RenderEffect` 是逻辑 Graph 和 Feature 集合的单一组合/编辑及版本拥有者。它不含 Scene/View/Target/Native Session 的长期反向引用。Native Graph 在唯一 DeviceSession 编译代码容器中，当前 `RenderGraphId` 指 Native 编译身份。

```cpp
// 概念性 class shape；不主张新增 FeatureCollectionManager
class RenderEffect {
    // small FeatureEntry collection (author configuration + code pin)
    // one logical RenderGraphDefinition / IR ownership
    // immutable candidate revision + shared hot parameter schema
};
```

不得把 Feature 当 GPU 数据的默认 Owner，也不再强制每个 Renderer 功能都必须有 Feature。例如旧 `LightFeature` 如果仅为资源提供者，改为独立 Scene `LightStorage` + Engine projection；真正阴影和 lighting Pass 才有对应 Feature。`StandardMeshStackFeature` 资源提供功能与 GPU-driven draw algorithm 明确拆开。

## 6.2 两类 Feature 的创建和状态

**纯作者 Feature**：可以是简单结构/自由函数，`static describe(config, requirements)` 和 `build(config,builder)`；不需要 DeviceSession，也不要求 `IRenderFeature` 继承。Shader variations 与 GPU ABI 的结构依赖可编译为版本化 Graph。

**确有设备相关行为的 Feature**：设备相关准备不要绑定到长期、跨 Scene 的 RenderEffect author；由 Native 侧在 compile/attach 阶段通过当前 `DeviceContext`/`ResourceContext` 或确实必要的 `DeviceSession&` 创建其运行期实现。其 Lifetime 由 DeviceSession/Attachment 持有，资源需求仍由统一 ResourcePlan 表达。允许插件提供可选 `prepareNative` ops，但是有真正消费者时才新增，Host 必须管理 CodePin；不要每个 Feature 默认启动 `Worker`、Pool、Registry。

```cpp
// 作者态示意
struct GridFeature {
    static void describe(const GridOptions&, ResourceRequirements&);
    static RenderResult<void> build(const GridOptions&, RenderGraphBuilder&) noexcept;
};
// native prepare 是可选 hook，不在 Native record 路径通过 Feature this 执行
```

如果一个 Effect 创建成功却从未 attach，默认不应预分配 Scene/View/Attachment GPU Storage。Session 设备级 Shader Code 可能在创建 Effect 时冷编译，但业务存储遵循 attach/preload lease；Effect 创建时不能捕获“某 Scene 的 LightStorage”。Feature 可以有 Effect-scope CPU 参数，实际 GPU runtime backing 必须有明确 Session/Attachment owner。

## 6.3 资源与数据需求

`Feature.requires<T>(scope,key,access,config)` 产生逻辑请求；`PassSchema` 保留真实 GPU 读写范围。`DataSchema` 可 CPU-only/GPU-only/external，GPU-only 不能要求虚构 CPU mirror；Renderer 不凭类型 T 自动生成 ECS Proxy，也不能按第一个注册的类型实例猜数据源。多个 Feature 共享相同 Storage 时由 Compiler 合并相容需求，attach 增加 lease；不相容则 cold error。

## 6.4 无状态 ≠ 无行为

少数 Feature 需要在帧开始准备 View jitter、CSM cascade、LOD budget 或反馈优先级。优先由明确的 typed Data Producer/Native preparation 上传参数或编译成 GPU Pass，不让编译 Graph 的 `[this]` Callback 在 GPU record 热态访问正在变化的 Feature。确实需要 CPU per-frame 程序时，用生命周期稳定的固定 Ops/准备表，在特定事件版本变化时执行，不能引入每 Feature 额外 FrameLoop。

## 6.5 C++20 作者与插件

C++20 Concepts 用于本地 Author `describe/build` 签名验证；可选 hook 通过 `if constexpr` 生成固定 Function Ops，不为所有 Feature 强制虚接口。跨插件 DLL 使用版本化 extern C 表和 Host-owned 值元数据，绝不跨边界传 owning STL/RTTI/异常；在 GPU Native program/callback/destructor 仍在途时保留插件 CodePin。Installed SDK consumer 需能不修改 Runtime 核心就新增 Feature+DataSchema+Projection。

## 6.6 Effect 编辑事务

Effect 的逻辑身份在结构修改时保持稳定；候选 Feature 配置和逻辑 Graph 编译成功后，影响所有 attach 及同 Target 组合后续闭包；一次发布或全部 last-good。调整 exposure/grid opacity 等 hot values 只更新数据，不重建结构。Graph native cache 按完整 structural/ABI/caps/target signature 复用，**没有第二个独立拥有逻辑 RenderGraph 的 GraphCache**。
