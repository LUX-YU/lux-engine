# Architecture convergence R4 implementation

Parent: `ebd8c855d94b00c495e6c1d31d5b19e54fa5a44f` (R3).

Client/Server each have one Impl with business methods and state. Public methods forward; requestStop is
replaced by idempotent nonblocking stop(), without a compatibility alias. Host still joins run before destruction.
No second business authority, DeviceSession, operation table, worker thread or scheduler is created.

Host lends bounded enqueue/join callbacks (both present or both absent). Capacity 1..8 is fixed cold. A rejected
enqueue executes nothing and retains nothing; moved request is restored and explicitly failed. Without an
executor CreateEffect is Unsupported. An accepted task runs exactly once and retains its true source request.
Feature assembly and Shader/Pipeline compilation run on that resource; only the server publishes the candidate.
The worker only reads frozen registrations and Device capabilities. It does not use VMA, SubmissionQueue,
live Effects or contexts. Pipeline compilation uses no shared VkPipelineCache. Private VkShaderModule/Pipeline
objects obey independent Vulkan host synchronization; no broad device lock is added to the rendering path.

Release/acquire done publishes the candidate and is the callback's last access to its job. Job reclamation
therefore cannot race a later task access. Shutdown closes command dispatch, joins every accepted callback,
cancels unpublished jobs/readbacks, settles retained requests, resets Responder, then Effect/Device/Instance.
A reply rejected by a closed Requester rolls back its unexposed Effect. Native code was never submitted, so
candidate destruction needs no fictitious GPU ticket. Already attached code remains protected by real tickets.

Verification (external runner E:/RenderV2Rebuild/ConvergenceR1/run.ps1):
- Release and ASan full all -j 4 -- -k 0; second builds both report no work.
- Full CTest -j 1 -R "^(render|platform)\." passes 60/60 in both configurations (49.77s / 64.00s).
- Offscreen holds a real Program callback with a latch while two Scenes/four Views retain all attachments.
  Submission count advances >40, all four pixel oracles remain correct; callback release publishes success.
  Separate host rejection and occupied compile slot return capacity failure and recover without partial publish.
- Held compilation overlaps Stop and Readback: Stop reply remains readable, worker is joined, compile is Cancelled.
- Closed Requester during held compile: request/inputs survive, candidate is not installed; ASan clean.
- Failed contribution and last-good tests remain; init failure/restart/old session tests remain.
- Repeated stop and stop-before-run tested; run is single-shot. Stable no-input GPU loop remains zero engine new.
- Vulkan Validation/SyncValidation zero errors. Header/relocation consumers and Legacy719/history/user protection pass.

R4 gates pass. Final independent Debug/Release/ASan qualification remains R6. Root product closure remains
BLOCKED; IPC historical PARTIAL unchanged. Linux/Android/TSan NOT_RUN. GPU timestamps NOT_MEASURED.
