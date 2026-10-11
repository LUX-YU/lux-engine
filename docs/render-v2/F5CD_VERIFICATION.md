# RD2-F5-C-D 独立诊断验收

本轮完成预定的 48 进程同环境 A/B 和一次 8 进程局部观测。**历史 Runtime CPU Frame 大幅 p50 增长未重现（NOT_REPRODUCED）；小幅差异及四 View 尾部仍 INCONCLUSIVE。没有据此修改生产代码或将 F5-C 改判 PASS。**

## 1. 提交、范围与独立检出

- 批准基线：`c14baf48500ba94d2beaead7d86d853a868726fc`。
- 第一 Implementation：`428fe5f47560e0abcf256ace36f74ccdf614d8bb`，在 48 进程运行前冻结协议和脚本。
- 最终 Implementation I：`9a660adcaa91cb30368fcd73ae9e25c458675866`，parent 为第一 I；在局部观测前追加其协议及测试派生脚本。原 48 进程协议、二进制和样本未覆盖。
- 本 Verification V 只新增本报告；确切 V SHA 从提交结果取得，不在报告中自引用。
- 独立 I 检出：`D:/LuxQualification/render-v2-f5cd/I-final/source`，`clone --no-hardlinks --no-checkout`，固定 `core.autocrlf=false`，精确 detached I；tracked snapshot 通过，无 alternates，5,643 个 Git object 文件链接数均为 1。
- I 相对批准基线的完整变更仅两个新增文件：`cmake/render-v2-bootstrap/diagnose_f5cd.py`、`docs/render-v2/F5CD_RUNTIME_DIAGNOSTIC_PROTOCOL.md`。没有生产、公共头、原测试或原 CMake 文件变化。
- 原始证据实际路径：`D:/LuxQualification/render-v2-f5cd/`。文件级 `evidence-manifest.json` SHA-256：`03cdc20300c6f7369ad44c0f9f933141a7224f1f27fcf026b8f9fed97d0544d7`。构建工件另由 `verification/artifact-identities.json` 索引 2,734 项，保留在 A/B 的 build 与 build-local 目录。

## 2. 真实构建与测量身份

| 项目 | A | B |
|---|---|---|
| 精确生产提交 | `e662ff59844cf69f00d94341768df58ddc7f44b0` | `e0c31de6678394c37ad7ed1ffc2d77231bccb9cc` |
| 主实验 executable SHA-256 | `7ebbed9e9fd9b0c904564cee8d5717356e59a13b004711ba7ae6b1223321f86e` | `591b11e319ef2f39321ad09864455e17ed06f9b4a69dc9bb0ac4a20edf66899e` |
| 原 profiling 源码 | 三个 Git blob 相等 | 三个 Git blob 相等 |
| 主实验两轮定向构建 | 完成；第二轮无工作 | 完成；第二轮无工作 |
| 局部观测两轮定向构建 | 完成；第二轮无工作 | 完成；第二轮无工作 |

MSVC 19.44 / tools 14.44.35207、C++20、RelWithDebInfo `/O2 /Ob1 /MD /DNDEBUG`、Ninja、同一安装依赖。CPU i7-13700KF；GPU RTX 4070 Ti，driver 591.86，Vulkan API 1.4.325。QPC 10,000,000 Hz。亲和 mask 4，由父进程继承；子进程 AboveNormal；高性能电源方案。没有并行构建、GPU 测试或录屏任务。未采集 ETW 调度或逐帧 CPU 频率，不将这些因素宣称为根因。vulkaninfo 的已安装 overlay layer 列表也不能证明计时受其干扰。

实际 File API、compile commands、Ninja dependencies/commands、link maps 已归档。两个 profiling 构建的源集合、编译器、flags、include 顺序相同；规范化后的唯一命令差异为 metadata TU 的对象输出目录。五项生成头/GLSL/SPIR-V 字节一致；RuntimeMessages.ops.hpp 仅绝对源码 include 路径不同。外部编译头集合及字节哈希一致，无 Legacy/Scene/Editor 输入。

初次只构建 A 的 benchmark 时缺 `F3Tonemap.pass.hpp`：原 benchmark 直接依赖漏列其生成目标。失败日志 `A-build-1.log` 保留。对两版显式构建既有 `render_vulkan_schema_F3Tonemap` 后成功，不修改 A/B 源码或 CMake。这是原定向构建入口问题，不是生产运行失败。

