# R4 — Vulkan Foundation 独立资格验收

结论：**PASS**，限本报告绑定的 Windows / MSVC / 单 GPU native Foundation 范围。
本地独立进程从 clean clone 运行，不表示 GitHub CI 或另一平台复现。
验收提交 V 只新增本文件；确切 V SHA 由 Git 历史及最终交接提供。
本轮完成后 **STOP**，R5 未获实施授权。

## 1. 身份与证据

- 批准的 R3 验收基线：`f9b943c9e768ef2c9d408962f0d5a32d2cf3de74`。
- R3 implementation：`ed4b78c1a4606f917eec1b30c654567935e6de48`。
- Frozen V1：`a669409a289a6fa4092f21176397795b1cdb7f3e`。
- R4 完整 implementation I：`83ffbb6d9859d87ca62a8a2acbb0083c2a8de420`。
- clean source：`D:/LuxQualification/render-v2-r4-83ffbb6d9859/source`。
- normal / ASan build：该目录同级的 `build` / `asan`，均在源码树外。
- 证据：`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/R4/83ffbb6d9859d87ca62a8a2acbb0083c2a8de420/`。
- `EVIDENCE_MANIFEST.json`：418 个文件，SHA-256 `ccca3c87df0900da222bb69038528d4cb33b9f0e14170147a7820f401745b443`。
- `qualification-runner.py`、`qualification-runner-resume.py` 保存独立执行过程；
  每项命令、退出码、原始日志、CTest XML、生成 shader/traits、依赖及哈希均在归档中。

按 native owner/依赖组提交，最后两个提交只增强测试/验收脚本：

```text
4e9e13d32b9338607aa0d300d58d58b0efe8bbcb feat(render): establish Vulkan instance device and allocator owners
af1f9589289572db9e71966c3f494a3e8dc2f50f feat(render): add native memory descriptor and compute pipeline owners
be6937de76a68797ba55b1acb37fd5e4214b21be feat(render): add bounded native transfers and fence-proven retirement
a4b62e63339385fc4800927f5e296af44a89f445 test(render): verify candidate replacement with in-flight native backing
83ffbb6d9859d87ca62a8a2acbb0083c2a8de420 test(render): recognize existing transport negative-probe codegen targets
```

早期 `be6937de76a6` clean clone 已有双构建与59/59测试；随后补充在途 backing
候选替换测试，并修正闭包脚本对**既有 R2 负例生成 target**的误拒绝。
这两个修订均先进入新的 implementation commit；旧 clean clone 没有补丁。
旧归档保留 PARTIAL / superseded 事实。所有最终检查重新绑定上述 I。
最终 audit 的裸 `dumpbin` 在 Windows 父进程 PATH 中未找到；改用已安装 MSVC 的
绝对路径恢复工具调用，未更改源码或重写已通过日志，见 `audit-runner-note.txt`。

## 2. 实际 owner 与生命周期

| 机制 | 实际所有权与资格结论 |
| --- | --- |
| Instance | `VulkanInstance` 独占 instance/debug messenger；明确借用 diagnostic user data；创建与 messenger 失败回滚 PASS |
| Device / queues | `VulkanDevice` 独占 device，借用 instance/physical device，持有从 device 获得的 queue；graphics+compute family 约束与显式扩展检查 PASS |
| VMA | 独立 `VulkanAllocator` owner 借用 device/instance；allocation 先释放、allocator 后释放、device 最后释放 PASS |
| Buffer / Image | 独占 VMA backing；host-visible/device-local、持久映射、flush/invalidate、move/assignment/rollback PASS |
| Descriptor | Layout、Pool 分别拥有 Vk 对象；pool 拥有全部 set；set 是受 pool/layout 约束的原生 borrow；真实 storage descriptor 使用 PASS |
| Pipeline | ShaderModule、PipelineLayout、ComputePipeline 各自 move-only；冷态创建；部分 pipeline 输出失败清理与 active candidate 保留 PASS |
| Submission / staging | stable-address queue owner，预留 command buffers/fences，固定 staging 分区；所有 native queue access 由一个外部串行 owner 域负责；三在途槽及背压 PASS |
| Retirement | 固定 optional native-owner 槽；publisher 停止新引用后移交 backing；仅 fence-proven serial 可 collect；乱序 admission、批量回收、容量失败保留 owner PASS |

