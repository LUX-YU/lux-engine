# F1-FIX 独立资格验收 — PASS

日期：2026-10-10。仅 F1-FIX；不授权 F2。

## 提交身份与独立性

| 项目 | 身份 |
|---|---|
| 审批基线 / F1 Verification | `877d44c6eaea03dd5746b03f67af158e97a8ee71` |
| F1 Implementation | `78a1248d1f5e9c59eb8f88ddd4869244e512050f` |
| F1-FIX 主实现 | `f83fd62565213dcf679fbf36b1ff05e2476ef22a` |
| 最终 Implementation I | `825cce0bf228af32e6c3acd330a8a6f518cc951b` |
| Frozen V1 | `a669409a289a6fa4092f21176397795b1cdb7f3e` |
| Verification V | 仅新增本文件的后续提交；确切 SHA 由 Git 提交身份及交接消息给出 |

首次 I 的普通/ASan 92/92 与性能均通过；归档发现 Legacy fixture 的 Git blob 为 CRLF，
冻结原 Git blob 为 LF。按独立资格纪律，在施工区追加 `825cce0bf228`，仅将 fixture 改为精确冻结字节。
首次证据原样保留并标 PARTIAL；没有在验收检出修补生产源码。以下结果全部重新绑定最终 I，
不是复用首次测试成绩。

施工 worktree：`C:/Users/ChenHui/.codex/worktrees/render-v2/lux-engine`。
独立 `clone --no-hardlinks --no-checkout` 后 detached checkout I：
`D:/LuxQualification/render-v2-f1-fix-825cce0bf228/source`。
普通/ASan 构建分别在同级 `build` / `asan`，没有构建与测试并行。

## 四项修复

- **格式：PASS。** Graph 自有格式枚举删除，直接使用中立 `rdesc::ETextureFormat`；
  explicit classification / usage / aspect 验证不依赖枚举顺序。覆盖 RGBA16_SFLOAT、sRGB、
  D16/D24/D32 Depth-Stencil、未定义格式、compressed/depth/sRGB 非法 storage/attachment。
  `HalfStorage` 经生产 Meta → emitter → glslc → SPIR-V reconciliation 验证 rgba16f。
- **范围：PASS。** UBO 默认 sizeof(T)，64 字节 UBO 的 1 字节声明被拒绝；SSBO 具有 stride、
  alignment 与有限 backing 上界，剩余范围在 finish 解析，固定数组不能冒充运行时数组。
  SampledTexture 默认覆盖完整 mip/layer view，显式 view 范围及维度/array/aspect 越界拒绝；
  Definition 中 binding/use 范围完全一致。没有实现 F2 Hazard Scheduler。
- **Stage：PASS。** 同一 Stages C++ Params 生成 Vertex 专属、Fragment 专属及共享资源。
  Vertex 实际反射 2 个资源，Fragment 4 个，共享 UBO 的 provisional binding 在两 Stage 均为 1。
  每 Stage 子集及完整 Vertex+Fragment 程序分别验证；缺模块、重复/错误 Stage、共享 UBO 布局不一致拒绝。
  version/extension/include 顺序经过真实编译，Legacy Tonemap 输入与 frozen Git blob 完全相等。
- **身份：PASS。** 作者通过 Pass 名称、Shader asset+variant、资源语义名称进入 Builder；
  复用 StrongId/Fnv1a64，Definition 保留 owning canonical names。import 必须有名称与 Scope；
  重复 semantic 不会隐式共享。没有全局名字管理器、Provider resolver 或 Native ImportContract 实现。

精确 API、范围语义、类型责任和来源见 [F1_FIX_CONTRACTS.md](F1_FIX_CONTRACTS.md)。

## 构建、测试与工具链

