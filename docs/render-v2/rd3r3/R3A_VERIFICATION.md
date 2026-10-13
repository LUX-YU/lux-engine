# RD3-R3 R3-A 独立验收 — PASS

日期：2026-10-13。结论仅覆盖本轮授权的 Attachment/Scope/Context 生命周期与现有 Offscreen Graph 切片。
没有进入 R3-B–E、Legacy 产品迁移、H1–H6，没有合并 main；全部历史 PASS/PARTIAL 保持原样。

## Git 与独立性

- Implementation I：`f98878f7e9551ea9da034a01c618420398ad7f58`。
- I Parent：`960e755395be2c9f78a86339a062c507826c4eb4`，也是施工前、I 后及 V 提交前实际查询的远端 HEAD。
- 本报告所在提交为 Verification V，Parent 为上述 I；V 仅新增本报告。最终 V SHA 与推送后的远端 HEAD 记录在交付消息及源码树外 `E:/RD3R3A/delivery.json`，避免提交内容自引用 SHA。
- 施工目录：`C:/Users/chenh/.codex/worktrees/render-rd3-d0/lux-engine`，分支 `codex/rd3-r3-a`。
- 独立源：`E:/RD3R3A/verify/source`；正常构建：`verify/build`；ASan 构建：`verify/asan`。

I clean 后执行 ValidateTrackedSnapshot，通过。随后使用 `git clone --no-local --no-checkout --single-branch
--branch codex/rd3-r3-a --depth 2` 创建独立仓库，设置 autocrlf=false、longpaths=true，detached checkout 精确 I。
额外只读获取 Legacy 冻结提交。Git/common dir 位于 clone 内，无 alternates，对象文件无硬链接。
前后源码均 clean，逐文件字节匹配 I 的 Git Blob；验证环境未修复生产内容，未用忽略文件补齐源码。
独立性指仓库、对象库、构建与进程隔离，由同一代理验收，不声称独立人工审查。

## 实测结果

| 门禁 | 精确 I 的独立结果 |
|---|---|
| 前置/后置完整性 | 各 **6131/6131 PASS**，两份 result.json SHA 相同 |
| Legacy | 冻结 **719/719** mode/blob/size 保持原样，基线 `a669409a289a6fa4092f21176397795b1cdb7f3e` |
| 设计来源 | ZIP SHA/CRC、65 个原始文件逐字节、Manifest、33 章、9 工作单、14 表及合订版一致性通过 |
| 追踪表 | F36/I44/H6=86、W25、REQ51 主键全集及 REQ→W 外键通过；阶段、Owner、Oracle/证据要求保留 |
| C++20 正常构建 | MSVC RelWithDebInfo/Ninja `all -j 4 -- -k 0`，**503 步成功**；第二轮 no work |
| 公共头 | Objects/Types/Runtime/Backend 独立编译，禁止传递包含 Vulkan；NativeAssembly/DeviceSession/ResourceDomain 按明确 Native 头独立编译 |
| 相关回归 | **43/43 PASS，31.54 秒**；与构建串行 |
| ASan 构建 | 精确 I 的另一个完整 all 构建，503 步成功；第二轮 no work |
| ASan 执行 | RD3 runtime/faults **2/2 PASS，14.30 秒**，无 ASan 错误；alloc_dealloc_mismatch=1 |
| 插桩阳性对照 | 外部小程序故意释放后读取，ASan 按预期报告 heap-use-after-free，exit=1；非产品回归 |
| Native 验证 | RD3 用实际 Vulkan Validation + SyncValidation，errors=0；保留系统 EOS overlay 重复 manifest 警告 |
| 类型/资源 | int/double/真实 Buffer 三类 factory、同类型不同 Key、完整配置冲突、租约 move/release、SlotMap 代际及稳定地址通过 |
| 在途生命周期 | GPU Event 阻塞真实提交；两个 Graph slot 继续读取共享 Buffer；Attachment 析构后 backing 保留，真实 Ticket 完成后才回收 |
| P2-A 保持 | 两 Scene、四 View、同 Graph、FIF=2/3 的原 RGBA Pixel Oracle 通过；同 View 第二 Effect/Target 保持独立输出 |
| 新作用域 GPU 结果 | Global/Scene/View 共享、Attachment-private 隔离、同类型第二 Key、detach 后剩余消费者、Scene 删除/重建均通过 |
| 错误与通信 | 非 OOM Native 候选回滚、Event/Thread 失败重试、部分提交后 Drain、跨 Runtime/Scene 旧身份拒绝、Client/Reply 晚析构通过 |
| OOM 新合同 | 子进程注入 Device OOM，必须调用 fatal handler 并 exit=77；不能返回旧的 retryable error |
| 热态 | 原 FIF=2/3 和新增 scoped 稳定帧的 C++ allocation count/bytes 均为 0；同结构 Effect 不再创建 Pipeline |
| Native Owner | 正常关闭与非 OOM 失败/部分提交后，18 类 Native owner 计数全部归零 |
| 算法保护 | graph/vulkan 全部既有生产文件、transport 实现与历史报告均未修改 |

