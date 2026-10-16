# Render V2 v11 S1/S2 — 独立 Verification V

结论：**S1 PASS，S2 PASS；完整产品 TOOLCHAIN 闭包 BLOCKED。**
本报告仅新增验收材料，没有修改精确 I 的生产代码、测试或历史资格。S3–S6 / G11B–G13 未施工。

## 精确身份与独立性

- 起点及推送前远端基线：`98798da75a201933e6950ef5631444fc199e783a`。
- 最终 Implementation I：`e1f98743471b2a4913419daebefd0016d7a255f6`。
- I Parent / G5：`6847e6693d1ceca8a61cbc53ff54f05d3f13d7f3`。
- S1 内部检查点：`222a90730dbb077372f0f19a10283a20076c4a7a`；S2 CPU 检查点：`82ec60ff572b0608a720dde18ce6c5e20db833ee`。
- V Parent 为上述 I；V 自身 SHA 及推送后的实际远端 HEAD 在提交后交付，避免自引用伪 SHA。
- 目标分支仅 `codex/render-v2-rebuild`。施工在隔离 `codex/render-v11-s1s2`；原用户工作树没有施工。
- 独立根：`E:/RenderV2Rebuild/V11/verification/e1f98743471b2a4913419daebefd0016d7a255f6/`。
  `source` 以 `git clone --no-hardlinks --no-checkout` 建立，detach 精确 I；无 alternates，验收前后 clean。
  先运行 `ValidateTrackedSnapshot.cmake`，再创建全新 release/debug/asan 构建树。V 源码没有临时补丁。
- 输入 ZIP SHA256：`32e8e5077c4fb91191df6b50d556606c97b8ee191754241f9de055f4ace4e60b`；21 项完整性已在 G0 核验。
  以专门 S1/S2 合同执行连续门禁，旧正文 S1-only 等矛盾及裁决保留在冻结材料中。

环境：Windows/MSVC 19.44.35226、CMake 3.31.10、Ninja、stdexec 0.10.0、Vulkan SDK 1.3.280.0；
CPU i9-13900H（14 核/20 逻辑处理器），实际 GPU RTX 4070 Laptop，驱动 32.0.16.1088，
Native API 整数 4211029 / driver 2559967232。lux-cxx `bf779515a120350c7c5412c366eea73afdf59ccf`，
lux-cmake-toolset `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`，工作区及版本均未改变。

## 构建与行为门禁

每组均从该 clean clone 全新 configure，执行完整 `all -j 4 -- -k 0`、第二轮构建、全部 CTest；没有只选新增用例。

| 配置 | 测试 | CTest 时间 | 第二轮构建 | GPU / 内存安全 |
|---|---:|---:|---|---|
| RelWithDebInfo | 98/98 | 104.04s | no work | Validation / SyncValidation Error=0 |
| Debug | 98/98 | 105.64s | no work | Validation / SyncValidation Error=0 |
| MSVC ASan | 98/98 | 137.69s | no work | ASan Error=0，GPU Error=0 |

ASan 使用 `/fsanitize=address /Zi`，`ASAN_OPTIONS=alloc_dealloc_mismatch=1`。79 个原测试名称全部保留；
原行为断言迁移至新接口，未删测试、降门槛或把超时当正常关闭。构建与测试串行执行。
真实警告未隐藏：GPU 日志为 EOS Overlay Win64/Win32 重复层提示；编译器既有 `/DNDEBUG` 被测试 `/UNDEBUG`
覆盖及安装消费者未使用变量提示保留在原始日志。没有把 warning 计数写成零。

