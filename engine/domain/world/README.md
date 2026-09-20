# World：持久内容、身份与加载边界

本文记录 World 子模块的长期设计意图。它不是开发阶段报告，也不表示下面列出的设计目标已经全部实现。

## 职责

World 描述世界中需要保存和恢复的内容；运行时内容物化到 ECS Registry，由 Simulation 的系统决定如何演进。World 的持久格式不应要求 Editor、Renderer 或图形后端存在。

本目录按以下职责组织：

| 子模块 | 职责 |
| --- | --- |
| `identity` | 持久对象身份 `WorldObjectId` |
| `description` | schema、分区器、分区与存储索引的描述 |
| `partition` | 内容归属、分区表及分区索引描述 |
| `storage` | 分区内容、存储格式与 codec |
| `asset` | World 资产的读取和编码边界 |

## WorldObjectId 不是运行时 Entity

[`WorldObjectId`](identity/include/lux/engine/world/WorldObjectId.hpp) 是持久内容中某些对象的静态身份。它用于恢复对象和持久引用，不是 ECS 分配的动态句柄。

| 问题 | 使用的身份 |
| --- | --- |
| 文件中哪个对象引用了另一个对象 | `WorldObjectId` |
| 将持久记录恢复成当前实例 | 加载边界建立的静态身份到 Entity 映射 |
| 当前 Registry 中访问哪个对象 | `ecs::Entity` |
| 射线命中了谁、脚本正在操作谁 | `ecs::Entity` |
| 保存运行中产生且明确要求持久化的对象 | 在持久化边界安排静态身份 |

普通运行时实体不要求拥有 `WorldObjectId`。脚本创建实体、空间查询、相机控制和渲染提取不能为了继续执行而先生成 UUID。

这里的“静态身份不进入运行时业务”指业务接口和对象操作不依赖它。加载、流送或保存所需的映射可以在相关边界存活；它不应扩张为运行时对象的第二套必备身份系统。

`Entity` 的位模式不能直接作为持久格式。加载后的 Entity 可以与保存前不同，跨实体引用需要通过加载／保存协议重映射。

## WorldDescription 是描述，不是活动 Registry

[`WorldDescription`](description/include/lux/engine/world/WorldDescription.hpp) 当前提供 schema、partitioner、分区数量、存储卷、分区表与索引描述。它不拥有所有活动实体，也不提供射线检测。

当前 `Scene::worldDescription()` 返回的是这份描述，实际组件位于 `Scene::registry()`。不能将这两个入口混为同一份数据，也不能为解释 World 概念再建立一份可写 EditorWorld。

## 分区索引服务于加载

分区表与空间分区索引解决“内容在哪里、应该加载哪些内容”。例如空间范围可以被映射成 `PartitionOrdinal`，随后由加载端点读取对应数据。

```text
加载策略
  → 查询分区索引
  → 取得内容位置
  → Process 读取与解码
  → 实际 owner 物化 Entity
```

分区索引不是动态实体包围体索引，也不提供三角形或碰撞形状。射线查询不遍历 World 存储目录，不隐式加载分区，不将分区号当作实体身份。

加载／卸载改变了活动实体集合，活动空间查询结构据此更新；两种索引之间没有“相同空间坐标就可以共用”的保证。具体边界见 [Spatial 说明](../spatial/README.md)。

## 空间与层级能力

World 不固定为三维、透视相机或父子树。

- 对象可用的组件和空间能力来自正式描述及其实现。
- 父子层级是否可用取决于相应描述与 schema；不能因为 Editor 支持 Outliner 就强制所有 World 使用 Parent。
- 2D／3D 的数据描述与相机的投影方式分开。三维内容也可以由正交相机观察。
- 多种空间能力共存时，消费者明确选择兼容能力；不能看见第一个 Transform3D 就把整个 World 判定为三维。

这些约束不要求在此增加一个覆盖所有未来世界的全局 `SceneType` 枚举，也不把窗口、鼠标或 Render View 写进 WorldDescription。

## 持久内容的边界

保存面向作者数据及正式 codec，不等于序列化 Registry 中的每一个组件。

需要保留的内容包括持久身份、分区归属、作者组件、尚未物化的内容和无法识别但必须透传的原始 payload。派生 Transform、GPU 句柄、CameraView、编辑器 CameraMan 和当前选择不属于作者源数据。

新建对象只有在被接纳为持久作者内容时才需要相应持久记录。删除与恢复必须正确维护引用，不能用当前 Entity 位模式代替这套协议。

## 当前实现与目标的差距

- `WorldObjectId`、World 描述、分区格式与组件 codec 已存在。
- [`WorldEntityMap`](../simulation/ecs/schema/include/lux/engine/simulation/ecs/WorldEntityMap.hpp) 当前承担身份映射，它不是空间查询索引。
- 现有 Editor／Run 对象目录仍有对持久身份的依赖，包括为新运行实体安排静态身份。这个调用关系需要收敛到持久化和编辑器需要的边界，不能作为新的运行时查询契约。

## 接线应验证的性质

1. 无静态 ID 的运行时实体仍可访问、查询和销毁。
2. 保存重开允许重新分配 Entity，同时恢复正确的持久引用。
3. 非层级 World 不因编辑器操作自动获得 Parent。
4. 运行时查询不触发 World 分区读取。
5. 编辑器临时对象和渲染句柄不会进入 World 资产。

相关说明：[ECS 身份与组件](../simulation/ecs/README.md)、[Scene 装配](../../scene/README.md)。
