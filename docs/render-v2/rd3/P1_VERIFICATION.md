# RD3-P1 独立验收 — PASS（修订指令的最小架构主线）

日期：2026-10-12。**本轮 P1 必需门禁全部通过。** 此结论不覆盖完整 P2/P3、P4/P5、产品替换或高级渲染。
D0、F4/F5/F6.b 的历史 PARTIAL 与原始证据保持不变。

## Git 身份、隔离与提交边界

- 施工前及推送前查询到的远端 HEAD / 施工基线：`38262ecd3e962690703173397f49796cfd81292e`。
- **Implementation I：`db9cec6d1268a4b2847c1723a702b5bad8505ccf`**，parent 为上述 D0 V。
- I 修改 45 个文件，2328 行增加、2854 行删除。精确清单见 [P1_CHANGESET.txt](P1_CHANGESET.txt)。
- Verification V 是 I 的直接子提交，**只增加本报告**；提交后 SHA 记录在外部 delivery.json 与交付消息中。
- 施工使用独立 worktree `C:/Users/chenh/.codex/worktrees/render-rd3-d0/lux-engine`，本地分支 codex/rd3-p1。
- 验收源目录 `E:/RD3P1/verify/source`，构建目录 `E:/RD3P1/verify/build`；二者均与施工源/构建树分离。

先在 clean tracked I 上执行 ValidateTrackedSnapshot.cmake，再以 `git clone --no-local --no-checkout`
建立独立仓库，禁用 autocrlf、启用 longpaths，detached checkout 精确 I。浅克隆未携带 Legacy 冻结
对象，补取 `a669409a289a6fa4092f21176397795b1cdb7f3e`（depth=1）后校验；没有修改验证源文件。
git dir 与 common dir 均属于该 clone，无 alternates，所有 Git object 文件链接数为 1。
验收前后全部工作文件字节与 I 的 Git Blob 一致，status clean；验证中没有实施修复。
独立性是仓库、对象存储、构建目录与执行进程隔离，由同一执行代理完成，不声称另一个人进行了审查。

ALLOWED：runtime 对象/协议组织与测试，Native Graph 的实际职责迁移和直接测试调用，bootstrap 注册，P1 文档/检查器。
READ-ONLY：R2、通用 Graph、Shader ABI/Pipeline/同步/Queue 算法，RD2/D0 与历史资格，Legacy 冻结文件、依赖和用户工作区。
FORBIDDEN：更改历史判定、合并 main、自动进入下一阶段。I 中仅删除被替换的 RuntimeBackend.hpp 与 Frame.cpp，未删除旧测试。

## 必需门禁结果

| 门禁 | 独立 I 上的实际结果 |
|---|---|
| clean snapshot / 来源与边界 | 前后均 **5946/5946 PASS**；无失败项 |
| C++20 全量构建 | MSVC 19.44.35226，RelWithDebInfo，Ninja `target all -j 4 -- -k 0`，500 个步骤全部成功 |
| 第二轮构建 | `ninja: no work to do` |
| 公共头独立编译 | Objects、Types、Runtime、Backend、NativeAssembly、RuntimeMessages、Native graph/Executable 全部通过；前四者禁止传递包含 Vulkan |
| CTest | **186/186 PASS，0 failed，124.08 秒**；串行，未与构建并发 |
| ID / Generation | 六种 Tag 的 SlotMap 分配、删除、clear、最大 generation 退休；实际 Scene/View/Target/Effect/Graph 重建旧 ID 失效 |
| ViewRef / Runtime owner | 不同 Scene 的局部 ViewId 可相同、完整 ViewRef 不同；错误 Scene generation 与另一个真实 Runtime 的 owner 均被拒绝 |
| SparseSet / GraphGroup | GraphId 删除/复用代际后的查找、移除、一致性通过；Server group 与有序 jobs 随拓扑维护 |
| Offscreen GPU / 共享 | **两 Scene、四 View、同 Effect、同 GraphId**，FIF=2 与 3 均通过；四个不同 exposure 的 RGBA CPU oracle 通过，Target 最终输出 readback 通过 |
| Shader / PSO / Graph 唯一性 | Native compile counter=1、cache hit=1；fault seam 断言重复 Effect 命中时 Pipeline 创建计数不增加 |
| 可变状态隔离 | 各 View 独立 scalar/slot/receipt；不同默认值 Effect 复用同 Graph 后仍输出各自值；删 View/Effect 后剩余 Scene 输出正确 |
| 热态 | 每种 FIF 下连续 12 个稳定帧的 C++ allocation count/bytes 均为 0；同 Scene Pass 实际复用 |
| Stop / 晚析构 | Client 在 Server 后仍可读停止结果；已发布 Reply 正常完成或正确返回 Stopping；Request 在 Client 与 Server 都销毁后仍可 abandon |
| 错误回滚 / GPU in-flight | Win32 event/thread 失败回滚及重试；Native image 分配失败后同 Scene/Target 创建成功；第一笔 Queue Submit 接受、第二笔失败后安全退出 |
| Native 退休 | fault seam 的 18 类 Native owner 计数最终全部为 0；RD3 两项 GPU 测试 validation_errors=0 |
| 算法保护 | Native Compile/Record/Export/Diagnostics 四个 TU 在角色改名与 submit 自由函数提取归一化后，与基线逐字相同 |