| 验证合同 | 实际证据 |
|---|---|
| 对象与类型 | 真正 C++ 构造/移动/析构，过对齐、非平凡对象、跨 DLL、ABI 错配、真实 FNV64 名称碰撞、CodePin 晚释放 |
| 请求与预算 | Ring/Reply/字节预算不足、NotReady/Closed 失败保留，vector 地址/容量返还，完整 Asset backing 计费，双 Client 同数值 RequestId |
| Batch 权威状态 | 1/100/1k/10k，LOCAL/RANDOM，两个 Scene，全有或全无、重复 Last-Wins、revision n 不确认 n+1、Session 重建 |
| CPU 完成 | 同步完成重入、0/1/16/128、受控逆序与突发、取消/停止、跨 Owner 线程；唯一固定槽和有界事件队列，终态不丢失 |
| Native 完成与上传 | 真 Queue submit/ticket、三拓扑配置、1/16/64MiB、Mip/BC1/padded Region、Readback、部分提交失败、DeviceLost 注入、last-good |
| LastUse | T1 Clone→T2 Draw→只观察 T1→替换/删除；T2 前不得释放；三配置及多 chunk 取消，源/Descriptor/Backing 精确释放 |
| 长寿命与停服 | Client close/abandon 不等于 Native cancel；CPU/编译/GPU/Readback 与 Reply Owner 晚析构；ReceivedRequest 先于 Responder reset 清偿 |
| 真实扩展 | 外部 TestBuffer Storage/Feature 走同一注册与 upload 接缝，真实 Copy、Shader 读取、Readback；不修改 Server 业务分支 |
| 渲染回归 | 两 Scene/四 View、共享 Graph、Texture×Buffer、Main 64 Reply 满 1.5s 时 Upload 与渲染仍前进；稳定帧首方 new=0 |

LastUse CSV 是 CPU 提交/观察/发布/退休时间线。Region 在三配置下实际仍使用 Graphics；多 chunk 取消用例只接受首 chunk
并证明没有后续 T3 掩盖 LastUse，完整多 chunk 由原上传用例覆盖。此路径无独立 Arena，不虚构其销毁计数。
Work reset 后 Staging 租约归还与缓存页仍驻留明确区分。GPU timestamp 对这些新时间线为 NOT_MEASURED。
Fault injection 只在测试构建启用；它没有替代正常路径及首个 accepted GPU work 的真实提交。

## 类型、所有权及删除

生产及必要构建变更为 **15 A / 9 D / 44 M**；测试/历史文档/测试 Hook 不计入这一分类。
完整路径、状态、哈希及 14 组职责迁移见归档 `verification/evidence/type-closure-audit.json/.md`。
这里的“14 组”不是虚构的新增 Owner 数，包含已有类型移动、改名和合并。

- IpcMessage 原地成为 TypeOps + SBO + opaque 元数据 + CodePin 的实际对象容器；物理 SPSC/Channel/Link/额度/关闭回收逐段未变。
- Client 原 SlotMap 唯一分配 RequestId；Server 原 SlotMap 原地成为唯一 Command 目录。BoundCommand 只由完整冷态绑定产生。
- Server 唯一固定 StableSlotMap 拥有原请求、有限 Sender operation 与 CPU/GPU/Readback 结果；事件仅携 Session/代际槽身份。
- CpuSchedule 在宿主 TU 适配现有 ExecutionRuntime::CpuScheduler，Render 不创建线程池、不反向链接 Engine。
- Queue 有界回调在 submit 前预约；真实 Ticket 接纳后登记。分发先摘下/unarm，再调用，返回后不读取可能销毁的节点。
- 唯一 `render::upload` / StorageTraits；Transfer 保留唯一 staging/copy/chunk/QFOT 状态机，Texture 只承担专业候选与发布。
- DeviceSession/VMA/ResourceRegistry、G4 Graph/Analysis、Effect-owned CompiledGraph 保持唯一。LastUse 最多三物理 Queue，同 Queue 单调合并。

实际删除 Runtime `Operations.hpp/.cpp`、Command 专属 `Wire.hpp/.cpp`、Runtime 专业 TextureStorage 实现、旧 Runtime Vulkan GpuCompletion 头，
Native `TextureUpload.hpp/.cpp`、私有 `UploadState.hpp`。无转发 shim。保留的 TextureUpload.cpp 是行为测试；
Session 私有 UploadState 内收于 Transfer.cpp，原 staging、预算、统计和 QueryPool 没有随头文件一起删除。
剩余 DataSchema/Wire 仅用于配置和 Binding，不是生产 Command 的旧编解码路径。

## SDK、可见性与保护

- 41 个公共安装头都有独立 C++20、`/GR-` 编译探针；清单和实际 compile command 存于 `public-header-probes.json`。
- 全新 install prefix、移动 prefix、外部 find_package/运行消费者通过；纯 Client 明确禁用 Vulkan 查找，仍通过。
- Native 生产反向 Runtime/IPC/Engine include 命中为 0；纯 Client 实际 export 仅 Core/Data/Protocol/IPC。
  Protocol/Resource/IPC 禁止依赖审查通过；公共头不泄漏私有/Native。源 include、编译命令和实际安装 export 均核验。
