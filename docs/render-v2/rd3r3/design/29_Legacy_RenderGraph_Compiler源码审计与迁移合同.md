# 29 — Legacy RenderGraph / Compiler：源码级能力与不可丢失的算法合同

> **来源定位**：本章针对 `render_legacy/` 冻结源码逐文件阅读形成；不是声称当前 R4/P2-A 已具备这些全部功能。新 C0–C13 的真实能力应以当前 `modules/function/render/{graph,vulkan}/` 的实现为准。**优先沿用当前正确的 R4 算法，Legacy 作为功能/优化正确性检查单，不得倒迁旧错误或恢复重复 Scheduler。**

## 29.1 源码地图与职责

| Legacy 源文件（前缀 `render_legacy/modules/function/render/`） | 已有功能/事实 | RD3-R3 处理 |
|---|---|---|
| `graph/src/DependencyAnalyzer.cpp` | 资源→Pass 预建索引、显式 before/after、读写依赖、分阶段 Kahn 拓扑、Painter stage 排序、Local-Read 邻接约束 | 中立 C0–C8：同一版本化依赖真相，不重建业务排序表 |
| `vulkan/include/lux/engine/render/graph/RGBuilder.hpp` | transient、persistent、ping-pong buffer/image、imports、dynamic slot、tracked external、前向引用、conditionChain、read/write/inputRead/sideEffect | 新 Graph Author/IR 必须能表达；不搬旧 Vulkan/raw DS 到中立 Builder |
| `vulkan/src/graph/RenderGraphCompiler.cpp` | 顺序编译步骤、forward ref、usage union、resourceDS 依赖、LayoutPlan、starved/dead 剪枝、queue/条件/barrier、compiled kernel | 新固定 Compiler phases；按依赖关系排序，不能凭命名把步骤丢掉 |
| `vulkan/src/graph/RenderGraphCompiler.ExecutionPlan.cpp` | attachment load/store、LocalReadBoundary、条件链验证、MeshBucket/ViewAllocator、BarrierProgram、QueueSubmitProgram、ExecutionProgram 字节码 | 在 R4 Native Recipe/Executor 中对应有价值的静态优化；不必复制旧 bytecode 类 |
| `vulkan/src/graph/RenderGraphCompiler.LayoutAllocator.cpp` | 全图反射合并、descriptor 绑定兼容、Engine-shared domain layout、StageUsage limits、layout identity | 新 Shader Layout/OwnerShape/ABI full shape，校验设备 Enabled Caps |
| `vulkan/src/graph/RenderGraphCompiler.Barriers.cpp` | Per-resource state、layout/access/stage、多个读者聚合、first/subsequent View、最终 export、DS binding recipe | R4 Sync2/Compiler 继续提供真实 Queue/Slice/Transition/Descriptor 正确性 |
| `vulkan/src/graph/RGVulkanResourceAllocator.cpp` | transient physical pool、persistent/ring、VMA suballocation pools、lazy attachment memory probing、recompile reuse、idle GC | 按真实 MemoryRequirements/LastUse 整合进 Native ResourcePlanner/ExecutionContext |
| `vulkan/src/graph/RGGraphResize.cpp` | resize 创建全新 ResourceState 候选，失败还原旧状态，更新动态外部导入，成功才退旧资源 | R3-C Target/History 采用整体候选+真实 GPU/WSI retire |
| `vulkan/src/graph/RGVulkanRecorder.cpp` | prebuilt barrier replay、dynamic imported refresh、per-frame descriptor、Native execution fast program 与普通 fallback | Executor 只用 prepared indexed binding，必要时复用低层现有录制器算法 |
| `vulkan/include/lux/engine/render/graph/FrameExternalSync.hpp`、`FrameExtensionRegistry.hpp` | 外部资源 ready/扩展接缝 | 新版按明确 Graph import/output、Ready/LastUse 和 code pin 表达 |

## 29.2 不可任意重排的 Compiler 语义链

源码 `RenderGraphCompiler::compile()` 的顺序不是装饰：

1. **Resolve forward resources → texture usage union → 声明的外部 DS 消费显式计入数据依赖**。在此之前建依赖会漏掉真实访问。新版由 DataSchema/PassSchema/ResourcePlan 统一声明，禁止 `trackExternal*` 变成完全图外同步。
2. **全局依赖、资源合法性 → transient descriptor layout 角色推断/核对 → graph-wide reflected LayoutPlan**。Layout/Owner 结果必须在 Native Pipeline 创建之前确定。
3. **Compiled pass → topo → `eliminateStarvedPasses()` → `eliminateDeadPasses()`**。先裁剪没有有效输入的 Pass，再按输出可达性裁剪；有 SideEffect/External 输出时不能误删。死 Pass 不能贡献 Barrier、Transient alloc 或 Query 需求。
4. **Assign queue → cross-queue dependencies → merged rendering scopes → LiveAccessIndex → classify elective**。`LiveAccessIndex` 必须由活 Pass 和真实执行序计算一次，条件链和 AttachmentOps 共享此事实，避免两个不同索引语义漂移。
5. **Pipeline/Descriptor bind plans → attachment load/store → Sync2 barriers → prebuilt barrier arrays**。LOAD 也是 Attachment READ，必须加入 Hazard 推导；descriptor 省绑定只在 Layout/Set 完整身份相同时合法。
6. **Transient DS / Native resource backing → classified Native execution → precompiled commands/patches/queue template**。新 R4 已将多阶段操作组织为 Typed Command/Recipe，保留优化语义，而非创建解释器与 Native Executor 两套录制路径。

