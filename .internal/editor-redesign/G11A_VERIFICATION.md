# G11A — Transfer + Global TextureStorage 独立验收

**结论：G11A PASS，限本轮 Windows / NVIDIA Vulkan 的已实施范围。**
Root TOOLCHAIN 完整产品闭包仍 BLOCKED；IPC R2 历史性能 PARTIAL 不变。
G11B–D / G12 / G13 NOT_STARTED；本轮停止，后续需用户另行授权。

## 精确输入与 I/V

- 目标：LUX-YU/lux-engine，`codex/render-v2-rebuild`。
- 起点及最终发布前核对远端：`9a2154371018b082dc2c0270a2c898559d69e213`。
- 历史 S0-I：`9b0bc12723fb63f781a413755d0ee4e8eb55fed6`；G10-I：`528c2c1f399df29343b9fe81d78be5160decd852`；G10-V 为本轮起点。
- G11A-0/1：`353d0c0731a8f2637d7a9cc71ef29b2c89363be8`。
- G11A-2：`c41864bea996df3bfe53760b99d3c4c71685c0ee`。
- G11A-3：`b45ab20f14b68640b38d260473f995d46b0ae927`。
- 最终 G11A Implementation I：`ed6df48ea8f631c6548f3479c33d1abea180e515`；Parent：`b45ab20f14b68640b38d260473f995d46b0ae927`。
- V 的 Parent 为上述最终 I，只新增本报告和 `G11A_EVIDENCE.json`。V 自身完整 SHA 与推送后远端 SHA 在提交后的交付信息给出，避免自引用。
- 包 SHA256：`2c7ba8aed1fc886c4af4da0a2f09786d8d1cc4cb7d5f7ae4b10208279072f11a`；六项 Manifest 与 ZIP CRC 已核对。
- 架构冻结、Legacy 算法审计和颜色选择见同目录 `G11A_ARCHITECTURE_FREEZE.md`、`SCENE_WORKING_COLOR_SPACE.md`；逐门禁施工见 `G11A_IMPLEMENTATION.md`，其中各时间点的 NOT_COMPLETE 保持原意。

## 独立性、命令和矩阵

施工：`E:/RenderV2Rebuild/G11A/source`。独立 V：`E:/RenderV2Rebuild/G11A/verification/ed6df48ea8f631c6548f3479c33d1abea180e515/source`。
`git clone --no-hardlinks --no-checkout` 后 detach 精确 I，无 alternates，全新三棵构建树。
先 `ValidateTrackedSnapshot.cmake`，配置、全量 all 两轮、全部 CTest、保护检查；V 源码始终干净，没有修实现或补本地头。

复现入口（完整命令、返回码和日志均已归档）：

```powershell
& E:/RenderV2Rebuild/G11A/verify.ps1 -Implementation ed6df48ea8f631c6548f3479c33d1abea180e515
& E:/RenderV2Rebuild/G11A/extra-verify.ps1 -Implementation ed6df48ea8f631c6548f3479c33d1abea180e515
py -3 E:/RenderV2Rebuild/G11A/final_checks.py ed6df48ea8f631c6548f3479c33d1abea180e515
py -3 E:/RenderV2Rebuild/G11A/analyze.py E:/RenderV2Rebuild/G11A/verification/ed6df48ea8f631c6548f3479c33d1abea180e515
```

每组实际执行 `cmake --build <build> --target all -j 4 -- -k 0` 两次，第二轮 `ninja: no work to do`；
`ctest --test-dir <build> --output-on-failure -j 1 -R .`。构建和实机测试串行，没有并发修改 DLL。
配置用真实 `cmake/render-v2-bootstrap`、MSVC19.44.35226.0、Ninja、CMake3.31.10、Vulkan SDK1.3.280.0。
ASan 使用 `/EHsc /fsanitize=address /Zi`、`ASAN_OPTIONS=alloc_dealloc_mismatch=1`，没有全局关闭异常。

