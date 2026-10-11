# RD2-F5-C 独立资格验收 — PARTIAL

Builder 失败事务、按需物理 Invocation、共享 Plan 和生命周期回归通过。Normal/Full ASan 各 175/175 通过，真实 GPU Validation/SyncValidation error 为 0。**Record/Submit 热态 p95 的 CI95 上界仍为 +5.5749%，未关闭 +5% 门禁，因此 F5-C 为 PARTIAL，不是 PASS。** 不追加实验，不将 F4 冷态延期改写为通过。

## 1. 身份与独立证据

- 批准基线 V：`28bff67f10366eaa5febd25d0be33e7d13ced6dd`；原 F5 I：`e662ff59844cf69f00d94341768df58ddc7f44b0`。
- 本轮最终 Implementation I：`e0c31de6678394c37ad7ed1ffc2d77231bccb9cc`。
- 第一实现提交：`7112247c9611b9525f52844b3bf68a33a632f88f`。其 Normal 175/175 后补充失败候选 VMA 峰值计数，形成最终 I；生产代码未变。旧候选证据保留在 `candidate-7112247c9611/`，不代替最终资格。
- 正式性能 A：原 F4-C-P I `92b126f67873ccbb576d0ffc5f1330964d6f84f6` 的锁定二进制；B 为最终 I 的独立 Normal 构建。
- Frozen V1：`a669409a289a6fa4092f21176397795b1cdb7f3e`。
- V 仅新增本报告；准确 V SHA 从 Git 提交取得并在交付中报告，不在文件中自引用。
- 独立检出：`D:/LuxQualification/render-v2-f5c-e0c31de66783/source`。`clone --no-hardlinks --no-checkout` 后精确检出 I；5,622 个 Git object 文件最大链接数 1，无 alternates，验收前后源码干净，未向 clone 补写生产修正。
- 原始证据：`D:/LuxQualification/render-v2-f5c-e0c31de66783/evidence/`。E: 空间不足，本轮新归档使用 D:，既有 E: 历史归档未修改。
- `evidence-manifest.json` SHA-256：`8d60a67d7043256a62dccd2a5a9658589ca05563db4b312bb0f61c541258797a`，覆盖 867 个文件。`artifact-identities.json` 另索引保留在独立 build/asan 树的 2,484 个工件及其字节数、哈希。

命令、退出码、全部原始采样、GPU 输出、故障复现、File API、编译/生成输入、Ninja dependencies、Link Map、保护快照均在归档中。设计/影响说明见 [F5C_IMPLEMENTATION.md](F5C_IMPLEMENTATION.md)，预先冻结协议见 [F5C_PERFORMANCE_PROTOCOL.md](F5C_PERFORMANCE_PROTOCOL.md)。

## 2. 独立构建与保护门禁

| 项目 | 本次实际结果 | 证据 |
|---|---|---|
| Tracked snapshot / clean clone | PASS | `tracked.log`、`clone-isolation.json` |
| Normal all `-j 4 -- -k 0` 两轮 | PASS；第二轮无工作 | `normal-all-1/2.log` |
| Normal CTest | **175/175 PASS，69.18s** | `normal-ctest.log` |
| Full ASan all 两轮 | PASS；第二轮无工作 | `asan-all-1/2.log` |
| Full ASan CTest | **175/175 PASS，147.39s** | `asan-ctest.log` |
| 原 168 项测试 | 全部保留并执行，新增 7 项 | 两套 closure 的 `test-obligations.json` |
| 实际 source/include/link/codegen closure | Normal/ASan PASS | `normal-closure/`、`asan-closure/` |
| Legacy | 719/719 blob、mode、size 一致 | closure、冻结 manifest |
| Core/Transport/F2/Shader Generator/Description、FINAL/RD2、历史报告 | Git 对象不变 | closure `protected_objects`、`diff.txt` |
| Foundation、Root/Product、Scene/Editor | 本轮未修改 | 全部允许范围检查 |
| 用户两个检出 | HEAD/branch/status/diff/untracked 及七个修改文件原字节不变 | `preflight/`、`user-protection.json` |
| 公共头三安装前缀 | `Executable.hpp` 三份同步，哈希一致 | `install-sync.json`、`install-final-check.json` |

配置：MSVC 19.44.35228 / 14.44.35207、C++20、RelWithDebInfo、Ninja、Vulkan SDK 1.4.304.0。Full ASan 全部 C++ TU 使用 `/fsanitize=address`，采用匹配 ABI 的 SPIRV-Cross ASan 依赖，没有屏蔽 annotations。准确工具和依赖参数以归档 configure/compile/link 输入为准。

