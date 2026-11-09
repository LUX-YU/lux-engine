# Editor Framework v2 — Project / Scene foundation

`editor/` builds the sole installed `lux_editor`. `editor_legacy/` remains frozen reference source.
The architecture uses typed Project Events, complete Contexts and atomic UI replacement. ProjectScenes opens authored Scenes through SceneRuntime; scene provider retirement is explicit.

Rendering integration is currently withdrawn pending the new Reaction/Collector implementation.
There is no installed UI rendering Scene or scene viewport factory in the application composition.
Project loading, authoring widgets and asset import remain source components; this is not a claim that
Editor runs against the redesigned Engine. ImguiFrameCapture remains an independent frame-data encoder.
Older Engine lifecycle consumers and their tests still require a separate migration.

## Generated value editors

`editor/ui` owns the typed widget functions and `value_editor` Inja templates. A consuming target supplies its
own parsing input and companion policy through `editor_target_add_value_codegen`. The builtin profiles use
`LUX_REFLECT_EXTERNAL` in `BuiltinValueTypes.hpp` to select lower-layer types and `ValueEditing.json` for control
labels and bounds. Engine/Render declarations contain no Editor annotations. Policy references are checked
during generation; typed control constraints are checked when the generated C++ is compiled.

Generated `.value_editor.cpp` functions compile into the consuming target and access members directly. There is
no per-frame metadata lookup or field-container construction. `ComponentEditor` and `SceneConfigurationEditor`
remain the composition interfaces. Fields edit an owned draft; existing component transactions and configuration
codecs validate and adopt the result. UI bounds neither define persistence nor replace domain validation.

EditorContext remains independent of ImGui. Its provider records retain callable code; the UI implementation and
specialized AssetId/Quaternion controls live in `lux_editor_ui`. Unknown types preserve their value and render a
read-only explanation rather than guessing a default.

## Ownership and lifetime

| Owner | Responsibility |
|---|---|
| EditorApplication | Window, Engine, UI Scene transport, owning product assembly, one loaded project Context and finite candidate preparation |
| EditorWindow / ui::Root | One native Input sample; Root uniquely owns mounted Panes and their UI routes |
| EditorContext | Manifest and summary, independent AssetVfs, verified PluginManager and immutable SceneRegistrations, complete runtime registries, cancellation TaskScope |
| EditorServices | Lazy unique services, destroyed in reverse successful construction order |
| EditorUiRegistry | Detached factories; createPane is implemented by lux_editor_ui, Root adopts the returned owners |
| SceneProfileRegistry | Immutable canonical profile declarations; creation returns an owned authored ScenePackage |
| Engine | Shared completion dispatch, Scene drive, frame handoff, platform wait and final execution shutdown |
| ExecutionRuntime / SceneRuntime / RenderExecution | Scheduling, actual Scene ownership and retirement, protocol/frame transport |

Parent links still do not own C++ objects. Object/UI/Error Framework v2 contracts remain unchanged.
TaskScope destruction only requests cancellation. Accepted tasks retain their TaskGroup and own all worker inputs,
results and code leases; they must not borrow a Context, service, Pane or mutable Registry. Context destruction never
blocks. A Context with associated Scenes first awaits its declared services and Scene retirement while retaining their providers;
the application continues completion and maintenance-frame processing throughout this phase. Only then does it close
the project TaskScope. Final application shutdown joins producers after accepted CPU, Render and protocol work settles.

## Project use

```cpp
config.project_file = project_file; // Optional absolute path. Empty starts project selection.
auto application = lux::editor::EditorApplication::create(std::move(config), std::move(assembly));
// Check the factory result. create starts asynchronous loading; it does not promise a project yet.
auto outcome = (*application)->run();
// The event loop returns only after accepted cleanup and execution join; then ordinary destruction is safe.
```

