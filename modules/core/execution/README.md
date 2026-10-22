# Owner-driven execution

Source and installed consumers require CMake 3.25 or newer, as does the pinned stdexec package.
Execution imports the complete stdexec export set with `find_package(stdexec CONFIG REQUIRED GLOBAL)`.
Render uses the same package and visibility contract. No replacement targets are created; sibling modules
can import the package in either order without relying on an application-level preload.

`RunLoop` is a nonblocking scheduling destination, not a worker thread. Its driving thread calls
`executeReady(limit)` at a safe point. The application supplies a borrowed wakeup callback;
Core knows nothing about IPC, graphics or request results. Producers use `scheduler()` through
standard Sender composition. The private moodycamel queue borrows nodes embedded in connected
schedule operations; it never owns their lifetime or allocates a separate result record.
Publication and consumption use no RunLoop mutex. Queue storage and producer registration may allocate;
ordinary heap exhaustion is fatal rather than dropping an accepted operation's completion.

```cpp
execution::RunLoop loop{notifyHost, host};
auto work = stdexec::schedule(cpu)
    | stdexec::then(prepare)
    | stdexec::continues_on(loop.scheduler())
    | stdexec::then(adopt);
// Connect/start or spawn in the host's existing counting_scope.
loop.executeReady(128);
```

Nodes are removed in batches before execution. Terminal delivery may destroy the connected
operation; the loop never touches that node afterwards. One thread executes ready work, while
producers may schedule concurrently. Ordering is per producer, not a total order across threads.
`size()` counts publishing, queued and executing operations; it is not a readiness predicate.

Owner scheduling guarantees delivery even when the receiver has requested stop. Actual
work decides whether cancellation is still possible. This preserves errors and committed
results rather than converting them to stopped during the final handoff.

Shutdown must keep executing ready work until accepted scopes finish, then join external
producers and wait for `activePublishers() == 0`. An empty queue alone does not establish
that wakeup callbacks have returned. The loop, host callback and their code modules must
outlive those tails; the loop does not own any of them.

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
