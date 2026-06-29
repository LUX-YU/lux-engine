#pragma once
/**
 * @file LauncherApp.hpp
 * @brief Top-level shell of the project launcher.
 *
 * Owns the long-lived objects the launcher needs to paint its one
 * ImGui window:
 *
 *   - GLFW runtime + window
 *   - UISystem (ImGui context + GLFW backend)
 *   - Render server thread + comm channel
 *   - UIRenderSession (main-thread submission)
 *   - One ProjectPickerPanel
 *
 * Lifetime:
 *   ctor       — store config only
 *   init()     — bring up window, render thread, session, picker panel
 *   run()      — main loop until either a project is chosen (in which
 *                case the editor is spawned and run() returns 0) or the
 *                user closes the window (run() returns 1, "cancelled").
 *   dtor       — tear down render thread + release everything
 */

#include <lux/engine/launcher/visibility.h>

#include <lux/engine/render/comm/FrameProgram.hpp>
#include <lux/engine/ui/ImGuiCommConfig.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace lux::window { class GlfwRuntime; class LuxWindow; }
namespace lux::ui     { class UISystem; class UIRenderSession; }

namespace lux::launcher
{
    class ProjectPickerPanel;

    struct LauncherConfig
    {
        int         width  = 900;
        int         height = 600;
        std::string title  = "Lux Launcher";

        /// Vulkan validation layers — off by default for the launcher
        /// (we're not debugging anything special here). Editor enables
        /// its own validation independently.
        bool        enable_vulkan_validation = false;
    };

    class LUX_ENGINE_LAUNCHER_PUBLIC LauncherApp
    {
    public:
        explicit LauncherApp(LauncherConfig config);
        ~LauncherApp();

        LauncherApp(const LauncherApp&)            = delete;
        LauncherApp& operator=(const LauncherApp&) = delete;
        LauncherApp(LauncherApp&&)                 = delete;
        LauncherApp& operator=(LauncherApp&&)      = delete;

        /// Bring up the window, render thread, panel. Returns false on
        /// failure (diagnostic written to stderr).
        [[nodiscard]] bool init();

        /// Main loop. Returns:
        ///   0 — a project was picked and `lux_editor.exe` was spawned
        ///   1 — user closed the launcher window without picking (cancel)
        ///   2 — spawn of `lux_editor.exe` was attempted but failed
        int run();

        /// Ask the main loop to exit at the next iteration. Thread-safe.
        void requestQuit() noexcept;

    private:
        // Background entry point for the render thread.
        void renderThreadMain();

        // Stop the render thread and tear down state. Safe to call
        // multiple times; idempotent. Called automatically by the dtor.
        void shutdown() noexcept;

        // Fired by the picker (from inside paint()) when the user
        // picks an existing `.luxproject` or completes a New / Demo
        // form. We do NOT spawn the editor here — the picker callback
        // runs inside an ImGui paint scope and `CreateProcessW` +
        // `requestQuit` would race the in-flight frame. Instead we
        // stash the choice; `run()` drains it at the next iteration's
        // top, outside any frame.
        void enqueueChoice(const std::filesystem::path& manifest);

        // Run at the top of each main-loop iteration, before
        // `ui_system_->newFrame`. Drains any choice the picker
        // enqueued during the previous frame, spawns the editor,
        // and asks the loop to quit. Frame-out side means there's
        // no ImGui state to corrupt and no render-thread submission
        // in flight while we touch the channel.
        void processPendingChoice();

        LauncherConfig                                          config_;
        bool                                                    initialised_{false};
        std::atomic<bool>                                       quit_requested_{false};

        // Exit code reported by run(). Picker callback overwrites this
        // when the user makes a choice.
        int                                                     exit_code_{1};

        // Picker → main-loop hand-off. Set by `enqueueChoice` from
        // inside the panel's paint() and consumed by
        // `processPendingChoice` at the next iteration's top. Held
        // by value (not pointer) since paths are small.
        std::optional<std::filesystem::path>                    pending_choice_;

        // Main-thread state.
        std::unique_ptr<lux::window::GlfwRuntime>               glfw_;
        std::unique_ptr<lux::window::LuxWindow>                 window_;
        std::unique_ptr<lux::ui::UISystem>                      ui_system_;
        std::unique_ptr<lux::ui::UIRenderSession>               session_;
        std::unique_ptr<ProjectPickerPanel>                     picker_panel_;

        // Comm + render-thread state.
        std::shared_ptr<lux::render::RenderProgramChannel<>>    channel_;
        std::shared_ptr<lux::render::RenderChannelSync>         sync_;
        lux::ui::ImGuiOperationIds                              imgui_ops_{};
        std::atomic<bool>                                       server_ready_{false};
        std::atomic<bool>                                       server_failed_{false};
        std::thread                                             render_thread_;
    };

} // namespace lux::launcher
