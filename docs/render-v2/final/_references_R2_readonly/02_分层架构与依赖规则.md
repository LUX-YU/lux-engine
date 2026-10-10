# 02 — 分层架构与依赖规则 R2

## 1. 第一原则

Render V2 必须让 reviewer 仅看目录和 target DAG 就能回答：

```text
哪些是基础机制？
哪些建立在基础机制上？
谁可以依赖谁？
谁绝不能知道谁？
```

新模块形成单向 DAG。`sinclude`、私有 include 或“只是内部使用”都不能绕过依赖规则。

## 2. Functional Strata

### FOUNDATION

#### `render_core`

Render 领域稳定词汇：

```text
strong IDs
small enums
plain data contracts
error/value types
resource handles
RenderData stable identity
Feature stable identity/static metadata
SceneCapability stable identity/static metadata
Target semantic identifiers
backend-neutral DeviceCaps value projection
```

禁止：

```text
Vulkan
thread/queue
Engine
Scene/ECS
Editor
RenderRuntime
Feature execution API
built-in Feature
```

#### `render_transport`

依赖：`render_core` + 通用并发/内存/container。

职责：

```text
PROGRAM
CONTROL
UPLOAD
bounded queues
packet/blob/attachment ownership
request/reply
stop/wake
runtime-local operation/route ID
typed route binding
```

禁止：

```text
Vulkan
Scene/ECS
Mesh/Light/Shadow vocabulary
RenderGraph compiler
```

#### `render_graph`

只依赖 `render_core` + 必要 math/container。

职责：

```text
logical resource/pass
read/write usage
dependency graph
target semantics
logical resource lifetime
validation
```

禁止：

```text
Vk*
Vulkan barrier
Feature implementation
Scene/ECS
```

#### Vulkan Foundation

位于 `render_vulkan` 内部：

```text
device/
memory/
descriptor/
pipeline/
transfer/
retirement/
```

只提供 native GPU mechanisms。

禁止知道：

```text
RenderScene
FeatureInstance
built-in Feature
Scene/ECS
```

---

### EXECUTION

#### Vulkan Backend

依赖：

```text
render_core
render_transport backend endpoint contracts
render_graph
Vulkan Foundation
```

职责：

```text
VulkanBackend
SceneRenderer
RenderScene
RenderView
RenderTargetRegistry
FrameExecutor
VulkanFrameDriver
VulkanGraphCompiler/Recorder
backend typed dispatch
FeatureInstance runtime
```

Backend core **不得依赖** `render_features`。

#### `render_runtime`

依赖：

```text
render_core
render_transport
PRIVATE render_vulkan
```

职责：

```text
backend thread lifetime
Main-side endpoints
pump/progress
diagnostics
feature registration transaction host
terminal failure / stop
```

禁止依赖 built-in `render_features`。

---

### EXTENSIONS

#### Vulkan Feature API

**不是独立 `render_feature` component。**

稳定、backend-neutral 的：

```text
FeatureTypeId
FeatureDescriptor
SceneCapabilityId
provides/requires metadata
portable configuration metadata
```

归 `render_core`。

执行扩展面诚实地属于：

```text
render_vulkan/feature/
```

包括：

```text
RenderFeatureOps
FeatureInstance execution hooks
RenderFeatureContext
RenderSceneContext
Vulkan graph/pass extension surface
```

原因：当前只有 Vulkan backend；不得为不存在的第二 backend 预造 feature execution abstraction。

#### `render_features`

具体领域能力：

```text
mesh
material
lighting
point_cloud
shadow
terrain
postprocess
...
```

依赖：

```text
render_core
render_graph
render_vulkan Feature API
```

反向依赖禁止：

```text
render_vulkan → render_features
render_runtime → render_features
```

---

### INTEGRATION

#### `scene_render`

位于 `engine/scene/...`。

依赖：

```text
Scene system
ECS
render_core
render_transport public typed API
render_runtime
resource/asset domain
```

职责：

```text
RenderSystem
RenderProjection
RenderResourceDomain
World/ECS → RenderData
Scene View request/publication
```

禁止：

```text
Vulkan internal
concrete built-in Feature header
Editor
```

#### Product / Editor / UI / Plugin Composition

只负责：

```text
注册 Feature
注册 Projection
创建 View/Target
装配 builtin/plugin registrations
Editor tooling
```

不得反向成为 lower layer 依赖。

## 3. 真实 target DAG

```text
render_core
├── render_data
├── render_transport
├── render_graph
└── render_vulkan
      ├── Vulkan Foundation
      ├── Backend
      └── Feature API
             ↑
             └── render_features

render_runtime
├── render_core
├── render_transport
└── PRIVATE render_vulkan

scene_render
├── Scene/ECS/resource
├── render_core
├── render_transport
└── render_runtime

Product
├── render_runtime
├── render_features
└── scene_render
```

`render_graph` 与 `render_transport` 是 core 上方平级机制，不是彼此依赖。

## 4. 三类 Dependency 必须彻底分离

### RenderData dependency

跨线程/域边界输入：

```text
RenderDataTypeId
```

例：

```text
MeshInstanceUpdate
LightUpdate
ViewCameraUpdate
```

### SceneCapability dependency

Backend RenderScene 中持久存在的共享能力：

```text
SceneCapabilityId
```

例：

