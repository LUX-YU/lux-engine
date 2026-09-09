# 同量成本验证发现的相机协议错误

ER-1 未完成；本记录不授予删除旧正式入口或进入 ER-2 的资格。

相同 seed、1600×900 窗口、1024×576 目标、单 View、三个资源、100 次预热和 500 个计量帧，
首次完整成本试跑两边都正常关闭，但实际 RGB 校验和不同：旧路径 11872603163728535614，
新路径 7978435335265164462。不得把这两份数据当作有效成本配对。

原始图像与统计位于 `E:/lux-er1/evidence/cost-smoke-04/`：
155281 个像素不同（26.3267%），最大单通道差 74，平均绝对差 1.03736。
`legacy.json.ppm` 和 `scene.json.ppm` 是实际 GPU 读回；PNG 只是相同像素的格式转换。
新图像的立方体正面有旧路径没有的阴影伪影，不是可以跳过的哈希波动。

核对实际生产 `ViewCameraOperationHandlers.cpp::buildViewFrameData`：
`render_origin` 被还原为 `camera_transform.position`，CPU view translation 从该位置与旋转重建。
新 `SceneView::synchronize()` 却发送向下取整到 1024 网格的原点，另在 view matrix 中保留平移。
GPU 投影与 CPU 的 shadow/cull 因而读到了不同相机位置。

修复仅在新 SceneView：发送完整相机位置及相对该位置的纯旋转 view，
由已有 RenderView 将完整位置拆为 page/local。未改 modules 或底层 shader。
真实 GPU 测试增加初始 origin=6/4/8、wire view translation=0 的检查。

修复后 `cost-smoke-05` 两个独立进程各完成 500 个计量帧，RGB 校验和均为
11872603163728535614，两个完整 PPM 像素一致。它只是工作树试跑，尚非最终五组资格。

成本测试入口的前两次失败也保留：首次等待旧 Presenter 未提交帧时没有走实际重试入口；
随后异步读回只有回复消费、没有推进 GPU 工作。两次均触发 90 秒断言，没有产出合格样本。
已分别改为原 Presenter 的 pending-submit 重试，以及两边相同的 8 个真实验证帧。
验证帧单独计数，未计入 500 个计量帧；等待时间包括这些读回验证工作。
不使用 waitIdle 调节帧率，不把 UI 循环次数当作完成帧数。

正式配对将记录工作/等待 wall time、OS owner/process CPU、owner 工作/等待 cycle 数，
以及精确 seed/source/DLL/EXE 身份。测试消费者驱动真实生产路径，
不声称覆盖完整 Application 事件循环、GUI 人工交互或 GPU 分配归因。
