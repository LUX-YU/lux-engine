# Process Performance / Correctness Gates

## 1. General law

任何单 key lookup/mutation：

```text
expected/amortized O(1)
```

Enumeration：

```text
O(actual output K)
```

Building N actual objects：

```text
O(N)
```

禁止：

- unrelated global scan；
- global sort for local mutation；
- erase-remove unrelated elements；
- per-frame world resync；
- callback registry linear lookup；
- unbounded queue；
- hidden per-request `std::function` allocation。

---

## 2. Timer

Required:

```text
schedule/cancel/fire      <= O(log active timers)
capacity                  bounded
queued node               operation state pointer
```

Benchmark sizes：

```text
10K
100K
1M timers
```

Measure：

- schedule ns/op；
- cancel ns/op；
- fire ns/op；
- allocations；
- wakeups；
- p99 lateness。

No callback registry.

---

## 3. File ingress

Admission：

```text
O(1)
bounded
preallocated ring
```

Required benchmark:

```text
1K / 100K queued tiny requests
multi producer
queue full rejection
shutdown with active reads
```

Steady admission internal allocations：

```text
0
```

Payload `FileBytes` allocation is output data and is accounted separately.

---

## 4. Render sender conversion

The new path must improve or preserve:

```text
producer submit O(1)
request lookup O(1) or absent
completion O(1)
```

Explicit target:

```text
delete shared_ptr<PreparedUpload> per request
delete request-id -> callback map where operation-state receiver can replace it
```

Do not regress:

- byte budget；
- bounded queue；
- source ownership pinning；
- queue-family ownership；
- timeline completion；
- stable resource handle semantics。

---

## 5. Scope policy

Standard `spawn` may allocate operation state.

Therefore:

- allowed for coarse cross-frame workflow；
- forbidden as per-entity/per-script/per-event primitive；
- benchmark real spawn frequency；
- use bulk/static DAG for frame-local fine grain work。

---

## 6. Error policy

No active production `throw` for semantic errors.

Sender primitives declare typed error channel.

All completion callbacks:

```text
noexcept
exactly once
```

Shutdown must not destroy an operation state while backend still references it.

---

## 7. Cancellation

For each Sender document:

```text
pre-admission stop
queued stop
running stop
foreign/backend non-preemptible region
completion race
```

File blocking fallback explicitly allows:

```text
cancel cannot preempt an already-running OS blocking read
```

but completion must become `set_stopped` after the read unwinds if cancellation won.

---

## 8. Architecture source gates

Fail build if Process source introduces:

```text
AsyncRuntime
AsyncRuntimeBuilder
AsyncOperationContext
OperationCatalog
ProcessManager
ProcessContext
ProcessServices
TBB type in public headers
asio type in public headers
asset/model/material/texture type in process/execution
render/vulkan include in process
legacy include
```

Do not ban standard/stdexec execution vocabulary.
