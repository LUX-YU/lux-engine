#pragma once
// ============================================================================
//  SceneController — owns the editor's live scene (EditorScene) and the path it
//  was loaded from / last saved to, plus the scene-lifecycle operations:
//  bring-up / tear-down (loadScene / unloadScene) and scene file I/O
//  (openScene / saveScene / saveSceneAs). Extracted verbatim from LuxEditor.
//
//  Holds non-owning refs/ptrs to the editor subsystems an EditorScene needs at
//  bring-up. All of them are created in LuxEditor::init() — after the render
//  server is up — BEFORE this controller, and outlive it, so the raw
//  pointers/references are stable for the controller's whole life. (Their
//  null-guards are kept verbatim from the original code; in practice they hold
//  by construction.)
//
//  Private editor header (engine/editor/src/app — not installed).
// ============================================================================

#include <lux/cxx/event/Connection.hpp>   // ConnectionGroup viewport_conns_

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace lux::ui    { class UIRenderSession; class UISystem; class SceneViewportPanel; }
namespace lux::asset { class AssetManager; }
namespace lux::exec  { class EngineExecutor; }

namespace lux::editor
{
    class EditorScene;
    class EditorBuiltins;
    class StateRegistry;
    class Selection;
    struct EditorRenderInfra;
    struct BringUpConfig;

    class SceneController
    {
    public:
        SceneController(lux::ui::UIRenderSession*                  session,
                        std::shared_ptr<lux::asset::AssetManager>  asset_mgr,
                        lux::ui::UISystem*                         ui_system,
                        EditorBuiltins*                            builtins,
                        const EditorRenderInfra&                   render_infra,
                        StateRegistry&                             states,
                        lux::exec::EngineExecutor*                 executor,
                        std::shared_ptr<Selection>                 selection,
                        lux::ui::SceneViewportPanel*               viewport_panel) noexcept;
        ~SceneController();   // out-of-line: unique_ptr<EditorScene> with fwd-decl

        SceneController(const SceneController&)            = delete;
        SceneController& operator=(const SceneController&) = delete;

        /// Bring up a scene from @p cfg (tears down any existing one first), wire
        /// the viewport + selection to the new World. Returns false on failure.
        [[nodiscard]] bool loadScene(const BringUpConfig& cfg);

        /// Tear down the live scene + clear its path. Idempotent.
        void unloadScene() noexcept;

        /// Open a scene file. @p project_name seeds the bring-up config's name:
        /// the editor passes the open project's name (even if it is empty) when
        /// a project is open, or std::nullopt when none is — mirroring the
        /// original "project open ⇒ use its name" rule.
        [[nodiscard]] bool openScene(const std::filesystem::path&    file,
                                     std::optional<std::string_view> project_name);
        [[nodiscard]] bool saveScene();
        [[nodiscard]] bool saveSceneAs(const std::filesystem::path& file);

        EditorScene*       currentScene()       noexcept { return current_scene_.get(); }
        const EditorScene* currentScene() const noexcept { return current_scene_.get(); }
        const std::filesystem::path& currentScenePath() const noexcept { return current_scene_path_; }

        /// Set the path WITHOUT touching the live scene. ProjectController::
        /// openProject calls this just before loadScene(): loadScene's internal
        /// unloadScene() early-returns (no scene yet, after closeProject) so the
        /// path survives — matching the original LuxEditor ordering.
        void setCurrentScenePath(std::filesystem::path p) { current_scene_path_ = std::move(p); }

    private:
        // Non-owning editor subsystems (see header note on lifetime).
        lux::ui::UIRenderSession*                 session_;
        std::shared_ptr<lux::asset::AssetManager> asset_mgr_;
        lux::ui::UISystem*                        ui_system_;
        EditorBuiltins*                           builtins_;
        const EditorRenderInfra&                  render_infra_;
        StateRegistry&                            states_;
        lux::exec::EngineExecutor*                executor_;
        std::shared_ptr<Selection>                selection_;
        lux::ui::SceneViewportPanel*              viewport_panel_;

        // Owned state.
        std::unique_ptr<EditorScene>              current_scene_;
        std::filesystem::path                     current_scene_path_;

        // Viewport pick/resize → live-scene forwarding. Connected ONCE in the
        // ctor; the slots guard on current_scene_, so scene swaps need no
        // per-scene (re)wiring. Declared last → disconnected first on teardown
        // (before current_scene_); viewport_panel_ (the signal source) is
        // host-owned and outlives this controller.
        lux::cxx::event::ConnectionGroup          viewport_conns_;
    };

} // namespace lux::editor
