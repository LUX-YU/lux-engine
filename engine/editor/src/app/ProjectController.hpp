#pragma once
// ============================================================================
//  ProjectController — owns the open Project and the project lifecycle:
//  open / new / close / save, plus the per-project window-visibility cache
//  (which panels are shown, auto-saved under .lux/). Extracted verbatim from
//  LuxEditor.
//
//  Drives the SceneController (open/close a project loads/unloads its scene),
//  so it holds a SceneController& — a one-way dependency (the scene controller
//  knows nothing about projects). Holds non-owning refs/ptrs to the other
//  subsystems it touches (UISystem, AssetManager / Registry / Browser, the
//  window catalogue), all created before it in LuxEditor::init().
//
//  Private editor header (engine/editor/src/app — not installed).
// ============================================================================

#include <lux/engine/editor/project/Project.hpp>   // Project (alias) — unique_ptr member

#include <filesystem>
#include <memory>
#include <string_view>

namespace lux::ui    { class UISystem; }
namespace lux::asset { class AssetManager; }

namespace lux::editor
{
    class SceneController;
    class AssetRegistry;
    class AssetBrowser;
    class PanelRegistry;

    class ProjectController
    {
    public:
        ProjectController(SceneController&                          scene,
                          lux::ui::UISystem*                        ui_system,
                          std::shared_ptr<lux::asset::AssetManager> asset_mgr,
                          AssetRegistry*                            asset_registry,
                          AssetBrowser*                             asset_browser,
                          PanelRegistry&                            panels) noexcept;

        ProjectController(const ProjectController&)            = delete;
        ProjectController& operator=(const ProjectController&) = delete;

        /// Open a project from a `.luxproject` manifest path. Closes any
        /// currently-open project first; loads the default scene if the
        /// manifest names one (else an empty scene). False on parse failure.
        [[nodiscard]] bool openProject(const std::filesystem::path& luxproject_file);

        /// Create + open a fresh project under @p root (skeleton + manifest
        /// written by Project::newOnDisk).
        [[nodiscard]] bool newProject(const std::filesystem::path& root,
                                      std::string_view             project_name);

        /// Close the project + unload its scene. Safe with no project open.
        void closeProject() noexcept;

        /// Re-write the current project's manifest (+ layout) to disk.
        [[nodiscard]] bool saveProject();

        Project*       currentProject()       noexcept { return current_project_.get(); }
        const Project* currentProject() const noexcept { return current_project_.get(); }

    private:
        // Per-project window-visibility cache (which panels are shown), saved to
        // current_project_->windowConfigPath() under .lux/ — auto-saved on close
        // so reopening remembers the last setup with no manual action.
        void loadWindowVisibility();
        void saveWindowVisibility() const;

        SceneController&                          scene_;
        lux::ui::UISystem*                        ui_system_;
        std::shared_ptr<lux::asset::AssetManager> asset_mgr_;
        AssetRegistry*                            asset_registry_;
        AssetBrowser*                             asset_browser_;
        PanelRegistry&                            panel_registry_;

        std::unique_ptr<Project>                  current_project_;
    };

} // namespace lux::editor
