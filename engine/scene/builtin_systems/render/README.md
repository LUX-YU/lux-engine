# Scene Render：通用系统推进与 Feature 提取

本模块把活动 Scene 数据连接到渲染客户端。它属于可选的内置 SceneSystem，不是 Scene 核心必须携带的能力。

## 现有类型的责任

| 类型 | 职责 |
| --- | --- |
| `RenderSystem` | 场景渲染使用权、资源需求、View 关联、组件提取和必要发布 |
| `RenderFeatureRegistration` | Render Feature 工厂、portable 配置 codec 和可选择性 |
| `RenderFeatureSceneBinding` | 将 Feature 注册到对应组件观察与提取阶段 |
| `RenderSyncStage` | Feature 的脏状态准备、提交和失败保留 |
| `RenderAssetSource` | 同一资产来源与版本的共享读取、上传和不可变几何 |
| 私有 `RenderAssets` | 当前完整 Entity 的请求关联、失效、采用和重试 |

这些类型不拥有 Editor 选择、鼠标状态或内容 Undo。

## RenderSystem 不随效果种类增长

不增加 `setHighlighted()`、`pick()`、`setShadow()` 等效果专用方法。不同效果通过已有 Feature 元信息、生成操作和相应提取阶段接入。

```text
RenderSystem
  → 已注册 RenderSyncStage
      → 对应 Feature 操作
          → RenderFeature
```

Scene 核心仍只提供通用稳定点；不能将效果逻辑搬回 `executePresentation()` 或同义入口。

## Camera 组件与提取

`Camera.hpp` 定义 PerspectiveProjection、OrthographicProjection 和 Camera。姿态唯一保存在 Transform3D／WorldTransform3D，投影参数使用固定 variant。`primary` 表示宿主选择默认游戏输出时的候选；零个和多个主相机由宿主准确处理，不按 Registry 遍历顺序选一个。

Camera 只保存 projection 与 primary。RenderSystem 的 View 关联记录保存完整 ViewHandle、Camera Entity 与实际输出尺寸，每个 View 单独计算宽高比；同一个 Camera 服务不同尺寸的 View 时不互写组件。组件 schema 沿普通注册与保存流程工作，不给 WorldDescription 增加相机字段。

私有 `CameraExtraction` 实现已有 RenderSyncStage，经 RenderFeatureSceneBinding 接到 ViewCamera Feature。它观察 Camera 与 WorldTransform3D 的脏事实，准备更新／解除关联，接纳后提交自身状态，失败保留脏状态。它不是另一个 SceneSystem；RenderSystem 提供 View 与 Camera 的关联，具体矩阵和 Feature 操作由提取阶段准备。

矩阵为右手系、相机局部 -Z 向前、+Y 向上，Vulkan 深度 0..1、图像 Y 向下。先以 double 减去相机原点，再转换矩阵和分页原点供渲染使用。

## View 与相机的生命周期

```text
宿主明确选择 Camera 与输出
  → 已有 View 创建和 owner 接纳
  → 在 RenderSystem 中建立 View → Camera Entity 关联
  → Camera 提取进入 Program
  → 允许该 View 绘制场景
```

退出时先封住新的相机／View 使用，再解除关联并推进原 owner 关闭。解除 View 与 Camera 的关联不等于 GPU 已经停止访问对应资源。

View 的尺寸变化是输出事实；它使相关投影数据失效，但不修改持久相机的 FOV 或位置。coordinate page size 必须来自实际 Scene 配置，不能使用另一处写死的常量。

未指定有效 Camera 时不采用默认矩阵冒充用户相机。存在窗口输出时应清除旧场景画面；Editor 可以额外显示缺少相机的提示。

Editor 帧封包不再生产相机矩阵。相机更新和场景内容沿同一个 Program 顺序发布；实际提交与 GPU 完成仍由渲染生命周期证明。

## Mesh 与 Light

提取使用当前 Registry 中的完整 Entity 作为来源，转换为渲染来源键时保留代次。渲染端资源 owner 负责自己的对象句柄和关联。

WorldObjectId 不进入每次提取或渲染更新。持久引用解析发生在内容／资源恢复边界，不能为没有静态 ID 的运行实体补 UUID 才允许它显示。

高亮由独立 Feature 接收对象集合并管理效果状态，不给 RenderSystem 增加选择表，也不在此维护 Editor 专属选择组件。

## Main 推进与背压

当前 Editor 中作者 Scene 和 Run 都由 Main 推进。相同线程上的准备与提交不需要再套一个跨线程 SPSC Consumer。

准备好的 StateUpdate 保留到实际接纳；失败或背压不清除尚未发布的脏状态。Main 每轮有限尝试并返回，必要更新未被接纳时不无限推进下一模拟步。

Main 到 Render 线程的可靠 Program 运输继续存在。删掉同线程中转不意味着删除后端 FIFO、GPU 同步或资源退休。

## 失败事实与关闭进度

RenderSystem 析构依次撤销阶段、断开观察、结束 Registry 借用并释放已接纳的 RenderSceneLease。lease 析构只登记释放意图，不申请命令槽、不泵 Main、不自旋。RenderRuntime 继续处理已接纳工作；View 与帧引用按各自用途保留资源。

资源收据分别保存持久失败和退休进度。CPU 系统消失后，晚到结果也只触达 runtime 记录，不调用已析构的系统。调用方在最终释放前仍采用收据中的首个失败。

关闭途中发生的后端错误必须进入所属 Run／文档最终结果；已有首个原始错误不被次生停止错误覆盖。

后端 STOPPING 表示停止意图，RETIRED 才表示相应清理完成。不能把未转发退休包计作成功转发，也不能在终局阶段继续等待不可能接纳的新 Program。

## 可选依赖

本模块依赖 Render client 和共享 RenderRuntime，通用 Scene composition 不反向依赖它。
工具元信息通过可选 Editor 插件登记，运行 Scene 不初始化反射目录。未选择渲染系统的游戏应能排除本模块和图形后端。

相关说明：[Scene](../../README.md)、[Camera 与 ECS](../../../../domain/simulation/ecs/README.md)、[Render Feature](../../../../../modules/function/render/README.md)。