主实验只在原 ProfileFrame 的冷态 `if (!queries)` 内增加元数据输出，计时边界和热态语句保持原样；不是原历史 executable 的字节复制。原 F5/F5-C 历史 Normal/ASan benchmark 文件仍存在且与旧工件清单哈希一致，见 `verification/inherited-identities.json`。本次主要比较使用上述同环境重建 A/B。

主实验冻结 script SHA：`8942c0652dcba68d25f30072152671299e0469c846d5d95e17b96de2d72de519`；protocol SHA：`a50ddffda1f727714954606d998f99418e38944d4f60bad2e56d327243d89b01`。首次运行锁定于 UTC `2026-10-11T22:51:58.925548+00:00`。最终 I 的局部协议/派生输入/二进制另见 local-input-manifest、local-experiment/lock；没有把后来的脚本 SHA 冒充主实验运行前身份。

## 3. Q1：历史的大幅 p50 差异是否重现？

**未重现该幅度。** 每配置严格 4 AB、2 AA、2 BB 对，共 48 个独立进程，全部退出 0，无删除、替换或追加。每进程原 24 warm-up、240 measured frames、原 Shader、readback 和像素 oracle。所有 6 个原始指标各保留 240 帧；另外逐帧计算嵌套 preparation 以外的 CPU 时间。原 benchmark 停止请求期间偶有第 265 帧，原始 progress 全部保留；计时仍精确取第 25–264 帧。

下表 μs 为各 AB 进程分位数的中位数，**不是合并所有帧后的分位数**。

| Scene/View | CPU p50 A/B | Prep p50 A/B | 剩余 CPU p50 A/B | CPU p95 A/B |
|---|---:|---:|---:|---:|
| 1/1 | 67.70 / 66.25 | 4.40 / 4.00 | 63.30 / 62.15 | 114.05 / 114.15 |
| 1/4 | 139.60 / 140.85 | 7.80 / 7.40 | 131.50 / 132.85 | 220.20 / 252.85 |
| 2/4 | 161.10 / 161.55 | 8.25 / 7.50 | 152.90 / 153.95 | 268.50 / 230.60 |

原历史 A 的 40.5/95.8/123.6 μs 并未在本轮 A 重建中复现；不能将两轮绝对数值之差全归于 F5-C。Prep 也未复现历史 0.6→3.8 μs 的同环境回退。

CPU Frame 每对变化百分比，按冻结 block 顺序，正值表示第二臂较慢：

| Scene/View | 指标 | AB 四对 | AA 两对 | BB 两对 |
|---|---|---|---|---|
| 1/1 | p50 | -4.64, -2.41, -1.50, -2.77 | +8.45, +4.23 | +3.50, -1.99 |
| 1/1 | p95 | -3.09, +2.24, +5.45, -18.45 | +3.28, +14.45 | +4.53, -0.99 |
| 1/4 | p50 | -2.15, -0.07, +1.89, +1.91 | +0.80, -0.42 | +4.46, +2.01 |
| 1/4 | p95 | +25.79, +3.01, -1.05, +49.59 | +8.75, -27.57 | +22.16, -11.28 |
| 2/4 | p50 | -0.73, +0.68, +1.26, -2.24 | -0.75, -0.87 | -6.68, +0.13 |
| 2/4 | p95 | -8.65, -15.76, -21.08, -25.71 | -6.26, -18.47 | +8.53, +5.08 |

Prep p50 的 AB 四对：1/1 为 -10.42/-5.13/-6.98/-11.11%；1/4 为 -6.17/-3.70/-5.33/-2.70%；2/4 为 -1.20/-17.86/-8.54/-3.85%。其 AA/BB p50 配对范围分别为 1/1 [+4.44,+25.71]/[0,+14.71]%，1/4 [0,+1.27]/[-2.70,+12.86]%，2/4 [-1.39,+1.20]/[-1.27,+1.23]%。全部指标的逐对 ns、百分比、p50/p95/max、进程 CPU time/I/O/working set 均在 `experiment/rows.jsonl` 和 `analysis.json`。

所有进程 compiles=1、warm allocations=0/bytes=0，所有 View readback 像素正确。一 Scene 四 View 的 Scene reuse 为每帧 3 次，两 Scene 四 View 为每帧 2 次。实际 slots：A 三配置均 16；B 为 2/8/8；均 imports=1、cells=2、max_views=8、FIF=2。每 Queue timestamp 与 pacing 的完整样本保留，GPU interval 包含 submission gap，不能当作纯 Shader GPU 时间。

