# N3 generic completion and Native Readback checkpoint

This is a prerequisite checkpoint, not completion of the joint N2/N3 gate. The remaining old public
Feature/Storage registration, manual versions, installation transaction and directory refresh still
require migration. N4-N7 are not started. No push is made by this checkpoint.

## Production ownership and deleted contracts

The generic protocol no longer classifies State/GPU/Result completion. ECompletionContract, the
registration GPU qualification bit, CommandInvocation's Native extension pointer, GpuAdmission,
GpuCompletionReservation/GpuCompletion headers and implementation, Runtime GPU completion bridge,
and its specialized Pending alternative are deleted. Native GpuWork only owns the existing Transfer
and its real publication candidate; its obsolete arbitrary accepted-Ticket path is removed. No Queue,
Transfer, IPC, pipeline or synchronization algorithm is replaced.

Runtime has one stable DeferredCompletion with one generic OperationCompletion. There is no CPU/GPU/
Readback variant or per-Tick Readback scan. A quota borrow accounts for compile admission; generic
request/result ownership, source return, stop state and bounded completion events remain authoritative.
The old raw-GPU and external Buffer fixtures now use a test-only finite SubmissionSender around the
existing real Queue reservation/Ticket callback. It is not installed or used as a second production
upload system. Output publication instrumentation observes the actual generic Reply, with a response
sample identity; it no longer infers origin from a thread-local last-ready sample.

ReadbackOperation/ReadbackSender implement the actual Native output use. ExecutionContext maintains
only a non-owning intrusive list of those uses and its existing reader count. Detach first admits
retirement, then cancels those borrows; refusal leaves the existing use unchanged. Cancellation unlinks
the Context, while an armed callback and operation remain alive until the actual Queue terminal event.
A completed Context may be physically destroyed before that callback without leaving a stale borrow.
The finite sender returns bytes through the same generic completion event, and Runtime only resolves
the Attachment, reserves Reply capacity and packages owned output. The optional Native submit observer
preserves the existing Render statistics for first-output execution; it is not a scheduler or owner.
Shutdown requests generic stop, drains accepted work/events, then clears ReceivedRequest before Responder.

## Behavior migration and tests

- Existing test names and valid GPU assertions are retained. No fake GPU completion replaces a Ticket.
- Retired GPU-enum qualification tests are explicitly superseded by v12: complete Protocol type/layout
  mismatch rejection, transferred Reply cannot complete or start a sender twice, and exact Failed source
  return are tested instead. This does not claim that an arbitrary business handler is GPU-certified.
- Real upload tests still assert Copy output, prior accepted Submit followed by failure, stale Ticket
  rejection, callback observation hold, three Queue topologies, two Clients, 1.5 s Main backpressure,
  exact Native destruction, returned Owner/capacity/budget, shutdown and DeviceLost behavior.
- Native Readback adds two concurrent uses, first-output execution, Fence observation separate from
  callback dispatch, Context destruction before callback, callback capacity rejection before Submit,
  pre-start stop and retirement-full Detach refusal preserving a successful original Readback.
- The pure Client installed consumer still forbids Native/Vulkan imports; its exact current-header list
  now requires Readback.hpp and its obsolete-header list forbids the removed GPU completion header.

Implementation Windows/MSVC RelWithDebInfo: full all -j 4 -- -k 0, second no-work, all 106/106 passed
in 212.41 s. The preceding full run was 105/106: its sole failure was the stale required-header entry
for GpuCompletion.hpp. Two subsequent build attempts exposed a test namespace typo and its overbroad
text replacement; both were corrected. All failed logs remain in the external evidence directories.
The bridge-only focused run was 5/5, including all three real GPU modes and external Buffer.

Measured final sizes: IpcMessage 144, IpcReply 304, RenderRequest 256, DeferredCompletion 672 (previous
768), CommandInvocation 24 (previous 32), SubmissionCallback 48, BufferPermit 16 bytes. Native buffer
creation/destruction counts matched in every topology (2694/2694, 2700/2700, 2694/2694 in this run).
Validation/SyncValidation errors were zero; actual environment warnings are retained. Independent
verification must use the exact following implementation commit; these are implementation-tree results.

The changed raw-GPU fixture now uses the generic completion event and a sample-bearing response. Its
timing is not an equal-work old/new performance qualification. Original per-stage timestamps/raw samples
are retained; no speedup or equivalence claim is made. Performance remains PARTIAL, root product remains
historically BLOCKED, upper Engine/Editor tests NOT_RUN. Final N7 Debug/Release/ASan, SDK three-prefix
synchronization and full delivery remain required. No legacy/history/user qualification is rewritten.
