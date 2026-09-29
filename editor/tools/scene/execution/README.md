# Scene execution (P06)

`RunStore` owns private `RunSession` records. Each run retains its frozen SceneSnapshot, original
ContentStamp, configuration, package, registration environment and one SceneInstanceLease. It never
owns a Registry or invokes driveFrame. RunController submits package preparation through the existing
ExecutionRuntime and publishes a generational RunId only after owner-thread instance creation succeeds.
StartRunId addresses preparation; it is not a RunId. There is no implicit ApplyRunChanges operation.

## One owner at each boundary

- The application EngineContext owns the actual SceneRuntime; EditorLoop/Launcher are the sole frame
  callers. SceneElement only borrows an instance and maintains its view. RunStore.update receives facts.
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

## Control and completion

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

## Compatibility and dependencies

The old SceneEditor is a P12 UI consumer of RunStore. It retains UI selection, cached display facts and
borrowed pause-editing pointers; source/package, task ownership, instance lease, Registry observers and
history ownership moved here. Its existing single-outstanding Step button policy remains a UI admission
rule, while the new RunStore/Runtime API accepts the full bounded FIFO. The private
`editor/transition/SceneRunCaptureAccess.hpp` accepts only a frozen legacy SceneCapture. It is not installed,
not linked into execution, has only editor_scene as consumer, and must disappear by P12. New code uses
SceneSession.capture / SceneSnapshot. No shadow SceneSession is synthesized for the legacy Registry.

`scene_execution_api` is a header target and `scene_execution` a static library, exported by
lux-engine-editor-scene-execution. The API uses SceneEditError and ContentStamp from the pure model;
implementation reuses the sole SceneEditing/History algorithms. Asset catalog validation is an explicit
host predicate; SceneEditing no longer links ProjectStorage. Neither Run target depends on old Editor,
EditorContext, UI or transition. Existing RenderSystem dependencies still include the renderer; the
engine SceneRuntime itself remains graphics/editor independent.

P07 author projection, P10 complete new UI/GPU product validation and P12 application lifetime integration
remain subsequent work. C01/C03/C04 remain unchanged. P06 qualification reproduced the archived
Physics2D generated-header ordering failure and corrected only the description target's generator
prerequisites. Both failures remain archived; this is not a whole-repository cold-build qualification.
