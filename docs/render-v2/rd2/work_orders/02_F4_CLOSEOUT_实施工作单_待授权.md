# RD2-F4-C｜Native Graph 风险、生命周期、性能收尾（必须在 D0 批准后单独授权）

> **预备指令，不是当前执行授权。** 当前 `F4=PARTIAL`; 旧 I `bd5399063907087e147ee159f24e1da6a87ab30f`，旧验证 V `b9f9a909bc34ac87f00c594996201a20f0af00c8`，不改变历史报告。F5 不授权。先确认 D0 有独立批准的 I/V 和 remote HEAD，再审查当前 F4 source，确定 minimal correction I。

## A. 不变量

沿用唯一 F2 LogicalGraph/PassSchema、F3 LayoutPlan/NativeProgram/R4 VMA/Submission/Retire，Feature 不拥有默认 DataSet；但本轮**不要**创建 RD2 的 TypedStorage/TypeCatalogue/Runtime，也不要为未来 GPU primitive 先造泛型抽象。只修 Native Graph 的已证明 Bug、接口生命周期漏洞、及 F4 性能门禁；保留已验证的 146 正常 + 146 full ASan / GPU 0 validation 资格结构和全部负例。

## B. 允许修改文件（按当前 repo 真实路径复核）

- **ALLOWED（仅确有最小复现证据）**：`modules/function/render/vulkan/src/graph/{Frame,Record,Resources,Sync,Recipes,Compile,Export,Copy}.cpp`、`modules/function/render/vulkan/pinclude/GraphNative.hpp`、`modules/function/render/vulkan/include/lux/engine/render/vulkan/graph/Executable.hpp`、直接受影响 `modules/function/render/vulkan/src/{memory,shader,transfer,descriptor}/...` 及其公开头，**但**改 R4/F3 已验收 owner 需要独立写影响/源码/性能理由；`modules/function/render/vulkan/test/NativeGraph*.cpp` 和需添加的最小 shader/codegen fixture、`cmake/render-v2-bootstrap/**` 的测试接线、`docs/render-v2/F4C_*.md` 新报告。
- **READ-ONLY**：F2 Logical Graph/Builder/Schema、`modules/function/render/core/**`、`transport/**`、`description/**`/generator 原源码、原 `docs/render-v2/final/**`, `rd2/**` 冻结设计、历史 `F4_VERIFICATION.md`、`F3_*` 验收和锁定 baseline。
- **FORBIDDEN**：`render_legacy/**` 719 blobs、RenderRuntime/Feature/Scene/ECS/Editor/UI/Product、第二 Vulkan allocator/queue/framework、自动跟进 F5。

如果某 bug 必须改 READ-ONLY 下层才能正确解决：**STOP**，提交最小复现与被影响的设计 ID，等用户批准有限依赖修复，不能越权。

## C. Issue C1 — GPU compute 写 Readback Buffer 的 Host memory visibility

精确源：`src/graph/Record.cpp` 当前 Readback 输出 Barrier 仅认 `TRANSFER_WRITE`（源码 L594–L650）而不处理 `SHADER_STORAGE_WRITE`。先构造 `Compute writes STORAGE_BUFFER`→`EGraphOutput::READBACK`→实际 completion→非一致内存适配 readback 的 GPU fixture（带 SyncValidation 和 CPU byte oracle），并存原失败/执行 trace。不应由 CPU wait 替代 shader-write→host-read memory dependency。

修改应在 cold Native Recipe 中建立 Readback 的实际最后写入者/Access/Staging，record 阶段只执行有证据的 Sync2 Buffer Barrier；不能再引入一套动态按业务 feature 处理 readback 的 hot loop。也检查原 F4 `HOST_READBACK` 路径不重复执行无意义 barrier。正负例覆盖 Compute/Transfer 两种 Producer、不同 Queue、间接 output chain、host invalidate、未 Ready/BUSY。

## D. Issue C2 — Imported resource native memory alias

