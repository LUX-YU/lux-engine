# G11A-R 独立 Verification

**G11A-R 架构与适用正确性/验证门禁：PASS。完整产品 Root TOOLCHAIN：BLOCKED（既有缺失消费者）。**
性能证据已取得，未声称与历史结果完全等速、加速或已消除所有尾部波动。IPC R2 历史 PARTIAL 不变。

## 精确输入和独立性

- 原始/施工前及推送前核对基线：`d5b4ff0ad792dd8e2d9ada7c26bda3c86fc33959`。
- 最终 Implementation I：`5cb564d6245c8a3c009ecc528fde6866643343ac`；Parent：`6ad87a920bc1194caca5d7626e6d59c3e5a36766`。
- V 是仅新增本报告与证据索引的后续提交；V 自身完整 SHA 和推送后的远端 HEAD 由提交后交付给出，避免自引用。
- 独立源码：`E:/RenderV2Rebuild/G11AR/verification/5cb564d6245c8a3c009ecc528fde6866643343ac/source`，`git clone --no-hardlinks --no-checkout` 后 detach 精确 I；无 alternates，最终工作树干净。
- 独立 release/debug/asan 构建树位于同级，各自从空目录配置。先通过 ValidateTrackedSnapshot，再构建；未在验证源码修复任何文件。
- 外置归档：`E:\RenderV2Rebuild\G11AR\archive\5cb564d6245c8a3c009ecc528fde6866643343ac`；`INDEX.json` SHA-256：`4fa440ea803759a1875b93728757ccd581c80d9babaf1598299e6a52512bf47d`。
  共 4332 个文件、69377682 字节，逐文件 SHA-256；包含所有中间失败、原始样本、脚本、日志、依赖与保护证据。

## 真实命令和结果

外置 `runners/verify.ps1`、`run.ps1`、`extra-verify.ps1` 和所有逐次命令日志可直接复查。

