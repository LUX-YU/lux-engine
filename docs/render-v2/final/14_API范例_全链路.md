# 14 — 七条 end-to-end API/GPU 验收场景（示意代码→实际职责→失败/输出）

> **接口示例是设计目标语义，不等于已存在于当前 R4 源码的可直接编译 API。** 各阶段须将示例变成真实独立 source fixture、使用生成接口并作为 GPU/SDK acceptance oracle。不可仅提交同名空函数。

## E01 — Tonemap：一份 PassParams 驱动 Graph 与 Shader

```cpp
struct LUX_PASS_PARAMS() TonemapParams {
    LUX_RESOURCE(role=sampled_read) SampledTexture hdr;
    LUX_RESOURCE(role=sampler, for=hdr) SamplerHandle sampler;
    LUX_RESOURCE(role=color_attachment_write) GraphTexture ldr_target;
    float exposure{1.0f};
};

graph.graphics("Tonemap", shader.tonemap,
               TonemapParams{.hdr=hdr_color, .sampler=linear,
                             .ldr_target=target, .exposure=view.exposure});
```

**编译链**：meta→generated C++/GLSL + C++/Shader layout assert→SPIR-V reflection→LayoutPlan set/binding→Graph sampled read/HDR and color attachment write→PSO/barrier/Descriptor recipe→GPU tonemap→HDR/SDR reference readback。

验收：作者不写裸 `VkDescriptorSet`、`set/binding`、`pass.read`；Exposure 每帧变化不 compile；Shader 输出/颜色空间与 reference 逐值接近，缺 HDR producer/format mismatch/field rename fail cold。

## E02 — Mesh/Light：ECS source 到 GPU Forward/Deferred/Shadow

```text
World ECS Mesh/Light/Transform
→ MeshProjection + LightProjection observe dirty source revision
→ Asset Domain dedupe Mesh/Material/Textures; UPLOAD owned immutable bytes
→ ready receipt → PROGRAM upsert instance/light
→ RenderScene persistent MeshSceneData/LightSceneData
→ scene pose epoch Skinning maybe once
→ View Group A/B: cull/Depth/GBuffer or Forward per path
→ Shared LightShadow if compatible light projection; clustered light → Tonemap
→ one Target or multiple composited outputs
```

验收：两个 Scenes 共用 asset GPU backing、每 Scene 自有 instance/light state；一个 Scene 两个不同 View 的 camera matrix、visibility GBuffer 不同但没有两份资源上传；改一盏灯不重建 graph；删 Scene A 后 GPU fence complete 不破坏 Scene B。故意 PROGRAM 比 UPLOAD 早到不能渲染未 ready resource。

## E03 — 同 Scene 四 View：同 RenderPath 计划共享和实际共享 GPU work

```text
Scene S: Skinning producer + Scene lights + MeshSceneData
 View A: Camera A, Deferred, 1920×1080, HDR + Tonemap
 View B: Camera B, Deferred, 1920×1080, HDR + Tonemap
 View C: Camera C, Deferred, 1920×1080, HZB history C
 View D: Camera D, Forward, 1280×720, HDR + Tonemap
```

预期：A/B/C 有相容的 Graph/PSO key时 compiled-plan count 不随 3 个 Camera 增长，D 使用独立 native plan；Scene-scope Skinning 同 pose epoch 执行一次；GBuffer/Depth/HZB/History per Camera 不共享结果。开启 compatible Vulkan Multiview 时 A/B 的 graphics 可合并，GPU 输出与 separate fallback 等价；不支持或可见集合不同不许强制合并。删除 B 不使 A/C 重新编译或释放它们的 history/backing。

## E04 — HZB mip + async Compute + GPU cull

```cpp
// logical image with 5 mips, input depth from this View
for (uint32_t mip = 0; mip < 5; ++mip) {
    graph.compute("HZB.Mip", hzb_downsample,
        HzbMipParams{.src=(mip==0 ? depth : hzb.mip(mip-1)),
                     .dst=hzb.mip(mip)});
}
// GPU cull reads current/previous valid history per actual View contract.
```