现有依赖方向保持：Core → 中立基础；Transport/Graph 不引入 Vulkan；Foundation 不引入 Graph/Runtime；Native Graph 不引入 Runtime；Runtime 冷态私有接入 Native。公共头独立编译、负例编译和生成器实际输入通过。测试专用 host-call 替换、footprint hook 和 profiling wrapper 未进入生产库；热态链接无 Shader compiler/SPIRV-Cross。安装头同步不等于完整 SDK 或 Android 资格。

## 3. Builder 与生命周期

`finish()` 的 Thread 创建失败分支先保存系统错误，再把 Backend/Transport 所有权完整移回 Assembly。临时 Impl 销毁其 Event/Timer 与 packet 候选；此时不存在 worker。失败 Builder 保持原 Backend/device 身份，可继续装配、移动、重试或析构。成功仍只移交一次。Event/Timer 失败发生在所有权转移之前。

`render.runtime.host_failures` 编译真实 Runtime.cpp，只有测试 TU 替换四个 Win32 调用。确定性覆盖 Event、Timer、Thread 失败、重试、移动、部分构造析构与 Stop。Normal/ASan 均记录 15 个 Handle 创建/关闭平衡、存活 worker 为 0；没有生产线程工厂或新状态机。

共享不可变 Plan 未改为每 View 编译。每 View 仅增加私有 `invocation_first` 索引；冷态 `NativeGraphInvocations::append()` 建完整候选后移动 slots，保留原 owner、receipt、资源地址和真实 completion。现有 Native Invocation GPU 测试在首个提交仍已接纳时追加存储，继续验证 Scene donor、四 View、2/3 FIF、独立像素与 Plan 保活。sampler 借用目录必须相容；CPU 装配仍要求串行。

View 删除/Path 切换/Resize 不释放尚被 GPU 使用的 backing。组复用逐 slot 核对真实完成；Resize 候选组互不重叠后才提交映射。Image/Descriptor 创建失败不破坏旧输出。缓存仍有既有 variant/slot 上限，保留高水位，不承诺删除 View 后立即缩减；失败的多 View 冷事务可以留下有界且未发布的合法缓存项。

Presentation 源码未变。本轮保留外部-use join、acquire consumption 与 Present fence 的独立责任，实际运行双 Target Present、Resize、Minimize/Restore、in-flight removal、Stop/Drain、首次/部分 submit Device Lost 注入。未发现需要扩大为 WSI 重构的已证实缺陷；这不构成未测试驱动失败分支的普遍证明。

## 4. 实际物理规模与失败峰值

同一 fixture：最大容量 8 View，一个 RGBA32F 输出及对应 host-readback buffer。以下是 **VMA allocation bytes / block bytes**，不是显存物理驻留估计。每个 slot 有一个 Image、Buffer、DescriptorPool 和 set；下表正常装配时创建数均等于 slot 数，VMA allocation count 为 slot 数的两倍。

| Extent / FIF | 实际 View | 修正前 slots / used bytes | 最终 slots / used bytes | 最终 reserved bytes |
|---|---:|---:|---:|---:|
| 1080p / 2 | 1 | 16 / 1,097,072,640 | 2 / 137,134,080 | 137,887,744 |
| 1080p / 2 | 4 | 16 / 1,097,072,640 | 8 / 548,536,320 | 752,877,568 |
| 1080p / 3 | 1 | 24 / 1,645,608,960 | 3 / 205,701,120 | 307,494,912 |
| 1080p / 3 | 4 | 24 / 1,645,608,960 | 12 / 822,804,480 | 894,435,328 |
| 4K / 2 | 1 | 16 / 4,262,461,440 | 2 / 532,807,680 | 532,807,680 |
| 4K / 2 | 4 | 16 / 4,262,461,440 | 8 / 2,131,230,720 | 2,131,230,720 |

逻辑有效载荷为每 slot `width × height × 16 × 2` 字节；allocation 差异包括实际 Vulkan memory requirements/alignment，reserved 包括 VMA block。单/四 View 同 extent 始终一份相容编译计划、一个 Pipeline/Layout。第二个 half-size extent 创建第二个物理 variant，仅增加其实际 FIF slots。不同 RenderPath 的共享资产、场景隔离继续由原 GPU oracle 验证。

故障窗口内真实 allocation 峰值及回滚（Normal/ASan 数值一致）：

| 配置 | candidate peak used | candidate peak reserved | 回滚后 used |
|---|---:|---:|---:|
| 1080p / 2 | 652,492,800 | 823,656,448 | 548,536,320 |
| 1080p / 3 | 962,150,400 | 1,000,603,648 | 822,804,480 |
| 4K / 2 | 2,531,328,000 | 2,531,328,000 | 2,131,230,720 |

