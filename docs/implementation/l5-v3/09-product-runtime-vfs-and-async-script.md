# Product Runtime Composition、AssetVfs 与 Async Script / Cross-frame Operation

Status: **Normative Runtime/Product Design (v3)**  
Date: **2026-09-02**

---

## 1. 目标

本文冻结三件此前分散在 Editor/L2 讨论中的全局语义：

1. 最终交付游戏是项目配置驱动的专用 executable target，而不是固定 Player。
2. `AssetVfs`、Execution、Render 等共享基础设施由 product/application composition 显式拥有；不使用万能 Context/global singleton。
3. Delay、资产 IO、GPU/physics query 等跨时间操作通过 Sender/domain async operation + script continuation 实现，不阻塞 game/main thread。

Current reviewed foundation has B+V1+V2 mechanical execution/asset-read capability in place；script continuation/state ABI itself is still a design gate and MUST be frozen before Wave S implementation。

---

## 2. Product 不是新的架构层

Lux architecture numbering 到 L5 Editor 为止。Product 是 build/composition dimension：

```text
Editor product
    L5 EditorApplication executable

Game product
    Project configuration
      + selected engine runtime modules
      + selected Simulation/Scene systems
      + project native/generated code
      + platform/backend selection
      + cooked asset/pak config
        -> generated project-specific executable target
```

不得创建一个仅为了装配而存在的 `Product/Application compositionManager/ProductHost/ApplicationServices` framework。

`PLAYER` 可继续作为 runtime-clean architecture qualification profile，验证 Runtime closure 不链接 Editor/L4 compiler；它不是最终产品身份。

---

## 3. Application composition root

Editor：

```text
EditorApplication
├─ ExecutionRuntime
├─ root TaskScope
├─ mutable AssetVfs
├─ AssetRead endpoint / AssetReadPort
├─ RenderRuntime (configured builds)
├─ SceneMetaManager
├─ Toolset
├─ UISession
├─ EditorSelection
└─ EditorContext (references/capabilities only)
```

Generated game：

```text
MyGame application composition
├─ ExecutionRuntime
├─ root TaskScope
├─ mutable AssetVfs
├─ AssetRead endpoint / AssetReadPort
├─ RenderRuntime if selected
├─ SceneMetaManager
└─ project-selected runtime/system composition
```

两者共享低层 capability contracts，而不是共享 `EditorContext`。

---

## 4. 三类共享依赖

### 4.1 Process-global facade：极少数

可以接受静态/global facade 的候选仅限：

```text
logging
assert/crash handling
low-level diagnostics/profiling sink
```

它们接近 process infrastructure，且不代表业务资源 ownership。

### 4.2 Application-owned shared infrastructure

必须显式 owner：

```text
ExecutionRuntime
AssetVfs
AssetRead endpoint
RenderRuntime
SceneMetaManager
```

消费者获得窄 capability，不通过 global Get() 查找。

### 4.3 Scope-owned state

```text
Scene / Simulation
script instance/continuation
EditorSelection
MaterialEditor / FlowForgeEditor
feature-local graph/document state
```

绝不能 process-global。

---

## 5. AssetVfs：explicit instance，不 static、不 lazy singleton

`AssetVfs` 的语义状态包括 mount order、provider、patch shadow/tombstone，因此需要显式初始化。

MUST NOT：

```cpp
AssetVfs::Get();
AssetVfs::resolve(...); // static business API
static AssetVfs instance; // lazy hidden owner
```

MUST：

```text
application creates AssetVfs
 -> mount /Engine
 -> mount /Game
 -> mount base pak / patch / plugin as configured
 -> publish read view
 -> start runtime/editor work
```

shutdown 顺序同样显式。

---

## 6. VFS read/control plane 与 concurrency

当前业务需要允许 main/game/worker 多方读取，而 mount 变化是低频控制操作。current contract：

```text
AssetVfs
    mutable control plane
    mount / unmount
       |
       -> build immutable MountTable
       -> atomically publish

AssetVfsView
    read capability
    resolve / open / enumerate / pathOf
    holds/loads safe immutable snapshot
```

推荐实现是 `shared_ptr<const MountTable>` snapshot + atomic publication；等价无锁/read-safe 方案可在不改变 public semantics 的前提下实现。

MUST prove：

- reader 不因 mount table republish UAF；
- read/read 并发安全；
- mount/unmount 与 read 的可见性规则确定；
- provider lifetime 至少覆盖所有引用它的 published snapshot。

`AssetVfsView::open()` 仍可能调用同步 provider，因此“线程安全”不等于“不会阻塞”。

---

## 7. Asset loading：VFS 不是脚本 IO API

已有 L2 contract：

```text
ReadAssetImage { AssetId }
AssetReadPort
loadAsset<T>(AssetReadPort, AssetId, limits) -> Sender
```

Product composition MUST 建立 production VFS-backed `AssetReadPort` endpoint：

```text
script/runtime request
  -> resolve path to AssetId (cheap read view)
  -> submit ReadAssetImage
  -> IO/backend thread or native async IO
  -> AssetBlob
  -> typed decode
  -> completion
```

如果 provider `open()` 是 blocking，MUST 使用 BlockingScheduler/IO executor/native async API；不得在 game/main thread inline 读取磁盘。

脚本不得获得 mutable VFS，也不得把 `open()` 暴露成同步 gameplay API。

