# Render V2 Layering L0–L4 独立验收

日期：2026-10-16。结构/功能：**PASS（本轮已测边界）**；性能保护：**PARTIAL**；因此 L4 综合资格为 **PARTIAL**，不宣称无保留 PASS。Root TOOLCHAIN 仍为 **BLOCKED**。本轮停止，没有执行 P1/P2/P3/W1/E1/S3/S4/S5。

## 1. 提交与证据身份

| 点 | 完整 SHA | Parent |
| --- | --- | --- |
| 起点 / 推送前远端 | `7ca2828d60df50e54ed299efeb1cbab20cf369ad` | Git 历史原样 |
| L0 | `5623b5c1b4f7cbaa254913dc4006e38383dd91ed` | `7ca2828d60df50e54ed299efeb1cbab20cf369ad` |
| L1 | `c5d66814fb3e2e2b4164b4b55dcc88a1e5df3819` | `5623b5c1b4f7cbaa254913dc4006e38383dd91ed` |
| L2 | `d707148fccd2108c3a03f0e375529bda05f50333` | `c5d66814fb3e2e2b4164b4b55dcc88a1e5df3819` |
| 最终 Implementation I / L3 | `b427f9d5ac99bcec42a154e6a865585c058a3365` | `d707148fccd2108c3a03f0e375529bda05f50333` |
| Verification V | 本报告所属提交；完整 SHA 在提交后交付 | `b427f9d5ac99bcec42a154e6a865585c058a3365` |

规范 SHA-256：`5d7db67994b2cd940ed04f824c18007ed61f6f815deb36756a9f41b0fb0d7f70`。
施工 worktree `E:/RenderV2Rebuild/Layer/source`；独立验收 clone `E:/RenderV2Rebuild/Layer/v/src`，以 `--no-hardlinks --no-checkout` 建立，detached 精确 I，无 alternates，tracked snapshot 先通过。V 环境没有修生产或测试源码。

完整外部归档：`E:/RenderV2Rebuild/Layer/archive/b427f9d5ac99bcec42a154e6a865585c058a3365`，996 文件，210,016,817 bytes。`INDEX.json` SHA-256：`39a71f7cc495fb2f4975e7f2589abaf7edf0d453854b2971cedc4598b6364ea3`。

[便携原始证据包](layer-verification/evidence.zip)（2,959,055 bytes）SHA-256：`25820bad9dcec235977506ee677b5c3ce56b7f485930645d833449dc06355569`。包含三组完整 CTest stdout、原始配对/重复 CSV、VTune 报表、实验副本精确 diff、运行/汇总脚本、目录与 SDK 清单；VTune 二进制结果和 SDK 原文件备份在完整外置归档。不是只有百分位摘要。[索引](layer-verification/index.json)将两份证据绑定到 I。

依赖保持：lux-cxx `bf779515a120350c7c5412c366eea73afdf59ccf`，lux-cmake-toolset `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`，stdexec 0.10.0。

## 2. 实际结构变化与唯一 Owner

- Texture 声明和中立 TextureWrite/TextureRegionWrite 归 Resource；专业 Native upload 不再接受 Command 嵌套 Request。
- CommandHandler 和 StorageFactory 由有签名约束的模板构造；Config/Ops 在首次 cast 前校验实际 C++ 类型、尺寸、对齐。Protocol 不含 DeviceSession owner policy，GPU 完成资格由 Native 注册接缝建立。受信任扩展必须遵守 ODR、元数据和代码寿命合同；不宣称防御恶意插件。
- 删除 Session 的隐式 Texture Factory；实际显式安装的 Factory 决定创建行为，重复注册拒绝。零 Texture Session 和外部 Buffer 都有实机测试。
- FeatureDescriptor、FeatureGraphOps、FeatureInstanceView 分离；Assembly 仅负责资源供需与唯一 Graph 装配/分析，Command 校验回归唯一注册边界。ProgramContribution 分离程序与 Effect 参数；贡献归属仍由 G5 审计。
- ShaderInterface/SpirvArtifact 归 Data，PassProgram 归 Graph，GraphStorageBinding 归 Resource。Compiler 只接收现有 Definition/Analysis/Binding/Program/Target，不复制 Graph、不重新分析。
- 真正拆出 `render_native_vulkan_extensions` 和按需 `render_protocol_async`；只新增这两个组件，分别具有专业实现和异步头合同消费者。Native base 不链接 Feature/专业 extensions；同步 Protocol/Client 的安装闭包不要求 stdexec。
- Wire 术语改为 Encoding；两个 view 都是借用，Retainable view 只是借用可保留的 SharedBytes Owner 集合。直接复用 SharedBytes，旧 OwnedWireBytes 别名、Wire.hpp、Feature Program/Texture 旧头实际删除。字节 ABI/golden/range/overflow 检查不变。
- 一个 RenderServer::Impl 分为六个职责明确的 TU；34 个移动成员函数经批准的 Encoding 更名后 token 逐项相同，成员集合及顺序不变。OS 熵使用现有 platform_process；没有新增 Host/Manager/Registry/线程。

