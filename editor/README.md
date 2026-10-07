# Editor Framework v2 — Project / Scene foundation

`editor/` builds the sole installed `lux_editor`. `editor_legacy/` remains frozen reference source.
The current terminal-architecture work has completed P01–P03; the earlier PS0–PS2 Project/Scene behavior remains in use until P05 replaces the transition. It does not yet open a project Scene
in SceneRuntime, create SceneSession/EditorScene/SceneWorkspace, or install editing tools.

## Ownership and lifetime

| Owner | Responsibility |
|---|---|
| LuxEngine | Window, EngineContext, UI Scene transport, owning product assembly, optional project Context and private transition |
| EditorWindow / ui::Root | One native Input sample; Root uniquely owns mounted Panes and their UI routes |
| EditorContext | Manifest and summary, independent AssetVfs, verified PluginManager and immutable SceneRegistrations, complete runtime registries, cancellation TaskScope |
| EditorServices | Lazy unique services, destroyed in reverse successful construction order |
| EditorUiRegistry | Detached factories; createPane is implemented by lux_editor_ui, Root adopts the returned owners |
| SceneProfileRegistry | Immutable canonical profile declarations; creation returns an owned authored ScenePackage |
| Original Process / SceneRuntime / RenderRuntime | Scheduling, completion, the single frame driver and resource retirement |

Parent links still do not own C++ objects. Object/UI/Error Framework v2 contracts remain unchanged.
TaskScope destruction only requests cancellation. Accepted tasks retain their TaskGroup and own all worker inputs,
results and code leases; they must not borrow a Context, service, Pane or mutable Registry. Context destruction never
waits. The old host transition still explicitly observes settlement until its P05 replacement; that is not a Context
lifetime requirement. `join()` remains an explicit synchronous application/runtime boundary.

## Project use

```cpp
auto assembly = [](lux::editor::EditorComposition& composition) noexcept
    -> lux::editor::FrameworkResult<void>
{
    return composition.registerSceneProfile(lux::editor::sceneProfile3D());
};
auto host = lux::editor::LuxEngine::create(config, std::move(assembly));
// Check host, then request an absolute path. Success here means request admission.
auto accepted = (*host)->openProject(project_file);
// frame()/exec() collect the owning result and adopt it at the owner safe point.
```

Assembly belongs to the host lifetime, may own move-only captures, and builds EditorComposition before any candidate
Context exists. Composition exposes registration only. Its tables move into immutable runtime registries and lazy
EditorServices when a complete PreparedProject is adopted into a Context. No freeze/closing flag or incomplete public
Context constructor exists. UI/service factories receive a fully constructed Context. A factory may start accepted transport work but must not publish project effects before adoption.

`createProject({absolute_root, manifest})` creates `Project.luxproj` in an empty directory. `openProject(path)` reads
an existing manifest. Product configuration supplies plugin catalog locations and the default layout, separately
from persisted project values. The product executable supports `lux_editor project.luxproj` and
`lux_editor --create directory name`; a project-free launch displays instructions. New product projects select the
existing `lux.builtin.scene_render` runtime plugin; this is product policy, not a framework branch.

Only one transition is admitted. `projectStatus()` separates PREPARING, CLOSING_CURRENT and terminal outcomes;
`manifest_published` records confirmed creation even when subsequent plugin preparation fails or adoption is cancelled.
A publication-unknown error remains explicit. Cancellation is accepted during preparation, before old-project closure.
New requests during a transition or a host callback return BUSY.

Preparation reads and verifies plugins on the existing blocking scheduler. Completion only deposits owning values.
The host prepares all candidate UI while A remains intact. A failed read/plugin/assembly/factory leaves A active.
After candidate preparation, A stops accepting work; its Panes become noninteractive while accepted work finishes.
UI is removed before A's Context; the complete B batch is then mounted. Failed candidates drain their own accepted
work before Context destruction. EngineContext, native window, Root and ImGui Context survive every switch.
Native close cancels preparation and continues draining before teardown; late results cannot adopt a new project.

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
Transform3D, Mesh3D, Light3D and Camera author schemas; Transform, WorldLoading and Render systems; an empty Simulation;
and the material/mesh-stack/view-camera/light/forward preset. No editor camera, picking entity or runtime scene is created.
Missing capabilities and incompatible configuration fail without partial output. A test-only 2D profile uses the same
public registration/creation path without changing the host.

