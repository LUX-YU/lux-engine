# GitHub 审阅续行 G01（2026-09-10）

审阅起点 dee904683e0398945b59c69168f4336e40ecbbe2。附件为源码推导，以下运行由实施方执行。
实际工作区 E:/lux-er1/src；既存 engine.zip、modules.zip 未修改。原 main/Script 文件单独记录于 review-github-01/protected-main.json。

扩展真实 view_failure 用例：无效 RenderScene 1000/1001 产生 scene::NotFound；比较 code、RenderError type/args、ViewId、request、backend_status。依次 nonzero resize、隐藏、恢复、原尺寸、camera。记录状态、序号、owner/descriptors/events，随后正常渲染并关闭全部 owner。

修前 all 构建成功；测试 exit=1（明确断言收集后退出，无超时/崩溃）：FAILED(6) 被覆盖为 RESIZING(2)/SUSPENDED(3)，序号1→5，resize/camera错误丢失，acquireImage返回NOT_READY(2)。末尾原有 GPU PASS 行只表示渲染/关闭子路径，整体 G01 失败以退出码和 G01 FAIL 为准。
修后 all 构建成功；同一用例 exit=0。四次请求保持FAILED及序号1，全部原错误身份一致；camera亦返回原失败。View owner2/2、descriptor0/0、render event2/2；后续正常场景图像两个checksum与修前一致，最终descriptor2/2且所有owner释放。

修正仅在 owner线程/关闭检查之后、普通参数/重复尺寸短路之前拒绝 FAILED。重建仍要求明确关闭并openView。

原始证据：E:/lux-er1/review-github-01/raw/g01-{before,after}{,-build}.log；图像分别在images-before/images-after。
这是工作树定向验证，尚非本轮最终clean clone/SDK资格。G02–G04与原剩余门槛继续执行，ER-1未通过。

## G02：粘性资源变化（第一组）

修前真实负例：raw/g02-before-03.log，exit=1。AssetReadPort实际接收provider的IO_FAILURE，另一个资源在真实render_client::compileShader准备分配失败；由SceneResources准备边界返回ALLOCATION_FAILURE。未运行新的renderer reply pump，重试同一个cycle后owner为UPLOADING/FAILED，公开snapshot却为READING/READING，revision1→1。显式关闭全部owner后准确失败退出。

修后raw/g02-after-resource_publication.log及g02-after-resource_snapshot.log均exit=0。前者覆盖实际shader请求分配，后者覆盖Scene快照构造分配。重试期间两个row状态与request key均未变化，公开revision1→2；真实direct通知只在完整snapshot发布后触发。再推进静止cycle，snapshot地址不变且不重复通知。最终图像checksum与修前一致，descriptor2/2，全部owner关闭。

pending_change属于SceneResources；每个row作用域记录异常退出前发生的状态变化，只有Session在owner gate内成功发布完整snapshot后确认消费。没有回滚异步完成、每帧复制或改动GPU释放保护。专用诊断接点只存在于LUX_EDITOR_SCENE_TEST_DIAGNOSTICS。

保留探索结果：g02-before.log因测试驱动沿用三实体断言而异常终止，不算合格负例；before-02实际命中snapshot分配并通过，不算复现。对应失败构建日志也保留；all失败后未运行测试。

本组尚未覆盖“先接纳READY再后续准备失败”的独立组合，将继续补齐；不据这两条结果将G02全部验收项标为通过。