**长期 Owner 增量为 0。** 唯一 Command SlotMap、ResourceRegistry、DeviceSession/VMA、Effect CompiledGraph、DeferredSlots、IPC Channel/Link 都保持原权威。GPU Ticket、LastUse、回调先摘链再调用、Stop 清偿顺序及 CPU 完成事件机制保留。

[完整路径清单](layer-verification/changed-files.tsv)：起点至 I 的 rename-aware Git stat 为 154 个文件；清单以 --no-renames 记录 158 个路径（含测试、CMake、文档和搬迁两端）；其中 59 个生产 C++/头路径（删除/搬迁两端按路径计）。[定义名审计](layer-verification/production-delta.json)新增 16、删除 10 个定义名，含重命名；不含 aliases/concepts/forward，不将同名不同命名空间分开。不能把它解释为新增 16 个 Owner。

## 3. 独立构建、行为、GPU 与安装

环境：Windows 11、i9-13900H（14 核/20 线程）、MSVC 19.44、CMake 3.31.10、Ninja；RTX 4070 Laptop GPU，Vulkan API 编码 4211029、NVIDIA driver 编码 2559967232（610.88），Vulkan SDK/Validation 1.3.280.0。

| 配置 | 全量 all / 第二轮 | CTest | CTest 时间 |
| --- | --- | --- | --- |
| RelWithDebInfo | 通过 / no work | 102/102 | 169.78 s |
| Debug | 通过 / no work | 102/102 | 176.53 s |
| MSVC ASan（RelWithDebInfo） | 通过 / no work | 102/102 | 229.13 s |

[逐测试矩阵](layer-verification/matrix-summary.json)保留全部原 98 个名称；新增 `render.protocol.binding`、`render.native.explicit_factory`、`render.native.direct_graph`、`platform.process_arguments`。没有通过删除/重命名旧测试取得数量。

核心命令（完整实参/退出码在证据包 runners 与每组日志）：

```powershell
git clone --no-hardlinks --no-checkout <implementation-repository> E:/RenderV2Rebuild/Layer/v/src
git -C E:/RenderV2Rebuild/Layer/v/src checkout --detach b427f9d5ac99bcec42a154e6a865585c058a3365
cmake -DLUX_SOURCE_DIR=E:/RenderV2Rebuild/Layer/v/src -P E:/RenderV2Rebuild/Layer/v/src/cmake/ValidateTrackedSnapshot.cmake
cmake -S <src>/cmake/render-v2-bootstrap -B <r|d|a> -G Ninja -DBUILD_TESTING=ON -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DCMAKE_BUILD_TYPE=<configuration> -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx -Dstdexec_DIR=D:/DevTools/vcpkg/installed/x64-windows/share/stdexec
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> -V --output-on-failure -j 1 -R .
```

ASan 附加 `/fsanitize=address /Zi`，`ASAN_OPTIONS=alloc_dealloc_mismatch=1`；三组串行，不与运行验证并发。公共头 C++20 `/GR-`；没有靠关闭异常或删测试掩盖问题。

Validation/SyncValidation error=0，ASan error=0。保留 EOS Overlay 重复 layer warning、编译 `/DNDEBUG` 被 `/UNDEBUG` 覆盖的测试警告、安装 CMake warning；完整原文见 stdout。真实 GPU 包含原三 Queue 拓扑、Mip/BC/Region、两 Scene/四 View Pixel oracle、独立 Buffer Storage、T1 Clone/T2 Draw LastUse、部分提交失败、GPU Ready、双 Client 背压/断连/Stop、并发编译。

45 个 Render 公共头 source closure inventory 通过；fresh install→move prefix→外部 find_package/运行通过。纯 Client 禁用 Vulkan 和 stdexec 查找；同步 Protocol 禁 stdexec；Native 直接 Graph 消费者不构造 Feature。旧三个头在全新 SDK 中不存在。按精确 I 同步 48 个 Render/Process 公共头至 Debug、RelWithDebInfo、Android include 前缀，逐文件备份和 hash，删除三个废弃路径；这不是 Android 构建资格。