普通 backing 析构不等待 device。Native parents 必须比其所有 CPU/GPU borrows 活得更久；
接口不承诺识别任意 raw Vulkan handle 的误用，也没有隐式全局 registry。
`SubmissionTicket` 为 queue-local admission evidence，不能跨 owner 或超出 queue 生命周期。

`CommandBatch` 取消不提交；submit 失败不前进 serial；只有已完成 fence 的槽允许复用。
StagingSlice 绑定 recording epoch；取消候选后下一 epoch 可复用，容量不足没有 overflow heap。
分区按 non-coherent atom 对齐，flush 不触及其他在途分区。

资源替换测试先证明失败候选保留旧 Buffer，再成功创建并换入新 Buffer；旧 descriptor 和
已提交 command 仍引用旧 backing，旧 backing 移交 retirement，完成 fence 后才销毁。
12 次真实 compute/buffer/image roundtrip 的结果逐值核对。

最终 Queue/Staging/Retirement owner 析构通过 fence join 收束在途 GPU 工作，无 public shutdown。
仍存活的 CommandBatch CPU borrow 触发 release-build fatal contract（独立进程 exit=86）。
没有 `vkDeviceWaitIdle()`、逐资源阻塞销毁、后台线程、FrameLoop 或 semantic zombie。
注入 Device Lost 保留精确错误并锁定 queue 终态，不尝试恢复，也不伪造完成 serial；
实际 GPU Device Lost 未发生，完整 Runtime 终态合同留给 R5。

公开 API、borrow 前提和安装限制见
[Foundation README](../../modules/function/render/vulkan/README.md)。
逐项 V1 来源、保留/差异及证明见 [R4_PROVENANCE.md](R4_PROVENANCE.md)。
固定分区/对齐、copy 前后同步、serial 完成谓词保留；去除 context/contributor hierarchy，
固定 retirement 扫描替代依赖 frame-monotone admission 的动态 FIFO，不改变安全销毁谓词。
不宣称迁完 V1 transfer scheduler、graphics catalog 或其历史性能/故障案例。

## 3. 原始运行结果

| 检查 | 结果 / 原始证据 |
| --- | --- |
| clean tracked snapshot、独立 clone | PASS；`01-snapshot`、`02-clone`、`03-checkout`、`04-clean-clone` |
| bootstrap configure / all | PASS；`05-configure`、`06-build-1`；统一 `-j 4 -- -k 0` |
| 第二轮 all | PASS；`06-build-2.log` 为 `ninja: no work to do` |
| 全部 CTest | **59/59 PASS**；`07-ctest.log`、`ctest.xml`；既有45项名称集合包含关系检查通过 |
| ASan 配置双构建 | PASS；`08-asan-*`；所有 native implementation TU 实际使用 `/fsanitize=address` |
| ASan 配置测试 | **13/13 PASS**；含4项 owner/native执行测试、borrow fatal、control 和7项编译负例；性能测试不在 ASan 配置运行 |
| 故障路径 | PASS；28次失败检查，另有 Device Lost 注入；13类 native create/destroy 计数平衡，完整关键父子释放次序检查 |
| real GPU / synchronization validation | PASS；normal 与 ASan 的 Device/Objects/Transfer/Faults 日志均为 `validation_errors=0` |
| warmed bounded first-party C++ allocations | PASS，0 次 / 0 字节；normal benchmark 不含 test seam |
| source / include / link / codegen | PASS；`closure.json`、File API、compile commands、Ninja deps、targets.dot |
| production test-seam 符号隔离 | PASS；`15-native-symbols.log` 与实际编译宏检查 |
| 公共头独立 TU | PASS；Vulkan 8个，加既有14个，共22个 |
| 三安装 include 前缀同步 | PASS，24/24；`install-header-sync.json`，旧字节备份在归档；不等于 installed SDK 资格 |
| Legacy / Core / Transport / Graph / 历史规范报告 | PASS；719个 blob/mode/size 与固定manifest一致，legacy tree720含保护CMake；其余 Git object 与 R3基线相同 |
| 原工作区 | PASS；`user-before.json == user-after.json`，HEAD、六个文件哈希、staged/unstaged diff、status 均未改变 |

