#pragma once
/**
 * @file LuxEditor.hpp
 * @brief Top-level editor shell.
 *
 * Owns every long-lived runtime piece the lux-engine editor needs:
 *   - GLFW runtime + window
 *   - ImGui-driven UI system (main-thread) and the per-frame UIRenderSession
 *   - Render server thread (UIRenderServer) reached through a shared channel
 *   - Process-wide AssetManager (survives scene swaps)
 *   - The default editor panels (AssetBrowser, InspectorPanel, SceneViewport)
 *   - The currently loaded `EditorScene` (one at a time; swappable)
 *
 * Lifetime:
 *   ctor          — store config only; no GPU / threads spun up yet
 *   init()        — bring up window, render thread, session, default panels,
 *                   and the initial EditorScene
 *   addPanel(...) — optional; must be called after init() and before run()
 *   run()         — block in the main loop until the window is closed
 *   dtor / shutdown — tear down the scene, stop the render thread, release
 *                     everything
 */

#include <lux/engine/editor/visibility.h>
#include <lux/engine/editor/panels/ToastQueue.hpp>   // transient notifications
#include <lux/engine/editor/app/StateRegistry.hpp>   // shared editor state table
#include <lux/engine/editor/app/Selection.hpp>
#include <lux/engine/editor/app/EditorEvents.hpp>   // EditorEvents (content_changed)
#include <lux/engine/editor/app/PanelRegistry.hpp>   // toggleable-window catalogue
#include <lux/engine/asset/Asset.hpp>   // asset_id_t for spawnModelEntity
#include <lux/engine/render/comm/FrameProgram.hpp>
#include <lux/engine/render/core/FeatureHandle.hpp>
#include <lux/engine/render/core/RenderSceneId.hpp>
#include <lux/engine/render/renderer/features/gizmo/LineListOperation.hpp>
#include <lux/engine/render/renderer/features/grid/GridOperation.hpp>
#include <lux/engine/render/renderer/features/sky_box/SkyboxOperation.hpp>
#include <lux/engine/render/renderer/features/postprocess/TonemapOperation.hpp>
#include <lux/engine/render/renderer/features/skinning/SkinningOperation.hpp>
#include <lux/engine/render/renderer/features/light/LightOperation.hpp>
#include <lux/engine/render/comm/server/FeatureRegistry.hpp>   // name-keyed feature op store
#include <lux/engine/ui/ImGuiCommConfig.hpp>
#include <lux/cxx/event/Connection.hpp>   // ScopedConnection for file-drop sub

#include <entt/entt.hpp>   // entt::entity returned by spawnModelEntity

#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace lux::window   { class GlfwRuntime; class LuxWindow; }
namespace lux::asset    { class AssetManager; }
namespace lux::exec     { class EngineExecutor; }
namespace lux::input { class ActionMapper; class InputContextStack; }

namespace lux::ui
{
    class Panel;
    class UISystem;
    class UIRenderSession;
    class SceneViewportPanel;
} // namespace lux::ui

// `lux::editor::Project` is an alias for `lux::project::Project` (the real
// class lives in engine/project/). Forward-declaring `class Project;`
// here would conflict with the alias — pull the shim in instead.
#include <lux/engine/editor/project/Project.hpp>

namespace lux::editor
{
    class AssetBrowser;
    class EditorBuiltins;
    class EditorScene;
    class HierarchyPanel;
    class InspectorPanel;
    class MaterialGraphPanel;
    class SceneFeatureSettingPanel;
    class ThumbnailService;
    class PreviewScene;
    class AssetRegistry;
    class EditorTextureCache;
    class EditorMenuBar;
    class ImportController;
    class SceneController;
    class ProjectController;
    class RenderThreadOrchestrator;
    struct BringUpConfig;