```powershell
git clone --no-hardlinks --no-checkout E:/RenderV2Rebuild/G11AR/source <verification-source>
git -C <verification-source> checkout --detach 5cb564d6245c8a3c009ecc528fde6866643343ac
cmake -DLUX_SOURCE_DIR=<verification-source> -P <verification-source>/cmake/ValidateTrackedSnapshot.cmake
cmake -S <verification-source>/cmake/render-v2-bootstrap -B <build> -G Ninja `
  -DCMAKE_BUILD_TYPE=<RelWithDebInfo|Debug> -DBUILD_TESTING=ON `
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset `
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx `
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF `
  -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF "-DCMAKE_CXX_FLAGS=/EHsc <ASan flags>"
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> --output-on-failure -j 1 -R .
```

| 独立配置 | all 两轮 | 全部 CTest | 耗时 | GPU Validation/SyncValidation Error | ASan Error |
|---|---|---|---|---|---|
| RelWithDebInfo | PASS / 第二轮 no work | 79/79 | 96.74s | 0 | N/A |
| Debug | PASS / 第二轮 no work | 79/79 | 99.76s | 0 | N/A |
| MSVC ASan，RelWithDebInfo | PASS / 第二轮 no work | 79/79 | 136.34s | 0 | 0 |

ASan 使用 `/fsanitize=address /Zi`、`ASAN_OPTIONS=alloc_dealloc_mismatch=1`。不是数据竞争检测器。
所有原 72 个测试名称逐项存在；七个新增测试为 resource.contract、resource.registry、native.resource_domains、
native.buffer_transfer、native.storage_bindings、runtime.storage_registration、runtime.buffer_extension（均有 render. 前缀）。
完整输出位于 `evidence/verification-5cb564d6245c8a3c009ecc528fde6866643343ac/<config>/LastTest.log`，每配置 23 份 CSV；不是只保存成功摘要。

环境：MSVC 19.44.35226、CMake 3.31.10、Ninja、Vulkan SDK 1.3.280.0；RTX 4070 Laptop GPU，API 1.4.341，
NVIDIA driver 610.88。`VK_LAYER_KHRONOS_validation` 与 SyncValidation 实际开启。
WARNING 未清零或掩盖：Native callback 输出包括系统中两个 EOS Overlay manifest 重复的 loader warning，
部分测试创建多个 Instance，因此累计 2/3/4 次；逐条内容和 Runtime warning 计数保存在原日志及 matrix-integrity.json。

## 逐项合同

| 编号 | 实际测试/证据 | 结论 |
|---|---|---|
| F-01 | render.resource.contract; render.runtime.storage_registration | PASS：完整 Type/ABI/Factory/API/能力与配置拒绝；非 GPU 第二 Factory 真实创建/读取，未声称 D3D12。 |
| F-02 | render.runtime.storage_registration; render.runtime.texture_operations; render.runtime.buffer_extension | PASS：类型注册贡献原 Operation SlotMap；Texture 无零 Pass Feature 仍可上传。 |
| F-03 | render.resource.registry; render.native.resource_domains; render.runtime.buffer_extension | PASS：Root/Scene/View/Attachment 身份及共享；两个 Feature、两 Scene、四 View 共用 Root Texture。 |
| F-04 | render.native.resource_domains; render.resource.registry; render.native.storage_bindings | PASS：六处 Native 创建失败、错误 Domain/配置/Binding，候选回滚和 last-good。 |
| F-05 | render.native.texture_storage; render.runtime.texture_operations; render.runtime.buffer_extension | PASS：旧 generation/revision 拒绝，已准备消费者保留旧 backing，删除重建不能复活旧 ID。 |
| F-06 | render.native.texture_upload; render.native.texture_sampling; render.runtime.texture_rendering | PASS：真实 1/16/64 MiB、Mip/BC1/Region、Shader 与 BT.2020 double oracle。 |
| F-07 | render.native.storage_bindings; render.runtime.buffer_extension; render.runtime.installed | PASS：外部 Buffer CRUD/Readback，经正式 Feature/Graph/Compiler/Attach/Shader，64/32/128 像素；移动 SDK 消费者运行。 |
| F-08 | render.native.buffer_transfer; render.native.transfer; render.runtime.gpu_ready_transfer; render.runtime.gpu_ready_same_family | PASS：alias 0:0、same-family 0:2、dedicated 1:0；不同 Queue 同 serial 不能替代 BOTH 完成条件。 |
| F-09 | render.native.texture_storage; render.runtime.texture_progress; render.runtime.gpu_ready | PASS：源完整 backing、staging/resident/descriptor/Deferred/Queue 容量；成功与失败 Owner 均有界。 |
| F-10 | render.native.buffer_transfer; render.runtime.texture_progress; render.native.failures | PASS：首 submit 已接受、后续失败，前 Ticket 保留；DeviceLost 是测试注入，非真实 GPU reset。 |
| F-11 | render.runtime.shutdown; render.runtime.gpu_ready; render.runtime.texture_rendering; platform.ipc.closeout | PASS：双 Client 同数值 RequestId、abandon/close、两个编译 worker、Main 64 回复满 1.5s，Upload/四 View 前进。 |
| F-12 | render.native.storage_bindings; render.native.compiler; render.native.binding_plan | PASS：同 Shader 同时消费 Texture/Buffer；错误 set/kind/count/size/同尺寸 uvec4 冷态拒绝；热态直接 Prepared Binding。 |
| F-13 | render.native.failures; render.native.resource_domains; render.native.texture_storage | PASS：可恢复 VkResult 故障与精确销毁；gapi descriptor-layout tryCreate 保留错误语义。 |
| F-14 | 所有 *.installed 与独立公共头目标；extra-verify.ps1 | PASS：C++20 /GR-、实际 SDK relocation；纯 Client 显式禁用 Vulkan 查找，无 Native include/target。 |
| F-15 | render.native.execution; render.native.binding_plan; render.runtime.offscreen; render.runtime.texture_rendering | PASS：Engine steady new=0；保留完整 CSV/分位/离群值与同时段旧新对照；不宣称更快或 GPU-ready GPU timestamp。 |
| F-16 | check_render_g11ar.py --extension; ValidateTrackedSnapshot.cmake; final_checks.py | PASS：独立 clean clone、无 alternates、原 72 名称、719 blobs、用户 SHA/状态、依赖、历史证据和白名单。 |

## 唯一 Owner 和扩展结论

一个 RenderServer::Impl 的原 Operation SlotMap、原 DeferredCompletion 集合和一个 DeviceSession 保持唯一。
DeviceSession 下一个 ResourceRegistry 替代 textures_/scoped_ 两套专用拥有关系；Allocation ledger/VMA、
Queue/SubmissionTicket/Retirement 未另造。Root/Scene/View/Attachment Domain 提供代际/父链和 admission，
Prepared StorageUse 与专业 Use 保留实际 Native 使用；Graph transient 继续由 Context 管理。
普通 OperationContext 只暴露中立资源调用；Native 权限在显式 runtime/vulkan 头中。

Transfer 统一 Buffer/Image staging、chunk、CPU prepare、copy、queue、QFOT 和 Ticket；TextureUpload 仅保留专业
Texture 布局/候选发布。GpuWork 是原 Deferred 槽中的一个有界 work owner，不是第二个请求集合。停止时保留
原 ReceivedRequest、staging/backing 到真实完成/安全清偿；不存在 CPU-ready 冒充 GPU 成功。

Compiler 的事实来源是中立 ShaderInterface + 真实 SPIR-V reflection + ResourcePlan。Effect 持具体参数，
CompiledGraph 不持 TextureId/资产值；Attach 冷态解析，稳定帧不查询 Registry/字符串/Feature。当前 StorageBuffer
布局明确只支持 offset 0 的单个 float4（16B）；错误同尺寸整数块被拒绝，不声称通用材质布局已实现。
同 Shader 的相邻 Graphics Pass 仍为 1 Pipeline / 1 Bind / 1 Scope / 2 Draw；不同 Shader 为 2/2/1/2。

外部 TestBufferStorage/TestBufferFeature 以现有公开注册入口增加真实 GPU CRUD 与 Shader 消费。
R4 之后的结构 diff 检查证明 R5 没有修改 Server 核心、DeviceSession、Transfer、Texture 或 Compiler 生产业务逻辑。
多 Feature Root Texture 共享及 material-like 间接保活均有真实 GPU readback，未提前实现 MaterialStorage。

I 的变更清单使用 Git 默认 rename 合并视图，列 54 个生产/build 路径、33/10 个定义增删。
本次独立审计使用 **--no-renames** 完整枚举，精确为 **57 个生产/build 路径、33 个新增定义位置、12 个删除定义位置**；
差异是搬迁旧路径纳入统计。详见 evidence/final-definition-census.json；这是定义位置计数（搬迁双计），不是新增 Owner 数。
I 总 diff 为 105 个 rename 合并文件，5167 行增加/1196 行删除，包含测试和实施材料。该统计口径澄清没有改写 I。

## 内存和性能

| 记录 | G11A Release | 本次 Release | 原因/口径 |
|---|---:|---:|---|
| IpcMessage / IpcReply | 144 / 296 B | 144 / 296 B | IPC 未改 |
| RenderRequest | 216 B | 216 B | 原请求唯一 Owner |
| DeferredCompletion | 512 B | 560 B | 通用 work 与多 Queue 接纳证明，固定容量仍保留 |
| Binding | 32 B | 32 B | 热态 Native 指针/handle，不恢复胖 RAII |
| ExecutionContext | 224 B | 232 B | 通用预解析资源使用关系 |
| RetirementEntry | 32 B | 72 B | 最多三个独立 Queue 完成条件 |
| Texture upload owner 合计 | 648 B | 672 B | TextureUpload 472 + Transfer 200；额外一个冷态 heap owner |
| 10k ledger + Binding | 1,343,640 B / 61 new | 相同 | CPU 元数据，不是 GPU allocations |

额外实测：`Transfer=200 TextureUpload=472 combined=672 GpuWork=152`。PreparedBinding=64、StorageUse=80、Registry=240、Domain=24、Factory=72 字节。
1000 个 Scene Domain + instance（含一字节 dummy owner）为 1111 cold new、424,673 累计请求字节；不是峰值或 GPU 数据量。
稳定四 View、Texture/Buffer Shader 均 Engine new=0；Native/driver 分配与完整 upload 分配不混为零分配。

CPU 小型 Operation 完整编码/处理/消费，N=1000，单位 µs，p50/p95/p99/max：

| 分段 | 分位 |
|---|---|
| submit_encode_ns | 0.8000 / 1.6000 / 2.0000 / 12.7000 |
| handler_reply_ns | 1.3000 / 3.9000 / 7.7000 / 59.8000 |
| successful_consume_decode_ns | 1.6000 / 3.5000 / 27.6000 / 133.0000 |
| wait_and_consume_ns | 1984.0000 / 2222.6000 / 2570.3000 / 2615.0000 |

wait_and_consume 含宿主轮询等待，不是 IPC 队列延迟。GPU-ready 每条的 inbound→submit→fence-observed→
publication→consumed 分段和所有分配次数在 performance-final.json；三个拓扑各 N=1000。该小操作 GPU timestamp
仍 NOT_MEASURED。Texture chunks 的真实 GPU timestamps 仅覆盖 Copy 及记录在同一 batch 的 barrier，不能当作
CPU preparation、排队、Graphics acquire 或完整请求时间；每个大小/格式/拓扑原样本直接保留，不给 N=1 伪造分位。

历史全套样本与本次全套样本有可见墙钟/尾部差异，因此另外运行同一机器、同一时段、同一 1000 次 record/execute
工作量的 old/new/new/old/new/old/old/new 对照；Texture 四 View + 上传也交替 old/new/new/old。
完整配对数值（含异常）如下，不自动推断加速或完全等速。

| Record/execute 轮次（µs） | p50/p95/p99/max |
|---|---|
| 0-old | 117.4000 / 195.2000 / 231.9000 / 1550.7000 |
| 1-new | 119.5000 / 195.0000 / 245.8000 / 1446.2000 |
| 2-new | 126.9000 / 206.0000 / 263.7000 / 1552.8000 |
| 3-old | 146.7000 / 203.9000 / 287.4000 / 1437.8000 |
| 4-new | 152.3000 / 227.2000 / 306.8000 / 1545.4000 |
| 5-old | 157.2000 / 210.4000 / 281.9000 / 1663.7000 |
| 6-old | 132.6000 / 213.6000 / 288.2000 / 1468.8000 |
| 7-new | 145.5000 / 204.2000 / 248.9000 / 1609.8000 |

最初补测脚本向 dedicated 传了字符串参数，而 fixture 的合同是 argc>1 即 alias；这些结果全部真实属于 alias，
已保留在 paired-* 并明确注明，未冒充 dedicated。paired-corrected-* 用无参数调用补齐真实 dedicated。
原正式 CTest 三拓扑调用一直正确。所有初始和修正补测样本均保留。

同时段旧版也出现相对早期归档变慢；现有对照显示环境/调度差异不能忽略，并不排除泛化后的少量 CPU 成本。
本次增加了已量出的 work/多 Queue 元数据及冷态分配，不据此猜测每个 tail 的唯一原因。
补测明确保留新版本单次 31.0695ms（dedicated）和 34.3358ms（初始 alias）帧间隔；
对应上传最大值 110.3056/112.5368ms，不能从汇总分位删掉。它们不是一次 GPU Copy 的耗时。
SleepProbe 两组各 1000 次实际测得 sleep_for(100µs/1ms) 中位约 15.54/15.53ms；说明长完成观察/消费间隔包含
明显宿主调度成本。新旧尾部离群值、波动与残余成本如实保留，无生产猜测性优化、无性能更快的资格声明。
稳定零分配和 Pipeline/Bind/Scope 计数门禁不回退；通用性能等价/吞吐 SLA 不属于本轮已证明的事实。

另用 Intel VTune 2026.4 的 threading/user-mode sampling 对旧新真实 dedicated 四 View 场景采样，
两次应用均成功、Engine new=0、两个编译 worker 和 1.5s 背压仍正常；Profiler 本身有明显扰动，不能与
无 profiler 的延迟样本混算。全流程热点包括 Vulkan loader、Validation 和 sleep_for；新/旧 loader CPU
样本合计约 1.616/2.042s，Validation 0.520/0.465s；1ms sleep 的累计 Wait 1.210/1.299s。
这进一步说明此端到端测量并非只测 Registry 或纯 CPU 执行。完整 summary/hotspots/callstacks、raw trace、
符号缺失 WARNING 和对比 JSON 都在 evidence/vtune。当前采样不能把上述两个单次离群事件唯一归因，
**尾部逐事件归因仍未关闭**，不据此进行生产猜测性优化；架构/正确性 PASS 不表示所有性能异常已解决。

## 安装、保护和剩余范围

每个配置的 fresh install→move prefix→外部 find_package/运行成功。外部 Buffer consumer 只借用测试夹具源码，
生产头和库全部来自移动 SDK，无 source/native 生产 include 偷渡。纯 Client 另显式禁用 Vulkan 查找，未生成
Native/Vulkan target，编译命令无 VulkanSDK include；公共头 C++20 /GR- 探针通过。
三个公共头前缀按精确 I 的 Git 内容同步：37 个当前头、每前缀 3 个删除旧 API 头，共 120 条操作记录，先备份
后写入/精确移除。完整 before/after SHA256 在 sdk-backup/manifest.json；Android 仅同步头，不代表构建资格。

冻结 Legacy 719/719 与 manifest、原 Git baseline 和工作区 blob 一致；历史报告/checker、IPC 源码未变。
用户原工作树 HEAD f80902e100879b114e848cabe1469540aba4d25d 及两处修改的精确 SHA/状态均保持，未 reset、
cherry-pick、覆盖。依赖 lux-cxx bf779515a120350c7c5412c366eea73afdf59ccf、toolset c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60 未变。
R4 调试 cwd 失误生成的空 CSV 已按生成时间和零长度核实后移至外置证据，原状态恢复；该程序性失误未隐去。

实施期中间失败和修正详见 I 的 G11A_R_IMPLEMENTATION.md 及 archive/evidence：包括 DeviceLost 退休队列错误、
夹具 owner 析构顺序、配置名称/ReadBuffer 完成合同、合法 pre-submit capacity 被旧寿命夹具误当失败。
最终精确 I 三套独立验证均一次通过，未在 V 源码打补丁。证据枚举脚本覆盖 LastTest 的问题已修正为先保存再枚举。

Root TOOLCHAIN 在独立源码再次实际配置，EXIT=1：render/client、feature_client.cmake、standard_content 仍缺失，
完整产品资格 **BLOCKED**；没有空 Target 或 Legacy 回填。IPC R2 历史 **PARTIAL** 保留。
Linux/Android build、TSan：NOT_RUN。GPU-ready 小操作 GPU timestamp：NOT_MEASURED。
完整 Material/Instance/Mesh Storage、通用 Shader block 布局、其他 GPU/Backend 产品资格：OUT_OF_SCOPE。

本轮 G11A-R 结束。G11B–G13 未开工；下一阶段仅可由用户审阅后另行授权。
