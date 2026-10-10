# F1-FIX-2 — 独立资格验收

结论：**PASS（仅本轮作者身份、Kind 与 Depth/Stencil 访问合法性）**。
日期：2026-10-10。独立性指实现锁定后的独立 clean clone，不宣称外部人工复审。

## 提交和环境

- 批准基线：`bf922e84235487dcdddf0c8a8c1c75ac29abadf8`。
- Implementation I：`905efe1306a9c427bf8a121a0470f385d3c0cf07`。
- 本验收提交 V 仅新增本报告；资格过程中没有修改生产源码或在 clone 中打补丁。
- Frozen V1：`a669409a289a6fa4092f21176397795b1cdb7f3e`。
- 独立源：`D:/LuxQualification/render-v2-f1-fix-2-905efe1306a9/source`。
- 普通 / ASan 输出：同级 `build` / `asan`，均在源码树外。
- `git clone --no-hardlinks --no-checkout`，detached 检出 I；实施树与独立源分别通过
  `ValidateTrackedSnapshot.cmake`，资格前后独立源均 clean。
- Windows x64、MSVC 19.44.35228 / tools 14.44.35207、C++20、Ninja、RelWithDebInfo。
- 完整 ASan：`/EHsc /fsanitize=address /Zi`，`MultiThreadedDLL`；使用上一修复独立构建的
  SPIRV-Cross 1.4.335.0 ASan 包，原始来源/头哈希/构建日志与包一并归档。
  这次重新构建和运行全部 bootstrap ASan 目标，不将旧结果冒充本次执行；未禁用 vector/string annotations。

## 原始证据

证据目录：
`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F1-FIX-2/905efe1306a9c427bf8a121a0470f385d3c0cf07/`

SHA-256 manifest：855 项，`manifest.json` 哈希：
`68b724f97d3b542ddcacbbbb411a3f5638babf6e70765aade2d66ade1a51f227`。

包含 preflight 原始 HEAD/branch/status/diff/文件哈希和安装前缀备份；configure/build/CTest 命令、退出码和日志；
普通与 ASan 的 File API、compile commands、Ninja deps、codegen jobs、生成物和 SPIR-V；
反射与校验输出、基线缺陷复现、基准全部样本、保护清单和实际资格脚本。

## 构建、测试和生成链

| 检查 | 本次独立结果 |
|---|---|
| 普通 configure / `all -j 4 -- -k 0` | PASS |
| 普通第二轮 all | PASS，`ninja: no work to do.` |
| 普通完整 CTest | **99/99 PASS**，21.61 s |
| 完整 ASan configure / all | PASS |
| ASan 第二轮 all | PASS，`ninja: no work to do.` |
| ASan 完整 CTest | **99/99 PASS**，27.08 s |
| 原 92 项义务 | 全部保留，未改写旧测试文件；新增 7 项 |
| 原 R3 依赖/排序/生命周期/300 随机图/10k 绑定复用 | PASS，原 oracle 与 Plan/Bindings 源码未变 |
| Meta generator → PassSchema → emitter → glslc → reflection/validator | PASS |
| 真实生成物 | 61 项哈希，11 个 SPIR-V 模块另经 spirv-val / reflect / disassemble |
| 独立公共头、强类型/Concept/越层编译负例 | PASS，原有目标和拒绝用例保留 |

`test.log` / `asan-test.log` 与两套 `Testing/Temporary/LastTest.log` 保存各项原始输出。
新增用例为 `render.graph.fix2_{same_kind,cross_kind,lifecycle,forged,roles,depth_ops,aspects}`。
合法引用、拒绝候选后继续使用 Builder、移动/移动赋值、finish 后复用、原地址重建、optional fallback、
Texture/Buffer 两方向伪造与跨 owner、全部相关字段角色及 Depth/Stencil 正负例均覆盖。
`forged` 使用保留真实作用域的故意 bit_cast，证明 Kind 拒绝独立于无作用域数字构造的拒绝。

## 缺陷复现与修复后合同

`baseline-repro.cpp` 通过同一生产生成物/公开 Builder 分别链接批准基线对应的旧 I 和本 I。
旧 I 的 Graph/Shader/Description tree 与本次批准基线完全一致，记录于 `baseline-binary-source-identity.json`。
`defect-reproduction.json` 保存命令和结果，四个非法输入旧版全部接受，新版全部拒绝：

| 输入 | 旧基线 accepted | 本 I accepted |
|---|---:|---:|
| 不同 Builder 的同 Kind 同槽位 | 1 | 0 |
| 同作用域 Texture 伪造成 Buffer | 1 | 0 |
| depth-only 格式执行 stencil clear | 1 | 0 |
| DS 格式但未声明 stencil aspect，却执行 stencil clear | 1 | 0 |

GraphTexture / GraphBuffer 现在携带冷态作用域和声明位置。Builder 移动转移归属，移出或 finish 后得到新作用域，
不依赖对象地址、不重用旧作用域；没有全局对象表、逐 token heap owner 或运行时注册表。
`isValid()` 仅说明非零位置；真实归属由 `addPass()` 检查，手工数字 token 不能作为有效作者引用。
捕获时选定的 primary/fallback 同时保留归属；Definition 只保存已核验的局部位置，不保存 Builder 指针或作用域。
同拓扑的独立 Builder 仍生成相等 Definition，未改变逻辑复用判据。

