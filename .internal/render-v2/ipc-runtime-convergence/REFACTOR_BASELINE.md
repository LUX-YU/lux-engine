# IPC / Runtime convergence: I0 source audit

Baseline: `9184c9f71c63af7f686822c76b89263b29ef7a63`, remote checked after fetch on 2026-10-17.
Contract SHA-256: `5b52a293293b75418598ff3c7314cc7160b3014c08c434d4757328a5fd9f9ef7`.
Construction: `E:/RenderV2Rebuild/IPCConvergence/source`, branch `codex/ipc-runtime-convergence`.
Independent baseline clone: `../baseline-source`, detached exact baseline, no alternates, --no-hardlinks.
This audit does not promote the N7 review checkpoint or inherit N6's 145/145 qualification.

## Actual consumers and migration

The complete file/line inventory is `../evidence/I0/consumers.md` (generated from tracked baseline files by
`../inventory.py`). Paths below are relative to modules; the inventory includes every matching test and
installed consumer, not just these production groups. False lexical matches in MaterialSource (Unicode
codepoint handling) are not pins and are excluded from changes.

| Current source / exposure | Actual consumer and behavior | Migration / evidence |
|---|---|---|
| platform/ipc/include/IpcMessage, src/Ipc, sinclude/detail/MessageAccess | Message owns object, canonical table and shared code pin; erased construction supports budget envelopes | I2: private template move/destroy only; real objects, DLL allocation/destruction, alignment |
| platform/ipc/include/IpcCore, src/Ipc | Channel owns Links, each Link owns two fixed rings; Requester tracks reply credits; ReceivedRequest borrows route | I2 direct response; I3 segmented/reusable rings, retain source route and close ordering |
| render/runtime/include/RenderClient, src/RenderClient | Only production application consumer is examples/basic_mesh/Client.cpp; client owns Pending, manifest, budget | I4 delete class/target/wrappers, caller ids and direct IPC; closure remains unknown to application |
| runtime/include/CommandValue, pinclude/TypedProtocol | Server registration, Request, Client and Async completion depend on transport, envelopes, Hello and session metadata | I4 delete budget/ABI/Hello; request/response packaging moves real values |
| runtime/pinclude/Request, src/Request | One ReceivedRequest; response pin/transport borrowed from stable command metadata; failed publication retained in unreplied_ | I4 preserve this sole source owner; add retry of actual result, never run Handler twice |
| runtime/pinclude/Deferred, src/ServerCompletion/ServerEffect | Fixed stable slots and fixed event queue; no steady-frame pending scan; compile and professional sender state owned once | I4 grow actual asynchronous records and notification capacity together; no Client mirror |
| runtime/include/RenderServer, src/ServerRegistration/RenderServer/ServerLoop | Device initialized at create, unique command and Handler tables; public Vulkan/Texture/config/pins; null-id Hello | I1 real cold commandId query; I5 selected Native initialization and one borrowed host Runtime; no extra device owner |
| protocol/include/Protocol, AsyncCommand | Handler thunk, Reply, actual finite sender and Receiver; generic completion; metadata also exposed to client | I4 preserve necessary server-only Reply, strong uint64 RequestId; I5 remove pins, preserve completion tail rules |
| data/ConfigValue, Payload | Shared real config/assets plus explicit code pin; used by Features, Storage and real 3D uploads | I5 remove code pins only; keep actual byte owner and bounds |
| resource/Factory, ResourceRegistry; native transfer/GpuWork | Factory owns matching erased implementation; public mutable fields; GPU sender propagates two code pins | I5 immutable factory and host-lifetime precondition; same actual storage and GPU owner |
| core/execution/WorkSchedule | GetWorkCodePins consumers are only render AsyncCommand and Native GpuWork; schedule's pin args used by render compile/upload | I5 remove unused query and propagation; preserve actual CPU pool, notifications and reentry protection |
| examples/basic_mesh/{Client,Host}, runtime/test/*_installed, native/test/installed | Pure client and combined installed consumers; source sample automatically recovers instance batch vector | I4/I6 direct message APIs; explicit business result only; same mesh/color/depth/readback assertions |
| runtime/CMakeLists, render bootstrap / component export | render_client DLL exports all Client and budget functions; runtime transitively links it and Native extensions | I4 remove empty client target, select pure protocol+IPC dependencies; I5 generic Server headers |

Public headers under include/ are installed by component exports. sinclude/MessageAccess is project-only
and must disappear once no erased ABI client construction remains. Installation tests enumerate installed
headers and use relocated prefixes; their expected sets must change with the actual deletions. Historical
checkers/reports remain immutable. No Engine/Editor production code uses this RenderClient; their historical
missing-component closure remains separate BLOCKED, not a reason to retain a forwarding target.

## Ownership and publication

`IpcCore -> Name -> current/retired Channel -> Link -> request/reply segments` remains the unique IPC owner.
Role qualifications and reader grace protect Channel lifetime. ReceivedRequest borrows a source route under
the responder's lifetime precondition; all such requests end before responder reset. No global RequestId map.

```
caller message --successful publish--> Request queue (Queued)
    --pop--> ReceivedRequest (Received / Running)
    --defer--> existing stable DeferredCompletion.request (Async)
    --completion--> same request + actual response message (ResultReady)
    --tryReply success--> Reply queue (ReplyPublished), end original request
    --tryReply allocation failure--> SAME ResultReady owner; retry at owner tick, no reexecution
    --closed--> destroy result and source route after accepted native/CPU work has ended
```

Before successful request publication, allocation/full/closed failure retains the caller's message.
There is no promised automatic original Request return after publication. Already published replies are
drained before CLOSED. Queue failure must not turn an already committed Success into Failed/Cancelled.
Existing `unreplied_` and `undelivered` lack retry today; do not mistake retention alone for completion.
The same RenderRequest owns the result through retry; no second reply owner or queue is required.

Command table remains SlotMap-owned, active call `{invoke, object}` borrows stable Handler owner. ListCommands
already has a real entry installed by Impl construction: Host cold query can return its existing SlotKey;
no bootstrap Registry or replacement identity is needed. Resource / Scene / View / Effect ownership and
SubmissionRequirements remain unchanged. Native options move to the selected Native construction entry;
the single Impl continues to own Instance then Device, with reverse destruction after work drain.

## New state permitted by this contract

- RequestId becomes a caller-generated strong uint64 value. Existing headers hold only correlation/routing.
- Existing IpcMessage loses qualification/pin fields; its private TypeOps remains static template machinery.
- Queue-internal Segment/Slot and safe reuse links replace fixed storage. No new external queue owner.
- Existing pending records may grow; their ready-notification landing place must grow before starting work.
- Existing request result retention gains retry; close and result publication are terminal, exactly once.

No new Manager/Registry/Client wrapper/CodeGuard/Backend owner is planned. If actual implementation requires
one beyond this contract, stop with DESIGN_CHANGE_REQUIRED.md before attempting that design.

## Behavior coverage retained / superseded

SUPERSEDED: automatic successful/failed original Request return, client abandon/pending capacity,
per-message byte budget, canonical ABI/storage-policy rejection, late plugin unload via shared pin,
null-id Hello, transparent session checks. Preserve their historical source and evidence through Git.
Replace with direct response/source identity, unsent retention, host-managed module lifetime, caller-owned
unknown outcomes and queue growth/OOM tests. Wrong get<T> is now a caller precondition, not a fake runtime ABI test.

RETAIN: nontrivial movement/destruction, over-alignment, correct DLL destructor, independent links/FIFO,
close/publication overlap, zero idle IPC worker, source retention through native work, typed handler switching,
transaction rollback, CPU synchronous completion/reentry, GPU partial submit/LastUse/retirement, real mesh,
camera/material/instance/depth/readback, pure consumers and relocation. Existing protocol, resource and graph
algorithms are not rewritten. Test consolidation must map each distinct invariant before removing a fixture.

## Protection / measurements

Initial legacy checker passed all 719 committed and working blobs; all four protected user files unchanged.
lux-cxx `bf779515a120350c7c5412c366eea73afdf59ccf`;
toolset `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`.
Only new contract scope is writable: IPC, affected Render/Execution seams, their tests/install configuration,
and this new record directory. Legacy, old reports/checkers, primary and historical dirty trees are excluded.
Core TaskInfo::code_lifetime / Task ownership have non-Render consumers and are excluded from pin removal.

Baseline commands/logs/probes live outside source in `../evidence/I0/`.
Exact-baseline Windows MSVC 19.44 /GR- RelWithDebInfo and Debug each built all 502 steps with `-j 4 -- -k 0`,
then reported no work. RelWithDebInfo affected Native/Runtime/IPC tests **74/74**, 126.61s; Debug targeted
Native execution/Runtime camera/control/offscreen/shutdown/typed-client/IPC tests **21/21**, 26.84s.
Installed consumers ran within those selections. Execution outcomes test observed all five outcomes,
one fence probe and Validation errors=0; duplicate EOS overlay warnings are preserved. Baseline clone stays
clean. This is a limited current baseline check, not a new full N7 or ASan qualification.

Five independent process runs used i9-13900H / Windows / the exact Release SDK. Per run:10,000 small
empty-pin/pinned/real-Client-budget and two-thread samples;1,000 each 64KiB/16MiB pointer-preserving asset
roundtrips. Full-ring failure retained the original input. IPC DLL hot allocation counts were zero;
10,000 two-thread requests moved objects 30,000 times on each side. Empty open IPC roles used no measured
CPU over 1.5s sleeping observation (Windows process-time granularity; not an assertion about Server idle).
Message=144/align16, Reply=304/align16, TypeOps=88, ReceivedRequest=160, Requester/Responder=24;
Client BudgetLease=24, Envelope<uint64>=32, RenderCompletion=384.

Across five runs, p95 RTT ranges (ns):empty pin200–200, nonempty pin200–200,64KiB200–300,
16MiB200–300,two-thread400–500,real Client pin+budget500–800. Timer quantization is about100ns;
tails include two-thread max178,900ns and Client max191,300ns. Retained bytes returned exactly to zero.
The Client probe includes Pending, qualification and packaging; **isolated budget-CAS cost NOT_MEASURED**.
These limited probes are not causal performance qualification or a speedup claim. Existing `platform.ipc.cost`
and full Feature Operation measurements were also retained with all samples, original sleep boundaries intact.

I0 status: **AUDITED**. No production modification. Raw logs, CSVs, consumer inventory, probe source,
commands and environment are sealed in `E:/RenderV2Rebuild/IPCConvergence/evidence/I0-baseline.zip`
(90 files), SHA-256 `20cdb321714ebf28780d8ba59fda82da43f9f1d922f7d64874d967634d5f8516`.
Historic Native/Alias/IPC performance PARTIAL; full product BLOCKED; unrun platforms NOT_RUN.
