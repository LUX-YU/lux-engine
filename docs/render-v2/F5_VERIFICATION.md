# RD2-F5 独立资格验收 — PARTIAL

本报告仅记录锁定实现的独立验收结果，不修改历史报告。F5 功能、生命周期、Normal/Full ASan 与实际依赖闭包通过；**Native Record/Submit 热态 p95 的统计门禁尚未关闭，F5 整体为 PARTIAL，不是 PASS。** 用户只延期了 F4 冷态债务，本报告不将该延期扩展至 F5 热态。

## 1. 提交身份与证据

- 批准入场：`719f3d06e077e639901ae598f230653c5312c905`。
- 最终 Implementation I：`e662ff59844cf69f00d94341768df58ddc7f44b0`。
- 比较基线 F4-C-P I：`92b126f67873ccbb576d0ffc5f1330964d6f84f6`；F4-C I：`5039fb42424dfda23bc7da518e7bbcf863f8c932`。
- Frozen V1：`a669409a289a6fa4092f21176397795b1cdb7f3e`。
- Verification V 仅新增本文件；准确 V SHA 由提交产生，不在报告中自引用。
- 独立检出：`D:/LuxQualification/render-v2-f5-e662ff59844c/source`。通过 `clone --no-hardlinks --no-checkout` 建立，精确检出 I；无 alternates，Git object 链接数为 1，施工期间未向该 clone 补写源码。
- Normal/ASan 输出：同级 `build/`、`asan/`，均在源码树外。
- 主证据目录：`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/RD2-F5/e662ff59844cf69f00d94341768df58ddc7f44b0/`。
- `evidence-manifest.json` SHA-256：`27cee8c554632bedc3207a85dac980e8a8fd3ece4f05fd22ab1febe70eeab943`。逐文件大小/哈希与检查脚本、原始命令/退出码、File API、Ninja deps、Link Map、Shader/codegen 输入、性能全部样本一并保留。
- 归档复制时 E: 空间不足；仅将本轮新复制的产物目录移至 `D:/LuxQualification/render-v2-f5-e662ff59844c/evidence-artifacts/` 后完成复制。主目录 `build-artifact-identities.json` 逐项记录外置产物路径、字节数和哈希；`external-artifact-verification.json` 已复核。原构建产物和历史证据未删除。

准入延期与下层变更的消费者/最小失败证据在 [F5_ENTRY_DECISION.md](F5_ENTRY_DECISION.md)、[F5_NATIVE_IMPACT.md](F5_NATIVE_IMPACT.md)。实际职责和增量测试依据见 [F5_TYPE_INVENTORY.md](F5_TYPE_INVENTORY.md)、[F5_TEST_IMPACT.md](F5_TEST_IMPACT.md)。正式比较在 [F5_PERFORMANCE_PROTOCOL.md](F5_PERFORMANCE_PROTOCOL.md) 预先固定。

## 2. 实际实现及所有权证明

