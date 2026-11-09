# Engine runtime

`composition` provides process initialization and advances externally assembled scenes.
`metadata` is independent cold metadata; it owns neither a running Engine nor its scenes.

## Initialization and ownership

`engine::init` returns `expected<Initialization, error::Error>`. Initialization is move-only and owns
ExecutionRuntime. With `LUX_ENGINE_WITH_RENDER`, it also creates RenderServer, RenderRuntime, RenderFrameSubmission and the rendering
thread. Rendering configuration is then an explicit argument; a build without the option does not initialize graphics.
Moving Initialization does not move its private state or invalidate borrowed schedulers/frame storage.

Create Initialization before the first LuxObject, Engine or asynchronous consumer, on the application thread.
ObjectRuntime obtains the shared ExecutionRuntime directly and never depends on Engine. Engine owns SceneSession
records containing SceneInstance, fixed/variable task graphs and time state; TaskExecutor borrows the shared CPU pool.

`removeScene` synchronously destroys a session at a safe point. Asynchronous operations must own their inputs and
finish any actual borrows before their objects disappear. The global Execution scope does not keep raw `this` alive.

## Engine stages

```cpp
using EngineStageCallback = std::function<Engine::Result()>;

engine.addEngineStageCallback(
    [&feature]() noexcept { return feature.prepare(); },
    EEngineStage::BEGIN_UPDATE
);
```

The callback argument precedes the stage. Engine owns the functions and releases them before destroying its scenes.
Borrowed captures must outlive Engine. Registration occurs outside advancement; callbacks must not reenter the loop,
change scene membership or modify the callback lists. No connection token or explicit unregistration is required.

One whole Engine update executes:

1. `BEGIN_UPDATE` callbacks.
2. Each due realtime Scene's fixed catch-up and variable Simulation group.
3. `SIMULATION_FINISHED` callbacks, once after all those simulations.
4. Enabled SceneSystems, once per scene, including scenes whose Simulation did not tick this round.
5. `END_UPDATE` callbacks, once for the whole round.

Within each stage callbacks run in registration order. A normal-stage error prevents subsequent normal stages and
invokes `UPDATE_ABORTED`. All abort callbacks run, including after an earlier cleanup callback fails. They release
only preparation they actually own; no simulation or already-submitted GPU work is rolled back. The original error
is preserved after all abort callbacks have been attempted.

Callbacks are stored once at registration and invoked by reference; Engine does not copy functions or build a new
callback list every frame. No renderer-specific branch exists in Engine's stage dispatch.

## Rendering integration

EngineContext owns RenderClient, RenderFrameSubmission and the service thread. Engine binds its existing
stage callbacks once at run entry: frame admission precedes external BEGIN_UPDATE work, submission follows
END_UPDATE writers, and UPDATE_ABORTED discards only unsubmitted input. Hosts do not bind rendering manually.

RenderSystem borrows World and Context and receives declarative capability composition plus EffectOutputDesc.
It obtains a temporary FrameWriter from RenderFrameSubmission during execute; no FrameStorage is passed or
retained by a System. Collectors receive that writer explicitly. FrameStorage backing blocks rotate without
copying payloads; the remaining const frame() accessor serves read-only packet inspection, not publication.

`beginFrame()` suspends only when all CPU carriers are submitted. A real reply completes the waiting Sender;
there is no per-frame IPC polling, timer or local Scope. The limit retains RenderServer's existing meaning:
one consumed frame plus `max_frames_ahead` prepared/queued frames. It limits frames, not requests per frame.
A pending begin is settled by a frame reply, including failure; cancellation does not free in-flight input.

Submission starts in Execution's existing scope. Synchronous admission failure is returned from submitFrame;
asynchronous frame failure reaches all local frame observers and the next beginFrame (first pending error).
The existing resynchronization epoch prevents an older successful frame from clearing a newer failure.
Server completion returns the CPU input borrow and submission result, not GPU completion or presentation.
GPU FIF resources and physical cleanup remain entirely in Render and its unique cleanup_scope_.

## One application loop

Engine::run is the normal Main completion driver. It dispatches outside TaskGraph/System/Collector traversal
and applies ECS structural commands at the existing barriers. Platform event pumping belongs to the host;
window applications install Execution's wake callback. No callback recursively dispatches the loop.

## Destruction order

1. Retire Scene/System consumers and their accepted tasks before destroying Engine.
2. Keep registered asset providers alive through Context repository settlement.
3. Context returns frame CPU borrows, then settles and destroys its asset repository while Render is available.
4. Context requests Server stop and dispatches Main until the exit callback joins the thread and settles Client replies.
5. Destroy communication objects, ObjectRuntime and VFS, then ExecutionRuntime last.

Initialization failure records the stop intent before any repository rollback may dispatch Main. A CLOSED endpoint
is expected during that shutdown, but protocol errors remain errors. No pending source borrow is abandoned merely
because IPC closed; final unknown outcomes are delivered only after Server settlement and join.
