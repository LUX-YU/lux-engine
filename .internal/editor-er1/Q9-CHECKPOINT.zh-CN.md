# ER-1 q9 开发检查点：尚未通过，等待 modules 窄修范围批准

本文件记录本轮实际开发结果，不是统一资格报告，不升级原 acceptance-results.csv 的阶段状态。
基线为 `9e2efeb96653c61a319fdfda6be660e8135d506a`；工作树为 `E:/lux-er1/src`，
分支 `codex/editor-rebuild-er1`。检查点提交及归档身份见交付 manifest.json。
根目录未知 `engine.zip`、`modules.zip` 保留，未加入提交或归档；main 和并行脚本分支未修改。

## 已执行的开发验证

所有本轮运行均为 RelWithDebInfo 诊断构建。每个后期 phase 保留当时的完整改动源码、
Git 过滤后的 blob OID、Windows 原始字节 SHA256、全部 bin DLL/EXE 身份、命令日志与返回值。
它们是相应工作树快照的证据，不能改称后续 clean clone 的资格。

| 原门槛 | 本轮结果及准确范围 | 原始证据目录（evidence.zip 内） |
|---|---|---|
| C05 分阶段启动 | 注入线程准入拒绝、真实 device 初始化后拒绝、真实 attach 后拒绝；逐项保留错误 type/args，worker/server 计数配对，同 Window/UI 重试后实际 GPU 完成、显式关闭及 join。各独立进程 cases=1。不是实际 OS 线程资源耗尽。实例初始化失败仍有下列缺陷 | affected-01/raw/startup-*.log |
| R06 material 独立子失败 | mesh 和两份 shader 实际成功后，对匹配 shader 身份的 material 上传注入一次服务端拒绝，经真实 typed upload/reply 通道返回 status=1。错误对应原 ResourceRequestKey，无部分 adoption，兄弟 handle/request 归零，其他对象实际绘制。未声称 MaterialResources/Vulkan 自然分配失败 | material-02、affected-01/raw/gpu-material_subfailure.log |
| R06 shader 回归 | 原真实无效 ShaderInfo 服务端失败路径仍通过，未用它替代 material 用例 | material-02/raw/gpu-shader_subfailure.log |
| R07 value/stopped | 真实 pak/provider/endpoint/decode/ResourceTasks sender；分别 VALUE 和 CANCELLED，回调被保持时 32 次关闭仍 PENDING，caller 丢结果后 sender 仍拥有；回调返回后 scope、结果、endpoint、Execution 按序结束 | affected-01/raw/resource-completion.log |
| X09 线程 | 原 21 项 fallible owner 调用保持拒绝；新增资源账目查询在外线程读取可变 owner 前返回 WRONG_THREAD。未把 owner-only const/reference query 的调用前提改成跨线程安全承诺 | affected-01/raw/gpu-base.log、resource-memory-02 |
| 旧图像/resize/关闭 | 旧 packet 保持 32 步；迟到的第 2 请求回复不能覆盖第 3 请求；零尺寸保持 32 步；generation 1→5，backing 1→4；最后 GPU submitted=227、完成水位 225→227 后才释放 | resource-tails-01/raw/gpu-image_lifetime.log |
| 资源重试/背压 | 实际 missing-ground pak → 新 recovery mount → 新 sequence；旧失败快照仍有效。Control/Upload 容量 2 时保留关闭 owner，消费者暂停期间 8 次 PENDING，恢复后 COMPLETE | resource-tails-01/raw/gpu-retry.log、gpu-resource_backpressure_close.log |
| 有限资源账目 | 普通 3 请求，churn 峰值 5/容量 6、关联 3、峰值 handle 19、退役 marker 2；关闭请求/handle 归零。VMA 在实际 render worker 内采样，churn allocationBytes 峰值 418900064、allocationCount 90；设备显式退出通过既有 VMA 零泄漏契约 | resource-memory-02、resource-tails-01 |
| 受影响构建/CTest | 最后 all 构建成功，随后 ninja no work to do。Editor 命名组实际 16/17，通过之外的一个是保留的新增 forward-array 负例；整体 CTest exit=8，绝非全通过 | noop-01/raw/build.log、ctest-editor.log |

CPU 账目分别列 request 对象、vector 容量、结果对象、快照容量、mesh payload capacity、SPIR-V capacity、
关联项/bucket 数与活跃句柄。未计 shared_ptr 控制块、unordered_map 节点分配开销、ShaderInfo 字符串等；
不是进程总内存，也不把各时刻的独立峰值相加称同时峰值。关闭后的不可变快照容量 424 字节仍单列。
VMA 最后样本位于 backend teardown **之前**；不把它伪造为关闭后零字节。诊断采样成本不进入正式性能样本。

## 新发现的实际失败与待批准修复

