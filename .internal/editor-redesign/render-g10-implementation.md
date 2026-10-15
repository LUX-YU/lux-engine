# G10 implementation: open GPU-ready Feature completion

Parent S0-I: `9b0bc12723fb63f781a413755d0ee4e8eb55fed6`. Exact original baseline and specification hash
remain in `render-s0-g10-ownership.md`. G11–G13 are NOT_STARTED.

## Ownership and admission

The Server still has one Operation SlotMap and one DeviceSession. An explicit `gpu_completion` handler capability
permits GPU-ready registration; ordinary `RenderRequest::reply()` still rejects that contract. Zero-pass Feature
operations require no Program callback. Client and Protocol carry no Native completion objects.

`OperationContext` is a noncopyable borrow restricted to the current Handler call. `GpuCompletionReservation` is a
move-only right to one preallocated slot, not an Owner, Future, or asynchronous task. It is bound to the current request;
there is no public operation accepting an arbitrary Request to reserve. Reserve verifies the already-bound reply schema,
byte budget and one-slot capacity before Native preparation/submission. It copies at most 128 result-header bytes and
retains the result attachment. Default capacity is 128, configurable 1–128, allocated once at Server construction.

Commit accepts only this DeviceSession graphics queue's genuine Ticket, newer than the queue serial observed at reserve.
A valid commit needs no second capacity admission or allocation: it moves the original source-qualified RenderRequest
and one unique Native pin owner into the reserved slot. Invalid commit preserves both. The Handler cannot issue two
reservations for one call. Borrow/reservation may not escape the Handler or be passed to a worker.

One bounded `DeferredCompletion` collection replaces `PendingReadback`; its variant holds either the existing Context
readback borrow or the GPU condition. No additional Request, resource, or retirement table exists. Tick polls real queue
completion without waiting, invokes the ready callback once to certify data/commit business state, destroys Native pins
while the original source is still retained, then publishes Success. This callback is per completed operation, not a
Feature dispatch in stable render frames. Shader/graph compilation and resource lookup remain cold.

`DeviceSession::releaseCompleted(BufferPermit&, Ticket)` supports exact completed pin disposal without another retirement
capacity admission. It resolves owner/key/state, checks the real queue, rejects an unfinished Ticket without consuming the
permit, and consumes/releases only after completion (or established device loss). It does not replace deferred retirement
for in-flight work. The S0 raw record destruction functions remain private; Binding remains 32 B.

Shutdown stops admission, joins compile work, settles compile results, drains accepted GPU work in the cold shutdown path,
then disposes GPU pins and returns OUTCOME_UNKNOWN for accepted work whose business completion was not published. It does
not promise cancellation of already executed GPU side effects. Readback cancellation retains its old semantics. Every
source request is settled/disposed before Responder reset; effects/DeviceSession are destroyed before InstanceContext.
Client close/abandon does not cancel Native work. Reply ownership can outlive both Client and Server.

## Real consumer and failure coverage

`test/GpuReady.cpp` registers `test.g10.gpuReady` from an external zero-pass Feature. It uses original IPC attachment bytes,
prepares two actual buffers in the existing Session/VMA ledger, records Copy + host-read barrier, submits through the real
queue, and compares all copied bytes after real completion before Success. No probe name or branch exists in Server Core.
The 64 B attachment view pins its entire 1 MiB backing. Native staging copy is required GPU upload work, not an IPC copy.

Coverage includes two equal RequestIds on different Links; premature ordinary reply; held completion observation with no
Success or early destruction; full completion slots and insufficient reply bytes before Submit; previous Ticket rejection;
submit failure after a prior accepted Ticket; raw malformed ExternalDataRef before Handler; actual queue saturation;
late Reply owners; abandon and Requester close; stop with two compile jobs + GPU upload + Readback; and explicit Unknown on
an open Client after stop. Four Views/two Scenes continue rendering while Main has 64 unread replies for 1.5 seconds.
Two host-owned cold workers concurrently compile real Shader/Pipeline candidates using the same DeviceSession.

## Diagnosed intermediate failures

All unsuccessful attempts remain in external logs. Initial fixture construction copied a deleted move-only permit and was
fixed by an explicit constructor. An abandon budget assertion originally kept an earlier failed Reply alive; releasing that
Reply at its intended test boundary corrected the fixture, without weakening budget ownership.

A later unconditional-Success benchmark assertion reproduced with error 7356206049137219893
(`lux.render.vulkan.capacity`), inbound timestamp present and Submit/Fence timestamps zero. The unchanged Native queue has
three submission slots shared with Views. A deterministic held-Fence experiment now proves this rejection occurs before
GPU submission and releases unsubmitted candidates safely. The benchmark retries only this precise capacity error within
a deadline and saves every rejected attempt separately; all other failures remain fatal assertions. No queue size,
publication algorithm, or lifecycle rule was changed to hide the observation.

## Cost and measurement scope

Release sizes: AllocationPermit 16 B, OperationContext 40 B, reservation 24 B, RenderRequest 216 B,
DeferredCompletion 448 B. Default pending storage is 57,344 B (previous 128 x 224 B readback reserve: 28,672 B).
Test capacity two uses 896 B. IpcMessage 144 B / IpcReply 296 B are unchanged. The extra space is bounded and measured.

The existing 1000 full bound CPU operations retain encode/send/handler/reply/consume/decode timing and allocation checks.
The new 1000 completed GPU operations save inbound, accepted Submit, observed Fence, publication-start and consumed times,
including all outliers. Publication-start bounds publication; it is not a timestamp of the atomic Ring store. GPU timestamp
is NOT_MEASURED. The Server's existing 1 ms idle sleep remains scheduling time, not an IPC queue latency claim.

CPU `new` calls cover participating Client/Server test code, including test output construction and Native C++ metadata;
Native buffer creation/destruction counts are separate VMA resource operations, not driver heap measurements. GPU requests
are not claimed allocation-free. Stable four-View windows and small bound CPU operations retain their zero-Engine-new checks.
No performance improvement is claimed without equivalent workload evidence.

## Gates and evidence

Commands and logs: `E:/RenderV2Rebuild/S0G10/run.ps1`, `implementation-matrix.ps1`, `evidence/`.
Full all builds use `-j 4 -- -k 0`; CTest uses `-j 1 -R .`. Release/Debug/MSVC ASan are repeated in the final independent
clone, including header and relocated installed consumers. Final exact-I evidence and archive digest are indexed in the
separate Verification report. Native Validation + SyncValidation and ASan must have zero errors; actual warnings are retained.
Legacy 719, primary user hashes and all protected history are checked against be9d7af3. IPC R2 PARTIAL is unchanged.
Root TOOLCHAIN closure remains a separately measured gate, never substituted by bootstrap success.
Linux/Android/TSan NOT_RUN; Android header synchronization is not a platform qualification.
Implementation gate results (before independent V): RelWithDebInfo 62/62 in 75.57 s, Debug 62/62 in 67.96 s,
MSVC ASan 62/62 in 88.47 s. All three second builds no work; Validation/SyncValidation/ASan errors zero.
Controlled queue saturation passed; natural benchmark capacity refusals are in `runtime-gpu-backpressure.csv`.
The final independent clone must repeat this matrix; these internal results do not substitute for V.