EditorApplication keeps its Window and project state across project switches. An empty startup
path shows ProjectCreationPane in SELECTING_PROJECT. `project()` is borrowed only after project publication; `ready()`
means RUNNING. The public state distinguishes selecting, loading, cleanup failure, running and closing. Only the private
transition function writes application mode; callers send intent through Object events.

Assembly owns its move-only captures through application shutdown. It registers factories into EditorComposition;
constructed Contexts receive immutable declarations and one EditorServices table. Optional `initializeProject()` operations
run in actual dependency construction order. A manifest startup Scene requires exactly one registered
`initializeStartupScene(AssetId)` provider (the built-in ProjectScenes), which uses the existing open/readiness path.
No startup Scene means an empty editable project. Required services initialize before detached Pane factories execute.

OpenProjectRequest/CreateProjectRequest follow one transaction. Manifest/plugin/layout IO runs on the shared blocking
scheduler; Context/Scene/UI construction and publication run on Main. Only one transaction is admitted: another load is BUSY.
Cancellation waits for actual reads and candidate cleanup. Root atomically replaces project Panes and their validated
placement before structural notifications. Application UI state is projected through Root::deferChange, never by changing
mounted Pane modality in an ordinary event callback.

`projectChanged` reports the publication of the new identity. `projectLoadCompleted(ProjectLoadOutcome)` is the exactly-once
terminal result after old project/candidate cleanup. It records request identity, path, error, manifest publication
(UNCHANGED/PUBLISHED/UNKNOWN), and whether the running project was published. A later cleanup failure does not undo a
published project or delete a successfully written manifest. RetryProjectCleanupRequest retries the retained cleanup
without repeating the original load terminal. No new load is admitted while that owner remains unresolved.

During loading the previous targets remain available, author commands are blocked by the modal/application admission
boundary, and simulation clocks are suspended without overwriting per-Scene Play/Pause state. Completion/publication,
asset upload, rendering and modal UI continue. First presentation and all asset streaming are not promises of load success.

CloseProjectRequest requests Engine shutdown. The project session, UI Scene, asset releases and remaining facilities
register their actual cleanup operations with dependency ordering. Engine stops shared execution after they finish. Failed cleanup retains its objects and reports CLOSE_FAILED; an explicit
RetryApplicationCloseRequest resumes unfinished work. The final result preserves earlier device/business errors even when
physical cleanup succeeds. EditorApplication destruction is not an implicit event loop or recovery driver.

Context construction remains private and accepts verified PluginManager/SceneRegistrations. Tests use existing internal
preparation fixtures or the public asynchronous host path; no incomplete SDK constructor or compatibility startup path remains.

## Manifest and scene profiles

ProjectManifest version 1 persists project UUID/name, plugin name/version selections, and scene AssetId/name/relative
file/profile records with an optional startup AssetId and project-relative editor_layout reference. It does not persist runtime handles, factories, tool state,
absolute asset paths or an additional scene identity. Empty projects are valid. Scene paths are physical project-relative
paths, distinct from asset VFS paths. Validation rejects traversal, aliases, duplicate identities and unsupported versions.

`lux_editor_project` supplies bounded JSON read/encode and atomic single-file CREATE/REPLACE publication. Temporary
files are exclusive and adjacent; cancellation is observed before publication. File/directory errors are not absence.
This is not a cross-file transaction, versioned save service or alternate write coordinator.

A profile owns canonical id/display/capability values and a noexcept factory, with an optional provider code lease.
Registration belongs to EditorComposition; runtime lookup/enumeration are stable owner-thread borrows. Copying a declaration
retains its code. Creation synchronously borrows immutable SceneRegistrations and returns owned ScenePackage values;
a worker must own those registrations for the whole call, not borrow a live Context.

