# G5-C → G9 独立综合验收 V

日期：2026-10-14。适用范围：Draft 7.3 / Batch Plan v7 / Integrated Command，以及用户单独确认的 IPC 有界附件 API。

**本批最小真实 Offscreen 路径：PASS。完整 TOOLCHAIN / Editor 产品闭包：BLOCKED。**
IPC R2 历史性能 PARTIAL 原样保留。G10–G13 NOT_STARTED，须另行授权。
这不是 Legacy 功能对等、专业 GPU Storage 或完整产品验收。

## 1. 提交、独立来源与证据

目标远端分支：`codex/render-v2-rebuild`。实施分支：`codex/g5c-g9-implementation`。
开工与交付前重新 fetch 所得远端均为 `9dc4fcf1146e375172bb970d3a335b70e1145637`。

| Gate | Implementation SHA | Parent |
|---|---|---|
| M0 / G5-C | `bde3b9aeeb700d9b83982ee28a7707954c4f77fa` | `9dc4fcf1146e375172bb970d3a335b70e1145637` |
| M1 / G6 | `69a46c5b10a1638f35bdf75e26576d08fdd60804` | `bde3b9aeeb700d9b83982ee28a7707954c4f77fa` |
| M2 / G7 | `f4ab9433fecf7fc6f338bd2e3655a85bb313465d` | `69a46c5b10a1638f35bdf75e26576d08fdd60804` |
| M3 / G8 | `931ca007c3e5cea98b69840ebbe1100b573d6f32` | `f4ab9433fecf7fc6f338bd2e3655a85bb313465d` |
| M4 / G9，最终 I | `8ed79e116909c92336da2700313b0e0edb2d0a57` | `931ca007c3e5cea98b69840ebbe1100b573d6f32` |

本 V 的 Parent 是最终 I；V 自身 SHA 在提交后交付，不在文件内自引用。V 只增加本报告与 JSON 索引。
独立源码：`E:/RenderV2Rebuild/G5C-G9/verification/8ed79e116909c92336da2700313b0e0edb2d0a57/source`。
通过 `git clone --no-hardlinks --no-checkout` 后 detached checkout 精确 I，无 objects alternates。
首先执行 ValidateTrackedSnapshot，随后配置全新构建树；验收期间及结束时 tracked/untracked 状态均为空。
验证克隆没有源码修复，也没有用原工作区、旧 SDK Render 头或被忽略的本地源码补齐构建输入。

完整归档：`E:/RenderV2Rebuild/G5C-G9/archive/8ed79e116909c92336da2700313b0e0edb2d0a57/`。
280 个证据文件包含原始失败/成功日志、三配置 LastTest、CSV、编译命令、Ninja 链接、SDK 清单、备份和脚本。
`INDEX.json` SHA-256：`359f5398c116c02388039593cca2aa7e495581f906092923f1968e251df07f0e`。
随本报告提交的 `render-batch-g5c-g9-verification.json` 是该索引的逐字副本，包含全部 **70** 个实施变更路径。
原始二进制、日志和样本留在源码树外，不把报告中的摘要代替原始证据。

## 2. 实际对象与唯一所有权

| 对象 | Owner / 分配身份 | 生命周期与用途 |
|---|---|---|
| InstanceContext | RenderServer 的私有 State | 持有真实 VkInstance/Validation，晚于 DeviceSession 销毁 |
| DeviceSession | 同一 State 的 unique_ptr | 唯一设备、VMA、真实 Queue Owner、退休、Context 和 Scoped Buffer backing |
| Scene / View | Server SlotMap / Scene 内部 View SlotMap | 使用已有强类型 SlotKey generation；没有第二份 View Owner |
| Target / RenderEffect | Server 各自 SlotMap | Target epoch 与 ID generation 分离；Effect 持有不可变编译代码 |
| Operation / Request | Server frozen SlotMap / 每个 Client 的 SlotMap | Operation 域共享；RequestId 连接局部，真实 ReceivedRequest 保留来源 |
| CompiledGraph | RenderEffect 的唯一代码引用，Context 受控 shared const pin | 一份 Native Pipeline/Layout、ResourcePlan、Recipe；无第二可编辑 Graph/Registry |
| ExecutionGroup | Effect 内唯一 vector | **恰好 SceneId、ViewId、TargetId、ExecutionContext*** 四字段；无 GroupId/反向权威索引 |
| ExecutionContext | DeviceSession stable unique_ptr；撤销后转入现有 RetirementQueue | 自有 Transient/Readback backing 与 ImageView，预解析 Binding/Barrier，保留代码与 Scope lease |
| ScopedBuffer | DeviceSession 内唯一物理 Owner | Global / Scene / View / Attachment；完整 key、type、ABI、size、usage 在 prepare 比较 |
| IPC Input / Output | 现有 IpcMessage 独占 Owner | Header、最多四个只读附件视图统一保活；没有逐附件 IPC Owner |

