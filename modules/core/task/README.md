# Task DAG execution

TaskGraphBuilder compiles resource hazards and explicit dependencies into an immutable graph with root and
successor CSR indexes. It owns no scheduler or borrowed object lifetime. The graph owner keeps captured objects
and their code alive until execution completes and the graph is destroyed.

TaskExecutor borrows `ExecutionRuntime::cpuScheduler()` and reuses raw node workspace. It creates no threads,
ready queues or scopes. Its only execution entry is `schedule(graph)`: a Sender whose connected operation
constructs and destroys the child scheduling operations. Each node completion releases only its own successors.
The final predecessor publishes a ready node with acquire/release visibility; no per-level graph barrier is added.

```cpp
task::TaskExecutor executor{execution.cpuScheduler()};
auto work = executor.schedule(graph);
```

The consumer supplies the receiver scheduler for CALLER_THREAD nodes. It must enqueue on that thread, be
infallible, and remain available until the accepted graph drains. WORKER nodes use the borrowed CPU pool.
An ObjectScheduler whose target can disappear is not a suitable graph caller scheduler. Graph completion may
run on a CPU worker; asynchronous consumers schedule application-state adoption on the application thread.

Engine normalizes the Sender error/stopped terminals into its result before using `stdexec::sync_wait` at the
scene execution boundary. The resulting local run_loop only executes graph caller nodes; it does not dispatch
arbitrary Object/Editor callbacks that could mutate the scene under traversal.

Stop before start prevents execution. Once accepted, the finite DAG drains; stopping halfway would misrepresent
writes already performed. One graph at a time uses an executor. A concurrent start reports
`ETaskExecutorError::ALREADY_EXECUTING` before any callable runs. Ordinary heap exhaustion is fatal.

Workspace grows when the connected node state needs more bytes or stricter alignment; there is no public
capacity hint or task limit. Same-sized executions reuse memory but still construct their node operation states.
All nodes are constructed before root publication. The startup reference and node completion references cover
synchronous reentry and predecessor completion tails. Storage is released for reuse before the final Receiver,
which may synchronously start another graph.

The executor is neither copyable nor movable. Graph, executor, CPU pool and captured objects outlive accepted
work. ExecutionRuntime is joined only after external graph/Render/asset operations are quiescent. Its pool
destructor joins producer callback tails; scope completion alone does not replace that join.
