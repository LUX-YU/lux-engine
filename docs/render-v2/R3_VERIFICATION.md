# R3 — Logical RenderGraph 独立资格验收

结论：**PASS（R3 CPU 逻辑图范围）**。产品仍为 `EXPECTED_UNAVAILABLE`。
本报告记录实施提交的独立 clean clone 结果；不把它等同于用户独立复现或后续 GPU/产品资格。

## 身份与隔离

| 项目 | 值 |
| --- | --- |
| 分支 | `codex/render-v2` |
| 批准 / 实际远端入场基线 | `e0eabe1640d94fafb80a57bd334507162f3b2f9f` |
| R2-FIX 实现 | `4ae55059cca949178a8a1f2eaffe32f720384e8d` |
| R3 implementation I | `ed4b78c1a4606f917eec1b30c654567935e6de48` |
| R3 verification V | 包含本文件的独立提交；仅新增本报告，父提交为 I |
| Frozen V1 | `a669409a289a6fa4092f21176397795b1cdb7f3e` |
| 独立源码 | `D:/LuxQualification/render-v2-r3-ed4b78c1a460/source` |
| 独立构建 | `D:/LuxQualification/render-v2-r3-ed4b78c1a460/build` |

先提交 I，再分别对施工 worktree 和独立 clone 执行 `ValidateTrackedSnapshot.cmake`。
clone 使用 `git clone --no-hardlinks --no-checkout` 后 detached checkout I。
正式验收未修改 I 或 clone 源码，结束后两处再次通过 clean tracked snapshot 检查。
原用户 checkout 仍在原分支/SHA；其 staged/unstaged binary diff、status 和六个文件字节哈希
均与开工快照一致。本阶段未修改 Core、Transport、Legacy、既有设计规范和历史报告。

## 交付及公开 API

真实静态 target：`lux::engine::render::render_graph`，编译依赖只有 `render_core`。

| 公共头（`lux/engine/render/graph/`） | 提供内容 |
| --- | --- |
| `Definition.hpp` | `GraphResourceId`、`GraphPassId`、资源 kind/origin、access/usage、资源/pass/dependency 值、`RenderGraphDefinition::create`、六个结构化错误 ID 与描述符出口 |
| `Plan.hpp` | `GraphResourceLifetime`、`CompiledGraphPlan::compile`、只读 definition/order/dependencies/lifetimes/imports、精确 `matches` |
| `Bindings.hpp` | `GraphBackingId`、`GraphImportBinding`、`GraphFrameValues`、`FrameGraphBindings::create` 及只读访问 |

Definition 拥有规范化声明，Plan 拥有定义快照与派生顺序，不保存 builder 内部指针。
资源/pass ID 是当前定义内的一基序号，不是跨 owner 或全局身份。显式依赖先做 Kahn 排序，
据此解释 mutable resource 版本，推导 RAW/WAW/WAR，再产生确定性计划和整资源使用区间。
Transient 第一次读取必须有此前写入；imported 资源具有初值。循环、错误 ID、非法用途和
缺 producer 返回准确 expected error，不产出半个执行计划。

FrameGraphBindings 显式借用指定 plan 与 import span；二者消费期间必须存活且不移动/修改。
禁止借用临时 plan。调用方提供不互相 alias 的 backend-scoped backing token；Graph 不拥有或
解引用实际资源，也不证明原生 readiness、格式、偏移范围或 GPU lifetime。
这些责任在真实 Backend consumer 中完成，不把简单非零检查冒充为跨 Runtime 验证。

## 独立渲染合同在 R3 的证明边界

- Plan 的拓扑相等依据资源 kind/origin/target semantic、pass 集合与顺序、uses 和显式依赖。
  use/edge 列表排序规范化，重复边去重，比较不依赖 hash，也不包含 Simulation revision。
- 在同一 plan 上连续创建 10,000 次 bindings，改变 frame serial、render time、frame slot、
  backing 和 buffer dynamic offset，计划对象与执行顺序存储保持不变。
- 独立构造的相同定义匹配；access、usage、origin、kind、target semantic、pass 集合和
  dependency 改变被识别。失败候选不会修改旧 plan。
- Binding 实现不调用 compiler，不比较拓扑，不含队列、线程或 Simulation tick 条件。
  冷态 `matches` 与 compile 由装配方显式调用；没有缓存 Manager 或隐藏全局编译器。

