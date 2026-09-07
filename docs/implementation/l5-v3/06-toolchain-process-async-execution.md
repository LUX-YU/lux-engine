# L2 Execution、L4 Compiler Sender 与 Runtime Async Operation 基建

Status: **Normative L2/L4/Runtime Async Integration Design (v3)**

Current checkpoint: B/V2 are implemented direction, but R0 MUST fix TaskScope re-entrant admission and rerun clean qualification before new feature work.

---

## 1. 目标与核心结论

Editor 与最终游戏都需要在不阻塞 UI/game owner thread 的情况下运行编译、资产 IO、Timer 以及跨帧 operation，同时保持 L0–L5 依赖方向与生命周期清晰。

本设计冻结四条核心原则：

1. **Compiler 是长期 capability，不是一次 Compilation。**
2. **Compiler 必须 logically stateless / reentrant；一次编译的 mutable state 属于 Sender operation state。**
3. **异步 Toolchain operation 必须消费 owned immutable snapshot，不借用 UI/document mutable source。**
4. **L2 只提供执行机制；Material/FlowForge Sender 仍由 L4 定义。**

Current checkpoint already implements the B/V2 direction；v3 preserves that architecture and adds the R0 TaskScope re-entrant admission requirement before further feature work。

简化图：

```text
EditorContext.Toolset
       │
       └─ MaterialGraphCompiler (immutable/reentrant)
                    │
           compile(owned snapshot)
                    ▼
                 Sender
                    │ TaskScope.start
                    ▼
              CpuScheduler (L2)
                    │
                    ▼
          deterministic L4 core
                    │
                    ▼
              MainScheduler (L2)
                    │
                    ▼
       apply by stable identity/revision
```

---

## 2. 分层职责

### L2 Process / Execution

只拥有：

```text
thread / worker ownership
bounded admission
scheduler capability
timer
stop propagation
structured task lifetime
main-thread mailbox / continuation
runtime shutdown / join
```

不得拥有：

```text
Material
FlowForge
Scene
Editor document
compiler diagnostics schema
compile cache policy
```

例外不是“domain leakage”：`engine/process/asset_loading` 是明确的 L2 package-scoped workflow，可以认识 `AssetId/AssetBlob/AssetSerDeser` 这些 lower-layer resource contracts；只有 `process/execution` leaf 必须完全 domain-blind。

### L4 Toolchain

拥有：

```text
MaterialGraphCompiler
FlowForgeCompiler
Model/Texture/Shader importer/cooker/compiler
synchronous deterministic core
L4 domain Sender factory
compiler environment
compile result/failure schema
```

### L5 Editor

拥有：

```text
when to request work
source/document revision
which result is current/stale
UI diagnostics/state
which stable document/entity/asset receives result
```

### Product/Application composition root

`EditorApplication` 或 generated project application 直接拥有 process-lifetime `ExecutionRuntime`，驱动 owner-thread `drainMain()` 与最终 shutdown/join。不存在独立 Product/Application composition layer。

---

## 3. 正确依赖方向

允许：

```text
L5 MaterialEditor -> L4 MaterialGraphCompiler -> L2 execution
L5 FlowForgeEditor -> L4 FlowForgeCompiler      -> L2 execution
```

禁止：

```text
L2 MaterialCompileProcess -> L4 Material compiler
L2 FlowForgeOperation     -> L4 FlowForge compiler
```

L2 不能通过“Operation registry”认识高层 domain vocabulary。

---

## 4. Active L2 已有基础

当前 active L2 已存在：

```text
PortSender
    OperationPort -> stdexec Sender adapter

TimerQueue / TimerClient / TimerSender
    bounded
    cancellable
    owner-thread backed
    stop-token aware

process/asset_loading
    ReadAssetImage + AssetReadPort
    loadAsset<T>() typed Sender
    stop/error/decode propagation
```

这证明项目已经选择 Sender/Receiver 作为异步 vocabulary，并已有正确的 stop callback 样例。