| 独立配置 | all / 第二轮 | CTest | 时间 s | Validation / SyncValidation / ASan |
|---|---|---|---:|---|
| RelWithDebInfo | PASS / no-work | 72/72 | 135.52 | 0 GPU error；ASan N/A |
| Debug | PASS / no-work | 72/72 | 141.18 | 0 GPU error；ASan N/A |
| MSVC ASan | PASS / no-work | 72/72 | 180.67 | 0 GPU error / 0 ASan error |

原 62 个具体测试名全部保留，新增 10 个；三组名字集合与编译闭包见 `verification/matrix-integrity.json`。
新增：native.transfer / texture_upload / texture_storage / texture_sampling；runtime.gpu_ready_transfer /
gpu_ready_same_family / texture_progress / texture_operations / texture_rendering / texture_rendering_alias。
公共头 C++20、`/GR-` 独立编译；编译命令无 Legacy 或用户主工作树源码回流。

## 所有权、类型和提交点

一个 DeviceSession 继续唯一拥有 VMA Allocation 账本、真实 Queue/Retirement。队列别名只有一个物理 owner；
完成/释放按 ticket 的真实 owner 查询，不跨 Queue 比较 serial。G10 reserve 记录指定队列的 frontier，commit 只能接受之后真实提交的 ticket。

新增实际职责：

- 中立值：TextureId（SlotKey）、TexturePublished、TextureRegion、UploadTexture、UploadTextureRegion 及三个 G2 Schema 特化。TextureId 不是 descriptor index，也不是 hash。
- TextureStorage：唯一 Global 业务 SlotMap；内部 Record 只指向 current version 并记 updating。
- TextureVersion：固定容量 StableSlotMap 的不可移动物理版本，持 ImagePermit、View、Sampler、descriptor slot、真实 last-use 和 Context leases。多版本是同一资产在途快照，不是第二份业务身份表。
- TextureUpload：一个受控 Native 候选，分块 CPU 准备/提交/完成状态；不拥有第二份 Request。TextureUploadOptions 是宿主 enqueue/join 合同，TextureUploadStats 是计数。
- DeviceSession::UploadState / Page：固定 staging 页与受控借用，未建立新的 UploadManager、线程或 Completion Registry。

原 `RenderServer::Impl::DeferredCompletion` 仍是唯一持源 RenderRequest 的固定容量集合；TextureUpload 是其中一种受控任务，
预留 Reply/Deferred 容量、准备候选、真实提交、最终 Graphics 完成、Storage publish、原来源 Reply 依序发生。
普通 GPU-ready `reply()` 仍不能伪成功。Texture Feature 使用现有开放注册表，不在 Server 中硬编码业务 switch。

完整替换和 Region 更新使用新物理版本。Region 先 clone 全图，再覆盖选定 mip/block rows；旧 sampled backing、descriptor 和 leases
不会被改写。最后 GPU ticket 和 Context lease 都安全后才回收。Release 立即失效业务 generation，已附着消费者仍可安全读旧版本。
新版本发布只在拓扑变化点刷新预解析 Context 指针；稳定帧没有名称、TextureId、Schema、Feature 或 GraphAnalysis 查找。

Stop：关闭接纳、join 已接受 compile/prepare、drain 实际 GPU、清偿原请求，再 reset Responder、清理 Context/Storage/Device、最后 Instance。
Client abandon/close 不等于 GPU cancel；创建 Client 不是 Global Texture Owner。原完整 backing retained-owner 预算直到真实 Owner 释放才结算。

## 真实 GPU 和失败矩阵

RTX 4070 Laptop GPU，API1.4.341，driver610.88.0.0（完整能力来自归档 vulkaninfo）。
三条实测配置：alias Graphics/Transfer=family0 queue0；same-family Transfer=0:2 → Graphics0:0；
dedicated Transfer=1:0 → Graphics0:0。family1 为真实 transfer-only；各自真实 Image Copy、timeline wait、EXCLUSIVE release/acquire 和 readback 通过。
最终 Graphics ticket 经真实 wait 覆盖 transfer copy；不是 FrameSerial/CPU ready。限制 copy granularity 时选已存在 Graphics queue，未额外创造 Queue owner。

