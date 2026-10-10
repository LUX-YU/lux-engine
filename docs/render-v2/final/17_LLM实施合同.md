# 17 — LLM 实施总合同、首个 F0 工作单与每阶段硬性禁令

> **本章对所有受委派的实施/测试/审核 LLM 具有最高执行优先级。** 它与 00–19 的功能/类型/性能合同一起使用。用户此次交付允许实施方**开始 F0 文档入库与结构验收**，但不意味着可以不经独立审阅自动修改 F1–H6 的生产代码。每阶段验收后 STOP，后续经用户明确放行。

## 17.1 你不是为了让 CTest 变绿而编程

交付目标是：**完整继承 Legacy 所有功能，建立可以容纳 UE 级先进算法的系统性 RenderGraph/Shader/Feature/Engine 模型，并按 C++20 值/算法/RAII/Concepts 明确职责。** 不接受空桩、最小删功能 vertical slice 代替完整阶段、不接真正 GPU 的接口声明、`Manager` 补偿依赖问题、只修测试让 PASS、自行削减范围、事后改报告掩盖失败。

## 17.2 每阶段开始的 Preflight

1. 获取实际 `origin/codex/render-v2` HEAD，不猜提交；检查是否与批准基线一致及是否有后续 commit。
2. 读取本 Final Docs `00–19`、阶段直接相关上下层、`appendix/CapabilityMatrix/TypeOwnership/RequirementsTrace/GoldenWorkloads`，核对 SOURCE SHA/hash/原工作树。
3. 读取真实当前消费者、CMake target/include/link/codegen graph，不依据过去助手陈述推断一个 API 已存在。
4. 列出**具体路径** `ALLOWED / READ-ONLY / FORBIDDEN` 与每个修改文件对应的阶段证据/设计条款。
5. 把功能清单中本阶段负责的 `Fxx/Ixx/Hxx` 行与至少一个 real consumer / result oracle 绑定。
6. 对新类型提交 Type Responsibility Inventory；对新模板/Concept 提交编译正负例与 ABI/perf 影响。
7. 检查是否产生两份 authority/重复缓存/额外 EventBus/FrameLoop/线程；有则先报告，不得实施。

## 17.3 不可变原则（违反立即 FAIL）

- Frozen Legacy `render_legacy/**` 不可编辑，719 个 files blob/mode/size 保持完全一致；旧 V1 oracle 使用独立 checkout，不与 V2 链接。
- 已通过的 R1 Core、R2/R2-FIX Transport、R4 Vulkan Foundation 的 source/行为/验证历史保持；R3 Graph 可按新设计重写，但不得保留旧 graph 永久 shadow implementation。
- `PROGRAM/CONTROL owner-thread SPSC`, `UPLOAD bounded MPMC`，bounded reply `consume or abandon`（WRITING writer-owned recycle），不增加 runtime fourth event bus。
- Simulation/Publication/Rendering 三条独立进度，Backend 不读 ECS，Render 不因无 PROGRAM 停帧，也不 busy spin。
- 单 Runtime/Backend/FrameExecutor/FrameDriver；N Scene, N View, N Targets；View 不默认创建 GraphInstance，同路径 compiled plan 共享；必须证明共享 GPU work 不改变输出。
- RenderData/SceneCapability/GraphResource 三域隔离；单 authority/typed provider；不得在 stable record/draw 每次查 registry/hash。
- `LUX_PASS_PARAMS` 及现有 meta/.lglsl toolchain 是 single source，不让 Feature 重复声明 shader binding 和 Graph read/write，作者不写裸 set/binding。
- Native owner 只在真实 Vulkan lifetime 作用域 RAII；no public init/shutdown/semantic zombie；GPU-backed retirement fence-proven，不以 WaitIdle 普通销毁。
- C++20 Concepts/value/free algorithms/static optional FeatureOps；无默认 Feature 虚基类、IBackend、GenericAllocator、TimeManager、UniversalHistoryManager。
- 外部 Plugin 真实可安装 SDK consumer + code pin/ABI/卸载；任何私有头引用为 fail。
- stable full frame zero first-party C++ allocation，Graph record 无 lazy pipeline/layout/Shader compile；性能/Correctness 必须同时达标。
- 高级 GPU 功能为**真实算法**、画质/错误/资源预算各有 oracle，不以 `Pass` 占位名称覆盖。

## 17.4 静态代码外观红线

下列新增模式默认拒绝，除非先有书面设计批准+真实消费者/性能/lifetime 证明：

```text
IBackend / IRenderFeature / Universal*Manager / *Controller / *Context wrapper
new Scripting/ShaderReflection/RenderData parser parallel to lux-cxx tooling
new secondary Renderer/FrameLoop/EventBus/TaskQueue/auto background worker
per-view GraphInstance by default; per-frame rebuild/compile with stable topology
per-draw std::function/shared_ptr/unordered_map/string lookup/reflection
Graph public include vulkan.h or Scene/ECS/Feature private headers
per Feature private VulkanDevice/DescriptorPool owner duplication
C++23 std::expected/std::move_only_function/std::ranges::to under cxx_std_20
std::span/string_view/raw callback borrowed across async packet or DLL unload
if (!some_required_resource) return success; while(!resource) spin
vkDeviceWaitIdle for routine view/scene/asset retirement
error status encoded as bool + out-param / second registry
compiled logical topology key misused as complete native plan equivalence
```

