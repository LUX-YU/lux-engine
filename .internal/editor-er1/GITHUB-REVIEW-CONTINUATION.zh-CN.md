# GitHub 审阅续行 G01（2026-09-10）

审阅起点 dee904683e0398945b59c69168f4336e40ecbbe2。附件为源码推导，以下运行由实施方执行。
实际工作区 E:/lux-er1/src；既存 engine.zip、modules.zip 未修改。原 main/Script 文件单独记录于 review-github-01/protected-main.json。

扩展真实 view_failure 用例：无效 RenderScene 1000/1001 产生 scene::NotFound；比较 code、RenderError type/args、ViewId、request、backend_status。依次 nonzero resize、隐藏、恢复、原尺寸、camera。记录状态、序号、owner/descriptors/events，随后正常渲染并关闭全部 owner。

修前 all 构建成功；测试 exit=1（明确断言收集后退出，无超时/崩溃）：FAILED(6) 被覆盖为 RESIZING(2)/SUSPENDED(3)，序号1→5，resize/camera错误丢失，acquireImage返回NOT_READY(2)。末尾原有 GPU PASS 行只表示渲染/关闭子路径，整体 G01 失败以退出码和 G01 FAIL 为准。
修后 all 构建成功；同一用例 exit=0。四次请求保持FAILED及序号1，全部原错误身份一致；camera亦返回原失败。View owner2/2、descriptor0/0、render event2/2；后续正常场景图像两个checksum与修前一致，最终descriptor2/2且所有owner释放。

修正仅在 owner线程/关闭检查之后、普通参数/重复尺寸短路之前拒绝 FAILED。重建仍要求明确关闭并openView。

原始证据：E:/lux-er1/review-github-01/raw/g01-{before,after}{,-build}.log；图像分别在images-before/images-after。
这是工作树定向验证，尚非本轮最终clean clone/SDK资格。G02–G04与原剩余门槛继续执行，ER-1未通过。
