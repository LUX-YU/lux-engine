# L1 ECS Double Precision 与 Generated Component Decode/Emplace 前置实施规范（Baseline v3.1）

> 施工 Phase：**4**。Spatial/Render/Physics 的具体 consumer conversion 由后续 probes 验证。
>
> Public Type Budget：优先修改现有 Transform/math/schema types；只允许 paired `ComponentDecodeFailure`，不建立 generic materialization type family。

> 状态：Normative precision baseline。
>
> 目标：同时服务游戏、机器人、超大 3D/2D 场景；避免为了 GPU/solver 性能把 canonical runtime state 降为 float。

## 1. 核心原则

```text
Canonical semantic spatial state = double
Dense/local execution data        = float/native
Precision lowering                = explicit subsystem boundary
```

不使用两套用户 Transform component。

## 2. 用户-facing ECS Components

现有：

```cpp
Transform2D  -> Vector2f / float / Vector2f
Transform3D  -> Vector3f / Quaternionf / Vector3f
WorldTransform2D -> Affine2f
WorldTransform3D -> Affine3f
```

迁移为：

```cpp
struct Transform2D final
{
    Eigen::Vector2d translation{Eigen::Vector2d::Zero()};
    double rotation{};
    Eigen::Vector2d scale{Eigen::Vector2d::Ones()};
};

struct WorldTransform2D final
{
    Eigen::Affine2d value{Eigen::Affine2d::Identity()};
};

struct Transform3D final
{
    Eigen::Vector3d translation{Eigen::Vector3d::Zero()};
    Eigen::Quaterniond rotation{Eigen::Quaterniond::Identity()};
    Eigen::Vector3d scale{Eigen::Vector3d::Ones()};
};

struct WorldTransform3D final
{
    Eigen::Affine3d value{Eigen::Affine3d::Identity()};
};
```

TransformSystem hierarchy composition全程 double。

## 3. 为什么用户 API 全 double

不采用 mixed user Transform：

```cpp
Vector3d translation;
Quaternionf rotation;
Vector3f scale;
```

理由：

- canonical pose/transform更易理解；
- robot pose/kinematics通常需要 float64；
- hierarchy composition不反复 narrow/widen；
- precision optimization集中在 subsystem boundary；
- 小场景也能使用同一 API。

## 4. 哪些 canonical data 使用 double

至少：

```text
Transform2D/3D
WorldTransform2D/3D
camera authoritative/world pose
Spatial3D query source position
Spatial3D bounds/BVH/octree/grid coordinates
Spatial2D global placement
Toolchain authored spatial placement
World cooked spatial facts
robot map/odom/world pose data
navigation/terrain tile global origin
```

## 5. 哪些 data 默认保持 float/native

```text
mesh local vertices/normals/tangents
skinning/animation dense local buffers
particle local positions
terrain tile local vertices
nav tile polygon-local coordinates
GPU instance/shader payload
sensor point-cloud samples relative to sensor frame
pixel-field per-cell data
```

机器人使用 double pose不意味着百万 LiDAR points必须 double。

典型：

```text
sensor WorldTransform = double
point sample in sensor frame = float32
```

## 6. Explicit relative conversion

禁止：

```cpp
auto p = world_position.cast<float>();
```

必须：

```cpp
Vector3d relative_d = world_position - subsystem_origin;
Vector3f relative_f = explicitSpatialNarrow(relative_d);
```

原则：

> subtract in double, cast afterwards.

这些 conversion 必须收敛到可 grep/test 的显式 helper；baseline 名称：

```text
toRenderRelative
toPhysicsRelative
toNavTileLocal
```

不要到处散落 `.cast<float>()`。

## 7. 不做 Scene-wide floating origin mutation

禁止：

```text
camera移动远
 -> 给所有 Entity Transform减一个大 offset
 -> canonical Registry坐标跳变
```

Canonical Registry稳定保存 double coordinates。

Origin/rebase属于 consumer-side frame：

```text
RenderOrigin
PhysicsOrigin (only if backend needs)
NavTileOrigin
SensorFrameOrigin
```

这些 origin不是 Scene component requirement。

## 8. Render boundary

默认 GPU继续 float。

每个 Render View 建立自己的 double `world_origin`，通常 camera position/translated-world origin。

```text
object world double
 - view origin double
 -> relative double
 -> float GPU transform
```

RenderOrigin应 per-view，而不是 Scene-global：

```text
main camera
editor/debug camera
reflection probe
shadow view
VR view
```

可以各自选择最佳 origin。

只有特殊 planetary/geospatial shader未来证明需要 absolute high precision GPU coordinate时，才增加 high/low representation；不成为所有 shader默认成本。