| 责任 | 实现、所有权及边界 |
|---|---|
| 不可变 Native Plan | `ExecutableGraphPlan` 持有共享不可变 NativeGraphCode；LogicalPlan、Shader/PSO/Layout、Graph/Sync2 recipe 为同一事实。初始 Import backing、allocator、动态参数不再长期借入 Plan。目标尺寸 specialization 共享精确的 native program owner。 |
| 独立 Invocation | `NativeGraphInvocations` 为 move-only bounded owner，拥有实际 backing、view、descriptor、scratch、输入快照与 queue tickets。四 View 的 2/3 FIF 使用 8/12 invocation slots；FIF 不等于 View/Invocation 数。逐次 submit 不复制共享 owner。 |
| 唯一执行域 | 短生命周期 RenderRuntimeBuilder 完整冷态装配后一次 `finish() &&` 移交；RenderRuntime 唯一拥有 R2 Transport、VulkanBackend、OS thread/event/timer。只有 Backend 线程推进 native 工作，没有第二 Renderer、FrameLoop 或数据存储系统。 |
| Scene/View/Target | Core SlotMap + endpoint owner 身份；Scene 持久值、View Camera/history/path/target、Target 输出顺序各自拥有。观察计数不是调度 authority。Native asset backing 由 Backend 唯一拥有，两个 Scene 借用同一实际 VkBuffer；仅证明 F5 机制。 |
| Scene 工作复用 | 使用既有 F2 `mayShareSceneInvocation`，核对输入、revision、Invocation scope 和副作用。只有同 frame 的合法 donor 可复用，consumer 的真实 queue completion 扩展 donor last-use；有非共享 writer、修订变化或 backing alias 时拒绝复用。 |
| Shared Buffer 分区 | 冷装配在 Scene 创建前汇总同一 actual backing 的各路径声明边界，经现有 Native Sync2 compiler 细分 cells；候选计划事务发布，Shader/PSO/Layout 仍共享。CPU ready 只保留在未被 GPU 触及的 cells，GPU 已用 cells 使用实际 tickets。未改 F2 版本算法。 |
| 独立进度 | 限额排空 CONTROL/PROGRAM；Win32 wake event + 高精度 deadline timer pacing。无新 PROGRAM 仍产生合法帧；无 active output 时等待新工作，不以空队列轮询或频繁 sleep 模拟调度。 |
| 输出与退休 | Target-relative 1:1 尺寸候选事务、bounded extent cache；旧 variants/backing 走既有 Retirement。Swapchain acquire/render/present 证据分离，Present fence 证明真实显示系统使用结束。每资源常态路径不依赖 waitIdle。 |
| 回复与失败 | RuntimeRequest 为 R2 inbox/ticket 的 move-only 接收责任，terminal poll 消费，析构 abandon；安全跨 Runtime 销毁。独立有界诊断 SPSC 出口不占 request reply 语义。Device Lost 为当前 Runtime 终态，不自动重建。Stop 关闭接纳、排空已接纳工作、等待真实 GPU 使用并 join。 |

原生变更限于既有 device/Submission/Graph/F3 Bindings 接缝，不创建新 Shader parser、Graph 调度器、资源 Registry 或 Feature Framework。公共 Feature API 的最终模块和插件 ABI 尚未实现，不能以冷态 NativeAssembly 冒充 F6 SDK。

## 3. 独立构建、ASan、依赖与保护

| 门禁 | 结果 | 原始证据 |
|---|---|---|
| Tracked snapshot / clone isolation | PASS | `tracked.log`、`clone-isolation.json` |
| Normal 完整 all `-j 4 -- -k 0` 两轮 | PASS，第二轮 `ninja: no work to do.` | `normal-all-1/2.log` |
| Normal CTest | **168/168 PASS，52.92s** | `normal-ctest.log` |
| Full ASan 完整 all 两轮 | PASS，第二轮无工作 | `asan-all-1/2.log` |
| Full ASan CTest | **168/168 PASS，119.42s** | `asan-ctest.log` |
| 实际 source/include/link/codegen closure | PASS，Normal/ASan 均复核 | `normal-closure/`、`asan-closure/`、`production-dependency-directions.json` |
| 原测试义务 | 原 156 个测试名全部保留，新增 12 项 | 两套 `test-obligations.json`；fixture API 迁移保留旧 oracle |
| 719 Legacy 文件 | PASS，逐文件 blob/mode/size 一致；含保护 CMake 共 720 项 | closure、原冻结 manifest |
| Core/Transport/F2 Graph/Shader Generator/Description | Git tree/blob 与批准入场相同 | `protected_objects` |
| FINAL/RD2、全部历史报告、Root/Product/Scene/Editor | 未修改 | 允许范围检查、Git diff 和保护对象 |
| 用户原始修改 | 两检出 HEAD/branch/status/staged/unstaged/untracked 及七个脏文件字节哈希相同 | `preflight/`、`user-protection.json` |
| 三安装头前缀同步 | 27 项字节核对 PASS | `header-sync-check.json`；仅头同步，不是 SDK/Android 资格 |