`sceneProfile3D()` contributes `lux.editor.scene.3d` and spatial.3d/transform.3d/mesh.3d/camera.3d capabilities. It uses
verified project schema/system registrations and their codecs to create a single-partition world with Parent,
Transform3D, Mesh3D, Light3D and Camera author schemas; Transform, WorldLoading, MeshQuery and Render systems; an empty Simulation;
and an explicit composition of raster, shadows, clustered lighting, deferred lighting and output contributions. Explicit
selections delegate to the shared `createSelectedScene` algorithm rather than the 3D preset. No editor camera, picking entity or runtime scene is created.
Missing capabilities and incompatible configuration fail without partial output. A test-only 2D profile uses the same
public registration/creation path without changing the host.

`scene_asset` supplies `lux::scene::ScenePackageFile` functions using the existing ScenePackage/Pak/world codecs and
the installed `platform_file` publication primitive. It preserves unknown package entries. There is no second scene format. These synchronous helpers are intended
for worker-side IO; ProjectScenes schedules read/decode/adoption and snapshot save on the shared execution resources.

Tools use the actual SceneDocument, ComponentSchema and product UI factories. EditorComposition registers
ComponentEditor callbacks and SceneConfigurationEditor widget factories; EditorContext publishes stable borrows.
The generic Inspector owns DecodedComponent drafts and uses revision-checked Apply/Revert. Product assembly
registers ComponentEditors3D and RenderConfigurationEditor; adding another component editor does not modify the Pane.
The creation widget selects geometry/domain/partition and independent Systems from registered contracts.
`SceneConfigurationForm` owns a typed draft and emits edits; selection changes prepare one revision-tagged candidate.
Create consumes that exact candidate and cached Builder qualification. The Render form provides contribution/port editing,
while the selected System's preparation performs the actual capability/codec/data-input qualification.
Unknown configuration remains intact and explicitly read-only. Profiles do not create tools;
there is no World-name-matching SceneToolRegistry or frozen runtime tool directory.

## Components and verification

The installed package is `lux-engine-editor-framework` under `lux::engine::editor::`:

- `lux_editor_project`: pure project values, codec and file IO; no Process/UI/Renderer dependency.
- `lux_editor_context`: composition declarations, complete runtime directories and Process cancellation scopes; no UI or rendering implementation.
- `lux_editor_ui`: Editor window policy and detached UI factory invocation; no scene_render or app dependency.
  Ordered window input translation lives in `lux::ui::feedWindowInput`; native surface conversion lives in Engine Rendering.
- `lux_editor_app`: typed Project Events, safe-point adoption, host and private UI Scene transport.
- `lux_editor_scene_profiles`: concrete 3D authoring preset.

These libraries are STATIC. The executable explicitly selects the concrete profile; Context and the host do not link
it. UI Scene implementation remains private; UiTargetImage is the installed token/Target value contract. Product-owned factory layout names are not a workspace persistence protocol.

Build the EDITOR profile, `all -j 4 -- -k 0`, with a second no-work build after CMake changes. CPU tests cover manifests,
actual plugin-driven profiles, scope completion and existing framework behavior. Explicit GPU/desktop tests cover async
project switching, native close, resize, minimization and the UiRasterFeature and the common complete-frame transport. Installed tests use SDK
public headers/libraries, including isolated project and context consumers and throwing-callback compile negatives.

Engine owns the single event loop, with one input sample and one SceneRuntime drive per iteration. Backpressure retains captured
resource ownership. Accepted completions and retirement continue while a project closes. No private executor, second
runtime, global event bus or frame-hook registry is added.

Interactive native input, IME and other platforms require their own qualification; automated desktop/GPU tests do not
qualify them. Exact sanitizer coverage, commands and raw results are retained outside source and bound to a commit.

### PS0–PS2 correction contracts

ProjectManifest schema 1 requires `format: "lux.editor.project"`. Duplicate keys (including escaped spellings)
and unknown fields at the root or in plugin/scene records are rejected. There is no pre-freeze format fallback.
Plugin selections use the same `plugin_identity` grammar as PluginCatalog; scene profile/capability names use
`isCanonicalSceneName`. Project display names remain human text.

