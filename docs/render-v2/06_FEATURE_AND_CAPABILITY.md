# 06 — Feature 系统与 SceneCapability R2

## 1. Feature 的定位

Render Feature 回答：

> backend 如何利用已经存在的 Scene data/resource state 形成 GPU 资源、行为与 RenderGraph pass？

它不回答：

> World/ECS 如何产生数据？

后者属于 RenderProjection。

## 2. 不创建独立 `render_feature` execution component

backend-neutral stable metadata 放在：

```text
render_core
```

包括：

```text
FeatureTypeId
FeatureDescriptor
SceneCapabilityId
provides/requires metadata
portable configuration metadata
```

执行扩展面放在：

```text
render_vulkan/feature/
```

包括：

```text
RenderFeatureOps
RenderFeatureContext
RenderSceneContext
FeatureInstance execution contract
Vulkan/graph extension surface
```

原因：

> 当前只有 Vulkan backend；不得为了未来第二 backend 预造伪 backend-neutral execution API。

## 3. Concrete Feature，不用抽象基类

作者：

```cpp
class TonemapFeature final
{
public:
    static RenderResult<TonemapFeature> create(const CreateInfo&) noexcept;
    RenderResult<void> attach(RenderFeatureContext&, RenderSceneContext&) noexcept;
    void addViewPasses(RenderGraphBuilder&, const ViewFrameContext&);
    void detach() noexcept;
};
```

不继承 `RenderFeature`。

## 4. Concepts/traits 只负责 authoring

`makeRenderFeatureRegistration<T>()` 在编译期探测 optional methods，并产生显式 registration / ops。

Runtime 不做：

```text
RTTI discovery
template registry magic
static constructor registration
```

## 5. FeatureInstance 单一 lifecycle authority

Backend container 持有：

```text
FeatureHandle
FeatureTypeId
FeatureDescriptor
EFeatureState
code lifetime
erased concrete object
ops table
```

concrete Feature 不复制这些字段。

对象销毁必须发生在 code pin 释放之前。

## 6. Feature allocation

Feature create 是 cold path。

默认允许一个清晰 heap owner。

禁止无 profile 就实现：

```text
Feature SBO
custom Feature arena
generic polymorphic allocator
```

## 7. FeatureContext

attach 时获得窄能力：

```text
RenderFeatureContext
RenderSceneContext
```

它们是 non-owning scope/context。

不得 expose：

```text
VulkanBackend&
RenderRuntime&
EngineContext&
Scene ECS
```

## 8. 三类 dependency 不混用

### RenderData input

某个 backend sink 接收：

```text
RenderDataTypeId
```

### SceneCapability

某 Feature/provider 提供：

```text
SceneCapabilityId
```

### RenderGraph resources

某 pass 声明：

```text
RG logical read/write
```

Feature descriptor 的 `provides/requires` 只用于 **SceneCapability**。

RenderData routes 和 Graph resources 有自己的 resolver/type。

## 9. SceneCapability 是 non-owning composition index

禁止 Capability registry 自己拥有 semantic resource。

正确：

```text
Provider Feature/domain resource
    owns MeshSceneData

Scene capability index
    capability id → provider/value projection

Consumer attach
    resolve once
    cache typed pointer
```

Execution：

```text
consumer.mesh_data->...
```

不每帧查 registry。

## 10. Provider 与 Effect 分层

例：

```text
MeshDataProvider
    provides MeshSceneData
    no graph passes

LightDataProvider
    provides LightSceneData

ForwardMesh
    requires MeshSceneData
    adds view passes

Shadow
    requires RenderableGeometry
    requires LightSceneData

Tonemap
    graph reads SceneColor
    does not require World RenderData
```

基础数据能力与效果能力因此在依赖 DAG 上自然分开。

## 11. Provider resolution

默认一个 SceneCapability 只能有一个 active provider。

缺 required provider：

```text
Scene/Feature composition fails explicitly
```

多个 required-equivalent provider：

```text
AMBIGUOUS_SCENE_CAPABILITY
```

V2 不实现 generic priority/fallback/multi-provider resolver。

真实需要出现后再设计。

## 12. 同画面组合

```text
Mesh
PointCloud
Terrain
Gizmo
UI
future representation
```

必须：

```text
same RenderScene
same View
same RenderGraph
same target chain
```

禁止独立：

```text
PointCloudRenderer
MeshRenderer
SpecialRepresentationRuntime
```

拥有自己的 scene/frame/target lifecycle。

## 13. Scene-global / View-local behavior

Concrete Feature 可选择提供：

```text
beginSceneFrame
addScenePasses
beginView
addViewPasses
```

不存在的方法无 thunk/no-op requirement。

不创建 `SceneFeature` / `ViewFeature` 继承体系。

## 14. Per-view persistent state

Feature 可持久拥有：

```text
per-view HZB
extent-dependent cache
history
descriptor state
```

V2 只保证：

```text
View create/remove/resize
GPU-safe retirement
```

不实现 Universal Temporal History framework。

## 15. Virtual/streamed resources

V2 只保证底层 seam：

```text
persistent GPU state
continuous typed UPLOAD
GPU feedback/readback
partial domain resource update
```

不实现 Universal VirtualResourceManager。

## 16. 参数

参数属于 cold/control plane。

允许 static/generated metadata；runtime reflection 只用于 tooling cold path。

`applyParams` 明确返回：

```text
HOT
GRAPH_INVALIDATION
RESOURCE_REBUILD
REJECTED
```

## 17. Capability 何时实现

最小 Feature API 阶段 **不先实现完整 SceneCapability framework**。

顺序：

```text
minimal concrete Feature
→ typed RenderData
→ Mesh
→ PointCloud/mixed rendering
→ 至少两个真实 provider/consumer domain
→ 再冻结最小 capability resolver
```

避免凭空设计。

## 18. 完成门禁

```text
0 abstract RenderFeature
0 inheritance-only IPointCloudFeature ABI
0 independent render_feature execution component
FeatureInstance lifecycle single authority
Capability index non-owning
execution uses cached typed pointer
mixed Mesh+PointCloud same View/Target
backend core 0 dependency on render_features
real external Feature SDK plugin
```

## 19. R2：数据类型与运行时 Feature 独立

`render_data` 独立公开 Mesh/Light/View 等 typed data contracts，绝不能要求安装 concrete Feature 才 include 这些类型。`render_features` 提供处理这些数据的 typed backend sink、provider 和 pass。`scene_render` 的 Projection 只能依赖 schema-only `render_data`，不依赖 built-in Vulkan Feature implementation。`render_content` 专注 cooked resource representation/codec，不与实时 state payload 混用。

## 20. R2：Feature 与 Capability 的生命周期先后

Capability index 不拥有对象，但消费者绑定的 typed pointer 不能在运行时 provider 替换/卸载后悬空。V2 默认只允许在 RenderScene 安全装配点进行 provider 变化；拆卸时先撤销依赖消费者，再撤销 provider，代码 pin 覆盖最后一次 callback 与析构。不得在热路径为此加入 `shared_ptr`、频繁 `find<T>()` 或一层新的 Manager。
