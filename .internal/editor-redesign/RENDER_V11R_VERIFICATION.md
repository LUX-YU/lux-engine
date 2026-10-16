# S1/S2-R 独立验收（2026-10-16）

结论：**失败 Request 恢复与 Canonical SBO 冷态一致性补修 PASS；性能 PARTIAL；完整产品 Root TOOLCHAIN BLOCKED（历史）**。本次不进入 S3。Factory 安装耦合与 Storage 退休扫描保留为后续实际资源阶段的工作，没有借此次实验修改生产调度、SPSC 或生命周期算法。

## 提交与独立性

- 审查起点 / 推送前实际远端：`ae77e17fcfe5549615b9bc01ffa5822246ab3314`。
- Implementation I：`2824180646c1c01eaa97ed86151abbc4852a4de1`，Parent 为上述起点。
- 本 Verification V 的 Parent 为精确 I；V 只新增本报告和 `RENDER_V11R_EVIDENCE.json`，自身 SHA 在提交后交付。
- 独立源码：`E:/RenderV2Rebuild/V11R/verification/2824180646c1c01eaa97ed86151abbc4852a4de1/source`。使用 `git clone --no-hardlinks --no-checkout`，detach 精确 I，无 alternates；先执行 tracked snapshot 检查。源码保持 clean；验证环境未修生产或测试源码。
- 施工 worktree 与用户原工作树分离；没有向原脏目录 cherry-pick、reset 或覆盖文件。

## 实际实现和合同

仅两个生产公共头发生修改，新增/删除生产类型均为 **0**。

| 文件 | 改动及证据 |
| --- | --- |
| `runtime/include/lux/engine/render/runtime/RenderClient.hpp` | `take<C>()` 不再因非 SUCCESS 拒绝真实返回的原 Request；校验身份和 Request 类型，只有 SUCCESS 才验证 Response。`tryResponse()` 对非成功返回 null；`response()` 以 SUCCESS 为前置条件，违反时终止，不能把错误对象解释为 Response。 |
| `ipc/include/lux/engine/platform/ipc/IpcMessage.hpp` | `bindType<T>()` 增加 local/canonical `usesInlineStorage()` 比较，仅在冷态绑定执行。 |
| `runtime/test/TypedBatch.cpp` | 真实 Server Handler 拒绝无效 vector 批次且不部分写入；取回相同 backing / capacity 127，原地更正后重提成功，预算最终精确归零。 |
| `runtime/test/TypedClient.cpp` | FAILED、CANCELLED 均保留相同 backing / capacity 89；有返回值时可恢复 Request，错误和 Outcome 不变。关闭且未返回原 Request 时仍为 OUTCOME_UNKNOWN，`take` 失败且不消费 Completion。 |
| `ipc/test/TypedFixture.hpp`、`TypedPlugin.cpp`、`TypedMessage.cpp` | 两 DLL 的测试类型具有相同 Token、尺寸、对齐、编译器/配置 ABI，但 move noexcept 导致不同 SBO 策略，冷态绑定拒绝；不实际传输不相容对象。 |

I 另新增简短实施说明和本轮白名单检查器，共 9 文件、196 行新增、11 行删除。未修改 Native、ResourceRegistry、Transfer、Runtime `.cpp` 或物理 IPC 算法。

恢复复用既有 `IpcReply.original_request`，没有第二个 Owner。Request 值移入应用不等于其仍存活 Completion 立即退还传输预算；Completion/租约结束后准确退款。关闭 Link 中尚有队列输入时，最后参与者退出前仍保活并计费。即使 ABORTED 返回原输入，也不把 OUTCOME_UNKNOWN 改为有保证的业务取消。

## 构建、安装与回归

Windows 11；i9-13900H（14 核 / 20 逻辑处理器）；RTX 4070 Laptop GPU / NVIDIA 610.88；MSVC 19.44.35226.0；CMake 3.31.10；Ninja 1.11.1；Vulkan SDK 1.3.280.0。lux-cxx `bf779515a120350c7c5412c366eea73afdf59ccf`、lux-cmake-toolset `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`，工作区均未改；stdexec 0.10.0 使用现有 vcpkg 安装。

