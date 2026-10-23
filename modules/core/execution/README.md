# Shared execution and explicit completion

Source and installed consumers require CMake 3.25 or newer, as does the pinned stdexec package.
Execution imports the complete stdexec export set with `find_package(stdexec CONFIG REQUIRED GLOBAL)`.
Render uses the same package and visibility contract. No replacement targets are created; sibling modules
can import the package in either order without relying on an application-level preload.
The pinned package uses the [stdexec overlay](../../../cmake/vcpkg-ports/stdexec/README.md) to support
`exec::task` with a parent `std::stop_token`; Engine does not introduce a token wrapper to hide that defect.

`RunLoop` is a nonblocking scheduling destination, not a worker thread. Its driving thread calls
`executeReady(limit)` at a safe point. The application supplies a borrowed wakeup callback;
Core knows nothing about IPC, graphics or request results. Producers use `scheduler()` through
standard Sender composition. The private moodycamel queue borrows nodes embedded in connected
schedule operations; it never owns their lifetime or allocates a separate result record.
Publication and consumption use no RunLoop mutex. Queue storage and producer registration may allocate;
ordinary heap exhaustion is fatal rather than dropping an accepted operation's completion.

```cpp
execution::RunLoop loop{notifyApplication, application};
auto work = stdexec::schedule(cpu)
    | stdexec::then(prepare)
    | stdexec::continues_on(loop.scheduler())
    | stdexec::then(adopt);
// Connect/start or spawn in the application's existing counting_scope.
loop.executeReady(128);
```

Nodes are removed in batches before execution. Terminal delivery may destroy the connected
operation; the loop never touches that node afterwards. One thread executes ready work, while
producers may schedule concurrently. Ordering is per producer, not a total order across threads.
`size()` counts publishing, queued and executing operations; it is not a readiness predicate.

RunLoop scheduling guarantees delivery even when the receiver has requested stop. Actual
work decides whether cancellation is still possible. This preserves errors and committed
results rather than converting them to stopped during the final handoff.

Shutdown must keep executing ready work until accepted scopes finish, then join external
producers and wait for `activePublishers() == 0`. An empty queue alone does not establish
that wakeup callbacks have returned. The loop, application callback and their code modules must
outlive those tails; the loop does not own the producers. For a dynamically loaded task continuation,
the task environment supplies its existing code pin. RunLoop moves that pin off the node before invoking it
and releases it only after control returns to Core. This prevents unloading the callback's DLL during
stdexec's terminal state destruction. It adds no operation owner or per-notification heap allocation;
unpinned operations carry an empty shared_ptr. Producer callback tails still require their own lifetime proof.

## Shared CPU pool

ExecutionRuntime owns one pinned `exec::static_thread_pool`; `cpu()` returns that library scheduler directly.
CPU scheduling has no Lux queue-capacity or weak lifetime wrapper. The scheduler is a borrow, not an ownership
lease: finish all external Render, Scene and asset operations before joining/destroying ExecutionRuntime.
`requestStop()` stops accepted business scopes and timers; the pool remains available for their cleanup.
`join()` destroys the pool only after its own tasks and completion work have settled. Applications must also
join their external producers. Pool destruction joins worker callback tails; a scope join alone does not.

TaskExecutor uses the same scheduler for worker-affine DAG nodes. BlockingScheduler retains its separate bounded
IO queue and threads because blocking IO must not occupy all CPU workers. Timer and Object/CompletionWork contracts
are unchanged. stdexec's pool may use locks when idle; only the Lux RunLoop publication path is lock-free.

## Finite tasks

`TaskScope::submit(options, factory)` admits one finite sender pipeline. The factory receives a borrowed
TaskReporter for identity, progress and its real cancellation token. The pipeline chooses its own scheduling
and consumes business values; its terminal value is void, expected<void, E>, or a terminal ETaskState.
A typed error or stopped signal records failure/cancellation. An exception escaping a sender is fatal.

ExecutionRuntime owns one `stdexec::simple_counting_scope` for these connected operations. TaskRecord stores
monitoring and cancellation metadata, never an operation pointer or deleter. The existing per-task cancellation
sources are authoritative; there is no additional scope stop source. Project TaskScope destruction requests
stop without joining or destroying accepted operations.

The outer spawn allocator uses ordinary allocation and reports destruction back to the admitted metadata.
It is not a pool and owns no operation. Nested sender allocations use their ordinary allocator. One submission
reference protects inline completion; the other ends after stdexec destroys its state. This returns admission
capacity independently of Main dispatch, while retaining code/input captures through their destruction.
External producers must still retain their callback code and finish their tails before Runtime shutdown.

ExecutionRuntime::submit returns a nonblocking Task cancellation handle. Destroying the handle requests stop;
it neither suppresses an already determined result nor waits/pumps Main. The Runtime must outlive its handles.
Callers retain captured input through actual pipeline completion, not merely through the handle's lifetime.

`ExecutionRuntime::spawn(sender)` accepts finite internal work without a TaskRecord, TaskId, history or
per-operation cancellation source. Consume values and normalize failures in the sender first. It uses the
same stdexec scope as monitored tasks; admission, synchronous start and operation destruction are covered
by Runtime shutdown. Frame submission uses this path and cannot exhaust the business task directory.
`TaskScope::stopToken()` supplies group cancellation to child IO without manufacturing another monitored
task. The enclosing pipeline or Runtime, rather than TaskScope monitoring, proves those reads have ended.

Before blocking a driver, capture both its notification epoch and `RunLoop::publicationEpoch()` **before**
consuming work and checking termination. Block only when `canWait(publication)` holds and the notification
epoch is unchanged. A wake callback may still be returning after its last queued node was consumed.
Main and Asset drivers use this same publication contract, with their own actual wait facilities.

## Transport and business adoption

A VFS read can complete on an IO worker and immediately schedule CPU decoding. Only code that changes
Main-thread state uses `continues_on(execution.mainScheduler())`; object-affine adoption may instead use the
Engine ObjectScheduler. Do not schedule through both unless they perform genuinely different actions.
`taskResult(sender)` materializes existing typed expected/error/stopped channels for a caller that must inspect
them. It is standard composition, without a queue or heap result envelope. Preserve the actual terminal state
with `taskState(result)` when consuming the value, rather than marking a handled failure as success.

`dispatchMainCompletions()` runs a bounded batch of ready continuations. `dispatchTaskEvents()` only notifies
monitoring; it never invokes business adoption. `settled()` observes monitored/unmonitored work and Main quiescence,
not external producer callback tails. CompletionWork shares this same RunLoop with a cold-allocated,
coalesced notification state and reusable schedule operation. It does not allocate on every request.
`hasReadyWork()` is only an event-loop wake predicate: a worker may still be preparing a continuation when
it returns false. Waiting for completion uses the actual result or `settled()`, never an empty-queue test.

Waits use the wake epoch and a coalesced semaphore; enqueueing a Main continuation takes no Runtime queue mutex.
Task metadata/cancellation still use their existing mutex. This is not a claim that all scheduler or library
internals are lock-free. Explicit shutdown continues Main dispatch until admitted pipelines settle, joins
stdexec's scope, and only then joins CPU/IO workers. Never wait inside a Main callback on work that requires
that same callback stack to return.
