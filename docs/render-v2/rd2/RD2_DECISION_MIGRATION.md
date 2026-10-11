# RD2-D0-C1 决策迁移与实现边界

本表是候选设计对账，不改历史验收。用户批准 RD2 后才激活 REPLACE/CLARIFY/EXTEND；D0 不授权代码迁移。全部旧决定来自只读 FINAL 第 19 章，完整原句与修订句如下。

| ID | 原 FINAL 决定 | RD2 类别 | 修订后的责任 |
|---|---|---|---|
| D01 | 恰好一个 `RenderRuntime`、一个 `VulkanBackend` 和一个 FrameExecutor/Driver；N Scene/N View/N Targets | KEEP | 唯一 Runtime、VulkanBackend、FrameExecutor、多 Scene/View/Target |
| D02 | `RenderView` 是 Camera/Output/History/可选 RenderPath owner，**非**默认 GraphInstance owner | CLARIFY | RenderView 拥有 ViewScope DataSets/Camera/History，但不默认拥有完整 GraphInstance |
| D03 | 相容 View 共享 immutable logical/native plans；Native key 包含 Shader/PSO/Target/Device 实际身份 | KEEP | 共享 immutable plan 的真实 Shader/Device/Target/Storage ABI 身份必须等价 |
| D04 | Scene-global pass 仅在 input/scope/epoch/side-effects 可证明相同的 invocation 合并结果 | CLARIFY | Scene pass 可共享须证明 DataSet snapshot/Revision/Scope/Side Effect 相同 |
| D05 | Vulkan Multiview 是可选 View-level native optimization，合法设备/attachment/shader/data 下使用，完整 separate fallback | KEEP | Multiview 只是可选物理优化、separate fallback |
| D06 | `RenderDataTypeId`、`SceneCapabilityId`、`GraphResourceId` 三个独立域，不能同一 requires/ID | KEEP+EXTEND | RenderDataType/SceneCapability/GraphResource 三独立域，另外 DataSetId 为 Scope/Runtime-local 实例身份、RecordHandle 受 DataSet 约束；既有 renderDataTypeId(canonical_name) 与版本字段不改 |
| D07 | 一 Scene 一个 `RenderSystem`，内部 typed Projection；CPU source 数据不能被 Backend 直接读取 | KEEP | 每 Scene 单 RenderSystem、typed Projections、O(changed) 发布 |
| D08 | 三 Lane：PROGRAM/CONTROL owner-thread SPSC、UPLOAD bounded MPMC，跨 Lane 无总序，使用 ready/receipt causality | KEEP | PROGRAM/CONTROL SPSC + UPLOAD MPMC、有界因果、无第四事件系统 |
| D09 | R2-FIX reply 采用已验收 pre-reserved cell + consume/abandon + writer reclamation | KEEP | R2 回复 consume/abandon/admission 语义保持 |
| D10 | 共享 `RenderResourceDomain` 默认每 Runtime integration 一份，是 CPU semantic asset ledger，不是 native GPU owner | KEEP | RenderResourceDomain 只持 CPU asset semantic ledger，不成为 native VMA owner；真正 GPU 数据由 Scope-owned Dataset+Native backing 管理 |
| D11 | Concrete Feature + C++20 Concepts optional hooks + generated fixed ops，FeatureInstance 单一 membership | KEEP | 具体 Feature + Concepts/Generated Ops，无 Feature 虚基类或 FeatureManager |
| D12 | Provider 能没有 Pass；Capability 每 Scene 一个 active provider 默认；互相依赖 cold attach resolve/teardown | CLARIFY | capability provider 默认唯一；**DataSet 可多个相同 T，但不同 key/scope**，不从类型随便选第一份 |
| D13 | `RenderGraphBuilder` 是主 Authoring API；Definition validated owning value；Logic 编译独立函数；Native plan RAII owner | KEEP | GraphBuilder/immutable Definition/free compiler/native RAII，DataSet/Feature 解析在 Composition；不改原 C0–C8 职责或 Graph 算法 |
| D14 | 使用现有 `LUX_PASS_PARAMS` / `LUX_RESOURCE` + lux-cxx meta generator + `.lglsl` emitter，并扩展语义到所有资源 | EXTEND | 继续仅用既有 generator/`LUX_PASS_PARAMS`；同一 meta 增加 `DataSchema<T>`（非第二 parser） |
| D15 | 完整类型化 Shader contract + reflection reconciliation + LayoutPlan 唯一最终 descriptor 位置与完整 shape | KEEP+EXTEND | 自动完整 Shader/Layout/ABI，增加可选 GPU Layout/Representation↔PassSchema 对账，CPU-only/Wire-only 分别验证 |
| D16 | Shared Set Shape 由真实 Engine/Feature owner 的完整契约决定；pass-private 才按 schema/reflection 生成 | **REPLACE** | Shared Descriptor Shape 权威从“Feature 身份”修正为 **DataSchema/Storage full shape + 结构性 BindingSlotKey**；具体 DataSetId 仅在 invocation 映射；Feature 只借数据；物理 set 由唯一 LayoutPlan 规划 |
| D17 | Cold SPIR-V descriptor relocation 与 schema/reflect/binary/layout checksum 校验；Pipeline candidate transaction | KEEP | SPIR-V relocation、cold last-good candidate、真实验证与缓存 |
| D18 | Logical Graph 支持 texture aspect/mip/layer 与 buffer byte ranges/versioned RAW/WAW/WAR | KEEP | 子资源/Buffer byte range 版本化 RAW/WAR/WAW |
| D19 | Pass scope Scene/View/Target，Culling/condition/side-effect 必须有证明和 fallback | KEEP+EXTEND | conditional/cull/side effect/scope 有合法 fallback；新增 DataSet Scope/Ready 契约 |
| D20 | Stable topology compiled plan 持久复用；camera/frame/render_time/offset 只改变 Bindings | KEEP+EXTEND | stable topology native key 还包含合法 Storage ABI/owner keys，不因 Camera/current data revision 无故重编译 |
| D21 | Graph native compilation C9–C13 包含 Shader/Pipeline/Descriptor/Barrier/Queue/Physical/Record recipe | KEEP | 唯一 Native Compiler C9–C13；C11=Native pass execution、C12=Queue/sync，保持 FINAL 分工，不冒称产品已可用 |
| D22 | Transient aliasing 只在 queue/GPU overlap proof 成立时使用；真实 noalias fallback + GPU oracle | KEEP | 只有真实 native queue/memory overlap proof 才 alias，真实 noalias |
| D23 | Temporal history 由具体 View/Feature 持有；Graph 只表达 current/previous/import 的读写 | **CLARIFY** | History 有效期归真实 ViewScope DataSet；Feature 负责产生/消费与 invalidation，**Feature 本体不默认拥有 History GPU backing**；禁 UniversalHistoryManager |
| D24 | R4 native Foundation RAII owner 保留；扩展它的 Graphics/Descriptor/Multi-queue 能力，不建第二套 VMA/Device | KEEP | 复用 R4 RAII/VMA/Queue/Retirement，不建立第二层 Foundation |
| D25 | Multi-queue 真 semaphore/ownership/sync2；不支持按合法 single queue fallback，真实 GPU 验证 | KEEP | 真实 Sync2、不同 Queue wait/ownership，single queue fallback |
| D26 | 公共插件接口为 typed Feature/Graph/Shader/capability + versioned C ABI/code pin，插件独立安装消费者验证 | EXTEND | Plugin SDK 额外公开 DataSchema/Typed DataSet views 及按用途可选的 Wire/GPU 合同（host-owned Type descriptors），继续 versioned C ABI/code pin |
| D27 | C++20 values/free functions/short Builders/RAII owners/Concepts 静态约束，既有 cxx::expected | KEEP+EXTEND | C++20 Concepts/RAII/value/free algorithms；存储算法允许充分使用模板、但不发明第二 ECS |
| D28 | 正常 failure 返回结构化 `RenderResult`，未满足 runtime invariant fail-fast，native GPU 严格 fence lifetime | KEEP | RenderResult、fail-fast 内部 invariants、GPU lifetime |
| D29 | Fixed warm stable frame 0 first-party C++ heap、无 lazy PSO/layout、zero recompile；cold growth 分开计量 | KEEP | 固定 hot 0 first-party heap/lazy native create、cold growth 单列 |
| D30 | 719 Legacy frozen，F01–F36/I01–I30 分条真实功能 parity，H01–H06 独立算法，不能删任意旧变体 | KEEP | frozen 719、66 旧能力、86 综合 ID、H1–H6；不可删任何业务路径 |
| D31 | F0–F11/H1–H6 独立 I/V 阶段和 STOP；旧 R0–R4 验收/NOT_RUN 不回溯修改，F11 真实产品/SDK cutover | KEEP+EXTEND | I/V/STOP 保留；新增 RD2 D0 docs-only 与 F4-C closeout 门禁，不可跨阶段自动推进 |
| D32 | Runtime 独立进度；无 PROGRAM 仍能 paced frame，GPU completion/callback/diagnostics与 FrameSerial 分开 | KEEP | 无 PROGRAM 也独立 paced frame，GPU/Frame serial 不混用 |

