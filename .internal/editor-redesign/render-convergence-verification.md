# Render V2 Architecture Convergence R0–R6 独立验收 V

本轮有界架构收敛 **PASS**。完整产品/root TOOLCHAIN **BLOCKED**，IPC 历史性能 **PARTIAL 保持不变**。
G10–G13 未开始。该结论只覆盖本文列明的真实实现、平台和测试，不是全部渲染业务资格。

## 提交、基线与独立性

- 最终 Implementation I：`056332d711824c07ea0a80807d039a3238493be8`。
- I Parent / 最后生产实现 R5：`ed975321ce348e4d30ad13296bf0a483ffc79bc2`。
- V 的 Parent 就是上述 I；V 仅新增本文，自身完整 SHA 在提交后的交付信息中提供。
- 施工前及最终推送前已核对的远端 `codex/render-v2-rebuild`：`b05904dc71f91be2c89964461430ad924ee97e2a`。
- 独立克隆：`E:/RenderV2Rebuild/ConvergenceR1/verification/056332d711824c07ea0a80807d039a3238493be8/source`，`git clone --no-hardlinks --no-checkout` 后 detached checkout 精确 I。
  无 alternates，不共享对象库；先通过 ValidateTrackedSnapshot，再配置。验证期间未修改源码，最终 status 干净。
- 原工作树 `E:/SyncForder/CodeRepos/lux-engine` 保持原分支、原 HEAD 和两处用户修改，不用于施工或验收。
- 架构文档 SHA256：`102721175cd18fde01757b420b03d31bf99c9207b70e2416d315be4e668fd703`。
- lux-cxx：`bf779515a120350c7c5412c366eea73afdf59ccf`；toolset：`c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`，两者干净。

内部顺序及每个提交 Parent（从前到后）：

```text
5c70db4b8ae834bbbd2eb7763954a60fdc01dcb9 b05904dc71f91be2c89964461430ad924ee97e2a render: enforce program authorship and atomic retirement admission (R0)
6becd3a8aca68d73395b8b9329638fd3fc9b25e2 5c70db4b8ae834bbbd2eb7763954a60fdc01dcb9 render: centralize native allocation records and persist business scopes (R1)
12f0221597f2ccca9344353cfc0b49bc1c27695e 6becd3a8aca68d73395b8b9329638fd3fc9b25e2 render: consume completions as values and compact IPC message storage
ebd8c855d94b00c495e6c1d31d5b19e54fa5a44f 12f0221597f2ccca9344353cfc0b49bc1c27695e render: bind typed operations through one open handler authority
5f29c622f3b02efbe790a7db3341cc1f1e53a9fe ebd8c855d94b00c495e6c1d31d5b19e54fa5a44f refactor(render): isolate bounded host compilation from server frame loop
ed975321ce348e4d30ad13296bf0a483ffc79bc2 5f29c622f3b02efbe790a7db3341cc1f1e53a9fe perf(render): compile shared pipelines and legal dynamic rendering scopes
056332d711824c07ea0a80807d039a3238493be8 ed975321ce348e4d30ad13296bf0a483ffc79bc2 test(render): freeze convergence R0-R5 independent verification inputs
```

R0–R5 各自的实施说明、检查器和原始证据均保留。本次 V 从最终 I 重新构建运行，未用实施树结果替代。

## 实际对象、职责和接口变化

