# 15 — 端到端 C++20 Authoring / Engine Client / Graph / Executor 使用示例

> 下面 API 全是**目标语义伪代码**，尚非 HEAD `246fe21...` 已安装可编译 SDK；签名/返回值需在 N 阶段先做 public header + clean-clone installed consumer 的 C++20 正负例。Client 的创建、提交、结果均为 R2 异步请求，示例为方便阅读省略某些 poll/await 层。

## E01 共享渲染效果的创建与两个场景使用

```cpp
auto effect_spec = RenderEffectBuilder{};
effect_spec.add<DepthPrepass>(depth_options);
effect_spec.add<GpuCull>(cull_options);
effect_spec.add<DeferredLighting>(light_options);
effect_spec.add<Tonemap>(tonemap_options);
auto effect = runtime.createEffect(std::move(effect_spec).finish());

auto scene_a = runtime.createScene();
auto scene_b = runtime.createScene();
auto view_a = runtime.createView(scene_a);
auto view_b = runtime.createView(scene_b);
runtime.setViewEffect(view_a, effect);
runtime.setViewEffect(view_b, effect);
// 修改一次 Effect 结构，两 View 都在安全 publish 点升级；
// 更新 scene_a 的 LightBuffer 只影响 A 的数据。
```

`RenderEffectId` 维持稳定，两个 View 经过 Native Graph full structural compatibility 可以得到同一个 `RenderGraphId`；它们的 `SceneResourceDomainId` 不相同。Graph shared pipeline/layout，Context/per FIF slot 独立。Effect 结构更新失败时两 View 保留旧版本，不能只更新其中一个。

## E02 Backend-neutral Scene/View/Target 与资源域

```cpp
// 仅 Backend owner-thread；不是 Client 跨线程调用
Scene& scene_a = backend.scene(scene_a_id);
View& view_a = scene_a.view(view_a_id);
const auto& lights = backend.resources(view_a.domain()).get<LightBuffer>();
const auto& mesh_storage = backend.resources(view_a.domain()).get<MeshStorage>();
// 最近 ancestor 读取；写入 LightBuffer 需在实际 owning Scene Domain 上授权。
```

Root MeshStorage 一份 geometry，被 A/B Scene 的不同实例 Transform 引用。Client 通过 GpuResourceId 保留 Asset retain/release，Backend last-use 真实推迟物理销毁。

## E03 RenderEffect 的普通 Feature 及算法复用

```cpp
RenderResult<void> buildCull(RenderGraphBuilder& graph, const CullOptions& p) noexcept;

struct ForwardMesh {
    static RenderResult<void> build(RenderGraphBuilder& graph, const ForwardOptions& options) noexcept {
        if (auto r=buildCull(graph,options.cull); !r) return r;
        return buildForwardDraw(graph,options.draw);
    }
};
struct GBufferMesh {
    static RenderResult<void> build(RenderGraphBuilder& graph, const GBufferOptions& options) noexcept {
        if (auto r=buildCull(graph,options.cull); !r) return r;
        return buildDeferredDraw(graph,options.draw);
    }
};
```

两个 Feature 没有共同虚基类也没有重复的 Native Pipeline owner。`buildCull` 创建 Graph 内部 `VisibleSlots/DrawArgs`（中间变量），Forward/GBuffer 消费真实版本和 Sync。长期 GPU Mesh/Instance/Light 由外部 Domains 注入并在冷态 Binding 准备。

## E04 一张 Graph 产出完整 Target 最终画面

```cpp
RenderGraphBuilder g;
auto target_color = g.importTarget("SceneColor", target_output_contract);
auto depth = g.texture(depth_desc,"Depth");
auto gbuf = g.texture(gbuffer_desc,"GBuffer");
auto hdr = g.texture(hdr_desc,"HDR");
auto args = g.buffer(indirect_desc,"DrawArgs");
g.compute("Cull", cull_shader, cull_params(args));
g.graphics("GBuffer", gbuffer_shader, gbuf_params(gbuf,depth,args));
g.compute("Lighting", lighting_shader, lighting_params(gbuf,depth,hdr));
g.graphics("Tonemap", tonemap_shader, tonemap_params(hdr,target_color));
g.output(target_color);
RenderGraphDefinition def = std::move(g).finish().value();
RenderGraphIR ir = RenderGraphCompiler::compile(def).value();
// Native Compiler compile(ir,caps,target_signature) -> immutable GraphId
// Executor.execute(Graph, View, Target, Context) 后只做合法 Present/Export。
```