- 三个公共头前缀按 `git show <I>:path` 同步，87 项操作有原文件备份、前后 SHA256，废弃头精确移除。
  三配置新 SDK 另保存 498 项完整文件哈希。Android 头同步不代表 Android 构建通过。
- Legacy **719** 个冻结、提交、工作区 blob 精确一致；全部历史 I/V/checker、IPC R2 PARTIAL、依赖与两处用户修改未动。
- 原用户 HEAD `f80902e100879b114e848cabe1469540aba4d25d`；settings SHA256 `f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c`；
  用户 Graph.hpp SHA256 `00c28abc7bd0a786666f101a5212400a5b467af6bdbbc5049c69d59d548c726a`。

Root TOOLCHAIN 在此精确 I 再次真实 configure，EXIT=1：旧 `render/client`、`render/features/cmake/feature_client.cmake`、
`render/standard_content` 不存在，并产生后续 target/export 错误。**产品闭包 BLOCKED**，没有空 Target、禁用测试或借 Legacy 补齐。

## 性能及明确限制

同机旧/新交替三轮，各 3000 个完整已绑定小 Command 和 Native 录制样本。旧独立 I 为
`5cb564d6245c8a3c009ecc528fde6866643343ac`，到本轮起点仅新增两份报告；旧/新 EXE/DLL SHA、运行时段和全部离群均保存。
下表为本次独立 V，分位数采用 nearest-rank；施工环境另一批样本原样归档，不覆盖或混合。

| 指标 | 旧 p50/p95/p99/max | 新 p50/p95/p99/max |
|---|---|---|
| 完整小 Command 往返 ms | 1.9951 / 2.0716 / 2.1661 / 2.6285 | 1.9927 / 2.0726 / 2.3931 / 2.8019 |
| Submit 构造/旧编码 µs | 0.3 / 0.9 / 1.4 / 8.0 | 0.3 / 0.9 / 1.3 / 25.9 |
| Handler + Reply µs | 0.7 / 2.6 / 4.9 / 165.3 | 0.8 / 2.8 / 5.8 / 26.9 |
| 成功消费/旧解码 µs | 0.5 / 2.3 / 23.2 / 517.6 | 0.6 / 2.1 / 18.9 / 167.2 |
| Native record-execute µs | 108.2 / 174.5 / 225.7 / 2231.7 | 110.0 / 173.9 / 219.4 / 1998.3 |
| Dedicated 上传期四 View 间隔 ms | 2.0093 / 2.5545 / 3.3165 / 5.6641 | 2.0090 / 2.4507 / 3.0079 / 4.1607 |
| Alias 上传期四 View 间隔 ms | 2.0057 / 2.3661 / 3.3919 / 5.8025 | 2.0268 / 3.6771 / 3.8928 / 4.5014 |

完整 Command p99 本次约增 10.5%；Alias 四 View 间隔 p95 约增 55.4%，真实观察不隐藏。上传次数旧/新不同
（Alias 64/105，Dedicated 63/111），旧原始 transfer 与新专业 Storage 事务计时边界也不同，不能据此证明同工作量机制退化或提升。
保留 `sleep_for(1ms)`，完整等待与调度不是 IPC 队列延迟；这些采样没有给出“Renderer 更快”的结论。
新小 Command 的 EXE/Client DLL 各 3000 个样本 new=0；驱动/VMA/OS 分配不在这个计数内。

CPU 完成：同一真实 Server/Device/2-worker Scheduler，sync/normal/burst × N=0/1/16/128，5 预热+100 批。
normal/burst N128 各 12800 个完成，K/popped/adopted=12800，drop=0。normal 采样峰 127，受控 burst 每批精确峰 128。
normal/burst 入队调用 p50 0.1µs、p99 0.9/1.0µs；端到端 p99 366.7/395.6µs；Owner drain p99 95.0/95.3µs。
N0 空 drain p50 0.1µs，无 Sender。原始 cycles、OS CPU time、等待/采用分段均保存，480 项摘要已重算相符。
CPU 表使用夹具的 floor((N−1)×百分位) 算法，观测钩子开销未扣除；GetThreadTimes 的 0/15.625ms 是量化，不能当作零 CPU。
这些路径的 heap 分配及 GPU timestamp 未独立测量，不据此宣称为零。