ASan 使用 `/fsanitize=address /Zi /D_DISABLE_VECTOR_ANNOTATION /D_DISABLE_STRING_ANNOTATION`。
预装 serialization_json 和 spirv-cross 静态库没有 MSVC STL 容器注解，直接混合会产生 LNK2038。
兼容选项只关闭 vector/string 的逻辑容量注解；本轮第一方代码的地址/释放后访问插桩保持启用，
导入表确认 `clang_rt.asan_dynamic-x86_64.dll`，阳性对照确认实际检测。外部预编译库和 GPU driver
不因此获得 ASan 资格；Windows leak sanitizer、STL capacity-overflow 注解不在本报告覆盖范围。
Native GPU 生命周期另由真实 Ticket、Validation/SyncValidation 与 owner counters 验证。

43 项过滤式与 P2-A 相同，保留本轮相关旧测试：

```text
^render\.(rd3\.|transport\.|vulkan\.(native_graph|multi_owner|device|objects|transfer|faults|lifetime_contract|bounded_allocation|graph_candidate|shader_candidate|shader_layout|shared_owners|compute_bindings|closeout_C1(-multi|-partial)?)$|graph\.(bindings|reuse|binding_allocation|scope_sharing|scope_hazards|logical_oracle)$)
```

## 实际 GPU Oracle

原像素公式 `x * exposure / (1 + x * exposure)`、RGB 输入 (1,2,3)、Alpha=1 和 1e-6 误差门槛未修改。
原四 View exposure=1/2/3/4，另一 Effect 为 7；共享 Native Graph/Shader/PSO，同时执行资源独立。

新增用同一构图函数加入已有 F5CReadBuffer 读回 Pass，继续执行原 F4Fill/F3Tonemap/最终 Target 路径。
故障测试只对现有 BufferFactory 的初始化 fill 调用记录计数并写入不同测试 word，没有替换 Graph、
Shader、Compiler 或 Executor。GPU 返回的五个资源 word 分别是：

| Attachment | Global / Scene / View / Private / 第二 Global Key |
|---|---|
| E1 / Scene A / View A / T1 | 1 / 2 / 3 / 4 / 5 |
| E2 / Scene A / View A / T2 | 1 / 2 / 3 / 6 / 5 |
| E1 / Scene B / View B / T3 | 1 / 7 / 8 / 9 / 5 |
| E1 / Scene A / View A2 / T4 | 1 / 2 / 10 / 11 / 5 |
| Scene A 删除并重建后的新 View / T1 | 1 / 12 / 13 / 14 / 5 |

逐资源读取四个 uint32 并断言，配合初始化创建计数 5→6→9→11→14，证明真实共享和 private backing 分离。
重复 attach 不增加计数；删除 A 之前按既有 Busy 合同先 detach/remove Views，B 的 1/7/8/9/5 和最终像素不变。
旧 Scene/View 身份不能用于重新 attach。默认生产初始化为零；不同 word 是测试输入，不是产品上传 API。