## 9. Physics / Jolt

Legacy 3D physics backend已经是 Jolt，并明确作为 PRIVATE implementation detail；新架构继续保留该边界。

固定 integration direction：

```text
ECS canonical Transform double
    -> private Jolt integration using double-position capability
    -> Jolt internal/local solver representation
```

禁止 public API暴露 `JPH::*`。

若未来某 backend只能 float：

```text
world position double
 - PhysicsOrigin double
 -> backend float
```

反向 result widen + origin。

不要为了一个 float backend修改 canonical Registry。

## 10. Physics huge-domain limitation

如果 float physics backend需要同时模拟彼此相距极远区域，一个 local origin可能不够。

正确方案：

```text
double-position backend
or
multiple local physics worlds/domains
```

而不是整体移动 Scene ECS coordinates。

## 11. Spatial3D Partitioner / Index

`scene/spatial3d` owns concrete global spatial semantics。

所有 canonical index data：

```text
AABB centers/extents
sphere center/radius
node bounds
partition origin
streaming source positions
```

必须 double。

禁止 Partitioner使用 double但 cooked index降成 float。

## 12. 2D / Pixel compatibility

普通 Spatial2D global Transform使用 double。

Pixel/Noita-like world的 simulation chunk/cell coordinates可以：

```text
int64 chunk coordinate
integer cell coordinate
float/local presentation values
```

不强迫 pixel domain用 double per cell。

Canonical Entity Transform若用于把 PixelField放入 Scene，则仍 double。

## 13. Robotics frame semantics

Precision与reference frame是两个不同问题。

本轮只冻结：

```text
frame transforms use double-capable Transform infrastructure
```

不把 ROS `earth/map/odom/base_link` ontology写入 core。

未来 robotics package可以定义自己的 frame identity/authority graph。

## 14. Memory/performance concern

`Affine3d` 相比 `Affine3f` 有明显 cache/storage成本。

第一版 correctness baseline直接 double；必须 benchmark：

```text
100k / 1M Transform3D + WorldTransform3D
TransformSystem dirty update
hierarchy depth/wide tree
snapshot copy
render extraction conversion
```

如果 derived `WorldTransform3D` memory成为瓶颈，可以研究 compact double TRS/3x4 representation，但 public precision semantic不变。

不要在 baseline前做 mixed precision optimization。

## 15. Wire/toolchain

Authoring double spatial values必须以 stable schema的 double wire编码进入 World。

不依赖 C++ struct padding/layout。

Mesh/local geometry asset仍可 float。

```text
World object placement = double
Mesh local positions    = float
```

## 16. Tests / gates

```text
Transform2D/3D scalar double static assertions
WorldTransform double
large coordinate hierarchy accuracy
render relative conversion: subtract-before-cast
Spatial3D index large-coordinate query
World wire roundtrip preserves double
private Jolt integration exposes no public JPH types
1M transform memory/performance benchmark
source scan: no canonical spatial3d Vector3f/Affine3f
```

## v3.1 施工限制

Phase 4 中**precision 子任务**允许的 production public surface change 仅限：

```text
修改现有 Transform2D/3D
修改现有 WorldTransform2D/3D
修改现有 TransformSystem math
必要的显式 conversion free functions（优先 private/domain-local）
```

禁止为了 precision 建：

```text
LargeWorldTransform
HighPrecisionTransformComponent
FloatTransformComponent
DoubleTransformComponent
WorldOriginManager
FloatingOriginService
CoordinateContext
```

Render/Physics 需要 relative origin 时，origin 属 consumer-owned runtime state；不回写 canonical Registry 做 Scene-wide origin shift。


---

# Part II — Generated Component decode/emplace seam

## 17. 为什么必须在 L1 先完成

L3 `WorldMaterializer` 需要把 durable component payload 写进 EnTT Registry。

当前 `ComponentOperations` 已提供：

```text
has / get / size / erase / reserve
```

但没有 stable payload decode + concrete `registry.emplace<Component>()` 能力。

这个缺口必须在 L1 schema/codegen 解决；**不得等 L3 实施时再创建 `WorldComponentAdapter/MaterializationBinding`。**

## 18. Allowed public surface

优先修改现有：

```text
ComponentSchema
component schema code generator
ComponentSchemaSet
```

本 Part 唯一允许新增的 paired public error type：

```text
ComponentDecodeFailure
```

不得新增：

```text
ComponentMaterializer
ComponentCodecRegistry
ComponentFactory
WorldComponentAdapter
ComponentDecodeContext
MaterializationBinding
```

## 19. Exact operation contract