    /// Render-side infrastructure created once at editor startup, before
    /// the render thread enters its tick loop. Lives for the entire
    /// editor process — scene swaps (File → Open / Switch Project) reuse
    /// the same scene_id / view / features and only replace the World
    /// content (entity tree).
    ///
    /// Populated by `renderThreadMain` via the server's same-thread
    /// init APIs (`addFeatureFactory`, `createScene`, `addUIView`), all
    /// invoked BEFORE `server_ready_` flips to true. By the time the
    /// main thread sees a ready server, every handle below is valid.
    struct EditorRenderInfra
    {
        lux::render::RenderSceneId              scene_id{};
        lux::render::ViewHandle                 main_view{};

        // Name-keyed feature registry: every feature's per-scene handle + dynamic
        // op-ids, looked up BY NAME — reg.handle("Tonemap") / reg.ops<LightOperationIds>
        // ("Light"). Replaces the old per-feature handle/op fields + the magic
        // createScene array indices. Plugins append their own feature here.
        lux::render::FeatureRegistry            feature_registry;
    };

    struct EditorConfig
    {
        int                   width                    = 1600;
        int                   height                   = 900;
        std::string           title                    = "Lux Editor";

        /// Working directory the AssetBrowser scans for .luxasset files.
        /// Empty => use std::filesystem::current_path() at init().
        std::filesystem::path project_root{};

        /// Forwarded to Vulkan ServerConfig::enable_validation.
        bool                  enable_vulkan_validation = false;
    };

    /**
     * @brief The editor application shell.
     *
     * Construct on the main thread, call init() once, then run().
     * Not copyable; not movable (holds a live thread + window).
     */
    class LUX_EDITOR_PUBLIC LuxEditor
    {
    public:
        explicit LuxEditor(EditorConfig config);
        ~LuxEditor();

        LuxEditor(const LuxEditor&)            = delete;
        LuxEditor& operator=(const LuxEditor&) = delete;
        LuxEditor(LuxEditor&&)                 = delete;
        LuxEditor& operator=(LuxEditor&&)      = delete;

        /// Spin up GLFW, the window, UISystem, the render thread, the default
        /// panels, and the initial scene. Returns false on any failure (a
        /// diagnostic is written to stderr). Must be called exactly once.
        [[nodiscard]] bool init();

        /// Register an extra panel. Ownership stays with the caller; the
        /// panel must outlive the editor (or be removed before destruction).
        void addPanel(lux::ui::Panel* panel);

        /// Main loop. Blocks until the window is asked to close (either by
        /// the OS / user, or by requestQuit()). Returns the process exit
        /// code (0 on a clean shutdown).
        int run();

        /// Ask the main loop to exit at the next iteration. Thread-safe.
        void requestQuit() noexcept;

        // ── Project management ────────────────────────────────────────

        /// Open a project from a `.luxproject` manifest path. Closes any
        /// currently-open project first. If the manifest defines a
        /// default scene that exists on disk, also opens it; otherwise
        /// the scene comes up empty (just the editor camera + reference
        /// grid) and the user opens/creates content.
        ///
        /// Returns false on parse failure / file-not-found.
        [[nodiscard]] bool openProject(const std::filesystem::path& luxproject_file);

        /// Create + open a fresh project under `root`. Folder skeleton
        /// (`Content/`, `Scenes/`, `Source/`, `Plugins/`, `Config/`,
        /// `.lux/`) + manifest are written by `Project::newOnDisk`.
        [[nodiscard]] bool newProject(const std::filesystem::path& root,
                                       std::string_view              project_name);

        /// Close the current project + tear down the current scene. The
        /// editor stays running with no scene (viewport is blank). Safe
        /// to call when no project is open.
        void closeProject() noexcept;

        /// Re-write the current project's manifest to disk. Used when
        /// the manifest has changed in memory (rename, settings edit,
        /// default-scene change) — those mutators don't auto-persist.
        [[nodiscard]] bool saveProject();

        // ── Scene file I/O — operate on `currentScene()`'s World ──────

