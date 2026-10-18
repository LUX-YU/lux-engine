# Execution 收敛：独立验证结果

## 结论与提交边界

**结构收敛及本轮 Render/Native 正确性：PASS（已测范围）；性能：PARTIAL；完整 Debug 矩阵：PARTIAL；Root TOOLCHAIN：BLOCKED（历史）。整体不授予无保留 PASS。**

这次确实由 stdexec 接管了异步 Command 的 connected OperationState 所有权，没有把旧 Owner 改名保留。普通 CPU 异步批次出现可测尾部退化，尚未完成因果归因；它与结构/生命周期通过分开记录。历史 IPC/Native/Alias 性能 PARTIAL、Debug `graph.edit` 失败均未改判。

| 提交 | 完整 SHA | Parent |
|---|---|---|
| 精确起点、推送前远端 | `6c9e95fc247a82e90430e6b503060896e735516f` | `a241cfc633909f963f9353009aff849142bf033f` |
| A：具体 CPU Scheduler | `52c52332c14a3a5157c9aa6b4447269918987090` | `6c9e95fc247a82e90430e6b503060896e735516f` |
| B/C/D：Scope 与独立结果交付 | `2531db5f5a79f06dd2b137bf532695a59fc6c455` | `52c52332c14a3a5157c9aa6b4447269918987090` |
| 最终 Implementation I | `a3923fe2600cca9d1c8fc9cce0cc4b2098ce93ab` | `2531db5f5a79f06dd2b137bf532695a59fc6c455` |

本报告所在的 Verification V 以最终 I 为 Parent，仅更新验收报告；V 自身 SHA 在提交后交付信息中提供，避免文件自引用。施工目录 `E:/RenderV2Rebuild/Execution/source`；独立验证目录 `E:/RenderV2Rebuild/Execution/verification-source`。后者由 `--no-hardlinks --no-checkout` clone 创建，精确 detached I、无 alternates，开始前运行 tracked snapshot，全部验证期间生产/测试源码保持 clean。

## 阶段结果与真实删除

| 阶段 | 结果 | 实际变化 |
|---|---|---|
| A | PASS | CreateEffect、Transfer::CpuPreparation 直接使用现有 CpuScheduler；删除 CpuSchedule/CpuSender/WorkSender 及绑定工厂 |
| B | PASS（正确性） | S/C 两方案真实运行，最终只保留 counting_scope；spawn 管理 connected state |
| C | PASS | 删除 PendingSlots、CompletionId、逐请求 stop source、operation 指针/删除器、ownOperation/operationStopToken 与关闭快照遍历；CommandCompletion 只拥有路由/结果 |
| D | PASS（已测 GPU 范围） | 真实 Upload、Readback、CreateEffect 使用同一外层 Scope；原 Native 状态机与退休算法不重写 |
| E | PASS（审计完成） | 六个模块逐消费者审查，DELETE/MERGE/KEEP/FUTURE 见 `EXECUTION_REDUNDANCY_AUDIT.md`；没有扩大到 Editor/Script |

生产 17 文件，新增 181 行、删除 350 行，净 **−169 行**；测试 15 文件，+159/−114；文档另计。唯一删除的生产文件是 `modules/core/execution/include/lux/engine/core/execution/WorkSchedule.hpp`（163 行），其仍有 Native 消费者的非拥有 CompletionPost/CpuResume/GetCompletionPost 移到 35 行窄头 `CompletionPost.hpp`。没有兼容转发头。CommandBinding 的专用终态 Receiver 被 upon_error/upon_stopped/then 替代。

CommandCompletion 的 `continuation` 仍是非拥有 Native CPU→Owner 回传，不是 OperationState Owner。CreateEffectSender::Operation/CpuReceiver 仍表达 CPU 编译→Owner 采用的真实线程切换，由 spawn 持有；本轮没有为消除它另造 OwnerScheduler。CommandDispatch.cpp 保留公开模板访问私有实现所需的窄函数，未暴露 ServerImpl。