| 门禁 | 已观察结果 |
|---|---|
| 上传/真实消费者 | 1/16/64 MiB，多 mip、RGBA8 UNORM/SRGB、BC1 SRGB，真实 Feature→Assembly→Compiler→Executor sample/readback |
| Global | 无 Scene/View 先上传，跨 Scene/View/Client 共享；创建 Client 关闭后另一 Client 更新/释放 |
| 原子替换 | full replace、padded Region、mip/BC block，旧 revision/descriptor 保活；失败保留 last-good |
| 信任边界 | 坏范围/长度/pitch/mip/block、溢出、Scope/ABI/Schema/Session/旧 generation 拒绝，固定 Reply 容量先预留 |
| 容量 | Completion、staging page/source bytes、Queue slot、descriptor version、resident image bytes 拒绝/有界推进，无兜底大 allocation |
| 部分成功 | 首 chunk 实际 Submit 后下次 Submit 失败，前 ticket、源 Owner 和旧 published version 仍被保活；仅一次失败终态 |
| Descriptor | in-flight slot 不复用，双版本并存，最终 lease/ticket 消失才恰好一次回收 |
| 并发/关停 | Main64 回复满1.5s，Upload 和四 View 继续；两冷编译 Worker 并行；CPU 准备中 Stop、Client close、abandon、延期 Readback 回归 |
| DeviceLost | 既有测试钩子在真实首提交后注入后续提交失败；不是声称诱发了物理 GPU reset |
| 旧合同 | G9/S0/G10 全部相关测试保留；R5 同 Shader 1 Pipeline/1 Bind/1 Scope/2 Draw，不同 Shader 2/2/1/2 不退化 |

所有成功最终运行的 GPU Validation/SyncValidation error=0、ASan error=0。
Release 三种 G10 Queue 配置的 Native Buffer 创建/销毁分别为 3218/3218、3234/3234、3232/3232。
保留 `VK_LAYER_EOS_Overlay` Win32/Win64 重复 layer warning（每次实例通常1条，部分双实例2条），不称 warning=0。

颜色：scene-linear BT.2020/D65；SRGB 由硬件 EOTF 一次，再做709→2020；旧 LINEAR 明确按线性709兼容转换；
DATA 不转换且逐值精确；UNKNOWN 与 encoded-SRGB premultiplied 的未支持组合拒绝。alpha不做gamma，linear premultiplied保留。
独立 double oracle 实测 max error=0/255，合同容差仍2/255。原 TextureInfo 声明只搬到轻量头，enum值/布局/资产codec未改；
没有 HDR显示、Tonemap、WSI、自动mip或MaterialGraph改造。

## 内存与性能（精确范围）

默认：4×1MiB staging、256MiB Native pending source bytes、128物理descriptor版本、256MiB resident image allocation bytes、128Deferred slots。
原 Runtime retained-owner budget 另按完整 backing 计费，不能用可见span代替。Native source预算只描述其借用输入范围；两者不混淆。
单候选一次租一页；1/16/64MiB 样本使用1/16/64 chunks、固定1MiB实际staging，pending peak64MiB。
Storage测试 observed resident_peak=8MiB，测试容量16slots；这不是 VMA allocator保留block或driver总显存峰值。
新未提交Image候选可先分配再尝试resident版本准入，拒绝后释放；resident账本限制的是准入版本，不宣称整个设备分配瞬时硬上限。
Region clone使替换期间双份image并存，预算需要容纳两份。

尺寸：IpcMessage144、IpcReply296、RenderRequest216、Permit16不变；Binding32、ExecutionContext224（增加24B指针vector；ABI1为空）、
RetirementEntry32、TextureVersion512、TextureUpload648；DeferredCompletion512（前448，默认128槽多8KiB）。
10k allocation/binding冷态账本1,343,640B、61次new；稳定四View Engine new=0；Native/VMA驱动内部分配不能由此推断为0。

以下 Release Native 实测每个尺寸/配置一条观测；不是性能置信区间或相对旧版加速结论。
CPU prepare 是逐页memcpy累计；owner是推进/flush/record/submit/poll累计；wall含创建和完整readback。
GPU query是真timestamp，包含copy及命令内barrier，排除CPU准备/调度/readback/单独Graphics acquire；不是端到端带宽。
固定每页query pair，真实ticket完成后host reset/读取，无WAIT_BIT。