`ScenePackageFile` reuses the existing ScenePackage/Pak/world codecs and the same file-publication primitive as the
manifest. It preserves unknown package entries. There is no second scene format. These synchronous helpers are intended
for worker-side IO; the async scene load/adopt layer belongs to PS3–PS5.

SceneToolRegistry remains **provisional and excluded from freeze**. Its replacement with 0..N applicable contributions
is scheduled for PS6, after a real SceneSession/EditorScene composition exists. Profiles do not create tools.

## Components and verification

The installed package is `lux-engine-editor-framework` under `lux::engine::editor::`:

- `lux_editor_project`: pure project values, codec and file IO; no Process/UI/Renderer dependency.
- `lux_editor_context`: composition declarations, complete runtime directories and Process cancellation scopes; no UI or rendering implementation.
- `lux_editor_ui`: native window/input and detached UI factory invocation; no scene_render or app dependency.
- `lux_editor_app`: async transition, host and private UI Scene transport.
- `lux_editor_scene`: generic ScenePackage file IO, independent of Project and concrete profiles.
- `lux_editor_scene_profiles`: concrete 3D authoring preset.
- `lux_editor_file_io`: shared internal file primitive; only its static library is installed, not its support header.

These libraries are STATIC. The executable explicitly selects the concrete profile; Context and the host do not link
it. UI Scene transport remains private. Product-owned factory layout names are not a workspace persistence protocol.

Build the EDITOR profile, `all -j 4 -- -k 0`, with a second no-work build after CMake changes. CPU tests cover manifests,
actual plugin-driven profiles, scope completion and existing framework behavior. Explicit GPU/desktop tests cover async
project switching, native close, resize, minimization and the original UI rendering chain. Installed tests use SDK
public headers/libraries, including isolated project and context consumers and throwing-callback compile negatives.

The event-driven loop retains one input sample and one SceneRuntime drive per iteration. Backpressure retains captured
resource ownership. Accepted completions and retirement continue while a project closes. No private executor, second
runtime, global event bus or frame-hook registry is added.

The ProjectBuilder user patch remains separately archived and unapplied. Original user formatting bytes remain archived; the Context definition was replaced under the P03 contract and the untouched Pane comment change remains uncommitted. Interactive native input, IME, Linux, sanitizer and old longbench deferrals remain unchanged;
automated desktop/GPU tests do not qualify those deferred checks. Logs and command provenance remain outside source.

### PS0–PS2 correction contracts

ProjectManifest schema 1 requires `format: "lux.editor.project"`. Duplicate keys (including escaped spellings)
and unknown fields at the root or in plugin/scene records are rejected. There is no pre-freeze format fallback.
Plugin selections use the same `project_identity` grammar as PluginCatalog; scene profile/capability names use
`isCanonicalSceneName`. Project display names remain human text.

A prepared ProjectDescription binds the canonical `manifest_file`, independently of its display name. `root`
is that file's parent, including when the input follows a filesystem alias. Every constructed Context has this exact file binding; the removed incomplete bootstrap is not an SDK fallback.

LuxEngine tracks only the handles from its project mount. Switching detaches those panes, retains their owners
until the old TaskScope settles, destroys the panes while their Context is alive, then adopts the next project.
Application panes remain mounted and continue maintenance. Root has no Project grouping or Project ownership tag.
A pane removed/re-registered by an external owner has a different registration and is no longer part of that mount.

`lux_editor_scene` supplies generic ScenePackage file IO; `lux_editor_scene_profiles` supplies the concrete 3D
preset. Neither the generic scene provider nor project file IO depends on the other domain. Their domain errors
map the single internal `lux_editor_file_io` publication algorithm; its support headers are not installed.
Feature dependency order/conflicts remain owned by FeatureCatalog/RenderSystem, not by the profile.
