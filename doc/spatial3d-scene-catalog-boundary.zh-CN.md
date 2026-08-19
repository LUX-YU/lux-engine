# Spatial3D Scene Catalog 所有权边界

## 目的

Spatial3D Catalog 同时选择 Engine Scene 的 Demand Channel、引用 ECS
EntitySection，并配置 World Partition 驻留预算。因此其规范所有者是
`engine/spatial3d`，不是公共 Resource SDK。

## 规范模型

`lux::spatial3d::SceneCatalog` 使用：

- `lux::scene::DemandChannelId`；
- `lux::ecs::scene_format::EntitySectionId`；
- `lux::spatial::GridCoord3i64`；
- Engine-owned residency policy。

Runtime 和 Toolchain 只能 include：

```cpp
#include <lux/engine/spatial3d/SceneCatalog.hpp>
```

不得直接 include `resource/spatial3d_scene`，也不得在 Runtime 中把 legacy ID
隐式转换成 canonical ID。

## 文件兼容

`encodeSceneCatalog()` / `decodeSceneCatalog()` 原生实现既有 L3SC v1 字节协议。
`modules/resource/spatial3d_scene` 暂时保留，只由兼容测试逐字节比较。
目录迁移不改变 Magic、Version、字段顺序、Stable Name 或 UUID wire 表达。

## 删除闸门

删除旧 Resource Component 前必须确认：

1. Runtime、Toolchain、Authoring 和产品代码均无 legacy include/namespace；
2. installed Engine Spatial3D target 的 public-link probe 通过；
3. L3SC v1 Golden Files 与交叉解码测试通过；
4. 外部兼容期已经结束，或旧 include 由明确的 SDK 版本策略处理。
