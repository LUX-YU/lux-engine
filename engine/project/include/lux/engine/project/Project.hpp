#pragma once
/**
 * @file Project.hpp
 * @brief User workspace — the top-level object the editor opens.
 *
 * A `Project` is a directory on disk with a `.luxproject` manifest at its
 * root, plus the standard sub-folders (`Content/`, `Scenes/`, `Source/`,
 * `Plugins/`, `Config/`, `.lux/`). The editor owns one `Project` at a
 * time; loading a project pushes its asset directories into the
 * `AssetManager` and its default scene into the active `EditorScene`.
 *
 * `Project` is value-semantic and does not own any runtime objects
 * (AssetManager, World, render state). It just resolves paths and holds
 * the parsed manifest. Side-effects (registering assets, loading scenes)
 * are driven by the editor (or by the launcher's "New Project" flow,
 * which just calls `newOnDisk` and then spawns the editor on the result).
 *
 * Lives in `lux::project` (not `lux::editor`) so the launcher and any
 * future standalone runtime can use it without dragging in the editor or
 * render dependencies.
 */

#include <lux/engine/project/visibility.h>
#include <lux/engine/project/ProjectManifest.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace lux::project
{
    class LUX_ENGINE_PROJECT_PUBLIC Project
    {
    public:
        /// Open an existing project. `luxproject_file` is the path to a
        /// `.luxproject` file; the project's root is its parent directory.
        /// Errors: file not found, parse failure, missing required fields.
        [[nodiscard]] static lux::cxx::expected<Project, std::string>
            openFromDisk(const std::filesystem::path& luxproject_file);

        /// Create a new project skeleton on disk: writes
        /// `<root>/<name>.luxproject` plus empty `Content/`, `Scenes/`,
        /// `Source/`, `Plugins/`, `Config/`, `.lux/` sub-folders. Returns
        /// the freshly-opened project.
        ///
        /// `template_name` selects which seed content to copy in (M3a:
        /// only `"Empty"` is supported; future templates can add starter
        /// scenes / Source modules).
        ///
        /// Errors: root already non-empty, filesystem failure, name
        /// invalid.
        [[nodiscard]] static lux::cxx::expected<Project, std::string>
            newOnDisk(const std::filesystem::path& root,
                      std::string_view              project_name,
                      std::string_view              template_name = "Empty");

        /// Re-serialize the manifest to disk. Safe to call after editing
        /// `manifest()`.
        [[nodiscard]] lux::cxx::expected<void, std::string>
            saveManifest() const;

        // ── Path accessors ──────────────────────────────────────────────

        [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
        [[nodiscard]] std::filesystem::path contentRoot()  const { return root_ / "Content"; }
        [[nodiscard]] std::filesystem::path scenesRoot()   const { return root_ / "Scenes"; }
        [[nodiscard]] std::filesystem::path sourceRoot()   const { return root_ / "Source"; }
        [[nodiscard]] std::filesystem::path pluginsRoot()  const { return root_ / "Plugins"; }
        [[nodiscard]] std::filesystem::path configRoot()   const { return root_ / "Config"; }
        [[nodiscard]] std::filesystem::path cacheRoot()    const { return root_ / ".lux"; }
        [[nodiscard]] std::filesystem::path manifestPath() const;

        /// Per-project ImGui layout file. Lives under `cacheRoot()` so
        /// it travels with the project on disk yet is treated as
        /// user-local cache (intended to be gitignored alongside the
        /// rest of `.lux/`).
        [[nodiscard]] std::filesystem::path layoutPath() const { return cacheRoot() / "editor-layout.ini"; }

        /// Per-project editor window-visibility cache (which panels are shown).
        /// Lives under `cacheRoot()` next to the dock layout — same user-local,
        /// gitignored, auto-saved cache; NOT the team-shared `.luxproject`
        /// manifest, so toggling a window doesn't churn version control.
        [[nodiscard]] std::filesystem::path windowConfigPath() const { return cacheRoot() / "editor-windows.cfg"; }

        // ── Manifest access ─────────────────────────────────────────────

        [[nodiscard]] const ProjectManifest& manifest() const noexcept { return manifest_; }
        [[nodiscard]] ProjectManifest&       manifest()       noexcept { return manifest_; }

        /// Resolve `manifest.default_scene` (relative POSIX) to an
        /// absolute path. Empty if the manifest doesn't specify one
        /// or the file doesn't exist.
        [[nodiscard]] std::filesystem::path defaultScenePath() const;

        // ── Filesystem enumeration ──────────────────────────────────────

        /// Recursive scan of `Content/` for `.luxasset` files. Absolute paths.
        [[nodiscard]] std::vector<std::filesystem::path> scanAssetFiles() const;

        /// Recursive scan of `Scenes/` for `.luxscene` files. Absolute paths.
        [[nodiscard]] std::vector<std::filesystem::path> scanSceneFiles() const;

    private:
        Project(std::filesystem::path root, ProjectManifest manifest) noexcept
            : root_(std::move(root)), manifest_(std::move(manifest)) {}

        std::filesystem::path root_;
        ProjectManifest       manifest_;
    };

} // namespace lux::project
