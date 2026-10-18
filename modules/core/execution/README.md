# Owner-driven execution

`RunLoop` is a nonblocking scheduling destination, not a worker thread. Its owner calls
`executeReady(budget)` at a safe point. The host supplies a borrowed wakeup callback;
Core knows nothing about IPC, Vulkan or request results. `post()` accepts a stable
intrusive work node and publishes it into the same queue used by `scheduler()`.
The queue allocates no nodes and invokes no callbacks under its short publication lock.

```cpp
execution::RunLoop loop{notifyHost, host};
auto work = stdexec::schedule(cpu)
    | stdexec::then(prepare)
    | stdexec::continues_on(loop.scheduler())
    | stdexec::then(adopt);
// Connect/start or spawn in the host's existing counting_scope.
loop.executeReady(128);
```

Work is removed before execution. A callback may destroy its node or repost it; the
loop never touches that node afterwards. One owner executes ready work, while producers
may post concurrently. A node cannot be posted again while it is still queued.

Owner scheduling guarantees delivery even when the receiver has requested stop. Actual
work decides whether cancellation is still possible. This preserves errors and committed
results rather than converting them to stopped during the final handoff.

Shutdown must keep executing ready work until accepted scopes finish, then join external
producers and wait for `activePublishers() == 0`. An empty queue alone does not establish
that wakeup callbacks have returned. The loop, host callback and their code modules must
outlive those tails; the loop does not own any of them.