当前缺的不是新的异步概念，而是能够承载普通 CPU work 和 owner-thread continuation 的 process-wide execution substrate。

---

## 5. L2 P0 基建：必须在 Wave B 完成

Wave B 只允许新增以下四个核心能力：

```text
ExecutionRuntime
CpuScheduler
MainScheduler
TaskScope
```

已有 `Timer` 与 `PortSender` 保留并整合到同一 execution package 语义中。

### 5.1 `ExecutionRuntime`

`ExecutionRuntime` 是 process-lifetime execution owner。

建议 public contract：

```cpp
enum class EExecutionError : std::uint8_t
{
    INVALID_ARGUMENT,
    CAPACITY_EXCEEDED,
    STOPPING,
    ALLOCATION_FAILURE,
    WORKER_CREATION_FAILURE,
    BACKEND_FAILURE,
    WRONG_THREAD,
    ALREADY_JOINED
};

struct ExecutionRuntimeConfig final
{
    std::size_t cpu_concurrency{};
    std::size_t cpu_queue_capacity{};
    std::size_t main_queue_capacity{};
    TimerQueueConfig timer{};
};

class ExecutionRuntime final
{
public:
    using CreateResult = expected<ExecutionRuntime, EExecutionError>;

    [[nodiscard]] static CreateResult create(ExecutionRuntimeConfig) noexcept;

    [[nodiscard]] CpuScheduler cpu() noexcept;
    [[nodiscard]] MainScheduler main() noexcept;
    [[nodiscard]] TimerClient timer() noexcept;

    std::size_t drainMain(std::size_t budget = static_cast<std::size_t>(-1));

    void requestStop() noexcept;
    [[nodiscard]] expected<void, EExecutionError> join() noexcept;
};
```

具体 backend 可使用 oneTBB、stdexec pool 或自有 bounded worker pool；public API 不暴露 backend 类型。

### 5.2 Runtime 状态机

至少需要：

```text
ACTIVE
  │ requestStop
  ▼
STOPPING
  │ join success
  ▼
JOINED
```

规则：

- `ACTIVE` 接受新的 CPU/Main/Timer submission；
- `STOPPING` 拒绝新的 admission；
- 已运行 operation 按其自己的 stop contract 收敛；
- `join()` 只在 application composition 指定的 owner thread/合法 thread 调用；
- `JOINED` 后所有 scheduler/client handle fail closed；
- Runtime destructor 不能偷偷允许 detached operation 越过其生命周期。

### 5.3 Bounded admission

CPU queue 与 Main queue MUST bounded。

原因：

```text
UI rapid edits
filewatch burst
asset batch import
```

都可能产生任务风暴。L2 负责“系统不能无限吃内存”，但不负责 domain coalescing。

例如 Material latest-revision coalescing 属于 L4/L5；L2 只返回 capacity/rejection。

---

## 6. `CpuScheduler`

`CpuScheduler` 是 copyable capability handle，满足 stdexec scheduler semantics。

期望使用：

```cpp
auto sender =
    stdexec::schedule(context.execution().cpu())
    | stdexec::then([] { /* CPU work */ });
```

硬约束：

- scheduler handle 不拥有 worker lifetime；Runtime owns workers；
- submission bounded；
- Runtime STOPPING/JOINED 后 fail closed；
- work item 之间可真正并发；
- 不通过一个全局 mutex 把所有 task 串行化；
- L2 不解释 work payload。

Compiler 并发目标：

```text
CPU0 -> Material compile A
CPU1 -> Material compile B
CPU2 -> FlowForge compile C
CPU3 -> asset cook D
```

---

## 7. `MainScheduler`

后台任务结束后，不能直接从 worker 修改 thread-affine Editor/UI state。

`MainScheduler` 是 copyable mailbox scheduler：

```text
worker thread
    -> enqueue continuation

application owner/main loop
    -> execution.drainMain(budget)
    -> run continuation on owner/main thread
```

期望组合：