## 关键变更的源码事实和未来责任

| 决定 | 基线源码/历史事实 | RD2 责任 | 未来阶段 |
|---|---|---|---|
| D06/A | core/Identity.hpp 的 renderDataTypeId(name)；Descriptors.hpp 版本独立 | 保留 Core；DataSet 是作用域实例，RecordHandle 限定集合 | F6/F7 |
| D10 | FINAL CPU ResourceDomain ledger | CPU retain 与 Backend RAII 分开；不是新 VMA authority | F7 |
| D13/B/C | graph/Plan.hpp 的自由 compileLogicalGraph；FINAL C0–C8 | 业务解析在 Composition；不改 Graph 调度器 | F6 接入/F7 数据 |
| D14/D15/D16 | shader/Layout.hpp OwnerAssignment 只有 scene/feature/pass_local；单源生成已有 | D14/15 扩展可选数据合同，D16 替换 full-shape authority；真实 multi-owner 尚未实现 | F6 |
| D21/B | vulkan/graph/Executable.hpp；FINAL C9–C13 | 继续 C11 配方/C12 同步；Native 仍 PARTIAL | F4-C |
| D23 | FINAL View/Feature history；当前无真实 Runtime/View owner | ViewScope DataSet 管有效期、Backend 管 GPU backing；Feature 提供算法/invalidation | F5/F7，业务 F9/H3 |
| D26 | FINAL typed SDK+C ABI/code pin；尚无完整 installed Feature SDK | Host-owned Schema +受限 Dataset view；CPU/Wire/GPU 分层 | F6 |