## Owner、租约与执行关系

Native bootstrap 在 Backend 实现中先持有稳定 `unique_ptr<InstanceContext>`，再持有唯一 Session。
Session 仅借用 InstanceContext；它拥有 DeviceContext 和 ResourceContext。DeviceContext 复用原
VulkanDevice/VulkanAllocator/SubmissionQueue；同 VkQueue 的角色只借用同一个队列 Owner。

ResourceDomain 已从中立父子 Domain 树收敛为 Session 内部唯一 Native 作用域存储，没有并存的
ScopedResources Owner。ScopeKey 来自 Backend 已校验的完整生成式身份，Global 的外层身份是 Session。
Scope/完整类型名和 hash/完整资源键共同匹配；配置不相容明确返回错误，不覆盖现有资源。

| ID/索引 | 分配或拥有者 |
|---|---|
| SceneId | Backend `SlotMap<Scene>` 自动分配 |
| ViewId | Scene `SlotMap<View>` 自动分配；跨 Scene 必须使用 ViewRef |
| EffectId / TargetId | Backend 各自 SlotMap 自动分配 |
| GraphId | Session Graph SlotMap 自动分配；完整结构比较决定复用 |
| ScopedResourceId | ResourceDomain `SlotMap<unique_ptr<Entry>>` 自动分配；不是 hash |
| Effect→Attachments | SlotKeySparseSet 辅助索引；不再分配 Effect/Attachment 身份 |
| Graph→ExecutionContext | SlotKeySparseSet 辅助索引；GraphId 来自 Graph Owner |
| 有序执行计划 | PreparedExecution 借用 Attachment 和 View，不拥有对象，不重新分配 ID |

EffectAttachment 是唯一可编辑关联记录，持完整 Effect/Scene/View/Target 关系、轻量 immutable Native
code pin、typed ResourceLease、准备好的 imports 与 Native pool/slot 引用。Graph pin 指向同一份 Native
GraphCode，不复制 IR/Shader/PSO。未创建 AttachmentId、Manager、第二 Registry 或新 GPU timeline。

ExecutionJob 类型删除。Native BindingExecution 也移除了重复 ViewRef，仅保留 slot occupancy/值/票据。
有序计划只保留必要的执行顺序和直接借用；SlotMap 拓扑变更后冷态刷新 View 地址。稳定 execute 中没有
Scene/Effect/ResourceDomain 查找。没有新增 unordered_map/unordered_set 对象索引。

Detach 顺序为移除计划借用 → Attachment 析构/解除 slot 占用 → 释放逻辑租约 → 冷态尝试收集。
GPU 物理 Owner 始终为 ResourceDomain。它保留已有 SubmissionQueue 的实际 accepted tickets，包含
部分提交的各 Queue evidence；只通过原 Queue wait/poll 判断安全，不建立另一套完成权威或提交线程。
共享 import 的 ready 采用最近实际 Graphics join，避免仅用初始化 ticket 掩盖尚在执行的读者。

关闭顺序：既有 R2 Stop/Drain → 计划/Attachments → Native GraphExecutionContexts 的真实票据 →
Session 其余已接受队列工作 → scoped backing/Graph → Queue/VMA/Device → 外部 Instance。
Client 和 Reply arena 的晚生命周期不依赖上述 Native Owner。

## 修改边界与文件

I 共 **81 文件，5598 行增加、451 行删除**，没有删除文件。7 个生产文件发生实际修改/新增：

- `runtime/include/lux/engine/render/runtime/DeviceSession.hpp`
- `runtime/include/lux/engine/render/runtime/NativeAssembly.hpp`
- `runtime/include/lux/engine/render/runtime/Objects.hpp`
- `runtime/include/lux/engine/render/runtime/ResourceDomain.hpp`
- `runtime/src/DeviceSession.cpp`
- `runtime/src/Backend.cpp`
- `runtime/pinclude/EffectAttachment.hpp`（新增）

