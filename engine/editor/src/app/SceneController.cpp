#include "app/SceneController.hpp"

#include <lux/engine/editor/scene/EditorScene.hpp>   // EditorScene + BringUpConfig
#include <lux/engine/editor/scene/Scene.hpp>          // Scene::save + SceneSaveOptions
#include <lux/engine/ecs/World.hpp>        // scene->world().registry()
#include <lux/engine/editor/app/Selection.hpp>        // selection_->entity / registry
#include <lux/engine/editor/app/StateRegistry.hpp>    // states_ (EditorScene ctor arg)
#include <lux/engine/ui/SceneViewportPanel.hpp>       // viewport_panel_ wiring
#include <lux/engine/ui/ImGuiLuxWidgets.hpp>          // encodeSceneViewSentinel
#include <lux/engine/asset/AssetManager.hpp>          // *asset_mgr_ (EditorScene ctor arg)
#include <lux/engine/execution/EngineExecutor.hpp>    // executor_->requestLoad

#include <entt/entt.hpp>   // entt::registry (empty fallback) + entt::null

#include <cstdio>
#include <string>
#include <utility>

namespace lux::editor
{
    // Empty fallback registry used while there is no live scene (between
    // unloadScene and the next loadScene). Without this, `setRegistry` would
    // have nothing safe to point at after the EditorScene's World is destroyed.
    namespace
    {
        entt::registry& emptyRegistry() noexcept
        {
            static entt::registry r;
            return r;
        }
    } // namespace

    SceneController::SceneController(
        lux::ui::UIRenderSession*                  session,
        std::shared_ptr<lux::asset::AssetManager>  asset_mgr,
        lux::ui::UISystem*                         ui_system,
        EditorBuiltins*                            builtins,
        const EditorRenderInfra&                   render_infra,
        StateRegistry&                             states,
        lux::exec::EngineExecutor*                 executor,
        std::shared_ptr<Selection>                 selection,
        lux::ui::SceneViewportPanel*               viewport_panel) noexcept
        : session_(session)
        , asset_mgr_(std::move(asset_mgr))
        , ui_system_(ui_system)
        , builtins_(builtins)
        , render_infra_(render_infra)
        , states_(states)
        , executor_(executor)
        , selection_(std::move(selection))
        , viewport_panel_(viewport_panel)
    {
        // Forward viewport interaction to the live scene. Connected ONCE here and
        // held for the controller's whole life: the slots read current_scene_ at
        // emit time (null between scenes → no-op), so scene swaps need no per-scene
        // (re)wiring or teardown. viewport_panel_ is host-owned and outlives us.
        if (viewport_panel_)
        {
            viewport_conns_.add(viewport_panel_->picked.connect(
                [this](const lux::ui::ViewportPicked& e)
                {
                    if (current_scene_)
                        current_scene_->onPick(e.content_x, e.content_y,
                                               e.content_w, e.content_h);
                }));
            viewport_conns_.add(viewport_panel_->resized.connect(
                [this](const lux::ui::ViewportResized& e)
                {
                    if (current_scene_)
                        current_scene_->queueResize(e.width, e.height);
                }));
        }
    }

    SceneController::~SceneController() = default;

    bool SceneController::loadScene(const BringUpConfig& cfg)
    {
        // Tear down any existing scene first so the render thread frees the
        // old scene_id/view before we ask it to make new ones.
        unloadScene();

        if (!session_ || !asset_mgr_ || !ui_system_ || !builtins_)
            return false;

        // Inject the engine executor's async-load hook: the scene's
        // RenderableSystem bridge + SkeletalAnimationResolver fire it for any
        // asset id that resolves to an absent / data-less shell.
        auto scene = std::make_unique<EditorScene>(
            *session_, *asset_mgr_, *builtins_, render_infra_, states_,
            [this](const lux::asset::asset_id_t& id)
            {
                if (executor_) executor_->requestLoad(id);
            });

        if (!scene->bringUp(cfg))
        {
            // bringUp left the scene in a half-initialised state; let it
            // destruct without calling tearDown (commands already in flight
            // will be drained as the render thread eventually quiesces).
            return false;
        }

        // Bind the viewport panel to the new scene's view via the sentinel
        // texture-id encoding. The render thread resolves the sentinel to a
        // real Vulkan descriptor set each frame.
        viewport_panel_->setTextureID(
            lux::ui::encodeSceneViewSentinel(
                scene->sceneId(), scene->mainView().index));

        // Inspector + Hierarchy follow the new World by reading the shared
        // Selection — bind the new registry and clear the stale entity in one
        // atomic write. (Pick + click-in-hierarchy write it; F-focus + gizmo read it.)
        selection_->select(&scene->world().registry(), entt::null);

        // Viewport pick/resize forwarding is connected once in the ctor and guards
        // on current_scene_ — no per-scene (re)wiring needed here.
        current_scene_ = std::move(scene);
        return true;
    }

    void SceneController::unloadScene() noexcept
    {
        if (!current_scene_)
            return;

        if (session_)
        {
            current_scene_->tearDown();
        }

        // Panels revert to the empty fallback registry (read via the shared
        // Selection) so they don't dangle on the destroyed World; clear it too.
        selection_->select(&emptyRegistry(), entt::null);
        if (viewport_panel_)
            viewport_panel_->setTextureID(ImTextureID{});
        // The viewport pick/resize/drop signals stay connected across scene swaps:
        // their slots guard on current_scene_ (now null) and no-op until the next
        // loadScene — no per-scene teardown needed (the old setPickCallback({}) /
        // setAssetDropCallback({}) dance is gone).

        current_scene_.reset();
        current_scene_path_.clear();
    }

    bool SceneController::openScene(const std::filesystem::path&    luxscene_file,
                                    std::optional<std::string_view> project_name)
    {
        BringUpConfig cfg;
        if (project_name)
            cfg.name = std::string(*project_name);
        cfg.from_scene_file = luxscene_file;

        if (!loadScene(cfg))
            return false;
        current_scene_path_ = luxscene_file;
        return true;
    }

    bool SceneController::saveScene()
    {
        if (current_scene_path_.empty())
            return false;
        return saveSceneAs(current_scene_path_);
    }

    bool SceneController::saveSceneAs(const std::filesystem::path& luxscene_file)
    {
        if (!current_scene_)
            return false;
        SceneSaveOptions opts;
        // current_scene_->cameraEntity() returns the editor camera when
        // it exists; otherwise null. We persist it so a reload restores
        // the active camera selection.
        opts.active_camera = current_scene_->cameraEntity();

        auto saved = Scene::save(luxscene_file,
                                  current_scene_->world().registry(),
                                  opts);
        if (!saved)
        {
            std::fprintf(stderr, "[LuxEditor] saveScene failed: %s\n",
                         saved.error().c_str());
            return false;
        }
        current_scene_path_ = luxscene_file;
        return true;
    }

} // namespace lux::editor