```cpp
auto work =
    stdexec::starts_on(cpu, domain_compile_sender)
    | stdexec::continues_on(main)
    | stdexec::then(apply_result);
```

或等价 sender composition。

硬约束：

- MainScheduler 不创建第二个 main thread；
- completion queue bounded；
- `drainMain()` 的 owner thread requirement 必须测试；
- main continuation 不能隐式执行在 submitter worker 上；
- 不与 LuxObject `ObjectDispatcher` 强行合并。二者可未来共享底层 mailbox mechanism，但 v1 保持不同语义 surface。

---

## 8. `TaskScope`：structured operation lifetime

如果只有 Scheduler，没有 Scope，就会出现：

```text
谁保存 operation state？
谁请求 stop？
窗口销毁后 task 是否还活着？
Context shutdown 如何知道全部 task 已结束？
```

因此 `TaskScope` 是 P0，而不是 “if needed”。

建议 contract：

```cpp
enum class ETaskStartError : std::uint8_t
{
    STOPPING,
    ALLOCATION_FAILURE,
    CAPACITY_EXCEEDED
};

class TaskScope final
{
public:
    TaskScope();
    ~TaskScope();

    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;

    template<stdexec::sender Sender>
    expected<void, ETaskStartError> start(Sender&& sender);

    void requestStop() noexcept;

    [[nodiscard]] TaskScopeCloseSender close() noexcept;
};
```

public implementation 可以内部使用 stdexec/exec 的 async scope，如果版本能力满足；也可以自有 type-erased operation ownership。无论 backend 如何，语义必须一致。

### 8.1 Scope 状态

```text
OPEN
 │ requestStop/close
 ▼
STOPPING
 │ all owned operations terminal
 ▼
CLOSED
```

### 8.2 v1 ownership

```text
EditorApplication
    └─ owns root TaskScope
         ↑
EditorContext only references it
```

首轮所有 Editor long-running background task 都交给这个 root scope。

关闭单个 `MaterialEditor`：

```text
destroy window local state
root TaskScope unchanged
compile operation may continue
```

关闭 EditorApplication：

```text
stop admitting new Editor work
root TaskScope.requestStop()
await root TaskScope.close()
destroy/reset EditorContext and closed TaskScope owner
then destroy dependent Toolset/VFS/runtime capabilities in the approved order
```

未来 Project/Document scope 只能在真实 project/document lifetime contract 冻结后再增加；v1 不预造 Scope tree framework。

### 8.3 Cancellation vocabulary

使用 stdexec stop token/source/callback，不新增：

```text
CancellationManager
CancellationContext
LuxCancellationToken
```

Domain sender 从 receiver environment 获取 stop token，并在合理阶段边界 cooperative check。

### 8.4 Re-entrant-safe admission / eager start contract

This requirement is added by the current implementation review and is P0 for R0 requalification。`async_scope::spawn`/equivalent eager start may execute Sender body/completion synchronously on the caller before returning；therefore TaskScope MUST assume user/Sender callbacks can re-enter the same scope。

MUST：

```text
start() marks/reserves admission while protected by its state mutex
start() releases its own state/admission mutex before eager spawn/start can execute
close/requestStop first prevents new admission
close accounts for all admitted-but-not-yet-registered starts
successful close means both:
    no start reservation remains
    no owned operation remains
```

MUST NOT：

```text
hold TaskScope state mutex while calling eager scope.spawn/start
hold TaskScope state mutex while invoking a stop path that may synchronously call callbacks
fix the issue by simply unlocking before spawn without an admission reservation
allow close to observe empty and finish while an admitted starter has not yet registered its operation
```

A valid implementation pattern is a two-phase admission reservation：

```text
1. lock -> verify OPEN -> increment starting/admission reservation
2. unlock -> connect/start/spawn eager operation
3. lock -> decrement reservation on every success/failure path, publish start outcome -> signal close waiter
4. if stop/close raced after admission, the admitted operation may finish registration but MUST immediately participate in the stopped scope and cannot escape ownership
5. close sets STOPPING first, then waits for reservation count to reach zero and async scope to become empty
6. transition CLOSED only after both conditions hold
```

