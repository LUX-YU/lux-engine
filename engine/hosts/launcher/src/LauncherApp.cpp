#include <lux/engine/hosts/launcher/LauncherApp.hpp>
#include <lux/engine/hosts/launcher/ProjectPickerPanel.hpp>
#include <lux/engine/hosts/launcher/SpawnHelpers.hpp>

#include <lux/engine/authoring/project/RecentProjects.hpp>

#include <lux/engine/meta/Meta.hpp>           // meta_module_init / deinit
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/runtime/frame/FrameCoordinator.hpp>
#include <lux/engine/log/Log.hpp>             // 校验层文本 + 渲染错误的落点
#include <lux/engine/runtime/render/scene/RenderDiagnostics.hpp>   // installRenderErrorLogging
#include <lux/engine/runtime/render/backend_host/RenderBackendHost.hpp>
#include <lux/engine/ui/ImGuiCommConfig.hpp>
#include <lux/engine/ui/UIRenderServer.hpp>
#include <lux/engine/ui/UIRenderFrameSession.hpp>
#include <lux/engine/ui/UISystem.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>

#include <imgui.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

namespace lux::launcher
{
    namespace
    {
        // ── UIRenderServer bring-up — RenderBackendHost 的 bring_up 回调正文,
        //    在**渲染线程**上跑 ────────────────────────────────────────────
        // 原 lux::ui::bringUpUIRenderServer(UiAppHost)的正文收进宿主(编辑器
        // 侧有它的孪生 bringUpEditorRenderServer):服务器怎么建是宿主的装配
        // 决定(装配归属 ADR 裁决三)。失败不打 stderr —— bring_up 返回 null
        // 经 start() 的握手带回 init(),那里有响亮出口。
        std::unique_ptr<lux::ui::UIRenderServer> bringUpLauncherRenderServer(
            const std::shared_ptr<lux::render::RenderFrameChannel<>>& channel,
            const std::shared_ptr<lux::render::RenderControlChannel<>>& control_channel,
            const std::shared_ptr<lux::render::RenderUploadChannel<>>& upload_channel,
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
            if (auto r = server->attachToWindow(window); !r)
                return nullptr;
            return server;
        }
    }

    /// Launcher 的一个 fully-live runtime aggregate。声明序即依赖序:
    ///
    ///   glfw ← window ← ui ← picker
    ///                    ↖ render_host ← session ← render_runtime
    ///
    /// 析构体先 stop render thread(此时 session/window 均在),成员再
    /// 自然逆序析构。不将这个协议泄漏给 LauncherApp::shutdown。
    struct LauncherApp::Runtime
    {
        lux::ecs::ComponentTypeCatalog             component_types;
        std::unique_ptr<lux::window::GlfwRuntime> glfw;
        std::unique_ptr<lux::window::LuxWindow>   window;
        std::unique_ptr<lux::ui::UISystem>        ui;
        std::unique_ptr<ProjectPickerPanel>       picker;
        lux::ui::PanelRegistration                picker_registration;

        lux::runtime::RenderBackendHost<lux::ui::UIRenderServer> render_host;
        lux::ui::ImGuiOperationIds                              imgui_ops{};
        std::unique_ptr<lux::ui::UIRenderFrameSession>               session;
        std::unique_ptr<lux::runtime::FrameCoordinator>         frame_coordinator;

        Runtime() = default;
        ~Runtime() { (void)render_host.stop(); }

        void close() noexcept
        {
            const auto report = render_host.stop();
            if (!report.clean())
                lux::log::error(
                    "launcher",
                    "render backend close was not clean (accepted={}, "
                    "ready={}, failed={}, active={})",
                    report.uploads.accepted,
                    report.uploads.terminal_ready,
                    report.uploads.terminal_failed,
                    report.uploads.active
                );
        }

        Runtime(const Runtime&)            = delete;
        Runtime& operator=(const Runtime&) = delete;
    };

    LauncherApp::LauncherApp(LauncherConfig config)
        : config_(std::move(config))
    {
    }

    LauncherApp::~LauncherApp()
    {
        shutdown();
    }

