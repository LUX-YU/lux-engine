#pragma once
#include <lux/engine/function/visibility_ui.h>
#include "Panel.hpp"

#include <lux/cxx/container/SparseSet.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ImGuiContext;

namespace lux::window { class LuxWindow; }

namespace lux::ui
{
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
     *   ui.addPanel(&my_panel);
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

        template<typename T>
        size_t addPanel(T* panel) requires std::is_base_of_v<Panel, T>
        {
            return registerPanel(panel);
        }

        template<typename T>
        void markPanelAsRemove(T* panel) requires std::is_base_of_v<Panel, T>
        {
            panel_remove_callbacks_.push_back(
                [this, panel]()
                {
                    unregisterPanel(panel);
                    panel->onDelete();
                }
            );
        }

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

        /// Replace the in-memory ini settings with the contents of the
        /// given file. No-op (with cleared state) if the file does not
        /// exist; ImGui will fall back to per-window FirstUseEver
        /// defaults on first paint.
        void loadLayoutFromFile(const std::filesystem::path& path);

        /// Force-flush current ini state to disk. Caller is responsible
        /// for ensuring the parent directory exists.
        void saveLayoutToFile(const std::filesystem::path& path) const;

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
        size_t registerPanel(Panel* panel);
        void   unregisterPanel(Panel* panel);

        std::vector<std::function<void()>> panel_remove_callbacks_;
        std::function<void()>              main_menu_bar_hook_;
        std::function<void()>              overlay_hook_;

        class Impl;
        std::unique_ptr<Impl> impl_;
    };
}
