# RD3-R2 P2-A 独立验收 — PASS

日期：2026-10-12。结论仅覆盖用户正式授权的 P2-A 和本报告列出的 Windows x64 实测路径。
没有进入 P2-B/P3/P4/P5，没有合并 main；历史 RD2/RD3/P1 判定保持不变。

## Git 身份与独立性

- 施工前、提交前和推送前查询的远端 `codex/render-v2`：`98002bb03bb8ae0c6ed156fcd7e847082b6a7148`。
- **Implementation I：`483d20ed900b8c226a740704f3c888498b1dae11`**，直接基于上述 P1 V。
- I 修改 19 个文件，1266 行增加、320 行删除；没有删除文件。精确路径见检查器输出。
- 本报告所在 **Verification V** 是 I 的直接子提交，只新增本报告；V SHA 和实际推送后的远端 HEAD
  记录在交付消息及源码树外 `E:/RD3R2P2A/delivery.json`，避免在提交内容中自引用其 SHA。
- 施工副本：`E:/SyncForder/CodeRepos/lux-engine-rd3r2-p2a`。
- 独立验收源：`E:/RD3R2P2A/verify/source`，独立构建：`E:/RD3R2P2A/verify/build`。

在 clean tracked I 执行 ValidateTrackedSnapshot 后，以 `git clone --no-local --no-checkout --depth 1`
建立独立仓库，配置 `core.autocrlf=false`、`core.longpaths=true`，detached checkout 精确 I。
只补取基线和冻结 Legacy 提交对象供只读核验。Git dir/common dir 均属于验收 clone，
无 alternates，Git 对象文件链接数全部为 1。验收源前后 clean，全部检出文件逐字匹配 I 的 Git Blob。
验证环境内没有修复生产代码，也没有通过忽略文件补充源码。
这里的独立性指仓库、对象存储、构建目录和执行进程隔离，由同一代理完成，不声称独立人工审查。

## 实测门禁

| 项目 | 精确 I 的独立结果 |
|---|---|
| 前后完整性 | 各 **5858/5858 PASS**；原 Legacy **719/719** 的 mode/blob/size 与冻结提交一致 |
| C++20 构建 | MSVC RelWithDebInfo、Ninja `target all -j 4 -- -k 0`，**504 个步骤成功** |
| 第二轮构建 | `ninja: no work to do` |
| 公共头 | Objects/Types/Runtime/Backend/ResourceDomain 独立编译并禁止传递包含 Vulkan；NativeAssembly/DeviceSession 显式 Native 头独立编译通过 |
| 格式 | clang-format 19.1.5 对 I 修改的 C++ 文件 `--dry-run --Werror` 通过；`git diff --check` 通过 |
| 相关测试 | **43/43 PASS，0 failed，38.39 秒**；串行，未与构建并发 |
| 三级 Context | 真实 Instance/Device/VMA/Queue/资源池所有权迁移，实例发现包含实际选中设备，ResourceContext 借用同一个 VMA |
| Domain/类型 | ResourceDomainId 与 SceneId 混用在 C++20 concept 检查中被拒绝；父链默认只读；三个类型 factory 的惰性命中与隔离通过 |
| Factory | 真实 Buffer 创建，配置冲突、unique_ptr 接管/冲突、Native OOM 后无发布且可重试，通过 |
| 地址与销毁 | 增加 128 个 Domain 并触发 dense swap/pop 后引用不变；真实 vkCmdFillBuffer 后销毁 Session，子资源析构检查 Queue Ticket 已完成且父资源仍存活 |
| 生成式 ID | Scene/View/Target/Effect/Graph/Domain 的生成、删除、重建、旧代际失效和 SlotMap 代际退休通过 |
| Binding | 重复 attach 不创建 Native backing；显式 detach、重复 detach 拒绝、仍被引用对象 Busy、跨 Scene 代际和跨 Runtime owner 拒绝，通过 |
| 一 View 多绑定 | 同一个 View 同时绑定两组 Effect/Target，参数和最终像素独立；两组仍复用同一 Native Graph |
| P1 GPU 主线 | 两 Scene、四 View、同 Effect、同 RenderGraphId，FIF=2 和 3 均通过；各绑定和 Target 的 RGBA 像素 Oracle 通过 |
| Shader/PSO 共享 | Runtime Native compile=1、cache hit=1；fault seam 验证重复 Effect 不新增 Pipeline |
| 热态 | 两种 FIF 下稳定 12 帧的 C++ allocation count/bytes 均为 0；无热态 Pipeline 构造 |
| Stop/Reply | Client 在 Backend 销毁后存活；Reply 正常完成或 Stopping；Client/Backend 都释放后仍可 abandon 已接受 Request |
| 失败与退休 | Event/Thread 失败重试、Native attachment 分配回滚、第一笔提交接受后第二笔失败、安全 Drain；18 类 Native owner 计数最终全部为 0 |
| 同步验证 | RD3 测试启用 Vulkan Validation 和 SyncValidation，`validation_errors=0`；原 Native queue/transfer/lifetime/fault 测试通过 |
| 算法保护 | 全部 Native `vulkan/src` 和通用 `graph/src` 翻译单元逐 Git Blob 保持不变；没有替换 C0–C13、Shader、Pipeline、Queue/Sync2、提交和退休算法 |