        /// Open a `.luxscene` file by path. Tears down the current
        /// scene and brings up a new one whose World is populated from
        /// the file. Tracks the path internally so a subsequent
        /// `saveScene()` writes back to the same place.
        [[nodiscard]] bool openScene(const std::filesystem::path& luxscene_file);

        /// Save the current scene to its tracked path (the one passed
        /// to the most recent `openScene` / `saveSceneAs`). Returns
        /// false if no current scene, or no tracked path — call
        /// `saveSceneAs` first in that case.
        [[nodiscard]] bool saveScene();

        /// Save the current scene to a chosen path, and remember that
        /// path for subsequent `saveScene()` calls.
        [[nodiscard]] bool saveSceneAs(const std::filesystem::path& luxscene_file);

        // ── Accessors ─────────────────────────────────────────────────

        // ── Accessors ─────────────────────────────────────────────────
        // All non-null only after a successful init().

        lux::window::LuxWindow&     window()           noexcept { return *window_; }
        lux::ui::UISystem&          uiSystem()         noexcept { return *ui_system_; }
        lux::asset::AssetManager&   assetManager()     noexcept { return *asset_mgr_; }

        /// Shared ownership of the process-wide AssetManager. SerDesers
        /// (TextureSerDeser / ModelSerDeser / etc.) take a shared_ptr so
        /// the asset registry survives the SerDeser's lifetime — this
        /// accessor lets the import path hand the same handle to them
        /// instead of wrapping the raw pointer.
        std::shared_ptr<lux::asset::AssetManager> assetManagerShared() noexcept { return asset_mgr_; }
        lux::ui::UIRenderSession&   renderSession()    noexcept { return *session_; }
        AssetBrowser&               assetBrowser()     noexcept { return *asset_browser_; }
        /// Editor-wide application events (content_changed). Producers emit
        /// `events().content_changed.emit({})` after writing/importing an asset;
        /// the host refreshes the browser + registry in one place. See EditorEvents.hpp.
        EditorEvents&               events()           noexcept { return *events_; }
        InspectorPanel&             inspectorPanel()   noexcept { return *inspector_panel_; }
        lux::ui::SceneViewportPanel& viewportPanel()   noexcept { return *viewport_panel_; }
        HierarchyPanel&             hierarchyPanel()   noexcept { return *hierarchy_panel_; }

        /// Editor-side built-in primitives (cube / plane / default material /
        /// skybox texture), available after a successful `init()`. Used by
        /// `EditorScene::bringUp` (reference grid + default material) and the
        /// asset-import / inspector "Add" paths.
        EditorBuiltins&             builtins()         noexcept { return *builtins_; }
        const EditorBuiltins&       builtins() const   noexcept { return *builtins_; }

        /// Editor-wide input action mapper. Lifetime = editor process.
        /// Always non-null after a successful `init()`. Used by EditorScene
        /// for camera control and by picking / drag-drop hooks.
        lux::input::ActionMapper& actionMapper() noexcept { return *action_mapper_; }
        const lux::input::ActionMapper& actionMapper() const noexcept { return *action_mapper_; }

        /// May return nullptr after `unloadScene()` or a failed `loadScene`.
        /// (Forwards to the SceneController, which owns the live scene.)
        EditorScene*                currentScene()       noexcept;
        const EditorScene*          currentScene() const noexcept;

        // ── M3 spawn helpers ─────────────────────────────────────────────

        /// Spawn an entity from a `ModelAsset` UUID. Picks
        /// SkeletalMeshComponent + AnimatorComponent when the model has a
        /// skeleton, MeshComponent otherwise. Auto-selects the new entity.
        ///
        /// Returns `entt::null` when there is no live scene, the id does
        /// not resolve to a ModelAsset, or the model has zero meshes.
        /// Multi-mesh models currently only spawn the first sub-mesh —
        /// the rest log a warning. (Plan §3.5 ↔ §4.1 follow-up.)
        entt::entity spawnModelEntity(lux::asset::asset_id_t model_id);

