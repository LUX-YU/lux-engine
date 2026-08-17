#pragma once
// ============================================================================
//  ProjectController — owns the open Project and the project lifecycle:
//  open / new / close / save, plus the per-project window-visibility cache
//  (which panels are shown, auto-saved under .lux/). Extracted verbatim from
//  LuxEditor.
//
//  Drives the SceneController (open/close a project loads/unloads its scene),
//  so it holds a SceneController& — a one-way dependency (the scene controller
//  knows nothing about projects). The other subsystems it touches (UISystem,
//  AssetManager / Registry / Browser, the window catalogue) are borrowed the
//  same way: all are created before it in LuxEditor::init() and outlive it, so
//  they are references and "never null" is a compiler-checked fact.
//
//  Private editor header (engine/editor/src/app — not installed).
// ============================================================================

#include <lux/engine/authoring/project/Project.hpp>
#include <lux/engine/runtime/extensions/EngineExtensions.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace lux::ui    { class UISystem; }
namespace lux::asset { class AssetManager; }

namespace lux::editor
{
    class SceneController;
    class AssetRegistry;
    class AssetBrowser;
    class EditorToolHost;

    class ProjectController
    {
    public:
        ProjectController(SceneController&                          scene,
                          lux::ui::UISystem&                        ui_system,
                          std::shared_ptr<lux::asset::AssetManager> asset_mgr,
                          AssetRegistry&                            asset_registry,
                          AssetBrowser&                             asset_browser,
                          EditorToolHost&                           tools,
                          lux::extensions::EngineExtensions&       extensions) noexcept;

        ProjectController(const ProjectController&)            = delete;
        ProjectController& operator=(const ProjectController&) = delete;

        /// Open a project from a `.luxproject` manifest path. Closes any
        /// currently-open project first; loads the default scene if the
        /// manifest names one (else an empty scene). False on parse failure.
        [[nodiscard]] bool openProject(const std::filesystem::path& luxproject_file);

        /// Advance an accepted project-open workflow. DLL I/O and registration
        /// complete independently of the frame; this safe point only observes
        /// tickets and commits the project/scene state on the main thread.
        [[nodiscard]] std::size_t processSafePoint() noexcept;

        /// Create + open a fresh project under @p root (skeleton + manifest
        /// written by Project::newOnDisk).
        [[nodiscard]] bool newProject(const std::filesystem::path& root,
                                      std::string_view             project_name);

        /// Close the project + unload its scene. Safe with no project open.
        void closeProject() noexcept;

        /// Re-write the current project's manifest (+ layout) to disk.
        [[nodiscard]] bool saveProject();

        lux::authoring::Project*       currentProject()       noexcept { return current_project_.get(); }
        const lux::authoring::Project* currentProject() const noexcept { return current_project_.get(); }

    private:
        // Per-project window-visibility cache (which panels are shown), saved to
        // current_project_->windowConfigPath() under .lux/ — auto-saved on close
        // so reopening remembers the last setup with no manual action.
        void loadWindowVisibility();
        void saveWindowVisibility() const;
        [[nodiscard]] bool finishOpenProject(const std::filesystem::path& luxproject_file);

        struct PendingProjectOpen final
        {
            std::filesystem::path luxproject_file;
            std::vector<lux::extensions::ExtensionLoadTicket> modules;
        };

        SceneController&                          scene_;
        lux::ui::UISystem&                        ui_system_;
        std::shared_ptr<lux::asset::AssetManager> asset_mgr_;
        AssetRegistry&                            asset_registry_;
        AssetBrowser&                             asset_browser_;
        EditorToolHost&                           tools_;
        lux::extensions::EngineExtensions&        extensions_;

        std::unique_ptr<lux::authoring::Project>                  current_project_;
        std::optional<PendingProjectOpen>                        pending_open_;
    };

} // namespace lux::editor