S 使用 simple_counting_scope 加一个 Server stop source，Release all/第二轮 no-work/146 项全部通过（153.58 s）；S 的 Debug/ASan 为 NOT_RUN，未留下生产策略开关。C 使用 counting_scope 内置停止源，CompletionEnvironment 不再注入第二个 token，避免 E0-C 原型中的双停止源融合。C 的最终独立矩阵如下。选择 C 是因为少一份自管停止状态和转发接口，不是因为原型大小更漂亮。

## 独立构建与运行

| 配置 | 全量 all / 第二轮 | CTest | 结论 |
|---|---|---|---|
| RelWithDebInfo | PASS / no work | 146/146，157.05 s | PASS |
| Debug | PASS / no work | 145/146，170.66 s；仅 `graph.edit` 失败 | PARTIAL，历史项未修改 |
| MSVC ASan | PASS / no work | 146/146，218.59 s | PASS，ASan error=0 |
| C++20、`/GR-`、禁异常展开 | PASS / no work | 146/146，154.85 s | PASS |

最后一项是独立配置 `/EHs-c- /D_HAS_EXCEPTIONS=0`，默认工程的异常选项没有改变。MSVC 对覆盖 `/EHsc` 的 D9025 警告保留；生产不主动 throw。四套均为 C++20 `/GR- /we4541`。MSVC 14.44.35207、Ninja、CMake 3.31.10；i9-13900H，RTX 4070 Laptop，驱动 617.42，Vulkan API 4211039。stdexec 0.10.0/upstream `7d704575019a1405b2067c5e47265245c55ca66e`。

原有 146 个 CTest 名称逐项一致，没有禁用/删除测试。公共头独立编译是 all 中的对象目标，不虚增 CTest 数量。所有配置都实际执行独立安装、前缀移动及消费者，包括 Core Execution、纯 Client 禁 Vulkan、外部 Storage、Native 和 IPC；日志中保留各消费者子 CTest。

GPU Validation + SyncValidation 启用，所有适用测试的 error=0。保留系统 `VK_LAYER_EOS_Overlay` 重复注册警告（fixture 常报告 warnings=1）；未静默丢弃 warning。Debug 失败来自未改动的通用 `modules/function/graph`，不是 Render Graph 或新 Scope 测试；未修复、免验或改成 PASS。

关键合同由原测试及迁移后的长期测试覆盖：

- `render.runtime.async`：同步 value/error/stopped、反序突发、稳定结果节点、停止与 final publication 尾栈；spawn 可在同步 terminal 内销毁状态，精确析构断言保留。
- `render.runtime.async_dll_tail`：跨 DLL Producer terminal 返回尾栈、Host join/Code Pin；Scope join 不冒充外部回调已经退栈。
- `render.runtime.typed_client`、`typed_batch`、`handlers`、`lifetime`、`shutdown`：同数值 Client-local RequestId、Outbox 背压、断连、晚结果、类型与错误合同。
- `render.runtime.gpu_ready{,_transfer,_same_family}`：真实 Copy/数据核对、三拓扑、提交后 unknown、停止、DeviceLost/部分失败、Main 64 回复停顿 1.5 s、Upload/四 View 持续进展及资源精确清偿。
- `render.native.buffer_transfer/transfer/last_use/execution`、Texture 系列：Source 保活、CPU 队列满/停止、Mip/BC/Region、真实 Readback、LastUse、GPU 退休。Fault injection 只在原测试设施中，真实正常 GPU 路径同时执行。
- `render.runtime.async_cost`：0/1/16/128，实际 ExecutionRuntime，同步/普通/突发完成；实际处理 K 与事件数一致，丢失=0；突发峰值=N。

执行命令以归档 runner 的完整参数/返回码为准：

