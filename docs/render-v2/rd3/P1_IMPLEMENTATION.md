# RD3-P1 最小完整架构主线

本轮依据用户的 **RD3-P1 修订指令**，施工基线为 D0 Verification
`38262ecd3e962690703173397f49796cfd81292e`。D0 的 PARTIAL、R01–R04 原始记录与全部历史证据不改判。
该指令接受 D0 的机械完整性，授权先完成单 View Offscreen Target 的生产路径；没有授权完整 P2/P3、P4/P5 或产品替换。

## 实际对象与唯一 Owner

| 对象 / 身份 | 唯一拥有者及容器 | 自动分配身份 |
|---|---|---|
| Scene / 既有 RenderSceneId | RenderBackend 的 `SlotMap<Scene, RenderSceneTag>` | 是 |
| View / 既有 RenderViewHandle | 各 Scene 的 `SlotMap<View, RenderViewTag>` | 是，局部身份必须配 SceneId |
| Target / 既有 RenderTargetId | RenderBackend 的 `SlotMap<Target, RenderTargetTag>` | 是 |
| RenderEffect / RenderEffectId | RenderBackend 的 `SlotMap<RenderEffect, RenderEffectTag>` | 是 |
| CompiledGraphEntry / RenderGraphId | RenderBackend 的 `SlotMap<CompiledGraphEntry, RenderGraphTag>` | 是 |
| ResourceDomain / ResourceDomainId | RenderBackend 的 `SlotMap<ResourceDomain, ResourceDomainTag>` | 是 |
| GraphViewGroup | `SlotKeySparseSet<RenderGraphId, GraphViewGroup>` | 否，借用 Graph Owner 的 ID |
| 每 Graph 的可变执行池 | `SlotKeySparseSet<RenderGraphId, unique_ptr<GraphExecution>>` | 否 |

所有上述身份均为 lux-cxx SlotKey。跨 Client/Server 地址额外携带现有 R2 RouteTable 的 ownerIdentity；
没有新建全局计数分配器。ViewRef = SceneId + ViewId；RuntimeView 再携带 Runtime owner。
槽删除、clear、耗尽时退休遵循 lux-cxx 生成式容器，不能跨 Scene 或 Runtime 解释裸局部 ViewId。

View 仅存 Effect/Graph/Target/Domain 身份。Backend 不再拥有第二个 View 容器。
GraphViewGroup 仅存 ViewRef/ExecutionJob，按 GraphId 辅助查找；有序 jobs 序列保留实际执行顺序。
索引只在拓扑变动时修改，稳定帧没有重建 map。本次新增/迁移的对象索引没有 unordered_map/unordered_set。
ExecutionJob.execution 是 Native 池的内部数组偏移，不是可导出对象身份，也不承担代际分配。

`GraphExecution` 只组织每 View 的发布数据、receipt 和 Native 池；`vulkan::GraphExecutionContext`
唯一拥有可变 GPU backing、Descriptor、View×FIF slot、真实 Queue Ticket。
Native backing 与 SubmissionQueue 均保持稳定地址；Graph 共享不可变 Native code，未为 View 复制 Shader/PSO。
Domain 当前实现 root→Scene→View 的中立归属树；完整 GPU Asset Registry 与导入驻留属于 P4，未预建空抽象。

## Client / Server 与执行链

宿主先独立创建 RenderBackend、冷态装配 Effect，再 start 得到协议 Client RenderRuntime。
Backend 独立拥有设备、队列、Native Graph 与 worker。Runtime.cpp 不包含 Vulkan，也不拥有 Backend。
二者共享的 RuntimeChannel 只维持 R2 Transport、Reply 生存期、停止与诊断；Client 或 Request 晚于
Server 销毁仍安全。请求、Reply、Admission、PROGRAM/CONTROL/UPLOAD 传输机制直接复用现有 R2；
本片段发布 View scalar，尚无 Asset UPLOAD 业务入口，但关闭时仍排空三个 lane。

实际路径：固定 Feature 值 → RenderGraphBuilder → 现有 compileLogicalGraph / LogicalGraphPlan（已有
通用 IR）→ NativeShaderProgram::create → compileVulkanGraph → Backend SlotMap 分配 GraphId →
自由函数 executeGraph → Offscreen Target 最终 readback。没有另建同义 IR 或 Compiler 管理框架。
Fixture 是 Fill(SCENE) → Tonemap(VIEW) → HostReadback(VIEW)，全部 Pass/中间纹理都在 Graph 声明。
Target.readback 从绑定 View 的完整 Graph 最后一个 HOST_READBACK 获得最终输出；本轮拒绝不满足此
有限输出合同的 Effect，不以隐藏 Pass 补全。单 Target 只绑定一个 View。

共享查找先比较完整 logical cacheIdentity、extent、commands、ShaderVariantIdentity（含 schema/SPIR-V）、
Pipeline 描述和拥有字段字符串的完整 OwnerShape/assignment 值。仅在未命中时创建 Native Shader/PSO/Graph。
GraphId 永远由 SlotMap 分配，Hash 不承担身份或碰撞判定。Effect 的初始 invocation 值分别保存，
相同结构、不同默认 exposure 的 Effect 可共享 Graph 而不串值。冷态源码/schema span 仅同步借用，
发布后缓存键与 Native identity 全部自持有。

删除 View 立即撤销身份与 Target 绑定，GPU slot 仅在其全部 FIF ticket 就绪后复用。
删除仍有消费者的 Scene/Target/Effect 返回 Busy；删除不再使用的 Effect 只在最后一份 Effect 引用解除时
退休 Graph，Native context 析构等待真实提交，包括部分提交后失败的路径。