MSVC 19.44.35228 / 14.44.35207、C++20、Ninja、RelWithDebInfo、Vulkan SDK 1.4.304.0。Full ASan 对全部 C++ translation units 使用 `/fsanitize=address`，采用已有匹配 ABI 的 SPIRV-Cross ASan 依赖，未屏蔽 annotations。准确配置、编译/链接参数、工具输入以归档 CMakeCache、compile_commands、build.ninja 和 File API 为准。

实际 DAG：Core 无反向依赖；Transport/Graph/Foundation 基于 Core；Native Graph 基于 Core/Graph/Foundation；Runtime 公有中立 Core/Transport/Graph，私有 Native 依赖。`render_runtime_operations_generate` 是既有 luxop 的 UTILITY 构建依赖，不是新的 linked framework。Foundation 不 include Native Graph/Runtime，Native Graph 不 include Runtime。四个中立 Runtime 公共头无 Vulkan，NativeAssembly 是明确的冷态 native 头；五个头均单独编译。热态 link maps 无 Shader compiler/SPIRV-Cross，profiling wrappers 只存在于测试 profiling binary。

前两个候选没有被改写成成功：`ac5312c6de0f...` all-build 失败于漏迁移的 ShaderHotLink，55 个可运行受影响诊断通过，缺失 executable 仍 NOT_RUN；`77a97ee3754d...` Normal 168/168 后 closure checker 漏列真实 UTILITY generator，Full ASan 尚未开始。均回施工检出提交修正，最终 I 重新完成两套全量资格。原 clone、失败日志及开发期真实错误保留，身份见 `preserved-prior-evidence.json`。

## 4. 新增测试及真实 GPU 证据

GPU：NVIDIA GeForce RTX 4070 Ti，driver raw `2480242688`，API `1.4.325`（raw 4211013），graphics/compute/transfer families 0/2/1。启用 Khronos validation 和 synchronization validation 的真实 GPU fixtures 正常通过；**Normal/ASan 所有显式 validation error 记录均为 0**。警告计数保留在原日志，不宣称零警告。性能 binary 关闭 validation，不能用性能日志替代 validation 资格。

| 新增义务 | 实际验证 |
|---|---|
| native_invocations | 一份真实 compute/graphics Native Plan，四 View、2/3 FIF；曝光不同，全像素 CPU Tonemap oracle，Scene 复用正例及 revision/副作用/alias 负例；Plan handle 提前销毁后执行 owner pin 保活。 |
| target_extent | 4x4→6x3 真实 allocation/copy/descriptor 范围；精确 program owner 地址共享，零 extent 拒绝，失败候选保留旧可读输出。 |
| progress | 一 Scene 四 View；无 PROGRAM 至少 24 帧，10 次 100ms publication 与约 120Hz 帧进度独立；全部 minimize 后停止帧进度、恢复后继续，数值像素证明、reply abandon/stop。 |
| shared_asset | 两 Scene 一次上传同一实际 VkBuffer，不同 Scene 值产生独立像素；不同 RenderPath 加入真实 Tonemap graphics；删除一 Scene 不影响另一 Scene/backing。 |
| presentation | 两个真实 Win32 Surface/Swapchain；同一 View 输出复用到两个 Target，逆创建顺序仍按显式 Layer 产生正确 base/rect BGRA 像素；双 minimize、32x32→48x24 resize、错误路径 last-good rollback。 |
| lifecycle | 实际 Runtime + 内部故障 seam；候选 GPU allocation rollback、foreign owner/wrong thread/stale View、retained immutable publication、in-flight removal、ticket 跨 Runtime 销毁、首次/部分 submit 后 Device Lost。 |
| history | 四 View 各自实际私有 GPU Buffer，3 FIF 累计计数与 frame serial 对账；Camera 1–4、RenderTime、HistoryEpoch 真实 Shader 输出；仅 resize View 的 history epoch 改变，非法 generated scalar-field 绑定拒绝。 |
| shared_asset_partial_range | 32-byte backing 只使用 16-byte 区间，保留未使用区间 CPU 初始化证据；真实 GPU 像素。 |
| shared_asset_partition_refinement | 两条路径分别访问同一 backing 的 [0,16)、[0,32)，冷态事务细分、共享实际 program，运行输出正确。 |
| public_contract / reject_ID / reject_REQUEST_COPY | 真正 C++20 值/所有权合同，跨类型身份和接收责任拷贝编译拒绝。 |

