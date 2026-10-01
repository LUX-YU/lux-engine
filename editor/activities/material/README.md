# Material activities

Existing domain activities, relocated with their separate real targets and ownership.
Author state remains in authoring; workbench and application are consumers, never dependencies.


## Material compilation and preview

`material_preview` owns concrete derived work, not a MaterialSession. Compilation takes an owning MaterialSnapshot,
settings, environment version and target generation. CPU work produces one encoded immutable artifact. Completion
is a leaf fact stored unconditionally when ExecutionRuntime dispatches accepted task events, independent of UI
admission. Owners call collectCompletions then dispatchTaskEvents; collection alone does not deliver callbacks.

Preview adoption compares the full desired key. Older completions settle without changing the displayed stamp.
Compilation/resource failures retain the last successful resources and report a stale diagnostic. Prepared,
resource-accepted and recorded/sampled output are different facts. PreviewStore owns one existing Runtime lease;
ViewportPresentation owns the view. Neither creates a second Runtime or drives frames. Calls are owner-thread only.

publishCompiledMaterial reserves the same WriteCoordinator used by source saves and submits the already
encoded artifact. Expected destination version and FIFO remain authoritative. There is no Session pointer or
checkpoint adoption. A source change does not silently relabel an old compiled artifact.