    bool LauncherApp::init()
    {
        if (initialised_)
            return true;

        // ── 0x. 主线程 DomainEvents；日志不进入事实通道。
        //       launcher 是终端启动的辅助壳，日志直达 stderr；frame pump
        //       在主循环安全点排空事实。
        events_     = std::make_unique<lux::events::DomainEvents>();
        frame_pump_ = &events_->createPump("frame");
        lux::log::setOutput(
            [](const lux::log::LogRecord& r)
            { lux::log::writeRecordToStderr(r); });

        // Build off to the side and publish only once the whole runtime is live.
        // Any early return destroys this local aggregate in the same safe order.
        auto runtime = std::make_unique<Runtime>();

        // ── 0. Reflection registry — drains the gameplay_meta sidecar
        //      registrar queue so ComponentTypeCatalog is populated
        //      before "New Demo Project" writes its World document. Without
        //      this, the demo .luxworld would be written with zero
        //      components (everything is silently skipped).
        if (!lux::ecs::initializeGeneratedMetadata(
                runtime->component_types))
            return false;

        // ── 1. GLFW + window ──
        runtime->glfw = std::make_unique<lux::window::GlfwRuntime>();
        if (!runtime->glfw->valid())
        {
            lux::log::error("launcher", "glfwInit failed");
            return false;
        }

        runtime->window = std::make_unique<lux::window::LuxWindow>(
            config_.width, config_.height, config_.title);
        if (!runtime->window->isInitialized())
            return false;   // LuxWindow::init already logged why

        // ── 2. ImGui UISystem ──
        runtime->ui = std::make_unique<lux::ui::UISystem>(*runtime->window);

        // ── 3. Picker panel — registered before the render thread starts
        //      so the very first frame already paints something. ──
        //
        //      The callback only enqueues — actual spawn + quit happens
        //      at the top of the next main-loop iteration, OUTSIDE any
        //      ImGui frame.
        runtime->picker = std::make_unique<ProjectPickerPanel>(
            [this](const std::filesystem::path& p){ enqueueChoice(p); },
            runtime->component_types);
        auto picker_registration = runtime->ui->registerPanel(*runtime->picker);
        if (!picker_registration)
            return false;
        runtime->picker_registration = std::move(*picker_registration);

        // ── 4. Comm channel + render thread — the ONE template host
        //      (RenderBackendHost<UIRenderServer>, 裁决三). bring_up runs the
        //      old UiAppHost body on the render thread; the host itself
        //      registers the standard feature plan; post_init grabs the ImGui
        //      op-ids — both strictly before ready flips (start() is still
        //      blocked, so writing the member is race-free). ──
        {
            lux::runtime::RenderBackendHost<lux::ui::UIRenderServer>::Config rtc;
            rtc.enable_validation = config_.enable_vulkan_validation;
            // 校验层文本的出口此前漏传 —— 于是 --vk-validation 打开校验层之后,
            // 它的**全部文本被静默丢弃**:只剩计数器在动,够知道出事了,不够知道
            // 出了什么事。打开校验层的人就是来看消息的。
            rtc.validation_message_sink =
                [](std::uint32_t severity, std::string_view text) {
                    static constexpr lux::log::ELevel kLevel[]{
                        lux::log::ELevel::Info, lux::log::ELevel::Warn,
                        lux::log::ELevel::Error};
                    lux::log::logf(kLevel[severity < 3 ? severity : 0], "vulkan",
                                   "{}", text);
                };
            Runtime* const building = runtime.get();
            rtc.bring_up =
                [building](
                    const std::shared_ptr<lux::render::RenderFrameChannel<>>& channel,
                       const std::shared_ptr<lux::render::RenderControlChannel<>>& control_channel,
                       const std::shared_ptr<lux::render::RenderUploadChannel<>>& upload_channel,
                       const std::shared_ptr<lux::render::RenderChannelSync>&      sync,
                    const lux::runtime::RenderThreadConfig&                     c)
                { return bringUpLauncherRenderServer(
                    channel, control_channel, upload_channel, sync,
                    *building->window, c); };
            rtc.post_init = [building](lux::ui::UIRenderServer& server)
                { building->imgui_ops = server.imguiOps(); };

            if (!runtime->render_host.start(std::move(rtc)))
            {
                lux::log::error("launcher", "render server failed to start");
                return false;   // the host already joined the thread
            }
        }

        // ── 5. Main-thread render session ──
        runtime->session = std::make_unique<lux::ui::UIRenderFrameSession>(
            runtime->render_host.channel(), runtime->render_host.sync(),
            runtime->imgui_ops);
        // 渲染线程自发上报的错误(与编辑器 / player 同一份实现;事件批C:
        // 处理器发布事件,格式化+打日志是 frame 泵订阅者)。必须在第一次
        // pumpReplies 之前。
        lux::runtime::installRenderErrorLogging(*runtime->session, *events_, *frame_pump_,
                                                subs_);

        runtime->frame_coordinator =
            std::make_unique<lux::runtime::FrameCoordinator>(
                *runtime->session, *frame_pump_);
        runtime_     = std::move(runtime);   // transactional commit
        initialised_ = true;
        return true;
    }