以下 modules 生产修改尚未应用。依据 v4 §3.3 的 modules 白名单及 EX-01—05 边界提出窄例外；
不是重新设计总体架构。待批准补丁完整保存在本目录 `q9-proposed/`，审批前不作为生产修复交付。

1. **EX-06 / 实例创建失败**：实际请求不存在的 Vulkan instance extension 后，Loader 报 invalid instance，
   原调用链仍枚举物理设备，进程异常结束。`raw/startup-02.log` 是早期开发负例，缺完整当时 binary 快照，
   不作为最终资格。修复空 instance 检查、debug callback guard 和无 logical device 的析构 guard；
   使用既有 device.vulkan_object_creation_failed，不编造 builder 未保留的 VkResult。
2. **EX-07 / 共享 atlas**：`shared-06` 实际双 View、真实 MeshShadow caster、同一个 4-layer atlas，
   3 个 `VUID-vkCmdDraw-None-09600`，层 1–3 仍 UNDEFINED。编译图还显示 ForwardDraw 先于 ShadowDraw。
   最终 frames=137、gpu_completed=135、descriptors=2/2；错误返回前全部 owner 显式关闭，exit=1。
   原八个 barrier 用例通过，但新增真实 forward reference 解析为 array_layers=4 时仍得到隐式 layer_count=1。
   补丁限定隐式 whole-array range 延后解析、显式单层保持、ForwardDraw 的真实 ShadowDraw 依赖。
3. **EX-08 / preserve 内容层**：`preserve-03` 使用两个真实 Session/Scene，分别回读基础、覆盖、合成与恢复图。
   覆盖层实际绘出 3954 像素，但清掉基础层 175607 个内容像素；移除覆盖层后基础像素完全恢复。
   Vulkan validation=0，所有 owner 显式 COMPLETE 后 exit=1。非首层虽设置 preserve，仍携带 UNDEFINED
   initial_state，实际执行计划选择 CLEAR。待修正非首颜色层的交接初始状态，不使用 waitIdle。

`preserve-01/02` 的覆盖图尚未正确提交相机，不能作为非空覆盖业务的准确负例；第三次修正测试接线后才取证。
`shared-03/04` 在发现验证错误后驱动仍有早退/abort，原始记录保留；`shared-06` 才完成全 owner 关闭。
`resource-memory-01` 缺 VMA 头路径、`material-01` 私有 handler 跨 DLL 链接失败、`shared-05` 错误字段名等
均为开发构建失败，驱动没有继续运行旧 EXE。早期 startup-01 的单帧/FIF 驱动超时不登记为产品缺陷。

恢复普通示例原有 feature 声明次序、加入 material 诊断后，以最新二进制复核了两个相关负例：
`shared-current` 仍为三条同一 VUID，frames=136、gpu_completed=134、descriptors=2/2；
`preserve-current` 仍为 3954 个覆盖像素和 175607 个不匹配像素，移除覆盖层恢复原图，
frames=243、gpu_completed=241、descriptors=2/2。二者均全 owner 显式关闭后 exit=1。
待批准补丁已通过 `git apply --check`，未应用或声称修后通过。

## 隔离、剩余门槛和下一步

新增诊断入口仅受 LUX_EDITOR_DIAGNOSTICS 及现有组件私有宏控制。VMA 只在 owning render_vulkan DLL 中
读取真实 allocator；material 诊断只调用原 owning feature handlers，不复制其实现，不更改消息格式。
正常构建不加入这两个诊断对象；隔离脚本扩展到 9 个 DLL 的对象/PDB/export 检查。
**新检查点尚未形成对应的正常 SDK/诊断双产物最终资格，不能拿 q8 的 7-DLL 结果替代。**

IME 状态保持：**修复接线已实施、辅助测试通过、真实桌面验收受环境阻塞、q7 已知失败尚未获得关闭证据。**
本轮没有重试 Computer Use，没有将工具故障记为产品缺陷。真实 n→i→选词、内部字符与 glyph、持续 RMB/MMB
捕获跨失焦/隐藏/关闭仍待固定候选人工桌面记录；见 Q9-DESKTOP-ACCEPTANCE.zh-CN.md。

原入口删除门槛仍不满足，旧正式入口保留；新栈没有增加旧栈调用、fallback 或安装转发头。
关闭卡住请求的完整诊断、全部 owner-only query 调用链审计、同量 resize/retry 尾部五组独立进程配对，
以及固定候选 clean clone、安装迁移、9-DLL 隔离尚未完成。q8 有效原始成本、安装和隔离结果保留，未改标签。
当前不启动新的微优化，不进入 ER-2/3，不合并 main，不发布/冻结。

范围批准后先应用窄修并执行对应修后/共享双 View 顺序/FIF/resize/关闭负例，再固定新候选执行已有统一资格工具。
只在实施与本轮资格完成后按用户最新授权推送工作分支；本检查点不是这一完成条件。
