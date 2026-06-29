#include <lux/engine/launcher/LauncherApp.hpp>
#include <lux/engine/launcher/ProjectPickerPanel.hpp>
#include <lux/engine/launcher/SpawnHelpers.hpp>

#include <lux/engine/project/RecentProjects.hpp>

#include <lux/engine/meta/Meta.hpp>           // meta_module_init / deinit
#include <lux/engine/ui/UIRenderServer.hpp>
#include <lux/engine/ui/UiAppHost.hpp>   // bringUpUIRenderServer (shared render-server bring-up)
#include <lux/engine/ui/UIRenderSession.hpp>
#include <lux/engine/ui/UISystem.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>

#include <imgui.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

namespace lux::launcher
{
    namespace
    {
        constexpr auto kServerWaitSlice = std::chrono::milliseconds(5);
    }

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

        // ── 0. Reflection registry — drains the gameplay_meta sidecar
        //      registrar queue so ComponentTypeRegistry is populated
        //      before "New Demo Project" runs Scene::save. Without
        //      this, the demo .luxscene would be written with zero
        //      components (everything is silently skipped).
        lux::meta::meta_module_init();

        // ── 1. GLFW + window ──
        glfw_ = std::make_unique<lux::window::GlfwRuntime>();
        if (!glfw_->valid())
        {
            std::fprintf(stderr, "[LauncherApp] glfwInit failed\n");
            return false;
        }

        window_ = std::make_unique<lux::window::LuxWindow>(
            config_.width, config_.height, config_.title);

        // ── 2. ImGui UISystem ──
        ui_system_ = std::make_unique<lux::ui::UISystem>(*window_);

        // ── 3. Picker panel — registered before the render thread starts
        //      so the very first frame already paints something. ──
        //
        //      The callback only enqueues — actual spawn + quit happens
        //      at the top of the next main-loop iteration, OUTSIDE any
        //      ImGui frame.
        picker_panel_ = std::make_unique<ProjectPickerPanel>(
            [this](const std::filesystem::path& p){ enqueueChoice(p); });
        ui_system_->addPanel(picker_panel_.get());

        // ── 4. Comm channel + render thread ──
        channel_ = lux::render::RenderProgramChannel<>::create();
        sync_    = std::make_shared<lux::render::RenderChannelSync>();

        render_thread_ = std::thread(&LauncherApp::renderThreadMain, this);

        while (!server_ready_.load(std::memory_order_acquire) &&
               !server_failed_.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(kServerWaitSlice);
        }

        if (server_failed_.load(std::memory_order_acquire))
        {
            if (render_thread_.joinable())
                render_thread_.join();
            std::fprintf(stderr, "[LauncherApp] render server failed to start\n");
            return false;
        }

        // ── 5. Main-thread render session ──
        session_ = std::make_unique<lux::ui::UIRenderSession>(
            channel_, sync_, imgui_ops_);

        initialised_ = true;
        return true;
    }

    int LauncherApp::run()
    {
        if (!initialised_)
        {
            std::fprintf(stderr,
                "[LauncherApp] run() called before successful init()\n");
            return 2;
        }

        while (!window_->shouldClose() &&
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

            ui_system_->newFrame();

            ImDrawData* draw_data = ImGui::GetDrawData();

            // Drain prior-frame replies and flush before asking for a new
            // frame slot (same pattern as LuxEditor::run() to avoid the
            // overlap deadlock).
            session_->pumpReplies();
            session_->submitFrame(true);

            if (!session_->beginFrame())
                continue;

            session_->submitImGuiDrawData(lux::render::RenderSceneId{}, draw_data);
            session_->submitFrame();
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
        lux::project::pushRecentProject(manifest);

        if (!spawnEditorForProject(manifest))
        {
            std::fprintf(stderr,
                "[LauncherApp] failed to spawn lux_editor.exe (missing or "
                "CreateProcessW failed) for project '%s'\n",
                manifest.string().c_str());
            exit_code_ = 2;
        }
        else
        {
            std::fprintf(stderr,
                "[LauncherApp] spawned lux_editor.exe for '%s'; exiting.\n",
                manifest.string().c_str());
            exit_code_ = 0;
        }
        requestQuit();
    }

    void LauncherApp::renderThreadMain()
    {
        auto server = lux::ui::bringUpUIRenderServer(
            channel_, sync_, *window_, config_.enable_vulkan_validation, "LauncherApp");
        if (!server)
        {
            server_failed_.store(true, std::memory_order_release);
            return;
        }

        imgui_ops_ = server->imguiOps();
        server_ready_.store(true, std::memory_order_release);

        while (server->tick())
        {
            // Loop until the channel is asked to stop.
        }
    }

    void LauncherApp::shutdown() noexcept
    {
        if (sync_)
            sync_->requestStop();

        if (render_thread_.joinable())
            render_thread_.join();

        // Tear down in reverse order. Session must die before
        // channel/sync (it borrows from them); panels before UISystem
        // (UISystem holds raw pointers).
        session_.reset();
        picker_panel_.reset();
        ui_system_.reset();
        window_.reset();
        glfw_.reset();

        channel_.reset();
        sync_.reset();

        initialised_ = false;
    }

} // namespace lux::launcher