| 门禁 | 结果 | 原始证据 |
|---|---|---|
| tracked snapshot / clean clone / 范围 | PASS | tracked-snapshot、clone、checkout 日志与 closure.json |
| MSVC RelWithDebInfo full all -j 4 -- -k 0 | PASS | configure.log / build.log |
| 普通第二轮 full all | PASS，无工作 | build-2.log |
| 普通全部 CTest | **92/92 PASS** | test.log、regular-Testing |
| 原 83 项测试义务 | 全部保留，无删除 | test-obligations.json、tests.json |
| 完整 MSVC ASan all 两轮 | PASS，第二轮无工作 | asan-configure/build/build-2 日志 |
| 完整 ASan CTest | **92/92 PASS** | asan-test.log、asan-Testing |
| 原 Graph CPU ASan 9 项 | 9/9 PASS，包含于完整运行 | declarations/hazards/lifetimes/legacy/bindings/reuse/generated/probe_control/authoring_ownership |
| Meta/codegen/Shader 实际产物 | PASS，61 个产物哈希，11 个 SPIR-V 模块 | closure.json、regular-generated、regular-codegen、spirv-inspection |
| SPIR-V 结构验证与反射 | 11/11 PASS | spirv-val / spirv-cross / spirv-dis 原始输出、命令及退出码 |
| C++20 公共头独立编译 | PASS | Graph/Schema/Builder/PassContract/Shader 公共 TU；另有 header-image.cpp/log/json |
| 原 R3 oracle | PASS，算法与测试源码未改写 | 原排序、生命周期、300 随机图、10k bindings/计划复用测试 |
| Legacy fixture 精确来源 | PASS | legacy-fixture.json |

工具：MSVC 19.44.35228、Ninja、CMake、Python 3.13、生产 lux_meta_generator、
Vulkan SDK 1.4.304.0 glslc / SPIR-V tools；依赖由实际 cache、compile_commands、File API 和命令记录固定。
测试 Shader 不手写最终 set/binding；生产 emitter 分配的暂定槽由同源 Schema validator 对账。

新增 9 个 CTest：

- `render.graph.fix_formats`
- `render.graph.fix_ranges`
- `render.graph.fix_identities`
- `render.graph.schema_HalfStorage`
- `render.graph.schema_Stages.vert`
- `render.graph.schema_Stages.frag`
- `render.graph.schema_LegacyTonemap.vert`
- `render.graph.stage_program`
- `render.graph.shader_reject_runtime_array`

### ASan 原阻碍的处理

正式安装的非 ASan SPIRV-Cross 与 MSVC STL annotations 不匹配，是 F1 原 PARTIAL 的原因之一。
本轮以现有 `-1.4.335.0-6d968ec968.clean` 源码在外部独立目录构建 core 静态库，使用
`/EHsc /fsanitize=address /Zi /MD`，无 annotation suppression，无第三方源码或 ABI 定义修改。
该隔离包的 9 份公开头与正式安装头逐字相同（`spirv-asan-source.json`），实际库和 CMake package 已归档。
完整 instrumented bootstrap 重新生成全部 fixtures、构建全部目标并运行 92 项测试。
旧 F1 报告不修改；本结果只归属于当前 I 与本机 MSVC 环境。

## 性能与分配

固定 affinity mask=4，原 F1 合格二进制与最终 I 二进制交替运行 7 组，每轮 1,000,000 binding 操作，
每轮独立 10,000 次预热，统计全部轮次的中位数，不丢样本。原始 before/after 与二进制 SHA-256
见 `performance.json` 和 14 份逐轮日志。

| Graph frame binding | Before ns/op | After ns/op | 变化 |
|---|---:|---:|---:|
| p50 | 10.7 | 10.6 | −0.93% |
| p95 | 10.8 | 10.6 | −1.85% |
| 每轮 max 的中位数 | 134.0 | 98.8 | −26.27% |

全部稳态轮次 **0 次 / 0 字节首方 C++ heap allocation**，无 >5% 回退。微小变化按本机观测记录，
不宣称渲染/GPU 性能提升。Cold Tonemap authoring 10k 次单独记录：p50 1060 ns、p95 1180 ns、
max 32880 ns，130000 次分配 / 32320000 字节；这是允许分配的冷态，不冒充 frame binding 零分配。

## 依赖与保护

- 实际 File API、compile commands、Ninja deps、link libraries、codegen jobs/depfiles：PASS。
  共 287 个实际编译头；Graph production 仍只链接 Core，仅增加中立 Image.hpp 作为格式权威。
  无 Description Math/Script 聚合，无 Vulkan/Scene/ECS/Editor/Runtime/Legacy 编译或链接越界。
