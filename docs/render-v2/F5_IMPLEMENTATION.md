# F5 实施记录（未取得阶段资格）

入场与权限见 `F5_ENTRY_DECISION.md`、`F5_NATIVE_IMPACT.md`。本文件记录开发事实，不替代独立 Verification。

## Native Plan / Invocation 接缝

| 类型 | 唯一职责、所有权 | 线程与失败 | 更简单表达的边界 |
|---|---|---|---|
| ExecutableGraphPlan | 冷态可复制的只读 handle 持有不可变 NativeGraphCode；Shader、Pipeline、Layout、结构 recipe 的唯一共享存储 | 冷态工厂 expected；发布后只读 | 不能再用同一 owner 的 frame slots 表示任意 View/Invocation |
| NativeGraphCode（私有） | 冷构造的 LogicalPlan、NativeShaderProgram、资源形状、Sync2/alias recipe；发布为 shared_ptr<const ...> | 编译期可变，发布后不可变 | shared pin 只在执行 owner 冷态创建时取得；逐次 submit 不复制或增减 refcount |
| NativeGraphInvocations | 有界 GPU backing、ImageView、Descriptor、scratch、queue ticket 的 move-only owner；显式 invocation slot | 单 Backend 执行域；容量/错误通过 expected；析构完成最后借用 | 容量与 FIF 分开；仍使用已有 R4 native owners，不重复拥有 Vulkan handle |
| NativeCompileInputs | LogicalPlan、设备、Queue 位置、Native command 和 alias 选择的短期冷编译借用 | 仅工厂调用期间 | 已移除 allocator、初始 Import backing、动态值与 frame capacity |
| NativeInvocationInputs | 冷态创建执行存储所需的 allocator、初始 binding、动态值、sampler 和独立容量 | 借用依赖必须覆盖实际 native 使用期 | 不承担 Shader/Pipeline 编译，不是第二 Graph |

跨 Invocation 的 FrameGraphBindings 仍仅为同步 borrow。最终 Runtime 必须拥有各次提交需要的输入快照和 native lifetime；不能持有调用栈临时 span，也不能用 CPU revision 代替 GPU completion。

旧测试的 `compileFixture` 仅把两个真实 production factories 顺序装配为测试值，测试显式访问 `.plan` 或 `.invocations`；没有 production 兼容入口、第二调度器或 fake Shader。原 benchmark 完整冷工作量继续包含所有 backing 和 descriptor 创建，其阶段计时按两个工厂累加，不能只测纯 Plan 工厂冒充等价结果。

旧 `frame_capacity=1` 的拒绝断言不能套在新的 Invocation capacity 上：一个 Invocation slot 是合法的独立容量，零容量非法；Runtime 的 FIF 约束须在实际 Runtime 配置中单独验证，不能遗失旧两/三 frame 回归义务。

## 已运行的开发回归

证据：`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/RD2-F5/development/`。

- Native Graph target 与迁移后的测试目标构建成功。
- `const-regression.log`：13/13；原 native_graph、graph_candidate、C1/C2/C3、五种 Draw，加新的 native_invocations。
- 新用例：一份包含 compute/graphics Pipeline 的 Native Plan，四个 View，2/3 FIF 对应 8/12 独立 Invocation slots；逐 View exposure 不同，所有像素与独立 CPU Tonemap 公式一致。
- 一批提交仅等待最后 completion，然后读取全部早期回执；没有 waitIdle 或每 View 新建 Plan。
- 24 个预热后批次提交路径首方 C++ allocation count/bytes 均为 0；Validation 0 error。Camera/history/target identity 在 fixture 中独立，但不据此宣称 Runtime/Target/History 功能已经完成。
- 已同步受改公共头至三安装 include 前缀并保存原字节。本项不是 installed SDK 或 Android 资格。

本段是早期 Native fixture 记录；后续真实 Runtime 结果见下文。独立 clean-clone、完整 Normal/ASan、最终性能与闭包仍未完成，F5 不能据此标 PASS。

## 未完成门禁

最终独立 Normal/Full ASan、性能、头文件/真实依赖闭包、保护快照、I/V 仍须完成；不能把以下开发结果替代这些门禁。
F4 冷态 p95 的延期结论保持原样；F6/F7 尚未授权。

## Scene reuse / Target physical specialization development evidence

