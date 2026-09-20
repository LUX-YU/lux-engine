# Scene Render：通用系统推进与 Feature 提取

本模块把活动 Scene 数据连接到渲染客户端。它属于可选的内置 SceneSystem，不是 Scene 核心必须携带的能力。

## 现有类型的责任

| 类型 | 职责 |
| --- | --- |
| `RenderSystem` | 通用稳定点发布与提取流水线推进 |
| `RenderSystemMetadata` | Render Feature 的配置和 Scene 接线元信息 |
| `RenderFeatureSceneBinding` | 将 Feature 注册到对应组件观察与提取阶段 |
| `RenderSyncPipeline`／`RenderSyncStage` | 脏状态准备、提交和失败保留 |
| `SceneRenderBinding` | 渲染 Scene 创建、Feature 装配、有限提交及关闭 |
| `SceneRenderInput` | 建立实际 producer 所需的窄输入 |

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

## 相机接线的目标（尚未完成）

相机实体具有通用 Camera 参数和 Transform。运行时另附 CameraView，将该实体与 RenderSceneId／ViewHandle 关联。

CameraView：

- 使用渲染模块的完整代次句柄，不使用 Editor 专属 View 身份作为游戏组件。
- 只在有效绑定期间存在，不写入作者文件、快照源或 Run 捕获。
- 是关联，不是 GPU 资源 owner。
- 相机或 View 退出时停止新的使用，仍由实际 lease owner 完成资源退休。

对应 CameraRenderStage 通过既有 `RenderFeatureSceneBinding` 注册，观察相机参数、派生 Transform、View 尺寸和绑定变化，生成 ViewCamera Feature 的数据。

这样新增另一种相机时扩展其组件／Feature 接线，不修改 RenderSystem 的公开业务方法。

## View 与相机的生命周期

```text
宿主明确选择 Camera 与输出
  → 已有 View 创建和 owner 接纳
  → 建立 CameraView 关联
  → Camera 提取进入 Program
  → 允许该 View 绘制场景
```

退出时先封住新的相机／View 使用，再解除关联并推进原 owner 关闭。移除 CameraView 组件不等于 GPU 已经停止访问对应资源。

View 的尺寸变化是输出事实；它使相关投影数据失效，但不修改持久相机的 FOV 或位置。coordinate page size 必须来自实际 Scene 配置，不能使用另一处写死的常量。

未指定有效 Camera 时不采用默认矩阵冒充用户相机。存在窗口输出时应清除旧场景画面；Editor 可以额外显示缺少相机的提示。

当前还由 Editor 的私有 SceneCamera 通过 RenderView 提交矩阵。此路径需要迁移到组件提取，迁移完成前不能声称 CameraView 已经是 ECS 正式能力，也不能同时保留两个相机数据写入者。

## Mesh 与 Light

提取使用当前 Registry 中的完整 Entity 作为来源，转换为渲染来源键时保留代次。渲染端资源 owner 负责自己的对象句柄和关联。

WorldObjectId 不进入每次提取或渲染更新。持久引用解析发生在内容／资源恢复边界，不能为没有静态 ID 的运行实体补 UUID 才允许它显示。

高亮由独立 Feature 接收对象集合并管理效果状态，不给 RenderSystem 增加选择表，也不在此维护 Editor 专属选择组件。

## Main 推进与背压

当前 Editor 中作者 Scene 和 Run 都由 Main 推进。相同线程上的准备与提交不需要再套一个跨线程 SPSC Consumer。

准备好的 StateUpdate 保留到实际接纳；失败或背压不清除尚未发布的脏状态。Main 每轮有限尝试并返回，必要更新未被接纳时不无限推进下一模拟步。

Main 到 Render 线程的可靠 Program 运输继续存在。删掉同线程中转不意味着删除后端 FIFO、GPU 同步或资源退休。

## 失败事实与关闭进度

Binding 可以同时“记录过失败”和“正在关闭／已经关闭”。调用方每次推进后读取持久失败事实，不只检查是否恰好处于 FAILED 枚举状态。

关闭途中发生的后端错误必须进入所属 Run／文档最终结果；已有首个原始错误不被次生停止错误覆盖。

后端 STOPPING 表示停止意图，RETIRED 才表示相应清理完成。不能把未转发退休包计作成功转发，也不能在终局阶段继续等待不可能接纳的新 Program。

## 可选依赖

本模块依赖 Render client，通用 Scene composition 与 Scene meta 不反向依赖它。未选择渲染系统的游戏应能排除本模块和图形后端。

相关说明：[Scene](../../README.md)、[Camera 与 ECS](../../../domain/simulation/ecs/README.md)、[Render Feature](../../../../modules/function/render/README.md)。