禁止只是 grep 关键词就自判这些全部已满足：必须审查真实 CMake File API/compile_commands、运行时 lifetime 和实际 output。`std::shared_ptr`, `std::function`, `Context` 在确有冷态/真实生命周期场景下可以被局部使用，但不能以字面禁词替代语义审查。

## 17.5 首阶段 F0：Document Authority Integration（现在可以执行）

### 入场与基线

```text
Repository: https://github.com/LUX-YU/lux-engine
Branch: codex/render-v2
Reviewed design-time HEAD: 7de3ddaa3f7614995745b41d73dfc98302310a4c
R4 implementation: 83ffbb6d9859d87ca62a8a2acbb0083c2a8de420
Frozen legacy SHA: a669409a289a6fa4092f21176397795b1cdb7f3e
```

实施时必须核对实时 HEAD；若不同，比较变化并反馈用户，不能重置或覆盖未经授权提交。

### ALLOWED

```text
docs/render-v2/final/**              (全新设计 00–19 + appendix)
docs/render-v2/00_README.md         (最小入口指针/新规范优先级)
docs/render-v2/F0_*                (work order、独立验收报告)
```

### READ-ONLY

```text
docs/render-v2/R0*_VERIFICATION.md, R1*/R2*/R3*/R4*_VERIFICATION.md
所有实际旧方案事实证据和问题报告
modules/function/render/{core,transport,graph,vulkan}/**
render_legacy/**, user checkout six modifications
```

### FORBIDDEN

```text
Root/product CMake, Engine, Scene/ECS, Editor/UI, Runtime/Feature implementation,
任何生产 C++ 头/源、第三方工具链/CMake target/test 变更
```

### F0 实施动作

1. 将本包 `00–19`、appendix CSV/manifest 同目录无缺失导入权威 `docs/render-v2/final/`；**保持字节一致或为路径/编码合法化作最小、逐项记录的更改**，不改 R0–R4 结果。
2. 更新 `docs/render-v2/00_README.md` 入口指针，标注**历史旧目标说明被新 Final Docs 的目标契约替换**。R2-FIX、R3、R4 已通过的事实/未运行范围不可被删除/改写。
3. 对比本包内 R2 旧章节/新的 00–19 与当前仓库五份已批准架构补丁：Simulation/Render independence、bounded reply, Device owner, Frame lifetime must all be present in Final Docs。列出 conflict mapping。
4. 运行 `MANIFEST.sha256`、Markdown fence/link检查、CSV 主键唯一性/追踪章号、初始 Status 合规检查、frozen Legacy manifest、用户工作区 status/hash 保护。
5. 仅提交 doc-only I；独立 clone 核查 docs 和 untouched source；V 只新增 F0 Verification（具体证据索引/manifest），push 后 STOP。

### F0 完成条件

- Final 00–19 一份权威（旧章节可保留为历史参考但不能二套同时生效）；版本号、Frozen SHA、已验收事实对齐。
- `CapabilityMatrix` 每一旧能力有来源路径和实现/验收目标，后续阶段 owner/测试可追踪；`TypeOwnership` 的所有独立 owner 无双权威；`RequirementsTrace` 每条 HARD GATE 有章节和测试。
- 无生产行为改动、Core/Transport/Graph/R4/native/Legacy 完全未改；用户 worktree untouched。
- STATUS=`F0 PASS` 仅代表文档集成，不等于 RenderGraph/UE 高画质已经实现。

## 17.6 后续阶段 F1–H6 的 work order 生成要求

F0 STOP 后依第 15 章 **单独逐阶段**生成 `Fxx_WORK_ORDER.md`；必须引用本 Final Design 精确章节和 source/targets/Capability ID、ALLOWED/READ-ONLY/FORBIDDEN 路径、负例/性能/GPU oracles、I/V/source identity、提交与 STOP。用户审查通过后才开始下一个阶段，不准同一个实现提交偷偷提前做 F3–F11。

## 17.7 资格验收规则

实现 I 提交固定后，独立 clone 构建两遍，第二遍 no work，CTest 既有全量+新增，无隐藏 `#include old SDK`、依赖 DAG 越界；实际 Codegen input/Shader source/Link packages hash，公共头独立 TU 与 negative compile target，Native GPU Validation/ASan when required；原 workspace diff/status/hash 未改变、719 legacy blobs 匹配。

对 GPU/installed SDK/product/Linux/Android 未运行的环节，只能写 `NOT_RUN` 并给理由；若本阶段硬性准入要求必须真实 GPU，缺 GPU 的阶段只能 `PARTIAL`。不能用“工具环境不允许”把未运行自动按 PASS。验收发现实现 bug，先新 I，重建 clean clone 验证，不能 V report 提交夹带修改。

## 17.8 STOP 报告模板

```text
Phase: Fxx / I SHA / V SHA / remote HEAD
Scope: changed files and authoritative design chapter IDs
Type changes: new/removed owner/value/algorithm/builder/ops, why
Legacy capability IDs: migrated / partial / not_run
Tests: old/new exact counts, clean clones, sanitizer/GPU, raw evidence
Performance: workload, warm/cold, p50/p95/max, alloc_count, gpu time, VRAM
Invariants: single owner, no double render loop, no private include, three lanes
NOT_RUN: exact items and platform/blocker
Bugs/risks: exact source/expected behavior/next user decision
Final STATUS=PASS|PARTIAL|FAIL, V2_PRODUCT=EXPECTED_UNAVAILABLE until cutover
STOP, await explicit user approval; no auto next phase
```