## C0–C13 与新增 N 决策

以下 C 表逐行继承用户裁决与原 FINAL，详细输入输出在 RD2 第 07 章；不从文本重命名推导新生产阶段。

| 阶段 | 输入/输出/证明 |
|---|---|
| C0 Canonicalize | Graph 作者声明 → owning IDs、typed resources/pass、normalized ranges |
| C1 Resolve | Graph 内 required/optional producer、semantic provider、import/export、fallback → 确切图资源；不查 DataSet Registry |
| C2 Validate Interfaces | PassSchema 与 Graph/Shader 资源访问接口契约一致；业务 authority 在 Composition 先验证 |
| C3 Build Versioned Hazard Index | exact buffer/image ranges、显式 producer/version/order → RAW/WAR/WAW 与初始化证明 |
| C4 Topological Schedule | 确定性拓扑；歧义/循环包含 pass/resource/path；不靠地址或注册顺序 |
| C5 Root/SideEffect Reachability | 从真实 present/readback/export/external effects 保留可达有用 Pass |
| C6 Lifetime/Usage | 子资源/版本逻辑 first/last use、有效 usage、alias 候选；不证明 GPU completion |
| C7 Scope & View Group Proof | Scene/View/Target 输入、revision/epoch/time/side effect 的合法共享证明 |
| C8 Emit LogicalPlan | 唯一中立、拥有型、不可变逻辑结果及诊断/完整缓存比较事实 |
| 阶段 | 输入/输出/证明 |
|---|---|
| C9 Layout/Shader | 完整结构性 owner shape、PassSchema、Shader 与 DeviceCaps → LayoutPlan、重定位且验证的 SPIR-V |
| C10 Physical resources | logical usage/lifetime 与预冻结 StorageInterfaceShape → 相容 native backing/placement、import/export/alias 物理约束；不晚改 Shader ABI |
| C11 Native pass execution | Shader variant 与 native pass contract → 完整 PSO、不可变录制/绑定配方；无 lazy creation |
| C12 Queue/sync | exact hazards、stage/access、queue/native overlap 与 external readiness → BarrierProgram、ownership/wait/signal 与提交调度 |
| C13 Executable assembly | 完整 C9–C12 候选 → ExecutableGraphPlan、native owners/code pins/bounded invocation 状态与退休合同 |

