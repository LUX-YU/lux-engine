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
 *   - UIRenderFrameSession (main-thread submission)
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

#include <lux/engine/hosts/launcher/visibility.h>

#include <lux/engine/events/DomainEvents.hpp>   // main-thread committed facts

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace lux::window { class GlfwRuntime; class LuxWindow; }
namespace lux::ui     { class UISystem; class UIRenderServer; class UIRenderFrameSession; }

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

        // Run at the top of each main-loop iteration, before the runtime UI's
        // `newFrame`. Drains any choice the picker
        // enqueued during the previous frame, spawns the editor,
        // and asks the loop to quit. Frame-out side means there's
        // no ImGui state to corrupt and no render-thread submission
        // in flight while we touch the channel.
        void processPendingChoice();

        LauncherConfig                                          config_;
        bool                                                    initialised_{false};
        std::atomic<bool>                                       quit_requested_{false};

        // 主线程 DomainEvents：launcher 是第三个宿主。日志保持独立 stderr
        // 出口；只有渲染诊断在主线程采纳后成为可选领域事实。声明序：
        // events 在所有订阅目标之前（最后死），subs_ 在文件尾部（最先死）。
        std::unique_ptr<lux::events::DomainEvents>                  events_;
        lux::events::EventPump*                                 frame_pump_{nullptr};

        // Exit code reported by run(). Picker callback overwrites this
        // when the user makes a choice.
        int                                                     exit_code_{1};

        // Picker → main-loop hand-off. Set by `enqueueChoice` from
        // inside the panel's paint() and consumed by
        // `processPendingChoice` at the next iteration's top. Held
        // by value (not pointer) since paths are small.
        std::optional<std::filesystem::path>                    pending_choice_;

        // One composition owner for GLFW/window/UI/render-thread/session/frame.
        // Its declaration order encodes all borrow lifetimes; Runtime::~Runtime
        // stops the render thread while the client session + window are alive,
        // then normal reverse destruction does the rest. LauncherApp therefore
        // never performs a field-by-field reset protocol.
        struct Runtime;
        std::unique_ptr<Runtime>                                runtime_;

        // 装配层订阅（声明最后，析构最先——退订先于订阅目标与 events 死亡）。
        lux::events::SubscriptionGroup                          subs_;
    };

} // namespace lux::launcher