Runtime 像素 Oracle 使用输入 RGB=(1,2,3)，计算 `x * exposure / (1 + x * exposure)`，逐通道误差
小于 `1e-6`，Alpha=1。四个 View 的 exposure=1/2/3/4，红通道分别核对 0.5、2/3、0.75、0.8；
同一 View 的另一绑定 exposure=7 核对红通道 0.875，同时原绑定仍为 0.5。
这是实际 GPU readback 断言通过，不是仅检查 GraphId 相等。
底层 multi_owner 另行输出：FIF=2/3、pipelines=1、shared_code=1、slots=8/12、oracle=PASS、warm_allocations=0。

测试集合为 RD3 两项、全部 transport 合同/回复/停止测试、六项相关 Graph binding/reuse/hazard/oracle，
以及 Native Graph、multi_owner、device/objects/transfer、candidate/fault/lifetime、共享 Shader/Layout、
compute binding 和 C1 单队列/多队列/部分提交。
精确可重复的 CTest 过滤式：

```text
^render\.(rd3\.|transport\.|vulkan\.(native_graph|multi_owner|device|objects|transfer|faults|lifetime_contract|bounded_allocation|graph_candidate|shader_candidate|shader_layout|shared_owners|compute_bindings|closeout_C1(-multi|-partial)?)$|graph\.(bindings|reuse|binding_allocation|scope_sharing|scope_hazards|logical_oracle)$)
```

## 对象模型验收

新增的主要生产文件为 runtime 的 `DeviceSession.hpp/.cpp` 和中立 `ResourceDomain.hpp`。
Backend 独占一个固定地址 Session；Session 独占三级 Context、稳定 Domain 和 Native Graph/执行池。
Scene 独占局部 View SlotMap，Backend 独占 Scene/Effect/Target。
Binding 的身份是 EffectId 加完整 SceneId/ViewId/TargetId 三元组，没有新增 BindingId 或 SessionId。
唯一可编辑关联表是 `SlotKeySparseSet<RenderEffectId, vector<EffectBinding>>`；有序 jobs 是冷态派生值。

`View.effect/graph/target`、`Target.view`、`GraphViewGroup` 已删除。
`createView(scene)` 与 attach/detach 分离；graph、参数发布、读回均使用明确的 RuntimeBinding。
Session 退出顺序为：停止/排空既有 Backend worker → 执行 Context 的实际 GPU 票据 → 剩余已接受
Native transfer → 子 Domain → 父/Root Domain → ResourceContext → Native Graph/Shader/PSO →
Queue → VMA → VkDevice → Instance/validation。队列地址和 Domain 地址均稳定。
详细 Owner/借用合同与调用示例见 [P2A_IMPLEMENTATION.md](P2A_IMPLEMENTATION.md)。

施工中真实发现的一项错误保留在外部失败日志：从一 View 一 Effect 解耦后，Backend 会尝试从已经
借用 Scene 输出的绑定再次借用，Native 正确拒绝。I 在 Backend 选择来源处限制为本帧真实生产者，
允许同一 View 的不同绑定合法复用，未修改 Native 拒绝规则。该修复已随 I 在 clean clone 重验通过。

