# 15 — Data-First C++20 API / Shader / Graph / Plugin 贯通示例

> **全部代码是目标 API 语义草案，不是目前 F4 可运行程序。** 现有 `LUX_PASS_PARAMS`, `LUX_RESOURCE`, `RenderGraphBuilder` 名称应复用；新 `LUX_RENDER_DATA`, `DataSchema<T>`, `DataSetWriter`, `graph.read/graphicsIndirect` 等接口最终拼写需经现有 Generator、MSVC C++20、分离 CPU/Wire/GPU 合同、安装 SDK 和真实 GPU fixture 固化，不准引入第二种 Meta Parser。按下面场景最小实现，不需增加没有消费者的类。

## 15.1 E01：零 Feature 的数据类型

```cpp
struct alignas(16) LUX_RENDER_DATA(
    name="example.sprite.v1", scope=scene, authority=cpu,
    residency=gpu, indexing=stable, update=dirty, layout=auto
) SpriteGpu {
    float xywh[4];
    float uv[4];
    uint32_t atlas_index, layer, flags, reserved;
};
// 目标 Shader-visible representation 为 48 字节；实际 GPU ABI 需生成器/反射核对。
// 这不要求所有 CPU-only 类型具有此布局，也不将 CPU bytes 默认视为 Wire bytes。
```

目标 Scope Writer：

```cpp
auto sprites = scene.data().write<SpriteGpu>("canvas.main");
auto first = sprites.emplace(SpriteGpu{.xywh={0,0,40,40}, .uv={0,0,1,1},
                                      .atlas_index=0, .layer=2});
sprites.patch(first, [](SpriteGpu& s) noexcept { s.xywh[0] += 10.0f; });
sprites.erase(first); // 撤销逻辑可见性；原 GPU slot 未退休前不得覆写
```

C++ 引用/Reader/Writer 不是跨线程事件；Engine 主线程应发 `PROGRAM SpriteDelta`，Backend owner thread 接纳后执行对应操作。DataSet 不因 SpriteFeature 卸载自动删除。

## 15.2 E02：GPU Compute Cull + Indirect Draw

目标 PassParams 中资源只声明一次：

```cpp
struct LUX_PASS_PARAMS() SpriteCullParams {
    LUX_RESOURCE(role=push_constant, stages=4) uint32_t active_count;
    LUX_RESOURCE(role=read_only_storage, stages=4)
       StorageBuffer<SpriteGpu> sprites;
    LUX_RESOURCE(role=read_only_storage, stages=4)
       StorageBuffer<uint32_t> active_slots;
    LUX_RESOURCE(role=read_write_storage, stages=4)
       StorageBuffer<uint32_t> visible_slots;
    LUX_RESOURCE(role=read_write_storage, stages=4)
       StorageBuffer<uint32_t> draw_args;
};
```

对应 Compute（生成器先初始化/绑定 `SpriteGpu` 的 GPU ABI）：

```glsl
#version 450
//! lux-shader stage=compute entry=main
layout(local_size_x=128) in;
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= lux_push.p_active_count) return;
    uint slot = p_active_slots.data[i];
    SpriteGpu s = p_sprites.data[slot];
    if (!spriteVisible(s)) return;
    // args[1] 是 draw VkDrawIndirectCommand::instanceCount，
    // 且在另一个经过 Graph 声明的 Init Pass 被置为 0。
    uint dst = atomicAdd(p_draw_args.data[1], 1u);
    p_visible_slots.data[dst] = slot;
}
```

Compute→Graphics Graph 必须见 `visible_slots` STORAGE_WRITE→SHADER_READ，`draw_args` STORAGE_WRITE→INDIRECT_READ；Shader 资源来自 generated schema，`graphicsIndirect` 为受限 Native Op，不能偷偷接受未声明为 INDIRECT 的 raw Buffer。

```cpp
// 目标 composition API，临时 Graph buffers 是本帧中间结果：
auto s = graph.import(view.data().require<SpriteGpu>("canvas.main"));
auto visible = graph.buffer(visible_desc, "sprite.visible");
auto args    = graph.buffer(indirect_desc, "sprite.draw_args");
graph.clearIndirect(args, /*vertexCount=*/6); // 实际初始化是合法 Pass
// cull params 用 `s.records`, `s.activeSlots` 及 `visible`, `args`
graph.compute("sprite.cull", {...}, EExecutionScope::VIEW, cull_params);
graph.graphicsIndirect("sprite.draw", {...}, EExecutionScope::VIEW, draw_params);
```

这里只创建一个 `SpriteGpu` 类型，没有 GeometryKind、Mesh Instance 或新 RenderGraph 调度器。`std::vector<SpriteGpu>` 的 CPU 整体 memcpy 并非强制 GPU 记录布局，draw Workset 可以独立 compact。

## 15.3 E03：跨 Feature 多个独立 Owner

