# LuxWindow UI example

`engine_ui_test` is a windowed Engine/Render integration test. It does not link any Editor target,
create a project, register an asset directory, or use the removed ECS bindings.
It initializes the normal EngineContext and an empty-VFS AssetRepository, including shared blocking IO,
so UI uses the same foundational lifetime as model scenes.

The target is available when `BUILD_TESTING`, `LUX_ENGINE_WITH_RENDER`, window and UI are enabled.
It uses the existing build directory; no standalone bootstrap, copied production sources or SDK is created.
The current native-window conversion is Win32; this test does not establish DirectX or Web qualification.

```text
engine_ui_test
engine_ui_test --frames 120
```

Without arguments the window stays open until closed. The finite form submits 120 frames; CTest registers
it as `engine.ui` with the `gpu` and `window` labels. It requires an interactive desktop and an eligible GPU.
The initial version uses a fixed output extent: resizing or minimizing ends with an unsupported error,
rather than silently claiming Resize support. There is no pixel comparison or performance qualification.

## Execution path

1. GlfwRuntime and LuxWindow are constructed before engine initialization, so the native window remains
   alive until RenderServer has joined, including when logical removal fails.
2. `engine::init` creates the shared ExecutionRuntime, Main response delivery and the service thread. RenderClient then
   resolves common commands through one IPC bootstrap batch before FrameSubmission and Context are published.
3. A CPU `ui::Root` owns one Pane with a label, checkbox and numeric input.
4. Startup creates a surface Target, queries the registered Imgui capability implementation, and creates
   Effect/Scene/View/Attachment. The font atlas is uploaded with the normal ImageCommands Sender;
   its CPU pixels remain in the coroutine until the upload Response.
5. A test-local ImguiCollector is registered through RenderCapabilityMetaRegistry. RenderSystem constructs
   it using the DrawView, font image and Attachment provided by the test. UI rendering needs no Reaction.
   The test also constructs the existing ViewReaction with no camera entities to verify failed factory rollback:
   Scene/Effect identities are visible during Collector construction, the failed attempt returns its Scene,
   subscriptions are disconnected, and the next construction succeeds without a second runtime.
6. An empty World and Simulation plus this RenderSystem are adopted by Engine. `Engine::run` owns the loop.
   BEGIN_UPDATE awaits `beginFrame`; SIMULATION_FINISHED feeds input and generates UI data. END_UPDATE
   submits the common frame. The next Root update never precedes copying the previous DrawView into FrameStorage.
7. Engine processes window events and Execution completions, including while frame admission waits.
   Startup and output cleanup use a separate outer task pump only while Engine is not running.
   No nested dispatch, IPC polling or extra thread pool is added.

The existing `captureImguiFrame` implementation lives in the RenderSystem module's project-visible
`sinclude`/`src` directories. The normal path uses it directly; there is no Editor dependency or duplicate encoder.

For upload boundary validation, `LUX_UI_FIF=1/2/3` selects the physical slice count and
`LUX_UI_SMALL_UPLOAD_WINDOW=1` limits preparation to one staging page and one Copy region.
`LUX_UI_INPUT_CASE=empty`, `odd-indices`, or `invalid-callback` selects test-authored FrameWriter data:
zero lists, two three-index lists with ResetRenderState, or an invalid callback in the second list.
The last case must return the frame contract error and still settle frame replies and remote owners;
it intentionally bypasses the normal capture function's earlier callback rejection.

`LUX_UI_TWO_WINDOWS=1` creates two native windows, each with its own View/Target/Attachment. Both display
the same Root through separate frame entries; this tests Render ownership and upload delivery, not input routing
between independent UI contexts. `invalid-second-attachment` accepts the first entry then rejects the second,
and verifies the reported entry index and resynchronization requirement. `no-draw` pauses both targets through
the normal command and requires successful NO_DRAW replies without GPU submissions. A one-frame run exercises
Stop with limited staging. `submit-oom` requires an external native-submit injection: two preparation submissions
must have been accepted before failure, and all frame replies and remote owners must still settle.

