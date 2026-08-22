# Spatial3D Scene Catalog 所有权边界

## 目的

Spatial3D Catalog 曾把 cooked 格式、ECS streaming 策略和 Engine 产品装配
放在 `engine/spatial3d` 的同一个类型里。现在按字段性质拆分，目录和 target
共同表达 owner，不再保留 Engine 侧的平行 Spatial3D domain。

## Cooked 格式

`ecs/scene_format/spatial3d` 与 target
`lux::engine::ecs::spatial3d_scene_format` 拥有：

- `SourceId`；
- source/channel/LOD band；
- cell coordinate 与 `EntitySectionId` 映射；
- L3SC header 中的序列化 limits；
- `encodeSceneCatalog()` / `decodeSceneCatalog()` 与格式校验。

消费者直接 include：

```cpp
#include <lux/engine/ecs/scene_format/spatial3d/SceneCatalog.hpp>
```

## Streaming 策略

`ecs/spatial3d/streaming` 拥有：

- `ResidencyCapacity`；
- resident 与 visual-LOD demand channel 名称；
- cooked band 到稳定 demand-source identity 的映射；
- Section source、interest-to-demand 行为和对应 `ISystem`。

Toolchain 配置使用 streaming policy，并在写 L3SC 时显式投影为 wire limits；
Runtime 解码 L3SC 后再显式构造 streaming capacity。格式对象不是运行时行为对象。

## 产品装配

`engine/runtime/scene/composition/InstallSpatial3DSystems.hpp` 只提供普通的
直接装配函数。它在 World 发布前把已验证的 cooked catalog 转换成 ECS 配置并
加入 Schedule，不定义 Component、`ISystem`、catalog、host 或动态 installer
framework。

以下边界和 target 已删除且不得恢复：

```text
engine/spatial3d
engine/runtime/spatial3d
lux::engine::spatial3d::scene_catalog
lux::engine::runtime::runtime_spatial3d_streaming_systems
```

## 文件兼容

迁移不改变 L3SC v1 的 Magic、Version、字段顺序、Stable Name 或 UUID wire
表达。`spatial3d_scene_catalog_contract_test` 使用冻结的逐字节 fixture 验证解码
与重编码；旧 include、namespace 和 target 不提供 shim 或 alias。
