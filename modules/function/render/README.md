# Render：Feature、View 与运行资源

渲染模块接收渲染领域的数据并执行绘制。它不拥有游戏 World、ECS 业务选择、编辑器历史或持久对象 ID。

## 目录与依赖

| 目录 | 职责 |
| --- | --- |
| `client` | 后端无关的请求协议、Feature 描述、生成代理及客户端生命周期 |
| `features` | 具体渲染能力、shader、pass 和操作处理 |
| `vulkan` | 当前 Vulkan 后端、执行、资源与 GPU 同步 |

Render client 的公开协议可以供不直接链接 Vulkan 的生产端使用。Vulkan 类型和实现不应经由相机组件或通用 Scene 元信息泄漏到无渲染程序。

## RenderFeature 是效果扩展点

相机数据、Mesh 绘制、高亮、阴影等具体能力通过 Feature 及对应协议接入。

新增效果不要求给 Scene、RenderSystem、Editor 或通用 RenderView 增加一个同名业务方法。已有 `FeatureCatalog`、生成式操作和依赖装配承担扩展责任。

Feature 自己拥有对应渲染资源和配置，使用现有 ResourceRegistry。不要为外部对象来源重新给 RenderScene 加一个通用 EnTT Registry。

## View 不等于 Camera

View 表达一次渲染视图及其目标、尺寸、代次和生命周期。相机表达世界的观察方式。

当前 `StandardViewCameraFeature` 拥有每 View 的相机状态；核心 View 不必硬编码三维相机矩阵。

[`ViewCameraUpdatePayload`](client/include/lux/engine/function/render/client/features/view_camera/ViewCameraOperation.hpp) 已包含 RenderSceneId、完整 ViewHandle、矩阵和坐标原点。这里的 ViewHandle 是运行时句柄，不是可保存的相机身份。

Scene 中应通过 Camera 组件及其提取阶段产生这些数据。Editor 的 CameraMan 与游戏用户 Camera 共用这条接线，差别在上层实体 owner 和输入来源。

## 身份与资源 owner

| 身份 | 意义 |
| --- | --- |
| RenderSceneId | 运行中的渲染场景 |
| ViewHandle | 带代次的视图 |
| RenderEntityId | 生产端给定的完整来源键 |
| RenderObjectHandle | 渲染器内部对象及代次 |

来源键不等于对象槽位。Mesh 与 Light 的来源关联分别由 InstanceResources 与 LightResources 维护；匿名渲染对象也必须保持合法。

Entity 接入时保留完整代次，并以所属 RenderScene 区分来源域。渲染模块不将 WorldObjectId 的哈希用作运行时对象身份。

## 高亮与选择独立

高亮是一种视觉效果。编辑器选择、游戏任务目标、交互提示等都可以驱动它，Feature 不应知道哪个对象是“编辑器当前选择”。

目标设计是通过 Highlight Feature 自己的协议提供对象集合与样式，由该 Feature 拥有集合／遮罩和 GPU 使用寿命。

```text
任意调用方决定强调哪些对象
  → Highlight 操作与有限提交
  → Feature 解析有效渲染对象
  → 高亮绘制
```

不得让 Editor 与普通 Mesh 更新分别重写同一个完整 flags 字段，从而互相覆盖。效果状态的写入责任必须唯一。

当前实现已有 HighlightFeature，并通过实例 flag 标记高亮；Feature 自有对象集合的协议尚待实现。迁移时需要处理现有消费者和 shader，不将目标协议冒充当前 API。

效果关闭或清空集合不修改作者 Mesh、Material，不生成内容 Undo。对象销毁或句柄代次变化后，旧集合不能高亮后来复用槽位的对象。

## GPU 拾取的适用范围

当前 RenderCluster 的拾取机制面向其管理的对象。它不等于当前普通 Scene Mesh 已经具备完整鼠标拾取。

GPU 拾取可以用于明确要求匹配渲染像素的功能，但不是通用 World 查询基础，也不是脚本 raycast 的前置依赖。本设计不因编辑器选择而强制提取或安装新的 PickingFeature。

默认运行时射线查询由活动实体的空间与几何数据完成。没有 Renderer 或相机，脚本仍可直接查询射线。几何命中与材质裁剪后的像素命中需要分别描述。

## 发布、采用与 GPU 完成

以下事实必须分开：

1. 生产端准备了更新。
2. Program 客户端接纳了更新。
3. 渲染后端采用了更新。
4. GPU 完成了相关使用。
5. 图像可能被平台显示。

客户端接纳、引用计数为零、关闭意图或 `noexcept` 都不是 GPU 完成证明。实际 CPU 引用与真实完成水位共同决定资源退休。

暂时背压保留原输入重试。后端停止意图与可靠退休事实分开；不能在尚未完成时丢弃 owner，也不能在后端已退出后永远等待新的 drain 被接纳。

## 扩展时保持的约束

- Feature pass 声明实际图像、buffer 和跨 pass 依赖。
- 不以 `waitIdle()` 掩盖缺失的同步。
- Program、Control 和 Upload 沿现有有限预算与 FIFO 推进。
- Shader、资源描述和协议代码生成留在对应 Render target。
- 生成的 ImGui、CameraMan、选择和拖放代码不进入渲染后端或游戏公共依赖。

相关说明：[Scene RenderSystem](../../../engine/scene/builtin_systems/render/README.md)、[Editor Rendering](../../../engine/editor/rendering/README.md)、[空间查询](../../../engine/domain/spatial/README.md)。