这些函数是简化的作者侧语义，真实 Pass 以现有 generated PassSchema、typed access/use 和 Result 表达。GBuffer/HDR/args 没有外部 Domain owner，也不允许 Graph 完成后被另一个 Graph 以“内部变量”方式读取。

## E05 多 View 共享 Effect 的统一结构更新

```cpp
auto e = runtime.effect(effect_id); // Client-side editable snapshot + revision
// 结构更新请求使用 expected revision 防并发冲突
runtime.updateEffect(effect_id, e.revision, [](EffectDraft& x) {
    x.replace<ShadowCSM, VirtualShadowMaps>(new_config);
});
```

Backend 编译所有 Target/Device 需要的 Native Graph 变体，验证 A/B/多 View 的 Domain 资源、History、Capabilities/Context slot，统一发布 active revision。任意无合法 fallback/ready 的 View 失败，整个更新保持 LastGood；Native GPU in-flight 旧 Graph 待 true completion 退休。

## E06 Effect 热参数与 Scene 数据

```cpp
runtime.updateEffectParams(effect_id, TonemapSettings{.exposure=1.2f});
runtime.publishSceneChanges(scene_a, compressed_light_delta);
// 两者均不得因为单个数据值变化而重编 Graph/Shader/PSO。
```

Effect Hot 参数使共用 Effect 的所有 View 在后续逻辑帧统一使用新快照；Scene A Light 仅更新 Scene A backing，不影响 B 的资源权威。若要求 View A 单独曝光，则建立独立 Effect 或有明确声明的 View override contract。

## E07 多 View 同一 Target + UI 合成

一个 Editor Target 可以输出 Camera A + Camera B + UI/Overlay。`TargetDesc/TargetLayer` 首先确定无歧义的区域、Blend/LoadStore、格式和顺序，Effect/Graph Builder 根据该 Target 的组合定义生成一张完整 Graph，提供多个 View 源和 Composite/Overlay Pass，`execute(graph, views, target)` **只执行一次完整的最终输出程序**。UI 自定义 Native 操作必须通过受限 Pass 接口申明访问，不能在 Graph 外私自 `vkCmdDraw` 改变最终像素。不同 Target 之间只可显式导入另一个 Offscreen Target 已经作为最终输出发布的 Texture，而不是 Graph 中间图。

## E08 GPU-authored Particle / HZB / 3DGS

GPU Particle Buffer 在 View/Scene Domain 创建，有 GPU-authority；Graph Pass Compute Update→Graphics Read，下一帧 History/import Ready 由真实 Queue Ticket 追踪，无 CPU 全量 readback。新 3DGS Feature 声明 `GaussianChunkGpu` + Projection/Sort/Splat 等 Pass，不要求 Mesh Instance 或核心修改 `switch(GeometryKind)`。`DataSchema<GaussianChunkGpu>` 与 Shader reflection 必须在真正 GPU oracle 中核验。

## E09 插件与 Installed SDK

插件注册 `FeatureOps` 及可选 `DataSchema<T>`，由 Effect 组合构建 Graph。Host 将插件的描述字段完整复制到稳定 Type Registry，并在任何旧 Graph Native owner、custom resource destructors 和 CPU callbacks 在途时 pin 插件模块；未提供 GPU capability 的插件应获得明确错误。外部 installed SDK Consumer 编译时不访问 `runtime/pinclude` 或 `backend/vulkan` 私有文件。

## E10 必须测试的负例

`GraphId` 被当成 `EffectId`；修改共享 Effect 只有一个 View 生效；ResourceDomain 从子域误写 Root；SceneB 借用 SceneA LightBuffer；外部 GPU 资产在 last use 完成前被回收；不同类型同 sizeof 错绑定；同 T/main 与 preview 被错误合并；Graph 中间 HDR 被当 persistent Image；Target 更新/缩放导致 History 旧 slot 生效；跨插件 DLL 析构或 callback 在卸载后调用；不同 Native VkImage 同 memory overlap 导致未证明 alias；未启用 RT/Mesh capability 仍执行；Record 热态重复 PSO 创建。
