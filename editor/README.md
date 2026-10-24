# Editor Framework v2 — Project / Scene foundation

`editor/` builds the sole installed `lux_editor`. `editor_legacy/` remains frozen reference source.
The architecture uses typed Project Events, complete Contexts and immediate UI replacement; Scene provider retirement is explicit. It does not yet open a project Scene
in SceneRuntime, create SceneSession/EditorScene/SceneWorkspace, or install editing tools.

## Ownership and lifetime

| Owner | Responsibility |
|---|---|
| LuxEngine | Window, EngineContext, UI Scene transport, owning product assembly, optional project Context and owned pending adoption |
| EditorWindow / ui::Root | One native Input sample; Root uniquely owns mounted Panes and their UI routes |
| EditorContext | Manifest and summary, independent AssetVfs, verified PluginManager and immutable SceneRegistrations, complete runtime registries, cancellation TaskScope |
| EditorServices | Lazy unique services, destroyed in reverse successful construction order |
| EditorUiRegistry | Detached factories; createPane is implemented by lux_editor_ui, Root adopts the returned owners |
| SceneProfileRegistry | Immutable canonical profile declarations; creation returns an owned authored ScenePackage |
| ExecutionRuntime / SceneRuntime / RenderExecution | Scheduling, completion, the single frame driver and resource retirement |

Parent links still do not own C++ objects. Object/UI/Error Framework v2 contracts remain unchanged.
TaskScope destruction only requests cancellation. Accepted tasks retain their TaskGroup and own all worker inputs,
results and code leases; they must not borrow a Context, service, Pane or mutable Registry. Context destruction never
blocks. A Context with adopted Scenes first closes those leases and retains its providers until their cleanup settles;
the application continues completion and maintenance-frame processing throughout this phase. Only then does it close
the project TaskScope. Final application shutdown joins producers after accepted CPU, Render and protocol work settles.

## Project use

```cpp
auto assembly = [](lux::editor::EditorComposition& composition) noexcept
    -> lux::editor::FrameworkResult<void>
{
    return composition.registerSceneProfile(lux::editor::sceneProfile3D());
};
auto host = lux::editor::LuxEngine::create(config, std::move(assembly));
// Check host, then request an absolute path. Success here means request admission.
lux::editor::OpenProjectRequest request{project_file};
auto delivered = lux::object::sendEvent(**host, request);
// Check delivery and request.rejection, then enter the sole public host loop.
auto result = (*host)->run();
```

Assembly belongs to the host lifetime, may own move-only captures, and builds EditorComposition before any candidate
Context exists. Composition exposes registration only. Its tables move into immutable runtime registries and lazy
EditorServices when a complete PreparedProject is adopted into a Context. No registration freeze or incomplete public Context constructor exists. Scene retirement is a separate explicit lifetime phase. UI/service factories receive a fully constructed Context. A factory may start accepted transport work but must not publish project effects before adoption.

`CreateProjectRequest{absolute_root, manifest}` creates `Project.luxproj` in an empty directory. `OpenProjectRequest{path}` reads
an existing manifest. Product configuration supplies plugin catalog locations and the default layout, separately
from persisted project values. The product executable supports `lux_editor project.luxproj` and
`lux_editor --create directory name`; a project-free launch displays instructions. New product projects select the
existing `lux.builtin.scene_render` runtime plugin; this is product policy, not a framework branch.

LuxEngine is an Object Event receiver. OpenProjectRequest/CreateProjectRequest carry owned inputs; CloseProjectRequest
and the application close/cancel commands express intent. Newer requests invalidate older serials and request stop;
accepted completion remains owned by Process/ObjectScheduler and is always settled. No public transition/status API exists.
projectChanged and projectOpenFailed are facts, not progress polling. A confirmed created manifest remains on disk even
when subsequent plugin preparation fails. Cancellation and stale completion do not emit a false open failure.

Preparation runs on the existing blocking scheduler with owned paths/configuration. ObjectScheduler protects the complete
owner continuation. The Event builds a complete detached candidate while A remains intact; failed candidates clean up
immediately without waiting for their cancellation scopes. Only the host safe point calls Root::replacePanes. Its commit
publishes the new Context/mount before notifications; old Panes are destroyed before their old Context, without joining
workers. Global UI remains mounted, and late requests from notifications enter the next batch. Window, Root, ImGui and
EngineContext survive project switches. Object callbacks may assemble/clean unrelated detached candidates, but cannot
mutate an attached UI tree or move an already-parented Object relation during dispatch.

Context construction is private and requires verified PluginManager and SceneRegistrations values. Source tests use a
noninstalled complete-preparation fixture. Installed consumers use the same public host/project path as the product;
they cannot obtain an incomplete Context through a test-only SDK overload.

## Manifest and scene profiles

ProjectManifest version 1 persists project UUID/name, plugin name/version selections, and scene AssetId/name/relative
file/profile records with an optional startup AssetId. It does not persist runtime handles, factories, tool state,
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
verified project schema/system/feature registrations and their codecs to create a single-partition world with Parent,
Transform3D, Mesh3D, Light3D and Camera author schemas; Transform, WorldLoading, MeshQuery and Render systems; an empty Simulation;
and the typed deferred-rendering Scene projection. No editor camera, picking entity or runtime scene is created.
Missing capabilities and incompatible configuration fail without partial output. A test-only 2D profile uses the same
public registration/creation path without changing the host.