## 环境、保护与证据

Windows x64；MSVC 19.44.35226；CMake 3.31.10；Python 3.13；编译 SDK 1.3.280；
验证使用 `VK_LAYER_PATH=E:/RD3P1/sdk-1.4.304/Bin`。
日志确认 GPU 为 NVIDIA GeForce RTX 4070 Laptop GPU，Vulkan API 1.4.341，driver_raw=2559967232
（Windows 驱动 32.0.16.1088）。系统另有 Intel Iris Xe。
系统 EOS overlay 的重复 manifest 警告仍存在，没有屏蔽；不将其算作新的 validation error。

依赖均 clean，未改动：lux-cxx `bf779515a120350c7c5412c366eea73afdf59ccf`，
toolset `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`，imgui `ad75bbcb15ba47f921dca082b1377cca9907ae98`，
node-editor `d948c439d84d9a12c36ac15fb8e2b4e6bd18a7ef`。
依 AGENTS 将 10 项公开/生成头同步到三个既有 install include 前缀，共 30 份；此前内容保存在
`E:/RD3R2P2A/install-backup`。这不是 Android 构建验证。
原 LuxEngine 工作区仍在 `246fe21eb6d060801b9a16ddcadde2be89dbbecc`，仍只有原有
`.vscode/settings.json` 修改；未改动 QGC 工作区。历史 docs 和全部 Legacy 文件保持原 Git Blob。

实际输入是用户附带的 Architecture ZIP，含所列 P2-A 工作单和架构章节；本轮授权来自用户消息。
ZIP SHA-256：`839ce60343d843e56317adbd72a12d7f388f0c22e75ed1b961d8c50a86e2d2d3`；
ZIP CRC 和 42 项 MANIFEST 摘要均通过。
证据根目录为 `E:/RD3R2P2A`，独立脚本为 `verify-dev.ps1`；下表为相对路径与 SHA-256：

| 文件 | SHA-256 |
|---|---|
| verify/build.log | `3920d9e450d8e4430af5a331900cd920404211bab688e95660d7af117c007bb8` |
| verify/build-noop.log | `4e4c47c7a58e7c8b526bbc6365fe73924a673a755a5404a28f3105566dc6d93b` |
| verify/ctest.log | `37c2859ab002f8d0016d9a7a4e45be96fa0d46f0e6d97ed5f15d40d6f5974c84` |
| verify/preflight/result.json | `088d0683b12b534423210ba080162b9577092d4bf18f22a10c62e9820e98674d` |
| verify/postflight/result.json | `088d0683b12b534423210ba080162b9577092d4bf18f22a10c62e9820e98674d` |
| header-sync.json | `fea8befd0e792059ccb2d989930da51f9945cc1beabf9458d196aa7ac4aa5907` |

## 明确延期与停止边界

本轮 **PASS**，不代表完整 Render V2 产品完成。仍未实现 P2-B 的完整 Effect-owned logical Graph、
Feature Factory/结构热更新原子发布；P3 的多 Effect 同 Target 合成、WSI/resize/history；P4 的
专业 GPU Storage、Native import leases 和异步资源替换；P5 的插件/CodePin/Engine/Editor 集成；
以及 L/H 算法。当前 registry 为同 Domain 一个 T、编译内类型标识，不是跨插件 ABI。

与本轮无关的完整 F4/F5/F6.b p95/CI、4K footprint、产品与 Legacy 业务恢复、Android/其他平台资格
为 NOT_RUN/NOT_IMPLEMENTED。运行中的已有 multi_owner 输出性能样本仅作观察，未重新授予旧性能资格。
P1 暂停注册的旧结构测试继续保留源码及原判定，没有为本轮额外删除测试。

P2-B 最小下一步：将固定 EffectSource 的逻辑定义移入 Effect-owned Graph/Feature 组合，复用本轮
Session/Domain/Binding 接缝，对全部绑定消费者执行完整候选的原子发布。需要另行授权。
本轮只推送 I/V 到 `codex/render-v2`，然后 STOP。