| 对象/接口 | 最终职责与唯一 Owner |
| --- | --- |
| FeatureContribution | G5 返回的作者范围及输入 selection_index；只记录已证明的 Pass 归属，无另一张图 |
| DeviceSession | Device/VMA、BufferAllocation/ImageAllocation StableSlotMap、Context 与 scoped backing 的唯一物理资源权威 |
| BufferAllocation/ImageAllocation | 稳定轻量记录，无逐对象 Device/Allocator；原 Buffer/Image 独立拥有者被收敛 |
| Retirement entry | 真实 SubmissionTicket + 账本记录借用，或 Context pin；完成后回到 Session 销毁路径 |
| ScopedBuffer | Global/Scene/View 的业务存续与 Context lease 分离；Attachment-private 最后 lease 释放后回收 |
| IpcMessage | 128 B inline / 唯一 Owned union，144 B；Reply 296 B；SPSC/Channel/Link 算法保持不变 |
| RenderCompletion | move-only 直接拥有 IpcReply，消费完成流；删除 poll(id)、collectReplies、RenderReply 缓存体系 |
| OperationBinding / BoundOperation | 冷态完整合同验证结果；携带 Session + Operation SlotKey，不复制 generation |
| OperationEntry / RenderRequest | 唯一 Server Operation SlotMap；真实来源请求的一次完成资格；Builtin 与 Feature 使用同一路径 |
| RenderClient::Impl / RenderServer::Impl | 业务方法及状态收进唯一 Impl；公开对象转发；stop() 替代 requestStop() |
| CompileJob | 实际有界冷态编译任务，保活原请求；宿主借出的执行资源运行，Server 线程唯一发布 |
| vulkan::ShaderArtifact / PassProgram | 从 Feature 迁到 Native Program.hpp；完整 SPIR-V/ABI 身份，无旧命名空间 alias |
| CompiledGraph / CompiledPass | 唯一不可变 Native 程序；唯一 Pipeline 集合，Pass 借用句柄和静态 bind/scope 决策 |

生产 ERenderCommand、万能 RenderCommand 和固定业务 switch 已删除。两个独立测试 Feature Operation 无需修改
Server 核心即可注册、绑定、发送、处理和完成。RequestId 仍为每个 Client 的 SlotMap 身份，Server 能力表与资源域共享。
G2 的最小 BorrowedWireView/encodeWireInto 接缝复用原字段校验算法；不伪造 Owner，不复制大型 Payload。
所有普通控制请求仍校验真实外部数据；冷态绑定之后不再重复做完整 Schema 元数据匹配。

Effect 的执行组仍只有 SceneId/ViewId/TargetId/ExecutionContext*。Scene 拥有 View；无另一份关联表。
一次 GraphAnalysis、一个 DeviceSession、一个 Server 业务权威均保持。没有新 Manager/Registry/Host/Endpoint。

## 生命周期与错误提交点

- Program 必须属于实际贡献它的 Feature Pass 范围；foreign/duplicate/missing 拒绝，旧 Effect 不变。
- Detach/Resize/Remove 先做退休容量 admission，再取消 Readback/改变组；真实在途容量耗尽保留旧图、像素和 epoch。
- Global/Scene/View backing 不依赖 leases>0 存续；删除业务 Scope 后，仍被在途 Context 使用的 backing 等真实 Ticket。
- 资源创建保留 Vulkan/VMA 可恢复错误及回滚；alias 在原 image 业务引用释放后仍保活实际 allocation。
- 小消息不分配 Payload envelope；大消息使用唯一真实输入 Owner，预算计入完整 backing，abandon 不提前释放执行者输入。
- Compile executor 由宿主持有、有界且不 inline 执行；Runtime 不创建线程。拒绝 enqueue 恢复请求并明确失败。
  Worker 只读冻结 Feature/Device 能力，创建独立 Shader/Pipeline，不操作 VMA、Queue、Context 或 live Effect。
  当前无共享 VkPipelineCache。release/acquire done 是任务的最后访问；Shutdown 仍 join 所有接受的回调。
- Stop：停止派发 → join 编译借用 → 结算未发布编译/Readback/未交付请求 → Responder → Effect/Device/Instance。
  已发布 Reply/客户端完成值可晚于 Server 销毁。Requester 关闭后的未发布 Effect 回滚，未提交 Native 候选正常销毁。
- 热态保留容量、GPU 错误、真实 Ticket 完成和异步寿命判断；无 Feature 动态分派、名称解析、Graph Analysis 或编译。

## 独立命令与结果

完整可复现 runner/configure 参数和逐命令日志已归档。核心命令为：

```powershell
git clone --no-hardlinks --no-checkout <implementation-worktree> <V>/source
git -C <V>/source checkout --detach 056332d711824c07ea0a80807d039a3238493be8
cmake -DLUX_SOURCE_DIR=<V>/source -P <V>/source/cmake/ValidateTrackedSnapshot.cmake
cmake -S <V>/source/cmake/render-v2-bootstrap -B <V>/<variant> -G Ninja `
  -DCMAKE_BUILD_TYPE=<RelWithDebInfo-or-Debug> -DBUILD_TESTING=ON `
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset `
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx `
  -DCMAKE_CXX_FLAGS="/EHsc <ASan-flags>" -DCMAKE_CXX_SCAN_FOR_MODULES=OFF `
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
cmake --build <V>/<variant> --target all -j 4 -- -k 0
cmake --build <V>/<variant> --target all -j 4 -- -k 0
ctest --test-dir <V>/<variant> --output-on-failure -j 1 -R .
py -3 <V>/source/.internal/editor-redesign/check_render_convergence_r6.py `
  --repo <V>/source --build <V>/<variant> --clean --user-worktree E:/SyncForder/CodeRepos/lux-engine
```

