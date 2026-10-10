# R4 — Vulkan Foundation 工作单

用户正式授权 R4，禁止提前进入 R5。开工实际读取 origin/codex/render-v2，精确等于
`f9b943c9e768ef2c9d408962f0d5a32d2cf3de74`；R3 implementation 为
`ed4b78c1a4606f917eec1b30c654567935e6de48`。原用户 checkout 六处修改单独保存并隔离。

ALLOWED：`modules/function/render/vulkan/**`、bootstrap 必要接线、`docs/render-v2/R4_*`。
READ-ONLY：Core/Transport/Graph、全部 frozen Legacy、既有架构规范和历史报告。
FORBIDDEN：Root/产品、Engine/Editor/UI/ECS、Runtime/Scene/View/TargetRegistry/FrameExecutor/
Graph compiler/recorder、Feature execution、Projection、任何第二套高层框架。

按 owner/依赖组实现并提交：A Instance/Device/VMA；B memory；C descriptor/shader/pipeline；
D/E 有界 native submission、staging 与 fence-proven retirement。最终提交 I 后建立独立
clean clone，全量双构建、保留45个旧CTest、新CPU ownership/fault/native GPU tests、ASan、
真实source/include/link依赖及冻结事实核验。V 只新增最终验收记录，完成后 STOP。

初始机制选择：Vulkan 1.3，显式验证并仅启用 synchronization2；单 graphics+compute family
（隐含支持 transfer）。设备优先离散，允许显式选择枚举索引。实例可显式请求 extensions；
validation 模式要求真实 Khronos layer、debug utils callback 与同步验证。无 surface/swapchain。
未来 presentation 必须针对实际 surface 重新验证 family/extension，不能以本阶段冒充支持。

原生父依赖为明确 borrow；Instance → Device → Allocator → allocations 的销毁顺序由宿主
词法 owner 顺序和实际测试证明。搬动 wrapper 不改变 native parent 身份。提交前保留候选；
提交成功后 native backing 必须保持到同队列 fence serial 完成。资源退休移交 native owner，
不把 public semantic object 变成 closing zombie。Foundation 不管理 Simulation 或帧进度。

R3 matches 仅为逻辑访问拓扑，不是未来 native plan 的完整 cache key。
R5/R6 须验证一 Scene 多 View、不同 graph/pass 组合、共享持久 SceneData、不同 target/history
及 native execution 对象正确隔离。本阶段不创建上述对象。

V1 provenance 以 `a669409a289a6fa4092f21176397795b1cdb7f3e` 固定：
VulkanContext 的 capability negotiation、VmaTypes 的单 owner move/release、StagingRingBuffer
的有界分区/对齐、TransferScheduler 的 copy 前后屏障、DeferredDestroyQueue 的 serial
退休不变量为参考。不迁旧 context/server/contributor hierarchy；迁移差异逐项记录于 R4 报告。
`render.transfer_idle` 与历史 cross-frame WAR 判定保持原样。

本机初查有 RTX 4070 Ti、Vulkan driver 591.86、Khronos validation 1.4.304、VMA 3.3.0。
正式资格以 clean I 的实际运行和原始日志为准，不能用环境枚举结果代替 smoke。
GPU/validation/ASan 如不能执行必须如实 PARTIAL/NOT_RUN，不降低门禁。
产品仍为 EXPECTED_UNAVAILABLE，Android/Linux/full installed SDK 不在本阶段资格矩阵。