---

## 8. L2 与 runtime async operation

L2 `execution` 负责机制：

```text
CpuScheduler
Main/owner scheduler
Timer
TaskScope / stop propagation
Blocking/IO isolation when required
```

L2 不拥有 GPU ray query、physics trace、Material compile 等语义。

Domain package 可以返回 Sender/OperationPort：

```text
asset_loading       -> AssetLoad Sender
render/scene query  -> RayQuery Sender/Port
physics domain      -> AsyncTrace Sender/Port
toolchain           -> Compile Sender
```

---

## 9. Script suspension：不能阻塞线程

FlowForge/Script runtime 最终必须支持 suspend/resume。

错误模型：

```text
Script native call
 -> Delay(2s)
 -> sleep(2s)             # forbidden
```

正确模型：

```text
Invoke continuation K0
 -> execute nodes
 -> encounter async node
 -> start Sender/domain operation
 -> save continuation state K1
 -> return to Simulation/frame loop

operation completes later
 -> enqueue ScriptResume{instance, continuation, result}
 -> explicit Simulation/script resume point
 -> validate instance/generation
 -> execute K1
```

不得持有 native C++ call stack 跨帧。Compiler 应将可挂起 graph lowering 成 explicit state machine/continuation representation。

---

## 10. Delay

Delay / next-frame operation 使用 L2 Timer/frame scheduling capability：

```text
Delay N
 -> TimerSender
 -> suspend script
 -> Timer completion
 -> enqueue resume
```

`DelayUntilNextFrame` 可以使用明确的 frame-resume queue，不需要 sleep 0/1ms。

---

## 11. Asset load in script

```text
LoadAsset("/Game/Foo")
 -> AssetVfsView.resolve()
 -> AssetReadPort/loadAsset<T>()
 -> suspend
 -> load/decode off owner thread
 -> result completion
 -> resume at Simulation point
```

路径是 soft address；真正异步 operation 应尽早转成 stable `AssetId`。

Scene/script instance 被销毁时，completion 只能通过 generation/instance validation 被丢弃或 stopped，不能回调悬空脚本对象。

---

## 12. GPU / physics cross-frame query

跨帧语义不因为“耗时”就归 L2 domain。

```text
Render/RHI or Physics
    owns query semantics, GPU fence/readback/resource lifetime
       |
       -> returns domain Sender/operation capability

Scripting
    owns suspension/resume

L2
    supplies scheduling/cancellation primitives only
```

GPU ray/readback 正常 gameplay path：

```text
Frame N submit
 -> GPU work/fence
 -> script suspended
 -> CPU frame continues
Frame N+1 or later readiness
 -> collect result
 -> enqueue resume
```

MUST NOT promise exactly next frame unless backend contract can prove it；use “earliest next frame / asynchronously when ready”。

MUST NOT call blocking GPU wait on the main gameplay path。

---

## 13. Resume affinity and stable point

Async completion may arrive on IO/CPU/render thread, but script execution MUST NOT resume there。

Completion records must contain stable identity/generation, not raw script/entity pointers：

```text
ScriptInstanceId/generation
continuation id/state
operation result
```

Simulation owns an explicit bounded resume/adoption point。This avoids arbitrary worker mutation of ECS/script state and makes ordering deterministic。

---

## 14. Cancellation / shutdown

Scope semantics：

```text
application shutdown -> stop application tasks/read endpoint
Scene stop           -> stop Scene/script async scope
script instance kill -> invalidate instance generation / stop owned continuations
```

Cooperative cancellation is acceptable for non-preemptible third-party calls；after completion, stale identity MUST be rejected before applying result。

---

## 15. Product target generation gate

Target outcome is frozen：

```text
Project config -> project-specific executable + cooked content
```

Exact project manifest、target generator、plugin/static module selection、platform packaging 尚需独立 normative spec。Until then coding agents MUST NOT invent：

```text
Generic Player executable architecture
Universal Host framework
Runtime service locator
Project manifest format
```

---

## 16. Implementation order relevant to this document

Current checkpoint treats B/V1/A/V2 as implemented foundation direction, subject to the R0 requalification in `07`。For this document the remaining order is：

```text
R0  Foundation requalification/hotfix
S0  freeze script continuation/resume contract
S1  continuation state + explicit Simulation resume queue
S2  Delay/Timer + AssetLoad/AssetRead bridges
Q   domain async GPU/physics query adapters (when first real consumer exists)
P   project target generation (after manifest/target spec)
```

S0 MUST freeze：

```text
script instance identity/generation
continuation/program point representation
locals/value state across suspension
resume result/error channel
cancellation and Scene shutdown
bounded resume queue / stable point
nested/repeated async behavior
ordering/reentrancy rules
```

Sender operation state MUST NOT be treated as a substitute for this language/runtime continuation contract。Q remains demand-driven but must follow the same suspension/resume rules。

---

## 17. Normative prohibitions

```text
No AssetVfs lazy singleton/static hidden state.
No global EngineContext/service locator.
No ExecutionRuntime or RenderRuntime singleton.
No synchronous script-facing disk IO.
No sleep/sync_wait on game/main thread for cross-time work.
No GPU blocking wait in normal gameplay path.
No worker-thread direct script/ECS resume.
No fixed Player as final product architecture.
No script continuation ABI invented ad hoc inside a Delay/AssetLoad implementation.
```
