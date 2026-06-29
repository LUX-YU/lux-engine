#pragma once
// ============================================================================
//  PanelRegistry.hpp — the editor's catalogue of toggleable windows.
//
//  Every editor panel registers here (id + live Panel*) as LuxEditor creates it.
//  The "Window" menu iterates this to toggle visibility, and project open/save
//  reads/writes the visible set by id — so adding a panel needs no edit to the
//  menu or the config code (the panel just registers itself once).
//
//  Not a global singleton: it is owned by LuxEditor, which owns the panels. A
//  plugin DLL would register its panel through the same LuxEditor seam.
// ============================================================================

#include <string>
#include <string_view>
#include <vector>

namespace lux::ui { class Panel; }

namespace lux::editor
{
    /// One registered editor window.
    struct EditorWindow
    {
        std::string     id;               ///< stable, config-facing key (e.g. "asset-browser")
        lux::ui::Panel* panel{nullptr};   ///< the live panel (owned elsewhere, by LuxEditor)
        bool            default_visible{true};  ///< seeds a project that has no saved config
    };

    /// Append-only catalogue of the editor's windows. Display names are read live
    /// from each Panel's title(), so the registry stores no duplicated label.
    class PanelRegistry
    {
    public:
        void add(std::string id, lux::ui::Panel* panel, bool default_visible = true)
        {
            if (panel)
                windows_.push_back({std::move(id), panel, default_visible});
        }

        [[nodiscard]] const std::vector<EditorWindow>& windows() const noexcept
        {
            return windows_;
        }

        /// The live panel registered under @p id, or nullptr if none.
        [[nodiscard]] lux::ui::Panel* find(std::string_view id) const noexcept
        {
            for (const auto& w : windows_)
                if (w.id == id)
                    return w.panel;
            return nullptr;
        }

    private:
        std::vector<EditorWindow> windows_;
    };

} // namespace lux::editor