```powershell
git clone --no-hardlinks --no-checkout <implementation-repository> <verification-source>
git -C <verification-source> checkout --detach a3923fe2600cca9d1c8fc9cce0cc4b2098ce93ab
cmake -DLUX_SOURCE_DIR=<verification-source> -P <verification-source>/cmake/ValidateTrackedSnapshot.cmake
cmake -S <verification-source>/cmake/render-v2-bootstrap -B <build> -G Ninja `
  -DCMAKE_BUILD_TYPE=<RelWithDebInfo-or-Debug> -DBUILD_TESTING=ON `
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset `
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx `
  -Dstdexec_DIR=D:/DevTools/vcpkg/installed/x64-windows/share/stdexec `
  -Dstduuid_DIR=D:/DevTools/vcpkg/installed/x64-windows/share/stduuid `
  -Dtomlplusplus_DIR=D:/DevTools/vcpkg/installed/x64-windows/share/tomlplusplus `
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF `
  -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF -DCMAKE_INSTALL_PREFIX=<build>/sdk
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> -V --output-on-failure -j 1 -R .
```

ASan 使用 `/fsanitize=address /Zi` 与 `ASAN_OPTIONS=alloc_dealloc_mismatch=1`。全部构建和运行串行。Native 测试显式部署真实 Core Execution DLL closure；独立 V 没有借旧 SDK DLL 修补源树。

## 性能：真实配对，不授予全面 PASS

精确旧基线 6c9e95 的另一 clean clone 重新 all 构建/第二轮 no-work；新版本使用最终 I 独立 V 的 Release 二进制。6 类程序 × 旧新 × 10 对，共 **120 个独立进程**全部正常退出，每对交换先后顺序，固定同机。小 Command、Native record 各进程 1000 个正式样本；GPU-ready 1000 个成功测量请求，背压拒绝记录另保留；CPU 批次各模式/N 预热 5、正式 100 次。所有正式样本开启相同 Validation，不与采样器或分配探针并发。

下表单位 µs。每格为十个进程各自分位数的中位数，顺序 **p50 / p95 / p99 / max**；最后列为逐对 p95 比值变化的中位数及按 run 配对 bootstrap 95% 区间，因而不等于表中两个中位数直接相除。区间只反映本机本时段，未控制温度/功耗/核心迁移，不构成跨机器因果结论。

| 测量 | 旧 | 新 | p95 配对变化 [区间] |
|---|---|---|---|
| 同步小 Command RTT | 0.80/0.90/1.90/7.75 | 0.80/0.90/1.90/8.60 | +0.00% [-5.00%, +5.56%] |
| CPU 异步 128 项 RTT | 151.10/218.25/249.30/260.55 | 175.30/240.85/267.85/291.60 | +12.87% [+2.73%, +19.33%] |
| CPU 异步 128 项发布→Owner 领取 | 44.75/114.00/144.60/183.15 | 57.35/146.40/180.05/236.95 | +22.17% [+12.19%, +32.27%] |
| CPU 单项调度等待 | 24.60/51.60/62.45/74.20 | 23.85/51.50/63.65/82.30 | +0.22% [-6.31%, +10.31%] |
| Native record/execute | 120.50/186.10/222.70/1478.05 | 117.80/189.75/228.55/1539.25 | +1.10% [-7.14%, +10.05%] |
| GPU-ready 入站→消费 | 2404.25/14957.20/16595.80/18503.35 | 2279.40/14938.45/16623.45/19660.40 | -1.99% [-22.24%, +3.82%] |
| 四 View 稳定间隔 | 393.50/636.55/751.85/2042.30 | 389.05/607.45/759.95/2046.60 | -4.92% [-14.67%, +8.98%] |
| Alias 上传期四 View（探索性） | 479.80/775.95/1709.00/2645.20 | 455.90/736.85/1594.65/2636.25 | -7.00% [-10.69%, +21.90%] |

CPU 普通 128 项 RTT 的 p95 +12.87%、发布→Owner 领取 +22.17% 是需要继续调查的退化，不能凭代码推断为 mutex、分配器或驱动的单一原因。GPU-ready 长尾包含真实 Fence 观察、调度与有界空闲等待，不能把约 15 ms p95 当作 IPC 队列时延。固定 1ms 有界空闲等待策略未改动；burst 控制等待及 OS GetThreadTimes 的量化限制在原始 CSV/fixture 输出中保留。