ScopeIdentity 仅组合已验证的 SceneId/ViewId，不分配身份、不复制 generation。Attachment-private 以稳定 Context
身份隔离；零 lease 的旧 backing 不会因地址复用而错误复活。没有 ResourceDomain + ScopedResources 双权威。
小型 BufferInitializer 是已有真实 CPU 初始化消费者，不是通用虚拟 GPU Storage；专业存储尚未迁移。

M0 在 G4 增加 dependencyCount，只审计实际新增显式依赖两端的 Feature 归属。M2 有界增加中立 EGraphUse，
与逻辑 EGraphAccess 分开。G7 直接消费现有 FeatureAssembly.analysis，不重做拓扑或再 analyze；Scope 分配为 0。
真实 SPIR-V 来自 glslc，SPIRV-Tools 验证、SPIRV-Cross Reflection 校验 ABI，Native 编译真实 Pipeline。

BOUNDARY：Wire/Operation/Session、所属 SlotMap generation、G5 配置、Native Shader/设备能力、Attach/Resize 相容性。
INVARIANT：成功准备后使用固化索引、句柄、Context 指针；不在稳定帧重新查业务 ID/名称、分派 Feature 或编译。
STATE-CHANGE：Queue 容量、真实 Submit 返回值、Ticket completion、退休和读回仍检查。
重复物理 Image 的首次覆盖虽丢弃旧内容，仍保留前次执行访问的显式同步依赖；不以 UNDEFINED 绕过同步。

## 3. 构建、安装与保护

环境：Windows 11 Pro 10.0.26200，Intel i9-13900H（14 核 / 20 线程），MSVC 19.44.35226.0，CMake 3.31.10，Ninja。
lux-cxx：`bf779515a120350c7c5412c366eea73afdf59ccf`；lux-cmake-toolset：`c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`。
依赖工作树干净；隔离 SDK 的 64 个现有 cxx 公共头与该源码内容一致（按 CRLF/LF 规范化比较并记录 SHA）。
五份交接文档与提供的 SHA256SUMS 全部一致。

实际复现脚本 `run.ps1`、`verify.ps1`、`extra-verify.ps1` 已归档。核心命令为：

```powershell
git clone --no-hardlinks --no-checkout <implementation-repository> <source>
git -C <source> checkout --detach 8ed79e116909c92336da2700313b0e0edb2d0a57
cmake -DLUX_SOURCE_DIR=<source> -P <source>/cmake/ValidateTrackedSnapshot.cmake
cmake -S <source>/cmake/render-v2-bootstrap -B <build> -G Ninja `
  -DCMAKE_BUILD_TYPE=<Debug-or-RelWithDebInfo> -DBUILD_TESTING=ON `
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset `
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx `
  "-DCMAKE_CXX_FLAGS=/EHsc <asan-flags>" -DCMAKE_CXX_SCAN_FOR_MODULES=OFF `
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> --output-on-failure -j 1 -R .
py -3 <source>/modules/function/render/native/vulkan/test/check_batch.py `
  --repo <source> --build <build> --clean --user-worktree E:/SyncForder/CodeRepos/lux-engine
```

ASan 使用 RelWithDebInfo、`/fsanitize=address /Zi`、`ASAN_OPTIONS=alloc_dealloc_mismatch=1`。
所有构建/实机验证串行；没有禁用失败测试、放宽超时或创建占位 Target。

| 独立配置 | all 构建 / 第二轮 | CTest | ASan / GPU |
|---|---|---|---|
| RelWithDebInfo | 成功 / no work | **56/56**，50.38 s | GPU Validation + SyncValidation |
| Debug | 成功 / no work | **56/56**，47.68 s | GPU Validation + SyncValidation |
| ASan RelWithDebInfo | 成功 / no work | **56/56**，59.93 s | ASan 无诊断；真实 GPU 用例同样执行 |

包含 Core/Data/Protocol/Graph/Feature 原回归、Native、Runtime、IPC 全部当前 CTest。
独立公共头 C++20 / 无 RTTI 编译纳入 all。每组安装测试使用全新前缀，移动后外部 find_package 构建并运行。
另有纯 Client 消费者设置 `CMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE`，断言没有 Native/Vulkan target，构建两轮
并运行通过；其实际编译命令没有 Vulkan/VMA。Graph/Features/Protocol/Client 的 source/include/link/install
闭包不引入 Legacy、Editor、ECS、stdexec 或 Native；专用 Native SDK 按需依赖 Vulkan/SPIR-V SDK。

