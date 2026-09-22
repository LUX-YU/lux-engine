# RenderRuntime：共享后端与已接纳工作的寿命

RenderRuntime 不包含 SceneDescription、Project、ImGui 或 Editor 类型。产品提供冷配置、Feature 工厂以及原生 surface 参数；一个 runtime 服务多个渲染 Scene，每个 Scene 拥有独立 Feature 实例。

## 所有权

- RenderSceneLease 表示一份使用权，取得前 runtime 已保存创建、Feature 附加和最终释放责任。
- RenderSceneReceipt 只观察创建、持久失败与退休，不额外保活 CPU Scene。
- RenderView 保存输出、尺寸、请求身份与关闭过程；ViewImage 持有实际本帧采样的图像引用。
- 已接纳 Program、CPU 帧和 GPU 工作各自保留所需资源，不能用 CPU 引用计数代替 GPU 完成水位。

释放意图写入既有记录，不需要普通命令槽。正常退休有限推进 Control／Program；后端终止则等待 Server 析构、线程退出、Main join 和已接纳 CPU 包清理后采用 RETIRED。停止意图不是完成事实。未提交包退休单独计数，不计作 forwarded。

RenderSystem、UI 系统或其他宿主可以先于 GPU 资源析构。维护不回调已经消失的宿主；正常关闭中途出现的失败仍保存在收据里。窗口 owner 必须活到其 surface 退休。

## 图与帧

RenderFeature 声明 pass 和资源使用。离屏 View 输出供另一个 Scene 的 UI pass 采样时，图根据实际图像版本、范围和访问关系生成依赖；同帧循环反馈拒绝。序号变化不等于图拓扑变化。

Program FIFO、接纳槽与 GPU FIF 是不同限制。有限回复消费、Control 接纳、Program 接纳各使用自己的预算；背压保留原输入。这里不增加 UI 专用命令或 Editor 手工 barrier。
