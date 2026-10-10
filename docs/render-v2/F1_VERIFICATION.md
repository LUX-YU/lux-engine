# F1 独立资格验收 — Typed Authoring Contract

结论：**F1 PASS**。仅针对本工作单的作者接口、同源 Schema/Shader 契约和独立依赖闭包。
全量 ASan 补充矩阵为 **PARTIAL**，原因与实际通过的 CPU 子集分列如下；不将其登记为全量通过。

- 批准基线：`00ba4075ada74f711fd08300b5f45f2f4ce2150f`。
- Implementation I：`78a1248d1f5e9c59eb8f88ddd4869244e512050f`。
- Frozen V1：`a669409a289a6fa4092f21176397795b1cdb7f3e`。
- Verification V：仅添加本报告的提交；不夹带生产修复。
- 独立 clone：`D:/LuxQualification/render-v2-f1-78a1248d1f5e/source`，使用 `clone --no-hardlinks --no-checkout` 后 detached checkout I。
- 资格构建：同级 `build`；补充 sanitizer 构建：同级 `asan`。源码树外输出，clone 始终 clean。
- 原始证据：`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F1/78a1248d1f5e9c59eb8f88ddd4869244e512050f/`。
- `evidence-manifest.json`：432 个文件；SHA-256 `7ae0003cba9c483cc53f282a7c394d4ce4ba8d118ac2400365e5ce47c7fdbfd8`。

## 实际实现与所有权

`RenderGraphBuilder` 提供 `texture`、`buffer`、`importTexture`、`importBuffer`、typed `addPass` 和
`finish() &&`。后者返回唯一的 owning `RenderGraphDefinition`。旧 vector factory 改为 private，原 R3
测试通过非安装的 `DefinitionAccess` 调用同一个验证器，不存在第二 Definition 或公共兼容适配层。

`GraphTexture` / `GraphBuffer` / `GraphSampler` 与 `PassKey` / `ShaderKey` 分离；资源 Origin、持久 Owner Scope、
ExecutionScope、PassKind 分别表达不同事实。Attachment/Resolve/DepthStencil、Transfer、Vertex/Index/Indirect
与 Shader descriptor 用途分离。范围包含 mip/layer/aspect 与 buffer 字节区间。

`addPass` 拷贝资源用途、字段身份、数组形状、owner/frequency/stage、sampler、attachment 操作、clear 值、
标量字节、标量 ABI 元数据和 Shader 声明；Definition 不保留 Params 或生成元数据的字符串借用。
标量、sampler 和 clear 初始值不参与逻辑拓扑相等性；这不意味着该相等性是未来 native plan 的完整缓存键。

正常字段使用既有 `LUX_PASS_PARAMS` / `LUX_RESOURCE`。既有 lux-cxx generator 解析 C++ 和 annotations，
既有 `lux_add_codegen_job` 生成 IR 投影，再由 `PassSchema.py` 进行确定的 IR 降低转换。
该脚本不读取或解析 C++ 源码、不解析 annotation 文本、不建立运行期 registry。Meta 模块及 lux-cxx 未修改。
生成的 `PassSchema<T>` 同时提供 Graph uses、typed 字段访问、ABI assertions、Shader 声明和布局元数据。

实际 glslc 输出经现有 `SpirvReflection.cpp` 和 SPIRV-Cross 核对 descriptor 名称/种类/数组/访问、image 形状/
格式/stage、scalar 类型/offset/覆盖、UBO 大小及 SSBO stride。生成器明确拒绝不支持的字段形态，
例如指针、bitfield、未定义角色、重复角色及未知 annotation key，不将不匹配布局视为成功。

旧通用 `emitCanonicalGlsl` 继续服务既有离线工具消费者；新的 typed `emitPassGlsl` 分支不查找旧固定
`LayoutContract` 词汇，也不允许作者指定物理 set/binding。F1 的临时编译槽由生成器决定，F3 才建立
最终 LayoutPlan、共享 owner shape、设备预算及 SPIR-V relocation，不能将临时槽当作 native ABI。

完整类型清单、语法、来源及阶段解释见 [F1_TYPE_RESPONSIBILITIES.md](F1_TYPE_RESPONSIBILITIES.md) 和
[F1_WORK_ORDER.md](F1_WORK_ORDER.md)。

