# Task DAG execution

TaskGraphBuilder compiles resource hazards and explicit dependencies into an immutable graph with root and
successor CSR indexes. It retains callable code pins until after callable destruction. It owns no scheduler.

TaskExecutor borrows `ExecutionRuntime::cpu()` and reuses raw node workspace. It creates no threads, ready queues
or scopes. `schedule(graph)` is one Sender whose operation constructs and destroys its connected child schedule
operations. Each completion releases only its own successors. The final predecessor publishes a ready node with
acquire/release visibility; no per-level graph barrier is introduced.

```cpp
task::TaskExecutor executor{execution.cpu(), 2048};
auto result = executor.execute(graph);
```

`execute` uses stdexec's local sync_wait run_loop for CALLER_THREAD nodes. WORKER nodes always use the borrowed
CPU pool. This wait does not dispatch UI, ObjectRuntime or arbitrary Engine completion work. Async consumers of
`schedule` must provide an infallible caller-thread scheduler and keep it progressing until completion. An
ObjectScheduler whose target can disappear is not such a scheduler. The final graph completion may run on a CPU
worker; an async consumer must explicitly schedule its application-state adoption on the application thread.

Stop before start prevents execution. Once accepted, the finite DAG completes, including caller-affine cleanup.
Stopping halfway would misrepresent ECS writes already performed. One graph at a time uses each executor;
ALREADY_EXECUTING is returned before any callable runs. Simulation restores its candidate time on that rejection.
Ordinary allocation failure is fatal, not a recoverable graph failure.

`minimumTaskCapacity()` reports a nondecreasing sizing hint, not allocated storage or a business limit.
`setMinimumTaskCapacity()` raises that hint only while idle; it does not allocate. Actual bytes depend on the
connected caller scheduler's operation size. The first execution allocates; same-size subsequent executions reuse storage. All nodes are
constructed before root publication. A startup reference and completion references protect synchronous reentry
and predecessor tails; workspace is released before the final Receiver, which may start another graph.

Graph, executor, CPU pool and code modules must outlive accepted work. Do not move an executor while a Sender or
connected operation borrows it. ExecutionRuntime is joined only after all external graph/Render/asset work is
quiescent. Its pool destructor joins producer callback tails. Scope completion
alone is not a substitute for that join.