在现有 `ComponentSchema` 直接加入一个 generated function pointer：

```cpp
using DecodeEmplaceComponentFn =
    lux::cxx::expected<void, ComponentDecodeFailure> (*)(
        Registry& registry,
        Entity entity,
        std::uint32_t encoded_schema_version,
        std::span<const std::byte> encoded_payload
    ) noexcept;
```

`ComponentSchema` baseline shape：

```cpp
struct ComponentSchema final
{
    lux::cxx::TypeToken cpp_type;
    ComponentSchemaId id;
    std::uint32_t version{1};
    ComponentOperations operations;
    DecodeEmplaceComponentFn decode_emplace{};
    EComponentSnapshotPolicy snapshot{EComponentSnapshotPolicy::COPY};
    std::shared_ptr<const void> code_lifetime;
};
```

字段名固定为 `decode_emplace`；只允许代码格式/字段排列按现有 style 调整，不得把该函数放进第二个 registry/catalog。

## 20. Generated thunk behavior

对于 `Component`：

```text
stable encoded payload
    -> validate encoded schema version
    -> decode fields using generated/schema-aware code
    -> construct temporary Component value when needed
    -> registry.emplace_or_replace<Component>(entity, ...)
```

要求：

1. 不依赖 C++ padding/ABI 直接 memcpy durable payload；
2. 不在 hot path 逐字段做 `RefClass` 字符串查找；
3. unsupported version -> structured failure；
4. malformed payload -> structured failure；
5. `std::bad_alloc`/construction failure -> structured failure；
6. 失败时目标 Entity 不得留下半构造 component。

如果现有 EnTT API 没有 `emplace_or_replace`，使用当前 Registry 对应 typed operation；不要为此包一层 adapter。

## 21. `decode_emplace == nullptr`

语义固定：

> 该 Component schema 当前**不能从 generic durable World payload 直接 materialize**。

L3 如果发现：

```text
WorldDataSchema canonical name == ComponentSchema canonical name
```

但 `decode_emplace == nullptr`：

```text
返回 materialization failure
```

不得 fallback：

```text
RefClass::construct + field walk
RuntimeObject
raw memcpy
skip silently
```

## 22. v1 direct-materializable restriction

Generic direct thunk只允许：

```text
Registry-owned reversible Component state
self-contained durable payload
no external resource creation
no runtime Entity lookup requirement
```

第一版不支持 generic direct materialization 的典型字段/行为：

```text
runtime ecs::Entity references that require WorldObjectId resolution
Jolt body handles
Render handles
Audio handles
OS/process handles
irreversible external side effects
```

这些需求由 concrete System 在 materialization 成功后根据 ECS state 建立 runtime resource，或由后续 probe 证明需要新的 generic seam。

## 23. World schema 与 Component schema identity

`WorldDataSchemaId` 与 `ComponentSchemaId` **继续是两个不同 semantic type**。

v1 direct mapping 规则冻结为：

```text
WorldDataSchemaId.name == ComponentSchemaId.name
```

即可成为 direct component candidate。

不要新增：

```text
WorldToComponentSchemaRegistry
SchemaAliasManager
MaterializationPlan
```

World schema 在当前 Product 没有对应 ComponentSchema：

```text
ignore
```

这是为了允许 headless/server/robot product 不链接视觉 component package。

有匹配 ComponentSchema 但 payload/version 不可解码：

```text
fail
```

## 24. Cross-object references are not solved here

Generated component thunk只负责**一个 entity 上一个 component payload**。

它不负责：

```text
WorldObjectId -> Entity
Parent WorldObjectId resolution
cross-partition references
streaming dependencies
```

如果一个 concrete World schema需要第二遍 relationship resolution，先在对应 probe/concrete System 中实现 transaction-local逻辑；在出现至少两个独立 domain 之前，不抽成 generic registry。

## 25. L1 tests

除 precision tests 外，必须增加：

```text
generated simple component roundtrip
encoded version supported -> emplace success
unsupported version -> failure
truncated payload -> failure
allocation/construction failure -> no half component
null decode_emplace is observable
ComponentSchemaSet find(id/type) behavior unchanged
headless schema set can omit visual components
no RefClass hot-path fallback
```

## 26. Phase 4 exit gate

L3 可以只凭：

```text
WorldDataSchemaId canonical name
ComponentSchemaSet
ComponentSchema::decode_emplace
```

完成 direct Registry materialization。

如果 L3 仍然需要新建：

```text
Binding
Plan
Registry
Adapter
Context
```

来完成简单 World-record→Component，则 Phase 4 未完成，必须回到这里修 seam。