The exact synchronization primitive may differ, but the semantics above are normative。

Required regression tests：

```text
inline sender callback re-enters requestStop() without deadlock
inline sender callback attempts nested start() without deadlock
start-vs-close race
start-vs-requestStop race
close cannot complete before an admitted starter is registered/resolved
no callback/user code runs under TaskScope's own admission/state mutex
```

### 8.5 TaskScope is lifetime ownership, not a result channel

`TaskScope` exists to own operation lifetime/stop/close。Its implementation may need to normalize terminal channels internally so an async scope can own arbitrary Senders；therefore callers MUST NOT assume `TaskScope::start()` itself preserves or exposes a domain result/error channel。

Before handing an operation to the root scope, the feature/domain pipeline MUST explicitly consume/materialize/apply the result it cares about, or attach a terminal observer/continuation whose lifetime is part of the scoped operation。

Correct conceptual pattern：

```text
domain Sender
  -> observe/map domain result + execution failure
  -> MainScheduler stable apply/discard by identity/revision
  -> terminal lifetime-only Sender
  -> TaskScope.start()
```

MUST NOT silently discard a compile/import/read failure merely because the scope needs a terminally-compatible Sender shape。

---

## 9. Existing Timer 与 debounce

`TimerSender` 已有 bounded/cancellable 语义，应作为 L2 时间 primitive 继续使用。

FileWatch debounce、stable-write window 等可以由 L5/L4 组合：

```text
raw file event
  -> domain coalescing state
  -> TimerSender
  -> semantic action
```

L2 Timer 不认识文件/资产。

---

## 10. Compiler 的“无状态”精确定义

这里的无状态不是“class 没有 member”，而是：

> 同一个 Compiler instance 的可观察语义不依赖前一次 invocation 的 mutable residue；多个 invocation 可以同时执行且互不修改彼此状态。

允许保存：

```text
immutable environment
immutable target configuration
resolved toolchain path
read-only include roots
copyable CpuScheduler handle
shared_ptr<const SharedConfig>
```

禁止保存：

```text
current source
current IR
current diagnostics
current temporary directory
current output
current receiver
current stop source
in-flight task list
bool compiling
mutable request cache without independent synchronization contract
```

因此最好使 compile facade 为 `const`：

```cpp
compiler.compile(snapshot, options) const;
```

---

## 11. Source Snapshot：异步正确性的核心

同步 core 可以借用 const source：

```cpp
compileMaterial(const MaterialGraph& graph, ...);
```

但异步 API MUST own source snapshot：

```cpp
auto sender = compiler.compile(material_graph.clone(), options);
```

禁止：

```cpp
compiler.compileAsync(material_graph); // stores const MaterialGraph* into worker
```

原因：UI 可在下一帧继续编辑 graph；借用将导致 data race / torn semantic snapshot。

最终 Shared Graph Source 可以提供更廉价的 snapshot/freeze，但在此之前 deep clone/move-owned snapshot 是 canonical safety contract。

---

## 12. Per-invocation state 属于 Sender Operation State

概念模型：

```text
MaterialGraphCompiler (long-lived)
      │
      │ compile(snapshot)
      ▼
Material compile Sender
      │ connect(receiver)
      ▼
OperationState
├─ MaterialGraph snapshot
├─ options
├─ immutable environment ref
├─ stop callback
├─ lowered MaterialIR
├─ ShaderIR / generated GLSL
├─ shaderc::Compiler / CompileOptions
├─ SPIR-V/reflection temporaries
└─ receiver
```

FlowForge：

```text
OperationState
├─ FlowGraph snapshot
├─ compile options
├─ MLIRContext (per invocation)
├─ IR / LLVM module
├─ temporary directory
├─ object bytes
├─ linker/process state (future split)
└─ receiver
```

当前 Material shaderc compiler/options 本来就是每次调用局部构造；FlowForge `IRContext::create()` 也是 per-call，这些应保持，而不是上提成共享 Compiler member。

