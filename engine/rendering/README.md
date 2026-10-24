# Render request transport

`RenderRequester` owns one application-thread IPC Link, its live response correlations and reusable frame
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

A publication producer without a carrier calls `requestFrameInput()`. `needsFrameInput()` combines this coalesced
request with queued commands; successful `beginFrame()` consumes it, while unavailable capacity preserves it.
A host must acquire that input **before** Scene publication, including maintenance during shutdown. Publication
cannot manufacture commands until it has their backing carrier; testing only the existing command queue can deadlock.

An Asset requester leaves `max_frames_ahead` absent. New immutable uploads can progress independently; visible
table updates belong in the Game frame. An Asset Link can set `max_pending_requests` for its actual outstanding
requests (`RenderExecutionConfig::asset_pending_requests` configures the built-in Asset Client). Main has no
request-count limit or reserved terminal slot: 1024, 1025 and 4096 distinct commands remain whole frames.
The only Main pacing gate is `beginFrame()` before preparing the next frame; the GPU's physical FIF is separate.

`consumeResponses()` never drives ObjectRuntime, ECS or UI callbacks except the requested Sender receivers.
The application invokes it at its normal completion/adoption boundary and retains its existing signal and
deferred-command rules. Do not block that thread in `sync_wait(request(...))` while expecting it to consume IPC.

Connection loss is different from completion. `closed()` reports it, but missing responses retain their nodes
and input borrows. Only after stopping/joining Render and its accepted producers may the application call
`settleClosed()`. Remaining results become `kOutcomeUnknown`; missing frame results use `EFrameOutcome::UNKNOWN`.
Unreceived child responses remain empty. No response or cancellation is invented before this shutdown proof.
Destroying a requester with outstanding correlations is a contract violation.

`RenderExecution` assembles one Server, one RenderThread and one fixed Asset protocol thread. Both Clients
reuse this Requester; all CPU/IO work borrows the application's `ExecutionRuntime`. `create()` is synchronous
startup on Main, outside a dispatched callback. It discovers commands and prepares the two actual Links.
`consumeResponses()` runs at Main's normal safe point; the Asset thread drives its own Link and RunLoop.
A full response service batch re-arms Main's existing wake epoch, because more already-published replies may
remain even when Server is idle. This service chunk is not a request/frame admission budget. Direct Requester
consumers must continue draining when the returned count reaches their requested limit.

Frame-scoped `request()` inputs join the next `beginFrame()` in ordered groups. Their original standard Sender
receives the typed child Response. This permits finite create/remove transactions without a second operation
owner or a second frame source. Hot Transform/data batches still use direct `append`, not one Task per item.
Continue maintenance frames when `hasQueuedFrameCommands()` is true, including when Simulation is paused.
The application's normal path uses `RenderExecution::beginFrame()` before preparing inputs; nullptr means wait.
`RenderExecution::submitFrame()` takes over that carrier and recycles it after all real replies. Failed completion
uses the existing diagnostic outlet and Requester's resynchronization revision; Scenes refresh their authority.
Direct Requester consumers can still handle FrameCompletion themselves, with the same explicit release contract.
A carrier released before submission cancels its queued commands; a lost response after submission stays Unknown.

`FrameInput::allocate<T>()` allows writing actual frame arrays directly. `freeze()` is the optional copying path;
`memory()` supports direct UI capture. Each carrier owns its data interval, backed by reusable standard PMR blocks.
No live Scene/Pane buffer is borrowed after that object closes.

`RenderAssets` owns only the Main asset/version-to-handle directory and actual Client use counts. VFS reads,
CPU decode, Asset upload and Main adoption form finite standard Sender pipelines. Mesh conversion produces the
actual static GPU vertex layout once; Texture/Mesh source bytes are released on real Response, before Main adoption.
The first Scene projection consumes LOD0; animation/LOD selection is not fabricated by uploading all LOD indices.
Different source revisions can coexist. `maintain()` visits pending release changes, not every live asset.

Close Scenes before closing their project task admission, drive their pending frame commands, then release asset
references. After `RenderAssets::close()`, continue Main completion and maintenance frames until `settled()`.
Then call `RenderExecution::beginClose()` and keep driving Main plus `advanceClose()`. Unsent inputs cancel
locally; accepted requests retain their borrows until real replies. Each Client closes on its own SPSC thread
before Server is stopped and joined. `shutdown()` is the synchronous standalone form of this same progression.
`settleClosed()` is reserved for an abnormal, already joined Server, never used as normal cancellation.
ExecutionRuntime, file providers, native windows and callback code outlive their respective accepted producers.
Render diagnostics use the single Engine log bridge; this module never selects a console/output target.

Window presentation and Editor UI assembly are separate consumers of these facilities. No new CPU pool is created.

Only one carrier may be BUILDING. `buildingFrame()` lets existing Scene systems append during the
application's frame safe point; it is not a second frame owner. `hasFrameCapacity()` is a pacing observation,
not an admission promise across callbacks. Received failed frames advance `resynchronization()` so every
Scene can refresh its own authoritative state.

Asset acquire methods accept an optional explicit project read port. It is captured into that finite
pipeline before control returns; simultaneously active project generations never consult a mutable global
source selector. The caller supplies a distinct `RenderAssetVersion::revision` for distinct source generations.

UI captures `rdesc::UiDrawData` directly into `FrameInput::memory()`. `makeUiDrawInput` freezes only per-draw
clip/image selections and the projection parameters; geometry remains the original frame borrow. Native pads
only a final non-four-byte-aligned copy fragment. Sampled UI input values are linear: use an SRGB Target for an
already display-encoded LDR viewport, or a deliberately linear UNORM/float source. The final UI pass performs
one display encoding and no tone mapping.

## Window presentation

When window support is enabled, `makeWindowSurface(window.nativeWindow(), vsync, surface)` translates the
non-owning typed window value to NRI's surface description. It creates no device, window or swap chain.
The caller keeps the actual window alive until its surface target is retired. Unsupported native handle
forms return false; currently only the Windows path is implemented. Offscreen builds without window
support omit this function and its installed header instead of advertising an unusable stub.

Execution failure does not close the Server endpoint. `failure()` reports the first execution error; ordinary
production commands subsequently receive typed failures. Commands declaring `failure_safe` may query or logically
release existing objects without creating GPU work. `Stop` requests this quiescence; it does not destroy the Server.
The application stops producers, drains its cleanup replies, then calls `beginClose()`/`advanceClose()` to close both
Clients and stop/join the Server. `advanceClose()` reports progress independently of `failure()`; `shutdown()` returns
the original error only after this sequence completes. Abnormal external disconnect still uses `settleClosed()`.