        /// May return nullptr when no project is open. (Forwards to the
        /// ProjectController, which owns the open project.)
        Project*                    currentProject()       noexcept;
        const Project*              currentProject() const noexcept;

        /// Filesystem path the current scene was opened from / last
        /// saved to. Empty if `currentScene()` is null or the scene
        /// hasn't been persisted yet. (Forwards to the SceneController.)
        const std::filesystem::path& currentScenePath() const noexcept;

        /// Pre-built render-side scene / view / feature handles. Valid
        /// for the editor process's lifetime (populated once in
        /// `renderThreadMain` before the render thread enters its
        /// tick loop). Consumed by `EditorScene::bringUp` instead of
        /// re-creating these resources per scene swap.
        const EditorRenderInfra& renderInfra() const noexcept { return render_infra_; }

        // ── Command surface (used by EditorMenuBar / ImportDialog) ─────────

        /// Defer a heavy action (scene/project swap, import) out of the current
        /// ImGui frame. `run()` drains the queue at the top of each loop
        /// iteration, between frames, where scene tearDown/bringUp is safe.
        void enqueue(std::function<void()> action);

        /// The toggleable-window catalogue (Window menu + visibility config).
        PanelRegistry& panels() noexcept { return panel_registry_; }

        /// Native OS window handle (HWND on Windows, nullptr elsewhere) used to
        /// parent owner-modal file dialogs.
        [[nodiscard]] void* nativeWindowHandle() const noexcept;

        /// The import subsystem (Import Options modal + import operations). The
        /// menu-bar hook paints its modal each frame.
        ImportController& importController() noexcept { return *import_controller_; }

        /// Transient on-screen notifications (used by the import flow and panels).
        ToastQueue& toasts() noexcept { return toasts_; }

        /// Entry point for importing an external asset (model / texture) into the
        /// current project — a thin forward to the ImportController (kept for the
        /// menu / OS file-drop call sites). Safe only from the main thread / a
        /// drained pending action — never from a GLFW callback or a scene-swapping
        /// frame.
        void importExternalAsset(const std::filesystem::path& source);

    private:
        // Stop the render thread and tear down state. Safe to call multiple
        // times; idempotent. Called automatically by dtor / end of run().
        void shutdown() noexcept;

        // Register a panel as a toggleable editor window: add it to the UISystem
        // AND record it in `panel_registry_` under a stable `id` (used by the
        // Window menu and the per-project visibility config). `default_visible`
        // seeds a project that has no saved window config.
        void registerWindow(std::string id, lux::ui::Panel* panel,
                             bool default_visible = true);


        EditorConfig                                            config_;
        bool                                                    initialised_{false};
        std::atomic<bool>                                       quit_requested_{false};

        // Main-thread state.
        std::unique_ptr<lux::window::GlfwRuntime>               glfw_;
        std::unique_ptr<lux::window::LuxWindow>                 window_;
        std::unique_ptr<lux::ui::UISystem>                      ui_system_;
        // Catalogue of toggleable windows (id -> live Panel*), populated in init()
        // via registerWindow(). Drives the Window menu + per-project visibility
        // config. Holds non-owning pointers into the panel members below.
        PanelRegistry                                           panel_registry_;
        // Main menu bar + Import Options modal — extracted command-surface UI.
        // Declared after ui_system_ so they destruct before it; unique_ptr to
        // forward-declared types is safe because ~LuxEditor() is out-of-line.
        std::unique_ptr<EditorMenuBar>                          menu_bar_;
        std::unique_ptr<ImportController>                       import_controller_;
        std::shared_ptr<lux::asset::AssetManager>               asset_mgr_;
        // Engine async runtime (stdexec): CPU pool + main-thread sync point +
        // asset lazy-load orchestrator. Created after asset_mgr_, destroyed
        // before it (declared right after so it dies first; also reset
        // explicitly in shutdown). Its requestLoad closure is injected into the
        // EditorScene (RenderableSystem bridge + SkeletalAnimationResolver), and
        // run() drains its main-thread continuations each frame.
        std::unique_ptr<lux::exec::EngineExecutor>             executor_;
        std::unique_ptr<EditorBuiltins>                         builtins_;
        std::unique_ptr<lux::ui::UIRenderSession>               session_;
        // Resident thumbnail subsystem (PreviewScene + renderers + cache).
        // Created after `session_` is up; ticked from the main loop.
        std::unique_ptr<ThumbnailService>                       thumbnail_service_;