以下命令作用于精确 I 的 clean clone，实际命令、参数、返回码在归档 `inherited-runners/run.ps1` 和 `verification-evidence/short-path-*`：

```powershell
cmake -DLUX_SOURCE_DIR=<source> -P <source>/cmake/ValidateTrackedSnapshot.cmake
cmake -S <source>/cmake/render-v2-bootstrap -B <build> -G Ninja `
  -DCMAKE_BUILD_TYPE=<configuration> -DBUILD_TESTING=ON `
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset `
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx `
  -Dstdexec_DIR=D:/DevTools/vcpkg/installed/x64-windows/share/stdexec `
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF `
  -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF -DCMAKE_INSTALL_PREFIX=<build>/sdk
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> --output-on-failure -j 1 -R .
```

Runner 设置 `/EHsc`；ASan 另加 `/fsanitize=address /Zi` 和 `ASAN_OPTIONS=alloc_dealloc_mismatch=1`。最终独立构建树位于 `E:/RenderV2Rebuild/V11R/verify-build/{release,debug,asan}`。构建与实机运行串行。

| 配置 / 门禁 | 实际结果 |
| --- | --- |
| RelWithDebInfo 全量 all、第二轮、CTest | PASS；no work；98/98，160.16 s |
| Debug 全量 all、第二轮、CTest | PASS；no work；98/98，112.79 s |
| MSVC ASan 全量 all、第二轮、CTest | PASS；no work；98/98，199.89 s；ASan error 0 |
| 原 98 个测试名称与有效回归 | 全部保留，见 `evidence/test-inventory.json`；未删测试或禁用消费者 |
| GPU Validation / SyncValidation | 既有真实 GPU 测试通过；Validation Error / SYNC-HAZARD 标记 0；保留原始 loader/EOS Overlay warning，不宣称零 warning |
| 公共头 C++20 / 无 RTTI、fresh install、relocation、外部消费者 | 纳入全部三组构建/CTest，PASS；纯 Client 消费者禁止 Vulkan 查找仍通过 |
| 三安装前缀 | 精确 I 的两个公共头共 6 项备份、同步、SHA-256；见 `evidence/sdk-sync/sdk-sync.json` |
| 稳定四 View / Pipeline、Bind、Scope / 生命周期 | 原测试通过；稳定帧首方 new=0；没有修改对应生产实现 |
| Root TOOLCHAIN | 真实重新 configure 返回 1；仍缺 render/client、feature_client.cmake、standard_content 及其依赖目标；BLOCKED，没有空 Target |
| Linux / Android / TSan | NOT_RUN；Android 仅头同步，不代表构建通过 |

第一轮独立长路径 Release 为 **96/98**，两个 installed consumer 失败。Ninja 1.11.1 对一个实际存在的 **260 字符**生成 CMake package 路径反复报告缺失，最终 `manifest still dirty after 100 tries`。保留该失败和 `ninja-explain.log`；仅把全新构建树移到较短路径，对未改动的精确 I 完整重跑三个配置，得到上述 98/98。没有通过删除断言或改源码绕过。

归档复核另发现嵌套 CTest 调用会截断/稀疏化父级 `LastTest.log`，主控制台的 98/98 记录仍完整。原日志未修写；额外对同一精确 I 的三个构建树串行执行 `ctest --output-on-failure -V -j 1 -R .`，直接保存完整 stdout，逐项复核 98 个 Passed 行、无 NUL 空洞及诊断。补充归档的 `summary.json` 记录重跑时间与全部 warning 行；不能以损坏日志中的“未发现错误字符串”独立证明 GPU 资格。

消息布局保持：`sizeof(IpcMessage)=144`、`sizeof(IpcReply)=304`、`sizeof(TypeOps)=88`、SBO=80；Batch Request=32 / align8。公开恢复 API 不添加布局字段。GPU Timestamp 在下面上传实验实际测量；VTune 没有 GPU trace 或硬件计数资格。

## 同工作量性能实验