环境：Windows 11 Pro 26200，i9-13900H，实际 GPU RTX 4070 Laptop，MSVC 19.44.35226.0，CMake 3.31.10，
Vulkan SDK 1.3.280.0。C++20、/GR-；ASan `/fsanitize=address /Zi`，`alloc_dealloc_mismatch=1`。
所有构建及实机验证串行；日志包含命令、返回码、完整失败/警告输出。

| 独立配置 | all 构建 | 第二轮 | CTest | 保护 |
| --- | --- | --- | --- | --- |
| RelWithDebInfo | PASS | no work | 61/61，52.73 s | PASS |
| Debug | PASS | no work | 61/61，53.57 s | PASS |
| ASan | PASS | no work | 61/61，65.61 s，无 sanitizer 错误 | PASS |

每套覆盖全部 Core/Data/Protocol/Graph/Feature/Native/Runtime/IPC 现有适用测试。公共头单独编译和全新安装、移动
前缀、外部消费者均执行；额外纯 Client 消费者禁用 Vulkan 查找，未产生 Vulkan/Native Target，构建两轮后运行 1/1。
安装导出、编译命令和链接闭包检查通过。没有借 Legacy 或旧 Render SDK 补齐模块依赖。

真实 GPU 验证包含两 Scene/四 View、共享 Native code、独立像素、Scope 共享、Resize、别名、部分提交、Ticket 等待、
退休容量回滚、编译失败和 Stop/关闭交错。暂停真实编译回调期间，四 View 仍持续提交并通过像素检查。
Release 稳定 1.2 s 窗口记录 497 ticks / 1969 submissions，engine new=0；没有重新调用 Feature/Analysis/编译。
Validation/SyncValidation 错误均为 0。存在系统 Epic EOS Overlay 重复 layer 警告，原始日志完整保留，没有宣称零警告。

## Native 编译与性能证据

八个真实 GPU 对照，实际调用计数：

| 场景 | 同/异 Shader | Pipeline 创建 / Bind | Begin/End | Barrier | Draw |
| --- | --- | --- | --- | --- | --- |
| 相邻同附件 LOAD | 同 | 1/1 | 1/1 | 3 | 2 |
| 相邻同附件 LOAD | 异 | 2/2 | 1/1 | 3 | 2 |
| 第二次 CLEAR | 同 / 异 | 1/1 或 2/2 | 2/2 | 4 | 2 |
| 不同附件 | 同 / 异 | 1/1 或 2/2 | 2/2 | 4 | 2 |
| 中间 Transfer Read | 同 / 异 | 1/1 或 2/2 | 2/2 | 5 | 2 |

异 Shader 真实产生蓝色而非红色；LOAD 保留背景，CLEAR 改变背景。条件执行明确拒绝，已创建 Pipeline 正确回滚。
只对合法相邻单 color attachment 合并，既不重排 Pass，也不跨越 Transfer/Hazard。Draw 数量没有减少。

独立 Release 1000 次完整绑定后小型 Operation（每次编码/执行/回复编码/消费/解码，预热 20 次）：

| 区段 | p50 | p95 | p99 | max |
| --- | ---: | ---: | ---: | ---: |
| Submit 含编码 | 800 ns | 1600 ns | 2400 ns | 47300 ns |
| 等待 + 消费 | 1993100 ns | 2422000 ns | 2523900 ns | 2556100 ns |
| 成功消费 + 解码 | 1500 ns | 3500 ns | 21500 ns | 114600 ns |
| Handler 解码 + Reply 发布 | 1200 ns | 3500 ns | 6100 ns | 9500 ns |