当前 `src/graph/Frame.cpp` L34+ 对 distinct imported resources 主要以 Native Buffer/Image handle 相等识别，同 `VkDeviceMemory` 重叠但不同 `VkImage` handle 可能逃过。构造 `Image::alias()` 创建两个 native handle 同一 Memory/overlap 的实际 Vulkan 负例；至少 conservative cold or submit validation 用 Buffer/Image actual device memory+offset/length 检测，未显式 alias recipe 直接 reject。`Image::alias` 已由 R4 创建，按当前已公开 memoryBinding API 做必要补足；不为此引入通用跨插件 GPU alias scheduler。不能拿同 handle 才相等当 storage 唯一证明。

## E. Issue C3 — Import A→B 原始 ImageView/Backing lifetime

`src/graph/Recipes.cpp` 会为 Imported Image 在编译期创建长期 `ImageView`，`src/graph/Frame.cpp` 改写 views 时借用新 backing。独立测试：A/B 两 native Images，compile 用 A；B replacement 后，A 的 GPU 最后使用确实 completed；**在 Native Plan 仍存活期间**销毁 A backing 或视图，再正常调用 B、完成、销毁 plan，要求 0 native validation/lifetime use-after-free。验证当前代码是否在 plan retain old ImageView 或 retained borrowed reference 上产生错误。

如果失败：令 long-lived Plan 只存 value 的 Import shape/requirement 与必要的 precreated view 规则，而具体 Native imported image/view 由 invocation pin 到 GPU completion；创建新 view 的成本应在 cold prepare/safe mutation 而非 warmed submit。修正须保持每 Frame 0 native creations。不能为了简化生命周期每个 Submit `vkDeviceWaitIdle`。

## F. Issue P1 — fixed Binding 5% 未解决（绝不挑样本）

Locked F3 I `0fbbbd6db3aa4ad53283522e4a54d96556b5b478`; F4 baseline p50 +5.0420168067%，CI [+3.3058%, +7.6271%]、p95 +4.7619%；被测 `BoundDescriptorSets::bind()` 本体指令指纹曾相同，不能假定全部由噪声或函数源代码导致。

按照先证据再改代码顺序：锁定 CPU affinity/power/driver/compilers、本体及 calling binary/disassembly/field layout/size/link map、调用前后对象内存 locality；PMU instructions/cycles/cache（如果工具可用）和 A/A B/B controls；更新线程/queue/test impact。最小变更优先围绕对象布局、访问及调用方实际热点，不允许删合法验证或把 Immutable default 改回额外 Allocation 使 Binding 快。只在修复后使用预注册定量样本与同等 warm state 执行完整可比资格；若不满足，保持 `PARTIAL` 并说明根因/阻断，不运行到一次幸运结果才报 PASS。

## G. Issue C4 — Native limits / F4 边界

根据 FINAL Stage 15 和 RD2 07/09 逐项核查 **3D/Cube、Target-relative extent、Vertex/Indexed/Indirect** 等缺项到底原 F4 哪些是必须、哪些是 F5/F8 授权。现有 F4 明确 NOT_IMPLEMENTED，不能将其改名为 device fallback 或因 RD2 推进擅自缩减原责任。若必须追加 F4 scope：先提供最小互依需求与 user approval，然后只实现对应真正 Native 原语；不在收尾批次混入全套 F8 Mesh/Material/Feature。

## H. 类型和性能审查

对每个新 struct/class 列 Category、single responsibility、owner/borrow、hot/cold、为何不能是现有 type 的新增字段。拒绝 `GpuResourceManager`, `F4GraphManager`, `ResourceSlotService` 等无真正 authority 的类型。潜在 cold/precomputed recipes 优先合并重复热循环；不因追求少代码减掉 Sync2/fallback/retirement 检查。

## I. I/V 资格与强制 STOP

按设计批准基线建立 I，新 I hash 下独立 no-hardlinks clean clone，完整两轮 Normal+Full ASan、既有 146 CTest name/功能保持、新 native GPU fixtures、真实 Validation+SyncValidation zero Error、fault/memory/allocator, compiler/link/codegen closure/SDK public headers/negative compile、frozen 719 source bytes、user dirty worktree hash、真实 fixed Native Bind perf 配对原样报告。V **只新增** `docs/render-v2/F4C_VERIFICATION.md`；任何代码修正必须返回新 I 重做资格。通过门禁后 push STOP，报告原 F4 PARTIAL 的独立补充收据与用户审查；**不得自行进入 F5**。