```text
MeshSceneData
LightSceneData
RenderableGeometry
```

### RenderGraph dependency

本次 compiled graph 内逻辑资源的 read/write：

```text
RGResourceHandle / semantic slot
```

例：

```text
SceneDepth
SceneColor
GBuffer
```

**禁止**：

```text
一个 requires[] 同时塞 DataTypeId / CapabilityId / Graph resource
一个 generic DependencyId
```

三类依赖拥有不同 type system、不同 lifetime、不同 resolver。

## 5. Stable ID 与 Runtime-local ID

稳定 identity：

```text
RenderDataTypeId
FeatureTypeId
SceneCapabilityId
```

来源：

```text
canonical semantic name + 全仓统一 stable identity primitive
```

runtime-local 紧凑 identity：

```text
RenderRouteId / OperationId
```

流程：

```text
canonical semantic id
    ↓ registration / ABI/layout validation
runtime-local dense route id
    ↓ composition bind once
BoundRoute<T>
```

hot path 只携带/访问 local numeric route，不做 canonical hash lookup。

禁止另外实现 Render 专属 stable hash algorithm。

## 6. Public include

```text
lux/engine/render/...
lux/engine/scene/render/...
```

最终禁止：

```text
lux/engine/function/render/client/...
lux/engine/function/render/features/...
```

`include/` public installed，`pinclude/` private，`sinclude/` 仅在同一明确 layer cluster 内共享；`sinclude` 不能穿层。

## 7. 建议 CMake targets

```text
render_core
render_data
render_transport
render_graph
render_vulkan
render_runtime
render_features
render_content
scene_render
```

**不创建**：

```text
render_feature
```

仅为了文件组织可以有 `render/vulkan/feature/` 目录。

## 8. Negative Architecture Tests

必须自动化：

```text
render_core      → Vulkan           MUST FAIL
render_core      → Engine           MUST FAIL
render_transport → Vulkan           MUST FAIL
render_transport → Scene/ECS        MUST FAIL
render_graph     → Vulkan           MUST FAIL
render_vulkan    → render_features  MUST FAIL
render_runtime   → render_features  MUST FAIL
function/render  → EngineContext    MUST FAIL
function/render  → SceneRuntime     MUST FAIL
function/render  → Editor           MUST FAIL
scene_render     → Vulkan internal  MUST FAIL
scene_render     → built-in Feature MUST FAIL
```

还需验证：

```text
Feature stable metadata 可从 render_core 独立 include
Feature execution API 必须依赖 render_vulkan，而不是伪 backend-neutral component
```

## 9. 基础能力与高层能力

基础：

```text
Core
Transport
Logical Graph
Vulkan Device/Memory/Descriptor/Pipeline/Transfer/Retirement
Backend Scene/View/Target/Frame
```

中层执行/扩展：

```text
RenderRuntime
Vulkan Feature API
Feature composition/capability resolution
RenderSystem/Projection
```

高层：

```text
Mesh data provider
Lighting
PointCloud
Shadow
Forward/Deferred
Postprocess
Terrain
未来高级算法
```

高层能力只能建立在基础机制上，不能把自己的 vocabulary 下沉到 foundation。

## 10. 长期复杂功能增长规则

未来增加高级功能，优先增加：

```text
typed RenderData
persistent Scene capability
domain resource
concrete Feature
RenderGraph passes
shader/algorithm
```

不增加：

```text
第二 RenderRuntime
第二 RenderScene lifecycle
独立 geometry renderer
独立 frame loop
Universal Manager
```

真正 remote renderer / multi-process renderer 属于执行架构变化，另立项目。

## 11. R2：领域 RenderData 的权威归属

`render_core` 不定义 `MeshInstanceUpdate`、`LightUpdate` 等具体业务 payload；只提供 ID、类型元数据、通用错误和绑定机制。业务端和 backend 都需要的**非 Vulkan typed schema** 放入明确的 `render_data` 契约 target/目录，而不是复用用于 cooked assets/codecs 的 `render_content`：

```text
render_core
  └── render_data (mesh/light/view/... typed wire contracts, NO Vulkan)
         ├── scene_render projection clients
         ├── render_features data sinks
         └── offline TOOLCHAIN/schema-only consumers
```

`render_data` 不拥有 Vulkan pipeline、descriptor、GPU Feature object、Scene/ECS。`render_transport` 不依赖具体 `render_data`；通用 typed encode 由公开 traits/codegen 在调用点实例化。`render_features` 可依赖 `render_data`，**`scene_render` 不可依赖 `render_features`**。独立 `render_data` 是真实跨 Scene/Feature/TOOLCHAIN 的 contract target（header-only 或少量生成源），不是新增 runtime owner。`render_content` 独立承担 cooked resource codec/content representation。

R0 原旧 `render_feature_client` 与 `render_standard_content` 的 TOOLCHAIN 实际消费属于迁移清单，不能因删 V1 后漏掉；在 R1–R9 对应合同逐项复建，而非拷贝 V1 Feature implementation。

## 12. R2：静态依赖图的两种方向

CMake `target_link_libraries(A B)` 表示 **A depends on B**。图中的箭头必须与这个定义一致；不要把运行时数据流箭头当成编译依赖箭头。新增 target 单向依赖测试须由真实 CMake File API / install consumer 验证，不能仅 grep include。