上述路径中的 `runtime/` 对应 `modules/function/render/runtime/`。另外修改测试 RD3Runtime.cpp、RD3Faults.cpp、
ResourceDomains.hpp；新增 archive/P2A_RD3Faults.cpp、archive/P2A_ResourceDomains.hpp，逐字节保存
失效的旧 Domain-tree/OOM-retry 合同。改动 bootstrap 的 RD3Runtime.cmake 以接入已有 schema 和 Native 头检查。
新增 docs/render-v2/rd3r3 下 65 文件原始设计包、Implementation 说明、只读验收检查器和局部 .gitattributes。
完整 81 路径在外部 `verify/postflight/result.json`。

移除的类型/字段：ExecutionJob、ResourceDomainId/Tag、Scene.domain、View.domain、BindingExecution.view。
保留并改造 ResourceDomain、ResourceFactoryFor、BufferFactory、DeviceSession 和既有 Native RAII。
新增 EffectAttachment、PreparedExecution、EResourceScope、ScopedBufferRequest、ResourceScopeKey、
ResourceLease<T> 以及稳定 typed entry/SlotKey，分别承担关联使用权、派生顺序和资源生命周期。

ALLOWED 为上述 Runtime、测试、bootstrap 接线和新 RD3-R3 文档。
READ-ONLY 为既有 docs/report、Legacy719、graph/vulkan/transport 算法、依赖仓库。
FORBIDDEN 为用户未提交修改、main 合并和任何后续阶段生产实施。

## 失败、限制与资格边界

施工中出现两次普通编译错误（新增 EQueueRole 命名空间、复用 F5 schema 的 include 路径），均在 I 前修正。
首次 ASan 链接因第三方 STL 注解配置失败，按上文兼容选项重建，未修改生产实现以迎合 ASan。
验收后置检查曾将 ZIP 路径拼写为 RD3R3R3，未产出 PASS；修正命令路径后对同一 clean I 重跑，6131 项通过。
所有失败日志保存在 `E:/RD3R3A`，没有将失败算作通过。

原始 ZIP 包含 14 个 CRLF CSV 和图示尾空格，因此无过滤的 git diff --check 会报告输入包空白；
输入包按 Manifest 保持原字节，局部 -text 属性仅防止 Git 改写原文件。自写生产代码、测试和说明的 diff
空白检查与 clang-format 通过；未通过编辑设计包来消除警告。

当前 PASS 是有界 R3-A：只读 Buffer imports、graphics role、冷态同步初始化；它不是异步大资源传输。
最后一次 detach 后仍 pending 的资源保留至后续冷态 collect 或 Session 关闭，没有后台回收线程，也未实现
Cached/Explicit eviction policy API。ResourcePlan、可变 Storage provider、跨 family imports、图像存储、
Target resize/WSI/history 和资源修订事务均不在本轮资格内。

Chapter 26 允许先 detach 再删除的 Busy 合同，本轮保留；appendix B05 的自动级联删除措辞未实现。
REQ10/11/22 尚带继承的 Domain/history 用语，原包保留，实际 Owner 以采用的 04/19/26 章和本报告为准。
这不引入第二套 Owner，也不改变历史资格；后续阶段应在其追踪更新中解决这些措辞。

| 能力 | 本轮状态 |
|---|---|
| R3-A 唯一 Attachment、四种 Scope、外部 Instance Owner、真实 Ticket 退休 | PASS |
| P2-A 共享 Native Graph、输出隔离、通信与 GPU 安全相关回归 | PASS |
| 完整 Effect/Feature/ResourcePlan、原子消费者修订 | NOT_IMPLEMENTED，R3-B |
| 多 Effect 最终 Target、WSI/resize/history/local-read/transient 优化 | PARTIAL：仅既有 Native 部分能力，R3-C 完整目标未实现 |
| 专业 GPU Storage、MaterialGraph runtime 集成、大 UPLOAD/Transfer pipeline | NOT_IMPLEMENTED，R3-D 及后续合同 |
| Plugin Code Pin、ECS DataProjection、NativeProvider 可替换接口 | NOT_IMPLEMENTED，R3-E |
| F01–F36 全业务、H1–H6 真实高级算法、产品替换 | 未授予新资格；按 L/H 工作单实施 |
| 历史 F4/F5/F6.b 性能、完整 4K footprint、Android | NOT_RUN，历史 PARTIAL 原样保留 |