1000 次样本 engine new 均为 0；不等于 driver/OS 零分配。等待列保留当前 1 ms 宿主 sleep 和调度开销，整体往返约
2 ms，不能用队列微基准替代。旧 ControlFixture 的冷绑定计时另存，不作为该 API 测量。
Native CPU poll/record/submit：1000 次、预热20次，p50=154.6 us、p95=277.2 us、p99=330.8 us、max=1790.3 us；
显式 fence wait 和 readback 在计时外。GPU timestamp 未测，不把 CPU 时间当 GPU 时间。

Host 元数据对比（R1 原始源/命令保留）：Binding 160→32 B，Retirement entry 88→32 B；5000 Buffer +5000 Image 与
对应 binding/index 存储 1,600,024→1,302,680 B，约18.6%降低，当前实际61次批量账本分配。不是 GPU 总内存节省声明。
IPC Message 176→144 B、Reply 360→296 B。R2 三轮交替测量表明128 B部分路径改善，但32/64 B同线程存在小幅退化，
尾延迟不一致；历史 PARTIAL 不升级。大Owner 1 MiB/256 MiB保持地址、精确释放次数和消费线程释放证据均保留。

## 保护、边界与尚未完成项

- 每套检查：719 Legacy Git blobs、3041 个非白名单保护文件、全部原历史报告/checker 不变。
- 主工作树 settings SHA256：`f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c`；
  用户 Graph.hpp：`00c28abc7bd0a786666f101a5212400a5b467af6bdbbc5049c69d59d548c726a`，均匹配。
- 三个公共头前缀25头×3=75份与精确 I blob逐字节一致；旧 Feature Program.hpp 已备份移除，无转发头。Android只是头同步。
- ALLOWED：R0–R5 检查器列出的精确74项文件变化与新增验收材料；READ-ONLY：Legacy、Graph作者算法、Protocol、历史材料、
  IPC SPSC/Channel/Link/发布/额度/退休算法；FORBIDDEN：用户工作树覆盖、main合并、占位Target、删/禁测试、伪造历史资格。
- 根 TOOLCHAIN configure 真实退出1：modules/function/CMakeLists.txt 仍引用缺失的 render/client、
  render/features/cmake/feature_client.cmake、render/standard_content，并引出对应安装Target不存在。
  新 Runtime 的 render_client 不自动修复旧产品装配路径；本轮不加假目标、不隐藏依赖。完整产品闭包 BLOCKED。
- Descriptor Binding、Compute/Indirect/RT、专业Mesh/Material/Texture Storage、动态History/WSI、ECS/stdexec产品接入
  NOT_IMPLEMENTED。Linux/Android构建、TSan NOT_RUN；GPU timestamp NOT_MEASURED。
- G10 下一入口应是已开放Operation表上的真实GPU完成合同与受控异步请求；G11 应消费现有Native账本、Scope和编译计划。
  Descriptor/Indirect优化必须随真实消费者加入。宿主产品还需提供生产有界编译执行资源及stop/join装配。
  这些是后续建议，本轮未开始。

## 证据索引与完整文件变化

不可变证据归档：`E:/RenderV2Rebuild/ConvergenceR1/archive/056332d711824c07ea0a80807d039a3238493be8`。
`INDEX.json` 覆盖732个日志/样本/脚本/安装清单/备份文件；SHA256：`5984fa8996f86d153f3390f8e5a753f0ef6afdc6601d0e21d98f944af3ba3fac`。
包括三套完整 LastTest.log、CMakeCache/compile_commands/build.ninja、SDK manifest、原始CSV、环境和输入提交清单。
实施期中间失败（旧IPC ABI安装断言、测试夹具编译修正等）也保留，未删除失败日志或改变历史结论。
本次最终独立矩阵没有生产修复；根配置失败作为独立 BLOCKED 项保留。