## Completion and ownership

Each frame records a test observer in FrameStorage. A normal run requires nonempty UI draw data,
matching submitted/completed frame counts, and successful SUBMITTED outcomes with actual batches.
The explicit boundary modes require their stated empty-output or error outcome instead.
The final replies are checked as well; reaching the requested loop count alone is not success.

Engine stops Scene borrowing tasks when run returns, but does not destroy the Scene. The test owner
settles frame replies before destroying any observer/collector input. Its shutdown verification
detaches the output, removes Target/View and the Engine Scene, then releases the font. Partial startup
records only successfully created IDs; explicit output-removal errors produce a nonzero test exit status.
Engine owns the SceneSession task barrier and RenderSystem's remote resource release.
Repository destruction settles its own independent tasks before releasing cached uses. GPU physical cleanup continues
to use Render's unique cleanup_scope_; the test adds no Scope or resource manager.

The native window is destroyed after EngineContext, which also protects failed cleanup paths. Root, Scene
and LuxObject-based controls are destroyed before ExecutionRuntime. Runtime failures use the existing
error catalog for readable messages; this example installs no general diagnostics framework.

## Simulation scheduling example

`engine_scheduling_test` exercises two independent fixed periods (8 ms and 13 ms) and a Variable group.
Each update waits 40 ms at BEGIN_UPDATE; the fixed groups must continue advancing during that wait.
Both caller-thread and shared-worker task execution are covered. A SceneSystem starts a borrowed timer
task, which must finish cancellation before Scene destruction. Engine run must retain the Scene;
explicit asynchronous removal then destroys it, and a stale removal must fail.

Fixed deadlines advance from the previous deadline. Missed steps are retained within a sampled horizon;
this example does not establish an overload/drop policy or real-time scheduling guarantees.

## Validation scope

This example validates its own current Engine/Render/UI dependency path. It does not migrate the older
Engine/Editor consumers or fixtures. A successful example run does not establish a successful full-product
build or qualification for those older consumers. Lua is optional and can remain disabled for this example.

## Ordinary model example

The executable has two explicit paths. Cooking requires a source model; running requires only cooked assets.
There is no automatic import, recook or fallback in the runtime path.

```text
engine_model_example --cook <path-to-model.gltf> <asset-directory>
engine_model_example <asset-directory> --frames 180
engine_model_example <asset-directory> --model <second-asset-directory> --floor <floor-asset-directory> --full-scene
```

`--cook` creates only the existing ExecutionRuntime, using its CPU and blocking IO schedulers. It writes
`model.luxasset`, Mesh/Material/Texture children, and Skeleton/Animation children when present. The ModelAsset
root is written last. Use a dedicated offline output directory; this helper is not a concurrent reimport transaction.
`COOK_RESULT` reports source-read, import/cook, encoding and file-write elapsed times. These intervals include
scheduler delivery and are not exclusive CPU timings.

Normal startup mounts each asset directory through DirectoryAssetProvider and reads its ModelAsset through
AssetRepository. Model components retain the serialized child AssetIds. MeshInstanceReaction uses the existing
repository/upload path; the example does not substitute direct uploads. The framing code supports multiple
primitives and reads the mesh assets through the same repository. Missing or corrupt cooked assets fail explicitly.
The running loop changes only Transform and desired target size. The LightCollector remains test-authored.

The default runs until the window closes. `--frames`, `--warmup`, `--width`, `--height`, `--instances`,
`--dirty-percent`, `--no-validation`, `--no-vsync` and `--offscreen` select bounded measurement conditions.
`--full-scene` enables the ten-light Clustered/PBR/shadow fixture. `--shadow-resolution` sets the shadow map size.
The lights are created once with their complete values and shadow settings; the static fixture does not resend
LightUpdates each frame. `--dirty-percent 0` keeps model transforms static as well. Camera interaction and Resize
remain real changes. Dynamic-light contracts are exercised by the Render fixtures, independently of this benchmark.