这是纯逻辑三段模型的验证。真正无 PROGRAM 时持续 GPU render/present、frame pacing、
Device Lost 和 in-flight 退休仍归 R5，R3 没有 FrameLoop 或假 Runtime。

## 构建与测试

环境：Windows / MSVC x64 19.44.35228.0（toolset 14.44.35207）、C++20、
RelWithDebInfo `/EHsc /DNDEBUG`；CMake 4.1.2、Ninja 1.11.1、Python 3.13.0。
CPU：Intel Core i7-13700KF，16 核 / 24 逻辑处理器。

唯一 dependency prefix 为 `E:/SyncForder/CodeRepos/install/Framework-v2-dependencies`，
该目录无 `include/lux/engine`。禁用 CMake 用户/系统 package registry；清除 Vulkan SDK
变量及旧 Engine build/install 的 PATH 项。未导入 Root 产品 CMake。

`lux-cxx` 源码 SHA=`bf779515a120350c7c5412c366eea73afdf59ccf`；
`lux-cmake-toolset` SHA=`99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`。
通用依赖源码 checkout 干净；九个实际消费的 lux-cxx 头与该源码逐字比较（仅归一 CRLF）。
消费的 package CMake 文件、code generator 和反射 DLL 哈希归档。

正式命令参数及退出码见证据 `01-*` 到 `11-*`；核心步骤：

```text
cmake -DLUX_SOURCE_DIR=<clean-I> -P <clean-I>/cmake/ValidateTrackedSnapshot.cmake
cmake -S <clean-I>/cmake/render-v2-bootstrap -B <build> -G Ninja
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_PREFIX_PATH=<generic-dependencies>
      -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> --output-on-failure -j 1 --output-junit <evidence>/ctest.xml
```

| 检查 | 结果 |
| --- | --- |
| 第一轮全量构建 | PASS，47 个步骤，退出码 0 |
| 第二轮全量构建 | PASS，`ninja: no work to do`，退出码 0 |
| 完整 CTest | **45/45 PASS**，10.72 秒；27 个旧测试原样保留 |
| 独立公共头 TU | PASS，Graph 3 个；加 Core/Transport 共 14 个 |
| 禁止依赖与类型编译负例 | PASS，核对预期诊断代码和缺失头，另有正例控制 |
| 无安装 target / 无旧 SDK 补闭包 | PASS；不宣称 installed SDK 合格 |

新增 18 个测试：

| CTest（`render.graph.*`） | 覆盖 |
| --- | --- |
| `declarations` | enum/ID/target semantic/usage 合法性，重复 use、自依赖、规范化 |
| `hazards` | RAW、WAW、多 reader WAR、read/read 无边、imported 初值、显式反向声明排序 |
| `lifetimes` | inclusive 执行位置、显式重排、未使用资源、imported 区间 |
| `legacy` | 两节点环、无 producer、空 side-effect pass 保留、move 后自有快照、失败候选保留旧 plan、缺 import binding |
| `bindings` | 数量/顺序、零 token、重复 alias token、image offset、空图 |
| `reuse` | 10,000 次不同帧绑定复用计划与真实拓扑变化检查 |
| `generated` | 固定 seed 的 300 张图（24 passes × 7 resources），独立 pairwise conflict/reachability/lifetime oracle |
| `probe_control` | 实际 Graph target 的可用正例 |
| `reject_ID` / `reject_TEMPORARY` | 强类型不可互换，临时 plan 借用被编译拒绝 |
| `reject_VULKAN/SCENE/RUNTIME/TRANSPORT/LEGACY/EDITOR/FEATURE` | 七项真实 target 头依赖负例 |
| `binding_allocation` | 一百万次 binding，测试专用分配计数 |

适用 V1 向量源于 frozen `vulkan/test/compiler_result.cpp`，算法参考
`graph/src/DependencyAnalyzer.cpp`；准确映射及延期项见 Graph README。
未运行旧 GPU 编译器 suite，未迁移 native pipeline/queue/conditional attachment 逻辑。

## 真实依赖闭包

`test/verify_r3.py` 检查实际 CMake File API、compile_commands、Ninja include DB、link fragments、
CMake 配置输入、Transport codegen job/template/depfile，以及允许修改路径。