新增 N01–N17（候选）逐行对账：

| ID | 决策 | 拒绝 |
|---|---|---|
| N01 | Data First：Data Type Schema / Scope DataSet / Native GPU backing / Feature 四者分离 | Feature 默认 private owns GPU data；每 Feature 一个 Registry |
| N02 | CPU 数据索引用 C++20 `TypedStorage<T>`（CPU-only 仅按真实生命周期策略约束，不要求 GPU ABI），强类型 Handle+Generation/Sparse membership/Dense active+dirty | 引入 Universal ECS Entity、全局 `void*` store、多余虚继承 |
| N03 | Composition 先冻结中立 StorageInterfaceShape；原 C10 根据实际 Pass Access+Caps 决定相容 backing，不晚改 ABI，非法形状明确拒绝 | 无事实依据凭 `sizeof(T)` 算最优 GPU layout；每帧重规划 |
| N04 | 同 T 可以多个 Dataset；身份为 Scope+Type+Key+Generation | 一类 Type 唯一 Runtime 单例或按注册次序 first-match |
| N05 | CPU authoritative、GPU authoritative、external authoritative 各有合法不同 API | GPU-only Buffer 冒充可 CPU `.get`；CPU 每帧覆盖 GPU 结果 |
| N06 | Stable Record Handle 不是 native slot/address/dense index；物理 reuse 必须 GPU completion | CPU 删除就释放在途 GPU 字节；Slot reuse 不更 generation |
| N07 | DataSet Reader/Writer/Graph Views 类型强约束、CPU Write 只在 backend owner 合法线程 | 全局跨线程裸 `T&`、每帧 runtime type registry find |
| N08 | Feature Composition 先解析 Type/Key/Scope/Access/Capability，降低成 typed Graph 声明；原 C1 只解析图内关系，原 C2 只验证 Pass 接口，版本算法从真实访问建立依赖 | Feature requires 自动生成假的 GPU pass/barrier 或当作安装顺序 |
| N09 | DataSchema/Storage 的 Full Shape 是 Shader 的结构性 BindingSlot Owner；Runtime DataSetId 在 Invocation 映射，LayoutPlan 唯一最终 set/binding | `OwnerAssignment{scene,feature,pass}` 冒充多个真实 Owner，或把 DataSetId 纳入 PipelineLayout key 破坏多 View 共享 |
| N10 | CPU Schema、可选 Wire 编码和可选 Shader GPU representation 由同一现有 generator 生成；GPU 布局另验证 fingerprint，不改 Core descriptor 含义 | 重复手写 GLSL data struct/Descriptor set offset |
| N11 | GPU 计算按 representation/pipeline 专用 Workset 与 Kernel；无 Universal GeometryKind 热分支 | 每线程查询 DataTypeId/GeometryKind/map 才能读记录 |
| N12 | 可选择 stable slot/packed/page/chunk/SoA GPU 布局，真实 GPU ABI 和性能可验证 | 所有 Data 强制 GPU mirror EnTT packed pages、不可控 Shader divergence |
| N13 | Feature 删除只撤销算法/访问，不默认删除共享 DataSet；Scope 才决定逻辑数据 lifetime | 装卸一个 Feature 即移动/释放所有 Consumer 共享 Buffer |
| N14 | 外部插件先复用已有数据 Schema，否则注册新 T；Host 深拷贝 ABI 描述/校验版本 | 重复 PluginLight/Mesh schema、跨 DLL 不安全 C++ RTTI/STL |
| N15 | Native Primitive 扩展只改已授权 Vulkan/Shader/Graph access 能力层 | 修改 Runtime/Scene 或建立第二 Renderer 来支持 RT/Mesh Shader |
| N16 | 每个新类型须证明作者 API 简化与 Storage/GPU 性能、ABI/错误/GPU retirement | 新模板抽象仅凭“泛型/少代码”就合入 |
| N17 | F4 PARTIAL 时首先 docs-only RD2 门禁，按 Gate 授权 F4 closeout；禁止先修改 F5/F6/F7 代码 | 以架构修订为由大规模同时改多阶段生产源码 |