Batch：320 有效样本，LOCAL/RANDOM 各160，两个 Scene/四 View 共用一 Graph；完整路径与 Client DLL new=0，
每次真实 12 次 move construction + 1 次 assignment，原 vector backing/capacity 返还。对照在 Server stop/join 后离线测量，
不会扣掉并发渲染分配。308/320 个请求窗口观察到 GPU 前进，窗口提交增量合计1228；零增量窗口可在下一 Tick 前取得 Reply，
不等于停止渲染。四 View 最终像素、每组推进和编译计数不变均核验。测试 CPU values 不冒称生产 GPU transforms。

四 View 稳定帧首方 new=0。R5 合法相邻 Graphics 情形仍为相同 Shader `1 Pipeline/1 Bind/1 Scope/2 Draw`，
不同 Shader `2/2/1/2`；不合法合并的其他情形仍有正确多个 Scope。
实测 Message144B/align16（object80）、Reply304B、TypeOps88B、Client Pending48B、CommandEntry136B、
RenderRequest256B、RenderCompletion384B、Deferred736B/align16、Event24B、Callback/Requirements各48B；
Binding32B、PreparedBinding64B、Retirement72B，Release 10k 记录1,343,640B/61次冷态分配。
96/144/208 SBO 候选与原始尾样本在 G3 归档；未删除21.6922ms离群。未预设没有依据的吞吐 PASS 数。

## 可复现命令与证据

以下变量 R 为上述独立根，S=R/source。完整 argv、返回码与所有配置路径由索引中的 runner/log 给出。

```powershell
cmake -DLUX_SOURCE_DIR=$S -P "$S/cmake/ValidateTrackedSnapshot.cmake"
cmake -S "$S/cmake/render-v2-bootstrap" -B "$R/release" -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON `
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset `
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx `
  -Dstdexec_DIR=D:/DevTools/vcpkg/installed/x64-windows/share/stdexec `
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DCMAKE_CXX_FLAGS=/EHsc `
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
cmake --build "$R/release" --target all -j 4 -- -k 0
cmake --build "$R/release" --target all -j 4 -- -k 0
ctest --test-dir "$R/release" --output-on-failure -j 1 -R .
```

Debug 同命令改配置；ASan 使用独立树、RelWithDebInfo 加 `/fsanitize=address /Zi`。
执行脚本 `g6-matrix.ps1`、`run.ps1` 及后续性能/保护/布局脚本均归档。
未完成的中间尝试保留：S1 canonical DLL 分配来源失败、PortSender 同步终态 ASan UAF 重现、
G5 新 UNORM 测试错误要求精确128、G6夹具 namespace/物理SlotKey追踪错误，均先修正后验收。
V 的头编译命令文本检查首次只接受 `/std:c++20`，实际合法参数是 `-std:c++20`；修正检查器解析后通过，未改源码或编译参数。

不可变归档与总 INDEX SHA256 见同提交 `RENDER_V11_S1S2_EVIDENCE.json`。归档含 construction 的失败记录、
各门禁构建/测试/CSV，以及 verification 的三矩阵、源码/依赖身份、完整 SDK 备份/清单、布局、性能、闭包、保护检查。

## 保留事项与停止

- 本轮没有未关闭的已复现正确性问题；Root TOOLCHAIN BLOCKED、历史 IPC R2 性能 PARTIAL 保持。
- 旧 Runtime README 含 G9/R4 阶段的 Operation/Wire/enqueue/done 说明，原样保留为失效历史说明；当前合同以本轮 Implementation 为准。
  TransferOptions 注释仍提旧 advance 名称，属于陈旧注释，不是存活 API 或第二上传路径。
- Linux、Android configure/build/CTest、TSan：NOT_RUN。新事件/LastUse/GPU-ready 端到端 GPU timestamp：NOT_MEASURED；
  原 Transfer QueryPool 能力和测试保留，不把 CPU wall 当 GPU 时间。ASan 不是数据竞争检测器。
- 没有生产 Mesh/Material/Instance、Engine ECS、RHI、WSI/HDR、帧调度或执行模块下沉施工。
- S1/S2 资格允许本轮交付，不构成后续阶段授权。完成正常 FF 推送后停止，等待用户审阅。