L1 初始配置资格缺失、L2 DirectGraph fixture 的 Domain/UNORM oracle、L3 安装 fixture 对旧头名和传递 Client include 的依赖，均在 I 前修复；失败日志仍保留。L2/L3 原门禁采用全量失败记录加针对性补验，不谎称那些第一次全量运行全绿。最终 I 的三组完整 102/102 没有失败或 V 中修源码。

## 4. 尺寸、分配与容量

| 实测项 | 基线 → I |
| --- | --- |
| IpcMessage / SBO / IpcReply | 144B / 80B / 304B，不变 |
| TypeOps | 88B，不变 |
| RenderRequest / DeferredCompletion | 256B / 736B，不变 |
| Reservation / Context borrow / Permit | 16B / 32B / 16B，不变 |
| BufferAllocation / ImageAllocation / Binding | 56B / 72B / 32B，不变 |
| ExecutionContext / RetirementEntry | 232B / 72B，不变 |
| PreparedBinding / StorageUse | 64B / 80B，不变 |
| StorageFactory | 72B → 152B；两份冷态 Config/Ops C++ 资格增加 80B |
| 10k allocation records + bindings | 1,343,640B，61 次冷态 new，不变 |
| 1000 Domain/Storage metadata fixture | 1111 次冷态 new 不变；累计 424,673B → 424,753B |

DeferredSlots 仍为 lux-cxx StableSlotMap 默认 256 项物理页；逻辑 pending 上限仍由 completion+compile 配置限制，最多 136，不把物理页容量宣传成逻辑准入。Queue 容量 3、Staging 两页/1MiB Chunk、sleep_for(1ms) 均未改变。

小 Command 每次真实构造/发送/执行/回复/消费的 EXE 和 Client DLL 首方分配均为 0；Native record/execute、四 View 稳定期断言仍为 0。128B 消息仍为外置对象并发生真实分配。驱动/Validation/VMA 分配与首方 new 不混计；没有宣称整个初始化或 GPU 命令零分配。

相邻合法 Graphics Pass：相同 Shader = 1 Pipeline/1 Bind/1 Scope/2 Draw；不同 Shader = 2/2/1/2，其他拆 Scope 用例也保留原计数。

## 5. 性能保护：真实差异仍为 PARTIAL

对照基线是起点的生产 I `2824180646c1c01eaa97ed86151abbc4852a4de1`，已核实它到 `7ca2828...` 的 modules/cmake 差异为空。使用原独立干净源/构建和新 I 独立构建，同机同配置 Validation 开启。没有拿旧 v11 施工前不同上传次数的结果代替本次对照。

第一组 4 轮，每轮顺序 AB、BA、AB、BA；每版本 4000 条完整 Command、4000 次 Native record/execute。固定 GPU 夹具每次 16×16MiB = 256MiB，256 chunks、4096 Draw；Alias 4096 graphics + 256 transfer submissions，Dedicated 4096 + 272（含 acquire）；全部逐 run 断言匹配。工作量控制和事件观测只存在源码树外的实验副本，精确 diff/编译命令已归档，V 源码未修改。

| 第一组 pooled p95 | 基线 | I |
| --- | --- | --- |
| 完整小 Command | 2.0758 ms | 2.0775 ms |
| Native record/execute | 155.8 us | 172.4 us |
| Alias 上传往返 | 50.6500 ms | 50.1729 ms |
| Alias 完整固定窗口四 Draw 组间隔 | 2.6953 ms | 2.9608 ms |
| Alias 上传活跃窗口四 Draw 组间隔 | 3.7703 ms | 3.7922 ms |
| Dedicated 完整窗口四 Draw 组间隔 | 2.5196 ms | 2.4489 ms |
| Dedicated 活跃窗口四 Draw 组间隔 | 2.4861 ms | 2.6087 ms |

没有筛掉不利样本。小 Command p99 2.1910→2.2789 ms；完整成本是 submit+wait_and_consume，成功消费时间已包含在 wait 中，不重复相加。固定 sleep、等待与调度包含在端到端成本，不能算成 IPC Ring 延迟。Native fence wait 在计时外。四 Draw 组是记录事件间隔，不等于 WSI 呈现帧间隔。

针对差异，再以 AB/BA 交替做 8 轮 Native+Alias：每版本 8000 Native 样本、128 上传、8184 完整窗口间隔。Native p95 **168.9→187.0 us（+10.7%）**；Alias 完整窗口 **2.5713→2.7817 ms（+8.2%）**，活跃窗口 **3.7525→3.7677 ms（+0.4%）**；上传 p95 **49.3086→52.0402 ms（+5.5%）**。因此不能把第一组变化全部归为噪声或宣布无回归。

