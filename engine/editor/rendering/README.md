# Editor Rendering：View、帧引用与相机接线

本模块拥有编辑器渲染前端与图像生命周期。它不拥有作者内容、世界选择、模型放置规则或组件历史。

## 类型责任

| 类型 | 职责 |
| --- | --- |
| EditorRenderer | Main 侧渲染服务、后端状态、有限回复推进与最终退出 |
| RenderView | 一个编辑器 View 的创建、尺寸请求、失败与关闭 |
| ViewImage／ViewImageLease | 图像版本、纹理引用和实际使用寿命 |
| EditorFramePacket | 已封装 UI Frame 及其完整图像引用 |

这些对象使用 Render client 和后端服务。通用 World／Simulation 不反向依赖这个 Editor 模块，游戏也不需要复用 EditorRenderer 才能显示场景。

## View 的公共语义

View 保存输出尺寸、句柄代次、图像和资源状态。不要为了每种效果给 RenderView 增加一个业务方法，也不要把它作为通用 raycast 接口。

渲染模块的 ViewHandle 与本模块的 RenderViewId 不同：前者是渲染协议身份，后者还标识编辑器 Renderer owner。Camera 的 view 字段使用渲染协议身份，不依赖 Editor 的类型。

## 唯一相机生产端

Camera 组件和派生 Transform 经 Scene 渲染集成的提取阶段生产矩阵。RenderView 只暴露实际 ViewHandle、目标尺寸与输出意图，不保存位置／方向／投影。旧 setCamera、CameraFrame 和封包时重复发送矩阵的路径已经移除。

ScenePane 在创建／resize 后建立 Camera.view 关联，关闭或切换目标时先解除关联，保留原 View owner 直到关闭完成。无有效用户相机时 setOutput 禁止场景层，后端用清屏 pass 清除旧内容；UI 层仍可显示。

ImageContentStamp 的 source 是请求者声明的来源，RECORDED／GPU_COMPLETE 表示对应帧的实际记录／完成，不等同于 CPU 当前世界已全部采用，也不证明物理屏幕显示。

## Feature 操作

编辑器的高亮需求沿 Highlight Feature 自己的生成式协议进入现有 Program 提交，不绕过 Main 线程契约，也不新增事件总线或单独渲染线程。

操作携带的外部数据在被接纳后有明确拥有者，不能借用已经离开绘制栈的临时 span。效果请求与普通 Frame 共用现有有限接纳和退休规则。

效果开关或选择变化不修改作者 Mesh／Material，不进入内容历史。GPU 像素拾取若有明确产品需求，可以是独立 Feature 能力；默认实体查询仍位于运行时空间模块。

## 同帧引用

```text
绘制 Pane → 结束 UI Frame → 收集实际图像引用 → seal
```

Pane 在绘制后被隐藏，本帧仍可能引用它的纹理。引用收集依据实际 image lease，不依据后来变化的 visible 标志。

seal 成功后由 packet 接管引用；提交背压重试原 packet，不重新执行整帧 UI 和业务修改。失败时保持必要输入与引用以供正确处理。

## 状态与错误

NOT_READY、FAILED、关闭 PENDING 是不同事实。View 的原始错误、请求身份和代次要保留到调用方，不压成泛化“准备中”。

Reopen 先关闭旧 View，再建立新 View；不能覆盖原 owner 或改变所属 Run 身份。关闭过程中仍须采用持久失败事实。

## GPU 生命周期

CPU 引用释放与真实 GPU 完成水位同时满足，资源才能退休。不能只用 shared_ptr 数量推断 GPU 已结束，也不能以 waitIdle 掩盖遗漏的依赖。

停止意图、后端停止、客户端清理与最终资源退休分别记录。后端终局时沿已有准确事实收尾，不等一个已不可能接纳的 drain，也不提前释放在途附件。

## UI 几何稳定性

加载提示、错误和统计行尽量保持稳定布局，避免 readiness 改变内容高度，内容高度触发 resize，resize 又使图像未就绪的反馈循环。

无新 UI 帧、Renderer 背压、Pane 隐藏和输入结束不能互相推断。交互归 UI owner，帧是否重绘不是用户是否结束手势的证据。

相关说明：[Render 模块](../../../modules/function/render/README.md)、[Scene Render](../../scene/builtin_systems/render/README.md)、[Scene Editor](../editors/scene/README.md)。