新增 CTest（全部真实运行，无删除旧失败项或降级）：

- `render.vulkan.bounded_allocation` — PASS
- `render.vulkan.device` — PASS
- `render.vulkan.faults` — PASS
- `render.vulkan.lifetime_contract` — PASS
- `render.vulkan.objects` — PASS
- `render.vulkan.probe_control` — PASS
- `render.vulkan.reject_EDITOR` — PASS
- `render.vulkan.reject_FEATURE` — PASS
- `render.vulkan.reject_GRAPH` — PASS
- `render.vulkan.reject_LEGACY` — PASS
- `render.vulkan.reject_RUNTIME` — PASS
- `render.vulkan.reject_SCENE` — PASS
- `render.vulkan.reject_TRANSPORT` — PASS
- `render.vulkan.transfer` — PASS

ASan 检查覆盖第一方 owner 代码及被编入该 TU 的实现；不把外部 loader/driver/validation DLL
称为已插桩，也不声称检测了数据竞争。Foundation 的约定是外部串行使用。

## 4. Native 设备与配置

- NVIDIA GeForce RTX 4070 Ti；vendor `0x10de`，device `0x2782`；queue family 0。
- NVIDIA driver `591.86.0.0`（native raw `2480242688`），device API `1.4.325`。
- 系统 loader API `1.4.321`；创建 instance/device 的 API 合同为 `1.3`。
- Vulkan SDK `1.4.304.0`；VMA `3.3.0`；glslc 真实编译 `Smoke.comp` 为 Vulkan1.3 SPIR-V。
- device extensions：空；启用 feature：`synchronization2`，其余未无条件启用。
- instance extensions：`VK_EXT_debug_utils`、`VK_EXT_validation_features`。
- layer：`VK_LAYER_KHRONOS_validation` `1.4.304`；启用 synchronization validation。
- normal/ASan GPU correctness 运行均为 **0 error**；Galaxy overlay 的三条 layer 命名
  policy warning 原样保留，重复创建 instance 时重复出现。没有过滤后宣称零 warning。
- 详细枚举、已启用配置、SDK/VMA/loader hashes 见 `14-device-info.log`、device test 输出及
  `dependency-package-hashes.json`。系统 vulkaninfo 的第三方 loader warning 亦保留。

## 5. 性能基线

MSVC19.44 x64 RelWithDebInfo、i7-13700KF / RTX4070Ti，同一个独立 I。
validation 关闭的正常生产库，20轮 transfer 预热后100个样本；原始 `09-benchmark.log`。
不是 V1/V2 等价比较，因此百分比回退门禁没有可用的等价旧 baseline；不编造提升。

| case | p50 ns | p95 ns | max ns | C++ allocation 次数 / 字节 |
| --- | ---: | ---: | ---: | ---: |
| `transfer_1MiB_roundtrip` | 201200 | 297000 | 409400 | 0 / 0 |
| `native_end_reset_submit` | 8700 | 18400 | 26100 | 0 / 0 |
| `staging_1MiB` | 26800 | 30500 | 33800 | 0 / 0 |
| `buffer_1MiB_create_destroy_cold` | 300 | 400 | 15100 | 0 / 0 |
| `retire_collect_native_buffer` | 200 | 500 | 2100 | 0 / 0 |

1 MiB roundtrip 含 staging、GPU upload/copy、fence wait、invalidate 和 CPU read；
p50约0.2012ms，对应按单份 payload 计的有效吞吐约4.85GiB/s，不等于纯GPU带宽。
Native submit 项含 end-command/reset-fence/submit。Create/destroy 是允许分配的冷配置操作，
此处使用已建立的 VMA allocator；retire/collect backing 为4KiB。零分配计数覆盖普通/数组/
aligned C++ new，不计 VMA malloc、native allocations 或外部DLL堆，不声称整个驱动零分配。
三个 transfer 子项共享整体测量窗口的零分配证明；原始 timing samples 的聚合口径写在源码中。