**尾部结论是 INCONCLUSIVE，不是性能等价。** 特别是 1/4 的两个较大不利 p95 样本不能删除，也不能只凭 AA/BB 波动判为噪声。2/4 的相反方向同样不证明生产优化。小样本未计算新的资格 CI，不生成新的 5% PASS。

## 4. Q2：具体哪段代码承担差异？

历史大幅 p50 差异没有同环境复现，因此其原因仍 **UNKNOWN**。1/4 尾部的差异主要在 Prep 之外：剩余 CPU p95 AB 四对 +25.98/+4.07/+2.20/+51.83%；Prep p95 四对均下降。剩余区间包含 Runtime 装配/Scene donor proof、Native record、Queue poll/submit 等，不能将这个聚合差值直接命名为驱动或调度成本。

调用路径为 Runtime `executeRuntimeFrame()` → `NativeGraphInvocations::submit()` → `prepareSlot()` → `validateImports()`；还包括 Admission 的 `slotReady()`。`has_pending_cells` 调用 `SubmissionQueue::completed()`，该函数只读取缓存 completed 值；真正 `vkGetFenceStatus` 在 `poll()` 中。无 pending 时 B 跳过旧历史 cell 扫描，但不跳过 import 身份、ready、范围、usage、native alias 等校验；有 pending 时保留原历史扫描。

一次局部观测共 8 个进程，两版各 1/4 View×2/3 FIF，全退出 0、原像素检查和热态零分配通过。计数与 clocks 仅编入独立测试 profiling 目标，不进入任何 production library/public ABI；原 validateImports 条件和返回路径没有改写。外部生成 C++、原源码哈希、编译/链接输入均保留。

| View/FIF | slots A/B | retained-complete validation p50 A/B (μs) | pending validation p50 A/B (μs) | 每帧 G/C/T submit |
|---|---:|---:|---:|---|
| 1/2 | 16 / 2 | 1.5 / 1.2 | 1.45 / NOT_OBSERVED in measured frames | 1 / 2 / 1 |
| 1/3 | 24 / 3 | 1.6 / 1.6 | 1.6 / 1.6（各仅 1 次） | 1 / 2 / 1 |
| 4/2 | 16 / 8 | 1.4 / 0.9 | 0.3 / 0.3 | 4 / 2 / 4 |
| 4/3 | 24 / 12 | 1.8 / 1.7 | 0.4 / 0.3 | 4 / 2 / 4 |

空 ticket 仅各进程首次 warm-up 出现一次；B 原预扫描分别访问 4/6/16/24 个 cells；有 pending 时首个 pending 前访问 1–6 个 cells。空状态 validation 单次 1.6–4.0 μs（包含首调用冷效应），不足以估计稳态成本。retained-complete 状态有 236–253 次 measured 观测；四 View pending 状态有 704–708 次。

**这些是完整 validation body 的插桩时间，不是 has_pending_cells 净成本。** 额外计数会预热 cache；pending 分类与 donor View/被禁用的 Scene pass 相关，不能用“pending 更快”推导因果关系。局部结果只支持未发现该检查的明显正向成本回退，不支持宣布其无成本。实际 submit 数 A/B 相同；每 View admission 与 submit 各 poll 三个 Queue，成功路径每个 batch begin 再 poll 一次，因此观测的 1/4 View 分别对应 CPU 区间 10/34 次 poll 调用（不等于 fence 查询数）。profile 前后额外 query submissions 在区间外。未伪造逐调用 cycles/cache-miss 或完整 scheduler 证据。

本轮不提出生产性能补丁。无法归因的 Runtime 尾部保持开放，不再增加实验。

## 5. Q3：新增确定性正确性阻断与局部结构审查

在已审阅范围和本轮原 GPU readback 证据内，**没有确认新的确定性正确性阻断**。这不等于重新证明所有平台、WSI 或全部 Native hazards。

