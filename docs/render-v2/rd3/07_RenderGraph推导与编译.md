# 07 — Renderer-independent Graph IR 和两级编译器

## 7.1 Graph 是单次最终 Target 输出的完整程序

**一个 `execute(graph, view(s), target)` 包含该目标画面全部 GPU Pass**。GBuffer、Depth、Shadow Scratch、HDR、Tonemap、Overlay/Composite 等中间变量都在同一份 `RenderGraphDefinition` 中声明；Builder 明确读写和最后 target output，Compiler 从真实 Producer/Use/范围确定 Pass 顺序、分配和状态绑定。执行结束（GPU completion 后）即是这次 Target 最终结果，没有图外隐藏 Tonemap、内部纹理第二次 Graph 读取或私有 UI 绘制环路。

Graph 可以显式输入 persistent Scene/View/Root 数据、上一帧 History、Camera/Video 或另一 Offscreen Target 已经发布的**最终输出**，但这是一份经过授权的外部 Import，绝非将当前 Graph 内中间临时图像延长到 Graph 外。在确实有不同 Target 之间的明确外部输出依赖时，Server 只对完整 Target job 进行因果排序，不增加第二套内部 Pass scheduler。多 View 同一 Target 的最终合成由其完整 Graph 中的 composition passes 表达；普通单 View 使用 `execute(graph,view,target)`。

## 7.2 中立 RenderGraphIR 的必要数据

- Graph-local 资源：Buffer/Image、format/aspect/extent（可相对 Target）、import/transient/history/export、semantic 以及访问许可和必要初始化合同。
- Pass：Graphics/Compute/Transfer/Readback 等基本工作、Shader reference、typed PassSchema/Draw/Dispatch/Indirect/Copy ops、conditions、explicit order for real business constraints。
- 子资源/字节精确 `READ/WRITE/READ_WRITE`、Producer Version/fallback、输出 Root/SideEffect 与 Scene/View reuse 证明数据。
- 不携带 `VkDevice/VkBuffer/VkPipeline`，不携带具体 SceneId、DomainId 或 CPU 指针；业务 Light/Mesh/Gaussian 不是 Graph 的通用 enum 变体。
- 编译结果拥有结构性身份和完整比较事实，hash 仅索引缓存，hash 相同不等于允许跳过相容校验。

## 7.3 C0–C8 保留现有分析算法

| 阶段 | 算法与输出 |
|---|---|
| C0 | 规范化 Builder 的稳定声明、pass/resource IDs、精确 ImageRange/BufferRange |
| C1 | 解析 explicit producer/import/semantic/fallback 到唯一资源版本，歧义 cold reject |
| C2 | `PassSchema`、资源接口、Shader 结构性需求、usage 与写权限验证 |
| C3 | Subresource segment + version hazard：RAW/WAR/WAW/ORDER/FALLBACK edges |
| C4 | 确定性拓扑排序，真实 cycle path 和 producer conflicts 诊断 |
| C5 | 从输出/外部副作用反向裁剪无效 Pass，条件组合法 fallback |
| C6 | Logic live interval、usage union、potential alias candidates（非 GPU completion 证明） |
| C7 | Scene-scope Pass 在多 View 可共享的真实输入证明，View-scope work 必须分开 |
| C8 | 拥有型 immutable `RenderGraphIR`、diagnostic 和完整 cache identity |

逻辑 Pass 的作者顺序不一定就是执行顺序；Compiler 可在依赖许可内改善 Pipeline/Descriptor 切换局部性，但不得改变 Transparent/Blend/Overlay 的真实顺序要求，也不能跨资源读写边任意调换。

## 7.4 Backend C9–C13：像 LLVM 后端，但无无意义注册框架

| 阶段 | Vulkan Compiler 的真实工作 |
|---|---|
| C9 | DeviceCaps/Shader Stage/PassSchema→布局/Shader 编译或再定位、SPIR-V reflection 验证、完整 `BindingLayout`/PSO key |
| C10 | 从 IR 实际访问与能力选择 Native Buffer/Image/Transient plan、usage、alignment、alias/noalias 与 Target signature 专门化 |
| C11 | 编译 native pass/PSO/Descriptor/Pipeline bind recipe、Dynamic Rendering/Copy/Indirect 等固定命令数据；禁止 lazy GPU 对象构造 |
| C12 | 精确 Sync2 src/dst stage/access、Subresource barriers、Queue family transfer、external ready wait/signal 与 submission budget |
| C13 | 验证并构造不可变 `vulkan::Graph`，保留 Pipeline/Shader/Layout owner lease；给 Executor 可直接解释的指令序列 |

`vulkan::GraphPass` 是**编译后的 GPU Pass 数据**；`CompilerPass` 是可测的分析/变换函数，二者不可混用。默认各 C9–C13 是固定函数/实现文件，只有新 Native primitive 真实需要扩展时才增加阶段处理函数。**不默认实现运行时 `ICompilerPass`/`PassManager`/`PassRegistry` 动态插件。**

## 7.5 最小化绑定与屏障

Compiler 静态消除等价布局转换与多余 hazard、合并兼容 Pass 和复用 PSO、预排序常用 Binding，生成 Descriptor/PushConstant/Pipeline 切换配方。Executor 维护**当前 CommandBuffer 的**真实绑定状态，避免重复 `vkCmdBindPipeline` 和 `vkCmdBindDescriptorSets`；状态绝不跨新的 CommandBuffer/Queue 无条件沿用。相同 Struct 多实例与完整 Shader Owner 的不同 named binding 要正确区分，不能只按类型/频率归并。一个新的 GPU Binding 读者不应该要求作者再次手写 `set=3,binding=7`。

## 7.6 GraphId、Native 变体和 Cache

`RenderGraphId` 唯一标识一个已编译的 Device-local immutable Graph。Cache key 为完整 IR identity、Native Backend、设备 enabled capabilities/ABI、Shader/Material variant、真实的 Target attachment format/sample/local-read/必要 extent signature 等；**不含 Scene/View ID、Domain、当前 Camera、Effect Hot 参数或 FrameSerial**。同一个 Effect E1 在两个相容 Target 上直接共享 Graph G1；必要 extent specialization 产生 G2；Graph 经 full equality 命中无需重编。旧 Graph 在 GPU 完成后安全可退役，不因 Effect 改版本而原地写入其 pass/PSO。

## 7.7 Scene Pass 共享与真正 GPU 工作

Graph immutable 复用并不意味 GPU passes 在多 View 只需运行一次。C7 将可跨同 Scene/View-independent inputs 的 Pass 标记为候选，Executor 通过实际 Scene resource revision/ready/layout/side-effect 等快照核验，只有成立才借用一次已接受 GPU 结果并延长最后 consumer 的 use ticket。Scene A/B 即使同 Graph，也不因此共享各自 Light/Instance 结果；不同场景共用真正 Root Asset 仍遵守 Native owner。

## 7.8 禁止的架构滑坡

- 不因 Target output 需要 readback 就在 Graph 内部偷偷保留永久 Texture；只有作者明确导出结果才获得跨生命周期身份。
- 不通过 Frame 序号推测资源初始化/同步；Native Compiler 证明逻辑 access，Executor 证明实际 ready/state/ticket。
- 不每执行一次 Graph 都构建 `RenderGraphDefinition`、重排 Pass 或重建 Descriptor/Pipeline。
- 不因为未来 Ray/3DGS 需要新 Native op 就写一个跨 API 的万能 IRenderer；在 IR 与具体后端添加受限能力。