旧生产源码为 `5cb564d6245c8a3c009ecc528fde6866643343ac`，到 v11 起点 `98798da75a201933e6950ef5631444fc199e783a` 的差异仅两份 G11A-R 报告，生产等价。新生产为精确 I。实验代码、diff、编译参数、EXE hash 全在源码树外保存，不能把实验限制误认为生产优化。

固定每次 **16 × 16 MiB = 256 MiB，256 个 1 MiB Chunk，2 个 Staging Page，两个 Scene / 四个 View，4096 Draw = 1024 个四 View 组**。Alias 是同物理 Queue / family 0；Dedicated 为 family 0/1。测量窗口从 RenderThread tick 边界开始/结束；Alias 256 个 Transfer Submit + 4096 个 Draw Submit，Dedicated 含 Acquire 的 Transfer Submit 为 272，Draw 仍 4096。每轮核对计数和真实输出。实验仅在外置 server 副本限制额外 Draw，保留生产完成、Ticket 和固定 sleep 行为。

四轮 AB/BA 顺序交替，共 16 次运行；每种组合 64 个 upload 样本、4092 个全窗口四 View 间隔。无 VTune 附着的样本用于延迟统计；VTune 另跑。上传更快会自然缩短活跃窗口，故另外报告全固定工作量与上传活跃期，不能把两者混为同一样本集合。Alias 活跃期旧 2233、新 1242 个间隔；结果析构、收尾不冒充上传时间。

| 指标（p95） | 旧 | 新 |
| --- | ---: | ---: |
| Alias：16 MiB 上传往返 | 71.0355 ms | 44.1868 ms |
| Alias：上传活跃期四 View 间隔 | 2.4000 ms | 3.1318 ms（+30.5%） |
| Alias：全固定工作量四 View 间隔 | 2.3591 ms | 2.5282 ms（+7.2%） |
| Dedicated：16 MiB 上传往返 | 71.8173 ms | 40.0921 ms |
| Dedicated：上传活跃期四 View 间隔 | 2.3386 ms | 2.3345 ms |
| Dedicated：全固定工作量四 View 间隔 | 2.3668 ms | 2.3545 ms |

Alias 活跃期 p99 为 2.8930 → 3.8742 ms。完整 p50/p95/p99/max、CPU preparation、first/last Submit、首次真实 Ready 观察、callback/reply、消费时间和 GPU timestamp 统计见 `evidence/fixed-work/summary.{json,csv}` 及每轮 raw CSV。Ready 采用 Fence 完成**观察时点**，不是精确 GPU 执行结束时间。

### 尾部归因：证明到哪里

1. Alias Transfer 间隔 p50 从 **4.0268 → 2.0136 ms**，上传更密集；Ready → Reply p95 **24.5 → 24.0 µs**，不是可见的主要增长点。活跃帧内 graphics submit CPU 总时长 p95 **178.7 → 204.5 µs**，本身不足以解释全部帧间隔尾部增长。
2. 附加只读事件实验两轮：新版 Alias 跳过 View 提交 166 / 97 次，旧版 24 / 35 次，均不是该 View 自身上一 Ticket 未完成。带跳过的新版帧 p95 约 4.2 ms；无跳过约 2.6–2.7 ms。tick CPU 时长没有一致增长。
3. 再用四次固定工作量实验记录实际 Queue 状态：新版 Alias **131 次**、旧版 **17 次** View 跳过，**每次均为 submitted-completed=3、capacity=3**，无 recording 冲突或未知原因。Dedicated 旧 14 / 新 20 次。`DeviceSession` 的既有容量为 3；`SubmissionQueue::begin()` 满时使 `ExecutionContext::execute()` 返回暂不可执行。
4. 源码与事件相符：新路径在绘制前处理完成事件，较密集的 Transfer 使用共享 Queue 槽位；旧路径推进延期上传的位置不同。证据支持 **更快上传增加共享三槽 Queue 的准入压力，进而增加 View 跳过**。这证明了一个直接限速点，不能断言所有离群样本只有这一原因，更不能把改善上传吞吐等同于无帧延迟回归。