[第一组全百分位](layer-verification/paired-summary.csv)、[重复组](layer-verification/repeat-summary.csv)及便携包保留每 run/每样本、p50/p95/p99/max、计数、时间窗、运行顺序与所有异常值。分位采用 nearest-rank `ceil(n*p)-1`，没有裁剪。准备 CPU、Owner CPU、真实 GPU timestamp 累计值、submit→ready→reply→consume 的事件数据分别保留；Native record/execute 的 GPU timestamp 为 NOT_MEASURED，不与上传查询池值混用。

VTune 2026.4 software hotspots 对两版同一 Alias 固定工作量，各采一次：只在 ITT 指定上传窗口 resume，其他时间 paused。基线/新版采样 CPU Time 为 0.178717/0.234429 s；VTune 将 sleep_for 栈分别记为 48.6%/56.6%，另有 Validation 等待、驱动与 Unknown（27.3%/7.0%）。这是短时采样分类，不证明 sleep 本身消耗了同等真实 CPU，也不足以锁定分层改动的某个函数为因果来源。

**未关闭项：** 固定工作量下的尾部差异已复现，但生产函数级归因未闭环。保持性能 PARTIAL；未改 Queue 容量、sleep、Callback、Staging 或引入优化缓存/线程。若继续处理，需单独 P1 性能门禁，以本次 I 和原始样本为新基线；本次停止，不假借分层授权提前优化。

## 6. 39 项处置矩阵

FIXED 只表示本轮指定结构动作及相应验证闭环，不提升未来功能资格。PROTECTED 表示回归/边界审计，不宣称修复未复现风险。

