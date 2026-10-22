# Render request transport

`RenderRequester` owns one application-thread IPC Link, its finite response correlations and reusable frame
message carriers. It creates no thread or task scope and owns no connected OperationState or GPU object.
Requests are standard Senders; the application's existing scope retains their connected state.

Create the Link and requester, select its `id()` with `RenderServer::setFrameSource()` before starting Render,
then adopt the actual `ListCommands` result with `setCommands()`. The host already has the bootstrap CommandId
from `RenderServer::commandId<ListCommands>()`. `bind<C>()` performs cold type/policy qualification and returns
a command bound to this Link. Another requester cannot use that bound command.

Ordinary independent commands use `request(bound, request_value)`. Start and consume responses on the same
application thread; SPSC does not permit worker threads to publish on this Link. A sender's input reference must
remain valid through `start()`. Accepted Requests are moved into IPC; backing ranges borrowed by the Request must
remain valid until its Response. Admission/send failure restores the Request, including vector backing/capacity.
Stopping an already sent Sender does not cancel Native work or end that borrow.

For a complete game frame:

1. Call `beginFrame()` before preparing its data. A Game requester has `max_frames_ahead + 1` carriers; when none
   is available, consume/wait at the application's known safe point instead of preparing unbounded future frames.
2. Use `carrierIndex()` for the application's corresponding input buffers. It is independent of the Server's
   physical FIF slot and any future swapchain image index.
3. Append typed commands and supply exclusive `group_ends`. Commands inside a group are independent; groups
   execute in order. Source spans must reference this frame's stable data, not the next simulation frame's arrays.
4. Start `submitFrame(input)` in the existing scope. `consumeResponses()` completes it only after the frame
   terminal and its child responses are observed. Read typed child values from `input.responses`.
5. Call `releaseFrame()` after adopting the result. It keeps vector capacities. FrameCompletion ends the CPU
   input borrow and reports submission facts; it is not GPU completion or presentation.

An Asset requester leaves `max_frames_ahead` absent. New immutable uploads can progress independently; visible
table updates belong in the Game frame. Per-Link `request_capacity` limits actual outstanding correlations,
including child commands. It does not split a frame or decide when the Server draws.

`consumeResponses()` never drives ObjectRuntime, ECS or UI callbacks except the requested Sender receivers.
The application invokes it at its normal completion/adoption boundary and retains its existing signal and
deferred-command rules. Do not block that thread in `sync_wait(request(...))` while expecting it to consume IPC.

Connection loss is different from completion. `closed()` reports it, but missing responses retain their nodes
and input borrows. Only after stopping/joining Render and its accepted producers may the application call
`settleClosed()`. Remaining results become `kOutcomeUnknown`; missing frame results use `EFrameOutcome::UNKNOWN`.
Unreceived child responses remain empty. No response or cancellation is invented before this shutdown proof.
Destroying a requester with outstanding correlations is a contract violation.

This module implements transport and frame-input control only. Window presentation, asset residency, Scene/LuxObject
adoption and Engine/Editor product wiring remain separate work. It uses the installed Render contracts and neither
includes NRI nor creates another ExecutionRuntime.
