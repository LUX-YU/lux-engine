# Scene：World 与 Simulation 的装配边界

Scene 是运行时组合对象。它根据正式描述装配 World 内容、Registry、Simulation 与可选 SceneSystem，不承担编辑器窗口或具体渲染效果的业务。

## 当前对象与接口

[`Scene`](composition/include/lux/engine/scene/Scene.hpp) 提供：

- `worldDescription()`：持久世界描述。
- `registry()`：实际活动 Entity 与组件。
- `simulation()`：演化与派生执行。
- SceneSystem 查找、能力查询、稳定点及停止协议。

WorldDescription 不是 Registry。当前 API 不存在另一份拥有全部业务内容的 `World` 实例，文档不能把概念上的 World 与已实现类型混为一谈。

## 通用装配与具体能力

`composition/`、`description/`、`system/`、`meta/` 负责通用描述、依赖、provider 与生命周期。

`builtin_systems/` 和 `integration/` 放置具体系统接线。Render Feature 元信息属于可选渲染集成，不重新塞回通用 `SceneMetaManager`。

新增领域能力优先沿既有 SceneSystem 注册、窄 provider 和系统依赖接入。不要重新引入可以取得所有业务对象的万能 Context，也不为一个具体能力新建全局 Manager。

## 不由 Scene 核心执行呈现

Scene 不提供 `executePresentation()`、`updateViews()` 等承担相同职责的专门入口。

```text
Simulation 结束或作者派生完成
  → Scene 通用稳定点
  → 已安装系统各自执行其工作
```

RenderSystem 可以在稳定点提取并发布数据。Renderer 负责采用、绘制与 GPU 资源退休。这不要求 Scene 核心认识相机投影、高亮、阴影或拾取算法。

## 可选 RenderSystem 与相机

游戏场景的期望行为：

| 配置 | 行为 |
| --- | --- |
| 无 RenderSystem | Simulation 和运行时查询正常工作；场景不要求图形后端 |
| 有 RenderSystem，无活动 Camera | 不绘制场景内容；存在窗口输出时清为黑色 |
| 有 RenderSystem 和有效 Camera／View 绑定 | 渲染对应场景图像 |

无相机不能继续展示已销毁相机留下的旧图像，也不能暗中建立默认游戏相机。

Camera 参数是普通实体组件，CameraView 是可选渲染集成的运行时关联。相机数据通过对应 Feature 的 ECS 提取阶段进入渲染器；不会给 Scene 核心或 RenderSystem 增加每种效果的专用方法。

编辑器自身 UI 可以独立渲染。因此打开没有 RenderSystem 的文档时，Inspector、Outliner 和编辑器窗口仍可存在，场景区可以提示能力缺失。

## 查询不是内容加载

运行时空间查询面向当前 Registry 中的 Entity。World 分区索引服务于加载，不参与一次 raycast，也不决定查询结果的身份。

鼠标拾取先由当前相机生成射线，再使用空间查询；脚本可以直接提供射线。选择、高亮和拖放放置是上层对结果的使用，Scene 查询能力不带这些编辑器语义。

## 作者实例与 Run 实例

Editor 可以保留作者 Scene 并从捕获创建独立 Run，保护作者组件与历史。可共享的不可变资产和 GPU 资源按已有 lease／pins 保留，不因为有两份 Registry 就复制全部资源。

默认在同一视口切换作者与 Run。编辑时由编辑器 CameraMan 观察作者内容；Play 使用 Run 的用户 Camera；Stop 恢复编辑观察状态。CameraMan 不属于 Run 捕获和游戏导出。

上述相机实体与自动切换协议是待接线设计，当前私有 SceneCamera 不等同于它们。

## 建立与销毁

系统装配可能需要最终 Registry／Simulation 地址，并可能返回正常业务错误。保留真实 owner 和失败回收路径，不为了统一构造语法把这些失败藏起来。

关闭遵守依赖逆序：先停止新工作，退出借用者与 View，再销毁系统和 Registry，最后释放仍被引用的运行资源。异步关闭未完成时保留 owner；`requestClose()` 或 `requestStop()` 不是已经销毁的证明。

LuxObject 通知中可以请求逻辑关闭，物理删除发生在安全点，不能从回调中销毁仍在执行的方法所属对象。

## 构建边界

通用 Scene 的源码和安装依赖不能因为可选渲染而公开依赖 Vulkan。游戏导出根据实际系统与 Feature 选择依赖闭包。

仅从描述中移除 RenderSystem 不会自动移除已经链接的 DLL。无渲染消费者需要独立验证其配置、链接和运行时依赖，而不是以“没有创建设备”替代“没有后端依赖”。

相关说明：[World](../domain/world/README.md)、[Simulation](../domain/simulation/README.md)、[RenderSystem](builtin_systems/render/README.md)、[Scene Editor](../editor/editors/scene/README.md)。
