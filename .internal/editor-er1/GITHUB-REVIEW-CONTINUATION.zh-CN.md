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

## G03：真实Scene坐标页配置贯通

采用RenderSystem已经用于Scene创建和Mesh/Light extraction的同一不可变page size，在创建SceneView时携同RenderScene身份传入RenderView。RenderView验证范围，camera origin分页与wire.coordinate_page_size使用该值；默认相机position/rotation-only view协议保留。

正式修前基线为g03-before-build-03.log成功all之后的g03-before-verified-coordinate_{1024,256}.log。1024通过；256明确exit=1，scene_page=256但wire_page=1024，两个实际GPU图像相同背景checksum13526231069286072813，最终所有owner关闭。不是超时/崩溃负例。

修后all成功；两个variant加base均exit=0。全场景（包括根Transform、子Mesh和独立point light）平移(256,-256,1024)，相机同步平移，覆盖页边界、负坐标及非零相机。256 wire page=(1,-1,4), local=(6,4,8)。1024与256在两个相机姿态下的PPM SHA256分别相同：9F1E3680...E0076C、C1093556...077B6；原base也同checksum。g03-image-hashes.json记录完整值。

同时验证zero/negative/max-double/infinity/NaN页尺寸在owner/request接纳之前准确返回INVALID_ARGUMENT。GPU mesh/camera movement、point lighting与实际画面覆盖成立；不推广为所有空间/cull模式的穷尽证明。

流程偏差保留：g03-before-build-02因120列检查失败，但批处理随后误执行了测试。该两份无verified后缀日志不用于资格；256还因旧的camera改变图像断言中止。这次偏差已纠正，修复驱动后重新all成功，再执行上述正常关闭的明确负例。不得删除或混用这些探索日志。

## G04：Workspace保留具体Scene错误

修前g04-before.log明确exit=1。真实无效RenderScene请求返回scene::NotFound(1001)，SceneView携带RESOURCE_FAILURE、DEVICE_FAILURE、request=1191182336及Session1；Workspace同步丢失来源。真实imageChanged direct回调内beginClose返回BUSY，Workspace关闭也丢失来源。两条都在收集准确错误后正常关闭全部owner才退出；View1→1、lease2→2。

修后g04-after.log两条preserved=1，所有身份匹配且资源计数相同。SceneWorkspaceResult仅用于业务同步/推进关闭边界，拥有WindowFailure或SceneFailure；没有向generic editor_ui添加Scene依赖。Application按具体来源继续转发。

g04-application-after-02.log由实际Application start/run/advanceShutdown执行。先消费实际Renderer diagnostic，再次run从Workspace收到同一View/request/render_error的SceneFailure，最后views=0/leases=0。原4组部分启动+Toolset销毁回归同时通过。首次Application探针遗漏BlockingScheduler配置导致start断言失败（after.log），已保留为驱动错误，不算负例资格。

CMake变更后的all/no-op见g04-application-build-02.log和g04-application-noop.log，后者ninja:no work。最终仍需绑定新clean源码重新资格。

## G02补充：真实READY后另一资源准备失败

增加resource_ready_publication。A实际完成mesh/shader/material上传，B的material provider读取被暂扣。诊断构建仅暂缓A的Registry接纳，持有实际GPU句柄（没有模拟完成或替代业务owner）；读取完成后解除暂缓，A正常接纳READY，再在B的真实shader请求准备分配失败。无新的renderer reply pump重试。

为复核旧算法，把459eeb3a的prepareUpdate函数精确取回，仅添加两轮共有的诊断接纳门；其余G01/G03/G04保持。g02-ready-before.patch记录与577ce66a的差异。old all成功、g02-ready-before.log exit=1：owner READY/UPLOADING，公开UPLOADING/READING，revision2→2。随后恢复修正，g02-ready-restored-build-02.log重新编译，restored-after-02.log exit=0：revision2→3，完整快照及direct通知一致，下一静止cycle不复制/不重发。两轮最终图像checksum相同、descriptor2/2、所有owner关闭。

探索前置条件未成立的超时/断言日志after至after-05均保留但不算负例资格。真实material reply有机会在另一个准备步骤内接纳，单靠外部poll后窥探未能稳定停在READY前；因此采用专用诊断接纳门固定顺序，上传和资源拥有者仍为实际路径。

另保留一次时间戳偏差：第一次Copy-Item恢复源码保留较旧mtime，Ninja误报no work，restored-after仍运行旧算法并失败；该次结果不属于修正产物。更新准确源文件mtime后all实际重新编译，形成restored-after-02的有效通过。最终clean clone资格排除此类增量历史。
