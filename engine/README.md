# Engine

Engine schedules externally assembled Scenes. It does not choose concrete rendering, physics or scripting behavior.

- `foundation`: identity, ECS infrastructure and spatial algorithms.
- `domain`: data Components and World/Simulation/Scene contracts and ownership.
- `services`: assets, Context declarations, world storage and persistence codecs.
- `systems`: concrete optional implementations of the domain system interfaces.
- `runtime`: Engine scheduling, process initialization, metadata and plugin assembly.
- `toolchain`: offline compilation; `test`: executable consumers.

Each module uses include (installed), sinclude (project-visible), pinclude (private), and src.
Directory groupings are not additional runtime objects or automatic library boundaries.

`init` returns an owning EngineContext. It owns ExecutionRuntime, ObjectRuntime, a live VFS, the asset repository,
and the selected rendering facilities. Engine borrows Context; it owns SceneSessions and the retirement queue.
RenderSystem borrows its World and Context. Components remain reflected data without member functions.

`engine_context` supplies declarations; `engine_runtime` schedules Scenes; `engine_initialization` owns startup.
The optional Engine `system_render` component is static. Initialization links it only in rendering builds;
Engine scheduling itself has no Render dependency. The lower Render library retains its own backend boundary.

For in-process assembly from a `SceneDescription`, register component schemas and system declarations in
[`EngineMetaFunctionRegistry`](runtime/metadata/include/lux/engine/EngineMetaFunctionRegistry.hpp).
[`Engine::SceneBuilder`](runtime/engine/include/lux/engine/SceneBuilder.hpp) resolves system names and configuration
versions, then calls each declaration's `create` function. Version zero takes empty configuration bytes. A
configurable system supplies its own encoding and validates it during construction. The candidate owns the
World, Simulation, Scene systems and the component schemas they borrow. Construction starts no work; after
`Engine::addScene` adopts the candidate, `Engine::whenSceneInitialized` reports the initialization result.
The [installed consumer](../cmake/installed-consumers/headless-engine/main.cpp) assembles and runs Transform this way.

Scene retirement completes before Context destruction. Registered providers outlive repository settlement.
Context destruction runs outside dispatch on Main and leaves Execution alive until all consumers have settled.

Each SceneSession owns one TaskScope; Systems borrow it directly and Reactions submit through their RenderSystem.
Neither AsyncMixin nor RetirementMixin is part of the framework. Engine settles the complete Scene before
releasing its task graphs and professional resources. AssetRepository owns a separate scope for shared work.
Consumers use EnTT scoped connections and their own filtered dirty sets; World owns no parallel change directory.
VisibilityComponent stores author permission and the current global range requirement. Business callbacks observe
individual writes; render collection coalesces only the state to be synchronized.
