# SV-1 scene workbench

`editor_scene` owns the non-project development scene used by `lux_editor`.
It uses the existing Scene Registry, TransformSystem, RenderSystem and
RenderSyncPipeline. The application supplies one borrowed SceneViewRenderPort
from its existing Vulkan presentation owner. No second renderer or device is
created. Production Pane callbacks change selection, filtering and layout;
the Inspector uses generated const callbacks and does not provide editing.

Build with `LUX_BUILD_PROFILE=EDITOR`, `LUX_BUILD_PACKED_RENDER_CONTENT=ON`
and `RelWithDebInfo`. Build `lux_scene_seed` in the TOOLCHAIN profile, run
`lux_scene_seed output.luxpak`, and configure the editor with
`LUX_EDITOR_SEED_PAK` pointing to that file. The seed uses the existing asset
Pak format, mesh asset serialization and material cooker. `--omit-ground`
produces a package for testing a missing asset.

Run `lux_editor --validation`, optionally with `--assets path/to/file.luxpak`.
The default package location is `../share/lux-engine/editor/sv1.luxpak` beside
the installed executable. Runtime dependencies include the configured lux-cxx
and vcpkg distributions. The SDK consumer uses C++20, matching this build.

Controls: right mouse plus WASD/QE flies, middle mouse pans, wheel moves,
F focuses the selected entity's bounds, Home resets. Text entry, modal UI and
loss of window focus block/release camera capture. An OS input method may
consume letter keys before GLFW; use its English input state for these keys.
Panels use fixed regions with draggable splitters. Restore panels/layout
returns to the default arrangement. Persistent project layout and full docking
are outside this version.

Ownership and shutdown:

1. SceneWorkbench owns Scene, selection registration, resource jobs, view and
   target leases. Resource jobs retain all received handles, including failed
   replies, until their release step. Adoption checks scene generation, full
   entity identity, both source asset IDs and request serial.
2. UI snapshots retain a CPU lifetime lease. Their opaque texture token is
   resolved on the render thread against the current target generation,
   backing revision and actual FrameStamp slot. Newly allocated slots cannot
   be sampled before rendering establishes their layout.
3. Descriptor entries retire before their image views. Offscreen old backing
   uses the existing fence-proven completion watermark. No per-frame waitIdle
   is added.
4. Close stops new snapshots, drains existing submissions and pending resource
   replies, closes target/view, releases uploaded assets, deactivates selection
   and destroys Scene. The application joins the renderer before releasing
   remaining owners on an error exit.

Tests cover camera math/capture, actual UISession layout and input, retained
RenderSyncPipeline updates under real RenderProgramSession backpressure,
resource identity rejection and read-only Inspector callbacks. The installed
consumer is under `cmake/installed-consumers/editor-scene`.

With BUILD_TESTING enabled, `--verify-scene directory` runs a private diagnostic
owner: capture the initial scene, change transform/light, focus camera, remove
visual components, then clear the registry. It writes five GPU PPM readbacks
and the existing renderer GPU timing report, then closes. These mutations and
readbacks are not production Pane editing or a per-frame display path.

SV-1 does not register a Scene EditHistoryTarget. History explicitly reports
not connected. Business editing, project save, gameplay and SV-2 are not enabled.