没有调整 Queue 容量、添加缓存/线程池或修改回调算法。性能资格维持 **PARTIAL**，如需改善共享 Queue 上传与绘制之间的公平性，需另行有界任务及等价测量。

### VTune

实际使用 Intel VTune **2026.4.0 build 632893**（oneAPI 安装），旧/新 Alias 各一次 user-mode hotspots，均 EXIT=0。`-start-paused`，ITT 仅包围上传活跃阶段；采样运行不混入延迟统计。命令、raw result、summary/hotspots/modules 表位于 `evidence/vtune/{old,new}`。

新版报告热点占比：`sleep_for(1ms)` 43.7%、Unknown stack 38.2%、Validation wait_until 11.1%、process condition_variable 2.2%、Validation condition_variable 2.1%。固定 sleep 和验证层等待不能解释为 IPC 队列纯成本；VTune 的 Spin Time 分类也不等于生产实现主动忙自旋。Unknown 比例高且活跃期短，无法据此断言 CompletionQueue 锁竞争占主导或不存在。两版活跃时长不同，不用总采样 CPU 时间比值宣称速度提升。

### 小 Command 尾部复测

原实际客户端完整操作，各 4 × 1000 = **4000** 样本，AB/BA 交替；保留固定 sleep 与调度等待。

| 端到端 | 旧 | 新 |
| --- | ---: | ---: |
| p50 | 1.9969 ms | 1.9944 ms |
| p95 | 2.0856 ms | 2.0772 ms |
| p99 | 2.3874 ms | 2.1561 ms |
| max | 3.9598 ms | 2.5550 ms |

Submit p95 0.9 → 0.7 µs，Handler/Reply p95 3.4 → 3.3 µs；完整分段与全部离群值保留。新测试首方/Client DLL 分配断言为 0。本轮未复现此前小 Command p99 上升，不能据一轮环境结果消除历史样本或宣布普遍更快。

端到端为 **submit + wait_and_consume**；successful consume 是后者的子区间，不重复相加。审阅汇总脚本时纠正了此前双算该子区间的派生表，原 CSV 未变，错误表以 `REJECTED` 保留。

## 保护与证据

- Legacy **719**：冻结源、基线、I Git blob 及工作区内容一致；所有非白名单文件和历史报告/checker 不变。
- 用户主工作树 HEAD 仍为 `f80902e100879b114e848cabe1469540aba4d25d`，仅原两处修改，状态与快照一致。
- 用户 `.vscode/settings.json` SHA-256：`f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c`。
- 用户 `Graph.hpp` SHA-256：`00c28abc7bd0a786666f101a5212400a5b467af6bdbbc5049c69d59d548c726a`。
- 历史 IPC R2 PARTIAL、S1/S2 原报告、Root 阻断与未测平台资格不改写。

最终证据快照：`E:/RenderV2Rebuild/V11R/archive/2824180646c1c01eaa97ed86151abbc4852a4de1/`。
包含 **844 文件 / 591754820 bytes**，逐文件 SHA-256 已复核。`INDEX.json` SHA-256：
`b0a423af7f0e0ebf69480701764cb892aa2353f61f4d8e3a7ba3ee96c044e1a6`。
仓库的 `RENDER_V11R_EVIDENCE.json` 给出路径映射、依赖和资格。归档只读使用；失败尝试不删除，包括开发期错误退款断言、独立长路径失败、初始非 tick 边界产生 4097 Draw 的无效性能窗口和派生统计修正。

完整 stdout 补验同样三组 **98/98**：Release 104.48 s、Debug 104.79 s、ASan 136.43 s。
补充快照位于上述 SHA 后缀 `-supplement/`，10 文件 / 1935980 bytes；`INDEX.json` SHA-256：
`c96424559df3f6526c8c7726954fc5c56124ae3f4d1eb6921fdec91832b72a77`。
它包含完整三组输出、warning 行汇总、再次运行的保护检查、六份安装头哈希复核与继承的原始保护快照；第一份归档未改写。

**停止条件已满足：小范围正确性补修通过，性能遗留明确保留；不实施 S3、不处理 Factory/退休扫描、不扩大生产优化。**