        // Live material-graph preview — a generalized PreviewScene (forward
        // graph-frag override) driven each frame; the MaterialGraphPanel
        // displays + orbits it.
        std::unique_ptr<PreviewScene>                           material_preview_;
        // Project asset index + texture-upload cache for the material-graph
        // editor's pickers.
        std::unique_ptr<AssetRegistry>                          asset_registry_;
        std::unique_ptr<EditorTextureCache>                     texture_cache_;

        // Input layer — owns the editor's ActionMapper and the active
        // InputContextStack. Created in `init()` after the meta module is
        // up; ticked from `run()` once per frame on the captured
        // `InputSnapshot`. EditorScene reads action axes from here in tick().
        std::unique_ptr<lux::input::ActionMapper>            action_mapper_;
        std::unique_ptr<lux::input::InputContextStack>       input_stack_;

        // Edge-tracker for cursor capture (GLFW_CURSOR_DISABLED +
        // RAW_MOUSE_MOTION). Flipped each frame by `run()` based on the
        // current scene's camera mode. Initialized to false so the editor
        // boots with a normal pointer.
        bool                                                    cursor_captured_{false};

        // Shared editor state table (selection, current registry, …). Refcounted:
        // each state lives as long as a consumer holds the shared_ptr ensure()
        // returns, so member order no longer matters for state lifetime.
        StateRegistry                                          states_;
        // Canonical owner of the Selection — holds a strong ref so it persists
        // for the whole session (written on scene swap / spawn; panels + scene
        // share the same instance via ensure()).
        std::shared_ptr<Selection>                            selection_;
        // Editor-wide app events (content_changed). Ensured from the StateRegistry
        // like Selection; the host holds a strong ref so it persists for the
        // session. Producers emit via events(); the host subscribes once (in init)
        // to refresh the browser + registry.
        std::shared_ptr<EditorEvents>                         events_;

        // Default panels — owned here so they outlive UISystem.
        std::unique_ptr<AssetBrowser>                          asset_browser_;
        std::unique_ptr<InspectorPanel>                        inspector_panel_;
        std::unique_ptr<lux::ui::SceneViewportPanel>            viewport_panel_;
        std::unique_ptr<HierarchyPanel>                         hierarchy_panel_;
        std::unique_ptr<SceneFeatureSettingPanel>             scene_feature_setting_panel_;
        // Material-graph node editor panel. Held as the base ui::Panel type so
        // this public header needs no material_graph dependency.
        std::unique_ptr<lux::ui::Panel>                         material_graph_panel_;
        // The same panel cached as its concrete type at creation, so the
        // setup-time uses need no RTTI downcast (forward-declared above keeps the
        // header dependency-free).
        MaterialGraphPanel*                                    material_graph_panel_typed_ = nullptr;
        // FlowForge graph editor panel (the first GraphKit host). Same
        // type-erasure: the concrete FlowGraphPanel is a src-private header.
        std::unique_ptr<lux::ui::Panel>                         flow_graph_panel_;
        // Lua script console (engine bindings registered in its ctor).
        std::unique_ptr<lux::ui::Panel>                         lua_console_;

