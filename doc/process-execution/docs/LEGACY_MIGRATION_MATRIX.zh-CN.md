# Legacy migration matrix

原则：

> Legacy 是 behavior/test reference，不是等待搬家的源码树。

| Legacy concept | Decision | New ownership |
|---|---|---|
| `AsyncRuntime` | DELETE public concept | no replacement |
| Asio single coordinator | REUSE only if a concrete primitive needs it | private backend only |
| `AsyncRuntimeBuilder` | DELETE | object composition / sender factories |
| `AsyncRuntimePlan` | DELETE | none |
| operation registry | DELETE from Process core | domain-specific ownership if ever needed |
| prerequisites catalog | DELETE | constructor/capability composition |
| `AsyncOperationBundle` | DELETE until proven need | none |
| `AsyncClient` | DELETE | concrete capability handles |
| `AsyncOperationContext` | DELETE | explicit captures/dependencies |
| `AsyncCompletion<T>` | DELETE high-level model | receiver completion |
| `OperationPort<T>` | KEEP | L0 typed cross-layer capability |
| `AsyncExecuteSender<T>` | REWRITE | `portSender()` adapter |
| `AsyncScope` wrapper | DELETE | standard/stdexec async scope |
| coordinator scheduler wrapper | DELETE | standard scheduler if concrete owner needs one |
| background TBB pool | DELETE from Process ownership | shared system parallel scheduler |
| blocking static pool | REUSE concept | `process/io` backend only |
| `AsyncFileService` | REWRITE | `FileIo`/`FileClient` senders |
| file native async code | EXTRACT | `process/io` private backend |
| timer state | EXTRACT | TimerQueue |
| main mailbox | MOVE OUT | Host/main scheduler owner |
| `CoordinatorSignal` | DELETE core concept | endpoint-specific ingress/notification |
| giant runtime stats | SPLIT | per primitive diagnostics/bench only |
| structured shutdown tests | PORT intent | Timer/File/Render scope tests |
| module lease behavior | PORT only where plugin ownership exists | owning subsystem |
| AssetLoadService workflow | DO NOT MIGRATE into Process | Asset/Streaming module |
| manual `BatchJoin` | REPLACE later | bounded runtime fanout algorithm |

## Things to copy almost verbatim only after review

- native file open/read mechanics；
- close-race tests；
- producer admission tests；
- cancellation tests；
- queue-budget tests；
- no-work second-build/install tests。

## Things never to copy

```text
engine/runtime
runtime_execution target name
compat headers
compat aliases
old operation registry vocabulary
AssetManager ownership
render upload bridge as Process service
```