`scene_asset` supplies `lux::scene::ScenePackageFile` functions using the existing ScenePackage/Pak/world codecs and
the installed `platform_file` publication primitive. It preserves unknown package entries. There is no second scene format. These synchronous helpers are intended
for worker-side IO; ProjectScenes schedules read/decode/adoption and snapshot save on the shared execution resources.

Tools use the actual SceneDocument, ComponentSchema and product UI factories. Profiles do not create tools;
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

The event-driven loop retains one input sample and one SceneRuntime drive per iteration. Backpressure retains captured
resource ownership. Accepted completions and retirement continue while a project closes. No private executor, second
runtime, global event bus or frame-hook registry is added.

Interactive native input, IME and other platforms require their own qualification; automated desktop/GPU tests do not
qualify them. Exact sanitizer coverage, commands and raw results are retained outside source and bound to a commit.

### PS0–PS2 correction contracts

ProjectManifest schema 1 requires `format: "lux.editor.project"`. Duplicate keys (including escaped spellings)
and unknown fields at the root or in plugin/scene records are rejected. There is no pre-freeze format fallback.
Plugin selections use the same `project_identity` grammar as PluginCatalog; scene profile/capability names use
`isCanonicalSceneName`. Project display names remain human text.

A prepared ProjectDescription binds the canonical `manifest_file`, independently of its display name. `root`
is that file's parent, including when the input follows a filesystem alias. Every constructed Context has this exact file binding; the removed incomplete bootstrap is not an SDK fallback.

LuxEngine tracks only the handles from its project mount. Switching atomically replaces those panes, publishes the new Context before notification, then destroys the old
panes before retiring the old Context. Adopted Scene leases keep their providers until asynchronous cleanup settles;
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

`run()` is the only public event-loop entry. Single-step access belongs to a noninstalled test support header;
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

## Shared rendering and project retirement

The app owns one RenderExecution and RenderAssets, sharing EngineContext::execution(). The Asset protocol
thread owns only its SPSC Link; IO and CPU decoding use the existing bounded blocking/CPU resources.
An independent Render UI Scene presents the native window; it does not create an empty Engine World.
All Engine Scene commands and UI draw lists join the same Main `FrameInput` before one SubmitFrame.

Project factories obtain RenderExecution/RenderAssets from ContextExtensions and their own typed read port
from EditorContext::prepareAssetReads(). Supply that port to RenderSystemDependencies; use a distinct source namespace when
opening another project generation. Build with the existing SceneRuntime, then transfer its SceneInstanceLease
to EditorContext::adoptScene(). Keep the Scene providers and view inputs in project services.

Project replacement removes Panes immediately. A Context with adopted Scenes remains in the app's closing
list until Scene retirement receipts and its accepted cleanup tasks settle; no new GPU scope is involved.
A Context without Scene borrows keeps the original immediate, nonblocking destruction contract. Worker
operations still own their captured inputs and code instead of borrowing a destroyed Context.

Viewport code supplies `UiTargetImage` values through LuxEngine::setUiImages. The UI owns only sampling
relations. After removal, wait for uiImagesApplied before destroying the old output Target through the Main
Client; its actual creator remains responsible for that resource. Scene/Target editing tools are separate
product work, not manufactured by this transport layer.

## Model import and Scene placement

The product registers ProjectAssets before ProjectScenes. ProjectAssets opens `.luxassets/catalog.json`,
cooks finite model requests on the shared CPU scheduler, reads/writes through the bounded blocking scheduler,
and publishes immutable LUXPAK products before replacing the catalog. Model subasset identities remain stable
across reimport; ambiguous source identities fail. AssetBrowserPane displays this catalog and sends a small
copied AssetId/revision drag payload. SceneViewportPane creates one author SceneEdit through ModelPlacement;
RenderSystem performs the asynchronous GPU projection.

RenderAssetInput publishes one captured VFS table with its asset revision. Material and texture loads use
that same snapshot, while RenderAssetVersion distinguishes project generations. A failed unadopted upload
candidate retains the existing Scene binding; it does not clear the old picture first. RenderSystem exposes
per-entity pending/error queries for tools without exposing RenderServer or Storage instances.

The current viewport places imported models on the world Y=0 plane. General picking, Inspector, author save
and isolated Play belong to the Scene tools workflow; their implementation status is recorded separately,
not inferred from successful importing. Old published product files are retained until a reference-aware
cleanup is provided; importing never deletes a prior published package that a reader may still need.

Camera outputs are runtime components on ordinary ECS CameraMan entities. ProjectScenes owns each viewport
Target and temporary editor camera; RenderSystem owns the derived View/Effect/Attachment records. The editor
camera has no author WorldObjectId and is excluded from save/Undo/Play author snapshots. Picking reads the same
selected ECS camera and adopted image aspect that rendering uses.

A viewport keeps its image token while Resize is pending. ImageElement fits the last adopted physical aspect
inside its logical rectangle; framebuffer scale is applied exactly once when requesting pixels. Resize success
is resumed in a complete frame carrying matching camera/UI data and the replied Target epoch.

Application startup publishes a fully constructed LuxEngine::Impl; partial startup rollback is local to its factory.
OpenProject retains its Context until Scene/UI retirement completes. The regular loop and destructor share one
nonblocking close progression. Clients drain and close before RenderServer stops; the shared ExecutionRuntime is
joined only outside a driven callback. There is no Main 1024-request limit or per-frame reserved correlation slot.
