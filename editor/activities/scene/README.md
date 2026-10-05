# Scene activities

Existing domain activities, relocated with their separate real targets and ownership.
Author state remains in authoring; workbench and application are consumers, never dependencies.


## Scene execution (P06)

`RunStore` owns private `RunSession` records. Each run retains its frozen SceneSnapshot, original
ContentStamp, configuration, package, registration environment and one SceneInstanceLease. It never
owns a Registry or invokes driveFrame. RunStore submits package preparation through the existing
ExecutionRuntime and publishes a generational RunId only after owner-thread instance creation succeeds.
StartRunId addresses preparation; it is not a RunId. There is no implicit ApplyRunChanges operation.

### One owner at each boundary

- The application EngineContext owns the actual SceneRuntime; EditorLoop/Launcher are the sole frame
  callers. ViewportElement only borrows an instance and maintains its view. RunStore.update receives facts.
- SceneRuntime owns instances, clocks, drivers and fixed-capacity step records. Each lease destructor
  only marks preallocated retirement state and wakes the owner. It cannot wait or erase a record.
- Requesting retirement immediately rejects new Registry borrows. Clock/receipt diagnostics remain
  readable until reclamation. Previously returned borrows end before driveFrame, owner wait or structure
  changes. Records are erased after stop maintenance reports completion outside all callbacks.
- World loading waits for cancelled reads; RenderSystem begins resource retirement and waits for its
  scene receipt. Runtime teardown may drain at the final owner boundary while ExecutionRuntime and
  RenderResources remain alive. A lease may be released in a callback; destroying Runtime there is invalid.
  A standalone host that pumps RenderRuntime manually must keep that pump running until retirement
  completes before destroying SceneRuntime; EngineContext's rendering adapter supplies that integration.

### Control and completion

Pause disables Simulation execution while maintenance, accepted read results, Transform synchronization
and publication continue. Resume rebases the existing FixedStepClock. Steps use 32 unacknowledged slots
per instance, bounded FIFO, one actual simulation target per accepted ticket. A ticket completes after
that step's publication, reports its original execution failure, or is cancelled on retirement. Acknowledging
a terminal ticket releases capacity; acknowledging an in-flight ticket is BUSY. No request is merged.

A pause owns a separate EditHistory and SceneEditing adapter. Resume/step end that epoch, with callback
admission guarded and active edits committed first. Stop closes editing and retires the instance; its receipt
is complete only after reclamation. It never modifies the author session, History, observed version or binding.

Worker preparation retains frozen inputs, never a live Session/ReadView/Pane. Owner task completion
stores its result independently of new-business admission. Nested delivery does not clear the outer
RunStore dispatch guard. The task cycle is released after completion; abandoning a start cancels it and
late completion only releases owned data. A consumed failed build cannot be adopted again.

### Execution and dependencies

RunStore accepts SceneSession capture or an owning SceneSnapshot. Worker preparation never
borrows live author data. The old Registry capture friend and encoding branch have been removed.
The formal RunInspector uses RunStore's existing paused Registry editing and its separate history.

`scene_execution_api` is a header target and `scene_execution` a static library, exported by
lux-engine-editor-scene-execution. The API uses SceneEditError and ContentStamp from the pure model;
implementation reuses the sole SceneEditing/History algorithms. Asset catalog validation is an explicit
host predicate; SceneEditing no longer links ProjectStorage. Neither Run target depends on old Editor,
EditorContext, UI or transition. Existing RenderSystem dependencies still include the renderer; the
engine SceneRuntime itself remains graphics/editor independent.

Author projection is implemented in this activity; formal View/UI/GPU integration lives in workbench.
EditorApplication owns frame driving and lifetime draining. Historical qualification failures/results stay in dev_log.

### P06 R1: results after instance retirement

Instance resources, step results and Run identity have separate lifetimes. SceneRuntime remains the
sole producer of FIFO results. Its preallocated InstanceLifetime owns the single 32-slot ledger;
Record no longer contains a second steps array or sequence counter. Instance retirement completes
at the original safe point without keeping Registry, Simulation, tasks or GPU scenes alive for queries.