- `native-regression.log`: 19/19, including W01, shared owners, compute descriptors,
  Shader/Graph candidate rollback, Native Graph, C1/C2/C3, all five Draw modes, and two F5 fixtures.
- `NativeInvocations` now also runs Scene compute once with four View executions in 2/3 FIF.
  Native owner captures the accepted values/imports; a same-frame donor is checked using F2
  `mayShareSceneInvocation`. Changed revision is rejected. Caller mutation cannot rewrite the
  donor snapshot. Actual shared resource states/tickets advance with each consumer; donor
  retention follows their GPU joins. External borrowing also extends the donor and invalidates
  state reuse until its owner supplies a new invocation. No revision is treated as a fence.
- Shared outputs cannot alias View scratch; a non-shared writer to that backing disallows this
  reuse path. Native fixture allocation still reserves bounded private scratch per invocation;
  these are not duplicate Scene data authorities. Runtime owns the eventual sharing/cache policy.
- `TargetExtent`: 1:1 TARGET_RELATIVE 4x4→6x3, explicit physical dimensions in allocation, copy,
  and descriptor validation. Actual Shader/PSO/Layout owner address is identical across physical
  specializations. Zero extent is rejected before allocation; old invocation remains readable
  after failed candidate and while new size executes. CPU pixel oracle passes, validation 0 error.
- The public Plan handle can be destroyed while invocation/code pins retain native programs.
  Cold `specializeVulkanGraph` calls the same native compiler, not a parallel compiler.
- Relative scaling expressions and DYNAMIC extent classes are not inferred from nominal width/
  height. Current native 1:1 contract requires nominal width=height=1. This limitation must remain
  explicit when reviewing Target authoring consumers; it is not a hardware fallback.
- Original failures `target-entry-gpu.log` (Unsupported) and `extent-gpu.log` (descriptor extent
  mismatch) remain archived. Corrected result is `extent-gpu-2.log`.

These are development results, not the final independent F5 qualification. Runtime, WSI,
independent pacing, lifecycle/failure integration, final hot performance and clean-clone I/V
remain outstanding. No F6/F7 authorization is inferred.

## Runtime assembly boundary under implementation

The RD2 DAG places VulkanBackend/Scene/View/Target in render_runtime (02 §2.1),
not in the Foundation target. Runtime's neutral header will expose Core/Transport
values only; NativeAssembly is the explicit cold Vulkan assembly header. A short-lived
RenderRuntimeBuilder will own the real transport route table and complete Backend,
then transfer them once into a started RenderRuntime. It is not an init/shutdown
protocol or a second execution owner. After handoff only the backend thread accesses
native state. No Feature SDK API is introduced.

RuntimeAddress<Local> adds the actual endpoint owner identity to existing Core SlotKey;
there is no new global registry. RuntimePathSource consumes existing LogicalGraphPlan,
NativeShaderProgram, NativePassCommand and GraphInvocationData without a second Shader
schema. RuntimeCapacity bounds Scene/View/Target/path/variant/asset/queue capacities and
keeps FIF separate. RuntimeProgress is an observation value, not scheduling authority.
The forthcoming Type/ownership inventory and tests must verify these roles before F5
qualification; declarations alone are not an implemented Runtime.

## Real Runtime development checkpoint (not independent qualification)

`runtime-seven.log`: seven Runtime GPU/contract tests pass. Original 156 obligations
have not been relabelled as rerun. All development failures remain in the external archive.

- Runtime owns one R2 Transport and one VulkanBackend; one backend thread drains bounded
  CONTROL/PROGRAM batches and executes the sole free frame algorithm. An OS wake event
  and high-resolution deadline timer pace it; zero active output waits for new work.
  Simulation publication never waits for a GPU frame.
- Four Views share one immutable native plan and execute independently. Native scene reuse
  requires F2 proof and actual retained donor GPU resources. 2/3 FIF are separate from the
  8/12 invocation capacities. Warm CPU allocation count/bytes are zero in the tested intervals.
- 10 publications at 100ms intervals coexist with approximately 120Hz rendering; at least
  24 consecutive frames without PROGRAM updates execute real GPU work and CPU pixel oracles.
- Two Scenes share the exact same retained VkBuffer after one upload. A different RenderPath
  adds a real Tonemap graphics pass. Switching/deleting one Scene leaves the other's pixels
  and asset backing valid (`paths-gpu.log`).