SDK 同步：最终 I 的 **24 个公共头 × 3 前缀 = 72 份**，同步前备份并记录前后哈希。
Debug/include、RelWithDebInfo/include、Android/lux-engine/include 都使用 git show I 的内容，未混入用户未提交头。
这些是工程规定的头同步；完整可运行 SDK 资格来自本次全新安装/移动前缀。IPC ABI 已改变，旧 DLL 消费者必须
重新安装匹配库并重新编译。Android 头同步不代表 Android 构建通过。

保护：719/719 冻结 Legacy blob 与冻结 manifest、提交及工作区一致；3,016 个保护文件包含历史报告保持原样。
原工作区仍只有原先两项修改，SHA-256 为：

- `.vscode/settings.json`：`f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c`
- `Graph.hpp`：`00c28abc7bd0a786666f101a5212400a5b467af6bdbbc5049c69d59d548c726a`

允许的生产变更集中于 Native/Runtime、新 Feature Program/BufferInitializer 接缝、G4 dependencyCount/EGraphUse、
G5 审计及授权的 IpcMessage。Core/Data/Protocol 无生产修改，旧检查器/历史报告未改。
新检查器同时确认 Ipc.cpp 中消息方法以外的目录、Channel/Link、SPSC、回复额度和退休算法与基线一致。

## 4. 真实 GPU、IPC 与生命周期结果

设备：**NVIDIA GeForce RTX 4070 Laptop GPU**。实际 API raw=`4211029`，driver raw=`2559967232`；SDK 1.3.280.0。
开启 `VK_LAYER_KHRONOS_validation` 和 synchronization validation。三配置所有 Native/Runtime 用例错误数均 **0**。
保留 Epic Overlay Win32/Win64 重复层警告，普通用例 1、双 Instance 用例 2；不声称零 warning。

- G6：真实 Instance/Device/VMA、Buffer/Image、Queue submit、Fence/Ticket、跨队列依赖与实际队列别名；半初始化和
  后续 submit 故障测试保留前次已接受 Ticket。Fault seam 仅测试构建，生产没有间接故障派发。
- G7：真实 transfer / graphics 编译、实际 Pipeline/Layout、ResourcePlan/Barrier；重复消费既有 Analysis 不回调
  G5。编译期间 Buffer/Image 创建计数为 0。错误 Target、Shader ABI、阶段、能力拒绝并清理候选。
- G8：真实两个 Client、相同数值 RequestId 不串线、完整能力/Schema 绑定、旧服务 epoch/旧 View generation 拒绝、
  Native 初始化失败不假 Ready、正常 Stop/关闭/晚 Reply 安全。1 MiB Asset 的 64 B 可见片段按整个 backing 计费，
  预算满拒绝保持输入，返回值持有和 abandon 不提前退款；实际 G2 解码看到原始 Asset 地址，关闭后恰好一次释放。
- G9：真实 Feature → G4/G7 → Attach → Execute → Ticket → GPU Readback。Triangle 的中心/边缘与 Transfer clear
  的像素通过数值校验。两个 Scene / 四个 View 共用一份代码；Global 1 + Scene 2 + View 4 + Attachment 4 共 11
  个 scoped buffers，独立 transient/readback。Native 测试直接比较共享/隔离 VkBuffer，并实际 GPU copy/readback。
- 重复 Target Writer 拒绝；Detach 一个 View 不影响其余；移除 Scene 不影响另一 Scene；兼容 resize 32×32→64×48
  保持 TargetId、epoch 1→2，不编译；samples=4 拒绝且 last-good 像素仍可读。
- Main 64 个请求/回复额度占满并停顿 1.5 s，Upload 完成 262 个 create/remove 循环，实际 GPU submit 继续前进。
  每 epoch 累计 Main/Upload 两个长期参与者，仍遵守 IPC R2 同一逻辑 Owner 合同；不宣称通用恶意裸 IPC 的全局预算。
- 延期 Readback 等实际 Ticket；真实提交后，测试控制 completion **观察**保持 Pending，再执行 Stop/Requester close。
  Stop 返回 Cancelled；关闭 Requester 不提前销毁 backing，最终真实 fence wait 后 image/view/buffer 各销毁一次。
  已返回 CPU 读回结果在 Server/Client 销毁后仍有效。所有 ReceivedRequest 先清偿，再释放 Responder；Native 另行 drain。

## 5. 性能测量及边界

RelWithDebInfo 独立样本，Validation/SyncValidation 开启；不把以下结果解释为关闭验证层的产品帧性能。