---

## 13. 同步 deterministic core 必须保留

Async Sender 不能形成第二套 lowering/compiler pipeline。

Material：

```text
compileMaterial(snapshot, environment)
    = canonical synchronous semantic core

MaterialGraphCompiler::compile(snapshot)
    = scheduling/lifetime wrapper around canonical core
```

FlowForge 同理。

直接同步 core 的消费者：

```text
unit tests
CLI Toolchain
Cooker
qualification harness
```

异步消费者：

```text
Editor
batch orchestration
future background build UX
```

---

## 14. Compiler Environment vs Request Options

把“安装/工具链配置”和“一次 source semantic input”分开。

Material：

```cpp
struct MaterialCompilerEnvironment final
{
    std::vector<std::filesystem::path> shader_include_paths;
    ShaderTarget target;
};

struct MaterialCompileOptions final
{
    // only invocation-specific semantic/codegen choices
};
```

这也用于修复编译进 library 的绝对 source/build include path：include roots 必须来自 relocatable runtime/compiler environment，而不是 compile-time absolute macro。

FlowForge：

```cpp
struct FlowForgeCompilerEnvironment final
{
    std::filesystem::path linker;
    TargetDescription target;
};

struct FlowForgeCompileOptions final
{
    std::string module_name;
};
```

`linker`、SDK/sysroot、target toolchain path 是 environment；module name 是 invocation input。

---

## 15. Domain Result 与 Execution Error 分离

Material compile failure：

```text
INVALID_GRAPH
TYPE_MISMATCH
SHADER_COMPILATION_FAILURE
...
```

是正常 domain outcome，不应该伪装成 scheduler/runtime failure。

推荐 Sender semantic：

```text
set_value(expected<MaterialDescription, MaterialCompileFailure>)
set_stopped()            # cooperative cancellation
set_error(EExecution...) # only if execution infrastructure truly fails and chosen sender shape exposes it
```

或者将 execution admission error 在 `TaskScope::start` / sender start 阶段 fail closed。

关键不是具体 stdexec channel 选型，而是 MUST NOT 把“代码编译失败”与“executor 已停止/queue 满”合并成同一个 enum。

---

## 16. Cancellation

禁止：

```cpp
compiler.cancel();
```

因为并发时无法定义取消哪次 invocation。

正确：

```text
Task A -> stop token A
Task B -> stop token B
```

Compiler core 在阶段边界 cooperative check：

Material：

```text
validate
check stop
lower
check stop
emit
check stop
shaderc
check stop
reflect/assemble
```

FlowForge：

```text
validate
check stop
create context/lower
check stop
passes/codegen
check stop
link
check stop
read/finalize
```

第三方同步函数内部若不可抢占，不要求 unsafe thread kill；只要求进入前/返回后及时响应 stop。

---

## 17. Revision / stale result

异步结果不能因为“完成了”就直接覆盖当前文档。

L5 request 必须带 stable target identity + revision：

```text
DocumentId
SourceRevision
```

完成后 main-thread apply：

```text
lookup target by stable id
if missing -> discard
if current revision != result revision -> discard as stale
else apply diagnostics/artifact/preview
```

Sender operation 不持有 `MaterialEditor*`、`FlowForgeEditor*`。

---

## 18. Cache

v1 Compiler MUST NOT 内置 mutable ad-hoc cache。

若未来 profiling 证明需要：

```text
MaterialCompileCache
FlowForgeCompileCache
```

应成为独立、明确 thread-safe、content-addressed facility；Compiler 可以引用它，但 cache 有自己的一致性/并发 contract。

禁止简单在 Compiler 中加入：

```cpp
mutable std::unordered_map<...> cache_;
std::mutex cache_mutex_;
```

然后把生命周期/一致性问题隐藏起来。

---

## 19. 不把每个 compiler stage Sender 化

v1 一个 Material compile 是一个 domain operation boundary：

```text
validate -> lower -> emit -> shaderc -> reflect -> assemble
```