全部旧 F3/R4/F4-C GPU Shader/layout/alias/import/retirement/draw、C1/C2/C3 与 UINT32 draw 继续执行。Runtime fixtures 有数值/图像 oracle，而非仅判断 VkResult 成功。Warm census 检查首方 C++ new count/bytes=0，无 lazy Shader/PSO/Layout creation、无无故热态 Graph compilation。

Device Lost 是确定性故障注入，测试用 seam 先合法排空仍健康的 GPU 再注入，避免把模拟设备死亡等同于真实硬件取消工作。**真实驱动 Device Lost 未人为触发，NOT_RUN**；不宣称已经复现实际设备故障。

## 5. 性能：固定实验、全部样本和未关闭门禁

CPU i7-13700KF，24 logical processors。A 二进制与原 F4-C-P 归档 SHA 一致；B 为独立最终 I。完整原工作负载，冷总量包含代码与 invocation/backing 创建。每 workload 11 A/A + 11 B/B + 21 A/B，独立进程、交替顺序、固定 seed、affinity mask 4、AboveNormal，无并行构建/GPU 测试，共 **172 个进程**。没有删除样本、追加有利实验或对 ASan/validation timing 作直接比较。

下表为每对进程百分位数差值的中位数及 20,000-resample bootstrap CI95，不是池化全部进程后的百分位数。每个原始样本/配对/命令/退出码保留在 `performance/`。

| 指标 | A/B p50 [CI95] | A/B p95 [CI95] | 判定 |
|---|---|---|---|
| C9_identity_and_candidate | -2.0833% [-5.7692%, -2.0000%] | -2.9851% [-10.0000%, +3.0769%] | PARTIAL |
| complete_native_candidate_including_cook_io | -0.3563% [-0.8748%, +0.6630%] | -1.7377% [-7.4222%, +5.3898%] | PARTIAL |
| native_graph_compile_prebuilt_programs | +3.2184% [+2.3641%, +5.1643%] | -3.5413% [-7.9070%, -1.0067%] | PARTIAL |
| fixed_native_binding_recipe | +1.7094% [+1.7094%, +1.7241%] | +0.0000% [-1.5625%, +1.6000%] | PASS |
| native_graph_record_submit_256x256 | +0.4219% [+0.0000%, +2.1368%] | +1.4925% [-3.6199%, +7.9439%] | PARTIAL |

| 指标 | 自比控制 | p50 [CI95] | p95 [CI95] |
|---|---|---|---|
| C9_identity_and_candidate | AA | +0.0000% [-5.7692%, +12.5000%] | +3.3898% [-13.8462%, +12.9630%] |
| C9_identity_and_candidate | BB | +6.1224% [+2.0833%, +10.6383%] | +13.5593% [-6.1538%, +23.6364%] |
| complete_native_candidate_including_cook_io | AA | +0.0912% [-0.9835%, +1.2724%] | +6.1546% [-0.2489%, +13.0856%] |
| complete_native_candidate_including_cook_io | BB | +0.3816% [-0.5141%, +1.4537%] | -0.6937% [-2.6854%, +5.5749%] |
| native_graph_compile_prebuilt_programs | AA | -0.9195% [-5.8442%, +5.9666%] | +2.2472% [-10.8731%, +17.3489%] |
| native_graph_compile_prebuilt_programs | BB | +5.5202% [+2.4887%, +14.9378%] | +7.5704% [+2.3569%, +9.6429%] |
| fixed_native_binding_recipe | AA | +0.0000% [-0.8475%, +0.0000%] | +0.0000% [-1.5625%, +0.7874%] |
| fixed_native_binding_recipe | BB | +0.0000% [-0.8403%, +0.8475%] | +0.0000% [-5.3435%, +0.8065%] |
| native_graph_record_submit_256x256 | AA | +0.0000% [-4.4534%, +2.1008%] | +0.8989% [-7.6446%, +26.0465%] |
| native_graph_record_submit_256x256 | BB | +1.6807% [-1.2552%, +6.1983%] | -0.4246% [-4.9896%, +9.8214%] |

