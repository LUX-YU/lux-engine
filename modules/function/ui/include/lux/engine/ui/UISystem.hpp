#pragma once
#include <lux/engine/function/visibility_ui.h>
#include "Panel.hpp"

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/container/SparseSet.hpp>

#include <filesystem>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

struct ImGuiContext;

namespace lux::window { class LuxWindow; }

namespace lux::ui
{
    class UISystem;

    enum class EPanelRegistrationError : std::uint8_t
    {
        ALREADY_REGISTERED
    };

    /// Move-only lifetime token for a panel registered with one UISystem.
    /// Destroying it removes the panel before the panel object can disappear.
    class LUX_FUNCTION_UI_PUBLIC PanelRegistration final
    {
    public:
        PanelRegistration() noexcept = default;
        ~PanelRegistration();

        PanelRegistration(const PanelRegistration&) = delete;
        PanelRegistration& operator=(const PanelRegistration&) = delete;
        PanelRegistration(PanelRegistration&& other) noexcept;
        PanelRegistration& operator=(PanelRegistration&& other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return owner_ != nullptr;
        }

        void reset() noexcept;

    private:
        friend class UISystem;
        PanelRegistration(
            UISystem& owner,
            Panel& panel,
            std::size_t handle) noexcept
            : owner_(&owner), panel_(&panel), handle_(handle)
        {}

        UISystem*   owner_{nullptr};
        Panel*      panel_{nullptr};
        std::size_t handle_{0u};
    };

    /**
     * @brief ImGui context + panel management, decoupled from Vulkan rendering.
     *
     * UISystem owns the Dear ImGui context and the GLFW platform backend.
     * It drives the per-frame ImGui pipeline:
     *   ImGui_ImplGlfw_NewFrame → ImGui::NewFrame → docking → panels → ImGui::Render
     *
     * The Vulkan rendering backend lives on the render server thread
     * (UIRenderServer); main-thread code only builds draw lists here.
     *
     * Usage:
     * @code
     *   lux::window::LuxWindow window(1280, 720, "App");
     *   lux::ui::UISystem ui(window);
     *   auto registration = ui.registerPanel(my_panel);
     *
     *   while (!window.shouldClose()) {
     *       lux::window::LuxWindow::pollEvents();
     *       ui.newFrame();   // builds ImGui draw lists
     *       // ... submit ImGui::GetDrawData() to render session ...
     *   }
     * @endcode
     */
    class LUX_FUNCTION_UI_PUBLIC UISystem
    {
    public:
        explicit UISystem(lux::window::LuxWindow& window);
        ~UISystem();

        UISystem(const UISystem&) = delete;
        UISystem& operator=(const UISystem&) = delete;

        // ── Panel management ─────────────────────────────────────────

        [[nodiscard]] lux::cxx::expected<
            PanelRegistration,
            EPanelRegistrationError>
        registerPanel(Panel& panel);

        // ── Per-frame ────────────────────────────────────────────────
        /// Drive ImGui pipeline: NewFrame → docking → panels → Render.
        /// Also calls ImGui::UpdatePlatformWindows() for multi-viewport.
        /// After this call, ImGui::GetDrawData() is valid.
        void newFrame();

        /// Optional hook fired inside `newFrame()` between `ImGui::NewFrame`
        /// and panel paints. Intended for top-level ImGui constructs that
        /// must live OUTSIDE any window scope — chiefly the main menu bar.
        /// Callers register one lambda; passing `{}` clears it.
        void setMainMenuBarHook(std::function<void()> hook);

        /// Optional hook fired inside `newFrame()` AFTER panels are painted and
        /// the docking host is closed, but BEFORE `ImGui::Render()`. Intended
        /// for foreground overlays that must float above all panels — toasts,
        /// transient HUD, drag ghosts. Callers register one lambda; `{}` clears.
        void setOverlayHook(std::function<void()> hook);

        /// Access the ImGui context (for cross-DLL sharing).
        [[nodiscard]] ImGuiContext* context() const noexcept;

        // ── Layout persistence ───────────────────────────────────────
        //
        // ImGui's IniFilename is held at nullptr (manual mode) so that
        // each project can own its layout file under
        // `<project>/.lux/editor-layout.ini`. The editor drives Load /
        // Save explicitly on project open / close / save; in addition,
        // newFrame() will throttle-autosave to `setAutosaveTarget` while
        // a project is open so a crash does not lose layout edits.
        //
        // Paths are passed by value and held internally — ImGui never
        // sees the C-string for IniFilename, so caller need not keep
        // the path alive past the call.

        // 这两个函数原本返回 void。它们**碰文件系统**,而磁盘满、路径不可写、
        // 权限不足都会让保存失败 —— 返回 void 意味着用户的面板布局丢了而没有
        // 任何渠道能知道。ImGui 的 Load/SaveIniSettingsToDisk 自己吞掉失败,
        // 所以下面改走内存版 + 自己做 IO,失败才看得见。
        //
        // 用 std::error_code 而不是 render 的 Expected:这是文件系统错误,
        // errc 就是它的标准词汇,没必要为此把渲染错误注册表拉进 UI 层。

        /// Replace the in-memory ini settings with the contents of the
        /// given file. No-op (with cleared state) if the file does not
        /// exist; ImGui will fall back to per-window FirstUseEver
        /// defaults on first paint.
        /// @return 空 error_code 表示成功(含"文件不存在"这一正常情形);
        ///         非空表示文件存在却读不出来。
        [[nodiscard]] std::error_code loadLayoutFromFile(const std::filesystem::path& path);

        /// Force-flush current ini state to disk. Caller is responsible
        /// for ensuring the parent directory exists.
        /// @return 空 error_code 表示确实写成功了。
        [[nodiscard]] std::error_code saveLayoutToFile(const std::filesystem::path& path) const;

        /// Drop all ini settings ImGui currently holds. Useful when
        /// switching projects so the next Load starts from a clean
        /// slate (otherwise the previous project's window settings
        /// would leak into the merge).
        void clearLayout();

        /// Enable or disable in-frame throttled autosave. Empty path
        /// disables; any non-empty path becomes the autosave target.
        /// Frequency follows ImGui's `io.IniSavingRate` (default ~5s).
        void setAutosaveTarget(std::filesystem::path path);

        // ── Utilities ────────────────────────────────────────────────

        /// Required Vulkan instance extensions for a windowed surface.
        [[nodiscard]] static std::vector<const char*> requiredVulkanExtensions();

    private:
        friend class PanelRegistration;
        void unregisterPanel(
            std::size_t handle,
            Panel* expected_panel) noexcept;

        std::function<void()>              main_menu_bar_hook_;
        std::function<void()>              overlay_hook_;

        class Impl;
        std::unique_ptr<Impl> impl_;
    };
}