- Four View-owned history bindings reference four distinct Backend-owned native buffers.
  Their GPU counters accumulate across 3 FIF; generated scalar fields receive each View's
  camera, current render time and history epoch. Resize invalidates only that View's epoch.
  GPU output independently proves all values (`history-gpu-4.log`). This is not DataSchema,
  TypedStorage, a universal history manager or a temporal reconstruction algorithm.
- RuntimeFrameField is a cold link to existing generated scalar metadata. The failed
  `history-gpu[-2/-3].log` exposed a mistaken link to `history.data`, a buffer member layout
  entry. Runtime now rejects resource-member fields as push-value destinations. No generator
  or Shader schema authority was changed.
- Real Win32 Surface/Swapchain, FIFO Present, acquire synchronization and maintenance1
  Present fences. Two targets explicitly reuse the same View output; declared opaque layers
  use deterministic order, independent of View creation order. BGRA8 readback proves pixels
  in the base and overlapping regions. Shader blending remains declared Graph work, not
  hidden Driver shader dispatch or a claimed F10 UI implementation.
- A minimized source target suspends new source invocations; another active output target
  may explicitly present its last legal result without re-executing Graph work. All minimized
  outputs stop frame progress. 32x32→48x24 rebuilds the native extent variant while retaining
  exact shared Shader/PSO/Layout owners (`mirror-gpu.log`).
- Pre-admission includes Native Graph slots/queues and bounded output release/copy/retention
  submissions. CPU revisions never authorize slot reuse. Runtime target and asset owners keep
  actual range state and GPU tickets; the Driver updates state after external output access.
- Candidate Image allocation failure preserves last-good rendering. Real Runtime tests also
  cover retained immutable publication bytes, foreign owner, wrong thread, stale View,
  in-flight removal, reply obligations after Runtime destruction, and Device Lost at first
  and partial native submission (`lifecycle-gpu-new.log`, then `runtime-seven.log`).
  Device Lost is explicit fault injection, not an observed driver failure. The test-only seam
  drains the healthy GPU before injecting loss because injection cannot physically cancel it.
- RuntimeRequest is a move-only reception obligation over R2's bounded arena. Poll consumes
  a terminal value; destruction/abandon releases unread capacity, including after Runtime death.
  Independent async diagnostics use a bounded SPSC ring; they are not request replies.

## Concrete boundaries

- No DataSchema/TypedStorage/Feature SDK/Scene ECS implementation. Fixed F5 messages are generated
  by the existing luxop pipeline. Runtime public Runtime.hpp depends on neutral Core/Transport/
  Graph contracts; NativeAssembly.hpp is the explicit cold native assembly surface and its
  consumer also links the native target. No reverse Vulkan→Runtime edge exists.
- Window qualification is Win32 + available swapchain maintenance1. Missing capability returns
  an explicit error; untested platforms/presentation modes are not registered as passed.
- TARGET_RELATIVE is the explicit 1:1 form already stated above. BGRA8 UNORM FIFO is the current
  window output signature. Other presentation encodings are not claimed by these tests.
- Public creation is cold builder assembly; backend ownership transfers once into the running
  Runtime. Borrowed assembly/native references must not be used after handoff. Live controls
  cover publication, path changes, activation, resize, removal, readback and stop.
- F4 cold p95 debt remains PARTIAL under the user's limited deferral. F5 hot correctness,
  allocation, lifecycle and independent qualification have no such waiver.

## Shared native Buffer partition refinement

All RenderPaths are assembled before Scenes. Cold assembly unions the declared
boundaries for each shared actual Buffer, then the existing Native Sync2 compiler
refines affected plans. Candidate plans share the exact Shader/PSO/Layout owners;
publication is transactional, capacity checked, and counted as cold compilation.
There is no hot repartition, F2 algorithm change or second state/resource authority.
Extent specialization preserves these boundaries. The two-Scene fixture covers
[0,16) versus [0,32) access to the same 32-byte backing through different paths.

The 32-byte shared Buffer fixture uses only its first 16 bytes. `range-before.log`
records the real Runtime failure after CPU-ready evidence for the untouched cell was
lost. The correction retains that initial evidence; touched cells still carry their
actual native tickets. `range-after.log` verifies both real GPU pixel oracles and zero
warm heap with the same fixture. No historical report is changed.
