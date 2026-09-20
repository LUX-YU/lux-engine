# Spatial：加载分区索引与运行时空间查询

本模块的关键边界是：加载索引解决内容定位，运行时查询解决活动实体命中。两者不能因为都处理空间坐标而混成同一项能力。

## 当前已有的能力

[`Spatial2DPartitionIndex`](spatial2d/include/lux/engine/spatial/Spatial2DPartitionIndex.hpp) 与 [`Spatial3DPartitionIndex`](spatial3d/include/lux/engine/spatial/Spatial3DPartitionIndex.hpp) 保存坐标到分区的关联。

它们的输入是空间位置或范围，输出是 `PartitionOrdinal`。它们不保存 `Entity`、活动 Transform、碰撞形状或模型三角形。

```text
现有分区索引：空间范围 → 分区 → 读取内容
运行时对象查询：射线／点／范围 → 活动对象空间数据 → Entity
```

目前尚不能将这些分区索引称为完整的运行时 raycast 实现。

## 运行时查询的设计意图

查询范围是指定 Registry 中当前存在且满足过滤条件的实体。接口返回 `ecs::Entity`，不返回 `world::WorldObjectId`。

下面仅示意结果语义，不表示仓库已提供此类型：

```cpp
struct RayHit3D final
{
    simulation::ecs::Entity entity;
    Eigen::Vector3d position;
    Eigen::Vector3d normal;
    double distance{};
};
```

Entity 包含完整代次，只在所属 Registry 中有效。同步调用通过查询对象的 owner 确定 Registry；跨帧任务在请求外层保留 Scene／Registry 代次和请求身份，不能用静态 ID 掩盖实例已被替换。

命中结果不负责选择对象、编辑内容或建立历史。查询不修改 Registry，不创建实体，不隐式补载分区。

## 相机与检测分成两步

```text
屏幕坐标 + 视口尺寸 + 相机投影和姿态
    → 世界空间射线

射线 + 实体空间数据
    → 命中 Entity、位置、法线和距离
```

只有第一步需要相机。脚本、传感器、武器或地面探测可以直接构造射线，完全跳过屏幕和相机。

透视相机与正交相机生成射线的方式不同，但射线一旦形成，相交算法不需要再知道相机类型。二维空间可以提供点、区域、线段和射线查询，不应为了统一入口把每个二维查询都伪装成三维射线。

## 查询数据与加速结构

运行时查询需要对应领域的实际数据：

- 对象当前的空间变换。
- 包围体或其他粗筛结构。
- 精确检测所需的形状、模型几何或领域数据。
- 完整 Entity 代次及过滤条件。

具体实现应先复用已有数学、物理和资源设施。当前分区索引不因此升级为对象级索引。具体运行时索引归查询实现所有，随实体创建、删除及变换变化增量更新。

Mesh 的局部 BVH 可以按几何资产版本复用；实例移动只改变对象级空间数据，不需要重建同一模型的三角形 BVH。高频查询应允许调用方提供结果存储，不要求每次分配完整结果容器。

对象索引与几何 BVH 解决不同规模的问题，不能因为单个 Mesh 有 BVH，就宣称整个 World 的候选筛选已经无需扫描。

## 物理查询与可视几何查询

两者需要明确区分：

| 查询 | 典型数据 | 典型使用者 |
| --- | --- | --- |
| 碰撞查询 | Collider、物理形状、碰撞层 | 游戏脚本、传感器 |
| 可视几何查询 | Mesh、Transform、可选择过滤条件 | 编辑器拾取 |
| 二维点查询 | 二维形状、层次或排序规则 | 二维编辑视口 |

没有 Collider 的 Mesh 仍可能是合法的编辑器选择对象。可视几何的 CPU 命中也不自动等价于材质裁剪之后的实际像素命中。

## 稳定点与一致性

编辑器先完成字段写入、组件通知及必要的派生更新，再对相应版本的空间数据查询。不能让控件已经显示新 Transform，而查询索引一直停在旧位置。

Simulation 内的查询通过 TaskGraph 的访问约束与阶段依赖读取稳定数据。实现不能一边遍历索引一边无约束修改 Registry，也不能在查询回调中递归推进整个 Scene。

查询未命中表示当前查询集合中没有命中。未加载内容不是该集合的一部分；加载覆盖范围不是本接口的隐含返回协议。

## 脚本和依赖方向

脚本通过既有 Ability／代码生成接入查询能力，返回拥有型结果或受控的 Entity 值，不泄漏 Registry 指针与内部迭代器。

查询契约依赖数学与 ECS 身份。具体 Scene 接线可以使用 SceneSystem 的能力注册，但查询算法不依赖 Editor、RenderView、RenderSystem 或 Vulkan。

## 接线应验证的性质

1. 动态实体没有 WorldObjectId 也能被命中。
2. 实体销毁并复用槽位后，旧代次不会命中新实体。
3. 射线查询可以在没有相机和渲染后端时执行。
4. 加载索引不参与一次 raycast，不发生隐式 IO。
5. 透视、正交、缩放变换与大坐标有各自准确的数学结果。
6. 2D 查询保持自己的输入和结果语义。

相关说明：[World](../world/README.md)、[数学几何](../../../modules/core/math/README.md)、[Simulation](../simulation/README.md)。