内部保持同步函数。

禁止无真实并发边界时拆成：

```text
ValidateSender
LowerSender
EmitSender
SpirvSender
ReflectionSender
```

Sender 应包装 time-spanning operation boundary，而不是替代普通函数调用。

---

## 20. Product VFS-backed AssetRead endpoint

`loadAsset<T>()` 已经定义 typed workflow，但最终 product 仍需要 concrete `AssetReadPort` endpoint。

MUST：

```text
AssetRead submit
    -> validate/admit
    -> execute potentially blocking provider/VFS open off owner thread
    -> return AssetBlob
    -> typed loadAsset<T>() decode
    -> completion
```

MUST NOT 在 game/main/UI thread inline 调用可能触盘的 `AssetVfsView::open()`。当 provider 只提供同步 open 时，production path 需要 `BlockingScheduler` 或 native async file backend；因此 Blocking/IO isolation 是 product-runtime async asset gate，而不是 Material compiler Wave B 的前置。

---

## 21. BlockingScheduler：不属于 Wave B，但属于 product-runtime V2 gate

Wave B core 仍只实现 CPU/Main/TaskScope。However，最终 product 的 Pak/provider `open()` 如果是同步阻塞 API，就必须在暴露 script/runtime async asset loading 前提供 blocking/IO isolation。

推荐 generic capability：

```cpp
BlockingScheduler blocking();
```

职责：

```text
blocking asset/provider IO
wait external process
blocking compatibility SDK invocation
```

不负责 CPU-heavy lowering/optimization。

规则：

- Wave B MUST NOT 顺手实现它；
- Wave V2（production AssetRead seam）MUST implement `BlockingScheduler` **或**证明使用了等价 native async file backend；
- 一旦 BlockingScheduler 已由 V2 存在，FlowForge/Toolchain MUST reuse it，不再等待“第二次 starvation review”；
- 如果 product provider 全部是可证明 non-blocking/native-async，V2 可以不暴露 BlockingScheduler，但必须用测试证明 owner thread 不会执行阻塞 read。

---

## 22. P1：ProcessSender

未来建议窄 OS process primitive：

```cpp
struct ProcessRequest
{
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
};

struct ProcessResult
{
    int exit_code{};
    std::string stdout_text;
    std::string stderr_text;
};

auto runProcess(ProcessRequest) -> ProcessSender;
```

它属于 L2，因为只表达 time-spanning subprocess execution，不知道 `lld-link`/FlowForge。

但 Wave B MUST NOT 实现它。

---

## 23. Toolset、TaskScope 与窗口生命周期

```text
EditorApplication
├─ Toolset
│    ├─ MaterialGraphCompiler
│    └─ FlowForgeCompiler
└─ root TaskScope

EditorContext
└─ references both capabilities
```

Compiler 与 task lifetime 解耦：

```text
Tool lifetime        = EditorApplication Toolset lifetime
Operation lifetime   = root TaskScope owned interval
Window lifetime      = arbitrary shorter UI lifetime
```

关闭窗口不销毁 Compiler、不取消 root TaskScope。Toolset/compiler MUST NOT become the owner of in-flight operation state merely because it created the Sender。

---

## 24. FileWatch / Asset pipeline

FileMonitor 可以产生 L4 import/cook Sender，但 L2 不解释 `FileEvent -> Asset` mapping。

```text
Platform FileWatcher
  -> L5 semantic FileMonitor
  -> Toolset importer/cooker
  -> domain Sender
  -> EditorApplication root TaskScope exposed via Context
  -> CpuScheduler
  -> MainScheduler
  -> stable asset/project generation re-check
```

---

## 25. Runtime shutdown sequence

必须使用显式顺序：

```text
EditorApplication stops new work admission
  ↓
root TaskScope.requestStop()
  ↓
await root TaskScope.close()
  ↓
destroy/reset EditorContext + closed TaskScope owner
  ↓
requestStop/destroy Toolset and other Editor control state
  ↓
close/join AssetRead endpoint; then destroy VFS/providers when safe
  ↓
ExecutionRuntime.requestStop()
  ↓
drain required MainScheduler completions
  ↓
ExecutionRuntime.join()
```