RunStore reads that same ledger through the matching InstanceRetirement after the live slot disappears.
COMPLETED, FAILED (including its original phase/payload) and CANCELLED (STOPPED reason) remain available
until acknowledgeStep. acknowledgeStop explicitly acknowledges all remaining terminal results, then
releases the Run slot. Keeping a StopTicket copy does not prevent that cleanup. A stopped unconfirmed
Run still counts against the existing store capacity. Confirmed tickets and old Run generations are invalid.

Runtime stepStatus/acknowledgeStep accept an optional matching retirement receipt; without one they
retain live-slot-only semantics. Both paths enforce the same owner thread, runtime domain, instance
and ticket identity, and dispatch exclusion. Completion polling alone remains cross-thread safe.

Only failure values retain the required registration code pins, ordered after payload destruction.
Copied SceneStepStatus values own their pins too; replacing a value cannot unload its old code before
its payload destructor returns. Retirement drops the ledger's creation-time pins; it retains no graph
or rendering ownership. No parallel scheduler, history, global ticket registry or result mirror was added.

## Scene projection

`scene_projection` is a static Editor library with no UI/Context dependency. `ScenePresentationHub` shares one
projection for a session/history/configuration/environment key. The author Session is read only during capture;
no live read view escapes. Changed cursors currently use a full immutable capture; a lost cursor rebuilds too.
The cache is bounded, including records waiting for real Runtime retirement. It never drives Runtime frames.

`ViewportPresentation` owns only its view request, output reference, overlay candidate and retirement receipt.
`HighlightRenderer` prepares only after resource capture, keeps pins under backpressure, advances accepted only
on submission, and addresses the actual backend by the complete ViewHandle. RenderResources and the backend
retain their existing GPU completion ownership. Resources and overlay are not author edits.

All calls are on the Runtime owner thread. Environment registrations, fixed asset reads and metadata/code owners
must describe one version. Renderer/resources/executor outlive all consumers and their Runtime retirement.
The original product host remains the only frame driver. Shared projections rebuild together; callers resolve
new instance/camera bindings when ProjectionVersion changes. No persistent raw Entity is an author identity.

## Declarative source admission (EC4)

`makeSceneSessionFactory()` declares its original source-format relationship and the exact borrowed
`lux.simulation.components` dependency. Selection and registration do not capture schemas or create
services. At load admission the factory copies the immutable ComponentSchemaSet; its backing and code
pins travel with the decoder and prepared installation. A later directory replacement, scope release or
window closure cannot change that accepted input. Worker IO/codec and owner installation/reload continue
through the original SessionLoadJob, SessionStore and SaveService gates.

The selected `scene_module` archive declares this source factory, the existing projection service and
formal Scene UI factory. It uses the same module declaration as a dynamic extension and introduces no
second projection or Runtime. The product's remaining legacy window factory is a tracked M6 consumer;
its removal and the Application cutover are not certified by this declaration closure.

## Project environment ownership (EC4 M6)

`kProjectSceneEnvironment` is declared by the Scene module and implemented in the existing
`scene_project` activity. Registration does not capture assets. On first use it copies the fixed
registration inputs and captures the original ProjectStorage asset version. Scene and Material
views retain the same genuine shared allocation; there is no Application environment member or
second asset cache. Renderer/resources remain borrowed infrastructure.

The original scope maintenance point refreshes already-created environments when the project
catalog revision changes. This may become visible to a view on the next UI update. Until then the
previous complete version remains a valid frozen input, never a mixture with current files. Failed
capture preserves the complete previous environment and reports the original error domain; stable
revision maintenance performs no capture. Run preparation and compilation copy that fixed input,
so a later refresh does not change an already accepted task. Pure projection/Run consumers can still
construct the existing ProjectionEnvironment value without ProjectStorage or UI.

The project module declares this provider exactly once alongside project saving and the browser.
Products select project_module plus their desired author modules; Material does not require the Scene
module to obtain it. The service stays unconstructed until a view or Run actually asks for it.