## 环境、证据与下一入口

Windows x64，MSVC 19.44.35226、CMake 3.31.10、Python 3.13；编译 SDK 1.3.280，验证层路径
`E:/RD3P1/sdk-1.4.304/Bin`。实测 GPU 为 NVIDIA GeForce RTX 4070 Laptop GPU，API 1.4.341，
driver_raw=2559967232。原始警告保留，没有屏蔽 Validation。

依赖均 clean 且匹配 P2-A：lux-cxx bf779515a120350c7c5412c366eea73afdf59ccf，
toolset c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60，imgui ad75bbcb15ba47f921dca082b1377cca9907ae98，
node-editor d948c439d84d9a12c36ac15fb8e2b4e6bd18a7ef。没有不必要地更新依赖。
4 个改动的公共头同步三个要求的 install include 前缀，共 12 份；旧内容保存在 install-backup，摘要见 header-sync.json。
原用户 checkout 仍为 246fe21eb6d060801b9a16ddcadde2be89dbbecc，其 settings.json SHA 和完整 diff 字节与预检相同。

源码树外证据根：`E:/RD3R3A`。复现脚本 verify-dev.ps1、verify-asan-dev.ps1；结果摘要如下。

| 证据相对路径 | SHA-256 |
|---|---|
| verify/build.log | `a9863e78f373a3553088302b49bd64daa1d5db88cb044a77ef773f12d484c8aa` |
| verify/build-noop.log | `4e4c47c7a58e7c8b526bbc6365fe73924a673a755a5404a28f3105566dc6d93b` |
| verify/ctest.log | `732510e79e66398c92857f16569bc1e03975934c7590aee4db90f1cc8dbd0e10` |
| verify/ctest-detail.log | `69279bcdb9868dca6563e556caf7795855fc3ed075fa2833c4c3a95b7ae63d6f` |
| verify/asan-build.log | `ab7df90c6ccc8556f52c25f8145a7381a98f4d4e1096781272488dda3d6d1bd7` |
| verify/asan-ctest.log | `f47a0872557615327b38866474e9f657b1d603d4f6f9ad82eaf83e10e0115e82` |
| verify/asan-detail.log | `1de6a498dbb0abcb9bee1ffa261458c7598eeaf7642b169f7fda838d9c8dbbb2` |
| verify/asan-probe.log | `e2dc1b90b65b004dd3add469908a2a024976c82adcc448be2e46588eb68ce610` |
| verify/preflight/result.json | `076517b643e8e1f15f12c125eb4250543a96fe7073bef63af9d20ff5d13e1f1b` |
| verify/postflight/result.json | `076517b643e8e1f15f12c125eb4250543a96fe7073bef63af9d20ff5d13e1f1b` |
| header-sync.json | `ac55265befeea6fc72e62523fecc1ec4947912fbf5bc08aa5968cdd601ba0ff6` |


R3-B 的明确入口是 `design/work_orders/02_R3B_Effect_Feature_ResourcePlan.md`：将固定 EffectSource
作者输入迁到 Effect-owned Feature/逻辑 Graph，以真实 Schema/ResourceUse 生成 ResourcePlan，复用现有
Scope+Lease store 解析准备好的 slot，加入 provider/writer/config/version 冲突与全部消费者的原子发布。
当前基础无未关闭的 R3-A GPU 安全阻断项；可变资源修订与完整候选闭包仍须在 R3-B 实现，不能据此提前宣称完成。
29–32 章的 Legacy Compiler、独立 MaterialGraph、三层 UPLOAD 和专业 Storage 合同继续保留。

本轮结论 **PASS（R3-A 有界范围）**。V 仅为本报告；推送 I/V 到 codex/render-v2 后 STOP，等待下一阶段授权。