| ID | 项目 | 状态 | 处置/边界 |
| --- | --- | --- | --- |
| L01 | Texture 中立声明错置于 features | FIXED | 本轮生产与对应行为/安装测试闭环 |
| L02 | FeatureDefinition 混合元数据与 Graph 贡献调用表 | FIXED | 本轮生产与对应行为/安装测试闭环 |
| L03 | SelectedFeature / selection_index 命名带有不必要的选择语义 | FIXED | 本轮生产与对应行为/安装测试闭环 |
| L04 | Feature Assembly 重复审查 Command 注册合同 | FIXED | 本轮生产与对应行为/安装测试闭环 |
| L05 | Native Compiler 接受整个 FeatureAssembly | FIXED | 本轮生产与对应行为/安装测试闭环 |
| L06 | Shader、Pass 执行参数和 Effect Binding 值混入同一 PassProgram | FIXED | 本轮生产与对应行为/安装测试闭环 |
| L07 | Protocol 定义 DEVICE_SESSION 绑定政策 | FIXED | 本轮生产与对应行为/安装测试闭环 |
| L08 | gpu_completion 与语义完成合同并列，桥接含过宽 void* | FIXED | 本轮生产与对应行为/安装测试闭环 |
| L09 | StorageRegistration 和 Native 绑定入口混在 Factory.hpp | FIXED | 本轮生产与对应行为/安装测试闭环 |
| L10 | Texture Factory 在 DeviceSession 和显式注册中有两个安装入口 | FIXED | 本轮生产与对应行为/安装测试闭环 |
| L11 | Native TextureStorage 直接接受 SetTexture::Request | FIXED | 本轮生产与对应行为/安装测试闭环 |
| L12 | 手拼 Descriptor、函数和裸 Ops，编译期资格没有贯穿注册入口 | FIXED | 本轮生产与对应行为/安装测试闭环 |
| L13 | CpuSchedule / CompletionPost 归属 resource | DEFERRED | E1 执行层归位 |
| L14 | 纯 Protocol 使用者被强制传播 stdexec 依赖 | FIXED | 本轮生产与对应行为/安装测试闭环 |
| L15 | 公开头暴露范围和 include 闭包过大 | FIXED | 本轮生产与对应行为/安装测试闭环 |
| L16 | RenderServer.cpp 编排职责集中，平台调用混入 | FIXED | 本轮生产与对应行为/安装测试闭环 |
| L17 | DeviceSession 基础头知道 Feature，构造析构藏在 Transfer.cpp | FIXED | 本轮生产与对应行为/安装测试闭环 |
| W01 | OwnedWireBytes 把普通拥有型字节命名成传输格式 | FIXED | 本轮生产与对应行为/安装测试闭环 |
| W02 | WireView 与 BorrowedWireView 的所有权命名不够准确 | FIXED | 本轮生产与对应行为/安装测试闭环 |
| W03 | 同进程配置与Binding仍被强制编码，未区分持久格式需求 | DEFERRED | W1 配置对象表示 |
| W04 | 配置Schema、C++ ABI、Shader布局与资源参数合同混读风险 | FIXED | 实际 Config/Ops C++ 资格已补；未迁移编码配置表示 |
| W05 | 同一冷态配置存在重复校验和重复解码机会 | FIXED | 已完成真实消费者盘点；重复解码优化留 W1 |
| P01 | 共享物理Queue三槽竞争导致上传期View跳过增加 | DEFERRED | P1 Queue 公平性 |
| P02 | 每帧全量Storage和Backing退休扫描 | DEFERRED | P2 退休收集 |
| P03 | Registry查找为线性，不能把O(1)路由宣传为整个资源命令O(1) | DEFERRED | P2 查找规模 |
| P04 | 热Command记录偏大，多层擦除与对象移动尚有测量空间 | DEFERRED | P3 记录与移动成本 |
| P05 | 有限异步分配与物理槽页容量尚未完整量化 | DEFERRED | P3 有限异步分配 |
| P06 | execute返回bool混淆多种暂不可执行原因 | DEFERRED | P1 执行原因分类 |
| E01 | 异步操作寿命与回调代码寿命必须分别审计 | PROTECTED | 原 DLL/CodePin/尾栈/停止测试回归；不宣称修复未复现 UAF |
| E02 | Effect编译Worker借用Session的可并发范围需要明确 | PROTECTED | 冻结注册和原请求保活，Worker 借 Session 编译，Owner 线程采用结果；并发编译/停服回归 |
| E03 | 固定1ms sleep和提交容量3是既有调度债务 | DEFERRED | S5 帧调度 |
| E04 | 借用字节与CodePin的公开接口容易被调用方误用 | PROTECTED | 两类 Encoding view 均明确借用，结果通过原 Request/Payload/Pin 保活 |
| F01 | Shader/Resource能力仍局限于当前演示子集 | DEFERRED | S3/S4 真实能力门禁 |
| F02 | 中立资源规划边界不够显式，但不能为名字再建一份IR | FIXED | NativeResourcePlan 明确为后端计划，Compiler 只消费现有 Graph/Analysis；扩展原语另行门禁 |
| D01 | README、示例、错误域与当前实现不一致 | FIXED | 本轮生产与对应行为/安装测试闭环 |
| D02 | Root TOOLCHAIN仍被旧消费者路径阻塞 | OPEN | 根 TOOLCHAIN 独立实际配置结果记录；既有产品 BLOCKED 不升级 |
| D03 | 平台依赖与未测平台资格边界 | FIXED | 冷态 OS 熵移入现有 platform_process；仅 Windows 有资格 |
| D04 | 原始证据可访问性、长路径与嵌套CTest日志 | FIXED | 完整 stdout、外部哈希归档与可下载便携证据子集 |
| D05 | P1/P2混用功能缺陷、结构债务和未来需求 | FIXED | 结构/性能/产品/未测平台资格分列 |

C01 FAILED/CANCELLED 原 Request 恢复、C02 Canonical SBO policy、C03 LastUse 为受保护回归，三组原测试通过。P/W/E/S 延后是合同范围，不以本批编译成功替代后续资格。

## 7. 保护与最终结论

Legacy 719 个冻结文件 Git blob 与 manifest 精确相同；原 `.vscode/settings.json`、用户 Graph.hpp SHA 与 L0 一致；其他既有 worktree 的 HEAD/status 未改变；依赖工作树干净且 SHA 不变。历史 I/V、旧检查器、IPC production include/src/pinclude/sinclude 均无修改；仅两处 IPC 测试迁移 SharedBytes/Encoding 消费路径。

Root TOOLCHAIN 在独立 clone 真实 configure，EXIT=1：缺 `render/client`、`render/features/cmake/feature_client.cmake`、`render/standard_content`，继而不存在 render_client/render_standard_content targets。未创建 Stub/空 Target，也未禁用测试；完整产品闭包 BLOCKED。

Linux/Android build、TSan 为 NOT_RUN。Windows 平台隔离通过不代表其他平台通过；Android 仅头同步。历史 IPC R2 和 S1/S2-R 性能 PARTIAL 原样保留。

L0–L3 的生产结构收敛和独立功能/安全/安装验证 **PASS**；L4 的性能保护仍 **PARTIAL**，所以本轮整体 **PARTIAL**。生产改动与真实证据可以独立审阅，不宣称已获得全性能资格。V 仅新增本报告及验证材料；推送前再次 fetch 核对、正常 fast-forward，不强推、不改变原脏工作树。交付后停止。