| 测量 | 样本 / 范围 | p50 | p95 | p99 / max | 分配 |
|---|---|---|---|---|---|
| Native CPU record/execute | 预热 20，采样 1000；poll + record + submit，显式 fence wait 在计时外 | 127.5 µs | 232.2 µs | 317.0 / 1413.6 µs | Engine C++ new/aligned-new = 0 |
| 完整 Client CreateScene | 100 次；encode/send、Server mutation、reply、poll/decode；RemoveScene 在计时外 | 30.7419 ms | 31.7595 ms | 32.2087 / 32.2087 ms | 每次 11 次 Engine C++ 分配 |

完整 API 包含当前 Server 的 1 ms sleep 和测试 poll 等待，未进行调度延迟优化或逐段因果归因；不能拿 IPC 队列
微基准替代这个成本。GPU 时间 **NOT_MEASURED**，不报告虚构 FPS 或相对 Legacy 的速度优势。

固定四个 View 的无输入窗口 1.2 s：544 ticks、2166 个真实 submit，Engine C++ heap 调用 **0**；Feature describe/build、
编译次数和 Scoped Buffer 创建计数不变。额外 fault 计数证明 execute 不创建 Shader/Pipeline/Layout/Buffer/Image。
这里只统计可归因的引擎 C++ 分配，不把 Vulkan 驱动、Validation DLL 的不透明分配算成“整个系统零分配”。

授权附件扩展的旧路径比较：基线 exact M2 `f4ab9433` 独立 clone，旧/新同编译设置，同一原测试夹具，三轮交替。
8/32/64/128 B、预编码队列、完整小 Operation、双物理核、1 MiB/256 MiB Owner 交接 CSV/输出全部保留。
小消息 EXE 与 IPC DLL 分配计数仍为 0。128 B 单线程 p99：旧三轮 700/700/1000 ns，新 600/900/2300 ns；
完整小 Operation 同线程 p50：旧 300/300/300 ns，新 300/300/200 ns。当前双核 p99 为 1800/1300/1400 ns，
旧 1200/1200/1300 ns。存在样本波动，不主张等价、改善或解释历史慢区间，历史 **PARTIAL 不变**。

Windows x64 ABI：IpcMessage **160→176 B**，IpcReply **328→360 B**，inline **128 B 不变**。
默认深度 64 的两条 Ring 仅元素载荷合计 **31,232→34,304 B**（+3,072 B，未含 Ring/Link/allocator 元数据）。
这是明确的 ABI/内存代价，不声称二进制兼容。附件上限 4，描述数组借用单一 Owner 内稳定存储。
新增测试覆盖过量附件拒绝、Move、Ring Full、正常 Reply、ABORTED、关闭后失败保留结果、最后引用准确释放，
没有修改 SPSC/发布/回收算法，也没有 G2 API 改写或大 Asset copyOf。

## 6. 失败证据、剩余阻断与停止点

实施过程中保留的真实失败：G7 初次编译的成员名/缺少 include 错误；Reflection 对非资源 RayQuery kind 的无效查询
导致 compiler fixture abort；Client 使用不存在的 SlotMap::contains；G9 测试初稿缺失类型/include；IPC 安装消费者
旧尺寸断言失败。全部在实施工作树修正，形成对应 M 提交后重验，未在独立 V 中改源码或削弱生命周期断言。
IPC 尺寸断言按授权后的真实 ABI 更新并保留明确说明。外部 runner 的一次位置参数误用、重复 VS 环境初始化的
命令行过长也留在日志中；runner 已修正。最终独立 I 的三配置无失败、无源码修补。

根 TOOLCHAIN 从精确 I 重新配置，**EXIT=1 / BLOCKED**：

- `modules/function/CMakeLists.txt:30` 仍引用旧 `render/client` 路径；本批真实 Client 在 `render/runtime` 组件中。
- 第 31 行 `render/features/cmake/feature_client.cmake` 尚不存在。
- 第 32 行 `render/standard_content` 尚不存在，并产生对应 install target 连带错误。

没有创建占位消费者或链接 Legacy，未把独立 bootstrap all 成功替代根产品 all 成功。
后续入口是另行授权的 G10 Feature Operation/Handler；根产品装配、专业 Storage、ECS/stdexec 等按后续阶段完成。

Linux / Android 构建 / TSan：**NOT_RUN**。ASan 不是数据竞争检测器。WSI/Present、动态 History、Async Compute 全拓扑、
Mesh/Texture/Material 专业迁移、插件热卸载、高级渲染：**NOT_IMPLEMENTED**。
当前 Native 执行仅完整范围 RGBA8、单 mip/layer/sample、真实 graphics push-color 与 transfer/readback；不支持项明确拒绝。

所有本批适用门禁通过，允许提交本 V 并正常 fast-forward 推送；不合并 main，不改原脏工作树，不进入 G10。