现有 Native multi_owner 测试另行输出 `scenes=2 views=4 FIF=3 pipelines=1 shared_code=1 slots=12 oracle=PASS warm_allocations=0`。
这与新的 Runtime 端到端测试互相补充，不能用底层结果替代 Client/Server 端到端结果。

## 所有权与模块依赖结论

| 身份 | 自动分配的唯一容器 / Owner |
|---|---|
| RenderSceneId | Backend → SlotMap<Scene, RenderSceneTag> |
| RenderViewHandle | Scene → SlotMap<View, RenderViewTag>；跨 Scene 地址为 ViewRef |
| RenderTargetId | Backend → SlotMap<Target, RenderTargetTag> |
| RenderEffectId | Backend → SlotMap<RenderEffect, RenderEffectTag> |
| RenderGraphId | Backend → SlotMap<CompiledGraphEntry, RenderGraphTag> |
| ResourceDomainId | Backend → SlotMap<ResourceDomain, ResourceDomainTag> |

以上均为 lux-cxx SlotKey。跨协议还携带既有 R2 ownerIdentity；没有新 ID 分配器、第二个 View Owner
或 Hash 身份。GraphViewGroup 与 GraphExecution 池使用 SlotKeySparseSet 借用 GraphId，**不分配 ID**。
有序 ExecutionJob 列表只含非拥有 ViewRef/GraphId 与池偏移，不把 Graph 分组当作重排授权。
新增及实际迁移的对象索引完全避免 unordered_map/unordered_set。

RenderRuntime 是协议 Client，Runtime.cpp 不包含 Native；Backend 持有唯一对象与执行权威。
Scene/View/Target/Domain/Effect 值类型中立，Backend 冷装配入口仅前置声明 Vulkan 类型，NativeAssembly
是显式 Native 编译输入边界。Graph 共享不可变 code；独立 GraphExecutionContext 持有 GPU backing/
descriptor/slot/ticket。SubmissionQueue 与 Native backing 地址稳定，P1 没有为每 View 复制 Graph/PSO。

Graph Cache 比较完整结构、Shader/SPIR-V/schema、绑定 OwnerShape/assignment、Pipeline 与 extent，
命中发生在 Native Shader/Pipeline 创建之前；GraphId 由 SlotMap 单独分配。
Effect 的默认 invocation 值与共享 Graph 分离，避免结构相同但默认值不同的 Effect 串扰。
Scene/View 拓扑状态没有第二份权威副本；执行池中的 ViewRef 与发布数据仅承担执行职责。
没有新增 Manager、EventBus、FrameScheduler 或通用 CompilerPassManager。

## 历史、Manifest 与用户保护

- Legacy Manifest 的 **719/719** 文件：冻结提交的 source 与现有 render_legacy destination 的 mode/blob/size 全部一致。
- 基线中的全部 docs 与 render_legacy Git Blob 保持原值，包含旧 RD2、D0、性能原始报告和 PARTIAL 判定。
- D0_MANIFEST.sha256 与输入 MANIFEST.sha256 的每项均重新通过；原 ZIP SHA-256
  `3ac411f5c6d000b1a7b7a3ffd0e8e3e0b0aeb45936a91433065f9cbc7ead88af`，ZIP CRC 通过。