- 200 个实际 compiler input 路径；Graph 部分 143 个（含 STL），Engine 头限 Graph/Core/generic Error。
- 56 个 compile command source（含负例和独立头 TU）、151 个 CMake inputs、129 个 codegen inputs。
- Graph static target 与其消费者没有 Transport 链接或 include；无 `lux/cxx/concurrent` 消费。
- 无 Vulkan/Scene/ECS/Editor/Runtime/Features/Legacy 实际编译依赖或旧 Engine SDK 输入。
- Core/Transport 原有 target/include/link 约束继续通过；Transport 生成 traits 在归一 checkout
  路径后与 R2-FIX 独立验收产物一致；reply 测试 hook 仍只存在于原专用测试可执行文件。
- 新 Graph production 无主动 throw/try/catch、terminal I/O、release-disabled assert。

## 有限范围性能记录

独立 clone 的 `render_graph_benchmark`，同一预编译 plan、一个 imported buffer，warm-up
10,000 次，测量 1,000 批 × 1,000 次，默认 OS 调度、无指定 affinity。每次改变 frame/backing/offset。

| 指标 | 结果 |
| --- | --- |
| operations | 1,000,000 |
| p50 / p95 / max | **10.600 / 10.600 / 30.600 ns/op** |
| C++ new/new[]（含 aligned） | **0 次 / 0 字节** |

上述百分位是批次平均值的分布，不是单请求延迟，也不是 render frame/GPU 性能。
这是新逻辑 API 的首个记录，没有可直接相比的 V1 native binding 基线，不作性能提升或
5% before/after 回退结论。编译是允许分配的冷路径；完整稳定帧零分配门禁留待真实执行阶段。
Transport 实现/测试/基准均未变化，本轮保留并重跑其全部 CTest，不重复百万次 Transport 比较。

## 保护与证据

719 个 frozen 文件的 blob/mode/size 全部匹配，Legacy tree 仍为 720 项（含 guard）。
Core、Transport 和八项其它关键冻结 tree/blob 身份保持不变，详见 `preservation.json`。
原工作区六个文件及 diff/status 未改变。三个新公共头同步三个前缀，共九项新文件，
全部字节一致；此前这些目标路径不存在。Android include 同步不代表 Android 构建。

外部证据目录：

```text
E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/R3/ed4b78c1a4606f917eec1b30c654567935e6de48/
```

`EVIDENCE_MANIFEST.json` 索引 **228 个文件**（不包括 manifest 自身）；全部长度与 SHA-256 已复核。
manifest SHA-256：`fac5a5200c2ad831a44e3002991d49634e415f82c12a4ce8dd563615c8a29fa4`。
其中有命令/退出码、两轮 build log、CTest/JUnit、负例诊断、File API、真实编译与 codegen
依赖、package/generator 哈希、公开头同步、用户前后快照、基准原始 JSON 和冻结事实检查。

I 的 16 个变更文件：bootstrap CMake、`R3_WORK_ORDER.md`；Graph 下
`CMakeLists.txt`、`README.md`；三个 `include/lux/engine/render/graph/{Definition,Plan,Bindings}.hpp`；
三个 `src/{Definition,Plan,Bindings}.cpp`；
`test/{CMakeLists.txt,Graph.cpp,Probe.cpp,RejectCompile.cmake,Benchmark.cpp,verify_r3.py}`。
V 仅新增本报告，不夹带生产或验收脚本修复。

## 保留项与停止

- R3 整资源模型不含 subresource/alias planning、条件执行、native format/layout/queue/barrier、
  recorder、Feature callback 或任何真正帧循环。GPU backing 存活/ready 由未来 Backend 证明。
- Definition/Plan 是自有值；FrameBindings 是明确借用。调用方不能把 borrow 跨 owner 销毁保存。
- 泛用 Error 头的完整 SDK 安装闭包继续在 R16/R17 处理。
- R5 必须落实 ticket consume/abandon 接收责任及独立异步诊断事件，不能将 ReplyArena 当所有通知。
- GPU、完整产品、V1 全矩阵、Android、Linux、Sanitizer、完整 installed SDK、最终 V1/V2
  渲染性能对照均 **NOT_RUN**；没有改写原历史 PARTIAL/NOT_RUN。

```text
R3 = PASS
V2_PRODUCT = EXPECTED_UNAVAILABLE
NEXT_STAGE = R4 / Vulkan Foundation, requires explicit user review approval
STOP
```