## 验收结果

| 检查 | 结果 | 原始证据与边界 |
| --- | --- | --- |
| tracked snapshot | PASS | `tracked-worktree.log`、`tracked-clone.log`；锁定 I |
| 两轮完整构建 | PASS | `build-first.*`、`build.*`；`all -j 4 -- -k 0`；第二轮 `ninja: no work to do` |
| 全部 CTest | PASS 83/83 | `test.log`、`Testing/Temporary/LastTest.log`；21.02 秒，串行运行 |
| 既有测试保留 | PASS 59/59 | `test-mapping.json`；59 个原名完整保留，新增 24 项 |
| R3 语义 oracle | PASS | 原 300 张随机图、10k bindings/reuse、hazards/排序/生命周期测试保留；仅 factory 入口和新增错误数量迁移 |
| 真实 Shader fixtures | PASS 7/7 | Tonemap、Blur、Composite、Storage、HZB、Complex、LocalRead；实际生成、glslc、逐字段 SPIR-V 对账 |
| 完整生成物 | PASS | 11 组生成 Schema，共 47 个 IR/HPP/LGLSLH/GLSL/SPV 文件哈希；`generated/`、`graph-codegen/` |
| 非 Shader 与可选字段 | PASS | NonShader、Optional、Attachments、Transfer；实际生成的 C++ 元数据及 Builder 测试 |
| 拥有权与非法作者输入 | PASS | Params 修改/析构后快照不变；缺资源/fallback、范围溢出、重叠 mip、非法 resolve 拒绝 |
| 概念与编译负例 | PASS | typed texture/buffer、缺 Schema、未标注 Params、非法 borrow、裸 factory、非法 annotation/layout |
| Shader 负例 | PASS | 实际编译 offset、额外 scalar、descriptor count、访问、额外 resource 候选，生产校验精确拒绝 |
| 公共头与源码外消费者 | PASS | 6 个 Graph 头、PassContract、2 个 Shader API 独立 TU；仅拷贝公开头和 I 静态库的消费者构建并执行成功 |
| 实际依赖闭包 | PASS | File API、compile commands、Ninja deps、link、真实 codegen depfiles；282 个头输入，`closure.json` |
| Graph CPU ASan | PASS 9/9 | declarations/hazards/lifetimes/legacy/bindings/reuse/generated/probe/authoring ownership；`asan-cpu-tests.*` |
| 全 bootstrap ASan | PARTIAL | 全量构建已尝试但未成功；预装 SPIRV-Cross 的 `annotate_string/vector=0` 与 MSVC ASan 对象 `=1` 发生 LNK2038；未关闭注解或修改第三方 |
| 保护事实 | PASS | Core、Transport、R4 Vulkan、所有既有 docs tree 不变；719/719 Legacy blob/mode/size；`closure.json` |
| 用户修改 | PASS | 原 HEAD/branch/status/staged/unstaged/untracked 及六个原始字节哈希一致；`final-protection.json` |
| 三安装头前缀同步 | PASS | Debug/RelWithDebInfo/Android 共 21 个头副本，来源 I 与字节哈希一致；`header-sync/`；不是 Android/SDK 构建资格 |

除了新 fixture 的逐字段生产对账，七组 SPIR-V 另通过 `spirv-val`，保存实际 reflection JSON 与 disassembly
于 `spirv-inspection/`。这仍属于 Shader CPU 工具链资格，不是 GPU 输出资格。

F1 public Graph 的编译/链接依赖为 Core；中立 `PassContract.hpp` 无 Script/Math 聚合链接。
Shader 工具链独立使用 header-only `shader_metadata` 和私有 SPIRV-Cross；Graph 不链接这些离线执行工具。
Core/Transport/Graph 无 Vulkan 依赖，Vulkan Foundation 仍只依赖 Core；无 Scene/Editor/Runtime/Legacy 输入。
源码外消费者证明本阶段公开头闭包，**不等于正式安装 package/完整 SDK 已资格化**。

环境：Windows x64、MSVC 19.44.35228 / toolset 14.44.35207、C++20、Ninja、RelWithDebInfo、Python 3.13.0、
Vulkan SDK/glslc 1.4.304.0、已安装 SPIRV-Cross Release 库；lux-cxx HEAD
`bf779515a120350c7c5412c366eea73afdf59ccf`。工具二进制哈希见 `tool-identities.json`。