**固定 Binding 热态 PASS。Native Record/Submit p95 点估计 +1.4925%，但 CI95 上限 +7.9439%，仍跨 +5% 门限；故 F5 热态资格 PARTIAL。** 自比亦有明显尾部波动；现有证据既不能认定稳定 >5% 生产回退，也不能证明其已被排除。没有擅自归因于算法、驱动或测量噪声，没有据此重写 F2、选择有利样本或豁免门禁。按预先冻结的有界协议停止重复测量。

冷态 C9/完整 cook I/O/Native construction 的控制或 CI 也未关闭。保留 F4-C 原不利 p95 +6.5574% / +5.9257% 以及 F4-C-P 原完整样本；本次冷态结果不重写历史债务，也不将更低点估计冒充 PASS。后续若收尾，须先冻结能改善尾部辨识力的受控方案，同时重测锁定 A/B，或由用户明确裁决；本报告不自动提出豁免。

## 6. 新 Runtime 多 Scene/View 基线

不存在同功能旧 Runtime 可作百分比比较。真实 2x2 RGBA32F shared-UBO compute/readback，三配置各三个进程，每进程预热 24 + 计量 240 帧。profiling 包含实际生产 Frame/Native preparation 源的测试专用 wrapper，maps 证明不进入生产 ABI。GPU timestamps 是每队列区间、包含提交间隙，不是纯 Shader kernel 时间；import 项包含验证和 binding preparation，是 CPU frame 的子集，不能再相加。

表中 p50/p95 为三个进程各百分位数的中位数，max 为全部进程最大值，单位 µs；完整逐进程数据见 `runtime-baseline/rows.json`。

| Scene/View | 计量项 | p50 | p95 | max |
|---|---|---:|---:|---:|
| 1/1 | compute_GPU_interval_including_submission_gaps | 16.160 | 62.112 | 1509.664 |
| 1/1 | graphics_GPU_interval_including_submission_gaps | 114.528 | 335.616 | 1621.024 |
| 1/1 | native_import_validation_and_binding_preparation | 0.600 | 2.800 | 4.900 |
| 1/1 | runtime_CPU_frame_including_import_validation_record_submit | 40.500 | 67.700 | 221.500 |
| 1/1 | runtime_pacing_interval | 8282.100 | 8775.600 | 12701.300 |
| 1/1 | transfer_GPU_interval_including_submission_gaps | 113.216 | 1049.664 | 1470.400 |
| 1/4 | compute_GPU_interval_including_submission_gaps | 63.328 | 141.344 | 1696.320 |
| 1/4 | graphics_GPU_interval_including_submission_gaps | 150.528 | 320.928 | 1852.224 |
| 1/4 | native_import_validation_and_binding_preparation | 2.200 | 7.400 | 14.200 |
| 1/4 | runtime_CPU_frame_including_import_validation_record_submit | 95.800 | 149.300 | 1401.700 |
| 1/4 | runtime_pacing_interval | 8310.000 | 9299.000 | 13356.000 |
| 1/4 | transfer_GPU_interval_including_submission_gaps | 138.176 | 1187.360 | 1663.680 |
| 2/4 | compute_GPU_interval_including_submission_gaps | 79.136 | 257.632 | 2910.624 |
| 2/4 | graphics_GPU_interval_including_submission_gaps | 160.160 | 1497.504 | 2998.336 |
| 2/4 | native_import_validation_and_binding_preparation | 2.100 | 3.900 | 14.500 |
| 2/4 | runtime_CPU_frame_including_import_validation_record_submit | 123.600 | 163.800 | 3010.600 |
| 2/4 | runtime_pacing_interval | 8309.400 | 8724.300 | 13733.900 |
| 2/4 | transfer_GPU_interval_including_submission_gaps | 149.824 | 1257.312 | 3054.464 |