在部分 Image 成功后、部分 DescriptorPool 成功后分别故障，allocation count/used bytes 恢复，原四 View 映射不变。池所属 DescriptorSet/CommandBuffer 随 owner 释放，不将缺少单独 free 调用计作泄漏。较小 fixture 真实执行像素检查、删除复用、失败 Resize 保留旧输出、来回尺寸切换及十个尺寸的有界缓存淘汰，Pipeline/Layout 创建数仍为 1。

冷态计时包含 target Image、scratch/readback、descriptor 初始化。1080p/2 的四次 addView 为 1.0097、6.0984、4.5717、2.1445 ms；全部配置、失败峰值、cold resize 原始值见 CTest JSON 行和 `qualification-summary.json`。这些不是通用优化百分比。1080p/4K 用例证明真实冷态分配；不冒充 4K 渲染吞吐或复杂 GBuffer 业务资格。修正前 descriptor 汇总键拼写错误及正确 per-operation 原始计数已在实现文档解释，未改写旧样本。

## 5. Import 规模与 GPU 回归

1/8/32/64 个真实 Import，每个两个范围；1/4 个消费者、2/3 FIF 全部运行并核对 GPU 字节。Normal 的 4 消费者/3 FIF production preparation（包含 binding preparation）如下：

| Imports | p50 μs | p95 μs | 首方 allocation count/bytes |
|---:|---:|---:|---|
| 1 | 0.3 | 0.8 | 0 / 0 |
| 8 | 3.4 | 4.2 | 0 / 0 |
| 32 | 25.7 | 29.0 | 0 / 0 |
| 64 | 91.2 | 150.1 | 0 / 0 |

修改前同 64/4/3 诊断为 567.8/663.5 μs，原二进制、源码、样本均保留。证据支持的最小改动：一次检查现有所有 cell 的真实 queue ticket；全部完成时跳过重复历史扫描，任一仍在途则执行原有验证。没有新缓存 authority、revision 替代或全局索引。**这不是线性复杂度证明，也没有关闭最坏在途扫描成本**；规模与全部异常样本保留。

GPU：RTX 4070 Ti 12GB，driver 591.86（raw 2480242688），Vulkan API 1.4.325，graphics/compute/transfer families 0/2/1。真实 fixtures 启用 Khronos validation/synchronization validation，Normal/ASan error 0；原日志保留 warnings，不声称零警告。性能程序关闭 validation，不能替代 GPU 正确性资格。

新增七项：host failures、2/3 FIF slot transactions、1080p 2/3 FIF footprint、4K footprint、Import scale。既有 168 项未删减；包含 C1 compute→host、C2 imported memory alias/在途冲突、C3 A→B ImageView、Vertex/UINT16/UINT32/Indirect GPU draw、四 View Camera/History、Scene 共享证明负例、共享 Backing、双 Target 像素、独立帧进度及故障退休。原 300 张随机图、版本/Scope/绑定复用、Schema/Shader 和基础 ownership 义务也实际重跑。

## 6. 正式热态统计与退出

协议 SHA-256：`003d8917095dce8a3c64c8d947a0abe31e9cfee46fa419536ffecf057f35ea63`。实验脚本 SHA-256：`8f514d292434280d473344e60e31ba38fa5f92c41721dbfc219ee9af7f3dd173`。两者随 I 先冻结，再观察正式结果。

恰好 332 个独立进程；每 workload 21 AA、21 BB、41 AB 对，seed 20261016、交替顺序、affinity mask 4、AboveNormal、高性能电源计划、QPC 10MHz。没有并行构建/GPU 测试，没有删样本、静默重试或第二轮正式实验。每对差值、原始样本、CPU/I/O/可用 working-set、时间与二进制哈希见 `performance/`。CI 为预登记的 20,000 次 paired-median bootstrap；不是池化分位数。

| Workload / 指标 | AB 配对中位差 | CI95 | 门禁 |
|---|---:|---|---|
| Fixed Native Binding p50 | +0.8547% | [+0.8547%, +1.7094%] | PASS |
| Fixed Native Binding p95 | +0.7752% | [0.0000%, +1.5385%] | PASS |
| Native Record/Submit 256×256 p50 | +0.8333% | [-1.1494%, +2.7451%] | PASS |
| Native Record/Submit 256×256 p95 | +0.3322% | [-8.2126%, +5.5749%] | **PARTIAL** |

对照不隐去：

| Workload / control | p50 差与 CI95 | p95 差与 CI95 |
|---|---|---|
| Binding AA | 0.0000% [0.0000%, +0.8475%] | -0.7692% [-2.3438%, +1.5504%] |
| Binding BB | 0.0000% [0.0000%, 0.0000%] | +0.7874% [-0.7692%, +1.6000%] |
| Record/Submit AA | +0.4237% [-0.8333%, +1.6736%] | -4.5061% [-14.7727%, +11.0155%] |
| Record/Submit BB | -1.6327% [-6.0606%, +1.1765%] | -0.5803% [-17.3691%, +5.5432%] |