| 模式 | MiB | prepare ms | owner ms | GPU copy ms | copy GiB/s | wall含readback ms |
|---|---:|---:|---:|---:|---:|---:|
| alias | 1 | 0.045 | 1.238 | 0.087 | 11.27 | 4.432 |
| alias | 16 | 0.761 | 3.223 | 1.357 | 11.51 | 11.983 |
| alias | 64 | 3.986 | 12.296 | 5.474 | 11.42 | 60.923 |
| same-family | 1 | 0.041 | 1.735 | 0.086 | 11.30 | 4.792 |
| same-family | 16 | 0.882 | 3.664 | 1.368 | 11.42 | 12.037 |
| same-family | 64 | 4.114 | 14.042 | 5.505 | 11.35 | 66.184 |
| dedicated | 1 | 0.043 | 1.660 | 0.085 | 11.53 | 4.389 |
| dedicated | 16 | 1.007 | 3.356 | 1.339 | 11.67 | 11.764 |
| dedicated | 64 | 4.229 | 13.873 | 5.388 | 11.60 | 58.416 |

四View CPU记录节奏（ms），每4个draw记录一次；包含既有1ms sleep、OS调度和poll，**不是GPU帧时或每View presentation间隔**。
idle/upload各保存全部样本，分位取排序后的floor(n×p)，max不剔除。

| 模式/阶段 | n | p50 | p95 | p99 | max |
|---|---:|---:|---:|---:|---:|
| alias / idle | 146 | 2.001 | 2.482 | 2.652 | 3.551 |
| alias / upload | 760 | 2.010 | 2.401 | 2.914 | 3.944 |
| dedicated / idle | 146 | 2.004 | 2.419 | 2.881 | 3.960 |
| dedicated / upload | 712 | 2.015 | 2.761 | 3.126 | 4.048 |

16MiB实际Operation路径计时（ms）：逐次encode/send；total包括宿主/IPC/所有chunk等待、GPU完成、Reply消费和decode。
注意CSV字段 `completion_consume_decode_ns` 的计时起点在wait返回后，实际只测decode及其检查/Owner析构，不包括成功tryReceive；
本表保留原字段名方便定位，不能将其宣传为完整consume+decode成本。成功tryReceive成本已包括在total，但本新夹具未单独分离。
构造输入数据不在计时内，现有1ms sleep未删除。G10原有每配置1000个完整小Operation样本也继续归档，不以预编码队列微基准替代。

| 模式/计时 | n | p50 | p95 | p99 | max |
|---|---:|---:|---:|---:|---:|
| alias / submit_encode_ns | 22 | 0.0043 | 0.0102 | 0.0110 | 0.0110 |
| alias / completion_consume_decode_ns | 22 | 0.0050 | 0.0136 | 0.0275 | 0.0275 |
| alias / total_ns | 22 | 68.1200 | 70.9245 | 72.8749 | 72.8749 |
| dedicated / submit_encode_ns | 20 | 0.0047 | 0.0235 | 0.0235 | 0.0235 |
| dedicated / completion_consume_decode_ns | 20 | 0.0052 | 0.1655 | 0.1655 | 0.1655 |
| dedicated / total_ns | 20 | 71.6908 | 78.8187 | 78.8187 | 78.8187 |

节奏尾部/最大值全部保留；当前观测不足以将差异单独归因于GPU copy、OS唤醒或驱动，因此不作“专用Queue更快”结论，未据此猜测优化。
Release关键原始计数如下（其余两配置见summary和LastTest）：