`GraphFieldBinding::resource_kind` 由 typed wrapper 捕获，并在 Definition 中与实际 kind、字段角色分别验证。
Depth/Stencil 的非零 aspect 必须是实际格式所支持的子集；未声明 aspect 的 load/clear/store 明确失败。
合法 depth-only、组合 DS 和关闭 depth 操作的 stencil-only DS view 均通过；stencil LOAD 保持 READ_WRITE。
这些是声明合法性检查，不实现跨 Pass 子资源 Hazard 调度。

## 性能与分配

对上一批准实现的 binding binary 与本 I 进行 7 组交替对照，每次 100 万操作、10k 预热，
固定同机进程 affinity mask=4；全部样本保留，以各组指标的中位数比较，不删除离群样本。

| 指标 | 基线 | 本 I | 变化 |
|---|---:|---:|---:|
| p50 ns/op | 10.6 | 10.6 | 0.00% |
| p95 ns/op | 10.6 | 10.6 | 0.00% |
| 每次运行 max 的中位数 ns/op | 223.5 | 141.7 | 仅记录抖动，不宣称优化 |
| 稳态 C++ allocation count / bytes | 0 / 0 | 0 / 0 | 保持 |

性能门禁 PASS。作者作用域分配与检查只发生在冷态。
本 I 冷态 Tonemap authoring 单独记录：10,000 次，p50 1040 ns、p95 1350 ns、max 19010 ns；
150,000 次分配、33,120,000 字节。此工作包含 owning metadata 与 scope 捕获，不宣称冷态零分配。
原始记录见 `performance-*.log` / `performance.json`。

## 闭包与保护

普通和 ASan 分别检查真实 File API/source/include/link/codegen/Ninja deps，各记录 287 个编译输入头。
Graph production 仍只链接 Core；公开头只消费中立 Description Image/PassContract 头，
无 Vulkan、Runtime、Scene/ECS、Editor、Transport、Legacy 或 Description Math/Script 聚合依赖。
离线 Shader 工具和 SPIRV-Cross 仅在已有生成/验收目标，不进入 Graph production 链接。

- Core、Transport、R4 Vulkan、719 个 Legacy 的 blob/mode/size 与冻结清单一致；保护 CMake 仍是第 720 项。
- Shader Toolchain、Description、bootstrap、Graph codegen、旧测试与 Plan/Bindings tree/blob 未变。
- FINAL、R0–R4/F0/F1/F1-FIX 历史报告及旧判定未改写；完整对象清单见 `protected-objects.json`。
- 原用户工作区 HEAD、branch、status、staged/unstaged diff、untracked 清单和六文件原始字节哈希全部一致，
  见 `user-protection.json` 及 `preflight/`。没有 reset/stash/恢复覆盖行为。
- 四个变动公共头同步到 Debug、RelWithDebInfo、Android 三安装 include 前缀；12 项来源/前后哈希和备份已归档。
  同步不代表 Android 或完整 installed SDK 验收。

## 完整变更清单

- `docs/render-v2/F1_FIX_2_F3_PREREQUISITES.md`
- `docs/render-v2/F1_FIX_2_WORK_ORDER.md`
- `modules/function/render/graph/include/lux/engine/render/graph/Authoring.hpp`
- `modules/function/render/graph/include/lux/engine/render/graph/Builder.hpp`
- `modules/function/render/graph/include/lux/engine/render/graph/Definition.hpp`
- `modules/function/render/graph/include/lux/engine/render/graph/Schema.hpp`
- `modules/function/render/graph/src/Builder.cpp`
- `modules/function/render/graph/src/Definition.cpp`
- `modules/function/render/graph/test/CMakeLists.txt`
- `modules/function/render/graph/test/authoring/LocalIdentity.cpp`

上列 10 个文件属于 I；V 仅新增 `docs/render-v2/F1_FIX_2_VERIFICATION.md`。

## 保留项与停止

整数 RenderTarget Clear 类型与 Shader Include/声明顺序仍为 **NOT_IMPLEMENTED / OPEN**。
精确追踪、关闭证据和 F3 准入规则见 `F1_FIX_2_F3_PREREQUISITES.md` 的 F3-PRE-01 / F3-PRE-02。
本轮没有修复或关闭它们；既有有限 Shader fixtures 通过不能代表通用 Include 顺序已正确。

新 F1 GPU 输出、F2 完整调度器、Native Graph、Runtime/FrameLoop、完整产品、Linux、Android、
完整 installed SDK：**NOT_RUN**。原有 R4 native tests 作为保留回归实际运行，不能替代上述未实施功能。

`V2_PRODUCT = EXPECTED_UNAVAILABLE`

`NEXT_STAGE = F2, only after explicit user review approval`

**STOP。没有进入 F2、VulkanGraphCompiler 或 Runtime。**
