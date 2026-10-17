# I4 migration: direct caller-owned messages (implementation in progress)

No qualification is claimed by this record. Failed build logs remain in the external I4 evidence tree.

## Production ownership

The caller owns its sequence and discovered CommandIds. `render_commands` publishes only builtin value
contracts. `RenderClient`, `BoundCommand`, typed completion wrappers, transport budgets and the Hello
protocol have been removed. Protocol declarations remain independent of IPC; the existing server-side
Reply/Handler types moved to CommandHandler.hpp and their separate component.

The existing RenderRequest owns one ReceivedRequest. On completion it owns the actual response or error
message; successful publication destroys the source and transfers only that result. A failed segment
growth keeps both objects in ResultReady. The owner retries publication, never business invocation.
The existing owner collection retains undelivered requests, with no arbitrary total admission quota.
Disconnect is delivery loss, not a promise of rollback. Accepted Native work keeps the source until safe.

True asynchronous promotion allocates in the existing dynamically growing stable slot container. Each
slot supplies its own ready-chain link before work starts. The existing CPU completion channel uses a
short critical section to link/unlink completed slots; its notifier does not run handlers/destructors
inside the lock. Synchronous requests do not allocate an asynchronous slot. The publisher-tail counter
is captured before publishing, so a callback does not subsequently dereference a reclaimed slot.

Native completion adapters depend on neutral CommandFailure plus Core Execution, not the IPC Handler
header. Professional command implementation translation units explicitly include AsyncCommand.
The selected Native bootstrap and remaining registration/module pins are still I5 work.

## Invariant migration

| Former contract | Current evidence target | Status of old contract |
|---|---|---|
| Canonical TypeOps / cross-build ABI acceptance | IPC same-build DLL object moves; cold full Command declaration comparison | SUPERSEDED; mixed ABI unsupported |
| Client SlotMap RequestId and Session checks | Caller uint64 sequence; equal values on distinct physical Links; stale Command generation | SUPERSEDED; new Link requires rediscovery |
| Hello/bind gate | Host obtains actual ListCommands ID before run; ordinary IPC directory request | SUPERSEDED |
| Generic 64 Reply/128 Pending quota | Segmented IPC growth; 129 async requests; 4097 stable ready nodes | SUPERSEDED |
| Original input automatically returned for every terminal | Actual response/error only; caller retains authoritative retry data; explicit response Owner stays valid | SUPERSEDED |
| Generic retained-byte budget | Explicit professional Storage/Transfer limits and true Owner lifetime | SUPERSEDED; Native capacity checks retained |
| Automatic ABORTED on request destruction | Explicit Render terminal, or closed connection and caller unknown outcome | SUPERSEDED |
| Reply growth failure after accepted business work | typed_client test holds one ready result, independent Client progresses, Handler invoked once | Required |
| CPU batch all-or-none, last-wins, revisions and restart snapshot | typed_batch through direct IPC, caller builds each vector outside timed window | Retained |
| Batch implicit vector return/reuse benchmark | Submitted backing observed directly by Handler; real move cost baseline and construction excluded explicitly | SUPERSEDED; not a like-for-like old reuse workload |
| Scene/View/Storage generation and failure rollback | control, camera, storage_registration, mesh/material/instance_operations | Retained |
| Dynamic Handler selection and shared Handler Owner | handlers, feature_registration, feature_bundle | Retained |
| GPU data, partial submit, real Queue saturation, held completion, close/stop | gpu_ready, texture_progress, texture_rendering, shutdown, external_buffer | Retained |
| DLL auto-pin/unload | Host retains module through response/source destruction and callback return | SUPERSEDED; I5 removes remaining propagation |

The test-only Peer is application state: requester, caller sequence and directory values. Free test
functions construct/send/receive actual IpcMessage. It neither recreates a library Client facade nor
owns a second pending/request table. Distinct error/status handling is visible in the real sample and
SDK consumers. Test samples retain their existing CPU/GPU separation and Native pixel oracles.

Performance remains PARTIAL. Changed caller preparation and removed implicit return work must not be
reported as equivalent to old batch roundtrip/reclaim measurements. Full final performance and independent
qualification belong to I7; this checkpoint does not upgrade baseline or historical qualifications.

## First full test attempt and corrections

Release attempt 1: 138/146. The eight failures are preserved in i4-tests1.log. Protocol binding lacked
its IPC DLL beside the executable; the GPU test-only publication observer dereferenced a null binding
for a rejected stale CommandId; Mesh retained two caller-owned input copies but cleared only one.
Programs expected disconnect to roll back an accepted compile. Under the new contract it commits safely
and discards only delivery; the assertion now requires the fourth actual graph to be committed.

Two old tests corrupted private erased descriptors through a friend backdoor. That construction path is
SUPERSEDED by immutable typed construction. They now install real, correctly typed same-name commands
with conflicting Request/Response contracts, preserving cold rejection, untouched active state and
compile-time signature constraints without a second descriptor/transport validation authority.

The eight failures have targeted passing reruns: i4-tests2.log (five corrected paths, including all
three real GPU queue modes) and i4-tests3.log (three semantic/typed-installation cases). Full all build
and immediate no-work pass are build7/build7-repeat. This is implementation evidence only; independent
I4 clone verification follows the checkpoint commit. I5/I6/I7 are not complete and nothing is pushed.
