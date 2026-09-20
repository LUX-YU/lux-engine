# ECS：动态身份、组件与持久化边界

ECS Registry 是当前实体和组件的运行时存储。本文约束身份、组件访问和相机组件的设计，不将拟议接口描述为已经实现。

## Entity 的语义

[`Entity`](core/include/lux/engine/simulation/ecs/Entity.hpp) 是带代次的 64 位动态身份。保留完整位模式才能识别槽位复用。

- Entity 只对所属 Registry 有意义，不是跨 World 的全局 ID。
- 查询、脚本、运行时选择和组件访问使用 Entity。
- `NullEntity` 表示无对象，不以整数零代替其契约。
- Entity 的原始位模式不能持久化为对象身份；现有类型已禁止通用枚举序列化绕过 ECS 语义归档。
- 在途操作同时绑定原 owner／Scene 代次。两个 Registry 即使具有相同 Entity 位模式，也不是同一个对象。

[`WorldEntityMap`](schema/include/lux/engine/simulation/ecs/WorldEntityMap.hpp) 负责静态身份与实例的转换，不负责分配 Entity，也不应成为每个动态实体必须登记的表。它的存在不能导致 raycast 返回 WorldObjectId。

## Registry 是当前值的唯一来源

直接修改 typed component 是正常的编辑和系统操作。需要区分写入内存与发布变化：普通成员赋值不会自动产生 EnTT 更新通知。

组件写入完成后，调用既定 `patch`／变化发布入口，派生系统、索引和渲染提取才能看到脏变化。Undo／Redo 使用同一条通知路径。

结构变更可能使组件地址、引用和迭代器失效。跨帧操作保存 Entity 和字段身份，并在使用时重新解析；不能把可失效的组件地址当作稳定对象身份。

## 解码值与安装组件分离

[`DecodedComponent`](schema/include/lux/engine/simulation/ecs/DecodedComponent.hpp) 拥有已经解码的值及其正确析构／安装操作。准备阶段不为了暂存值而建立一个临时 Registry。

结构编辑在接纳前验证实体与引用；提交时按已验证计划创建 Entity 并安装组件。值及其代码 lease 的寿命必须覆盖实际析构。

EcsCommandBuffer、闭包列表以及不抛异常的 move 都不是自动原子事务。原子性只能由完整准备、提交约束和通知时序证明。

## 作者组件与派生组件

| 数据 | 保存策略 |
| --- | --- |
| 作者 Transform、Camera 参数等正式 schema | 经作者 codec 保存 |
| 派生 WorldTransform | 从作者数据重新计算 |
| GPU 资源、CameraView 等运行时绑定 | 重新建立，不保存 |
| Editor CameraMan 与编辑器局部状态 | 不进入游戏作者内容 |

不能用“Registry 中存在某组件”推导“此组件必须被写入 World 文件”。

## Camera 组件的设计意图（待接线）

相机应成为实体组件，不再由 UI 私有对象维护另一份相机位置和投影。

```text
实体
  ├── Transform2D / Transform3D
  ├── Camera2D / Camera3D：投影参数
  └── CameraView：渲染集成层建立的临时关联
```

Camera 参数是后端无关的数据，可以没有 Renderer 而存在。标准三维相机可以提供透视和正交投影；固定投影集合适合值类型或 variant，插件定义的新相机不要求加入一个覆盖所有未来类型的全局 variant。

Camera 不再重复存储已经由 Transform 表达的位置和朝向。输出尺寸来自 View，不把窗口 resize 写成作者相机内容修改。

`CameraView` 属于 Scene 的可选渲染集成层，不放入 ECS core 或通用 Camera 数据头。它携带 RenderScene 和完整 View 代次；它不拥有 GPU 资源，也不参加持久化。

未来 Camera 组件与 schema 应放在本目录下的独立 `camera` 子模块，遵守 `include/pinclude/sinclude/src` 的基本结构。该子模块当前尚不存在，本文不是已生成或已安装相机 API 的声明。

## 层级与空间能力

Parent 是否存在由正式描述与能力决定。不能把所有实体都视为三维对象，也不能为了统一编辑操作强制所有实体加入父子树。

运行时实体可能只是逻辑、声音、二维对象或其他领域数据。空间查询只处理具有对应空间数据并满足过滤条件的对象。

## 实现约束

- 固定对象优先直接持有；动态多态只用于真实开放的组件／provider 边界。
- 不引入全工程 OOM 转换或专门恢复流程；内存分配本身是允许的。
- 业务失败、无效身份和关闭过程仍须准确表达，不能以“不处理 OOM”取消这些检查。
- 公共通知发生在组件与历史状态一致之后；通知栈内不销毁仍在执行的 owner。

相关说明：[持久身份](../../world/README.md)、[运行时查询](../../spatial/README.md)、[Simulation](../README.md)、[Scene Editor](../../../editor/editors/scene/README.md)。
