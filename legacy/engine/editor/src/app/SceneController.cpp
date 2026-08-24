#include "app/SceneController.hpp"
#include "app/EditorAsyncService.hpp"

#include <lux/engine/editor/scene/EditorScene.hpp>   // EditorScene + BringUpConfig
#include <lux/engine/ui/SceneViewportPanel.hpp>       // viewport_panel_ wiring
#include <lux/engine/ui/ImGuiLuxWidgets.hpp>          // encodeRenderTargetSentinel
#include <lux/engine/resource/asset/AssetManager.hpp>          // *asset_mgr_ (EditorScene ctor arg)
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>

#include <cstdio>
#include <string>
#include <utility>

namespace lux::editor
{
    SceneController::SceneController(
        lux::ui::UIRenderFrameSession&                  session,
        std::shared_ptr<lux::asset::AssetManager>  asset_mgr,
        const EditorRenderInfra&                   render_infra,
        lux::asset_runtime::AssetClient            asset_client,
        lux::exec::AsyncRuntime&                   async,
        EditorAsyncService&                        editor_async,
        const lux::ecs::ComponentTypeCatalog&      components,
        lux::ui::SceneViewportPanel&               viewport_panel) noexcept
        : session_(session)
        , asset_mgr_(std::move(asset_mgr))
        , render_infra_(render_infra)
        , asset_client_(std::move(asset_client))
        , async_(async)
        , editor_async_(editor_async)
        , components_(components)
        , viewport_panel_(viewport_panel)
    {
        // Forward viewport interaction to the live scene. Slots set ONCE here
        // for the controller's whole life: they read current_scene_ at call
        // time (null between scenes → no-op), so scene swaps need no per-scene
        // (re)wiring. viewport_panel_ is host-owned and outlives us — the
        // destructor clears the slots so they never outlive `this`(单槽回调缝,
        // 信号层退役批:置空即断开,旧 ConnectionGroup 的 RAII 由此替代)。
        viewport_panel_.on_picked =
            [this](const lux::ui::ViewportPicked& e)
            {
                if (current_scene_)
                    current_scene_->onPick(e.content_x, e.content_y,
                                           e.content_w, e.content_h);
            };
        viewport_panel_.on_resized =
            [this](const lux::ui::ViewportResized& e)
            {
                if (current_scene_)
                    current_scene_->queueResize(e.width, e.height);
            };
    }

    SceneController::~SceneController()
    {
        // 自己收自己的尾(此前是 `= default`)。unloadScene() 是 noexcept 且幂等
        // (`if (!current_scene_) return;`),它替场景跑 tearDown(归还 render target /
        // mesh instance / view)并通知场景域消费者放手 —— 靠外部记得调,漏了就是
        // 静默泄漏 + 一批指向死场景的悬垂订阅。
        unloadScene();
        // 槽先于 this 断开:面板活得比控制器久,悬垂的 [this] 闭包不能留。
        viewport_panel_.on_picked  = {};
        viewport_panel_.on_resized = {};
    }

    bool SceneController::loadScene(const BringUpConfig& cfg)
    {
        // Tear down any existing scene first so the render thread frees the
        // old scene_id/view before we ask it to make new ones.
        unloadScene();

        // asset_mgr_ is a shared_ptr (shared OWNERSHIP, not a borrow), so unlike
        // the reference members it can genuinely arrive null. Its
        // pointer-vs-reference question belongs to the ownership batch.
        if (!asset_mgr_)
            return false;

        auto scene = std::make_unique<EditorScene>(
            session_, *asset_mgr_, render_infra_, asset_client_, async_,
            editor_async_, components_);

        if (!scene->bringUp(cfg))
        {
            // bringUp left the scene in a half-initialised state; let it
            // destruct without calling tearDown (commands already in flight
            // will be drained as the render thread eventually quiesces).
            return false;
        }

        // Bind the viewport panel to the new scene's render target via the
        // sentinel texture-id encoding. The render thread resolves the
        // sentinel to a real Vulkan descriptor set each frame.
        viewport_panel_.setTextureID(
            lux::ui::encodeRenderTargetSentinel(scene->mainTarget()));

        // Selection + file binding were born INSIDE the scene at bringUp
        // (C11); consumers re-target through on_scene_changed_ below.
        current_scene_ = std::move(scene);
        if (on_scene_changed_)
            on_scene_changed_(current_scene_.get());   // scene-domain consumers re-target (C2)
        return true;
    }

    void SceneController::unloadScene() noexcept
    {
        if (!current_scene_)
            return;

        // Unconditional. This used to be `if (session_)` — a guard whose
        // predicate had nothing to do with the action it gated: skipping
        // tearDown returns the render target, mesh instances and view to
        // nobody, while unloadScene() still reports success.
        if (!current_scene_->tearDown())
        {
            std::fprintf(
                stderr,
                "[SceneController] scene close is retryable; keeping the "
                "scene and render dependencies alive\n"
            );
            return;
        }

        viewport_panel_.setTextureID(ImTextureID{});
        // The viewport pick/resize/drop signals stay connected across scene swaps:
        // their slots guard on current_scene_ (now null) and no-op until the next
        // loadScene. The Selection died with the scene (C11) — consumers drop
        // their pointers through the callback below.

        current_scene_.reset();
        if (on_scene_changed_)
            on_scene_changed_(nullptr);   // scene-domain consumers drop their targets (C2)
    }

    bool SceneController::openScene(const std::filesystem::path&    luxscene_file,
                                    std::optional<std::string_view> project_name,
                                    std::filesystem::path play_cache_root)
    {
        BringUpConfig cfg;
        if (project_name)
            cfg.name = std::string(*project_name);
        cfg.from_scene_file = luxscene_file;   // becomes the scene's file binding
        cfg.play_cache_root = std::move(play_cache_root);
        return loadScene(cfg);
    }

    bool SceneController::saveScene()
    {
        return current_scene_ && current_scene_->save();
    }

    bool SceneController::saveSceneAs(const std::filesystem::path& luxscene_file)
    {
        return current_scene_ && current_scene_->saveTo(luxscene_file);
    }

    const std::filesystem::path& SceneController::currentScenePath() const noexcept
    {
        static const std::filesystem::path kEmpty;
        return current_scene_ ? current_scene_->scenePath() : kEmpty;
    }

} // namespace lux::editor