A prepared ProjectDescription binds the canonical `manifest_file`, independently of its display name. `root`
is that file's parent, including when the input follows a filesystem alias. Every constructed Context has this exact file binding; the removed incomplete bootstrap is not an SDK fallback.

EditorApplication tracks only the handles from its project mount. Switching atomically replaces those panes, publishes the new Context before notification, then destroys the old
panes before retiring the old Context. Scene retirement receipts keep the project providers alive until actual asynchronous cleanup settles;
project switching does not block on a TaskScope or stop the application's shared execution resources.
Application panes remain mounted and continue maintenance. Root has no Project grouping or Project ownership tag.
A pane removed/re-registered by an external owner has a different registration and is no longer part of that mount.

`scene_asset` supplies generic ScenePackage file IO; `lux_editor_scene_profiles` supplies the concrete 3D
preset. Neither the generic scene provider nor project file IO depends on the other domain. Scene errors retain
the actual codec or `platform::FileFailure` cause. Platform file headers are installed independently of Editor;
Pak and asset publication use the same streaming `AtomicFileWriter` implementation.
Feature dependency order/conflicts remain owned by FeatureCatalog/RenderSystem, not by the profile.

## Host boundary

Each component owns its error catalog: `ProjectErrors`, `ContextErrors`, `EditorUiErrors`, and `AppErrors`.
Explicit assembly registers only the catalogs it uses; the host assembles all four. Failure paths construct the
same stable numeric errors and never register descriptors. There is no aggregate FrameworkErrors header or
registration function. Project's catalog links only its own component and the core error provider, not Context.
Independent installed consumers qualify registration, idempotence, collisions and descriptor ownership.

`EditorApplication::run()` delegates to Engine. Engine exposes the same driver as `step()`; Editor single-step access belongs to a noninstalled test support header;
there is no public frame/exec or host profiling API. Pacing tests measure their own elapsed time and query original
component statistics. EditorWindow owns the native resize/framebuffer/minimize slots and exposes cached metrics;
the host synchronizes UI extent only when their revision changes.

The installed consumer uses public run/Project Events, not private pumping. Granular source tests retain their
safe-point, queued-stale, callback and shortcut assertions through private test access. SDK tests separately verify
complete Contexts, the same service/VFS assertions, blocked-worker replacement, global UI, candidate rollback,
metrics, real rendering and final posted-completion drain. A minimal consumer requests only the app component.

## Action and menu composition

The product's Scene workflow uses the same commands: **Edit → Save Scene**, **Undo/Redo**, **Play Scene**,
**Play Headless** and **Stop Play**. Open the Scenes tab, create or open a Scene, import a model in Assets,
then drag it into a viewport. The default 3D preset includes the existing MeshQuery system. Clicking geometry
selects its authored object; Hierarchy and Inspector share that selection. Inspector applies a complete typed
component draft with its captured revision. Unknown component editors remain read-only without discarding data.

The viewport's three translation axes publish normal author updates while dragging and create one undo entry.
Escape cancels a drag; Ctrl+Z undoes its complete result. Rotation and scale are editable in Inspector. A stale
Inspector draft requires **Revert / refresh draft**; it never silently overwrites newer values. Shader/material
graph editing, indexed-world placement generation and production game compilation remain separate work.

**Observe Play instance** chooses the independently running Registry for a viewport. Stop returns it to the
author instance after withdrawing the Play observation. Headless preview runs Scene/Simulation without creating
a project RenderSystem; the Editor's own UI remains graphical. The installed CPU-only Engine consumer checks
the separate no-graphics build closure.

Product panes use Root's existing docking tree. Asset/Hierarchy rows are derived caches; stable frames do not
encode the Registry or rebuild the hierarchy. Hierarchy regeneration follows structural changes, while async
asset readiness is adopted at the existing Scene safe point.