    int LauncherApp::run()
    {
        if (!initialised_)
        {
            lux::log::error(
                "launcher",
                "run() called before successful init()"
            );
            return 2;
        }

        Runtime& runtime = *runtime_;
        while (!runtime.window->shouldClose() &&
               !quit_requested_.load(std::memory_order_acquire))
        {
            lux::window::LuxWindow::pollEvents();

            // Drain any picker choice the previous frame enqueued.
            // Must happen OUTSIDE the ImGui frame (before newFrame)
            // because spawnEditorForProject + requestQuit + setting
            // up the next quit-shutdown can race ImGui state.
            processPendingChoice();
            if (quit_requested_.load(std::memory_order_acquire))
                break;     // skip newFrame on the quit iteration

            runtime.ui->newFrame();

            ImDrawData* draw_data = ImGui::GetDrawData();

            auto frame = runtime.frame_coordinator->begin();
            if (!frame)
                continue;

            frame.record([&]
            {
                runtime.session->submitImGuiDrawData(
                    lux::render::RenderSceneId{}, draw_data);
            });
        }

        shutdown();
        return exit_code_;
    }

    void LauncherApp::requestQuit() noexcept
    {
        quit_requested_.store(true, std::memory_order_release);
    }

    void LauncherApp::enqueueChoice(const std::filesystem::path& manifest)
    {
        // Called from inside ProjectPickerPanel::paint() — store the
        // choice and bail. Heavy work is deferred to
        // `processPendingChoice` at the next main-loop iteration's
        // top, where we are guaranteed to be OUTSIDE any ImGui frame.
        pending_choice_ = manifest;
    }

    void LauncherApp::processPendingChoice()
    {
        if (!pending_choice_)
            return;

        // Move out so any re-entry is a no-op.
        auto manifest = std::move(*pending_choice_);
        pending_choice_.reset();

        // Record in the shared recent-projects file so the editor's
        // File→Recent submenu picks it up too.
        lux::authoring::pushRecentProject(manifest);

        if (!spawnEditorForProject(manifest))
        {
            lux::log::error(
                "launcher",
                "failed to spawn lux_editor.exe for project '{}'",
                manifest.generic_string()
            );
            exit_code_ = 2;
        }
        else
        {
            lux::log::info(
                "launcher",
                "spawned lux_editor.exe for '{}'; exiting",
                manifest.generic_string()
            );
            exit_code_ = 0;
        }
        requestQuit();
    }

    void LauncherApp::shutdown() noexcept
    {
        // Restore the synchronous emergency fallback for shutdown diagnostics.
        lux::log::setOutput({});
        subs_.clear();

        // One owner, one explicit lifetime transition. The final safe-point
        // passes run while the session and render thread are both live; member
        // declaration order handles only passive storage release afterwards.
        if (runtime_)
            runtime_->close();
        runtime_.reset();

        initialised_ = false;
    }

} // namespace lux::launcher