## 6. 闭包与保留项

生产 target `render_vulkan`：10个真实cpp，PUBLIC Core + Vulkan loader/headers，PRIVATE VMA。
File API、97条 compile-command entries、231个实际 compiler headers、Ninja deps 和 linked
libraries 验证：Core/Transport/Graph 均未接入 Vulkan；Foundation 没有 Scene/ECS、Editor、
Feature、Runtime、Transport、Graph 或 Legacy 编译/链接输入。Transport codegen 的源码、
模板、6个 operation traits / 1个 reply traits 与 R3 的规范化生成结果一致。

仍为 **NOT_RUN**：Linux、Android构建、完整 installed SDK、Surface/Swapchain/Present、
跨queue/family同步、R5 Runtime/FrameLoop、V1全矩阵、等价V1/V2渲染性能比较。
旧 `render.transfer_idle`、cross-frame skinning WAR 和其他 V1 已知问题保持历史判定；
本次小型compute成功不覆盖或关闭它们。泛用 Error headers 的完整安装依赖仍按既定阶段处理。
本机 native memory 实测不能外推为所有厂商/所有内存类型均已覆盖。

R5 准入：用户审查本轮 I/V 后明确放行。待办为真正 Backend execution domain 与 target、
independent render progress/pacing、Runtime终态错误和异步诊断、reply consume/abandon责任、
完整 native graph cache key，以及 R5/R6 的一 Scene 多 View /不同graph-pass/共享持久状态/
各自target-history生命周期。R3 `matches()` 不能充当完整 native cache key。

```text
R4 = PASS (qualified scope above)
V2_PRODUCT = EXPECTED_UNAVAILABLE
NEXT_STAGE = R5 (requires explicit review authorization)
STOP
```

## 7. 完整 implementation 修改清单

I 相对批准 R3 的37个文件，均在授权范围；V另加本报告1个文件。

```text
cmake/render-v2-bootstrap/CMakeLists.txt
docs/render-v2/R4_PROVENANCE.md
docs/render-v2/R4_WORK_ORDER.md
modules/function/render/vulkan/CMakeLists.txt
modules/function/render/vulkan/README.md
modules/function/render/vulkan/include/lux/engine/render/vulkan/Error.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/descriptor/Descriptors.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/device/Device.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/memory/Memory.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/pipeline/Pipeline.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/retirement/Retirement.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/transfer/Submission.hpp
modules/function/render/vulkan/include/lux/engine/render/vulkan/transfer/Transfer.hpp
modules/function/render/vulkan/pinclude/Native.hpp
modules/function/render/vulkan/src/Error.cpp
modules/function/render/vulkan/src/descriptor/Descriptors.cpp
modules/function/render/vulkan/src/device/Device.cpp
modules/function/render/vulkan/src/device/Vma.cpp
modules/function/render/vulkan/src/memory/Memory.cpp
modules/function/render/vulkan/src/pipeline/Objects.cpp
modules/function/render/vulkan/src/pipeline/Pipeline.cpp
modules/function/render/vulkan/src/retirement/Retirement.cpp
modules/function/render/vulkan/src/transfer/Submission.cpp
modules/function/render/vulkan/src/transfer/Transfer.cpp
modules/function/render/vulkan/test/Allocation.hpp
modules/function/render/vulkan/test/Benchmark.cpp
modules/function/render/vulkan/test/CMakeLists.txt
modules/function/render/vulkan/test/Device.cpp
modules/function/render/vulkan/test/Faults.cpp
modules/function/render/vulkan/test/Lifetime.cmake
modules/function/render/vulkan/test/Objects.cpp
modules/function/render/vulkan/test/Probe.cpp
modules/function/render/vulkan/test/RejectCompile.cmake
modules/function/render/vulkan/test/Smoke.comp
modules/function/render/vulkan/test/Support.hpp
modules/function/render/vulkan/test/Transfer.cpp
modules/function/render/vulkan/test/verify_r4.py
```