AA/BB 满足预登记的中位幅度及 CI 包含零规则，但 Record/Submit 尾部区间依然较宽。数据不能证明稳定 >5% 的生产回退，也不能排除它；不能把不确定性直接归因于 Windows/driver/frequency。旧 F5 +1.4925%、CI [-3.6199%, +7.9439%] 永久保留。所有热态样本首方 allocation count/bytes 为 0，没有新增 lazy Shader/PSO/Layout。

脚本随原工作负载输出的冷态 groups 仅为诊断。即使本轮诊断区间落入门限，**也不关闭 F4 C9 / complete cook I/O 的既有 PARTIAL 延期债务**。

## 7. Runtime 基线、边界与结论

原真实共享 Backing 工作负载，每配置三进程，24 warm-up + 240 measured frames。下表为三个进程各自分位数的中位数，不是新的相对性能门禁：

| Scene / View | CPU frame p50/p95 μs | Import preparation p50/p95 μs | Pacing p50/p95 ms |
|---|---|---|---|
| 1 / 1 | 67.3 / 116.7 | 3.8 / 5.0 | 8.4179 / 13.3791 |
| 1 / 4 | 138.1 / 201.4 | 7.1 / 9.5 | 8.4015 / 18.2949 |
| 2 / 4 | 156.9 / 242.9 | 7.3 / 9.1 | 8.3673 / 12.2433 |

九进程均有独立像素 oracle、一次 native compile、热态 0 allocation/bytes、无每帧 CPU wait；四 View cache hits=3。单 Scene 四 View 每 264 帧复用 792 次 Scene 工作；双 Scene 对应 528 次。VMA reserved peak 均 100,663,296 bytes（小尺寸机制 fixture，不与大尺寸 footprint 混用）。各 queue GPU timestamps **包含 submission gaps**，不是纯 Shader 时间；p50/p95/max 与所有样本在 `runtime-baseline/rows.json`、`summary.json`。保留了 5–25 次 deadline misses 和最高 102.777 ms pacing 样本，没有宣称稳定达到目标 FPS。

本轮剩余资格阻塞是 Record/Submit p95 统计门禁。功能/资源/ASan/真实 GPU/closure 通过不能替代它；不继续无界重复实验。F4 冷态债务单列保留。Win32 BGRA8/FIFO/maintenance1、1:1 relative extent、缓存高水位和未解决的在途扫描复杂度边界不扩大。Linux/Android、硬件真实 Device Lost、完整 installed SDK、产品构建和新业务场景为 NOT_RUN；注入 Device Lost 不冒充硬件失效复现。DataSchema/TypedStorage/Feature SDK 未实施。

```text
F5C_FUNCTIONAL_RESOURCE_LIFETIME = PASS
F5C_NORMAL_FULL_ASAN_GPU_CLOSURE = PASS
F5C_FIXED_NATIVE_BINDING = PASS
F5C_NATIVE_RECORD_SUBMIT_P95 = PARTIAL
F5-C = PARTIAL
F4_COLD_DEBT = PARTIAL (existing user deferral)
V2_PRODUCT = EXPECTED_UNAVAILABLE
F6 = NOT_AUTHORIZED
F7 = NOT_AUTHORIZED
STOP
```

不具备据本报告自动进入 F6 的条件；后续任何统计复核、门禁决定或阶段授权均由用户审阅后另行确定。

## 8. Implementation 完整变更清单

```text
cmake/render-v2-bootstrap/F5Runtime.cmake
cmake/render-v2-bootstrap/measure_f5c.py
cmake/render-v2-bootstrap/verify_f5c.py
docs/render-v2/F5C_IMPLEMENTATION.md
docs/render-v2/F5C_PERFORMANCE_PROTOCOL.md
modules/function/render/runtime/pinclude/RuntimeBackend.hpp
modules/function/render/runtime/src/Backend.cpp
modules/function/render/runtime/src/Frame.cpp
modules/function/render/runtime/src/Runtime.cpp
modules/function/render/runtime/test/Footprint.cpp
modules/function/render/runtime/test/HostFailures.cpp
modules/function/render/runtime/test/HostFaultRuntime.cpp
modules/function/render/runtime/test/ImportScale.cpp
modules/function/render/runtime/test/NativeInvocations.cpp
modules/function/render/runtime/test/shader/F5CReadBuffer.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/graph/Executable.hpp
modules/function/render/vulkan/src/graph/Compile.cpp
modules/function/render/vulkan/src/graph/Frame.cpp
```