禁止 Runtime/VFS/provider 先析构、Sender/Scope 后收尾；也禁止让 `EditorContext` 存活并引用已经销毁的 Toolset/Selection/UISession。

---

## 26. Wave B 测试矩阵

### ExecutionRuntime

- create with valid/invalid config；
- CPU concurrency > 1 实际并发；
- bounded CPU queue overflow/rejection；
- bounded main queue behavior；
- requestStop rejects new work；
- join exactly once / wrong-thread behavior；
- scheduler handle after runtime stop fails closed；
- no active work after successful join。

### MainScheduler

- worker continuation only runs after `drainMain()`；
- runs on owner thread；
- budgeted drain；
- stop race；
- capacity race。

### TaskScope

- owns operation state until terminal completion；
- close waits all operations；
- requestStop propagates stop token；
- start after stopping fails；
- task completing concurrently with close is race-safe；
- scope destructor does not leak/detach work；
- synchronous/eager sender re-entry into requestStop/nested start does not deadlock；
- close waits admitted-but-not-yet-registered starters；
- no user/stop callback executes while TaskScope holds its own admission/state mutex。

### Compiler integration qualification（不属于 Wave B implementation，但必须在 H/I 前验证）

Material：

- N parallel compiles on one Compiler instance；
- independent results；
- TSAN/race instrumentation if available；
- mutable source continues editing while snapshot compiles；
- stale revision discarded；
- window destruction does not cancel EditorApplication root-TaskScope-owned task。

FlowForge：

- per-invocation MLIRContext；
- parallel temporary paths unique；
- no shared mutable IR；
- linker blocking does not corrupt another compile；
- if Wave V2 provides BlockingScheduler, linker/file blocking stages reuse it；otherwise no private blocking pool is created。

---

## 27. 明确禁止项

```text
NO JobSystem / JobManager / JobGraph vocabulary parallel to Sender
NO legacy AsyncRuntime wholesale restore
NO domain operation registry in L2
NO Material/FlowForge names in process/execution
NO std::thread/std::async inside Editor windows
NO global compiler mutex to fake thread safety
NO mutable current-job state in Compiler
NO borrowed mutable graph in async operation
NO compiler.cancel() ambiguous API
NO detached fire-and-forget operation outside TaskScope
NO BlockingScheduler/ProcessSender during Wave B
NO per-stage Sender explosion
NO mutable compile cache in v1 Compiler
```

---

## 28. v3 完成判据

L2 foundation 只有在以下条件全部成立时才可视为 ready：

```text
ExecutionRuntime owns and joins CPU workers
CpuScheduler provides bounded real concurrency
MainScheduler provides bounded owner-thread continuation
TaskScope provides structured task ownership, stop and re-entrant-safe admission
Timer/PortSender remain compatible
L2 contains zero Material/FlowForge/Editor vocabulary
one immutable Compiler instance can safely serve concurrent owned snapshots
window lifetime is independent from operation lifetime
shutdown has no detached work
```

---

> Coding implementation MUST also comply with `08-normative-execution-contract.md`.
## 29. Runtime scripting：Sender 与 continuation 的边界

L2 Sender/OperationState 不等于 script coroutine。

```text
Sender/domain async operation
    = time-spanning external work protocol

FlowForge/scripting continuation
    = suspend/resume language/runtime protocol
```

Delay 使用 `TimerSender`；asset load 使用 `AssetReadPort + loadAsset<T>()`；GPU/physics query 使用对应 domain 提供的 async Sender/port。脚本运行遇到 async node 时必须保存 continuation/state 并返回 Simulation，不得 `sync_wait`、sleep 或 GPU wait。completion 只能通过明确的 script-resume queue/stable point 继续执行。

精确语言/runtime contract 见 `09-product-runtime-vfs-and-async-script.md`。

