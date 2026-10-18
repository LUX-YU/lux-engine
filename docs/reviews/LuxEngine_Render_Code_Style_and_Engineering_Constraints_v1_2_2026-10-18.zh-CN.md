# LuxEngine Render v1.2 — 源自 Legacy 的代码风格、架构边界与工程验收约束

**文档号：RENDER-STYLE-1.2**  
**日期：2026-10-18**  
**用途：** 后续 Codex/LLM 实施与独立审阅的强制约束（v1.2：补充渲染循环阶段、安全点、Effect 独占所有权和彻底移除专用 Readback）。本文不是实现报告，不声称测试已经运行。  
**审查基线：** [`LUX-YU/lux-engine@71fbe803`](https://github.com/LUX-YU/lux-engine/tree/71fbe803620aba25ab34d79ca446325058d30df9)（`codex/render-v2-rebuild`，2026-10-18 核验）；对应冻结旧代码在 `render_legacy/`。  
**配套文档：** `LuxEngine_RenderV2_Deep_Architecture_and_Code_Audit_v1_1_2026-10-18.zh-CN.md`。  
**优先级：** 用户当前明确要求 > 本文 > 经确认仍有效的 RGAM-DESIGN-2.0 目标及 Execution/IPC 决议 > 历史迁移实施计划 > 目前代码外观。明确冲突时必须列出设计差额，不能暗中叠加一层兼容代码。

> 核心声明：**保留新 Render V2 的 C/S、Command、Feature/Effect、独立 Graph、stdexec；以人工编写的 `modules/platform/gapi/vk` 为主要 C++ 代码风格与 Vulkan 接口组织参照，以 Legacy Render 为专业数据结构和算法参照。优先复用既有 gapi 封装，避免 Render V2 再实现同义 Wrapper；仅将 gapi 中未满足当前 Vulkan 1.3/Result/RAII 合同的部分做必要小范围改进。**


**v1.1 优先级补充（当前用户明确决议）：**

1. C++ 风格、Native API 表达和常用 Builder/RAII：优先参考没有 LLM 主导设计的 `modules/platform/gapi/include/lux/engine/gapi/vk`。
2. GPU 数据布局、算法、内存分配方式、增量更新：以冻结 `render_legacy` 的实际代码为对照，仍需去除其中历史遗留。
3. 新 C/S 模块边界、Command/Reply、stdexec、Graph：保留当前已通过结构性验证的设计。
4. **gapi 是参照及复用源，不是一个不需审查的“完美库”**：已知一部分类型是轻量 carrier，依赖调用者释放；部分旧 Builder 使用 `VK_FUNC_INVOKE`，不具有可恢复的 `Result` 合同。禁止机械替换导致错误传播或 VMA 所有权回退。

## 0. 使用说明：事实、风格与设计建议三类信息必须区分

- **Legacy 源码事实**：以下列出的已核对文件为依据，可以复查。但其所有语句不天然是永久设计约束，例如 Legacy 也有 `unordered_map`、单体 `InstanceResources` 和暂未被使用的 `mutationSerial`。
- **用户明确风格**：偏好现代 C++20/26、简洁类型、RAII、generation 身份、lux-cxx `SlotMap`/`SlotKeySparseSet`/`SparseSet`、无 RTTI/异常、使用 stdexec、单 RenderThread、零热态 C++ 堆分配、Server 不处理上层去重；本稿把它们具体化为可审查合同。
- **提议采用的实施准则**：索引策略、目录、文件归属、阈值和示意接口须在相应代码闭包验证；不要把建议类型直接当作批准的新生产类型。

**确认的 Legacy 依据：**

1. [`DeviceObject.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/render_legacy/modules/function/render/vulkan/sinclude/lux/engine/render/gpu/lifecycle/DeviceObject.hpp)：move-only Native 对象；创建、借用和释放的职责在同一类。
2. [`FifOwned.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/render_legacy/modules/function/render/vulkan/sinclude/lux/engine/render/gpu/lifecycle/FifOwned.hpp)：销毁动作按真实在途安全点延迟，并通过析构而非调用惯例执行。
3. [`InstanceResources.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/render_legacy/modules/function/render/vulkan/sinclude/lux/engine/render/resources/mesh/InstanceResources.hpp) / [`SparseInstanceStream.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/render_legacy/modules/function/render/vulkan/sinclude/lux/engine/render/resources/mesh/SparseInstanceStream.hpp)：Slot 身份，分页 SoA，独立 Dirty Page 与 GPU Stream。
4. [`InstanceResources.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/render_legacy/modules/function/render/vulkan/src/resources/mesh/InstanceResources.cpp)：字段变更直接写入、局部标脏、统一收集上传 Chunk，不为每个 Transform 建独立异步事务。
5. [`MeshResources.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/render_legacy/modules/function/render/vulkan/sinclude/lux/engine/render/resources/mesh/MeshResources.hpp) / [`MeshResources.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/render_legacy/modules/function/render/vulkan/src/resources/mesh/MeshResources.cpp)：几何 Arena、Segment、Stable Record、LOD/Section，统一 GPU 存储而非一 Mesh 一服务。
6. [`MaterialResources.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/render_legacy/modules/function/render/vulkan/src/resources/material/MaterialResources.cpp)：Material 参数以成组 GPU 表储存，不能把固定旧 Shading Family 当新终态。
7. [`Renderer.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/render_legacy/modules/function/render/vulkan/src/renderer/Renderer.cpp)：GPU 最终清偿是生命周期边界，不要求每个资源析构独自 `waitIdle()`。
8. [`ViewCameraOperationHandlers.cpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/render_legacy/modules/function/render/features/src/assembly/view_camera/ViewCameraOperationHandlers.cpp)：Camera 专业 Feature 输入，不属于通用 View/Server。

---

## 1. 设计优先级与“最简终态”

应按以下优先级做决定：

1. **正确性**：真实 GPU、内存、线程、IPC 请求与回复不能出错。
2. **职责明确与维护成本**：删除同义权威与隐藏协议，而不是多写“预防性”适配器。
3. **数据布局与算法复杂度**：GPU 密集数据按访问模式组织；需要 O(1) 查找时不接受无说明全局搜索。
4. **稳定帧性能**：RenderThread 全速运行；热路径零堆分配；Record/Submit、批量实例/材质更新优先于空 Sender 微基准。
5. **类型、文件和命名可读性**：新开发者沿调用链能识别业务实体、数据与硬件事实。
6. **局部源代码行数**：最后衡量。宁可增加一个 30 行的清晰算法函数，也不应保留几个相互纠缠的 15 行补偿类。

### 1.1 类型预算：先看职责是否已经有归属

增加一个长期类型之前写明：**一句话职责、真实拥有对象、实际消费者、方法集合、数据规模、复杂度、预期删除的旧类型/代码、所属模块**。

优先使用：

- 局部值、独立自由函数、`std::span`/`std::vector`/`std::array`、类型化 `SlotMap`、唯一 RAII 对象；
- 已有 stdexec Sender/Scheduler/Scope 和已存在的一个 RenderThread 就绪队列；
- 已经存在的专业 Storage/Feature 与 Graph/Plan 结构。

需要额外“Owner、Lease、Version、Ticket、Permit、Candidate、Manager、Context、Registry、State、Bridge、Adapter”时，**名称不能成为职责存在的证据**；先论证能否由原对象自身、框架、执行结果或容器 membership 表达。同名概念仍可以有真实用途，例如 Native 内部 GPU queue serial；不能机械禁词。

### 1.2 只保留一份事实来源

违规示例：`live_ + reserved_ + updating_ + retired_ + references + leases + 当前容器成员关系` 同时描述一个资源能否被使用。

正确方向：外部句柄是否存在由 generation 表决定；当前内容由一个 `current` 引用决定；正在运行的 CPU 操作由 stdexec 管理；已提交 GPU 工作由物理队列完成进度决定；尚在使用的 Native 对象由明确 RAII 保活/退休机制保证。

确实独立的事实可以共存，但必须列出**每种组合允许的状态**，若大部分组合都非法，优先重塑拥有关系使非法组合不能被构造。

## 2. C++ 类型与生命周期硬约束

### 2.1 RAII、移动语义与所有权

- 一个真实资源只应有一个明确的强拥有权源；当资源确实可能在被替换后继续被 GPU 使用，**移动拥有型对象**到现有 GPU 清偿/退休位置，或用可证明必要的现成共享不变产物保活，不是额外维护“是否已退休”的副本。
- Move-only Native 对象必须在析构中执行其**合法的**释放动作或入队退休；如果析构实际上无动作，不能把它包装成一个貌似 RAII 的 `Permit` 并要求其他类型承担隐式兜底。
- 不写全局析构补丁：`Session::~Session` 遍历所有 Buffer/Image 逐个手工销毁属于最后应移除的结构。
- 成员声明顺序和拥有型字段确保必要的父子对象顺序，避免析构函数里逐项 `reset()`；复杂关闭步骤只允许属于明确的服务生命周期边界。
- `shared_ptr` **默认不用**，但不禁用。确有多使用者共享不可变编译计划、资产 Source Owner 或跨异步任务的独立寿命时可以使用，须解释为什么一个 Move-only Owner + borrow 不能成立。
- `weak_ptr` 也不自动等于“安全引用”，尤其对象析构仍借用下级 VkDevice 时必须有明确父对象寿命证明。
- 禁 RTTI、禁异常。C++20 `concept`/`requires`、`std::exchange`、移动构造、显式 `Result<T>`、`std::span` 等优先；不为“现代化”刻意引入模板元程序设计。

### 2.2 核心生命周期区分

| 生命周期 | 真正责任方 | 禁止重复机制 |
|---|---|---|
| Client Request/Reply | IPC + Command 路由/Outbox | 二次 Binary Wire 协议（除另有真实资产消费） |
| CPU Sender OperationState | stdexec `counting_scope + spawn` | PendingOperationOwner、手写每请求 destroy 函数 |
| 专业 CPU/GPU 数据 | 相应 Storage | 复制几份为了存活的整表数据；除非算法/在途合同确有需要 |
| GPU 提交与进度 | Native `SubmissionQueue` | Client/Feature 层 `SubmissionTicket` |
| GPU 最后使用与物理回收 | 现有 Queue/Retirement + RAII | 每个 Storage 另造一套退休队列/Version/Lease |
| 编译计划与执行实例 | Effect 的计划拥有关系 + Attachment/Frame | Session 同时建立第二个长期 Context Owner |
| Shader/Material 作者图 | Material/Toolchain | Graph/Server 引用作者 Node/Pin 信息 |

### 2.3 绝对不要混淆 CPU 顺序与 GPU 完成

- 单 RenderThread 只提供 CPU 派发顺序；不同物理 Queue 的执行依赖仍需同步。
- 成功资源上传 Reply 的新目标语义是：必要 GPU 提交已接受、后续使用依赖成立、公开句柄已发布，**不强制 CPU 等待 Copy 实际完成**。Readback 成功仍意味着 CPU 字节可读取。
- GPU 失败与 `outcome_unknown` 在实际不可撤销副作用时保留。Device Lost 是 Vulkan 真实终态，不应通过多层 `is_device_lost` 变量扩散到普通业务。
- Frame/FIF=2/3、Alias、Bindless/BDA、Descriptor 更新与外部 Present 仍需要物理进度证据；仅 C++ 引用计数不能证明 Vulkan 内存同步。
- 保留唯一真实 RetirementQueue 语义；可以重塑内部节点和 RAII，不在同一系统增加第二个批次管理框架。

## 3. 专业 Storage 风格与 Feature 扩展

### 3.1 专业 Storage 由其作者命名和定义

不得有一个内建、通用、假定所有算法都使用 4×4 Transform 和一个 CPU DRAW Group 的 `InstanceStorage`。未来 Mesh/Skinned/Splat/Particle/Volume 的 Storage 可以具有不同 GPU ABI。

**专用声明及其 `static constexpr description` 必须紧邻该声明/实现所在的专业领域**。依赖 Vulkan 的实现可放同一功能目录下的 Backend 私有文件；**客户端使用的中立声明不得因“同一个物理文件”要求而包含 Vulkan**。

示意目录：

```text
features/mesh/include/.../mesh/MeshInstanceStorage.hpp     # 中立声明、Config、Descriptor、专业 Ops
native/vulkan/resources/mesh/MeshInstanceStorage.cpp        # Native 实现、Buffer/Stream/上传策略
features/particles/include/.../particles/ParticleStorage.hpp
native/vulkan/resources/particles/ParticleStorage.cpp
```

**不要额外创建一份名为 `vulkan::InstanceStorage` 的“镜像领域类型”，在公有 `InstanceStorage` 和 Native 实现之间强迫所有消费者按两个 Instance 的含义猜名字。** 必要的专业后端类应按具体职责命名。无需凭空给每一 Feature 都造 Storage；只有真实数据类型/更新算法不同才新增。

### 3.2 先设计 GPU 数据，后设计 API

- Instance：Slot identity，分页或增量 SoA Stream，分别维护 Transform/Property/Cull/Alive，Dirty Page/Range；更新 K 项的成本只与 K 和脏范围相关。
- Mesh：共享几何 Arena、suballocation、Section/LOD/GPU Record；大型与小型 Mesh 的策略可以不同，但独立 Buffer 不能无证据替代已经成熟的 Arena。
- Material：连续参数表及 Shader/ParameterLayout；参数变更不每次新建一个 416B Buffer；不恢复 Legacy 固定材质族。
- Texture：独立 VkImage 合理；Mips/Region/Bindless/Residency 依赖专业策略，但不复制五套生命周期状态。
- DrawList、Cull/Compact/Count 属于专业 Feature/GPU 算法，不塞回 InstanceStorage 作为内建标准 Draw 格式。

### 3.3 `StorageRegistry` 的职责边界

登记类型与定位实例。不得：决定具体 Mesh/Texture 的内存布局、推测 Handler 构造参数、执行 GPU 上传、反复用字符串寻找已定位对象。

实际 C++ `TypeToken`、Size/Alignment 校验只在安装或外部跨边界一次完成。热态 `get<T>()` 应在已验证类型的直接索引中工作，不再同时引入多个 Access/Ops/void* 层。

## 4. 容器和复杂度

### 4.1 强制查找原则

- **已知 generation ID → 直接 SlotMap 索引，O(1)**；不得 `find_if(vector)` 查找相应对象。
- **已知 GraphPassId/GraphResourceId → 紧凑数组直接定位，O(1)**；字符串键仅作为作者/冷装配名称，编译前一次性解析为 Index。
- **已知 ViewId/SceneId/EffectId → 对应直接索引，O(1)**；删除一个 View 最多遍历其 K 个真实 Attachments，不扫描全服务器。
- **已知绑定到某资源的 K 个消费者 → O(K)**；不要在每次更新时扫描所有 Effect/Group。
- 批量 Transform K 项：在 Engine 已去重的合同下应用 O(K)，脏页收集与实际上传量相关；不再 O(K log K) 排序去重。
- FIFO GPU Queue：不全表扫描已提交资源，完成进度处理实际完成的 K 个批次。
- 对字符串/复杂范围等不能无成本直接索引者，允许 O(log n) 有序查找、一次冷态排序或输出相关 O(V+E)。不要为了表面 O(1) 引入自制 hash/unordered_map 和大量稀疏内存。

### 4.2 具体容器准则

| 目的 | 首选 | 慎用/禁忌 |
|---|---|---|
| 动态 ID 对象 | `cxx::SlotMap<T, Tag>` | `vector` + 多套 generation/alive/free 状态重复实现 |
| 索引稳定的存储对象 | 实际必要时 `StableSlotMap` 或 `SlotMap<unique_ptr<T>>` | 为所有值无差别稳定地址导致多次 heap allocation |
| 动态 membership | `SlotKeySparseSet` / `SparseSet`，或稠密 vector+反向位置 | 没有明确数据密度就为每个对象开全局高水位 SparseSet |
| 大量 GPU 字段 | 连续 SoA/分页 Stream | 每元素/每材质独立小 VkBuffer，除非实测合理 |
| 图静态引用 | 紧凑 `uint32_t` ID、CSR/稠密扁平数组 | 重复字符串搜索、每边额外分配、每节点多个虚表对象 |
| 真实 Queue 角色 | 小 `std::array`，物理集合 + 角色索引 | 每种角色复制一份有独立强拥有权的对象 |
| 名称映射 | 冷态排序表/有序索引/受控 TypeRegistry | 手写 Hash + `unordered_map`，无证据与零开销目标不合 |
| 上传脏范围 | Page bitmap/dirty list/预热 scratch | 每次逐记录排序、创建临时 vector 与冗余历史快照 |
| 生命周期 | Move-only RAII，必要时现有 Retirement | Lease、Permit、Version、Owner 自制协议堆叠 |

**不强迫所有 O(n) 都改 O(1)。** 实际要处理 K 个对象必然 O(K)。Graph 的拓扑分析不是按已知 ID 查找，应按算法最优复杂度判断。对于只有三个物理 Queue 角色的数组线性扫描，O(3) 完全合理；如果两个类型因此多维护四张平行数组，错误在状态组织而非 O(3)。

## 5. Execution、RenderThread 和 C/S

- 保留单进程 1 Server + 多 Client、Requester/Responder、双 SPSC；各消息是进程内类型化 Command + 拥有型附件，不能重新引入二进制 Wire 转码。
- 每个合法请求必须得到一个最终 Reply。`RequestId` 与 Requester 身份属于 IPC 业务关系，不可由 GPU Ticket 代替。
- stdexec 负责 CPU 调度、Continuation 与 OperationState 生命周期；`RenderServer::run` 执行真实事件循环，**不得加第二线程、额外 Render 专用线程池或 `sleep_for` 降速**。
- `RenderThread` 满速运行，受 VSync/GPU 或实际空闲等待限制。已有 `waitUntil` 的空闲唤醒策略不是人为降帧。
- 传输工作用可组合 Sender 表达，Native Queue 接受 GPU 工作后，取消不能伪造 GPU 回滚；必须保留准确的 partial submit outcome。
- 自定义 Sender/Scheduler 只能用于没有标准库对应的真实外部异步源/RenderLoop 接缝；不能为每个 Effect/Storage 实现一套私有 Operation、Receiver、ErrorBridge。
- CPU Worker 不直接修改 Server 内的 Scene/Effect/CommandRegistry 或 ResponseOutbox。专业缓存可在可证明线程安全条件下访问。
- 新异步路径允许少量堆分配；真实帧 Record/Submit 的零分配和真实流送时延优先于空 Command 的微秒级 p95。

## 6. 命名、代码布局与文件组织

### 6.1 名称必须是“真实事物 + 真实动作”

禁止将 `cold`, `hot`, `owner`, `ready`, `candidate`, `backend`, `state`, `context`, `data`, `work`, `operation` 等**含糊词单独用作语义标识**；有真实限定时允许，如 `VkPipelineCreateInfo`、`RenderGraphInstance` 或标准 stdexec `operation_state`。

| 旧式或不清晰 | 正确意图需明确表达为 |
|---|---|
| `CommandEntry::cold` | `description`/`bindings`/`selected_handler`（按实际内容） |
| `queue_owners_` + `queues_` | `physical_queues_` + `queue_by_role_`（单拥有集合） |
| `retirement_owners_` + `retirements_` | 每物理 Queue 的退休组件，不建第二张“owner”表 |
| `globals_` | `global_storages_` |
| `uploads_` | `upload_context_` / `staging_`（按实际职责选其一） |
| `buffers_`/`images_`（Session） | 移除中央资源目录；若确有记录，标明 `buffer_allocations_` |
| `storageCandidate` | 一次注册计划/准备的明确动作，或删除该隐式模板 |
| `drain()` | `waitForSubmittedWork()`（如确有公开等待需求） |
| `owner()`（SubmissionTicket） | `queue()`；更多 Ticket 应缩至 Native 私有 |
| `Use`/`Physical`/`Backing`（不说明用途） | `PreparedBinding`、`GpuImage`、`GeometryAllocation` 等实际概念；无独立职责则删除 |
| `updateCamera()`（Runtime） | 删除通用 Camera 路径；专业 Feature 接收已计算的 frame input |
| `RenderScheduler` 置于 `ServerEffect.cpp` | 若确为通用 RenderThread 调度，应由统一事件循环入口提供 |

**不要因为这张表而仅仅换名字。** 如果一个现有类型的成员分不到单一职责中，它需要重分配或删除。每个新文件、类型和字段都必须出现在职责图中。

### 6.2 代码书写

- 遵守根目录 [`.clang-format`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/.clang-format)：Allman braces、120 列、`InsertBraces: true`、`SeparateDefinitionBlocks: Always`。
- `if/for/while` 单句也写 `{}`，除非既有人工批准风格另有局部例外。至少段落间保留空行：验证、定位、准备、提交、发布、回收各自是一段。
- 长函数应按**独立算法阶段**拆成少量局部函数，不为减少函数行数新建业务 Manager。
- 非平凡变量有说明其业务/物理语义的名字。`value`, `input`, `owner`, `state`, `result` 在小范围纯局部可用；跨 100 行或跨对象成员不可含糊。
- `[[nodiscard]]` 放在有实际丢返回值风险的操作；无意义的手写 setter/getter、只转发一行的 Impl 不能因“封装”而无条件增加。
- 热路径不重复 TypeToken/Size/Alignment、对象有效性、Shader ABI 等已经在冷装配保证的条件。
- 测试专用宏 `*_SEAM`、成本仪表、人工时序门禁不能进入生产代码；真正有条件编译意义的配置宏/VMA 实现宏另论。
- Header 只导出真实消费者所需 API；私有实现放 `pinclude`/`src`，不靠相互 include 补齐隐含依赖。

### 6.3 目录是边界，不是类别词拼贴

公开目标：

```text
render/
  core/               # Error/IDs/纯中立值
  commands/           # 类型化Command声明；无运行时Handler/IPC
  resource/           # 存储抽象合同/注册目录；专业数据放专业Feature所属模块
  graph/              # 唯一逻辑图及分析（不含Feature/Native/Runtime）
  features/           # Feature声明、端口和专业算法
  native/vulkan/      # device/memory/submission/transfer/compiler/execution/专业Native
  runtime/            # Server装配、场景与Effect关系、IPC Reply
```

旧 `render/data`、`render/protocol` 不以名字永久保留：真实 Shader 合同进入已有 `rdesc`，必要 Command 合同进入 commands；`Encoding/Schema` 必须验证外部消费者，已不用的可删。**同一功能的中立声明与 Vulkan 实现可分在相邻对应子目录，不为“把所有代码塞进同一头”牺牲 Client-only 依赖。**

目录必须使扩展新 Feature/Storage 时仅新增该功能下的文件和注册入口，不修改 Core 的目录结构和通用类型枚举。

## 7. 测试政策：必须执行用户指定的严格分界

### 7.1 仓内 `modules/function/render/**/test/` 的唯一用途

**仅保留验证最终可观察渲染成果的测试。** 至少从正式 Client/已安装公共接口构建真实资源、Effect、Scene/View、执行/Readback，并断言实际像素、深度、几何、材质、纹理、阴影、实例和多 View 结果；需要的话开启 Vulkan Validation/SyncValidation 与 ASan。允许少量明确的渲染样例作为测试素材。

**不得再把以下测试常驻 Render 模块：**

- SlotMap、CommandRegistry、Factory、StorageSet、TypeToken、Handler 目录、纯结构一致性测试；
- stdexec `spawn`/`counting_scope`、特殊 Receiver 尾栈、人工 worker freeze 或自制调度状态机证明；
- Queue Ticket、Retirement 内部机制、`GpuWork`、失败注入/堆分配次数、ASan 诱发器；
- `AsyncCost`、`MemoryCost`、ResponseCost、TSC microbenchmark、测试阶段独有事件/seam；
- Header 逐个编译、安装 relocation、Toolchain 环境探针等**基础设施验证**（转入外部 CI/验收工作区，不占据 Render 项目测试目录）。

### 7.2 这些机制需要验证时如何做

- 在 **仓库外**建立短寿命验证工作区（sandbox/实验分支外部镜像/独立验收项目），允许临时测试源、fault injector、microbenchmark、mock 和时序闸门。
- 与生产源码交互优先用正式接口；如必须实验性插桩，使用外部编译/链接注入，不把 `#ifdef LUX_*_SEAM` 合入生产文件。
- 保存必要的验收证据：被测精确提交、完整编译参数、原始日志、失败样本、性能脚本、哈希和可复现方法。
- **删除仓内旧测试之前先分类**：最终渲染效果继续留；机制测试迁出外部空间；失效/同义、无可复用价值者删除。不能仅删除测试使失败数变少，就宣布生产代码通过。
- 允许外部短期深度测试 GPU 生命周期，但最终仓内必须有真实端到端场景证明效果和 Validation 0 error；测试粒度服从用户需求。
- 不把测试框架设计回填成产品 API。无法从真实 API 验收的内部机制应通过逻辑/静态推导、专业审阅和外部实验验证，而不是永久建设专用测试接口。

### 7.3 验收报告分类

| 结果类型 | 何时使用 |
|---|---|
| `PASS` | 实际最终业务行为及必要真实 GPU 安全已执行通过 |
| `PARTIAL` | 功能正确但有已记录的性能/平台/语义缺口 |
| `NOT_RUN` | 没有真实执行证据 |
| `BLOCKED` | 工具链、设备或依赖导致真实验证无法运行 |
| `KEEP / FUTURE` | 发现疑似冗余但没有净删除或安全证据；不伪称已删除 |

过去的 Root TOOLCHAIN BLOCKED、Debug 历史失败和性能 PARTIAL 继续保留；新方案可接受已明示的微基准成本，不代表真实稳定帧回归豁免。

## 8. 不引入重复框架的实施流程

1. **查 Legacy 来源**：同一功能的字段、GPU ABI、算法、时间/空间复杂度、生命周期和测试依据。
2. **查当前实现**：列出完整消费者、关键热点和所有可以互相影响的状态，分别标为 KEEP、MOVE、MERGE、DELETE、RESEARCH。
3. **写一张职责表**：每个长期类型最多一个主要职责；每个成员及公开方法能按职责归位。
4. **先删除并行表达，再引入必需对象**：例如 `CommandEntry` 不再保留 `active+cold` 两份权威，队列角色不再保留四张互相配对的数组。
5. **明确拓扑和索引**：ID 直接查；专业 Storage 保留 Legacy 成熟算法；Frame/GPU 使用寿命独立于 CPU Owner。
6. **代码实施**：一个职责闭包一个生产 I；旧代码、转发头、失效 CMake 条目同步清理。不要新增长期迁移桥，也不要留双路生产实现。
7. **外部机制验收 + 内部终态渲染验收**：只认真实编译运行证据；独立 V 仅保存报告。
8. **成本与维护对照**：实际删除类/字段/手工状态/资源扫描；对热路径记录真实数据，不用 “+N 个测试” 作为进度指标。

### 8.1 设计必须暂停并呈报的情况

- 需要第二 ResourceRegistry、第二 Graph/IR、第二 GPU 完成队列或第二 OperationState Owner；
- 需要给专业 Feature 加上通用 Camera、材质模型或 Instance ABI 限制；
- 新增 Lease/Version/Owner 只是为旧实现保活而不能证明一个真实业务事实；
- 需要一组状态布尔值才能判断对象能否被析构；
- 改变资源上传成功点、GPU LastUse、Shader ABI、跨 Queue 同步或 Readback 合同；
- 必须隐藏真实错误、删除测试或更改工作量以宣称验收通过；
- 需要修改冻结 Legacy 原件、用户工作树或其他非授权模块。

## 9. 代码审查表（每项必须回答，不能只打勾）

| 维度 | 审查问题 |
|---|---|
| 职责 | 类名能否准确描述所有主要方法？有无属于其他层的方法？ |
| 实际算法 | 相比 Legacy，GPU 存储和计算逻辑是否等价或有经验证的替代收益？ |
| 生命周期 | 只看成员及析构能否理解资源何时释放？有多少手工 `release`/`retire` 配对？ |
| 状态 | 哪些字段是原始事实，哪些仅是其他字段的副本？非法组合是否可构造？ |
| 查找 | 已有 generation ID 时是否直接 O(1)；已知 K 个受影响对象是否仅遍历 K？ |
| 动态内存 | 稳定帧有无 Heap allocation；高频 K 更新有无排序/每项分配？ |
| 命名 | `owner`, `cold`, `state`, `work`, `candidate`, `globals` 具体是什么意思？ |
| 公开 API | Client 是否能看到 Ticket、Native Vk 类型、Frame Slot 等不必要概念？ |
| C/S 与 Execution | 是否继续使用已有命令管线、唯一 RenderThread、stdexec Scope？ |
| 依赖 | Graph/Resource 声明是否能独立于 Runtime/IPC/Vulkan/执行线程？ |
| 目录 | 这个新 Feature 能否只增专业文件而不改通用文件夹结构？ |
| 测试 | 生产测试目录是否仅有真实最终渲染；其他验证是否在外部？ |
| 回归 | 正确性/GPU Validation/ASan/热态成本是否有真实数据，失败证据是否保留？ |
| 净收益 | 实际删除哪些类型、分支、成员、文件；新增对象是否比旧机制更少？ |

---

## 10. 反例与正确方向（示意，不直接照抄为产品 API）

### 10.1 错误：四份同义的角色/物理资源映射

```cpp
std::array<std::unique_ptr<SubmissionQueue>, 3> queue_owners_;
std::array<SubmissionQueue*, 3> queues_{};
std::array<std::optional<RetirementQueue>, 3> retirement_owners_;
std::array<RetirementQueue*, 3> retirements_{};
```

方向：实际物理 Queue 的拥有型记录包含其提交与退休组件；逻辑角色只保存物理 Queue 索引。**同一 VkQueue 只能有一套提交序号**。表达成一个拥有集合 + 一个角色映射，而非四个可各自失同步的字段。

### 10.2 错误：将领域模型拆成几层不明确的生命周期状态

```text
Storage::Entry(updating)
  → Candidate(published)
  → Backing(live/references/last_use/retirement)
  → Physical(shared_ptr+bytes+versions)
  → Use(binding callback)
```

方向：专业 Storage 保存当前记录；真实 Native Backing Move-only RAII；必要的退休由现有 Queue 保留；Renderer 持有已准备的资源绑定。可减少 Candidate 状态，保留真实“操作尚未提交”的局部事务值。

### 10.3 错误：将示例 Feature 的 ABI 当全局标准

```text
render/resource/Instance.hpp
  InstanceDescription = Mesh + Material + Submesh + Mat4
  EInstanceBinding = MATRICES | DRAW
```

方向：专业 MeshInstance 定义自己的 ABI 和 `static constexpr description`，粒子、Splat、Skinned 等自行声明本领域的 Storage；Core 不提供一个“全世界唯一的 Instance”。

### 10.4 错误：重复寻找已经有 ID 的对象

```text
updateCamera → 遍历全部 Effects/Groups
releaseContext → find_if(contexts_)
setParameters(GraphPassId) → find_if(passes_)
```

方向：删除 Camera 的 Core 语义；Attach/Context 有稳定 ID 与直接数组位置；GraphPassId 直接访问数组。必要关系迭代的成本是 O(K)，不是 O(全局所有对象)。

---

**本规范的目标：用明确的数据类型、独占 RAII、正确容器、既有 Execution/GPU 完成机制，使复杂功能“可自然写出来”，而不是用一长串互相检查的状态去证明代码暂时不会出错。**


---

# 附录 A（v1.1 强制追加）：以人工编写的 gapi/vk 为主要代码风格基准

本附录是正式工程约束，而不是建议安装另一个图形抽象框架。源码固定为 `LUX-YU/lux-engine@71fbe803620aba25ab34d79ca446325058d30df9`，`modules/platform/gapi/include/lux/engine/gapi/vk`。这 26 个 `vk/*.hpp` 文件已具备物理设备、逻辑设备、实例、命令池/缓冲、Fence、Semaphore、Swapchain、Buffer、Image、ImageView、Sampler、Shader、Pipeline、Descriptor 和 Builder 等封装。新的 Render V2 不应对这些事实重新造一套通用 API。

## A1. 对 gapi 风格的正确理解：持有对象、值载体和借用不是同一件事

`gapi/vk/Object.hpp` 开篇的 Ownership model 是优先阅读材料：

- **设备级低基数对象**：如 `gapi::vk::LogicalDevice`、`Instance`、`Semaphore`、`Swapchain`，自身包含 Native Handle、必要的 parent/allocator 信息、析构、移动和 `reset()`，是真正的 RAII Owner。
- **高基数轻量载体**：如现有 `gapi::vk::Buffer`、`Image`、`ImageView`、`CommandBuffer`、`Fence`、`DescriptorSet`、`ShaderModule` 中不少类型，故意主要保存句柄，并提供 `release(device, ...)`；它们不是独立资源 RAII Owner。所属的专业 Storage、Buffer Array、Submitted Batch 或唯一集合 Owner 在安全点统一负责销毁。
- **借用**：普通 Storage/Handler/Feature 只是对 DeviceSession 等上游 Owner 的明确引用，既不另存一份强 Owner，也无需每个对象再维护一个可独立失效的设备缓存。

> **禁止错误解读：** 不能因为某个 gapi class 名叫 `Buffer` 就假定其析构一定调用 `vkDestroyBuffer`；也不能为了“全 RAII 化”给每个 VkImageView/DescriptorSet 载体塞一个 `VkDevice` 和一个独立析构器，使上万个热数组成员无谓膨胀。**RAII 可以位于拥有整个数组的容器层**，不必一定放在每个叶子值上。

### A1.1 DeviceSession 派生类型的设备引用规则

| 所在类型 | 持有什么 | 不允许持有什么 | 理由 |
|---|---|---|---|
| `DeviceSession` | 一个已选物理设备的缓存快照、一个逻辑设备的唯一 Owner、真实启用能力以及 Queue/Allocator 组件 | 第二个逻辑设备 Owner 或重复的属性缓存 | 所有硬件事实一个来源 |
| 专业 `Storage` | `DeviceSession&` 或明确更窄的长期借用 | 额外 `VkDevice device_`、重复 `VkPhysicalDevice physical_`、每次使用重新查询属性 | 外部 Session 是真实寿命边界 |
| Native `Handler` | 构造时显式取得实际所需服务的借用 | `VkDevice`/VkPhysicalDevice 复制缓存，除非独立持有 Native 对象所必需 | Handler 负责 Commands，不负责设备身份 |
| `RenderGraph` 中立层 | 纯数据/声明 | `DeviceSession`、gapi、Vulkan Handle | 保持 Graph 独立 |
| Native `GraphCompiler` | 必需的已启用硬件能力与图/Shader 输入 | 任意资源 Registry/Server/Scene 查询 | 编译期能力查询由选定设备提供 |
| Native 小型 `Buffer/Image/Sampler` 载体 | 单个 Native Handle | 每份复制物理设备属性、DeviceSession 全部功能 | 防止高基数对象膨胀 |
| 跨 GPU 退休边界的独立 RAII Owner | 自身正确释放所需的最小 Device/Allocator Borrow（可能是一个 VkDevice 值） | 多份同义强拥有者 | 当退休可晚于局部 Storage，必须保证销毁所需 parent 仍在 |

**精确解释用户的“不要重复持有 VkDevice”：** 主要禁止 Storage、Handler 等长期组件重复保存已有 Session 设备身份；并不绝对禁止独立 RAII 叶子 Owner 保存销毁所必需的 `VkDevice`（一个 64-bit Handle 并不会成为第二个逻辑设备所有者）。若将此类字段改成 `DeviceSession&` 导致高基数对象变大或退休时悬空，则不应这样做。

## A2. 设备能力：`PhysicalDevice` 提供唯一支持能力来源，Session 提供启用能力来源

`gapi::vk::PhysicalDevice` 构造时缓存：

| 查询入口 | 缓存事实 | 使用例 |
|---|---|---|
| `properties()` | `VkPhysicalDeviceProperties2`，其中 `.properties.limits` | Vulkan Limits、设备名称/API 版本 |
| `features()` | `VkPhysicalDeviceFeatures2` 的基础核心成员 | 支持的基础 Features |
| `memoryProperties()` | `VkPhysicalDeviceMemoryProperties2` | Heap 与 Memory Type |
| `queueFamilyProperties()` | `vector<VkQueueFamilyProperties2>` | Queue 族选择 |
| `descriptorIndexingProperties()` | `VkPhysicalDeviceDescriptorIndexingProperties` | Descriptor 数量、UpdateAfterBind 限制 |
| `findMemoryTypeIndex()` | 在已缓存 Memory Type 中 O(最多32) 查找 | 选择内存类型 |
| `findQueueFamilyIndexByFlags()` | 扫描小型已缓存 Queue Family 表 | 单一 Queue 能力查找；复合 flag 要核对 any/all 语义 |

因此，`DeviceSession` 应提供**无需二次查询的转发访问**：

```cpp
const gapi::vk::PhysicalDevice& physicalDevice() const noexcept;
VkDevice nativeDevice() const noexcept;
const EnabledDeviceCapabilities& enabledCapabilities() const noexcept;
```

其中 `EnabledDeviceCapabilities` 仅是**真实启用能力的最小不可变值**（可复用已有归一化 Options/字段；不是必须新增新类），**不能直接把 `physicalDevice().features()` 当作“已启用”**。Graphics/Compute/Raster 的路径需要确认 Device 创建时已经开启所需 Features/Extensions。

### A2.1 不允许重复的属性缓存

- `VulkanDevice::properties_` 与 `PhysicalDevice::properties()` 不应长期并存为两个独立复制、可分别更新的权威。
- `DescriptorArray::sampledImageCapacity()` 不能每次再构造 `VkPhysicalDeviceProperties2` 调 `vkGetPhysicalDeviceProperties2`，而应读取 `physicalDevice().descriptorIndexingProperties()`。
- Device 的 GPU 选择逻辑应复用已缓存的 Queue Family/Properties/Memory 类型；只在 Bootstrapping 时为选定物理设备查询尚缺少的扩展 Feature chain、Extensions。
- Storage/Handler/Feature 的 `create`、`prepare`、`upload` 不应该自己调用“设备 Properties 查询”来决定实现，应该读取 Session 的已缓存事实。

### A2.2 缓存不得盲目扩大

- 当前 `PhysicalDevice` 只缓存基础 `features()`，不包含所有 Vulkan 1.2/1.3 的 Feature 链；如需 `timelineSemaphore`、`synchronization2`、`dynamicRendering`、`descriptorBindingPartiallyBound`、multiview 等，须在**设备发现/创建阶段**统一查询并保存合适的支持/启用结果。
- `vkGetPhysicalDeviceFormatProperties` 可在首次遇到特定 `VkFormat` 时查询并缓存（或在启动时针对实际使用的有限格式建立表），但不能误用一个“默认 RGBA8 支持”结果代替所有格式。
- `vkGetPhysicalDeviceImageFormatProperties` 的结果取决于 `format/imageType/tiling/usage/createFlags`，应以完整关键参数缓存或在真实冷态资源创建时查询；不得按 Format 单字段错误缓存。
- WSI `VkSurfaceCapabilitiesKHR`、Surface Formats/Present Modes 与特定 Surface/窗口状态有关，不应永久当成静态设备属性；按创建/Resize/Surface 更新合同重新查询。
- 设备内存预算/使用量是动态事实，不能错误地永远缓存启动值；只有实际触发内存预算决策时按需要更新。
- `PhysicalDevice::deviceUUID()` 当前仍调用 `vkGetPhysicalDeviceProperties2`，不是缓存值；频繁使用时应在构造期保存 UUID，但一般只在启动/互操作匹配时用，不为“彻底零查询”单独重构。

## A3. C++ Builder 及 OOP Vulkan 包装：优先复用、不能重复发明

优先使用 GAPI 已有名字与惯例：

| 场景 | 候选既有 API | 正确用法 |
|---|---|---|
| 物理设备发现和缓存 | `gapi::vk::Instance` / `PhysicalDevice` | 选定后只保留一份缓存实例，保持 Instance 生命周期 |
| 逻辑设备 RAII | `gapi::vk::LogicalDevice` | `create()` 返回 `expected`，析构 `reset()`，不隐式 waitIdle |
| Queue 原生载体 | `gapi::vk::Queue` | 作为非拥有 VkQueue Handle，Queue Submit/Fence 仍由 Native SubmissionQueue 管理 |
| 单个 Semaphore 所有权 | `gapi::vk::Semaphore` | `adopt()` 及移动式 RAII，父 Device 必须存活 |
| Frame/Slot Fence | `gapi::vk::Fence` | `wait()/reset()` 返回 VkResult，调用者必须检查；载体由集合统一析构 |
| Buffer/Image CreateInfo | `BufferBuilder` / `ImageBuilder` | 只有公开 API 支持只生成 Vulkan Info，且 lifetime 正确时才用于 VMA；否则在 VMA 创建点局部构造 Vk*CreateInfo |
| PipelineLayout/DescriptorLayout/Sampler | 相应 Builder | 参数确实由 Builder 保存且 `Result` 正确返回时使用 |
| CommandBuffer 录制 | `gapi::vk::CommandBuffer` 已有 OOP 方法 | 已实现且确实正确的方法可以使用；未覆盖 Vulkan1.3 dynamic rendering/synchronization2 时直接在 Native 补充，不重造第二套普遍 Wrapper |

**关键限制：** 当前 `BufferBuilder::build()`、`ImageBuilder::build()` 直接调用 Vulkan 对象创建，而 Render V2 的 Buffer/Image 由 VMA (`vmaCreateBuffer/vmaCreateImage`) 创建。这些 Builder **不能不经设计就用于 VMA 对象创建**。可为 Builder 增加安全的 `createInfo()`/`view()` 访问，确保所有 pNext、queue-family 数组及生命周期在调用期有效；若单一局部 `VkBufferCreateInfo` 更清晰且无需重复 Builder，则直接使用原生值。不要为了形式上的 OOP 付出多重封装。

**错误返回是底线：** `gapi::vk::Object.hpp` 的 `VK_FUNC_INVOKE` 是旧诊断宏；一部分 `Buffer/Image/Sampler/ShaderModule/Pipeline` 构造与 `build()` 使用该宏，遇到可恢复错误无法向上返回正确 `RenderResult<T>`。**不得把当前 Native 的显式 `VkResult → RenderError` 路径改回“宏打印、assert 后继续”**。先在原 gapi 类型中提供/完善 `tryCreate`/`expected` 接口并复用，再删除 Render 的同义原生工厂。必要时保持一个局部原生调用，也比两套半成品封装强。

### A3.1 现有 gapi 需要的小范围修正（这是已发现事实，不是授权大重写）

- `PhysicalDevice::_properties.pNext = &_descriptor_index_properties`：当前类型具有隐式复制/移动途径，移动或复制这个保存着自身成员地址的结构，可能留下指向旧对象的悬空 `pNext`。应将 `pNext` 仅用于构造期查询并清空，或正确定义移动/复制后重新绑定；不得无审查直接嵌入 Session 后复制。
- `PhysicalDevice::findQueueFamilyIndexByFlags(flag)` 当前用 `(queueFlags & flag)`，表示**任一位匹配**而非“全部要求的位”，调用者传复合 Flags 前必须修正或限定用途。
- `PhysicalDevice` 的部分读接口当前非 `const`（如 `deviceName()`、`type()`），为了以 `const PhysicalDevice&` 暴露不可变查询，应小范围补 const。
- `CommandBuffer::bind(bind_point, layout, ...)` 目前忽略 `bind_point`，写死 `VK_PIPELINE_BIND_POINT_GRAPHICS`，不能用它直接录制 Compute Descriptor。
- 某些 `Buffer`、`Image`、`ShaderModule` 的 move-assignment 覆盖旧 Handle，因其不带 Device/Owner，不能自行销毁被覆盖对象。对这类非拥有载体：要么禁止 move-assignment，要么保证只在空对象上赋值；不因“优先用 gapi”把潜在泄漏带进新代码。
- `ImageBuilder`/`BufferBuilder` 的 `pNext` 可指向其自身子结构，复制/移动 Builder 时必须维护该指针；不要跨异步生命周期借用 Builder 的局部地址。

以上修正应当是 gapi 模块自身的**小型通用缺陷修正**。只有确认 GAPI 现有消费者和 ABI/安装影响之后才能改；若全仓联动过大，本轮可以先保留正确的 Native 局部创建路径，提出单独的小范围 PR，而不是再写一个 Render 专属 DeviceCapabilities/BufferBuilder 作为替代。

## A4. 向 Session 集中硬件事实，但不要把 Session 变成万能设备管理器

建议以**单份物理缓存 + 单份逻辑设备 Owner**组织 DeviceSession，选择其中一种实际可迁移方案：

**方案 A（推荐最终结构）：** `DeviceSession` 唯一持有 `gapi::vk::PhysicalDevice`（只读物理属性快照）与 `gapi::vk::LogicalDevice`（真正 VkDevice RAII），并持有已启用能力值、Queue、Allocator、必要 Upload 组件。`VulkanDevice` 的重复物理/逻辑 Owner 被移除。若现有 `InstanceContext` 不能直接提供 PhysicalDevice 实例，须先完成该构造入口的最小适配。

**方案 B（过渡最小修改）：** `VulkanDevice` 唯一持有一份 gapi::PhysicalDevice 缓存并返回 `const&`；`DeviceSession::physicalDevice()` 只转发，不保存第二份缓存。待全调用链完成后，再评估是否将 `VulkanDevice` 与 Session 合并。**禁止同时在 VulkanDevice、DeviceSession、每个 Storage 都各保存一份物理属性。**

示意：

```cpp
class DeviceSession {
public:
    const gapi::vk::PhysicalDevice& physicalDevice() const noexcept;
    VkDevice nativeDevice() const noexcept;
    bool supportsTextureDescriptorArray() const noexcept; // 必须以“已启用能力”为依据
};

// Storage/Handler 使用现有借用，不缓存第二份 VkDevice/Properties。
auto maxDescriptors = session.physicalDevice()
    .descriptorIndexingProperties().maxDescriptorSetUpdateAfterBindSampledImages;
auto maxImageSize = session.physicalDevice().properties().properties.limits.maxImageDimension2D;
```

上例中的两个调用都读取已缓存物理属性。`nativeDevice()` 返回轻量借用 VkDevice，不是第二个逻辑设备 Owner。

**不得由此恢复万能 DeviceContext：** Session 提供设备事实、真实选择的 Queue/Allocator 及其生命周期；Command、Feature、Material、GraphBuilder 不能利用 Session 访问任意业务目录。GraphCompiler 若只需 Device Capabilities 和 Native 创建入口，参数应收窄至这些事实；避免“方便”而将 DeviceSession 当作查询所有资源的全局 ServiceLocator。

## A5. 本附录的检验门槛

1. 不额外引入 Native 第二设备 Owner、第二物理能力缓存或新的 Render 专属 `PhysicalDeviceInfo`/`DeviceCapabilitiesManager`。
2. Native GPU 能力查询记录：初始化期静态查询只做必要次数；运行期热路径（Record/Submit、实例更新、Texture lookup）无 `vkGetPhysicalDevice*Properties*` 或重复设备发现。动态 WSI/MemoryBudget 例外有说明。
3. 旧 `VulkanDevice`/`InstanceContext` 和新的 gapi Owner 在迁移过程中可以短暂共存，但必须有唯一 VkInstance/VkDevice 销毁责任；最后清理同义路径，不永久双轨。
4. GAPI 参与的构建配置至少验证 Vulkan1.3、Validation/SyncValidation、显式失败返回、Descriptor Indexing、Graphic/Compute/Transfer Queue 选择、FIF=2/3、Source/Device 释放次序和外部安装消费者。
5. 仓库内 Render/test 继续只验最终渲染可观察成果。针对 gapi Header/Builder/独立 Native 机制的临时概念与故障试验使用仓外工作空间，保留原始证据；不要为了测覆盖率把 `*_SEAM` 散播到生产源码。
6. 每次改动统计移除的同义字段、Native 包装类型、重复初始化查询、原生资源清理分支和 CMake 依赖；若只是重命名又增加一套 wrapper，**架构验收失败**。

## A6. 原始源码锚点

- [`gapi/vk/PhysicalDevice.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/platform/gapi/include/lux/engine/gapi/vk/PhysicalDevice.hpp)
- [`gapi/vk/Object.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/platform/gapi/include/lux/engine/gapi/vk/Object.hpp)
- [`gapi/vk/LogicalDevice.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/platform/gapi/include/lux/engine/gapi/vk/LogicalDevice.hpp)
- [`gapi/vk/Instance.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/platform/gapi/include/lux/engine/gapi/vk/Instance.hpp)
- [`gapi/vk/CommandBuffer.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/platform/gapi/include/lux/engine/gapi/vk/CommandBuffer.hpp)
- [`gapi/vk/Buffer.hpp`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/platform/gapi/include/lux/engine/gapi/vk/Buffer.hpp)
- [`gapi/vk/ImageBuilder (Image.hpp)`](https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/platform/gapi/include/lux/engine/gapi/vk/Image.hpp)

**v1.1 生效解释：** 以前只写“参考 Legacy”可能使实施方再次复制 Legacy 综合类型，也可能继续创建新的 Native 同义包装。后续必须对照人工 gapi 的已存在 API 来裁决“是否真的需要这个新类型”，同时保持真正适合渲染的 Legacy GPU 数据结构和新的 C/S、Graph/Execution 分层。
> **v1.2 当前优先裁决（覆盖本文与此前实现相冲突之处）**
>
> 1. 渲染循环采用明确的 CPU 结构安全点，GPU 已提交资源只在 GPU 完成安全点物理释放。阶段由函数和顺序表达，不维护新的每帧 Phase 状态机。
> 2. EffectId 定位的 RenderEffect 唯一拥有 CompiledGraph 与 ExecutionGroup。ExecutionGroup 唯一拥有 ExecutionContext；DeviceSession 不再拥有 Context 目录；Context 不长期保存 CompiledGraph 的 shared_ptr 或裸指针。
> 3. OffscreenTarget 保存实际可用的输出资源。生产模块完全删除 Readback 专用 Command、Sender、Operation、字段、Feature 开关；CPU 获取数据由通用 Image/Buffer Copy、Host 可访问 Buffer、同步与数据所有权构成。
> 4. 结构撤销时结束 CPU 引用、订阅和可能的新提交；GPU 在途 Native 对象按真实提交条件移交唯一 RetirementQueue。不能因为 Effect 撤销就等待整个 GPU。
> 5. 删除以生命周期焦虑为动机的补偿对象/状态；保留真正的 VkQueue Serial、跨队列同步、Frame-slot 使用水位、已接受部分提交后的实际安全清偿。
> 6. 以前文档中把 shared_ptr 的泛化使用、Readback 成功合同、或者新的双层 Owner/Lease 作为长期设计建议的内容，以本条和下列 v1.2 条款为准。

---

# 附录 B（v1.2 强制追加）：阶段化 RenderThread 与单一生命周期责任

## B0. 本附录的地位

这是在现有风格规范 v1.1 基础上的**新增设计决议**，不是已经通过 GPU 验收的生产实现。所有文件与类型引用均基于上述固定 Git SHA。新增约束以用户确认的设计终态为依据；具体迁移需按配套 v1.2 Audit 的消费者闭包实施。不得隐式重写 Legacy 719 个文件、用户修改或现有 C/S/IPC/stdexec 契约。

最重要的原则：**对象什么时候不再被 CPU 使用由 RenderThread 安全点决定；GPU 什么时候不再使用物理资源由已经存在的 SubmissionQueue 与 RetirementQueue 决定。** 两者是互补事实，不要因“阶段化”而在每个阶段插入 vkDeviceWaitIdle、也不要因此引入新的 GPU Owner 抽象。

## B1. RenderThread 六个逻辑阶段（不存储 ERenderPhase）

| 顺序 | 阶段职责 | 允许修改 | 不能修改 |
|---|---|---|---|
| 1 | observeGpuProgress：poll 完成进度、触发已经到达的 GPU 完成事件、收集退休项 | GPU 基础进度与已完成队列 | 当前活跃 Effect/Scene/Target 表 |
| 2 | collectCommands：从 IPC 取得有限请求、从唯一就绪队列取得 CPU Sender 结果 | 准备回复及拥有型结构变更请求/完成值 | Native Record/Submit 中途重入式修改 |
| 3 | applyChanges：创建/删除 Effect、Scene/View/Attachment，采用完成的编译/上传结果、Resize、失效旧 ID | 唯一业务目录及其索引，准备最终退休对象 | 新增未确立的 GPU 提交、遍历时释放借用目标 |
| 4 | prepareFrame：确定可执行 Attachment、同步专业数据、准备实际 GPU 绑定、帧槽和 Target 输出 | 本次录制使用的可变帧资源 | Effect 图结构、全局注册与 Handler 目录 |
| 5 | recordAndSubmit：按已准备的 Graph/Plan 录制提交 | 本次 CommandBuffer、SubmissionQueue 与真实 GPU 使用进度 | Effect/Scene/View 结构，CPU 绑定的插拔 |
| 6 | publishResults：完成本轮 Reply/背压处理、收集实际进展、决定是否空闲等待 | Reply、状态统计、下一轮唤醒 | “为了发送 Reply”而重建 Effect 或重复 GPU 提交 |

以上是**逻辑职责**；真实实现可合并相邻无副作用函数，不强制创建六个新类、六条队列或六个全量扫描。Async callback 发生在阶段 1/2 时，只发布**拥有型待采用结果**，真正修改 Scene/Effect/Storage 的代码留在阶段 3。阶段 5 不允许触发同步 CPU 回调直接删除当前 Graph。

RenderThread 仍保持全速运行，空闲时通过现有 IPC/完成通知等待，必要 GPU poll 是设备完成机制，**不增加 sleep_for 帧率节流**。预算只限制 Command 接纳和其他有限工作量，不应把稳定帧渲染人为降速。

### B1.1 两个边界不能互相冒充

- **CPU 安全点**：本轮录制已经结束；由 RenderThread 保证没有同步 Reader 正在使用被撤销对象；正在执行的 CPU 工作不得绕过最终采用阶段直接调用 Native。
- **GPU 安全点**：涉及的所有实际物理 Queue 的提交已达到所需序号；GPU 可能还在使用的 Pipeline、Image、Descriptor、CommandBuffer、Staging 不能释放。
- 已提交不等于已完成；结构修改不等于销毁资源；stdexec Scope join 不等于外部 Producer terminal 调用栈完全退栈。
- 在 WSI/Present、外部 CUDA/Interop、跨 Queue 同步中，物理提交完成条件必须覆盖真正使用资源的消费者；不可用“一次 Graphics Queue Serial”冒充全部完成。

## B2. 单一拥有关系：Effect / Graph / Execution

目标关系：

~~~text
RenderServer
  effects : SlotMap<unique_ptr<RenderEffect>>
    RenderEffect : CompiledGraph（唯一、稳定对象）
      ExecutionGroup (SceneId, ViewId, TargetId)
        ExecutionContext（唯一拥有的动态执行数据）
  targets : Target 对象（Offscreen Target 实际拥有 Image/Views）
DeviceSession : 设备、能力、物理Queue、Allocator、Registry
RetirementQueue : 仅接纳已经退出活跃目录的 Move-only GPU 对象
~~~

要求：

1. EffectId 对应一个稳定地址的 RenderEffect；如其内部 Graph/Pass 包含指针借用，在迁移前必须核对其地址稳定性。可以使用 SlotMap<unique_ptr<RenderEffect>> 或成熟的稳定地址容器；不为每个 View 构造第二份 Graph Owner。
2. RenderEffect 唯一拥有 CompiledGraph。ExecutionGroup 保存其 Scene/View/Target 关系，并唯一拥有 ExecutionContext。DeviceSession 不再维护并行的 contexts_ 对象拥有表，也不接受释放时通过 find_if(contexts_) 定位指针。
3. ExecutionContext 不长期保存 shared_ptr<const CompiledGraph>、CompiledGraph* 或指向内部 CompiledPass 的悬空可能指针。必须将本帧需要的数据准备成自有的资源绑定/Pass 索引；执行时通过 Executor::execute(graph, context, target) 显式借用 Graph。
4. Effect 的寿命并非“C++ EffectId 删掉就立刻析构”；效果进入非活跃状态后，整个 Move-only Effect 可被既有 RetirementQueue 暂存，直到真实物理 GPU 最后使用完成。
5. 同一个 Effect 可有多个 ExecutionGroup，完成条件是它们已经产生的实际提交的合并安全界限。保守的每物理 Queue lastSubmitted 水位可作为低频删除候选，但前提是**禁止之后任何操作继续提交使用已撤销的对象**。
6. Native 已编译 Graph 仍可以被多个执行实例共同**借用**；共享读取不等于共享所有权。无特殊证明时不得用 shared_ptr 来掩盖父子关系。

### B2.1 Effect、Scene/View/Target 生命周期交叉时的特别规则

**不能天真地把一个还借用 Scene/View StorageSet 的 ExecutionContext 连同 Effect 移到 GPU 退休队列，然后立即销毁 Scene/View。** 那仍会造成 C++ 悬空。

阶段 3 需要先结束可同步变动的 CPU 绑定与订阅；保留用于 GPU 完成前真正存活的 Native 对象，并保证其 RAII 销毁不会再访问被删除的 Scene/View 或 Feature 对象。可以让退役的 Effect 持有自己的独立 Native Binding 内容，但不能用新的 Lease、SceneOwner、DeviceOwner 对象掩盖错误的引用方向。

Target Resize 必须为新尺寸建立实际 GPU 输出，旧图实例/旧 Target Image 依照相同 GPU 完成界限安全退出。新输出采用发生在阶段 3/4；旧帧的 GPU 命令不会因采用新 Target 被“重定向”。

### B2.2 删除 Effect 的最小事务合同

~~~text
输入 EffectId，RenderThread 阶段 3：
  (a) 从活跃调度目录撤销，generation 失效，不再选择本 Effect 录制
  (b) 结束会提交旧 Effect 的 CPU continuation / 结构订阅
  (c) 停止并清偿 CPU 对 Scene/View/Storage 的短期借用
  (d) 把实际物理 GPU 提交的全部安全条件归并
  (e) 移动唯一拥有的 RenderEffect / Native 必需对象到已有 Retirement
GPU 阶段 1 后续迭代：
  (f) 完成条件满足，移动拥有者被析构，按 RAII 次序释放 Graph/Pipeline/Descriptor
~~~

**不能**为阶段 (e) 再增加 RetiredEffectManager / GraphLease / GraphOwner / Version / OwnerScheduler。若现有 RetirementQueue 的 T.device() 类型约束迫使 RenderEffect 增加一个只为符合接口的 device()，应先收窄 RetirementQueue 接受 Move-only 析构对象的设计，不给业务 Effect 增加假的硬件职责。

## B3. Readback 作为专用概念全部退出 Render 生产代码

最终禁止保留以下专用代码符号及同义新壳：

~~~text
ReadbackSender / ReadbackOperation / Readback Command
readBackOutput / installReadbackCommand
ExecutionContext::readback / readbackBytes / cancelReadbacks
readbacks_ / readers_ / READBACK_HELD / readback_
NativeResourcePlan::readback / Graph export => HOST_READ 自动推断
MeshFeatureConfig::readback 及以像素检查为由添加的固定 Copy Pass
~~~

Offscreen Target 是正式输出对象。CPU 需要数据时，只使用框架已有或必要的**通用** GPU Image/Buffer Copy、Host 可访问 Buffer、Map/Invalidate、Queue 完成证据和 IPC 拥有型字节交付。不要添加 OffscreenCaptureManager、ScreenshotSender、DownloadOperation 等重命名旧 Readback 链的新系统。

特别要求：

- Offscreen Image 通常不可直接被 CPU 解引用；从目标 Image 到 Host Buffer 的 Copy 和完成等待是 Vulkan 事实，不属于可被删除的 GPU 数据传输原语。
- 不要让一次 CPU 提取自动调用 ExecutionContext::execute()；渲染由明确的本帧执行阶段负责。
- 要采集某帧图像，通用 Copy 必须在该帧产出后、下一次覆盖同一输出前建立正确 GPU 顺序；源/目的资源一直活到 Copy 实际完成。不能再通过 readers_ 阻断整个后续渲染流程来保证安全。
- Graph 的普通 Export 是 GPU 数据流出口；GraphBuffer 被 Export 绝对不等于自动分配 Host READ Buffer。真正 Host 访问由通用 Transfer/Copy 合同表达。
- 仓内最终成果测试可以从 Offscreen Target 通过通用 Copy 得到像素证据；内部 Readback 单元实验迁入仓外验证空间，不得保留测试专用业务 API。

## B4. 资源生命周期与“补偿性结构”判断

下列模式出现时必须发起职责审查；不是一律删除具体的 GPU 事实：

| 当前模式 | 首先尝试什么 | 允许残留的实际事实 |
|---|---|---|
| shared_ptr<CompiledGraph> | Effect 唯一拥有 + ExecutionGroup 正确借用 + 退休 | GPU 完成水位 |
| weak_ptr<DescriptorArray> | DeviceSession 设备级唯一拥有，Storage/Context 借用 | Descriptor 槽位及使用安全 |
| shared_ptr<Physical> | 专业 Storage/Move-only GPU Backing 独占，延迟退休归 Native | GPU 真正使用期 |
| StorageLease + preloads + closing_ | Scene/View/Attachment StorageSet 的真实父子寿命及结构撤销阶段 | 公开预加载若有实际独立使用合同 |
| AllocationPermit 无析构释放 | Native Allocation RAII 或资源拥有集合级 RAII | Alias 内存依赖和在途 GPU 使用 |
| RetirementReservation 每次预订 | 按已接受 GPU 提交/真正需退休容量统一管理 | 无异常模式下提交后退休容量可靠性 |
| GpuWork+Candidate+Ready callback | stdexec CPU 准备、Storage 专业创建、Queue 提交、阶段 3 发布 | 部分提交/源字节/Staging 寿命 |
| Transfer::EPhase + started_ + initialized_ | 执行链体现阶段与唯一真实位置/剩余范围 | 多 Chunk 偏移、GPU 等待条件 |
| PreparedStorageBinding.updated + Use 订阅 | 阶段 4 重新构建受影响绑定，预解析并使用 Dirty 集合 | 明确的实际动态 Binding |
| CompletionPost+CpuResume + CommandCompletion 混合字段 | 唯一 RenderThread 调度接缝和单一就绪队列 | 外部 Producer 退栈保护 |
| started/running/stop_requested | 分析不同线程观察者与单次启动合同 | 确有独立含义的线程安全状态 |

**不允许为了删除一个 bool 引入另一组相互依赖的 enum/optional/状态类。** 不允许依靠 std::shared_ptr::use_count() 或 StorageLease 的引用计数“猜测”GPU 已完成；Native 始终需要真实同步证据。

### B4.1 编译期与热路径的容器规则

- GraphPassId、GraphResourceId 不可每次用字符串在 vector 中查找；编译前一次映射成紧凑 ID。
- EffectId/ViewId/TargetId 由 SlotMap 定位；删除局部 View 只访问关联的 K 个 ExecutionGroup；不能扫所有 Effects/Groups。
- 发布/撤销的关系索引只能是权威 Attachment 表的定位辅助，不应各自保存可独立修改的第二份 Scene/View/Effect 真实关系。
- GPU 已完成批次按实际完成 K 个处理，不要用每帧全 ResourceRegistry 扫描。
- 允许对物理 Queue 角色的最多 3 项线性检查；反对为此新增 hash 或 unordered_map。

## B5. 渲染循环对 stdexec 和 GPU 完成的分工

- stdexec 负责 CPU Scheduler、OperationState 的正常生命周期和异步组合；RenderThread 只消费唯一就绪队列。
- 上传新业务合同：CPU/必要 GPU 提交已接受、依赖建立、句柄发布后即可成功 Reply；不要求先 wait GPU 全部完成。Staging/物理目标安全释放独立留给现有 GPU 完成进度。
- 取消只能阻止未来 CPU/Chunk，不能撤销 Vulkan 已接受 Submit。接受后发生无法判定后置条件的操作必须保留原有 outcome_unknown 语义。
- Queue completion callback 只通知实际完成并交付拥有型结果；不得在 Callback 的中途重入修改正在录制的 Effect/Storage。发布/采用统一在阶段 3。
- 关闭阶段：停止准入，close/request_stop Scope，RenderThread 继续泵 CPU 完成和 GPU completion；等待所有外部 Producer 尾栈退出；仅一次最终 GPU 清偿；业务资源在 Device/Allocator 前析构。**不能依靠正在被阻塞的 RenderThread 自己消费等待的事件**。
- RenderThread 不通过人为 sleep 控制帧速，不因“现在还有异步上传在执行”降低稳定帧录制频率。

## B6. 测试与删除纪律

- 仓内 Render/test 只保留基于真实 Client/Command、真实 Graph、Offscreen/Swapchain 最终输出和 GPU 可观察结果的高价值端到端测试；测试框架的可行性、同步原语、Fault Injection 和微基准实验放仓外。
- 已有异常、DeviceLost、Alias、GPU Fence、PartialSubmit、Source Payload 和 DLL tail 风险不能因为删除测试而被认定不存在。删除/搬迁旧测试前收集能支持新结构的真实效果级验证。
- 禁止新 TEST_SEAM、ASYNC_COST_SEAM 或以测试为目的改变生产 API、Graph 输出合同、Storage ABI。
- 提交必须报告：旧文件实际删除清单、减少的状态源、还有哪些真实 GPU 不变量、稳定帧零堆分配资格、离线机制证据以及 Debug/ASan/Validation/SynchronizationValidation。
- 残留机制如果有清晰职责可以 KEEP。**已通过独立 GPU 正确性的旧实现允许作为阶段性比较基线，但不得以“已有测试”阻止删除补偿结构。**

## B7. 本次决议的源码依据（固定 SHA）

- Runtime Event Loop: https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/src/ServerLoop.cpp
- Effect/Group Ownership: https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/runtime/pinclude/ServerImpl.hpp
- Context internal code pointers: https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/ExecutionContext.hpp
- Context GPU retirement: https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/Execution.cpp
- Special Readback: https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/Readback.cpp
- Compiler implicit Host Export: https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/Compile.cpp
- GPU Retirement: https://github.com/LUX-YU/lux-engine/blob/71fbe803620aba25ab34d79ca446325058d30df9/modules/function/render/native/vulkan/src/retirement/Retirement.cpp

**v1.2 最终原则：RenderEffect 唯一拥有 Graph 和执行绑定；RenderThread 安全点约束 CPU 结构变动，Queue/Retirement 约束 GPU 物理寿命；Readback 不再是生产架构中的专用名词。**