现有 ExecutableGraphPlan / NativeGraphInvocations 实体改为 Graph / GraphExecutionContext，
原 submit 算法直接移为自由 executeGraph；无旧名字兼容层。C0–C13、Shader ABI、layout、pipeline、
barrier、Sync2 与 queue 算法未重新实现。原 RuntimeBackendState / Frame.cpp 组织被实际替换。

## 验证与旧测试分类

新增 render.rd3.runtime：六种对象 Tag 的分配/删除/clear/代际耗尽退休，辅助 SparseSet 的旧代失效；
真实 Server 的 Scene/View/Target 重建、跨 Scene ViewRef、跨 Runtime owner、旧 Effect/Graph ID；
两 Scene/四 View 共用一个 Effect/Graph，独立 GPU 输出，FIF=2/3，同 Scene Pass 合法复用，删除后剩余
消费者继续正确；12 个稳定帧内零 heap allocation；Client/Reply 在 Server 后销毁。

新增 render.rd3.faults：事件/线程创建失败回滚和重试，Native image 分配失败后 Target/Scene 可复用，
首次 Queue Submit 成功、第二次失败时安全停止，18 类 Native owner 计数归零，缓存命中零 Pipeline 创建。
测试 seam 只编译在测试目标；没有生产故障开关。

五个公共入口独立 TU 使用 C++20 编译；Objects/Types/Runtime/Backend 还机械禁止传递包含 Vulkan。
bootstrap 构建始终 target all、-j 4、-k 0，GPU 测试串行且不与构建并发。

原矩阵 200 项中保留 184 项，新增 2 项，当前共 186 项。以下 16 项保留原源文件、历史报告与资格，
从当前 RD3 bootstrap 注册移除，**不计为本轮通过**：

| 原测试 | 本轮处理 |
|---|---|
| public_contract、reject_ID、reject_REQUEST_COPY | 旧 RuntimePath/API 的编译结构检查；新公共头、SlotKey/ViewRef 运行测试替换其相关合同 |
| lifecycle、host_failures | 新 RD3 runtime/faults 覆盖本轮 Client/Server 生命周期、线程失败及安全退休 |
| progress | 新 RD3 独立 worker/PROGRAM 更新检查覆盖本片段；原完整计数 oracle 保留供后续迁移 |
| slot_transactions_2、slot_transactions_3 | 新测试验证 FIF=2/3、image 回滚、部分 submit；原 Asset/slot 组合事务未整体迁移 |
| shared_asset、shared_asset_partial_range、shared_asset_partition_refinement | P4 导入/Registry 业务接入待迁移；Native 导入、共享/区间底层测试继续运行 |
| history | Runtime History 接入待后续阶段，Native 算法保留 |
| presentation | WSI Target 接入待后续阶段，Presentation.cpp 原算法保留，P1 仅 Offscreen |
| footprint_1080_2、footprint_1080_3、footprint_4k | 本轮按修订指令不重跑历史 footprint/微基准资格，不改判历史 PARTIAL |

测试名前缀均为 render.runtime.。完整实际 CTest 清单与执行结果由独立 Verification 报告记录。

## 环境与边界

依赖使用同级仓库要求的已安装版本：lux-cxx bf779515a120350c7c5412c366eea73afdf59ccf；
lux-cmake-toolset c104eb6；imgui ad75bbcb1；imgui-node-editor d948c43。没有为本阶段修改这些源码。
MSVC 19.44、CMake 3.31.10、Ninja、C++20、RelWithDebInfo、Windows x64；编译 SDK 1.3.280.0，
独立验收加载本地 copy-only 的 **Vulkan Validation Layers 1.4.304.0**。

初次 SDK 1.3.280 的 SyncVal 不支持 Timeline Semaphore，产生跨提交 WRITE_RACING_WRITE 诊断；
[该版本官方说明](https://raw.githubusercontent.com/KhronosGroup/Vulkan-ValidationLayers/vulkan-sdk-1.3.280.0/docs/synchronization_usage.md)
列明该限制。[1.4.304 官方说明](https://raw.githubusercontent.com/KhronosGroup/Vulkan-ValidationLayers/vulkan-sdk-1.4.304.0/docs/syncval_usage.md)
支持 timeline。验证层通过 VK_LAYER_PATH 指定，未屏蔽同步检查、未引入全局 wait-idle 绕过。
旧诊断、无效 barrier 实验日志保留在外部 evidence；实验改动已撤回，Native 同步算法保持不变。

ALLOWED：runtime 对象/协议组织及测试、Native Graph 实体职责迁移及直接测试调用、bootstrap 注册、本阶段文档和检查器。
READ-ONLY：R2/C0–C8/Shader/Queue 核心算法、RD2/D0 与性能原始报告、719 个 Legacy 冻结文件、用户工作区、依赖源码。
FORBIDDEN：修改历史资格、合并 main、完整 P2–P5 或高级算法、产品替换。逐文件变更见 P1_CHANGESET.txt。

R01 单 View Target 片段已落实；多 Effect 最终 Graph 仍待扩展。R02 采用启动前同 Owner 线程串行构建，
没有异步 Compiler Scheduler。R03 不改写旧 CSV，相关功能保留未来阶段。R04 本轮迁移 Native 对象唯一
Owner 已落实，未来 Registry 未提前实现。R05 由 Stop/Drain/晚 Reply/部分提交测试覆盖。
仍待实现 Effect 热发布事务、完整 Feature SDK、GPU Asset/Registry/ECS 流、WSI/resize/history Runtime
接入、Engine/Editor 完整集成、Plugin、Legacy 业务与 H1–H6。不得把这个可运行纵向片段认定为这些功能完成。

Implementation 与 Verification 分开提交；后者必须针对精确 I 的独立 clean clone，只增加验收报告。
完成推送后停止，后续阶段需要用户重新授权。