验收：GPU readback verifies each mip resolution/value、No RAW gap、不重叠 mip 无假 WAW、queue-eligible async compute 有 native wait/signal/ownership barrier；单 queue fallback 结果相同。View resize/camera cut 无陈旧 history、mip invalidation 正确，故意缺 producer/读未初始化 mip cold fail。

## E05 — Terrain + RenderCluster + 五点云策略 + Streaming Feedback

Scene receive bounded UPLOAD pages/chunks with per-owner generations + ready receipts；Terrain PageCache parent fallback、LOD/error selection；RenderCluster producer GPU cull/LOD/pick；PointCloud Simple/GPUDriven/LOD/Splatting/Transient 各自切换 RenderPath/Feature variant；UI display picking feedback。多 View 可共享 Mesh/PointCloud buffer/octree, 但各自的 Camera/visibility/cull output 不强制共享。Texture/Terrain page cache 容量压力不会错误抛弃后续 revisions。

验收：空中/地面 Camera移动、大世界 rebase、Parent fallback、Chunks remove-reuse、GPU pick after View destroy, Alpha soft-splat blending, Transient reset, Upload backpressure。验证 GPU output、错误回执、resident bytes、overflows、GPU memory peak、FPS CPU/GPU。

## E06 — External Plugin：Shader/PassParams/能力/卸载

```text
External Plugin SDK consumer (separate project, installed-prefix-only)
  declares LUX_PASS_PARAMS() OverlayParams + shader/.lglsl
  publishes FeatureDescriptor {provides OptionalHeatmapCapability}
  registers generated FeatureOps table from concrete OverlayFeature
  contributes pass using GraphBuilder public API
  Backend codegen/Shader reflection/LayoutPlan native compile
  integrated with built-in Mesh + UI Target, real GPU output readback
  uninstall while prior executable graph/callback in-flight
  stop new work, quiesce, GPU retire, release last code pin, DLL unload
```

验收：missing provider cold diagnostic、private include negative compile、ABI mismatch reject、Feature re-install with new DLL version no stale callback、fence pending cannot unload early。宿主暴露对能力的受限 API，不允许直接 `VulkanBackend*`。

## E07 — Simulation 10Hz / Render 60–144Hz & Target stop/resize

一次 PROGRAM 建 Scene/mesh/camera，后连续输出多帧（无新 PROGRAM），每帧增量 FrameSerial/RenderTime，Camera via legit UI update independent of Simulation；Graph compile_count stays fixed，GPU stable output/updated camera正确。多个 surface/offscreen targets 有各自 readiness，minimize 时 idle 不忙等，恢复时重建 GPU-safe；Device Lost 注入立即 terminal、无假继续渲染。

测试同时记录 **R2 input packet_count ≠ Frame rendered count** 和 **GPU completed serial ≠ frame serial**，CPU wake/sleep/latency、公平 UPLOAD 进度、独立 CTest + GPU validation。

## 14.8 哪些是编译期负例，哪些是 GPU 正例

- 编译期负例：错误 `GraphTexture` 传入 Buffer 参数、无 generated `PassSchema<T>`、临时 plan/borrow 泄漏、Feature 缺正确 `noexcept`、private SDK include。
- 冷编译负例：Shader/Graph access mismatches, invalid attachment format, missing required producer, incorrect capability or LayoutPlan budget。
- GPU 正例：真实 Tonemap/PBR/Shadow/HZB/alias/async/Multiview/Plugin/Target 输出与同步，不能由 fake native graph 替代。
- 生命周期正例：3 lane backpressure/stop/ticket abandon、Scene/View remove + GPU in-flight、Plugin unload、Asset shared across Scene。

以上七条是完成系统闭环的**最低真实集成测试**，业务 `Fxx` 和 UE `Hxx` 仍需要更完整的专项 oracle；不能将这七条冒称完整 Legacy parity。
