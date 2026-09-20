# Math：射线生成与几何相交

本模块提供可独立复用的数学与几何算法，不认识 Editor、WorldObjectId、ECS Registry 或 Render View 生命周期。

## 两个不同的问题

屏幕拾取需要先得到射线，再检测相交：

```text
视口坐标 + 相机投影和姿态 → 射线
射线 + 几何数据 → 几何命中
```

相机只参与第一步。武器、传感器或脚本可以直接构造射线，调用第二步。几何相交不能要求 Camera、窗口或 Vulkan 实例存在。

## 现有代码

| 文件 | 职责 |
| --- | --- |
| [Ray.hpp](include/lux/engine/math/Ray.hpp) | 三维射线 |
| [Picking.hpp](include/lux/engine/math/Picking.hpp) | 视口坐标反投影成射线 |
| [Intersection.hpp](include/lux/engine/math/Intersection.hpp) | 射线与 AABB／三角形相交 |
| [MeshBVH.hpp](include/lux/engine/math/MeshBVH.hpp) | 单个模型局部空间的三角形 BVH |
| [Position.hpp](include/lux/engine/math/Position.hpp)、[RelativePosition.hpp](include/lux/engine/math/RelativePosition.hpp) | 位置与相对坐标基础类型 |

这些算法提供几何结果。几何属于哪个 Entity，由运行时查询 owner 维护，不应反向给 Math 增加 ECS 或持久身份依赖。

## 坐标约定必须显式

生成鼠标射线至少需要：

1. 鼠标在实际图像内容区域内的位置。
2. 对应视口尺寸与 UV 方向。
3. 相机投影矩阵及姿态。
4. 匹配的 NDC 深度与坐标方向约定。

透视投影与正交投影都可以反投影近远点形成射线。不能用“总从相机位置出发”的透视专用公式处理正交投影。

DPI 换算应在 UI／视口边界完成一次；几何函数不再读取平台窗口状态。矩阵乘法顺序、行列存储和相机原点必须与提供矩阵的代码一致。

当前 `Picking.hpp` 使用深度范围 `[0, 1]`，这是一项输入约定，不代表几何查询需要链接 Vulkan。

## 距离与变换

射线方向归一化时，参数 t 可以表示距离；如果变换后方向长度改变，必须明确换算。

将世界射线变到模型局部空间后，精确相交通常使用局部 BVH。非均匀缩放下要正确处理：

- 局部参数与世界距离的换算。
- 最近命中的跨实例比较。
- 法线的逆转置变换。
- 奇异矩阵与无效方向。

不能只用单位缩放模型验证世界空间命中。实现接线前必须用已知距离的放缩案例核对现有 `MeshBVH::intersect()`，不能照抄其注释作为数学证明。

## 精度

当前 `Ray` 使用 float；`screenToRay<double>` 最终也写入 float Ray。模板参数为 double 不等于输出已具有大坐标精度。

大世界查询应复用相对坐标设施，在足够精度下计算世界射线和原点偏移，再进入局部几何空间。不要先把大绝对位置截成 float，再期望 BVH 恢复丢失的精度。

需要扩展通用射线类型时在 Math 中完成，避免 Editor、脚本和渲染各复制一套不一致的几何公式。

## 性能责任

Mesh BVH 可按不可变几何版本构建一次并复用。实体移动不重建同一局部 BVH。

模型内部 BVH 不负责整个 Registry 的对象候选筛选。对所有实例逐一测试包围体仍有实例数量相关成本，应由运行时查询实现选择合适的对象级加速结构。

本模块不将空间分区加载索引当作几何加速结构，也不在查询中执行资源 IO。

## 应验证的数学性质

- 透视与正交投影下的中心、边缘射线。
- 不同视口尺寸和坐标方向。
- 射线平行于边界、起点位于包围体内及无效输入。
- 缩放与非均匀缩放下的距离和法线。
- 大坐标下的相对位置精度。
- 不依赖图形后端的几何检测。

相关说明：[运行时空间查询](../../../engine/domain/spatial/README.md)、[相机与 ECS](../../../engine/domain/simulation/ecs/README.md)。