固定 64 MiB Alias 上传，64 Chunk：总 wall 的十次运行中位数 53.470→53.934 ms，配对 +1.09%（区间 −2.94%～+5.40%）；CPU preparation 4.351→4.431 ms；GPU timestamp 累计 5.516→5.527 ms。每进程该资产只有一次测量，因此不伪称这是进程内 p95。全部 1/16/64MiB、Mip/BC 与三拓扑数据保留。

Alias 四 View 上传程序是时间窗口负载，完成上传次数会变化，因此其帧间隔仅为探索性；没有等价总上传量就不下“更快/无回归”结论。Native record/execute 排除 Fence wait，GPU timestamp NOT_MEASURED；GPU-ready 也为 GPU timestamp NOT_MEASURED，只有固定 Chunk fixture 的真实 timestamp 单列。Validation-off、VTune、Linux、Android、TSan 本轮 NOT_RUN。

## 分配、状态尺寸和峰值

正式时延程序不装测量 allocator。另在旧/新施工测量树各跑一进程，EXE 全局 new 计数器只计入 RenderThread 测量窗口分配、允许任意线程释放；每模式/N 100 次。测量补丁/源文件/命令全部归档，之后逐字节恢复 tracked 源码；独立 V 源码未修改。请求字节不含 allocator header/alignment、CRT 元数据、Core DLL/driver 内存，也不是整个进程 RSS。

| 实际路径 | 旧 | 新 |
|---|---:|---:|
| 同步 Handler 小 Command（预热，EXE + IPC DLL） | 0 分配 | 0 分配 |
| 立即完成的异步 Sender，每项 | 1 次 / 184 B | 2 次 / 488 B |
| 普通/突发 CPU 异步 Sender，每项 | 3 次 / 416 B | 2 次 / 488 B |
| GPU-ready UploadProbe，首方 new/请求 | 4 | 5 |
| CPU burst N=128 累计申请 | 384 次 / 53,248 B | 256 次 / 62,464 B |
| CPU burst N=128 被测存活峰值 | 145,930 B | 82,807 B |
| 批次结束后被测保留字节（此顺序） | 98,778 B | 20,343 B |
| Server 关闭后被测存活字节 | 0 | 0 |

旧表保留页被删除，但 Outbox 等真实容器仍保留容量；新峰值不能解读为固定上限。Scope 不是全局 admission/字节预算，原 request_work_budget 也只是每轮 dispatch 工作量；持续接受而不结算仍可能增长，本轮没有新造通用预算管理器。

| MSVC 实际类型布局 | 旧 B | 新 B |
|---|---:|---:|
| Native CpuPreparation（不含其旧擦除后端额外分配） | 112 | 80 |
| CreateEffect connected Operation（不含旧擦除后端额外分配） | 392 | 360 |
| CreateEffect 完整 spawn state（包含上述 Operation） | N/A | 456 |
| Readback 完整 spawn state | N/A | 336 |
| Pending / 独立 CommandCompletion | 272 | 224 |
| counting_scope（整个 Server 一个） | N/A | 40 |
| simple_counting_scope（S 对照，未含外置 stop source） | N/A | 16 |
| IpcMessage / ResponseRoute | 96 / 32 | 96 / 32 |

CreateEffect 新 spawn 456 B 已含其 360 B 内部状态，不能再次相加；独立结果记录 224 B 另计。旧 392 B 来自只在外部诊断源副本中的编译期 sizeof 诊断；该探针有意编译失败，不是产品构建失败。所有正式配置无此代码。四 View 稳定帧首方 new=0；原 Pipeline/Bind/Scope/Draw 合法复用与像素断言保持。

结论是**减少手工所有权和部分 CPU 擦除分配，同时付出独立结果记录、部分路径额外分配及可见的异步尾延迟成本**。不能宣称零开销，也尚不能证明所有代价均可接受。下一项最小验证应对 128 项完成积压中的 Owner 领取/释放成本做受控归因；不先增加池、线程或 Manager。

## 保护、失败尝试与范围