九进程 warm allocation count/bytes 均 0；编译次数均 1，四 View 相容计划 cache hits=3，计量期没有热态编译。每 264 帧四 View 的 Scene reuse 为单 Scene 792、两 Scene 528（个别进程停止采样后额外完成一帧，原计数原样保留）。VMA reserved peak 为 100663296 bytes（96 MiB），包含 allocator block reservation，不等于业务有效载荷或全进程/驱动显存峰值。OS waits 有记录，deadline_misses=0；pacing 原始尾部仍保留，不保证每帧达到固定 FPS。该小机制 fixture 不是产品场景性能，也不是 F7 实际资产数据链。

## 7. 边界、未运行事项与下一阶段条件

- `F5 = PARTIAL`：热态 Record/Submit p95 统计门禁未关闭。功能/生命周期及本机完整回归 PASS 不替代该门禁。
- `F4_COLD_DEBT = PARTIAL`：仅沿用有限延期，不声明 F4 整体 PASS。
- `TARGET_RELATIVE` 当前明确为 1:1，nominal width/height=1；任意相对比例表达和 DYNAMIC shape 未实现，不是设备 fallback。已验证真实 Target Signature/resize/variant cache 路径。
- WSI 当前 Win32、BGRA8_UNORM/SRGB_NONLINEAR、FIFO、显式 swapchain maintenance1/present-fence 能力；缺少能力返回明确错误。其它平台、输出编码和 present mode 未取得资格。Target opaque blit composition 不是 UI alpha compositor。
- Scene/View/Target 创建在冷态 Builder 完成，公开 live controls 覆盖 publication、path/active/resize/removal/readback/stop。冷态 assembly borrow 在移交后失效；不宣称已经提供动态插件装配或 F6 SDK。
- 已有 luxop 模板生成 `RuntimeMessages.ops.hpp` 含当前 clean source 的绝对 include。此次使用真实生产 generator 并同步精确产物，没有手写修补模板；**完整 installed SDK portability = NOT_RUN**。F6 必须在其授权内处理插件 build/install codegen 与安装闭包，不可把本次三前缀同步当成 SDK PASS。
- Linux、Android build/test、Root/Product、完整安装 SDK、真实硬件 Device Lost、F6 Feature SDK、F7 DataSchema/TypedStorage/真实资产发布链：NOT_RUN 或 NOT_IMPLEMENTED，原因是本阶段边界；不拿历史成绩替代。
- F6 入场需用户审阅 F5 及未关闭热态指标后单独批准，并冻结无环公共 Feature API 依赖、插件代码生成时机/安装入口、code pin 生命周期。F5 shared backing 机制证据不能关闭 F7 数据链义务。F7 仍需另行授权。
- `V2_PRODUCT = EXPECTED_UNAVAILABLE`。**STOP；F6/F7 = NOT_AUTHORIZED。**

## 8. 完整修改文件清单

I 相对批准入场共 72 文件。逐文件旧/新 Git object、SHA-256 在 `changed-files.json`；全量 binary-safe diff 在 `implementation.patch`。V 另仅新增本报告。

