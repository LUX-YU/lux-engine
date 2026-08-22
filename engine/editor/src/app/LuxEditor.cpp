#include <lux/engine/editor/app/LuxEditor.hpp>
#include <lux/engine/editor/app/EditorActions.hpp>
#include <lux/engine/editor/app/EditorEvents.hpp>
#include <lux/engine/editor/app/SpawnRegistry.hpp>
#include <lux/engine/editor/panels/ToastQueue.hpp>
#include <lux/engine/editor/panels/AssetBrowser.hpp>
#include <lux/engine/editor/content/EditorBuiltins.hpp>
#include <lux/engine/editor/thumbnail/ThumbnailService.hpp>
#include "thumbnail/MaterialPreviewHost.hpp"    // private (engine/editor/src) — live material preview host
#include <lux/engine/editor/scene/EditorScene.hpp>
#include <lux/engine/editor/panels/HierarchyPanel.hpp>
#include <lux/engine/editor/AssetRegistry.hpp>
#include "app/EditorShell.hpp"                  // private (engine/editor/src) — panel composition
#include "app/EditorAsyncService.hpp"
#include "app/EditorMenuBar.hpp"                // private (engine/editor/src) — extracted main menu bar
#include "app/ImportController.hpp"             // private (engine/editor/src) — import subsystem (modal + ops)
#include "app/SceneController.hpp"              // private (engine/editor/src) — scene lifecycle + state
#include "app/ProjectController.hpp"            // private (engine/editor/src) — project lifecycle + state
#include "app/AssetDeleteController.hpp"        // private (engine/editor/src) — 资产删除流程(列引用者+强删)
#include "app/AssetFileWatcher.hpp"             // private (engine/editor/src) — 资产文件监视(热更新触发源)
#include <lux/engine/resource/asset/AssetEvents.hpp>
#include <lux/engine/runtime/render/backend_host/RenderBackendHost.hpp>
#include <lux/engine/runtime/render/scene/AsyncRenderUploadService.hpp>
#include <lux/engine/runtime/render/scene/SceneGeometryPrepareService.hpp>
#include <lux/engine/runtime/frame/FrameCoordinator.hpp>
#include <lux/engine/runtime/frame/MainCloseDriver.hpp>
#include <lux/engine/events/DomainEvents.hpp>
#include <lux/engine/filewatch/FileWatcher.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/ui/ImGuiCommConfig.hpp>
#include "flow/FlowGraphCompiler.hpp" // private Authoring compile coordinator
#include "panels/FlowGraphPanel.hpp" // private editor panel
#include <lux/engine/authoring/flowforge/NodeRegistry.hpp>
#include <lux/engine/authoring/project/Project.hpp>
#include <lux/engine/authoring/world/WorldSourceCodec.hpp>
#include <lux/engine/runtime/scene/SceneRuntime.hpp>   // clampFrameDt (§2.4)
#include <lux/engine/scene/SceneAssetSerDeser.hpp>
#include <lux/engine/runtime/render/scene/RenderDiagnostics.hpp>   // 诊断出口装配(§7.1)
#include <lux/engine/runtime/extensions/EngineExtensions.hpp>
#include <lux/engine/editor/extensions/EditorContributionRegistrar.hpp>
#include <lux/engine/runtime/extensions/ExtensionModuleManager.hpp>
#include <lux/engine/log/Log.hpp>                          // setOutput / LogRecord(事件批C)

#include <lux/engine/input/Input.hpp>
#include <lux/engine/input/InputContext.hpp>

#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/ecs/animation/InstallAnimationSystems.hpp>
#include <lux/engine/runtime/scene/composition/InstallNavigation3DSystems.hpp>
#include <lux/engine/runtime/scene/composition/InstallPhysics3DSystems.hpp>
#include <lux/engine/runtime/scene/composition/InstallPresentation3DSystems.hpp>
#include <lux/engine/ecs/integration/presentation2d/InstallPresentation2DSystems.hpp>
#include <lux/engine/ecs/physics/InstallSimulationSystems.hpp>
#include <lux/engine/runtime/scene/composition/InstallTilemapSystems.hpp>
#include <lux/engine/ecs/transform/InstallTransformSystems.hpp>
#include <lux/engine/runtime/spatial3d/partitioned/InstallSpatial3DStreamingSystems.hpp>
#include <lux/engine/runtime/logging/LogRouter.hpp>
#include <lux/engine/runtime/assets/AssetLoadService.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionService.hpp>
#include <lux/engine/runtime/assets/navigation/Navigation3DPrepareService.hpp>
#include <lux/engine/runtime/spatial3d/physics/StaticCollider3DPrepareService.hpp>
#include <lux/engine/runtime/assets/tilemap/TilemapPrepareService.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/authoring/assets/FlowGraphSerDeser.hpp>
#include <lux/engine/ecs/render/RenderResourceEvents.hpp>
#include <lux/engine/runtime/render/scene/ResidencyAssembly.hpp>          // 驻留三件套装配(裁决二)

#include <filesystem>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/components/Transform2DComponent.hpp>   // Spinner2D demo script (temporary)
#include <lux/engine/ecs/script/systems/ScriptRegistry.hpp>             // scriptRegistry() — register demo script
#include <lux/engine/ecs/script/systems/ScriptBehavior.hpp>             // ScriptBehavior base — demo script
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/ui/ImGuiLuxWidgets.hpp>
#include <lux/engine/editor/panels/InspectorPanel.hpp>
#include <lux/engine/editor/panels/SceneSettingsPanel.hpp>
#include <lux/engine/editor/panels/ExtensionMonitorPanel.hpp>
#include <lux/engine/ui/Panel.hpp>
#include <lux/engine/ui/SceneViewportPanel.hpp>
#include <lux/engine/ui/UIRenderServer.hpp>
#include <lux/engine/ui/UIRenderFrameSession.hpp>
#include <lux/engine/ui/UISystem.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>

#include <imgui.h>


