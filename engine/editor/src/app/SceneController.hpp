#pragma once
// ============================================================================
//  SceneController — the ONE entry point for scene lifecycle:
//  bring-up / tear-down (loadScene / unloadScene) and the scene file-I/O
//  verbs (openScene / saveScene / saveSceneAs). Scene-domain STATE — the
//  Selection and the file binding (path) — lives inside EditorScene;
//  this controller only orchestrates and owns the current-scene pointer.
//
//  Holds non-owning references to the editor subsystems an EditorScene needs at
//  bring-up. All of them are created in LuxEditor::init() — after the render
//  server is up — BEFORE this controller, and outlive it, so they are stable for
//  the controller's whole life. That is why they are REFERENCES: "never null"
//  is a fact about the construction order, and the type system is where such a
//  fact belongs (a raw pointer would invite a null-guard at every use site).
//
//  Private editor header (engine/editor/src/app — not installed).
// ============================================================================


#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include <lux/engine/runtime/assets/AssetLoadService.hpp>

namespace lux::ui    { class UIRenderFrameSession; class SceneViewportPanel; }
namespace lux::asset { class AssetManager; }
namespace lux::exec  { class AsyncRuntime; }
namespace lux::ecs { class ComponentTypeCatalog; }

namespace lux::editor
{
    class EditorScene;
    class EditorAsyncService;
    struct EditorRenderInfra;
    struct BringUpConfig;

    class SceneController
    {
    public:
        SceneController(lux::ui::UIRenderFrameSession&                  session,
                        std::shared_ptr<lux::asset::AssetManager>  asset_mgr,
                        const EditorRenderInfra&                   render_infra,
                        lux::asset_runtime::AssetClient            asset_client,
                        lux::exec::AsyncRuntime&                   async,
                        EditorAsyncService&                        editor_async,
                        const lux::ecs::ComponentTypeCatalog&      components,
                        lux::ui::SceneViewportPanel&               viewport_panel) noexcept;
        ~SceneController();   // out-of-line: unique_ptr<EditorScene> with fwd-decl

        SceneController(const SceneController&)            = delete;
        SceneController& operator=(const SceneController&) = delete;

        /// Bring up a scene from @p cfg (tears down any existing one first) and
        /// wire the viewport to its view. The new scene's Selection + file
        /// binding are born inside it (C11). Returns false on failure.
        [[nodiscard]] bool loadScene(const BringUpConfig& cfg);

        /// Tear down the live scene. Idempotent.
        void unloadScene() noexcept;

        /// Open a scene file. @p project_name seeds the bring-up config's name:
        /// the editor passes the open project's name (even if it is empty) when
        /// a project is open, or std::nullopt when none is — mirroring the
        /// original "project open ⇒ use its name" rule.
        [[nodiscard]] bool openScene(const std::filesystem::path&    file,
                                     std::optional<std::string_view> project_name,
                                     std::filesystem::path play_cache_root = {});
        [[nodiscard]] bool saveScene();
        [[nodiscard]] bool saveSceneAs(const std::filesystem::path& file);

        EditorScene*       currentScene()       noexcept { return current_scene_.get(); }
        const EditorScene* currentScene() const noexcept { return current_scene_.get(); }

        /// The live scene's file binding (owned by the scene, C11); a static
        /// empty path when no scene is open.
        [[nodiscard]] const std::filesystem::path& currentScenePath() const noexcept;

        /// Fired after a scene is loaded (with the new scene) and on unload
        /// (with nullptr). Consumers that hold SCENE-domain render state
        /// (per-scene RenderSceneId / feature handles — e.g. the
        /// rendering-settings panel) re-target here. One subscriber (the shell).
        void setOnSceneChanged(std::function<void(EditorScene*)> cb) noexcept
        { on_scene_changed_ = std::move(cb); }

    private:
        // Non-owning editor subsystems (see header note on lifetime).
        lux::ui::UIRenderFrameSession&                 session_;
        std::shared_ptr<lux::asset::AssetManager> asset_mgr_;
        const EditorRenderInfra&                  render_infra_;
        lux::asset_runtime::AssetClient           asset_client_;
        lux::exec::AsyncRuntime&                  async_;
        EditorAsyncService&                       editor_async_;
        const lux::ecs::ComponentTypeCatalog&     components_;
        lux::ui::SceneViewportPanel&              viewport_panel_;

        // Owned state.
        std::unique_ptr<EditorScene>              current_scene_;
        std::function<void(EditorScene*)>         on_scene_changed_;

        // (曾有 viewport_conns_(ConnectionGroup):viewport pick/resize 的信号
        //  连接。信号层退役批改为面板的单槽回调 —— ctor 设置,dtor 置空断开,
        //  槽内 guard current_scene_,场景换装零接线。)
    };

} // namespace lux::editor