证据采集曾遇到两项 harness 问题：Git 中文路径转义及短名称 `cl.exe` 启动路径。修正读取参数/使用配置中的
绝对编译器路径后重新采集，未修改 I 源码；分别记录于 `qualification-harness-notes.txt` 和
`public-consumer-launcher-note.txt`。ASan ABI 失败完整保留，不归类为 harness 成功。

## 性能与分配

CPU 为 i7-13700KF，配对进程固定 affinity mask 4；七组交替 before/after，所有样本保留、不挑选结果。
每组一百万次 binding，预热一万次，batch 1000。before 二进制来自原独立 R4 环境，Graph tree 与 F0 基线
逐字节相同；`baseline-identity.json` 锚定 tree 和二进制。源码及测量口径一致。

| 项目 | Before | F1 | 结论 |
| --- | ---: | ---: | --- |
| binding p50 中位数 | 10.6 ns/op | 10.6 ns/op | 0.00% |
| binding p95 中位数 | 10.6 ns/op | 10.6 ns/op | 0.00% |
| 每次运行 max 的中位数 | 135.3 ns/op | 154.4 ns/op | +14.12%，如实保留；不是 p50/p95 门禁指标 |
| 首方 C++ 分配次数/字节 | 0 / 0 | 0 / 0 | 稳定 binding 合同保持 |

新冷态 Tonemap Builder 基线：10k 次，p50 760 ns、p95 1150 ns、max 9190 ns，总计 110,000 次分配、
26,720,000 字节。它包括拥有型快照构造，允许冷态分配；没有等价旧 authoring baseline，不宣称性能提升。
原始七组配对与冷态数据见 `performance.json`、`performance-*.log`。本阶段没有每帧 GPU 性能结论。

## 追踪编号与保留项

- I01：本阶段 Builder/typed authoring PASS；完整逻辑编译和 scope/hazard 证明在 F2。
- I07/I08/I28/I29/I30：本阶段同源生成、ABI、emitter、反射与中立 metadata PASS；不关闭后续 native
  descriptor writes、共享 LayoutPlan、设备预算、relocation 和完整 installed SDK 义务。
- I38：PassSchema Concept 部分 PASS；真实 Feature optional ops 仍为 F6 未完成项。
- REQ11：Graph ID 与 RenderData/Capability 类型隔离部分 PASS；Scene 集成部分留 F7。
- REQ12/REQ44：F1 PASS；REQ13/REQ15 的 F1 部分 PASS，F3 的 native layout/绑定事务保留。
- W18：本阶段真实正负编译、public closure PASS；Feature optional-hook 部分按 F0 解释在 F6。
- W01/W15：仅作者/Shader 生成与反射部分 PASS，GPU 与完整 owner layout 部分 NOT_RUN，不能关闭整行。

当前 whole-resource R3 compiler 保留原 oracle；对 partial mip/layer/buffer 调度返回明确
`kGraphUnsupportedScheduling`，不把 F1 HZB 声明误报为 F2 调度已通过。
Pointer/reference、bitfield、未支持的 vector/matrix 与多维数组形态明确拒绝；工作单列明已冻结语法。

本轮 NOT_RUN：新 F1 GPU draw/readback、Runtime/FrameLoop、F2 完整编译、F3 native LayoutPlan、产品矩阵、
正式 packaged SDK、Linux、Android、V1 全矩阵。原有 R4 GPU CTest 作为回归实际执行，不代替这些结果。
历史 R2 PARTIAL、各阶段 GPU/ASan/平台判定及 FINAL 原包均未改写。

## 完整实现文件清单