`STARTUP` records cumulative time from entering main to window/context creation, scene initialization, adopted
mesh/view resources and the first successful full-scene submission reply. `STARTUP_ASSETS` separately reports
VFS indexing, ModelAsset reads and mesh framing reads. `read_upload_adoption_ms` spans the existing asynchronous
resource-read/upload/instance-adoption chain; it is not pure GPU transfer time. A submission reply is not proof of
scanout. `gpu_drained_including_shutdown_ms` is an upper bound including Scene/asset/GPU teardown, useful with
`--frames 1 --warmup 0`. Material preparation and presentation timing require independent profiler evidence;
no production timing protocol or additional lifetime mechanism is introduced.

`ModelSceneSupport.hpp/.cpp` retains the shared cook, provider, capability-request and View helpers. Upload
providers outlive Context repository settlement. Engine retires the Scene before Context destroys its services.

## Real model lifecycle example

`engine_model_scene_test` retains the lifecycle assertions in `ModelSceneLifecycle.cpp`, separately from
the ordinary example. It uses LuxWindow and Engine without Editor. It accepts a source glTF and a
writable directory outside the source tree for its cooked MeshAsset. Source files are not modified.
The fixture currently requires a single mesh primitive, such as Khronos glTF-Sample-Assets `Duck`.

```text
engine_model_scene_test <path-to-Duck.gltf> <cooked-directory>
engine_model_scene_test <path-to-Duck.gltf> <cooked-directory> --hold
engine_model_scene_test <path-to-Duck.gltf> <cooked-directory> --two-scenes
engine_model_scene_test <path-to-Duck.gltf> <cooked-directory> --remove-loading
engine_model_scene_test <path-to-Duck.gltf> <cooked-directory> --offscreen
```

The finite mode retires the scene after its lifecycle checks and more than 300 completed frames.
`--hold` performs the same checks, then keeps the model rotating until the window is closed.
Vulkan Validation is enabled. Select a Validation layer compatible with the installed NRI/Vulkan
version through the normal loader configuration; the executable does not change system configuration.

The source cooker reads on the shared blocking IO scheduler and cooks on the shared CPU scheduler.
It writes separate MeshAsset, MaterialAsset and TextureAsset files into the existing DirectoryAssetProvider.
AssetRepository loads them through VFS. Production MeshInstanceReaction and MeshTransformCollector are
assembled by RenderCapabilityMetaRegistry factories bound to the actual upload providers. Static model expansion creates components before subscriptions.

Two entities share the same prepared Mesh/Material while receiving distinct Instances. The test removes
one Mesh3D and verifies that the other still retains the shared resources. Re-adding Mesh3D reuses MeshId
but receives a new Instance generation. Final Scene retirement releases both Instances before the repository
releases derived resources and texture dependencies, even after their Effect was removed.
These are logical release checks; they do not measure physical GPU memory reclamation latency.
GPU destruction still uses the existing cleanup_scope_.

Production ViewReaction and capability ViewCollectors create two independent outputs from ViewComponent,
Transform3D and RenderTargetComponent. The first uses perspective, the second orthographic projection.
The test only writes desired target extents. Reaction pauses zero-sized outputs, adopts the server's actual
Resize extent/epoch, updates parameters and resumes rendering. The other View must keep advancing during
pause and retain its own extent after Resize. Effect compilation count must remain one per Scene.
`--offscreen` exercises the same component path without native surface ownership.

`--two-scenes` shares the same asset providers between two Scenes, removes the original Scene/Effect,
then renders the surviving Scene with the same Mesh/Material identities. `--remove-loading` holds the
existing blocking IO workers in a test-only gate until Engine has requested Scene retirement; the gate
is then released and normal preparation/cleanup must settle. It does not inject a production task gate.

The LightCollector remains test-authored. Invalid material replacement checks last-good retention.
Resource-exhaustion, DeviceLost, every cancellation boundary, and actual PSO counts are separate
qualification topics. Inspect the actual window for textured rotating models; successful submission
alone does not establish pixel-reference equivalence. No production Readback is introduced.