| 审查点（路径相对于 modules/function/render） | 证据与结论 |
|---|---|
| `vulkan/src/graph/Compile.cpp:211` append | 完整候选创建成功后才 reserve/move slots；GraphSlot noexcept move 转移 vector storage，既有 backing/descriptor 内部存储地址不因外层 vector relocation 改变。 |
| `vulkan/pinclude/GraphNative.hpp:93` | shared_resources 指向 resource vector 元素；vector move 保留其存储；scene_source 持久保存 slot index。submit 中临时 donor 指针仅在同一串行 owner 调用内借用，不与冷态 append 并发。 |
| `runtime/src/Backend.cpp:368` | append 成功后 targets.push_back 可能再增长 CPU vector。这是 C4 冷态简化建议：可先 reserve 目标容量。普通 heap OOM 为 fatal，未发现可恢复 native 失败造成半提交的最小复现；本轮不改。 |
| `runtime/src/Backend.cpp:316` acquireRuntimeSlots | 排除活跃 View 和候选占用，逐 slot 调用 slotReady；其复用证明是实际 Queue tickets/poll，没有用 CPU revision/serial 冒充 completion。 |
| `vulkan/src/graph/Export.cpp:111` | retainExternalUse 先保留外部 ticket，再 join；join 失败保留 terminal 和借用；donor last-use 继续延长。append 不改变既有 backing owner 或 receipt slot 身份。 |
| `vulkan/src/graph/Frame.cpp:58` | pending predicate 覆盖所有 cell tickets，包括 imported/donor；完成值单调且 owner 外部串行。仅在全部已完成时跳过历史扫描，别名/ready/资源状态校验仍在。 |
| Builder failure rollback | F5-C `Runtime.cpp` 在 CreateThread 失败时还原 backend/transport；本轮未更改，也未重新运行故障注入。继承原资格。 |

原 import alias、跨 Queue、donor/retirement 负例和 WSI 资格引用 F5-C 175/175；没有声称本轮重跑。没有最小错误复现就不构造生产补丁、资源 Registry 或额外 owner。

## 6. Q4/Q5、保护与最终状态

**Q4：Native Record/Submit p95 未关闭。** 原正式比较仍是 F4-C-P `92b126f...` 对 F5-C，而本轮是 F5 对 F5-C。原 +0.3322%、CI95 [-8.2126%, +5.5749%] 原样保留；`PARTIAL — CI95 upper bound +5.5749% > +5%`。固定 Native Binding 历史 PASS 保留，F4 冷态有限延期仍 PARTIAL。

**Q5：建议 HOLD，提交用户裁决。** 大幅 p50 回退未重现、没有新确认的正确性错误，是积极证据；但 1/4 Runtime 尾部仍有不能归因的不利样本，本轮不足以证明它只是环境噪声。该建议不是新增架构阶段或强制无限补测：若用户愿意在保留原正式 p95 和本轮 Runtime 尾部不确定性的前提下有限延期，可另行明确授权 F6.a；本报告不替用户批准，也不要求先实施没有根因的优化。

机械资格检查：48/48 主进程、8/8 局部进程；所有退出/240帧原始样本/配对与冻结哈希核验通过；主 executable 在局部实验后字节未变。批准基线全部 3,201 tracked 文件 Git identity 不变；Legacy 719/719 blob/mode/size 一致；Core/Transport/Graph/Vulkan/Runtime、FINAL/RD2、原报告均无改动。原工作区与用户 V2 worktree 的 HEAD/branch/status/staged/unstaged/untracked 及七个修改文件字节哈希保持不变。

Normal/Full ASan 175/175、GPU Validation/SyncValidation：**历史 PASS 继承，本轮 NOT_RUN**。当前 benchmark validation 关闭，像素 oracle PASS 不能替代 validation 资格。完整 all/CTest、ASan、WSI/Resize/DeviceLost、SDK 安装、Android/Linux、产品、性能 counters/ETW、本轮新的正式 5% 资格均 NOT_RUN，原因是无生产变更且本轮仅有界诊断；公共头未变，无三前缀同步变更。

```text
F5C_FUNCTIONAL_RESOURCE_LIFETIME = PASS (inherited)
F5C_NATIVE_RECORD_SUBMIT_P95 = PARTIAL
F5CD_DIAGNOSTIC = COMPLETED
HISTORICAL_LARGE_RUNTIME_P50_REGRESSION = NOT_REPRODUCED
RUNTIME_TAIL_ATTRIBUTION = INCONCLUSIVE
F4_COLD_DEBT = PARTIAL
F6a_ENTRY_RECOMMENDATION = HOLD
F6/F7 = NOT_AUTHORIZED
V2_PRODUCT = EXPECTED_UNAVAILABLE
STOP
```