数据预先存在：Scene 数据集 `Light.main`、Runtime 数据集 `Material.catalog`、View 数据集 `Camera.current`，Shadow Optional 数据集由相关 provider 使用 GPU Pass 更新。

```cpp
struct LUX_PASS_PARAMS() DeferredLightingParams {
    // 结构性 Owner 依据 Schema/semantic/scope/full shape；实际 DataSetRef 留到 Invocation。
    LUX_RESOURCE(role=read_only_storage, stages=4, semantic="Light.main")
       StorageBuffer<LightGpu> lights;
    LUX_RESOURCE(role=read_only_storage, stages=4, semantic="Material.catalog")
       StorageBuffer<MaterialGpu> materials;
    LUX_RESOURCE(role=sampled_read, stages=4, semantic="GBuffer.Normal")
       SampledTexture normals;
    LUX_RESOURCE(role=storage_write, stages=4, format=rgba16f)
       StorageTexture hdr;
};
```

Feature Composition 用 `require<LightGpu>(Scene,"main")`, `require<MaterialGpu>(Runtime,"catalog")`, `require<GBufferNormal>(View,...)`, optional Shadow + explicit fallback 做冷态 resolve，然后将 Reader 的 data views 转成 PassSchema resources。Graph 自动建立 GBuffer Write→Lighting Read 的 RAW；CPU Light Dirty Upload 的数据 ready 使用 Native ticket，不伪造 Light Pass。相同字段尺寸但不同 TypeID 的材料与灯光不允许误接。当前 F3 单一 `OwnerAssignment.feature` **不支持上述多真实 Owner 合同**，未来实施必须最小更改 LayoutPlan 绑定键而非扩建固定 owner 枚举。

## 15.4 E04：GPU-authored Particle / HZB

```cpp
struct LUX_RENDER_DATA(name="example.particle.v1", authority=gpu,
                       scope=scene, residency=gpu, layout=auto)
ParticleGpu { float position[4]; float velocity[4]; };
```

CPU DataSet 记录 Shader Buffer 的 Shape/Capacity 与 Write Authority，不暴露假的实时 `get<ParticleGpu>()`；GPU `Integrate` 写 `current`, Raster 或再次 Compute 读版本，下一帧通过 Native Ticket 和合法 ping-pong/slot reuse 建因果关系。CPU 发射命令来自 PROGRAM/UPLOAD，不能每帧读回粒子数。对于 View HZB，History 合法性来自 View DataSet Epoch，不来自 UniversalHistoryManager。

## 15.5 E05：插件只需声明数据，不一定声明 Feature

插件 A 单独注册 `example.gaussian.chunk.v1` 的 `DataSchema<GaussianChunkGpu>`，Host 在 Scope 创建 DataSet。插件 B 可定义 Gaussian feature 读取它，插件 C 可另写 TileSort Feature。Type Catalogue 只注册一次 Schema，二者共享 DataSet，功能 Code Pins 独立；Type/Backing 可以在特定合法 retention 下比 Feature 存活更久。跨 DLL 传 versioned POD descriptor/opaque routes，插件 C++ wrapper 内静态 `require<GaussianChunkGpu>()`，不持有私有 Vulkan Device。

## 15.6 E06：两个 Scene、四个 View

Scene A/B 共享 `MeshGeometry` 资产 Native backing（按 Asset retain），各自 `TransformGpu/MeshDrawGpu` DataSets。Scene A 四个 View 使用同 RenderPath，共享一个 Native Plan；四份 Camera/History/Bindings/Invocation Slots，若 Skinning 姿态和数据版本相同只做一次 Scene Compute，Cull/HZB/GBuffer 各 View 运行。实时 Sim 10Hz、Render 144Hz，即使 PROGRAM 暂无数据仍可按 Target deadline 绘制。

## 15.7 E07：纯工具/2D，无 ECS Entity

一个离线 Viewer/Editor 模块直接发布 `Sprite2D`, `TileMap2D`, `GizmoLine`, `Trajectory` DataSets，不创建 MeshInstance，也不强制创建 ECS World；所有颜色/深度/UI/Target pass 进入同一个 RenderGraph 和 Native Executor。Layer/Painter Order 作为 typed constraints 而非 GPU AtomicList 的偶然输出顺序。

## 15.8 每个示例的负例

类型同 size 不同 schema；同 type 两 dataset 没给 Key；多 writer 冲突；Writer 跨 Scope/线程；GPU-only 数据 CPU `.get` 编译错误；未 Ready Shader 读取；空 History/fallback；Draw Args 未初始化/未声明 INDIRECT role；插件 unload 时仍有在途 shader/code callback；旧 record handle 在 slot reuse 后复活；两个 imported VkImages 不同 handles 但 memory overlap；CPU Buffer 写 GPU in-flight 同 Range。