        // Scene lifecycle + state. SceneController owns the live EditorScene and
        // its on-disk path; LuxEditor forwards currentScene()/currentScenePath()
        // here. Declared after every subsystem it borrows (session_, asset_mgr_,
        // viewport_panel_, …) so it destructs first; constructed at the end of
        // init() once those are live. Shutdown unloads the scene explicitly (via
        // closeProject) before the borrowed subsystems die.
        std::unique_ptr<SceneController>                        scene_controller_;

        // Project lifecycle + state. ProjectController owns the open Project
        // (parsed `.luxproject` + paths — the source of truth for "where the
        // user's assets and scenes live") and drives scene_controller_ on
        // open/close. Declared after scene_controller_ (holds a SceneController&)
        // so it destructs first; constructed right after it at the end of init().
        std::unique_ptr<ProjectController>                      project_controller_;

        // (Filesystem-path picking is done through native OS dialogs — see the
        // lux::editor::*Dialog facade in FileDialog.hpp — so there is no ImGui
        // path-prompt state to carry here anymore. The whole import subsystem —
        // the Import Options modal + the import operations — lives in
        // `import_controller_` (ImportController), declared above.)

        // Deferred-action queue. Menu callbacks run INSIDE an ImGui frame
        // (EditorMenuBar::paint is invoked from UISystem::newFrame between
        // ImGui::NewFrame and the panel paints). Anything that swaps the
        // scene — newProject / openProject / closeProject / openScene /
        // Switch Project — calls EditorScene::tearDown + bringUp, which
        // begin/submit render frames via session_->syncCall to talk to
        // the render thread. Running those inside an open ImGui frame
        // would interleave editor-side ImGui state with multi-frame
        // render-thread submissions and corrupt the panel paint half-way
        // through.
        //
        // The fix: menu items enqueue the heavy action here and return
        // immediately. `run()` drains the queue at the top of each loop
        // iteration — outside any ImGui frame — so scene swaps happen
        // cleanly in the gap between frames.
        std::vector<std::function<void()>>                      pending_actions_;

        // Transient on-screen notifications (import results, etc.). Painted
        // via a UISystem overlay hook set in init(); pushed from drained
        // pending actions. Main-thread only.
        ToastQueue                                              toasts_;

        // RAII handle for the window's file-drop subscription. Disconnected
        // in shutdown() before window_ is destroyed.
        lux::cxx::event::ScopedConnection                       file_drop_conn_;

        // Signal-slot connections from editor panels (AssetBrowser::activated /
        // create_instance_requested, …) into this host. Declared AFTER the panels
        // it borrows so RAII destruction disconnects BEFORE those signal sources
        // die; also cleared explicitly in shutdown().
        lux::cxx::event::ConnectionGroup                        panel_conns_;

        // ImGui op-ids captured from the render server during bring-up (used to
        // build session_). Filled by render_thread_orch_ before it flips ready.
        lux::ui::ImGuiOperationIds                              imgui_ops_{};

        // Render-side infrastructure created BEFORE the orchestrator flips
        // ready. Main thread reads via `renderInfra()` once it sees the ready
        // signal; from that point on the struct is read-only and safe to share
        // without synchronisation. Filled by render_thread_orch_.
        EditorRenderInfra                                       render_infra_;

        // Render thread + comm channel + GeneralRenderServer bring-up (the fixed
        // feature set), encapsulated. Owns the channel / sync / thread + the
        // ready handshake; fills imgui_ops_ + render_infra_ (above) through
        // references on the render thread. Declared LAST so it destructs FIRST —
        // its borrowed refs (window_ / config_ / render_infra_ / imgui_ops_) stay
        // valid through its teardown. shutdown() requestStop()s it mid-teardown
        // and resets it after session_ (which borrows the channel / sync).
        std::unique_ptr<RenderThreadOrchestrator>              render_thread_orch_;
    };

} // namespace lux::editor