- Core、Transport、R4 Vulkan tree 和全部历史 R0–R4/F0/F1 报告：Git 对象与基线相同，
  见 `protected-objects.json`。历史 PARTIAL/NOT_RUN 判定未改写。
- Frozen Legacy 719 文件 blob/mode/size 全部相同；保护 CMake 一并保持，tree 仍为 720 项。
- 原用户工作区 HEAD、branch、status、staged/unstaged binary diff、未跟踪清单、六文件 SHA-256
  全部与开工前相同：`user-protection.json` 及原始快照。
- 6 份 modules 公共头 × 三安装前缀共 18 项已同步并核验源哈希，原内容备份保留于 preflight。
  这不表示 Android 或完整 installed SDK 已验证。

## 证据位置与复跑

`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F1-FIX/825cce0bf228af32e6c3acd330a8a6f518cc951b/`

`run.py` 固定 VS 环境与各配置，`qualify-all.py` 顺序执行最终独立验收，`qualify.py` 验证依赖/来源，
`performance.py` 保留比较口径，`archive.py` 保存原始测试/生成物/保护事实。

证据 manifest：**806 项**，SHA-256：
`6170a7c2de6a319ac2ed374b9fb862d694372b338188abb7b1384034575cc68a`。
首次资格尝试保留在同级 `f83fd62565213dcf679fbf36b1ff05e2476ef22a/`，不替代当前证据。

## 未运行及停止

F1 新增 Shader 的 GPU 图像输出、F2 完整逻辑调度、Native Graph、Runtime/FrameLoop、产品构建、
Linux、Android、完整 installed SDK：**NOT_RUN**。
原 R4 native GPU 测试作为 83 项保留回归重新运行并通过，不冒充 F1 新增 Shader GPU 资格。

`F1_FIX = PASS`

`V2_PRODUCT = EXPECTED_UNAVAILABLE`

`NEXT_STAGE = F2 / Complete Logical Graph，仅在用户复审并明确放行后`

`STOP`

## 完整实现变更清单

下列为审批基线至最终 I 的 27 个文件；V 仅新增本报告。

- `docs/render-v2/F1_FIX_CONTRACTS.md`
- `docs/render-v2/F1_FIX_WORK_ORDER.md`
- `engine/toolchain/shader/include/lux/engine/toolchain/shader/PassValidation.hpp`
- `engine/toolchain/shader/src/PassValidation.cpp`
- `engine/toolchain/shader/src/lglsl/LglslEmitter.cpp`
- `modules/function/render/graph/cmake/PassSchema.py`
- `modules/function/render/graph/include/lux/engine/render/graph/Authoring.hpp`
- `modules/function/render/graph/include/lux/engine/render/graph/Builder.hpp`
- `modules/function/render/graph/include/lux/engine/render/graph/Definition.hpp`
- `modules/function/render/graph/include/lux/engine/render/graph/Schema.hpp`
- `modules/function/render/graph/src/Builder.cpp`
- `modules/function/render/graph/src/Definition.cpp`
- `modules/function/render/graph/test/CMakeLists.txt`
- `modules/function/render/graph/test/authoring/Benchmark.cpp`
- `modules/function/render/graph/test/authoring/Builder.cpp`
- `modules/function/render/graph/test/authoring/Contracts.cpp`
- `modules/function/render/graph/test/authoring/HalfStorage.hpp`
- `modules/function/render/graph/test/authoring/HalfStorage.lglsl`
- `modules/function/render/graph/test/authoring/LegacyTonemap.vert.lglsl`
- `modules/function/render/graph/test/authoring/RejectShader.py`
- `modules/function/render/graph/test/authoring/StageCommon.lglslh`
- `modules/function/render/graph/test/authoring/StageProgram.cpp`
- `modules/function/render/graph/test/authoring/Stages.frag.lglsl`
- `modules/function/render/graph/test/authoring/Stages.hpp`
- `modules/function/render/graph/test/authoring/Stages.vert.lglsl`
- `modules/resource/description/include/lux/engine/description/Image.hpp`
- `modules/resource/description/include/lux/engine/description/PassContract.hpp`