```text
A	.internal/editor-redesign/check_render_convergence_r0.py
A	.internal/editor-redesign/check_render_convergence_r1.py
A	.internal/editor-redesign/check_render_convergence_r2.py
A	.internal/editor-redesign/check_render_convergence_r3.py
A	.internal/editor-redesign/check_render_convergence_r4.py
A	.internal/editor-redesign/check_render_convergence_r5.py
A	.internal/editor-redesign/check_render_convergence_r6.py
A	.internal/editor-redesign/render-convergence-r0.md
A	.internal/editor-redesign/render-convergence-r1.md
A	.internal/editor-redesign/render-convergence-r2.md
A	.internal/editor-redesign/render-convergence-r3.md
A	.internal/editor-redesign/render-convergence-r4.md
A	.internal/editor-redesign/render-convergence-r5.md
A	.internal/editor-redesign/render-convergence-r6.md
M	modules/function/render/data/include/lux/engine/render/data/Wire.hpp
M	modules/function/render/data/src/Wire.cpp
A	modules/function/render/data/test/Borrowed.cpp
M	modules/function/render/data/test/CMakeLists.txt
M	modules/function/render/features/include/lux/engine/render/features/Feature.hpp
M	modules/function/render/features/src/Assemble.cpp
M	modules/function/render/features/test/CMakeLists.txt
M	modules/function/render/native/vulkan/README.md
M	modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/CompiledGraph.hpp
M	modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/DeviceSession.hpp
M	modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/ExecutionContext.hpp
R097	modules/function/render/features/include/lux/engine/render/features/Program.hpp	modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/Program.hpp
M	modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/memory/Memory.hpp
M	modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/retirement/Retirement.hpp
M	modules/function/render/native/vulkan/pinclude/ExecutionState.hpp
M	modules/function/render/native/vulkan/pinclude/Native.hpp
M	modules/function/render/native/vulkan/pinclude/Shader.hpp
M	modules/function/render/native/vulkan/src/Compile.cpp
M	modules/function/render/native/vulkan/src/DeviceSession.cpp
M	modules/function/render/native/vulkan/src/Execution.cpp
M	modules/function/render/native/vulkan/src/memory/Memory.cpp
M	modules/function/render/native/vulkan/src/retirement/Retirement.cpp
A	modules/function/render/native/vulkan/test/BindingPlan.cpp
M	modules/function/render/native/vulkan/test/CMakeLists.txt
M	modules/function/render/native/vulkan/test/Execution.cpp
M	modules/function/render/native/vulkan/test/Foundation.cpp
A	modules/function/render/native/vulkan/test/MemoryCost.cpp
M	modules/function/render/native/vulkan/test/SceneFixture.hpp
A	modules/function/render/native/vulkan/test/shaders/Swizzle.frag
A	modules/function/render/native/vulkan/test/shaders/Swizzle.hpp
M	modules/function/render/native/vulkan/test/shaders/generate.py
M	modules/function/render/runtime/CMakeLists.txt
M	modules/function/render/runtime/README.md
A	modules/function/render/runtime/include/lux/engine/render/runtime/Operations.hpp
M	modules/function/render/runtime/include/lux/engine/render/runtime/RenderClient.hpp
M	modules/function/render/runtime/include/lux/engine/render/runtime/RenderServer.hpp
M	modules/function/render/runtime/pinclude/Wire.hpp
A	modules/function/render/runtime/src/Operations.cpp
M	modules/function/render/runtime/src/RenderClient.cpp
M	modules/function/render/runtime/src/RenderServer.cpp
A	modules/function/render/runtime/src/Request.cpp
M	modules/function/render/runtime/src/Wire.cpp
M	modules/function/render/runtime/test/CMakeLists.txt
A	modules/function/render/runtime/test/CompileWorker.hpp
A	modules/function/render/runtime/test/CompletionTest.hpp
A	modules/function/render/runtime/test/ControlFixture.hpp
A	modules/function/render/runtime/test/FeatureOperations.cpp
M	modules/function/render/runtime/test/Lifetimes.cpp
M	modules/function/render/runtime/test/Offscreen.cpp
A	modules/function/render/runtime/test/Programs.cpp
M	modules/function/render/runtime/test/Runtime.cpp
M	modules/function/render/runtime/test/Shutdown.cpp
M	modules/function/render/runtime/test/installed/Client.cpp
M	modules/function/render/runtime/test/installed/Main.cpp
M	modules/platform/ipc/include/lux/engine/platform/ipc/IpcMessage.hpp
M	modules/platform/ipc/src/Ipc.cpp
M	modules/platform/ipc/test/Attachments.cpp
M	modules/platform/ipc/test/Ipc.cpp
M	modules/platform/ipc/test/PluginA.cpp
M	modules/platform/ipc/test/installed/Main.cpp
```
