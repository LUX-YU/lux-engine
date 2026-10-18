# LuxEngine Render V2 v1.2 — 全模块类型、成员、算法、依赖与代码质量深度审计

**文档号：RENDER-AUDIT-1.2**  
**日期：2026-10-18**  
**来源基线：** [`LUX-YU/lux-engine@71fbe803620aba25ab34d79ca446325058d30df9`](https://github.com/LUX-YU/lux-engine/tree/71fbe803620aba25ab34d79ca446325058d30df9)，分支 `codex/render-v2-rebuild`；冻结 Legacy 位于 `render_legacy/`。  
**配套：** `LuxEngine_Render_Code_Style_and_Engineering_Constraints_v1_2_2026-10-18.zh-CN.md`（以后实施方应先完整阅读）。  
**性质：** 源码静态审查与设计建议；**不是**已完成改造、构建或 GPU 性能验收。未完成的全仓链接消费者核对以 `NEEDS_CLOSURE` 标识。  
**审查范畴：** Render V2 的 Native GPU 基础、Device/Queue/Retirement、专业 Storage、Graph/Compiler/Executor、Command/Feature/Runtime/IPC、文件/构建组织、测试、命名和可读性；并从冻结 Legacy 提炼对照。  
**总裁决：** **保留新的 C/S/Command/stdexec/Graph 主框架；清理、替换其中明显偏离 Legacy 有效 GPU 数据算法和 C++ RAII 的实现。** 不回退到 Legacy 的旧客户端协议，也不为删除当前代码再造一套未来更复杂的抽象。


> **v1.1 修订提示：** 在此前 91 个审查项之外，本次增加人工 `modules/platform/gapi/vk` 作为 Native 源码比较基准，补充设备能力缓存、创建/RAII Wrapper、Builder 的复用与限制、对 DeviceSession 与 Storage/Handler 的依赖归属，和迁移门禁。本次扩展**不改变旧审查项编号**；额外项使用 `V01–V20`。此处先审查，不声称已经改动生产源码或完成 GPU 测试。

## 0. 如何阅读：三种证据等级和四种处置

### 0.1 证据等级

- **CONFIRMED**：在上述 Git SHA 中直接读到了类型、字段、实现或 CMake 依赖；文内有永久源码链接。
- **INFERRED**：由多个源码事实推断存在维护/性能风险，但**未作可比运行时测量**；不能宣称已证明具体 FPS 回归。
- **NEEDS_CLOSURE**：需要搜索全仓真实消费者、链接/安装闭包或外部 SDK 后才有安全删除资格。

### 0.2 处置编码

| 编码 | 含义 |
|---|---|
| **DELETE** | 职责同义或不符合目标；相关消费者迁完后删除旧类/字段/文件 |
| **MOVE** | 真实职责存在，但放错了层或专业模块 |
| **MERGE** | 若干类型/状态在表达同一事实，应由一个真正拥有者统一 |
| **KEEP** | GPU/IPC/Shader 合同中的真实必要机制，可能还要改命名/API |
| **RESEARCH** | 需要独立原型或消费者/性能证据才能决定，不强行实施 |

### 0.3 本次核查的定量基线

按远端 Git Tree (`modules/function/render`) 统计：

| 项目 | 数量 / 大小 |
|---|---:|
| 所有跟踪文件 | **328** |
| `test/` 文件 | **189**，约 **1,081,836 B** |
| 测试 `.cpp` | **94**，约 **806,391 B** |
| 生产 `.cpp` | **46**，约 **512,675 B** |
| 生产 `.hpp` | **76**，约 **238,440 B** |
| 生产 `.cpp` + `.hpp` | **122**，约 **751,115 B** |
| 测试 `.cpp` / 生产 `.cpp` 文本字节比 | **1.57 倍** |

这些数据包括构建目标/测试实验；测试源码大于生产 `.cpp` 并不单独证明实现有 bug，却证明测试组织与用户期望明显不一致。**不能**把 189 个测试文件说成 189 个无用文件。

## 1. 总体结构诊断

### 1.1 框架设计本身应保留的成果

1. 单进程多 Client、单 RenderServer、双 SPSC，类型化 Command/Reply，进程内对象零额外二进制转码。
2. 通过 `stdexec::counting_scope + spawn` 接管通用异步 Command OperationState；不再维护早期的 PendingOperation/destroy thunk。
3. 独立 `RenderGraphDefinition + GraphAnalysis`，中立依赖、数据访问、版本和裁剪的算法基础。
4. 可安装的 Feature、Handler、Storage Factory 及 Scene/View/Target/Effect 分层意图。
5. 有真实 GPU 提交证据、跨 Queue 同步、LastUse 与 Retirement 概念，已积累一些有价值的正常与失败场景验证。

### 1.2 当前不合理之处的共通根因

**根因 A：通用框架被当作保存专业数据布局的地方。** 例如通用 `InstanceStorage` 固定 Mesh+Material+Mat4+CPU DrawGroup，Camera 成为 RenderServer 内建业务。

**根因 B：物理资源的拥有权没有由 RAII 对象表达，而依靠多级 Permit/Lease/Retirement/Use/Physical/Backing 与手动清理维持。**

**根因 C：按“过渡期冷热/安全/测试”实现的结构被长期保存。** 如 `cold`/`active`、`*_SEAM`、`version/live/reserved`、每阶段专用测试工作流。

**根因 D：在有 ID 的地方继续按字符串或全局集合查找，给后续规模化开发埋下 O(n) 或 O(n²) 成本。**

**根因 E：模块目录虽然细分，但真正的 CMake/API 依赖不符合目标层次。** `resource` 公开 stdexec、`commands` 定义在 `runtime`、Native extension 依赖 protocol、ShaderInfo 与 RenderData 重叠。

### 1.3 设计终态（不是增加每一种示意类型的授权）

```text
Engine / Scene / Material Author
        │      （Camera/Entity/Animation只在此解释）
        ▼
Client 类型化 Command  ──────────────→ 统一 IPC 请求/Reply
                                          │
                                 RenderServer 单线程事件循环
                               /          │           \
                      注册/目录          Effect/Attach       stdexec异步准备
                            │               │                │
                    专业 Storage         中立 RenderGraph     CPU工作结果
                            │               │                │
                      Native资源       Native RenderPlan     RenderThread采用
                            └───────────────┴────────────────┘
                                            │
                                   Native Queue / GPU progress
                                            │
                                 唯一Retirement / RAII释放
```

Renderer 不理解专业 Camera；Graph 不解析 StorageId/Handler；Storage 不决定具体 DrawList；Queue 不认识 TextureId/MeshId；Ticket 不暴露给 Client；stdexec 不负责实际 Vulkan GPU 完成证据。

---

# 2. DeviceSession / Queue / Memory：最显著的状态重复

## D01 — 四个平行数组表达两件事（CONFIRMED，P0，MERGE）

**位置：** [`DeviceSession.hpp` L109–126](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/DeviceSession.hpp#L109-L126)，[`DeviceSession.cpp` L44–72](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/DeviceSession.cpp#L44-L72)。

```cpp
std::array<std::unique_ptr<SubmissionQueue>, 3> queue_owners_;
std::array<SubmissionQueue*, 3> queues_{};
std::array<std::optional<RetirementQueue>, 3> retirement_owners_;
std::array<RetirementQueue*, 3> retirements_{};
```

真实事实只有：**若干真实物理 `VkQueue`** 和 **Graphics/Compute/Transfer 到这些物理队列的映射**。每条物理队列拥有一个提交状态和相应的退休收集设施。现在四份并行结构容许初始化错位，而且析构还必须单独清理退休数组。

**目标：** 一份实际物理队列拥有集合（Queue 与退休组件组合）；一份由三个 `EQueueRole` 到物理队列下标的值数组。不因为三个角色可能 alias 而维护三套强拥有表。实际对象地址在建表后稳定。示意 `PhysicalQueue` 仅在确实替换至少三套旧表达时新增，或直接让 `SubmissionQueue` 拥有必要的退休设施。**请验证 `RetirementQueue` 借用 `DeviceSession`、SubmissionQueue 构造地址和析构顺序。**

不是禁止 O(3) 扫描：这里只有三个角色，真正错误是四份状态表达一个映射。

## D02 — DeviceSession 成为物理资源、装配、Context、Upload 的综合拥有者（P0，MOVE/MERGE）

**位置：** [`DeviceSession.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/DeviceSession.hpp)。

它公开 `createBuffer/createImage/aliasImage/write/read`、`prepare(ExecutionContext)/release(ExecutionContext)`、`uploadImage/uploadBuffer`、`sampledImageArray`、`collect/drain/dispatchCompletions`，还直接持有 `buffers_`, `images_`, `contexts_`, `uploads_`, `resources_`, `globals_`。这些不是一个 Device Session 可以自然承担的同一种职责。其他层必须通过 DeviceSession 全能 API 完成最基本的资源操作。

**目标归属：** Native Allocator 管物理内存；Buffer/Image 是 Move-only RAII；Upload 部件管暂存+Copy；Queue 管提交和进度；Retirement 管最终安全释放；RenderGraphInstance/Attachment 拥有实例；ResourceRegistry/StorageSet 负责类型/范围。DeviceSession 只组合设备级长期对象、暴露少数明确组件借用及最终安全边界。新类的数量可以少于现有类型，不能拆成更多 Manager。

## D03 — `AllocationPermit` 名义 Move-only、析构却不释放（P0，REPLACE）

**位置：** [`memory/Memory.hpp` L25–68](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/memory/Memory.hpp#L25-L68)。

Permit 的注释直接说明：遗失这个 Token 不触发回收，最后由 Session 析构补救。分配得到的真实 `BufferAllocation`/`ImageAllocation` 又只是带 Vk/VMA 原始句柄的记录，没有自己的合法资源销毁职责。调用者必须决定 `destroyUnsubmitted` 还是 `releaseCompleted`，之后才可能真正释放。它不像 Legacy 的 [`TDeviceObject`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/render_legacy/modules/function/render/vulkan/sinclude/lux/engine/render/gpu/lifecycle/DeviceObject.hpp) / [`TFifOwnedAllocated`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/render_legacy/modules/function/render/vulkan/sinclude/lux/engine/render/gpu/lifecycle/FifOwned.hpp)。

**目标：** 直接返回实际 Move-only Buffer/Image 资源。资源拥有者在合法 GPU 安全点释放或转交已有退休机制。若对象尚在 GPU 使用则不可原地销毁；可采用“拥有型资源转交一次现有 Retirement”而不是另造 Owner/Lease/Ticket。**先验证 Alias Image、Buffer 映射、Partial Submit 和跨 Queue LastUse，再删除 Permit。**

## D04 — DeviceSession 析构中的全局资源遍历和手动重置（P0，REPLACE）

**位置：** [`DeviceSession.cpp` L16–42](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/DeviceSession.cpp#L16-L42)。

析构 `drain()` → `contexts_.clear()` → `globals_.clear()` → `uploads_.reset()` → `retirement_owners_.reset()` → 扫 `buffers_` → **反复 `std::find_if(images_)` 找 Source alias** → 再扫 Image。前述顺序需要人工记忆，Alias 搜索最坏可二次复杂度。

**目标：** 物理资源实际所有者的成员析构保证局部释放，唯一 GPU 安全清偿负责最终等待与退休。Alias 对象按其真实依赖表达存活，不能依靠 Session 收尾时扫全体 Image 先挑出 alias。不能简单删 `drain` 令独立使用 Session 的 SDK/测试变不安全。

## D05 — `Session::contexts_` 与 Effect/ExecutionGroup 产生不必要的双层拥有（P0，MOVE）

**位置：** [`DeviceSession.hpp` L120–123](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/DeviceSession.hpp#L120-L123)；[`Execution.cpp` 中 DeviceSession::prepare/release](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/Execution.cpp#L47-L185)。

`DeviceSession` 持有 `vector<unique_ptr<ExecutionContext>>`，而 EffectGroup 只存 `ExecutionContext*`，释放时还需要在 `contexts_` 里 `std::find_if` 指针。实际 Context 的语义和所属 View/Target/Effect Attachment 完全不同于“设备上随意一个 Context”。

**目标：** Attachment/RenderGraphInstance 独占执行实例，DeviceSession 只借必要 GPU 基础；自然析构撤回订阅并延迟回收正在执行的资源。无需维护 DeviceSession 的全局 Context Object 目录。`Context` 应在定义时取得必要引用；不能为 UAF 再增加设备级引用计数。

## D06 — `queueFor(ticket)`、`deviceLost()`、`collect()` 各自重复扫数组（P1，MERGE）

**位置：** [`DeviceSession.cpp` L75–129](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/DeviceSession.cpp#L75-L129)。

三个小数组扫描的复杂度本身是 O(3)、**可接受**。问题是实际 Queue、对应 Retirement、Terminal 是否结束等三个事实分别在不同数组和多个函数中读取。按 D01 统一后，可以直接遍历物理队列表、在对应节点上调用 `poll→dispatch→retire`。不要为了消灭三项短循环新增哈希表。

## D07 — `drain` 语义含糊，多个组件重复负责最终 GPU 清偿（P1，MERGE）

**位置：** [`DeviceSession::drain`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/DeviceSession.cpp#L75-L88)、[`ServerLoop.cpp::shutdown`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/src/ServerLoop.cpp#L110-L181)、[`Retirement.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/retirement/Retirement.cpp)、[`Submission.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/transfer/Submission.cpp)。

Legacy 也会在 Renderer/FrameDriver 关闭时 `waitIdle()`；并非“底层绝对不等待 GPU”。区别是 Legacy `DeviceContext::~DeviceContext()` 不重复 wait，而是验证资源已清偿。**目标：** 保留一个可辨认的 GPU 最终清偿动作和一个独立安全的 DeviceSession 销毁合同；不在 Server、Session、Retirement、Submission 四层各维护一套等待策略。必要 `waitForSubmittedWork` 的命名表达清楚要等什么。

## D08 — 同一个 Device Lost 在多层解释 Error args 与 Terminal（P1，MOVE/MERGE）

**位置：** [`DeviceSession.cpp` L16–22](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/DeviceSession.cpp#L16-L22)、[`Submission.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/transfer/Submission.cpp) 和 [`GpuWork.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/transfer/GpuWork.hpp)。

`VK_ERROR_DEVICE_LOST` 是实际 Vulkan 运行时事实；不能删除。但把同一个错误从 `RenderError.args[0]` 解码到 `is_device_lost`、`queue.deviceLost`、`GpuWork` 是否停止/unknown，说明解释范围过宽。**目标：** Vulkan API 失败处归一记录一次 Terminal；业务错误和可安全回收是两个不同语义。资源安全清偿不能冒充成功执行。未知结果只保留在确有已接受不可撤销工作场景。

## D09 — GPU Queue 的两套完成机制并存（P2，RESEARCH）

**位置：** [`Submission.cpp` L13–115, L294–407](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/transfer/Submission.cpp)。

Queue 拥有 `vector<VkFence>` 和可选 `VkTimelineSemaphore`：前者跟踪 CPU 完成与 CommandBuffer Slot 复用，后者用于跨 Queue GPU 同步。这是 Vulkan 合法设计，但未来可以研究单 Timeline 能否同时承担 GPU→CPU 完成信号和跨 Queue 提交序号，减少重复进度查询。**非本次代码扫除的硬删除项**；Windows WSI、无 Timeline 的配置和 CommandBuffer Slot 复用必须真正测试。

## D10 — `SubmissionTicket` 的 `owner()` 名字不表意（P2，RENAME/KEEP）

**位置：** [`Submission.hpp` L13–45](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/transfer/Submission.hpp#L13-L45)。

实际数据是 `SubmissionQueue* + serial`。Queue 并不是 Ticket 拥有的对象；`owner()` 返回 `queue()` 更直接。Serial 是实际 GPU 提交事实，应由 Native 拥有，**不能因为名字不好删除 Queue Serial 语义**。业务层 Texture/Mesh/Material/Instance 返回不暴露 Ticket。

## D11 — `SubmissionCallback` 同时承担 Capacity Reservation、Armed Borrow、链表节点（P1，RESEARCH）

**位置：** [`Submission.hpp` L45–91](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/transfer/Submission.hpp#L45-L91)。

`queue_/next_/callback_/user_/serial_/armed_` 再加 `reserved()`、`reset()`。它用于防止提交后回调注册因容量不足而失败，是真实需求；但也使每个 Native 上传状态要管理额外的 `reserve/onComplete/reset` 转换。研究是否能让内建 GPU Completion Sender/Queue 在提交前取得固定容量/就绪节点，生产逻辑不同时维护一个可重注册的手工 callback 状态机。必须保留同步完成/自销毁/外部 Producer 尾栈正确性；不只为减少两行代码删容量前置约束。

## D12 — `RetirementReservation`、`SubmissionRequirements` 与 Storage 专有退休状态叠加（P0，MERGE）

**位置：** [`Retirement.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/retirement/Retirement.hpp)、[`TextureStorage.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/texture/TextureStorage.hpp)。

为保证提交后退休不因分配容量失败，当前先 `reserve()` 一条 RetirementRecord，专业 Backing 又保存 `last_use/live/reserved/retired/lease`。**预留可避免晚期失败这一设计依据存在**，但现在多层生命期证据混在一起。目标由真实 RAII 资源与一个 Retirement 提交接口承担安全释放，冷态可提前准备容量而无需每张纹理持有多个同义标志。不能在缺少真实跨 Queue/partial-submit 测试时删除预留行为。

## D13 — RetirementQueue 同时存在 Queue 候选列表与泛型资源所有权（P1，RESEARCH）

**位置：** [`Retirement.hpp` L42–149](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/retirement/Retirement.hpp#L42-L149)。

`StableSlotMap<Entry>`、`waiting_[3]`、`ready_`、Entry 链表与 Variant<Buffer,Image,Composite> 负责同一退休逻辑，设计上比一个线性 Queue 复杂得多。**它不是无理由：资源可能同时被多物理队列使用，不能只靠一个 Queue 的 FIFO serial 决定回收。** 可研究在实际资源保留/提交计划里直接归并必要条件；只保留一个有证据的最终清偿设施。不得为了“简洁”错误地按任意帧号回收跨 Queue 对象。

## D14 — `DescriptorArray` 使用 `weak_ptr` 配合引用 VulkanDevice 的对象（P1，RESEARCH）

**位置：** [`DescriptorArray.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/DescriptorArray.hpp)、[`DeviceSession.cpp` 与 `DescriptorArray.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/DescriptorArray.cpp)。

Session 缓存 `weak_ptr<DescriptorArray>`，调用返回 `shared_ptr`，后者持有 `const VulkanDevice&`，析构销毁 DescriptorPool/Layout。Session 关闭时需要 `expired()` 检查，不是自然成员生命周期。建议让 DeviceSession/Native Device 明确拥有 DescriptorArray，执行/纹理引用是借用或由既有 GPU Retire 保活。若某些旧计划需要延迟使用，证明为何不能移动拥有型布局资源到退休节点后再删除 shared/weak 组合。

## D15 — 物理资源类别、设备初始化与专业逻辑使用了同一错误通道（P2，RESEARCH）

**位置：** [`memory/Memory.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/memory/Memory.cpp)、[`device/Device.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/device/Device.cpp)。

`kWrongOwner`、`kNativeFailure`、`kStorageCapacity` 等在多层混用，且一些内部已验证的 Buffer 借用在每次 read/write 仍调用 `owns(&buffer)`。外部传入 Handle 需要边界验证；内部引用经创建/借用已经保证时不必每次重查全 Session。按 API 边界进行一次类型/身份验收，不再创建 `validated_` 布尔记录。真实设备故障等仍需返回。

---

# 3. Command / IPC / Registry：泛型框架也被写成了状态网络

## C01 — `CommandEntry::active` 与 `cold` 双重权威（P0，MERGE/DELETE）

**位置：** [`CommandRegistry.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/pinclude/CommandRegistry.hpp)。

```cpp
struct CommandMetadata { CommandId id; CommandBinding binding;
    std::vector<SupportedHandler> supported; HandlerId selected; };
struct CommandEntry { HandlerCall active; std::unique_ptr<CommandMetadata> cold; };
```

`cold` 不是可读的业务名；一个注册对象的声明、支持的 Handler、当前绑定和执行函数分散在不同内存对象。优化来源的冷热标签不是长期设计依据。**方向：** 一个 Command 记录保存真实 Descriptor、Handlers 和 selected 索引；当前函数由其选中记录取得，避免第二份 `active` 状态。若经过真实 Profile 证明已选中的函数指针需要热缓存，可以只缓存一个不可变条目索引/紧凑值并证明同步更新，不保留泛化双重权威。

## C02 — Command 查找/Handler 查找仍对整个目录线性扫描（P1，INDEX）

**位置：** [`ServerRegistration.cpp` L1–26, L187–214](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/src/ServerRegistration.cpp#L1-L26)。

`CommandRegistry::findCommand(descriptor)` 遍历所有 `command_entries_`，`findHandler(type,key)` 遍历所有 `handler_owners_`。这些通常是冷态发现/安装，可接受有限规模，但不是必然要求 O(n)；在需要反复发现/选择时应有类型/名称索引。**不要为了字符串查找创建一个 hash 大表**：冷装配期排序目录或直接稳定 CommandName→ID 索引可以用，一旦得到 CommandId 派发必须 O(1)。

## C03 — Handler 初始化从构造函数形状猜 DeviceSession 依赖（P0，DELETE）

**位置：** [`VulkanServer.hpp` 的 `registerFeature` 与 `storageCandidate`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/include/lux/engine/render/runtime/VulkanServer.hpp)。

当没有显式 Args 且 `H` 可从 `DeviceSession&` 构造时，系统自动注入 Native Device。这是类型检查在猜业务意图，扩展 Handler 时行为不透明。**目标：** 装配层明确传入需要的依赖；`requires` 只校验真实签名，不能决定对象应属于哪一个设备生命周期。

## C04 — `storageCandidate` 一个方法跨越四个 Registry 责任（P0，DELETE/MOVE）

**位置：** [`VulkanServer.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/include/lux/engine/render/runtime/VulkanServer.hpp) 与 [`ServerRegistration.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/src/ServerRegistration.cpp)。

它准备 StorageFactory、ConfigValue、Handler、CommandBinding 和依赖去重；另一层再次执行 `prepareStorage/installStorage/installFeature`。核心 Server API 已因此需包含大段 Native 模板实例化。**目标：** 明确一次 Cold Install 的拥有型局部事务；StorageFactory 只描述实例构造，CommandBinding 只描述方法，Server 装配函数最后一次发布。不为此新增一组永久 Manager；过渡期 `StorageInstall` 的职责在事务中完成后及时消失。

## C05 — `HandlerEntry` `unique_ptr<void,deleter>` 与重复类型 Token（P1，SIMPLIFY）

**位置：** [`RenderServer.hpp` L89–123](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/include/lux/engine/render/runtime/RenderServer.hpp#L89-L123)。

异构 Handler 的单次类型擦除可以合理；当前 HandlerEntry 同时持 ID、TypeToken+size/alignment、name、key、object，并在注册时与 Provider/Factory/Command 各维护一组相同的类型描述。应复用一个中立 C++ type descriptor，转换仅发生于安装边界。**不得把动态类型检查移到逐请求 Handler hot call。**

## C06 — 资源 Factory 同时存在中立声明、Native Type、Config Type、Ops Type 和 BindingOps（P1，MERGE）

**位置：** [`Factory.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/resource/include/lux/engine/render/resource/Factory.hpp)。

```text
StorageDescriptor*
void* Ops
void* BindingOps
Create(void* Device, StorageId, void* Config)
Compatible(void* Native, void* Config)
StorageCppType(Config/Ops/Native)
```

类型擦除是插件边界的合法选择，问题是同一专业 Storage 需要显式维护过多中立/Native 镜像声明，读者必须追查两个同名类才能理解真实接口。建议把专业 `description/Config/Ops/Factory` 声明贴近专业 Feature/Storage 的中立头，将 Native 具体 Factory thunk 与 Native 专业实现同域；一次编译期认证后，Runtime 执行通过已绑定函数，不再每层重复类型合同。不能通过 `void*` 把本来可类型化的 GPU 热路径重新擦除。

## C07 — `ConfigValue::Type`、`StorageCppType`、`CommandValueType` 同义表示（P1，MERGE）

**位置：** [`ConfigValue.hpp` L29–40](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/data/include/lux/engine/render/data/ConfigValue.hpp#L29-L40)、[`Factory.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/resource/include/lux/engine/render/resource/Factory.hpp)、[`Protocol.hpp` L45–72](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/protocol/include/lux/engine/render/protocol/Protocol.hpp#L45-L72)。

均有 `TypeToken,size,alignment`，只因模块不同换了名称。**这是可以确认的重复值结构**，可以复用已有通用 TypeDescriptor（若 lux-cxx 已存在则直接用）或一份最小中立值类型；不要再为每个场景新定义类型族。字段语义若有真实差异须写明。

## C08 — `ConfigValue` 在所有动态配置上用 `shared_ptr<const void>`（P2，RESEARCH）

**位置：** [`ConfigValue.hpp` L25–129](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/data/include/lux/engine/render/data/ConfigValue.hpp#L25-L129)。

复制冷态不可变 Config 的确可利用共享拥有，不能因 Legacy 少用 shared_ptr 就直接删。**问题是它已经成为通用 Config 机制的唯一存储策略，并再经静态 Metadata/equal/retainedBytes/type/token 组成复杂类型擦除。** 如果配置只在某一次安装/编译中使用，优先移动值或小对象直接拥有；只有跨 Effect/Feature/异步编译长期共享才使用共享不可变值。必须以真实引用数和分配次数决定，不能推测性能已受损。

## C09 — `render/protocol` 不再做二进制 Wire，模块名称/依赖不匹配（P0，MOVE/DELETE target）

**位置：** [`Protocol.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/protocol/include/lux/engine/render/protocol/Protocol.hpp) 和 [`protocol/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/protocol/CMakeLists.txt)。

它当前主要声明 CommandId、HandlerId、CommandName、请求/回复 C++ Type、CommandFailure 和类型化 CommandConcept。功能应保留，但归入 `render/commands` 或适合的中立合同层。确认 Client-only 外部消费者后删除名义上独立的 `render_protocol` 目标，不留永久转发头。

## C10 — `render/data/Encoding+Schema` 仍维护另一套 Wire 合同（P1，NEEDS_CLOSURE）

**位置：** [`Encoding.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/data/include/lux/engine/render/data/Encoding.hpp)、[`Schema.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/data/include/lux/engine/render/data/Schema.hpp)。

有 `DataSchemaDescriptor::schema_version/encoding_version`、`encodeData/decodeData/SharedBytes`，与当前进程内类型化 IPC 无直接必要关系。**不能直接证明全仓无消费者**：可能存在外部持久资产/工具链，应做安装 SDK/全仓链接闭包。若无真实用途，直接 DELETE；若有实际持久化用途，MOVE 至已有序列化/资产模块，不继续作为 Render 基础依赖。

## C11 — 一部分“命令/请求/结果”结构成为 Runtime 的混合头（P1，MOVE）

**位置：** [`runtime/Commands.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/include/lux/engine/render/runtime/Commands.hpp)。

289 行一个头塞 Scene/View/Camera、Resource Find/Prepare、Effect、Readback、Command目录、Stats、Transfer等多域公共值，甚至多个连续空行。按中立 Command 值类型分组，专业 Storage 的请求声明放专业接口附近；通用 Server/Client 命令契约独立于 Runtime 私有实现。无需“一 POD 一头”，按真实使用界面/消费者分组即可。

## C12 — `StorageSet::find` O(n)，且清理过程在未解决 Lease 中反复扫描（P0，INDEX/RAII）

**位置：** [`StorageSet.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/resource/src/StorageSet.cpp)。

`find(type,key)` 线性遍历 `records_`；`clear()` 反复 `find_if` 找 `leases == 0` 的记录。一般全可释放时可能接近 O(n)，但是**若前方存在持续借用记录，反复扫描可在最坏情形退化 O(n²)**。更根本问题：`preloads/leases/closing` 和 Registry 位置/Native 指针组合出多种人工生命周期状态。

**目标：** 实际 StorageSet 独占 Storage；Registry 仅借用索引；Effect/异步操作拥有必要明确引用；ID 直接查。业务名字只作为一次冷态 getOrCreate，之后使用 generation ID。若仅优化 `clear()` 而不收敛状态，无法取得结构性收益。

## C13 — `ResourceRegistry::typeId()` O(types)，安装类型批次 O(n²)（P1，INDEX/RESEARCH）

**位置：** [`ResourceRegistry.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/resource/src/ResourceRegistry.cpp)。

按 SchemaName 查 Type 遍历 `types_`；安装 N 个候选每次再与以前候选两两比较。后者只在明确冷态、N 较小可有意识接受，不能“理论上最多几个”而没有规模上限。Type 目录可建立有序索引或直接维护稳定注册映射；拿到 TypeId 后绝不重复字符串查找。避免用任意 64 位 name hash 作为 SparseSet 下标。

## C14 — 同一个 Storage Type 同时表现为逻辑声明类和 Vulkan 镜像实现（P0，MOVE/RENAME）

**位置：** [`resource/Instance.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/resource/include/lux/engine/render/resource/Instance.hpp) 与 [`native/vulkan/instance/InstanceStorage.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/instance/InstanceStorage.hpp)；Mesh/Material/Texture 亦有类似形态。

中立声明与 Vulkan 具体实现分离是**正确的依赖原则**，但专业类型名称重合、实例 ABI 固定为 Mesh/Material/Submesh/Mat4、`StorageDescriptor` 与 Factory 远离实际算法，导致整个框架用“唯一通用 InstanceStorage”塑造其他 Feature。按专业领域命名（如 MeshInstanceStorage、SplatStorage），中立声明、Config、`static constexpr description` 和 Typed Ops 在同一个功能目录；具体 Vulkan 数据对象放同功能的 Native 实现，不让中立 public header 包含 Vulkan。

## C15 — Request 业务错误、Handler 初始错误与 GPU 终态被同一类外围类型反复规约（P2，MERGE）

**位置：** [`CommandBinding.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/include/lux/engine/render/runtime/CommandBinding.hpp)、[`CommandDispatchContext.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/pinclude/CommandDispatchContext.hpp)、[`ServerEffect.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/src/ServerEffect.cpp)。

当前 OperationState Owner 已由 stdexec 接管，是应保留的成果；但请求初始错误经 `variant_sender`、CPU `upon_error`、RenderScheduler、`then(adopt)`、终态 `CommandCompletion` 等再次变换。一些转换必要，但业务层类型名混乱、CreateEffect 私有 Scheduler 说明标准机制仍未真正与唯一 RenderLoop 结合。**目标：** 标准 Sender 组合清楚表达 CPU→RenderThread→Reply，所有错误只在必要边界转换一次，不恢复另一套 Handler OperationState。

## C16 — `CommandCompletion` 同时被用作最终 Reply 记录与中间 CPU→RenderThread 工作邮槽（P1，RESEARCH）

**位置：** [`Deferred.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/pinclude/Deferred.hpp)。

```text
ResponseRoute + IpcMessage
CompletionQueue* + publishers*
ready_next
void* continuation + CpuResume
continuation_result
测试 measurement
```

一份结构里混了 IPC Reply 数据和泛型 RunLoop continuation，故所有中间调度都要携带一个 Request 的回传 landing place。**合理目标**是只保留一条真正的 RenderThread ready queue，但分清 RenderThread 调度节点与拥有型 Reply。是否要两个记录结构，必须核对对象尺寸/分配、同步完成和不可取消 Reply；**不得因此引入第二条队列**。

## C17 — `RenderScheduler` 放在 ServerEffect.cpp 的匿名命名空间（P1，MOVE/MERGE）

**位置：** [`ServerEffect.cpp` 约 L115–168](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/src/ServerEffect.cpp#L115-L168)。

这里手写了 `RenderScheduler::Sender::Operation`，借 `CompletionPost` 作为排队/回到 RenderThread 的入口。调度机制是所有可能的异步 Command 共用的 Runtime 接缝，不应藏在 CreateEffect 私有实现中；要么作为唯一事件循环/调度入口公开最小 borrow，要么若实际上只有 CreateEffect 使用并无净简化，则保留最小局部适配但明确命名为本地工作，不扩大为通用 Scheduler。新旧 stdexec 迁移已收口，不能无限重构它。

## C18 — `ResponseOutbox` 通过自制 index free-list+FIFO 管理回复记录（P2，RESEARCH/KEEP）

**位置：** [`ResponseOutbox.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/pinclude/ResponseOutbox.hpp)。

`responses_` + `head_/tail_/free_/size_` 保证出队和重用都近似 O(1)，是一个**有性能价值**的自制容器；不能因“自制”直接删。问题有两项：它又负责对失效 Requester 丢弃回复；`flushTo()` 中 `delivered` 同时统计已发送和终止丢弃，名称含糊；重复 `*_COST_SEAM` 污染主流程。可考虑使用已验证的简单 Slot/Ring 类型替代，但若会增加分配或更多状态，不动 FIFO 算法，只清除命名/测试侵入。

## C19 — 公开 RenderServer.hpp 含安装事务和 Native Handler 类型擦除细节（P1，MOVE）

**位置：** [`RenderServer.hpp` L37–147](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/include/lux/engine/render/runtime/RenderServer.hpp#L37-L147)。

类公开 API 只有 `run/stop/stats/register` 等，但私有模板不得不将 `HandlerEntry`, `StorageInstall`, `storageCandidate`, `prepareHandler`, `publishHandler` 都暴露给用户 TU 的头文件编译。应将 Vulkan Provider 安装放入专业装配头/单次注册入口，主 Server 声明保持窄。不要为了隐藏模板使用二次运行时 type erasure 或包装每个专业 Storage 的虚基类。

---
# 4. Runtime Scene/Effect、RenderThread 调度与 Readback

## R01 — Camera 是 Engine/专业 Feature 的概念，却成为 Core 与 Server 的强制 ABI（P0，DELETE/MOVE）

**位置：** [`core/Camera.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/core/include/lux/engine/render/core/Camera.hpp)、[`runtime/Commands.hpp` L27–31](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/include/lux/engine/render/runtime/Commands.hpp#L27-L31)、[`ServerImpl.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/pinclude/ServerImpl.hpp)、[`ServerScene.cpp::updateCamera`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/src/ServerScene.cpp#L310-L353)、[`ServerEffect.cpp::compileEffect`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/src/ServerEffect.cpp#L58-L65)。

`CreateViewRequest` 内建 `CameraState`，通用 View 保存相机，RenderServer 扫 Effect/Group 为每个 Native Context 写投影矩阵；Graph 编译时检查全部活 Pass 的 `context_parameters == cameraParameterBlock()`。**这是跨 Core、Command、Runtime、Native、Shader 的同一业务污染链**。Legacy 的 [`CameraExtraction`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/render_legacy/engine/scene/builtin_systems/render/src/CameraExtraction.cpp) 和 [`ViewCameraOperationHandlers`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/render_legacy/modules/function/render/features/src/assembly/view_camera/ViewCameraOperationHandlers.cpp) 已将其放到专业 Feature 域。**必须删除 Core Camera/UpdateCamera 整条依赖，保留中立 View/Pass 参数输入和 Vulkan Viewport/Scissor 的真实图形语义。**

## R02 — Scene/View/Target/Effect/ExecutionGroup 关系没有直接索引（P0，RESTRUCTURE）

**位置：** [`ServerImpl.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/pinclude/ServerImpl.hpp)、[`ServerScene.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/src/ServerScene.cpp)。

`ExecutionGroup` 只保存 `(SceneId,ViewId,TargetId,ExecutionContext*)`，由 `Effect` 中一组 vector 持有。更新 View、删除 Scene、删除 View、Target Resize 都遍历大量 Effect/Group，甚至为了判断同 Target 有无其他 Effect 而全局搜索。这里每个对象都已有 generation 句柄，运行期完全可以维护受影响的关联 ID 列表。

**目标：** 一张唯一拥有型 Attachment 记录表；Scene/View/Target/Effect 存不拥有的 AttachmentId 与直接删除位置。一次 Attach/Detach 统一更新这些索引，删除 View O(其实际 Attachment 数 K)，不扫全部 Effect。若 Target 有严格顺序则按实际顺序的容器存 ID，定位已有对象时不扫描整表。**Attachment 不是给现有四元组加一层 Owner/Version 的包装；它必须删除旧 EffectGroup 的同义权威。**

## R03 — `RenderEffect` 同时包含 `shared_ptr<CompiledGraph>`、动态 arguments、全部 groups 和轮询索引（P1，MOVE）

**位置：** [`ServerImpl.hpp` 的 RenderEffect](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/pinclude/ServerImpl.hpp)。

不可变编译计划应由 Effect 单一拥有或按真实设备复用；动态 Resource Argument 属于实际 Attach/Frame 输入，不应成为 Effect 级共享权威；各 Group 执行运行期状态属于 Attachment；轮询位置属于调度或运行列表。共享 `CompiledGraph` 目前可能是为了多 Context 借同一计划——**这种真实共享需尊重 GPU 在途安全**，但通常一个 Effect 可以持有计划而其 Attachment 借用，旧计划替换时移动拥有型对象入退休完成点；不能预设全用 shared_ptr 最好。

## R04 — `advanceRendering()` 每轮遍历所有 Effect/Group，轮转索引做模运算（P1，RESEARCH）

**位置：** [`ServerLoop.cpp` L5–30](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/src/ServerLoop.cpp#L5-L30)。

每个 RenderThread tick 都执行二重循环，计算 `(next_effect+e)%effects.size()` 与 `(next_group+g)%groups.size()`，对所有 Context 尝试 `execute()`。空 Context 也可能一轮轮返回 PreviousSubmissionPending。**这不是一定需要复杂 Scheduler 的证据**，但轮转索引和全表每轮尝试是否带来可见公平收益需实测。目标是只遍历实际活动的 Target/Attachment（稠密 active 列表），GPU 已满时用已有完成进度判断本次是否适合录制。不得加一个庞大的 `RenderWorkManager` 替代一个小循环。

## R05 — GPU 完成不可主动唤醒，空闲 tick 带 1ms revisit deadline（P1，性能风险 RESEARCH）

**位置：** [`ServerLoop.cpp` L197–237](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/src/ServerLoop.cpp#L197-L237)。

在没有 IPC/CPU 进展时调用 `IpcCore::waitUntil(observed, now()+1ms)`，因为 Fence 自身没有 CPU 通知到该 IPC wait。这里不是 `sleep_for` 限速：空闲等待合理。但对于 RenderThread 刚好被 GPU 进度阻塞的场景，完成检测可能额外等待约一个检查周期（调度不确定）。在 300 FPS 约 3.333ms 帧预算下值得比较：旧逻辑、GPU completion event/fence wait、短轮询的 CPU 占用、帧 p99 与资源 Ready 延迟。**暂不宣称造成了 FPS 损失，也不能仅因存在 `waitUntil` 就认为违背满速原则。**

## R06 — Native `ExecutionContext` 既是实例、准备器、Descriptor Rewriter、提交器、Readback 宿主（P0，MOVE/SPLIT RESPONSIBILITIES）

**位置：** [`ExecutionContext.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/ExecutionContext.hpp)、[`Execution.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/Execution.cpp)。

一类含 `shared_ptr<CompiledGraph>`, StorageSet, StorageLease, PreparedStorageBinding, bindings/pass/barriers, DescriptorPool, two banks, Context parameter bytes, `last_use_`, `Progress`, `readbacks_` 链表和 `readers_` 计数；方法含 prepareDescriptors, rebind, setParameters, execute, readback, cancelReadbacks。**它把 RenderGraphInstance + Executor + 输出读取 + GPU last-use 事务放成了长期综合对象**。RGAM 目标原本已有 `RenderGraphInstance`、`RenderExecutor`、`FrameResources` 三种职责，建议在这些真实职责下拆分，不在每个 Pass 新建虚表执行器。

## R07 — `ExecutionContext::setParameters(GraphPassId)` 仍线性寻找 Pass（P1，INDEX）

**位置：** [`Execution.cpp` L308–337](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/Execution.cpp#L308-L337)。

GraphPassId 已有 `index`，却通过 `std::ranges::find_if(passes_)` 按 `recipe->source` 匹配。若只保留 live Pass，应在编译时一次建立 `GraphPassId.index → active pass index` 的数组（或保留按 source 直接放置的数组），后续 O(1)；Pass 参数写入热路径不能变成 O(全部 Pass)。

## R08 — Runtime 参数改变时 Storage Use 回调调用整组 `rebind()`（P0，RESTRUCTURE）

**位置：** [`StorageBinding.hpp` L12–25](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/StorageBinding.hpp#L12-L25)、[`Execution.cpp` L175–307](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/Execution.cpp#L175-L307)。

`PreparedStorageBinding` 持 `void* update_context`、函数指针 `updated`、`unique_ptr<void,fn> reference`、`SubmissionRequirements* last_use`；Storage 发布新 Backing 后遍历订阅者，修改 Binding 指针并回调 `ExecutionContext::rebind()`，后者重写相关 Descriptor/参数状态。**数据更改从 Storage → O(所有 Use) → 多个 Context 重绑**，引入生命周期链表与一系列函数指针。应改为 Graph/Attachment 预先确定可用资源槽与动态参数范围，在明确的帧准备边界更新受影响绑定；Storage 管 Dirty 数据，不长期反向通知并借用整个 Context。对实际 Descriptor 地址变化需合法重绑，但不一定等价于逐次重新扫描/发布所有绑定。

## R09 — Frame Slot 与 GPU submit lifetime 仍是单 Context 单在途（P1，功能边界 KEEP/EVOLVE）

**位置：** [`ExecutionContext.hpp` 的 `last_use_`、descriptor_bank_、submitted_bank_](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/ExecutionContext.hpp)，[`Execution.cpp::execute`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/Execution.cpp#L362-L520)。

如果上一笔提交还未完成，该 Context 可能返回 `PREVIOUS_SUBMISSION_PENDING`。SubmissionQueue 有多个 CommandBuffer Slot 不等于 Context 已支持两个或三个 FrameResources 并行。本稿不要求立即实现 FIF2/3，但未来 RGAM 的 History/多 View 必须以真实帧槽资源与提交进度完成，而不是在 Context 继续堆 `bank/submitted/started` 组合状态。

## R10 — `ReadbackOperation::arm()` 内部主动调用 `context_->execute()`（P0，MOVE）

**位置：** [`Readback.cpp` L63–108](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/Readback.cpp#L63-L108)。

一个叫 Readback 的异步操作在没有可用提交时可能发起新一轮 GPU 渲染，这不是普通“读当前输出”的 API 合同。应由明确的 Render/Readback 序列负责是否触发执行；Readback 专注于已准备结果的复制、真实 GPU 完成后拷贝 CPU 字节并回复。独立 Sender 作为 Native 完成源可合法存在，但不能借此保留 Context 的 Readback 双向链表和业务副作用。

## R11 — Readback 绑在 Context 的 `readbacks_` intrusive 链表 + `readers_`（P1，RESEARCH）

**位置：** [`ExecutionContext.hpp` L52–117](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/ExecutionContext.hpp)、[`Readback.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/Readback.cpp)。

`ReadbackOperation` 自身有 Owner/Complete Fn、`next_/previous_`、Queue Callback、cancel，Context 保持 Readback 链表和读者数以阻止提前释放。必要的真实 GPU 完成要保留；但 `stdexec` 已管理外层 CPU OperationState，需研究能否让一次 Readback 持有其必要资源借用和 Native completion，并在 Frame 执行完成时交付，不再把读者管理变成 Context 的另一类长期状态。不能提前析构已 armed 的外部 GPU 回调借用。

## R12 — Effect 的 Target 与 Feature 的 TargetDescription 形成多份配置权威（P1，RESEARCH）

**位置：** [`runtime/Commands.hpp::CreateEffectRequest`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/include/lux/engine/render/runtime/Commands.hpp#L90-L96)、[`features/MeshFeature.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/features/include/lux/engine/render/features/MeshFeature.hpp)、[`CompiledGraph.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/CompiledGraph.hpp)。

Effect 创建请求有 TargetDesc，MeshFeatureConfig 也携带 TargetDesc，编译结果再缓存 TargetDesc、Runtime Target 对象又有 epoch/description。某些属于目标格式/专用 Depth 输出合同，未必都同义；必须建立**一个实际输出兼容性/重编译键**，防止 resize 只能通过多层描述恰巧同步。Feature 不强制知道某个 Target 的具体身份；Graph 编译按描述/输出输入而非 Server TargetId。

## R13 — Server 统计字段/启动停止标志渐成次要状态网（P2，RESEARCH）

**位置：** [`ServerImpl.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/pinclude/ServerImpl.hpp)。

`stop_requested/started/running` 与 `ticks/commands/compiled_graphs/submissions/validation_errors/validation_warnings/scoped_buffers_created` 分散存放。停止和可并发 stats 是实际接口，不能机械减少标志；但应按真实并发读者区别无锁计数与 RenderThread-only 数据。`scoped_buffers_created` 更像阶段性验证指标，不应永久成为 Runtime 业务 API。按当前消费者分类，演示/验收计数迁到诊断 sidecar。

---

# 5. Graph / Feature / Shader：目标架构与当前实现之间的差异

## G01 — `PassProgram` 是所有 Pass 种类字段的混合体（P0，REPLACE）

**位置：** [`graph/Program.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/graph/include/lux/engine/render/graph/Program.hpp)、[`graph/Draw.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/graph/include/lux/engine/render/graph/Draw.hpp)。

一个 `PassProgram` 同时保存 Shader、ClearColor、Depth、`VDrawCommand`、GraphicsState、VertexBuffers、DispatchCount、Storage/Context Param、ProgramResourceBinding。每个 Pass 包含大量与自身种类无关字段。它是演示期缩小 Compiler 实现的权宜模型，增加 Compute/Transfer/MRT/IndirectCount 会愈发膨胀。**目标：** 专业类型化 Pass 参数和 WorkBody，Shader 接口/参数布局归 Cooked Shader，录制原语进入唯一 RenderPlan；不为每种 Work 再加一套新的 Graph DSL。

## G02 — `CompiledPass` 更严重：所有 Native Recipe 的参数塞进一个结构（P0，REPLACE）

**位置：** [`CompiledGraph.hpp` L25–92](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/CompiledGraph.hpp#L25-L92)。

`ERecipe/DRAW/DISPATCH/CLEAR/FILL/COPY` 同时保存 `color/depth/source/destination/pipeline/images/buffers/load/clear/draw/index/indirect/count/dispatch/begin_rendering/end_rendering/bind_pipeline` 等大量互斥字段。大部字段在每条路径无效，Executor 通过 switch 读少数。当前 RGAM 要求的 RecordingProgram/typed arguments 恰好是为了避免它。这不是“需要更美的 enum”问题，而是**缺少正交的物理录制计划**。按目标迁移当前真实支持的 Graphics/Compute/Transfer，不建平行 CompiledGraph 和 RenderPlan。

## G03 — `CompiledGraph.hpp` 公开包含 `DeviceSession.hpp`，编译器依赖完整 Session（P0，MOVE）

**位置：** [`CompiledGraph.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/CompiledGraph.hpp)、[`Compile.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/Compile.cpp)。

Native 编译产物和编译函数通过整个 DeviceSession 访问能力。编译计划并不需要认识全体 StorageSet、UploadState、IPC。目标用 `VulkanDevice`/必要 Shader/Layout 创建依赖和已解析图输入；不要增加 `CompilerContext` 巨大转发对象，只收窄必要参数与 include。目标是独立 Native Graph 消费者可以不链接 Runtime/专业 Storage。

## G04 — `compileEffect()` 反复按字符串查找 GraphPass/Program/Resource（P0，INDEX）

**位置：** [`ServerEffect.cpp` L11–110](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/src/ServerEffect.cpp#L11-L110)。

它对每个专业 Program 用 `none_of(passes,key)` 验证、对每个 Program 用 `any_of(analysis.execution_order,key)` 判断 live、对每个 Argument 用 `find(resources,key)` 再扫描四组 external binding 和 lifetime、最后对每个活 Pass 用 `find(programs,key)` 再查一次。这是明显重复的关系发现。**目标：** 装配一次建立作者 Name→GraphPassId/GraphResourceId/Program Index，编译链后半段全部使用紧凑 Index/Span。可以用暂态有序索引或同次装配的直接位置，不必为每个阶段都创建 HashMap。每个索引的数据生命周期只覆盖编译。

## G05 — `Compile.cpp` 640 行、单 `compileGraph` 约 400+ 行、多阶段混杂（P1，REORGANIZE）

**位置：** [`Compile.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/Compile.cpp)。

一段编译函数混合：Target 校验、Resource 索引、Pass Program 匹配、Shader/Layout 规划、Barrier/访问跟踪、Pipeline 创建、Recipe 降低、Rendering Scope 合并、最后输出/Readback 决定。文件 640 行但只有约 11 空行（静态源码计数）。应按实际**编译阶段的纯函数**拆分文件，而不是把一个函数拆成七个 `Manager`。同一张 GraphAnalysis 不能在 Native 再建立第二套依赖权威。

## G06 — `LayoutPlan.cpp` 在 Shader 接口/资源名称之间线性匹配，魔法 stage 位（P1，MERGE/INDEX）

**位置：** [`LayoutPlan.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/LayoutPlan.cpp)。

它通过字符串 `shader_field`、`resource_key` 重复查 ShaderBlock fields 和 Graph Resource；Vertex=1/Fragment=2/Compute=4 的 stage mask 在多个源文件手工重写。冷编译可接受少量 O(n)，但 Name→ID 和 Shader stage 布局应在一次编译合同中统一。不要复制一份 `ShaderInterface` 作为新的反射真理。

## G07 — `Shader.cpp` 独立实现完整 SPIR-V 验证、反射校验、Pipeline、Module 生命周期（P1，SPLIT BY ALGORITHM）

**位置：** [`Shader.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/Shader.cpp)。

576 行、约 11 空行；既有 SPIRV-Tools validator、SPIRV-Cross reflection 校验、资源位置验证、DescriptorSetLayout 构建、Compute/Graphics Pipeline 构建及 ShaderModule RAII。问题不是“功能都错”，而是同一文件横跨多个可独立推理的算法阶段。目标：ShaderABI validation、Layout lowering、Pipeline creation 三部分按函数与 file 清晰划分，共用唯一 Cooked ShaderInterface；ShaderModule 暂时拥有型局部对象本身是值得保留的正确做法。

## G08 — 同义的 Shader Metadata 来自 Render Data 和 `rdesc::ShaderInfo`（P0，MERGE）

**位置：** [`render/data/ShaderInterface.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/data/include/lux/engine/render/data/ShaderInterface.hpp)、[`modules/resource/description/ShaderInfo.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/resource/description/include/lux/engine/description/ShaderInfo.hpp)。

都有 DescriptorSet/Binding/Name/Stage/访问事实；前者仅定义 sampled image array 与 storage buffer 两类 DescriptorKind，当前 Native 验证过的范围不等于将来 Shader 系统全范围。目标一份真正的中立 ShaderProgram/ShaderInterface，Cooked 反射来自合法编译链，在 `rdesc` 层表示。若布局 relocation 需要特殊事实，列出映射表，不留长期的“Render Shader 和 Resource Shader 各自解释一套 set/binding”。

## G09 — `GraphAnalysis` 自身不应被这轮重写（P2，KEEP/LOCAL OPT）

**位置：** [`graph/src/Analysis.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/graph/src/Analysis.cpp)。

现有依赖图、资源版本、拓扑、RAW/WAR/WAW、区间/生命周期分析是有价值基础。源码有 `vector<vector<uint32_t>> edges` 与 `priority_queue` 来得到稳定拓扑顺序，部分区间比较可能是 O(R²)，**但它们位于冷编译期**，不能仅以 O(n) 为由重写成更复杂的执行图。真实需要优化时按输入规模/编译 Profile 改为扁平邻接或预分配，不再造另一个 `RenderGraphIR`。分析和 Native Plan 独立性仍是硬约束。

## G10 — Feature 装配存在一套字符串 Claim 编排，Renderer 专业算法仍是手工固定示例（P0，MIGRATE）

**位置：** [`features/src/Assemble.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/features/src/Assemble.cpp)、[`features/src/MeshFeature.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/features/src/MeshFeature.cpp)、[`native/vulkan/src/features/MeshFeature.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/features/MeshFeature.cpp)。

`MeshFeatureConfig` 固定 Mesh/Material/Instance Storage 名称、RGBA8/D32、Camera 128B、set/binding、CPU members Indirect；这些是一个最小样例能力，不是全局 Feature 合同。RGAM 已要求类型化端口、通用 compose、专业 Feature 贡献自己的 Pass；**保留现有装配算法中有价值的依赖/去重，不让一个 BasicMesh 示例决定所有高阶渲染必须用相同存储。**

## G11 — 专业 Feature 的 `StorageRequirement` 和 `PassProgram` 使用大量字符串，降低编译可读性（P1，INDEX/LOCALIZE）

**位置：** [`resource/Binding.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/resource/include/lux/engine/render/resource/Binding.hpp) / [`graph/Program.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/graph/include/lux/engine/render/graph/Program.hpp)。

`resource_key`, `pass_key`, `shader_field`, `source_field`、Scope 内的 `global/scene/view/attachment` 多组向量。作者名称用于装配和诊断合理；进入 Native Compiler 时再以字符串遍历不得成为必需接口。目标接入一个冷态 Name→Index 的装配结果，让执行路径只接受 NumericResourceID + CompiledParameterOffset，消除重复分配字符串与多次查找。

## G12 — Context 的固定 Camera ABI 与 Feature Shader 物理绑定已渗透 Native（P0，DELETE/MOVE）

**位置：** [`native/vulkan/src/features/MeshFeature.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/features/MeshFeature.cpp)、[`ExecutionContext.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/ExecutionContext.hpp)。

Feature 写死 Shader set3,binding7、set2,binding6、set4,binding4 和 `cameraParameterBlock()`。这是具体样例 Shader 的实际 ABI，但不应演变为通用 Shader binding 的固定内建规则。必须用 Shader Interface + 专业 Feature 的参数合同驱动编译，Camera 只作为 Engine/专业 Feature 输出；通用编译与 View 不含相机语义。

---

# 6. 专业 Storage、GPU 数据布局与算法偏移（本节是为了验证框架边界，不是唯一审查对象）

## S01 — `InstanceStorage` 是特定 MeshInstance，不是可扩展的通用 Instance（P0，REPLACE）

**位置：** [`resource/Instance.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/resource/include/lux/engine/render/resource/Instance.hpp) 与 [`native/vulkan/instance/InstanceStorage.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/instance/InstanceStorage.cpp)。

当前 `InstanceDescription = MeshId + MaterialId + Submesh + mat4`，`InstanceBinding = MATRICES/DRAW`，一个 `InstanceStorage` 名字占用了整个专业类别。Legacy 不是该固定模型，未来 Skinned/Particle/Splat 也不应修改一份全局 Instance enum。专业声明、配置、`static constexpr description` 就近放在 MeshInstance 类型旁；Vulkan实现位于同专业目录，不为两层添加镜像同名类型。

## S02 — Instance 三套完整 GPU Bank + Entry/Shadow 两套 CPU Transform（P0，REPLACE DATA LAYOUT）

**位置：** [`InstanceStorage.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/instance/InstanceStorage.hpp) 和 [`InstanceStorage.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/instance/InstanceStorage.cpp)。

`entries_.description.transform`、`shadow_`、`Banks[3]`，Backings 各持 `dirty/pending/last_use/retirement`。更新一次 Transform 协调三份状态，频繁选可写 Bank 并重新绑定；这是为了防止 GPU 读写冲突而重建的 CPU 快照系统。Legacy 已有 [`TSparseInstanceStream`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/render_legacy/modules/function/render/vulkan/sinclude/lux/engine/render/resources/mesh/SparseInstanceStream.hpp) 的独立字段流、脏页、分页上传。**目标是保留后者的真实算法，使用新框架的 stdexec/UploadContext 执行，不是继续补齐三 Bank 并维护更多标志。**

## S03 — Instance 批量 Transform 做了 Engine 已负责的排序去重（P0，DELETE）

**位置：** [`InstanceStorage.cpp::update`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/instance/InstanceStorage.cpp#L233-L289)。

每次 K 项 `iota(order) + sort + unique`，时间 O(K log K)，额外分配索引向量，随后复制旧 Bank Dirty 再写当前值。用户早已规定 Engine 负责去重，RenderServer 只接收“已经确定发生改变的 K 项”。边界验证 ID/数据范围应保留，但不能再次进行业务去重。若 Client 契约允许重复同 ID，必须由上层明确责任或收窄合同，而不是偷偷替它排序。

## S04 — Instance `Use` 与 `Group` 把 Storage、DrawList、Binding、退休全混在一起（P0，MOVE）

**位置：** [`InstanceStorage.cpp` L12–87, L300–456](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/instance/InstanceStorage.cpp)。

`Use` 析构 `std::erase(storage.uses_, this)`，有 Group 时对 `group->uses` 再删除；`Group` 内含 5 个 DrawIndirect uint32 + 实例 Slot 列表和三份 GPU Bank。prepareGroup 需要遍历所有 `entries_` 构建成员列表；findGroup 线性搜 Group；release 在组内 `std::find`；一次 publish 需要通知所有 Use。**它不是 Legacy 的 GPU Cull→Compact→Count/MDC 算法。** 目标 InstanceStorage 只维护专业实例数据与 Dirty；MeshVisibility/DrawList/RasterBinding 处理实际绘制组织；GPU 数据改变的合法绑定更新在 Attachment/Frame 处进行。

## S05 — Mesh 从共享 Geometry Arena 退化为逐 Mesh Buffer（P0，RESTORE ALGORITHM）

**位置：** [`MeshStorage.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/mesh/MeshStorage.cpp) 对照 [`Legacy MeshResources.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/render_legacy/modules/function/render/vulkan/src/resources/mesh/MeshResources.cpp)。

旧 Mesh 共享 VBO/IBO 分段 Arena、Suballocation、Section/LOD/Stable GPU Record；新 Mesh Backing 为独立 Buffer，并围绕 Candidate/Retirement/References/Use 管理每对象生命周期。多数静态 Mesh 长期留存，新的分配方式改变 GPU 几何组织与 DrawBatch 形成条件。应继承已有 Arena/Section 真实算法，上传由新 stdexec 统一调度，不得简单将旧 MeshResources 整个综合类复制回 Runtime。

## S06 — Material 从连续 SSBO 退化为逐材质 416B Buffer（P0，RESTORE ALGORITHM）

**位置：** [`MaterialStorage.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/material/MaterialStorage.cpp) 对照 [`Legacy MaterialResources.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/render_legacy/modules/function/render/vulkan/src/resources/material/MaterialResources.cpp)。

每个 Backing 含固定 Header/16 vec4/8 Texture，以及自己的 Buffer/LastUse/Retirement/lease/texture binding/Use。旧版本使用成组 SlicedSSBO，按 MaterialFamily/Slot 组织；未来应由实际 Cooked MaterialProgram/ParameterLayout 驱动连续参数表、GPU 索引与脏更新，**不回退固定旧 LightingFamily**。逐材质小 Buffer 的 Host/Driver 成本需实测，不宣称静态已量出具体 FPS 下降。

## S07 — Texture Storage 有真实图像功能，但自身生命周期层次过多（P0，SIMPLIFY）

**位置：** [`TextureStorage.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/texture/TextureStorage.hpp)、[`TextureStorage.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/texture/TextureStorage.cpp)。

Backing 字段 `last_use_,leases_,live_,reserved_,retirement_,retired_`，外有 Physical、RetiredBacking、Candidate、Use、SlotMap<Record>；`RetiredBacking` 析构反向从 StableSlotMap 找 Backing 并手工 Destroy，已不像直观 RAII。保留独立 Image/Mip/Region/Bindless 的真实 GPU 语义，重新设计一个 Move-only Image+View+Sampler 的 RAII 组合，由统一现有退休机制保证安全；逻辑 TextureId 的 SlotMap 只指向当前物理内容。

## S08 — Mesh/Material/Texture 更新扫描全部 `uses_`，Release 自己订阅时 `std::erase`（P1，INDEX）

**位置：** [`MeshStorage.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/mesh/MeshStorage.cpp)、[`MaterialStorage.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/material/MaterialStorage.cpp)、[`InstanceStorage.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/instance/InstanceStorage.cpp)。

当前一个 Material 更新可能循环 Storage 里所有 Use 再按 ID 判断，多个 Storage 的 `Use` 析构 `std::erase(vector,this)`。这使成本与全 Storage 已绑定对象总数有关，而不与实际变更关系数 K 有关。若确实需要持续追踪绑定集合，应维护直接关系 ID 与位置实现 O(1) 拆除；优先进一步消除 Use→Context 的反向通知，让 Frame PreparedBindings 持有真正需要的引用。

## S09 — `StorageLease`、`Backing::references`、`TextureBacking::leases`、`Physical::versions/bytes` 多次计量寿命（P0，MERGE）

**位置：** [`StorageSet.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/resource/include/lux/engine/render/resource/StorageSet.hpp)、专业 Storage 的 `Physical/Backing` 实现。部分引用针对不同层，需要先画真实生命周期表；但它们合在一起明显要求人工同步太多计数。CPU Storage 实例的存活、某个物理 Backing 的被引用、GPU 内仍未完成的提交是三种不同事实，每种只保留一个权威。**不能一刀切删除所有引用计数然后破坏跨存储绑定的生命周期**；必须在准确归属后迁移。

## S10 — 每个 Storage 拷贝一套 `Physical/Backing/Candidate/Use` 模板（P0，ARCHITECTURAL REPLACEMENT）

**位置：** [`MeshStorage.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/mesh/MeshStorage.hpp)、[`MaterialStorage.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/material/MaterialStorage.hpp) 及对应 `.cpp`，Texture/Instance 又各有变体。

**不应**通过 `GenericGpuStorage<T>` 复制这套同义事务；那会锁死数据布局。共性应是已存在的 Upload/Queue/Retirement/RAII，专业 Storage 只保存其特有 GPU 数据结构/操作。必要的异步 Sender 由 stdexec 组合，Native 只实现真实 GPU Completion Source。类型 `Physical` 常仅用 `shared_ptr` 让 Buffer 在 Storage 死后还能读 Device 引用/计数，提示所有权原设计不对。

## S11 — `GpuWork` 并不是通用 GPU 工作，而是专用上传后发布包装（P0，MERGE/DELETE）

**位置：** [`GpuWork.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/transfer/GpuWork.hpp)、[`GpuWork.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/transfer/GpuWork.cpp)。

类型携候选 Owner、`unique_ptr<Transfer>`、通用 `ready_` 回调，再由 `GpuWorkSender<Response>` 适配到 stdexec。它是将“上传完成后才能发布专业结果”封装为一套通用生命周期事务，正好偏离“专业 Storage 保留，上传调度交 stdexec/Server”的初衷。应让 Storage 返回真实业务 Sender/完成结果，Native Transfer 提供复制提交/可用性事实，发布在拥有型 Storage 作用域；删除没有不可替代职责的额外 work 包装。不要把所有 GPU 工作混称为 `GpuWork`。

## S12 — `Transfer` 的阶段枚举与多个布尔/Callback 状态共同表达上传过程（P0，RESEARCH/REPLACE）

**位置：** [`Transfer.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/transfer/Transfer.hpp)、[`Transfer.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/transfer/Transfer.cpp)。

`EPhase PREPARE/CPU_PENDING/COPY/COPY_PENDING/ACQUIRE/FINAL_PENDING/COMPLETE`，又有 `started_,initialized_,query_pending_,failure_,cpu_preparation_,copy_callback_,graphics_callback_`；部分用于真实分块/跨 Queue，不可无脑删。理想是把 CPU 准备和必要 GPU 分块/Acquire 交已有 stdexec Sender + 提交 Queue，算法状态只留 index/row/offset 等真实进度，不再维护相同 Phase 的另一份布尔权威。要验证多 Mip/Region、跨 Queue 与 Copy partially submitted；不能只保留一个连续 Buffer Upload 演示路径冒充完成。

---
# 7. CMake/文件结构与公开 API 的深层依赖问题

## M01 — `render_resource` 中立资源合同公开依赖 stdexec / CoreExecution（P0，MOVE）

**位置：** [`resource/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/resource/CMakeLists.txt) 和 [`resource/AsyncWork.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/resource/include/lux/engine/render/resource/AsyncWork.hpp)。

资源合同层 `find_package(stdexec CONFIG REQUIRED)`，`target_link_libraries(render_resource PUBLIC STDEXEC::stdexec lux::engine::core::core_execution)`。实际上 `AsyncWork.hpp` 仅将 `execution::CpuResume<RenderError>` 和 `CompletionPost<RenderError>` 重命名到 `lux::render`，这是为了异步工作回传而增加的**执行机制反向依赖**。目标资源合同只声明 Config、类型、资源 ID、Storage 描述/绑定数据，不依赖 Server 和调度。异步连接放 Native Upload/Runtime；既有 CompletionPost 若仅服务 Native→RenderThread，放在对两侧中立、但不是资源值层的执行接缝中。**不得通过移动头使 Native 反向 include Runtime。**

## M02 — `render_commands` target 实际定义在 `runtime/CMakeLists.txt`（P1，MOVE）

**位置：** [`runtime/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/CMakeLists.txt)、[`render/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/CMakeLists.txt)。

Client-only 构建靠先进入 `runtime` 子目录而只创建 `render_commands` interface target。虽然能够工作，但源码文件归属已经错误：Command 值声明不该在 Runtime 的文件夹和 CMake 内注册。把 Command 本身移至 `commands/`，Client-only 不包含 Native/Server；Runtime 仅依赖 commands 与 IPC。保留当前安装目标名的短期 CMake 导出兼容可由真正 SDK 消费者验证，不保留空模块。

## M03 — `render/native/vulkan/extensions` 既包含专业 Storage，又依赖 `render_protocol`（P1，MOVE）

**位置：** [`native/vulkan/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/CMakeLists.txt)。

`render_native_vulkan_extensions` 同时编译 Texture/Mesh/Material/Instance 的 Storage 与 Commands.cpp，还依赖 `render_protocol`；这使“纯 Native 专业数据实现”被具体 Command 协议绑定。建议专业 Storage 构建保持 Native/Resource 合同，专业 Command Handler/Provider 在装配层连接；一个 Feature 新增时只需要在自己的专业目录加入文件并显式安装。不让 Native GraphCompiler 的单独消费者间接链接 Commands/IPC。

## M04 — `render_data` 变成 Wire Codec、Config、ShaderABI 的混合目标（P0，SPLIT/DELETE）

**位置：** [`data/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/data/CMakeLists.txt) 与 [`data/include`](https://github.com/LUX-YU/lux-engine/tree/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/data/include/lux/engine/render/data)。

同时提供 `ConfigValue/Payload/Encoding/Schema/Shader/ShaderBlock/ShaderInterface/Camera`。这些不是同一模块：Camera 应归专业 Engine/Feature；Shader 应以现有 rdesc 为唯一中立合同；Config/OwnedBytes 可在 core/commands/资源合同合理位置；Wire Schema 如无真实消费者删。**不新建五个空的小型静态库**，可把相近值类型合并到 core 或既有 rdesc；构建目标按实际独立消费条件划分。

## M05 — Render 包装器复制安装其他模块的 Header，可能隐藏真正依赖（P1，NEEDS_CLOSURE）

**位置：** [`core/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/core/CMakeLists.txt)、[`data/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/data/CMakeLists.txt)。

`render_core` 在自身安装路径追加安装 `modules/core/error/Error.hpp`、`modules/resource/description/Image.hpp/TextureInfo.hpp`；`render_data` 又安装 ShaderInfo/DataLayout 和其他资源 visibility header。这看起来是为安装包自洽进行的兼容补丁，但当真正的 Error/rdesc target 变化时容易产生来源不一致。若已有独立正式 SDK 包，优先通过**真实 target依赖与安装组件**取得原头，而不是 Render 再复制一份安装权威。必须检查当前包管理/安装 consumer，不能未经验证直接删头造成包不完整。

## M06 — Graph/Native Graph 独立可复用目标在头闭包上尚不完整（P0，RESTRUCTURE）

**位置：** [`graph/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/graph/CMakeLists.txt)、[`CompiledGraph.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/CompiledGraph.hpp)。

逻辑 Graph 目前依赖 render_data，该库包含 Codec/Shader 等无关内容；Native 编译头 include `DeviceSession.hpp`，同时 Native 库还包含 Upload/Readback/Storage Registry。这使“不加载 Runtime/专业Storage就直接编译执行一张图”的安装消费者需要更多头/链接对象。按 RGAM 的实际依赖图逐步收窄：中立 Graph 只含通用数据访问和算法，Native Compiler 只依赖设备/Shader/API 基础，专业 Storage 到 Attachment/Frame 的 Resolve 边界提供实际句柄。既有单静态库可暂时存在，只要头和构建依赖独立；是否拆库看真实外部消费者，不因“分层好看”无限拆 target。

## M07 — 顶层 `render/CMakeLists.txt` 现在有 core/data/protocol/resource/graph/features/native/runtime 许多组件（P1，CONSOLIDATE）

**位置：** [`render/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/CMakeLists.txt)。

当前 Component 数目不是问题本身；问题是 `data/protocol/commands` 这些名称代表相邻合同而依赖方向混乱。**建议稳定后核心为 Core、Commands、Resource、Graph、Features、Native、Runtime**，专业功能依照真实可独立安装消费者决定要不要拆；不为单个 POD 和仅 1 个源文件强制新建 CMake 包。

## M08 — `VulkanServer.hpp` 的安装模板公开了 DeviceSession/专业 Provider 构造协议（P1，MOVE）

**位置：** [`runtime/VulkanServer.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/include/lux/engine/render/runtime/VulkanServer.hpp)。

`#include <.../DeviceSession.hpp>` 以及 `registerFeature/registerStorage/storageCandidate` 多层模板使 Native 绑定和通用 Server API 耦合。需要安装第三方 Feature 时可以公开一个**专业扩展装配入口**，但客户端或纯 Server 控制代码不应被迫看到 Vulkan. Runtime 内部保持 Compiler/Handler 的职责分离。外部插件 ABI 是否需要公开某些模板必须在安装消费者实证下选择，不假装所有代码都可以私有化。

## M09 — 公共模板描述与专业 Native 实现散落多个文件，违反“局部可理解性”（P0，REORGANIZE）

**位置：** `resource/{Texture,Mesh,Material,Instance}.hpp`、`native/vulkan/{texture,mesh,material,instance}/*Storage.hpp`、`*Commands.cpp`、`features/MeshFeature.hpp`、`runtime/VulkanServer.hpp`。

新增一个专业 Storage 需要同时定位 Config、`static inline const StorageDescriptor description`、Ops/Access、Native Factory、CommandHandler、FeatureProvider、BindingOps，并且可能修改通用 Instance 枚举。**目标不是把 Vulkan 写到 Client 可见头**；而是把每一类专业声明、Config、静态 description、Typed Command 合同相邻放在该功能的中立专业目录，Vulkan 数据类和 Factory 在相邻 Native 专业目录。注册合同一次对接；不让 `resource` 公共目录规定一份适用于所有 Feature 的 `Instance`。

## M10 — `features` 根目录混入专用 `MeshFeature`，将示例变成框架默认（P1，MOVE）

**位置：** [`features/include/`](https://github.com/LUX-YU/lux-engine/tree/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/features/include/lux/engine/render/features)、[`features/src/MeshFeature.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/features/src/MeshFeature.cpp)。

当前根部 `MeshFeature.hpp` 与 Assembly、Feature、Registration 共目录。后续 Particle/Splat/Skinning/Hzb/Shadow 若都在根部添加 `Feature` 文件会无法扩展。改为 `features/core/` 放抽象，`features/mesh` 等专业目录放真实算法、Shader 和 Storage 声明；新增专业能力不需要修改通用目录的源集结构。

---

# 8. 代码密度、命名和测试机制污染

## T01 — `.clang-format` 既存在却没有实际执行（P0，STYLE）

**位置：** [`.clang-format`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/.clang-format)、[`Compile.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/Compile.cpp)、[`LayoutPlan.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/LayoutPlan.cpp)、[`Execution.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/Execution.cpp)。

已观察的代码密度：Compile.cpp 640 行约 11 个空行（~1.7%）；LayoutPlan.cpp 285 行约 4 个空行（~1.4%）；Execution.cpp 538 行约 14 个空行（~2.6%）。仓库设置了 `BreakBeforeBraces: Allman`、`InsertBraces: true`、`SeparateDefinitionBlocks: Always`、`ColumnLimit: 120`，实际大量 `if (...) return` 无 braces。**建议机械格式化与语义重构分两个 Git 提交**，否则真正架构 diff 难审阅；格式修正不是重构完成证明。

## T02 — 生产源码暴露大量测试专用 `*_SEAM`（P0，MOVE/DELETE）

**位置：** [`Deferred.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/pinclude/Deferred.hpp) 的 `LUX_RUNTIME_ASYNC_COST_SEAM / ASYNC_TEST_SEAM`、[`ResponseOutbox.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/pinclude/ResponseOutbox.hpp) 的 RESPONSE_COST、[`ServerLoop.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/src/ServerLoop.cpp) 的 ASYNC_COST/TEST、[`ServerEffect.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/src/ServerEffect.cpp) 的 EFFECT_TEST。

这些宏改变编译出的生产源分支，要求维护多种并行语义，是“测试/调试主导产品”的证据。实现与测量/诊断应明确分离，测试原型转到外部验证空间，旧宏迁出后完整 Render 产品只保留真实 API 分支。`VMA_IMPLEMENTATION`、平台可见性等构建宏不在此范畴。

## T03 — 直接 Vulkan 调用也被 `LUX_NATIVE/LUX_RECORD/LUX_DESTROY` 宏包围（P1，RESEARCH）

**位置：** [`native/vulkan/pinclude/Native.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/pinclude/Native.hpp)、[`native/vulkan/test/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/test/CMakeLists.txt)。

该宏的目的在 instrumented 测试目标中进行故障注入和调用计数，不一定生成 Release 额外成本；但阅读者看到的不是真正 Vulkan 函数，宏语义必须查其他头。迁出注入机制/仅外部验证工作区编译宏后的生产代码应优先直接调用 Vk/VMA 或极少数真正需统一的 Native 边界函数。不为替代每个宏再建设一个 `VulkanDispatchManager`。

## T04 — `Error` 使用 TypeId+args 字段被不同层解释，影响错误语义可读性（P2，RESEARCH）

**位置：** [`core/Error.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/core/include/lux/engine/render/core/Error.hpp)、[`native/vulkan/src/DeviceSession.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/DeviceSession.cpp)。

`RenderError{kNativeFailure, {encoded_vkresult}}` 便于无异常统一传递，但多个层必须记住 args[0] 就是 Vulkan Result。高频代码不是问题，**错误生产与消费处的语义分配不清**。只在 Native 边界判断 VkResult，明确映射为可恢复错误/设备终态，不在 Server/Storage 下层继续逐项解码 args；无需重新设计异常体系。

## T05 — `CommandFailure{outcome_unknown}` 不应成为任意 GPU 错误的默认兜底（P1，KEEP WITH CONTRACT）

**位置：** [`Protocol.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/protocol/include/lux/engine/render/protocol/Protocol.hpp)、[`GpuWork.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/transfer/GpuWork.hpp)。

已有 GPU 部分接受后无法确认撤销的真实问题，所以应保留 unknown 语义。但如果资源没有对外发布，上传中部分 Copy 不必自动等同“客户端无法确认公开业务副作用”，前提是后台拥有的未发布对象能够安全延迟回收。**这需要修改业务完成合同并真实测 GPU**。不要为“保守”将每类业务错误套成未知，导致 Client 不能判断资源句柄是否可用。

## T06 — `RenderServer` 的默认验证指标可能长驻生产接口（P2，MOVE）

**位置：** [`RenderServer.hpp::RenderServerStats`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/include/lux/engine/render/runtime/RenderServer.hpp)、[`ServerImpl.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/pinclude/ServerImpl.hpp)。

`scoped_buffers_created` 等指标服务阶段性验证或诊断，而不是 C/S 必须的业务值。Stats 可以保留真实观测数据，但 instrumentation 应可选 sidecar，并在 Release 非诊断构建中不引入 hot-path 额外原子计数。不要把每次阶段性验收用的计数都永久加到 RenderServerStats。

## T07 — 源码语义行列密度之外，更严重的是大量文件职责不相称（P1，REORGANIZE）

**位置举例：** `Compile.cpp`、`Shader.cpp`、`ServerRegistration.cpp`（561 行）、`ServerScene.cpp`（466 行）、`Transfer.cpp`（574 行）、`TextureStorage.cpp`（550 行）。

不能简单设定“200 行封顶”而机械拆分成很多类型。**按函数职责边界拆文件**：例如 Vulkan 编译输入、Pipeline 构造、Layout/Record Lowering；注册值准备、Directory 发布；Scene/View 的关系维护；Transfer CPU Copy 与 GPU 提交。若一个较长文件描述的是同一紧密算法（如原 Legacy Instance Stream），可以保留，只需组织段落和工具函数。

## T08 — 以“测试数量”替代了架构正确性的判断（P0，PROCESS）

**来源：** 当前测试目录、[`native/vulkan/test/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/test/CMakeLists.txt)、[`runtime/test/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/test/CMakeLists.txt)。

94 个测试 `.cpp`，多为单个框架实现闭环或微观实验，当前生产 `46 cpp`。某些实现以局部单件上传/读回/小规模 Draw 得到 PASS，但并不能证明 Legacy 的 Geometry Arena、Dirty SoA、Cull/Compact/IndirectCount 算法实际迁移。强制改变测试目标：**仓库内仅保留真实、端到端渲染结果测试**；机制原型、故障注入、性能分析、头文件/安装编译转到仓库外可复现验收空间；无用或被新设计替代的机制测试删除，不再污染产品类型。

## T09 — 测试目标重新编译生产源码并换宏语义（P0，MOVE）

**位置：** [`runtime/test/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/test/CMakeLists.txt) 的 `${LUX_RUNTIME_SOURCES}` 与 `LUX_RUNTIME_*_SEAM`，以及 [`native/vulkan/test/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/test/CMakeLists.txt) 的 `render_native_instrumented`。

维护者必须考虑生产源码存在两种甚至三种宏编译形态，这会污染设计和审查。用户已明确要求此类实验迁至仓库外；先保存必要原始结果及失败证据，再删这些专用目标、宏与辅助代码。GPU 正确性通过正式渲染场景的 Validation/SyncValidation 与 ASan 验证。

## T10 — 真实结果测试应当更深，而不是更多（P0，REPLACE TEST COVERAGE）

仓内应该优先维护最少但足够有说服力的业务集成场景：纹理/材质可见结果、Mesh 多实例、Scene/View/Target、Camera 专业输入、GPU Cull/Compact/Indirect、帧槽、Readback、真实资源流送、RenderGraph 部分/错误输出。**测试不以是否能访问内部 `CommandRegistry`、Queue callback 或 Test Seam 为准，而应从正式 Client/SDK 接口获得真实像素和 GPU 结果。** 测试所需的外部资产、Host 安装样例和 Shader 编译仍应可复现。

---

# 9. 容器与复杂度逐点裁决

> 复杂度只对明确操作和规模有意义；同样 O(n)，遍历 3 个 Queue 角色与遍历 100000 个 Instance 的风险不同。不能“为了绝对 O(1)”引入无限哈希内存或另一张大表。所有表中的成本是源码可推导的算法复杂度，**不是运行时测得的 FPS**。

| 位置/动作 | 当前模型 | 数据规模 | 建议终态 | 备注 |
|---|---|---|---|---|
| `DeviceSession::queue(role)` | 角色索引 O(1) | 3 | 保留 O(1) | 问题在四张并行拥有表 |
| `DeviceSession::queueFor(ticket)` | 角色数组最多扫3项 | 3 | 可以保留 O(3) | 只需引用物理队列、别另建哈希 |
| `DeviceSession::~DeviceSession` Alias Image 清理 | 反复 `find_if(images_)` | 全部 Image | RAII 成员顺序/直接依赖 | 大量 Alias 时可能二次复杂度 |
| `DeviceSession::release(context)` | `find_if(contexts_)` O(N) | Context 总数 | Attachment/SlotMap 直接拥有 | 不应由 Device 拥有 |
| `StorageSet::find(type,key)` | `vector<Record>` 线性 O(S) | Set 内 Storage 数 | 名称索引/一次查后持 ID | 不在热态反复找 |
| `StorageSet::clear()` | 重复从头找 `leases==0` | Set 内 S | 确定拥有生命周期 + 一次清空 | 最坏 O(S²) |
| `ResourceRegistry::typeId(name)` | 遍历 Type 表 O(T) | 安装类型 T | 冷索引，得到TypeId后O(1) | 安装期 T 小可有意识保留 |
| `ResourceRegistry::installFactories(N)` | 候选双重比较 O(N²) | 一次安装 N | 冷态排序或直接索引 | N 明确小可接受 |
| `CommandRegistry::findCommand(descriptor)` | 全目录 O(C) | Commands C | 名称索引/编译ID | 实际 dispatch 已 O(1)，不要退化 |
| `CommandRegistry::findHandler(type,key)` | 全 Handler O(H) | Handler H | 安装/发现索引 | 不增热路径成本 |
| `registerFeature` 重复 Candidate Handler 扫描 | 前述候选与旧目录嵌套 | 注册组 K | 一个准备/发布事务 | 关键是职责收敛 |
| `updateCamera` | 遍历所有 Effect × Group | 全 Server | 删除内建 Camera；精确绑定 ID | P0 架构污染 |
| `removeScene` / `removeView` | 多个 Effect 全扫描 | 全 Server | 只遍历 Scene/View 附着列表 O(K) | K 为真实关系数 |
| `resizeTarget` | 遍历全部 Effect/Group | 全 Server | Target Attachments O(K) | O(K) 有必要 |
| `advanceRendering` | 每 tick 访问所有 Effect/Group | 所有活 Group | 真正 Active Attachment 列表 | 与实际渲染数成正比 |
| `compileEffect` Pass → Program | 每个 Pass 用字符串找 Program | P×M | GraphPassId→programIndex O(1) | 冷态但明显重复 |
| `compileEffect` Argument → Resource | N 次 `find(resources,key)` | N×R | 单次 Name→ResourceId 冷索引 | 不要在 Native 再查 |
| `LayoutPlan` ShaderField → Resource | 多次字符串比较/子集合扫描 | Pass×Field | 预解析 Parameter/Resource ID | 冷编译优化后减少代码 |
| `CompiledGraph::compile` Program pass 查找 | `find(graph.passes(), key)` | P×M | 输入编译前已配 ID | 保留一次接口验证 |
| `ExecutionContext::setParameters(GraphPassId)` | `find_if(passes_)` | Live Pass 数 | source→slot数组 O(1) | 高频 Pass 参数重要 |
| `ExecutionContext::rebind` | 扫 leases/pass/descriptor | 绑定总量 | 按实际变更资源索引批量更新 | Descriptor 合法使用仍保留 |
| `MaterialStorage::Use` destructor | `std::erase(uses_, this)` | 全 Storage uses | 直接移除或不需要Use | O(U)，多次析构易二次 |
| `MaterialStorage::publish` | 扫所有 Uses 过滤某 Material | 全 Storage uses | MaterialId→实际 bindings K | O(K) |
| `InstanceStorage::findGroup` | `find_if(groups_)` | Group 总量 | 直接 GroupId / 专业索引 | 不在 Storage 混 DrawList |
| `InstanceStorage::prepareGroup` | 扫所有 Instance 找 Group 成员 | Instance 总量 | 增删时维护会员关系 | 初建可以 O(N)，热态不能重做 |
| `InstanceStorage::release` | 扫 Use + `std::find` 成员 | Use/Group 成员 | O(1) 位置移除 | 或 DrawList GPU 生成 |
| `InstanceStorage::update(K)` | `sort + unique` O(KlogK)，另复制 Bank Dirty | K | 已去重 K 项直接应用 O(K) | Engine 负责去重 |
| Dirty Page Stream | 只遍历被标记页面/Upload Chunk | Dirty 页 K | Legacy 策略保留 | 高水位内存需测量 |
| `RetirementQueue::collectReady` | 已提交 Queue frontier/list | 完成批次 K | 仅处理真正 ready 的对象 | 不强迫单物理队列替代跨Queue条件 |
| `GraphAnalysis::topological` | PQ O((V+E)log V) | 冷编译 | 先 KEEP，按编译性能证明优化 | 输出可重复顺序可能有价值 |

---
# 10. 建议的最终职责与类型归属（以减法为主）

此表不是要求“每行新建一个 class”。相同职责可由已有类的一个方法或一个小的自由函数完成。**核心是现有成员必须归位，旧同义成员全部迁走后删除。**

| 实际职责 | 单一主要归属 | 应保留的成员/方法 | 当前应迁出的内容 |
|---|---|---|---|
| VkDevice 能力及创建销毁 | `VulkanDevice` | VkDevice, Properties, enabled capability, create/native | Storage Registry/Upload/Context 目录 |
| 实际 GPU Buffer/Image 资源 | 现有 `VulkanAllocator` + Move-only RAII Buffer/Image | Handle+VMA allocation、mapped span、合法 release | `DeviceSession::buffers_/images_` 中央强拥有表、遗失不释放的 Permit |
| 物理 Queue 提交和 GPU 进度 | `SubmissionQueue` | VkQueue, CommandPool/Slots, submitted/completed, FIFO completion | Texture/Material 业务逻辑与名称 |
| 物理 Queue 角色映射 | `DeviceSession` 轻量物理 Queue 组合 | 唯一队列集合 + `queue_by_role_[3]` | 4 份 `*_owners_`/借用数组 |
| 安全 GPU 退休 | 已有 RetirementQueue | 队列进度/真实 wait frontier、Move-only 延迟对象 | 专业 Storage 的第二套 retirement 状态与手工 destroy switch |
| 上传到已存在 Native 资源 | `UploadContext` 或已有收敛 Transfer | staging、Copy、Queue 依赖和真实 GPU Progress | GpuWork 的专业 Candidate/Reply/TextureId |
| CPU 工作执行与返回 RenderThread | 已有 stdexec + 唯一 RenderLoop 接缝 | Scheduler、CountingScope、一个 ready queue | 各 Effect/Storage 独有的 OperationState Owner/辅助 Sender 链表 |
| C++ Command 的类型/Handler 目录 | CommandRegistry | Descriptor、实际 Handler Binding、Selected | `cold+active` 两套权威、复杂隐式 Handler 构造 |
| 一次 Feature/Storage 的冷态安装 | Server 安装函数中的拥有型临时事务 | prepare/validate/publish 三阶段 | `storageCandidate` 同时干 4 件事，长期 Registry 状态 |
| Type/Storage 实例身份 | ResourceRegistry/StorageSet | Factory/SlotMap/专业配置、真实 Storage Owner | 对象内重复 type/token/config/lease/preload 状态 |
| Scene/View/Target/Effect 业务关系 | RenderServer 现有表 + Attachment 关系表 | Generation ID + 受影响的 ID 列表 | 全 Effect/Group 扫描、Camera 字段 |
| Graph 编译、参数映射 | RenderGraph/Analysis/Native Compiler | 一份 Resource/Pass ID 编译索引 | 字符串重复查、二套 ShaderInterface、Camera 128B 固定 ABI |
| 一个已编译 GPU 计划 | Effect 的单一 RenderPlan 拥有者 | Pipeline/Descriptor/Record/Barrier/Resource plan | CompiledGraph+RenderPlan 双份拥有、混合 `CompiledPass` 字段 |
| 具体 View/Target 的运行实例 | EffectAttachment + RenderGraphInstance | PreparedResources/FrameBindings/FrameResources | `DeviceSession::contexts_`、Context 自己管理 Readback/特殊 Storage 类别 |
| 真实实例属性与 GPU SoA | 专业 MeshInstanceStorage | SlotMap、Transform/Property/Cull Streams、Dirty pages | Use/Group/DrawIndirect CPU members/三份完整 Bank |
| 实际可见性与绘制分组 | Mesh Visibility/Raster Feature | DrawList/MDC/Cull/IndirectCounts | InstanceStorage 直接构造固定 DrawGroup |
| 几何上传与空间管理 | MeshStorage 的 GeometryArena | VBO/IBO Segments、Mesh GPU Records、LOD/Section | 每个 Mesh 申请独立大 Buffer 的默认策略 |
| 材质参数与 GPU 索引 | MaterialStorage 的参数流 | Cooked Program、Parameter Layout、Texture index、Dirty | 每个材质独立 416B Backing/Buffer 默认策略 |
| 专业 Image/Mips/Bindless | TextureStorage | Image/Mip/Residency、纹理句柄、Descriptor | Candidate/Use/RetiredBacking 重复责任 |
| Camera/ECS/动画业务转换 | Engine Scene integration/专业 Feature | CameraExtraction、FrameInput 生成 | Core Camera、UpdateCamera 通用业务 |
| 图执行后的 Readback | 明确的 Native Readback 操作 | Copy、GPU 完成、CPU Buffer 读取 | `Readback::arm()` 隐式触发 render、Context 中的长期链表 |

### 10.1 什么情况下允许一个较大的类？

一个类保存多项字段不是错误。Legacy 的 InstanceStream 需要同时保存 CPU Page、GPU Buffer、DirtyFlags、Page Table，这是**同一数据结构的内在组成**。如果所有方法都操作它们以实现稳定 Slot+分页 SoA 的单一存储职责，较长的实现也合理。

另一方面，`DeviceSession` 的 Handler 注册、Upload、Context、物理 Allocation 与 Descriptor 缓存互不构成一个数据算法；其大体积不是相同性质。**不要拿“一个类不要超过 N 行”作为拆分准则。**

### 10.2 `shared_ptr` 的精确裁决

**源码抽样事实：** 旧版 Instance/Mesh/Material/Texture 八个专业头/源文件共 5016 行，`std::shared_ptr<` 出现 0 次；当前 `Instance/Mesh/Material` 直接使用 `shared_ptr<Physical>`，`ConfigValue`、`RenderPayload`、`CompiledGraph`、`DescriptorArray` 也有 shared/weak 设计。

- **应优先去掉：** 仅为了 Backing 在 Storage 析构后访问 Session/计数而建的 `shared_ptr<Physical>`；它通常是所有权设计错误的补偿。
- **需要判明后才能动：** `CompiledGraph` 多 Attach 共享同一不可变计划（有真实共享关系）、RenderPayload 跨 Client/Worker 保留上传 Source（有真实跨线程数据寿命）、ConfigValue 的冷态不可变配置（可用直接移动值或只分享大数据）。
- **默认不要新增：** “每个 GPU 已提交批次都持有 std::shared_ptr<N个资源>”通用网络；它可能再次把简单 Slot/Arena+退休改为另一套引用计数架构。优先沿既有 Queue/Frame/Retirement 唯一拥有转移，并在真实 GPU 用例上验证安全。

**结论：Legacy 少用 shared_ptr 是明确风格线索，但不是全项目禁用 shared_ptr 的形式化规则。**

---

# 11. 目录与文件的建议重整——不为了好看新建 20 个库

## 11.1 目标目录骨架

```text
modules/function/render/
  core/
    include/lux/engine/render/core/{Ids,Error,ConfigValue,Payload,...}.hpp
    src/                         # 仅实际需要非平凡实现的值类型

  commands/
    include/lux/engine/render/commands/
      Command.hpp                # Request/Reply/ID/Descriptor/Concept
      Scene.hpp                  # Scene/View/Target 等纯请求类型
      Effect.hpp                 # Effect/Attachment 管理请求类型
    # 专业 Storage Command 放相应专业功能目录的中立声明处

  resource/
    include/lux/engine/render/resource/{Storage,Factory,Registry,StorageSet}.hpp
    src/{Registry,StorageSet}.cpp

  graph/
    include/lux/engine/render/graph/{RenderGraph,Builder,PassInterface,Analysis}.hpp
    src/{Builder,Analysis}.cpp

  features/
    include/lux/engine/render/features/{Feature,EffectDefinition,Composition}.hpp
    mesh/                        # MeshInstance/Drawable/DrawList专业声明与Shader资产
    deformation/                 # Skinning / Pose 专业声明与算法
    lighting/                    # 专业Light/Shadow Feature
    postprocess/                 # 专业时域/色彩效果

  native/vulkan/
    include/lux/engine/render/native/vulkan/  # 只需要真实外部消费者的Native SDK
    pinclude/
      compiler/                  # Shader验证/Layout/Barriers/Recording lowering
      execution/                 # FrameResources/Binding/Executor辅助
    src/
      device/                    # VkDevice、PhysicalQueue映射/Session基础
      memory/                    # VMA、Move-only Buffer/Image
      submission/                # Submit、progress、唯一退休队列
      transfer/                  # Staging、Transfer、Readback（无业务Candidate）
      compiler/                  # Native GraphCompiler
      execution/                 # RenderGraphInstance/Executor
      resources/{mesh,material,texture,instance}/ # 按具体专业Storage
      features/                  # 对应专业Feature Native Shader/Pass

  runtime/
    include/lux/engine/render/runtime/{RenderServer,ExtensionRegistration}.hpp
    pinclude/{CommandRegistry,ServerImpl,ResponseOutbox,...}.hpp
    src/{CommandDispatch,Scene,Effect,Attach,Registration,ServerLoop}.cpp
```

目标只是**概念边界**，不是一个文件只允许放一个 class。按原始实际生产类型增删文件，不创建没有调用者的额外 `Manager`、`Service`、`Facade`。如果 `core` 的某个值可以直接沿用现有 lux-cxx/rdesc，**优先不新建文件**。

## 11.2 旧文件归属迁移表

| 当前文件 | 目标动作 |
|---|---|
| `core/Camera.hpp/cpp` | DELETE；Camera 模型与计算归 Engine/专业 Feature，纯 viewport 留专业图形接口 |
| `data/{Encoding,Schema}.hpp/cpp` | 全仓/SDK 消费者检查后 DELETE 或 MOVE 至真实资产序列化域 |
| `data/{ConfigValue,Payload}.hpp` | MOVE 到中立基础值合同；复用已有 C++ Typed Value/Bytes 优先 |
| `data/{Shader,ShaderBlock,ShaderInterface}.hpp` | 与 `rdesc` 共享合同逐字段 MERGE，避免双权威 |
| `protocol/{Protocol,CommandMethod}.hpp/cpp` | MOVE 到 `commands`，确认外部后删除独立 protocol target |
| `runtime/Commands.hpp` | 按实际 Command 使用域重组；不机械一请求一头 |
| `runtime/VulkanServer.hpp` | 收窄到明确 Vulkan provider 装配，移出 storageCandidate 事务 |
| `runtime/pinclude/CommandRegistry.hpp` | CommandRecord 正确命名，DELETE `cold/active` 同义权威 |
| `runtime/pinclude/Deferred.hpp` | 删除测试宏；审查 completion 与 RenderLoop Ready 的混合职责 |
| `runtime/src/ServerScene.cpp` | 按关系索引/Attach 拆分 Scene/View/Target 行为 |
| `runtime/src/ServerRegistration.cpp` | 明确准备/发布函数，不多处重复查 Handler/Factory |
| `runtime/src/ServerEffect.cpp` | Sender 保留；编译索引替代字符串搜索；RenderScheduler 移到真实 RunLoop 接缝 |
| `native/vulkan/src/DeviceSession.cpp` | Session 收口设备组合，删除中央 Buffer/Image 手工析构 |
| `native/vulkan/memory/Memory.hpp/cpp` | 引入正确 Move-only RAII Native 对象，删除伪所有权 Permit |
| `native/vulkan/transfer/GpuWork.*` | 候选 DELETE；真实 Transfer+Storage+stdexec 合并其职责 |
| `native/vulkan/transfer/Transfer.*` | 重塑 CPU/GPU Chunk 状态机，保留跨Queue的 Vulkan 事实 |
| `native/vulkan/src/Execution.cpp` | 按 Instance/Executor/Resource Preparation 职责分块迁出 |
| `native/vulkan/src/{Compile,LayoutPlan,Shader}.cpp` | 按 Compiler Pass/Shader ABI 等功能移动，逐步替换混合 Work/Recipe |
| `native/vulkan/src/Readback.cpp` | 从 RenderGraph 执行责任分离；保留 Readback 真实完成合同 |
| `native/vulkan/resources/instance/*` | 移回专业 MeshInstance + GPU Streams；Cull/DrawGroup 进入 Feature |
| `native/vulkan/resources/mesh/*` | Legacy Arena/Suballocation/Section/LOD 与现代 RAII 结合 |
| `native/vulkan/resources/material/*` | 连续参数流与 Cooked ShaderProgram；不采用逐材质小 Buffer 默认布局 |
| `native/vulkan/resources/texture/*` | 保留 VkImage/Mip/Region；整理版本、Upload、Bindless 与寿命 |

## 11.3 哪些内容不因“想清理”而动？

- 现有 `RenderGraphDefinition + GraphAnalysis` 的成功算法和已验证的 RAW/WAR/WAW；未来唯一中立图演化，不增加 GraphIR/GraphDefinition 双体系。
- 真实 Vulkan Submission Serial、Fence/Timeline、QueueFamily Acquire、Image Layout、Aliasing Barrier；不把 GPU 完成替换成“RenderThread 已处理完请求”。
- IPC RequesterId 与 RequestId 的请求对应关系、所有请求必 Reply、WOULD_BLOCK/断连规则；不为了清理 `protocol` 再引入 bytes 序列化。
- GPU Partial Submit 后的物理资源存活、Unknown outcome、Retirement、Source Payload 保活；不能将 “stdexec Scope 完成”当作 GPU Fence。
- frozen Legacy 原始 719 文件及用户工作区修改；必要算法抽取迁到新实现，源头只读。

---

# 12. 测试文件与验证边界：执行用户明确的新政策

## 12.1 内部产品测试与外部机制试验分开

目前 `modules/function/render/**/test/` 下 189 文件、94 cpp、约 1.08 MB；部分测试重编译生产源码、打开 `LUX_RUNTIME_*_SEAM` 或 `LUX_VULKAN_TEST_SEAM`，有一整套独立的微基准与强制故障路径。**以后 Render 模块内的测试目录只保留最终渲染效果、可观察场景输出的验收测试。**

### 仓内保留/重写的类别

| 类别 | 具体结果 | 准入条件 |
|---|---|---|
| 基本资源 | 纹理、Mesh、Material 创建成功，实际 GPU Render 输出正确 | 正式 Client/专业 Feature 完整业务链 |
| 多实例 | K 个 MeshInstance 不同 Transform/Material，真实像素和可见集合正确 | 同场景真实 GPU 与连续帧验证 |
| 多 View/Target | 正确场景输入、多输出与图执行顺序 | 不直接窥探私人 Queue Ticket 来宣称完成 |
| 图形算法 | Cull/Compact/Count、LOD、阴影、深度、材质、时域 History | 有可比输出和不变量；不是演示三角形 |
| 动态资源 | 上传成功后首次合法使用、替换、回收、资源跨帧存活 | Validation/SyncValidation 0 error，ASan 检查 |
| 外部交付 | 安装后的真实 Client/SDK 场景 | 可作为外部消费者/CI 验收，不塞每头单独 test |

### 仓外试验目录（不在项目 Render `test/`）

- CPU stdexec Sender/Scope/Tail Stack 原型；
- 内部 CommandRegistry/StorageFactory/SlotMap/IPC Payload 结构实验；
- GPU Queue Callback、RetirementReservation、Device Lost 时序/故障注入；
- Allocator/ASan 内存统计、TSC/ETW/VTune、`AsyncCost`/`MemoryCost`；
- Dummy Renderer、单函数错误注入、Shader ABI 类的短实验；
- 各构建模式独立头、安装前缀 relocation 和 Toolchain 试验。

**迁移前保存精确 SHA 和日志**；无法再次从真实场景覆盖的安全风险，使用外部短期实验补证后才可以改生产。删除这些仓内框架测试是目录/产品政策变更，不意味着允许任何测试缺席时就声称 CUDA/Vulkan/ASan PASS。这里不新增永久测试支持类到生产 API。

## 12.2 不是删得越多越好——须保持最终结果的质量

`basic_mesh`、`Offscreen`、`TextureRendering`、`InstanceDraw` 含真实像素/场景验证，但部分仍通过 Test Seam、Native 私有接口构造测试场景；它们可**整合改写**成少量正式 Client/已安装公共 API 的真正效果测试。其余 `AsyncCost/LastUse/MemoryCost/StorageRegistration/Handlers` 等机制测试**迁出**，没有继续价值的删除。

用户要求的是“仓库内最终结果测试”，不是“以后禁止开发者在仓库外做任何单元实验”，也不是“为了所有真实 GPU 安全只靠逻辑想象”。

## 12.3 现有 CMake 风险

- [`runtime/test/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/test/CMakeLists.txt) 约399行，重编 `${LUX_RUNTIME_SOURCES}`，打开 ASYNC_COST/EFFECT_TEST 等。
- [`native/vulkan/test/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/test/CMakeLists.txt) 约517行，建立 `render_native_instrumented`、多个独立故障注入可执行文件与源编译变体。
- 这些东西与真实 Shader/资产构建或最终产品集成混在一起，需要以“是否为最终渲染成果”重新分类；`BUILD_TESTING` 打开不应导致完整生产 Native 源再编译一套人工宏变种作为长期公共依赖。

---

# 13. 明确的删除/保留/验证优先级

## 13.1 第一批明确目标（P0，结构先于小优化）

| 子闭包 | 必须取得的净收益 | 绝不能丢失的合同 |
|---|---|---|
| DeviceSession RAII | 删除4张Queue/Retire映射，清除Buffer/Image中央手工拥有 | 物理Queue只有一套Serial；独立Session安全析构 |
| Command/Registry | `cold/active` 同义权威收敛；Storage安装职责独立 | 类型化Command、Handler动态选择、每Request回复 |
| Camera | 移除通用Core/Runtime/Compiler固定Camera | Engine/专业View Camera仍可正确输出矩阵/视锥与实际像素 |
| Runtime关系 | Attachment ID直达，删除全局Effect/Group ID查找 | 多Scene/View/Target与实际生命周期 |
| Shader/Graph | 合并ShaderInterface权威，拆掉混合PassProgram/CompiledPass | Graph分析、Pass访问与Native真实GPU调用不回退 |
| 专业数据 | Legacy实例SoA/MeshArena/Material表/TextureBindless算法迁移 | 插件化Storage、stdexec Server调度、GPU正确性 |
| 测试 | 移出机制测试与`*_SEAM`，以真实渲染效果为准 | 旧原始证据与新GPU Validation均可追溯 |

## 13.2 需要机制研究，不能直接一刀切 DELETE

| 对象 | 原因 | 原型要求 |
|---|---|---|
| `SubmissionTicket` | 真实GPU queue serial | 在Native内部缩范围；多物理Queue正确 |
| `SubmissionRequirements` | 多Queue最后使用事实 | 资源退休不能只看某一个Queue |
| `RetirementReservation` | 避免已接受GPU工作后晚期回收容量不足 | 必须证明提交后退回安全路径不会失败 |
| `SubmissionCallback` | CPU/GPU异步完成通知必须有安全引用 | 同步完成、自销毁/尾栈/取消测试 |
| `ConfigValue::shared_ptr` | 真实跨异步冷态配置共享可能成立 | 每件配置实际生命周期和分配成本 |
| `RenderEffect::shared_ptr<CompiledGraph>` | 多Attachment共享不可变计划可能成立 | 计划替换与GPU在途安全，非盲目禁用 |
| `DescriptorArray::weak_ptr` | Descriptor Set可能被旧Plan/Texture引用 | 父Device寿命与最后GPU使用证据 |
| `ResponseOutbox`的自制FIFO | 当前有实际O(1)复用收益 | 需要测量替代容器，不因名称奇怪删除 |
| GPU Timeline替代Fence | Vulkan同/跨Queue、WSI约束 | 实际设备及完整GPU/CPU完成测试 |
| `ResourceRegistry`名称索引 | 类型数量/安装规模可能很小 | 输出实际T、N上限，不凭空增加HashMap |

## 13.3 必须严格保留的实际能力

- 单 RenderThread 满速事件循环，不人为 sleep 降速；VSync可配置。
- 单进程内1 Server 多 Client 双 SPSC；Type Command/Reply，消息拥有型零拷贝路径；正常请求强制回复。
- stdexec 既有 `CpuScheduler + counting_scope + spawn` 的通用异步生命周期；不恢复 PendingOperation/OwnerScheduler/自定义线程池。
- 已通过的 GPU Validation/SyncValidation 0 error 作为后续实现的安全要求，独立提交不自动复用未跑资格。
- Legacy 文件及其经过验证的 GPU 数据布局、算法源头不修改；只将算法迁往新框架，不重新封装一份 Legacy Runtime。
- Geometry Arena、分页 Instance、GPU Cull/Compact/MDC、Material参数流、TextureBindless/Mip 等真实算法能力必须在最终迁移中得到相应验证。

---

# 14. 实施顺序：以真实职责闭包推进，不再无限增加阶段

### P0 — 基线、职责清单与测试/产物清点（只做一次）

- 固定当前远端 HEAD、用户工作树/Legacy blob 和外部性能/失败证据；确认 `71fbe803` 之后是否已有新的 Execution 收尾提交。
- 给全体主要类型写**一行职责、成员拥有清单、真实消费者、性能等级、预计删除清单**。以前以 `Owner/Cold/Backing/Use` 命名的类型必须重审，而不是默认延用。
- 对 94 个测试 cpp 分类：终态渲染保留/合并、机制实验迁往仓外、过期删除；记录迁移路径与证据。
- 建立依赖图与类型 ID 映射，发现未验证外部消费者时标 `NEEDS_CLOSURE`，不得猜是死代码。

### P1 — 格式化与不改变行为的命名整理

- `.clang-format` 全 Render 生产源适用；**单独提交**。
- 消除 `cold`/`owner`/`globals` 等明显误导字段；改名时不把 1 套错误状态换成另 1 套错误状态。
- 在这一步不改变 GPU 生命周期、不改 Buffer 布局；确保 Legacy 和用户更改不被触碰。

### P2 — Native 基础的 RAII/队列所有权收敛（最关键的基础）

- PhysicalQueue 只保留一份强拥有与一份 role index，物理别名真实身份保持。
- Buffer/Image Move-only RAII，彻底去掉“Permit析构不释放、Session最后扫表兜底”的状态网络。
- 现有 Queue/Retirement 的 GPU最后使用与最终关闭合同证明；保留已接受Copy、跨Queue、Alias和DeviceLost的安全性。
- 研究只增加小范围实际必要的 Backend helper，不得再建第二 ResourceOwner/MemoryManager。
- 适配 Native 直接消费者；未通过多Queue/Descriptor/LastUse验证，不进入 Storage 全量推广。

### P3 — ResourceRegistry/CommandRegistry 与 Runtime 业务边界收敛

- 合并 TypeToken大小对齐表示、CommandEntry冷热双权威；ID直接查并调整必要冷索引。
- StorageFactory、Handler、Feature安装恢复显式一次事务，不再通过 Handler构造形状猜依赖。
- 移除 Core Camera，专业 FrameInput 由 Engine/Feature 接入；Effect/Target/View的Attachment关系统一拥有并直接索引。
- 合并真正有用的 `protocol` 到 Commands，处理 `data/Encoding` 真实消费者后删除或归位。

### P4 — 将 Legacy Storage 的数据/算法迁到正确的专业领域

优先 Instance 小闭环：稳定 Slot/分页 SoA/Dirty Upload/多 Frame 读取；CPU Cull Group 只作为旧能力退出阶段对照，不能当新最终 GPU-Driven。然后 Mesh Geometry Arena/LOD/Section、Material连续参数、TextureBindless/Region/Mip。**每一步保留新 C/S 与已有 stdexec 上传编排，只改变职责错位的数据/物理寿命实现。**

### P5 — Graph/Feature/Native Compiler 重新贴合 RGAM

完成唯一 ShaderInterface、Pass WorkBody、Native Recording Program、FrameResources；按实际 GPU功能增量迁移，不再保留 `ERecipe/PassProgram` 混合结构与第二个字符串关系发现系统。GraphAnalysis 现有算法不能因目录重构丢失。

### P6 — 撤出框架测试、清理公开API与独立验收

将机制实验移到外部工作区，删除全部生产 `*_SEAM` 与无实际消费者的兼容代码；仓内最终只保留真实渲染成果测试，测试代码不反向定义产品对象。另做独立 RelWithDebInfo/Debug/ASan/禁异常RTTI、真实 Validation/SyncValidation、安装SDK消费者、热态 0 Heap 和实际资源流送/帧时间对照。

此阶段可以合并前几阶段的审阅，但**不能一个实现提交同时大面积重写 Native所有权、ShaderABI、Legacy数据算法和测试后直接自称PASS**。对独立失败的部分保留旧 I/V 资格和诊断证据，不用回退已证明正确的 C/S 框架。

---

# 15. 必须提交的结构化审计与验收成果

下一次正式实施前，实施方应交付：

1. **`TYPE_RESPONSIBILITY_MANIFEST.md`**：现有类型→最终类型的职责、每个实际成员与方法的归属、拥有关系、依赖和删除名单；无需为每一个 struct 建 1 个文件。
2. **`LEGACY_DATA_ALGORITHM_MAP.md`**：Instance/Mesh/Material/Texture 的真实 GPU ABI、内存分配、Dirty、RenderGraph消费/Shader接口、逐帧变化、LastUse，以旧文件和行范围作映射；标注哪些直接迁移、哪些被合理改良、哪些是当前未实现能力。
3. **`RENDER_MODULE_DEPENDENCY_REPORT.md`**：CMake target 图、实际公共头 include 闭包、独立 Graph/Client/SDK consumer、需移动/删除文件与公开接口；证明目录重整没有破坏安装。
4. **`RENDER_TEST_RELOCATION.md`**：189 个测试文件的明确处理结果（保留真实输出 / 迁出机制 / 过时删除），旧证据保存位置及提交 SHA；不能仅给统计数字。
5. **`RENDER_CLEANUP_RESULT.md`**：实际 I/V 提交、生产新增/删除行、类/字段/接口数、GPU 0 error、ASan、准确的 300 FPS 真实负载/微基准边界、PARTIAL/BLOCKED 历史状态。

### 15.1 每一项完成都必须能回答的问题

1. 从 Legacy 搬回了什么**实际 GPU 算法**，而不是只搬同名 Storage？
2. 新增的类型替换了什么，删除了什么；有没有一个新 `Owner/Manager/State/Adapter` 其实只转发旧类型？
3. 已经有 generation ID 的操作还有哪些 `find_if`？每个查找的规模上界与复杂度是什么？
4. 新 Resource/Attachment/Frame 如何保证 GPU 在途安全？是否又偷偷维护多份同义寿命标志？
5. Client 只拿业务句柄和 Reply 吗？是否意外暴露 VkQueue、SubmissionTicket、FrameSlot 等 Native 概念？
6. 你的 GPU Buffer/Shader 参数布局是否仍受一个临时 BasicMesh 样例的 128B Camera/416B Material/固定 set/binding 约束？
7. 生产 CMake 是否还能为纯 Graph/Client/Native Graph 提供独立消费者？
8. 仓内测试是否仅剩可观察渲染成果？旧机制失败证据是否迁出并保留？
9. 真实稳定帧热路径是否零分配，资源流送下 1%/0.1% 慢帧是否明显变化？
10. 当前源码是否足够可读，让另一位 C++ 开发者在不学习十几种“候选/租约/准备态”的前提下完成一个新专业 Storage？

---

# 16. 审查局限与实施方不得伪称的结论

- 本文核对了 **精确 Git SHA 的源码、头文件、构建目标和抽样 Legacy**，并用实际容器/函数定义证明存在复杂度或职责问题；本轮没有执行 Windows/MSVC/真实 GPU、ETW/VTune 或全库编译。
- **未完整证明所有源码/符号的全仓及外部安装消费者闭包**：`Encoding/Schema`、某些无消费者内部工具、某些旧测试/空转发头在实施时必须继续查证，才能标为最终 DELETE。
- 不能从静态代码推出某个改法一定使 FPS 提升 N%、内存下降 N%；尤其异步 `spawn` 的 10–30µs 高密度 burst 退化不是每帧固定成本。
- 不能依据类型/宏数量单独断定安全性无用；已接受 GPU 操作、跨Queue同步、DeviceLost、部分提交和外部 DLL 退栈仍然是底层必要约束。
- Legacy 不是完全无缺陷：`InstanceResources` 自身职责偏宽、MdcTable 使用 unordered_map 和无人使用的 mutationSerial、旧 MaterialFamily 固定分支都不应复制进新框架。
- 本稿中建议的新类型名字只是**职责候选**，不是给 Codex 的自由造类授权。实施目标是实际长期生产类型、成员、状态和目录复杂度降低。

**结论：最严重的代码质量问题，是“一个真实事实被多套类型表达”和“实际 GPU 数据布局被测试导向的事务包装替代”。新 C/S/Graph/stdexec 架构应当保留，但内部需以 Legacy 有效的数据结构、现代 C++ RAII、正确 ID 容器和清晰的层次重写这些错位职责，而不是继续在现有状态网络上追加适配层。**


---

# 17. v1.1 追加：人工 gapi/vk 与当前 Render V2 的差异和收敛设计

**正式风格优先级：** 人工编写的 `modules/platform/gapi/include/lux/engine/gapi/vk`（Native C++/Builder/抽象手法）+ Legacy Render（成熟 GPU 数据算法）+ 新 Render V2（C/S、Graph、Execution 架构）。既不回退 Legacy，也不把目前 gapi 所有历史 Wrapper 视为已经能直接满足每一个 Vulkan1.3/VMA/Result 要求。

## 17.1 源码事实矩阵：gapi 已有能力，并非 Render 再造它们的理由

| gapi 原有文件/类型 | 已确认提供的职责 | Render V2 现实重叠 |
|---|---|---|
| `PhysicalDevice.hpp` / `gapi::vk::PhysicalDevice` | 缓存 Properties2、MemoryProperties2、基础 Features2、QueueFamilyProperties2、DescriptorIndexingProperties；DeviceName、MemoryType、QueueFamily 查询 | `VulkanDevice` 自己重复 `vkGetPhysicalDeviceProperties`、Features、Queue Family 发现；`DescriptorArray` 再次查询 Descriptor Indexing |
| `Instance.hpp` / `gapi::vk::Instance` | 创建/枚举 Instance 与 PhysicalDevice 的 expected/RAII 值 | `InstanceContext` 另有原始 `VkInstance` 所有权及枚举 |
| `LogicalDevice.hpp` / `gapi::vk::LogicalDevice` | VkDevice RAII、创建失败 `expected<VkResult>`、Queue、waitIdle | `VulkanDevice` 重复 Native VkDevice create/release/move 代码 |
| `Queue.hpp` / `gapi::vk::Queue` | 轻量无拥有权 VkQueue 值 | `NativeQueue.handle` 可使用其 handle 风格；SubmissionQueue 仍是独立 GPU 提交进度机制 |
| `Buffer.hpp`/`Image.hpp`/Builder | Buffer/Image 的轻量句柄和声明式 CreateInfo 表达 | `Memory.cpp` 到处构造原始 Vulkan create structs，但实际分配由 VMA；不能直接用 Builder::build 取代 VMA |
| `Descriptor.hpp`/Builder | Descriptor Pool、Layout、Set 的创建与更新封装 | `DescriptorArray`、Shader/LayoutPlan 各自有裸 Vulkan 创建和毁灭路径 |
| `Pipeline.hpp`/Builder | Pipeline/Layout 的封装与创建参数构建 | Shader/Compile.cpp 自己构造 VkCreateInfo、VkPipeline 的清理列表 |
| `CommandPool.hpp` / `CommandBuffer.hpp` | 轻量录制句柄、命令池 Builder 与部分录制方法 | `SubmissionQueue` 自己管理 CmdPool/CmdBuffer/Fence，仍需要 GPU 状态；现有 OOP API 对 dynamic rendering/Sync2 不完整 |
| `Fence.hpp` / `Semaphore.hpp` | 明确返回 `VkResult` 的 fence wait/reset、可完整 RAII 的 Semaphore | `SubmissionQueue::Backing` 保存裸 Fence 数组、Timeline VkSemaphore，手工退出 |
| `Object.hpp` | **明确写出了 owning wrapper 与 non-owning handle carrier 的边界** | 当前 `AllocationPermit` 名义 Move-only 却丢失时不自动释放，属于另一种隐式清偿协议 |

**GAPI 包装分两种，不能混为一谈。** `LogicalDevice/Instance/Semaphore/Swapchain` 是有析构的 RAII Owner；部分 `Buffer/Image/ImageView/ShaderModule/DescriptorSet/Fence/CommandBuffer` 只是 Handle Carrier，`release(device,...)` 由其集合 Owner 执行。它们省掉每个高频句柄一份 parent handle 的成本，这是合理的性能优化，但**不等于可以泄漏、忘记释放或者默认依赖 Session 最终全表兜底**。

## 17.2 新增问题清单（与主文 D/C/R/G/M/T 审计并列）

### V01 — 同一物理设备被两套 API 查询（CONFIRMED；P0；DELETE DUPLICATE）

**位置**：`native/vulkan/device/Device.cpp` `VulkanDevice::create()`、`device/Device.hpp`；gapi `PhysicalDevice.hpp`。

**证据**：前者自己 `vkEnumeratePhysicalDevices`、`vkGetPhysicalDeviceProperties`、`vkGetPhysicalDeviceFeatures2` 和 Queue Family 两段查询，并缓存自己的 `VkPhysicalDeviceProperties`；gapi PhysicalDevice 构造已经缓存基础 Properties2、MemoryProperties2、Queue Family 及 Descriptor Indexing 属性。

**目标**：选定设备仅拥有一个 `gapi::vk::PhysicalDevice` 属性来源；设备选择/能力启用使用该对象的缓存（扩展 1.2/1.3 Feature chain 仍需初始化期查询）。淘汰 Render 专属同义 Properties 缓存，不再为每个 Storage/Handler 保存它。

**特别注意**：gapi 的 PhysicalDevice 构造器只对 `gapi::vk::Instance` 友元开放，现有 `InstanceContext` 持有原始 Instance；必须先确定唯一 VkInstance Owner 与最小可达缓存创建入口，不能新建第二个 Vulkan Instance 才获得缓存。

### V02 — `DescriptorArray::sampledImageCapacity()` 重复查 `VkPhysicalDeviceProperties2`（CONFIRMED；P0；DELETE QUERY）

**位置**：`native/vulkan/src/DescriptorArray.cpp:10–22`。当前实现每次创建 DescriptorArray 时重新构造 `VkPhysicalDeviceDescriptorIndexingProperties` chain 并调用 `vkGetPhysicalDeviceProperties2(device_.physical(), ...)`。

**目标**：从 Session 传入或借用 `physicalDevice().descriptorIndexingProperties()`，只做限制计算。真实启用/关闭的 `textureDescriptors()` 仍从 LogicalDevice 启用能力取得，不能只看物理支持。

### V03 — `VulkanDevice` 与 `LogicalDevice` 是同义 VkDevice Owner（CONFIRMED；P0；MERGE）

**位置**：`native/vulkan/device/Device.hpp:79–185`、`device/Device.cpp:303–318,520–583` 对照 `gapi/vk/LogicalDevice.hpp`。

**现状**：VulkanDevice 自己管理 `VkInstance` Borrow、`VkPhysicalDevice`、`VkDevice`、Properties、Queue 角色，以及多个布尔能力；gapi::LogicalDevice 已提供 VkDevice RAII 及 Queue 访问。

**目标**：收敛到一份 LogicalDevice RAII Owner，保存真实已启用的能力快照；DeviceSession 的 `nativeDevice()` 和 `physicalDevice()` 返回唯一事实，不再为“兼容现有接口”再保留一份 `VulkanDevice` 能力缓存。可分阶段迁移，最终无两套 Device Owner。

### V04 — Storage/Handler 重复保留 Device 信息（CONFIRMED 部分；P1；TRIM）

**位置**：各专业 Storage 保留 `DeviceSession&`，多数并未额外保存 VkDevice，这一点本身**已经符合用户期望**；但 `DescriptorArray` 同时借 `const VulkanDevice&`，其他 Native Backing 通过 `Physical` 间接获得 Device；部分 gapi 封装需要自行保存最小 parent handle。

**裁决**：不是机械删除所有 VkDevice 成员，而是任何长期 Device 属性、Capability、Queue、Allocator 都必须有唯一引用源。常规 Storage/Handler 应借现有 Session，不另存裸设备句柄；真正独立且可能被移动至 GPU 退休队列的 RAII leaf owner 为了销毁可携带一个 `VkDevice` 借用值，并由父 Session 寿命保证。

### V05 — Properties、支持 Feature、启用 Feature 的概念混淆（CONFIRMED 风险；P0；SEPARATE FACTS）

`PhysicalDevice.features()` 当前仅缓存基础 VkPhysicalDeviceFeatures2，没有 Vulkan11/12/13 支持链全部成员；而 Render V2 根据 options 选择同步2、Timeline、Dynamic Rendering、Descriptor Indexing、HostQueryReset 等条件。

**必须建立明确访问合同**：`supportedFeatures` 只描述物理设备能力；`enabledCapabilities()` 只描述已选 LogicalDevice 实际启用能力。Shader/Compiler/Queue 只消费 latter 的合同，不能假定“物理支持=逻辑已启用”。不为每种 Storage 再建立自己的能力布尔组。

### V06 — 设备静态格式支持被多个编译点重复查询（CONFIRMED；P1；CACHE COLD）

`native/vulkan/src/Shader.cpp` 对 Shader/Vertex Format 调用 `vkGetPhysicalDeviceFormatProperties`；`Compile.cpp` 对 Target/资源格式也调用，存在同一 Format 的重复查询。

**目标**：从 Session/PhysicalDevice 的只读格式查询入口读取，按 `VkFormat` 缓存静态格式支持；读到的物理支持仍须根据实际创建用途验证。Cold Compile 可以有首次查询；稳定帧 Execute 不调用它。没有证据不新增独立 `FormatService`。

### V07 — `Memory.cpp` 在分配 Image 时查询 VMA 的 PhysicalDevice 后再次查询 ImageFormatProperties（CONFIRMED；P1；CACHE WITH KEY）

`native/vulkan/src/memory/Memory.cpp:144–160` 从 `vmaGetAllocatorInfo` 取得 PhysicalDevice，然后按 `format,type,tiling,usage,flags` 查询 VkImageFormatProperties。这个查询不是简单地用 `PhysicalDevice::properties()` 即可替代；它依赖具体 Image 创建参数。

**目标**：PhysicalDevice/Session 提供专业的 `imageFormatProperties(format,type,tiling,usage,flags)` 静态查询，必要时按完整 key 做冷态缓存；如果实际种类少，也可以只在初始化配置/资源创建时查。不要对一切 Image 都错误复用某格式在另一 Usage 下的支持结果。VMA Allocator 仍处理实际 Memory Allocation。

### V08 — 重复 Native RAII：`SubmissionQueue::Backing` 与 gapi Fence/Semaphore/CommandPool（CONFIRMED；P1；REUSE WHEN SAFE）

`SubmissionQueue::Backing` 保存 `vector<VkFence>`、裸 `VkSemaphore timeline`、裸 `VkCommandPool`，在析构函数中逐项手动销毁。gapi 已有 `Semaphore` RAII、`Fence` 载体、`CommandPool` 载体与 `CommandBuffer` 载体。

**目标**：单个 Timeline Semaphore 可直接使用 `gapi::vk::Semaphore` 的 `adopt()`；大量 Fence/CommandBuffer 继续由一个聚合 Owner 统一清理，避免每个元素重复保存 VkDevice。不要因为 gapi CommandPool 旧的构造使用宏就绕过当前显式 VkResult 错误处理。能通过现成 create/factory/adopt 安全使用则迁移，不能则在 gapi 中小范围补充而非在 Render 复制 Wrapper。

### V09 — Pipeline/Shader/Descriptor 创建入口散落裸 Vulkan，仍存在重复封装（CONFIRMED；P1；TRIM）

`Shader.cpp`、`Compile.cpp` 与 `DescriptorArray.cpp` 分别创建 Shader Module、DescriptorLayout、PipelineLayout、Pipeline、DescriptorPool/Set，并手写清理。GAPI 已有 `PipelineLayoutBuilder`、`DescriptorSetLayoutBuilder`、`DescriptorPoolBuilder`、`SamplerBuilder` 和部分 Handle Carrier。

**目标**：只复用 GAPI 能完整表达当前 CreateInfo、pNext、VkResult 和 Vulkan 1.3 Feature 的路径；当已有 Builder 还只能在内部直接 create/`VK_FUNC_INVOKE` 时，应**先改进同一个 gapi 类的可恢复结果接口**，或者暂保留单个简洁的 Native RAII 创建逻辑。不能为了“使用 GAPI”新增 `RenderPipelineBuilder` 与同义 CreationState。

### V10 — 使用 Buffer/ImageBuilder 时容易误把 VMA 路径改成裸 `vkCreate*`（CONFIRMED 约束；P0；DO NOT REGRESS）

当前 GAPI `BufferBuilder::build()` 返回用 `vkCreateBuffer` 创建的 `gapi::vk::Buffer`，`ImageBuilder::build()` 返回用 `vkCreateImage` 创建的 Image；Render V2 的存储显式使用 VMA `vmaCreateBuffer/vmaCreateImage`。

**必须禁止**：将 Storage 改成 `gapi::vk::BufferBuilder().build(device)` 后再调用 VMA 的另一套分配过程；也不允许由裸 `gapi::vk::Image::release()` 去销毁 VMA 拥有的 Image/Allocation。若希望复用 Builder，必须提供返回合法 Vk*CreateInfo（包括所有 pNext Lifetime）的接口，再交给 VMA 调用。

### V11 — GAPI `PhysicalDevice` 本身含移动/复制的 `pNext` 绑定风险（CONFIRMED；P0 BEFORE EMBEDDING）

`PhysicalDevice.hpp` 构造时设置 `_properties.pNext = &_descriptor_index_properties`。类未明确禁复制或定义修正移动，常规复制/移动后该指针仍指向源实例成员。`Instance::listPhysicalDevices()` 又返回 `vector<PhysicalDevice>`，因此持久 Cache 实例被复制/移动是实际可能路径。

**范围说明**：该指针通常只在初始化查询时供 Vulkan 使用，因此不能据此直接认定当前代码已经 UAF；但公开缓存数据中的 `pNext` 不应在移动后引用无效成员。修复方案：把 pNext 查询链只放临时栈对象，结束后缓存普通值并清除/重建 pNext；或者明确自定义移动与复制规则。没有修复前不通过复制来构造第二份 Session 缓存。

### V12 — `PhysicalDevice` 若作为只读 Session 属性，部分方法缺 const（CONFIRMED；P2；TIDY）

`deviceName()`、`type()`、`findQueueFamilyIndexByFlags()` 和 `findMemoryTypeIndex()` 当前没有 `const` 限定，但访问的是缓存快照。应在 GAPI 原类加 const，不在 Render 新建一组 ConstPhysicalDeviceView。

### V13 — `findQueueFamilyIndexByFlags()` 的复合位语义需要明确（CONFIRMED；P1；CORRECT IF USED）

现有条件 `queueFlags & flag` 对任一位匹配就返回；如果调用方希望 Graphics+Compute 都具备，就不正确。若当前只传单个标志则不影响；未来改为统一设备选择时，应按实际需求更改 `(available & required) == required` 或清晰命名 `findAnyQueueFamily`。不维护平行的 `VulkanDevice` QueueFamily 查询实现。

### V14 — `PhysicalDevice::deviceUUID()` 并未缓存（CONFIRMED；P2；CACHE ONLY IF HOT）

方法内部每次调用 `vkGetPhysicalDeviceProperties2`，但一般只在 CUDA/Vulkan 匹配期间读取一两次。可以在设备发现阶段顺带缓存 UUID，**但如果该功能不是高频路径，不应因追求形式零查询而新增全局缓存机制**。

### V15 — GAPI `CommandBuffer::bind` 忽略传入 bind_point（CONFIRMED；P0 IF REUSED）

`gapi/vk/CommandBuffer.hpp` 的 `bind(VkPipelineBindPoint bind_point, VkPipelineLayout, ...)` 内调用 `vkCmdBindDescriptorSets` 时使用硬编码 `VK_PIPELINE_BIND_POINT_GRAPHICS`，因此不能用此接口绑定 Compute Descriptor。应在 GAPI 原方法小范围修正，再由 Render 的 Native Executor 采用；不应将这个已知问题复制到另一层 Wrapper。

### V16 — 部分 GAPI 旧载体的移动赋值可能覆盖活资源（CONFIRMED；P1；FIX NARROWLY）

现有 `Buffer`、`Image`、`ShaderModule`、`ImageView`、`Sampler` 等载体提供直接 handle move-assignment，不能自行释放被覆盖的旧 handle。GAPI `Object.hpp` 自身已明确警告：非拥有 carrier 不能 move-assign 到仍持有旧句柄的对象。Render V2 复用时必须遵守“先释放后转移”或删除不安全赋值入口；不能把“人工代码”误判成“所有细节完美”。

### V17 — GAPI 部分 Builder 的指针归属需限制到当前构造作用域（CONFIRMED 风险；P1）

`BufferBuilder`、`ImageBuilder` 可把 `pNext` 指向自身扩展结构；`PipelineLayoutBuilder::setLayouts()` 记录外部 vector 的 `data()` 借用。Builder 不能任意复制、移动或跨 CPU Sender 异步保活而不修复这些指针；其 `build()` 应同步在引用有效时完成。若需要长期持有 CreateInfo，应持有值数组/必要 backing，不随便复制 Native `pNext` 借用。

### V18 — `Object.hpp::VK_FUNC_INVOKE` 不满足 Render 的可恢复 Result 合同（CONFIRMED；P0 FOR MIGRATION）

当前宏遇到非成功且非 `VK_ERROR_DEVICE_LOST` 时主要打印诊断并 `assert`，release 模式不能可靠传播创建错误；这不适合 Render V2 的无异常 `RenderResult<T>`。GAPI 中已有如 `LogicalDevice::create()`、`Fence::wait()`、`DescriptorSetLayout::tryCreate()` 等更符合合同的先例。**应复制这种行为模式，而不是复制宏**。

### V19 — 不能把运行中变化的 WSI/显存预算缓存成物理设备静态属性（CONFIRMED 设计边界；P1）

Surface 能力、Present Modes、Format 可能随着具体 Surface/重建改变；MemoryBudget/HeapUsage 属于动态资源状态。PhysicalDevice 现有的 `swapChainSupport(surface)` 每次查询是正确的边界，不应强行把这些数据永久缓存到 DeviceSession。静态 Properties、Queue Family、Memory Type 则可以一次查询长期保留。

### V20 — GAPI 的 CMake 依赖已在 Native 可用，却没有形成代码复用（CONFIRMED；P1；REUSE）

`native/vulkan/CMakeLists.txt` 的 `render_native_vulkan` 已私有链接 `lux::engine::platform::gapi`。因此可在 Native 通过已有 gapi 头使用合理 Wrapper，不必新增新的 RenderGpuApi、VulkanBackendShim 或新的公共库。需要检查公共头引用 gapi 的安装传递依赖；中立 Graph/Resource/Client 不因 Native 复用而新增 Vulkan 依赖。

## 17.3 最少状态的目标 DeviceSession：两种方案及唯一选型

**推荐终态 A：**

```text
RenderServer / Host
  └─ InstanceContext（唯一 VkInstance Owner）
       └─ DeviceSession（唯一逻辑设备使用域）
           ├─ gapi::vk::PhysicalDevice（非拥有，缓存一次选中的物理属性）
           ├─ gapi::vk::LogicalDevice（拥有唯一 VkDevice）
           ├─ enabled capabilities（已协商启用的最小不可变值）
           ├─ VulkanAllocator/VMA（实际显存分配）
           ├─ physical queues + role index（无四份同义数组）
           ├─ staging/retirement（真实 GPU 工作状态）
           └─ Storage Registry + global StorageSet（设备层实例作用域）
```

如果 InstanceContext 最终改为 `gapi::vk::Instance`，必须同时保留现有 Vulkan Debug Utils Messenger 与真实 Instance Extension 初始化合同；gapi::Instance 的 `DebugReport` 不是自动等于现有 Debug Utils。不要为了拿缓存创建第二个 Instance。

**过渡 B：** `VulkanDevice` 内嵌且唯一拥有 `gapi::vk::PhysicalDevice` 缓存、`gapi::vk::LogicalDevice`，DeviceSession 只转发；迁移完成后才决定是否移除 VulkanDevice。**禁止双物理设备缓存/双 VkDevice Owner 长期共存。**

建议对外访问面限定为：

```cpp
const gapi::vk::PhysicalDevice& physicalDevice() const noexcept;
VkDevice nativeDevice() const noexcept;
// 以真实已启用能力为依据；不把 DeviceOptions 原样当作启用结果。
bool dynamicRenderingEnabled() const noexcept;
bool timelineSemaphoreEnabled() const noexcept;
```

- `Storage/Handler/Upload` 一律借单个 `DeviceSession&` 或实际所需的更窄 Native 组件。
- 不复制 `VkPhysicalDeviceProperties`、`VkDevice` 或现成的 Enabled Flags 到各专业对象作为新的权威。
- 在真正需要延迟退休的**独立 RAII 叶子 Owner**中保留释放所必需的最小 parent VkDevice/VMA 借用并不违规：否则已脱离 Storage 的在途资源在析构时可能找不到父设备。
- `GraphCompiler` 只接受静态设备能力及实际编译依赖，不把 DeviceSession 当 Runtime/Storage/Scene 万能查找表。

## 17.4 具体迁移关系：旧方法/成员 → 唯一目标

| 现有路径 | 当前语义 | 建议 | 必须保护 |
|---|---|---|---|
| `DeviceSession::device_`（VulkanDevice） | VkDevice Owner + PhysHandle + properties + enabled flags | 复用 gapi LogicalDevice + PhysicalDevice 缓存；消除重复的 own/release | VkDevice 唯一销毁、DeviceLost、所有子对象已退休 |
| `VulkanDevice::properties_` | 冗余的 `VkPhysicalDeviceProperties` 快照 | 读取物理缓存 `.properties().properties` | Vulkan Limit 与物理能力不变 |
| `VulkanDevice::physical()` | Raw Physical Handle | 仅在 Native/VMA 初始化必要时 `physicalDevice().handle()` | Instance 生命周期 |
| `DeviceSession::sampledImageCapacity()` | 每次额外查询 Descriptor Indexing | 缓存属性计算并可缓存最终 Capacity 值 | Enabled Descriptor Indexing 条件 |
| `Shader.cpp` format support | format/vertex 兼容性 | 从 DeviceSession 的缓存/统一冷查询读取 | 格式+用途语义 |
| `Compile.cpp` format support | Native Target/Graph format | 与 Shader 使用同一静态 format 事实 | Graph IR 独立 |
| `Memory.cpp` Image support | Image tuple capability | 统一的 ImageFormat 查询（按完整 tuple） | mips、layers、flags、usage、samples |
| `SubmissionQueue::Backing.timeline` | 单个 `VkSemaphore` | 优先 gapi::Semaphore Adopt RAII | Queue 与 Semaphore 释放顺序 |
| `SubmissionQueue::Backing.fences` | 固定容量 Fence 数组 | 有集合层的 RAII 毁灭，不必每元素保存 Device | Frame Slot Fence 正确性 |
| `DescriptorArray::layout_/pool_` | Native Descriptor Owner | 使用 GAPI 或同等一个实际 RAII Owner；删除重复销毁逻辑 | UpdateAfterBind、DescriptorIndex |
| `Shader::ShaderModule` 临时 handle | 编译临时 Native 资源 | GAPI Result+RAII 或已有正确的极小作用域清理 | Shader错误要可恢复 |
| `Compile.cpp::CompiledGraph` 多个 VkPipeline | 原生 Plan 成员 | 使用 gapi 管理正确的聚合 Owner 或单个 RAII 句柄组 | Pipeline 在 Graph/Frame 使用完前存活 |
| `BufferAllocation`/`ImageAllocation` | VMA Allocation 与 Vk 对象 | 移动式 VMA RAII，不把 GAPI 裸 Image 创建器塞进 VMA | Alias/LastUse/部分提交 |
| `Storage/Handler` 的 GPU 查询 | 运行中可能触发底层查值 | 借 DeviceSession 只读缓存和单份已启用条件 | 读取不能产生隐藏设备选择 |

## 17.5 具体优先级（与旧清扫路线合并，不另起无限新阶段）

1. **I：GAPI 接口可行性与少量缺陷修正**（只做缓存构造入口、移动/pNext 修复、`const` 查询，必要时 `CommandBuffer::bind` 的真实 bug）；结果必须能被现有 Native 单独消费者编译。
2. **II：DeviceSession 单物理缓存和唯一 LogicalDevice Owner**；完全保留当前已启用 Features/Queue/Instance/DebugUtils/VMA 创建资格；删除 Properties 重复权威。
3. **III：Descriptor/Shader/Compile/Memory 的静态查询归一与 Builder 复用**；优先改最容易证明的 DescriptorArray，Format 类按真正参数和收益审计。
4. **IV：Queue/Submission/Native 资源聚合 RAII**；分别判定哪种 GAPI owner/carrier 有效，避免引入新的泛化 ObjectManager。
5. **V：删除老 Native 包装和重复测试入口**，更新旧分析文档的文件/类对照、CMake 公共闭包，真实最终 GPU 场景验收。

这一顺序与原文 P2 Native 基础整改是**同一个整改阶段的子步骤**，不是必须再增五个独立项目。

## 17.6 强制实现与验收合同

- **证据**：每一项在最终结果中给出 Git 文件路径/函数、原始裸 Vulkan 创建或查询点、使用 gapi 的实质替代、删除的重复字段/初始化代码/宏以及剩余不可替代的 Vulkan1.3 API。
- **功能**：原同步/多Queue组合（Graphics/Compute/Transfer same physical vs separate）、FIF=2/3、Buffer/Image Aliasing、Descriptor UpdateAfterBind、Shader Pipeline、Offscreen 多 View、真实 Texture Upload/Readback 均不能退化。
- **状态**：物理支持、逻辑启用、动态 WSI/MemoryBudget 分开；不把兼容性查询变成每 Draw 的重复工作。
- **所有权**：没有第二个 VkDevice/Instance Owner；GAPI carrier 的销毁由一个明确聚合 RAII 所有者完成；VMA 对象绝不能误用 vkDestroy Buffer/Image。
- **风格**：没有新 `DeviceCapabilitiesManager`/`GpuFactoryFacade`/`VulkanObjectAdapter` 等补偿抽象；静态 Device 属性直接只读访问，专业 Storage/Handler 不再缓存同义 Device 值。
- **测试**：仓内 Render/test 保留真实效果与业务路径的高价值测试；硬件缓存/Builder 机制原型和故障注入如有需要在仓外可复现实验空间完成，不将新 `*_SEAM` 注入生产。
- **资格**：测试应覆盖 Debug/RelWithDebInfo/ASan、MSVC no-exception/no-RTTI、GPU Validation/SyncValidation error 0、Client-only、安装消费者；环境中未实际执行的不能声称 PASS。

## 17.7 源码锚点

- `PhysicalDevice.hpp`：https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/platform/gapi/include/lux/engine/gapi/vk/PhysicalDevice.hpp
- `Object.hpp`：https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/platform/gapi/include/lux/engine/gapi/vk/Object.hpp
- `LogicalDevice.hpp`：https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/platform/gapi/include/lux/engine/gapi/vk/LogicalDevice.hpp
- `CommandBuffer.hpp`：https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/platform/gapi/include/lux/engine/gapi/vk/CommandBuffer.hpp
- `Device.cpp`：https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/device/Device.cpp
- `DescriptorArray.cpp`：https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/DescriptorArray.cpp
- `Memory.cpp`：https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/memory/Memory.cpp
- `native/vulkan/CMakeLists.txt`：https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/CMakeLists.txt

**v1.1 审查结论：** 当前 Render V2 重复造了部分设备/属性/Wrapper 能力，而自己编写的 gapi/vk 已经提供了较多恰当的轻量、OOP 和 RAII 基础。优先在 existing GAPI 能力处修正真正的 API 缺口，减少 Render Native 自制创建/缓存重复代码；不要把“复用 gapi”误解为将所有 `Vk*` CreateInfo 和命令直接替换成无法返回错误的 legacy Builder。
> **v1.2 审查修订**：新增 L01–L32，重点追踪阶段化 RenderThread 的 CPU/GPU 双安全点、RenderEffect 独占 CompiledGraph 与 ExecutionGroup/Context、彻底删除专用 Readback，以及其它补偿性状态和错误归属。旧 D/R/V 审查项保留原编号；此前建议如与 v1.2 用户确认的生命周期方案矛盾，以此修订为准。新增内容为源码审查与设计指导，尚无新的生产改动或 GPU PASS 资格。

---

# 18. v1.2 增补：阶段化生命周期与补偿性设计

## 18.1 核心模型与不变量

~~~text
RenderThread:
  observeGpuProgress -> collectCommandsAndCpuResults -> applyChanges
  -> prepareFrame -> recordAndSubmit -> publishReplies

RenderEffect:
  CompiledGraph [sole move-only owner]
  ExecutionGroup [SceneId, ViewId, TargetId, unique ExecutionContext]

OffscreenTarget:
  real GPU output Image/Views; not a Readback business service

Physical GPU:
  SubmissionQueue serial + cross-queue dependencies -> existing RetirementQueue
~~~

两个安全点绝对不能混淆：结构修改安全点仅表明 RenderThread 当前不再 CPU 使用旧对象；GPU 安全点通过**实际物理 Queue 提交进度**判断是否可以销毁 Pipeline/Image/Buffer/Descriptor。不能通过 RenderThread tick、EffectId 失效或 C++ 引用计数推断 GPU 已完成；也不应在每个 CPU 阶段等待 GPU idle。

下面所有结论均相对固定 SHA 71fbe803620aba25ab34d79ca446325058d30df9。CONFIRMED 表示读到现有源码；DESIGN 表示目标；NEEDS_CLOSURE 表示需跑完整消费者/Native 正确性门禁。

## 18.2 生命周期问题台账 L01–L17

### L01 — ServerLoop 缺乏集中结构安全点（CONFIRMED，P0）

**证据** runtime/src/ServerLoop.cpp:182–239：当前每轮先 drainCompletions，再接受并立即分发 Command，随后 Outbox、advanceRendering、Completions、Outbox。advanceRendering 内执行 Graph 后直接 collect/dispatch GPU callbacks。ServerScene/ServerEffect 的同步 Handler 可直接变更 Effect、Scene/View、Target。

**问题** 结构改动、CPU Sender 完成、GPU callback 与本轮记录的先后约束并未由一个明确阶段边界表达，于是其他组件自己维护额外存活和取消状态。**目标** 阶段由函数及调用顺序明确，CPU/GPU 回调只将已完成结果送入同一待采用通道，结构变动在 applyChanges；recordAndSubmit 期间 Graph/Attachment/Target 绑定不可变。不为此增加新队列、新线程或显式阶段状态机。

### L02 — CompiledGraph 的父子所有权被横向切断（CONFIRMED，P0）

**证据** runtime/pinclude/ServerImpl.hpp 的 RenderEffect::code 是 shared_ptr<const CompiledGraph>；Effect::groups 保存裸 ExecutionContext*；DeviceSession.hpp 的 contexts_ 是 vector<unique_ptr<ExecutionContext>>；ExecutionContext.hpp 的 code_ 再持有 shared_ptr<const CompiledGraph>。

**根因** DeviceSession 拥有不属于设备生命周期的业务执行实例。Effect 删除后 Context 可以单独退休，Graph 不得被销毁，只能叠加 shared_ptr。**目标** Effect 唯一拥有 Graph 和 Group，Group 唯一拥有 Context；DeviceSession 不保留 contexts_。若同一 Effect 有多个 Group，它们只是共享读取 Graph，不独立共享拥有。Effect 退役后唯一拥有权移动到现有退休机制。

### L03 — Context 对 CompiledPass 保存长期指针（CONFIRMED，P0）

**证据** ExecutionContext::Pass::recipe 保存 const CompiledPass*；ExecutionContext::code_ 提供 plan、shader和device查询；Execution.cpp 构造 Pass 时保存 &recipe；DeviceSession::release 允许旧 Context 在 Graph 归属 Effect 已擦除之后继续存活。

**目标** ExecutionContext 只保存实际可变资源、Pass 索引、参数和 FrameBindings，不保存 Graph 的长期指针、shared_ptr、CompiliedPass*；Executor::execute(graph, context, target) 仅在一次调用期借用唯一编译结果，避免 C++ 指针寿命与独立退休交织。GPU 内部 pipeline 引用需要按物理提交水位退休，不能靠 CPU 借用证明。

### L04 — 退役 Effect 可能继续借用已删除的 Scene/View StorageSet（CONFIRMED 结构风险，P0）

**证据** ExecutionContext::StorageUse 同时保存 StorageLease/PreparedStorageBinding，StorageLease 借用 StorageSet*；removeScene/removeView 遍历效果删除 Groups 后马上擦除 Scene/View；release(Context) 会将部分 Context 转交 Retirement，并手动 withdraw()。

**风险** 若直接退休整个 Effect 却保留其 CPU 订阅，稍后 Context 析构可能访问已经删除的 Scene/View/Storage/Feature。**目标** CPU 结构安全点先解除引用、订阅、回调，留下真正必须存活到 GPU 完成的 Native RAII 所有物。此动作不能依赖新 SceneOwner、StorageLease2、GraphLease 等补偿类型。

### L05 — Effect 删除采用每物理 Queue 提交水位是有前提的保守方案（DESIGN，P0）

撤销 EffectId 并关闭后续执行入口后，可记录该 Effect 已经造成的实际 GPU 使用条件。对单 RenderThread、异步上传和多 Queue，低频 Effect 删除可保守使用全部相关**真实物理 Queue**的 lastSubmitted serial。不能仅看 Graphics，也不能在撤销以后允许 CPU Worker/插件回调再次提交使用旧 Effect；WSI Present 与 CUDA 互操作属于独立需要满足的实际完成条件。

**验证** 删除 Effect 时 GPU 提交仍在途、紧接着再次创建同名 Effect、两个 View 分属不同 Queue、Device Lost、Source/Module tail 都正确。最终不增加 RetiredEffectOwner：移动原有 unique_ptr<RenderEffect> 入唯一 RetirementQueue。

### L06 — Effect 移动与 C++ 指针稳定性必须一起证明（DESIGN，P0）

SlotMap 值若在 erase/insert 时移动，CompiledGraph 的 Pass 指针和旧 Frame 借用可能失效。建议 Effect 稳定对象地址，例如 SlotMap<unique_ptr<RenderEffect>> 或满足稳定性要求的成熟现有容器；把 unique_ptr 移入退休队列，而不是把内部整个 Graph/Effect 值搬走。不能给 Effect 添加不属于它的 device() 只为满足现有 RetirementQueue 模板 requires。

### L07 — Target 只是描述信息，而真实输出被放进 Context（CONFIRMED，P0）

**证据** ServerImpl.hpp:Target 只有 TargetDesc 与 epoch；Execution.cpp::DeviceSession::prepare 按 NativeResourcePlan 创建 Graph 输出 Image 和 ImageView，绑定在 Context 内；ResizeTargetResources 先创建替代 Context，手动释放旧 Context 后才提交新 Target。

**目标** OffscreenTarget 真正拥有自己的持久输出资源和相关视图；Frame transient、History、临时 Depth 等仍归实际 GraphInstance/帧资源。Resize 在 applyChanges/prepareFrame 采用新输出，旧资源与旧提交一起安全退休。同 Target 多 Effect 的合法顺序应按输出与资源合同明确，不以“只允许一个 Effect 占 Target”掩盖组合问题。

### L08 — ReadbackOperation 是侵入 Context 的另一套生命周期系统（CONFIRMED，P0，DELETE）

**证据** native/vulkan/Readback.hpp/.cpp：ReadbackOperation 有双向链表 previous_/next_、linked_、SubmissionCallback、owner/finish 函数指针、cancel/unlink；ReadbackSender 又包装 stdexec Receiver、Stop Callback。ExecutionContext 拥有 readbacks_/readers_/readback_ 并在析构时 cancelReadbacks。

**目标** 完全删除专用 ReadbackSender/ReadbackOperation/Readback Command；使用通用 Image→Buffer Copy、Host-visible Buffer、真实 Queue completion、Map/Invalidate 与拥有型字节消息。不得把相同功能改名 OffscreenCaptureSender 或 DownloadManager。

### L09 — 读取操作有隐藏 Draw 副作用和 RenderThread 阻塞（CONFIRMED，P0，DELETE）

**证据** ReadbackOperation::arm() 在 Context 没有 lastUse 时调用 context_->execute()；ExecutionContext::execute() 遇到 last_use_ && readers_ 返回 READBACK_HELD；completed() 还在 Callback 里可能再次 arm()。

**目标** 指定帧产生后在 GPU 队列中建立 Copy 顺序，将数据保存在独立 Host Buffer；之后的 RenderFrame 不受 CPU 下载是否完成影响。数据拷贝不是重新渲染的指令；任何结果提取函数都不得暗中调用 execute()。

### L10 — 任一 Buffer Graph Export 被强制变成 Host Read 资源（CONFIRMED，P0，DELETE）

**证据** Compile.cpp:290–295：GraphOutput 是 GraphBuffer 时直接 plan.readback=true；Compile.cpp:625–636 再制造 Host barrier；Execution.cpp:133–146 通过 EMemoryAccess::READBACK 创建物理 Host Buffer、设置 readback_ 索引。

**问题** GPU 数据流输出不是 CPU 消费声明。**目标** 删除 plan.readback、图导出自动 CPU staging/host barrier；由通用 Buffer/Copy/Host Access 的真实用途确定内存访问。Graph 自身保留普通 COPY_BUFFER、COPY_IMAGE_BUFFER 等 GPU 操作。

### L11 — BasicMesh 特意为验证建立 .pixels 子图（CONFIRMED，P0，DELETE）

**证据** features/src/MeshFeature.cpp:124–135；config.readback 为 true 时自动产生 color.pixels/depth.pixels Buffer 与额外 Transfer Pass、Graph Output。它为了得到像素，让基础 Mesh Feature 长期知道 CPU 结果读取语义。

**目标** 删除开关、固定 Copy Pass 与 Shader 依赖。仓内最终渲染效果测试仍可使用独立 Offscreen + 通用 Copy 路径验证像素。不得删除验证本身又声称原行为继续有效。

### L12 — Readback Command 被提升为内建业务协议（CONFIRMED，P0，DELETE）

**证据** runtime/include/.../Commands.hpp 的 ReadbackRequest/ReadbackResult/Readback；ServerCompletion.cpp 的 readBackOutput()/installReadbackCommand()；ServerEffect.cpp::installAsyncBuiltins() 调用安装；CMake 包含 src/Readback.cpp。

**目标** 删除对应 Command、Handler、源文件、头文件与专用编译路径。ServerCompletion.cpp 仍包含真正 CompletionQueue/Reply 的职责，不能整文件删除。

### L13 — Offscreen Image 获取 CPU 结果仍需要真实通用基础操作（DESIGN，P0）

CPU 不可直接读取最常见的 GPU-only Offscreen Image。通用 Copy 必须排在指定输出帧写入之后、同一输出复写之前，并以 Host 可访问 Buffer 保存独立快照。后续 CPU Map/Invalidate 要等**这个 Copy**的 GPU 完成。若 Client-only 目前缺少通用 Copy/Buffer访问入口，补必要中立 Command/资源能力，不加专门 Capture 服务。连续 300FPS 下的数据提取不得阻断正常新帧。

### L14 — RetirementReservation 预订未来 GPU 退休位置（CONFIRMED，P1，RESEARCH）

**证据** retirement/Retirement.hpp/.cpp，DeviceSession::prepare，Storage Candidate；Reservation 析构取消预订 SlotMap<Entry>，commit 后不能因容量或分配而失败。这个机制**有实际正确性目的**：提交后必须有安全位置留住 GPU 对象。

但是它把 GPU Queue 容量保证向上扩散，迫使 Effect/Context/Backing 维护专门的 Reservation 成员。建议研究物理提交/退休边界预热的 slot 预算或按真实 Accepted Submission 管理退休容量，若能证明提交后安全收纳不会失败再删除旧 Reservation。没有上述证明，**不能直接删 reserve()**。

### L15 — AllocationPermit 看似 Move-only RAII，析构并不释放（CONFIRMED，P0）

**证据** memory/Memory.hpp::AllocationPermit 的注释承认丢弃 permit 后交给 Session 最终回收；DeviceSession::~DeviceSession 扫全 buffers_/images_ 并反复寻找 alias；资源本身又有 retired_/released_/aliases_/source_。

**目标** 恢复已明确拥有权的 VMA Buffer/Image RAII 或拥有型集合级 RAII，将在途 Native 对象直接交既有 GPU Retirement。Image Aliasing 的共享底层内存与释放顺序必须保持，但不能再依赖 Session 全局扫描决定先释放谁。

### L16 — DeviceSession 是几种不同寿命的综合兜底拥有者（CONFIRMED，P0，REWORK）

**证据** DeviceSession.hpp 同时保存 buffers_/images_/contexts_/uploads_/ResourceRegistry/globals/DescriptorArray weak cache、多份 Queue 映射；析构 drain→contexts.clear→globals.clear→uploads.reset→退休清理→手动 Image/Buffer 销毁。

**目标** DeviceSession 只组合 GPU 物理能力、唯一 VkDevice/VMA Owner、物理 Queue、上传基础、Registry 等长期设备组件；业务 Effect 的 Context 归 Group，Native Buffer/Image 归实际专业资源或 FrameResources。销毁顺序通过真实 Owner/成员顺序表达，不靠通用 Session 扫全资源兜底。修订版 DeviceSession 继续读取 gapi PhysicalDevice 缓存。

### L17 — StorageLease/preloads/closing_ 企图弥补作用域寿命不清（CONFIRMED，P1）

**证据** resource/StorageSet.hpp 的 records(id,type,key,config,native,leases,preloads)，closing_，StorageLease 持 StorageSet*+Id；StorageSet::clear 在 leases/preloads 条件下手工寻找可释放项；Runtime PrepareStorage/ReleasePreparedStorage 单独提供显式预加载合同。

**目标** 先由 Global/Scene/View/Attachment Scope 及 Stage 3 删除次序建立父子关系，再决定是否有任何真实独立的预加载消费者需要一项专业有效身份。不能通过大规模 shared_ptr<Storage> 替换 Lease 后留下更复杂的共享所有权网。Stage 3 撤销时保证后续 CPU 操作不能取得旧 StorageId，GPU Native 退休则沿 SubmissionQueue 完成水位进行。