- `cmake/render-v2-bootstrap/CMakeLists.txt`
- `cmake/render-v2-bootstrap/F5Runtime.cmake`
- `cmake/render-v2-bootstrap/measure_f5.py`
- `cmake/render-v2-bootstrap/verify_f5.py`
- `docs/render-v2/F5_ENTRY_DECISION.md`
- `docs/render-v2/F5_IMPLEMENTATION.md`
- `docs/render-v2/F5_NATIVE_IMPACT.md`
- `docs/render-v2/F5_PERFORMANCE_PROTOCOL.md`
- `docs/render-v2/F5_TEST_IMPACT.md`
- `docs/render-v2/F5_TYPE_INVENTORY.md`
- `modules/function/render/runtime/CMakeLists.txt`
- `modules/function/render/runtime/include/lux/engine/render/runtime/NativeAssembly.hpp`
- `modules/function/render/runtime/include/lux/engine/render/runtime/Runtime.hpp`
- `modules/function/render/runtime/include/lux/engine/render/runtime/RuntimeMessages.hpp`
- `modules/function/render/runtime/include/lux/engine/render/runtime/Types.hpp`
- `modules/function/render/runtime/pinclude/Presentation.hpp`
- `modules/function/render/runtime/pinclude/RuntimeBackend.hpp`
- `modules/function/render/runtime/src/Backend.cpp`
- `modules/function/render/runtime/src/Frame.cpp`
- `modules/function/render/runtime/src/Presentation.cpp`
- `modules/function/render/runtime/src/Runtime.cpp`
- `modules/function/render/runtime/test/History.cpp`
- `modules/function/render/runtime/test/HotLink.cpp`
- `modules/function/render/runtime/test/Lifecycle.cpp`
- `modules/function/render/runtime/test/NativeInvocations.cpp`
- `modules/function/render/runtime/test/Presentation.cpp`
- `modules/function/render/runtime/test/Probe.cpp`
- `modules/function/render/runtime/test/ProfileFrame.cpp`
- `modules/function/render/runtime/test/ProfileNativeFrame.cpp`
- `modules/function/render/runtime/test/RejectCompile.cmake`
- `modules/function/render/runtime/test/RuntimeBenchmark.cpp`
- `modules/function/render/runtime/test/RuntimeProgress.cpp`
- `modules/function/render/runtime/test/SharedAsset.cpp`
- `modules/function/render/runtime/test/TargetExtent.cpp`
- `modules/function/render/runtime/test/shader/F5History.comp.lglsl`
- `modules/function/render/runtime/test/shader/F5History.hpp`
- `modules/function/render/runtime/test/shader/F5Shared.comp.lglsl`
- `modules/function/render/runtime/test/shader/F5Shared.hpp`
- `modules/function/render/vulkan/include/lux/engine/render/vulkan/device/Device.hpp`
- `modules/function/render/vulkan/include/lux/engine/render/vulkan/graph/Executable.hpp`
- `modules/function/render/vulkan/include/lux/engine/render/vulkan/shader/Bindings.hpp`
- `modules/function/render/vulkan/include/lux/engine/render/vulkan/transfer/Submission.hpp`
- `modules/function/render/vulkan/pinclude/GraphNative.hpp`
- `modules/function/render/vulkan/src/device/Device.cpp`
- `modules/function/render/vulkan/src/graph/Compile.cpp`
- `modules/function/render/vulkan/src/graph/Copy.cpp`
- `modules/function/render/vulkan/src/graph/Diagnostics.cpp`
- `modules/function/render/vulkan/src/graph/Export.cpp`
- `modules/function/render/vulkan/src/graph/Frame.cpp`
- `modules/function/render/vulkan/src/graph/Recipes.cpp`
- `modules/function/render/vulkan/src/graph/Record.cpp`
- `modules/function/render/vulkan/src/graph/Resources.cpp`
- `modules/function/render/vulkan/src/graph/Sync.cpp`
- `modules/function/render/vulkan/src/shader/Bindings.cpp`
- `modules/function/render/vulkan/src/transfer/Submission.cpp`
- `modules/function/render/vulkan/test/CMakeLists.txt`
- `modules/function/render/vulkan/test/NativeGraph.cpp`
- `modules/function/render/vulkan/test/NativeGraphAlias.cpp`
- `modules/function/render/vulkan/test/NativeGraphBenchmark.cpp`
- `modules/function/render/vulkan/test/NativeGraphCandidate.cpp`
- `modules/function/render/vulkan/test/NativeGraphCloseout.cpp`
- `modules/function/render/vulkan/test/NativeGraphConditional.cpp`
- `modules/function/render/vulkan/test/NativeGraphCopy.cpp`
- `modules/function/render/vulkan/test/NativeGraphDraw.cpp`
- `modules/function/render/vulkan/test/NativeGraphHzb.cpp`
- `modules/function/render/vulkan/test/NativeGraphImport.cpp`
- `modules/function/render/vulkan/test/NativeGraphRaster.cpp`
- `modules/function/render/vulkan/test/NativeGraphReadWrite.cpp`
- `modules/function/render/vulkan/test/NativeGraphShared.cpp`
- `modules/function/render/vulkan/test/NativeGraphSupport.hpp`
- `modules/function/render/vulkan/test/NativeGraphViews.cpp`
- `modules/function/render/vulkan/test/ShaderHotLink.cpp`