```text
Native CPU record/execute: samples=1000 warmup=20 new_calls=0 p50=118.500 p95=181.700 p99=227.600 max=1416.600 us; GPU Timing=NOT_MEASURED; fence wait outside timing
VkBuffer=8 VkImage=8 BufferAllocation=56 ImageAllocation=72 Binding=32 Context=224 RetirementEntry=32 host_records=10000 ledger_and_bindings_bytes=1343640 new_calls=61
texture storage descriptors=16 resident_peak=8388608 bytes; TextureVersion=512 TextureUpload=648; exact last-good/generation/leases
texture sample: BT2020 double oracle DATA exact SRGB/LINEAR tolerance=2/255; 2 scenes/4 views/4 mips; new=0 stable; descriptor_binds=1633 max_error=0/255 validation_errors=0 warnings=1
G9 Runtime: 2 Scenes/4 Views/1 code; hot ticks=574 submissions=2295 engine_new=0; Validation/SyncValidation errors=0 warnings=1; API_us p50=16060.300 p95=32030.400 p99=36884.100; upload_progress=273
texture rendering queue=dedicated 2scene/4view new=0 two_compile_workers=2 main64_full_ms=1500 upload_progress=20 graphics_progress=2948 compiled_graphs=3 warnings=1
texture rendering queue=alias 2scene/4view new=0 two_compile_workers=2 main64_full_ms=1500 upload_progress=22 graphics_progress=3065 compiled_graphs=3 warnings=1
```

## SDK、保护与未完成范围

新prefix install + relocation，Native installed consumer真实上传/sample/readback，Feature/Graph/Data/Protocol/Runtime/IPC安装测试通过。
纯Client消费者 `CMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE`，无Native/Vulkan target，构建两轮及实际运行通过。
CMake 保留该开关未被使用的 warning，因为纯 Client 闭包根本没有调用 Vulkan 查找；不是绕过必需依赖。
32个公共头从精确I的Git blob同步三个prefix共96份，先备份，含rdesc轻量头；manifest记录原/新SHA。
Android头同步不是Android构建。根TOOLCHAIN真实configure返回1，仍缺旧render/client、feature_client.cmake、standard_content；未造占位目标。

3094个受保护文件（包含Legacy719/719）、旧报告/checker、IPC算法和两份用户修改逐项一致。
用户主树HEAD仍f80902e100879b114e848cabe1469540aba4d25d：
settings SHA256=f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c；
Graph.hpp SHA256=00c28abc7bd0a786666f101a5212400a5b467af6bdbbc5049c69d59d548c726a。
依赖lux-cxx bf779515a120350c7c5412c366eea73afdf59ccf、toolset c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60保持干净。

已实施边界：2D单layer、RGBA8 UNORM/SRGB与可选BC1 SRGB、显式mip字节、nearest/integer LOD。
一行须容纳于配置chunk；有lease时不相容尺寸/格式/layers/mips替换拒绝。Region当前整图clone，未优化为sparse residency。
没有通用Texture业务更新框架、Mesh/MaterialStorage、自动mip、3D/Cube或G11B–G13功能。
Linux/Android构建、TSan为NOT_RUN；实际GPU copy timestamp已测，但GPU frame timestamp NOT_MEASURED。
IPC历史PARTIAL、Root产品BLOCKED与G11A模块PASS分开，不回写历史。

## 中间失败及不可变证据

保留首次独立Queue饱和fixture错误：四GraphicsView无法填满单独TransferQueue；改为填该真实Queue后通过，未改生产容量。
保留同数值RequestId fixture错误：只有一侧先做过拒绝提交而推进generation；对称执行后通过，未改IPC身份。
保留timestamp初次vkCmdResetQueryPool在transfer-only的真实VUID：改用已协商hostQueryReset，前ticket完成后reset。
保留头依赖泄漏的早期编译失败，使用轻量UploadOptions接缝修复，未禁用头独立测试。
外置编译命令审计脚本首次只识别`/std:c++20`，实际CMake/MSVC命令使用等效`-std:c++20`，因此误报；
保存失败输出后修正解析，两种拼写仍严格要求C++20和`/GR-`，三组各119个TU通过，未改验收源码或编译选项。
前版I 9a96d970675ea8ff3e99f396ad5d4b447a319d57已独立三组72/72；最后diff审查发现测试夹具EOF空行，
只删除该行形成最终I，重新clean clone完整三组；未拿前版测试代替本版资格。最终diff --check通过。

外置按最终I归档：`E:/RenderV2Rebuild/G11A/archive/ed6df48ea8f631c6548f3479c33d1abea180e515`。
INDEX.json SHA256：`e418e5ab700030324032742e4471d9afefa315efe4aca1eee1af3a063c8b726d`。
包含原包、manifest、逐条命令/返回码、失败日志、能力/驱动、全量构建/CTest、全部CSV、SDK备份/清单、保护与依赖SHA。
源码树只提交报告和定位index，不将大量原始日志加入Git。报告不改写任何旧验收资格。