以上是 Legacy 的实际流程；新 RD3-R3 可用更强的 SSA/ResourceVersion 将其中若干步骤合并，但必须用等价反例证明其语义不丢失。

## 29.3 高价值的非显然约束

- **`inputRead`/Dynamic Rendering Local Read**：同一合并 rendering scope 的 producer 与 consumer 须合法相邻；用显式 dependency/producer version，而不是依赖 Feature 注册顺序；实现时校验 VkDevice 确实启用 KHR dynamic rendering local read，并为不支持设备提供拆分 render scopes 的合法 fallback。局部 by-region Barrier 不能伪装成非法 scope 内的普通 pipeline barrier。
- **Painter/after/before 与 WAW**：Stage key 只作为确定性的 tie-break，不覆盖真实生产/消费依赖；绘制顺序在最终 Target Composition 也须显式。
- **Conditional/Skip Chain**：Legacy `conditionChain()` 令一组 Pass 原子启停，编译器检查 `CONDITIONAL_CHAIN`、`INTRA_GROUP_ADDITIVE`、unconditional reader、首写 CLEAR。RD3-R3 不能用每 Pass `if(enabled)` 直接跳过导致下游读取未生产的 GBuffer/History。
- **LOAD/STORE 优化**：`Transient + 最后一次访问已结束` 才可 `STORE_OP_DONT_CARE`；跨 View/History/Export 需要保留数据；清除必须由有效首写执行者负责。
- **Barrier 性能/安全**：Legacy 已撤销错误的 split barrier：两个没有 VkEvent 的 NONE/NONE release/acquire 不能自动建立依赖；跨读者的 stage/access 应累积，不要把后续 RAR 当作不需继承前者同步。R4 Sync2 是后续唯一 native 真相，禁止重启旧失败算法。
- **Descriptor binding dedup 风险**：Legacy曾因跨 Pass 去重而漏了与 Pipeline Layout/Domain 不兼容的实际重绑。最终按**精确 Native PipelineLayout + binding set + slots + dynamic offsets + version + command scope**证明；无法证明便绑定，不能只比较逻辑 DomainId。
- **First View/Subsequent View**：Legacy 有两套 BarrierProgram，针对同一 Scene Graph 多 View/共享 Scene pass 调整输入状态；新实现必须依赖真实上一使用 Ticket、GraphResource state、donor/provenance，不能只按照 View 序号忽略危险依赖。
- **Native fast path**：Legacy 包括 MeshBucketLayout、ViewAllocatorPlan、BarrierProgram、QueueSubmitProgram、带动态 Patch 的 ExecutionProgram；且存在非 Kernel recorder fallback。新系统可用 R4 `NativePassCommand`/Recipe 实现同等热态效果，**不是要求复制解释器**，不能让每帧运行大量字符串查询/动态回调分支。

## 29.4 物理资源规划与缓存（不能混为一谈）

- **逻辑 Graph 的版本与资源 lifetime**：Transient、Persistent、Imported、External/Tracked、PingPong Current/Previous、Slotted image；History 的过去/现在互不别名且受 resize/camera-cut/version 约束。
- **同次 Graph 物理 Alias**：依赖可证明不重叠的完整生命周期、相同设备内存类别、alignment/size/usage/format/alias limits、真实 ResourceCell/barrier；不等于跨帧图像对象复用。
- **跨重新编译的 Physical Pool**：Legacy `TransientResourceKey` 全字段包含 image format/size/depth/mip/layers/samples/usage 与 buffer usage/size/memory usage；`RGVulkanResourceAllocator` 有 transient image VMA pool、Lazy Memory pool、recompile pool+GC。新系统可用小容量 flat exact-key cache 替代 hash multimap，但不能退化为“每 Resize/new Effect 都重新 VkAllocateMemory”。物理 owner 未完成 GPU 使用时不得进入可重用池。
- **Tile GPU**：Legacy 单独探测 `LAZILY_ALLOCATED` 是否真的可用，且缺失时也使用 pooled allocation fallback；新 Native Capability/MemoryPlan 要保留可查询事实，不要只因 `VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT` 设为 preferred 就断言真正分配到了惰性内存。
- **Resize**：先创建完整新 backing/Views/descriptor，再原子替换；旧状态正确退休。不要用“直接重建整个逻辑 Effect”来代替 Target 的物理重专门化。

## 29.5 新实现阶段及必须证明的反例

- **R3-B**：前向引用、同名不同资源/Scope/Key、外部 read 依赖、starved/dead、SideEffect、WAW、环、Reader 不可达、Shader Owner/Layout 负例；ResourcePlan 只由活资源/业务 demand 产生。
- **R3-C**：LocalRead/merge/attachment clear/load/store、dynamic import/resize/persistent history、conditional chain、multi queue DAG/ownership/sync2、初次/后继 View、Graph Retry/LastGood；`GraphExecutionContext` 使用 R4 模块，不回迁 Legacy 全套 NativeGraphRecorder。
- **R3-D/L1–L3**：MeshBucket/Indirect/Run-length Dirty/physical pooled alias/lazy GPU、VRAM 分配次数、稳定 Graph 重编频率、上传+渲染竞争/吞吐。
- **H1–H6**：反馈、Virtual Resources、时域/跨帧、AsyncCompute/RT/volume/GS 必须基于真实 `ResourcePlan`/Pass/Queue/Retirement 图证据，不能仅列一个枚举类型。

> 本章新增需求逐条在 `appendix/LegacyCompilerMechanisms.csv` 绑定源码、保留策略、阶段与测试；旧 86 项 F/I/H、W25、REQ51 的来源行不改。
