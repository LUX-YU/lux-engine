# UI：CPU 控件与可选渲染 Feature

`ui` target 提供 CPU Context、Frame、输入值、Pane 基础类型和控件，不依赖 Vulkan。`ui_rendering` 提供真实 UiRenderFeature 及其 GPU 后端，可供任意渲染宿主使用。

## CPU 所有权

Context 只拥有 ImGui Context、冷字体配置及字体／range backing。Frame 借用 Context 与 Theme；显式 finish 生成一份可捕获 CPU 帧，未完成 Frame 的析构只结束 CPU 帧，不生成待提交输出。输入按 Context 归属，失焦清理状态，文字锚点为平台无关值。

Pane 遍历、登记、布局和 CommandRouter 的产品组合由 Editor 的 UIRenderSystem 承担。CPU UI 不拥有 dispatcher 队列，不提供第二套事件系统。

## 异步绘制

UiFrameSnapshot 一次拥有型复制绘制结果，保留 DrawList 与顶点／索引／命令容量供下一次捕获复用。复制不是零复制；Context 可以先退出，GPU 不会再解引用 CPU shared data 或 viewport。

字体 atlas 作为拥有型像素及元数据交给 GPU。ImGui Vulkan Ex 的 create、draw、destroy 均不访问 CPU Context。普通绘制与 reset-render-state callback 支持；没有跨线程拥有协议的任意回调在捕获前拒绝，原有快照保持不变。

UiRenderFeature 通过标准 Feature 工厂与动态操作注册。它拥有字体、descriptor、顶点／索引 buffer 和 RenderGraph pass；快照、纹理引用及输出同属本次 Program 的帧输入。Clear 操作停止对最近快照的后续使用，在途 GPU 仍按真实完成水位退休。

本帧已经绘制的图像引用不能因为 Pane 随后隐藏而丢弃。接纳背压期间保留同一帧，不再绘制业务控件；上下文、原生窗口和 GUI 产品状态不进入 render_client。