## 精确文件变更（相对起点，含施工说明与测试）

```text
A	.internal/editor-redesign/G11A_ARCHITECTURE_FREEZE.md
A	.internal/editor-redesign/G11A_IMPLEMENTATION.md
A	.internal/editor-redesign/SCENE_WORKING_COLOR_SPACE.md
A	.internal/editor-redesign/check_render_g11a.py
A	.internal/editor-redesign/run_g11a.ps1
M	modules/function/render/core/CMakeLists.txt
M	modules/function/render/features/CMakeLists.txt
A	modules/function/render/features/include/lux/engine/render/features/Texture.hpp
A	modules/function/render/features/src/Texture.cpp
M	modules/function/render/features/test/CMakeLists.txt
M	modules/function/render/graph/include/lux/engine/render/graph/Graph.hpp
M	modules/function/render/graph/src/Builder.cpp
M	modules/function/render/graph/test/Graph.cpp
M	modules/function/render/native/vulkan/CMakeLists.txt
M	modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/CompiledGraph.hpp
M	modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/DeviceSession.hpp
M	modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/ExecutionContext.hpp
M	modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/Program.hpp
M	modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/device/Device.hpp
M	modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/memory/Memory.hpp
A	modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/texture/TextureStorage.hpp
A	modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/transfer/TextureUpload.hpp
A	modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/transfer/UploadOptions.hpp
A	modules/function/render/native/vulkan/pinclude/UploadState.hpp
M	modules/function/render/native/vulkan/src/Compile.cpp
M	modules/function/render/native/vulkan/src/DeviceSession.cpp
M	modules/function/render/native/vulkan/src/Execution.cpp
M	modules/function/render/native/vulkan/src/Shader.cpp
M	modules/function/render/native/vulkan/src/device/Device.cpp
M	modules/function/render/native/vulkan/src/memory/Memory.cpp
A	modules/function/render/native/vulkan/src/texture/TextureStorage.cpp
A	modules/function/render/native/vulkan/src/transfer/TextureUpload.cpp
M	modules/function/render/native/vulkan/test/CMakeLists.txt
A	modules/function/render/native/vulkan/test/TextureFixture.hpp
A	modules/function/render/native/vulkan/test/TextureSampling.cpp
A	modules/function/render/native/vulkan/test/TextureSceneFixture.hpp
A	modules/function/render/native/vulkan/test/TextureStorage.cpp
A	modules/function/render/native/vulkan/test/TextureUpload.cpp
A	modules/function/render/native/vulkan/test/Transfer.cpp
M	modules/function/render/native/vulkan/test/installed/CMakeLists.txt
A	modules/function/render/native/vulkan/test/installed/Texture.cpp
A	modules/function/render/native/vulkan/test/shaders/Texture.hpp
A	modules/function/render/native/vulkan/test/shaders/cook_texture.py
A	modules/function/render/native/vulkan/test/shaders/texture.frag
A	modules/function/render/native/vulkan/test/shaders/texture.vert
M	modules/function/render/runtime/CMakeLists.txt
M	modules/function/render/runtime/include/lux/engine/render/runtime/RenderServer.hpp
A	modules/function/render/runtime/include/lux/engine/render/runtime/TextureFeature.hpp
M	modules/function/render/runtime/pinclude/Deferred.hpp
M	modules/function/render/runtime/src/RenderServer.cpp
M	modules/function/render/runtime/src/Request.cpp
A	modules/function/render/runtime/src/TextureFeature.cpp
M	modules/function/render/runtime/test/CMakeLists.txt
M	modules/function/render/runtime/test/GpuReady.cpp
A	modules/function/render/runtime/test/TextureOperations.cpp
A	modules/function/render/runtime/test/TextureProgress.cpp
A	modules/function/render/runtime/test/TextureRendering.cpp
M	modules/resource/description/include/lux/engine/description/Texture.hpp
A	modules/resource/description/include/lux/engine/description/TextureInfo.hpp
```