719 Legacy Git blobs、主工作树 settings/Graph.hpp、V12 工作树用户 RenderServer.hpp 的 SHA 全部一致。IPC SPSC/Channel/Link、Graph Analysis、Native Submission/Retirement/Execution 与 Transfer resumeCpu/resumeGpu/continueTransfer 主算法保护检查通过。lux-cxx=`bf779515a120350c7c5412c366eea73afdf59ccf`，lux-cmake-toolset=`c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`，依赖工作树 clean。

公共头三前缀同步共 18 项动作（含删除旧 WorkSchedule、新增 CompletionPost），从精确 I 的 git blob 写入，原文件备份及前后哈希在证据中。Android 仅同步头，不授构建资格。

Root TOOLCHAIN 真 configure 返回 1：仍引用不存在的 `render/client`、`render/features/cmake/feature_client.cmake`、`render/standard_content`，引出安装目标错误。完整产品 BLOCKED，未用 Legacy、Stub 或关闭测试绕过。上层 Editor/Script 没有额外实施或运行资格。

保留的失败尝试：A 初次构建头清单/测试命名空间遗漏；修复后首次 Release 130/146，16 项 Native EXE 缺 Core DLL 以 0xc0000135 失败，随后通过实际 CMake DLL 部署修复，32/32 Native 重跑及最终全矩阵通过；独立 Debug 后 runner 原本未识别 `graph.edit` 的历史失败而返回 8，修正外部编排后从 ASan 继续，V 源码未变。S 候选通过后按设计还原 C；未将 S 的 Release 资格冒充其 Debug/ASan 资格。

冻结所有历史 E0/E0B/E0C 报告，不重写过去的 PARTIAL。额外审计未发现能在本轮消费者闭包内安全删除的另一独立长期组件；TaskScope 内部 Owner、OperationInbox 等列 FUTURE；CompletionWork/PortSender/Timer/Task DAG 和真实 GPU 状态列 KEEP，理由见审计。

## 可携带证据索引

- Archive：`E:\RenderV2Rebuild\archives\a3923fe2600cca9d1c8fc9cce0cc4b2098ce93ab\execution-convergence-evidence.zip`
- ZIP SHA-256：`be1e31a42d771d0019566104482f88b13a91be7e2a1998a3f5a6e8a5217ce419`
- 内含 `SHA256SUMS.json` SHA-256：`202e048a21f71699784e31e3093d588a50b0e39fe611bda7bae2fd43169ea0bd`
- 870 个文件，ZIP 63363767 字节；按精确 I 归档，只读保留，不用新结果覆盖旧失败。

| 包内路径 | 内容 |
|---|---|
| `evidence/baseline.json`、`environment.json`、`final-state.json` | 起点、依赖、用户保护、环境、推送前远端 |
| `evidence/verification-matrix.json`、`verification-evidence/` | 独立构建/CTest/返回码、原始日志与 CSV |
| `evidence/build-metadata/` | 全部配置 flags、compile_commands、Ninja target 链、安装清单 |
| `evidence/test-name-regression.json`、`diagnostics.json` | 146 名称一致、GPU/ASan error 检索和实际 GPU 输出 |
| `evidence/a*/`、`evidence/c*/`、`evidence/s*/`、`s-candidate.patch` | 阶段尝试、S/C 对照及回退；S 补丁实际位于 `evidence/` |
| `performance/` | 120 进程命令/二进制哈希、所有样本及离群值、逐 run 分位数、配对统计 |
| `allocation*/`、`evidence/allocation-summary.json` | 外部分配探针/完整补丁、计数/累计字节/峰值，原文件备份 |
| `evidence/layout/`、`layout-summary.json` | 精确生产 TU 的 MSVC 布局与外部 sizeof 诊断；汇总位于 `evidence/` |
| `evidence/protection-*.json`、`code-change.json` | 白名单、719 blobs、用户 SHA、净行数；汇总均位于 `evidence/` |
| `sdk-backup/`、`evidence/sdk-sync.json` | 三前缀原头、删除/替换清单及哈希 |
| `evidence/root-configure.console.log` | 真实产品配置阻断 |

本报告仅授予表中明确的已测范围。此次到 A–E 收口为止，不自动开始额外 Native 状态复用或其他渲染阶段。