#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <lux/cxx/core/Format.hpp>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace lux::editor
{
    namespace
    {
        // ── Built-in demo script — TEMPORARY dev/smoke affordance ─────────
        // The editor ships ONE registered C++ ScriptBehavior so the Edit/Play
        // loop is testable end to end: author its manifest asset via the
        // browser's New Script ▸ C++ ▸ Spinner2D (A-6), drop it on a 2D
        // entity's ScriptComponent, press Play → it spins. Registered
        // EXPLICITLY (not via LUX_REGISTER_SCRIPT static-init) so linking the
        // editor static lib into the exe can't strip it.
        class Spinner2DBehavior final : public lux::ecs::ScriptBehavior
        {
        public:
            void onUpdate(float dt) noexcept
            {
                if (!hasComponent<lux::ecs::Transform2DComponent>())
                    return;
                // 改组件走 patchComponent —— 裸 getComponent 写回不发信号,
                // 变更驱动的变换系统看不到(见 ScriptBehavior::patchComponent)。
                patchComponent<lux::ecs::Transform2DComponent>([dt](auto& t)
                {
                    t.rotation += kSpinRadPerSec * dt;
                });
            }
        private:
            static constexpr float kSpinRadPerSec = 1.5f;   // ~86°/s — clearly visible
        };

        void registerBuiltinDemoScripts()
        {
            auto& reg = lux::ecs::scriptRegistry();
            if (reg.hasCppScript("Spinner2D"))
                return;   // idempotent
            // Typed registration: the template emits the
            // per-type pool + DEVIRTUALIZED shims for exactly the overridden
            // lifecycle methods (Spinner2D → onUpdate only).
            reg.registerCppScript<Spinner2DBehavior>("Spinner2D");
        }

        // ── UIRenderServer bring-up — RenderBackendHost 的 bring_up 回调正文,
        //    在**渲染线程**上跑 ────────────────────────────────────────────
        // 原 lux::ui::bringUpUIRenderServer(UiAppHost)的正文收进宿主:服务器
        // 怎么建是宿主的装配决定(装配归属 ADR 裁决三),ui 库从此不再对
        // LuxWindow / 校验层出口发言。失败不打 stderr —— bring_up 返回 null 经
        // start() 的握手带回 init(),那里有响亮出口。
        std::unique_ptr<lux::ui::UIRenderServer> bringUpEditorRenderServer(
            const std::shared_ptr<lux::render::RenderFrameChannel<>>&   channel,
            const std::shared_ptr<lux::render::RenderControlChannel<>>& control_channel,
            const std::shared_ptr<lux::render::RenderUploadChannel<>>&  upload_channel,
            const std::shared_ptr<lux::render::RenderChannelSync>&      sync,
            lux::window::LuxWindow&                                     window,
            const lux::runtime::RenderThreadConfig&                     cfg)
        {
            auto server = std::make_unique<lux::ui::UIRenderServer>(
                channel,
                control_channel,
                upload_channel,
                sync
            );

            lux::render::ServerConfig scfg;
            scfg.enable_validation       = cfg.enable_validation;
            scfg.capacity_request        = cfg.capacity_request;
            scfg.capacity_shortfall_output =
                cfg.capacity_shortfall_output;
            scfg.validation_message_sink = cfg.validation_message_sink;
            for (auto* ext : lux::ui::UISystem::requiredVulkanExtensions())
                scfg.instance_extensions.emplace_back(ext);

            // Font atlas pixels — the ImGui context was created by UISystem on
            // the main thread, so reading the atlas here (on the render thread)
            // is safe: it is fully built before this thread first touches the GPU.
            lux::ui::ImGuiCommConfig imgui_cfg{};
            imgui_cfg.color_format = lux::render::ETextureFormatHint::SBGRA8;
            ImGui::GetIO().Fonts->GetTexDataAsRGBA32(
                &imgui_cfg.font_pixels, &imgui_cfg.font_width, &imgui_cfg.font_height);

            if (auto r = server->init(std::move(scfg), imgui_cfg); !r)
                return nullptr;
            if (cfg.capacity_plan_output)
                *cfg.capacity_plan_output = server->capacityPlan();
            if (auto r = server->attachToWindow(window); !r)
                return nullptr;
            return server;
        }
    } // namespace

    struct LuxEditor::Runtime
    {
        // Declaration order is the passive lifetime graph. Destruction runs in
        // reverse, so consumers disappear before every service they borrow.
        // shutdown() performs only the active protocols that require a live
        // render server; Runtime's destructor performs ordinary storage release.
        // Must be destroyed last: every catalogue descriptor, factory,
        // active panel and operation can carry a ModuleLease.
        lux::extensions::ExtensionModuleManager                 extension_modules_;
        lux::ecs::ComponentTypeCatalog                          component_types_;
        // Transitional process registry. FlowGraph codecs still require the
        // legacy global instance; consumers receive it explicitly from here.
        lux::flowforge::NodeRegistry&                            flow_nodes_{
            lux::flowforge::NodeRegistry::global()};
        std::unique_ptr<lux::events::DomainEvents>              events_;
        lux::events::EventPump*                                 frame_pump_{nullptr};
        lux::events::EventPump*                                 ui_pump_{nullptr};

        // Already stopped by LuxEditor::shutdown(); its storage intentionally
        // outlives window/GLFW just as in the previous explicit teardown order.
        using RenderBackend = lux::runtime::RenderBackendHost<lux::ui::UIRenderServer>;
        std::unique_ptr<RenderBackend>                          render_thread_host_;

        std::unique_ptr<lux::window::GlfwRuntime>               glfw_;
        std::unique_ptr<lux::window::LuxWindow>                 window_;

        std::shared_ptr<lux::asset::AssetManager>               asset_mgr_;
        std::unique_ptr<lux::exec::AsyncRuntime>                async_;
        std::unique_ptr<lux::runtime::AsyncRenderUploadService>
                                                                upload_service_;
        std::unique_ptr<lux::logging::LogRouter>                log_router_;
        std::unique_ptr<lux::asset_runtime::AssetLoadService>   asset_load_;
        std::unique_ptr<lux::runtime::entity_scene::EntitySectionService> entity_sections_;
        std::unique_ptr<lux::runtime::SceneGeometryPrepareService> geometry_preparation_;
        std::unique_ptr<lux::runtime::assets::navigation::
            Navigation3DPrepareService> navigation_preparation_;
        std::unique_ptr<lux::runtime::spatial3d::StaticCollider3DPrepareService> physics_preparation_;
        std::unique_ptr<
            lux::runtime::assets::tilemap::TilemapPrepareService>
            tilemap_preparation_;
        std::unique_ptr<EditorAsyncService>                     editor_async_;
        // Panels borrow this coordinator. Declaring it before every UI owner
        // guarantees those borrowers disappear first during reverse teardown.
        std::unique_ptr<FlowGraphCompiler>                      flow_graph_compiler_;
        std::unique_ptr<lux::ui::UISystem>                      ui_system_;
        std::unique_ptr<lux::ui::UIRenderFrameSession>          session_;
        std::unique_ptr<lux::runtime::FrameCoordinator>         frame_coordinator_;
        std::unique_ptr<lux::runtime::MainCloseDriver>          close_driver_;

        // Process-domain render resources. Preview hosts are declared after
        // these services and therefore disappear before them.
        std::unique_ptr<lux::runtime::ResidencyAssembly>    residency_;
        EditorRenderInfra                                   render_infra_;
        lux::ui::ImGuiOperationIds                          imgui_ops_{};
        std::unique_ptr<AssetRegistry>                      asset_registry_;
        std::unique_ptr<MaterialPreviewHost>                material_preview_;
        std::unique_ptr<ThumbnailService>                   thumbnail_service_;

        std::unique_ptr<lux::input::Input>                  input_;
        bool                                                cursor_captured_{false};

        // Panels borrow the process services above. Controllers and command
        // surfaces are declared afterwards, so their callbacks disconnect
        // before the panel objects are released.
        std::unique_ptr<EditorShell>                        shell_;
        // Declared after shell_/async/catalog owners so it closes first.
        std::unique_ptr<lux::extensions::EngineExtensions>  extensions_;
        std::unique_ptr<EditorMenuBar>                      menu_bar_;
        std::unique_ptr<ImportController>                   import_controller_;
        std::unique_ptr<SceneController>                    scene_controller_;
        std::unique_ptr<ProjectController>                  project_controller_;
        std::unique_ptr<AssetDeleteController>              asset_delete_;
        std::unique_ptr<AssetFileWatcher>                   asset_watcher_;

        lux::platform::FileWatcher                          os_watcher_;
        std::filesystem::path                               watched_root_;

        // Values containing closures/subscriptions are last so their captures
        // are released before any target above.
        SpawnRegistry                                       spawn_registry_;
        std::vector<std::function<void()>>                  pending_actions_;
        ToastQueue                                          toasts_;
        lux::events::SubscriptionGroup                      subs_;
    };

    LuxEditor::LuxEditor(EditorConfig config)
        : config_(std::move(config))
    {
    }

    LuxEditor::~LuxEditor()
    {
        shutdown();
    }

    lux::ui::UISystem& LuxEditor::uiSystem() noexcept
    {
        return *runtime_->ui_system_;
    }

    lux::ecs::ComponentTypeCatalog& LuxEditor::componentTypes() noexcept
    {
        return runtime_->component_types_;
    }

    lux::extensions::EngineExtensions& LuxEditor::extensions() noexcept
    {
        return *runtime_->extensions_;
    }

    std::shared_ptr<lux::asset::AssetManager>
    LuxEditor::assetManagerShared() noexcept
    {
        return runtime_->asset_mgr_;
    }

    lux::ui::UIRenderFrameSession& LuxEditor::renderSession() noexcept
    {
        return *runtime_->session_;
    }

    lux::render::RenderControlSession&
    LuxEditor::renderControlSession() noexcept
    {
        return *runtime_->render_infra_.control;
    }

    const lux::render::FeatureCatalog&
    LuxEditor::featureCatalog() const noexcept
    {
        return runtime_->render_infra_.feature_catalog;
    }

    lux::events::DomainEvents& LuxEditor::events() noexcept
    {
        return *runtime_->events_;
    }

    lux::events::EventPump& LuxEditor::framePump() noexcept
    {
        return *runtime_->frame_pump_;
    }

    ImportController& LuxEditor::importController() noexcept
    {
        return *runtime_->import_controller_;
    }

    AssetDeleteController& LuxEditor::assetDeleteController() noexcept
    {
        return *runtime_->asset_delete_;
    }

    ThumbnailService* LuxEditor::thumbnailService() noexcept
    {
        return runtime_->thumbnail_service_.get();
    }

    ToastQueue& LuxEditor::toasts() noexcept
    {
        return runtime_->toasts_;
    }

    bool LuxEditor::init()
    {
        if (runtime_)
            return true;

        runtime_ = std::make_unique<Runtime>();

        // 0x. DomainEvents：进程域对象，两个 pump = 两个主线程安全点——
        //     frame(帧 OPEN 段,渲染命令可发)、ui(ImGui overlay 内,可用
        //     ImGui 状态)。发布与订阅都集中在本装配层(设计稿 §7.95);
        //     建在最前，之后的装配只广播已经提交的领域事实。
        runtime_->events_     = std::make_unique<lux::events::DomainEvents>();
        runtime_->frame_pump_ = &runtime_->events_->createPump("frame");
        runtime_->ui_pump_    = &runtime_->events_->createPump("ui");

        // Logs are operational records, not committed domain facts. The
        // process LogRouter replaces this early synchronous sink once the
        // AsyncRuntime has been assembled.
        lux::log::setOutput(
            [](const lux::log::LogRecord& r)
            { lux::log::writeRecordToStderr(r); });

        // 0. ecs 渲染桥的诊断去处。必须在任何桥开始工作之前装好 —— 之后只读
        //    (桥的回复延续跑在泵回复的线程上,运行期改它就是数据竞争)。
        //    此前这里直接写 stderr,而 game_host 写 lux::log、Android 什么都没装
        //    —— 同一条诊断三个宿主三种命运。现在统一走 runtime_render_scene 的共享装配,
        //    落点由本进程的 log sink 决定(main.cpp 装的 stderr + 下面的 toast)。
        lux::runtime::installRenderBridgeLogging();

        // 1. Reflection registry — drains the self-registering chain so any
        //    LUX_CLASS-annotated component built into the editor process is
        //    available to InspectorPanel from the first frame.
        lux::meta::meta_module_init();
        if (auto registered = lux::ecs::registerGeneratedComponents(
                runtime_->component_types_); !registered)
        {
            lux::log::error(
                "editor",
                "generated component schema registration failed for '{}'",
                registered.error().name);
            shutdown();
            return false;
        }

        // 1b. Register the editor's built-in demo script(s) into the process-wide
        //     script registry (TEMPORARY smoke affordance — see anon namespace).
        //     The flow-graph compiler gets the AOT dll cache。此时还没有
        //     openProject,先落 cwd/.lux 兜底;工程一开 repointFlowGraphCache()
        //     就把它搬到 <工程根>/.lux/cache/flowforge(此前恒落 cwd 是被已删的
        //     EditorConfig::project_root 恒空字段藏住的缺口,清理批补上重指)。
        registerBuiltinDemoScripts();
        // 2. GLFW + window.
        runtime_->glfw_ = std::make_unique<lux::window::GlfwRuntime>();
        if (!runtime_->glfw_->valid())
        {
            std::fprintf(stderr, "[LuxEditor] glfwInit failed\n");
            shutdown();
            return false;
        }

        runtime_->window_ = std::make_unique<lux::window::LuxWindow>(
            config_.width, config_.height, config_.title
        );
        if (!runtime_->window_->isInitialized())
        {
            shutdown();
            return false;   // LuxWindow::init already logged why
        }

        // 3. ImGui-driven UI system (creates the ImGui context).
        runtime_->ui_system_ = std::make_unique<lux::ui::UISystem>(*runtime_->window_);

        // 4. Process-wide AssetManager — survives scene swaps.
        const auto scene_codecs = lux::scene::makeSceneAssetCodecCatalog(
            *lux::authoring::authoringAssetCodecCatalog());
        if (!scene_codecs)
        {
            std::fprintf(
                stderr,
                "[LuxEditor] Scene asset codec catalog composition failed (%u)\n",
                static_cast<unsigned>(scene_codecs.error()));
            shutdown();
            return false;
        }
        runtime_->asset_mgr_ = std::make_shared<lux::asset::AssetManager>(
            *scene_codecs);

        // 4a. Register editor builtin assets (cube / plane / white PBR /
        //     skybox texture) so demo entities and any drag-drop targets
        //     can refer to them via stable UUIDs.
        // registerInto 把资产交给 AssetManager 拥有;EditorBuiltins 本身只是
        // 注册过程的工作对象,注册完即弃(消费者用编译期 UUID 常量寻址)。
        if (EditorBuiltins builtins; !builtins.registerInto(*runtime_->asset_mgr_))
        {
            std::fprintf(stderr, "[LuxEditor] EditorBuiltins::registerInto failed\n");
            shutdown();
            return false;
        }

        // 4b. Start the render host before creating AsyncRuntime. From the
        // moment AsyncRuntime exists, every later failure must have a live
        // FrameCoordinator/MainCloseDriver available to finish its sender-first
        // shutdown protocol.
        {
            lux::runtime::RenderBackendHost<lux::ui::UIRenderServer>::Config rtc;
            rtc.enable_validation = config_.enable_vulkan_validation;
            rtc.validation_message_sink =
                [](std::uint32_t severity, std::string_view text) {
                    static constexpr const char* kSeverity[]{
                        "INFO", "WARN", "ERROR"};
                    std::fprintf(
                        stderr,
                        "[Vulkan %s] %.*s\n",
                        kSeverity[severity < 3 ? severity : 0],
                        static_cast<int>(text.size()),
                        text.data());
                };
            rtc.bring_up =
                [this](
                    const std::shared_ptr<
                        lux::render::RenderFrameChannel<>>& channel,
                    const std::shared_ptr<
                        lux::render::RenderControlChannel<>>& control_channel,
                    const std::shared_ptr<
                        lux::render::RenderUploadChannel<>>& upload_channel,
                    const std::shared_ptr<
                        lux::render::RenderChannelSync>& sync,
                    const lux::runtime::RenderThreadConfig& c)
                {
                    return bringUpEditorRenderServer(
                        channel,
                        control_channel,
                        upload_channel,
                        sync,
                        *runtime_->window_,
                        c);
                };
            rtc.post_init = [this](lux::ui::UIRenderServer& server)
                { runtime_->imgui_ops_ = server.imguiOps(); };

            runtime_->render_thread_host_ = std::make_unique<
                lux::runtime::RenderBackendHost<lux::ui::UIRenderServer>>();
            if (!runtime_->render_thread_host_->start(std::move(rtc)))
            {
                std::fprintf(
                    stderr,
                    "[LuxEditor] render server failed to start\n");
                shutdown();
                return false;
            }
        }

        // 4c. Engine async runtime. Holds the CPU pool + main-thread sync point;
        //     its requestLoad orchestrates background open+decode of absent
        //     assets, injecting the decoded data into the registered shell at the
        //     next drainMainThreadCompletions() (run() pumps it each frame). Created here so its
        //     requestLoad closure is available when loadScene builds an EditorScene.
        lux::exec::AsyncRuntimeBuilder async_builder;
        auto asset_load = lux::asset_runtime::AssetLoadService::addTo(async_builder, *runtime_->asset_mgr_);
        if (!asset_load)
        {
            std::fprintf(stderr, "[LuxEditor] asset async assembly failed\n");
            shutdown();
            return false;
        }
        auto entity_sections =
            lux::runtime::entity_scene::EntitySectionService::addTo(
                async_builder);
        if (!entity_sections)
        {
            std::fprintf(
                stderr,
                "LuxEditor: EntitySectionService assembly failed\n");
            return false;
        }
        auto editor_async = EditorAsyncService::addTo(async_builder);
        if (!editor_async)
        {
            std::fprintf(stderr, "[LuxEditor] editor async assembly failed\n");
            shutdown();
            return false;
        }
        auto upload_service =
            lux::runtime::AsyncRenderUploadService::addTo(async_builder);
        if (!upload_service)
        {
            std::fprintf(stderr, "[LuxEditor] upload async assembly failed\n");
            shutdown();
            return false;
        }
        auto geometry_preparation =
            lux::runtime::SceneGeometryPrepareService::addTo(async_builder);
        if (!geometry_preparation)
        {
            std::fprintf(
                stderr,
                "[LuxEditor] scene geometry preparation assembly failed\n");
            shutdown();
            return false;
        }
        auto navigation_preparation = lux::runtime::assets::navigation::
            Navigation3DPrepareService::addTo(async_builder);
        if (!navigation_preparation)
        {
            std::fprintf(
                stderr,
                "[LuxEditor] navigation preparation assembly failed\n"
            );
            shutdown();
            return false;
        }
        auto physics_preparation = lux::runtime::spatial3d::StaticCollider3DPrepareService::addTo(async_builder);
        if (!physics_preparation)
        {
            std::fprintf(
                stderr,
                "[LuxEditor] static collider preparation assembly failed\n");
            shutdown();
            return false;
        }
        auto tilemap_preparation = lux::runtime::assets::tilemap::
            TilemapPrepareService::addTo(async_builder);
        if (!tilemap_preparation)
        {
            std::fprintf(
                stderr,
                "[LuxEditor] tilemap preparation assembly failed\n");
            shutdown();
            return false;
        }
        auto async_plan = std::move(async_builder).compile();
        if (!async_plan)
        {
            std::fprintf(stderr, "[LuxEditor] async runtime assembly failed\n");
            shutdown();
            return false;
        }
        runtime_->async_                        = std::make_unique<lux::exec::AsyncRuntime>(std::move(*async_plan));
        runtime_->upload_service_               = std::make_unique<lux::runtime::AsyncRenderUploadService>(std::move(*upload_service));
        runtime_->log_router_                   = std::make_unique<lux::logging::LogRouter>(*runtime_->async_);
        runtime_->log_router_->install(
            [this](const lux::log::LogRecord& record)
            {
                char message[512]{};
                const auto length = lux::log::formatRecord(
                    record,
                    message,
                    sizeof(message) - 1u);
                runtime_->toasts_.push(
                    std::string(message, length),
                    ToastLevel::Error);
            }
        );
        runtime_->asset_load_                   = std::make_unique<lux::asset_runtime::AssetLoadService>(std::move(*asset_load));
        runtime_->entity_sections_              = std::make_unique<lux::runtime::entity_scene::EntitySectionService>(std::move(*entity_sections));
        runtime_->geometry_preparation_         = std::make_unique<lux::runtime::SceneGeometryPrepareService>(std::move(*geometry_preparation));
        runtime_->navigation_preparation_ = std::make_unique<
            lux::runtime::assets::navigation::Navigation3DPrepareService>(
                std::move(*navigation_preparation));
        runtime_->physics_preparation_          = std::make_unique<lux::runtime::spatial3d::StaticCollider3DPrepareService>(std::move(*physics_preparation));
        runtime_->tilemap_preparation_ = std::make_unique<
            lux::runtime::assets::tilemap::TilemapPrepareService>(
            std::move(*tilemap_preparation));
        runtime_->render_infra_.entity_sections =
            runtime_->entity_sections_->loadClient();
        runtime_->editor_async_                 = std::make_unique<EditorAsyncService>(std::move(*editor_async));
        runtime_->editor_async_->bind(*runtime_->async_);
        runtime_->flow_graph_compiler_ = std::make_unique<FlowGraphCompiler>(
            std::filesystem::current_path() / ".lux" / "cache" / "flowforge",
            runtime_->editor_async_->flowGraphCompileClient()
        );

        runtime_->render_infra_.feature_catalog = runtime_->render_thread_host_->featureCatalog();
        runtime_->render_infra_.feature_plan    = runtime_->render_thread_host_->featurePlan();
        runtime_->render_infra_.control         = &runtime_->render_thread_host_->controlSession();
        runtime_->render_infra_.extension_modules = &runtime_->extension_modules_;
        runtime_->render_infra_.install_systems = [runtime = runtime_.get()](
            lux::ecs::ScheduleBuilder& builder)
        {
            return lux::ecs::installSpatial3DTransformSystems(
                       builder,
                       runtime->component_types_) &&
                lux::ecs::installAnimation3DSystems(
                    builder,
                    runtime->component_types_) &&
                lux::runtime::installPhysics3DSystems(
                    builder,
                    runtime->component_types_,
                    runtime->physics_preparation_->client()) &&
                lux::runtime::installNavigation3DSystems(
                    builder,
                    runtime->component_types_,
                    runtime->navigation_preparation_->client()) &&
                lux::ecs::installSpatial2DTransformSystems(
                    builder,
                    runtime->component_types_) &&
                lux::ecs::installSimulation2DSystems(
                    builder,
                    runtime->component_types_) &&
                lux::runtime::installTilemap2DSystems(
                    builder,
                    runtime->component_types_,
                    runtime->tilemap_preparation_->client()) &&
                lux::runtime::installSpatial3DStreamingSystems(
                    builder,
                    runtime->component_types_) &&
                lux::runtime::installPresentation3DSystems(
                    builder,
                    runtime->component_types_,
                    runtime->geometry_preparation_->classicMeshClient(),
                    runtime->geometry_preparation_->terrainClient()) &&
                lux::ecs::installPresentation2DSystems(
                    builder,
                    runtime->component_types_);
        };

        runtime_->session_ = std::make_unique<lux::ui::UIRenderFrameSession>(
            runtime_->render_thread_host_->channel(),
            runtime_->render_thread_host_->sync(),
            runtime_->imgui_ops_
        );
        
        lux::runtime::installRenderErrorLogging(
            *runtime_->session_,
            *runtime_->events_,
            *runtime_->frame_pump_,
            runtime_->subs_
        );

        runtime_->frame_coordinator_ =
            std::make_unique<lux::runtime::FrameCoordinator>(
                *runtime_->session_,
                runtime_->render_thread_host_->controlSession(),
                *runtime_->frame_pump_,
                *runtime_->async_
            );

        runtime_->close_driver_ =
            std::make_unique<lux::runtime::MainCloseDriver>(
                *runtime_->frame_coordinator_,
                *runtime_->async_
            );

        runtime_->render_infra_.close_driver = runtime_->close_driver_.get();

        if (!runtime_->upload_service_->bind(
                *runtime_->async_,
                runtime_->render_thread_host_->uploadSession(),
                runtime_->render_thread_host_->sync()))
        {
            std::fprintf(
                stderr,
                "[LuxEditor] upload coordinator bind failed\n");
            shutdown();
            return false;
        }
        runtime_->render_infra_.upload = runtime_->upload_service_->client();

        // 5. Default panels — created BEFORE the scene so the bring-up
        //    frame-pumping draws into a populated UI dockspace and the
        //    viewport panel exists in time for setTextureID at the end
        //    of bringUp. The panel composition lives on the EditorShell;
        //    build order inside mirrors the original.
        const auto browser_root = std::filesystem::current_path();

        // EditorAssetChanged 是已提交事实，不是全局 invalidation。保存内容
        // 不再重扫目录；只有 ADDED/REMOVED 改变 registry/browser 的结构。
        runtime_->subs_.add(runtime_->events_->subscribe<EditorAssetChanged>(
            *runtime_->frame_pump_,
            [this](const EditorAssetChanged& fact)
            {
                if (fact.change != EEditorAssetChange::CONTENT_UPDATED
                    && runtime_->asset_registry_)
                {
                    runtime_->asset_registry_->refresh();
                }
            }));

        runtime_->asset_registry_ = std::make_unique<AssetRegistry>();
        runtime_->shell_ = std::make_unique<EditorShell>();
        if (!runtime_->shell_->buildPanels(
                *this,
                *runtime_->asset_registry_,
                runtime_->asset_mgr_,
                *runtime_->events_,
                runtime_->flow_nodes_,
                *runtime_->flow_graph_compiler_))
        {
            shutdown();
            return false;
        }
        runtime_->shell_->assetBrowser()->setWorkingDirectory(browser_root);

        if (auto* mg = runtime_->shell_->materialGraphPanel())
        {
            mg->setCompileDispatch(
                [this, mg](
                    std::uint64_t request_id,
                    std::shared_ptr<const MaterialCompileJob> job)
                {
                    return runtime_->editor_async_->compileMaterial(
                        CompileMaterialOperation{std::move(job)},
                        [mg, request_id](auto outcome) mutable noexcept
                        {
                            if (!outcome)
                            {
                                auto failed = std::make_shared<MaterialCompileOutcome>();
                                failed->status = "material compile operation failed";
                                mg->onCompiled(request_id, std::move(failed));
                                return;
                            }
                            mg->onCompiled(request_id, std::move(*outcome));
                        });
                });
        }

        // Create-menu wiring: the recipes live in runtime_->spawn_registry_
        // (built-ins seeded by the registry-side free function); the
        // panels only own the ENTRY POINTS (+ button / right-click). The menu
        // CONTENT hooks stay host-wired — they call the host's private drawer.
        registerBuiltinSpawnRecipes(runtime_->spawn_registry_);
        runtime_->shell_->hierarchyPanel()->setCreateMenuHook([this] {
            drawSpawnMenuItems(std::nullopt);
        });
        runtime_->shell_->viewportPanel()->setContextMenuHook(
            [this](const lux::ui::SceneViewportPanel::ViewportPointer& click) {
                // "Create HERE": resolve the tap to a 2D world position when the
                // scene has a 2D camera (3D uses the focal target inside).
                std::optional<lux::math::Position2d> pos2d;
                if (auto* scene = currentScene())
                    pos2d = scene->viewportToWorld2D(
                        click.pos_in_content.x, click.pos_in_content.y,
                        click.content_size.x,  click.content_size.y);
                if (ImGui::BeginMenu("Create"))
                {
                    drawSpawnMenuItems(pos2d);
                    ImGui::EndMenu();
                }
            });

        // Import subsystem — owns the Import Options modal + the model/texture
        // import operations; on confirm it defers the heavy import through the
        // action queue. Constructed before the menu bar, which paints its modal
        // each frame.
        runtime_->import_controller_ = std::make_unique<ImportController>(*this);
        runtime_->import_controller_->setImportDispatch(
            [this](
                std::uint64_t,
                std::shared_ptr<const ImportJob> job)
            {
                return runtime_->editor_async_->importAsset(
                    ImportAssetOperation{std::move(job)},
                    [this](auto outcome) mutable noexcept
                    {
                        if (!outcome)
                        {
                            runtime_->toasts_.push(
                                "Import operation failed.",
                                ToastLevel::Error);
                            return;
                        }
                        runtime_->import_controller_->adoptImportResult(
                            std::move(outcome->report),
                            outcome->source);
                    });
            });

        // Main menu bar — drawn inside `UISystem::newFrame` between
        // ImGui::NewFrame and the dockspace, so it sits at the top of the
        // OS window and is not nested in any panel scope. It paints the Import
        // Options modal at its tail.
        runtime_->menu_bar_ = std::make_unique<EditorMenuBar>(*this);
        runtime_->ui_system_->setMainMenuBarHook([this]{ runtime_->menu_bar_->paint(); });

        // Toast overlay — painted after panels, above everything.
        // ui 泵在这里排空(ImGui 帧内的安全点):Error 级 LogRecord 的 toast
        // 订阅在 init 顶部接好,handler 里直接 push(ToastQueue::push 用
        // ImGui 时间,这里恒在帧内 —— 旧 LogToastSink 的两段缓冲不再需要)。
        runtime_->ui_system_->setOverlayHook([this]{
            runtime_->ui_pump_->drain();
            runtime_->toasts_.paint();
        });

        // OS file drop → import. GLFW fires this on the main thread during
        // pollEvents(); we only enqueue, so the heavy import runs from the
        // drained pending-action queue (outside the ImGui frame) like the
        // menu-driven path. One action per file so partial failures isolate.
        // 单槽回调缝(信号层退役批):shutdown() 在 runtime_->window_ 亡前置空断开。
        runtime_->window_->on_file_drop =
            [this](const lux::window::FileDropEvent& ev)
            {
                for (const auto& p : ev.paths)
                    runtime_->pending_actions_.emplace_back(
                        [this, p]{ importExternalAsset(p); });
            };

        runtime_->extensions_ = std::make_unique<
            lux::extensions::EngineExtensions>(
                lux::extensions::EngineExtensionServices{
                    .modules = runtime_->extension_modules_,
                    .async = *runtime_->async_,
                    .components = runtime_->component_types_,
                    .events = runtime_->events_.get(),
                    .prepare_editor =
                        lux::extensions::makeEditorRegistrationAdapter(
                            runtime_->shell_->panelCatalog())},
                std::vector<
                    lux::extensions::ExtensionModuleRequirement>{});

        {
            auto& tools = runtime_->shell_->toolHost();
            if (!tools.addService(*runtime_->extensions_) ||
                !tools.addService(runtime_->shell_->panelCatalog()))
            {
                std::fprintf(
                    stderr,
                    "[LuxEditor] duplicate or invalid editor service "
                    "registration\n");
                shutdown();
                return false;
            }

            lux::editor::EditorPanelContributionDescriptor descriptor;
            descriptor.id = PanelId{
                "org.lux.editor.extension-monitor"};
            descriptor.display_name = "Extension Monitor";
            descriptor.default_visible = true;
            descriptor.provider = lux::extensions::ExtensionId{
                "org.lux.editor.core"};
            descriptor.required_editor_services = {
                lux::cxx::typeToken<
                    lux::extensions::EngineExtensions>(),
                lux::cxx::typeToken<
                    lux::editor::EditorPanelCatalog>()};
            descriptor.create = [](const auto& context)
                -> lux::cxx::expected<
                    std::unique_ptr<lux::ui::Panel>,
                    lux::editor::EEditorPanelCreateError>
            {
                auto* extensions = context.template find<
                    lux::extensions::EngineExtensions>();
                auto* panels = context.template find<
                    lux::editor::EditorPanelCatalog>();
                if (!extensions || !panels)
                {
                    return lux::cxx::unexpected(
                        lux::editor::EEditorPanelCreateError::
                            REQUIRED_SERVICE_MISSING);
                }
                return std::unique_ptr<lux::ui::Panel>{
                    std::make_unique<
                        lux::editor::ExtensionMonitorPanel>(
                            "Extension Monitor",
                            *extensions,
                            *panels)};
            };
            if (!runtime_->shell_->panelCatalog().add(
                    std::move(descriptor)))
            {
                std::fprintf(
                    stderr,
                    "[LuxEditor] ExtensionMonitor registration failed\n");
                shutdown();
                return false;
            }
            const auto ticket = runtime_->shell_->tools().requestOpen(
                panelId(
                    "org.lux.editor.extension-monitor"));
            (void)runtime_->shell_->processToolSafePoint();
            if (ticket.snapshot().terminal !=
                lux::extensions::EOperationTerminalState::SUCCEEDED)
            {
                std::fprintf(
                    stderr,
                    "[LuxEditor] ExtensionMonitor activation failed\n");
                shutdown();
                return false;
            }
        }
        // 7a-bis. 驻留三件套装配(裁决二 + J6-六修):进程域一份,三个
        //     SceneRuntime(主场景/缩略图/材质预览)共享 —— 同一资产全进程
        //     只一份 GPU 副本。装配唯一实现在 ResidencyAssembly;宿主保有
        //     订阅转接、失败观察 publish、关停序泵驱动。
        runtime_->residency_ = std::make_unique<lux::runtime::ResidencyAssembly>(
            runtime_->render_thread_host_->controlSession(),
            runtime_->render_infra_.upload,
            *runtime_->asset_mgr_, runtime_->render_thread_host_->featureCatalog(),
            runtime_->asset_load_->client(),
            *runtime_->async_,
            [b = runtime_->events_.get()](const lux::ecs::RenderResourceFailed& f)
            { b->publish(f); });
        runtime_->render_infra_.residency = runtime_->residency_.get();
        runtime_->render_infra_.components = &runtime_->component_types_;
        // 进程域 DomainEvents 走同一条注入通道：三个 SceneRuntime 宿主
        // 从 infra 取，填进各自 Config::events。
        runtime_->render_infra_.events     = runtime_->events_.get();
        runtime_->render_infra_.frame_pump = runtime_->frame_pump_;

        // 资产广播装配(批E:三队列退役):账本回调 → 事件(唯一翻译处,
        // 裁决③);GPU 缓存的消费变 frame 泵订阅 —— 订阅顺序镜像旧
        // 承载序(invalidated → content_changed →
        // unreferenced:通道按首订阅序派发),失效集/纪元先于本帧 resolver
        // 更新,同帧闭合不变。
        runtime_->asset_mgr_->setBroadcast({
            .on_unreferenced =
                [b = runtime_->events_.get()](const lux::asset::asset_id_t& id)
                {
                    b->publish(lux::asset::AssetUnreferenced{id});
                },
            .on_invalidated =
                [b = runtime_->events_.get()](const lux::asset::asset_id_t& id)
                {
                    b->publish(lux::asset::AssetInvalidated{id});
                },
            .on_content_changed =
                [b = runtime_->events_.get()](const lux::asset::asset_id_t& id, std::uint32_t rev)
                {
                    b->publish(lux::asset::AssetContentChanged{id, rev});
                },
            .on_registered =
                [b = runtime_->events_.get()](const lux::asset::asset_id_t& id)
                {
                    // 驻留 T11:失效封印的推式解封(删除后恢复的 id 重加载)。
                    b->publish(lux::asset::AssetRegistered{id});
                },
            });
        auto residency_events =
            runtime_->residency_->makeAssetEventCallbacks();
        runtime_->subs_.add(runtime_->events_->subscribe<lux::asset::AssetInvalidated>(
            *runtime_->frame_pump_,
            [fn = std::move(residency_events.invalidated)](
                const lux::asset::AssetInvalidated& e) mutable
            { fn(e.id); }));
        runtime_->subs_.add(runtime_->events_->subscribe<lux::asset::AssetContentChanged>(
            *runtime_->frame_pump_,
            [fn = std::move(residency_events.content_changed)](
                const lux::asset::AssetContentChanged& e) mutable
            { fn(e.id); }));
        runtime_->subs_.add(runtime_->events_->subscribe<lux::asset::AssetUnreferenced>(
            *runtime_->frame_pump_,
            [fn = std::move(residency_events.unreferenced)](
                const lux::asset::AssetUnreferenced& e) mutable
            { fn(e.id); }));
        runtime_->subs_.add(runtime_->events_->subscribe<lux::asset::AssetRegistered>(
            *runtime_->frame_pump_,
            [fn = std::move(residency_events.registered)](
                const lux::asset::AssetRegistered& e) mutable
            { fn(e.id); }));

        // 7a-quater. 驻留失败的可见性(T14,裁决J4 的观察半边):终态失败
        //     toast + 资产浏览器标红;内容修复/重注册清除标红。ui 泵 ——
        //     碰的是 ImGui 状态(条例:ImGui 状态只在 ui 泵 handler 里碰)。
        runtime_->subs_.add(runtime_->events_->subscribe<lux::ecs::RenderResourceFailed>(
            *runtime_->ui_pump_,
            [this](const lux::ecs::RenderResourceFailed& f)
            {
                static constexpr std::string_view kDomainNames[] = {
                    "网格", "贴图", "材质", "材质实例"};
                const auto d = static_cast<std::size_t>(f.domain);
                lux::log::error(
                    "residency",
                    "resource failed (domain={}, stage={}): {}",
                    d,
                    static_cast<std::size_t>(f.failure.stage),
                    f.failure.reason
                );
                runtime_->toasts_.push(
                    lux::format("资源驻留失败({}): {}",
                                d < std::size(kDomainNames) ? kDomainNames[d]
                                                            : "未知域",
                                f.failure.reason),
                    ToastLevel::Error, 8.0f);
                if (runtime_->shell_ && runtime_->shell_->assetBrowser())
                    runtime_->shell_->assetBrowser()->markResidencyFailed(
                        f.id, f.failure.reason);
            }));
        runtime_->subs_.add(runtime_->events_->subscribe<lux::asset::AssetContentChanged>(
            *runtime_->ui_pump_,
            [this](const lux::asset::AssetContentChanged& e)
            {
                if (runtime_->shell_ && runtime_->shell_->assetBrowser())
                    runtime_->shell_->assetBrowser()->clearResidencyFailed(e.id);
            }));
        runtime_->subs_.add(runtime_->events_->subscribe<lux::asset::AssetRegistered>(
            *runtime_->ui_pump_,
            [this](const lux::asset::AssetRegistered& e)
            {
                if (runtime_->shell_ && runtime_->shell_->assetBrowser())
                    runtime_->shell_->assetBrowser()->clearResidencyFailed(e.id);
            }));

        // 7b. Thumbnail subsystem — bring up its private preview SceneRuntime
        //     (offscreen target + light/camera entities) now that the server is
        //     up and no editor frame is in flight (initialize() is blocking).
        //     Wired into the asset browser by the shell below.
        runtime_->thumbnail_service_ = std::make_unique<ThumbnailService>(
            *runtime_->asset_mgr_, *runtime_->session_, runtime_->render_infra_,
            runtime_->asset_load_->client(), *runtime_->async_);
        if (!runtime_->thumbnail_service_->initialize())
        {
            std::fprintf(stderr, "[LuxEditor] ThumbnailService init failed\n");
            shutdown();
            return false;
        }

        // 7c. Live material-graph preview — the editor's SECOND private preview
        //     SceneRuntime (装配归属 ADR 工作线三批 2), brought up now (blocking
        //     initialize(), no editor frame in flight). Its SAMPLED target is
        //     displayed through the ImGui target sentinel; driven each frame by
        //     runtime_->material_preview_->tick(); the MaterialGraphPanel displays + orbits it.
        runtime_->material_preview_ = std::make_unique<MaterialPreviewHost>(
            *runtime_->asset_mgr_, *runtime_->session_, runtime_->render_infra_,
            runtime_->asset_load_->client(), *runtime_->async_);
        if (!runtime_->material_preview_->initialize(512))
        {
            std::fprintf(stderr, "[LuxEditor] material preview host init failed\n");
            shutdown();
            return false;
        }

        //（EditorTextureCache 已并轨进进程域驻留表:它就是「材质预览
        //  的贴图槽解析器」,拿的 bindless index 与共享缓存 ensureTexture 同物,
        //  却带着永不销毁/不上账/忽略 NO_MIPS 三个隐性 bug。面板现经
        //  MaterialPreviewHost::resolveTextureIndex 查同一份缓存。）

        // 8. Shell wiring phase 2: everything that connects the
        //    panels to the render session + the asset services created above —
        //    thumbnails, Inspector asset fields, material/flow/script asset
        //    editors, viewport asset-drop spawn. All services are live by
        //    now: init() returns false rather than wiring a null one.
        runtime_->shell_->wireAssetServices(*this,
                                  runtime_->thumbnail_service_.get(),
                                  runtime_->asset_registry_.get(),
                                  runtime_->material_preview_.get());


        // 9. Editor input layer — one Input + a single default
        //    InputContext carrying the M2 viewport bindings (WASD for fly,
        //    LMB / RMB / MMB for camera modes + picking, F / End hotkeys).
        //    Owned by LuxEditor; ticked in run(). EditorScene reads it
        //    each frame for camera control. Built BEFORE the initial scene
        //    so EditorScene::tick can rely on it from frame 0.
        runtime_->input_ = std::make_unique<lux::input::Input>();
        actions::registerAll(runtime_->input_->mapper());
        auto* editor_ctx = actions::editorContext();
        if (!editor_ctx)
        {
            // Without it the editor comes up looking healthy but every hotkey
            // and the whole viewport camera are dead — not a degraded mode.
            std::fprintf(stderr, "[LuxEditor] actions::editorContext() unavailable\n");
            shutdown();
            return false;
        }
        runtime_->input_->contexts().push(editor_ctx);

        // 10. Initial scene is NOT brought up here. Caller (main.cpp / the
        //     File menu) decides which scene the editor should open first
        //     via `openProject` / `loadScene`. Auto-loading a demo scene
        //     inside init() would create a tiny window where the demo
        //     scene's async upload replies are still in flight while
        //     `closeProject` already starts tearing the scene down — those
        //     stale replies would then crash on a freed adapter/context.
        //     Forcing the bring-up to be an explicit caller decision
        //     eliminates that race by construction.

        // Scene controller — the one scene-lifecycle entry point. Constructed
        // last: every subsystem it borrows (runtime_->session_, runtime_->asset_mgr_, runtime_->render_infra_,
        // runtime_->async_, asset-load feature and viewport panel are live.
        runtime_->scene_controller_ = std::make_unique<SceneController>(
            *runtime_->session_, runtime_->asset_mgr_, runtime_->render_infra_,
            runtime_->asset_load_->client(), *runtime_->async_,
            *runtime_->editor_async_,
            runtime_->component_types_,
            *runtime_->shell_->viewportPanel());

        // Scene-domain re-targeting: everything that addresses
        // per-scene state re-points on every load/unload — the shell repoints
        // the settings panel + the panels' Selection pointers (the Selection
        // lives INSIDE each EditorScene; it must die/renew with the scene).
        runtime_->scene_controller_->setOnSceneChanged(
            [this](EditorScene* scene) { runtime_->shell_->retargetScene(*this, scene); });

        // Project controller — drives runtime_->scene_controller_ on open/close, so it is
        // constructed right after it (and holds a SceneController&).
        runtime_->project_controller_ = std::make_unique<ProjectController>(
            *runtime_->scene_controller_, *runtime_->ui_system_, runtime_->asset_mgr_,
            *runtime_->asset_registry_, *runtime_->shell_->assetBrowser(),
            runtime_->shell_->toolHost(), *runtime_->extensions_);

        // 资产删除流程(列引用者 + 允许强删)。scene_registry 回调每次现取 ——
        // 场景会换;AssetBrowser 的 Delete… 信号由 shell 在 wireAssetServices
        // 里接到 request()(订阅先于本构造,但 lambda 到 emit 时才解引用),
        // 对话框由菜单栏钩子每帧画,真正的删除在 run() 的 frame-OPEN 段 tick
        // (缩略图作废要发 destroyTexture)。
        runtime_->asset_delete_ = std::make_unique<AssetDeleteController>(
            AssetDeleteController::Services{
                .assets = runtime_->asset_mgr_,
                .thumbnails = runtime_->thumbnail_service_.get(),
                .events = runtime_->events_.get(),
                .components = runtime_->component_types_,
                .scene_registry = [this]() -> lux::ecs::Registry*
                {
                    auto* sc = runtime_->scene_controller_ ? runtime_->scene_controller_->currentScene()
                                                 : nullptr;
                    return sc ? &sc->world().registry() : nullptr;
                }
            }
        );

        // 资产文件监视(热更新批6):文件变化是内容变更的唯一根因 —— 编辑器
        // Save/外部覆写全部经盘上 mtime 收敛到 reload → notifyContentChanged,
        // 批5 的缓存/resolver/实例链条接手,场景数帧内跟随。
        runtime_->asset_watcher_ = std::make_unique<AssetFileWatcher>(
            AssetFileWatcher::Services{
                runtime_->asset_mgr_, runtime_->asset_registry_.get(), runtime_->thumbnail_service_.get()});

        runtime_->asset_watcher_->setReloadDispatch(
            [this](
                const lux::asset::asset_id_t& id,
                const std::filesystem::path& abs_path,
                std::uint64_t generation)
            {
                const bool accepted = runtime_->editor_async_->reloadAsset(
                    ReloadAssetOperation{id, generation, abs_path},
                    [this, id, generation](auto outcome) mutable noexcept
                    {
                        if (!outcome)
                        {
                            runtime_->asset_watcher_->adoptReloadResult(
                                id,
                                generation,
                                nullptr,
                                "reload operation failed");
                            return;
                        }
                        runtime_->asset_watcher_->adoptReloadResult(
                            outcome->id,
                            outcome->generation,
                            std::move(outcome->asset),
                            outcome->error);
                    });
                return accepted;
            });

        return true;
    }

    // Project accessor — forwards to the ProjectController (which owns the open
    // project). Null-guarded so it is safe even before the controller is
    // constructed at the end of init().
    lux::authoring::Project* LuxEditor::currentProject() noexcept
    {
        return runtime_->project_controller_ ? runtime_->project_controller_->currentProject() : nullptr;
    }

    const lux::authoring::Project* LuxEditor::currentProject() const noexcept
    {
        return runtime_->project_controller_ ? runtime_->project_controller_->currentProject() : nullptr;
    }

    // Panel accessors — the composition lives on the EditorShell;
    // out-of-line because the shell is a src-private type.
    AssetBrowser&                LuxEditor::assetBrowser()   noexcept { return *runtime_->shell_->assetBrowser(); }
    EditorTools LuxEditor::tools() const noexcept
    {
        return runtime_->shell_ ? runtime_->shell_->tools() : EditorTools{};
    }

    // Scene accessors — forward to the SceneController (which owns the live
    // scene + its path). Null-guarded so they are safe even before the
    // controller is constructed at the end of init().
    EditorScene* LuxEditor::currentScene() noexcept
    {
        return runtime_->scene_controller_ ? runtime_->scene_controller_->currentScene() : nullptr;
    }

    const EditorScene* LuxEditor::currentScene() const noexcept
    {
        return runtime_->scene_controller_ ? runtime_->scene_controller_->currentScene() : nullptr;
    }

    const std::filesystem::path& LuxEditor::currentScenePath() const noexcept
    {
        static const std::filesystem::path kEmpty;
        return runtime_->scene_controller_ ? runtime_->scene_controller_->currentScenePath() : kEmpty;
    }

    // ──────────────────────────────────────────────────────────────────
    //  Play mode (editor Edit/Play) — forwards to the live EditorScene.
    // ──────────────────────────────────────────────────────────────────
    void LuxEditor::enterPlayMode()
    {
        auto* scene = currentScene();
        if (!scene || scene->isPlaying())
            return;
        // actions = nullptr for now: the editor registers only camera actions, not
        // game actions, so name→id resolution isn't meaningful in play yet (input-in-
        // play is a follow-up). Scripts still receive the ActionMapper (ctx.input).
        if (!scene->enterPlay(
                runtime_->input_->mapper(),
                &runtime_->input_->actionRegistry()
            ))
            std::fprintf(stderr, "[LuxEditor] enterPlayMode: failed to enter play.\n");
    }

    void LuxEditor::exitPlayMode()
    {
        auto* scene = currentScene();
        if (scene && scene->isPlaying())
            scene->exitPlay();
    }

    bool LuxEditor::isPlaying() const noexcept
    {
        const auto* scene = currentScene();
        return scene && scene->isPlaying();
    }

    // ──────────────────────────────────────────────────────────────────
    //  Entity authoring — the Create menu is DATA: SpawnRecipes.
    //  The built-ins live beside their registry (SpawnRecipes.cpp);
    //  this shell only draws the menu and selects the spawn result.
    // ──────────────────────────────────────────────────────────────────
    void LuxEditor::drawSpawnMenuItems(const std::optional<lux::math::Position2d>& pos2d)
    {
        auto* scene = currentScene();
        if (!scene)
        {
            ImGui::TextDisabled("No scene open");
            return;
        }
        const bool presents_2d = scene->isPlanar2D();
        const bool presents_3d = !presents_2d;

        // 3D placement (v1): the camera's focal target — where the user is looking.
        std::optional<lux::math::Position3d> pos3d;
        if (presents_3d)
        {
            pos3d = scene->viewportFocus3D();
        }

        const auto drawItem = [&](const SpawnRecipe& r)
        {
            const bool available = r.domain == ESpawnDomain::ANY ||
                (r.domain == ESpawnDomain::SPATIAL_2D && presents_2d) ||
                (r.domain == ESpawnDomain::SPATIAL_3D && presents_3d);
            ImGui::BeginDisabled(!available);
            if (ImGui::MenuItem(r.label.c_str()) && r.spawn)
            {
                SpawnContext ctx{
                    scene->world(),
                    presents_2d ? pos2d : std::nullopt,
                    pos3d};
                const auto e = r.spawn(ctx);
                if (e != entt::null)
                    scene->selection().selectEntity(e);   // scene-domain (C11)
            }
            ImGui::EndDisabled();
            if (!available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Not available in this scene kind / capability set.");
        };

        // Top-level recipes first, then one submenu per category (registration
        // order preserved in both — the registry is the menu's source of truth).
        for (const auto& r : runtime_->spawn_registry_.all())
            if (r.category.empty())
                drawItem(r);
        std::vector<std::string_view> seen;
        for (const auto& r : runtime_->spawn_registry_.all())
        {
            if (r.category.empty()) continue;
            if (std::find(seen.begin(), seen.end(), r.category) != seen.end()) continue;
            seen.push_back(r.category);
            if (ImGui::BeginMenu(r.category.c_str()))
            {
                for (const auto& s : runtime_->spawn_registry_.all())
                    if (s.category == r.category)
                        drawItem(s);
                ImGui::EndMenu();
            }
        }
    }

    // ──────────────────────────────────────────────────────────────────
    //  Project management
    // ──────────────────────────────────────────────────────────────────

    // Project lifecycle — thin forwards to the ProjectController. Guarded
    // against a null controller because closeProject() runs from shutdown(),
    // which can fire after a failed init() (before the controller is built).
    bool LuxEditor::openProject(const std::filesystem::path& luxproject_file)
    {
        const bool ok =
            runtime_->project_controller_ && runtime_->project_controller_->openProject(luxproject_file);
        if (ok) repointFlowGraphCache();
        return ok;
    }

    bool LuxEditor::newProject(const std::filesystem::path& root,
                                std::string_view              project_name)
    {
        const bool ok =
            runtime_->project_controller_ && runtime_->project_controller_->newProject(root, project_name);
        if (ok) repointFlowGraphCache();
        return ok;
    }

    void LuxEditor::repointFlowGraphCache()
    {
        // AOT 缓存随工程走(init 期还没有工程,只能先落 cwd/.lux —— 见 init 里
        // 的成因注释)。工程一开就搬到工程根,换工程各用各的缓存。
        const auto* proj = runtime_->project_controller_ ? runtime_->project_controller_->currentProject()
                                               : nullptr;
        if (proj && runtime_->flow_graph_compiler_)
            runtime_->flow_graph_compiler_->setCacheDir(
                proj->root() / ".lux" / "cache" / "flowforge");
    }

    void LuxEditor::closeProject() noexcept
    {
        if (runtime_->project_controller_) runtime_->project_controller_->closeProject();
    }

    bool LuxEditor::saveProject()
    {
        return runtime_->project_controller_ && runtime_->project_controller_->saveProject();
    }

    // ──────────────────────────────────────────────────────────────────
    //  Scene file I/O
    // ──────────────────────────────────────────────────────────────────

    // Scene file I/O — thin forwards to the SceneController. openScene seeds the
    // bring-up name from the open project (if any).
    bool LuxEditor::openScene(const std::filesystem::path& luxscene_file)
    {
        // Seed the bring-up name from the open project (if any) — original rule:
        // project open ⇒ use its name (even if empty); no project ⇒ leave the
        // BringUpConfig default.
        std::optional<std::string_view> project_name;
        if (const auto* p = currentProject())
            project_name = std::string_view(p->manifest().name);
        std::filesystem::path play_cache_root;
        if (const auto* p = currentProject())
            play_cache_root = p->cacheRoot() / "cache" / "world-play";
        return runtime_->scene_controller_->openScene(
            luxscene_file,
            project_name,
            std::move(play_cache_root));
    }

    bool LuxEditor::newScene(const std::filesystem::path& luxscene_file, bool spatial_2d)
    {
        auto source = lux::authoring::makeWorldSourceDocument(
            spatial_2d
                ? lux::authoring::EPartitionTopology::PLANAR_XY
                : lux::authoring::EPartitionTopology::PLANAR_XZ);
        if (auto saved = lux::authoring::saveWorldSource(
                luxscene_file,
                source); !saved)
        {
            std::fprintf(stderr, "[LuxEditor::newScene] save failed: %s\n",
                         saved.error().c_str());
            return false;
        }
        return openScene(luxscene_file);
    }

    bool LuxEditor::saveScene()
    {
        return runtime_->scene_controller_->saveScene();
    }

    bool LuxEditor::saveSceneAs(const std::filesystem::path& luxscene_file)
    {
        return runtime_->scene_controller_->saveSceneAs(luxscene_file);
    }

    // ──────────────────────────────────────────────────────────────────
    //  Command surface
    //
    //  Small seams the extracted EditorMenuBar / ImportDialog call back into.
    //  (The menu-bar tree and the Import Options modal now live in
    //  EditorMenuBar.cpp / ImportDialog.cpp; only their hooks into the editor
    //  state stay here.)
    // ──────────────────────────────────────────────────────────────────

    void LuxEditor::enqueue(std::function<void()> action)
    {
        // Heavy actions (scene / project swaps, imports) must run between
        // frames, outside the live ImGui frame — run() drains this queue at the
        // top of each loop iteration, where EditorScene tearDown/bringUp (which
        // pump render frames that re-enter ImGui) is safe.
        runtime_->pending_actions_.emplace_back(std::move(action));
    }

    void* LuxEditor::nativeWindowHandle() const noexcept
    {
        // The editor drives native OS file dialogs (nativefiledialog-extended)
        // for its open / save / import flows; they are owner-modal to the editor
        // window, so they need its platform handle (HWND on Windows) to parent
        // them. Only Windows exposes one today (LuxWindow::win32Handle is
        // declared under __PLATFORM_WIN32__); elsewhere return nullptr and the
        // dialog opens unparented.
#ifdef __PLATFORM_WIN32__
        return runtime_->window_ ? runtime_->window_->win32Handle() : nullptr;
#else
        return nullptr;
#endif
    }

    void LuxEditor::importExternalAsset(const std::filesystem::path& source)
    {
        // Thin forward — the import subsystem (modal + ops) lives in
        // ImportController. Kept as a member so the menu / OS file-drop call
        // sites stay unchanged.
        runtime_->import_controller_->importExternalAsset(source);
    }

    void LuxEditor::cookProjectContent()
    {
        auto* project = currentProject();
        if (!project || !runtime_->editor_async_)
            return;

        const auto out_pak =
            project->contentRoot().parent_path()
            / "Cooked"
            / (project->manifest().name + ".luxpak");
        runtime_->toasts_.push("Cooking content…", ToastLevel::Info);
        const bool accepted = runtime_->editor_async_->cookContent(
            CookContentOperation{project->contentRoot(), out_pak},
            [this](auto outcome) mutable noexcept
            {
                if (!outcome)
                {
                    runtime_->toasts_.push("Cook operation failed.", ToastLevel::Error);
                    return;
                }
                if (outcome->ok)
                {
                    std::fprintf(
                        stderr,
                        "[LuxEditor] cooked %zu asset(s) -> %s\n",
                        outcome->asset_count,
                        outcome->out_pak.string().c_str());
                    runtime_->toasts_.push(
                        lux::format(
                            "Cooked {} asset(s)",
                            outcome->asset_count),
                        ToastLevel::Success);
                    return;
                }
                std::fprintf(
                    stderr,
                    "[LuxEditor] cook FAILED: %s\n",
                    outcome->message.c_str());
                runtime_->toasts_.push(
                    lux::format("Cook failed: {}", outcome->message),
                    ToastLevel::Error);
            });
        if (!accepted)
        {
            runtime_->toasts_.push("Cook queue is stopping or full.", ToastLevel::Error);
        }
    }

    int LuxEditor::run()
    {
        if (!runtime_)
        {
            std::fprintf(stderr, "[LuxEditor] run() called before successful init()\n");
            return 1;
        }

        using clock = std::chrono::steady_clock;
        auto prev = clock::now();

        while (!runtime_->window_->shouldClose() && !quit_requested_.load(std::memory_order_acquire))
        {
            const auto now = clock::now();
            float dt = std::chrono::duration<float>(now - prev).count();
            prev = now;
            // §2.4: the one shared clamp (hosts had drifted to 0.1/0.25 with
            // no explainable difference — debugger stalls must not scramble
            // the orbit camera or become physics leaps, same requirement).
            dt = lux::runtime::clampFrameDt(dt);

            // build the ImGui frame on the main thread.
            //    The viewport panel paints here and may invoke its resize
            //    callback (queues into the live scene).
            lux::window::LuxWindow::pollEvents();

            // Sample raw OS input AFTER pollEvents so the snapshot reflects
            // this frame's accumulated GLFW callbacks. The snapshot is fed
            // to ActionMapper below (after ImGui::NewFrame so we can ask
            // ImGui whether it is capturing keyboard / mouse for its own
            // text inputs and drag handles).
            runtime_->input_->sample(*runtime_->window_);

            // Drain the deferred-action queue BEFORE starting the next
            // ImGui frame. Menu callbacks (EditorMenuBar::paint) cannot
            // perform scene swaps inline because tearDown re-enters
            // ImGui::NewFrame via FramePumper; they enqueue here and
            // we execute at the safe in-between-frames boundary.
            // Swap the queue into a local so an action that itself
            // enqueues further work (e.g. an open->open chain) gets
            // its enqueued items processed next iteration, not in the
            // middle of this drain (which would invalidate iterators).
            if (runtime_->extensions_)
                (void)runtime_->extensions_->processSafePoint();
            if (runtime_->project_controller_)
                (void)runtime_->project_controller_->processSafePoint();
            if (runtime_->shell_)
                (void)runtime_->shell_->processToolSafePoint();

            if (!runtime_->pending_actions_.empty())
            {
                auto actions = std::move(runtime_->pending_actions_);
                runtime_->pending_actions_.clear();
                for (auto& fn : actions)
                    if (fn) fn();
            }

            runtime_->ui_system_->newFrame();

            // Tick the editor input layer. The host has to decide whether
            // each input category flows to gameplay (the ActionMapper) or
            // to ImGui (the panel UI). Three sources are folded together:
            //
            //   - ImGui::Image (the SceneViewportPanel's rendered texture)
            //     is itself an interactive item, so the moment the cursor
            //     enters the viewport, ImGui flips `io.WantCaptureMouse`
            //     to TRUE — even when the user is just trying to RMB-fly
            //     the camera. If we honored WantCaptureMouse blindly, the
            //     mapper would never see RMB and fly mode could never
            //     start. So: when the cursor is *over the viewport* we
            //     override ImGui and route mouse → gameplay.
            //   - Same logic for keyboard: WASD over the viewport means
            //     "drive the camera", not "fire ImGui hotkeys."
            //   - `ownsMouse()` is the sticky companion — once fly mode is
            //     running, the cursor is hidden + locked so `hovered`
            //     stops being meaningful; the ownership flag carries the
            //     "keep routing to gameplay" state across that gap.
            //   - When the cursor is in any other panel (Inspector text
            //     input, menu bar, etc.), ImGui's WantCapture* wins and
            //     gameplay sees nothing — that's the expected behavior.
            const auto& io           = ImGui::GetIO();
            const auto* vp           = runtime_->shell_ ? runtime_->shell_->viewportPanel() : nullptr;
            const bool  over_view    = vp ? vp->pointer().hovered : false;
            const bool  owns_mb      = vp ? vp->ownsMouse()       : false;
            const bool  route_to_gp  = over_view || owns_mb;
            const bool  want_kb      = route_to_gp || !io.WantCaptureKeyboard;
            const bool  want_ms      = route_to_gp || !io.WantCaptureMouse;
            runtime_->input_->evaluate(dt, want_kb, want_ms);

            ImDrawData* draw_data = ImGui::GetDrawData();

            auto frame = runtime_->frame_coordinator_->begin();
            if (!frame)
                continue;

            frame.beforeMain([&]
            {
                // These services write commands into the newly-open frame.
                runtime_->residency_->tickTextureStreaming();
                runtime_->thumbnail_service_->tick();
                runtime_->material_preview_->tick();
                runtime_->asset_delete_->tick();
            });

            frame.beforeEvents([&]
            {
                // Translate OS notifications after async asset completions are
                // visible, immediately before the event safe point.
                pumpFileWatchEvents();
            });

            frame.record([&]
            {
                runtime_->asset_watcher_->tick();
                runtime_->shell_->sceneSettingsPanel()->tickRender();

                if (auto* scene = currentScene())
                {
                    scene->processPendingResize();
                    const auto cs = runtime_->shell_->viewportPanel()->contentSize();
                    scene->tick(dt, cs.x, cs.y, runtime_->input_->mapper());

                    const bool fly = scene->cameraWantsCursorCapture();
                    if (fly != runtime_->cursor_captured_)
                    {
                        runtime_->window_->hideCursor(fly);
                        runtime_->window_->setRawMouseMotion(fly);
                        if (auto* v = runtime_->shell_->viewportPanel())
                            v->setOwnsMouse(fly);
                        runtime_->cursor_captured_ = fly;
                    }
                }

                runtime_->session_->submitImGuiDrawData(
                    lux::render::RenderSceneId{}, draw_data);
            });
        }

        shutdown();
        return 0;
    }

    void LuxEditor::requestQuit() noexcept
    {
        quit_requested_.store(true, std::memory_order_release);
    }

    void LuxEditor::pumpFileWatchEvents()
    {
        if (!runtime_->asset_registry_)
            return;

        // 工程根就绪/切换时 (re)watch —— OS 层递归监视整个 Content 树。
        const auto& root = runtime_->asset_registry_->root();
        if (root.empty())
            return;
        if (root != runtime_->watched_root_)
        {
            runtime_->os_watcher_.unwatchAll();
            if (!runtime_->os_watcher_.watch(root, /*recursive=*/true))
                lux::log::warn("editor", "file watch failed for '{}' — asset "
                               "hot-reload disabled for this project",
                               root.string());
            runtime_->watched_root_ = root;   // 失败也记下:别每帧重试刷屏,重开工程再来
        }

        // Raw OS notifications have one consumer and are not committed domain
        // facts, so they bypass DomainEvents and enter the reload controller.
        for (auto& ev : runtime_->os_watcher_.drain())
            if (runtime_->asset_watcher_)
                runtime_->asset_watcher_->observe(ev);
    }

    // -------------------------------------------------------------------------
    lux::async::SubmitResult
    LuxEditor::spawnModelEntity(lux::asset::asset_id_t model_id, InstanceSpawnClient::Completion completion)
    {
        auto* scene = currentScene();
        if (!scene)
            return lux::cxx::unexpected(lux::async::ESubmitError::STOPPING);
        return scene->spawnModel(model_id, std::move(completion));
    }

    void LuxEditor::shutdown() noexcept
    {
        if (!runtime_)
            return;

        // Restore the synchronous emergency fallback before active teardown.
        lux::log::setOutput({});

        // Tear the scene down BEFORE stopping the render thread — tearDown
        // pumps frames, which the render thread must still be serving.
        // closeProject() also flushes the per-project ImGui layout
        // while runtime_->ui_system_ is still alive.
        closeProject();

        // Stop all event ingress before any active teardown. Runtime declaration
        // order later destroys subscriptions/controllers before their targets;
        // no individual owner needs an eager reset here.
        runtime_->subs_.clear();
        if (runtime_->asset_mgr_)
            runtime_->asset_mgr_->setBroadcast({});   // 账本回调收口:拆解期归零不再空发事件
        runtime_->os_watcher_.unwatchAll();     // 停 OS 监视:此后不再有事件进队列

        // Tear down the thumbnail service's preview SceneRuntime + offscreen
        // target while the render thread is still serving frames — tearDown
        // issues render commands (releaseAssetRefs / destroyScene), which after
        // the thread stops would be silently dropped (leaks) or assert. An
        // in-flight readback job is parked; shutdown() below reaps it after the
        // thread stopped.
        if (runtime_->thumbnail_service_)
            if (!runtime_->thumbnail_service_->releaseGpu())
                return;

        // Tear down the live material-preview host's SceneRuntime + offscreen
        // target while the render thread is still serving frames — tearDown
        // issues render commands, which after the thread stops would be
        // silently dropped (leaks) or assert.
        if (runtime_->material_preview_)
            if (!runtime_->material_preview_->releaseGpu())
                return;

        // Built-in panels are owned by EditorToolHost. Destroy every
        // controller which borrows a panel before close() clears that host's
        // active panel storage. Declaration order alone cannot protect this
        // boundary because close() is an eager protocol, not passive Runtime
        // destruction.
        runtime_->asset_watcher_.reset();
        runtime_->asset_delete_.reset();
        runtime_->project_controller_.reset();
        runtime_->scene_controller_.reset();
        runtime_->import_controller_.reset();
        runtime_->menu_bar_.reset();

        if (runtime_->shell_)
            (void)runtime_->shell_->toolHost().close();
        if (runtime_->extensions_)
        {
            const auto report = runtime_->close_driver_->close(
                *runtime_->extensions_);
            if (!report)
            {
                lux::log::error(
                    "editor",
                    "extension close watchdog expired; dependencies stay alive");
                return;
            }
            runtime_->extensions_.reset();
        }

        // ── 驻留主动关闭:所有场景已 tearDown,渲染依赖仍全部活着 ───────────
        // Assembly 内聚 clean drain、统一 AsyncScope stop/join、全域 late-owner
        // reaper、状态表 teardown 与最终 frame/reply/main passes。宿主只判定
        // terminal；把这些步骤移到 render stop 之后会静默丢 destroy 命令。
        if (runtime_->residency_)
        {
            const auto report = runtime_->close_driver_->close(
                *runtime_->residency_);
            if (!report)
            {
                lux::log::error(
                    "editor",
                    "residency close watchdog expired; dependencies stay alive"
                );
                return;
            }
            if (!report->clean())
                lux::log::error("editor", "residency close was not clean");
        }

        // Residency's in-flight RPC continuations use the executor's main
        // scheduler, so its owner reaches a terminal close before the process
        // executor. With the table terminal and external spawners detached,
        // shutdown can settle the remaining CPU work without producing new
        // residency work.
        if (runtime_->editor_async_)
        {
            if (!runtime_->close_driver_->close(
                    runtime_->editor_async_->closeAsync()))
            {
                lux::log::error("editor", "editor async close timed out");
                return;
            }
        }
        if (runtime_->entity_sections_)
            runtime_->entity_sections_->close();
        if (runtime_->geometry_preparation_)
            runtime_->geometry_preparation_->close();
        if (runtime_->navigation_preparation_)
            runtime_->navigation_preparation_->close();
        if (runtime_->physics_preparation_)
            runtime_->physics_preparation_->close();
        if (runtime_->tilemap_preparation_)
            runtime_->tilemap_preparation_->close();
        if (runtime_->asset_load_)
            runtime_->asset_load_->close();
        if (runtime_->upload_service_)
        {
            const auto report = runtime_->close_driver_->close(
                *runtime_->upload_service_);
            if (!report)
            {
                lux::log::error(
                    "editor",
                    "upload close watchdog expired; dependencies stay alive");
                return;
            }
        }
        // Stop + join the render thread now (runtime_->window_ + runtime_->session_ still alive; the
        // scene tearDown above pumped frames the thread was serving). stop()
        // drops the host's channel / sync references, which is safe here:
        // runtime_->session_ holds its own shared_ptr copies, so the comm objects
        // stay alive through active reply draining.
        if (runtime_->render_thread_host_)
        {
            const auto report = runtime_->render_thread_host_->stop();
            if (!report.clean())
            {
                lux::log::error(
                    "editor",
                    "render backend close was not clean (accepted={}, "
                    "ready={}, failed={}, active={})",
                    report.uploads.accepted,
                    report.uploads.terminal_ready,
                    report.uploads.terminal_failed,
                    report.uploads.active
                );
                return;
            }
        }
        if (runtime_->upload_service_)
            runtime_->upload_service_->unbind();

        if (runtime_->log_router_)
        {
            if (!runtime_->close_driver_->close(*runtime_->log_router_))
            {
                lux::log::error("editor", "log close watchdog expired");
                return;
            }
        }
        if (runtime_->async_)
        {
            if (!runtime_->close_driver_->close(*runtime_->async_))
            {
                lux::log::error("editor", "async runtime close failed");
                return;
            }
        }

        // 断开窗口 file-drop 槽:闭包捕获 this,不能活过本对象(单槽回调缝,
        // 置空即断开 —— 旧 ScopedConnection 的职责)。
        if (runtime_->window_)
            runtime_->window_->on_file_drop = {};

        // Everything below this line is passive release. One top-level reset
        // invokes Runtime's reverse declaration order; child destructors own
        // their local CPU-side close protocol.
        runtime_.reset();
    }

} // namespace lux::editor