`lux_editor_ui` provides the pure `composeEditorMenu(EditorMenuComposition)` algorithm. Products and external
extensions append owning action, menu, group and placement declarations; they do not register handlers here.
The product contributes project and editing commands through this same public input instead of assembling a
complete hard-coded menu tree. `EditorComposition`/Context does not acquire a dependency on UI for this purpose.

Menu and group before/after anchors address siblings. Action anchors address placements in the same menu,
including other groups. Explicit edges win; ready placements use group rank, descending priority and canonical
ID as tie-breakers. A group transition inserts a separator. Nested menus precede a parent's action groups.
Unknown names, duplicate placements and cycles return owning diagnostics, never depend on extension load order.
The result is an owned generic UI MenuDefinition; Root separately validates presentation/action references before
adoption. There is no mutable command registry, plugin callback, runtime enabled/checked cache or second dispatcher.

Menu placement/order and descriptor shortcut customization leave CommandId and its routed handler unchanged.
Descriptor values can outlive their declaring DLL; no plugin code is required to interpret them. The receiver's
own endpoint still governs business execution and lifetime. `cmake/installed-consumers/ui-actions` exercises the
public contract, including actual DLL declarations and UI input after that DLL has unloaded.

## Asset import and professional consumers

ProjectAssets owns directory facts and VFS mounts. Register `AssetImportDeclaration` values with
`declareAssetImporter<T>()`; the concept binds a typed configuration codec and finite Cook operation.
The generic transaction owns publication and catalog conflict checks. `ModelImporter` owns model source
discovery, Cook and product encoding, in `lux_editor_model_import`; the generic assets/app targets do not
link the model cooker through this path. One import may publish several asset types.

Catalog version 2 stores importer name/version, opaque configuration, root type and typed product identities.
Products inherit their record's revision and immutable package location. Version-1 model directories migrate
without decoding payloads. Dependency types absent in that old format remain explicitly unknown (zero) until
reimport; they are never used to choose a decoder. Unknown importers preserve configuration and products, and
reimport reports unavailability. Package opening retains its header/index roots, not decoded Model contents.

AssetBrowserPane lists generic AssetId/type/revision values. Configuration forms are generated by the Editor's
Inja projection and matched by importer identity/version plus C++ configuration type. A form owns one decoded
draft; encoding occurs on explicit import. Toolchain configuration contains no UI annotations. Specialized
consumers interpret generic drag values; model placement uses `loadAsset<ModelAsset>` on demand, then resolves
both tool and Document generations and rechecks author/asset revisions before adopting one SceneEdit.

`MeshQueryAssets` owns the project's requested Mesh/version BVH cache and its actual Scene subscriptions.
It folds in existing project Scenes and subscribes to Engine's created fact. Reads use AssetRepository, decoding
and BVH building use the shared CPU scheduler, and adoption uses the Scene edit safe point. Reimport invalidates
only affected cached meshes; a repeated request shares the same result. The Scene close participant disconnects
the stable MeshQuery borrow on Main before MeshQuerySystem destruction, including a failed Scene where author
edits are correctly forbidden. Late work resolves ObjectId/SceneInstanceId and version before adoption.

## Completion-driven editing and updates

ProjectScenes starts finite document/Play operations. SceneViewport starts only the affected output's operation;
Camera selection, extent and close intent invalidate that output. MeshQueryAssets delivers geometry to actual
subscribers. Unchanged documents and
viewports are not scanned by the app loop. SceneRuntime::editInstanceAsync applies its borrowed callback at
the actual structural safe point before delivering completion. whenEditable is only a notification: it does
not grant an edit permit that remains valid across later callbacks.

Hierarchy and Inspector request UI updates on document/selection facts; Inspector keeps the original draft
revision and reports stale edits. Static
Pane/Element update callbacks run only after requestUpdate; there is no static hierarchy-wide update pass.