- `cmake/render-v2-bootstrap/CMakeLists.txt`
- `docs/render-v2/F1_TYPE_RESPONSIBILITIES.md`
- `docs/render-v2/F1_WORK_ORDER.md`
- `engine/toolchain/shader/CMakeLists.txt`
- `engine/toolchain/shader/include/lux/engine/toolchain/shader/PassValidation.hpp`
- `engine/toolchain/shader/include/lux/engine/toolchain/shader/lglsl/LglslEmitter.hpp`
- `engine/toolchain/shader/src/PassValidation.cpp`
- `engine/toolchain/shader/src/SpirvReflection.cpp`
- `engine/toolchain/shader/src/lglsl/LglslEmitter.cpp`
- `modules/function/render/graph/CMakeLists.txt`
- `modules/function/render/graph/cmake/PassSchema.py`
- `modules/function/render/graph/cmake/RenderGraphCodegen.cmake`
- `modules/function/render/graph/cmake/pass_schema.template`
- `modules/function/render/graph/include/lux/engine/render/graph/Authoring.hpp`
- `modules/function/render/graph/include/lux/engine/render/graph/Builder.hpp`
- `modules/function/render/graph/include/lux/engine/render/graph/Definition.hpp`
- `modules/function/render/graph/include/lux/engine/render/graph/Schema.hpp`
- `modules/function/render/graph/pinclude/lux/engine/render/graph/DefinitionAccess.hpp`
- `modules/function/render/graph/src/Builder.cpp`
- `modules/function/render/graph/src/Definition.cpp`
- `modules/function/render/graph/src/Plan.cpp`
- `modules/function/render/graph/test/AllocationCounter.hpp`
- `modules/function/render/graph/test/Benchmark.cpp`
- `modules/function/render/graph/test/CMakeLists.txt`
- `modules/function/render/graph/test/Graph.cpp`
- `modules/function/render/graph/test/Probe.cpp`
- `modules/function/render/graph/test/authoring/Attachments.hpp`
- `modules/function/render/graph/test/authoring/BadBorrow.hpp`
- `modules/function/render/graph/test/authoring/BadDuplicateRole.hpp`
- `modules/function/render/graph/test/authoring/BadLayout.hpp`
- `modules/function/render/graph/test/authoring/BadRole.hpp`
- `modules/function/render/graph/test/authoring/BadUnknown.hpp`
- `modules/function/render/graph/test/authoring/Benchmark.cpp`
- `modules/function/render/graph/test/authoring/Blur.hpp`
- `modules/function/render/graph/test/authoring/Blur.lglsl`
- `modules/function/render/graph/test/authoring/Builder.cpp`
- `modules/function/render/graph/test/authoring/Complex.hpp`
- `modules/function/render/graph/test/authoring/Complex.lglsl`
- `modules/function/render/graph/test/authoring/Composite.hpp`
- `modules/function/render/graph/test/authoring/Composite.lglsl`
- `modules/function/render/graph/test/authoring/Fixture.cpp`
- `modules/function/render/graph/test/authoring/Hzb.hpp`
- `modules/function/render/graph/test/authoring/Hzb.lglsl`
- `modules/function/render/graph/test/authoring/LocalRead.hpp`
- `modules/function/render/graph/test/authoring/LocalRead.lglsl`
- `modules/function/render/graph/test/authoring/NonShader.hpp`
- `modules/function/render/graph/test/authoring/Optional.hpp`
- `modules/function/render/graph/test/authoring/Reject.cmake`
- `modules/function/render/graph/test/authoring/Reject.cpp`
- `modules/function/render/graph/test/authoring/RejectCodegen.cpp`
- `modules/function/render/graph/test/authoring/RejectShader.py`
- `modules/function/render/graph/test/authoring/Storage.hpp`
- `modules/function/render/graph/test/authoring/Storage.lglsl`
- `modules/function/render/graph/test/authoring/Tonemap.hpp`
- `modules/function/render/graph/test/authoring/Tonemap.lglsl`
- `modules/function/render/graph/test/authoring/Transfer.hpp`
- `modules/resource/description/cmake/ShaderMetadata.cmake`
- `modules/resource/description/include/lux/engine/description/PassContract.hpp`

## 停止与下阶段准入

`V2_PRODUCT = EXPECTED_UNAVAILABLE`

`NEXT_STAGE = F2 / 完整逻辑图，仅在用户审查后单独授权`

`STOP`

F2 必须以本 I/V 为基线并保留本次生成/Shader/原 R3 oracle，实施 C0–C8、版本化子资源、required/optional
producer、确定性排序、裁剪/条件/副作用、完整 scope/lifetime 验证。不得将本次有限调度器或临时 Shader
编译槽当作未来完整方案。当前报告不授权 F2、F3 或 Runtime。