- D0 的 21 章、工作单、86 能力/25 Workloads/51 Requirements/60 类型以及迁移 CSV 均逐字保留；P1 不声称完成这些未来能力。
- 原工作区 HEAD 仍为 `246fe21eb6d060801b9a16ddcadde2be89dbbecc`，仅原有 `.vscode/settings.json` 修改；
  binary diff、该文件 SHA、D0 已记录的 7 个本地配置文件 SHA 全部不变。
- 同级 lux-cxx / toolset / imgui / node-editor 均在要求版本且 clean。公共头与生成的协议头共 8 项
  同步到 Debug、RelWithDebInfo、Android/lux-engine 三个 include 前缀（24 份），此前内容外部备份；未做 Android 构建资格。

## 实际问题、暂缓项与资格限制

开发过程中遇到并保留的失败证据：

1. SDK 1.3.280 的 SyncVal 不支持 Timeline Semaphore，产生跨提交竞态诊断。改用官方签名安装包的
   copy-only VVL 1.4.304，保留同步验证，未屏蔽诊断、未增加 wait-idle；先前 barrier 实验已撤回。
2. 新增晚析构测试一度连续提交第三个请求，超过原有有限 CONTROL 队列容量；测试改为两个已接受请求，
   其中一个在 Client/Server 均销毁后释放。没有放宽生产 Admission，也没有把正确背压改为成功。
3. 独立 clone 初次预检缺少浅历史外的 Legacy SHA，补取只读历史对象后通过；没有在验证源修复代码。

最终精确 I 的独立构建、CTest、前后完整性检查没有未通过门禁。
Windows Vulkan loader 仍报告系统 EOS overlay 的重复 manifest 提示；RD3 验证层 error 计数为零。

旧矩阵 200 项保留 184 项，新增 2 项；16 项依赖旧 Runtime 结构的测试保留源码而暂停注册，
逐项理由见 [P1_IMPLEMENTATION.md](P1_IMPLEMENTATION.md#验证与旧测试分类)。它们不算本轮通过，历史资格不变。
没有重做 F4/F5/F6.b 完整性能实验，没有使用旧 5% 微基准作为本轮门禁。

本轮为 Windows x64、固定 Feature 完整 Offscreen Graph、单 View Target、启动前冷态 Effect 装配。
仍待授权实施：Effect 热更新整体事务、完整 Feature/Plugin SDK、GPU Asset Registry/ECS 增量流、
WSI/resize/history Runtime 接入、多 Effect Target 完整组合、Engine/Editor 产品集成、Legacy 业务及 H1–H6。
ResourceDomain 当前只有中立层级和生成式身份，未宣称完整 P4 资源系统已经完成。
R01/R02/R04 在本轮有限路径中落实，R05 已验证；R03 的旧 CSV 阶段映射继续保留未来阶段语义。

## 环境与可复核证据

开发入口：`E:/RD3P1/dev.ps1 configure|build|test|install`；先点源其 shell 动作可进入 MSVC 环境。
独立验证使用 `E:/RD3P1/verify-dev.ps1`，编译 SDK 1.3.280，验证时
`VK_LAYER_PATH=E:/RD3P1/sdk-1.4.304/Bin`；CMake 3.31.10、Python 3.13、MSVC 19.44.35226。
宿主 GPU 清单记录 RTX 4070 Laptop / Intel Iris Xe，Native 默认选择合格独显。
完整日志与前后 checks.json/result.json 保存在 `E:/RD3P1/evidence` 和 `E:/RD3P1/verify/results`。

| 证据（路径相对 E:/RD3P1） | SHA-256 |
|---|---|
| evidence/verification-build.log | `4de86ae0de57d3151ad801856618070fb3abe2272adafe8c373d763b449450bb` |
| evidence/verification-ctest.log | `bc340913e71eeb4b1d664d22f47906d045fdef7b07a395d5fd0f8802aef1cd5a` |
| verify/results/postflight/result.json | `f50726300b62c29dac745e892ec592501218cdb1d8900af06b2ba445564d2178` |
| evidence/P1_EVIDENCE.sha256（16 项证据清单） | `e4f9c828474661432fe004a52ea8995c9cd6fecf97bec248cd8b8497439e2de1` |

本报告是 report-only V。推送 I/V 到 codex/render-v2 后停止，等待用户审查；不合并 main，不自动进入下一阶段。
